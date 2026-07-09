// Shared state and declarations for the VietKi Windows shell.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "vietki/engine.h"
#include "vietki/gaming.h"

namespace vietki::win {

// Phase 5 A/H: Gaming Mode policy. "Gaming Mode" is only a group name — there is
// no separate on/off switch; the policy is one of three mutually exclusive modes.
//   Disabled            the Gaming Apps list has no effect at all (B.3).
//   ToggleForCurrentApp games default to V-; the existing "toggle V-/V+ for the
//                       current app" hotkey forces V+ via Override::ForceVN (B.2).
//   TemporaryTrigger    games receive keys raw; a trigger key starts a short
//                       Vietnamese session driven by GamingSession (C/D).
enum class GamingPolicy { Disabled, ToggleForCurrentApp, TemporaryTrigger };

// Phase 5 C.2: the temporary-trigger binding, stored by physical key. The OEM
// keys recommended for the trigger (']' ';' '\'' '\\') change character with the
// keyboard layout, and games bind by physical key, so we keep the scan code as
// the primary identity and fall back to the virtual key.
struct TriggerBinding {
    UINT vk = 0;         // virtual key (VK_OEM_6 for ']' by default)
    UINT scanCode = 0;   // OEM scan code; preferred when matching the hot path
    UINT modifiers = 0;  // HOTKEYF_* mask; normally 0 (trigger is a bare key)
};

// Full Windows-side configuration (superset of the engine Config).
struct AppConfig {
    Method method = Method::Telex;
    TonePlacement tone = TonePlacement::Modern;
    bool enabled = true;            // master Vietnamese on/off (guide 5.7)
    bool spellCheck = true;         // Phase 3 E.2 spell checking
    // Phase 4 C.4: lock the rest of the word to literal after an explicit cancel.
    bool lockWordAfterCancel = true;
    // Phase 6: restore the word Space just committed if Backspace follows
    // immediately, so the user can keep correcting tones (see engine.h).
    bool restoreAfterSpace = true;
    // Phase 6 section 7: local-only typing stats (word counts, WPM, backspace
    // ratio). Off by default; collected data lives in its own file next to
    // config.ini, never here and never sent anywhere (see typing_stats.h).
    bool typingStats = false;
    bool autostart = false;
    // When autostart is on, launch elevated via a "highest privileges" logon task
    // instead of the HKCU Run value, so VietKi can drive admin/anti-cheat windows
    // from boot. Registering that task needs admin rights (config.cpp).
    bool autostartAdmin = false;
    bool autocompleteFix = true;
    bool soundOnGlobalToggle = true;    // beep when switching E <-> V globally
    bool soundOnExcludedToggle = true;  // beep when switching V- <-> V+ for excluded app
    // Whether the per-app exclusion list is honoured at all (Phase 2 D.2).
    bool exclusionFeatureOn = true;
    // Drop a per-app override when its app loses focus (Phase 2 D.4). Default off.
    bool revertOverrideOnBlur = false;
    // Master Vietnamese on/off hotkey (Phase 3 D.1). Now any combination, e.g.
    // "Ctrl+Shift", "Ctrl+Alt", "Alt+Z" or "Ctrl+Shift+Space". A modifier-only
    // value fires on release; a value with a base key fires on key-down and is
    // swallowed so e.g. Alt+Z does not ring the menu bell.
    std::wstring hotkey = L"Ctrl+Shift";
    bool toggleVietnameseHotkeyEnabled = true;
    // Phase 2 hotkeys, encoded the way the msctls_hotkey32 control reports them:
    // LOBYTE = virtual key, HIBYTE = HOTKEYF_* modifier mask. 0 means unbound.
    // Default Ctrl+Shift+Space: VK_SPACE (0x20) | (HOTKEYF_CONTROL|HOTKEYF_SHIFT)<<8.
    // Toggle V- <-> V+ for the focused app, but only when it is in Excluded Apps.
    WORD toggleForCurrentAppHotkey = 0x0320;
    bool toggleForCurrentAppHotkeyEnabled = true;
    WORD toggleExclusionHotkey = 0;          // legacy config key; no longer used
    std::vector<std::wstring> excludedProcesses = {
        L"devenv.exe", L"Code.exe", L"WindowsTerminal.exe", L"cmd.exe",
        L"powershell.exe"};
    std::vector<std::wstring> autocompleteProcesses = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe"};

