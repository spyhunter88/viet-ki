// VietKi Windows shell entry point: message-only window, foreground tracking, and
// the message loop that owns the low-level keyboard hook (guide 5).
#include "app.h"

#include <commctrl.h>
#include <ole2.h>
#include <psapi.h>

#include <algorithm>
#include <string>

#include "resource.h"
#include "typing_stats.h"

namespace vietki::win {

namespace {

AppState g_state;
constexpr UINT_PTR kSingleClickTimer = 1;
constexpr UINT kSingleClickDelayMs = 220;
bool g_ignoreNextLeftUpAfterDoubleClick = false;
// Explorer broadcasts this registered message to every top-level window when the
// taskbar is (re)created, so we can re-add a tray icon that vanished after an
// Explorer restart (or that never appeared because the shell was not yet ready).
UINT g_taskbarCreatedMsg = 0;

std::wstring foregroundProcessName() {
    HWND fg = GetForegroundWindow();
    if (!fg) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        std::wstring full(path, len);
        size_t slash = full.find_last_of(L"\\/");
        name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(h);
    return name;
}

bool listContains(const std::vector<std::wstring>& list, const std::wstring& name) {
    for (const auto& e : list)
        if (_wcsicmp(e.c_str(), name.c_str()) == 0) return true;
    return false;
}

std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// True if this process was started by one of the logon autostart mechanisms,
// which both pass kAutostartArg on the command line (config.cpp). lpCmdLine holds
// only the arguments (the exe name is stripped), so a plain double-click — whether
// from Explorer or a shortcut — never carries the flag and surfaces the UI.
bool launchedViaAutostart(const wchar_t* cmdLine) {
    if (!cmdLine || !*cmdLine) return false;
    return toLower(cmdLine).find(kAutostartArg) != std::wstring::npos;
}

void CALLBACK focusChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    refreshForegroundApp();
}

// EVENT_OBJECT_FOCUS: focus moved within (or into) some window. In a Chromium
// browser the omnibox and web content live in one HWND, so this is the only
// signal that the caret crossed between the address bar and the page. Re-run the
// UIA check only for detected browsers (avoids a UIA call on every desktop-wide
// focus change); everything else is definitively "not the omnibox".
void CALLBACK objectFocusChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                 DWORD, DWORD) {
    // updateOmniboxFocus() reads the true global focus (not this event's object),
    // so re-evaluating on any focus change in a detected browser is self-correct;
    // it only runs on focus changes, never on the typing hot path.
    if (g_state.omniboxDetect) updateOmniboxFocus();
    else clearOmniboxFocus();
}

// Look up the current per-app override (None if the app has no entry).
Override overrideFor(const std::wstring& app) {
    auto it = g_state.perAppOverride.find(app);
    return (it == g_state.perAppOverride.end()) ? Override::None : it->second;
}

