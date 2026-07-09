// Phase 2 C: the macOS Settings window, built programmatically in AppKit.
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>

#include "app.h"

using namespace vietki;
using namespace vietki::mac;

@interface HotkeyField : NSTextField
@end

@implementation HotkeyField

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)canBecomeKeyView {
    return YES;
}

- (void)mouseDown:(NSEvent *)event {
    [self.window makeFirstResponder:self];
}

- (BOOL)becomeFirstResponder {
    self.backgroundColor = [NSColor selectedControlColor];
    [self setNeedsDisplay:YES];
    return YES;
}

- (BOOL)resignFirstResponder {
    self.backgroundColor = [NSColor controlBackgroundColor];
    [self setNeedsDisplay:YES];
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    unsigned short keycode = event.keyCode;
    if (keycode == 51 || keycode == 117) { // 51 is Backspace, 117 is Delete
        self.stringValue = @"";
        if (self.target && self.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [self.target performSelector:self.action withObject:self];
#pragma clang diagnostic pop
        }
        return;
    }
    std::string name = keyNameFromKeycode(keycode);
    if (!name.empty()) {
        self.stringValue = @(name.c_str());
        if (self.target && self.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [self.target performSelector:self.action withObject:self];
#pragma clang diagnostic pop
        }
    } else {
        NSBeep();
    }
}

- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (self.window.firstResponder == self) {
        unsigned short keycode = event.keyCode;
        if (keycode == 48 || keycode == 53) { // 48 is Tab, 53 is Escape
            return NO;
        }
        [self keyDown:event];
        return YES;
    }
    return [super performKeyEquivalent:event];
}

@end

@interface VietKiSettingsController
    : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate,
                NSTextFieldDelegate>
@property(strong) NSWindow* window;
@property(strong) NSTabView* tabView;
@property(strong) NSTableView* excludedTable;
@property(strong) NSPopUpButton* runningApps;
@property(strong) NSTextField* statusLabel;
@property(strong) NSPopUpButton* methodPopup;
@property(strong) NSPopUpButton* tonePopup;
@property(strong) NSButton* autostartCheck;
@property(strong) NSButton* exclusionCheck;
@property(strong) NSButton* revertCheck;
@property(strong) NSButton* spellCheckBox;
@property(strong) NSButton* lockCancelBox;
@property(strong) NSButton* restoreAfterSpaceBox;
@property(strong) NSPopover* helpPopover;
@property(strong) NSButton* soundGlobalCheck;
@property(strong) NSButton* soundExcludedCheck;
@property(strong) NSButton* masterHotkeyCheck;
@property(strong) NSArray<NSButton*>* masterMods;
@property(strong) NSTextField* masterKey;
@property(strong) NSButton* overrideHotkeyCheck;
@property(strong) NSArray<NSButton*>* overrideMods;
@property(strong) NSTextField* overrideKey;

// Gaming settings properties
@property(strong) NSPopUpButton* gamingPolicyPopup;
@property(strong) NSTextField* gamingTriggerKey;
@property(strong) NSButton* soundGamingCheck;
@property(strong) NSButton* gamingOverlayCheck;
@property(strong) NSPopUpButton* gamingOverlayCornerPopup;
@property(strong) NSTableView* gamingTable;
@property(strong) NSTableView* gamingPasteTable;
@property(strong) NSPopUpButton* gamingRunningApps;

// Button properties for dynamic enable/disable
@property(strong) NSButton* removeButton;
@property(strong) NSButton* removeGameButton;
@property(strong) NSButton* removePasteButton;
@end

static VietKiSettingsController* g_settings = nil;

static NSImage* appIconImage() {
    NSString* path = [[NSBundle mainBundle] pathForResource:@"icon2" ofType:@"png"];
    return path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
}

@implementation VietKiSettingsController

- (NSButton*)checkbox:(NSString*)title at:(NSRect)f action:(SEL)sel {
    NSButton* b = [[NSButton alloc] initWithFrame:f];
    [b setButtonType:NSButtonTypeSwitch];
    b.title = title;
    b.target = self;
    b.action = sel;
    return b;
}

- (NSButton*)helpButtonAt:(NSRect)f action:(SEL)sel {
    NSButton* b = [[NSButton alloc] initWithFrame:f];
    b.bezelStyle = NSBezelStyleHelpButton;
    b.title = @"";
    b.target = self;
    b.action = sel;
    b.toolTip = @"Giải thích tuỳ chọn";
    return b;
}

