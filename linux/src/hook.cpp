// XRecord keyboard monitor for the VietKi Linux shell (guide 6.2, analogue of the
// Windows WH_KEYBOARD_LL hook and the macOS CGEventTap).
//
// X11 reality check: a normal client cannot *consume* a key bound for another
// window, so unlike Windows/macOS this monitor is passive — the original letter
// always reaches the focused app. We compensate the way every X11 rewriter does:
// the engine's correction is applied with one *extra* backspace to erase the
// character that already leaked onto the screen (see the onChar handling below).
//
// Two connections are used: g_ctrl for synchronous lookups + context setup, and
// g_data for the async record stream. The record fd is pumped from the GLib main
// loop that tray.cpp/main.cpp run, so no extra thread is needed.
//
// NOTE: mirrors the verified Windows build; not compiled on Linux here.
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/extensions/record.h>

#include <glib.h>

#include <cctype>
#include <string>

#include "app.h"

namespace vietki::lin {

namespace {

Display* g_ctrl = nullptr;            // synchronous: lookups + context creation
Display* g_data = nullptr;            // async: the XRecord data stream
XRecordContext g_ctx = 0;
GIOChannel* g_channel = nullptr;
guint g_watch = 0;

int g_ignoreEvents = 0;               // synthetic key events still to be skipped

// Our own modifier bookkeeping (independent of the X Lock/Mod maps so a
// modifier-only hotkey like "Ctrl+Shift" can be detected on release).
enum { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_SUPER = 8 };
unsigned g_mods = 0;
bool g_otherUsed = false;             // a real key was pressed while mods were held
bool g_gamingTriggerDown = false;     // trigger key is currently held down

struct Hotkey {
    unsigned mods = 0;
    KeySym base = 0;                  // 0 == modifier-only (fires on release)
    bool valid = false;
};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Parse "Ctrl+Shift", "Alt+Z", "Ctrl+Alt+Space" into modifier bits + a base key.
Hotkey parseHotkey(const std::string& spec) {
    Hotkey hk;
    std::string token;
    auto flush = [&](std::string t) {
        t = lower(trim(t));
        if (t.empty()) return;
        if (t == "ctrl" || t == "control") hk.mods |= MOD_CTRL;
        else if (t == "shift") hk.mods |= MOD_SHIFT;
        else if (t == "alt" || t == "option" || t == "opt" || t == "meta")
            hk.mods |= MOD_ALT;
        else if (t == "super" || t == "win" || t == "cmd" || t == "command")
            hk.mods |= MOD_SUPER;
        else {
            std::string name = t;
            if (name == "space") name = "space";
            else if (name.size() == 1) { /* letter/digit: keysym name == char */ }
            else if (name[0] == 'f') name = "F" + name.substr(1);  // f1 -> F1
            hk.base = XStringToKeysym(name.c_str());
        }
    };
    for (char c : spec) {
        if (c == '+') { flush(token); token.clear(); }
        else token.push_back(c);
    }
    flush(token);
    hk.valid = (hk.mods != 0 || hk.base != 0);
    return hk;
}

unsigned modBitFor(KeySym sym) {
    switch (sym) {
        case XK_Control_L: case XK_Control_R: return MOD_CTRL;
        case XK_Shift_L:   case XK_Shift_R:   return MOD_SHIFT;
        case XK_Alt_L:     case XK_Alt_R:
        case XK_Meta_L:    case XK_Meta_R:    return MOD_ALT;
        case XK_Super_L:   case XK_Super_R:   return MOD_SUPER;
        default: return 0;
    }
}

bool isWordBreakKeysym(KeySym sym) {
    switch (sym) {
        case XK_Return: case XK_KP_Enter: case XK_Tab: case XK_Escape:
        case XK_Left: case XK_Right: case XK_Up: case XK_Down:
        case XK_Home: case XK_End: case XK_Page_Up: case XK_Page_Down:
        case XK_Delete: case XK_KP_Delete:
            return true;
        default:
            return false;
    }
}

bool overrideAllowedForCurrentApp() {
    AppState& st = state();
    if (!st.config.overrideHotkeyEnabled) return false;
    if (gamingAppliesTo(st.currentApp) && st.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
        return true;
    return st.config.exclusionFeatureOn && isExcludedMember(st.currentApp);
}

bool matchesGamingTrigger(KeySym base, unsigned modsSpec) {
    AppState& st = state();
    if (st.config.gamingPolicy != GamingPolicy::TemporaryTrigger) return false;
    Hotkey trigger = parseHotkey(st.config.gamingTrigger);
    return trigger.valid && trigger.base == base && trigger.mods == modsSpec;
}

void handleKey(int type, KeyCode kc, unsigned xstate) {
    AppState& st = state();
    KeySym base = XkbKeycodeToKeysym(g_ctrl, kc, 0, 0);  // layout-independent key id
    unsigned bit = modBitFor(base);

    Hotkey master = st.config.toggleVietnameseHotkeyEnabled
                        ? parseHotkey(st.config.hotkey) : Hotkey{};
    Hotkey over = st.config.overrideHotkeyEnabled
                      ? parseHotkey(st.config.overrideHotkey) : Hotkey{};

    bool bypassHotkeys = gamingAppliesTo(st.currentApp) && st.config.gamingPolicy == GamingPolicy::TemporaryTrigger;

    if (type == KeyRelease) {
        if (bypassHotkeys && matchesGamingTrigger(base, g_mods)) {
            g_gamingTriggerDown = false;
            st.gamingSession.onTriggerUp();
            return;
        }
        if (bit) {
            unsigned before = g_mods;
            if (!g_otherUsed && !bypassHotkeys) {
                if (master.valid && master.base == 0 && before == master.mods)
                    toggleEnabled();
                else if (over.valid && over.base == 0 && before == over.mods &&
                         overrideAllowedForCurrentApp())
                    toggleOverrideForCurrentApp();
            }
            g_mods &= ~bit;
            if (g_mods == 0) g_otherUsed = false;
        }
        return;
    }

    // --- KeyPress ---
    if (bit) { g_mods |= bit; return; }
    if (g_mods != 0) g_otherUsed = true;

    if (bypassHotkeys) {
        if (matchesGamingTrigger(base, g_mods)) {
            bool isRepeat = g_gamingTriggerDown;
            g_gamingTriggerDown = true;
            GamingTypingState before = st.gamingSession.state();
            GamingAction a = st.gamingSession.onTriggerDown(isRepeat);
            if (before == GamingTypingState::Idle &&
                st.gamingSession.state() == GamingTypingState::Armed) {
                if (st.engine) st.engine->reset(); // F.2: do not feed the trigger
                applyResolvedState();
            }
            if (a.swallow) {
                injectBackspaces(1); // erase the leaked physical trigger key
                if (a.replayTrigger) replayPhysicalKey(kc);
            }
            if (before == GamingTypingState::Armed &&
                st.gamingSession.state() == GamingTypingState::Idle) {
                applyResolvedState();
            }
            return;
        }

        // Session-ending keys (D.5).
        if (base == XK_Return || base == XK_KP_Enter) {
            endGamingTypingSession(GamingEndReason::EndedEnter);
            return; // let return leak to the app
        }
        if (base == XK_Escape) {
            endGamingTypingSession(GamingEndReason::EndedEscape);
            return; // let escape leak to the app
        }

        if (st.gamingSession.state() == GamingTypingState::Armed) {
            GamingAction a = st.gamingSession.onNonModifierKey();
            if (a.activateVietnamese) {
                applyResolvedState();
                notifyGamingMode(GamingEndReason::Triggered);
            }
        }
    } else {
        // Base-key hotkeys fire on key-down (the char itself still leaks; harmless).
        if (master.valid && master.base != 0 && base == master.base &&
            g_mods == master.mods) { toggleEnabled(); return; }
        if (over.valid && over.base != 0 && base == over.base &&
            g_mods == over.mods && overrideAllowedForCurrentApp()) {
            toggleOverrideForCurrentApp();
            return;
        }
    }

    Engine* eng = st.engine;
    if (!eng || !st.currentModeVN) return;

    // Phase 4 E.5: an editing/navigation shortcut (Ctrl/Alt/Super + key) can move
    // the caret or rewrite the text, so reset and let the key go. Bare typing
    // never holds these modifiers.
    if (g_mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) { eng->reset(); return; }

    // On Backspace, follow the documented behaviour: clear the composition and
    // let the app's native delete stand. The next key starts a fresh syllable.
    if (base == XK_BackSpace) { eng->reset(); return; }

    // Resolve the actual character honouring Shift/CapsLock/layout.
    XKeyEvent ke{};
    ke.type = KeyPress;
    ke.display = g_ctrl;
    ke.keycode = kc;
    ke.state = xstate;
    char buf[8] = {0};
    KeySym sym = 0;
    int n = XLookupString(&ke, buf, sizeof(buf), &sym, nullptr);
    unsigned char c = (n >= 1) ? (unsigned char)buf[0] : 0;

    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9');
    if (alnum) {
        KeyResult r = eng->onChar((char32_t)c, false);
        if (r.swallow)
            injectResult(r.backspaces + 1, r.commit);  // +1: erase the leaked key
        return;
    }
    // Space and printable punctuation end the word (engine resets, key passes).
    if (n >= 1 && c >= 0x20 && c != 0x7F) { eng->onChar(0, true); return; }
    if (isWordBreakKeysym(base)) { eng->onChar(0, true); return; }
    // Anything else (F-keys, Insert, ...) leaves the composition untouched.
}

void recordCallback(XPointer, XRecordInterceptData* data) {
    if (data->category != XRecordFromServer || data->client_swapped) {
        XRecordFreeData(data);
        return;
    }
    const xEvent* xe = reinterpret_cast<const xEvent*>(data->data);
    int type = xe->u.u.type & 0x7F;
    if (type == KeyPress || type == KeyRelease) {
        if (g_ignoreEvents > 0) {
            --g_ignoreEvents;           // one of our own injected events
        } else {
            handleKey(type, xe->u.u.detail, xe->u.keyButtonPointer.state);
        }
    } else if (type == ButtonPress) {
        // Phase 4 E.5: a mouse click can move the caret or change the focused
        // field, so drop the composition.
        AppState& st = state();
        if (st.config.gamingPolicy != GamingPolicy::Disabled && isGamingApp(st.currentApp)) {
            endGamingTypingSession(GamingEndReason::EndedMouse);
        }
        Engine* eng = st.engine;
        if (eng) eng->reset();
    }
    XRecordFreeData(data);
}

gboolean onRecordReadable(GIOChannel*, GIOCondition, gpointer) {
    if (g_data) XRecordProcessReplies(g_data);
    return TRUE;  // keep the watch alive
}

} // namespace

// Called by injector.cpp: skip the next `count` recorded key events.
void noteInjectedEvents(int count) { g_ignoreEvents += count; }

bool startHook() {
    g_ctrl = XOpenDisplay(nullptr);
    g_data = XOpenDisplay(nullptr);
    if (!g_ctrl || !g_data) return false;

    int major = 0, minor = 0;
    if (!XRecordQueryVersion(g_ctrl, &major, &minor)) return false;  // no XRecord

    XRecordClientSpec clients = XRecordAllClients;
    XRecordRange* range = XRecordAllocRange();
    if (!range) return false;
    // Phase 4 E.5: capture KeyPress..ButtonRelease so a mouse click also resets
    // (KeyPress=2, KeyRelease=3, ButtonPress=4, ButtonRelease=5).
    range->device_events.first = KeyPress;
    range->device_events.last = ButtonRelease;
    g_ctx = XRecordCreateContext(g_ctrl, 0, &clients, 1, &range, 1);
    XFree(range);
    if (!g_ctx) return false;
    XSync(g_ctrl, False);

    if (!XRecordEnableContextAsync(g_data, g_ctx, recordCallback, nullptr))
        return false;

    int fd = ConnectionNumber(g_data);
    g_channel = g_io_channel_unix_new(fd);
    g_watch = g_io_add_watch(g_channel, G_IO_IN, onRecordReadable, nullptr);
    return true;
}

void stopHook() {
    if (g_watch) { g_source_remove(g_watch); g_watch = 0; }
    if (g_channel) { g_io_channel_unref(g_channel); g_channel = nullptr; }
    if (g_ctx && g_ctrl) {
        XRecordDisableContext(g_ctrl, g_ctx);
        XRecordFreeContext(g_ctrl, g_ctx);
        g_ctx = 0;
    }
    if (g_data) { XCloseDisplay(g_data); g_data = nullptr; }
    if (g_ctrl) { XCloseDisplay(g_ctrl); g_ctrl = nullptr; }
}

} // namespace vietki::lin
