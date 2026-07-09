// Phase 2 C + A: the Settings window and the drag-to-exclude window picker.
//
// The window is a modeless Win32 dialog (IDD_SETTINGS) opened from the tray. The
// drag picker captures the mouse while the user holds the "Giữ và kéo" button,
// previews the target process, then stages it until the user explicitly clicks
// "Thêm". The dialog keeps edits as a draft until "Lưu" is pressed.
#include "app.h"
#include "typing_stats.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <psapi.h>

// Win11 rounded-corner API. Older SDKs (pre-NTDDI_WIN10_CO) don't declare
// these, so provide a fallback definition. The attribute is a harmless no-op
// on systems that don't recognize it.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#if !defined(NTDDI_WIN10_CO) || (NTDDI_VERSION < NTDDI_WIN10_CO)
typedef enum {
    DWMWCP_DEFAULT = 0,
    DWMWCP_DONOTROUND = 1,
    DWMWCP_ROUND = 2,
    DWMWCP_ROUNDSMALL = 3
} DWM_WINDOW_CORNER_PREFERENCE;
#endif

#include <shellapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <cwctype>
#include <algorithm>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

namespace vietki::win {

namespace {

// --- dialog state ----------------------------------------------------------
bool g_picking = false;
HBRUSH g_tabPageBrush = nullptr;
COLORREF g_tabPageColor = RGB(255, 255, 255);
HWND g_tabPages[5] = {};
HWND g_highlight = nullptr;
HCURSOR g_crossCursor = nullptr;
HWND g_tooltip = nullptr;
HICON g_settingsIcon = nullptr;
HICON g_settingsSmallIcon = nullptr;
HIMAGELIST g_listImages = nullptr;
std::vector<std::wstring> g_draftExcluded;
bool g_dirty = false;
bool g_suppressDirty = false;
int g_activeTab = 0;
// Phase 5: gaming-tab draft state. The crosshair targets one of the two lists.
std::vector<std::wstring> g_draftGaming;
std::vector<std::wstring> g_draftGamingPaste;
bool g_refreshingGamingList = false;
TriggerBinding g_draftTrigger;
int g_pickTarget = IDC_CROSSHAIR; // which crosshair started the current pick
// DPI the dialog's controls are currently laid out for (see WM_DPICHANGED);
// g_dlgFont is the font we created for that DPI, owned by us (must delete).
int g_dlgDpi = USER_DEFAULT_SCREEN_DPI;
HFONT g_dlgFont = nullptr;

void updateActionButtons(HWND dlg);
INT_PTR CALLBACK pageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam);
// Cross-list dedupe: an app may live in the excluded list or the gaming list,
// but not both. Reports which list already holds `exe` (nullptr to ignore).
bool inEitherDraft(const std::wstring& exe, const wchar_t** whichList);

HWND settingsControl(HWND dlg, int id) {
    if (HWND control = ::GetDlgItem(dlg, id)) return control;
    for (HWND page : g_tabPages) {
        if (page) {
            if (HWND control = ::GetDlgItem(page, id)) return control;
        }
    }
    return nullptr;
}

BOOL settingsSetDlgItemText(HWND dlg, int id, LPCWSTR text) {
    HWND control = settingsControl(dlg, id);
    return control ? SetWindowTextW(control, text) : FALSE;
}

UINT settingsGetDlgItemText(HWND dlg, int id, LPWSTR text, int maxChars) {
    HWND control = settingsControl(dlg, id);
    return control ? GetWindowTextW(control, text, maxChars) : 0;
}

BOOL settingsCheckDlgButton(HWND dlg, int id, UINT check) {
    HWND control = settingsControl(dlg, id);
    if (!control) return FALSE;
    SendMessageW(control, BM_SETCHECK, check, 0);
    return TRUE;
}

UINT settingsIsDlgButtonChecked(HWND dlg, int id) {
    HWND control = settingsControl(dlg, id);
    return control ? (UINT)SendMessageW(control, BM_GETCHECK, 0, 0) : BST_UNCHECKED;
}

BOOL settingsCheckRadioButton(HWND dlg, int first, int last, int checked) {
    for (int id = first; id <= last; ++id)
        settingsCheckDlgButton(dlg, id, id == checked ? BST_CHECKED : BST_UNCHECKED);
    return TRUE;
}

#define GetDlgItem settingsControl
#define SetDlgItemTextW settingsSetDlgItemText
#define GetDlgItemTextW settingsGetDlgItemText
#define CheckDlgButton settingsCheckDlgButton
#define IsDlgButtonChecked settingsIsDlgButtonChecked
#define CheckRadioButton settingsCheckRadioButton

void refreshTabPageBrush(HWND dlg) {
    COLORREF color = GetSysColor(COLOR_WINDOW);
    HWND tab = GetDlgItem(dlg, IDC_TAB);
    if (tab && IsThemeActive() && IsAppThemed()) {
        if (HTHEME theme = OpenThemeData(tab, L"TAB")) {
            // TMT_FILLCOLOR is theme metadata and may be the accent/highlight
            // color rather than the pixels used to draw TABP_PANE. Render the
            // pane off-screen and sample its interior instead.
            HDC screen = GetDC(nullptr);
            HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
            HBITMAP bitmap =
                (screen && memory) ? CreateCompatibleBitmap(screen, 64, 64) : nullptr;
            if (bitmap) {
                HGDIOBJ previous = SelectObject(memory, bitmap);
                RECT sampleRect = {0, 0, 64, 64};
                FillRect(memory, &sampleRect, GetSysColorBrush(COLOR_WINDOW));
                if (SUCCEEDED(DrawThemeBackground(theme, memory, TABP_PANE, 0,
                                                  &sampleRect, nullptr))) {
                    COLORREF sampled = GetPixel(memory, 32, 32);
                    if (sampled != CLR_INVALID) color = sampled;
                }
                SelectObject(memory, previous);
                DeleteObject(bitmap);
            }
            if (memory) DeleteDC(memory);
            if (screen) ReleaseDC(nullptr, screen);
            CloseThemeData(theme);
        }
    }

    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    if (g_tabPageBrush) DeleteObject(g_tabPageBrush);
    g_tabPageColor = color;
    g_tabPageBrush = brush;
}

INT_PTR paintThemedDialogControl(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, g_tabPageColor);
    return (INT_PTR)(g_tabPageBrush ? g_tabPageBrush : GetSysColorBrush(COLOR_WINDOW));
}

bool isTabPageTextControl(int id) {
    switch (id) {
        case IDC_GRP_STATUS:
        case IDC_STATUS_TEXT:
        case IDC_GRP_TYPING:
        case IDC_RADIO_TELEX:
        case IDC_RADIO_VNI:
        case IDC_RADIO_TONE_MODERN:
        case IDC_RADIO_TONE_OLD:
        case IDC_GRP_SPELL:
        case IDC_CHECK_SPELLCHECK:
        case IDC_CHECK_LOCKCANCEL:
        case IDC_CHECK_RESTOREAFTERSPACE:
        case IDC_HELP_SPELLCHECK:
        case IDC_HELP_LOCKCANCEL:
        case IDC_HELP_RESTOREAFTERSPACE:
        case IDC_DESC_SPELLCHECK:
        case IDC_DESC_LOCKCANCEL:
        case IDC_DESC_RESTOREAFTERSPACE:
        case IDC_GRP_HOTKEYS:
        case IDC_CHECK_MASTER_HOTKEY:
        case IDC_LABEL_MASTERHK:
        case IDC_MASTER_CTRL:
        case IDC_MASTER_ALT:
        case IDC_MASTER_SHIFT:
        case IDC_MASTER_WIN:
        case IDC_CHECK_OVERRIDE_HOTKEY:
        case IDC_LABEL_OVERRIDE:
        case IDC_HELP_OVERRIDE_HOTKEY:
        case IDC_OVERRIDE_CTRL:
        case IDC_OVERRIDE_ALT:
        case IDC_OVERRIDE_SHIFT:
        case IDC_OVERRIDE_WIN:
        case IDC_GRP_OPTIONS:
        case IDC_CHECK_AUTOSTART:
        case IDC_CHECK_AUTOSTART_ADMIN:
        case IDC_CHECK_AUTOCOMPLETE:
        case IDC_HELP_AUTOCOMPLETE:
        case IDC_CHECK_EXCLUSIONON:
        case IDC_CHECK_REVERTBLUR:
        case IDC_CHECK_SOUND_GLOBAL:
        case IDC_CHECK_SOUND_EXCLUDED:
        case IDC_GRP_EXCLUDED:
        case IDC_CROSSHAIR_HINT:
        case IDC_EXCLUDED_NAME_LABEL:
        case IDC_GRP_GAMING:
        case IDC_CHECK_GAMING_TOGGLE:
        case IDC_CHECK_GAMING_TEMP:
        case IDC_LABEL_GAMING_TRIGGER:
        case IDC_GAMING_TRIGGER_WARN:
        case IDC_GAMING_ADMIN_WARN:
        case IDC_GAMING_ADMIN_ICON:
        case IDC_GAMING_PASTE_WARN:
        case IDC_CHECK_GAMING_SOUND:
        case IDC_CHECK_GAMING_OVERLAY:
        case IDC_LABEL_OVERLAY_POS:
        case IDC_HELP_GAMING_TOGGLE:
        case IDC_HELP_GAMING_TEMP:
        case IDC_HELP_GAMING_SOUND:
        case IDC_HELP_GAMING_OVERLAY:
        case IDC_HELP_GAMING_PASTE:
        case IDC_GRP_GAMING_APPS:
        case IDC_GAMING_NAME_LABEL:
        case IDC_GRP_STATS_TOGGLE:
        case IDC_CHECK_TYPINGSTATS:
        case IDC_HELP_TYPINGSTATS:
        case IDC_DESC_TYPINGSTATS:
        case IDC_GRP_STATS_SUMMARY:
        case IDC_STATS_SUMMARY:
        case IDC_GRP_STATS_WORDS:
            return true;
        default:
            return false;
    }
}

HICON loadAppIcon(int cx, int cy) {
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_VIETKI),
                             IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
}

void applyDialogIcon(HWND dlg) {
    if (!g_settingsIcon)
        g_settingsIcon = loadAppIcon(GetSystemMetrics(SM_CXICON),
                                     GetSystemMetrics(SM_CYICON));
    if (!g_settingsSmallIcon)
        g_settingsSmallIcon = loadAppIcon(GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON));
    if (g_settingsIcon)
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)g_settingsIcon);
    if (g_settingsSmallIcon)
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)g_settingsSmallIcon);
}

void addTooltip(HWND dlg, int controlId, const wchar_t* text) {
    // The control may live on a tab-page child dialog rather than on `dlg`
    // itself. GetDlgItem still finds it, but the tooltip tool has to be bound
    // to the control's *actual* parent window — otherwise TTF_SUBCLASS never
    // relays the hover and the tip silently never appears (the bug that hid
    // every tab-page tooltip, e.g. on the "Chơi game" tab).
    HWND ctl = GetDlgItem(dlg, controlId);
    if (!ctl) return;
    HWND host = GetParent(ctl);
    if (!host) host = dlg;
    if (!g_tooltip) {
        g_tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
                                    WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, dlg, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 320);
    }
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = host;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void closeHelp(); // fwd: defined with the popover, used on tab change

constexpr int kTabCount = 5;
constexpr int kStatsTabIndex = 4;

void refreshStatsDisplay(HWND dlg); // fwd: populated from typingStatsSnapshot()

