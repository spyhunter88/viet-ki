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

    raw_.push_back(ch);

    // Phase 4 D: once the word is locked, every key is a literal. We do not
    // re-process or re-run spell check; the literal equals the typed key, so the
    // OS can deliver it and we just track it for diffing/Backspace.
    if (mode_ == CompositionMode::LiteralLocked) {
        display_.push_back(ch);
        return KeyResult{}; // swallow=false
    }

    bool modern = cfg_.tone == TonePlacement::Modern;
    ProcessResult res;
    switch (cfg_.method) {
        case Method::Telex: res = processTelex(raw_); break;
        case Method::VNI:   res = processVni(raw_); break;
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
    // Backspace removes the last on-screen character (e.g. "được" -> "đượ").
    // Because trailing raw keys can be tone/transform keys that affect an
    // earlier glyph, popping the raw buffer would not delete the last visible
    // character. Instead we clear the composition and let the app delete the
    // last glyph natively; the next key starts a fresh syllable.
    reset();
    return KeyResult{}; // swallow=false: the app handles Backspace itself
}

} // namespace vietki
