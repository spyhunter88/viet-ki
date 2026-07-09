// Unit tests for the VietKi core engine. The section-8 cases in
// IMPLEMENTATION_GUIDE.md are authoritative; this file encodes them directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "vietki/engine.h"

#include <string>

using namespace vietki;

namespace {

bool isWordBreak(char32_t c) {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'.' || c == U',' ||
           c == U';' || c == U'!' || c == U'?';
}

// Drive the engine exactly like an OS shell would and return the resulting
// on-screen text. This exercises onChar plus the backspace/commit diffing.
std::u32string runConfig(const Config& cfg, const std::u32string& keys) {
    Engine e(cfg);
    std::u32string screen;
    for (char32_t c : keys) {
        bool wb = isWordBreak(c);
        KeyResult r = e.onChar(c, wb);
        if (wb) {
            screen.push_back(c); // break key passes through
            continue;
        }
        if (r.swallow) {
            for (int i = 0; i < r.backspaces && !screen.empty(); ++i)
                screen.pop_back();
            screen += r.commit;
        } else {
            screen.push_back(c); // pass-through key reaches the app unchanged
        }
    }
    return screen;
}

// Config fields: {method, tone, enabled, spellCheck, lockWordAfterCancel}.
std::u32string typeKeys(Method method, const std::u32string& keys,
                        TonePlacement tone = TonePlacement::Modern) {
    return runConfig(Config{method, tone, true}, keys);
}

std::u32string telex(const std::u32string& k) { return typeKeys(Method::Telex, k); }
std::u32string vni(const std::u32string& k) { return typeKeys(Method::VNI, k); }

// Phase 4 C.4: lockWordAfterCancel = false (free cancel — keep composing).
std::u32string telexFreeCancel(const std::u32string& k) {
    return runConfig(Config{Method::Telex, TonePlacement::Modern, true, true, false}, k);
}
std::u32string vniFreeCancel(const std::u32string& k) {
    return runConfig(Config{Method::VNI, TonePlacement::Modern, true, true, false}, k);
}

std::vector<std::u32string> telexSnapshots(const std::u32string& keys) {
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    std::u32string screen;
    std::vector<std::u32string> snapshots;
    for (char32_t c : keys) {
        KeyResult r = e.onChar(c, false);
        if (r.swallow) {
            for (int i = 0; i < r.backspaces && !screen.empty(); ++i)
                screen.pop_back();
            screen += r.commit;
        } else {
            screen.push_back(c);
        }
        snapshots.push_back(screen);
    }
    return snapshots;
}

} // namespace

TEST_CASE("Telex section-8") {
    CHECK(telex(U"vieejt") == U"việt");
    CHECK(telex(U"tieengs") == U"tiếng");
    CHECK(telex(U"dduwowcj") == U"được");
    CHECK(telex(U"nuwowcs") == U"nước");
    CHECK(telex(U"cura") == U"của");
    CHECK(telex(U"mias") == U"mía");
    CHECK(telex(U"toans") == U"toán");
    // Open oa/oe/uy clusters: Modern places the tone on the second vowel
    // (guide 3.5 + section B.4). The Old placement is checked separately below.
    CHECK(telex(U"hoaf") == U"hoà");
    CHECK(telex(U"khoer") == U"khoẻ");
    CHECK(telex(U"thuys") == U"thuý");
    CHECK(telex(U"ngoaif") == U"ngoài");
    CHECK(telex(U"nguyeenx") == U"nguyễn");
    CHECK(telex(U"ddi") == U"đi");
    CHECK(telex(U"as") == U"á");
    CHECK(telex(U"ass") == U"as");
    CHECK(telex(U"aw") == U"ă");
    CHECK(telex(U"ww") == U"w");
    CHECK(telex(U"Vieetj Nam") == U"Việt Nam");
}

TEST_CASE("VNI section-8") {
    CHECK(vni(U"vie6t65") == U"việt");
    CHECK(vni(U"tie6ng1") == U"tiếng");
    CHECK(vni(U"d9uo7c5") == U"được");
    CHECK(vni(U"cu3a") == U"của");
    CHECK(vni(U"toan1") == U"toán");
    CHECK(vni(U"hoa2") == U"hoà"); // open oa, Modern -> second vowel (see B.4)
}

