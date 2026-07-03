// System tray icon and right-click menu (guide 5.6).
#include "app.h"

#include <shellapi.h>

#include "resource.h"

namespace vietki::win {

namespace {

NOTIFYICONDATAW g_nid = {};
constexpr UINT kTrayId = 1;

// Cached status icons, loaded once at startup (hot-path note).
HICON g_iconV = nullptr;
HICON g_iconE = nullptr;
HICON g_iconVPlus = nullptr;
HICON g_iconVMinus = nullptr;
HICON g_iconGaming = nullptr;   // G  — Gaming Mode, Vietnamese off
HICON g_iconGamingVN = nullptr; // G+ — Gaming Mode, Vietnamese on

// Menu command ids.
enum {
    CMD_TOGGLE = 100,
    CMD_TELEX,
    CMD_VNI,
    CMD_TONE_MODERN,
    CMD_TONE_OLD,
    CMD_AUTOSTART,
    CMD_AUTOCOMPLETE,
    CMD_RUN_ADMIN,
    CMD_SETTINGS,
    CMD_EXIT,
};

HICON loadIcon(WORD id) {
    HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
                                  IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    return ico;
}

} // namespace

bool createTray(HWND owner) {
    g_iconV = loadIcon(IDI_TRAY_V);
    g_iconE = loadIcon(IDI_TRAY_E);
    g_iconVPlus = loadIcon(IDI_TRAY_VPLUS);
    g_iconVMinus = loadIcon(IDI_TRAY_VMINUS);
    g_iconGaming = loadIcon(IDI_TRAY_G);
    g_iconGamingVN = loadIcon(IDI_TRAY_GPLUS);
    if (!g_iconV) g_iconV = loadIcon(IDI_VIETKI);
    if (!g_iconV) g_iconV = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_iconE) g_iconE = g_iconV;
    if (!g_iconVPlus) g_iconVPlus = g_iconV;
    if (!g_iconVMinus) g_iconVMinus = g_iconE; // fall back to the English icon
    if (!g_iconGaming) g_iconGaming = g_iconVMinus;   // G ~ English-in-game
    if (!g_iconGamingVN) g_iconGamingVN = g_iconVPlus; // G+ ~ Vietnamese-in-game

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_VIETKI_TRAY;
    g_nid.hIcon = g_iconV;
    lstrcpynW(g_nid.szTip, L"VietKi — Vietnamese input", ARRAYSIZE(g_nid.szTip));
    return Shell_NotifyIconW(NIM_ADD, &g_nid) == TRUE;
}

void readdTrayIcon() {
    // Explorer (re)created the taskbar (its "TaskbarCreated" broadcast), or the
    // shell tray was not ready when createTray() first ran (e.g. launched early at
    // logon). Re-add our icon with the full add flags, then repaint the resolved
    // state. NIM_ADD is harmless if the icon happens to already be present.
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    updateTrayIcon();
}

void destroyTray() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_iconV) DestroyIcon(g_iconV);
    if (g_iconE && g_iconE != g_iconV) DestroyIcon(g_iconE);
    if (g_iconVPlus && g_iconVPlus != g_iconV) DestroyIcon(g_iconVPlus);
    if (g_iconVMinus && g_iconVMinus != g_iconV && g_iconVMinus != g_iconE)
        DestroyIcon(g_iconVMinus);
    if (g_iconGaming && g_iconGaming != g_iconVMinus) DestroyIcon(g_iconGaming);
    if (g_iconGamingVN && g_iconGamingVN != g_iconVPlus) DestroyIcon(g_iconGamingVN);
}