- (void)showHelpTitle:(NSString*)title body:(NSString*)body relativeTo:(NSView*)anchor {
    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    ps.lineBreakMode = NSLineBreakByWordWrapping;
    ps.paragraphSpacing = 6;
    NSMutableAttributedString* s = [[NSMutableAttributedString alloc] init];
    [s appendAttributedString:
            [[NSAttributedString alloc]
                initWithString:[title stringByAppendingString:@"\n"]
                    attributes:@{
                        NSFontAttributeName : [NSFont boldSystemFontOfSize:13],
                        NSParagraphStyleAttributeName : ps
                    }]];
    [s appendAttributedString:
            [[NSAttributedString alloc]
                initWithString:body
                    attributes:@{
                        NSFontAttributeName : [NSFont systemFontOfSize:12],
                        NSParagraphStyleAttributeName : ps
                    }]];

    const CGFloat width = 320, pad = 16;
    NSTextField* text =
        [[NSTextField alloc] initWithFrame:NSMakeRect(pad, pad, width - 2 * pad, 10)];
    text.editable = NO;
    text.bordered = NO;
    text.drawsBackground = NO;
    text.attributedStringValue = s;
    text.preferredMaxLayoutWidth = width - 2 * pad;
    NSSize sz = [text.cell cellSizeForBounds:NSMakeRect(0, 0, width - 2 * pad, 10000)];
    text.frame = NSMakeRect(pad, pad, width - 2 * pad, sz.height);

    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width,
                                                               sz.height + 2 * pad)];
    [content addSubview:text];
    NSViewController* vc = [[NSViewController alloc] init];
    vc.view = content;

    if (!self.helpPopover) self.helpPopover = [[NSPopover alloc] init];
    self.helpPopover.contentViewController = vc;
    self.helpPopover.contentSize = content.frame.size;
    self.helpPopover.behavior = NSPopoverBehaviorTransient;
    [self.helpPopover showRelativeToRect:anchor.bounds
                                  ofView:anchor
                           preferredEdge:NSRectEdgeMaxX];
}

- (void)onSpellHelp:(id)sender {
    [self showHelpTitle:@"Kiểm tra và khôi phục từ không phải tiếng Việt"
                   body:@"VietKi kiểm tra cấu trúc âm tiết, không dùng từ điển. Nếu một "
                        @"chuỗi chắc chắn không thể là âm tiết tiếng Việt, VietKi trả "
                        @"lại đúng các phím đã gõ và giữ phần còn lại của từ ở dạng "
                        @"thường.\n\nVí dụ:  override → override\n\nMột số chuỗi vẫn "
                        @"mơ hồ. “test” là cách gõ Telex hợp lệ của “tét”; để gõ từ "
                        @"tiếng Anh, hãy gõ “tesst” để chủ động hủy dấu."
             relativeTo:sender];
}
- (void)onLockHelp:(id)sender {
    [self showHelpTitle:@"Giữ nguyên phần còn lại của từ sau khi hủy dấu"
                   body:@"Khi bạn gõ lại cùng một phím dấu để hủy, VietKi hiểu rằng bạn "
                        @"muốn gõ từ ở dạng thường. Các phím còn lại sẽ không tiếp tục "
                        @"tạo dấu cho đến khi kết thúc từ.\n\nBật:  offf → off, "
                        @"tesst → test\n\nTắt: chỉ lần hủy hiện tại có hiệu lực; phím "
                        @"sau có thể tạo dấu lại, nên “offf” → “òf”. Hữu ích khi gõ "
                        @"chuỗi đặc biệt như “đượợợợợc”."
             relativeTo:sender];
}

- (void)onRestoreAfterSpaceHelp:(id)sender {
    [self showHelpTitle:@"Tiếp tục sửa từ sau khi xóa dấu cách"
                   body:@"Khi bạn nhấn dấu cách, VietKi chốt từ vừa gõ lại. Nếu "
                        @"bấm Backspace ngay sau đó, VietKi khôi phục từ đó để "
                        @"bạn gõ tiếp dấu thay vì phải gõ lại cả từ.\n\nVí dụ: "
                        @"nguyen [cách][xóa]x → nguyên\n\nChỉ khôi phục cho đúng "
                        @"một lần dấu cách + một lần Backspace liên tiếp. Gõ "
                        @"tiếp, click chuột, đổi cửa sổ hoặc di chuyển con trỏ "
                        @"sẽ hủy khôi phục."
             relativeTo:sender];
}

- (NSTextField*)label:(NSString*)title at:(NSRect)f {
    NSTextField* l = [[NSTextField alloc] initWithFrame:f];
    l.editable = NO;
    l.bordered = NO;
    l.drawsBackground = NO;
    l.stringValue = title;
    return l;
}

