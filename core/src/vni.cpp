// VNI transform rules (IMPLEMENTATION_GUIDE.md 3.6; Phase 3 E.1; Phase 4) and
// the VIQR placeholder. Mirrors telex.cpp: modifier digits target the right
// syllable component wherever they are typed (so "me5t" and "met5" both give
// "mẹt", A.1), and the processor reports status + cancelled to the engine
// without deciding word locking itself (C.6).
#include "internal.h"

namespace vietki {

namespace {

char32_t lower(char32_t c) {
    return (c >= U'A' && c <= U'Z') ? c + 32 : c;
}
bool isUpper(char32_t c) { return c >= U'A' && c <= U'Z'; }

Tone toneOf(char32_t d) {
    switch (d) {
        case U'1': return Tone::Acute;
        case U'2': return Tone::Grave;
        case U'3': return Tone::Hook;
        case U'4': return Tone::Tilde;
        case U'5': return Tone::Dot;
        default: return Tone::None;
    }
}

void pushLiteral(std::vector<Unit>& units, char32_t ch) {
    Unit u;
    u.base = lower(ch);
    u.upper = isUpper(ch);
    u.literal = true;
    units.push_back(u);
}

std::pair<int, int> nucleusRange(const std::vector<Unit>& units) {
    int s = -1;
    for (int i = 0; i < static_cast<int>(units.size()); ++i)
        if (isVowelBase(units[i].base)) { s = i; break; }
    if (s < 0) return {-1, -1};
    int e = s;
    while (e + 1 < static_cast<int>(units.size()) && isVowelBase(units[e + 1].base))
        ++e;
    return {s, e};
}

// Apply a VNI modifier digit (6,7,8,9) to the syllable component it targets,
// regardless of where the digit lands (E.1). Digits with no applicable letter
// are swallowed so stray modifiers do not leak into the text.
bool applyMark(std::vector<Unit>& units, char32_t d, bool consec, bool& cancelled) {
    auto [s, e] = nucleusRange(units);
    switch (d) {
        case U'6': // circumflex on a/e/o
            if (s >= 0) {
                for (int i = e; i >= s; --i) {
                    Unit& v = units[i];
                    if ((v.base == U'a' || v.base == U'e' || v.base == U'o') &&
                        v.mark == Mark::None && !v.literal) {
                        v.mark = Mark::Circumflex;
                        return true;
                    }
                }
                if (consec) {
                    for (int i = e; i >= s; --i) {
                        Unit& v = units[i];
                        if ((v.base == U'a' || v.base == U'e' || v.base == U'o') &&
                            v.mark == Mark::Circumflex) {
                            v.mark = Mark::None;
                            cancelled = true;
                            pushLiteral(units, d);
                            return true;
                        }
                    }
                }
            }
            // C.7.1: only "handled" when something actually changed. If a
            // compatible vowel exists it was a redundant no-op (still swallow);
            // if there is no a/e/o at all the digit is a literal ("i6" -> "i6").
            for (int i = s; s >= 0 && i <= e; ++i)
                if (units[i].base == U'a' || units[i].base == U'e' ||
                    units[i].base == U'o')
                    return true;
            return false;
        case U'8': // breve on a
            if (s >= 0) {
                for (int i = e; i >= s; --i) {
                    Unit& v = units[i];
                    if (v.base == U'a' && v.mark == Mark::None && !v.literal) {
                        v.mark = Mark::Breve;
                        return true;
                    }
                }
                if (consec) {
                    for (int i = e; i >= s; --i) {
                        Unit& v = units[i];
                        if (v.base == U'a' && v.mark == Mark::Breve) {
                            v.mark = Mark::None;
                            cancelled = true;
                            pushLiteral(units, d);
                            return true;
                        }
                    }
                }
            }
            for (int i = s; s >= 0 && i <= e; ++i)
                if (units[i].base == U'a') return true; // redundant no-op
            return false; // C.7.1: no 'a' to mark -> literal ("e8" -> "e8")
        case U'7': // horn on o/u, pairing u->ư for the uo diphthong
            if (s >= 0) {
                for (int i = e; i >= s; --i) {
                    Unit& v = units[i];
                    if (v.base == U'o' && v.mark == Mark::None && !v.literal) {
                        v.mark = Mark::Horn;
                        if (i > s) {
                            Unit& prev = units[i - 1];
                            if (prev.base == U'u' && prev.mark == Mark::None &&
                                !prev.literal)
                                prev.mark = Mark::Horn;
                        }
                        return true;
                    }
                }
                for (int i = s; i <= e; ++i) {
                    Unit& v = units[i];
                    if (v.base == U'u' && v.mark == Mark::None && !v.literal) {
                        v.mark = Mark::Horn;
                        return true;
                    }
                }
                if (consec) {
                    for (int i = e; i >= s; --i) {
                        Unit& v = units[i];
                        if ((v.base == U'o' || v.base == U'u') &&
                            v.mark == Mark::Horn) {
                            v.mark = Mark::None;
                            cancelled = true;
                            pushLiteral(units, d);
                            return true;
                        }
                    }
                }
            }
            for (int i = s; s >= 0 && i <= e; ++i)
                if (units[i].base == U'o' || units[i].base == U'u')
                    return true; // redundant no-op
            return false; // C.7.1: no o/u to mark -> literal ("a7" -> "a7")
        case U'9': // bar on the onset d
            for (size_t i = 0; i < units.size(); ++i) {
                Unit& v = units[i];
                if (isVowelBase(v.base)) break;
                if (v.base == U'd' && v.mark == Mark::None && !v.literal) {
                    v.mark = Mark::Bar;
                    return true;
                }
                if (consec && v.base == U'd' && v.mark == Mark::Bar) {
                    v.mark = Mark::None;
                    cancelled = true;
                    pushLiteral(units, d);
                    return true;
                }
            }
            for (size_t i = 0; i < units.size(); ++i) {
                if (isVowelBase(units[i].base)) break;
                if (units[i].base == U'd') return true; // redundant no-op
            }
            return false; // C.7.1: no onset 'd' to bar -> literal ("a9" -> "a9")
        default:
            return false;
    }
}

} // namespace

ProcessResult processVni(const std::u32string& raw) {
    Syllable syl;
    std::vector<Unit>& units = syl.units;
    char32_t prev = 0;
    bool lastCancelled = false;

    for (int idx = 0; idx < static_cast<int>(raw.size()); ++idx) {
        char32_t ch = raw[idx];
        bool consec = (prev == ch);
        bool handled = false;
        bool cancelled = false;

        Tone tn = toneOf(ch);
        if (tn != Tone::None && hasVowel(units)) {
            if (syl.tone == tn && consec) {
                syl.tone = Tone::None;
                cancelled = true;
                pushLiteral(units, ch);
                handled = true;
            } else if (syl.tone == tn) {
                // C.7.1 no-op: nothing changes -> keep the digit as a literal.
                pushLiteral(units, ch);
                handled = true;
            } else {
                syl.tone = tn;
                handled = true;
            }
        } else if (ch == U'0') {
            // C.7: '0' clears only the tone, like Telex 'z'. Swallowed only when
            // it actually removed a tone; otherwise literal (C.7.1) — pushed
            // literal so kept marks render ("a80" -> "ă0"). Not an undo (C.7.2).
            if (syl.tone != Tone::None) {
                syl.tone = Tone::None;
            } else {
                pushLiteral(units, ch);
            }
            handled = true;
        } else if (ch == U'6' || ch == U'7' || ch == U'8' || ch == U'9') {
            // C.7.1: a modifier digit that changes nothing is a literal, not an
            // eaten key ("i6" -> "i6"). applyMark returns false in that case.
            if (!applyMark(units, ch, consec, cancelled))
                pushLiteral(units, ch);
            handled = true;
        }

        if (!handled) {
            char32_t lc = lower(ch);
            Unit u;
            u.base = lc;
            u.upper = isUpper(ch);
            units.push_back(u);
        }
        prev = ch;
        lastCancelled = cancelled;
    }

    return {syl, classify(units, syl.tone), lastCancelled};
}

Syllable processPlain(const std::u32string& raw) {
    Syllable syl;
    for (char32_t ch : raw) {
        Unit u;
        u.base = lower(ch);
        u.upper = isUpper(ch);
        u.literal = true;
        syl.units.push_back(u);
    }
    return syl;
}

} // namespace vietki