// Phase 2 D.2 + Phase 5 E: resolve the effective Vietnamese state, with the
// Gaming Apps branch taking priority over the Excluded-Apps logic.
bool resolveModeVN() {
    if (!g_state.config.enabled) return false;             // master off
    const std::wstring& app = g_state.currentApp;
    if (gamingAppliesTo(app)) {
        if (g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
            return overrideFor(app) == Override::ForceVN;
        // TemporaryTrigger: Vietnamese only during an active session.
        return g_state.gamingSession.state() == GamingTypingState::Active;
    }
    if (overrideFor(app) == Override::ForceVN) return true;
    if (g_state.config.exclusionFeatureOn && isExcludedMember(app))
        return false;
    return true;
}

// Phase 2 E + Phase 3 + Phase 5 G.1: pick the status icon. Gaming Apps show
// G / G+; otherwise the master switch maps to E, an excluded app to V-/V+.
IconState resolveIcon() {
    if (!g_state.config.enabled) return IconState::E;   // master off → English globally
    const std::wstring& app = g_state.currentApp;
    if (gamingAppliesTo(app)) {
        if (g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
            return overrideFor(app) == Override::ForceVN ? IconState::GamingVN
                                                         : IconState::Gaming;
        // Armed counts as G+: the user has armed and the next key types Vietnamese.
        return g_state.gamingSession.state() == GamingTypingState::Idle
                   ? IconState::Gaming
                   : IconState::GamingVN;
    }
    if (!g_state.currentModeVN) return IconState::VMinus; // this app is English, master on
    if (g_state.config.exclusionFeatureOn && isExcludedMember(app) &&
        overrideFor(app) == Override::ForceVN)
        return IconState::VPlus; // Vietnamese inside an otherwise-excluded app
    return IconState::V;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Registered messages are runtime values, so they cannot be switch cases.
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0) {
        readdTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_VIETKI_SHOW_SETTINGS:
            // A second launch asked us to surface our UI. openSettings() brings an
            // already-open window to the front, so an open window is not re-created.
            openSettings();
            return 0;
        case WM_VIETKI_TRAY:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                KillTimer(hwnd, kSingleClickTimer);
                showTrayMenu(hwnd);
            } else if (lParam == WM_LBUTTONUP) {
                if (g_ignoreNextLeftUpAfterDoubleClick) {
                    g_ignoreNextLeftUpAfterDoubleClick = false;
                    return 0;
                }
                SetTimer(hwnd, kSingleClickTimer, kSingleClickDelayMs, nullptr);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                KillTimer(hwnd, kSingleClickTimer);
                g_ignoreNextLeftUpAfterDoubleClick = true;
                openSettings();
            }
            return 0;
        case WM_TIMER:
            if (wParam == kSingleClickTimer) {
                KillTimer(hwnd, kSingleClickTimer);
                toggleEnabled();
                return 0;
            }
            if (wParam == kPasteFlushTimer) {
                onPasteFlushTimer();   // pause fallback for a deferred diacritic
                return 0;
            }
            if (wParam == kPasteRestoreTimer) {
                onPasteRestoreTimer(); // deferred original-clipboard restore
                return 0;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            saveTypingStats(); // message thread, safe point for the one disk write
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

AppState& state() { return g_state; }

bool isExcludedMember(const std::wstring& app) {
    return listContains(g_state.config.excludedProcesses, app);
}

// Push the resolved mode to the engine (which resets its buffer) and repaint the
// tray icon. Call this after anything that can change the resolved state.
void applyResolvedState() {
    g_state.currentModeVN = resolveModeVN();
    g_state.currentIcon = resolveIcon();
    if (g_state.engine)
        g_state.engine->setConfig(Config{g_state.config.method, g_state.config.tone,
                                         g_state.currentModeVN,
                                         g_state.config.spellCheck,
                                         g_state.config.lockWordAfterCancel,
                                         g_state.config.restoreAfterSpace,
                                         g_state.config.fixWholeWord});
    // Phase 5.1 E: keep the per-game paste session (clipboard snapshot) warm from
    // the moment the trigger arms — not just once Vietnamese goes active — so the
    // first text key only does a lightweight write + Ctrl+V instead of also taking
    // the clipboard snapshot, which used to race the first key and drop its output
    // ("first 't' shows nothing").
    const bool gamingPasteCtx = gamingAppliesTo(g_state.currentApp) &&
                                isGamingPasteApp(g_state.currentApp);
    const bool gamingSessionLive =
        g_state.gamingSession.state() != GamingTypingState::Idle;
    if (gamingPasteCtx && (g_state.currentModeVN || gamingSessionLive))
        beginPasteSession();
    else
        endPasteSession();
    updateTrayIcon();
    updateGamingOverlay();
    refreshSettingsWindow();
}

void refreshForegroundApp() {
    std::wstring prev = g_state.currentApp;
    // Phase 4 E.3: track the foreground top-level window, not just the process,
    // so switching windows inside the same app (e.g. two chrome.exe windows)
    // still resets the composition below via applyResolvedState().
    HWND prevWindow = g_state.currentWindow;
    g_state.currentWindow = GetForegroundWindow();
    std::wstring name = toLower(foregroundProcessName());
    // Phase 5 D.5: any foreground process or top-level window change ends a live
    // temporary session before we re-resolve for the new focus.
    if (g_state.currentWindow != prevWindow || name != prev) {
        // Phase 5 D.5/E.1: focus moved — drop any deferred gaming paste (never
        // inject it into the window we switched to) and end a live session before
        // re-resolving. applyResolvedState() below resets the engine and (when
        // leaving a paste app) restores the clipboard.
        discardPendingPaste();
        if (g_state.gamingSession.state() != GamingTypingState::Idle) {
            g_state.gamingSession.reset();
            notifyGamingMode(GamingEndReason::EndedFocus);
        }
    }
    // Phase 2 D.4: optionally drop the previous app's override as we leave it.
    if (g_state.config.revertOverrideOnBlur && !prev.empty() && prev != name)
        g_state.perAppOverride.erase(prev);
    g_state.currentApp = name;
    g_state.useAutocompleteFix =
        g_state.config.autocompleteFix &&
        listContains(g_state.config.autocompleteProcesses, name) &&
        _wcsicmp(name.c_str(), L"excel.exe") != 0;
    // Chromium browsers render the omnibox and web content in one HWND, so the
    // Shift+Left autocomplete fix must be refined per focused control via UIA:
    // apply it only in the address bar, use plain Backspace in web content
    // (Notion/Docs). Prime the verdict now; EVENT_OBJECT_FOCUS keeps it current.
    g_state.omniboxDetect = _wcsicmp(name.c_str(), L"chrome.exe") == 0 ||
                            _wcsicmp(name.c_str(), L"msedge.exe") == 0;
    if (g_state.omniboxDetect) updateOmniboxFocus();
    else clearOmniboxFocus();
    applyResolvedState(); // also resets the engine for the new focus (guide 3.7)
    // Phase 5 G.2: announce entering a game under the temporary-trigger policy.
    if (prev != name && gamingAppliesTo(name) &&
        g_state.config.gamingPolicy == GamingPolicy::TemporaryTrigger)
        notifyGamingMode(GamingEndReason::EnteredGame);
}

void toggleEnabled() {
    g_state.config.enabled = !g_state.config.enabled;
    // Phase 5 D.5: turning the master off ends any live temporary session.
    if (!g_state.config.enabled) g_state.gamingSession.reset();
    saveConfig(g_state.config);
    applyResolvedState();
    playGlobalToggleSound(g_state.config.enabled);
}

void setMethod(Method m) {
    g_state.config.method = m;
    saveConfig(g_state.config);
    applyResolvedState();
}

void setTonePlacement(TonePlacement t) {
    g_state.config.tone = t;
    saveConfig(g_state.config);
    applyResolvedState();
}

// Toggle V- <-> V+ for the focused app. Applies to Excluded Apps and, in the
// ToggleForCurrentApp gaming policy, to Gaming Apps as well (Phase 5 F.1).
void toggleOverrideForCurrentApp() {
    const std::wstring& app = g_state.currentApp;
    if (app.empty()) return;
    if (!canToggleForCurrentApp(app)) return;
    Override cur = overrideFor(app);
    bool forceVN = cur != Override::ForceVN;
    g_state.perAppOverride[app] = forceVN ? Override::ForceVN : Override::None;
    applyResolvedState();
    playExcludedToggleSound(forceVN);
}

// --- Phase 5: Gaming Mode helpers ------------------------------------------
bool isGamingApp(const std::wstring& app) {
    return listContains(g_state.config.gamingProcesses, app);
}

bool isGamingPasteApp(const std::wstring& app) {
    return listContains(g_state.config.gamingPasteProcesses, app);
}

bool gamingAppliesTo(const std::wstring& app) {
    return g_state.config.gamingPolicy != GamingPolicy::Disabled && isGamingApp(app);
}

bool canToggleForCurrentApp(const std::wstring& app) {
    // A gaming-controlled app obeys the gaming policy: manual V-/V+ only exists
    // in ToggleForCurrentApp; TemporaryTrigger is driven solely by the trigger.
    if (gamingAppliesTo(app))
        return g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp;
    // Otherwise the Phase 2 Excluded-Apps rule is unchanged.
    return g_state.config.exclusionFeatureOn && isExcludedMember(app);
}

void seedGamingListIfNeeded() {
    AppConfig& c = g_state.config;
    if (c.gamingListInitialized) return;
    if (c.gamingProcesses.empty()) {
        c.gamingProcesses = defaultGamingProcesses();
        c.gamingPasteProcesses = defaultGamingPasteProcesses();
    }
    c.gamingListInitialized = true; // never auto-seed again (B.4)
}

void applyGamingPolicy(GamingPolicy p) {
    AppConfig& c = g_state.config;
    if (p != GamingPolicy::Disabled) seedGamingListIfNeeded();
    if (p != c.gamingPolicy) {
        // ForceVN (from ToggleForCurrentApp) and a live session must never
        // coexist for a gaming context, so clear both on any mode change (B.2).
        for (const auto& g : c.gamingProcesses)
            g_state.perAppOverride.erase(toLower(g));
        g_state.gamingSession.reset();
    }
    c.gamingPolicy = p;
    applyResolvedState();
}

void notifyGamingMode(GamingEndReason reason) {
    const AppConfig& c = g_state.config;
    // Only the automatic temporary-trigger policy notifies (G.2); manual V-/V+
    // keeps the existing sound feedback.
    if (c.gamingPolicy != GamingPolicy::TemporaryTrigger) return;
    // Phase 5.1: a beep is audible even in fullscreen, where the toast cannot
    // show. Vietnamese just turned on only for the Triggered transition.
    if (c.soundOnGamingModeSwitch)
        MessageBeep(reason == GamingEndReason::Triggered ? MB_ICONASTERISK
                                                         : MB_ICONHAND);
    if (!c.notifyOnAutomaticModeSwitch) return;
    const wchar_t* msg = nullptr;
    switch (reason) {
        case GamingEndReason::EnteredGame:
            msg = L"Gaming Mode — Tiếng Việt đang tắt"; break;
        case GamingEndReason::Triggered:
            msg = L"Gõ tiếng Việt tạm thời"; break;
        case GamingEndReason::EndedEnter:
        case GamingEndReason::EndedEscape:
        case GamingEndReason::EndedMouse:
        case GamingEndReason::EndedFocus:
            msg = L"Gaming Mode — Đã tắt gõ tiếng Việt"; break;
    }
    if (msg) showModeNotification(msg);
}

void endGamingTypingSession(GamingEndReason reason) {
    // Idempotent: ending an Idle session does nothing and fires no notification.
    if (g_state.gamingSession.state() == GamingTypingState::Idle) return;
    g_state.gamingSession.onContextLost();
    if (g_state.engine) g_state.engine->reset();
    applyResolvedState();
    notifyGamingMode(reason);
}

void toggleExclusionFeature() {
    g_state.config.exclusionFeatureOn = !g_state.config.exclusionFeatureOn;
    saveConfig(g_state.config);
    applyResolvedState();
}

void playGlobalToggleSound(bool vietnameseNow) {
    if (!g_state.config.soundOnGlobalToggle) return;
    MessageBeep(vietnameseNow ? MB_ICONASTERISK : MB_ICONHAND);
}

void playExcludedToggleSound(bool forceVietnameseNow) {
    if (!g_state.config.soundOnExcludedToggle) return;
    MessageBeep(forceVietnameseNow ? MB_ICONEXCLAMATION : MB_OK);
}

} // namespace vietki::win

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    using namespace vietki;
    using namespace vietki::win;

    // A logon autostart launch (HKCU Run value or the elevated task) carries
    // kAutostartArg and must stay quietly in the tray; a manual double-click has no
    // flag and surfaces the UI. Holds for both the normal and the admin path, since
    // both launch mechanisms append the same flag.
    const bool autostartLaunch = launchedViaAutostart(lpCmdLine);

    // Single instance guard. When an instance is already running, a manual relaunch
    // (double-click) asks it to show its window instead of exiting silently — even
    // if the running instance is hidden in the tray, this surfaces its UI (bringing
    // the window to front if already open). An autostart launch that races an
    // already-running instance just exits without disturbing the tray.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"VietKiSingletonMutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!autostartLaunch) {
            HWND running = FindWindowW(L"VietKiMessageWindow", nullptr);
            if (running) PostMessageW(running, WM_VIETKI_SHOW_SETTINGS, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    // DPI awareness (Per-Monitor v2) is declared in app.manifest (D.3 §10), so we
    // do not call SetProcessDPIAware() here — the manifest wins and is applied
    // before any window exists.

    // Register the common controls the Settings dialog uses: hotkey box, tab
    // control, list-view, and the standard themed button/edit/combo classes.
    INITCOMMONCONTROLSEX icc = {sizeof(icc),
                                ICC_HOTKEY_CLASS | ICC_LISTVIEW_CLASSES |
                                    ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);

    AppState& st = state();
    loadConfig(st.config);
    loadTypingStats();
    // Keep both logon mechanisms in lockstep with the config: refresh the HKCU
    // Run path and ensure the elevated task matches. When launched by that task
    // we are already elevated, so an out-of-date task is repaired here; a missing
    // task while non-elevated is left for the Settings dialog to fix as admin.
    reconcileAutostart(st.config);

    static Engine engine(Config{st.config.method, st.config.tone, st.config.enabled,
                                st.config.spellCheck, st.config.lockWordAfterCancel,
                                st.config.restoreAfterSpace, st.config.fixWholeWord});
    st.engine = &engine;

    // Resolve the "TaskbarCreated" broadcast so wndProc can re-add the tray icon
    // after an Explorer restart. Same string -> same value in every process.
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Hidden top-level window for tray callbacks and the message loop. It must be a
    // real top-level window (not HWND_MESSAGE) because message-only windows do not
    // receive broadcast messages such as "TaskbarCreated". WS_EX_TOOLWINDOW keeps it
    // off the taskbar and Alt+Tab, and it is never shown, so it stays invisible.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_VIETKI));
    wc.lpszClassName = L"VietKiMessageWindow";
    RegisterClassW(&wc);
    st.messageWindow = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"VietKi",
                                       WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                       hInstance, nullptr);
    if (!st.messageWindow) return 1;

    // When we run elevated (e.g. launched by the "VietKi Autostart" scheduled task
    // with highest privileges), UIPI blocks messages coming from lower-integrity
    // processes. Without lifting the filter we would never receive Explorer's
    // "TaskbarCreated" broadcast (tray icon stays gone after a restart) nor a
    // WM_VIETKI_SHOW_SETTINGS post from a normal double-click relaunch (UI never
    // surfaces). Allow exactly those two messages through. Harmless when not
    // elevated. ChangeWindowMessageFilterEx is unavailable on very old Windows, so
    // resolve it dynamically and skip silently if missing.
    if (auto allow = reinterpret_cast<BOOL(WINAPI*)(HWND, UINT, DWORD, void*)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "ChangeWindowMessageFilterEx"))) {
        constexpr DWORD kMsgfltAllow = 1; // MSGFLT_ALLOW
        allow(st.messageWindow, g_taskbarCreatedMsg, kMsgfltAllow, nullptr);
        allow(st.messageWindow, WM_VIETKI_SHOW_SETTINGS, kMsgfltAllow, nullptr);
    }

    createTray(st.messageWindow);
    initGamingOverlay();
    updateTrayIcon();

    if (!installKeyboardHook()) {
        MessageBoxW(nullptr, L"Không cài được hook bàn phím.", L"VietKi", MB_ICONERROR);
        destroyTray();
        return 1;
    }
    installMouseHook(); // Phase 4 E.3: reset on click; non-fatal if it fails.

    st.focusHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                   nullptr, focusChanged, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Track focus moves within a window too, so the omnibox<->web-content switch
    // inside a Chromium browser re-runs the UIA address-bar check (main.cpp
    // objectFocusChanged). Non-fatal if it fails: selectionReplace() then just
    // keeps the last cached verdict.
    st.focusObjHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
                                      nullptr, objectFocusChanged, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    refreshForegroundApp();
    // Only a manual launch opens the Settings window; a logon autostart launch
    // stays in the tray. The tray icon (createTray above) remains the entry point,
    // and a later double-click surfaces the UI via the single-instance guard.
    if (!autostartLaunch) openSettings();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Let the modeless Settings dialog handle tab/arrow navigation itself.
        if (st.settingsWindow && IsDialogMessageW(st.settingsWindow, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.focusHook) UnhookWinEvent(st.focusHook);
    if (st.focusObjHook) UnhookWinEvent(st.focusObjHook);
    shutdownOmniboxProbe();
    removeMouseHook();
    removeKeyboardHook();
    endPasteSession();
    onPasteRestoreTimer(); // the loop is gone; restore the clipboard synchronously now
    destroyGamingOverlay();
    destroyTray();
    if (mutex) ReleaseMutex(mutex);
    OleUninitialize();
    return 0;
}
