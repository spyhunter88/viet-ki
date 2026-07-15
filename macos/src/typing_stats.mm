// PHASE6.md 7 (mac): local-only typing statistics (see typing_stats.h).
// Everything here is in-memory during normal operation; saveTypingStats() is
// the only function that touches disk, and callers keep it off the
// CGEventTap thread (see the comment on that function).
#import <Foundation/Foundation.h>

#include "typing_stats.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <time.h>

namespace vietki::mac {

namespace {

NSString* supportDir() {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* base = paths.firstObject ?: NSHomeDirectory();
    return [base stringByAppendingPathComponent:@"VietKi"];
}

std::string typingStatsPath() {
    NSString* p = [supportDir() stringByAppendingPathComponent:@"typing_stats.dat"];
    return std::string(p.UTF8String);
}

std::string u32ToUtf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u32string utf8ToU32(const std::string& s) {
    std::u32string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp = 0;
        size_t len = 1;
        if ((c & 0x80) == 0) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        for (size_t k = 1; k < len; ++k)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string trim(std::string s) {
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) ++first;
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) --last;
    return s.substr(first, last - first);
}

long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Cap how many distinct words we keep on disk so months of daily use cannot
// grow the file without bound. The long tail (rarely typed words) is dropped
// first, by count, at save time.
constexpr size_t kMaxPersistedWords = 500;
// A gap this long or longer between keystrokes is "the user stepped away",
// not typing speed, so it does not count toward the WPM estimate.
constexpr long long kIdleGapMs = 4000;

struct State {
    bool loaded = false;
    bool dirty = false;
    long long totalWords = 0;
    long long totalKeystrokes = 0;
    long long totalBackspaces = 0;
    long long activeMs = 0;
    long long lastKeystrokeMs = 0;
    std::map<std::u32string, long long> wordCounts;
};

State g_stats;

void ensureLoaded() {
    if (g_stats.loaded) return;
    loadTypingStats();
}

} // namespace

void loadTypingStats() {
    g_stats = State{};
    g_stats.loaded = true;

    std::ifstream in(typingStatsPath(), std::ios::binary);
    if (!in) return;

    std::string raw((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    size_t start = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                    (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
                       ? 3
                       : 0;

    bool inWords = false;
    std::istringstream stream(raw.substr(start));
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            inWords = (line == "[TuHayGo]");
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (inWords) {
            try {
                g_stats.wordCounts[utf8ToU32(key)] += std::stoll(value);
            } catch (...) {
            }
            continue;
        }
        try {
            if (key == "totalWords") g_stats.totalWords = std::stoll(value);
            else if (key == "totalKeystrokes") g_stats.totalKeystrokes = std::stoll(value);
            else if (key == "totalBackspaces") g_stats.totalBackspaces = std::stoll(value);
            else if (key == "totalActiveMs") g_stats.activeMs = std::stoll(value);
        } catch (...) {
        }
    }
}

void saveTypingStats() {
    // Nothing recorded since the last save (or nothing loaded at all): skip
    // the write entirely so an app that never enables the option never
    // touches disk for this feature.
    if (!g_stats.loaded || !g_stats.dirty) return;

    std::vector<std::pair<std::u32string, long long>> words(g_stats.wordCounts.begin(),
                                                              g_stats.wordCounts.end());
    std::sort(words.begin(), words.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (words.size() > kMaxPersistedWords) words.resize(kMaxPersistedWords);

    [[NSFileManager defaultManager] createDirectoryAtPath:supportDir()
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];

    std::ostringstream out;
    out << "\xEF\xBB\xBF";
    out << "; VietKi typing stats -- local only, never sent anywhere.\n";
    out << "; Cai dat -> Thong ke -> \"Xoa toan bo du lieu\" xoa tep nay.\n\n";
    out << "totalWords=" << g_stats.totalWords << "\n";
    out << "totalKeystrokes=" << g_stats.totalKeystrokes << "\n";
    out << "totalBackspaces=" << g_stats.totalBackspaces << "\n";
    out << "totalActiveMs=" << g_stats.activeMs << "\n\n";
    out << "[TuHayGo]\n";
    for (auto& [word, count] : words)
        out << u32ToUtf8(word) << "=" << count << "\n";

    std::ofstream file(typingStatsPath(), std::ios::binary | std::ios::trunc);
    if (!file) return;
    std::string text = out.str();
    file.write(text.data(), (std::streamsize)text.size());
    g_stats.dirty = false;
}

void recordWordCommitted(const std::u32string& word) {
    if (word.empty()) return;
    ensureLoaded();
    ++g_stats.totalWords;
    ++g_stats.wordCounts[word];
    g_stats.dirty = true;
}

void recordKeystroke() {
    ensureLoaded();
    ++g_stats.totalKeystrokes;
    long long now = nowMs();
    if (g_stats.lastKeystrokeMs != 0) {
        long long gap = now - g_stats.lastKeystrokeMs;
        if (gap > 0 && gap < kIdleGapMs) g_stats.activeMs += gap;
    }
    g_stats.lastKeystrokeMs = now;
    g_stats.dirty = true;
}

void recordBackspace() {
    ensureLoaded();
    ++g_stats.totalBackspaces;
    g_stats.dirty = true;
}

TypingStatsSnapshot typingStatsSnapshot(int topN) {
    ensureLoaded();
    TypingStatsSnapshot snap;
    snap.totalWords = g_stats.totalWords;
    snap.totalKeystrokes = g_stats.totalKeystrokes;
    snap.totalBackspaces = g_stats.totalBackspaces;
    long long denom = g_stats.totalKeystrokes + g_stats.totalBackspaces;
    snap.backspaceRatioPct =
        denom > 0 ? (100.0 * (double)g_stats.totalBackspaces / (double)denom) : 0.0;
    double activeMinutes = (double)g_stats.activeMs / 60000.0;
    snap.wpm = activeMinutes > 0.001 ? (double)g_stats.totalWords / activeMinutes : 0.0;

    std::vector<WordCount> words;
    words.reserve(g_stats.wordCounts.size());
    for (auto& [word, count] : g_stats.wordCounts) words.push_back({word, count});
    std::sort(words.begin(), words.end(),
              [](const WordCount& a, const WordCount& b) { return a.count > b.count; });
    if ((int)words.size() > topN) words.resize(topN);
    snap.topWords = std::move(words);
    return snap;
}

void clearTypingStats() {
    g_stats = State{};
    g_stats.loaded = true; // dirty stays false: nothing to persist until new activity
    [[NSFileManager defaultManager] removeItemAtPath:@(typingStatsPath().c_str()) error:nil];
}

} // namespace vietki::mac