TEST_CASE("Telex extras") {
    // Phase 4 C: a re-typed circumflex key cancels the mark and locks the word
    // to literal, so a triple vowel resolves to the standard Telex double
    // ("aaa" -> "aa", "ooo" -> "oo") rather than the Phase-3 raw restore.
    CHECK(telex(U"aaa") == U"aa");
    CHECK(telex(U"oo") == U"ô");
    CHECK(telex(U"ooo") == U"oo");
    CHECK(telex(U"ddd") == U"dd");          // consonant-only: still cancels
    CHECK(telex(U"as ") == U"á ");          // word break commits and passes space
    CHECK(telex(U"sao") == U"sao");         // leading s with no vowel is literal
    CHECK(telex(U"saos") == U"sáo");
    CHECK(telex(U"quas") == U"quá");        // qu onset: tone on a
    CHECK(telex(U"gias") == U"giá");        // gi onset: tone on a
}

TEST_CASE("Old tone placement") {
    // Old places the tone on the first vowel of an open oa/oe/uy cluster.
    CHECK(typeKeys(Method::Telex, U"hoaf", TonePlacement::Old) == U"hòa");
    CHECK(typeKeys(Method::Telex, U"khoer", TonePlacement::Old) == U"khỏe");
    CHECK(typeKeys(Method::Telex, U"thuys", TonePlacement::Old) == U"thúy");
}

// Section B.4: the tone re-settles on the current vowel cluster after every
// keystroke, so adding/upgrading a vowel moves the mark (hú + ê -> huế).
TEST_CASE("Tone re-placement (B.4 Telex)") {
    CHECK(telex(U"husee") == U"huế");   // mark moves u -> ê when uê forms
    CHECK(telex(U"thuees") == U"thuế"); // same, with a cluster onset
    CHECK(telex(U"huee") == U"huê");    // cluster changes, no tone added
    CHECK(telex(U"hus") == U"hú");      // baseline: single vowel
    CHECK(telex(U"huse") == U"húe");    // valid intermediate (mark still on u)
    CHECK(telex(U"nguwowif") == U"người"); // ươi cluster, mark on ơ
    CHECK(telex(U"gias") == U"giá");    // gi rule: i is a consonant, mark on a
    CHECK(telex(U"quas") == U"quá");    // qu rule: u is a consonant, mark on a
    CHECK(telex(U"toaj") == U"toạ");    // open oa, dot, modern -> a
    CHECK(telex(U"oas") == U"oá");      // open oa, modern -> second vowel
}

TEST_CASE("Tone re-placement (B.4 VNI)") {
    CHECK(vni(U"hu1e6") == U"huế");
    CHECK(vni(U"gia1") == U"giá");
    CHECK(vni(U"qua1") == U"quá");
}

TEST_CASE("Backspace removes the last character") {
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    std::u32string screen;
    auto feed = [&](char32_t c) {
        KeyResult r = e.onChar(c, false);
        if (r.swallow) {
            for (int i = 0; i < r.backspaces && !screen.empty(); ++i) screen.pop_back();
            screen += r.commit;
        } else {
            screen.push_back(c);
        }
    };
    // Simulate the OS Backspace: when the engine passes it through, the app
    // deletes the last on-screen glyph itself.
    auto backspace = [&]() {
        KeyResult r = e.onBackspace();
        if (r.swallow) {
            for (int i = 0; i < r.backspaces && !screen.empty(); ++i) screen.pop_back();
            screen += r.commit;
        } else if (!screen.empty()) {
            screen.pop_back();
        }
    };

    for (char32_t c : std::u32string(U"dduwowcj")) feed(c);
    CHECK(screen == U"được");
    backspace();
    CHECK(screen == U"đượ"); // last character removed, tone on ơ preserved
    backspace();
    CHECK(screen == U"đư");
    // Buffer was reset, so typing continues as a fresh syllable.
    feed(U'a');
    CHECK(screen == U"đưa");
}