- (NSTextField*)keyEditAt:(NSRect)f {
    HotkeyField* t = [[HotkeyField alloc] initWithFrame:f];
    t.editable = NO;
    t.selectable = NO;
    t.drawsBackground = YES;
    t.backgroundColor = [NSColor controlBackgroundColor];
    t.bezeled = YES;
    t.bezelStyle = NSTextFieldSquareBezel;
    t.alignment = NSTextAlignmentCenter;
    t.target = self;
    t.action = @selector(onHotkeyChanged);
    t.placeholderString = @"Space";
    return t;
}

- (NSArray<NSButton*>*)modifierChecksAtY:(CGFloat)y inView:(NSView*)parentView {
    NSArray* titles = @[ @"Ctrl", @"Option", @"Shift", @"Command" ];
    NSMutableArray<NSButton*>* buttons = [NSMutableArray array];
    CGFloat x = 164;
    CGFloat widths[] = {55, 70, 60, 90};
    for (NSUInteger i = 0; i < titles.count; ++i) {
        NSButton* b = [self checkbox:titles[i]
                                  at:NSMakeRect(x, y, widths[i], 20)
                              action:@selector(onHotkeyChanged)];
        [buttons addObject:b];
        [parentView addSubview:b];
        x += widths[i] + 8;
    }
    return buttons;
}

- (void)setHotkey:(Hotkey)h mods:(NSArray<NSButton*>*)buttons key:(NSTextField*)key {
    uint64_t mods = h.mods;
    buttons[0].state = (mods & kCGEventFlagMaskControl) ? NSControlStateValueOn
                                                        : NSControlStateValueOff;
    buttons[1].state = (mods & kCGEventFlagMaskAlternate) ? NSControlStateValueOn
                                                          : NSControlStateValueOff;
    buttons[2].state = (mods & kCGEventFlagMaskShift) ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
    buttons[3].state = (mods & kCGEventFlagMaskCommand) ? NSControlStateValueOn
                                                        : NSControlStateValueOff;
    key.stringValue = @(keyNameFromKeycode(h.keycode).c_str());
}

