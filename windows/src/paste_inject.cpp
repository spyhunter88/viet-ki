// Per-game Unicode paste injection (Phase 5.1 E).
//
// Some games corrupt KEYEVENTF_UNICODE input (Vietnamese diacritics become "?")
// but accept CF_UNICODETEXT pasted with Ctrl+V. This module saves the clipboard
// once per gaming typing session, writes composed text to it, pastes with Ctrl+V,
// and restores the original clipboard when the session ends.
//
// Strategy (Phase 5.1 E.1):
//   * Plain ASCII letters are NOT mangled by the game, so the hook lets them type
//     NATIVELY (instant, no clipboard) — that removes the typing lag.
//   * Only the engine TRANSFORMS (diacritics/tones, r.swallow) go through the
//     clipboard, and ADJACENT transforms are COALESCED into a single paste so a
//     word like "ước" (w horns two letters, s adds a tone) pastes once instead of
//     racing two back-to-back Ctrl+V's. A transform typed after a real pause is
//     applied immediately (safe and responsive); only fast-burst transforms are
//     deferred until the next plain key / word-break / short idle flushes them.
//
// Clipboard save/restore uses plain Win32 (a byte copy of CF_UNICODETEXT), NOT
// OLE: OleGetClipboard hands back an IDataObject proxy to the clipboard OWNER, and
// OleSetClipboard + OleFlushClipboard later render every format synchronously over
// COM/RPC into it — which, when the user Alt-Tabs away, blocks the message loop
// (IME lag) and can fault on a disconnected proxy (crash). A byte copy avoids that.
#include "app.h"

#include <cstring>
#include <string>
#include <vector>

namespace vietki::win {

namespace {

// RAII: releases any modifier the user is physically holding for the duration of
// an injected paste, then re-presses the ones still held. A held Shift (typing an
// uppercase letter) would otherwise turn our Ctrl+V into Ctrl+Shift+V, which the
// game treats as paste-as-plain-text / nothing — so Shift+letter produced no
// output. Ctrl/Alt/Win are cleared too in case one is held when this runs.
class ModifierGuard {
public:
    ModifierGuard() {
        const WORD mods[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                             VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN};
        std::vector<INPUT> ups;
        for (WORD vk : mods) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                held_.push_back(vk);
                INPUT i = {};
                i.type = INPUT_KEYBOARD;
                i.ki.wVk = vk;
                i.ki.dwFlags = KEYEVENTF_KEYUP;
                ups.push_back(i);
            }
        }
        if (!ups.empty()) SendInput((UINT)ups.size(), ups.data(), sizeof(INPUT));
    }
    ~ModifierGuard() {
        if (held_.empty()) return;
        std::vector<INPUT> downs;
        for (WORD vk : held_) {
            INPUT i = {};
            i.type = INPUT_KEYBOARD;
            i.ki.wVk = vk;
            downs.push_back(i);
        }
        SendInput((UINT)downs.size(), downs.data(), sizeof(INPUT));
    }
    ModifierGuard(const ModifierGuard&) = delete;
    ModifierGuard& operator=(const ModifierGuard&) = delete;

private:
    std::vector<WORD> held_;
};

// Original clipboard text saved at session start, written back when it ends.
// Non-text clipboard content (images/files) is not preserved — an accepted
// trade for never touching cross-process OLE clipboard objects.
std::wstring g_savedText;
bool g_restorePending = false;
bool g_pasteSession = false;

// g_committed: text currently on the game screen for this syllable.
// g_target:    text the syllable should show (the engine's display()); may be
//              ahead of g_committed while a fast-burst transform is deferred.
std::u32string g_committed;
std::u32string g_target;
bool g_flushTimerArmed = false;
ULONGLONG g_lastKeyTick = 0;

