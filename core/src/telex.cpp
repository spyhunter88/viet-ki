// Telex transform rules (IMPLEMENTATION_GUIDE.md 3.3, 3.4; Phase 3 E.1; Phase 4).
//
// A tone/transform key targets the right *component* of the syllable (onset /
// nucleus / coda) regardless of where it is typed — "khongo", "khonog", "uuw",
// "ddonw" all resolve correctly without a word list, and "mejt"/"metj" both give
// "mẹt" (Phase 4 A.1: the order of the tone key never changes the result).
//
// Phase 4 C.6: this processor no longer decides foreign-word fallback or word
// locking. It always applies the transforms, then reports the built syllable,
// its structural status (Valid/ViablePrefix/Invalid), and whether the *last*
// key cancelled a mark. The engine owns the Composing -> LiteralLocked move.
#include "internal.h"

namespace vietki {

namespace {

char32_t lower(char32_t c) {
    return (c >= U'A' && c <= U'Z') ? c + 32 : c;
}
bool isUpper(char32_t c) { return c >= U'A' && c <= U'Z'; }

Tone toneOf(char32_t lc) {
    switch (lc) {
        case U's': return Tone::Acute;
        case U'f': return Tone::Grave;
        case U'r': return Tone::Hook;
        case U'x': return Tone::Tilde;
        case U'j': return Tone::Dot;
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

// Index range [start, end] of the first maximal vowel run (the nucleus), or
// {-1, -1} if there is no vowel yet.
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

bool hasOnsetQBefore(const std::vector<Unit>& units, int vowelStart) {
    return vowelStart > 0 && units[vowelStart - 1].base == U'q' &&
           units[vowelStart - 1].mark == Mark::None;
}

// Apply Telex 'w' to the nucleus (E.1): o->ơ (uo->ươ), a->ă, u->ư, anywhere in
// the syllable; standalone -> ư; a re-typed 'w' cancels the last horn/breve.
bool applyW(std::vector<Unit>& units, char32_t ch, bool consec, bool& cancelled) {
    bool up = isUpper(ch);
    auto [s, e] = nucleusRange(units);
    if (s >= 0) {
        // In oa/ua clusters, w modifies the spelling target users expect:
        // hoa + w -> hoă, chua + w -> chưa. In qu + a, the u belongs to the
        // onset, so keep targeting the a (quawng -> quăng).
        for (int i = s; i < e; ++i) {
            Unit& cur = units[i];
            Unit& next = units[i + 1];
            if (cur.base == U'o' && next.base == U'a' &&
                next.mark == Mark::None && !next.literal) {
                next.mark = Mark::Breve;
                return true;
            }
            if (cur.base == U'u' && next.base == U'a' &&
                cur.mark == Mark::None && !cur.literal &&
                !hasOnsetQBefore(units, s)) {
                cur.mark = Mark::Horn;
                return true;
            }
        }
        // o -> ơ, pairing u->ư for the uo/uô diphthong.
        for (int i = s; i <= e; ++i) {
            Unit& v = units[i];
            if (v.base == U'o' && v.mark == Mark::None && !v.literal) {
                v.mark = Mark::Horn;
                if (i > s) {
                    Unit& prev = units[i - 1];
                    if (prev.base == U'u' && prev.mark == Mark::None && !prev.literal)
                        prev.mark = Mark::Horn;
                }
                return true;
            }
        }
        // a -> ă
        for (int i = s; i <= e; ++i) {
            Unit& v = units[i];
            if (v.base == U'a' && v.mark == Mark::None && !v.literal) {
                v.mark = Mark::Breve;
                return true;
            }
        }
        // u -> ư (first plain u; "uu" -> "ưu").
        for (int i = s; i <= e; ++i) {
            Unit& v = units[i];
            if (v.base == U'u' && v.mark == Mark::None && !v.literal) {
                v.mark = Mark::Horn;
                return true;
            }
        }
        // Re-typed 'w' on an already-marked vowel: cancel it.
        if (consec) {
            for (int i = e; i >= s; --i) {
                Unit& v = units[i];
                if (v.base == U'a' && v.mark == Mark::Breve) {
                    v.mark = Mark::None;
                    cancelled = true;
                    pushLiteral(units, ch);
                    return true;
                }
                if ((v.base == U'o' || v.base == U'u') && v.mark == Mark::Horn) {
                    if (v.synthetic) {
                        units.erase(units.begin() + i);
                        cancelled = true;
                        pushLiteral(units, ch);
                    } else {
                        v.mark = Mark::None;
                        cancelled = true;
                        pushLiteral(units, ch);
                    }
                    return true;
                }
            }
        }
    }
    // Standalone w -> ư (synthetic). "ww" cancel is handled above via synthetic.
    Unit u;
    u.base = U'u';
    u.mark = Mark::Horn;
    u.upper = up;
    u.synthetic = true;
    units.push_back(u);
    return true;
}

// Apply a circumflex doubling (aa->â, ee->ê, oo->ô) to the matching nucleus
// vowel, wherever it sits in the syllable (E.1: "khongo", "tooi"). Returns true
// if the key was consumed.
bool applyCircumflex(std::vector<Unit>& units, char32_t lc, char32_t ch, bool consec, bool& cancelled) {
    auto [s, e] = nucleusRange(units);
    if (s < 0) return false;
    // A matching plain vowel -> add the circumflex.
    for (int i = e; i >= s; --i) {
        Unit& v = units[i];
        if (v.base == lc && v.mark == Mark::None && !v.literal) {
            v.mark = Mark::Circumflex;
            return true;
        }
    }
    // Re-typed third vowel on an already-circumflexed match: cancel.
    if (consec) {
        for (int i = e; i >= s; --i) {
            Unit& v = units[i];
            if (v.base == lc && v.mark == Mark::Circumflex) {
                v.mark = Mark::None;
                cancelled = true;
                pushLiteral(units, ch);
                return true;
            }
        }
    }
    return false;
}

// Apply Telex 'd' doubling to the onset 'd' (dd->đ), targeting the onset rather
// than just the previous key.
bool applyDBar(std::vector<Unit>& units, char32_t ch, bool consec, bool& cancelled) {
    for (size_t i = 0; i < units.size(); ++i) {
        Unit& v = units[i];
        if (isVowelBase(v.base)) break; // past the onset
        if (v.base == U'd' && v.mark == Mark::None && !v.literal) {
            v.mark = Mark::Bar;
            return true;
        }
        if (consec && v.base == U'd' && v.mark == Mark::Bar) {
            v.mark = Mark::None;
            cancelled = true;
            pushLiteral(units, ch);
            return true;
        }
    }
    return false;
}

} // namespace

ProcessResult processTelex(const std::u32string& raw) {
    Syllable syl;
    std::vector<Unit>& units = syl.units;
    char32_t prev = 0;
    bool lastCancelled = false; // did the *last* key undo a mark? (C.1/C.6)

    for (int idx = 0; idx < static_cast<int>(raw.size()); ++idx) {
        char32_t ch = raw[idx];
        char32_t lc = lower(ch);
        bool up = isUpper(ch);
        bool consec = (prev != 0 && lower(prev) == lc);
        bool handled = false;
        bool cancelled = false;

        Tone tn = toneOf(lc);
        if (tn != Tone::None && hasVowel(units)) {
            if (syl.tone == tn && consec) {
                syl.tone = Tone::None;
                cancelled = true;
                pushLiteral(units, ch); // double-tap cancels -> literal letter
                handled = true;
            } else if (syl.tone == tn) {
                // C.7.1 no-op: the tone is already set and this is not a cancel,
                // so nothing changes -> keep the key as a literal letter (pushed
                // literal so spell check skips it and earlier marks are kept).
                pushLiteral(units, ch);
                handled = true;
            } else {
                syl.tone = tn;
                handled = true;
            }
        } else if (lc == U'z') {
            // C.7: 'z' removes only the tone, never a structural mark (mũ/trăng/
            // móc/đ). It is swallowed only when it actually cleared a tone;
            // otherwise it is a literal letter (no-op must not swallow, C.7.1) —
            // pushed literal so the kept marks render ("awz" -> "ăz", not "awz").
            // Clearing a tone is NOT an undo: it never sets cancelled and never
            // triggers lockWordAfterCancel (C.7.2).
            if (syl.tone != Tone::None) {
                syl.tone = Tone::None;
            } else {
                pushLiteral(units, ch);
            }
            handled = true;
        } else if (lc == U'w') {
            handled = applyW(units, ch, consec, cancelled);
        } else if (lc == U'a' || lc == U'e' || lc == U'o') {
            handled = applyCircumflex(units, lc, ch, consec, cancelled);
        } else if (lc == U'd') {
            handled = applyDBar(units, ch, consec, cancelled);
        }

        if (!handled) {
            Unit u;
            u.base = lc;
            u.upper = up;
            units.push_back(u);
        }
        prev = ch;
        lastCancelled = cancelled;
    }

    // Phase 4 A.1: the result depends only on the final syllable structure, not
    // on where the tone key was typed. The engine decides what to do with the
    // status; the processor never restores to literal on its own.
    return {syl, classify(units, syl.tone), lastCancelled};
}

} // namespace vietki