void showTab(HWND dlg, int tab) {
    closeHelp(); // a tab change is a context break for the help popover (H.1)
    g_activeTab = tab;
    for (int i = 0; i < kTabCount; ++i) {
        if (g_tabPages[i]) ShowWindow(g_tabPages[i], i == tab ? SW_SHOW : SW_HIDE);
    }
    updateActionButtons(dlg);
    if (tab >= 0 && tab < kTabCount && g_tabPages[tab])
        SetWindowPos(g_tabPages[tab], HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // The stats can change any time (typing in other apps), so refresh the
    // numbers whenever the user comes back to look at them.
    if (tab == kStatsTabIndex) refreshStatsDisplay(dlg);
}

void initTabs(HWND dlg) {
    HWND tab = GetDlgItem(dlg, IDC_TAB);
    // The tab hosts the page child-dialogs (each WS_EX_CONTROLPARENT), and the
    // dialog itself is a control parent. The tab control sits between them, so
    // it must carry WS_EX_CONTROLPARENT too — otherwise the chain is broken and
    // GetNextDlgTabItem infinite-loops when focus must move (e.g. disabling the
    // focused "Xoá" button after a delete), hanging the window.
    SetWindowLongPtrW(tab, GWL_EXSTYLE,
                      GetWindowLongPtrW(tab, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(L"Cơ bản");
    TabCtrl_InsertItem(tab, 0, &item);
    item.pszText = const_cast<wchar_t*>(L"Phím tắt");
    TabCtrl_InsertItem(tab, 1, &item);
    item.pszText = const_cast<wchar_t*>(L"Hệ thống");
    TabCtrl_InsertItem(tab, 2, &item);
    item.pszText = const_cast<wchar_t*>(L"Chơi game");
    TabCtrl_InsertItem(tab, 3, &item);
    item.pszText = const_cast<wchar_t*>(L"Thống kê");
    TabCtrl_InsertItem(tab, 4, &item);

    RECT pageRect;
    GetClientRect(tab, &pageRect);
    TabCtrl_AdjustRect(tab, FALSE, &pageRect);
    const int pageIds[] = {
        IDD_SETTINGS_BASIC,
        IDD_SETTINGS_HOTKEYS,
        IDD_SETTINGS_SYSTEM,
        IDD_SETTINGS_GAMING,
        IDD_SETTINGS_STATS,
    };
    for (int i = 0; i < kTabCount; ++i) {
        g_tabPages[i] = CreateDialogParamW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(pageIds[i]), tab,
            pageProc, (LPARAM)dlg);
        if (g_tabPages[i]) {
            SetWindowPos(g_tabPages[i], HWND_TOP, pageRect.left, pageRect.top,
                         pageRect.right - pageRect.left,
                         pageRect.bottom - pageRect.top,
                         SWP_NOACTIVATE);
        }
    }

    TabCtrl_SetCurSel(tab, 0);
    showTab(dlg, 0);
}

void initExcludedList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_LIST_EXCLUDED);
    SetWindowTheme(list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                LVS_EX_LABELTIP);
    if (!g_listImages) {
        g_listImages = ImageList_Create(GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON),
                                        ILC_COLOR32 | ILC_MASK, 1, 1);
        HICON appIcon = LoadIconW(nullptr, IDI_APPLICATION);
        ImageList_AddIcon(g_listImages, appIcon);
    }
    ListView_SetImageList(list, g_listImages, LVSIL_SMALL);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Ứng dụng");
    col.cx = 178;
    ListView_InsertColumn(list, 0, &col);
}

int selectedExcludedIndex(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_LIST_EXCLUDED);
    return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

std::wstring toLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring trim(std::wstring s) {
    size_t first = 0;
    while (first < s.size() && iswspace(s[first])) ++first;
    size_t last = s.size();
    while (last > first && iswspace(s[last - 1])) --last;
    return s.substr(first, last - first);
}

std::wstring keyNameFromVk(BYTE vk) {
    if (vk == VK_SPACE) return L"Space";
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    return {};
}

void setHotkeyEditor(HWND dlg, int ctrlId, int altId, int shiftId, int winId, int keyId,
                     BYTE mods, BYTE vk) {
    CheckDlgButton(dlg, ctrlId, (mods & HOTKEYF_CONTROL) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, altId, (mods & HOTKEYF_ALT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, shiftId, (mods & HOTKEYF_SHIFT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, winId, (mods & VIETKI_HOTKEYF_WIN) ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(dlg, keyId, keyNameFromVk(vk).c_str());
}

std::wstring hotkeyFromEditor(HWND dlg, int ctrlId, int altId, int shiftId, int winId,
                              int keyId, const std::wstring& fallback) {
    std::wstring out;
    auto add = [&](const wchar_t* part) {
        if (!out.empty()) out += L"+";
        out += part;
    };
    if (IsDlgButtonChecked(dlg, ctrlId)) add(L"Ctrl");
    if (IsDlgButtonChecked(dlg, altId)) add(L"Alt");
    if (IsDlgButtonChecked(dlg, shiftId)) add(L"Shift");
    if (IsDlgButtonChecked(dlg, winId)) add(L"Win");

    wchar_t keyBuf[32] = {};
    GetDlgItemTextW(dlg, keyId, keyBuf, ARRAYSIZE(keyBuf));
    std::wstring key = trim(keyBuf);
    if (!key.empty()) {
        if (!out.empty()) out += L"+";
        out += key;
    }
    std::wstring norm = normalizeHotkeyString(out);
    return norm.empty() ? fallback : norm;
}

WORD hotkeyWordFromEditor(HWND dlg, int ctrlId, int altId, int shiftId, int winId,
                          int keyId) {
    std::wstring text = hotkeyFromEditor(dlg, ctrlId, altId, shiftId, winId, keyId, {});
    BYTE mods = 0, vk = 0;
    if (!parseHotkeyString(text, mods, vk) || vk == 0) return 0;
    return (WORD)(vk | (mods << 8));
}

// Phase 3 D.3 §9: a modern Task Dialog for a simple notice (OS-drawn icon and
// button), replacing MessageBox. Falls back to MessageBox if the Task Dialog API
// is unavailable (no v6 context).
void infoDialog(HWND owner, PCWSTR instr, PCWSTR content, PCWSTR icon) {
    if (FAILED(TaskDialog(owner, GetModuleHandleW(nullptr), L"VietKi", instr, content,
                          TDCBF_OK_BUTTON, icon, nullptr))) {
        std::wstring text = instr;
        if (content && *content) text += std::wstring(L"\n\n") + content;
        MessageBoxW(owner, text.c_str(), L"VietKi", MB_OK);
    }
}

bool draftContains(const std::wstring& exe) {
    for (const auto& e : g_draftExcluded)
        if (_wcsicmp(e.c_str(), exe.c_str()) == 0) return true;
    return false;
}

void updateActionButtons(HWND dlg) {
    int sel = selectedExcludedIndex(dlg);
    EnableWindow(GetDlgItem(dlg, IDC_BTN_REMOVE), sel >= 0);
    int gsel = ListView_GetNextItem(GetDlgItem(dlg, IDC_LIST_GAMING), -1, LVNI_SELECTED);
    EnableWindow(GetDlgItem(dlg, IDC_BTN_GAMING_REMOVE), gsel >= 0);
    ShowWindow(GetDlgItem(dlg, IDC_DIRTY_MARK), g_dirty ? SW_SHOW : SW_HIDE);
}

void markDirty(HWND dlg) {
    if (g_suppressDirty) return;
    g_dirty = true;
    updateActionButtons(dlg);
}

// Fill a name-entry edit box with a crosshair-picked exe name so the user can
// review/edit it, then press "Thêm tên đã nhập" to save — shared by both the
// excluded and gaming lists.
void fillPickedName(HWND dlg, int editId, const std::wstring& exe) {
    SetDlgItemTextW(dlg, editId, exe.c_str());
    SetFocus(GetDlgItem(dlg, editId));
    const wchar_t* which = nullptr;
    if (!exe.empty() && inEitherDraft(exe, &which))
        infoDialog(dlg, (exe + L" đã có trong " + which + L".").c_str(),
                   L"Mỗi app chỉ có thể nằm trong một danh sách.", TD_WARNING_ICON);
}

// --- Phase 5: gaming-tab helpers -------------------------------------------

bool gamingDraftContains(const std::wstring& exe) {
    for (const auto& e : g_draftGaming)
        if (_wcsicmp(e.c_str(), exe.c_str()) == 0) return true;
    return false;
}

bool gamingPasteDraftContains(const std::wstring& exe) {
    for (const auto& e : g_draftGamingPaste)
        if (_wcsicmp(e.c_str(), exe.c_str()) == 0) return true;
    return false;
}

void setGamingPasteDraft(const std::wstring& exe, bool enabled) {
    auto it = std::find_if(g_draftGamingPaste.begin(), g_draftGamingPaste.end(),
                           [&](const std::wstring& e) {
                               return _wcsicmp(e.c_str(), exe.c_str()) == 0;
                           });
    if (enabled) {
        if (it == g_draftGamingPaste.end()) g_draftGamingPaste.push_back(exe);
    } else if (it != g_draftGamingPaste.end()) {
        g_draftGamingPaste.erase(it);
    }
}

bool inEitherDraft(const std::wstring& exe, const wchar_t** whichList) {
    if (draftContains(exe)) {
        if (whichList) *whichList = L"danh sách loại trừ";
        return true;
    }
    if (gamingDraftContains(exe)) {
        if (whichList) *whichList = L"danh sách game";
        return true;
    }
    return false;
}

// Normalize a hand-typed app name to match how the runtime compares (a
// lowercase .exe basename). Adds ".exe" when the user omits an extension.
std::wstring normalizeExeName(std::wstring s) {
    s = toLower(trim(s));
    if (!s.empty() && s.find(L'.') == std::wstring::npos) s += L".exe";
    return s;
}

// Stage a hand-typed name into one of the lists, refusing cross-list duplicates.
void addTypedName(HWND dlg, int editId, std::vector<std::wstring>& target,
                  const wchar_t* emptyHint) {
    wchar_t buf[MAX_PATH] = {};
    GetDlgItemTextW(dlg, editId, buf, ARRAYSIZE(buf));
    std::wstring name = normalizeExeName(buf);
    if (name.empty()) {
        infoDialog(dlg, emptyHint, L"", TD_INFORMATION_ICON);
        return;
    }
    const wchar_t* which = nullptr;
    if (inEitherDraft(name, &which)) {
        infoDialog(dlg, (name + L" đã có trong " + which + L".").c_str(),
                   L"Mỗi app chỉ có thể nằm trong một danh sách.", TD_WARNING_ICON);
        return;
    }
    target.push_back(name);
    SetWindowTextW(GetDlgItem(dlg, editId), L"");
    markDirty(dlg);
}

void refreshGamingList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_LIST_GAMING);
    int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    g_refreshingGamingList = true;
    ListView_DeleteAllItems(list);
    for (int i = 0; i < (int)g_draftGaming.size(); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(L"");
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, i, 1, g_draftGaming[i].data());
        ListView_SetCheckState(list, i,
                               gamingPasteDraftContains(g_draftGaming[i]) ? TRUE
                                                                         : FALSE);
    }
    int count = ListView_GetItemCount(list);
    if (sel >= 0 && sel < count)
        ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    g_refreshingGamingList = false;
    updateActionButtons(dlg);
}

