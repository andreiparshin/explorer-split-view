#pragma once

#include "../../include/SharedProtocol.h"
#include "RightPaneView.h"
#include <memory>
#include <unordered_set>

namespace ExplorerSplit {

class TabSplitManager {
public:
    TabSplitManager(HINSTANCE hInst, HWND hwndShellTab, BrowseRedirectCallback browseRouter);
    ~TabSplitManager();

    TabSplitManager(const TabSplitManager&) = delete;
    TabSplitManager& operator=(const TabSplitManager&) = delete;

    bool Attach();
    void DetachAndRestore();

    HWND GetShellTabHwnd() const { return m_hwndShellTab; }
    bool IsSplitActive() const { return m_isSplitActive; }
    int  GetActivePaneSide() const { return m_activePaneSide; }

    void ToggleSplit();
    void SetSplitActive(bool active);
    void ResetSplitRatio();
    void RefreshFocusIndicator();
    void NotifyPointerDownOnHwnd(HWND hwndClicked);
    void OnChildCreatedOrSized();

    // Intercepts IShellBrowser::BrowseObject when Right Pane is active (e.g. Left Sidebar clicks)
    bool TryRedirectBrowseToActivePane(PCUIDLIST_RELATIVE pidl, UINT wFlags);

    bool PreTranslateMessage(MSG* pMsg);

    static bool RegisterHelperClasses(HINSTANCE hInst);

private:
    static LRESULT CALLBACK ShellTabSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData
    );
    static LRESULT CALLBACK DuiViewSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData
    );

    static LRESULT CALLBACK SplitterWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK FocusBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ToggleBtnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void SyncDuiViews();
    void EnsureHelperWindows();
    void EnsureOverlayZOrder();
    void ApplyLayout();
    void SyncRightPaneToLeft();
    void FocusLeftPane();
    void SetActivePaneSide(int side);
    void UpdateThemeAndDpi();

    LRESULT HandleSplitterMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleFocusBarMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleToggleBtnMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HINSTANCE m_hInst = nullptr;
    HWND m_hwndShellTab = nullptr;
    HWND m_hwndDuiView = nullptr;
    BrowseRedirectCallback m_browseRouter = nullptr;

    std::unordered_set<HWND> m_subclassedDuiViews;
    std::unordered_set<HWND> m_clipSiblingsModified;

    HWND m_hwndSplitter = nullptr;
    HWND m_hwndFocusBar = nullptr;
    HWND m_hwndToggleBtn = nullptr;
    HWND m_hwndToggleTooltip = nullptr;

    std::unique_ptr<RightPaneView> m_rightPane;

    bool m_tabSubclassed = false;
    bool m_isSplitActive = false;
    bool m_inInternalLayout = false;
    bool m_creatingHelpers = false;

    double m_splitRatio = 0.50;
    bool m_draggingSplitter = false;
    bool m_hoverSplitter = false;
    bool m_trackingSplitterMouse = false;

    bool m_hoverToggleBtn = false;
    bool m_pressToggleBtn = false;
    bool m_trackingToggleMouse = false;

    int m_activePaneSide = 0; // 0 = left, 1 = right
    int m_cachedLeftWidth = 0;
    int m_cachedSplitterWidth = 6;

    UINT m_dpi = 96;
    ThemePalette m_theme{};
};

} // namespace ExplorerSplit