// Section E.1/E.2/E.3: tones and transforms target the right component even
// when a consonant sits between them and their vowel, and the spell checker
// keeps foreign words intact. spellCheck defaults to true.
TEST_CASE("Non-adjacent transforms (E.1)") {
    CHECK(telex(U"khongo") == U"không"); // circumflex o after the ng coda
    CHECK(telex(U"khonog") == U"không"); // circumflex o wedged before g
    CHECK(telex(U"uuw") == U"ưu");       // horn targets the u in the uu cluster
    CHECK(telex(U"ddonw") == U"đơn");    // horn reaches o past the n
    CHECK(telex(U"tooi") == U"tôi");     // circumflex then a trailing vowel
}

TEST_CASE("Spell check / foreign words (E.2, E.3)") {
    CHECK(telex(U"over") == U"over");
    CHECK(telex(U"overr") == U"overr");
    CHECK(telex(U"overide") == U"overide");
    CHECK(telex(U"Window") == U"Window");
    CHECK(telex(U"wwind") == U"wind");
    CHECK(telex(U"wwindo") == U"windo");
    // Phase 4 C: the double letter is the cancel gesture, so it collapses to a
    // single literal and locks the rest of the word ("resstore" -> "restore",
    // "deteect" -> "detect"), instead of preserving the doubled key.
    CHECK(telex(U"resstore") == U"restore");
    CHECK(telex(U"deteect") == U"detect");
    // PHASE3.md E.3 lists "congj -> cộng", but c-o-n-g-j has no circumflex
    // trigger (that needs a doubled o), so the correct Telex output is "cọng"
    // (also a real word). The point of the case stands: a tone typed after a
    // complete coda still applies and the word is not treated as foreign.
    CHECK(telex(U"congj") == U"cọng");
    // Phase 4 C.7: 'z' only clears a tone. "lo" has no tone, so the 'z' is a
    // no-op and must stay literal rather than being swallowed (C.7.1).
    CHECK(telex(U"loz") == U"loz");
    // Phase 4 A.3/B.5: with no dictionary, "test" is a valid Telex spelling of
    // "tét" (s = sắc, t = stop coda). The order of the tone key is never used to
    // guess English, so this is "tét"; deliberate cancel ("tesst") gives "test".
    CHECK(telex(U"test") == U"tét");
    CHECK(telex(U"eee") == U"ee");       // re-typed circumflex cancels -> "ee"
    CHECK(telex(U"as") == U"á");         // genuine single-vowel tone still works
}

TEST_CASE("Regression: tone and w placement") {
    CHECK(telex(U"casc") == U"các");
    CHECK(telex(U"Casc") == U"Các");
    CHECK(telex(U"hoawcj") == U"hoặc");
    CHECK(telex(U"Hoawcj") == U"Hoặc");
    CHECK(telex(U"chuaw") == U"chưa");
    CHECK(telex(U"Chuaw") == U"Chưa");
    CHECK(telex(U"quawng") == U"quăng");
}

TEST_CASE("Regression: English double letters after cancel") {
    // Phase 4 C: the double letter cancels the mark and locks to literal, so a
    // single literal letter remains ("ress" -> "res", "detee" -> "dete").
    auto ress = telexSnapshots(U"ress");
    CHECK(ress.back() == U"res");

    auto detee = telexSnapshots(U"detee");
    CHECK(detee.back() == U"dete");
}

TEST_CASE("Spell check off applies freely (E.2 toggle)") {
    // E.1 component targeting is always on, but the foreign-word gate is not:
    // with spellCheck=false the engine mangles freely the way Phase 1/2 did.
    auto run = [](bool spell, const std::u32string& keys) {
        Engine e(Config{Method::Telex, TonePlacement::Modern, true, spell});
        std::u32string screen;
        for (char32_t c : keys) {
            KeyResult r = e.onChar(c, false);
            if (r.swallow) {
                for (int i = 0; i < r.backspaces && !screen.empty(); ++i)
                    screen.pop_back();
                screen += r.commit;
            } else {
                screen.push_back(c);
            }
        }
        return screen;
    };
    // Phase 4 G.4: cancel does not depend on spell check, so "aaa" cancels the
    // circumflex to the standard "aa" with the gate either on or off.
    CHECK(run(false, U"aaa") == U"aa");
    CHECK(run(true, U"aaa") == U"aa");
    CHECK(run(false, U"khongo") == U"không");  // E.1 targeting still works
    // With the gate off, a structurally impossible word is no longer locked to
    // literal: marks apply freely the way Phase 1/2 did.
    CHECK(run(false, U"ddi") == U"đi");
}