    // --- Phase 5: Gaming Mode -------------------------------------------------
    GamingPolicy gamingPolicy = GamingPolicy::Disabled;
    // The default suggestion list is seeded exactly once, the first time a gaming
    // policy is enabled, and is the user's to edit from then on (B.4).
    bool gamingListInitialized = false;
    // Notify on automatic mode switches (TemporaryTrigger only). Off by default
    // so it never covers fullscreen game content (G.2).
    bool notifyOnAutomaticModeSwitch = false;
    // Resolved to the physical ']' key at load time when unset (C.1/H.1).
    TriggerBinding gamingTrigger;
    // The Gaming Apps list, kept independent of excludedProcesses (B.1).
    std::vector<std::wstring> gamingProcesses;
    // Games that require composed text to be pasted as CF_UNICODETEXT instead
    // of injected one UTF-16 key at a time. Must remain an explicit per-game opt-in.
    std::vector<std::wstring> gamingPasteProcesses;
    // Translucent on-screen badge while a gaming Vietnamese session is on (G+),
    // so the player is sure Vietnamese typing is active. Renders over
    // borderless-fullscreen games, not exclusive-fullscreen DirectX.
    bool gamingOverlayEnabled = true;
    int gamingOverlayCorner = 3; // 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right
    // Phase 5.1: beep when VietKi auto-switches Vietnamese on/off in a gaming
    // session — audible even in fullscreen, where a toast cannot show.
    bool soundOnGamingModeSwitch = false;
};

// The built-in Windows suggestion list, restored on demand (B.4). Lower-cased
// matching is done at the call sites; entries keep their display casing.
const std::vector<std::wstring>& defaultGamingProcesses();
// The subset (currently: all) of defaultGamingProcesses() that ships with the
// "Dán Unicode" (CF_UNICODETEXT + Ctrl+V) option pre-enabled.
const std::vector<std::wstring>& defaultGamingPasteProcesses();

// Temporary per-session override of the typing mode for one app (Phase 2 D.2).
// Windows currently only creates ForceVN for apps in Excluded Apps; ForceEN is
// kept as a legacy enum value so old in-memory/debug labels remain harmless.
enum class Override { None, ForceVN, ForceEN };

// Which status icon the tray currently shows (Phase 2 E + Phase 3).
//   V      master on, current app types Vietnamese.
//   E      master off — Vietnamese disabled globally (true "English everywhere").
//   VPlus  Vietnamese forced inside an otherwise-excluded app (override).
//   VMinus master on, but the current excluded app types English. Distinct from E
//          so "excluded app" no longer looks like "VietKi turned off".
//   Gaming   a Gaming App is active and typing English safely (G).
//   GamingVN a Gaming App has Vietnamese on — ForceVN in ToggleForCurrentApp,
//            or an Armed/Active session in TemporaryTrigger (G+).
enum class IconState { V, E, VPlus, VMinus, Gaming, GamingVN };

// Phase 5 G.2: why a temporary session changed, used to pick the notification.
enum class GamingEndReason { EnteredGame, Triggered, EndedEnter, EndedEscape,
                             EndedMouse, EndedFocus };

// Process-wide application state. The low-level hook reads this on the hot path,
// so it is kept as a plain struct already resident in RAM (guide 10).
struct AppState {
    Engine* engine = nullptr;
    AppConfig config;
    HWND messageWindow = nullptr;
    HWND settingsWindow = nullptr;  // modeless Settings dialog, or null
    HHOOK keyboardHook = nullptr;
    HHOOK mouseHook = nullptr;       // Phase 4 E.3: reset on mouse button down
    HWINEVENTHOOK focusHook = nullptr;
    std::wstring currentApp;        // foreground process .exe name, lower-cased
    HWND currentWindow = nullptr;    // Phase 4 E.3: foreground top-level window
    bool useAutocompleteFix = false; // foreground app needs selection-replace
    bool currentModeVN = true;      // resolved Vietnamese state for currentApp
    IconState currentIcon = IconState::V;
    // Per-app override, keyed by .exe name. RAM-only, never persisted (D.2).
    std::map<std::wstring, Override> perAppOverride;
    // Phase 5: temporary-trigger session for the focused gaming context. RAM-only.
    GamingSession gamingSession;
};

AppState& state();

// Command-line flag appended to both autostart launch mechanisms (the HKCU Run
// value and the elevated logon task). Its presence means "started at logon" — the
// app stays silently in the tray; its absence means a manual launch (double-click)
// and the Settings window is surfaced. Shared verbatim by config.cpp (which writes
// it into the launch commands) and main.cpp (which detects it).
constexpr wchar_t kAutostartArg[] = L"--autostart";

// config.cpp
std::wstring configPath();
void loadConfig(AppConfig& cfg);
void saveConfig(const AppConfig& cfg);
// Point/clear the non-elevated HKCU Run logon value at the current VietKi.exe.
void applyAutostart(bool enable);
// Reconcile both logon mechanisms (HKCU Run + the elevated "highest privileges"
// scheduled task) with cfg, leaving at most one active. Returns false only when
// the elevated task needs to be created or removed but the process is not
// elevated — the caller should then offer to relaunch as admin.
bool reconcileAutostart(const AppConfig& cfg);
// True if the elevated logon task is currently registered.
bool autostartAdminTaskExists();
// True if the current process is running elevated (admin token).
bool isProcessElevated();

// Parse a hotkey string such as "Alt+Z" or "Ctrl+Shift" into a HOTKEYF_* modifier
// mask and a virtual-key code (0 = modifier-only). Returns false if nothing
// usable was found. Defined in config.cpp, shared with the hook (D.1).
bool parseHotkeyString(const std::wstring& s, BYTE& mods, BYTE& vk);
std::wstring normalizeHotkeyString(const std::wstring& s); // canonical form, or empty

// Local extension bit stored beside HOTKEYF_* flags for Win-key hotkeys.
constexpr BYTE VIETKI_HOTKEYF_WIN = 0x08;

// hook.cpp
bool installKeyboardHook();
void removeKeyboardHook();
bool installMouseHook();    // Phase 4 E.3
void removeMouseHook();

// injector.cpp — execute a KeyResult against the focused control.
void injectResult(int backspaces, const std::u32string& commit, bool useSelectionReplace);
void sendUnicodeString(const std::u32string& text);
void sendBackspaces(int n);
// Phase 5 F.4: replay one physical key press (down+up) for the double-trigger
// escape. The injected events carry LLKHF_INJECTED so the hook ignores them.
void replayPhysicalKey(UINT vk, UINT scanCode);

// paste_inject.cpp — per-game Unicode clipboard injection (Phase 5.1 E).
// Only engine TRANSFORMS (diacritics/tones, r.swallow) are pasted via the
// clipboard; plain ASCII keys type natively for instant feedback. Adjacent
// fast-burst transforms are coalesced into one paste (pasteHandleKey).
bool beginPasteSession();
void endPasteSession();
bool injectResultPaste(int backspaces, const std::u32string& commit);
// Hot path: handle one composed key. display = engine display(), transform =
// r.swallow. Returns true if pasted (swallow the key), false to type natively.
bool pasteHandleKey(const std::u32string& display, bool transform);
void flushPendingPaste();   // paste any deferred diacritic now (game still focused)
void resetPasteBaseline();  // flush, then forget the current syllable's text
void discardPendingPaste(); // drop deferred text WITHOUT pasting (focus lost)
void onPasteFlushTimer();   // WM_TIMER handler: kPasteFlushTimer (pause fallback)
void onPasteRestoreTimer(); // WM_TIMER handler: kPasteRestoreTimer (restore clipboard)

// Timer IDs shared between paste_inject.cpp and the message window in main.cpp.
constexpr UINT_PTR kPasteFlushTimer = 2;
constexpr UINT_PTR kPasteRestoreTimer = 3;

// overlay.cpp — Phase 5 translucent "Tiếng Việt" badge over the game (G+).
void initGamingOverlay();
void destroyGamingOverlay();
void updateGamingOverlay();   // show/hide + reposition from the resolved state

// tray.cpp
bool createTray(HWND owner);
void destroyTray();
// Re-register the tray icon after Explorer (re)creates the taskbar — used both on
// the "TaskbarCreated" broadcast and when the shell was not yet ready at startup.
void readdTrayIcon();
void updateTrayIcon();      // reflect the resolved V / E / V+ / G / G+ state
void showTrayMenu(HWND owner);
// Phase 5 G.2: a non-activating tray notification, de-duplicated over a short
// window so repeated focus events do not spam the user.
void showModeNotification(const std::wstring& text);

// settings.cpp — the Phase 2 Settings window (C) and drag-to-exclude picker (A).
void openSettings();
void refreshSettingsWindow();   // re-read state into the open dialog, if any

// main.cpp helpers shared with tray/hook/settings
void toggleEnabled();                  // flip the master VN/EN switch
void setMethod(Method m);
void setTonePlacement(TonePlacement t);
void refreshForegroundApp();           // recompute resolved mode, reset engine, repaint
void toggleOverrideForCurrentApp();    // D.3 override hotkey action
void toggleExclusionFeature();         // D.3 exclusion-feature toggle
void applyResolvedState();             // push resolved mode to engine + icon
bool isExcludedMember(const std::wstring& app); // membership in the list (any case)
void playGlobalToggleSound(bool vietnameseNow);
void playExcludedToggleSound(bool forceVietnameseNow);

// --- Phase 5: Gaming Mode helpers (main.cpp) -------------------------------
bool isGamingApp(const std::wstring& app);       // membership in Gaming Apps
bool isGamingPasteApp(const std::wstring& app);  // per-game Unicode paste opt-in
bool gamingAppliesTo(const std::wstring& app);   // policy != Disabled && gaming app
// Whether the "toggle V-/V+ for current app" action applies to this app, given
// both Excluded Apps and the Gaming policy (F.1). Gaming apps in TemporaryTrigger
// are deliberately excluded — only the trigger drives them.
bool canToggleForCurrentApp(const std::wstring& app);
// Seed the suggestion list once on first enable; no-op afterwards (B.4).
void seedGamingListIfNeeded();
// Switch policy with the required cleanup: seed once, clear gaming ForceVN and
// reset the session when the mode actually changes (B.2).
void applyGamingPolicy(GamingPolicy policy);
// End the temporary session (idempotent) and re-resolve state (F.3).
void endGamingTypingSession(GamingEndReason reason);
// Emit a mode-switch notification for the given reason, subject to the user's
// notification option and the active policy (G.2).
void notifyGamingMode(GamingEndReason reason);
// hook.cpp: handle the trigger and session for a TemporaryTrigger gaming app.
// Returns true if the event was fully consumed (swallowed).
bool handleGamingTriggerAndSession(UINT vk, UINT scanCode, bool keyDown, bool keyUp);
bool matchesGamingTrigger(UINT vk, UINT scanCode); // hot-path trigger compare

// Window messages.
constexpr UINT WM_VIETKI_TRAY = WM_APP + 1;
// Posted to the running instance's message window by a second launch so it surfaces
// its UI instead of the second process exiting silently.
constexpr UINT WM_VIETKI_SHOW_SETTINGS = WM_APP + 2;

} // namespace vietki::win
