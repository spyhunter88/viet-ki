// PHASE6.md 7: local-only typing statistics (see typing_stats.h). Everything
// here is in-memory during normal operation; saveTypingStats() is the only
// function that touches disk, and callers keep it off the keyboard-hook
// thread (see the comment on that function).
#include "typing_stats.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>

namespace vietki::win {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr,
                        nullptr);
    return s;
}

// All composed Vietnamese characters are in the BMP (see engine.h), so a
// direct code-point cast is exactly the same conversion injector.cpp does.
std::wstring u32ToWide(const std::u32string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char32_t c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

std::wstring trim(std::wstring s) {
    size_t first = 0;
    while (first < s.size() && iswspace(s[first])) ++first;
    size_t last = s.size();
    while (last > first && iswspace(s[last - 1])) --last;
    return s.substr(first, last - first);
}

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

std::wstring typingStatsPath() { return exeDir() + L"\\typing_stats.dat"; }

// Cap how many distinct words we keep on disk so months of daily use cannot
// grow the file without bound. The long tail (rarely typed words) is dropped
// first, by count, at save time.
constexpr size_t kMaxPersistedWords = 500;
// A gap this long or longer between keystrokes is "the user stepped away",
// not typing speed, so it does not count toward the WPM estimate.
constexpr ULONGLONG kIdleGapMs = 4000;

struct State {
    bool loaded = false;
    bool dirty = false;
    long long totalWords = 0;
    long long totalKeystrokes = 0;
    long long totalBackspaces = 0;
    long long activeMs = 0;
    ULONGLONG lastKeystrokeTick = 0;
    std::map<std::wstring, long long> wordCounts;
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
    // Skip a UTF-8 BOM if present.
    size_t start = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                    (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
                       ? 3
                       : 0;

    bool inWords = false;
    std::istringstream stream(raw.substr(start));
    std::string lineUtf8;
    while (std::getline(stream, lineUtf8)) {
        if (!lineUtf8.empty() && lineUtf8.back() == '\r') lineUtf8.pop_back();
        std::wstring line = trim(utf8ToWide(lineUtf8));
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            inWords = (line == L"[TuHayGo]");
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = trim(line.substr(0, eq));
        std::wstring value = trim(line.substr(eq + 1));
        if (inWords) {
            try {
                g_stats.wordCounts[key] += std::stoll(value);
            } catch (...) {
            }
            continue;
        }
        try {
            if (key == L"totalWords") g_stats.totalWords = std::stoll(value);
            else if (key == L"totalKeystrokes") g_stats.totalKeystrokes = std::stoll(value);
            else if (key == L"totalBackspaces") g_stats.totalBackspaces = std::stoll(value);
            else if (key == L"totalActiveMs") g_stats.activeMs = std::stoll(value);
        } catch (...) {
        }
    }
}

void saveTypingStats() {
    // Nothing recorded since the last save (or nothing loaded at all): skip
    // the write entirely so an app that never enables the option never
    // touches disk for this feature.
    if (!g_stats.loaded || !g_stats.dirty) return;

    std::vector<std::pair<std::wstring, long long>> words(g_stats.wordCounts.begin(),
                                                           g_stats.wordCounts.end());
    std::sort(words.begin(), words.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (words.size() > kMaxPersistedWords) words.resize(kMaxPersistedWords);

    std::ostringstream out;
    out << "\xEF\xBB\xBF";
    out << "; VietKi typing stats -- local only, never sent anywhere.\n";
    out << "; Settings -> Thong ke -> \"Xoa toan bo du lieu\" deletes this file.\n\n";
    out << "totalWords=" << g_stats.totalWords << "\n";
    out << "totalKeystrokes=" << g_stats.totalKeystrokes << "\n";
    out << "totalBackspaces=" << g_stats.totalBackspaces << "\n";
    out << "totalActiveMs=" << g_stats.activeMs << "\n\n";
    out << "[TuHayGo]\n";
    for (auto& [word, count] : words)
        out << wideToUtf8(word) << "=" << count << "\n";

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
    ++g_stats.wordCounts[u32ToWide(word)];
    g_stats.dirty = true;
}

void recordKeystroke() {
    ensureLoaded();
    ++g_stats.totalKeystrokes;
    ULONGLONG now = GetTickCount64();
    if (g_stats.lastKeystrokeTick != 0) {
        ULONGLONG gap = now - g_stats.lastKeystrokeTick;
        if (gap < kIdleGapMs) g_stats.activeMs += (long long)gap;
    }
    g_stats.lastKeystrokeTick = now;
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
    DeleteFileW(typingStatsPath().c_str());
}

} // namespace vietki::win
