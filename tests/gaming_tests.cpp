// Pure state-machine tests for Gaming Mode's temporary-typing session
// (Phase 5 K.1). These exercise vietki::GamingSession with no OS or shell.
#include "doctest.h"

#include "vietki/gaming.h"

using namespace vietki;

namespace {

// Drive a session through arm -> activate so a test can start from Active.
GamingSession armedActive() {
    GamingSession s;
    s.onTriggerDown(false);
    s.onTriggerUp();
    s.onNonModifierKey();
    return s;
}

} // namespace

TEST_CASE("gaming trigger arms temporary Vietnamese") {
    GamingSession s;
    auto a = s.onTriggerDown(false);
    CHECK(a.swallow);
    CHECK_FALSE(a.activateVietnamese);
    CHECK(s.state() == GamingTypingState::Armed);
}

TEST_CASE("the first trigger key-up is swallowed and keeps Armed") {
    GamingSession s;
    s.onTriggerDown(false);
    auto up = s.onTriggerUp();
    CHECK(up.swallow);
    CHECK(s.state() == GamingTypingState::Armed);
}

TEST_CASE("double trigger replays once and stays English") {
    GamingSession s;
    s.onTriggerDown(false);
    s.onTriggerUp();
    auto a = s.onTriggerDown(false);
    CHECK(a.swallow);
    CHECK(a.replayTrigger);
    CHECK(a.suppressUntilTriggerUp);
    CHECK(s.state() == GamingTypingState::Idle);
}

TEST_CASE("the second trigger's key-up is swallowed after returning to Idle") {
    GamingSession s;
    s.onTriggerDown(false);
    s.onTriggerUp();
    s.onTriggerDown(false);            // double-trigger -> Idle, suppress next up
    CHECK(s.state() == GamingTypingState::Idle);
    auto up = s.onTriggerUp();
    CHECK(up.swallow);                 // the orphaned up is eaten (D.3)
    // A subsequent stray up is no longer suppressed.
    auto up2 = s.onTriggerUp();
    CHECK_FALSE(up2.swallow);
}

TEST_CASE("auto-repeat is not a double trigger") {
    GamingSession s;
    s.onTriggerDown(false);
    auto a = s.onTriggerDown(true);
    CHECK_FALSE(a.replayTrigger);
    CHECK(s.state() == GamingTypingState::Armed);
}

TEST_CASE("a trigger-down without a release is not a double trigger") {
    GamingSession s;
    s.onTriggerDown(false);            // Armed, not yet released
    auto a = s.onTriggerDown(false);   // no key-up in between
    CHECK_FALSE(a.replayTrigger);
    CHECK(s.state() == GamingTypingState::Armed);
}

TEST_CASE("a modifier between the two triggers does not break Armed") {
    // The shell never calls the controller for a bare modifier, so a modifier
    // tap between the two trigger presses is simply a no-op here: the double
    // trigger still completes.
    GamingSession s;
    s.onTriggerDown(false);
    s.onTriggerUp();
    // (modifier down/up happen at the shell layer — no controller call)
    auto a = s.onTriggerDown(false);
    CHECK(a.replayTrigger);
    CHECK(s.state() == GamingTypingState::Idle);
}

TEST_CASE("first text key activates and is not lost") {
    GamingSession s;
    s.onTriggerDown(false);
    s.onTriggerUp();
    auto a = s.onNonModifierKey();
    CHECK(a.activateVietnamese);
    CHECK_FALSE(a.swallow);            // the key is handed to the engine, not eaten
    CHECK(s.state() == GamingTypingState::Active);
}

TEST_CASE("a text key breaks the double-trigger escape window") {
    GamingSession s;
    s.onTriggerDown(false);
    s.onTriggerUp();
    auto a = s.onNonModifierKey();     // a different key arrives, not a 2nd trigger
    CHECK(a.activateVietnamese);
    CHECK(s.state() == GamingTypingState::Active);
}

TEST_CASE("enter ends temporary Vietnamese and passes through") {
    GamingSession s = armedActive();
    auto a = s.onEnter();
    CHECK_FALSE(a.swallow);
    CHECK(a.deactivateVietnamese);
    CHECK(s.state() == GamingTypingState::Idle);
}

TEST_CASE("escape ends the session") {
    GamingSession s = armedActive();
    auto a = s.onEscape();
    CHECK(a.deactivateVietnamese);
    CHECK(s.state() == GamingTypingState::Idle);
}

TEST_CASE("mouse down ends the session") {
    GamingSession s = armedActive();
    auto a = s.onMouseDown();
    CHECK(a.deactivateVietnamese);
    CHECK(s.state() == GamingTypingState::Idle);
}

TEST_CASE("focus change ends the session") {
    GamingSession s = armedActive();
    auto a = s.onContextLost();
    CHECK(a.deactivateVietnamese);
    CHECK(s.state() == GamingTypingState::Idle);
}

TEST_CASE("ending twice does not report a second transition") {
    GamingSession s = armedActive();
    auto a1 = s.onEnter();
    CHECK(a1.deactivateVietnamese);
    auto a2 = s.onEnter();
    CHECK_FALSE(a2.deactivateVietnamese); // idempotent (F.3)
    auto a3 = s.onMouseDown();
    CHECK_FALSE(a3.deactivateVietnamese);
}

TEST_CASE("trigger while Active passes through as an ordinary key") {
    GamingSession s = armedActive();
    auto down = s.onTriggerDown(false);
    CHECK_FALSE(down.swallow);
    CHECK_FALSE(down.replayTrigger);
    CHECK(s.state() == GamingTypingState::Active);
    auto up = s.onTriggerUp();
    CHECK_FALSE(up.swallow);
}

TEST_CASE("reset returns the session to Idle") {
    GamingSession s = armedActive();
    s.reset();
    CHECK(s.state() == GamingTypingState::Idle);
    // After reset a trigger arms cleanly again.
    auto a = s.onTriggerDown(false);
    CHECK(a.swallow);
    CHECK(s.state() == GamingTypingState::Armed);
}