- (Hotkey)hotkeyFromMods:(NSArray<NSButton*>*)buttons
                     key:(NSTextField*)key
       allowModifierOnly:(BOOL)allowModifierOnly
                fallback:(const char*)fallback {
    std::string s;
    auto add = [&](const char* part) {
        if (!s.empty()) s += "+";
        s += part;
    };
    if (buttons[0].state == NSControlStateValueOn) add("Ctrl");
    if (buttons[1].state == NSControlStateValueOn) add("Option");
    if (buttons[2].state == NSControlStateValueOn) add("Shift");
    if (buttons[3].state == NSControlStateValueOn) add("Command");

    NSString* keyText = [key.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (keyText.length) {
        if (!s.empty()) s += "+";
        s += keyText.UTF8String;
    }
    Hotkey h = hotkeyFromString(s);
    if (!allowModifierOnly && h.keycode == 0) return {};
    if (!h.bound() && fallback) h = hotkeyFromString(fallback);
    return h;
}

- (void)build {
    NSRect frame = NSMakeRect(0, 0, 580, 600);
    self.window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable)
                                       backing:NSBackingStoreBuffered
                                         defer:NO];
    self.window.title = @"VietKi — Cài đặt";
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    NSImage* icon = appIconImage();
    if (icon) {
        self.window.miniwindowImage = icon;
        [NSApp setApplicationIconImage:icon];
    }
    NSView* v = self.window.contentView;

    self.statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 568, 548, 20)];
    self.statusLabel.editable = NO;
    self.statusLabel.bordered = NO;
    self.statusLabel.drawsBackground = NO;
    [v addSubview:self.statusLabel];

    // Create NSTabView
    self.tabView = [[NSTabView alloc] initWithFrame:NSMakeRect(10, 42, 560, 520)];
    
    // --- TAB 1: CHUNG ---
    NSTabViewItem* tab1 = [[NSTabViewItem alloc] initWithIdentifier:@"general"];
    tab1.label = @"Chung";
    NSView* tab1View = [[NSView alloc] initWithFrame:self.tabView.bounds];
    tab1.view = tab1View;
    
    [tab1View addSubview:[self label:@"Kiểu gõ:" at:NSMakeRect(16, 442, 60, 20)]];
    self.methodPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(80, 440, 100, 26)];
    [self.methodPopup addItemsWithTitles:@[ @"Telex", @"VNI" ]];
    self.methodPopup.target = self;
    self.methodPopup.action = @selector(onMethod);
    [tab1View addSubview:self.methodPopup];

    [tab1View addSubview:[self label:@"Đặt dấu:" at:NSMakeRect(206, 442, 60, 20)]];
    self.tonePopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(270, 440, 200, 26)];
    [self.tonePopup addItemsWithTitles:@[ @"Dấu kiểu mới", @"Dấu kiểu cũ" ]];
    self.tonePopup.target = self;
    self.tonePopup.action = @selector(onTone);
    [tab1View addSubview:self.tonePopup];

    self.autostartCheck = [self checkbox:@"Khởi động cùng hệ thống"
                                      at:NSMakeRect(16, 412, 220, 20)
                                  action:@selector(onAutostartToggle)];
    [tab1View addSubview:self.autostartCheck];

    self.exclusionCheck = [self checkbox:@"Bật tính năng loại trừ theo app"
                                      at:NSMakeRect(16, 388, 300, 20)
                                  action:@selector(onExclusionToggle)];
    [tab1View addSubview:self.exclusionCheck];
    
    self.revertCheck = [self checkbox:@"Bỏ bật/tắt riêng khi rời app"
                                   at:NSMakeRect(16, 364, 300, 20)
                               action:@selector(onRevertToggle)];
    [tab1View addSubview:self.revertCheck];
    
    self.spellCheckBox =
        [self checkbox:@"Kiểm tra và khôi phục từ không phải tiếng Việt"
                    at:NSMakeRect(16, 340, 500, 20)
                action:@selector(onSpellToggle)];
    [tab1View addSubview:self.spellCheckBox];
    NSButton* spellHelp = [self helpButtonAt:NSMakeRect(520, 338, 24, 24)
                                      action:@selector(onSpellHelp:)];
    [tab1View addSubview:spellHelp];
    
    self.lockCancelBox =
        [self checkbox:@"Giữ nguyên phần còn lại của từ sau khi hủy dấu"
                    at:NSMakeRect(16, 316, 500, 20)
                action:@selector(onLockToggle)];
    [tab1View addSubview:self.lockCancelBox];
    NSButton* lockHelp = [self helpButtonAt:NSMakeRect(520, 314, 24, 24)
                                     action:@selector(onLockHelp:)];
    [tab1View addSubview:lockHelp];

    self.restoreAfterSpaceBox =
        [self checkbox:@"Tiếp tục sửa từ sau khi xóa dấu cách"
                    at:NSMakeRect(16, 292, 500, 20)
                action:@selector(onRestoreAfterSpaceToggle)];
    [tab1View addSubview:self.restoreAfterSpaceBox];
    NSButton* restoreHelp = [self helpButtonAt:NSMakeRect(520, 290, 24, 24)
                                        action:@selector(onRestoreAfterSpaceHelp:)];
    [tab1View addSubview:restoreHelp];

    self.soundGlobalCheck = [self checkbox:@"Âm báo khi đổi E/V"
                                        at:NSMakeRect(16, 266, 210, 20)
                                    action:@selector(onSoundToggle)];
    [tab1View addSubview:self.soundGlobalCheck];
    self.soundExcludedCheck = [self checkbox:@"Âm báo khi đổi V-/V+"
                                          at:NSMakeRect(240, 266, 210, 20)
                                      action:@selector(onSoundToggle)];
    [tab1View addSubview:self.soundExcludedCheck];

    self.masterHotkeyCheck = [self checkbox:@""
                                         at:NSMakeRect(16, 236, 18, 20)
                                     action:@selector(onHotkeyChanged)];
    [tab1View addSubview:self.masterHotkeyCheck];
    [tab1View addSubview:[self label:@"Bật/tắt tiếng Việt" at:NSMakeRect(38, 238, 120, 18)]];
    self.masterMods = [self modifierChecksAtY:236 inView:tab1View];
    self.masterKey = [self keyEditAt:NSMakeRect(480, 236, 64, 22)];
    [tab1View addSubview:self.masterKey];

    self.overrideHotkeyCheck = [self checkbox:@""
                                           at:NSMakeRect(16, 204, 18, 20)
                                       action:@selector(onHotkeyChanged)];
    [tab1View addSubview:self.overrideHotkeyCheck];
    [tab1View addSubview:[self label:@"Bật V+ app loại trừ" at:NSMakeRect(38, 206, 120, 18)]];
    self.overrideMods = [self modifierChecksAtY:204 inView:tab1View];
    self.overrideKey = [self keyEditAt:NSMakeRect(480, 204, 64, 22)];
    [tab1View addSubview:self.overrideKey];

    // Excluded list.
    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 60, 528, 130)];
    scroll.hasVerticalScroller = YES;
    self.excludedTable = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"bundle"];
    col.title = @"Bundle ID bị loại trừ (kèm override)";
    col.width = 508;
    [self.excludedTable addTableColumn:col];
    self.excludedTable.dataSource = self;
    self.excludedTable.delegate = self;
    scroll.documentView = self.excludedTable;
    [tab1View addSubview:scroll];

    self.removeButton = [[NSButton alloc] initWithFrame:NSMakeRect(16, 32, 100, 28)];
    self.removeButton.title = @"Xoá";
    self.removeButton.bezelStyle = NSBezelStyleRounded;
    self.removeButton.target = self;
    self.removeButton.action = @selector(onRemove);
    [tab1View addSubview:self.removeButton];

    self.runningApps = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(122, 32, 300, 28)];
    [tab1View addSubview:self.runningApps];
    NSButton* add = [[NSButton alloc] initWithFrame:NSMakeRect(430, 32, 114, 28)];
    add.title = @"Thêm app";
    add.bezelStyle = NSBezelStyleRounded;
    add.target = self;
    add.action = @selector(onAddRunning);
    [tab1View addSubview:add];

    // --- TAB 2: GAMING ---
    NSTabViewItem* tab2 = [[NSTabViewItem alloc] initWithIdentifier:@"gaming"];
    tab2.label = @"Gaming";
    NSView* tab2View = [[NSView alloc] initWithFrame:self.tabView.bounds];
    tab2.view = tab2View;

    [tab2View addSubview:[self label:@"Gaming Mode:" at:NSMakeRect(16, 442, 100, 20)]];
    self.gamingPolicyPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(120, 440, 416, 26)];
    [self.gamingPolicyPopup addItemsWithTitles:@[
        @"Không dùng (Disabled)",
        @"Bật đổi V-/V+ cho ứng dụng game (Toggle)",
        @"Bật gõ tiếng Việt tạm thời (Temporary Trigger)"
    ]];
    self.gamingPolicyPopup.target = self;
    self.gamingPolicyPopup.action = @selector(onGamingPolicy);
    [tab2View addSubview:self.gamingPolicyPopup];

    [tab2View addSubview:[self label:@"Phím kích hoạt:" at:NSMakeRect(16, 408, 100, 20)]];
    self.gamingTriggerKey = [self keyEditAt:NSMakeRect(120, 408, 64, 22)];
    [tab2View addSubview:self.gamingTriggerKey];

    self.soundGamingCheck = [self checkbox:@"Bật âm thanh khi VietKi chuyển chế độ"
                                        at:NSMakeRect(16, 376, 400, 20)
                                    action:@selector(onGamingConfigChanged)];
    [tab2View addSubview:self.soundGamingCheck];

    self.gamingOverlayCheck = [self checkbox:@"Hiện overlay khi gõ tiếng Việt"
                                           at:NSMakeRect(16, 348, 200, 20)
                                       action:@selector(onGamingConfigChanged)];
    [tab2View addSubview:self.gamingOverlayCheck];

    [tab2View addSubview:[self label:@"Vị trí:" at:NSMakeRect(230, 350, 50, 20)]];
    self.gamingOverlayCornerPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(280, 348, 150, 26)];
    [self.gamingOverlayCornerPopup addItemsWithTitles:@[
        @"Trên trái",
        @"Trên phải",
        @"Dưới trái",
        @"Dưới phải"
    ]];
    self.gamingOverlayCornerPopup.target = self;
    self.gamingOverlayCornerPopup.action = @selector(onGamingConfigChanged);
    [tab2View addSubview:self.gamingOverlayCornerPopup];

    [tab2View addSubview:[self label:@"Ứng dụng Game:" at:NSMakeRect(16, 318, 260, 20)]];
    [tab2View addSubview:[self label:@"Sử dụng Clipboard (Paste):" at:NSMakeRect(284, 318, 260, 20)]];

    NSScrollView* scrollGaming = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 120, 260, 190)];
    scrollGaming.hasVerticalScroller = YES;
    self.gamingTable = [[NSTableView alloc] initWithFrame:scrollGaming.bounds];
    NSTableColumn* colG = [[NSTableColumn alloc] initWithIdentifier:@"gamingApp"];
    colG.title = @"Tên/Bundle ID game";
    colG.width = 240;
    [self.gamingTable addTableColumn:colG];
    self.gamingTable.dataSource = self;
    self.gamingTable.delegate = self;
    scrollGaming.documentView = self.gamingTable;
    [tab2View addSubview:scrollGaming];

    NSScrollView* scrollGamingPaste = [[NSScrollView alloc] initWithFrame:NSMakeRect(284, 120, 260, 190)];
    scrollGamingPaste.hasVerticalScroller = YES;
    self.gamingPasteTable = [[NSTableView alloc] initWithFrame:scrollGamingPaste.bounds];
    NSTableColumn* colGP = [[NSTableColumn alloc] initWithIdentifier:@"gamingPasteApp"];
    colGP.title = @"Tên/Bundle ID paste";
    colGP.width = 240;
    [self.gamingPasteTable addTableColumn:colGP];
    self.gamingPasteTable.dataSource = self;
    self.gamingPasteTable.delegate = self;
    scrollGamingPaste.documentView = self.gamingPasteTable;
    [tab2View addSubview:scrollGamingPaste];

    self.gamingRunningApps = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(16, 82, 300, 28)];
    [tab2View addSubview:self.gamingRunningApps];
    
    NSButton* addGame = [[NSButton alloc] initWithFrame:NSMakeRect(324, 82, 110, 28)];
    addGame.title = @"Thêm -> Game";
    addGame.bezelStyle = NSBezelStyleRounded;
    addGame.target = self;
    addGame.action = @selector(onAddGame);
    [tab2View addSubview:addGame];

    NSButton* addPaste = [[NSButton alloc] initWithFrame:NSMakeRect(438, 82, 110, 28)];
    addPaste.title = @"Thêm -> Paste";
    addPaste.bezelStyle = NSBezelStyleRounded;
    addPaste.target = self;
    addPaste.action = @selector(onAddPaste);
    [tab2View addSubview:addPaste];

    self.removeGameButton = [[NSButton alloc] initWithFrame:NSMakeRect(16, 48, 100, 28)];
    self.removeGameButton.title = @"Xoá Game";
    self.removeGameButton.bezelStyle = NSBezelStyleRounded;
    self.removeGameButton.target = self;
    self.removeGameButton.action = @selector(onRemoveGame);
    [tab2View addSubview:self.removeGameButton];

    self.removePasteButton = [[NSButton alloc] initWithFrame:NSMakeRect(284, 48, 100, 28)];
    self.removePasteButton.title = @"Xoá Paste";
    self.removePasteButton.bezelStyle = NSBezelStyleRounded;
    self.removePasteButton.target = self;
    self.removePasteButton.action = @selector(onRemovePaste);
    [tab2View addSubview:self.removePasteButton];

    [self.tabView addTabViewItem:tab1];
    [self.tabView addTabViewItem:tab2];
    [v addSubview:self.tabView];

    NSTextField* about = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 12, 548, 20)];
    about.editable = NO;
    about.bordered = NO;
    about.drawsBackground = NO;
    about.stringValue = @"VietKi 0.6 — bộ gõ tiếng Việt";
    [v addSubview:about];
}

