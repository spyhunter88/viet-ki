// VietKi Linux (X11) shell entry point (guide 6, Phase 2 A/C/D/E). A background
// process with an AppIndicator status menu, driven by an XRecord monitor over
// the shared core engine. Mirrors the Windows and macOS shells.
//
// NOTE: this shell mirrors the verified Windows build but has not been compiled
// on Linux in this environment.
#include <gtk/gtk.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>

#include "app.h"

using namespace vietki;
using namespace vietki::lin;

namespace vietki::lin {

namespace {

AppState g_state;
Display* g_util = nullptr;             // utility connection: active window + bell
std::string g_lastSeenClass;
Window g_lastActiveWindow = 0;         // Phase 4 E.5: track the window, not just class

Override overrideFor(const std::string& app) {
    auto it = g_state.perAppOverride.find(app);
    return (it == g_state.perAppOverride.end()) ? Override::Default : it->second;
}

std::string trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string stripInlineComment(std::string s) {
    bool inQuote = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') inQuote = !inQuote;
        if (!inQuote && (s[i] == ';' || s[i] == '#')) return s.substr(0, i);
    }
    return s;
}

std::string readKey(const std::string& line) {
    size_t eq = line.find('=');
    return trim(eq == std::string::npos ? line : line.substr(0, eq));
}

std::string readValue(const std::string& line) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return {};
    std::string value = trim(stripInlineComment(line.substr(eq + 1)));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

bool parseBool(const std::string& value, bool def) {
    std::string v = lower(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

std::string boolText(bool value) { return value ? "true" : "false"; }

std::string configDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? xdg
                     : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".")
                           + "/.config";
    return base + "/vietki";
}

std::string autostartPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? xdg
                     : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".")
                           + "/.config";
    return base + "/autostart/vietki.desktop";
}

// Phase 2 D.2 + Phase 5 E: resolve the effective Vietnamese state, with the
// Gaming Apps branch taking priority over the Excluded-Apps logic.
bool resolveModeVN() {
    if (!g_state.config.enabled) return false;
    const std::string& app = g_state.currentApp;
    if (gamingAppliesTo(app)) {
        if (g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
            return overrideFor(app) == Override::ForceVN;
        return g_state.gamingSession.state() == GamingTypingState::Active;
    }
    if (overrideFor(app) == Override::ForceVN) return true;
    if (g_state.config.exclusionFeatureOn && isExcludedMember(app))
        return false;
    return true;
}

// Phase 2 E + Phase 3 + Phase 5 G.1: pick the status icon.
IconState resolveIcon() {
    if (!g_state.config.enabled) return IconState::E;       // master off
    const std::string& app = g_state.currentApp;
    if (gamingAppliesTo(app)) {
        if (g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
            return overrideFor(app) == Override::ForceVN ? IconState::GamingVN : IconState::Gaming;
        return g_state.gamingSession.state() == GamingTypingState::Idle ? IconState::Gaming : IconState::GamingVN;
    }
    if (!g_state.currentModeVN) return IconState::VMinus;    // this app English
    if (g_state.config.exclusionFeatureOn && isExcludedMember(app) &&
        overrideFor(app) == Override::ForceVN)
        return IconState::VPlus;
    return IconState::V;
}

// The _NET_ACTIVE_WINDOW id, or 0 if none/unsupported.
Window activeWindowId() {
    if (!g_util) return 0;
    Window root = DefaultRootWindow(g_util);
    Atom net = XInternAtom(g_util, "_NET_ACTIVE_WINDOW", True);
    if (net == None) return 0;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(g_util, root, net, 0, 1, False, XA_WINDOW, &actualType,
                           &actualFormat, &nItems, &bytesAfter, &prop) != Success ||
        !prop)
        return 0;
    Window active = *reinterpret_cast<Window*>(prop);
    XFree(prop);
    return active;
}

// WM_CLASS res_class of a window, lower-cased.
std::string classOfWindow(Window w) {
    if (!w || !g_util) return {};
    std::string result;
    XClassHint hint{};
    if (XGetClassHint(g_util, w, &hint)) {
        if (hint.res_class) result = lower(hint.res_class);
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
    }
    return result;
}

gboolean pollActiveWindow(gpointer) {
    // Phase 4 E.5: reset when the active *window* changes, even if the new
    // window has the same WM_CLASS (e.g. two terminal windows). The composition
    // must never carry across a focus change.
    Window active = activeWindowId();
    if (active != g_lastActiveWindow) {
        g_lastActiveWindow = active;
        std::string cls = classOfWindow(active);
        g_lastSeenClass = cls;
        if (g_state.engine) g_state.engine->reset();
        refreshForegroundApp(cls); // also resets via applyResolvedState
    }
    return TRUE;  // keep polling
}

bool isValidConfig(const std::string& text) {
    if (text.empty()) {
        return true;
    }

    const unsigned char* bytes = (const unsigned char*)text.data();
    const unsigned char* end = bytes + text.size();
    while (bytes < end) {
        if (bytes[0] == 0x00)
            return false;
        if (bytes[0] < 0x80) {
            if (bytes[0] < 0x20 && bytes[0] != 0x09 && bytes[0] != 0x0A &&
                bytes[0] != 0x0D) {
                return false;
            }
            bytes++;
        } else if ((bytes[0] & 0xE0) == 0xC0) {
            if (bytes + 1 >= end) return false;
            if ((bytes[1] & 0xC0) != 0x80) return false;
            bytes += 2;
        } else if ((bytes[0] & 0xF0) == 0xE0) {
            if (bytes + 2 >= end) return false;
            if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) return false;
            bytes += 3;
        } else if ((bytes[0] & 0xF8) == 0xF0) {
            if (bytes + 3 >= end) return false;
            if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 ||
                (bytes[3] & 0xC0) != 0x80)
                return false;
            bytes += 4;
        } else {
            return false;
        }
    }

    std::string lowerText = text;
    for (auto& c : lowerText) {
        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
    }

    if (lowerText.find("[caidat]") == std::string::npos &&
        lowerText.find("[settings]") == std::string::npos &&
        lowerText.find("[excludedbundles]") == std::string::npos &&
        lowerText.find("[apploaitru]") == std::string::npos) {
        return false;
    }

    return true;
}

} // namespace

