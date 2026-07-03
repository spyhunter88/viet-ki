// Gaming Mode temporary-typing state machine (Phase 5 D). See gaming.h.
#include "vietki/gaming.h"

namespace vietki {

GamingAction GamingSession::onTriggerDown(bool isRepeat) {
    switch (state_) {
        case GamingTypingState::Idle:
            // First trigger tap: arm the session and swallow the key so it never
            // reaches the game (F.2). The shell resets the engine on arming.
            state_ = GamingTypingState::Armed;
            triggerReleased_ = false;
            return GamingAction{/*swallow=*/true};
        case GamingTypingState::Armed:
            // A second, distinct tap (the first was released and this is not an
            // auto-repeat) is the double-trigger escape: hand exactly one trigger
            // back to the game, stay in English, and eat the coming key-up (D.3).
            if (!isRepeat && triggerReleased_) {
                state_ = GamingTypingState::Idle;
                triggerReleased_ = false;
                suppressUp_ = true;
                GamingAction a;
                a.swallow = true;
                a.replayTrigger = true;
                a.suppressUntilTriggerUp = true;
                return a;
            }
            // Auto-repeat or a down without an intervening release: keep waiting.
            return GamingAction{/*swallow=*/true};
        case GamingTypingState::Active:
            // The trigger is now an ordinary key (D.4): let it through.
            return GamingAction{};
    }
    return GamingAction{};
}

GamingAction GamingSession::onTriggerUp() {
    // Eat the key-up that follows a double-trigger so the game sees a matched
    // down/up pair only (the replayed press), never a stray up (D.3).
    if (suppressUp_) {
        suppressUp_ = false;
        return GamingAction{/*swallow=*/true};
    }
    if (state_ == GamingTypingState::Armed) {
        // The first trigger's release: swallow it and note that a following
        // trigger-down may complete a double-trigger.
        triggerReleased_ = true;
        return GamingAction{/*swallow=*/true};
    }
    return GamingAction{};
}

GamingAction GamingSession::onNonModifierKey() {
    if (state_ == GamingTypingState::Armed) {
        // The first text key starts the session. It is NOT swallowed: the shell
        // feeds this same key to the engine in the same callback (F.2).
        state_ = GamingTypingState::Active;
        triggerReleased_ = false;
        GamingAction a;
        a.activateVietnamese = true;
        return a;
    }
    return GamingAction{};
}

GamingAction GamingSession::endSession() {
    // Idempotent: ending an already-Idle session reports no transition, so the
    // shell never fires a duplicate notification or sound (F.3).
    if (state_ == GamingTypingState::Idle) return GamingAction{};
    state_ = GamingTypingState::Idle;
    triggerReleased_ = false;
    GamingAction a;
    a.deactivateVietnamese = true;
    return a;
}

GamingAction GamingSession::onEnter() { return endSession(); }
GamingAction GamingSession::onEscape() { return endSession(); }
GamingAction GamingSession::onMouseDown() { return endSession(); }

GamingAction GamingSession::onContextLost() {
    suppressUp_ = false;
    return endSession();
}

void GamingSession::reset() {
    state_ = GamingTypingState::Idle;
    triggerReleased_ = false;
    suppressUp_ = false;
}

} // namespace vietki
