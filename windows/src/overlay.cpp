// Phase 5: a translucent, click-through "Tiếng Việt" badge shown over the game
// while a temporary Vietnamese session is active (the G+ state), so the player
// always knows Vietnamese typing is on without watching the tray. It is a
// layered, topmost, no-activate tool window: it renders over borderless /
// windowed-fullscreen games (e.g. Genshin) but NOT over exclusive-fullscreen
// DirectX, which would require a graphics-API hook overlay.
#include "app.h"

namespace vietki::win {

namespace {

HWND g_overlay = nullptr;
bool g_overlayVisible = false;
constexpr wchar_t kOverlayClass[] = L"VietKiGamingOverlay";
// Background pixels painted in this color become fully transparent (LWA_COLORKEY),
// so only the rounded badge shows over the game.
constexpr COLORREF kColorKey = RGB(255, 0, 255);
constexpr int kMargin = 28; // gap from the screen edge, in pixels
constexpr int kWidth = 64;
constexpr int kHeight = 46;

void paintOverlay(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // Everything outside the badge is the color key -> invisible.
    HBRUSH keyBrush = CreateSolidBrush(kColorKey);
    FillRect(dc, &rc, keyBrush);
    DeleteObject(keyBrush);

    // Rounded dark badge with a soft green outline.
    HBRUSH badge = CreateSolidBrush(RGB(24, 24, 28));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 200, 120));
    HGDIOBJ ob = SelectObject(dc, badge);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 16, 16);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(badge);
    DeleteObject(pen);

    // Label.
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 255, 230));
    HFONT font = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, font);
    DrawTextW(dc, L"VN", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    DeleteObject(font);

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        paintOverlay(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void initGamingOverlay() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = overlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    RegisterClassW(&wc);
    // WS_EX_TRANSPARENT = click-through; NOACTIVATE = never steals focus from the
    // game; TOOLWINDOW = no taskbar button; LAYERED = alpha + color key.
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW,
        kOverlayClass, L"", WS_POPUP, 0, 0, kWidth, kHeight, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (g_overlay)
        SetLayeredWindowAttributes(g_overlay, kColorKey, 205,
                                   LWA_COLORKEY | LWA_ALPHA);
}

void destroyGamingOverlay() {
    if (g_overlay) {
        DestroyWindow(g_overlay);
        g_overlay = nullptr;
    }
}

// Show the badge in the configured corner whenever the resolved icon is G+
// (a gaming app with Vietnamese on); hide it otherwise. Called from
// applyResolvedState(), i.e. only on mode/icon transitions, not per keystroke.
void updateGamingOverlay() {
    if (!g_overlay) return;
    AppState& st = state();
    bool show =
        st.config.gamingOverlayEnabled && st.currentIcon == IconState::GamingVN;
    if (!show) {
        if (g_overlayVisible) {
            ShowWindow(g_overlay, SW_HIDE);
            g_overlayVisible = false;
        }
        return;
    }

    // Position in the chosen corner of the game's monitor. rcMonitor (not
    // rcWork) is used so it sits correctly even when the game covers the taskbar.
    HWND ref = st.currentWindow ? st.currentWindow : GetForegroundWindow();
    HMONITOR mon = MonitorFromWindow(ref, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT r = mi.rcMonitor;
    int x, y;
    switch (st.config.gamingOverlayCorner) {
        case 0: x = r.left + kMargin;           y = r.top + kMargin;              break;
        case 1: x = r.right - kWidth - kMargin; y = r.top + kMargin;              break;
        case 2: x = r.left + kMargin;           y = r.bottom - kHeight - kMargin; break;
        default: x = r.right - kWidth - kMargin; y = r.bottom - kHeight - kMargin; break;
    }
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, kWidth, kHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_overlayVisible = true;
    InvalidateRect(g_overlay, nullptr, TRUE);
}

} // namespace vietki::win
