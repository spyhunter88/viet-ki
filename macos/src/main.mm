// VietKi macOS shell entry point (guide 6, Phase 2 A/C/D/E). A background agent
// (LSUIElement) with a status-bar menu, driven by a CGEventTap over the shared
// core engine.
//
// NOTE: this shell mirrors the verified Windows build but has not been compiled
// on macOS in this environment.
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "typing_stats.h"

using namespace vietki;
using namespace vietki::mac;

@class VietKiDelegate;
static VietKiDelegate *g_delegate = nil;

@interface GamingOverlayWindow : NSPanel
@end

@implementation GamingOverlayWindow
- (instancetype)initWithContentRect:(NSRect)contentRect
                            styleMask:(NSWindowStyleMask)aStyle
                              backing:(NSBackingStoreType)bufferingType
                                defer:(BOOL)flag {
  self = [super initWithContentRect:contentRect
                          styleMask:NSWindowStyleMaskBorderless
                            backing:bufferingType
                              defer:flag];
  if (self) {
    self.opaque = NO;
    self.backgroundColor = [NSColor clearColor];
    self.level = NSPopUpMenuWindowLevel;
    self.ignoresMouseEvents = YES;
    self.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
  }
  return self;
}
@end

@interface GamingOverlayController : NSObject
@property(strong) GamingOverlayWindow *window;
@property(strong) NSTextField *label;
- (void)showState:(IconState)state corner:(int)corner;
- (void)hide;
@end

@implementation GamingOverlayController
- (void)initWindow {
  if (self.window) return;
  NSRect rect = NSMakeRect(0, 0, 52, 52);
  self.window = [[GamingOverlayWindow alloc] initWithContentRect:rect
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
  
  NSVisualEffectView *vev = [[NSVisualEffectView alloc] initWithFrame:rect];
  vev.state = NSVisualEffectStateActive;
  vev.material = NSVisualEffectMaterialDark;
  vev.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  vev.wantsLayer = YES;
  vev.layer.cornerRadius = 14.0;
  self.window.contentView = vev;
  
  self.label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 52, 52)];
  self.label.editable = NO;
  self.label.bordered = NO;
  self.label.drawsBackground = NO;
  self.label.alignment = NSTextAlignmentCenter;
  self.label.font = [NSFont boldSystemFontOfSize:26];
  self.label.textColor = [NSColor whiteColor];
  self.label.frame = NSMakeRect(0, 8, 52, 38);
  [vev addSubview:self.label];
}

- (void)showState:(IconState)state corner:(int)corner {
  [self initWindow];
  
  NSString *text = @"";
  NSColor *color = [NSColor whiteColor];
  if (state == IconState::GamingVN) {
    text = @"G+";
    color = [NSColor systemRedColor];
  } else if (state == IconState::Gaming) {
    text = @"G";
    color = [NSColor lightGrayColor];
  } else if (state == IconState::V) {
    text = @"V";
    color = [NSColor systemRedColor];
  } else if (state == IconState::E) {
    text = @"E";
    color = [NSColor lightGrayColor];
  } else if (state == IconState::VPlus) {
    text = @"V+";
    color = [NSColor systemRedColor];
  } else if (state == IconState::VMinus) {
    text = @"V-";
    color = [NSColor lightGrayColor];
  }
  self.label.stringValue = text;
  self.label.textColor = color;
  
  NSScreen *screen = [NSScreen mainScreen];
  NSRect sFrame = screen.visibleFrame;
  CGFloat w = 52, h = 52;
  CGFloat padding = 20;
  CGFloat x = 0, y = 0;
  
  switch (corner) {
    case 0:
      x = sFrame.origin.x + padding;
      y = sFrame.origin.y + sFrame.size.height - h - padding;
      break;
    case 1:
      x = sFrame.origin.x + sFrame.size.width - w - padding;
      y = sFrame.origin.y + sFrame.size.height - h - padding;
      break;
    case 2:
      x = sFrame.origin.x + padding;
      y = sFrame.origin.y + padding;
      break;
    case 3:
    default:
      x = sFrame.origin.x + sFrame.size.width - w - padding;
      y = sFrame.origin.y + padding;
      break;
  }
  
  [self.window setFrameOrigin:NSMakePoint(x, y)];
  [self.window orderFront:nil];
}

- (void)hide {
  if (self.window) {
    [self.window orderOut:nil];
  }
}
@end

static GamingOverlayController *g_overlay = nil;

