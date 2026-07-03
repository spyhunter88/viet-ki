// INI config persistence next to the running executable and autostart registry
// handling. The file is intentionally human-editable: comments explain each
// setting, and app lists use one process name per line.
#include "app.h"

#include <commctrl.h> // HOTKEYF_* modifier flags
#include <comdef.h>   // _bstr_t / _variant_t for the Task Scheduler COM API
#include <taskschd.h> // ITaskService: the elevated logon task

#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vietki::win {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::wstring trim(std::wstring s) {
    size_t first = 0;
    while (first < s.size() && iswspace(s[first])) ++first;
    size_t last = s.size();
    while (last > first && iswspace(s[last - 1])) --last;
    return s.substr(first, last - first);
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring stripInlineComment(std::wstring s) {
    bool inQuote = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'"') inQuote = !inQuote;
        if (!inQuote && (s[i] == L';' || s[i] == L'#')) return s.substr(0, i);
    }
    return s;
}

std::wstring readValue(const std::wstring& line) {
    size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) return {};
    std::wstring value = trim(stripInlineComment(line.substr(eq + 1)));
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::wstring readKey(const std::wstring& line) {
    size_t eq = line.find(L'=');
    return trim(eq == std::wstring::npos ? line : line.substr(0, eq));
}

bool parseBool(const std::wstring& value, bool def) {
    std::wstring v = lower(trim(value));
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
    if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
    return def;
}

int parseInt(const std::wstring& value, int def) {
    try {
        return std::stoi(trim(value));
    } catch (...) {
        return def;
    }
}

std::string boolText(bool value) { return value ? "true" : "false"; }

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

void writeList(std::ostringstream& out, const std::vector<std::wstring>& values) {
    for (const auto& value : values)
        if (!trim(value).empty()) out << wideToUtf8(value) << "=\n";
}

// Phase 5 H.1: the three valid gamingPolicy strings.
std::string gamingPolicyText(GamingPolicy p) {
    switch (p) {
        case GamingPolicy::ToggleForCurrentApp: return "toggleForCurrentApp";
        case GamingPolicy::TemporaryTrigger: return "temporaryTrigger";
        default: return "disabled";
    }
}

GamingPolicy parseGamingPolicy(const std::wstring& value, GamingPolicy def) {
    std::wstring v = lower(trim(value));
    if (v == L"toggleforcurrentapp") return GamingPolicy::ToggleForCurrentApp;
    if (v == L"temporarytrigger") return GamingPolicy::TemporaryTrigger;
    if (v == L"disabled") return GamingPolicy::Disabled;
    return def;
}

// Resolve the default trigger to the physical ']' key (VK_OEM_6). The scan code
// is layout-dependent, so it is computed from the current layout at load time
// rather than hard-coded (H.1).
TriggerBinding defaultGamingTrigger() {
    TriggerBinding t;
    t.vk = VK_OEM_6;
    t.scanCode = MapVirtualKeyW(VK_OEM_6, MAPVK_VK_TO_VSC);
    t.modifiers = 0;
    return t;
}

} // namespace

const std::vector<std::wstring>& defaultGamingProcesses() {
    // Phase 5 B.4: game executables (not launchers); no broad runtimes like
    // javaw.exe / UnityPlayer.exe. Matching is case-insensitive. All of these
    // also default to "Dán Unicode" (see defaultGamingPasteProcesses below).
    static const std::vector<std::wstring> kDefaults = {
        L"GenshinImpact.exe", L"StarRail.exe", L"ZenlessZoneZero.exe",
        L"Client-Win64-Shipping.exe", L"League of Legends.exe",
        L"VALORANT-Win64-Shipping.exe", L"fconline.exe", L"crossfire.exe",
        L"cs2.exe", L"dota2.exe", L"GTA5.exe", L"FiveM_GTAProcess.exe",
        L"TslGame.exe"};
    return kDefaults;
}

const std::vector<std::wstring>& defaultGamingPasteProcesses() {
    // Every entry in defaultGamingProcesses() ships with "Dán Unicode" on.
    return defaultGamingProcesses();
}