// A human-friendly name for a trigger binding, e.g. "]", "F8", "NumpadEnter".
std::wstring triggerKeyName(const TriggerBinding& t) {
    if (t.vk == 0) return L"(chưa đặt)";
    wchar_t buf[64] = {};
    LONG lparam = (LONG)(t.scanCode << 16);
    if (GetKeyNameTextW(lparam, buf, ARRAYSIZE(buf)) > 0 && buf[0]) return buf;
    UINT ch = MapVirtualKeyW(t.vk, MAPVK_VK_TO_CHAR) & 0x7fff;
    if (ch) { wchar_t c[2] = {(wchar_t)ch, 0}; return c; }
    wchar_t hx[16];
    wsprintfW(hx, L"VK 0x%02X", t.vk);
    return hx;
}

// Phase 5 C.3: keys that usually already have a function in PC games.
bool isCommonGameTriggerKey(UINT vk) {
    switch (vk) {
        case VK_OEM_3: case VK_TAB: case VK_RETURN: case VK_ESCAPE:
        case VK_SPACE: case VK_OEM_2: case 'T': case 'Y': case 'U': case 'V':
            return true;
        default:
            break;
    }
    return vk >= VK_F1 && vk <= VK_F12;
}

void updateTriggerWarning(HWND dlg) {
    const wchar_t* warn = L"";
    if (isCommonGameTriggerKey(g_draftTrigger.vk))
        warn = L"⚠ Phím này thường có chức năng trong game; hãy cân nhắc phím khác.";
    SetDlgItemTextW(dlg, IDC_GAMING_TRIGGER_WARN, warn);
}

// Enable the trigger and notification controls only under the temporary-trigger
// policy, and disable the V-/V+ option while temporary trigger is on (I).
void updateGamingEnableState(HWND dlg) {
    bool temp = IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TEMP) != 0;
    bool toggle = IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TOGGLE) != 0;
    bool anyPolicy = temp || toggle;
    EnableWindow(GetDlgItem(dlg, IDC_CHECK_GAMING_TOGGLE), !temp);
    // Trigger + the auto-switch beep only apply to the temporary-trigger policy.
    const int tempOnly[] = {IDC_LABEL_GAMING_TRIGGER, IDC_GAMING_TRIGGER_KEY,
                            IDC_GAMING_TRIGGER_WARN, IDC_CHECK_GAMING_SOUND};
    for (int id : tempOnly) EnableWindow(GetDlgItem(dlg, id), temp);
    // The overlay (G+ indicator) applies to either gaming policy; the corner
    // picker follows the overlay checkbox.
    EnableWindow(GetDlgItem(dlg, IDC_CHECK_GAMING_OVERLAY), anyPolicy);
    bool overlayOn =
        anyPolicy && IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_OVERLAY) != 0;
    EnableWindow(GetDlgItem(dlg, IDC_LABEL_OVERLAY_POS), overlayOn);
    EnableWindow(GetDlgItem(dlg, IDC_COMBO_OVERLAY_CORNER), overlayOn);
}

// Draw (or, drawn a second time, erase) a thick XOR frame around a window.
void drawFrame(HWND h) {
    if (!h) return;
    RECT rc;
    if (!GetWindowRect(h, &rc)) return;
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    HDC dc = GetWindowDC(h);
    if (!dc) return;
    int prevRop = SetROP2(dc, R2_NOT);
    HPEN pen = CreatePen(PS_INSIDEFRAME, 3, RGB(0, 0, 0));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, w, ht);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SetROP2(dc, prevRop);
    DeleteObject(pen);
    ReleaseDC(h, dc);
}

std::wstring processExeName(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        std::wstring full(path, len);
        size_t slash = full.find_last_of(L"\\/");
        name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(proc);
    return toLower(name);
}

bool isUnselectable(HWND h) {
    if (!h) return true;
    if (h == state().settingsWindow || h == state().messageWindow) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0 ||
           lstrcmpW(cls, L"Shell_TrayWnd") == 0;
}

void finishPick(HWND crosshair) {
    if (g_highlight) {
        drawFrame(g_highlight);
        g_highlight = nullptr;
    }
    if (GetCapture() == crosshair) ReleaseCapture();
    g_picking = false;
    SetWindowTextW(crosshair,
                   GetDlgCtrlID(crosshair) == IDC_GAMING_CROSSHAIR
                       ? L"Chọn cửa sổ"
                       : L"⌖");
}

LRESULT CALLBACK crosshairProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                               UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_picking = true;
            g_highlight = nullptr;
            g_pickTarget = GetDlgCtrlID(hwnd); // excluded vs gaming crosshair
            SetWindowTextW(hwnd, L"⌖");
            SetCapture(hwnd);
            if (!g_crossCursor) g_crossCursor = LoadCursorW(nullptr, IDC_CROSS);
            SetCursor(g_crossCursor);
            return 0;
        case WM_MOUSEMOVE: {
            if (!g_picking) break;
            SetCursor(g_crossCursor);
            POINT pt;
            GetCursorPos(&pt);
            HWND h = GetAncestor(WindowFromPoint(pt), GA_ROOT);
            if (h != g_highlight) {
                if (g_highlight) drawFrame(g_highlight);
                g_highlight = (h && !isUnselectable(h)) ? h : nullptr;
                if (g_highlight) drawFrame(g_highlight);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!g_picking) break;
            POINT pt;
            GetCursorPos(&pt);
            finishPick(hwnd);
            HWND h = GetAncestor(WindowFromPoint(pt), GA_ROOT);
            HWND dlg = state().settingsWindow;
            bool gaming = (g_pickTarget == IDC_GAMING_CROSSHAIR);
            int nameEdit = gaming ? IDC_GAMING_NAME : IDC_EXCLUDED_NAME;
            if (isUnselectable(h)) {
                infoDialog(dlg, L"Không chọn được cửa sổ này.", L"",
                           TD_WARNING_ICON);
                return 0;
            }
            std::wstring exe = processExeName(h);
            if (exe.empty()) {
                MessageBoxW(dlg,
                            L"Không đọc được tiến trình của cửa sổ này. Hãy chạy "
                            L"VietKi bằng quyền Admin để chọn app này.",
                            L"VietKi", MB_ICONWARNING);
                return 0;
            }
            // Fill the name box; the user reviews/edits it, then presses "Thêm
            // tên đã nhập" to save — same flow for both lists.
            fillPickedName(dlg, nameEdit, exe);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (g_picking) finishPick(hwnd);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// --- Phase 4 H: custom help popover ----------------------------------------
//
// The two "word handling" options need more than a one-line checkbox can carry,
// and H.1 forbids the default Win32 tooltip. Instead each option has an ⓘ icon
// that opens this borderless, theme-aware popover (rounded corners + drop
// shadow via DWM, a bold title, wrapped body text and monospace examples). It
// opens on hover (after a short delay) or click, flips to stay on screen, and
// closes on Esc, focus loss, a tab change, or when the pointer leaves it.

struct HelpLine {
    const wchar_t* text;
    bool mono; // render in a monospace font (the "Gõ → Kết quả" examples)
    bool gap;  // extra space before this line (paragraph break)
};
struct HelpDoc {
    const wchar_t* title;
    const HelpLine* lines;
    int count;
};

const HelpLine kSpellLines[] = {
    {L"VietKi kiểm tra cấu trúc âm tiết, không dùng từ điển. Nếu một chuỗi chắc "
     L"chắn không thể là âm tiết tiếng Việt, VietKi trả lại đúng các phím đã gõ và "
     L"giữ phần còn lại của từ ở dạng thường.", false, false},
    {L"Ví dụ:", false, true},
    {L"override  →  override", true, false},
    {L"Một số chuỗi vẫn mơ hồ. “test” là cách gõ Telex hợp lệ của “tét”; để gõ "
     L"từ tiếng Anh, hãy gõ “tesst” để chủ động hủy dấu.", false, true},
};
const HelpLine kLockLines[] = {
    {L"Khi bạn gõ lại cùng một phím dấu để hủy, VietKi hiểu rằng bạn muốn gõ từ ở "
     L"dạng thường. Các phím còn lại sẽ không tiếp tục tạo dấu cho đến khi kết "
     L"thúc từ.", false, false},
    {L"Bật:", false, true},
    {L"offf   →  off", true, false},
    {L"tesst  →  test", true, false},
    {L"Tắt: chỉ lần hủy hiện tại có hiệu lực; phím sau có thể tạo dấu lại, nên "
     L"“offf” → “òf”. Hữu ích khi gõ chuỗi đặc biệt như “đượợợợợc”.", false, true},
};
const HelpDoc kSpellDoc = {L"Kiểm tra và khôi phục từ không phải tiếng Việt",
                           kSpellLines, ARRAYSIZE(kSpellLines)};
const HelpDoc kLockDoc = {L"Giữ nguyên phần còn lại của từ sau khi hủy dấu",
                          kLockLines, ARRAYSIZE(kLockLines)};

// Phase 6: restore the word Space just committed if Backspace follows right away.
const HelpLine kRestoreAfterSpaceLines[] = {
    {L"Khi bạn nhấn dấu cách, VietKi chốt từ vừa gõ lại. Nếu bấm Backspace ngay "
     L"sau đó (không có phím nào xen giữa), VietKi khôi phục lại từ đó để bạn gõ "
     L"tiếp dấu thay vì phải gõ lại cả từ.", false, false},
    {L"Ví dụ:", false, true},
    {L"nguyen [cách][xóa]x  →  nguyễn", true, false},
    {L"Chỉ khôi phục cho đúng 1 lần dấu cách + 1 lần Backspace liên tiếp. Gõ tiếp, "
     L"click chuột, đổi cửa sổ hoặc di chuyển con trỏ sẽ hủy khôi phục.",
     false, true},
    {L"Tắt tùy chọn này nếu bạn cần dấu cách luôn xóa hẳn từ đang gõ (vd sau khi "
     L"dán nhầm nội dung và muốn Backspace xóa từng ký tự bình thường).",
     false, true},
};
const HelpDoc kRestoreAfterSpaceDoc = {L"Tiếp tục sửa từ sau khi xóa dấu cách",
                                       kRestoreAfterSpaceLines,
                                       ARRAYSIZE(kRestoreAfterSpaceLines)};

// Phase 5.1: the gaming-tab ⓘ icons reuse the same popover as the Basic tab
// (consistent help UI; see docs/UI_GUIDELINES.md). One HelpDoc per icon.
const HelpLine kGamingToggleLines[] = {
    {L"Hoạt động tương tự như Danh sách loại trừ: game mặc định gõ tiếng Anh "
     L"(V-).", false, false},
    {L"Dùng phím tắt đổi V-/V+ để bật tiếng Việt cho game đang mở.", false, true},
};
const HelpLine kGamingTempLines[] = {
    {L"Game nhận phím thẳng (tiếng Anh) để không loạn điều khiển.", false, false},
    {L"Bấm phím bắt đầu (mặc định ']') rồi gõ để bật tiếng Việt tạm thời; tự tắt "
     L"khi gửi chat, click chuột hoặc rời game.", false, true},
    {L"Ô 'Phím' đặt phím bắt đầu — nhấn vào ô rồi bấm phím; bấm hai lần để gửi "
     L"chính phím đó vào game.", false, true},
};
const HelpLine kGamingSoundLines[] = {
    {L"Phát một tiếng 'bíp' khi VietKi tự bật/tắt tiếng Việt trong game (âm khác "
     L"nhau cho bật và tắt).", false, false},
    {L"Hữu ích khi chơi fullscreen — nơi thông báo dạng cửa sổ không hiện được.",
     false, true},
};
const HelpLine kGamingOverlayLines[] = {
    {L"Hiện một ô mờ 'VN' ở góc màn hình khi đang gõ tiếng Việt trong game, để "
     L"luôn biết chắc đang ở chế độ tiếng Việt.", false, false},
    {L"Hiện trên game borderless/windowed-fullscreen (vd Genshin), không hiện "
     L"trên exclusive-fullscreen.", false, true},
    {L"Chọn 1 trong 4 góc ở ô 'Vị trí'.", false, true},
};
const HelpDoc kGamingToggleDoc = {L"Bật đổi V-/V+ cho ứng dụng game",
                                  kGamingToggleLines,
                                  ARRAYSIZE(kGamingToggleLines)};
const HelpDoc kGamingTempDoc = {L"Chế độ gõ tiếng Việt tạm thời", kGamingTempLines,
                                ARRAYSIZE(kGamingTempLines)};
const HelpDoc kGamingSoundDoc = {L"Bật âm thanh khi VietKi chuyển chế độ",
                                 kGamingSoundLines, ARRAYSIZE(kGamingSoundLines)};
const HelpDoc kGamingOverlayDoc = {L"Hiện overlay khi gõ tiếng Việt",
                                   kGamingOverlayLines,
                                   ARRAYSIZE(kGamingOverlayLines)};
const HelpLine kGamingPasteLines[] = {
    {L"Một số game nhận ký tự Unicode gõ trực tiếp thành dấu '?'. Khi bật tùy "
     L"chọn này cho một game, VietKi sẽ đưa phần chữ đã ghép vào clipboard và dán "
     L"bằng Ctrl+V để game nhận đúng tiếng Việt.", false, false},
    {L"VietKi lưu clipboard hiện tại một lần khi bắt đầu phiên gõ và khôi phục khi "
     L"phiên kết thúc. Nội dung tạm được đánh dấu để hạn chế xuất hiện trong "
     L"lịch sử clipboard và đồng bộ cloud.", false, true},
    {L"Chỉ bật cho game thực sự hiển thị sai chữ. Ctrl+V có thể trùng với phím "
     L"điều khiển hoặc phím tắt riêng của game.", false, true},
};
const HelpDoc kGamingPasteDoc = {L"Dán Unicode theo từng game", kGamingPasteLines,
                                 ARRAYSIZE(kGamingPasteLines)};
const HelpLine kOverrideHotkeyLines[] = {
    {L"Tạm thời chuyển sang gõ tiếng Việt trong app loại trừ, tính năng chỉ "
     L"enable khi bật chế độ gõ Tiếng Việt và đang trong ứng dụng loại trừ.",
     false, false},
};
const HelpDoc kOverrideHotkeyDoc = {L"Bật V+ cho App loại trừ", kOverrideHotkeyLines,
                                    ARRAYSIZE(kOverrideHotkeyLines)};
const HelpLine kAutocompleteLines[] = {
    {L"Ở các ô có gợi ý tự bồi (thanh địa chỉ trình duyệt, ô tìm kiếm…), khi VKey "
     L"đặt lại dấu bằng cách xóa rồi gõ lại, đoạn gợi ý có thể chen vào làm dấu "
     L"nhảy sai vị trí.", false, false},
    {L"Khi bật, thay vì gửi Backspace, VKey bôi đen đúng số ký tự cần sửa "
     L"(Shift+Trái) rồi gõ đè — nuốt luôn đoạn gợi ý nên dấu không bị nhảy.",
     false, true},
    {L"Chỉ áp dụng cho nhóm app trong cấu hình (mặc định: chrome.exe, msedge.exe, "
     L"firefox.exe).", false, true},
    {L"Hãy TẮT nếu thấy ký tự bị nhân đôi — gõ “mes” ra “mée” thay vì “mé”:",
     false, true},
    {L"mes  →  mée", true, false},
    {L"Một số trình duyệt (vd Chrome) xử lý sự kiện phím bất đồng bộ, khiến thao "
     L"tác bôi đen chưa kịp có hiệu lực trước khi gõ đè nên dư ra một ký tự. Lỗi "
     L"này tùy máy (CPU/bản Windows) nên có máy bị, có máy không. Gặp thì tắt tùy "
     L"chọn để VKey quay về dùng Backspace.", false, true},
};
const HelpDoc kAutocompleteDoc = {L"Sửa lỗi nhảy dấu ở ô gợi ý (autocomplete)",
                                  kAutocompleteLines,
                                  ARRAYSIZE(kAutocompleteLines)};

// Phase 6 section 7: local-only typing stats.
const HelpLine kTypingStatsLines[] = {
    {L"Đếm số từ đã gõ, ước tính tốc độ gõ (WPM) và tỷ lệ dùng Backspace, để bạn "
     L"tự theo dõi thói quen gõ của mình.", false, false},
    {L"Dữ liệu chỉ lưu trong tệp typing_stats.dat cạnh VietKi.exe. Không gửi đi "
     L"đâu, không kèm trong config.ini, không dùng cho quảng cáo.", false, true},
    {L"Bấm 'Xoá toàn bộ dữ liệu' để xoá sạch bất cứ lúc nào.", false, true},
};
const HelpDoc kTypingStatsDoc = {L"Thống kê gõ phím", kTypingStatsLines,
                                 ARRAYSIZE(kTypingStatsLines)};

// Map an ⓘ icon control to its help document. Returns nullptr if the id is not a
// help icon. Every ⓘ icon must be listed here (UI_GUIDELINES.md).
const HelpDoc* helpDocFor(int iconId) {
    switch (iconId) {
        case IDC_HELP_SPELLCHECK: return &kSpellDoc;
        case IDC_HELP_LOCKCANCEL: return &kLockDoc;
        case IDC_HELP_RESTOREAFTERSPACE: return &kRestoreAfterSpaceDoc;
        case IDC_HELP_GAMING_TOGGLE: return &kGamingToggleDoc;
        case IDC_HELP_GAMING_TEMP: return &kGamingTempDoc;
        case IDC_HELP_GAMING_SOUND: return &kGamingSoundDoc;
        case IDC_HELP_GAMING_OVERLAY: return &kGamingOverlayDoc;
        case IDC_HELP_GAMING_PASTE: return &kGamingPasteDoc;
        case IDC_HELP_OVERRIDE_HOTKEY: return &kOverrideHotkeyDoc;
        case IDC_HELP_AUTOCOMPLETE: return &kAutocompleteDoc;
        case IDC_HELP_TYPINGSTATS: return &kTypingStatsDoc;
        default: return nullptr;
    }
}

HWND g_help = nullptr;          // the open popover, or null
const HelpDoc* g_helpDoc = nullptr;
bool g_helpSticky = false;      // opened by click/keyboard (takes focus)
RECT g_helpIconRect = {};       // screen rect of the anchoring ⓘ icon
HFONT g_helpTitleFont = nullptr;
HFONT g_helpBodyFont = nullptr;
HFONT g_helpMonoFont = nullptr;
HFONT g_helpIconFont = nullptr;
HFONT g_adminStatusIconFont = nullptr;
bool g_runningElevated = false;

constexpr UINT_PTR kHelpHoverTimer = 4001; // on the dialog: delayed hover open
constexpr UINT_PTR kHelpWatchTimer = 4002; // on the popover: pointer-left check
constexpr UINT kHelpHoverDelayMs = 180;
constexpr DWORD kHelpFadeDurationMs = 90;

bool isDarkTheme() {
    DWORD val = 1, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                     L"Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &val,
                     &sz) != ERROR_SUCCESS)
        return false;
    return val == 0;
}