// ---------------------------------------------------------------------------
// Phase 4 — required test battery (PHASE4.md G).
// ---------------------------------------------------------------------------

// G.1: the position of the tone/transform key in the raw buffer must not change
// the result; only the final syllable structure matters.
TEST_CASE("Phase 4 G.1: tone-key order is irrelevant") {
    CHECK(telex(U"mejt") == U"mẹt");
    CHECK(telex(U"metj") == U"mẹt");
    CHECK(telex(U"lafm") == U"làm");
    CHECK(telex(U"lamf") == U"làm");
    CHECK(telex(U"bafn") == U"bàn");
    CHECK(telex(U"banf") == U"bàn");
    CHECK(telex(U"sajch") == U"sạch");
    CHECK(telex(U"sachj") == U"sạch");
    CHECK(vni(U"me5t") == U"mẹt");
    CHECK(vni(U"met5") == U"mẹt");
}

// G.2: a re-typed tone/transform key cancels, and (with lockWordAfterCancel on)
// the rest of the word stays literal.
TEST_CASE("Phase 4 G.2: cancel and literal lock") {
    CHECK(telex(U"of") == U"ò");
    CHECK(telex(U"off") == U"of");
    CHECK(telex(U"offf") == U"off");
    CHECK(telex(U"offff") == U"offf");

    CHECK(telex(U"tes") == U"té");
    CHECK(telex(U"test") == U"tét");
    CHECK(telex(U"tesst") == U"test");
    CHECK(telex(U"tesster") == U"tester");

    // Free cancel (C.4): only the current key is undone, then composing resumes,
    // so the third 'f' re-creates the grave -> "òf".
    CHECK(telexFreeCancel(U"off") == U"of");
    CHECK(telexFreeCancel(U"offf") == U"òf");
}

TEST_CASE("Phase 4 G.2: cancel covers every mark") {
    CHECK(telex(U"ass") == U"as");   // sắc
    CHECK(telex(U"aff") == U"af");   // huyền
    CHECK(telex(U"arr") == U"ar");   // hỏi
    CHECK(telex(U"axx") == U"ax");   // ngã
    CHECK(telex(U"ajj") == U"aj");   // nặng
    CHECK(telex(U"aww") == U"aw");   // breve (mũ trăng)
    CHECK(telex(U"oww") == U"ow");   // horn (râu)
    CHECK(telex(U"ddd") == U"dd");   // đ bar
    // A key after a cancel must not re-trigger a transform in the same word.
    CHECK(telex(U"offw") == U"ofw");
    // VNI: re-typed digit cancels its tone/mark.
    CHECK(vni(U"a11") == U"a1");
    CHECK(vni(U"a66") == U"a6");
    CHECK(vniFreeCancel(U"a11") == U"a1");
}

// G.3: a structurally impossible word locks to its literal keys and never jumps
// a tone back in for the rest of the word.
TEST_CASE("Phase 4 G.3: invalid words restore to literal") {
    CHECK(telex(U"over") == U"over");
    CHECK(telex(U"override") == U"override");
    CHECK(telex(U"Window") == U"Window");
    CHECK(telex(U"restore") == U"restore");
    CHECK(telex(U"detect") == U"detect");
}

TEST_CASE("Phase 4 G.3: invalid-word snapshots restore immediately") {
    auto s = telexSnapshots(U"override");
    CHECK(s[0] == U"o");
    CHECK(s[1] == U"ov"); // the moment it is Invalid, display is back to raw
    CHECK(s[2] == U"ove");
    CHECK(s[3] == U"over");
    CHECK(s.back() == U"override");
}