// ---------------------------------------------------------------------------
// Process state + config persistence
// ---------------------------------------------------------------------------
namespace vietki::mac {

namespace {
AppState g_state;

NSString *supportDir() {
  NSArray *paths = NSSearchPathForDirectoriesInDomains(
      NSApplicationSupportDirectory, NSUserDomainMask, YES);
  NSString *base = paths.firstObject ?: NSHomeDirectory();
  return [base stringByAppendingPathComponent:@"VietKi"];
}

Override overrideFor(const std::string &app) {
  auto it = g_state.perAppOverride.find(app);
  return (it == g_state.perAppOverride.end()) ? Override::None : it->second;
}

std::string trim(std::string s) {
  size_t first = 0;
  while (first < s.size() && std::isspace((unsigned char)s[first]))
    ++first;
  size_t last = s.size();
  while (last > first && std::isspace((unsigned char)s[last - 1]))
    --last;
  return s.substr(first, last - first);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

uint16_t keycodeFromToken(const std::string &token) {
  std::string t = lower(trim(token));
  if (t == "space")
    return kVK_Space;
  if (t == "]" || t == "rightbracket") return kVK_ANSI_RightBracket;
  if (t == "[" || t == "leftbracket") return kVK_ANSI_LeftBracket;
  if (t == ";" || t == "semicolon") return kVK_ANSI_Semicolon;
  if (t == "'" || t == "quote") return kVK_ANSI_Quote;
  if (t == "\\" || t == "backslash") return kVK_ANSI_Backslash;
  if (t == "," || t == "comma") return kVK_ANSI_Comma;
  if (t == "." || t == "period") return kVK_ANSI_Period;
  if (t == "/" || t == "slash") return kVK_ANSI_Slash;
  if (t == "`" || t == "grave") return kVK_ANSI_Grave;
  if (t == "-" || t == "minus") return kVK_ANSI_Minus;
  if (t == "=" || t == "equal") return kVK_ANSI_Equal;
  if (t.size() == 1 && t[0] >= 'a' && t[0] <= 'z') {
    static const uint16_t keys[] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
        kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L,
        kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
        kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X,
        kVK_ANSI_Y, kVK_ANSI_Z};
    return keys[t[0] - 'a'];
  }
  if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') {
    static const uint16_t keys[] = {
        kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
        kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
    return keys[t[0] - '0'];
  }
  if (t.size() >= 2 && t[0] == 'f') {
    int n = std::atoi(t.c_str() + 1);
    static const uint16_t fkeys[] = {
        kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
        kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
        kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
    if (n >= 1 && n <= 20)
      return fkeys[n - 1];
  }
  return 0;
}

std::string stripInlineComment(std::string s) {
  bool inQuote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '"')
      inQuote = !inQuote;
    if (!inQuote && (s[i] == ';' || s[i] == '#'))
      return s.substr(0, i);
  }
  return s;
}

std::string readKey(const std::string &line) {
  size_t eq = line.find('=');
  return trim(eq == std::string::npos ? line : line.substr(0, eq));
}

std::string readValue(const std::string &line) {
  size_t eq = line.find('=');
  if (eq == std::string::npos)
    return {};
  std::string value = trim(stripInlineComment(line.substr(eq + 1)));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  return value;
}

bool parseBool(const std::string &value, bool def) {
  std::string v = lower(trim(value));
  if (v == "1" || v == "true" || v == "yes" || v == "on")
    return true;
  if (v == "0" || v == "false" || v == "no" || v == "off")
    return false;
  return def;
}

std::string boolText(bool value) { return value ? "true" : "false"; }

std::string legacyJsonPath() {
  NSString *p = [supportDir() stringByAppendingPathComponent:@"config.json"];
  return std::string(p.UTF8String);
}
} // namespace

AppState &state() { return g_state; }

bool isExcludedMember(const std::string &bundleId) {
  for (const auto &b : g_state.config.excludedBundles)
    if (b == bundleId)
      return true;
  return false;
}

std::string configPath() {
  NSString *p = [supportDir() stringByAppendingPathComponent:@"config.ini"];
  return std::string(p.UTF8String);
}

Hotkey hotkeyFromString(const std::string &s) {
  Hotkey h;
  std::string token;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '+') {
      std::string t = lower(trim(token));
      if (t == "ctrl" || t == "control")
        h.mods |= kCGEventFlagMaskControl;
      else if (t == "alt" || t == "option" || t == "opt")
        h.mods |= kCGEventFlagMaskAlternate;
      else if (t == "shift")
        h.mods |= kCGEventFlagMaskShift;
      else if (t == "cmd" || t == "command" || t == "win" || t == "window")
        h.mods |= kCGEventFlagMaskCommand;
      else if (!t.empty())
        h.keycode = keycodeFromToken(t);
      token.clear();
    } else {
      token.push_back(s[i]);
    }
  }
  return h;
}