void ensureHelpFonts(int dpi) {
    if (g_helpTitleFont) return;
    auto px = [dpi](int p) { return -MulDiv(p, dpi, 72); };
    g_helpTitleFont = CreateFontW(px(11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                  L"Segoe UI");
    g_helpBodyFont = CreateFontW(px(9), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                 L"Segoe UI");
    g_helpMonoFont = CreateFontW(px(9), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                 L"Consolas");
}

void destroyHelpFonts() {
    if (g_helpTitleFont) { DeleteObject(g_helpTitleFont); g_helpTitleFont = nullptr; }
    if (g_helpBodyFont) { DeleteObject(g_helpBodyFont); g_helpBodyFont = nullptr; }
    if (g_helpMonoFont) { DeleteObject(g_helpMonoFont); g_helpMonoFont = nullptr; }
    if (g_helpIconFont) { DeleteObject(g_helpIconFont); g_helpIconFont = nullptr; }
    if (g_adminStatusIconFont) {
        DeleteObject(g_adminStatusIconFont);
        g_adminStatusIconFont = nullptr;
    }
}

void applyHelpIconFont(HWND dlg) {
    if (!g_helpIconFont) {
        int dpi = GetDpiForWindow(dlg);
        g_helpIconFont =
            CreateFontW(-MulDiv(13, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    if (!g_helpIconFont) return;
    for (int helpId : {IDC_HELP_SPELLCHECK, IDC_HELP_LOCKCANCEL,
                       IDC_HELP_RESTOREAFTERSPACE, IDC_HELP_TYPINGSTATS,
                       IDC_HELP_OVERRIDE_HOTKEY, IDC_HELP_AUTOCOMPLETE,
                       IDC_HELP_GAMING_TOGGLE, IDC_HELP_GAMING_TEMP,
                       IDC_HELP_GAMING_SOUND, IDC_HELP_GAMING_OVERLAY,
                       IDC_HELP_GAMING_PASTE}) {
        if (HWND icon = GetDlgItem(dlg, helpId))
            SendMessageW(icon, WM_SETFONT, (WPARAM)g_helpIconFont, TRUE);
    }
}

bool isCurrentProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                            &size) &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

void updateAdminStatus(HWND dlg) {
    g_runningElevated = isCurrentProcessElevated();
    SetDlgItemTextW(dlg, IDC_GAMING_ADMIN_ICON,
                    g_runningElevated ? L"✓" : L"⚠");
    SetDlgItemTextW(
        dlg, IDC_GAMING_ADMIN_WARN,
        g_runningElevated
            ? L"VietKi đang chạy với quyền Admin."
            : L"Game chạy quyền Admin: hãy chạy VietKi bằng 'Run as administrator'.");

    if (!g_adminStatusIconFont) {
        int dpi = GetDpiForWindow(dlg);
        g_adminStatusIconFont =
            CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    if (g_adminStatusIconFont) {
        SendMessageW(GetDlgItem(dlg, IDC_GAMING_ADMIN_ICON), WM_SETFONT,
                     (WPARAM)g_adminStatusIconFont, TRUE);
    }
}

// "Chạy quyền Administrator" only applies when autostart is on, so grey it out
// (and clear it) when "Khởi động cùng Windows" is unchecked.
void updateAutostartAdminEnable(HWND dlg) {
    bool on = IsDlgButtonChecked(dlg, IDC_CHECK_AUTOSTART) != 0;
    EnableWindow(GetDlgItem(dlg, IDC_CHECK_AUTOSTART_ADMIN), on);
    if (!on) CheckDlgButton(dlg, IDC_CHECK_AUTOSTART_ADMIN, BST_UNCHECKED);
}

int measureLine(HDC dc, HFONT font, const wchar_t* text, int width) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = {0, 0, width, 0};
    DrawTextW(dc, text, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

void closeHelp() {
    if (g_help) {
        HWND h = g_help;
        g_help = nullptr; // guard re-entry from WM_DESTROY
        DestroyWindow(h);
    }
    g_helpDoc = nullptr;
}

LRESULT CALLBACK helpWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            bool dark = isDarkTheme();
            COLORREF bg = dark ? RGB(43, 43, 43) : RGB(255, 255, 255);
            COLORREF border = dark ? RGB(78, 78, 78) : RGB(214, 214, 214);
            COLORREF titleCol = dark ? RGB(240, 240, 240) : RGB(22, 22, 22);
            COLORREF bodyCol = dark ? RGB(205, 205, 205) : RGB(64, 64, 64);
            COLORREF monoCol = dark ? RGB(120, 200, 255) : RGB(0, 92, 175);
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dc, &rc, bgBrush);
            DeleteObject(bgBrush);
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(pen);

            SetBkMode(dc, TRANSPARENT);
            int dpi = GetDpiForWindow(hwnd);
            int pad = MulDiv(16, dpi, 96);
            int contentW = rc.right - rc.left - 2 * pad;
            int y = pad;
            // Title (semibold).
            {
                HGDIOBJ of = SelectObject(dc, g_helpTitleFont);
                SetTextColor(dc, titleCol);
                RECT tr = {pad, y, pad + contentW, y};
                DrawTextW(dc, g_helpDoc->title, -1, &tr,
                          DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
                int h = tr.bottom - tr.top;
                tr = {pad, y, pad + contentW, y + h};
                DrawTextW(dc, g_helpDoc->title, -1, &tr,
                          DT_WORDBREAK | DT_NOPREFIX);
                y += h + MulDiv(10, dpi, 96);
                SelectObject(dc, of);
            }
            for (int i = 0; i < g_helpDoc->count; ++i) {
                const HelpLine& ln = g_helpDoc->lines[i];
                if (ln.gap) y += MulDiv(8, dpi, 96);
                HFONT f = ln.mono ? g_helpMonoFont : g_helpBodyFont;
                HGDIOBJ of = SelectObject(dc, f);
                SetTextColor(dc, ln.mono ? monoCol : bodyCol);
                int h = measureLine(dc, f, ln.text, contentW);
                RECT lr = {pad, y, pad + contentW, y + h};
                DrawTextW(dc, ln.text, -1, &lr, DT_WORDBREAK | DT_NOPREFIX);
                y += h + MulDiv(4, dpi, 96);
                SelectObject(dc, of);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kHelpWatchTimer) {
                // Hover mode: close once the pointer is over neither the icon
                // nor this popover.
                POINT pt;
                GetCursorPos(&pt);
                RECT pr;
                GetWindowRect(hwnd, &pr);
                if (!PtInRect(&pr, pt) && !PtInRect(&g_helpIconRect, pt))
                    closeHelp();
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { closeHelp(); return 0; }
            break;
        case WM_ACTIVATE:
            // Sticky mode: clicking/tabbing away deactivates us -> close.
            if (g_helpSticky && LOWORD(wParam) == WA_INACTIVE) closeHelp();
            return 0;
        case WM_DESTROY:
            if (g_help == hwnd) g_help = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerHelpClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSW wc = {};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = helpWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VietKiHelpPopover";
    RegisterClassW(&wc);
    done = true;
}

void showHelp(HWND dlg, int iconId, const HelpDoc& doc, bool sticky) {
    closeHelp();
    registerHelpClass();
    g_helpDoc = &doc;
    g_helpSticky = sticky;

    HWND icon = GetDlgItem(dlg, iconId);
    GetWindowRect(icon, &g_helpIconRect);

    int dpi = GetDpiForWindow(dlg);
    ensureHelpFonts(dpi);
    int popW = MulDiv(348, dpi, 96);
    int pad = MulDiv(16, dpi, 96);
    int contentW = popW - 2 * pad;

    // Measure the required height with a scratch DC.
    HDC dc = GetDC(dlg);
    int y = pad;
    y += measureLine(dc, g_helpTitleFont, doc.title, contentW) + MulDiv(10, dpi, 96);
    for (int i = 0; i < doc.count; ++i) {
        const HelpLine& ln = doc.lines[i];
        if (ln.gap) y += MulDiv(8, dpi, 96);
        y += measureLine(dc, ln.mono ? g_helpMonoFont : g_helpBodyFont, ln.text,
                         contentW) +
             MulDiv(4, dpi, 96);
    }
    ReleaseDC(dlg, dc);
    int popH = y + pad;

    // Position: to the right of the icon, flipping/clamping to the work area so
    // it never covers the icon or runs off screen (H.1).
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfo(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
    int gap = MulDiv(8, dpi, 96);
    int x = g_helpIconRect.right + gap;
    if (x + popW > mi.rcWork.right) x = g_helpIconRect.left - gap - popW;
    if (x < mi.rcWork.left) x = mi.rcWork.left + gap;
    int top = g_helpIconRect.top;
    if (top + popH > mi.rcWork.bottom) top = mi.rcWork.bottom - popH - gap;
    if (top < mi.rcWork.top) top = mi.rcWork.top + gap;

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (!sticky) exStyle |= WS_EX_NOACTIVATE;
    g_help = CreateWindowExW(exStyle, L"VietKiHelpPopover", L"", WS_POPUP, x, top,
                             popW, popH, dlg, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    if (!g_help) { g_helpDoc = nullptr; return; }

    // Rounded corners (Win11). Harmless no-op on older systems.
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_help, DWMWA_WINDOW_CORNER_PREFERENCE, &pref,
                          sizeof(pref));

    AnimateWindow(g_help, kHelpFadeDurationMs, AW_BLEND);
    if (sticky) {
        SetForegroundWindow(g_help);
        SetFocus(g_help);
    } else {
        SetTimer(g_help, kHelpWatchTimer, 150, nullptr);
    }
}

// Subclass for the ⓘ help icons: hover (delayed) and click both open the
// popover; the dialog owns the hover delay timer.
LRESULT CALLBACK helpIconProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                              UINT_PTR, DWORD_PTR refData) {
    int iconId = (int)refData;
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!g_help) {
                HWND dlg = state().settingsWindow;
                // Re-arm the delay; it fires when the pointer settles (H.2).
                SetTimer(dlg, kHelpHoverTimer, kHelpHoverDelayMs, nullptr);
                SetWindowLongPtrW(dlg, GWLP_USERDATA, iconId);
            }
            break;
        case WM_LBUTTONUP: {
            HWND dlg = state().settingsWindow;
            KillTimer(dlg, kHelpHoverTimer);
            if (const HelpDoc* doc = helpDocFor(iconId))
                showHelp(dlg, iconId, *doc, true);
            return 0;
        }
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Phase 5 C.2: the trigger edit captures the next physical key press and stores
// it by virtual key + scan code, displaying a friendly name. It is read-only so
// the captured key is the only thing ever shown.
LRESULT CALLBACK triggerEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS; // grab Tab/arrows/Enter too while focused
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            UINT vk = (UINT)wParam;
            switch (vk) { // a bare modifier is not a usable trigger
                case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                case VK_MENU: case VK_LMENU: case VK_RMENU:
                case VK_LWIN: case VK_RWIN:
                    return 0;
                default: break;
            }
            UINT scan = (UINT)((lParam >> 16) & 0xFF);
            g_draftTrigger.vk = vk;
            g_draftTrigger.scanCode = scan ? scan : MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            g_draftTrigger.modifiers = 0;
            HWND dlg = state().settingsWindow;
            SetWindowTextW(hwnd, triggerKeyName(g_draftTrigger).c_str());
            updateTriggerWarning(dlg);
            markDirty(dlg);
            return 0;
        }
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0; // swallow so the read-only edit only shows our name
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void initGamingList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_LIST_GAMING);
    SetWindowTheme(list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Dán Unicode");
    col.cx = 92;
    ListView_InsertColumn(list, 0, &col);
    col.pszText = const_cast<wchar_t*>(L"Game");
    col.cx = 190;
    ListView_InsertColumn(list, 1, &col);
}

// Phase 6 section 7: the "Thống kê" tab's word-frequency list.
void initStatsList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_LIST_TOPWORDS);
    SetWindowTheme(list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Từ");
    col.cx = 200;
    ListView_InsertColumn(list, 0, &col);
    col.pszText = const_cast<wchar_t*>(L"Số lần");
    col.cx = 80;
    ListView_InsertColumn(list, 1, &col);
}

// Pull the latest counters and repaint the summary text + top-words list.
// Read-only: this never feeds back into AppConfig, so it needs no draft/dirty
// tracking the way the excluded/gaming lists do.
void refreshStatsDisplay(HWND dlg) {
    TypingStatsSnapshot s = typingStatsSnapshot(50);

    wchar_t summary[256];
    swprintf_s(summary, L"Tổng số từ đã gõ: %lld\nTốc độ gõ trung bình: %.0f WPM\n"
                        L"Tỷ lệ dùng Backspace: %.1f%%",
               s.totalWords, s.wpm, s.backspaceRatioPct);
    SetDlgItemTextW(dlg, IDC_STATS_SUMMARY, summary);

    HWND list = GetDlgItem(dlg, IDC_LIST_TOPWORDS);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < s.topWords.size(); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.pszText = const_cast<wchar_t*>(s.topWords[i].word.c_str());
        int row = ListView_InsertItem(list, &item);
        wchar_t countText[32];
        swprintf_s(countText, L"%lld", s.topWords[i].count);
        ListView_SetItemText(list, row, 1, countText);
    }
}

// --- dialog population -----------------------------------------------------

void populate(HWND dlg) {
    g_suppressDirty = true;
    const AppConfig& c = state().config;
    g_draftExcluded = c.excludedProcesses;
    g_dirty = false;

    CheckRadioButton(dlg, IDC_RADIO_TELEX, IDC_RADIO_VNI,
                     c.method == Method::VNI ? IDC_RADIO_VNI : IDC_RADIO_TELEX);
    CheckRadioButton(dlg, IDC_RADIO_TONE_MODERN, IDC_RADIO_TONE_OLD,
                     c.tone == TonePlacement::Old ? IDC_RADIO_TONE_OLD
                                                  : IDC_RADIO_TONE_MODERN);

    BYTE mods = 0, vk = 0;
    parseHotkeyString(c.hotkey, mods, vk);
    setHotkeyEditor(dlg, IDC_MASTER_CTRL, IDC_MASTER_ALT, IDC_MASTER_SHIFT,
                    IDC_MASTER_WIN, IDC_MASTER_KEY, mods, vk);
    setHotkeyEditor(dlg, IDC_OVERRIDE_CTRL, IDC_OVERRIDE_ALT, IDC_OVERRIDE_SHIFT,
                    IDC_OVERRIDE_WIN, IDC_OVERRIDE_KEY,
                    HIBYTE(c.toggleForCurrentAppHotkey),
                    LOBYTE(c.toggleForCurrentAppHotkey));
    CheckDlgButton(dlg, IDC_CHECK_MASTER_HOTKEY,
                   c.toggleVietnameseHotkeyEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_OVERRIDE_HOTKEY,
                   c.toggleForCurrentAppHotkeyEnabled ? BST_CHECKED : BST_UNCHECKED);

    CheckDlgButton(dlg, IDC_CHECK_AUTOSTART, c.autostart ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_AUTOSTART_ADMIN,
                   c.autostartAdmin ? BST_CHECKED : BST_UNCHECKED);
    updateAutostartAdminEnable(dlg);
    CheckDlgButton(dlg, IDC_CHECK_AUTOCOMPLETE,
                   c.autocompleteFix ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_EXCLUSIONON,
                   c.exclusionFeatureOn ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_REVERTBLUR,
                   c.revertOverrideOnBlur ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_SPELLCHECK,
                   c.spellCheck ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_LOCKCANCEL,
                   c.lockWordAfterCancel ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_RESTOREAFTERSPACE,
                   c.restoreAfterSpace ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_TYPINGSTATS,
                   c.typingStats ? BST_CHECKED : BST_UNCHECKED);
    refreshStatsDisplay(dlg);
    CheckDlgButton(dlg, IDC_CHECK_SOUND_GLOBAL,
                   c.soundOnGlobalToggle ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_SOUND_EXCLUDED,
                   c.soundOnExcludedToggle ? BST_CHECKED : BST_UNCHECKED);

    // Phase 5: gaming tab.
    g_draftGaming = c.gamingProcesses;
    g_draftGamingPaste = c.gamingPasteProcesses;
    g_draftTrigger = c.gamingTrigger;
    CheckDlgButton(dlg, IDC_CHECK_GAMING_TOGGLE,
                   c.gamingPolicy == GamingPolicy::ToggleForCurrentApp ? BST_CHECKED
                                                                       : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_GAMING_TEMP,
                   c.gamingPolicy == GamingPolicy::TemporaryTrigger ? BST_CHECKED
                                                                    : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_GAMING_SOUND,
                   c.soundOnGamingModeSwitch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHECK_GAMING_OVERLAY,
                   c.gamingOverlayEnabled ? BST_CHECKED : BST_UNCHECKED);
    HWND corner = GetDlgItem(dlg, IDC_COMBO_OVERLAY_CORNER);
    SendMessageW(corner, CB_RESETCONTENT, 0, 0);
    SendMessageW(corner, CB_ADDSTRING, 0, (LPARAM)L"Trên trái");
    SendMessageW(corner, CB_ADDSTRING, 0, (LPARAM)L"Trên phải");
    SendMessageW(corner, CB_ADDSTRING, 0, (LPARAM)L"Dưới trái");
    SendMessageW(corner, CB_ADDSTRING, 0, (LPARAM)L"Dưới phải");
    int cornerSel = (c.gamingOverlayCorner < 0 || c.gamingOverlayCorner > 3)
                        ? 3
                        : c.gamingOverlayCorner;
    SendMessageW(corner, CB_SETCURSEL, cornerSel, 0);
    SetDlgItemTextW(dlg, IDC_GAMING_TRIGGER_KEY,
                    triggerKeyName(g_draftTrigger).c_str());
    refreshGamingList(dlg);
    updateTriggerWarning(dlg);
    updateGamingEnableState(dlg);

    SetDlgItemTextW(dlg, IDC_ABOUT_TEXT, L"VietKi 0.6 — bộ gõ tiếng Việt");
    addTooltip(dlg, IDC_CROSSHAIR,
               L"Giữ nút này, kéo/trỏ vào cửa sổ app cần loại trừ, rồi nhả.");
    // Phase 3 D.2 tooltips on the hotkey labels and capture controls.
    const wchar_t* tipMaster =
        L"Công tắc tổng để chuyển nhanh giữa gõ tiếng Việt và tiếng Anh trên "
        L"toàn hệ thống. Nhận mọi tổ hợp, ví dụ Alt+Z.";
    addTooltip(dlg, IDC_CHECK_MASTER_HOTKEY,
               L"Bật hoặc tắt riêng phím tắt chuyển E/V.");
    addTooltip(dlg, IDC_LABEL_MASTERHK, tipMaster);
    addTooltip(dlg, IDC_MASTER_KEY, L"Nhập ký tự cuối, ví dụ Z, hoặc Space. Để trống nếu tổ hợp chỉ gồm Ctrl/Alt/Shift/Win.");
    addTooltip(dlg, IDC_OVERRIDE_KEY, L"Nhập ký tự cuối, ví dụ Z, hoặc Space.");
    addTooltip(dlg, IDC_GAMING_CROSSHAIR,
               L"Giữ nút, kéo sang cửa sổ game cần thêm rồi thả chuột.");
    // Phase 5.1: explanations for the gaming options live on the ⓘ icons via the
    // shared hover popover (helpDocFor + helpIconProc), like the Basic tab — no
    // Win32 tooltips here, and no subtitle lines, so the tab fits vertically.
    // See docs/UI_GUIDELINES.md.
    g_suppressDirty = false;
}

void refreshLists(HWND dlg) {
    const AppState& st = state();
    if (!g_dirty) g_draftExcluded = st.config.excludedProcesses;

    HWND list = GetDlgItem(dlg, IDC_LIST_EXCLUDED);
    int sel = selectedExcludedIndex(dlg);
    ListView_DeleteAllItems(list);
    for (int i = 0; i < (int)g_draftExcluded.size(); ++i) {
        const auto& exe = g_draftExcluded[i];
        std::wstring line = exe;
        auto it = st.perAppOverride.find(toLower(exe));
        if (it != st.perAppOverride.end() && it->second != Override::None)
            line += L"  [V+]";
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.iImage = 0;
        item.pszText = line.data();
        ListView_InsertItem(list, &item);
    }
    int count = ListView_GetItemCount(list);
    if (sel >= 0 && sel < count)
        ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);

    const wchar_t* modeStr = st.currentModeVN ? L"Tiếng Việt" : L"Tiếng Anh";
    std::wstring app = st.currentApp.empty() ? L"(không rõ)" : st.currentApp;
    std::wstring extra;
    if (isExcludedMember(st.currentApp)) extra += L" • bị loại trừ";
    auto ov = st.perAppOverride.find(st.currentApp);
    if (ov != st.perAppOverride.end() && ov->second != Override::None)
        extra += L" • V+";
    std::wstring status = std::wstring(L"Đang gõ: ") + modeStr + L"   App: " + app + extra;
    SetDlgItemTextW(dlg, IDC_STATUS_TEXT, status.c_str());
    updateActionButtons(dlg);
}

