// Global low-level keyboard hook (guide 5.1, 5.2, 5.7). Runs on the message
// thread, so it stays light: a snapshot of config lives in AppState and the
// engine call + SendInput happen inline.
#include "app.h"

#include <commctrl.h> // HOTKEYF_* modifier flags

namespace vietki::win {

namespace {

// Phase 2 D.3 hotkeys. The msctls_hotkey32 control reports HOTKEYF_SHIFT/CONTROL/
// ALT in the high byte; build the same mask from the current modifier state so a
// stored hotkey can be matched on the hot path.
BYTE currentHotkeyMods() {
    BYTE m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= HOTKEYF_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= HOTKEYF_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= HOTKEYF_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
        m |= VIETKI_HOTKEYF_WIN;
    return m;
}

bool g_overrideHeld = false;       // de-bounce auto-repeat of the override hotkey
bool g_masterHeld = false;         // de-bounce auto-repeat of a combo master hotkey

// Modifier-only hotkey tracking for a modifier-only master toggle (e.g.
// Ctrl+Shift). A master toggle that includes a base key (Alt+Z) is matched on
// key-down instead and never reaches this tracker.
struct HotkeyTracker {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool win = false;
    bool otherUsed = false; // a non-modifier key was pressed during the hold
};
HotkeyTracker g_hk;

bool isCtrl(DWORD vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }
bool isShift(DWORD vk) { return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT; }
bool isAlt(DWORD vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }
bool isWin(DWORD vk) { return vk == VK_LWIN || vk == VK_RWIN; }
bool isModifier(DWORD vk) {
    return isCtrl(vk) || isShift(vk) || isAlt(vk) || isWin(vk) || vk == VK_CAPITAL;
}

// Returns true if releasing this modifier exactly completes the modifier-only
// hotkey described by `mods` (a HOTKEYF_* mask): every required modifier is
// held, no extra one is, and no other key was tapped during the hold.
bool modifierOnlyCompleted(DWORD vk, BYTE mods) {
    if (g_hk.otherUsed || mods == 0) return false;
    bool reqCtrl = (mods & HOTKEYF_CONTROL) != 0;
    bool reqShift = (mods & HOTKEYF_SHIFT) != 0;
    bool reqAlt = (mods & HOTKEYF_ALT) != 0;
    bool reqWin = (mods & VIETKI_HOTKEYF_WIN) != 0;
    if (g_hk.ctrl != reqCtrl || g_hk.shift != reqShift || g_hk.alt != reqAlt ||
        g_hk.win != reqWin)
        return false;
    return (reqCtrl && isCtrl(vk)) || (reqShift && isShift(vk)) ||
           (reqAlt && isAlt(vk)) || (reqWin && isWin(vk));
}

char32_t mapChar(DWORD vk, bool shift, bool caps) {
    if (vk >= 'A' && vk <= 'Z') {
        bool upper = shift ^ caps;
        char32_t base = U'a' + (vk - 'A');
        return upper ? (base - 32) : base;
    }
    if (vk >= '0' && vk <= '9' && !shift) {
        return U'0' + (vk - '0'); // bare digits feed VNI; harmless in Telex
    }
    return 0;
}

// Keys that end the current word and reset the engine buffer (guide 3.7).
bool isWordBreakKey(DWORD vk, bool shift) {
    switch (vk) {
        case VK_SPACE: case VK_TAB: case VK_RETURN:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_ESCAPE: case VK_DELETE: case VK_INSERT:
        case VK_OEM_1: case VK_OEM_2: case VK_OEM_3: case VK_OEM_4:
        case VK_OEM_5: case VK_OEM_6: case VK_OEM_7: case VK_OEM_PLUS:
        case VK_OEM_MINUS: case VK_OEM_COMMA: case VK_OEM_PERIOD:
            return true;
        default:
            break;
    }
    // Shifted digit row produces punctuation/symbols -> word break.
    if (vk >= '0' && vk <= '9' && shift) return true;
    return false;
}

// Phase 5: the physical trigger key is currently held, used to tell a real
// second tap from key auto-repeat (D.3). LL hooks carry no repeat flag.
bool g_gamingTriggerDown = false;

LRESULT CALLBACK proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION) return CallNextHookEx(nullptr, nCode, wParam, lParam);

    auto* p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    // Ignore anything we (or anyone) injected: this is the main anti-loop gate.
    if (p->flags & LLKHF_INJECTED)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    const DWORD vk = p->vkCode;

    AppState& st = state();

    // Phase 3 D.1: the master Vietnamese toggle can be any combination.
    BYTE masterMods = 0, masterVk = 0;
    if (st.config.toggleVietnameseHotkeyEnabled)
        parseHotkeyString(st.config.hotkey, masterMods, masterVk);

    // --- master toggle with a base key (e.g. Alt+Z): match on key-down and
    // swallow, so Alt+Z never rings the Windows menu bell. ---
    if (st.config.toggleVietnameseHotkeyEnabled &&
        masterVk != 0 && vk == masterVk && currentHotkeyMods() == masterMods) {
        if (keyDown) {
            if (!g_masterHeld) { g_masterHeld = true; toggleEnabled(); }
            return 1; // swallow
        }
        if (keyUp) { g_masterHeld = false; return 1; }
    }
    if (keyUp && vk == masterVk) g_masterHeld = false;