// A transform typed within this gap of the previous key counts as a fast burst
// and is deferred+coalesced; slower than this it is applied immediately. ~110 ms
// sits just under a normal key-to-key interval, so ordinary typing stays instant.
constexpr ULONGLONG kFastGapMs = 110;
// Pause fallback: a deferred transform with no following key flushes after this,
// so a syllable ending in a diacritic still appears when the user stops. Larger
// than a key interval so active typing flushes via the next key, not this timer.
constexpr UINT kPendingIdleMs = 250;
// Delay before restoring the original clipboard after a session ends, so the
// final paste's Ctrl+V is consumed before its content is replaced.
constexpr UINT kRestoreDelayMs = 150;

size_t commonPrefix(const std::u32string& a, const std::u32string& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

std::wstring toUtf16(const std::u32string& text) {
    std::wstring out;
    for (char32_t c : text) {
        if (c <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            c -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
        }
    }
    return out;
}

HGLOBAL makeGlobal(const void* data, SIZE_T bytes) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return nullptr;
    void* dst = GlobalLock(memory);
    if (!dst) {
        GlobalFree(memory);
        return nullptr;
    }
    memcpy(dst, data, bytes);
    GlobalUnlock(memory);
    return memory;
}

bool openClipboardWithRetry() {
    for (int i = 0; i < 8; ++i) {
        if (OpenClipboard(state().messageWindow)) return true;
        Sleep(2);
    }
    return false;
}

// Always (re)writes the clipboard. We deliberately do NOT short-circuit on an
// unchanged commit: the game (or another app) can take clipboard ownership when
// it pastes, so assuming our text is still there would paste stale content.
bool setClipboardCommit(const std::u32string& commit) {
    std::wstring text = toUtf16(commit);
    if (!openClipboardWithRetry()) return false;
    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    HGLOBAL unicode =
        makeGlobal(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    if (!unicode || !SetClipboardData(CF_UNICODETEXT, unicode)) {
        if (unicode) GlobalFree(unicode);
        CloseClipboard();
        return false;
    }

    const DWORD disabled = 0;
    const wchar_t* formats[] = {
        L"CanIncludeInClipboardHistory",
        L"CanUploadToCloudClipboard",
        L"ExcludeClipboardContentFromMonitorProcessing",
    };
    for (const wchar_t* name : formats) {
        UINT format = RegisterClipboardFormatW(name);
        HGLOBAL flag = makeGlobal(&disabled, sizeof(disabled));
        if (format && flag && !SetClipboardData(format, flag)) GlobalFree(flag);
    }
    CloseClipboard();
    return true;
}

void sendPasteShortcut() {
    INPUT input[4] = {};
    for (INPUT& i : input) i.type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].ki.wVk = 'V';
    input[2].ki.wVk = 'V';
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].ki.wVk = VK_CONTROL;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(input), input, sizeof(INPUT));
}