AppState& state() { return g_state; }

bool isExcludedMember(const std::string& wmClass) {
    for (const auto& c : g_state.config.excludedClasses)
        if (c == wmClass) return true;
    return false;
}

std::string configPath() { return configDir() + "/config.ini"; }

void loadConfig(AppConfig& cfg) {
    std::ifstream in(configPath(), std::ios::binary);
    if (!in) { saveConfig(cfg); return; }

    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    in.close();

    if (!isValidConfig(text)) {
        GtkWidget* dialog = gtk_message_dialog_new(
            nullptr,
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_CLOSE,
            "File config.ini đã bị lỗi hoặc chứa ký tự không hợp lệ. VietKi sẽ sao lưu file lỗi thành config.ini.bak và khôi phục cấu hình mặc định."
        );
        gtk_window_set_title(GTK_WINDOW(dialog), "VietKi");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        std::string path = configPath();
        std::string backupPath = path + ".bak";
        std::remove(backupPath.c_str());
        std::rename(path.c_str(), backupPath.c_str());

        AppConfig defaultCfg;
        saveConfig(defaultCfg);
        cfg = defaultCfg;
        return;
    }

    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    std::istringstream lines(text);
    std::string line, section;
    bool sawExcluded = false, sawAutocomplete = false, sawGaming = false;

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string raw = trim(line);
        if (raw.empty() || raw[0] == ';' || raw[0] == '#') continue;

        if (raw.front() == '[' && raw.back() == ']') {
            section = lower(trim(raw.substr(1, raw.size() - 2)));
            if (section == "apploaitru" || section == "excludedclasses") {
                cfg.excludedClasses.clear();
                sawExcluded = true;
            } else if (section == "autocomplete") {
                cfg.autocompleteClasses.clear();
                sawAutocomplete = true;
            } else if (section == "ungdunggame" || section == "gamingclasses") {
                cfg.gamingClasses.clear();
                cfg.gamingListInitialized = true;
                sawGaming = true;
            }
            continue;
        }

        std::string key = readKey(raw);
        if (key.empty()) continue;
        std::string keyLower = lower(key);
        std::string value = readValue(raw);

        if (section == "caidat" || section == "settings") {
            if (keyLower == "method")
                cfg.method = (value == "VNI") ? Method::VNI
                           : (value == "VIQR" ? Method::VIQR : Method::Telex);
            else if (keyLower == "tone")
                cfg.tone = (value == "Old") ? TonePlacement::Old : TonePlacement::Modern;
            else if (keyLower == "enabled") cfg.enabled = parseBool(value, cfg.enabled);
            else if (keyLower == "spellcheck") cfg.spellCheck = parseBool(value, cfg.spellCheck);
            else if (keyLower == "lockwordaftercancel")
                cfg.lockWordAfterCancel = parseBool(value, cfg.lockWordAfterCancel);
            else if (keyLower == "autostart") cfg.autostart = parseBool(value, cfg.autostart);
            else if (keyLower == "autocompletefix")
                cfg.autocompleteFix = parseBool(value, cfg.autocompleteFix);
            else if (keyLower == "soundonglobaltoggle")
                cfg.soundOnGlobalToggle = parseBool(value, cfg.soundOnGlobalToggle);
            else if (keyLower == "soundonexcludedtoggle")
                cfg.soundOnExcludedToggle = parseBool(value, cfg.soundOnExcludedToggle);
            else if (keyLower == "exclusionfeatureon")
                cfg.exclusionFeatureOn = parseBool(value, cfg.exclusionFeatureOn);
            else if (keyLower == "revertoverrideonblur")
                cfg.revertOverrideOnBlur = parseBool(value, cfg.revertOverrideOnBlur);
            else if (keyLower == "togglevietnameseenabled" ||
                     keyLower == "togglevietnamesehotkeyenabled")
                cfg.toggleVietnameseHotkeyEnabled =
                    parseBool(value, cfg.toggleVietnameseHotkeyEnabled);
            else if (keyLower == "toggleforcurrentappenabled" ||
                     keyLower == "overridehotkeyenabled")
                cfg.overrideHotkeyEnabled = parseBool(value, cfg.overrideHotkeyEnabled);
            else if (keyLower == "togglevietnamese" || keyLower == "hotkey") {
                if (!value.empty()) cfg.hotkey = value;
            } else if (keyLower == "toggleforcurrentapp" || keyLower == "overridehotkey") {
                if (!value.empty()) cfg.overrideHotkey = value;
            } else if (keyLower == "gamingpolicy") {
                std::string v = lower(trim(value));
                if (v == "toggleforcurrentapp") cfg.gamingPolicy = GamingPolicy::ToggleForCurrentApp;
                else if (v == "temporarytrigger") cfg.gamingPolicy = GamingPolicy::TemporaryTrigger;
                else cfg.gamingPolicy = GamingPolicy::Disabled;
            } else if (keyLower == "gamingtrigger") {
                if (!value.empty()) cfg.gamingTrigger = value;
            } else if (keyLower == "soundongamingmodeswitch") {
                cfg.soundOnGamingModeSwitch = parseBool(value, cfg.soundOnGamingModeSwitch);
            } else if (keyLower == "notifyonautomaticmodeswitch") {
                cfg.notifyOnAutomaticModeSwitch = parseBool(value, cfg.notifyOnAutomaticModeSwitch);
            } else if (keyLower == "gamingoverlayenabled") {
                cfg.gamingOverlayEnabled = parseBool(value, cfg.gamingOverlayEnabled);
            } else if (keyLower == "gamingoverlaycorner") {
                try {
                    int corner = std::stoi(value);
                    if (corner >= 0 && corner <= 3) cfg.gamingOverlayCorner = corner;
                } catch (...) {
                }
            }
        } else if (section == "apploaitru" || section == "excludedclasses") {
            if (sawExcluded) cfg.excludedClasses.push_back(lower(key));
        } else if (section == "autocomplete") {
            if (sawAutocomplete) cfg.autocompleteClasses.push_back(lower(key));
        } else if (section == "ungdunggame" || section == "gamingclasses") {
            if (sawGaming) cfg.gamingClasses.push_back(lower(key));
        }
    }
}

