// Shared state for the VietKi macOS shell. Mirrors the Windows shell (guide 6,
// Phase 2 A/C/D/E). NOTE: this shell is written to mirror the verified Windows
// build but has not been compiled on macOS in this environment.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "vietki/engine.h"
#include "vietki/gaming.h"

namespace vietki::mac {

// Tag stamped onto our own injected events so the tap can ignore them and avoid
// a feedback loop (guide 6.2).
constexpr int64_t kInjectMagic = 0x564B; // 'VK'

// A global hotkey: a key code plus a modifier mask in CGEventFlags bits. A zero
// keycode means "unbound". Stored numerically so it survives config round-trips.
struct Hotkey {
    uint16_t keycode = 0;
    uint64_t mods = 0;
    bool bound() const { return keycode != 0 || mods != 0; }
};

enum class GamingPolicy { Disabled, ToggleForCurrentApp, TemporaryTrigger };

enum class GamingEndReason { EnteredGame, Triggered, EndedEnter, EndedEscape, EndedMouse, EndedFocus };

struct AppConfig {
    Method method = Method::Telex;
    TonePlacement tone = TonePlacement::Modern;
    bool enabled = true;            // master VN/EN switch
    bool spellCheck = true;         // Phase 3 E.2 spell checking
    bool lockWordAfterCancel = true; // Phase 4 C.4
    bool autostart = false;
    bool autocompleteFix = true;
    bool soundOnGlobalToggle = true;    // beep when switching E <-> V globally
    bool soundOnExcludedToggle = true;  // beep when switching V- <-> V+ in excluded app
    bool exclusionFeatureOn = true;       // Phase 2 D.2
    bool revertOverrideOnBlur = false;    // Phase 2 D.4
    std::string hotkey = "Ctrl+Shift";
    bool toggleVietnameseHotkeyEnabled = true;
    // Default override: Ctrl+Option+Space (kVK_Space = 49).
    Hotkey overrideHotkey{49, /*control*/ (1ull << 18) | /*option*/ (1ull << 19)};
    bool overrideHotkeyEnabled = true;
    Hotkey toggleExclusionHotkey{};       // unbound by default
    // On macOS the exclusion list holds bundle identifiers (guide 6.5).
    std::vector<std::string> excludedBundles = {
        "com.apple.Terminal", "com.googlecode.iterm2", "com.microsoft.VSCode"};
    std::vector<std::string> autocompleteBundles = {
        "com.google.Chrome", "com.apple.Safari", "org.mozilla.firefox"};

    // Phase 5: Gaming configuration
    GamingPolicy gamingPolicy = GamingPolicy::Disabled;
    bool gamingListInitialized = false;
    Hotkey gamingTrigger{30, 0}; // default to ']' (kVK_ANSI_RightBracket = 30)
    std::vector<std::string> gamingBundles;
    std::vector<std::string> gamingPasteBundles;
    bool gamingOverlayEnabled = true;
    int gamingOverlayCorner = 3; // 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right
    bool soundOnGamingModeSwitch = true;
    bool notifyOnAutomaticModeSwitch = true;
};

// Temporary per-session override of the typing mode for one app (Phase 2 D.2).
enum class Override { None, ForceVN, ForceEN };

// Which menu-bar status icon to show (Phase 2 E + Phase 3 + Phase 5).
//   V/E/VPlus as Phase 2; VMinus = master on but this app types English
//   (excluded or forceEN) — distinct from E (master off everywhere).
//   Gaming/GamingVN = Gaming mode active
enum class IconState { V, E, VPlus, VMinus, Gaming, GamingVN };

struct AppState {
    Engine* engine = nullptr;
    AppConfig config;
    std::string currentApp;          // foreground bundle identifier
    bool currentModeVN = true;       // resolved Vietnamese state for currentApp
    IconState currentIcon = IconState::V;
    // Per-app override keyed by bundle id. RAM-only, never persisted (D.2).
    std::map<std::string, Override> perAppOverride;
    GamingSession gamingSession;
};

AppState& state();

// EventTap.mm
bool startEventTap();
void stopEventTap();

// Injector.mm
void injectResult(int backspaces, const std::u32string& commit);

// main.mm helpers (also used by the menu / settings window)
void toggleEnabled();
void setMethod(Method m);
void setTonePlacement(TonePlacement t);
void refreshForegroundApp(const std::string& bundleId);
void toggleOverrideForCurrentApp();   // Phase 2 D.3
void toggleExclusionFeature();        // Phase 2 D.3
void applyResolvedState();            // push resolved mode to engine + icon
bool isExcludedMember(const std::string& bundleId);
void playGlobalToggleSound(bool vietnameseNow);
void playExcludedToggleSound(bool forceVietnameseNow);

// Gaming mode helpers
bool isGamingApp(const std::string& bundleId);
bool isGamingPasteApp(const std::string& bundleId);
bool gamingAppliesTo(const std::string& bundleId);
void applyGamingPolicy(GamingPolicy policy);
void notifyGamingMode(GamingEndReason reason);
void endGamingTypingSession(GamingEndReason reason);
std::vector<std::string> defaultGamingBundles();

// PasteInject.mm
bool pasteHandleKey(const std::u32string& display, bool transform);
void flushPendingPaste();
void discardPendingPaste();
void resetPasteBaseline();

// config (in main.mm)
std::string configPath();
void loadConfig(AppConfig& cfg);
void saveConfig(const AppConfig& cfg);
Hotkey hotkeyFromString(const std::string& s);
std::string stringFromHotkey(const Hotkey& h);
std::string keyNameFromKeycode(uint16_t keycode);

// settings window (Settings.mm) — Phase 2 C
void openSettings();
void refreshSettingsWindow();

} // namespace vietki::mac