// G.4: cancel is independent of spell check; locking the whole word depends only
// on lockWordAfterCancel.
TEST_CASE("Phase 4 G.4: cancel independent of spell check") {
    auto cfg = [](bool spell, bool lock) {
        return Config{Method::Telex, TonePlacement::Modern, true, spell, lock};
    };
    // Spell check off: no invalid-word lock, but cancel + lock still applies.
    CHECK(runConfig(cfg(false, true), U"offf") == U"off");
    CHECK(runConfig(cfg(false, false), U"offf") == U"òf");
    // Spell check on behaves the same for the cancel path.
    CHECK(runConfig(cfg(true, true), U"offf") == U"off");
    CHECK(runConfig(cfg(true, false), U"offf") == U"òf");
}

// G.5: reset() drops the buffer so a new word is never diffed against the old
// one, and a LiteralLocked word returns to Composing.
TEST_CASE("Phase 4 G.5: reset clears buffer and mode") {
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    auto run = [&](const std::u32string& keys) {
        std::u32string screen;
        for (char32_t c : keys) {
            KeyResult r = e.onChar(c, false);
            if (r.swallow) {
                for (int i = 0; i < r.backspaces && !screen.empty(); ++i)
                    screen.pop_back();
                screen += r.commit;
            } else {
                screen.push_back(c);
            }
        }
        return screen;
    };
    run(U"vie");   // partial word in "input A"
    e.reset();     // simulate a focus / mouse / caret context change
    CHECK(run(U"as") == U"á"); // fresh word, not diffed against "vie"

    run(U"ov");    // Invalid -> LiteralLocked
    e.reset();
    CHECK(run(U"ddi") == U"đi"); // composing again after the reset
}

// PHASE6.md: Space commits and clears the buffer as before, but an immediate
// Backspace restores it instead of just deleting the space, so the user can
// keep correcting tones. Helpers below drive the engine the way the Windows
// hook does: onChar for regular keys, onSpace() for Space specifically, and
// onBackspace() for Backspace (hook.cpp routes VK_SPACE to onSpace()).
namespace {

void feedInto(Engine& e, std::u32string& screen, char32_t c) {
    KeyResult r = e.onChar(c, false);
    if (r.swallow) {
        for (int i = 0; i < r.backspaces && !screen.empty(); ++i) screen.pop_back();
        screen += r.commit;
    } else {
        screen.push_back(c);
    }
}

void spaceInto(Engine& e, std::u32string& screen) {
    e.onSpace();
    screen.push_back(U' '); // Space always passes through to the app
}

void backspaceInto(Engine& e, std::u32string& screen) {
    KeyResult r = e.onBackspace();
    if (r.swallow) {
        for (int i = 0; i < r.backspaces && !screen.empty(); ++i) screen.pop_back();
        screen += r.commit;
    } else if (!screen.empty()) {
        screen.pop_back();
    }
}

// Types `before`, then Space, then Backspace, then `after` — the PHASE6.md 1
// restore path — and returns the resulting on-screen text.
std::u32string restoreAfterSpace(const Config& cfg, const std::u32string& before,
                                  const std::u32string& after) {
    Engine e(cfg);
    std::u32string screen;
    for (char32_t c : before) feedInto(e, screen, c);
    spaceInto(e, screen);
    backspaceInto(e, screen);
    for (char32_t c : after) feedInto(e, screen, c);
    return screen;
}

} // namespace

TEST_CASE("Phase 6: restore after Space + Backspace matches uninterrupted typing") {
    Config cfg{Method::Telex, TonePlacement::Modern, true};
    // "nguyen" + Space + Backspace + "x" must behave exactly as if the user had
    // never pressed Space at all (PHASE6.md edge case 1).
    CHECK(restoreAfterSpace(cfg, U"nguyen", U"x") == telex(U"nguyenx"));
    CHECK(restoreAfterSpace(cfg, U"dduwowcj", U"j") == telex(U"dduwowcjj"));
}

