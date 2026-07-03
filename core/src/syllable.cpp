// Small syllable helpers plus the Phase 3 E.2 syllable-structure tables.
//
// These tables are how the engine decides whether to apply diacritics: VietKi,
// like Unikey, does NOT use a word dictionary (see PHASE3.md E.1/E.2). It only
// knows the set of valid onsets, vowel nuclei and codas, and the tone rule for
// stop-final syllables. A buffer that is still a prefix of some valid syllable
// is "viable"; one that is not signals a foreign word that must pass through.
#include "internal.h"

#include <algorithm>
#include <string>

namespace vietki {

bool isVowelBase(char32_t b) {
    return b == U'a' || b == U'e' || b == U'i' || b == U'o' || b == U'u' || b == U'y';
}

bool hasVowel(const std::vector<Unit>& units) {
    for (const auto& u : units) {
        if (isVowelBase(u.base)) return true;
    }
    return false;
}

namespace {

// Valid initial consonants (onsets). qu/gi are listed because the trailing
// vowel is absorbed into the onset when a further vowel follows (quá, giá).
const char* const kOnsets[] = {
    "b", "c", "ch", "d", "g", "gh", "gi", "h", "k", "kh", "l", "m", "n",
    "ng", "ngh", "nh", "p", "ph", "qu", "r", "s", "t", "th", "tr", "v", "x"};

// Valid final consonants (codas). Semivowels (i/y/o/u) are part of the nucleus.
const char* const kCodas[] = {"c", "ch", "m", "n", "ng", "nh", "p", "t"};

// Valid vowel nuclei, written in *base-letter* form (marks stripped: ă/â->a,
// ê->e, ô/ơ->o, ư->u). The engine works on bases, so e.g. "ươ" and "uô" both
// reduce to "uo". This list is intentionally generous to avoid mangling real
// Vietnamese words; the only same-letter pairs deliberately left out are the
// circumflex doublings (aa/ee/oo) so that a stray triple restores literally.
const char* const kNuclei[] = {
    "a",   "e",   "i",   "o",   "u",   "y",
    "ai",  "ao",  "au",  "ay",  "eo",  "eu",  "ia",  "ie",  "iu",
    "oa",  "oe",  "oi",  "ua",  "ue",  "uo",  "ui",  "uy",  "uu",  "ye",
    "oai", "oay", "oeo", "uay", "uoi", "uou", "uya", "uye", "ueu",
    "uyu", "ieu", "yeu"};

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin());
}

template <size_t N>
bool isPrefixOfAny(const std::string& s, const char* const (&table)[N]) {
    if (s.empty()) return true;
    for (size_t i = 0; i < N; ++i)
        if (startsWith(table[i], s)) return true;
    return false;
}

template <size_t N>
bool isExactMember(const std::string& s, const char* const (&table)[N]) {
    for (size_t i = 0; i < N; ++i)
        if (s == table[i]) return true;
    return false;
}

bool isStopCoda(const std::string& coda) {
    return coda == "c" || coda == "ch" || coda == "p" || coda == "t";
}

// Parsed view of a unit list: onset / nucleus / coda as base-letter strings,
// with the qu-/gi-onset absorption applied. Literal consonant echoes from a
// minimal cancel ("ass" -> "as", "ww" -> "w") are skipped, but only while the
// syllable has no onset/coda yet. In a real word body ("ress", "bass"), that
// echo must count as a consonant so English double letters fall back to raw.
struct Parts {
    std::string onset, nucleus, coda;
    bool doubleNucleus = false; // a second vowel run after a coda -> impossible
};

Parts parse(const std::vector<Unit>& units) {
    Parts p;
    int stage = 0; // 0 = onset, 1 = nucleus, 2 = coda
    for (const Unit& u : units) {
        // SỬA TẠI ĐÂY: Nếu là ký tự literal (phím chức năng/hủy dấu), 
        // tuyệt đối không đưa vào onset/coda để xét cấu trúc âm tiết tiếng Việt.
        if (u.literal) {
            continue; 
        }

        if (isVowelBase(u.base)) {
            if (stage == 2) { p.doubleNucleus = true; break; }
            stage = 1;
            p.nucleus.push_back(static_cast<char>(u.base));
        } else {
            if (stage == 0)
                p.onset.push_back(static_cast<char>(u.base));
            else {
                stage = 2;
                p.coda.push_back(static_cast<char>(u.base));
            }
        }
    }
    // qu / gi: the u/i belongs to the onset (quá, giá), matching buildDisplay.
    if (p.onset == "q" && !p.nucleus.empty() && p.nucleus[0] == 'u') {
        p.onset += 'u';
        p.nucleus.erase(p.nucleus.begin());
    } else if (p.onset == "g" && p.nucleus.size() >= 2 && p.nucleus[0] == 'i') {
        p.onset += 'i';
        p.nucleus.erase(p.nucleus.begin());
    }
    return p;
}

} // namespace

bool isSyllableViable(const std::vector<Unit>& units) {
    Parts p = parse(units);
    if (p.doubleNucleus) return false;
    if (!isPrefixOfAny(p.onset, kOnsets)) return false;
    if (!isPrefixOfAny(p.nucleus, kNuclei)) return false;
    if (!p.coda.empty()) {
        // A coda only makes sense once the nucleus is itself complete.
        if (!isExactMember(p.nucleus, kNuclei)) return false;
        if (!isPrefixOfAny(p.coda, kCodas)) return false;
    }
    return true;
}

bool isValidVietnameseSyllable(const std::vector<Unit>& units, Tone tone) {
    Parts p = parse(units);
    if (p.doubleNucleus) return false;
    if (p.nucleus.empty()) return false; // every syllable needs a vowel
    if (!p.onset.empty() && !isExactMember(p.onset, kOnsets)) return false;
    if (!isExactMember(p.nucleus, kNuclei)) return false;
    if (!p.coda.empty() && !isExactMember(p.coda, kCodas)) return false;
    // Stop-final syllables only carry sắc (acute) or nặng (dot).
    if (isStopCoda(p.coda) && tone != Tone::None && tone != Tone::Acute &&
        tone != Tone::Dot)
        return false;
    return true;
}

// Phase 4 B.2: a complete valid syllable is Valid; an incomplete-but-still-
// reachable one is a ViablePrefix; anything else is Invalid. The decision is
// purely structural (onset/nucleus/coda/tone) and never looks at the order in
// which the user typed the tone/transform keys (A.1).
SyllableStatus classify(const std::vector<Unit>& units, Tone tone) {
    if (isValidVietnameseSyllable(units, tone)) return SyllableStatus::Valid;
    if (isSyllableViable(units)) return SyllableStatus::ViablePrefix;
    return SyllableStatus::Invalid;
}

} // namespace vietki