void saveConfig(const AppConfig& cfg) {
    g_mkdir_with_parents(configDir().c_str(), 0700);
    std::ostringstream out;
    out << "; VietKi Linux configuration file.\n";
    out << "; Stored in ~/.config/vietki/config.ini. Lines starting with ';' or '#'\n";
    out << "; are comments. Excluded/autocomplete apps are matched by WM_CLASS.\n\n";

    out << "[Caidat]\n";
    out << "method=" << (cfg.method == Method::VNI ? "VNI"
                        : cfg.method == Method::VIQR ? "VIQR" : "Telex") << "\n";
    out << "tone=" << (cfg.tone == TonePlacement::Old ? "Old" : "Modern") << "\n";
    out << "enabled=" << boolText(cfg.enabled) << "\n";
    out << "spellCheck=" << boolText(cfg.spellCheck) << "\n";
    out << "lockWordAfterCancel=" << boolText(cfg.lockWordAfterCancel) << "\n";
    out << "autostart=" << boolText(cfg.autostart) << "\n";
    out << "autocompleteFix=" << boolText(cfg.autocompleteFix) << "\n";
    out << "soundOnGlobalToggle=" << boolText(cfg.soundOnGlobalToggle) << "\n";
    out << "soundOnExcludedToggle=" << boolText(cfg.soundOnExcludedToggle) << "\n";
    out << "exclusionFeatureOn=" << boolText(cfg.exclusionFeatureOn) << "\n";
    out << "revertOverrideOnBlur=" << boolText(cfg.revertOverrideOnBlur) << "\n";
    out << "toggleVietnameseEnabled=" << boolText(cfg.toggleVietnameseHotkeyEnabled) << "\n";
    out << "toggleVietnamese=" << cfg.hotkey << "\n";
    out << "toggleForCurrentAppEnabled=" << boolText(cfg.overrideHotkeyEnabled) << "\n";
    out << "toggleForCurrentApp=" << cfg.overrideHotkey << "\n";

    std::string gpStr = "disabled";
    if (cfg.gamingPolicy == GamingPolicy::ToggleForCurrentApp) gpStr = "toggleForCurrentApp";
    else if (cfg.gamingPolicy == GamingPolicy::TemporaryTrigger) gpStr = "temporaryTrigger";
    out << "gamingPolicy=" << gpStr << "\n";
    out << "gamingTrigger=" << cfg.gamingTrigger << "\n";
    out << "soundOnGamingModeSwitch=" << boolText(cfg.soundOnGamingModeSwitch) << "\n";
    out << "notifyOnAutomaticModeSwitch=" << boolText(cfg.notifyOnAutomaticModeSwitch) << "\n";
    out << "gamingOverlayEnabled=" << boolText(cfg.gamingOverlayEnabled) << "\n";
    out << "gamingOverlayCorner=" << cfg.gamingOverlayCorner << "\n\n";

    out << "[Apploaitru]\n";
    out << "; One excluded WM_CLASS per line, e.g. gnome-terminal=\n";
    for (const auto& s : cfg.excludedClasses)
        if (!trim(s).empty()) out << s << "=\n";
    out << "\n[Autocomplete]\n";
    for (const auto& s : cfg.autocompleteClasses)
        if (!trim(s).empty()) out << s << "=\n";
    out << "\n[Ungdunggame]\n";
    for (const auto& s : cfg.gamingClasses)
        if (!trim(s).empty()) out << s << "=\n";

    std::ofstream f(configPath(), std::ios::binary | std::ios::trunc);
    f << out.str();
}