TEST_CASE("Phase 6: a keystroke between Space and Backspace cancels the restore") {
    // Edge case 2: "nguyen" Space "a" Backspace -> no restore; Backspace just
    // removes the "a" that was typed after the space.
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    std::u32string screen;
    for (char32_t c : std::u32string(U"nguyen")) feedInto(e, screen, c);
    spaceInto(e, screen);
    feedInto(e, screen, U'a');
    backspaceInto(e, screen);
    CHECK(screen == U"nguyen ");
}

TEST_CASE("Phase 6: a second Space cancels the restore (multi-space, v1 scope)") {
    // Edge case 3: two spaces in a row are deliberate spacing, not a mistake to
    // restore from; Backspace only removes the second space.
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    std::u32string screen;
    for (char32_t c : std::u32string(U"nguyen")) feedInto(e, screen, c);
    spaceInto(e, screen);
    spaceInto(e, screen);
    backspaceInto(e, screen);
    CHECK(screen == U"nguyen ");
}

TEST_CASE("Phase 6: reset() between Space and Backspace cancels the restore") {
    // Stand-in for mouse click / focus change / caret movement (edge cases
    // 4-6), all of which route through Engine::reset() in every shell.
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    std::u32string screen;
    for (char32_t c : std::u32string(U"nguyen")) feedInto(e, screen, c);
    spaceInto(e, screen);
    e.reset();
    backspaceInto(e, screen);
    CHECK(screen == U"nguyen");
}

TEST_CASE("Phase 6: restoreAfterSpace=false behaves exactly like today") {
    // Edge case 7: with the option off, Backspace only deletes the space and a
    // fresh buffer starts on the next key.
    Config cfg{Method::Telex, TonePlacement::Modern, true, true, true, false};
    Engine e(cfg);
    std::u32string screen;
    for (char32_t c : std::u32string(U"nguyen")) feedInto(e, screen, c);
    spaceInto(e, screen);
    backspaceInto(e, screen);
    CHECK(screen == U"nguyen");
    feedInto(e, screen, U'x');
    CHECK(screen == U"nguyenx"); // new buffer, not a continuation of "nguyen"
}

TEST_CASE("Phase 6: restored word still goes through spell check (edge case 8/11)") {
    // "overrid" is a valid Vietnamese prefix; completing it to "override" makes
    // it structurally invalid, so spell check must restore it to literal keys
    // exactly as it would without the Space/Backspace in the middle.
    Config cfg{Method::Telex, TonePlacement::Modern, true};
    CHECK(restoreAfterSpace(cfg, U"overrid", U"e") == telex(U"override"));
}

TEST_CASE("Phase 6: only the first Backspace after Space restores (auto-repeat)") {
    // Edge case 10: a held Backspace fires many times; only the first one may
    // restore, every following one removes on-screen characters as usual.
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    std::u32string screen;
    for (char32_t c : std::u32string(U"nguyen")) feedInto(e, screen, c);
    spaceInto(e, screen);
    backspaceInto(e, screen); // restores: removes the space only
    CHECK(screen == U"nguyen");
    backspaceInto(e, screen); // no cache left: removes one on-screen glyph
    CHECK(screen == U"nguye");
}

// C.7: the clear-tone keys (Telex 'z', VNI '0') only strip a tone, never a
// structural mark, and are swallowed only when they actually removed a tone.
TEST_CASE("Phase 4 C.7: clear-tone keys (z / 0)") {
    CHECK(telex(U"asz") == U"a");    // s = sắc, z clears it
    CHECK(telex(U"awsz") == U"ă");   // ắ -> ă: z clears only the tone, keeps trăng
    CHECK(telex(U"aasz") == U"â");   // ấ -> â: keeps mũ
    CHECK(telex(U"owjz") == U"ơ");   // ợ -> ơ: keeps móc
    CHECK(telex(U"awz") == U"ăz");   // ă has no tone -> z is literal
    CHECK(telex(U"aaz") == U"âz");   // â has no tone -> z is literal
    CHECK(telex(U"haiz") == U"haiz");       // no tone to clear
    CHECK(telex(U"haizzzz") == U"haizzzz"); // every z is literal

    CHECK(vni(U"a10") == U"a");
    CHECK(vni(U"a810") == U"ă");
    CHECK(vni(U"a80") == U"ă0");
    CHECK(vni(U"hai000") == U"hai000");
    // Note: PHASE4.md line 418 tabulates "a60 -> â", but that contradicts both
    // the section's mandatory pseudo-code (lines 424-431: a clear-tone key is a
    // no-op when there is no tone) and the Telex analog "aaz -> âz". â carries no
    // tone, so the '0' is a literal: the authoritative result is "â0".
    CHECK(vni(U"a60") == U"â0");
}

