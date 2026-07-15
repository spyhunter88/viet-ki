// CGEventTap keyboard interception (guide 6.2). Mirrors the Windows hook: it
// ignores its own injected events, maps the key to a character, runs the shared
// engine, and either swallows + re-injects or passes the key through.
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>

#include "app.h"
#include "typing_stats.h"

namespace vietki::mac {

namespace {

CFMachPortRef g_tap = nullptr;
CFRunLoopSourceRef g_source = nullptr;
uint64_t g_lastMods = 0;
bool g_masterOtherUsed = false;
bool g_gamingTriggerDown = false;

bool matchesGamingTrigger(CGKeyCode kc, uint64_t flags) {
    const Hotkey& t = state().config.gamingTrigger;
    if (!t.bound()) return false;
    if (kc != t.keycode) return false;
    // The trigger must be a bare key, so Ctrl/Alt/Cmd modifiers must NOT be held.
    // Shift is allowed because the binding is by physical key.
    if (flags & (kCGEventFlagMaskControl | kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand))
        return false;
    return true;
}

void replayPhysicalKey(CGKeyCode kc) {
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventRef down = CGEventCreateKeyboardEvent(src, kc, true);
    CGEventRef up = CGEventCreateKeyboardEvent(src, kc, false);
    
    CGEventSetIntegerValueField(down, kCGEventSourceUserData, kInjectMagic);
    CGEventSetIntegerValueField(up, kCGEventSourceUserData, kInjectMagic);
    
    CGEventPost(kCGSessionEventTap, down);
    CGEventPost(kCGSessionEventTap, up);
    
    CFRelease(down);
    CFRelease(up);
    CFRelease(src);
}

bool handleGamingTriggerAndSession(CGKeyCode kc, CGEventType type, uint64_t flags) {
    AppState& st = state();
    GamingSession& s = st.gamingSession;
    bool keyDown = (type == kCGEventKeyDown);
    bool keyUp = (type == kCGEventKeyUp);

    if (matchesGamingTrigger(kc, flags)) {
        if (keyDown) {
            bool isRepeat = g_gamingTriggerDown;
            g_gamingTriggerDown = true;
            GamingTypingState before = s.state();
            GamingAction a = s.onTriggerDown(isRepeat);
            if (before == GamingTypingState::Idle &&
                s.state() == GamingTypingState::Armed) {
                if (st.engine) st.engine->reset(); // F.2: do not feed the trigger
                applyResolvedState();
            }
            if (a.replayTrigger) {
                replayPhysicalKey(kc);
            }
            if (before == GamingTypingState::Armed &&
                s.state() == GamingTypingState::Idle) {
                applyResolvedState();
            }
            return a.swallow;
        }
        if (keyUp) {
            g_gamingTriggerDown = false;
            return s.onTriggerUp().swallow;
        }
        return false;
    }

    if (!keyDown) return false;

    // Session-ending keys (D.5).
    if (kc == kVK_Return || kc == kVK_ANSI_KeypadEnter) {
        if (isGamingPasteApp(st.currentApp)) flushPendingPaste();
        endGamingTypingSession(GamingEndReason::EndedEnter);
        return false;
    }
    if (kc == kVK_Escape) {
        endGamingTypingSession(GamingEndReason::EndedEscape);
        return false;
    }

    // Bare modifiers never affect the session.
    if (kc == 56 || kc == 60 || kc == 59 || kc == 62 || kc == 58 || kc == 61 || kc == 55 || kc == 54) {
        return false;
    }

    if (s.state() == GamingTypingState::Armed) {
        GamingAction a = s.onNonModifierKey();
        if (a.activateVietnamese) {
            applyResolvedState();
            notifyGamingMode(GamingEndReason::Triggered);
        }
    }
    return false;
}

// Modifier bits we care about for hotkeys (CGEventFlags): shift/control/option/cmd.
constexpr uint64_t kModMask =
    kCGEventFlagMaskShift | kCGEventFlagMaskControl |
    kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand;

// Does this key-down event match a configured Phase 2 hotkey (D.3)?
bool matchesHotkey(const Hotkey& hk, CGKeyCode kc, uint64_t flags) {
    if (!hk.bound()) return false;
    return hk.keycode == kc && (flags & kModMask) == (hk.mods & kModMask);
}

bool exactMods(const Hotkey& hk, uint64_t flags) {
    return (flags & kModMask) == (hk.mods & kModMask);
}

// kVK_Space is handled separately (Engine::onSpace(), Phase 6) so it can cache
// the committed word for a possible restore on the very next Backspace.
bool isWordBreakKeycode(CGKeyCode kc) {
    switch (kc) {
        case kVK_Return: case kVK_Tab: case kVK_Escape:
        case kVK_ANSI_KeypadEnter:
        case kVK_LeftArrow: case kVK_RightArrow: case kVK_UpArrow: case kVK_DownArrow:
        case kVK_Home: case kVK_End: case kVK_PageUp: case kVK_PageDown:
        case kVK_ForwardDelete:
            return true;
        default:
            return false;
    }
}

// Extract the typed character for letters/digits; 0 otherwise. Punctuation is
// reported as a word break by the caller.
char32_t mapChar(CGEventRef ev, bool& isPunctBreak) {
    UniChar buf[4] = {0};
    UniCharCount n = 0;
    CGEventKeyboardGetUnicodeString(ev, 4, &n, buf);
    isPunctBreak = false;
    if (n != 1) return 0;
    UniChar c = buf[0];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return (char32_t)c;
    // Any other single printable character ends the word.
    if (c >= 0x20 && c != 0x7F) isPunctBreak = true;
    return 0;
}

CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef event, void*) {
    // Re-enable if the system disabled the tap under load (guide 6.2).
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (g_tap) CGEventTapEnable(g_tap, true);
        return event;
    }