std::string keyNameFromKeycode(uint16_t keycode) {
  if (keycode == kVK_Space)
    return "Space";
  if (keycode == kVK_ANSI_RightBracket) return "]";
  if (keycode == kVK_ANSI_LeftBracket) return "[";
  if (keycode == kVK_ANSI_Semicolon) return ";";
  if (keycode == kVK_ANSI_Quote) return "'";
  if (keycode == kVK_ANSI_Backslash) return "\\";
  if (keycode == kVK_ANSI_Comma) return ",";
  if (keycode == kVK_ANSI_Period) return ".";
  if (keycode == kVK_ANSI_Slash) return "/";
  if (keycode == kVK_ANSI_Grave) return "`";
  if (keycode == kVK_ANSI_Minus) return "-";
  if (keycode == kVK_ANSI_Equal) return "=";
  static const std::pair<uint16_t, const char *> names[] = {
      {kVK_ANSI_A, "A"}, {kVK_ANSI_B, "B"}, {kVK_ANSI_C, "C"},
      {kVK_ANSI_D, "D"}, {kVK_ANSI_E, "E"}, {kVK_ANSI_F, "F"},
      {kVK_ANSI_G, "G"}, {kVK_ANSI_H, "H"}, {kVK_ANSI_I, "I"},
      {kVK_ANSI_J, "J"}, {kVK_ANSI_K, "K"}, {kVK_ANSI_L, "L"},
      {kVK_ANSI_M, "M"}, {kVK_ANSI_N, "N"}, {kVK_ANSI_O, "O"},
      {kVK_ANSI_P, "P"}, {kVK_ANSI_Q, "Q"}, {kVK_ANSI_R, "R"},
      {kVK_ANSI_S, "S"}, {kVK_ANSI_T, "T"}, {kVK_ANSI_U, "U"},
      {kVK_ANSI_V, "V"}, {kVK_ANSI_W, "W"}, {kVK_ANSI_X, "X"},
      {kVK_ANSI_Y, "Y"}, {kVK_ANSI_Z, "Z"}, {kVK_ANSI_0, "0"},
      {kVK_ANSI_1, "1"}, {kVK_ANSI_2, "2"}, {kVK_ANSI_3, "3"},
      {kVK_ANSI_4, "4"}, {kVK_ANSI_5, "5"}, {kVK_ANSI_6, "6"},
      {kVK_ANSI_7, "7"}, {kVK_ANSI_8, "8"}, {kVK_ANSI_9, "9"},
  };
  for (const auto &n : names)
    if (n.first == keycode)
      return n.second;
  static const uint16_t fkeys[] = {kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,
                                   kVK_F6,  kVK_F7,  kVK_F8,  kVK_F9,  kVK_F10,
                                   kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15,
                                   kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
  for (int i = 0; i < 20; ++i)
    if (fkeys[i] == keycode)
      return "F" + std::to_string(i + 1);
  return "";
}

std::string stringFromHotkey(const Hotkey &h) {
  std::vector<std::string> parts;
  if (h.mods & kCGEventFlagMaskControl)
    parts.push_back("Ctrl");
  if (h.mods & kCGEventFlagMaskAlternate)
    parts.push_back("Option");
  if (h.mods & kCGEventFlagMaskShift)
    parts.push_back("Shift");
  if (h.mods & kCGEventFlagMaskCommand)
    parts.push_back("Command");
  std::string key = keyNameFromKeycode(h.keycode);
  if (!key.empty())
    parts.push_back(key);
  std::string out;
  for (const auto &p : parts) {
    if (!out.empty())
      out += "+";
    out += p;
  }
  return out;
}

static Hotkey hotkeyFromDict(NSDictionary *d) {
  Hotkey h;
  if ([d isKindOfClass:[NSDictionary class]]) {
    h.keycode = (uint16_t)[d[@"keycode"] unsignedIntValue];
    h.mods = (uint64_t)[d[@"mods"] unsignedLongLongValue];
  }
  return h;
}

static bool loadLegacyJsonConfig(AppConfig &cfg) {
  NSData *data = [NSData dataWithContentsOfFile:@(legacyJsonPath().c_str())];
  if (!data)
    return false;
  NSDictionary *d = [NSJSONSerialization JSONObjectWithData:data
                                                    options:0
                                                      error:nil];
  if (![d isKindOfClass:[NSDictionary class]])
    return false;
  NSString *m = d[@"method"];
  if ([m isEqualToString:@"VNI"])
    cfg.method = Method::VNI;
  else if ([m isEqualToString:@"VIQR"])
    cfg.method = Method::VIQR;
  else
    cfg.method = Method::Telex;
  cfg.tone = [d[@"tone"] isEqualToString:@"Old"] ? TonePlacement::Old
                                                 : TonePlacement::Modern;
  if (d[@"enabled"])
    cfg.enabled = [d[@"enabled"] boolValue];
  if (d[@"spellCheck"])
    cfg.spellCheck = [d[@"spellCheck"] boolValue];
  if (d[@"lockWordAfterCancel"])
    cfg.lockWordAfterCancel = [d[@"lockWordAfterCancel"] boolValue];
  if (d[@"restoreAfterSpace"])
    cfg.restoreAfterSpace = [d[@"restoreAfterSpace"] boolValue];
  if (d[@"autocompleteFix"])
    cfg.autocompleteFix = [d[@"autocompleteFix"] boolValue];
  if (d[@"soundOnGlobalToggle"])
    cfg.soundOnGlobalToggle = [d[@"soundOnGlobalToggle"] boolValue];
  if (d[@"soundOnExcludedToggle"])
    cfg.soundOnExcludedToggle = [d[@"soundOnExcludedToggle"] boolValue];
  if (d[@"exclusionFeatureOn"])
    cfg.exclusionFeatureOn = [d[@"exclusionFeatureOn"] boolValue];
  if (d[@"revertOverrideOnBlur"])
    cfg.revertOverrideOnBlur = [d[@"revertOverrideOnBlur"] boolValue];
  if (d[@"hotkeysEnabled"]) {
    bool enabled = [d[@"hotkeysEnabled"] boolValue];
    cfg.toggleVietnameseHotkeyEnabled = enabled;
    cfg.overrideHotkeyEnabled = enabled;
  }
  if (d[@"toggleVietnameseEnabled"])
    cfg.toggleVietnameseHotkeyEnabled =
        [d[@"toggleVietnameseEnabled"] boolValue];
  if (d[@"toggleForCurrentAppEnabled"])
    cfg.overrideHotkeyEnabled = [d[@"toggleForCurrentAppEnabled"] boolValue];
  if ([d[@"hotkey"] isKindOfClass:[NSString class]])
    cfg.hotkey = [d[@"hotkey"] UTF8String];
  if (d[@"overrideHotkey"])
    cfg.overrideHotkey = hotkeyFromDict(d[@"overrideHotkey"]);
  if (d[@"toggleExclusionHotkey"])
    cfg.toggleExclusionHotkey = hotkeyFromDict(d[@"toggleExclusionHotkey"]);
  NSArray *ex = d[@"excludedBundles"];
  if ([ex isKindOfClass:[NSArray class]]) {
    cfg.excludedBundles.clear();
    for (NSString *s in ex)
      cfg.excludedBundles.push_back(s.UTF8String);
  }
  NSArray *au = d[@"autocompleteBundles"];
  if ([au isKindOfClass:[NSArray class]]) {
    cfg.autocompleteBundles.clear();
    for (NSString *s in au)
      cfg.autocompleteBundles.push_back(s.UTF8String);
  }
  return true;
}

static bool isValidConfig(const std::string &text) {
  if (text.empty()) {
    return true;
  }

  const unsigned char *bytes = (const unsigned char *)text.data();
  const unsigned char *end = bytes + text.size();
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
  for (auto &c : lowerText) {
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

void loadConfig(AppConfig &cfg) {
  std::ifstream in(configPath(), std::ios::binary);
  if (!in) {
    if (loadLegacyJsonConfig(cfg))
      saveConfig(cfg);
    else
      saveConfig(cfg);
    return;
  }

  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  in.close();

  if (!isValidConfig(text)) {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Cấu hình không hợp lệ";
    a.informativeText = @"File config.ini đã bị lỗi hoặc chứa ký tự không hợp lệ. VietKi sẽ sao lưu file lỗi thành config.ini.bak và khôi phục cấu hình mặc định.";
    [a addButtonWithTitle:@"Đồng ý"];
    [a runModal];

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
  std::string line;
  std::string section;
  bool sawExcludedSection = false;
  bool sawAutocompleteSection = false;
  bool sawGamingSection = false;
  bool sawGamingPasteSection = false;

  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::string raw = trim(line);
    if (raw.empty() || raw[0] == ';' || raw[0] == '#')
      continue;

    if (raw.front() == '[' && raw.back() == ']') {
      section = lower(trim(raw.substr(1, raw.size() - 2)));
      if (section == "apploaitru" || section == "excludedbundles") {
        cfg.excludedBundles.clear();
        sawExcludedSection = true;
      } else if (section == "autocomplete") {
        cfg.autocompleteBundles.clear();
        sawAutocompleteSection = true;
      } else if (section == "ungdunggame") {
        cfg.gamingBundles.clear();
        sawGamingSection = true;
      } else if (section == "ungdunggamepaste") {
        cfg.gamingPasteBundles.clear();
        sawGamingPasteSection = true;
      }
      continue;
    }

    std::string key = readKey(raw);
    if (key.empty())
      continue;
    std::string keyLower = lower(key);
    std::string value = readValue(raw);

    if (section == "caidat" || section == "settings") {
      if (keyLower == "method")
        cfg.method = (value == "VNI")
                         ? Method::VNI
                         : (value == "VIQR" ? Method::VIQR : Method::Telex);
      else if (keyLower == "tone")
        cfg.tone =
            (value == "Old") ? TonePlacement::Old : TonePlacement::Modern;
      else if (keyLower == "enabled")
        cfg.enabled = parseBool(value, cfg.enabled);
      else if (keyLower == "spellcheck")
        cfg.spellCheck = parseBool(value, cfg.spellCheck);
      else if (keyLower == "lockwordaftercancel")
        cfg.lockWordAfterCancel = parseBool(value, cfg.lockWordAfterCancel);
      else if (keyLower == "restoreafterspace" || keyLower == "restore_after_space")
        cfg.restoreAfterSpace = parseBool(value, cfg.restoreAfterSpace);
      else if (keyLower == "autostart")
        cfg.autostart = parseBool(value, cfg.autostart);
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
      else if (keyLower == "typingstats")
        cfg.typingStats = parseBool(value, cfg.typingStats);
      else if (keyLower == "hotkeysenabled") {
        bool enabled = parseBool(value, true);
        cfg.toggleVietnameseHotkeyEnabled = enabled;
        cfg.overrideHotkeyEnabled = enabled;
      } else if (keyLower == "togglevietnameseenabled" ||
                 keyLower == "togglevietnamesehotkeyenabled")
        cfg.toggleVietnameseHotkeyEnabled =
            parseBool(value, cfg.toggleVietnameseHotkeyEnabled);
      else if (keyLower == "toggleforcurrentappenabled" ||
               keyLower == "overridehotkeyenabled")
        cfg.overrideHotkeyEnabled = parseBool(value, cfg.overrideHotkeyEnabled);
      else if (keyLower == "togglevietnamese" || keyLower == "hotkey") {
        Hotkey h = hotkeyFromString(value);
        if (h.bound())
          cfg.hotkey = stringFromHotkey(h);
      } else if (keyLower == "toggleforcurrentapp" ||
                 keyLower == "overridehotkey") {
        cfg.overrideHotkey = hotkeyFromString(value);
      } else if (keyLower == "toggleexclusion" ||
                 keyLower == "toggleexclusionhotkey") {
        cfg.toggleExclusionHotkey = hotkeyFromString(value);
      } else if (keyLower == "gamingpolicy") {
        cfg.gamingPolicy = static_cast<GamingPolicy>(std::atoi(value.c_str()));
      } else if (keyLower == "gamingtrigger") {
        cfg.gamingTrigger = hotkeyFromString(value);
      } else if (keyLower == "gamingoverlayenabled") {
        cfg.gamingOverlayEnabled = parseBool(value, cfg.gamingOverlayEnabled);
      } else if (keyLower == "gamingoverlaycorner") {
        cfg.gamingOverlayCorner = std::atoi(value.c_str());
      } else if (keyLower == "soundongamingmodeswitch") {
        cfg.soundOnGamingModeSwitch = parseBool(value, cfg.soundOnGamingModeSwitch);
      } else if (keyLower == "notifyonautomaticmodeswitch") {
        cfg.notifyOnAutomaticModeSwitch = parseBool(value, cfg.notifyOnAutomaticModeSwitch);
      }
    } else if (section == "apploaitru" || section == "excludedbundles") {
      if (sawExcludedSection)
        cfg.excludedBundles.push_back(key);
    } else if (section == "autocomplete") {
      if (sawAutocompleteSection)
        cfg.autocompleteBundles.push_back(key);
    } else if (section == "ungdunggame") {
      if (sawGamingSection)
        cfg.gamingBundles.push_back(key);
    } else if (section == "ungdunggamepaste") {
      if (sawGamingPasteSection)
        cfg.gamingPasteBundles.push_back(key);
    }
  }
}

static void setAutostartEnabled(bool enabled) {
  NSString *folder = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents"];
  NSString *plistPath = [folder stringByAppendingPathComponent:@"org.vietki.app.plist"];
  if (enabled) {
    [[NSFileManager defaultManager] createDirectoryAtPath:folder
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSString *execPath = [[NSBundle mainBundle] executablePath];
    if (!execPath) return;
    NSDictionary *dict = @{
      @"Label": @"org.vietki.app",
      @"ProgramArguments": @[ execPath ],
      @"RunAtLoad": @YES,
      @"ProcessType": @"Interactive"
    };
    [dict writeToFile:plistPath atomically:YES];
  } else {
    [[NSFileManager defaultManager] removeItemAtPath:plistPath error:nil];
  }
}

void saveConfig(const AppConfig &cfg) {
  setAutostartEnabled(cfg.autostart);
  [[NSFileManager defaultManager] createDirectoryAtPath:supportDir()
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  std::ostringstream out;
  out << "\xEF\xBB\xBF";
  out << "; VietKi macOS configuration file.\n";
  out << "; This file is stored in ~/Library/Application "
         "Support/VietKi/config.ini.\n";
  out << "; Lines starting with ';' or '#' are comments.\n\n";

  out << "[Caidat]\n";
  out << "method="
      << (cfg.method == Method::VNI    ? "VNI"
          : cfg.method == Method::VIQR ? "VIQR"
                                       : "Telex")
      << "\n";
  out << "tone=" << (cfg.tone == TonePlacement::Old ? "Old" : "Modern") << "\n";
  out << "enabled=" << boolText(cfg.enabled) << "\n";
  out << "spellCheck=" << boolText(cfg.spellCheck) << "\n";
  out << "lockWordAfterCancel=" << boolText(cfg.lockWordAfterCancel) << "\n";
  out << "restore_after_space=" << boolText(cfg.restoreAfterSpace) << "\n";
  out << "autostart=" << boolText(cfg.autostart) << "\n";
  out << "autocompleteFix=" << boolText(cfg.autocompleteFix) << "\n";
  out << "soundOnGlobalToggle=" << boolText(cfg.soundOnGlobalToggle) << "\n";
  out << "soundOnExcludedToggle=" << boolText(cfg.soundOnExcludedToggle)
      << "\n";
  out << "exclusionFeatureOn=" << boolText(cfg.exclusionFeatureOn) << "\n";
  out << "revertOverrideOnBlur=" << boolText(cfg.revertOverrideOnBlur) << "\n";
  out << "typingStats=" << boolText(cfg.typingStats) << "\n";
  out << "toggleVietnameseEnabled="
      << boolText(cfg.toggleVietnameseHotkeyEnabled) << "\n";
  out << "toggleVietnamese=" << cfg.hotkey << "\n";
  out << "toggleForCurrentAppEnabled=" << boolText(cfg.overrideHotkeyEnabled)
      << "\n";
  out << "toggleForCurrentApp=" << stringFromHotkey(cfg.overrideHotkey) << "\n";
  out << "gamingPolicy=" << static_cast<int>(cfg.gamingPolicy) << "\n";
  out << "gamingTrigger=" << stringFromHotkey(cfg.gamingTrigger) << "\n";
  out << "gamingOverlayEnabled=" << boolText(cfg.gamingOverlayEnabled) << "\n";
  out << "gamingOverlayCorner=" << cfg.gamingOverlayCorner << "\n";
  out << "soundOnGamingModeSwitch=" << boolText(cfg.soundOnGamingModeSwitch) << "\n";
  out << "notifyOnAutomaticModeSwitch=" << boolText(cfg.notifyOnAutomaticModeSwitch) << "\n";
  out << "\n";

  out << "[Apploaitru]\n";
  out << "; One excluded bundle identifier per line. Example: "
         "com.apple.Terminal=\n";
  for (const auto &s : cfg.excludedBundles)
    if (!trim(s).empty())
      out << s << "=\n";
  out << "\n";

  out << "[Autocomplete]\n";
  for (const auto &s : cfg.autocompleteBundles)
    if (!trim(s).empty())
      out << s << "=\n";
  out << "\n";

  out << "[Ungdunggame]\n";
  for (const auto &s : cfg.gamingBundles)
    if (!trim(s).empty())
      out << s << "=\n";
  out << "\n";

  out << "[Ungdunggamepaste]\n";
  for (const auto &s : cfg.gamingPasteBundles)
    if (!trim(s).empty())
      out << s << "=\n";

  std::ofstream f(configPath(), std::ios::binary | std::ios::trunc);
  f << out.str();
}

// Phase 2 D.2 + Phase 5 E: resolve the effective Vietnamese state, with the
// Gaming Apps branch taking priority over the Excluded-Apps logic.
static bool resolveModeVN() {
  if (!g_state.config.enabled)
    return false;
  const std::string& app = g_state.currentApp;
  if (gamingAppliesTo(app)) {
    if (g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
      return overrideFor(app) == Override::ForceVN;
    return g_state.gamingSession.state() == GamingTypingState::Active;
  }
  switch (overrideFor(app)) {
  case Override::ForceVN:
    return true;
  default:
    break;
  }
  if (g_state.config.exclusionFeatureOn && isExcludedMember(app))
    return false;
  return true;
}

// Phase 2 E + Phase 3 + Phase 5 G.1: pick the status icon.
static IconState resolveIcon() {
  if (!g_state.config.enabled)
    return IconState::E; // master off → English
  const std::string& app = g_state.currentApp;
  if (gamingAppliesTo(app)) {
    if (g_state.config.gamingPolicy == GamingPolicy::ToggleForCurrentApp)
      return overrideFor(app) == Override::ForceVN ? IconState::GamingVN
                                                   : IconState::Gaming;
    return g_state.gamingSession.state() == GamingTypingState::Idle
               ? IconState::Gaming
               : IconState::GamingVN;
  }
  if (!g_state.currentModeVN)
    return IconState::VMinus; // this app English, master on
  if (g_state.config.exclusionFeatureOn &&
      isExcludedMember(app) &&
      overrideFor(app) == Override::ForceVN)
    return IconState::VPlus;
  return IconState::V;
}

} // namespace vietki::mac

// ---------------------------------------------------------------------------
// App delegate + status-bar menu
// ---------------------------------------------------------------------------
@interface VietKiDelegate : NSObject <NSApplicationDelegate>
@property(strong) NSStatusItem *statusItem;
@property(strong) NSMenu *statusMenu;
@property(strong) NSWindow *settingsWindow;
- (void)updateUI;
@end

@implementation VietKiDelegate

- (NSImage *)iconNamed:(NSString *)name {
  NSString *path = [[NSBundle mainBundle] pathForResource:name ofType:@"png"];
  if (!path)
    return nil;
  NSImage *img = [[NSImage alloc] initWithContentsOfFile:path];
  img.size = NSMakeSize(18, 18);
  // Phase 2 E: keep colour to distinguish states — do NOT mark as template.
  // (`template` is a C++ keyword, so call the setter explicitly here.)
  [img setTemplate:NO];
  return img;
}

- (void)applyIcon {
  AppState &st = state();
  NSString *name = @"menubar_v";
  NSString *fallback = @"V";
  if (st.currentIcon == IconState::E) {
    name = @"menubar_e";
    fallback = @"E";
  } else if (st.currentIcon == IconState::VPlus) {
    name = @"menubar_vplus";
    fallback = @"V+";
  } else if (st.currentIcon == IconState::VMinus) {
    name = @"menubar_vminus";
    fallback = @"V-";
  } else if (st.currentIcon == IconState::Gaming) {
    name = nil;
    fallback = @"G";
  } else if (st.currentIcon == IconState::GamingVN) {
    name = nil;
    fallback = @"G+";
  }
  NSImage *img = name ? [self iconNamed:name] : nil;
  self.statusItem.button.image = img;
  self.statusItem.button.title =
      img ? @"" : fallback; // text fallback if no asset
}

- (void)rebuildMenu {
  AppConfig &c = state().config;
  AppState &st = state();
  NSMenu *menu = [[NSMenu alloc] init];

  NSMenuItem *toggle = [[NSMenuItem alloc]
      initWithTitle:(c.enabled ? @"Tắt tiếng Việt" : @"Bật tiếng Việt")
             action:@selector(onToggle)
      keyEquivalent:@""];
  toggle.target = self;
  [menu addItem:toggle];

  NSMenuItem *ov =
      [[NSMenuItem alloc] initWithTitle:@"Đảo chế độ cho app hiện tại"
                                 action:@selector(onOverride)
                          keyEquivalent:@""];
  ov.target = self;
  [menu addItem:ov];

  NSMenuItem *excl =
      [[NSMenuItem alloc] initWithTitle:@"Bật tính năng loại trừ"
                                 action:@selector(onToggleExclusion)
                          keyEquivalent:@""];
  excl.target = self;
  excl.state =
      c.exclusionFeatureOn ? NSControlStateValueOn : NSControlStateValueOff;
  [menu addItem:excl];
  [menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *telex = [[NSMenuItem alloc] initWithTitle:@"Kiểu gõ: Telex"
                                                 action:@selector(onTelex)
                                          keyEquivalent:@""];
  telex.target = self;
  telex.state = (c.method == Method::Telex) ? NSControlStateValueOn
                                            : NSControlStateValueOff;
  [menu addItem:telex];
  NSMenuItem *vni = [[NSMenuItem alloc] initWithTitle:@"Kiểu gõ: VNI"
                                               action:@selector(onVni)
                                        keyEquivalent:@""];
  vni.target = self;
  vni.state = (c.method == Method::VNI) ? NSControlStateValueOn
                                        : NSControlStateValueOff;
  [menu addItem:vni];
  [menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *modern = [[NSMenuItem alloc] initWithTitle:@"Đặt dấu: Kiểu mới"
                                                  action:@selector(onModern)
                                           keyEquivalent:@""];
  modern.target = self;
  modern.state = (c.tone == TonePlacement::Modern) ? NSControlStateValueOn
                                                   : NSControlStateValueOff;
  [menu addItem:modern];
  NSMenuItem *old = [[NSMenuItem alloc] initWithTitle:@"Đặt dấu: Kiểu cũ"
                                               action:@selector(onOld)
                                        keyEquivalent:@""];
  old.target = self;
  old.state = (c.tone == TonePlacement::Old) ? NSControlStateValueOn
                                             : NSControlStateValueOff;
  [menu addItem:old];
  [menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *settings = [[NSMenuItem alloc] initWithTitle:@"Cài đặt…"
                                                    action:@selector(onSettings)
                                             keyEquivalent:@","];
  settings.target = self;
  [menu addItem:settings];

  NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"Thoát"
                                                action:@selector(onQuit)
                                         keyEquivalent:@"q"];
  quit.target = self;
  [menu addItem:quit];

  self.statusMenu = menu;
  (void)st;
}

// Repaint icon + menu after any state change.
- (void)updateUI {
  [self applyIcon];
  [self rebuildMenu];
  refreshSettingsWindow();
}

- (void)onToggle {
  toggleEnabled();
}
- (void)onOverride {
  toggleOverrideForCurrentApp();
}
- (void)onToggleExclusion {
  toggleExclusionFeature();
}
- (void)onTelex {
  setMethod(Method::Telex);
}
- (void)onVni {
  setMethod(Method::VNI);
}
- (void)onModern {
  setTonePlacement(TonePlacement::Modern);
}
- (void)onOld {
  setTonePlacement(TonePlacement::Old);
}
- (void)onSettings {
  openSettings();
}
- (void)onQuit {
  [NSApp terminate:nil];
}

- (void)onStatusItemClick:(id)sender {
  NSEvent *event = NSApp.currentEvent;
  if (event.clickCount >= 2) {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(showStatusMenu)
                                               object:nil];
    openSettings();
    return;
  }
  if (event.type == NSEventTypeRightMouseUp) {
    [self showStatusMenu];
    return;
  }
  [self performSelector:@selector(showStatusMenu)
             withObject:nil
             afterDelay:0.22];
}

- (void)showStatusMenu {
  [self.statusItem popUpStatusItemMenu:self.statusMenu];
}

- (void)frontmostChanged:(NSNotification *)note {
  NSRunningApplication *app = note.userInfo[NSWorkspaceApplicationKey];
  std::string bid = app.bundleIdentifier ? app.bundleIdentifier.UTF8String : "";
  if (bid.empty() && app.localizedName) {
    bid = app.localizedName.UTF8String;
  }
  refreshForegroundApp(bid);
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
  loadConfig(state().config);

  BOOL trusted = AXIsProcessTrusted();
  if (!trusted) {
    NSDictionary *opts = @{(__bridge id)kAXTrustedCheckOptionPrompt : @YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
  }

  static Engine engine(Config{state().config.method, state().config.tone,
                              state().config.enabled, state().config.spellCheck,
                              state().config.lockWordAfterCancel,
                              state().config.restoreAfterSpace});
  state().engine = &engine;

  self.statusItem = [[NSStatusBar systemStatusBar]
      statusItemWithLength:NSVariableStatusItemLength];
  [self updateUI];
  self.statusItem.button.target = self;
  self.statusItem.button.action = @selector(onStatusItemClick:);
  [self.statusItem.button
      sendActionOn:(NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp)];

  [[NSWorkspace.sharedWorkspace notificationCenter]
      addObserver:self
         selector:@selector(frontmostChanged:)
             name:NSWorkspaceDidActivateApplicationNotification
           object:nil];

  if (!startEventTap()) {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"VietKi cần quyền Accessibility & Input Monitoring";
    a.informativeText =
        @"Mở System Settings → Privacy & Security → "
        @"Accessibility và Input Monitoring, bật VietKi, rồi mở lại.";
    [a runModal];
  }

  // macOS Tahoe (26) can silently disable the CGEventTap — after sleep/wake,
  // under load, or when TCC re-evaluates code identity — without ever firing
  // the disabled-callback. A periodic watchdog polls the tap and revives it so
  // Vietnamese typing recovers on its own instead of forcing an app restart
  // (the EVKey "không gõ được / Not Responding" symptom).
  [NSTimer scheduledTimerWithTimeInterval:5.0
                                  repeats:YES
                                    block:^(NSTimer *timer) {
    (void)timer;
    vietki::mac::ensureTapAlive();
  }];

  // Re-arm immediately on wake rather than waiting for the next watchdog tick,
  // and drop any stale composition that spanned the sleep.
  [[NSWorkspace.sharedWorkspace notificationCenter]
      addObserver:self
         selector:@selector(onWake:)
             name:NSWorkspaceDidWakeNotification
           object:nil];

  openSettings();
}

- (void)onWake:(NSNotification *)note {
  (void)note;
  if (state().engine)
    state().engine->reset();
  vietki::mac::ensureTapAlive();
}

- (void)applicationWillTerminate:(NSNotification *)note {
  [[NSWorkspace.sharedWorkspace notificationCenter] removeObserver:self];
  stopEventTap();
  vietki::mac::saveTypingStats(); // main thread, safe point for the one disk write
}

@end

// ---------------------------------------------------------------------------
// State mutators (C++ surface used by the tap + menu + settings window)
// ---------------------------------------------------------------------------
namespace vietki::mac {

void applyResolvedState() {
  g_state.currentModeVN = resolveModeVN();
  g_state.currentIcon = resolveIcon();
  if (g_state.engine)
    g_state.engine->setConfig(Config{
        g_state.config.method, g_state.config.tone, g_state.currentModeVN,
        g_state.config.spellCheck, g_state.config.lockWordAfterCancel,
        g_state.config.restoreAfterSpace});
  
  if (!g_overlay) {
    g_overlay = [[GamingOverlayController alloc] init];
  }
  if (g_state.config.gamingOverlayEnabled && gamingAppliesTo(g_state.currentApp)) {
    [g_overlay showState:g_state.currentIcon corner:g_state.config.gamingOverlayCorner];
  } else {
    [g_overlay hide];
  }
  
  [g_delegate updateUI];
}

void refreshForegroundApp(const std::string &bundleId) {
  std::string prev = g_state.currentApp;
  if (g_state.gamingSession.state() != GamingTypingState::Idle) {
    g_state.gamingSession.reset();
    notifyGamingMode(GamingEndReason::EndedFocus);
  }
  discardPendingPaste();
  if (g_state.config.revertOverrideOnBlur && !prev.empty() && prev != bundleId)
    g_state.perAppOverride.erase(prev);
  g_state.currentApp = bundleId;
  applyResolvedState();
  if (prev != bundleId && gamingAppliesTo(bundleId) &&
      g_state.config.gamingPolicy == GamingPolicy::TemporaryTrigger) {
    notifyGamingMode(GamingEndReason::EnteredGame);
  }
}

void toggleEnabled() {
  g_state.config.enabled = !g_state.config.enabled;
  if (!g_state.config.enabled) {
    g_state.gamingSession.reset();
  }
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
  const std::string &app = g_state.currentApp;
  if (app.empty())
    return;
  
  if (gamingAppliesTo(app)) {
    if (g_state.config.gamingPolicy != GamingPolicy::ToggleForCurrentApp) return;
    Override cur = overrideFor(app);
    bool forceVN = cur != Override::ForceVN;
    g_state.perAppOverride[app] = forceVN ? Override::ForceVN : Override::None;
    applyResolvedState();
    playExcludedToggleSound(forceVN);
    return;
  }
  
  if (!g_state.config.exclusionFeatureOn || !isExcludedMember(app))
    return;
  Override cur = overrideFor(app);
  bool forceVN = cur != Override::ForceVN;
  g_state.perAppOverride[app] = forceVN ? Override::ForceVN : Override::None;
  applyResolvedState();
  playExcludedToggleSound(forceVN);
}

void toggleExclusionFeature() {
  g_state.config.exclusionFeatureOn = !g_state.config.exclusionFeatureOn;
  saveConfig(g_state.config);
  applyResolvedState();
}

bool isGamingApp(const std::string& bundleId) {
  std::string lowerId = bundleId;
  std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), ::tolower);
  for (const auto& b : g_state.config.gamingBundles) {
    std::string lowerB = b;
    std::transform(lowerB.begin(), lowerB.end(), lowerB.begin(), ::tolower);
    if (lowerB == lowerId) return true;
  }
  return false;
}

bool isGamingPasteApp(const std::string& bundleId) {
  std::string lowerId = bundleId;
  std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), ::tolower);
  for (const auto& b : g_state.config.gamingPasteBundles) {
    std::string lowerB = b;
    std::transform(lowerB.begin(), lowerB.end(), lowerB.begin(), ::tolower);
    if (lowerB == lowerId) return true;
  }
  return false;
}

bool gamingAppliesTo(const std::string& bundleId) {
  return g_state.config.gamingPolicy != GamingPolicy::Disabled && isGamingApp(bundleId);
}

void applyGamingPolicy(GamingPolicy p) {
  AppConfig& c = g_state.config;
  if (p != GamingPolicy::Disabled) {
    if (!c.gamingListInitialized) {
      if (c.gamingBundles.empty()) {
        c.gamingBundles = defaultGamingBundles();
      }
      c.gamingListInitialized = true;
    }
  }
  if (p != c.gamingPolicy) {
    for (const auto& g : c.gamingBundles) {
      g_state.perAppOverride.erase(g);
    }
    g_state.gamingSession.reset();
  }
  c.gamingPolicy = p;
  applyResolvedState();
}

void notifyGamingMode(GamingEndReason reason) {
  const AppConfig& c = g_state.config;
  if (c.gamingPolicy != GamingPolicy::TemporaryTrigger) return;
  if (c.soundOnGamingModeSwitch) {
    NSSound *sound = [NSSound soundNamed:(reason == GamingEndReason::Triggered ? @"Glass" : @"Tink")];
    if (sound) [sound play];
  }
}

void endGamingTypingSession(GamingEndReason reason) {
  if (g_state.gamingSession.state() == GamingTypingState::Idle) return;
  g_state.gamingSession.onContextLost();
  if (g_state.engine) g_state.engine->reset();
  applyResolvedState();
  notifyGamingMode(reason);
}

std::vector<std::string> defaultGamingBundles() {
  return {
    "com.riotgames.leagueoflegends",
    "com.roblox.RobloxPlayer",
    "com.mojang.minecraftpe",
    "net.minecraft",
    "com.valvesoftware.dota2",
    "com.valvesoftware.cs2",
    "com.blizzard.worldofwarcraft",
    "com.larian.bg3",
    "League of Legends",
    "Roblox",
    "Minecraft",
    "Dota 2",
    "cs2",
    "csgo_osx64"
  };
}

void playGlobalToggleSound(bool vietnameseNow) {
  if (!g_state.config.soundOnGlobalToggle)
    return;
  NSSound *sound = [NSSound soundNamed:(vietnameseNow ? @"Ping" : @"Basso")];
  if (sound)
    [sound play];
  else
    NSBeep();
}

void playExcludedToggleSound(bool forceVietnameseNow) {
  if (!g_state.config.soundOnExcludedToggle)
    return;
  NSSound *sound =
      [NSSound soundNamed:(forceVietnameseNow ? @"Glass" : @"Tink")];
  if (sound)
    [sound play];
  else
    NSBeep();
}

} // namespace vietki::mac

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:
             NSApplicationActivationPolicyAccessory]; // LSUIElement
    VietKiDelegate *delegate = [[VietKiDelegate alloc] init];
    g_delegate = delegate;
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