// C.7.1: no Telex/VNI function key may be swallowed unless it actually changed
// the syllable; a true no-op stays literal.
TEST_CASE("Phase 4 C.7.1: no-op keys are never swallowed") {
    CHECK(vni(U"i6") == U"i6");   // 6 needs a/e/o
    CHECK(vni(U"e8") == U"e8");   // 8 needs a
    CHECK(vni(U"a7") == U"a7");   // 7 needs o/u
    CHECK(vni(U"a9") == U"a9");   // 9 needs an onset d
    CHECK(vni(U"6") == U"6");     // standalone digit
    CHECK(vni(U"789") == U"789"); // standalone digits
    // A redundant digit that still has a compatible vowel is absorbed, so the
    // canonical double-circumflex spelling of "việt" is unaffected.
    CHECK(vni(U"vie6t65") == U"việt");
}

// C.7.3 snapshot: "haizzzz" must be literal from the very first z onward.
TEST_CASE("Phase 4 C.7.3: haizzzz snapshots") {
    auto s = telexSnapshots(U"haizzzz");
    CHECK(s[0] == U"h");
    CHECK(s[1] == U"ha");
    CHECK(s[2] == U"hai");
    CHECK(s[3] == U"haiz");
    CHECK(s[4] == U"haizz");
    CHECK(s[5] == U"haizzz");
    CHECK(s[6] == U"haizzzz");
}

TEST_CASE("Disabled passes through") {
    Engine e(Config{Method::Telex, TonePlacement::Modern, false});
    KeyResult r = e.onChar(U's', false);
    CHECK_FALSE(r.swallow);
}

// Phase 4.1 (Linux/IBus): a composition-model shell ignores the KeyResult diff
// and just reads preedit() after each key. Verify preedit() tracks the full
// display and equals display(), so IBus can render it directly.
TEST_CASE("Phase 4.1: preedit() mirrors the composed display") {
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    CHECK(e.preedit().empty());
    for (char32_t c : std::u32string(U"vieejt")) e.onChar(c, false);
    CHECK(e.preedit() == U"việt");
    CHECK(e.preedit() == e.display());
    // A word break commits and clears the composition (shell reads the empty
    // preedit and hides it).
    e.onChar(0, true);
    CHECK(e.preedit().empty());
}

// Phase 4.1: the IBus shell implements preedit Backspace by dropping the last
// raw key and replaying the buffer (rawBuffer() + reset() + onChar). Verify
// that reproduces the expected per-keystroke undo.
TEST_CASE("Phase 4.1: raw-buffer replay backspaces one keystroke") {
    Engine e(Config{Method::Telex, TonePlacement::Modern, true});
    for (char32_t c : std::u32string(U"vieejt")) e.onChar(c, false);
    CHECK(e.preedit() == U"việt");

    // Shell-side backspace: pop the last raw key ('t'), replay the rest.
    std::u32string raw = e.rawBuffer();
    REQUIRE_FALSE(raw.empty());
    raw.pop_back();
    e.reset();
    for (char32_t c : raw) e.onChar(c, false);
    CHECK(e.preedit() == U"việ");

    // Pop the tone key 'j' next: the tone lifts, matching keystroke-level undo.
    raw = e.rawBuffer();
    raw.pop_back();
    e.reset();
    for (char32_t c : raw) e.onChar(c, false);
    CHECK(e.preedit() == U"viê");
}
