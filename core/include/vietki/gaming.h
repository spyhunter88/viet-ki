// VietKi Gaming Mode — OS-independent temporary-typing state machine (Phase 5 D/J).
//
// In Gaming Mode's "temporary trigger" policy a game receives every key raw, and
// the player presses a configurable trigger key to start a short Vietnamese
// typing session that auto-ends on Enter/Esc/click/focus-change. This controller
// is the pure policy core of that behaviour: it tracks the session state and
// reports, for each native event the shell hands it, what the shell should do.
//
// It includes no OS headers, holds no process list and injects no keys; the shell
// translates native events into the calls below and executes the returned policy.
// Keeping it pure lets the state machine (Phase 5 K.1) be unit-tested directly.
#pragma once

namespace vietki {

// The session state for the focused gaming context. Never persisted (Phase 5 D.1).
enum class GamingTypingState {
    Idle,    // game receives keys raw; Vietnamese off
    Armed,   // the trigger was just swallowed; the next text key starts typing
    Active   // a temporary Vietnamese session is in progress
};

// What the shell should do with the event it just reported. Pure policy: the
// controller never touches the engine, the icon or the keyboard itself (J.1).
struct GamingAction {
    bool swallow = false;             // eat the native event; do not pass it on
    bool activateVietnamese = false;  // switch the focused app to Vietnamese now
    bool deactivateVietnamese = false;// the session just ended; re-resolve mode
    bool replayTrigger = false;       // send one physical trigger press to the app
    bool suppressUntilTriggerUp = false; // also eat the matching key-up (D.3)
};

// A single foreground gaming context. The shell owns one instance and resets it
// on focus change / policy change. Only meaningful under the TemporaryTrigger
// policy; ToggleForCurrentApp never drives this controller (Phase 5 D.1).
class GamingSession {
public:
    GamingTypingState state() const { return state_; }

    // The trigger key went down. isRepeat is true for auto-repeat (held key),
    // which must never count as the second press of a double-trigger (D.3).
    GamingAction onTriggerDown(bool isRepeat);
    // The trigger key was released.
    GamingAction onTriggerUp();
    // Any non-modifier, non-terminating key. In Armed this starts the session
    // and the shell must still feed this same key to the engine (F.2).
    GamingAction onNonModifierKey();

    // Session-ending events (D.5). All let the original event reach the game.
    GamingAction onEnter();
    GamingAction onEscape();
    GamingAction onMouseDown();
    GamingAction onContextLost();

    void reset();

private:
    GamingAction endSession();

    GamingTypingState state_ = GamingTypingState::Idle;
    // The first trigger press has been released, so a fresh trigger-down may be
    // the second tap of a double-trigger. Cleared whenever the context resets.
    bool triggerReleased_ = false;
    // After a double-trigger we return to Idle but must still eat the key-up of
    // that second press so the game never sees an orphan key-up (D.3).
    bool suppressUp_ = false;
};

} // namespace vietki