void snapshotClipboardText() {
    g_savedText.clear();
    if (!openClipboardWithRetry()) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p) {
            g_savedText.assign(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

void restoreClipboardText() {
    if (!openClipboardWithRetry()) return;
    EmptyClipboard();
    if (!g_savedText.empty()) {
        HGLOBAL g = makeGlobal(g_savedText.c_str(),
                               (g_savedText.size() + 1) * sizeof(wchar_t));
        if (g && !SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
    g_savedText.clear();
}

void killFlushTimer() {
    if (g_flushTimerArmed) {
        KillTimer(state().messageWindow, kPasteFlushTimer);
        g_flushTimerArmed = false;
    }
}

// Replace the divergent tail of what is on screen (g_committed) with `target` via
// a single Backspace + Ctrl+V. Runs while the game still has focus.
void flushTo(const std::u32string& target) {
    if (!g_pasteSession) return;
    size_t pfx = commonPrefix(g_committed, target);
    int backspaces = static_cast<int>(g_committed.size() - pfx);
    std::u32string commit(target.begin() + pfx, target.end());
    g_committed = target;
    if (backspaces == 0 && commit.empty()) return; // already on screen
    if (!injectResultPaste(backspaces, commit))
        injectResult(backspaces, commit, false); // ASCII still renders via Unicode
}

} // namespace

bool beginPasteSession() {
    if (g_pasteSession) return true;
    if (g_restorePending) {
        // A deferred restore was still pending from a just-ended session: reclaim
        // it, keep the already-saved text, and cancel the restore.
        KillTimer(state().messageWindow, kPasteRestoreTimer);
        g_restorePending = false;
    } else {
        snapshotClipboardText();
    }
    g_pasteSession = true;
    g_committed.clear();
    g_target.clear();
    g_lastKeyTick = 0;
    killFlushTimer();
    return true;
}

void endPasteSession() {
    if (!g_pasteSession) return;
    killFlushTimer();
    g_committed.clear();
    g_target.clear();
    g_pasteSession = false;
    // Defer restoring the original clipboard: restoring synchronously would race
    // the final paste's Ctrl+V and make the game paste the ORIGINAL clipboard.
    SetTimer(state().messageWindow, kPasteRestoreTimer, kRestoreDelayMs, nullptr);
    g_restorePending = true;
}

void onPasteRestoreTimer() {
    KillTimer(state().messageWindow, kPasteRestoreTimer);
    if (g_restorePending) {
        g_restorePending = false;
        restoreClipboardText();
    }
}

// The hot-path entry. `display` is the engine's full current syllable; `transform`
// is r.swallow (the engine changed earlier glyphs). Returns true if the key was
// handled via the clipboard (caller swallows it), false if the caller should let
// the plain ASCII key type natively.
bool pasteHandleKey(const std::u32string& display, bool transform) {
    if (!g_pasteSession) return false;
    ULONGLONG now = GetTickCount64();
    ULONGLONG gap = now - g_lastKeyTick;
    g_lastKeyTick = now;

    if (transform) {
        g_target = display;
        if (gap >= kFastGapMs) {
            // Deliberate pace: apply now — responsive, and far enough from any
            // previous paste to be safe.
            killFlushTimer();
            flushTo(display);
        } else {
            // Fast burst: defer so adjacent diacritics coalesce into one paste.
            // The next plain key / word-break flushes it; this timer only fires if
            // the user pauses on a diacritic-final syllable.
            SetTimer(state().messageWindow, kPasteFlushTimer, kPendingIdleMs, nullptr);
            g_flushTimerArmed = true;
        }
        return true; // swallow the transform key
    }

    // Plain ASCII append. If the screen already holds everything before this char
    // (no deferred diacritic), let it type natively; otherwise flush the pending
    // diacritics together with this char as one paste and swallow it.
    std::u32string prev = display;
    if (!prev.empty()) prev.pop_back();
    killFlushTimer();
    if (g_committed == prev) {
        g_committed = display; // the native key will land
        g_target = display;
        return false; // type natively (instant)
    }
    flushTo(display);
    g_target = display;
    return true; // swallowed (this char came in via the paste)
}

void onPasteFlushTimer() {
    KillTimer(state().messageWindow, kPasteFlushTimer);
    g_flushTimerArmed = false;
    flushTo(g_target);
}

void flushPendingPaste() {
    killFlushTimer();
    flushTo(g_target);
}

void resetPasteBaseline() {
    flushPendingPaste();
    g_committed.clear();
    g_target.clear();
}

void discardPendingPaste() {
    killFlushTimer();
    g_committed.clear();
    g_target.clear();
}

// Replace the trailing `backspaces` glyphs with `commit` via the clipboard.
// Returns false if the clipboard was unavailable so the caller can fall back.
bool injectResultPaste(int backspaces, const std::u32string& commit) {
    if (!g_pasteSession && !beginPasteSession()) return false;
    // Prepare the clipboard before mutating the target. If clipboard access
    // fails, the caller can safely fall back without deleting text twice.
    if (!commit.empty() && !setClipboardCommit(commit)) return false;
    // Clear any held modifier (esp. Shift for uppercase) so the injected
    // Backspaces and Ctrl+V are not contaminated into Shift+Backspace /
    // Ctrl+Shift+V; the guard re-presses on scope exit.
    ModifierGuard guard;
    sendBackspaces(backspaces);
    if (commit.empty()) return true;
    sendPasteShortcut();
    return true;
}

} // namespace vietki::win
