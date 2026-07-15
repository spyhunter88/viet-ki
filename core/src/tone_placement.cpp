// Tone placement, the NFC composition table, and display building.
//
// The compose table and the choice of tone vowel are validated against the
// section-8 test suite in IMPLEMENTATION_GUIDE.md, which is authoritative.
#include "internal.h"

#include <array>
#include <utility>

namespace vietki {

namespace {

// Index of each (base, mark) combination into kCompose below.
enum Form {
    F_A_NONE, F_A_BREVE, F_A_CIRC,
    F_E_NONE, F_E_CIRC,
    F_I_NONE,
    F_O_NONE, F_O_CIRC, F_O_HORN,
    F_U_NONE, F_U_HORN,
    F_Y_NONE,
    F_COUNT
};

// [form][0=lower,1=upper][tone] -> precomposed NFC code point.
// Tone order: None, Acute, Grave, Hook, Tilde, Dot. Generated from Unicode data.
constexpr char32_t kCompose[F_COUNT][2][6] = {
    // A_NONE
    {{ 0x0061, 0x00E1, 0x00E0, 0x1EA3, 0x00E3, 0x1EA1 },
     { 0x0041, 0x00C1, 0x00C0, 0x1EA2, 0x00C3, 0x1EA0 }},
    // A_BREVE
    {{ 0x0103, 0x1EAF, 0x1EB1, 0x1EB3, 0x1EB5, 0x1EB7 },
     { 0x0102, 0x1EAE, 0x1EB0, 0x1EB2, 0x1EB4, 0x1EB6 }},
    // A_CIRC
    {{ 0x00E2, 0x1EA5, 0x1EA7, 0x1EA9, 0x1EAB, 0x1EAD },
     { 0x00C2, 0x1EA4, 0x1EA6, 0x1EA8, 0x1EAA, 0x1EAC }},
    // E_NONE
    {{ 0x0065, 0x00E9, 0x00E8, 0x1EBB, 0x1EBD, 0x1EB9 },
     { 0x0045, 0x00C9, 0x00C8, 0x1EBA, 0x1EBC, 0x1EB8 }},
    // E_CIRC
    {{ 0x00EA, 0x1EBF, 0x1EC1, 0x1EC3, 0x1EC5, 0x1EC7 },
     { 0x00CA, 0x1EBE, 0x1EC0, 0x1EC2, 0x1EC4, 0x1EC6 }},
    // I_NONE
    {{ 0x0069, 0x00ED, 0x00EC, 0x1EC9, 0x0129, 0x1ECB },
     { 0x0049, 0x00CD, 0x00CC, 0x1EC8, 0x0128, 0x1ECA }},
    // O_NONE
    {{ 0x006F, 0x00F3, 0x00F2, 0x1ECF, 0x00F5, 0x1ECD },
     { 0x004F, 0x00D3, 0x00D2, 0x1ECE, 0x00D5, 0x1ECC }},
    // O_CIRC
    {{ 0x00F4, 0x1ED1, 0x1ED3, 0x1ED5, 0x1ED7, 0x1ED9 },
     { 0x00D4, 0x1ED0, 0x1ED2, 0x1ED4, 0x1ED6, 0x1ED8 }},
    // O_HORN
    {{ 0x01A1, 0x1EDB, 0x1EDD, 0x1EDF, 0x1EE1, 0x1EE3 },
     { 0x01A0, 0x1EDA, 0x1EDC, 0x1EDE, 0x1EE0, 0x1EE2 }},
    // U_NONE
    {{ 0x0075, 0x00FA, 0x00F9, 0x1EE7, 0x0169, 0x1EE5 },
     { 0x0055, 0x00DA, 0x00D9, 0x1EE6, 0x0168, 0x1EE4 }},
    // U_HORN
    {{ 0x01B0, 0x1EE9, 0x1EEB, 0x1EED, 0x1EEF, 0x1EF1 },
     { 0x01AF, 0x1EE8, 0x1EEA, 0x1EEC, 0x1EEE, 0x1EF0 }},
    // Y_NONE
    {{ 0x0079, 0x00FD, 0x1EF3, 0x1EF7, 0x1EF9, 0x1EF5 },
     { 0x0059, 0x00DD, 0x1EF2, 0x1EF6, 0x1EF8, 0x1EF4 }},
};

// Map (base, mark) to a Form, or -1 if it is not a tone-bearing vowel.
int formOf(char32_t base, Mark mark) {
    switch (base) {
        case U'a':
            if (mark == Mark::Breve) return F_A_BREVE;
            if (mark == Mark::Circumflex) return F_A_CIRC;
            return F_A_NONE;
        case U'e':
            if (mark == Mark::Circumflex) return F_E_CIRC;
            return F_E_NONE;
        case U'i': return F_I_NONE;
        case U'o':
            if (mark == Mark::Circumflex) return F_O_CIRC;
            if (mark == Mark::Horn) return F_O_HORN;
            return F_O_NONE;
        case U'u':
            if (mark == Mark::Horn) return F_U_HORN;
            return F_U_NONE;
        case U'y': return F_Y_NONE;
        default: return -1;
    }
}

// First maximal run of vowel units; returns {start, end} inclusive or {-1,-1}.
std::pair<int, int> findVowelCluster(const std::vector<Unit>& units) {
    int start = -1;
    for (int i = 0; i < static_cast<int>(units.size()); ++i) {
        if (isVowelBase(units[i].base)) { start = i; break; }
    }
    if (start < 0) return {-1, -1};
    int end = start;
    while (end + 1 < static_cast<int>(units.size()) &&
           isVowelBase(units[end + 1].base)) {
        ++end;
    }
    return {start, end};
}

} // namespace

char32_t composeChar(char32_t base, Mark mark, Tone tone, bool upper) {
    if (base == U'd') {
        char32_t c = (mark == Mark::Bar) ? 0x0111 /* đ */ : U'd';
        return upper ? (mark == Mark::Bar ? 0x0110 /* Đ */ : U'D') : c;
    }
    int form = formOf(base, mark);
    if (form < 0) {
        // Non tone-bearing literal: just adjust case.
        if (upper && base >= U'a' && base <= U'z') return base - 32;
        return base;
    }
    return kCompose[form][upper ? 1 : 0][static_cast<int>(tone)];
}

bool decomposeChar(char32_t c, char32_t& base, Mark& mark, Tone& tone,
                   bool& upper) {
    // đ / Đ decompose to a d with a bar and no tone.
    if (c == 0x0111 || c == 0x0110) {
        base = U'd';
        mark = Mark::Bar;
        tone = Tone::None;
        upper = (c == 0x0110);
        return true;
    }
    // Reverse-scan the compose table for the vowel forms. All entries are BMP
    // single code points, so one match is exact.
    for (int f = 0; f < F_COUNT; ++f) {
        for (int cs = 0; cs < 2; ++cs) {
            for (int t = 0; t < 6; ++t) {
                if (kCompose[f][cs][t] != c) continue;
                upper = (cs == 1);
                tone = static_cast<Tone>(t);
                switch (f) {
                    case F_A_NONE:  base = U'a'; mark = Mark::None; break;
                    case F_A_BREVE: base = U'a'; mark = Mark::Breve; break;
                    case F_A_CIRC:  base = U'a'; mark = Mark::Circumflex; break;
                    case F_E_NONE:  base = U'e'; mark = Mark::None; break;
                    case F_E_CIRC:  base = U'e'; mark = Mark::Circumflex; break;
                    case F_I_NONE:  base = U'i'; mark = Mark::None; break;
                    case F_O_NONE:  base = U'o'; mark = Mark::None; break;
                    case F_O_CIRC:  base = U'o'; mark = Mark::Circumflex; break;
                    case F_O_HORN:  base = U'o'; mark = Mark::Horn; break;
                    case F_U_NONE:  base = U'u'; mark = Mark::None; break;
                    case F_U_HORN:  base = U'u'; mark = Mark::Horn; break;
                    case F_Y_NONE:  base = U'y'; mark = Mark::None; break;
                    default: return false;
                }
                return true;
            }
        }
    }
    return false;
}

int chooseToneIndex(const std::vector<Unit>& vowels, bool hasFinal, bool modern) {
    const int n = static_cast<int>(vowels.size());
    // 1) Priority to vowels that already carry a diacritic.
    for (int i = 0; i < n; ++i)
        if (vowels[i].base == U'o' && vowels[i].mark == Mark::Horn) return i; // ơ
    for (int i = 0; i < n; ++i)
        if (vowels[i].base == U'e' && vowels[i].mark == Mark::Circumflex) return i; // ê
    for (int i = 0; i < n; ++i) {
        Mark m = vowels[i].mark;
        if (m == Mark::Circumflex || m == Mark::Breve || m == Mark::Horn) return i; // â ă ô ư
    }
    // 2) Plain vowel clusters.
    if (n <= 1) return 0;
    if (n >= 3) return 1; // middle vowel: oai->a, oay->a, uye->y...
    // n == 2
    if (hasFinal) return 1; // toán->a, hoàng->a
    char32_t a = vowels[0].base, b = vowels[1].base;
    bool special = (a == U'o' && b == U'a') || (a == U'o' && b == U'e') ||
                   (a == U'u' && b == U'y');
    if (special) {
        // Open oa/oe/uy clusters: modern placement puts the tone on the SECOND
        // vowel (hoà, khoẻ, thuý), old placement on the first (hòa, khỏe, thúy).
        // This matches the guide 3.5 pseudocode and the section-B.4 tests, which
        // are authoritative.
        return modern ? 1 : 0;
    }
    return 0; // ai, ao, au, ay, eo, ia, iu, oi, ui, ua, ...
}

std::u32string buildDisplay(const Syllable& syl, bool modern) {
    const std::vector<Unit>& units = syl.units;
    int chosen = -1;
    auto cluster = findVowelCluster(units);
    if (cluster.first >= 0 && syl.tone != Tone::None) {
        int s = cluster.first, e = cluster.second;
        // Onset spelled before the vowel cluster (for qu/gi detection).
        std::u32string onset;
        for (int i = 0; i < s; ++i) onset.push_back(units[i].base);
        int offset = 0;
        int clusterLen = e - s + 1;
        char32_t firstV = units[s].base;
        Mark firstM = units[s].mark;
        bool endsQ = !onset.empty() && onset.back() == U'q';
        bool endsG = !onset.empty() && onset.back() == U'g';
        if (endsQ && firstV == U'u' && firstM == Mark::None && clusterLen > 1) {
            offset = 1; // qu: the 'u' is part of the onset
        } else if (endsG && firstV == U'i' && firstM == Mark::None && clusterLen > 1) {
            offset = 1; // gi: the 'i' is part of the onset
        }
        std::vector<Unit> eff(units.begin() + s + offset, units.begin() + e + 1);
        bool hasFinal = e < static_cast<int>(units.size()) - 1;
        if (!eff.empty()) {
            int idx = chooseToneIndex(eff, hasFinal, modern);
            chosen = s + offset + idx;
        }
    }
    std::u32string out;
    for (int i = 0; i < static_cast<int>(units.size()); ++i) {
        const Unit& u = units[i];
        Tone t = (i == chosen) ? syl.tone : Tone::None;
        out.push_back(composeChar(u.base, u.mark, t, u.upper));
    }
    return out;
}

} // namespace vietki