// --- hotkey string parsing (Phase 3 D.1) -----------------------------------
// A hotkey is written as modifier names joined by '+', optionally ending in a
// base key: "Ctrl+Shift", "Ctrl+Alt", "Alt+Z", "Ctrl+Shift+Space". The base
// key may be a letter, a digit, Space, or F1..F24. Case-insensitive.
bool parseHotkeyString(const std::wstring& s, BYTE& mods, BYTE& vk) {
    mods = 0;
    vk = 0;
    std::wstring token;
    std::vector<std::wstring> tokens;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == L'+') {
            std::wstring t = lower(trim(token));
            if (!t.empty()) tokens.push_back(t);
            token.clear();
        } else {
            token.push_back(s[i]);
        }
    }
    bool any = false;
    for (const auto& t : tokens) {
        if (t == L"ctrl" || t == L"control") { mods |= HOTKEYF_CONTROL; any = true; }
        else if (t == L"shift") { mods |= HOTKEYF_SHIFT; any = true; }
        else if (t == L"alt") { mods |= HOTKEYF_ALT; any = true; }
        else if (t == L"win" || t == L"window" || t == L"windows") {
            mods |= VIETKI_HOTKEYF_WIN; any = true;
        }
        else if (t == L"space") { vk = VK_SPACE; any = true; }
        else if (t.size() == 1 && t[0] >= L'a' && t[0] <= L'z') {
            vk = (BYTE)(t[0] - L'a' + 'A'); any = true;
        } else if (t.size() == 1 && t[0] >= L'0' && t[0] <= L'9') {
            vk = (BYTE)t[0]; any = true;
        } else if ((t[0] == L'f') && t.size() >= 2 && t.size() <= 3) {
            int n = parseInt(t.substr(1), 0);
            if (n >= 1 && n <= 24) { vk = (BYTE)(VK_F1 + n - 1); any = true; }
        }
    }
    return any;
}

