// Key injection via SendInput (guide 5.3, 5.4). All injected events carry the
// LLKHF_INJECTED flag automatically, which the hook uses to ignore its own
// output and break the feedback loop.
#include "app.h"

#include <vector>

namespace vietki::win {

namespace {

// Convert UTF-32 to UTF-16, splitting non-BMP code points into surrogate pairs.
std::vector<wchar_t> toUtf16(const std::u32string& text) {
    std::vector<wchar_t> out;
    out.reserve(text.size());
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

void pushKey(std::vector<INPUT>& in, WORD vk, bool keyUp) {
    INPUT i = {};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    i.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    in.push_back(i);
}

void pushUnicode(std::vector<INPUT>& in, wchar_t unit, bool keyUp) {
    INPUT i = {};
    i.type = INPUT_KEYBOARD;
    i.ki.wScan = unit;
    i.ki.dwFlags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0);
    in.push_back(i);
}

} // namespace

void replayPhysicalKey(UINT vk, UINT scanCode) {
    // Phase 5 F.4: send one physical press (down+up) for the double-trigger
    // escape, by scan code so the game gets the real key for the current layout.
    // KEYEVENTF_SCANCODE makes these carry the scan code; SendInput tags them
    // LLKHF_INJECTED, so our own hook ignores them and no loop forms (F.4).
    INPUT in[2] = {};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = (WORD)vk;
        in[i].ki.wScan = (WORD)scanCode;
        in[i].ki.dwFlags = (scanCode ? KEYEVENTF_SCANCODE : 0) |
                           (i == 1 ? KEYEVENTF_KEYUP : 0);
    }
    SendInput(2, in, sizeof(INPUT));
}

void sendBackspaces(int n) {
    if (n <= 0) return;
    std::vector<INPUT> in;
    in.reserve(n * 2);
    for (int i = 0; i < n; ++i) {
        pushKey(in, VK_BACK, false);
        pushKey(in, VK_BACK, true);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void sendUnicodeString(const std::u32string& text) {
    if (text.empty()) return;
    std::vector<wchar_t> units = toUtf16(text);
    std::vector<INPUT> in;
    in.reserve(units.size() * 2);
    for (wchar_t u : units) {
        pushUnicode(in, u, false);
        pushUnicode(in, u, true);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void injectResult(int backspaces, const std::u32string& commit,
                  bool useSelectionReplace) {
    if (backspaces <= 0 && commit.empty()) return;

    if (useSelectionReplace && backspaces > 0) {
        // Select the trailing characters with Shift+Left so the overwrite also
        // swallows any autocomplete suggestion, then type over them (guide 5.4).
        std::vector<INPUT> sel;
        sel.reserve(backspaces * 4);
        // Press Shift, tap Left N times, release Shift.
        pushKey(sel, VK_SHIFT, false);
        for (int i = 0; i < backspaces; ++i) {
            pushKey(sel, VK_LEFT, false);
            pushKey(sel, VK_LEFT, true);
        }
        pushKey(sel, VK_SHIFT, true);
        SendInput((UINT)sel.size(), sel.data(), sizeof(INPUT));
        if (commit.empty()) {
            // Nothing to type over the selection: delete it.
            sendBackspaces(1);
        } else {
            sendUnicodeString(commit);
        }
        return;
    }

    // Default path: send Backspaces then the new text in one batch each.
    sendBackspaces(backspaces);
    sendUnicodeString(commit);
}

} // namespace vietki::win
