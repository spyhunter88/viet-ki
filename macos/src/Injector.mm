// Key injection on macOS via CGEvent (guide 6.3).
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h> // kVK_Delete

#include <vector>

#include "app.h"

namespace vietki::mac {

namespace {

CGEventSourceRef sharedSource() {
    static CGEventSourceRef src =
        CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    return src;
}

void postTagged(CGEventRef ev) {
    // Tag the event as ours so the tap ignores it (anti-loop, guide 6.2).
    CGEventSetIntegerValueField(ev, kCGEventSourceUserData, kInjectMagic);
    CGEventPost(kCGSessionEventTap, ev);
}

void sendBackspace() {
    CGEventSourceRef src = sharedSource();
    CGEventRef down = CGEventCreateKeyboardEvent(src, (CGKeyCode)kVK_Delete, true);
    CGEventRef up = CGEventCreateKeyboardEvent(src, (CGKeyCode)kVK_Delete, false);
    postTagged(down);
    postTagged(up);
    CFRelease(down);
    CFRelease(up);
}

// Convert UTF-32 to UTF-16 (with surrogate pairs for non-BMP).
std::vector<UniChar> toUtf16(const std::u32string& text) {
    std::vector<UniChar> out;
    out.reserve(text.size());
    for (char32_t c : text) {
        if (c <= 0xFFFF) {
            out.push_back((UniChar)c);
        } else {
            c -= 0x10000;
            out.push_back((UniChar)(0xD800 + (c >> 10)));
            out.push_back((UniChar)(0xDC00 + (c & 0x3FF)));
        }
    }
    return out;
}

void sendUnicode(const std::u32string& text) {
    if (text.empty()) return;
    std::vector<UniChar> units = toUtf16(text);
    CGEventSourceRef src = sharedSource();
    CGEventRef down = CGEventCreateKeyboardEvent(src, 0, true);
    CGEventKeyboardSetUnicodeString(down, units.size(), units.data());
    CGEventRef up = CGEventCreateKeyboardEvent(src, 0, false);
    CGEventKeyboardSetUnicodeString(up, units.size(), units.data());
    postTagged(down);
    postTagged(up);
    CFRelease(down);
    CFRelease(up);
}

} // namespace

void injectResult(int backspaces, const std::u32string& commit) {
    for (int i = 0; i < backspaces; ++i) sendBackspace();
    sendUnicode(commit);
}

} // namespace vietki::mac