    AppState& st = state();

    // Phase 4 E.4: a mouse-down can move the caret or change the focused field,
    // even inside the same app, so the composition must be dropped.
    if (type == kCGEventLeftMouseDown || type == kCGEventRightMouseDown ||
        type == kCGEventOtherMouseDown) {
        if (st.config.gamingPolicy != GamingPolicy::Disabled && isGamingApp(st.currentApp)) {
            endGamingTypingSession(GamingEndReason::EndedMouse);
        }
        discardPendingPaste();
        if (st.engine) st.engine->reset();
        return event;
    }

    if (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged) return event;

    // Ignore our own injected events.
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kInjectMagic)
        return event;

    Engine* eng = st.engine;

    CGKeyCode kc = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    uint64_t flags = (uint64_t)CGEventGetFlags(event);
    uint64_t mods = flags & kModMask;

    // Phase 5 F.1: handle temporary-trigger session before active gate.
    if (st.config.gamingPolicy == GamingPolicy::TemporaryTrigger &&
        gamingAppliesTo(st.currentApp)) {
        if (handleGamingTriggerAndSession(kc, type, flags)) {
            return nil; // swallowed by gaming layer
        }
    }

    if (type == kCGEventKeyUp) return event;

    Hotkey master = st.config.toggleVietnameseHotkeyEnabled
                        ? hotkeyFromString(st.config.hotkey)
                        : Hotkey{};
    if (type == kCGEventFlagsChanged) {
        if (master.bound() && master.keycode == 0 && !g_masterOtherUsed &&
            g_lastMods == (master.mods & kModMask) && mods != g_lastMods) {
            toggleEnabled();
        }
        if (mods == 0) g_masterOtherUsed = false;
        g_lastMods = mods;
        return event;
    }

    bool repeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;

    if (!repeat && master.bound() && master.keycode != 0 &&
        master.keycode == kc && exactMods(master, flags)) {
        toggleEnabled();
        return nil;
    }
    if (master.bound() && master.keycode == 0 && mods == (master.mods & kModMask))
        g_masterOtherUsed = true;

    // Phase 2 D.3 override hotkey check.
    if (!repeat) {
        if (st.config.overrideHotkeyEnabled &&
            (gamingAppliesTo(st.currentApp) || (st.config.exclusionFeatureOn && isExcludedMember(st.currentApp))) &&
            matchesHotkey(st.config.overrideHotkey, kc, flags)) {
            toggleOverrideForCurrentApp();
            return nil; // swallow
        }
    }

    if (!eng || !st.currentModeVN) return event;

    const bool pasteMode = gamingAppliesTo(st.currentApp) && isGamingPasteApp(st.currentApp);

    // Editing/navigation shortcuts reset composition.
    if (mods & (kCGEventFlagMaskCommand | kCGEventFlagMaskControl |
                kCGEventFlagMaskAlternate)) {
        if (pasteMode) resetPasteBaseline();
        eng->reset();
        return event;
    }

    if (kc == kVK_Delete) { // Backspace
        if (st.config.typingStats) recordBackspace();
        if (pasteMode) resetPasteBaseline();
        KeyResult r = eng->onBackspace();
        if (r.swallow) {
            if (pasteMode) {
                pasteHandleKey(eng->display(), true);
            } else {
                injectResult(r.backspaces, r.commit);
            }
            return nil; // swallow
        }
        return event;
    }

    // Phase 6: Space gets its own entry point so it can cache the just-committed
    // word for a possible restore on the very next Backspace (PHASE6.md 3).
    if (kc == kVK_Space) {
        if (st.config.typingStats) recordWordCommitted(eng->display());
        if (pasteMode) resetPasteBaseline();
        eng->onSpace();
        return event;
    }

    if (isWordBreakKeycode(kc)) {
        if (st.config.typingStats) recordWordCommitted(eng->display());
        if (pasteMode) resetPasteBaseline();
        eng->onChar(0, true); // reset; let the key through
        return event;
    }

    bool punctBreak = false;
    char32_t ch = mapChar(event, punctBreak);
    if (ch == 0) {
        if (punctBreak) {
            if (st.config.typingStats) recordWordCommitted(eng->display());
            if (pasteMode) resetPasteBaseline();
            eng->onChar(0, true);
        }
        return event;
    }

    if (st.config.typingStats) recordKeystroke();
    KeyResult r = eng->onChar(ch, false);
    if (pasteMode) {
        if (pasteHandleKey(eng->display(), r.swallow)) {
            return nil; // swallowed
        }
    } else {
        if (r.swallow) {
            injectResult(r.backspaces, r.commit);
            return nil; // swallow original key
        }
    }
    return event;
}

} // namespace