void applyAutostart(bool enable) {
    std::string path = autostartPath();
    if (!enable) { std::remove(path.c_str()); return; }
    std::string dir = path.substr(0, path.find_last_of('/'));
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::ofstream f(path, std::ios::trunc);
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=VietKi\n"
      << "Comment=Vietnamese input method\n"
      << "Exec=vietki\n"
      << "Terminal=false\n"
      << "X-GNOME-Autostart-enabled=true\n";
}

// --- state mutators (shared with the hook + tray) -------------------------

void applyResolvedState() {
    g_state.currentModeVN = resolveModeVN();
    g_state.currentIcon = resolveIcon();
    if (g_state.engine)
        g_state.engine->setConfig(Config{g_state.config.method, g_state.config.tone,
                                         g_state.currentModeVN,
                                         g_state.config.spellCheck,
                                         g_state.config.lockWordAfterCancel});
    updateTrayIcon();
    updateGamingOverlay();
}

void refreshForegroundApp(const std::string& wmClass) {
    std::string prev = g_state.currentApp;
    if (g_state.config.revertOverrideOnBlur && !prev.empty() && prev != wmClass)
        g_state.perAppOverride.erase(prev);
    if (prev != wmClass && g_state.gamingSession.state() != GamingTypingState::Idle)
        g_state.gamingSession.onContextLost();
    g_state.currentApp = wmClass;
    if (prev != wmClass) {
        g_state.gamingSession.reset();
    }
    applyResolvedState();
}

