// Shared state for the VietKi Linux (X11) shell. Mirrors the Windows/macOS shells
// (guide 6, Phase 2 A/C/D/E) but built around X11: an XRecord passive monitor
// feeds the shared core engine and XTest injects the corrected text.
//
// NOTE: this shell is written to mirror the verified Windows build and the
// macOS shell, but has not been compiled on Linux in this environment.
//
// This header is kept free of X11/GTK headers so it can be included anywhere;
// hotkeys are stored as human strings and parsed inside hook.cpp.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "vietki/engine.h"
#include "vietki/gaming.h"

namespace vietki::lin {

enum class GamingPolicy { Disabled, ToggleForCurrentApp, TemporaryTrigger };

enum class GamingEndReason { EnteredGame, Triggered, EndedEnter, EndedEscape, EndedMouse, EndedFocus };

// Temporary per-session override of the typing mode for one app (Phase 2 D.2).
enum class Override { Default, ForceVN, ForceEN };

// Which status icon to show (Phase 2 E + Phase 3 + Phase 5).
//   V      master on, current app types Vietnamese.
//   E      master off — Vietnamese disabled globally.
//   VPlus  Vietnamese forced inside an otherwise-excluded app (override).
//   VMinus master on, but the current excluded app types English. Distinct from
//          E so an excluded app no longer looks like "VietKi turned off".
//   Gaming/GamingVN = Gaming mode active
enum class IconState { V, E, VPlus, VMinus, Gaming, GamingVN };

// Full Linux-side configuration (superset of the engine Config). Hotkeys are
// kept as strings (e.g. "Ctrl+Shift", "Ctrl+Alt+Space") and parsed in hook.cpp.
// On Linux the per-app identity is the focused window's WM_CLASS (res_class),
// lower-cased — the analogue of the Windows .exe name / macOS bundle id.
struct AppConfig {
    Method method = Method::Telex;
    TonePlacement tone = TonePlacement::Modern;
    bool enabled = true;            // master Vietnamese on/off (guide 5.7)
    bool spellCheck = true;         // Phase 3 E.2 spell checking
    bool lockWordAfterCancel = true; // Phase 4 C.4
    bool autostart = false;         // honoured via the XDG autostart .desktop file
    bool autocompleteFix = true;    // reserved; selection-replace is a no-op on X11
    bool soundOnGlobalToggle = true;    // beep when switching E <-> V globally
    bool soundOnExcludedToggle = true;  // beep when switching V- <-> V+ for an app
    bool exclusionFeatureOn = true;     // Phase 2 D.2: honour the exclusion list
    bool revertOverrideOnBlur = false;  // Phase 2 D.4: drop override on blur
    // Master Vietnamese on/off hotkey. A modifier-only value ("Ctrl+Shift")
    // fires on release; a value with a base key ("Alt+Z") fires on key-down.
    std::string hotkey = "Ctrl+Shift";
    bool toggleVietnameseHotkeyEnabled = true;
    // Toggle V- <-> V+ for the focused app, but only when it is in the list.
    std::string overrideHotkey = "Ctrl+Alt+Space";
    bool overrideHotkeyEnabled = true;
    // Excluded / autocomplete apps, by lower-cased WM_CLASS res_class.
    std::vector<std::string> excludedClasses = {
        "gnome-terminal", "gnome-terminal-server", "konsole", "xterm",
        "alacritty", "kitty", "code", "org.gnome.terminal"};
    std::vector<std::string> autocompleteClasses = {
        "google-chrome", "chromium", "firefox"};

    // Phase 5: Gaming configuration
    GamingPolicy gamingPolicy = GamingPolicy::Disabled;
    bool gamingListInitialized = false;
    std::string gamingTrigger = "]"; // default to ']'
    std::vector<std::string> gamingClasses;
    bool soundOnGamingModeSwitch = true;
    bool notifyOnAutomaticModeSwitch = true;
    bool gamingOverlayEnabled = true;
    int gamingOverlayCorner = 3; // 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right
};

// Process-wide application state, read by the XRecord callback on the hot path,
// so it is kept as a plain struct already resident in RAM (guide 10).
struct AppState {
    Engine* engine = nullptr;
    AppConfig config;
    std::string currentApp;          // focused window WM_CLASS, lower-cased
    bool currentModeVN = true;       // resolved Vietnamese state for currentApp
    IconState currentIcon = IconState::V;
    // Per-app override keyed by WM_CLASS. RAM-only, never persisted (D.2).
    std::map<std::string, Override> perAppOverride;
    GamingSession gamingSession;
};

AppState& state();

// hook.cpp — XRecord passive keyboard monitor + XTest-backed correction.
bool startHook();
void stopHook();

// injector.cpp — execute a KeyResult against the focused control. Because an
// X11 passive monitor cannot swallow the original key, the leaked character is
// erased by the caller via `extraBackspaces` (see hook.cpp).
void injectResult(int backspaces, const std::u32string& commit);
void injectBackspaces(int n);
void replayPhysicalKey(unsigned keycode);

// overlay.cpp — translucent G+ badge while temporary Vietnamese is active.
void initGamingOverlay();
void destroyGamingOverlay();
void updateGamingOverlay();

// tray.cpp — the AppIndicator status menu (Phase 2 E).
bool createTray();
void updateTrayIcon();   // reflect the resolved V / E / V+ / V- state

// main.cpp helpers shared with the hook / tray.
void toggleEnabled();                  // flip the master VN/EN switch
void setMethod(Method m);
void setTonePlacement(TonePlacement t);
void refreshForegroundApp(const std::string& wmClass);  // recompute resolved mode
void toggleOverrideForCurrentApp();    // Phase 2 D.3 override action
void toggleExclusionFeature();         // Phase 2 D.3 exclusion-feature toggle
void applyResolvedState();             // push resolved mode to engine + icon
bool isExcludedMember(const std::string& wmClass);
void playGlobalToggleSound(bool vietnameseNow);
void playExcludedToggleSound(bool forceVietnameseNow);

// Gaming mode helpers
bool isGamingApp(const std::string& wmClass);
bool isGamingPasteApp(const std::string& wmClass); // compatibility
bool gamingAppliesTo(const std::string& wmClass);
void applyGamingPolicy(GamingPolicy policy);
void notifyGamingMode(GamingEndReason reason);
void endGamingTypingSession(GamingEndReason reason);
std::vector<std::string> defaultGamingClasses();

// config (in main.cpp)
std::string configPath();
void loadConfig(AppConfig& cfg);
void saveConfig(const AppConfig& cfg);
void applyAutostart(bool enable);

} // namespace vietki::lin