std::wstring normalizeHotkeyString(const std::wstring& s) {
    BYTE mods = 0, vk = 0;
    if (!parseHotkeyString(s, mods, vk)) return {};
    std::wstring out;
    auto add = [&](const wchar_t* part) {
        if (!out.empty()) out += L"+";
        out += part;
    };
    if (mods & HOTKEYF_CONTROL) add(L"Ctrl");
    if (mods & HOTKEYF_ALT) add(L"Alt");
    if (mods & HOTKEYF_SHIFT) add(L"Shift");
    if (mods & VIETKI_HOTKEYF_WIN) add(L"Win");
    if (vk == VK_SPACE) add(L"Space");
    else if (vk >= 'A' && vk <= 'Z') { wchar_t c[2] = {(wchar_t)vk, 0}; add(c); }
    else if (vk >= '0' && vk <= '9') { wchar_t c[2] = {(wchar_t)vk, 0}; add(c); }
    else if (vk >= VK_F1 && vk <= VK_F24) {
        std::wstring f = L"F" + std::to_wstring(vk - VK_F1 + 1);
        add(f.c_str());
    }
    return out;
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

std::wstring configPath() { return exeDir() + L"\\config.ini"; }

void loadConfig(AppConfig& cfg) {
    // Resolve the layout-dependent default trigger up front so both the no-file
    // path and any config missing the trigger fields get a valid binding (H.1).
    cfg.gamingTrigger = defaultGamingTrigger();

    std::ifstream in(configPath(), std::ios::binary);
    if (!in) {
        saveConfig(cfg); // first run: create an editable default config.ini next to VietKi.exe
        return;
    }

    std::stringstream ss;
    ss << in.rdbuf();
    std::string rawBytes = ss.str();
    in.close();

    if (!isValidConfig(rawBytes)) {
        MessageBoxW(nullptr,
                    L"File config.ini đã bị lỗi hoặc chứa ký tự không hợp lệ. VietKi sẽ sao lưu file lỗi thành config.ini.bak và khôi phục cấu hình mặc định.",
                    L"VietKi",
                    MB_OK | MB_ICONWARNING);

        std::wstring path = configPath();
        std::wstring backupPath = path + L".bak";
        _wremove(backupPath.c_str());
        _wrename(path.c_str(), backupPath.c_str());

        AppConfig defaultCfg;
        saveConfig(defaultCfg);
        cfg = defaultCfg;
        return;
    }

    std::wstring text = utf8ToWide(rawBytes);
    if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());

    std::wistringstream lines(text);
    std::wstring line;
    std::wstring section;
    bool sawExcludedSection = false;
    bool sawAutocompleteSection = false;
    bool sawGamingSection = false;
    bool sawGamingPasteSection = false;
    // Phase 5 H.2: migrate experimental config keys when gamingPolicy is absent.
    bool sawGamingPolicy = false;
    bool legacyTemp = false;
    bool legacyToggle = false;
    bool sawTriggerScan = false;

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring raw = trim(line);
        if (raw.empty() || raw[0] == L';' || raw[0] == L'#') continue;

        if (raw.front() == L'[' && raw.back() == L']') {
            section = lower(trim(raw.substr(1, raw.size() - 2)));
            if (section == L"apploaitru") {
                cfg.excludedProcesses.clear();
                sawExcludedSection = true;
            } else if (section == L"autocomplete") {
                cfg.autocompleteProcesses.clear();
                sawAutocompleteSection = true;
            } else if (section == L"ungdunggame") {
                cfg.gamingProcesses.clear();
                sawGamingSection = true;
            } else if (section == L"ungdunggamepaste") {
                cfg.gamingPasteProcesses.clear();
                sawGamingPasteSection = true;
            }
            continue;
        }

        std::wstring key = readKey(raw);
        if (key.empty()) continue;
        std::wstring keyLower = lower(key);
        std::wstring value = readValue(raw);

        if (section == L"caidat" || section == L"settings") {
            if (keyLower == L"method")
                cfg.method = (value == L"VNI") ? Method::VNI
                           : (value == L"VIQR" ? Method::VIQR : Method::Telex);
            else if (keyLower == L"tone")
                cfg.tone = (value == L"Old") ? TonePlacement::Old : TonePlacement::Modern;
            else if (keyLower == L"enabled")
                cfg.enabled = parseBool(value, cfg.enabled);
            else if (keyLower == L"spellcheck")
                cfg.spellCheck = parseBool(value, cfg.spellCheck);
            else if (keyLower == L"lockwordaftercancel")
                cfg.lockWordAfterCancel = parseBool(value, cfg.lockWordAfterCancel);
            else if (keyLower == L"autostart")
                cfg.autostart = parseBool(value, cfg.autostart);
            else if (keyLower == L"autostartadmin")
                cfg.autostartAdmin = parseBool(value, cfg.autostartAdmin);
            else if (keyLower == L"autocompletefix")
                cfg.autocompleteFix = parseBool(value, cfg.autocompleteFix);
            else if (keyLower == L"hotkeysenabled") {
                bool enabled = parseBool(value, true);
                cfg.toggleVietnameseHotkeyEnabled = enabled;
                cfg.toggleForCurrentAppHotkeyEnabled = enabled;
            }
            else if (keyLower == L"togglevietnameseenabled" ||
                     keyLower == L"togglevietnamesehotkeyenabled")
                cfg.toggleVietnameseHotkeyEnabled =
                    parseBool(value, cfg.toggleVietnameseHotkeyEnabled);
            else if (keyLower == L"toggleforcurrentappenabled" ||
                     keyLower == L"toggleforcurrentapphotkeyenabled")
                cfg.toggleForCurrentAppHotkeyEnabled =
                    parseBool(value, cfg.toggleForCurrentAppHotkeyEnabled);
            else if (keyLower == L"soundonglobaltoggle")
                cfg.soundOnGlobalToggle = parseBool(value, cfg.soundOnGlobalToggle);
            else if (keyLower == L"soundonexcludedtoggle")
                cfg.soundOnExcludedToggle = parseBool(value, cfg.soundOnExcludedToggle);
            else if (keyLower == L"exclusionfeatureon")
                cfg.exclusionFeatureOn = parseBool(value, cfg.exclusionFeatureOn);
            else if (keyLower == L"revertoverrideonblur")
                cfg.revertOverrideOnBlur = parseBool(value, cfg.revertOverrideOnBlur);
            // Phase 3 C.3: clearer INI key names. The old "overrideHotkey" name
            // is still accepted so existing config.ini files keep working.
            else if (keyLower == L"toggleforcurrentapp" || keyLower == L"overridehotkey")
                cfg.toggleForCurrentAppHotkey =
                    (WORD)parseInt(value, cfg.toggleForCurrentAppHotkey);
            else if (keyLower == L"toggleexclusion" ||
                     keyLower == L"toggleexclusionhotkey")
                cfg.toggleExclusionHotkey =
                    (WORD)parseInt(value, cfg.toggleExclusionHotkey);
            else if (keyLower == L"togglevietnamese" || keyLower == L"hotkey") {
                std::wstring norm = normalizeHotkeyString(value);
                cfg.hotkey = norm.empty() ? value : norm;
            }
            // --- Phase 5 Gaming Mode ---
            else if (keyLower == L"gamingpolicy") {
                cfg.gamingPolicy = parseGamingPolicy(value, cfg.gamingPolicy);
                sawGamingPolicy = true;
            }
            else if (keyLower == L"gaminglistinitialized")
                cfg.gamingListInitialized =
                    parseBool(value, cfg.gamingListInitialized);
            else if (keyLower == L"notifyonautomaticmodeswitch")
                cfg.notifyOnAutomaticModeSwitch =
                    parseBool(value, cfg.notifyOnAutomaticModeSwitch);
            else if (keyLower == L"gamingtriggerkey")
                cfg.gamingTrigger.vk = (UINT)parseInt(value, (int)cfg.gamingTrigger.vk);
            else if (keyLower == L"gamingtriggerscancode") {
                cfg.gamingTrigger.scanCode = (UINT)parseInt(value, 0);
                sawTriggerScan = true;
            }
            else if (keyLower == L"gamingtriggermodifiers")
                cfg.gamingTrigger.modifiers = (UINT)parseInt(value, 0);
            else if (keyLower == L"gamingoverlayenabled")
                cfg.gamingOverlayEnabled = parseBool(value, cfg.gamingOverlayEnabled);
            else if (keyLower == L"gamingoverlaycorner")
                cfg.gamingOverlayCorner = parseInt(value, cfg.gamingOverlayCorner);
            else if (keyLower == L"soundongamingmodeswitch")
                cfg.soundOnGamingModeSwitch =
                    parseBool(value, cfg.soundOnGamingModeSwitch);
            // Migration from the experimental two-boolean shape (H.2).
            else if (keyLower == L"gamingtemporarytriggerenabled")
                legacyTemp = parseBool(value, false);
            else if (keyLower == L"gamingmodeenabled")
                legacyToggle = parseBool(value, false);
        } else if (section == L"apploaitru") {
            if (sawExcludedSection) cfg.excludedProcesses.push_back(key);
        } else if (section == L"autocomplete") {
            if (sawAutocompleteSection) cfg.autocompleteProcesses.push_back(key);
        } else if (section == L"ungdunggame") {
            if (sawGamingSection) cfg.gamingProcesses.push_back(key);
        } else if (section == L"ungdunggamepaste") {
            if (sawGamingPasteSection) cfg.gamingPasteProcesses.push_back(key);
        }
    }

    // Phase 5 H.2: if gamingPolicy was absent, derive it from the experimental
    // keys; temporaryTrigger wins over the old gamingMode toggle.
    if (!sawGamingPolicy) {
        if (legacyTemp) cfg.gamingPolicy = GamingPolicy::TemporaryTrigger;
        else if (legacyToggle) cfg.gamingPolicy = GamingPolicy::ToggleForCurrentApp;
    }
    // Keep the trigger binding valid: a vk with no stored scan code is resolved
    // from the current layout (C.2).
    if (cfg.gamingTrigger.vk == 0) cfg.gamingTrigger = defaultGamingTrigger();
    else if (!sawTriggerScan || cfg.gamingTrigger.scanCode == 0)
        cfg.gamingTrigger.scanCode =
            MapVirtualKeyW(cfg.gamingTrigger.vk, MAPVK_VK_TO_VSC);
}

