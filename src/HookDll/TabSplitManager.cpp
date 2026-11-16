#include "TabSplitManager.h"
#include <windowsx.h>
#include <cmath>
#include <vector>

namespace ExplorerSplit {

namespace {

struct BoolGuard {
    bool& flag;
    bool prev;
    explicit BoolGuard(bool& f) : flag(f), prev(f) { flag = true; }
    ~BoolGuard() { flag = prev; }
};

constexpr UINT_PTR kSubclassIdTab       = 0xE5710001;
constexpr UINT_PTR kSubclassIdDui       = 0xE5710002;
constexpr UINT     kWmRestoreRightFocus = WM_APP + 77;
constexpr UINT     kWmEnsureLayout      = WM_APP + 78;

} // namespace

TabSplitManager::TabSplitManager(HINSTANCE hInst, HWND hwndShellTab, BrowseRedirectCallback browseRouter)
    : m_hInst(hInst)
    , m_hwndShellTab(hwndShellTab)
    , m_browseRouter(browseRouter) {
    m_dpi = SafeGetDpiForWindow(hwndShellTab);
    m_theme = ThemePalette::Current();
}

TabSplitManager::~TabSplitManager() {
    DetachAndRestore();
}

bool TabSplitManager::RegisterHelperClasses(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);

    if (!GetClassInfoExW(hInst, kClsSplitter, &wc)) {
        wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = SplitterWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_SIZEWE);
        wc.lpszClassName = kClsSplitter;
        if (!RegisterClassExW(&wc)) return false;
    }

    if (!GetClassInfoExW(hInst, kClsFocusBar, &wc)) {
        wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = FocusBarWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClsFocusBar;
        if (!RegisterClassExW(&wc)) return false;
    }

    if (!GetClassInfoExW(hInst, kClsToggleBtn, &wc)) {
        wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = ToggleBtnWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kClsToggleBtn;
        if (!RegisterClassExW(&wc)) return false;
    }

    return RightPaneView::RegisterWindowClasses(hInst);
}

bool TabSplitManager::Attach() {
    if (!m_hwndShellTab || !IsWindow(m_hwndShellTab)) return false;
    if (!RegisterHelperClasses(m_hInst)) return false;

    if (!m_tabSubclassed) {
        if (!SetWindowSubclass(m_hwndShellTab, ShellTabSubclassProc, kSubclassIdTab, reinterpret_cast<DWORD_PTR>(this))) {
            return false;
        }
        m_tabSubclassed = true;
    }

    if (m_browseRouter) {
        if (ComPtr<IShellBrowser> spBrowser = FindShellBrowserForTab(m_hwndShellTab)) {
            EnsureBrowseObjectHookInstalled(spBrowser.Get(), m_browseRouter);
        }
    }

    EnsureHelperWindows();
    SyncDuiViews();
    ApplyLayout();
    return true;
}