void saveFromDialog(HWND dlg) {
    AppConfig& c = state().config;
    c.method = IsDlgButtonChecked(dlg, IDC_RADIO_VNI) ? Method::VNI : Method::Telex;
    c.tone = IsDlgButtonChecked(dlg, IDC_RADIO_TONE_OLD) ? TonePlacement::Old
                                                         : TonePlacement::Modern;
    c.hotkey = hotkeyFromEditor(dlg, IDC_MASTER_CTRL, IDC_MASTER_ALT,
                                IDC_MASTER_SHIFT, IDC_MASTER_WIN, IDC_MASTER_KEY,
                                L"Ctrl+Shift");
    c.toggleForCurrentAppHotkey =
        hotkeyWordFromEditor(dlg, IDC_OVERRIDE_CTRL, IDC_OVERRIDE_ALT,
                             IDC_OVERRIDE_SHIFT, IDC_OVERRIDE_WIN, IDC_OVERRIDE_KEY);

    // The logon mechanism is reconciled after saveConfig below, so the persisted
    // file matches whatever (re-launched) instance ends up applying it.
    c.autostart = IsDlgButtonChecked(dlg, IDC_CHECK_AUTOSTART) != 0;
    c.autostartAdmin =
        c.autostart && IsDlgButtonChecked(dlg, IDC_CHECK_AUTOSTART_ADMIN) != 0;
    c.autocompleteFix = IsDlgButtonChecked(dlg, IDC_CHECK_AUTOCOMPLETE) != 0;
    c.toggleVietnameseHotkeyEnabled =
        IsDlgButtonChecked(dlg, IDC_CHECK_MASTER_HOTKEY) != 0;
    c.toggleForCurrentAppHotkeyEnabled =
        IsDlgButtonChecked(dlg, IDC_CHECK_OVERRIDE_HOTKEY) != 0;
    c.exclusionFeatureOn = IsDlgButtonChecked(dlg, IDC_CHECK_EXCLUSIONON) != 0;
    c.revertOverrideOnBlur = IsDlgButtonChecked(dlg, IDC_CHECK_REVERTBLUR) != 0;
    c.spellCheck = IsDlgButtonChecked(dlg, IDC_CHECK_SPELLCHECK) != 0;
    c.lockWordAfterCancel = IsDlgButtonChecked(dlg, IDC_CHECK_LOCKCANCEL) != 0;
    c.restoreAfterSpace =
        IsDlgButtonChecked(dlg, IDC_CHECK_RESTOREAFTERSPACE) != 0;
    c.typingStats = IsDlgButtonChecked(dlg, IDC_CHECK_TYPINGSTATS) != 0;
    c.soundOnGlobalToggle = IsDlgButtonChecked(dlg, IDC_CHECK_SOUND_GLOBAL) != 0;
    c.soundOnExcludedToggle =
        IsDlgButtonChecked(dlg, IDC_CHECK_SOUND_EXCLUDED) != 0;
    c.excludedProcesses = g_draftExcluded;

    // --- Phase 5: gaming tab. Store from the draft, then resolve the single
    // policy enum from the two mutually-exclusive checkboxes (I). ---
    c.gamingTrigger = g_draftTrigger;
    c.soundOnGamingModeSwitch =
        IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_SOUND) != 0;
    c.gamingOverlayEnabled =
        IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_OVERLAY) != 0;
    LRESULT cornerSel =
        SendMessageW(GetDlgItem(dlg, IDC_COMBO_OVERLAY_CORNER), CB_GETCURSEL, 0, 0);
    if (cornerSel != CB_ERR) c.gamingOverlayCorner = (int)cornerSel;
    c.gamingProcesses = g_draftGaming;
    c.gamingPasteProcesses = g_draftGamingPaste;
    GamingPolicy gp = GamingPolicy::Disabled;
    if (IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TEMP))
        gp = GamingPolicy::TemporaryTrigger;
    else if (IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TOGGLE))
        gp = GamingPolicy::ToggleForCurrentApp;
    if (gp != GamingPolicy::Disabled || !c.gamingProcesses.empty())
        c.gamingListInitialized = true;
    // Phase 5 C.3: warn (don't block) if the temporary trigger collides with a
    // VietKi hotkey; the existing hotkey clash check is also advisory.
    if (gp == GamingPolicy::TemporaryTrigger && c.gamingTrigger.vk) {
        BYTE mMods = 0, mVk = 0;
        parseHotkeyString(c.hotkey, mMods, mVk);
        bool clashMaster = (mVk != 0 && (UINT)mVk == c.gamingTrigger.vk);
        bool clashToggle = (LOBYTE(c.toggleForCurrentAppHotkey) != 0 &&
                            (UINT)LOBYTE(c.toggleForCurrentAppHotkey) ==
                                c.gamingTrigger.vk);
        if (clashMaster || clashToggle)
            MessageBoxW(dlg,
                        L"Phím kích hoạt đang trùng với một phím tắt khác của VietKi. "
                        L"Hãy chọn phím khác nếu hoạt động không như mong đợi.",
                        L"VietKi", MB_ICONWARNING);
    }
    // applyGamingPolicy seeds once, clears stale gaming ForceVN / the session on
    // a mode change, sets c.gamingPolicy, and re-resolves (B.2). Done before
    // saveConfig so the persisted file matches the live state.
    applyGamingPolicy(gp);

    // Phase 3 D.1: a light clash check. Encode the three hotkeys the same way
    // (LOBYTE = vk, HIBYTE = HOTKEYF_* mask) and warn if two collide.
    {
        BYTE mMods = 0, mVk = 0;
        parseHotkeyString(c.hotkey, mMods, mVk);
        WORD master = (WORD)(mVk | (mMods << 8));
        if (master && c.toggleForCurrentAppHotkey &&
            master == c.toggleForCurrentAppHotkey) {
            MessageBoxW(dlg,
                        L"Hai phím tắt đang trùng nhau. Một số chức năng "
                        L"có thể không hoạt động đúng.",
                        L"VietKi", MB_ICONWARNING);
        }
    }

    saveConfig(c);

    // Reconcile the HKCU Run value and the elevated logon task with the saved
    // config. Adding/removing that task needs admin; if we lack it, offer to
    // relaunch elevated (the new instance applies the change at startup). This
    // is the "first run as admin" step the elevated autostart depends on.
    if (!reconcileAutostart(c)) {
        if (MessageBoxW(dlg,
                        L"Cần quyền Administrator để cập nhật tự khởi động bằng "
                        L"quyền Admin. Khởi động lại VietKi bằng quyền Admin ngay?",
                        L"VietKi", MB_ICONQUESTION | MB_YESNO) == IDYES) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if ((INT_PTR)ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr,
                                       SW_SHOWNORMAL) > 32) {
                g_dirty = false;
                PostMessageW(state().messageWindow, WM_CLOSE, 0, 0);
                return;
            }
        }
    }

    g_dirty = false;
    applyResolvedState();
    refreshLists(dlg);
}

