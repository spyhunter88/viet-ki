// VietKi core engine: rebuild-from-raw composition and display diffing.
#include "vietki/engine.h"

#include "internal.h"

namespace vietki {

namespace {

// Longest common prefix length of two UTF-32 strings (code-point granularity).
// All composed Vietnamese characters are in the BMP, so one code point equals
// one Backspace and one UTF-16 unit on both target platforms.
size_t commonPrefix(const std::u32string& a, const std::u32string& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

} // namespace

Engine::Engine(Config cfg) : cfg_(cfg) {}

void Engine::setConfig(const Config& cfg) {
    cfg_ = cfg;
    // A method/placement change invalidates any in-progress composition.
    reset();
}

void Engine::reset() {
    raw_.clear();
    display_.clear();
    mode_ = CompositionMode::Composing; // Phase 4 D: a fresh word composes again
    // Phase 6: any explicit reset (mouse click, focus change, caret movement, a
    // non-Space word break) invalidates a pending Space-commit cache.
    hasCommitCache_ = false;
    cachedRaw_.clear();
    cachedDisplay_.clear();
    // A real word/context break re-arms the whole-word fix for the next word.
    wordFixOverride_ = false;
    consecutiveBackspaces_ = 0;
}

// Diff `next` against what is currently on screen and turn the delta into a
// KeyResult. If the key passes through unchanged (no backspaces and the commit
// is exactly the typed character), let the OS deliver it normally.
KeyResult Engine::emitDiff(const std::u32string& next, char32_t ch) {
    size_t pfx = commonPrefix(display_, next);
    KeyResult r;
    r.backspaces = static_cast<int>(display_.size() - pfx);
    r.commit.assign(next.begin() + pfx, next.end());
    display_ = next;
    if (r.backspaces == 0 && r.commit.size() == 1 && r.commit[0] == ch) {
        r.swallow = false;
        r.commit.clear();
    } else {
        r.swallow = true;
    }
    return r;
}

KeyResult Engine::onChar(char32_t ch, bool isWordBreak) {
    if (!cfg_.enabled) {
        reset();
        return KeyResult{};
    }
    if (isWordBreak) {
        // The break key itself passes through; the composed word stays on screen.
        reset();
        return KeyResult{};
    }
    if (ch == 0) {
        // Non-character key (bare modifier, etc.): leave the buffer untouched.
        return KeyResult{};
    }

    // Phase 6: a real keystroke here means Backspace was not the very next key
    // after Space, so any pending commit cache is no longer eligible to restore.
    hasCommitCache_ = false;
    // A normal key ends any Backspace streak (but does not re-arm the per-word
    // whole-word-fix override — that stays off until the word actually breaks).
    consecutiveBackspaces_ = 0;

    raw_.push_back(ch);

    // Phase 4 D: once the word is locked, every key is a literal. We do not
    // re-process or re-run spell check; the literal equals the typed key, so the
    // OS can deliver it and we just track it for diffing/Backspace.
    if (mode_ == CompositionMode::LiteralLocked) {
        display_.push_back(ch);
        return KeyResult{}; // swallow=false
    }

    bool modern = cfg_.tone == TonePlacement::Modern;
    bool fix = cfg_.fixWholeWord && !wordFixOverride_;
    ProcessResult res;
    switch (cfg_.method) {
        case Method::Telex: res = processTelex(raw_, fix); break;
        case Method::VNI:   res = processVni(raw_, fix); break;
        case Method::VIQR:  res = {processPlain(raw_), SyllableStatus::ViablePrefix, false}; break;
    }

    std::u32string next;
    if (res.cancelled && cfg_.lockWordAfterCancel) {
        // C.5 path 1: keep the cancelled Telex/VNI output, then lock the rest.
        next = buildDisplay(res.syllable, modern);
        mode_ = CompositionMode::LiteralLocked;
    } else if (cfg_.spellCheck && res.status == SyllableStatus::Invalid) {
        // C.5 path 2: a structurally impossible word restores to its raw keys
        // (B.3) and locks, so no further marks jump in before the word break.
        next = buildDisplay(processPlain(raw_), modern);
        mode_ = CompositionMode::LiteralLocked;
    } else {
        next = buildDisplay(res.syllable, modern);
    }

    return emitDiff(next, ch);
}

KeyResult Engine::onSpace() {
    if (!cfg_.enabled) {
        reset();
        return KeyResult{};
    }
    // Only a non-empty buffer is worth caching; an empty raw_ means either
    // nothing was typed or this is a second consecutive Space (v1 restores at
    // most one Space + one Backspace, per PHASE6.md 4).
    const bool shouldCache = cfg_.restoreAfterSpace && !raw_.empty();
    std::u32string savedRaw = std::move(raw_);
    std::u32string savedDisplay = std::move(display_);
    CompositionMode savedMode = mode_;
    reset(); // clears raw_/display_/mode_ and any stale commit cache
    if (shouldCache) {
        hasCommitCache_ = true;
        cachedRaw_ = std::move(savedRaw);
        cachedDisplay_ = std::move(savedDisplay);
        cachedMode_ = savedMode;
    }
    return KeyResult{};
}

std::u32string Engine::reconstructRaw(const std::u32string& display) const {
    // Only a Composing Telex/VNI syllable can be rebuilt from its glyphs. A
    // LiteralLocked word (or VIQR) is already exactly its keystrokes, so it
    // reconstructs to itself.
    if (mode_ != CompositionMode::Composing ||
        (cfg_.method != Method::Telex && cfg_.method != Method::VNI)) {
        return display;
    }
    const bool telex = cfg_.method == Method::Telex;
    std::u32string raw;
    for (char32_t c : display) {
        char32_t base;
        Mark mark;
        Tone tone;
        bool upper;
        if (!decomposeChar(c, base, mark, tone, upper)) {
            raw.push_back(c); // plain letter / consonant: itself
            continue;
        }
        raw.push_back(upper ? static_cast<char32_t>(base - 32) : base); // cased base
        // Mark key(s): Telex doubles the vowel for the circumflex and uses 'w'
        // for breve/horn and 'd' for the bar; VNI uses 6/8/7/9.
        switch (mark) {
            case Mark::Circumflex: raw.push_back(telex ? base : U'6'); break;
            case Mark::Breve:      raw.push_back(telex ? U'w' : U'8'); break;
            case Mark::Horn:       raw.push_back(telex ? U'w' : U'7'); break;
            case Mark::Bar:        raw.push_back(telex ? U'd' : U'9'); break;
            case Mark::None: break;
        }
        switch (tone) {
            case Tone::Acute: raw.push_back(telex ? U's' : U'1'); break;
            case Tone::Grave: raw.push_back(telex ? U'f' : U'2'); break;
            case Tone::Hook:  raw.push_back(telex ? U'r' : U'3'); break;
            case Tone::Tilde: raw.push_back(telex ? U'x' : U'4'); break;
            case Tone::Dot:   raw.push_back(telex ? U'j' : U'5'); break;
            case Tone::None: break;
        }
    }
    return raw;
}

KeyResult Engine::onBackspace() {
    // Phase 6: the key right after a Space that just committed a word restores
    // the buffer instead of deleting a character, so the user can keep
    // correcting tones. Restoring only removes the space glyph on screen
    // (pass-through, swallow=false); everything else consumes the cache.
    if (hasCommitCache_) {
        raw_ = std::move(cachedRaw_);
        display_ = std::move(cachedDisplay_);
        mode_ = cachedMode_;
        hasCommitCache_ = false;
        cachedRaw_.clear();
        cachedDisplay_.clear();
        return KeyResult{}; // swallow=false: the app deletes the space itself
    }
    // A second (or later) Backspace in a row means the user is hand-editing;
    // suppress the whole-word fix for the rest of this word so they can place an
    // intentional oddity (e.g. repeated tone marks "ảảảả") without the engine
    // re-composing under them. The override is cleared on the next word break.
    if (++consecutiveBackspaces_ >= 2) wordFixOverride_ = true;
    const bool fix = cfg_.fixWholeWord && !wordFixOverride_;

    // With the fix on, Backspace removes the last on-screen glyph (e.g. "được" ->
    // "đượ") but keeps composing the rest of the syllable: the raw buffer is
    // rebuilt from the glyphs that remain so a following key re-places tones
    // correctly ("air" -> "ải", Backspace the i -> "ả", then "ir" -> "ải"). All
    // composed Vietnamese glyphs are single BMP code points, so dropping one code
    // point deletes exactly one glyph.
    if (fix && !display_.empty()) {
        std::u32string remaining(display_.begin(), display_.end() - 1);
        raw_ = reconstructRaw(remaining);
        display_ = std::move(remaining);
        if (display_.empty())
            mode_ = CompositionMode::Composing; // a cleared syllable composes afresh
        return KeyResult{}; // swallow=false: the app handles Backspace itself
    }

    // Fix off (setting off, or the per-word override latched): fall back to the
    // legacy behaviour — clear the composition and let the app delete the glyph
    // natively; the next key starts a fresh syllable. Keep the override latch and
    // Backspace counter so continued hand-editing stays in this mode.
    raw_.clear();
    display_.clear();
    mode_ = CompositionMode::Composing;
    return KeyResult{}; // swallow=false: the app handles Backspace itself
}

} // namespace vietki