void TabSplitManager::DetachAndRestore() {
    BoolGuard guard(m_inInternalLayout);
    m_isSplitActive = false;

    // 1. Remove ALL subclasses FIRST so resizing/destroying children cannot re-trigger layout callbacks!
    if (m_tabSubclassed && m_hwndShellTab && IsWindow(m_hwndShellTab)) {
        RemoveWindowSubclass(m_hwndShellTab, ShellTabSubclassProc, kSubclassIdTab);
    }
    m_tabSubclassed = false;

    std::vector<HWND> duiList(m_subclassedDuiViews.begin(), m_subclassedDuiViews.end());
    m_subclassedDuiViews.clear();
    for (HWND hDui : duiList) {
        if (hDui && IsWindow(hDui)) {
            RemoveWindowSubclass(hDui, DuiViewSubclassProc, kSubclassIdDui);
        }
    }

    // 2. Destroy all Split View child controls
    if (m_rightPane) {
        m_rightPane->Destroy();
        m_rightPane.reset();
    }

    if (m_hwndToggleTooltip && IsWindow(m_hwndToggleTooltip)) {
        DestroyWindow(m_hwndToggleTooltip);
    }
    m_hwndToggleTooltip = nullptr;

    if (m_hwndToggleBtn && IsWindow(m_hwndToggleBtn)) {
        SetWindowLongPtrW(m_hwndToggleBtn, GWLP_USERDATA, 0);
        DestroyWindow(m_hwndToggleBtn);
    }
    m_hwndToggleBtn = nullptr;

    if (m_hwndSplitter && IsWindow(m_hwndSplitter)) {
        SetWindowLongPtrW(m_hwndSplitter, GWLP_USERDATA, 0);
        DestroyWindow(m_hwndSplitter);
    }
    m_hwndSplitter = nullptr;

    if (m_hwndFocusBar && IsWindow(m_hwndFocusBar)) {
        SetWindowLongPtrW(m_hwndFocusBar, GWLP_USERDATA, 0);
        DestroyWindow(m_hwndFocusBar);
    }
    m_hwndFocusBar = nullptr;

    // 3. Restore original DUIView styles and 100% bounds
    RECT rcTab{};
    int tabW = 0;
    int tabH = 0;
    if (m_hwndShellTab && IsWindow(m_hwndShellTab)) {
        GetClientRect(m_hwndShellTab, &rcTab);
        tabW = rcTab.right - rcTab.left;
        tabH = rcTab.bottom - rcTab.top;
    }

    for (HWND hDui : duiList) {
        if (hDui && IsWindow(hDui)) {
            if (m_clipSiblingsModified.count(hDui)) {
                LONG_PTR st = GetWindowLongPtrW(hDui, GWL_STYLE);
                SetWindowLongPtrW(hDui, GWL_STYLE, st & ~static_cast<LONG_PTR>(WS_CLIPSIBLINGS));
            }
            if (tabW > 0 && tabH > 0 && IsWindowVisible(hDui)) {
                SetWindowPos(hDui, nullptr, 0, 0, tabW, tabH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        }
    }
    m_clipSiblingsModified.clear();
    m_hwndDuiView = nullptr;
}

void TabSplitManager::ToggleSplit() {
    SetSplitActive(!m_isSplitActive);
}

void TabSplitManager::SetSplitActive(bool active) {
    if (m_isSplitActive == active) return;

    EnsureHelperWindows();
    SyncDuiViews();

    if (m_browseRouter) {
        if (ComPtr<IShellBrowser> spBrowser = FindShellBrowserForTab(m_hwndShellTab)) {
            EnsureBrowseObjectHookInstalled(spBrowser.Get(), m_browseRouter);
        }
    }

    if (active) {
        UniquePidl leftPidl = QueryLeftPaneFolderPidl(m_hwndShellTab);

        RECT rcTab{};
        GetClientRect(m_hwndShellTab, &rcTab);
        int totalW = std::max<int>(200, rcTab.right - rcTab.left);
        int totalH = std::max<int>(200, rcTab.bottom - rcTab.top);
        int splitterW = ScaleForDpi(6, m_dpi);
        int focusBarH = ScaleForDpi(3, m_dpi);
        int leftW = static_cast<int>((totalW - splitterW) * m_splitRatio);
        RECT rcRight{ leftW + splitterW, focusBarH, totalW, totalH };

        if (!m_rightPane) {
            m_rightPane = std::make_unique<RightPaneView>();
        }

        if (!m_rightPane->IsCreated()) {
            bool ok = m_rightPane->Create(
                m_hInst,
                m_hwndShellTab,
                rcRight,
                leftPidl.Get(),
                [this]() { SetSplitActive(false); },
                [this]() { SyncRightPaneToLeft(); },
                [this]() { RefreshFocusIndicator(); }
            );
            if (!ok) {
                m_rightPane.reset();
                return;
            }
        } else {
            if (leftPidl) {
                m_rightPane->NavigateToPidl(leftPidl.Get());
            }
            m_rightPane->Show(true);
        }

        m_isSplitActive = true;
        m_activePaneSide = 1;
        ApplyLayout();
        m_rightPane->FocusShellView();
        RefreshFocusIndicator();
    } else {
        bool wasRightFocused = (m_rightPane && m_rightPane->ContainsFocus());
        m_isSplitActive = false;
        m_activePaneSide = 0;
        ApplyLayout();
        if (wasRightFocused) {
            FocusLeftPane();
        }
    }
}

void TabSplitManager::ResetSplitRatio() {
    m_splitRatio = 0.50;
    if (m_isSplitActive) {
        ApplyLayout();
    }
}

void TabSplitManager::SetActivePaneSide(int side) {
    if (side != 0 && side != 1) return;
    if (m_activePaneSide != side) {
        m_activePaneSide = side;
        if (m_hwndFocusBar && IsWindow(m_hwndFocusBar)) {
            InvalidateRect(m_hwndFocusBar, nullptr, FALSE);
        }
    }
}

void TabSplitManager::NotifyPointerDownOnHwnd(HWND hwndClicked) {
    if (!m_isSplitActive || !hwndClicked || !IsWindow(hwndClicked)) return;

    if (m_rightPane && m_rightPane->IsCreated()) {
        HWND hRight = m_rightPane->GetHwnd();
        if (hwndClicked == hRight || IsChild(hRight, hwndClicked)) {
            SetActivePaneSide(1);
            return;
        }
    }

    if (hwndClicked == m_hwndSplitter) {
        return;
    }

    if (IsChild(m_hwndShellTab, hwndClicked)) {
        // Clicking the shared Left Navigation Sidebar MUST keep the current active pane!
        if (HasAncestorWithClass(hwndClicked, m_hwndShellTab, L"NamespaceTreeControl")) {
            return;
        }
        SetActivePaneSide(0);
    }
}

void TabSplitManager::RefreshFocusIndicator() {
    if (!m_isSplitActive || !m_hwndFocusBar || !IsWindow(m_hwndFocusBar)) return;

    HWND hFocus = GetFocus();
    if (!hFocus) return;

    if (m_rightPane && m_rightPane->ContainsFocus()) {
        SetActivePaneSide(1);
    } else if (IsChild(m_hwndShellTab, hFocus)) {
        // Focus moving to NamespaceTreeControl (Left Sidebar) preserves active pane (Left or Right)!
        if (!HasAncestorWithClass(hFocus, m_hwndShellTab, L"NamespaceTreeControl") &&
            hFocus != m_hwndSplitter && hFocus != m_hwndFocusBar && hFocus != m_hwndToggleBtn) {
            SetActivePaneSide(0);
        }
    }
}

bool TabSplitManager::TryRedirectBrowseToActivePane(PCUIDLIST_RELATIVE pidl, UINT wFlags) {
    if (!m_isSplitActive || !m_rightPane || !m_rightPane->IsCreated()) {
        return false;
    }
    if (m_activePaneSide != 1) {
        return false;
    }
    if (wFlags & SBSP_NEWBROWSER) {
        return false;
    }

    // Safety check: if focus is genuinely inside Left Pane's file view (not Sidebar, not Right Pane),
    // treat Left Pane as active so double-clicking folders in Left File List opens on the Left.
    HWND hFocus = GetFocus();
    if (hFocus && IsChild(m_hwndShellTab, hFocus) &&
        !m_rightPane->ContainsFocus() &&
        !HasAncestorWithClass(hFocus, m_hwndShellTab, L"NamespaceTreeControl")) {
        SetActivePaneSide(0);
        return false;
    }

    if (wFlags & SBSP_NAVIGATEBACK) {
        m_rightPane->NavigateBack();
        PostMessageW(m_hwndShellTab, kWmRestoreRightFocus, 0, 0);
        return true;
    }
    if (wFlags & SBSP_NAVIGATEFORWARD) {
        m_rightPane->NavigateForward();
        PostMessageW(m_hwndShellTab, kWmRestoreRightFocus, 0, 0);
        return true;
    }
    if (wFlags & SBSP_PARENT) {
        m_rightPane->NavigateUp();
        PostMessageW(m_hwndShellTab, kWmRestoreRightFocus, 0, 0);
        return true;
    }

    if (pidl != nullptr) {
        bool navigated = false;
        if (wFlags & SBSP_RELATIVE) {
            UniquePidl base = QueryLeftPaneFolderPidl(m_hwndShellTab);
            if (base) {
                UniquePidl combined(ILCombine(base.Get(), pidl));
                if (combined) {
                    navigated = m_rightPane->NavigateToPidl(combined.Get());
                }
            }
        } else {
            navigated = m_rightPane->NavigateToPidl(reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl));
        }

        if (navigated) {
            PostMessageW(m_hwndShellTab, kWmRestoreRightFocus, 0, 0);
            return true;
        }
    }

    return false;
}

void TabSplitManager::OnChildCreatedOrSized() {
    EnsureHelperWindows();
    SyncDuiViews();
    ApplyLayout();
}

bool TabSplitManager::PreTranslateMessage(MSG* pMsg) {
    if (!m_isSplitActive || !m_rightPane || !m_rightPane->IsCreated()) return false;
    return m_rightPane->PreTranslateMessage(pMsg);
}

void TabSplitManager::SyncDuiViews() {
    if (!m_hwndShellTab || !IsWindow(m_hwndShellTab)) return;

    // Prune dead HWNDs
    for (auto it = m_subclassedDuiViews.begin(); it != m_subclassedDuiViews.end(); ) {
        if (!IsWindow(*it)) {
            m_clipSiblingsModified.erase(*it);
            it = m_subclassedDuiViews.erase(it);
        } else {
            ++it;
        }
    }

    // Subclass EVERY DUIViewWndClassName child (including newly created ones during folder navigation!)
    HWND hChild = nullptr;
    HWND visibleDui = nullptr;
    HWND firstDui = nullptr;

    while ((hChild = FindWindowExW(m_hwndShellTab, hChild, L"DUIViewWndClassName", nullptr)) != nullptr) {
        if (!firstDui) firstDui = hChild;
        if (!visibleDui && IsWindowVisible(hChild)) visibleDui = hChild;

        LONG_PTR style = GetWindowLongPtrW(hChild, GWL_STYLE);
        if (!(style & WS_CLIPSIBLINGS)) {
            SetWindowLongPtrW(hChild, GWL_STYLE, style | WS_CLIPSIBLINGS);
            m_clipSiblingsModified.insert(hChild);
        }

        if (m_subclassedDuiViews.find(hChild) == m_subclassedDuiViews.end()) {
            if (SetWindowSubclass(hChild, DuiViewSubclassProc, kSubclassIdDui, reinterpret_cast<DWORD_PTR>(this))) {
                m_subclassedDuiViews.insert(hChild);
            }
        }
    }

    m_hwndDuiView = visibleDui ? visibleDui : firstDui;

    if (m_browseRouter) {
        if (ComPtr<IShellBrowser> spBrowser = FindShellBrowserForTab(m_hwndShellTab)) {
            EnsureBrowseObjectHookInstalled(spBrowser.Get(), m_browseRouter);
        }
    }
}

void TabSplitManager::EnsureHelperWindows() {
    if (!m_hwndShellTab || !IsWindow(m_hwndShellTab) || m_creatingHelpers) return;
    BoolGuard guard(m_creatingHelpers);

    if (!m_hwndFocusBar || !IsWindow(m_hwndFocusBar)) {
        m_hwndFocusBar = CreateWindowExW(
            0, kClsFocusBar, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 100, ScaleForDpi(3, m_dpi),
            m_hwndShellTab, nullptr, m_hInst, this
        );
    }

    if (!m_hwndSplitter || !IsWindow(m_hwndSplitter)) {
        m_hwndSplitter = CreateWindowExW(
            0, kClsSplitter, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, ScaleForDpi(6, m_dpi), 100,
            m_hwndShellTab, nullptr, m_hInst, this
        );
    }

    if (!m_hwndToggleBtn || !IsWindow(m_hwndToggleBtn)) {
        m_hwndToggleBtn = CreateWindowExW(
            WS_EX_LAYERED, kClsToggleBtn, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, ScaleForDpi(72, m_dpi), ScaleForDpi(24, m_dpi),
            m_hwndShellTab, nullptr, m_hInst, this
        );
        if (m_hwndToggleBtn) {
            SetLayeredWindowAttributes(m_hwndToggleBtn, 0, 255, LWA_ALPHA);
            m_hwndToggleTooltip = CreateWindowExW(
                WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                m_hwndToggleBtn, nullptr, m_hInst, nullptr
            );
            if (m_hwndToggleTooltip) {
                SetWindowTheme(m_hwndToggleTooltip, m_theme.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                TTTOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                ti.hwnd   = m_hwndShellTab;
                ti.uId    = reinterpret_cast<UINT_PTR>(m_hwndToggleBtn);
                ti.lpszText = const_cast<LPWSTR>(L"Toggle Split View (Ctrl+Shift+E)");
                SendMessageW(m_hwndToggleTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            }
        }
    }
}

void TabSplitManager::EnsureOverlayZOrder() {
    BoolGuard guard(m_inInternalLayout);
    if (m_isSplitActive && m_rightPane && m_rightPane->IsCreated()) {
        if (m_hwndFocusBar && IsWindow(m_hwndFocusBar)) {
            SetWindowPos(m_hwndFocusBar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (m_hwndSplitter && IsWindow(m_hwndSplitter)) {
            SetWindowPos(m_hwndSplitter, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (m_rightPane->GetHwnd()) {
            SetWindowPos(m_rightPane->GetHwnd(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    } else if (!m_isSplitActive && m_hwndToggleBtn && IsWindow(m_hwndToggleBtn)) {
        SetWindowPos(m_hwndToggleBtn, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        InvalidateRect(m_hwndToggleBtn, nullptr, FALSE);
    }
}

void TabSplitManager::ApplyLayout() {
    if (!m_hwndShellTab || !IsWindow(m_hwndShellTab)) return;
    SyncDuiViews();

    RECT rcTab{};
    GetClientRect(m_hwndShellTab, &rcTab);
    int totalW = rcTab.right - rcTab.left;
    int totalH = rcTab.bottom - rcTab.top;
    if (totalW <= 40 || totalH <= 30) return;

    BoolGuard guard(m_inInternalLayout);

    if (m_isSplitActive && m_rightPane && m_rightPane->IsCreated()) {
        if (m_hwndToggleBtn && IsWindow(m_hwndToggleBtn)) {
            ShowWindow(m_hwndToggleBtn, SW_HIDE);
        }

        int focusBarH = ScaleForDpi(3, m_dpi);
        int splitterW = ScaleForDpi(6, m_dpi);
        int availW    = std::max(100, totalW - splitterW);
        int minPaneW  = std::max(120, availW / 5);
        int maxPaneW  = std::max(minPaneW, availW - minPaneW);
        int leftW     = std::clamp(static_cast<int>(std::lround(availW * m_splitRatio)), minPaneW, maxPaneW);
        int rightW    = std::max(60, availW - leftW);
        int paneY     = focusBarH;
        int paneH     = std::max(40, totalH - focusBarH);

        m_cachedLeftWidth     = leftW;
        m_cachedSplitterWidth = splitterW;

        // Resize ALL subclassed DUIViews so any incoming/transitioning view also matches leftW
        for (HWND hDui : m_subclassedDuiViews) {
            if (hDui && IsWindow(hDui)) {
                SetWindowPos(hDui, nullptr, 0, paneY, leftW, paneH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        if (m_hwndFocusBar && IsWindow(m_hwndFocusBar)) {
            SetWindowPos(m_hwndFocusBar, HWND_TOP, 0, 0, totalW, focusBarH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            InvalidateRect(m_hwndFocusBar, nullptr, FALSE);
        }
        if (m_hwndSplitter && IsWindow(m_hwndSplitter)) {
            SetWindowPos(m_hwndSplitter, HWND_TOP, leftW, paneY, splitterW, paneH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            InvalidateRect(m_hwndSplitter, nullptr, FALSE);
        }
        if (m_rightPane->GetHwnd()) {
            SetWindowPos(m_rightPane->GetHwnd(), HWND_TOP, leftW + splitterW, paneY, rightW, paneH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
    } else {
        if (m_hwndFocusBar && IsWindow(m_hwndFocusBar)) ShowWindow(m_hwndFocusBar, SW_HIDE);
        if (m_hwndSplitter && IsWindow(m_hwndSplitter)) ShowWindow(m_hwndSplitter, SW_HIDE);
        if (m_rightPane && m_rightPane->IsCreated()) m_rightPane->Show(false);

        for (HWND hDui : m_subclassedDuiViews) {
            if (hDui && IsWindow(hDui) && IsWindowVisible(hDui)) {
                SetWindowPos(hDui, nullptr, 0, 0, totalW, totalH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        if (m_hwndToggleBtn && IsWindow(m_hwndToggleBtn)) {
            int btnW = ScaleForDpi(72, m_dpi);
            int btnH = ScaleForDpi(24, m_dpi);
            int btnX = std::max(4, totalW - btnW - ScaleForDpi(24, m_dpi));
            int btnY = ScaleForDpi(3, m_dpi);
            SetWindowPos(m_hwndToggleBtn, HWND_TOP, btnX, btnY, btnW, btnH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            InvalidateRect(m_hwndToggleBtn, nullptr, FALSE);
        }
    }
}

void TabSplitManager::SyncRightPaneToLeft() {
    if (!m_rightPane || !m_rightPane->IsCreated()) return;
    UniquePidl leftPidl = QueryLeftPaneFolderPidl(m_hwndShellTab);
    if (leftPidl) {
        m_rightPane->NavigateToPidl(leftPidl.Get());
    }
}

void TabSplitManager::FocusLeftPane() {
    ComPtr<IShellBrowser> spBrowser = FindShellBrowserForTab(m_hwndShellTab);
    if (spBrowser) {
        ComPtr<IShellView> spView;
        if (SUCCEEDED(spBrowser->QueryActiveShellView(&spView)) && spView) {
            HWND hwndView = nullptr;
            if (SUCCEEDED(spView->GetWindow(&hwndView)) && hwndView && IsWindow(hwndView)) {
                SetFocus(hwndView);
            }
            spView->UIActivate(SVUIA_ACTIVATE_FOCUS);
        }
    } else if (m_hwndDuiView && IsWindow(m_hwndDuiView)) {
        SetFocus(m_hwndDuiView);
    }
    SetActivePaneSide(0);
}

void TabSplitManager::UpdateThemeAndDpi() {
    m_dpi = SafeGetDpiForWindow(m_hwndShellTab);
    m_theme = ThemePalette::Current();
    if (m_rightPane) m_rightPane->UpdateThemeAndDpi();
    ApplyLayout();
}

LRESULT CALLBACK TabSplitManager::ShellTabSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData
) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    auto* self = reinterpret_cast<TabSplitManager*>(dwRefData);
    if (!self) return DefSubclassProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case kWmRestoreRightFocus: {
        if (self->m_isSplitActive && self->m_rightPane && self->m_rightPane->IsCreated()) {
            self->SetActivePaneSide(1);
            self->m_rightPane->FocusShellView();
        }
        return 0;
    }
    case kWmEnsureLayout: {
        self->OnChildCreatedOrSized();
        self->EnsureOverlayZOrder();
        return 0;
    }
    case WM_SIZE: {
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        self->ApplyLayout();
        return res;
    }
    case WM_PARENTNOTIFY: {
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (LOWORD(wParam) == WM_CREATE) {
            self->OnChildCreatedOrSized();
            PostMessageW(hwnd, kWmEnsureLayout, 0, 0);
        } else if (LOWORD(wParam) == WM_LBUTTONDOWN || LOWORD(wParam) == WM_RBUTTONDOWN) {
            self->RefreshFocusIndicator();
        }
        return res;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DPICHANGED: {
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        self->UpdateThemeAndDpi();
        return res;
    }
    case WM_DESTROY: {
        self->m_isSplitActive = false;
        if (self->m_rightPane) {
            self->m_rightPane->Destroy();
            self->m_rightPane.reset();
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    case WM_NCDESTROY: {
        self->m_tabSubclassed = false;
        RemoveWindowSubclass(hwnd, ShellTabSubclassProc, kSubclassIdTab);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TabSplitManager::DuiViewSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData
) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    auto* self = reinterpret_cast<TabSplitManager*>(dwRefData);
    if (!self) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (msg == WM_SHOWWINDOW && wParam == TRUE && !self->m_inInternalLayout) {
        self->m_hwndDuiView = hwnd;
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        self->ApplyLayout();
        PostMessageW(self->m_hwndShellTab, kWmEnsureLayout, 0, 0);
        return res;
    }

    if (msg == WM_WINDOWPOSCHANGING && !self->m_inInternalLayout) {
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if (wp) {
            if ((wp->flags & SWP_SHOWWINDOW) || IsWindowVisible(hwnd)) {
                self->m_hwndDuiView = hwnd;
            }

            // 1. Always lock Z-order so DUIViewWndClassName NEVER covers [Split] button or Split panes!
            if (!(wp->flags & SWP_NOZORDER)) {
                if (self->m_isSplitActive && self->m_rightPane && self->m_rightPane->IsCreated() && self->m_rightPane->GetHwnd()) {
                    wp->hwndInsertAfter = self->m_rightPane->GetHwnd();
                } else if (!self->m_isSplitActive && self->m_hwndToggleBtn && IsWindow(self->m_hwndToggleBtn)) {
                    wp->hwndInsertAfter = self->m_hwndToggleBtn;
                }
            }

            // 2. Lock Left Pane dimensions whenever size, position, or visibility changes
            if (!(wp->flags & SWP_NOSIZE) || !(wp->flags & SWP_NOMOVE) || (wp->flags & SWP_SHOWWINDOW)) {
                RECT rcTab{};
                GetClientRect(self->m_hwndShellTab, &rcTab);
                int totalW = rcTab.right - rcTab.left;
                int totalH = rcTab.bottom - rcTab.top;
                if (totalW <= 0 && !(wp->flags & SWP_NOSIZE)) totalW = wp->cx;
                if (totalH <= 0 && !(wp->flags & SWP_NOSIZE)) totalH = wp->cy;

                if (self->m_isSplitActive && self->m_rightPane && self->m_rightPane->IsCreated() && totalW > 80 && totalH > 40) {
                    int focusBarH = ScaleForDpi(3, self->m_dpi);
                    int splitterW = ScaleForDpi(6, self->m_dpi);
                    int availW    = std::max(100, totalW - splitterW);
                    int minPaneW  = std::max(120, availW / 5);
                    int maxPaneW  = std::max(minPaneW, availW - minPaneW);
                    int leftW     = std::clamp(static_cast<int>(std::lround(availW * self->m_splitRatio)), minPaneW, maxPaneW);
                    int rightW    = std::max(60, availW - leftW);
                    int paneY     = focusBarH;
                    int paneH     = std::max(40, totalH - focusBarH);

                    self->m_cachedLeftWidth     = leftW;
                    self->m_cachedSplitterWidth = splitterW;

                    wp->x  = 0;
                    wp->y  = paneY;
                    wp->cx = leftW;
                    wp->cy = paneH;
                    wp->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);

                    BoolGuard guard(self->m_inInternalLayout);
                    if (self->m_hwndFocusBar && IsWindow(self->m_hwndFocusBar)) {
                        SetWindowPos(self->m_hwndFocusBar, HWND_TOP, 0, 0, totalW, focusBarH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
                    }
                    if (self->m_hwndSplitter && IsWindow(self->m_hwndSplitter)) {
                        SetWindowPos(self->m_hwndSplitter, HWND_TOP, leftW, paneY, splitterW, paneH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
                    }
                    self->m_rightPane->SetBounds(leftW + splitterW, paneY, rightW, paneH);
                } else if (!self->m_isSplitActive && self->m_hwndToggleBtn && IsWindow(self->m_hwndToggleBtn) && totalW > 80) {
                    BoolGuard guard(self->m_inInternalLayout);
                    int btnW = ScaleForDpi(72, self->m_dpi);
                    int btnH = ScaleForDpi(24, self->m_dpi);
                    int btnX = std::max(4, totalW - btnW - ScaleForDpi(24, self->m_dpi));
                    int btnY = ScaleForDpi(3, self->m_dpi);
                    SetWindowPos(self->m_hwndToggleBtn, HWND_TOP, btnX, btnY, btnW, btnH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
                }
            }
        }
    } else if (msg == WM_WINDOWPOSCHANGED && !self->m_inInternalLayout) {
        LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
        self->EnsureOverlayZOrder();
        return res;
    } else if (msg == WM_NCDESTROY) {
        self->m_subclassedDuiViews.erase(hwnd);
        self->m_clipSiblingsModified.erase(hwnd);
        if (self->m_hwndDuiView == hwnd) {
            self->m_hwndDuiView = nullptr;
        }
        RemoveWindowSubclass(hwnd, DuiViewSubclassProc, kSubclassIdDui);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TabSplitManager::SplitterWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TabSplitManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TabSplitManager*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TabSplitManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleSplitterMsg(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TabSplitManager::HandleSplitterMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        COLORREF bg = (m_draggingSplitter || m_hoverSplitter) ? m_theme.splitterHoverBg : m_theme.splitterBg;
        HBRUSH hbr = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);

        int pillW = std::max(2, ScaleForDpi(2, m_dpi));
        int pillH = ScaleForDpi(28, m_dpi);
        int px = (w - pillW) / 2;
        int py = (h - pillH) / 2;
        RECT rcPill{ px, py, px + pillW, py + pillH };
        COLORREF gripCol = m_draggingSplitter ? m_theme.accent : m_theme.splitterGrip;
        HBRUSH hbrGrip = CreateSolidBrush(gripCol);
        FillRect(hdc, &rcPill, hbrGrip);
        DeleteObject(hbrGrip);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!m_trackingSplitterMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            m_trackingSplitterMouse = true;
        }
        if (!m_hoverSplitter) {
            m_hoverSplitter = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (m_draggingSplitter && m_hwndShellTab) {
            POINT ptScreen{};
            GetCursorPos(&ptScreen);
            ScreenToClient(m_hwndShellTab, &ptScreen);

            RECT rcTab{};
            GetClientRect(m_hwndShellTab, &rcTab);
            int totalW = rcTab.right - rcTab.left;
            int splitterW = ScaleForDpi(6, m_dpi);
            int availW = std::max(100, totalW - splitterW);
            double newRatio = static_cast<double>(ptScreen.x - splitterW / 2) / static_cast<double>(availW);
            newRatio = std::clamp(newRatio, 0.20, 0.80);
            if (std::abs(newRatio - m_splitRatio) > 0.002) {
                m_splitRatio = newRatio;
                ApplyLayout();
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        m_trackingSplitterMouse = false;
        if (m_hoverSplitter) {
            m_hoverSplitter = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        m_draggingSplitter = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_draggingSplitter) {
            m_draggingSplitter = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        m_draggingSplitter = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        ResetSplitRatio();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TabSplitManager::FocusBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TabSplitManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TabSplitManager*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TabSplitManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleFocusBarMsg(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TabSplitManager::HandleFocusBarMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        HBRUSH hbrInactive = CreateSolidBrush(m_theme.inactiveBar);
        HBRUSH hbrAccent   = CreateSolidBrush(m_theme.accent);

        FillRect(hdc, &rc, hbrInactive);

        if (m_activePaneSide == 0) {
            RECT rcLeft{ 0, 0, m_cachedLeftWidth, rc.bottom };
            FillRect(hdc, &rcLeft, hbrAccent);
        } else {
            RECT rcRight{ m_cachedLeftWidth + m_cachedSplitterWidth, 0, rc.right, rc.bottom };
            FillRect(hdc, &rcRight, hbrAccent);
        }

        DeleteObject(hbrInactive);
        DeleteObject(hbrAccent);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TabSplitManager::ToggleBtnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TabSplitManager* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TabSplitManager*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TabSplitManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleToggleBtnMsg(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TabSplitManager::HandleToggleBtnMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HBRUSH hbrParentBg = CreateSolidBrush(m_theme.paneBg);
        FillRect(hdc, &rc, hbrParentBg);
        DeleteObject(hbrParentBg);

        COLORREF fillCol = m_pressToggleBtn ? m_theme.btnPressBg
                         : (m_hoverToggleBtn ? m_theme.btnHoverBg : m_theme.headerBg);
        COLORREF borderCol = m_hoverToggleBtn ? m_theme.accent : m_theme.editBorder;

        HBRUSH hbrBtn = CreateSolidBrush(fillCol);
        HPEN hPenBtn  = CreatePen(PS_SOLID, 1, borderCol);
        HGDIOBJ ob = SelectObject(hdc, hbrBtn);
        HGDIOBJ op = SelectObject(hdc, hPenBtn);
        int rad = ScaleForDpi(5, m_dpi);
        RoundRect(hdc, 0, 0, w, h, rad, rad);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(hbrBtn);
        DeleteObject(hPenBtn);

        int iconW = ScaleForDpi(13, m_dpi);
        int iconH = ScaleForDpi(11, m_dpi);
        int ix = ScaleForDpi(8, m_dpi);
        int iy = (h - iconH) / 2;
        COLORREF iconCol = m_hoverToggleBtn ? m_theme.accent : m_theme.textPrimary;
        HPEN hPenIcon = CreatePen(PS_SOLID, 1, iconCol);
        HGDIOBJ oldBrIcon = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ oldPenIcon = SelectObject(hdc, hPenIcon);
        RoundRect(hdc, ix, iy, ix + iconW, iy + iconH, 2, 2);
        int midX = ix + iconW / 2;
        MoveToEx(hdc, midX, iy + 1, nullptr);
        LineTo(hdc, midX, iy + iconH - 1);
        SelectObject(hdc, oldBrIcon);
        SelectObject(hdc, oldPenIcon);
        DeleteObject(hPenIcon);

        HFONT hFontLbl = CreateFontW(
            -ScaleForDpi(12, m_dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
        );
        HGDIOBJ oldFont = SelectObject(hdc, hFontLbl);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, iconCol);
        RECT rcLbl{ ix + iconW + ScaleForDpi(5, m_dpi), 0, w - ScaleForDpi(4, m_dpi), h };
        DrawTextW(hdc, L"Split", -1, &rcLbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hFontLbl);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!m_trackingToggleMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            m_trackingToggleMouse = true;
        }
        if (!m_hoverToggleBtn) {
            m_hoverToggleBtn = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        m_trackingToggleMouse = false;
        m_hoverToggleBtn = false;
        m_pressToggleBtn = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        m_pressToggleBtn = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        bool wasPressed = m_pressToggleBtn;
        m_pressToggleBtn = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (wasPressed && PtInRect(&rc, pt)) {
            SetSplitActive(true);
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace ExplorerSplit
