// macOS Clipboard-paste key injection (Phase 5.1 E).
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include <vector>
#include <string>
#include "app.h"

namespace vietki::mac {

namespace {

class ModifierGuard {
public:
    ModifierGuard() {
        const CGKeyCode keys[] = { kVK_Shift, kVK_RightShift, kVK_Control, kVK_RightControl,
                                  kVK_Option, kVK_RightOption, kVK_Command, kVK_RightCommand };
        for (CGKeyCode k : keys) {
            if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, k)) {
                held_.push_back(k);
                // Send keyUp event
                CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
                CGEventRef up = CGEventCreateKeyboardEvent(src, k, false);
                CGEventSetIntegerValueField(up, kCGEventSourceUserData, kInjectMagic);
                CGEventPost(kCGSessionEventTap, up);
                CFRelease(up);
                CFRelease(src);
            }
        }
    }
    ~ModifierGuard() {
        for (CGKeyCode k : held_) {
            CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
            CGEventRef down = CGEventCreateKeyboardEvent(src, k, true);
            CGEventSetIntegerValueField(down, kCGEventSourceUserData, kInjectMagic);
            CGEventPost(kCGSessionEventTap, down);
            CFRelease(down);
            CFRelease(src);
        }
    }
private:
    std::vector<CGKeyCode> held_;
};

NSString *g_savedText = nil;
bool g_restorePending = false;
bool g_pasteSession = false;

std::u32string g_committed;
std::u32string g_target;
bool g_flushTimerArmed = false;
uint64_t g_lastKeyTime = 0;
uint64_t g_flushTimerId = 0;

constexpr uint64_t kFastGapMs = 110;

size_t commonPrefix(const std::u32string& a, const std::u32string& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

NSString *toNSString(const std::u32string& text) {
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
    return [NSString stringWithCharacters:out.data() length:out.size()];
}

void snapshotClipboard() {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    g_savedText = [pb stringForType:NSPasteboardTypeString];
}

bool openAndSetClipboard(const std::u32string& commit) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb declareTypes:@[NSPasteboardTypeString] owner:nil];
    return [pb setString:toNSString(commit) forType:NSPasteboardTypeString];
}

void sendPasteShortcut() {
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventRef vDown = CGEventCreateKeyboardEvent(src, (CGKeyCode)kVK_ANSI_V, true);
    CGEventRef vUp = CGEventCreateKeyboardEvent(src, (CGKeyCode)kVK_ANSI_V, false);
    
    CGEventSetIntegerValueField(vDown, kCGEventSourceUserData, kInjectMagic);
    CGEventSetIntegerValueField(vUp, kCGEventSourceUserData, kInjectMagic);
    
    CGEventSetFlags(vDown, kCGEventFlagMaskCommand);
    
    CGEventPost(kCGSessionEventTap, vDown);
    CGEventPost(kCGSessionEventTap, vUp);
    
    CFRelease(vDown);
    CFRelease(vUp);
    CFRelease(src);
}

void sendBackspaces(int count) {
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    for (int i = 0; i < count; ++i) {
        CGEventRef down = CGEventCreateKeyboardEvent(src, (CGKeyCode)kVK_Delete, true);
        CGEventRef up = CGEventCreateKeyboardEvent(src, (CGKeyCode)kVK_Delete, false);
        CGEventSetIntegerValueField(down, kCGEventSourceUserData, kInjectMagic);
        CGEventSetIntegerValueField(up, kCGEventSourceUserData, kInjectMagic);
        CGEventPost(kCGSessionEventTap, down);
        CGEventPost(kCGSessionEventTap, up);
        CFRelease(down);
        CFRelease(up);
    }
    CFRelease(src);
}

bool injectResultPaste(int backspaces, const std::u32string& commit);

void killFlushTimer() {
    g_flushTimerId++;
    g_flushTimerArmed = false;
}

void restoreClipboardNow() {
    if (!g_restorePending) return;
    g_restorePending = false;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb declareTypes:@[NSPasteboardTypeString] owner:nil];
    if (g_savedText) {
        [pb setString:g_savedText forType:NSPasteboardTypeString];
    }
}

void scheduleClipboardRestore() {
    g_restorePending = true;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(150 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        restoreClipboardNow();
    });
}

void flushTo(const std::u32string& target) {
    if (!g_pasteSession) return;
    size_t pfx = commonPrefix(g_committed, target);
    int backspaces = static_cast<int>(g_committed.size() - pfx);
    std::u32string commit(target.begin() + pfx, target.end());
    g_committed = target;
    if (backspaces == 0 && commit.empty()) return;
    if (!injectResultPaste(backspaces, commit)) {
        injectResult(backspaces, commit);
    }
}

bool beginPasteSession() {
    if (g_pasteSession) return true;
    if (g_restorePending) {
        g_restorePending = false;
    } else {
        snapshotClipboard();
    }
    g_pasteSession = true;
    g_committed.clear();
    g_target.clear();
    g_lastKeyTime = 0;
    killFlushTimer();
    return true;
}

bool injectResultPaste(int backspaces, const std::u32string& commit) {
    if (!g_pasteSession && !beginPasteSession()) return false;
    if (!commit.empty() && !openAndSetClipboard(commit)) return false;
    ModifierGuard guard;
    sendBackspaces(backspaces);
    if (commit.empty()) return true;
    sendPasteShortcut();
    return true;
}

} // namespace

bool pasteHandleKey(const std::u32string& display, bool transform) {
    if (!g_pasteSession) return false;
    uint64_t now = (uint64_t)([[NSProcessInfo processInfo] systemUptime] * 1000.0);
    uint64_t gap = now - g_lastKeyTime;
    g_lastKeyTime = now;

    if (transform) {
        g_target = display;
        if (gap >= kFastGapMs) {
            killFlushTimer();
            flushTo(display);
        } else {
            killFlushTimer();
            uint64_t tid = ++g_flushTimerId;
            g_flushTimerArmed = true;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(250 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
                if (tid == g_flushTimerId) {
                    g_flushTimerArmed = false;
                    flushTo(g_target);
                }
            });
        }
        return true;
    }

    std::u32string prev = display;
    if (!prev.empty()) prev.pop_back();
    killFlushTimer();
    if (g_committed == prev) {
        g_committed = display;
        g_target = display;
        return false;
    }
    flushTo(display);
    g_target = display;
    return true;
}

void endPasteSession() {
    if (!g_pasteSession) return;
    killFlushTimer();
    g_committed.clear();
    g_target.clear();
    g_pasteSession = false;
    scheduleClipboardRestore();
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
    if (g_pasteSession) {
        g_pasteSession = false;
        restoreClipboardNow();
    }
}

} // namespace vietki::mac
