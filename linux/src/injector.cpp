// Key injection on Linux/X11 (guide 6.3, analogue of the Win SendInput and the
// macOS CGEvent injectors). Two jobs:
//   * Backspaces  — fake the physical BackSpace key via XTest.
//   * Unicode text — XTest cannot type a code point directly, so we borrow an
//     unused keycode, point its keysym at the target code point, fake the key,
//     then restore the map. This is the well-worn xdotool technique.
//
// Because our XRecord monitor (hook.cpp) also sees these synthetic events, every
// injected key press/release is announced to the hook via noteInjectedEvents()
// so it can skip exactly that many recorded events and avoid a feedback loop.
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <vector>

#include "app.h"

namespace vietki::lin {

// Declared in hook.cpp: tell the monitor to ignore the next `count` key events.
void noteInjectedEvents(int count);

namespace {

Display* injDisplay() {
    // A dedicated connection for injection keeps XTest off the record loop's
    // connection. Opened once and leaked deliberately for the process lifetime.
    static Display* dpy = XOpenDisplay(nullptr);
    return dpy;
}

int g_perCode = 0;   // server's keysyms-per-keycode; must match on XChangeKeyboardMapping

// Find a keycode whose keysym table is empty, so we can repurpose it for Unicode
// output without clobbering a real key. Cached after the first scan.
KeyCode spareKeycode(Display* dpy) {
    static KeyCode cached = 0;
    if (cached) return cached;
    int minKc = 0, maxKc = 0;
    XDisplayKeycodes(dpy, &minKc, &maxKc);
    int count = maxKc - minKc + 1;
    KeySym* map = XGetKeyboardMapping(dpy, (KeyCode)minKc, count, &g_perCode);
    if (map) {
        for (int i = count - 1; i >= 0; --i) {  // scan from the top, less used
            bool empty = true;
            for (int j = 0; j < g_perCode; ++j)
                if (map[i * g_perCode + j] != NoSymbol) { empty = false; break; }
            if (empty) { cached = (KeyCode)(minKc + i); break; }
        }
        XFree(map);
    }
    if (!cached && maxKc > minKc) cached = (KeyCode)maxKc;  // last-ditch fallback
    return cached;
}

void fakeTap(Display* dpy, KeyCode kc) {
    noteInjectedEvents(2);  // one KeyPress + one KeyRelease will be recorded
    XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
    XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
    XFlush(dpy);
}

void sendChar(Display* dpy, char32_t cp) {
    KeyCode kc = spareKeycode(dpy);
    if (!kc || g_perCode <= 0) return;
    // Code points outside the dedicated keysym ranges use the U+xxxx convention
    // (0x01000000 | code point), which X has understood since XFree86 4.3.
    KeySym sym = (cp <= 0xFF) ? (KeySym)cp : (KeySym)(0x01000000u | cp);
    // Fill every shift level so the tap yields the char regardless of modifiers,
    // and keep keysyms_per_keycode equal to the server's value.
    std::vector<KeySym> on(g_perCode, sym);
    std::vector<KeySym> off(g_perCode, NoSymbol);
    XChangeKeyboardMapping(dpy, kc, g_perCode, on.data(), 1);
    XSync(dpy, False);  // make the remap effective before the tap
    fakeTap(dpy, kc);
    XChangeKeyboardMapping(dpy, kc, g_perCode, off.data(), 1);  // free the key again
    XSync(dpy, False);
}

} // namespace

void injectBackspaces(int n) {
    Display* dpy = injDisplay();
    if (!dpy || n <= 0) return;
    KeyCode bs = XKeysymToKeycode(dpy, XK_BackSpace);
    for (int i = 0; i < n; ++i) fakeTap(dpy, bs);
}

void replayPhysicalKey(unsigned keycode) {
    Display* dpy = injDisplay();
    if (!dpy || keycode == 0) return;
    fakeTap(dpy, (KeyCode)keycode);
}

void injectResult(int backspaces, const std::u32string& commit) {
    Display* dpy = injDisplay();
    if (!dpy) return;
    injectBackspaces(backspaces);
    for (char32_t cp : commit) sendChar(dpy, cp);
}

} // namespace vietki::lin