void toggleEnabled() {
    g_state.config.enabled = !g_state.config.enabled;
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

void toggleOverrideForCurrentApp() {
    const std::string& app = g_state.currentApp;
    if (app.empty()) return;
    bool isGaming = gamingAppliesTo(app) && g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp;
    bool isExcluded = g_state.config.exclusionFeatureOn && isExcludedMember(app);
    if (!isGaming && !isExcluded) return;
    bool forceVN = overrideFor(app) != Override::ForceVN;
    g_state.perAppOverride[app] = forceVN ? Override::ForceVN : Override::Default;
    applyResolvedState();
    playExcludedToggleSound(forceVN);
}

void toggleExclusionFeature() {
    g_state.config.exclusionFeatureOn = !g_state.config.exclusionFeatureOn;
    saveConfig(g_state.config);
    applyResolvedState();
}

void playGlobalToggleSound(bool) {
    if (g_state.config.soundOnGlobalToggle && g_util) XBell(g_util, 0);
}
void playExcludedToggleSound(bool) {
    if (g_state.config.soundOnExcludedToggle && g_util) XBell(g_util, 0);
}

bool isGamingApp(const std::string& wmClass) {
    std::string lowerClass = lower(wmClass);
    for (const auto& c : g_state.config.gamingClasses) {
        std::string pattern = lower(trim(c));
        if (pattern.empty()) continue;
        if (pattern.back() == '*') {
            pattern.pop_back();
            if (!pattern.empty() && lowerClass.rfind(pattern, 0) == 0) return true;
        } else if (pattern == lowerClass) {
            return true;
        }
    }
    return false;
}

bool isGamingPasteApp(const std::string&) {
    return false;
}

bool gamingAppliesTo(const std::string& wmClass) {
    return g_state.config.gamingPolicy != GamingPolicy::Disabled && isGamingApp(wmClass);
}

void applyGamingPolicy(GamingPolicy p) {
    AppConfig& c = g_state.config;
    if (p != GamingPolicy::Disabled) {
        if (!c.gamingListInitialized) {
            if (c.gamingClasses.empty()) {
                c.gamingClasses = defaultGamingClasses();
            }
            c.gamingListInitialized = true;
        }
    }
    if (p != c.gamingPolicy) {
        for (const auto& g : c.gamingClasses) {
            g_state.perAppOverride.erase(g);
        }
        g_state.gamingSession.reset();
    }
    c.gamingPolicy = p;
    saveConfig(c);
    applyResolvedState();
}

void notifyGamingMode(GamingEndReason reason) {
    const AppConfig& c = g_state.config;
    if (c.gamingPolicy != GamingPolicy::TemporaryTrigger) return;
    if (c.soundOnGamingModeSwitch && g_util) {
        XBell(g_util, 0);
        if (reason == GamingEndReason::Triggered) {
            XFlush(g_util);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            XBell(g_util, 0);
        }
        XFlush(g_util);
    }
}

void endGamingTypingSession(GamingEndReason reason) {
    if (g_state.gamingSession.state() == GamingTypingState::Idle) return;
    g_state.gamingSession.onContextLost();
    if (g_state.engine) g_state.engine->reset();
    applyResolvedState();
    notifyGamingMode(reason);
}

std::vector<std::string> defaultGamingClasses() {
    return {
        "steam_app_*", "steam", "steamwebhelper", "lutris", "heroic", "bottles",
        "wine", "proton", "cs2", "dota2", "hl2_linux", "factorio", "rimworld",
        "terraria", "minecraft", "prismlauncher", "polymc", "roblox",
        "league of legends.exe", "valorant-win64-shipping.exe", "overwatch.exe",
        "fortniteclient-win64-shipping.exe", "r5apex.exe", "tslgame.exe",
        "gta5.exe", "eldenring.exe", "robloxplayerbeta.exe",
        "minecraft.windows.exe"
    };
}

} // namespace vietki::lin

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    g_util = XOpenDisplay(nullptr);
    if (!g_util) {
        g_printerr("VietKi: cannot open X display (is DISPLAY set? Wayland needs "
                   "XWayland).\n");
        return 1;
    }

    loadConfig(state().config);
    applyAutostart(state().config.autostart);
    initGamingOverlay();

    static Engine engine(Config{state().config.method, state().config.tone,
                                state().config.enabled, state().config.spellCheck,
                                state().config.lockWordAfterCancel});
    state().engine = &engine;

    if (!createTray())
        g_printerr("VietKi: status icon unavailable; running without a tray menu.\n");

    refreshForegroundApp(g_lastSeenClass);  // seed resolved state + icon
    g_timeout_add(400, pollActiveWindow, nullptr);  // track the focused app

    if (!startHook()) {
        g_printerr("VietKi: could not start the XRecord keyboard monitor. The "
                   "RECORD extension must be enabled (it is by default on Xorg).\n");
        return 1;
    }

    gtk_main();
    stopHook();
    destroyGamingOverlay();
    if (g_util) XCloseDisplay(g_util);
    return 0;
}