void saveConfig(const AppConfig& cfg) {
    std::ostringstream out;
    out << "\xEF\xBB\xBF";
    out << "; VietKi configuration file.\n";
    out << "; This file is stored next to VietKi.exe so it can be copied with the app.\n";
    out << "; Lines starting with ';' or '#' are comments.\n\n";

    out << "[Caidat]\n";
    out << "; Typing method: Telex, VNI, or VIQR.\n";
    out << "method=" << (cfg.method == Method::VNI ? "VNI"
                         : cfg.method == Method::VIQR ? "VIQR" : "Telex")
        << "\n";
    out << "; Tone placement: Modern or Old.\n";
    out << "tone=" << (cfg.tone == TonePlacement::Old ? "Old" : "Modern") << "\n";
    out << "; Master Vietnamese input switch.\n";
    out << "enabled=" << boolText(cfg.enabled) << "\n";
    out << "; Spell checking: only apply marks when the word stays Vietnamese.\n";
    out << "spellCheck=" << boolText(cfg.spellCheck) << "\n";
    out << "; Lock the rest of the word to literal after an explicit cancel\n";
    out << "; (e.g. \"offf\" -> \"off\"). Off keeps composing freely.\n";
    out << "lockWordAfterCancel=" << boolText(cfg.lockWordAfterCancel) << "\n";
    out << "; Start VietKi with Windows.\n";
    out << "autostart=" << boolText(cfg.autostart) << "\n";
    out << "autostartadmin=" << boolText(cfg.autostartAdmin) << "\n";
    out << "; Fix tone placement in autocomplete-heavy apps.\n";
    out << "autocompleteFix=" << boolText(cfg.autocompleteFix) << "\n";
    out << "; Play a sound when switching global English/Vietnamese mode.\n";
    out << "soundOnGlobalToggle=" << boolText(cfg.soundOnGlobalToggle) << "\n";
    out << "; Play a different sound when switching V-/V+ for an excluded app.\n";
    out << "soundOnExcludedToggle=" << boolText(cfg.soundOnExcludedToggle) << "\n";
    out << "; Honor the per-app exclusion list below.\n";
    out << "exclusionFeatureOn=" << boolText(cfg.exclusionFeatureOn) << "\n";
    out << "; Drop temporary app override after focus leaves that app.\n";
    out << "revertOverrideOnBlur=" << boolText(cfg.revertOverrideOnBlur) << "\n";
    out << "; Toggle Vietnamese on/off. Any combination, e.g. Ctrl+Shift,\n";
    out << "; Ctrl+Alt, Alt+Z, or Ctrl+Shift+Space.\n";
    out << "toggleVietnameseEnabled=" << boolText(cfg.toggleVietnameseHotkeyEnabled) << "\n";
    out << "toggleVietnamese=" << wideToUtf8(cfg.hotkey) << "\n";
    out << "; Hotkey value below is a Win32 code: LOBYTE=key, HIBYTE=modifiers.\n";
    out << "; toggleForCurrentApp: toggle V-/V+ for the focused app only if excluded.\n";
    out << "toggleForCurrentAppEnabled="
        << boolText(cfg.toggleForCurrentAppHotkeyEnabled) << "\n";
    out << "toggleForCurrentApp=" << cfg.toggleForCurrentAppHotkey << "\n";
    out << "; --- Phase 5: Gaming Mode ---\n";
    out << "; gamingPolicy: disabled, toggleForCurrentApp, or temporaryTrigger.\n";
    out << "gamingPolicy=" << gamingPolicyText(cfg.gamingPolicy) << "\n";
    out << "; The suggestion list is seeded once; this guards re-seeding.\n";
    out << "gamingListInitialized=" << boolText(cfg.gamingListInitialized) << "\n";
    out << "; Show a notification when VietKi auto-switches mode (temporary trigger).\n";
    out << "notifyOnAutomaticModeSwitch="
        << boolText(cfg.notifyOnAutomaticModeSwitch) << "\n";
    out << "; Temporary-typing trigger, stored by physical key (default ']').\n";
    out << "gamingTriggerKey=" << cfg.gamingTrigger.vk << "\n";
    out << "gamingTriggerScanCode=" << cfg.gamingTrigger.scanCode << "\n";
    out << "gamingTriggerModifiers=" << cfg.gamingTrigger.modifiers << "\n";
    out << "; Translucent on-screen 'Tiếng Việt' badge while a gaming session is on.\n";
    out << "gamingOverlayEnabled=" << boolText(cfg.gamingOverlayEnabled) << "\n";
    out << "; Overlay corner: 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right.\n";
    out << "gamingOverlayCorner=" << cfg.gamingOverlayCorner << "\n";
    out << "; Beep when VietKi auto-switches Vietnamese on/off in a gaming session.\n";
    out << "soundOnGamingModeSwitch=" << boolText(cfg.soundOnGamingModeSwitch) << "\n";
    out << "\n";

    out << "[Apploaitru]\n";
    out << "; One excluded process per line. Example: chrome.exe=\n";
    writeList(out, cfg.excludedProcesses);
    out << "\n";

    out << "[Autocomplete]\n";
    out << "; Apps where VietKi uses selection-replace to cooperate with autocomplete.\n";
    writeList(out, cfg.autocompleteProcesses);
    out << "\n";

    out << "[UngDungGame]\n";
    out << "; One game executable per line. Independent of [Apploaitru] (B.1).\n";
    writeList(out, cfg.gamingProcesses);
    out << "\n";

    out << "[UngDungGamePaste]\n";
    out << "; Games that receive composed text through CF_UNICODETEXT + Ctrl+V.\n";
    out << "; Opt in only when direct Unicode keystrokes display as '?'.\n";
    writeList(out, cfg.gamingPasteProcesses);

    std::ofstream f(configPath(), std::ios::binary | std::ios::trunc);
    f << out.str();
}

void applyAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // Append the autostart flag so the logon launch stays in the tray; a
        // manual double-click (no flag) surfaces the UI instead.
        std::wstring quoted =
            L"\"" + std::wstring(exe) + L"\" " + kAutostartArg;
        RegSetValueExW(key, L"VietKi", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"VietKi");
    }
    RegCloseKey(key);
}

// --- Elevated autostart: a Task Scheduler "highest privileges" logon task -----
// The HKCU Run value above always launches as the invoking (non-elevated) user,
// and pointing it at an app that wants elevation only earns a UAC prompt at every
// logon. A registered logon task with TASK_RUNLEVEL_HIGHEST instead starts VietKi
// already elevated and silently. Creating or deleting such a task needs admin.

namespace {

constexpr wchar_t kAutostartTaskName[] = L"VietKi Autostart";

// Balance CoInitialize so these helpers work whether or not the caller already
// initialised COM on this thread (main.cpp calls OleInitialize; cron-style
// callers may not).
struct ComScope {
    bool owned = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = (hr == S_OK || hr == S_FALSE); // S_FALSE: already init on thread
    }
    ~ComScope() {
        if (owned) CoUninitialize();
    }
};

// "DOMAIN\user" for the current session, used to scope the task to this user so
// it does not fire (elevated) for other accounts on the machine.
std::wstring currentUserSam() {
    wchar_t domain[256] = {}, name[256] = {};
    GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    GetEnvironmentVariableW(L"USERNAME", name, 256);
    if (!name[0]) return {};
    if (domain[0]) return std::wstring(domain) + L"\\" + name;
    return name;
}

