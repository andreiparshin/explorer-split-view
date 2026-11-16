#pragma once

#include "../../include/SharedProtocol.h"
#include "ShellHelpers.h"
#include <commctrl.h>
#include <functional>

namespace ExplorerSplit {

class RightPaneView {
public:
    RightPaneView();
    ~RightPaneView();

    RightPaneView(const RightPaneView&) = delete;
    RightPaneView& operator=(const RightPaneView&) = delete;

    bool Create(
        HINSTANCE hInst,
        HWND hwndParentTab,
        const RECT& rcBounds,
        PCIDLIST_ABSOLUTE initialPidl,
        std::function<void()> onCloseRequested,
        std::function<void()> onSyncFromLeftRequested,
        std::function<void()> onFocusActivity
    );

    void Destroy();

    HWND GetHwnd() const { return m_hwndContainer; }
    bool IsCreated() const { return m_hwndContainer != nullptr && IsWindow(m_hwndContainer); }
    bool ContainsFocus() const;

    void SetBounds(int x, int y, int width, int height);
    void Show(bool visible);
    void UpdateThemeAndDpi();

    bool NavigateToPidl(PCIDLIST_ABSOLUTE pidl);
    bool NavigateToPathText();
    void NavigateBack();
    void NavigateForward();
    void NavigateUp();
    void RefreshView();
    void FocusShellView();

    PCIDLIST_ABSOLUTE GetCurrentPidl() const { return m_currentPidl.Get(); }

    // Called by WH_GETMESSAGE hook for complete keyboard isolation from Left Pane
    bool PreTranslateMessage(MSG* pMsg);

    // Invoked by internal IExplorerBrowserEvents sink
    void OnBrowserNavigationComplete(PCIDLIST_ABSOLUTE pidlFolder);
    void OnBrowserNavigationFailed();

    static bool RegisterWindowClasses(HINSTANCE hInst);

private:
    enum class HeaderBtnId : int {
        None    = -1,
        Back    = 0,
        Forward = 1,
        Up      = 2,
        Refresh = 3,
        Sync    = 4,
        Close   = 5,
        Count   = 6
    };

    struct HeaderBtn {
        RECT rc{};
        wchar_t glyph = 0;
        const wchar_t* tooltip = L"";
    };

    static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PathEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void LayoutInternal();
    void PaintHeader(HDC hdc, const RECT& rcClient);
    void RecreateFontsAndBrushes();
    void ReleaseFontsAndBrushes();
    HeaderBtnId HitTestButton(POINT pt) const;
    void ExecuteHeaderButton(HeaderBtnId id);
    void EnsureTooltip(HINSTANCE hInst);
    void UpdateTooltipRects();

private:
    HINSTANCE m_hInst = nullptr;
    HWND m_hwndParentTab = nullptr;
    HWND m_hwndContainer = nullptr;
    HWND m_hwndPathEdit = nullptr;
    HWND m_hwndTooltip = nullptr;

    ComPtr<IExplorerBrowser> m_spExplorerBrowser;
    ComPtr<IExplorerBrowserEvents> m_spEventsSink;
    DWORD m_adviseCookie = 0;

    UniquePidl m_currentPidl;
    ThemePalette m_theme{};
    UINT m_dpi = 96;

    HFONT m_hFontText = nullptr;
    HFONT m_hFontIcons = nullptr;
    HBRUSH m_hbrEditBg = nullptr;

    HeaderBtn m_buttons[static_cast<int>(HeaderBtnId::Count)]{};
    RECT m_rcAddressBorder{};
    HeaderBtnId m_hoverBtn = HeaderBtnId::None;
    HeaderBtnId m_pressBtn = HeaderBtnId::None;
    bool m_trackingMouse = false;

    std::function<void()> m_onCloseRequested;
    std::function<void()> m_onSyncFromLeftRequested;
    std::function<void()> m_onFocusActivity;
};

} // namespace ExplorerSplit