- (void)populateRunningApps {
    [self.runningApps removeAllItems];
    [self.gamingRunningApps removeAllItems];
    NSArray<NSRunningApplication*>* apps = NSWorkspace.sharedWorkspace.runningApplications;
    for (NSRunningApplication* a in apps) {
        if (a.activationPolicy != NSApplicationActivationPolicyRegular) continue;
        if (!a.bundleIdentifier) continue;
        NSString* title =
            [NSString stringWithFormat:@"%@ (%@)", a.localizedName ?: @"?",
                                       a.bundleIdentifier];
        [self.runningApps addItemWithTitle:title];
        self.runningApps.lastItem.representedObject = a.bundleIdentifier;

        [self.gamingRunningApps addItemWithTitle:title];
        self.gamingRunningApps.lastItem.representedObject = a.bundleIdentifier;
    }
}

- (void)refresh {
    AppConfig& c = state().config;
    [self.methodPopup selectItemAtIndex:(c.method == Method::VNI ? 1 : 0)];
    [self.tonePopup selectItemAtIndex:(c.tone == TonePlacement::Old ? 1 : 0)];
    self.autostartCheck.state = c.autostart ? NSControlStateValueOn : NSControlStateValueOff;
    self.exclusionCheck.state = c.exclusionFeatureOn ? NSControlStateValueOn
                                                     : NSControlStateValueOff;
    self.revertCheck.state = c.revertOverrideOnBlur ? NSControlStateValueOn
                                                    : NSControlStateValueOff;
    self.spellCheckBox.state = c.spellCheck ? NSControlStateValueOn
                                            : NSControlStateValueOff;
    self.lockCancelBox.state = c.lockWordAfterCancel ? NSControlStateValueOn
                                                     : NSControlStateValueOff;
    self.restoreAfterSpaceBox.state = c.restoreAfterSpace ? NSControlStateValueOn
                                                          : NSControlStateValueOff;
    self.soundGlobalCheck.state = c.soundOnGlobalToggle ? NSControlStateValueOn
                                                        : NSControlStateValueOff;
    self.soundExcludedCheck.state = c.soundOnExcludedToggle ? NSControlStateValueOn
                                                           : NSControlStateValueOff;
    self.masterHotkeyCheck.state = c.toggleVietnameseHotkeyEnabled ? NSControlStateValueOn
                                                                   : NSControlStateValueOff;
    self.overrideHotkeyCheck.state = c.overrideHotkeyEnabled ? NSControlStateValueOn
                                                            : NSControlStateValueOff;
    [self setHotkey:hotkeyFromString(c.hotkey) mods:self.masterMods key:self.masterKey];
    [self setHotkey:c.overrideHotkey mods:self.overrideMods key:self.overrideKey];
    
    // Gaming fields
    [self.gamingPolicyPopup selectItemAtIndex:static_cast<NSInteger>(c.gamingPolicy)];
    self.gamingTriggerKey.stringValue = @(keyNameFromKeycode(c.gamingTrigger.keycode).c_str());
    self.soundGamingCheck.state = c.soundOnGamingModeSwitch ? NSControlStateValueOn : NSControlStateValueOff;
    self.gamingOverlayCheck.state = c.gamingOverlayEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    [self.gamingOverlayCornerPopup selectItemAtIndex:c.gamingOverlayCorner];
    
    [self populateRunningApps];
    [self.excludedTable reloadData];
    [self.gamingTable reloadData];
    [self.gamingPasteTable reloadData];

    AppState& st = state();
    NSString* mode = st.currentModeVN ? @"Tiếng Việt" : @"Tiếng Anh";
    NSString* app = st.currentApp.empty() ? @"(không rõ)" : @(st.currentApp.c_str());
    self.statusLabel.stringValue =
        [NSString stringWithFormat:@"Đang gõ: %@   App: %@", mode, app];
    [self updateRemoveButtonStates];
}