// Build and register (create-or-update) the logon task pointing at this exe.
bool createLogonTask(ITaskService* svc, ITaskFolder* root) {
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring user = currentUserSam();

    ITaskDefinition* task = nullptr;
    if (FAILED(svc->NewTask(0, &task)) || !task) return false;

    // Run with the highest privileges available, in the interactive user's token.
    if (IPrincipal* principal = nullptr;
        SUCCEEDED(task->get_Principal(&principal)) && principal) {
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        if (!user.empty()) principal->put_UserId(_bstr_t(user.c_str()));
        principal->Release();
    }
    // Keep it running on battery and without a time limit — it is a tray app.
    if (ITaskSettings* settings = nullptr;
        SUCCEEDED(task->get_Settings(&settings)) && settings) {
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
        settings->Release();
    }
    // Fire at this user's logon.
    if (ITriggerCollection* triggers = nullptr;
        SUCCEEDED(task->get_Triggers(&triggers)) && triggers) {
        if (ITrigger* trig = nullptr;
            SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trig)) && trig) {
            if (ILogonTrigger* logon = nullptr;
                SUCCEEDED(trig->QueryInterface(IID_ILogonTrigger,
                                               (void**)&logon)) &&
                logon) {
                logon->put_Id(_bstr_t(L"VietKiLogon"));
                if (!user.empty()) logon->put_UserId(_bstr_t(user.c_str()));
                logon->Release();
            }
            trig->Release();
        }
        triggers->Release();
    }
    // Launch this exe.
    if (IActionCollection* actions = nullptr;
        SUCCEEDED(task->get_Actions(&actions)) && actions) {
        if (IAction* action = nullptr;
            SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
            if (IExecAction* exec = nullptr;
                SUCCEEDED(action->QueryInterface(IID_IExecAction,
                                                 (void**)&exec)) &&
                exec) {
                exec->put_Path(_bstr_t(exe));
                // Same autostart flag as the HKCU Run value: this logon launch
                // stays in the tray rather than opening the Settings window.
                exec->put_Arguments(_bstr_t(kAutostartArg));
                exec->Release();
            }
            action->Release();
        }
        actions->Release();
    }

    IRegisteredTask* registered = nullptr;
    HRESULT hr = root->RegisterTaskDefinition(
        _bstr_t(kAutostartTaskName), task, TASK_CREATE_OR_UPDATE, _variant_t(),
        _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), &registered);
    bool ok = SUCCEEDED(hr);
    if (registered) registered->Release();
    task->Release();
    return ok;
}