void updateTrayIcon() {
    // Swap both the icon and the tooltip to match the resolved V / E / V+ state
    // (Phase 2 E). The method name keeps the tooltip informative.
    const AppState& st = state();
    const AppConfig& c = st.config;
    const wchar_t* method = (c.method == Method::VNI) ? L"VNI" : L"Telex";
    HICON ico = g_iconV;
    std::wstring tip;
    switch (st.currentIcon) {
        case IconState::E:
            // Only reached when the master switch is off (English everywhere).
            ico = g_iconE;
            tip = L"VietKi — Đã tắt (Tiếng Anh toàn cục)";
            break;
        case IconState::VMinus: {
            // Master is on, but this app types English because it is excluded.
            ico = g_iconVMinus;
            tip = L"VietKi — Tiếng Anh cho app bị loại trừ";
            break;
        }
        case IconState::VPlus:
            ico = g_iconVPlus;
            tip = std::wstring(L"VietKi — Tiếng Việt trong app loại trừ (override, ") +
                  method + L")";
            break;
        case IconState::Gaming:
            ico = g_iconGaming;
            tip = L"VietKi — Gaming Mode: tiếng Việt đang tắt";
            break;
        case IconState::GamingVN:
            ico = g_iconGamingVN;
            tip = std::wstring(L"VietKi — Gaming Mode: tiếng Việt đang bật (") +
                  method + L")";
            break;
        case IconState::V:
        default:
            ico = g_iconV;
            tip = std::wstring(L"VietKi — Tiếng Việt (") + method + L")";
            break;
    }
    g_nid.hIcon = ico;
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void showModeNotification(const std::wstring& text) {
    // Phase 5 G.2: a non-activating tray balloon. De-duplicate the same message
    // within a short window so repeated focus events from a game do not spam.
    static std::wstring lastText;
    static ULONGLONG lastTick = 0;
    ULONGLONG now = GetTickCount64();
    if (text == lastText && now - lastTick < 1500) return;
    lastText = text;
    lastTick = now;

    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_NONE; // no system sound (Phase 5 G.2)
    lstrcpynW(g_nid.szInfoTitle, L"VietKi", ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo, text.c_str(), ARRAYSIZE(g_nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_TIP; // restore for the next icon update
}

void showTrayMenu(HWND owner) {
    const AppConfig& c = state().config;
    HMENU m = CreatePopupMenu();

    std::wstring toggle =
        (c.enabled ? L"Tắt tiếng Việt" : L"Bật tiếng Việt");
    if (!c.hotkey.empty()) toggle += L"\t" + c.hotkey;
    AppendMenuW(m, MF_STRING, CMD_TOGGLE, toggle.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING | (c.method == Method::Telex ? MF_CHECKED : 0),
                CMD_TELEX, L"Kiểu gõ: Telex");
    AppendMenuW(m, MF_STRING | (c.method == Method::VNI ? MF_CHECKED : 0),
                CMD_VNI, L"Kiểu gõ: VNI");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING | (c.tone == TonePlacement::Modern ? MF_CHECKED : 0),
                CMD_TONE_MODERN, L"Đặt dấu: Kiểu mới");
    AppendMenuW(m, MF_STRING | (c.tone == TonePlacement::Old ? MF_CHECKED : 0),
                CMD_TONE_OLD, L"Đặt dấu: Kiểu cũ");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING | (c.autostart ? MF_CHECKED : 0), CMD_AUTOSTART,
                L"Khởi động cùng Windows");
    AppendMenuW(m, MF_STRING | (c.autocompleteFix ? MF_CHECKED : 0),
                CMD_AUTOCOMPLETE, L"Sửa lỗi dấu gợi ý");
    AppendMenuW(m, MF_STRING, CMD_RUN_ADMIN, L"Chạy với quyền Admin");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_SETTINGS, L"Cài đặt…");
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Thoát");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner); // so the menu dismisses on focus loss
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0,
                              owner, nullptr);
    DestroyMenu(m);

    AppConfig& cfg = state().config;
    switch (cmd) {
        case CMD_TOGGLE: toggleEnabled(); break;
        case CMD_TELEX: setMethod(Method::Telex); break;
        case CMD_VNI: setMethod(Method::VNI); break;
        case CMD_TONE_MODERN: setTonePlacement(TonePlacement::Modern); break;
        case CMD_TONE_OLD: setTonePlacement(TonePlacement::Old); break;
        case CMD_AUTOSTART:
            cfg.autostart = !cfg.autostart;
            applyAutostart(cfg.autostart);
            saveConfig(cfg);
            break;
        case CMD_AUTOCOMPLETE:
            cfg.autocompleteFix = !cfg.autocompleteFix;
            saveConfig(cfg);
            refreshForegroundApp();
            break;
        case CMD_RUN_ADMIN: {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            // UIPI blocks injecting into elevated windows from a normal process;
            // relaunch elevated (guide 5.9).
            if ((INT_PTR)ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr,
                                       SW_SHOWNORMAL) > 32) {
                PostMessageW(owner, WM_CLOSE, 0, 0);
            }
            break;
        }
        case CMD_SETTINGS: openSettings(); break;
        case CMD_EXIT: PostMessageW(owner, WM_CLOSE, 0, 0); break;
        default: break;
    }
}

} // namespace vietki::win
