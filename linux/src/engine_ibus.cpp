// VietKi IBus engine (Phase 4.1). A GObject subclass of IBusEngine that wraps
// the OS-independent core (vietki::Engine) and renders composition as an
// underlined preedit, committing on a word break.
//
// Model (PHASE4.1 §2): IBus has no backspace/retype channel, so we ignore the
// core's KeyResult diff entirely and, after every key, read the full syllable
// via Engine::preedit() and push it as the preedit text. On a word break
// (space, punctuation, Enter, arrows, ...) we commit the preedit and let the
// break key pass through to the app.
#include "engine_ibus.h"

#include <glib.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "vietki/engine.h"

using vietki::Config;
using vietki::Engine;
using vietki::Method;
using vietki::TonePlacement;

// ---------------------------------------------------------------------------
// Config: read/write ~/.config/vietki/config.ini. On Linux only the general
// typing options apply (PHASE4.1 §4); the hotkey/exclusion/gaming keys used by
// the Windows/macOS shells are ignored. We accept [General] (canonical) as well
// as the legacy [Caidat]/[Settings] section names for compatibility.
// ---------------------------------------------------------------------------
namespace {

std::string trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool parseBool(const std::string& v, bool def) {
    std::string s = lower(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

std::string configDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/vietki";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/vietki";
}

std::string configPath() { return configDir() + "/config.ini"; }

Config loadCoreConfig() {
    Config cfg; // defaults: Telex, Modern, enabled, spellCheck on
    std::ifstream in(configPath(), std::ios::binary);
    if (!in) return cfg;

    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string raw = trim(line);
        if (raw.empty() || raw[0] == ';' || raw[0] == '#') continue;
        if (raw.front() == '[' && raw.back() == ']') {
            section = lower(trim(raw.substr(1, raw.size() - 2)));
            continue;
        }
        size_t eq = raw.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(raw.substr(0, eq)));
        std::string value = trim(raw.substr(eq + 1));
        // Strip a trailing inline comment and surrounding quotes.
        for (size_t i = 0; i < value.size(); ++i)
            if (value[i] == ';' || value[i] == '#') { value = trim(value.substr(0, i)); break; }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);

        const bool general =
            section == "general" || section == "caidat" || section == "settings";
        if (!general) continue; // ignore Linux-irrelevant sections

        if (key == "method") {
            std::string v = lower(value);
            cfg.method = (v == "vni")  ? Method::VNI
                       : (v == "viqr") ? Method::VIQR
                                       : Method::Telex;
        } else if (key == "tone") {
            cfg.tone = (lower(value) == "old") ? TonePlacement::Old
                                               : TonePlacement::Modern;
        } else if (key == "spellcheck") {
            cfg.spellCheck = parseBool(value, cfg.spellCheck);
        } else if (key == "enabled") {
            cfg.enabled = parseBool(value, cfg.enabled);
        } else if (key == "lockwordaftercancel") {
            cfg.lockWordAfterCancel = parseBool(value, cfg.lockWordAfterCancel);
        } else if (key == "restoreafterspace" || key == "restore_after_space") {
            cfg.restoreAfterSpace = parseBool(value, cfg.restoreAfterSpace);
        }
    }
    return cfg;
}

void saveCoreConfig(const Config& cfg) {
    g_mkdir_with_parents(configDir().c_str(), 0700);
    std::ostringstream out;
    out << "; VietKi (Linux/IBus) configuration.\n"
        << "; Only [General] applies on Linux; Việt/Anh switching is done by\n"
        << "; changing the IBus input source (Super+Space), not by a hotkey.\n\n"
        << "[General]\n"
        << "method=" << (cfg.method == Method::VNI ? "VNI"
                        : cfg.method == Method::VIQR ? "VIQR" : "Telex") << "\n"
        << "tone=" << (cfg.tone == TonePlacement::Old ? "Old" : "Modern") << "\n"
        << "spellCheck=" << (cfg.spellCheck ? "true" : "false") << "\n"
        << "enabled=" << (cfg.enabled ? "true" : "false") << "\n";
    std::ofstream f(configPath(), std::ios::binary | std::ios::trunc);
    f << out.str();
}

// UTF-32 (core) -> newly allocated UTF-8 (IBus). Caller g_free()s the result.
gchar* toUtf8(const std::u32string& s) {
    return g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(s.data()),
                          static_cast<glong>(s.size()), nullptr, nullptr, nullptr);
}

bool isComposeChar(gunichar c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

} // namespace

// ---------------------------------------------------------------------------
// GObject type
// ---------------------------------------------------------------------------
struct _IBusVietKiEngine {
    IBusEngine parent;

    Engine* core;
    IBusPropList* props;    // owned (one ref)
    IBusProperty* prop_vn;  // borrowed (owned by props)
    IBusProperty* prop_telex;
    IBusProperty* prop_vni;
};

struct _IBusVietKiEngineClass {
    IBusEngineClass parent;
};

G_DEFINE_TYPE(IBusVietKiEngine, ibus_vietki_engine, IBUS_TYPE_ENGINE)

