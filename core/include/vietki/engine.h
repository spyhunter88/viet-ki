// VietKi core engine — OS-independent Vietnamese input method logic.
//
// The engine keeps a "raw buffer" of the printable keys typed for the current
// syllable and, on every key, rebuilds the whole display string from scratch
// (see IMPLEMENTATION_GUIDE.md 3.1). It then diffs the new display against the
// previous one and reports how many Backspaces to send and what text to commit.
//
// This header must NOT include any OS headers; it is compiled into both the
// Windows and macOS shells and is unit-tested in isolation.
#pragma once

#include <string>

namespace vietki {

enum class Method { Telex, VNI, VIQR };

// Tone position for the open oa/oe/uy clusters with no final consonant.
// Modern (default) places the tone on the second vowel (hoà, thuý); Old places
// it on the first (hòa, thúy). See tone_placement.cpp and the B.4 test cases.
enum class TonePlacement { Modern, Old };

// Phase 4 D — per-word composition state. While Composing the engine rebuilds
// the display from the raw buffer on every key. Once a word is LiteralLocked
// (an explicit cancel with lockWordAfterCancel on, or a structurally invalid
// word under spell check) every further key in the word passes through as a
// literal — no tones, no transforms, no re-running spell check — until a word
// or context break resets the engine.
enum class CompositionMode { Composing, LiteralLocked };

struct Config {
    Method method = Method::Telex;
    TonePlacement tone = TonePlacement::Modern;
    bool enabled = true; // Vietnamese on/off
    // Spell checking (Phase 3 E.2): only apply tones/transforms while the word
    // stays a plausible Vietnamese syllable, and let words that cannot be
    // Vietnamese (e.g. "override") pass through as typed. When false the engine
    // applies marks "freely" the way Phase 1/2 did.
    bool spellCheck = true;
    // Phase 4 C.4: after an explicit cancel (re-typing a tone/transform key to
    // undo it, e.g. "off" -> "of"), lock the rest of the word to literal so
    // later keys cannot re-create marks ("offf" -> "off", "tesst" -> "test").
    // When false, only the current key is cancelled and composing continues
    // freely ("offf" -> "òf"), which suits sequences like "đượợợợợc".
    bool lockWordAfterCancel = true;
};

// Result of feeding one key to the engine, for the OS shell to execute.
struct KeyResult {
    bool swallow = false;        // true: eat the original key, do not pass it to the app
    int backspaces = 0;          // number of Backspace presses to send first
    std::u32string commit;       // UTF-32 text to type after the backspaces
};

class Engine {
public:
    explicit Engine(Config cfg);

    void setConfig(const Config& cfg);
    const Config& config() const { return cfg_; }

    // ch: printable character already mapped from keycode + Shift/CapsLock,
    //     or 0 when the key is not a character (e.g. a bare modifier).
    // isWordBreak: true for keys that end the current word (space, tab, enter,
    //     punctuation, arrows, Home/End/PageUp/Down, Esc, Delete, ...).
    KeyResult onChar(char32_t ch, bool isWordBreak);

    // The user pressed Backspace themselves: undo the last raw key.
    KeyResult onBackspace();

    // Clear the buffer (focus change, mouse click, word break).
    void reset();

    // Exposed for testing / shells that want to inspect the current composition.
    const std::u32string& rawBuffer() const { return raw_; }
    const std::u32string& display() const { return display_; }

private:
    KeyResult emitDiff(const std::u32string& next, char32_t ch);

    Config cfg_;
    std::u32string raw_;     // printable keys typed for the current syllable
    std::u32string display_; // what is currently shown on screen for this syllable
    CompositionMode mode_ = CompositionMode::Composing; // Phase 4 D
};

} // namespace vietki