// --- table data source ---
- (NSInteger)numberOfRowsInTableView:(NSTableView*)t {
    if (t == self.excludedTable) {
        return (NSInteger)state().config.excludedBundles.size();
    } else if (t == self.gamingTable) {
        return (NSInteger)state().config.gamingBundles.size();
    } else if (t == self.gamingPasteTable) {
        return (NSInteger)state().config.gamingPasteBundles.size();
    }
    return 0;
}

- (id)tableView:(NSTableView*)t
    objectValueForTableColumn:(NSTableColumn*)col
                          row:(NSInteger)row {
    AppState& st = state();
    if (t == self.excludedTable) {
        if (row < 0 || row >= (NSInteger)st.config.excludedBundles.size()) return @"";
        std::string b = st.config.excludedBundles[row];
        NSString* line = @(b.c_str());
        auto it = st.perAppOverride.find(b);
        if (it != st.perAppOverride.end() && it->second != Override::None)
            line = [line stringByAppendingString:@"   [V+]"];
        return line;
    } else if (t == self.gamingTable) {
        if (row < 0 || row >= (NSInteger)st.config.gamingBundles.size()) return @"";
        return @(st.config.gamingBundles[row].c_str());
    } else if (t == self.gamingPasteTable) {
        if (row < 0 || row >= (NSInteger)st.config.gamingPasteBundles.size()) return @"";
        return @(st.config.gamingPasteBundles[row].c_str());
    }
    return @"";
}

