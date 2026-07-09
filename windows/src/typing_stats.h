// PHASE6.md 7: local-only typing statistics. A read-only observer of keyboard
// events — it never influences composition and hook.cpp only calls it after
// the engine has already decided what to do. Off by default; the caller
// (hook.cpp) is responsible for checking AppConfig::typingStats before
// calling any record*() function. No network, no telemetry.
#pragma once

#include <string>
#include <vector>

namespace vietki::win {

struct WordCount {
    std::wstring word;
    long long count = 0;
};

struct TypingStatsSnapshot {
    long long totalWords = 0;
    long long totalKeystrokes = 0;
    long long totalBackspaces = 0;
    double wpm = 0.0;              // words/minute, based on active typing time
    double backspaceRatioPct = 0.0; // backspaces / (keystrokes + backspaces) * 100
    std::vector<WordCount> topWords; // sorted by count desc
};

// Load persisted stats from their own local file (not config.ini) at startup.
void loadTypingStats();
// Persist the in-memory counters. Only call from the message thread (Settings
// close/save, app exit) — never from the low-level keyboard hook.
void saveTypingStats();

// A word was committed by a word-break key (Space, Tab, Enter, punctuation...).
// No-op if `word` is empty.
void recordWordCommitted(const std::u32string& word);
// One printable key reached the engine (used for the WPM estimate).
void recordKeystroke();
// One Backspace was pressed, whether or not it triggered a Phase 6 restore.
void recordBackspace();

// A snapshot for the Settings "Thống kê" tab. topN caps how many words come
// back in topWords (already sorted, most-typed first).
TypingStatsSnapshot typingStatsSnapshot(int topN = 20);

// Wipe the in-memory counters and delete the on-disk file.
void clearTypingStats();

} // namespace vietki::win