    // --- modifier-only master toggle tracking (e.g. Ctrl+Shift) ---
    if (keyDown) {
        if (isCtrl(vk)) g_hk.ctrl = true;
        else if (isShift(vk)) g_hk.shift = true;
        else if (isAlt(vk)) g_hk.alt = true;
        else if (isWin(vk)) g_hk.win = true;
        else g_hk.otherUsed = true;
    } else if (keyUp) {
        bool fire = st.config.toggleVietnameseHotkeyEnabled &&
                    masterVk == 0 && isModifier(vk) &&
                    modifierOnlyCompleted(vk, masterMods);
        if (isCtrl(vk)) g_hk.ctrl = false;
        else if (isShift(vk)) g_hk.shift = false;
        else if (isAlt(vk)) g_hk.alt = false;
        else if (isWin(vk)) g_hk.win = false;
        if (!g_hk.ctrl && !g_hk.shift && !g_hk.alt && !g_hk.win)
            g_hk.otherUsed = false;
        if (fire) {
            toggleEnabled();
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
    }

    // Toggle V- <-> V+ for Excluded Apps and, under the ToggleForCurrentApp
    // gaming policy, for Gaming Apps too (Phase 5 F.1). canToggleForCurrentApp()
    // encodes the precedence between the two lists.
    const WORD ov = st.config.toggleForCurrentAppHotkey;
    const BYTE mods = currentHotkeyMods();
    if (st.config.toggleForCurrentAppHotkeyEnabled && ov &&
        canToggleForCurrentApp(st.currentApp) &&
        LOBYTE(ov) == vk && HIBYTE(ov) == mods) {
        if (keyDown) {
            if (!g_overrideHeld) { g_overrideHeld = true; toggleOverrideForCurrentApp(); }
            return 1; // swallow
        }
        if (keyUp) { g_overrideHeld = false; return 1; }
    }

    // Phase 5 F.1: the temporary-trigger session is handled BEFORE the active
    // gate below, because a Gaming App is in English by design — placing this
    // after the `!currentModeVN` check would mean the trigger never ran.
    if (st.config.gamingPolicy == GamingPolicy::TemporaryTrigger &&
        gamingAppliesTo(st.currentApp)) {
        if (handleGamingTriggerAndSession(vk, p->scanCode, keyDown, keyUp))
            return 1; // swallowed by the gaming layer
        // Not swallowed: fall through. If the trigger just activated a session,
        // currentModeVN is now true and the engine block below handles this key.
    }

    if (!keyDown) return CallNextHookEx(nullptr, nCode, wParam, lParam);

    Engine* eng = st.engine;
    const bool active = eng && st.currentModeVN;

    // Bare modifiers never compose.
    if (isModifier(vk)) return CallNextHookEx(nullptr, nCode, wParam, lParam);

    if (!active) return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

    // Phase 5.1 E.1: in a per-game Unicode-paste app the engine's TRANSFORMS are
    // applied via the clipboard (pasteHandleKey), but plain ASCII keys are NOT
    // mangled by the game, so they type natively — instant, no clipboard. Every
    // boundary below that resets the engine must first flush deferred diacritics
    // so the on-screen text matches the engine before the boundary key passes.
    const bool pasteMode = gamingAppliesTo(st.currentApp) &&
                           isGamingPasteApp(st.currentApp);

    // Phase 4 E.2: an editing/navigation shortcut (Ctrl/Alt/Win + key, e.g.
    // Ctrl+A/C/V/X/Z/Y) can move the caret or change the text out from under us,
    // so the composition must not carry across it. Reset and let the key pass.
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool winDown = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
    if (ctrlDown || altDown || winDown) {
        if (pasteMode) resetPasteBaseline();
        eng->reset();
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (vk == VK_BACK) {
        if (pasteMode) resetPasteBaseline(); // flush deferred diacritics first
        KeyResult r = eng->onBackspace();
        if (r.swallow) {
            injectResult(r.backspaces, r.commit, st.useAutocompleteFix);
            return 1; // swallow original Backspace
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (isWordBreakKey(vk, shift)) {
        if (pasteMode) resetPasteBaseline();
        eng->onChar(0, true); // reset buffer; let the break key through
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    char32_t ch = mapChar(vk, shift, caps);
    if (ch == 0) {
        if (pasteMode) resetPasteBaseline();
        eng->reset(); // unknown printable: be safe and break the word
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    KeyResult r = eng->onChar(ch, false);
    if (pasteMode) {
        // Transforms paste via the clipboard (coalesced); plain ASCII types
        // natively. pasteHandleKey returns true when it handled (swallow) the key.
        if (pasteHandleKey(eng->display(), r.swallow)) return 1;
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    if (r.swallow) {
        injectResult(r.backspaces, r.commit, st.useAutocompleteFix);
        return 1; // swallow the original key
    }
    // Plain append of the typed (ASCII) key: let the OS deliver it natively.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Phase 4 E.3: a global low-level mouse hook. A button press (or wheel) can move
// the caret or switch the focused control even inside the same window, so the
// composition buffer must be dropped. We do no UI Automation here (guide E.3);
// we only reset and immediately pass the event on.
LRESULT CALLBACK mouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        switch (wParam) {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN: {
                // Phase 5 D.5: a click ends a live temporary session (which also
                // resets the engine); otherwise just drop the composition.
                AppState& st = state();
                if (st.gamingSession.state() != GamingTypingState::Idle)
                    endGamingTypingSession(GamingEndReason::EndedMouse);
                else if (st.engine) {
                    if (gamingAppliesTo(st.currentApp) &&
                        isGamingPasteApp(st.currentApp))
                        resetPasteBaseline();
                    st.engine->reset();
                }
                break;
            }
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                // Phase 5 D.5: the wheel never ends a session (the user may be
                // scrolling chat); it only resets the composition (Phase 4 E.3).
                AppState& st = state();
                if (st.engine) {
                    if (gamingAppliesTo(st.currentApp) &&
                        isGamingPasteApp(st.currentApp))
                        resetPasteBaseline();
                    st.engine->reset();
                }
                break;
            }
            default:
                break;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace

// Phase 5 C.2/F.1: does this physical key (scan code preferred, virtual key as a
// fallback) match the configured trigger, with no Ctrl/Alt/Win held? Shift is
// allowed because the binding is by physical key, not by produced character.
bool matchesGamingTrigger(UINT vk, UINT scanCode) {
    const TriggerBinding& t = state().config.gamingTrigger;
    bool keyMatch = (t.scanCode != 0 && scanCode == t.scanCode) ||
                    (t.vk != 0 && vk == t.vk);
    if (!keyMatch) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
        ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000))
        return false; // Ctrl/Alt/Win + this key is a shortcut, not the trigger
    return true;
}

// Phase 5 D/F.1: drive the temporary-trigger session for the focused gaming app.
// Returns true when the event is fully consumed (the caller swallows it).
bool handleGamingTriggerAndSession(UINT vk, UINT scanCode, bool keyDown, bool keyUp) {
    AppState& st = state();
    GamingSession& s = st.gamingSession;

    if (matchesGamingTrigger(vk, scanCode)) {
        if (keyDown) {
            bool isRepeat = g_gamingTriggerDown;
            g_gamingTriggerDown = true;
            GamingTypingState before = s.state();
            GamingAction a = s.onTriggerDown(isRepeat);
            if (before == GamingTypingState::Idle &&
                s.state() == GamingTypingState::Armed) {
                if (st.engine) st.engine->reset(); // F.2: do not feed the trigger
                applyResolvedState();              // icon -> G+ (Armed); pre-warms
                                                   // the paste session (Phase 5.1 E)
            }
            if (a.replayTrigger) replayPhysicalKey(vk, scanCode);
            if (before == GamingTypingState::Armed &&
                s.state() == GamingTypingState::Idle)
                applyResolvedState();              // double-trigger: icon back to G
            return a.swallow;
        }
        if (keyUp) {
            g_gamingTriggerDown = false;
            return s.onTriggerUp().swallow;
        }
        return false;
    }

    if (!keyDown) return false;

    // Session-ending keys (D.5). They still reach the game so chat is sent and
    // menus close; VietKi only resets first. Numpad Enter is also VK_RETURN.
    if (vk == VK_RETURN) {
        // Phase 5.1 E.1: commit any deferred diacritic into the still-focused game
        // before Enter sends the chat line (the session end below discards/restores).
        if (isGamingPasteApp(st.currentApp)) flushPendingPaste();
        endGamingTypingSession(GamingEndReason::EndedEnter);
        return false;
    }
    if (vk == VK_ESCAPE) { endGamingTypingSession(GamingEndReason::EndedEscape); return false; }

    if (isModifier(vk)) return false; // bare modifiers never affect the session

    // The first non-modifier key after arming starts the session; it is not
    // swallowed so the engine block below composes with this same key (F.2).
    if (s.state() == GamingTypingState::Armed) {
        GamingAction a = s.onNonModifierKey();
        if (a.activateVietnamese) {
            applyResolvedState();   // currentModeVN -> true, engine now active
            notifyGamingMode(GamingEndReason::Triggered);
        }
    }
    return false;
}

bool installKeyboardHook() {
    state().keyboardHook =
        SetWindowsHookExW(WH_KEYBOARD_LL, proc, GetModuleHandleW(nullptr), 0);
    return state().keyboardHook != nullptr;
}

void removeKeyboardHook() {
    if (state().keyboardHook) {
        UnhookWindowsHookEx(state().keyboardHook);
        state().keyboardHook = nullptr;
    }
}

bool installMouseHook() {
    state().mouseHook =
        SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
    return state().mouseHook != nullptr;
}

void removeMouseHook() {
    if (state().mouseHook) {
        UnhookWindowsHookEx(state().mouseHook);
        state().mouseHook = nullptr;
    }
}

} // namespace vietki::win