- (void)updateRemoveButtonStates {
    self.removeButton.enabled = (self.excludedTable.selectedRow >= 0);
    self.removeGameButton.enabled = (self.gamingTable.selectedRow >= 0);
    self.removePasteButton.enabled = (self.gamingPasteTable.selectedRow >= 0);
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    [self updateRemoveButtonStates];
}

// --- actions ---
- (void)onMethod {
    setMethod(self.methodPopup.indexOfSelectedItem == 1 ? Method::VNI : Method::Telex);
}
- (void)onTone {
    setTonePlacement(self.tonePopup.indexOfSelectedItem == 1 ? TonePlacement::Old
                                                             : TonePlacement::Modern);
}
- (void)onAutostartToggle {
    state().config.autostart = (self.autostartCheck.state == NSControlStateValueOn);
    saveConfig(state().config);
}
- (void)onExclusionToggle { toggleExclusionFeature(); }
- (void)onRevertToggle {
    state().config.revertOverrideOnBlur = (self.revertCheck.state == NSControlStateValueOn);
    saveConfig(state().config);
}
- (void)onSpellToggle {
    state().config.spellCheck = (self.spellCheckBox.state == NSControlStateValueOn);
    saveConfig(state().config);
    applyResolvedState();
}
- (void)onLockToggle {
    state().config.lockWordAfterCancel =
        (self.lockCancelBox.state == NSControlStateValueOn);
    saveConfig(state().config);
    applyResolvedState();
}
- (void)onRestoreAfterSpaceToggle {
    state().config.restoreAfterSpace =
        (self.restoreAfterSpaceBox.state == NSControlStateValueOn);
    saveConfig(state().config);
    applyResolvedState();
}
- (void)onSoundToggle {
    state().config.soundOnGlobalToggle =
        (self.soundGlobalCheck.state == NSControlStateValueOn);
    state().config.soundOnExcludedToggle =
        (self.soundExcludedCheck.state == NSControlStateValueOn);
    saveConfig(state().config);
}
- (void)onHotkeyChanged {
    Hotkey master = [self hotkeyFromMods:self.masterMods
                                     key:self.masterKey
                       allowModifierOnly:YES
                                fallback:"Ctrl+Shift"];
    Hotkey override = [self hotkeyFromMods:self.overrideMods
                                       key:self.overrideKey
                         allowModifierOnly:NO
                                  fallback:nullptr];
    
    std::string triggerStr = self.gamingTriggerKey.stringValue.UTF8String;
    Hotkey trigger = hotkeyFromString(triggerStr);

    state().config.toggleVietnameseHotkeyEnabled =
        (self.masterHotkeyCheck.state == NSControlStateValueOn);
    state().config.overrideHotkeyEnabled =
        (self.overrideHotkeyCheck.state == NSControlStateValueOn);
    state().config.hotkey = stringFromHotkey(master);
    state().config.overrideHotkey = override;
    
    if (trigger.bound()) {
        state().config.gamingTrigger = trigger;
    }

    saveConfig(state().config);
}