void removeSelected(HWND dlg) {
    int sel = selectedExcludedIndex(dlg);
    if (sel < 0 || sel >= (int)g_draftExcluded.size()) return;
    g_draftExcluded.erase(g_draftExcluded.begin() + sel);
    markDirty(dlg);
    refreshLists(dlg);
}

void removeSelectedGaming(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_LIST_GAMING);
    int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_draftGaming.size()) return;
    setGamingPasteDraft(g_draftGaming[sel], false);
    g_draftGaming.erase(g_draftGaming.begin() + sel);
    markDirty(dlg);
    refreshGamingList(dlg);
}

void restoreGamingDefaults(HWND dlg) {
    g_draftGaming = defaultGamingProcesses();
    g_draftGamingPaste = defaultGamingPasteProcesses();
    markDirty(dlg);
    refreshGamingList(dlg);
}

// React to one of the two mutually-exclusive gaming checkboxes changing (I).
void onGamingPolicyUiChanged(HWND dlg) {
    bool anyOn = IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TOGGLE) ||
                 IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TEMP);
    // Seed the suggestion draft the first time a policy is enabled (B.4).
    if (anyOn && !state().config.gamingListInitialized && g_draftGaming.empty()) {
        g_draftGaming = defaultGamingProcesses();
        g_draftGamingPaste = defaultGamingPasteProcesses();
        refreshGamingList(dlg);
    }
    updateGamingEnableState(dlg);
}