// Open the Task Scheduler root folder, invoking fn(folder). Returns false if the
// scheduler is unreachable.
template <typename Fn>
bool withTaskFolder(Fn&& fn) {
    ComScope com;
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ITaskService,
                                (void**)&svc)) ||
        !svc)
        return false;
    bool ret = false;
    if (SUCCEEDED(svc->Connect(_variant_t(), _variant_t(), _variant_t(),
                               _variant_t()))) {
        ITaskFolder* root = nullptr;
        if (SUCCEEDED(svc->GetFolder(_bstr_t(L"\\"), &root)) && root) {
            ret = fn(svc, root);
            root->Release();
        }
    }
    svc->Release();
    return ret;
}

// Create (enable) or delete (disable) the elevated logon task.
bool applyAutostartAdminTask(bool enable) {
    return withTaskFolder([enable](ITaskService* svc, ITaskFolder* root) {
        if (enable) return createLogonTask(svc, root);
        HRESULT hr = root->DeleteTask(_bstr_t(kAutostartTaskName), 0);
        return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    });
}

} // namespace

bool autostartAdminTaskExists() {
    return withTaskFolder([](ITaskService*, ITaskFolder* root) {
        IRegisteredTask* task = nullptr;
        bool found = SUCCEEDED(root->GetTask(_bstr_t(kAutostartTaskName), &task)) &&
                     task != nullptr;
        if (task) task->Release();
        return found;
    });
}

bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev = {};
    DWORD size = 0;
    bool elevated =
        GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size) &&
        elev.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool reconcileAutostart(const AppConfig& cfg) {
    bool wantTask = cfg.autostart && cfg.autostartAdmin;

    // The HKCU Run value never needs elevation; keep it in lockstep with cfg.
    applyAutostart(cfg.autostart && !cfg.autostartAdmin);

    // When elevated we can freely (re)create or delete the task. Re-register even
    // if it already exists so the stored exe path stays current, the way the Run
    // value is rewritten on every launch.
    if (isProcessElevated()) return applyAutostartAdminTask(wantTask);

    // Not elevated: we can neither create, refresh, nor delete the task. Report
    // success only when it already matches cfg; otherwise the caller must offer
    // to relaunch as admin to apply the change.
    return wantTask == autostartAdminTaskExists();
}

} // namespace vietki::win