- (void)controlTextDidChange:(NSNotification*)note { [self onHotkeyChanged]; }

- (void)onRemove {
    NSInteger row = self.excludedTable.selectedRow;
    auto& v = state().config.excludedBundles;
    if (row < 0 || row >= (NSInteger)v.size()) return;
    v.erase(v.begin() + row);
    saveConfig(state().config);
    [self.excludedTable reloadData];
    [self updateRemoveButtonStates];
    applyResolvedState();
}

- (void)onAddRunning {
    NSString* bid = self.runningApps.selectedItem.representedObject;
    if (!bid) return;
    std::string b = bid.UTF8String;
    for (const auto& e : state().config.excludedBundles)
        if (e == b) return;
    state().config.excludedBundles.push_back(b);
    saveConfig(state().config);
    [self.excludedTable reloadData];
    [self updateRemoveButtonStates];
    applyResolvedState();
}

- (void)onGamingPolicy {
    GamingPolicy gp = static_cast<GamingPolicy>(self.gamingPolicyPopup.indexOfSelectedItem);
    applyGamingPolicy(gp);
    saveConfig(state().config);
}

- (void)onGamingConfigChanged {
    state().config.soundOnGamingModeSwitch = (self.soundGamingCheck.state == NSControlStateValueOn);
    state().config.gamingOverlayEnabled = (self.gamingOverlayCheck.state == NSControlStateValueOn);
    state().config.gamingOverlayCorner = (int)self.gamingOverlayCornerPopup.indexOfSelectedItem;
    saveConfig(state().config);
    applyResolvedState();
}

- (void)onAddGame {
    NSString* bid = self.gamingRunningApps.selectedItem.representedObject;
    if (!bid) return;
    std::string b = bid.UTF8String;
    for (const auto& e : state().config.gamingBundles)
        if (e == b) return;
    state().config.gamingBundles.push_back(b);
    saveConfig(state().config);
    [self.gamingTable reloadData];
    [self updateRemoveButtonStates];
    applyResolvedState();
}

- (void)onAddPaste {
    NSString* bid = self.gamingRunningApps.selectedItem.representedObject;
    if (!bid) return;
    std::string b = bid.UTF8String;
    for (const auto& e : state().config.gamingPasteBundles)
        if (e == b) return;
    state().config.gamingPasteBundles.push_back(b);
    saveConfig(state().config);
    [self.gamingPasteTable reloadData];
    [self updateRemoveButtonStates];
    applyResolvedState();
}

- (void)onRemoveGame {
    NSInteger row = self.gamingTable.selectedRow;
    auto& v = state().config.gamingBundles;
    if (row < 0 || row >= (NSInteger)v.size()) return;
    v.erase(v.begin() + row);
    saveConfig(state().config);
    [self.gamingTable reloadData];
    [self updateRemoveButtonStates];
    applyResolvedState();
}

- (void)onRemovePaste {
    NSInteger row = self.gamingPasteTable.selectedRow;
    auto& v = state().config.gamingPasteBundles;
    if (row < 0 || row >= (NSInteger)v.size()) return;
    v.erase(v.begin() + row);
    saveConfig(state().config);
    [self.gamingPasteTable reloadData];
    [self updateRemoveButtonStates];
    applyResolvedState();
}

- (void)windowWillClose:(NSNotification*)n {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

@end

namespace vietki::mac {

void openSettings() {
    if (!g_settings) {
        g_settings = [[VietKiSettingsController alloc] init];
        [g_settings build];
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    [g_settings refresh];
    [g_settings.window center];
    [g_settings.window makeKeyAndOrderFront:nil];
}

void refreshSettingsWindow() {
    if (g_settings && g_settings.window.isVisible) [g_settings refresh];
}

} // namespace vietki::mac