// --- helpers ---------------------------------------------------------------
namespace {

void updatePreedit(IBusVietKiEngine* self) {
    IBusEngine* engine = IBUS_ENGINE(self);
    const std::u32string& pre = self->core->preedit();
    if (pre.empty()) {
        ibus_engine_hide_preedit_text(engine);
        return;
    }
    gchar* utf8 = toUtf8(pre);
    IBusText* text = ibus_text_new_from_string(utf8);
    guint len = static_cast<guint>(pre.size());
    ibus_text_append_attribute(text, IBUS_ATTR_TYPE_UNDERLINE,
                               IBUS_ATTR_UNDERLINE_SINGLE, 0, len);
    ibus_engine_update_preedit_text(engine, text, len, TRUE);
    g_free(utf8);
}

// Commit whatever is being composed (if anything) and clear the composition.
void commitAndReset(IBusVietKiEngine* self) {
    IBusEngine* engine = IBUS_ENGINE(self);
    const std::u32string& pre = self->core->preedit();
    if (!pre.empty()) {
        gchar* utf8 = toUtf8(pre);
        IBusText* text = ibus_text_new_from_string(utf8);
        ibus_engine_commit_text(engine, text);
        g_free(utf8);
    }
    self->core->reset();
    ibus_engine_hide_preedit_text(engine);
}

// Preedit-model Backspace: drop the last raw key and replay the buffer so the
// composed display shrinks one keystroke at a time. Returns TRUE if consumed.
gboolean handleBackspace(IBusVietKiEngine* self) {
    if (self->core->preedit().empty()) return FALSE; // nothing composing
    std::u32string raw = self->core->rawBuffer();     // copy before reset
    if (raw.empty()) return FALSE;
    raw.pop_back();
    self->core->reset();
    for (char32_t ch : raw) self->core->onChar(ch, false);
    updatePreedit(self);
    return TRUE;
}

void registerProperties(IBusVietKiEngine* self) {
    if (self->props) ibus_engine_register_properties(IBUS_ENGINE(self), self->props);
}

void buildProperties(IBusVietKiEngine* self) {
    const Config& cfg = self->core->config();
    self->props = ibus_prop_list_new();
    g_object_ref_sink(self->props);

    self->prop_vn = ibus_property_new(
        "InputMode", PROP_TYPE_TOGGLE,
        ibus_text_new_from_string("Tiếng Việt"), nullptr,
        ibus_text_new_from_string("Bật/tắt gõ tiếng Việt"),
        TRUE, TRUE,
        cfg.enabled ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED, nullptr);
    ibus_prop_list_append(self->props, self->prop_vn);

    const bool telex = cfg.method == Method::Telex;
    IBusPropList* methods = ibus_prop_list_new();
    self->prop_telex = ibus_property_new(
        "method.telex", PROP_TYPE_RADIO,
        ibus_text_new_from_string("Telex"), nullptr, nullptr, TRUE, TRUE,
        telex ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED, nullptr);
    self->prop_vni = ibus_property_new(
        "method.vni", PROP_TYPE_RADIO,
        ibus_text_new_from_string("VNI"), nullptr, nullptr, TRUE, TRUE,
        telex ? PROP_STATE_UNCHECKED : PROP_STATE_CHECKED, nullptr);
    ibus_prop_list_append(methods, self->prop_telex);
    ibus_prop_list_append(methods, self->prop_vni);

    IBusProperty* methodMenu = ibus_property_new(
        "method", PROP_TYPE_MENU,
        ibus_text_new_from_string("Kiểu gõ"), nullptr,
        ibus_text_new_from_string("Chọn kiểu gõ (Telex/VNI)"),
        TRUE, TRUE, PROP_STATE_UNCHECKED, methods);
    ibus_prop_list_append(self->props, methodMenu);
}

} // namespace

// --- vfuncs ----------------------------------------------------------------
static gboolean ibus_vietki_engine_process_key_event(IBusEngine* engine,
                                                      guint keyval, guint keycode,
                                                      guint modifiers) {
    (void)keycode;
    IBusVietKiEngine* self = reinterpret_cast<IBusVietKiEngine*>(engine);

    // 1. Ignore key releases.
    if (modifiers & IBUS_RELEASE_MASK) return FALSE;

    // Master off: behave like a plain "English (US)" layout.
    if (!self->core->config().enabled) return FALSE;

    // 2. A Ctrl/Alt/Super/Meta chord is a shortcut, not typing: commit and let
    //    the app handle it. (Shift/CapsLock/NumLock alone are fine.)
    if (modifiers & (IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK |
                     IBUS_HYPER_MASK | IBUS_META_MASK | IBUS_MOD4_MASK)) {
        commitAndReset(self);
        return FALSE;
    }

    // 5. Backspace: edit the preedit if composing, else pass through.
    if (keyval == IBUS_KEY_BackSpace) {
        return handleBackspace(self);
    }

    // 4. Word-break / navigation keys: commit the preedit, then pass through.
    switch (keyval) {
        case IBUS_KEY_space:
        case IBUS_KEY_Return:
        case IBUS_KEY_KP_Enter:
        case IBUS_KEY_Tab:
        case IBUS_KEY_ISO_Left_Tab:
        case IBUS_KEY_Escape:
        case IBUS_KEY_Left:  case IBUS_KEY_Right:
        case IBUS_KEY_Up:    case IBUS_KEY_Down:
        case IBUS_KEY_Home:  case IBUS_KEY_End:
        case IBUS_KEY_Page_Up: case IBUS_KEY_Page_Down:
        case IBUS_KEY_Delete: case IBUS_KEY_Insert:
            commitAndReset(self);
            return FALSE;
        default:
            break;
    }

    // 3. Map to a Unicode character.
    gunichar c = ibus_keyval_to_unicode(keyval);
    if (c == 0) {                 // function key etc.: not typing.
        commitAndReset(self);
        return FALSE;
    }
    if (!isComposeChar(c)) {       // punctuation/symbol: end the word.
        commitAndReset(self);
        return FALSE;
    }

    // 6. A composing character: feed the core, refresh the preedit, swallow.
    self->core->onChar(static_cast<char32_t>(c), false);
    updatePreedit(self);
    return TRUE;
}

