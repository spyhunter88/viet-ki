// Internal types shared by the engine implementation files. Not installed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vietki {

enum class Mark : uint8_t { None, Circumflex, Breve, Horn, Bar };
enum class Tone : uint8_t { None, Acute, Grave, Hook, Tilde, Dot };

// One letter slot of a syllable while it is being composed.
struct Unit {
    char32_t base = 0;          // lowercased letter a-z (or any literal char)
    Mark mark = Mark::None;
    bool upper = false;         // was typed uppercase
    bool synthetic = false;     // created by a standalone Telex 'w' -> u-horn
    bool literal = false;       // forced plain: not subject to further transforms
    Tone tone = Tone::None;     // assigned during display building
};

// A processed syllable: the letter slots plus the single syllable tone.
struct Syllable {
    std::vector<Unit> units;
    Tone tone = Tone::None;
};

// Phase 4 B.2 — three-way structural classification of the current buffer.
enum class SyllableStatus {
    ViablePrefix, // not yet complete, but could still become a valid syllable
    Valid,        // a complete, valid Vietnamese syllable
    Invalid       // cannot grow into any valid Vietnamese syllable
};

// Phase 4 C.6 — what a method processor reports back to the engine. The engine
// owns the word state, so the processor only describes the *current* key:
// `cancelled` is true when this key undid a tone/transform (a re-typed mark).
struct ProcessResult {
    Syllable syllable;
    SyllableStatus status = SyllableStatus::ViablePrefix;
    bool cancelled = false;
};

bool isVowelBase(char32_t b);
bool hasVowel(const std::vector<Unit>& units);

// Phase 3 E.2 — Vietnamese syllable-structure checks (no word dictionary).
//
// A "viable" syllable is one that is, or could still grow into, a valid
// Vietnamese syllable: its onset/nucleus/coda are prefixes of the allowed
// tables. Literal consonant echoes (e.g. the dropped 's' in "as") are ignored
// so deliberate double-tap output is not flagged. A non-viable buffer is the
// signal that the current word is foreign and must pass through literally.
bool isSyllableViable(const std::vector<Unit>& units);

// True if the units form a complete, valid Vietnamese syllable for the given
// tone (used for the spell-check gate). Honours the stop-coda tone constraint.
bool isValidVietnameseSyllable(const std::vector<Unit>& units, Tone tone);

// Phase 4 B.2 — classify a buffer as Valid / ViablePrefix / Invalid. This is
// the structural decision the engine uses to lock a foreign word to literal.
SyllableStatus classify(const std::vector<Unit>& units, Tone tone);

// Compose a single NFC code point from base/mark/tone and case.
char32_t composeChar(char32_t base, Mark mark, Tone tone, bool upper);

// Choose which vowel (index into the effective vowel cluster) carries the tone.
int chooseToneIndex(const std::vector<Unit>& vowels, bool hasFinal, bool modern);

// Render a processed syllable to its display string (UTF-32, NFC).
std::u32string buildDisplay(const Syllable& syl, bool modern);

// Method-specific processing of the raw buffer into a syllable (Phase 4 C.6).
// The processor always applies Telex/VNI transforms with component targeting
// (E.1); it does NOT decide foreign-word fallback itself. It returns the built
// syllable, its structural status, and whether the *last* key cancelled a mark.
// The engine alone owns the Composing -> LiteralLocked transition.
ProcessResult processTelex(const std::u32string& raw);
ProcessResult processVni(const std::u32string& raw);
Syllable processPlain(const std::u32string& raw); // literal pass-through (VIQR / locked)

} // namespace vietki