bool confirmUnsaved(HWND dlg) {
    if (!g_dirty) return true;
    TASKDIALOG_BUTTON buttons[] = {
        { IDYES, L"Lưu" },
        { IDNO, L"Không lưu" },
        { IDCANCEL, L"Hủy" },
    };
    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = dlg;
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = L"VietKi";
    cfg.pszMainIcon = TD_WARNING_ICON;
    cfg.pszMainInstruction = L"Bạn có thay đổi chưa lưu";
    cfg.pszContent = L"Lưu thay đổi trước khi tiếp tục?";
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = IDYES;
    int result = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&cfg, &result, nullptr, nullptr))) {
        result = MessageBoxW(dlg,
                             L"Bạn có thay đổi chưa lưu. Lưu thay đổi trước khi tiếp tục?",
                             L"VietKi", MB_ICONQUESTION | MB_YESNOCANCEL);
    }
    if (result == IDYES) {
        saveFromDialog(dlg);
        return true;
    }
    return result == IDNO;
}

void closeSettings(HWND dlg) {
    if (!confirmUnsaved(dlg)) return;
    saveTypingStats(); // message thread: a safe, low-frequency point to flush
    DestroyWindow(dlg);
}

void exitApplication(HWND dlg) {
    if (!confirmUnsaved(dlg)) return;
    DestroyWindow(dlg);
    PostMessageW(state().messageWindow, WM_CLOSE, 0, 0);
}

bool isDirtyControl(int id, int code) {
    switch (id) {
        case IDC_RADIO_TELEX:
        case IDC_RADIO_VNI:
        case IDC_RADIO_TONE_MODERN:
        case IDC_RADIO_TONE_OLD:
        case IDC_CHECK_AUTOSTART:
        case IDC_CHECK_AUTOSTART_ADMIN:
        case IDC_CHECK_AUTOCOMPLETE:
        case IDC_CHECK_EXCLUSIONON:
        case IDC_CHECK_REVERTBLUR:
        case IDC_CHECK_SPELLCHECK:
        case IDC_CHECK_LOCKCANCEL:
        case IDC_CHECK_RESTOREAFTERSPACE:
        case IDC_CHECK_TYPINGSTATS:
        case IDC_CHECK_SOUND_GLOBAL:
        case IDC_CHECK_SOUND_EXCLUDED:
        case IDC_CHECK_MASTER_HOTKEY:
        case IDC_CHECK_OVERRIDE_HOTKEY:
        case IDC_MASTER_CTRL:
        case IDC_MASTER_ALT:
        case IDC_MASTER_SHIFT:
        case IDC_MASTER_WIN:
        case IDC_OVERRIDE_CTRL:
        case IDC_OVERRIDE_ALT:
        case IDC_OVERRIDE_SHIFT:
        case IDC_OVERRIDE_WIN:
        case IDC_CHECK_GAMING_TOGGLE:
        case IDC_CHECK_GAMING_TEMP:
        case IDC_CHECK_GAMING_SOUND:
        case IDC_CHECK_GAMING_OVERLAY:
            return code == BN_CLICKED;
        case IDC_COMBO_OVERLAY_CORNER:
            return code == CBN_SELCHANGE;
        case IDC_MASTER_KEY:
        case IDC_OVERRIDE_KEY:
            return code == EN_CHANGE;
        default:
            return false;
    }
}

INT_PTR CALLBACK pageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(page, GWLP_USERDATA, lParam);
        return TRUE;
    }

    HWND dlg = (HWND)GetWindowLongPtrW(page, GWLP_USERDATA);
    switch (msg) {
        case WM_COMMAND:
        case WM_NOTIFY:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return dlg ? SendMessageW(dlg, msg, wParam, lParam) : FALSE;
        case WM_ERASEBKGND: {
            HDC dc = (HDC)wParam;
            RECT client;
            GetClientRect(page, &client);
            FillRect(dc, &client,
                     g_tabPageBrush ? g_tabPageBrush
                                    : GetSysColorBrush(COLOR_WINDOW));
            return TRUE;
        }
        case WM_DPICHANGED_BEFOREPARENT:
        case WM_DPICHANGED_AFTERPARENT:
            // The top dialog's WM_DPICHANGED handler already rescaled every
            // descendant (including this page's controls) via a single
            // recursive EnumChildWindows pass. Returning FALSE here would let
            // this page's own DefDlgProc run its default DPI rescaling too,
            // compounding a second scale on top of ours.
            return (INT_PTR)TRUE;
        default:
            break;
    }
    return FALSE;
}

// --- DPI change handling -----------------------------------------------------
//
// The dialog templates use fixed dialog units resolved at creation-time DPI.
// Windows does not relayout an already-created dialog when the user drags it
// to a monitor with a different DPI — every control keeps its old pixel rect
// and font size while only the window frame gets resized, which is exactly
// why the controls end up scattered. WM_DPICHANGED is our cue to manually
// rescale every descendant control's rect and font by the DPI ratio.

HFONT createScaledDialogFont(int dpi) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(9, dpi, 72); // matches "FONT 9, Segoe UI" in resources.rc
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

struct ChildLayout {
    HWND hwnd;
    RECT rc; // relative to the immediate parent's client area, pre-scale pixels
};