static void ibus_vietki_engine_focus_in(IBusEngine* engine) {
    IBusVietKiEngine* self = reinterpret_cast<IBusVietKiEngine*>(engine);
    self->core->reset();
    registerProperties(self);
    ibus_engine_hide_preedit_text(engine);
}

static void ibus_vietki_engine_focus_out(IBusEngine* engine) {
    commitAndReset(reinterpret_cast<IBusVietKiEngine*>(engine));
}

static void ibus_vietki_engine_reset(IBusEngine* engine) {
    commitAndReset(reinterpret_cast<IBusVietKiEngine*>(engine));
}

static void ibus_vietki_engine_enable(IBusEngine* engine) {
    IBusVietKiEngine* self = reinterpret_cast<IBusVietKiEngine*>(engine);
    self->core->reset();
    registerProperties(self);
}

static void ibus_vietki_engine_disable(IBusEngine* engine) {
    commitAndReset(reinterpret_cast<IBusVietKiEngine*>(engine));
}

static void ibus_vietki_engine_property_activate(IBusEngine* engine,
                                                 const gchar* prop_name,
                                                 guint prop_state) {
    (void)prop_state;
    IBusVietKiEngine* self = reinterpret_cast<IBusVietKiEngine*>(engine);
    // Flush any in-progress word before switching settings, so it lands with
    // the old method and the preedit UI does not go stale.
    commitAndReset(self);
    Config cfg = self->core->config();

    if (g_strcmp0(prop_name, "InputMode") == 0) {
        cfg.enabled = !cfg.enabled;
        self->core->setConfig(cfg);
        ibus_property_set_state(
            self->prop_vn,
            cfg.enabled ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
        ibus_engine_update_property(engine, self->prop_vn);
        saveCoreConfig(cfg);
    } else if (g_strcmp0(prop_name, "method.telex") == 0 ||
               g_strcmp0(prop_name, "method.vni") == 0) {
        const bool telex = g_strcmp0(prop_name, "method.telex") == 0;
        cfg.method = telex ? Method::Telex : Method::VNI;
        self->core->setConfig(cfg);
        ibus_property_set_state(self->prop_telex,
                                telex ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
        ibus_property_set_state(self->prop_vni,
                                telex ? PROP_STATE_UNCHECKED : PROP_STATE_CHECKED);
        ibus_engine_update_property(engine, self->prop_telex);
        ibus_engine_update_property(engine, self->prop_vni);
        saveCoreConfig(cfg);
    }
}

// --- construction / teardown ----------------------------------------------
static void ibus_vietki_engine_init(IBusVietKiEngine* self) {
    self->core = new Engine(loadCoreConfig());
    self->props = nullptr;
    self->prop_vn = self->prop_telex = self->prop_vni = nullptr;
    buildProperties(self);
}

static void ibus_vietki_engine_finalize(GObject* object) {
    IBusVietKiEngine* self = reinterpret_cast<IBusVietKiEngine*>(object);
    delete self->core;
    self->core = nullptr;
    if (self->props) {
        g_object_unref(self->props);
        self->props = nullptr;
    }
    G_OBJECT_CLASS(ibus_vietki_engine_parent_class)->finalize(object);
}

static void ibus_vietki_engine_class_init(IBusVietKiEngineClass* klass) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    IBusEngineClass* engine_class = IBUS_ENGINE_CLASS(klass);

    object_class->finalize = ibus_vietki_engine_finalize;
    engine_class->process_key_event = ibus_vietki_engine_process_key_event;
    engine_class->focus_in = ibus_vietki_engine_focus_in;
    engine_class->focus_out = ibus_vietki_engine_focus_out;
    engine_class->reset = ibus_vietki_engine_reset;
    engine_class->enable = ibus_vietki_engine_enable;
    engine_class->disable = ibus_vietki_engine_disable;
    engine_class->property_activate = ibus_vietki_engine_property_activate;
}