bool startEventTap() {
    // Phase 4 E.4: also watch mouse-down so a click resets the composition.
    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) |
                       CGEventMaskBit(kCGEventKeyUp) |
                       CGEventMaskBit(kCGEventFlagsChanged) |
                       CGEventMaskBit(kCGEventLeftMouseDown) |
                       CGEventMaskBit(kCGEventRightMouseDown) |
                       CGEventMaskBit(kCGEventOtherMouseDown);
    g_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                             kCGEventTapOptionDefault, mask, callback, nullptr);
    if (!g_tap) return false; // missing Accessibility / Input Monitoring permission
    g_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), g_source, kCFRunLoopCommonModes);
    CGEventTapEnable(g_tap, true);
    return true;
}

void stopEventTap() {
    if (g_source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), g_source, kCFRunLoopCommonModes);
        CFRelease(g_source);
        g_source = nullptr;
    }
    if (g_tap) {
        CGEventTapEnable(g_tap, false);
        CFRelease(g_tap);
        g_tap = nullptr;
    }
}

bool ensureTapAlive() {
    // Healthy tap: nothing to do.
    if (g_tap && CGEventTapIsEnabled(g_tap)) return true;
    // Tap exists but is disabled: the cheap recovery is to just re-enable it.
    if (g_tap) {
        CGEventTapEnable(g_tap, true);
        if (CGEventTapIsEnabled(g_tap)) return true;
    }
    // Tap is missing or went silently inert (Tahoe race): rebuild from scratch.
    // Also covers the case where startEventTap() failed earlier because the
    // Accessibility/Input-Monitoring permission had not been granted yet.
    stopEventTap();
    return startEventTap();
}

} // namespace vietki::mac