// EnumChildWindows recurses into grandchildren too, so one call starting from
// the main dialog reaches every control on every tab page in a single pass.
void collectChildLayouts(HWND root, std::vector<ChildLayout>& out) {
    EnumChildWindows(
        root,
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* results = reinterpret_cast<std::vector<ChildLayout>*>(lp);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            MapWindowPoints(nullptr, GetParent(hwnd), (LPPOINT)&rc, 2);
            results->push_back({hwnd, rc});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
}

void rescaleListViewColumns(HWND dlg, int numX, int denX) {
    for (int id : {IDC_LIST_EXCLUDED, IDC_LIST_GAMING, IDC_LIST_TOPWORDS}) {
        HWND list = settingsControl(dlg, id);
        if (!list) continue;
        for (int col = 0;; ++col) {
            int width = ListView_GetColumnWidth(list, col);
            if (width <= 0) break;
            ListView_SetColumnWidth(list, col, MulDiv(width, numX, denX));
        }
    }
}

// Re-fit the tab pages into the (now-rescaled) tab control, exactly as
// initTabs() does when they're first created.
void repositionTabPages(HWND dlg) {
    HWND tab = GetDlgItem(dlg, IDC_TAB);
    RECT pageRect;
    GetClientRect(tab, &pageRect);
    TabCtrl_AdjustRect(tab, FALSE, &pageRect);
    for (HWND page : g_tabPages) {
        if (page)
            SetWindowPos(page, HWND_TOP, pageRect.left, pageRect.top,
                        pageRect.right - pageRect.left,
                        pageRect.bottom - pageRect.top, SWP_NOACTIVATE);
    }
}

void handleDpiChanged(HWND dlg, int newDpi, const RECT* suggested) {
    int oldDpi = g_dlgDpi;
    if (newDpi == oldDpi) return;

    closeHelp(); // the popover is a separate popup; don't let it go stale-sized

    std::vector<ChildLayout> layouts;
    collectChildLayouts(dlg, layouts); // must capture before anything moves

    RECT oldClient;
    GetClientRect(dlg, &oldClient); // client size BEFORE the resize below

    HFONT newFont = createScaledDialogFont(newDpi);
    HFONT oldFont = g_dlgFont;

    SetWindowPos(dlg, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(dlg, WM_SETFONT, (WPARAM)newFont, TRUE);

    // The OS-suggested outer rect does not grow by exactly newDpi/oldDpi —
    // non-client chrome (title bar, borders) scales independently, so the
    // client area's actual growth ratio drifts slightly from the raw DPI
    // ratio (measured: DPI ratio 1.75 vs. client-width ratio 1.71 at
    // 100%->175%, enough to push wide rows outside the window). Rescale
    // every descendant by the MEASURED client-area ratio, not the DPI ratio.
    RECT newClient;
    GetClientRect(dlg, &newClient);
    int numX = newClient.right, denX = oldClient.right;
    int numY = newClient.bottom, denY = oldClient.bottom;

    for (const auto& c : layouts) {
        int x = MulDiv(c.rc.left, numX, denX);
        int y = MulDiv(c.rc.top, numY, denY);
        int w = MulDiv(c.rc.right - c.rc.left, numX, denX);
        int h = MulDiv(c.rc.bottom - c.rc.top, numY, denY);
        SetWindowPos(c.hwnd, nullptr, x, y, w, h,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SendMessageW(c.hwnd, WM_SETFONT, (WPARAM)newFont, TRUE);
    }
    repositionTabPages(dlg); // pixel-perfect fit under the rescaled tab strip
    rescaleListViewColumns(dlg, numX, denX);

    if (oldFont) DeleteObject(oldFont);
    g_dlgFont = newFont;
    g_dlgDpi = newDpi;

    // The ⓘ / admin-status icon fonts are cached independently of the dialog
    // font (Phase 4 H) and keyed on "already created", not on DPI — drop and
    // recreate them now so they don't stay the wrong size.
    destroyHelpFonts();
    applyHelpIconFont(dlg);
    updateAdminStatus(dlg);

    InvalidateRect(dlg, nullptr, TRUE);
}

INT_PTR CALLBACK dlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            g_dlgDpi = GetDpiForWindow(dlg);
            applyDialogIcon(dlg);
            initTabs(dlg);
            initExcludedList(dlg);
            refreshTabPageBrush(dlg);
            SetWindowSubclass(GetDlgItem(dlg, IDC_CROSSHAIR), crosshairProc, 1, 0);
            // Phase 4 H: the ⓘ icons open the custom help popover on hover/click.
            SetWindowSubclass(GetDlgItem(dlg, IDC_HELP_SPELLCHECK), helpIconProc, 2,
                              IDC_HELP_SPELLCHECK);
            SetWindowSubclass(GetDlgItem(dlg, IDC_HELP_LOCKCANCEL), helpIconProc, 2,
                              IDC_HELP_LOCKCANCEL);
            SetWindowSubclass(GetDlgItem(dlg, IDC_HELP_RESTOREAFTERSPACE),
                              helpIconProc, 2, IDC_HELP_RESTOREAFTERSPACE);
            SetWindowSubclass(GetDlgItem(dlg, IDC_HELP_TYPINGSTATS),
                              helpIconProc, 2, IDC_HELP_TYPINGSTATS);
            SetWindowSubclass(GetDlgItem(dlg, IDC_HELP_OVERRIDE_HOTKEY),
                              helpIconProc, 2, IDC_HELP_OVERRIDE_HOTKEY);
            SetWindowSubclass(GetDlgItem(dlg, IDC_HELP_AUTOCOMPLETE),
                              helpIconProc, 2, IDC_HELP_AUTOCOMPLETE);
            // Phase 5.1: gaming-tab ⓘ icons share the same hover popover.
            for (int helpId : {IDC_HELP_GAMING_TOGGLE, IDC_HELP_GAMING_TEMP,
                               IDC_HELP_GAMING_SOUND, IDC_HELP_GAMING_OVERLAY,
                               IDC_HELP_GAMING_PASTE})
                SetWindowSubclass(GetDlgItem(dlg, helpId), helpIconProc, 2, helpId);
            applyHelpIconFont(dlg);
            updateAdminStatus(dlg);
            initGamingList(dlg);
            initStatsList(dlg);
            // Phase 5: the gaming crosshair shares the picker proc; the trigger
            // edit captures a physical key.
            SetWindowSubclass(GetDlgItem(dlg, IDC_GAMING_CROSSHAIR), crosshairProc, 1, 0);
            SetWindowSubclass(GetDlgItem(dlg, IDC_GAMING_TRIGGER_KEY),
                              triggerEditProc, 3, 0);
            populate(dlg);
            refreshLists(dlg);
            return TRUE;
        case WM_TIMER:
            if (wParam == kHelpHoverTimer) {
                KillTimer(dlg, kHelpHoverTimer);
                int iconId = (int)GetWindowLongPtrW(dlg, GWLP_USERDATA);
                HWND icon = GetDlgItem(dlg, iconId);
                POINT pt;
                RECT ir;
                GetCursorPos(&pt);
                if (icon && GetWindowRect(icon, &ir) && PtInRect(&ir, pt) && !g_help)
                    if (const HelpDoc* doc = helpDocFor(iconId))
                        showHelp(dlg, iconId, *doc, false);
                return TRUE;
            }
            break;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            refreshTabPageBrush(dlg);
            InvalidateRect(dlg, nullptr, TRUE);
            for (HWND page : g_tabPages) {
                if (page) InvalidateRect(page, nullptr, TRUE);
            }
            break;
        case WM_DPICHANGED:
            // Must return TRUE ("handled"): a DLGPROC returning FALSE here
            // makes the framework fall through to DefDlgProc, which has its
            // own built-in (and much cruder) dialog-template rescaling for
            // apps that don't handle this message — compounding with ours
            // and blowing the layout out far beyond the window.
            handleDpiChanged(dlg, LOWORD(wParam), (const RECT*)lParam);
            return (INT_PTR)TRUE;
        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            HDC dc = (HDC)wParam;
            int id = GetDlgCtrlID(ctl);
            if (id == IDC_GAMING_TRIGGER_KEY) {
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
            }
            if (id == IDC_GAMING_ADMIN_WARN || id == IDC_GAMING_ADMIN_ICON) {
                SetTextColor(dc, g_runningElevated ? RGB(22, 140, 60)
                                                   : RGB(230, 126, 34));
                return paintThemedDialogControl(dc);
            }
            if (id == IDC_GAMING_PASTE_WARN) {
                SetTextColor(dc, RGB(230, 126, 34));
                return paintThemedDialogControl(dc);
            }
            if (id == IDC_DIRTY_MARK) {
                SetTextColor(dc, RGB(255, 193, 7));
                SetBkMode(dc, TRANSPARENT);
                return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
            }
            if (isTabPageTextControl(id)) {
                return paintThemedDialogControl(dc);
            }
            break;
        }
        case WM_CTLCOLORBTN: {
            HWND ctl = (HWND)lParam;
            HDC dc = (HDC)wParam;
            if (isTabPageTextControl(GetDlgCtrlID(ctl))) {
                return paintThemedDialogControl(dc);
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            switch (id) {
                case IDC_BTN_SAVE: saveFromDialog(dlg); return TRUE;
                case IDC_BTN_EXCLUDED_ADD:
                    addTypedName(dlg, IDC_EXCLUDED_NAME, g_draftExcluded,
                                 L"Hãy nhập tên file app (ví dụ: game.exe).");
                    refreshLists(dlg);
                    return TRUE;
                case IDC_BTN_REMOVE: removeSelected(dlg); return TRUE;
                case IDC_BTN_GAMING_ADD:
                    addTypedName(dlg, IDC_GAMING_NAME, g_draftGaming,
                                 L"Hãy nhập tên file game (ví dụ: game.exe).");
                    refreshGamingList(dlg);
                    return TRUE;
                case IDC_BTN_GAMING_REMOVE: removeSelectedGaming(dlg); return TRUE;
                case IDC_BTN_GAMING_RESTORE: restoreGamingDefaults(dlg); return TRUE;
                case IDC_BTN_CLEAR_STATS: {
                    int r = MessageBoxW(dlg,
                        L"Xoá toàn bộ dữ liệu thống kê gõ phím đã lưu? Không thể hoàn tác.",
                        L"Xoá thống kê", MB_YESNO | MB_ICONWARNING);
                    if (r == IDYES) {
                        clearTypingStats();
                        refreshStatsDisplay(dlg);
                    }
                    return TRUE;
                }
                case IDC_CHECK_GAMING_TOGGLE:
                    if (IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TOGGLE))
                        CheckDlgButton(dlg, IDC_CHECK_GAMING_TEMP, BST_UNCHECKED);
                    onGamingPolicyUiChanged(dlg);
                    markDirty(dlg);
                    return TRUE;
                case IDC_CHECK_GAMING_TEMP:
                    if (IsDlgButtonChecked(dlg, IDC_CHECK_GAMING_TEMP))
                        CheckDlgButton(dlg, IDC_CHECK_GAMING_TOGGLE, BST_UNCHECKED);
                    onGamingPolicyUiChanged(dlg);
                    markDirty(dlg);
                    return TRUE;
                case IDC_CHECK_GAMING_OVERLAY:
                    updateGamingEnableState(dlg);
                    markDirty(dlg);
                    return TRUE;
                case IDC_CHECK_AUTOSTART:
                    updateAutostartAdminEnable(dlg);
                    markDirty(dlg);
                    return TRUE;
                case IDC_BTN_RUNADMIN: {
                    if (!confirmUnsaved(dlg)) return TRUE;
                    wchar_t exe[MAX_PATH];
                    GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    if ((INT_PTR)ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr,
                                               SW_SHOWNORMAL) > 32)
                        PostMessageW(state().messageWindow, WM_CLOSE, 0, 0);
                    return TRUE;
                }
                case IDC_BTN_EXIT_APP:
                    exitApplication(dlg);
                    return TRUE;
                case IDC_BTN_CLOSE:
                case IDCANCEL:
                    closeSettings(dlg);
                    return TRUE;
                default:
                    break;
            }
            if (isDirtyControl(id, code)) markDirty(dlg);
            return FALSE;
        }
        case WM_NOTIFY: {
            NMHDR* hdr = (NMHDR*)lParam;
            if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
                showTab(dlg, TabCtrl_GetCurSel(hdr->hwndFrom));
                return TRUE;
            }
            if ((hdr->idFrom == IDC_LIST_EXCLUDED ||
                 hdr->idFrom == IDC_LIST_GAMING) &&
                hdr->code == LVN_ITEMCHANGED) {
                if (hdr->idFrom == IDC_LIST_GAMING && !g_refreshingGamingList) {
                    auto* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
                    UINT oldImage =
                        (changed->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                    UINT newImage =
                        (changed->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                    if (changed->iItem >= 0 &&
                        changed->iItem < (int)g_draftGaming.size() &&
                        oldImage != newImage && (newImage == 1 || newImage == 2)) {
                        setGamingPasteDraft(g_draftGaming[changed->iItem],
                                            newImage == 2);
                        markDirty(dlg);
                    }
                }
                updateActionButtons(dlg);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            closeSettings(dlg);
            return TRUE;
        case WM_DESTROY:
            if (g_tabPageBrush) {
                DeleteObject(g_tabPageBrush);
                g_tabPageBrush = nullptr;
            }
            if (g_dlgFont) {
                DeleteObject(g_dlgFont);
                g_dlgFont = nullptr;
            }
            g_dlgDpi = USER_DEFAULT_SCREEN_DPI;
            KillTimer(dlg, kHelpHoverTimer);
            closeHelp();
            destroyHelpFonts();
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_CROSSHAIR), crosshairProc, 1);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_GAMING_CROSSHAIR), crosshairProc, 1);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_GAMING_TRIGGER_KEY),
                                 triggerEditProc, 3);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_HELP_SPELLCHECK), helpIconProc, 2);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_HELP_LOCKCANCEL), helpIconProc, 2);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_HELP_RESTOREAFTERSPACE),
                                 helpIconProc, 2);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_HELP_TYPINGSTATS),
                                 helpIconProc, 2);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_HELP_OVERRIDE_HOTKEY),
                                 helpIconProc, 2);
            RemoveWindowSubclass(GetDlgItem(dlg, IDC_HELP_AUTOCOMPLETE),
                                 helpIconProc, 2);
            for (int helpId : {IDC_HELP_GAMING_TOGGLE, IDC_HELP_GAMING_TEMP,
                               IDC_HELP_GAMING_SOUND, IDC_HELP_GAMING_OVERLAY,
                               IDC_HELP_GAMING_PASTE})
                RemoveWindowSubclass(GetDlgItem(dlg, helpId), helpIconProc, 2);
            state().settingsWindow = nullptr;
            g_dirty = false;
            g_draftExcluded.clear();
            g_draftGaming.clear();
            g_draftGamingPaste.clear();
            g_tooltip = nullptr;
            for (HWND& page : g_tabPages) page = nullptr;
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

} // namespace

void openSettings() {
    if (state().settingsWindow) {
        SetForegroundWindow(state().settingsWindow);
        return;
    }
    HWND dlg = CreateDialogParamW(GetModuleHandleW(nullptr),
                                  MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, dlgProc, 0);
    if (!dlg) return;
    state().settingsWindow = dlg;
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
}

void refreshSettingsWindow() {
    HWND dlg = state().settingsWindow;
    if (dlg) refreshLists(dlg);
}

} // namespace vietki::win
