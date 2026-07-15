// UIA detection of whether the focused control is a browser address bar
// (Chrome/Edge omnibox, UIA ClassName "OmniboxViewViews").
//
// The selection-replace "autocomplete fix" (injector.cpp) selects the trailing
// characters with Shift+Left and types over them so the overwrite also swallows
// the omnibox's inline autocomplete suggestion. That is only correct in the
// address bar: a web-content editor such as Notion or Google Docs does not honor
// the synthetic Shift+Left selection when the replacement Unicode is inserted, so
// the pre-transform characters survive and the result accumulates
// ("DĐoôiổi" for "Đổi"). Those surfaces must use plain Backspace instead.
//
// Chrome renders the omnibox and the web content in one HWND
// (Chrome_WidgetWin_1), so a window-class check cannot tell them apart — only UIA
// can. A global EVENT_OBJECT_FOCUS hook (main.cpp) calls updateOmniboxFocus() on
// the message-loop thread whenever focus moves inside a detected browser; it
// reads the focused element's UIA ClassName and caches the verdict in an atomic
// that the low-level keyboard hook reads with zero UIA cost on the hot path.
#include "app.h"

#include <atomic>

// app.h pulls in <windows.h> with WIN32_LEAN_AND_MEAN, which omits the COM/OLE
// headers. <uiautomation.h> needs the `interface` macro, IUnknown and BSTR, so
// include the COM automation headers explicitly before it.
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>

namespace vietki::win {

namespace {

std::atomic<bool> g_omniboxFocused{false};
IUIAutomation* g_uia = nullptr;
bool g_uiaTried = false;

// Lazily create the UIA client. OLE is already initialized (STA) on this
// message-loop thread by WinMain (OleInitialize), which is also where the
// EVENT_OBJECT_FOCUS callback runs, so no extra CoInitialize is needed here.
IUIAutomation* uia() {
    if (!g_uia && !g_uiaTried) {
        g_uiaTried = true;
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&g_uia));
    }
    return g_uia;
}

} // namespace

bool omniboxFocused() {
    return g_omniboxFocused.load(std::memory_order_relaxed);
}

void updateOmniboxFocus() {
    bool isOmnibox = false;
    if (IUIAutomation* a = uia()) {
        IUIAutomationElement* el = nullptr;
        if (SUCCEEDED(a->GetFocusedElement(&el)) && el) {
            BSTR cls = nullptr;
            if (SUCCEEDED(el->get_CurrentClassName(&cls)) && cls) {
                // Chrome/Edge address bar. Substring match so a minor future
                // class-name variant ("OmniboxViewViews…") still routes right.
                isOmnibox = wcsstr(cls, L"Omnibox") != nullptr;
                SysFreeString(cls);
            }
            el->Release();
        }
    }
    g_omniboxFocused.store(isOmnibox, std::memory_order_relaxed);
}

void clearOmniboxFocus() {
    g_omniboxFocused.store(false, std::memory_order_relaxed);
}

void shutdownOmniboxProbe() {
    if (g_uia) {
        g_uia->Release();
        g_uia = nullptr;
    }
}

} // namespace vietki::win
