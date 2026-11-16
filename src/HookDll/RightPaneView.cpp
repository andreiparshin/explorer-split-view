#include "RightPaneView.h"
#include <windowsx.h>
#include <atomic>

namespace ExplorerSplit {

namespace {

class BrowserEventsSink final : public IExplorerBrowserEvents {
public:
    explicit BrowserEventsSink(RightPaneView* owner) : m_owner(owner) {}

    void Detach() { m_owner = nullptr; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerBrowserEvents) {
            *ppv = static_cast<IExplorerBrowserEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return ++m_refCount;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG res = --m_refCount;
        if (res == 0) delete this;
        return res;
    }

    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override {
        return S_OK;
    }

    IFACEMETHODIMP OnViewCreated(IShellView* psv) override {
        UNREFERENCED_PARAMETER(psv);
        return S_OK;
    }

    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE pidlFolder) override {
        if (m_owner) {
            m_owner->OnBrowserNavigationComplete(pidlFolder);
        }
        return S_OK;
    }

    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override {
        if (m_owner) {
            m_owner->OnBrowserNavigationFailed();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
    RightPaneView* m_owner = nullptr;
};

bool FontExists(HDC hdc, const wchar_t* faceName) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, faceName, _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(
        hdc,
        &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) -> int {
            *reinterpret_cast<bool*>(lParam) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found),
        0
    );
    return found;
}

} // namespace

RightPaneView::RightPaneView() {
    // Fluent icon glyphs (Segoe Fluent Icons / Segoe MDL2 Assets share these codepoints)
    m_buttons[static_cast<int>(HeaderBtnId::Back)]    = { {}, L'\xE72B', L"Back (Alt+Left Arrow)" };
    m_buttons[static_cast<int>(HeaderBtnId::Forward)] = { {}, L'\xE72A', L"Forward (Alt+Right Arrow)" };
    m_buttons[static_cast<int>(HeaderBtnId::Up)]      = { {}, L'\xE74A', L"Up one level (Alt+Up Arrow)" };
    m_buttons[static_cast<int>(HeaderBtnId::Refresh)] = { {}, L'\xE72C', L"Refresh (F5)" };
    m_buttons[static_cast<int>(HeaderBtnId::Sync)]    = { {}, L'\xE8AB', L"Sync with left pane" };
    m_buttons[static_cast<int>(HeaderBtnId::Close)]   = { {}, L'\xE711', L"Close split pane (Ctrl+Shift+E)" };
}

RightPaneView::~RightPaneView() {
    Destroy();
}

bool RightPaneView::RegisterWindowClasses(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExW(hInst, kClsRightPane, &wc)) {
        return true;
    }

    wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = ContainerWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // Custom double-buffered header paint
    wc.lpszClassName = kClsRightPane;

    return RegisterClassExW(&wc) != 0;
}

bool RightPaneView::Create(
    HINSTANCE hInst,
    HWND hwndParentTab,
    const RECT& rcBounds,
    PCIDLIST_ABSOLUTE initialPidl,
    std::function<void()> onCloseRequested,
    std::function<void()> onSyncFromLeftRequested,
    std::function<void()> onFocusActivity
) {
    if (IsCreated()) return true;
    if (!RegisterWindowClasses(hInst)) return false;

    m_hInst = hInst;
    m_hwndParentTab = hwndParentTab;
    m_onCloseRequested = std::move(onCloseRequested);
    m_onSyncFromLeftRequested = std::move(onSyncFromLeftRequested);
    m_onFocusActivity = std::move(onFocusActivity);

    m_dpi = SafeGetDpiForWindow(hwndParentTab);
    m_theme = ThemePalette::Current();
    RecreateFontsAndBrushes();

    int w = std::max<int>(100, rcBounds.right - rcBounds.left);
    int h = std::max<int>(100, rcBounds.bottom - rcBounds.top);

    m_hwndContainer = CreateWindowExW(
        0,
        kClsRightPane,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        rcBounds.left,
        rcBounds.top,
        w,
        h,
        hwndParentTab,
        nullptr,
        hInst,
        this
    );
    if (!m_hwndContainer) {
        Destroy();
        return false;
    }

    // Create borderless inner Edit control for folder path
    m_hwndPathEdit = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 100, 20,
        m_hwndContainer,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
        hInst,
        nullptr
    );
    if (m_hwndPathEdit) {
        SendMessageW(m_hwndPathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontText), TRUE);
        SetWindowTheme(m_hwndPathEdit, m_theme.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
        SetWindowSubclass(m_hwndPathEdit, PathEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    EnsureTooltip(hInst);

    // Instantiate official Windows Shell IExplorerBrowser
    HRESULT hr = CoCreateInstance(
        CLSID_ExplorerBrowser,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_spExplorerBrowser)
    );
    if (FAILED(hr) || !m_spExplorerBrowser) {
        Destroy();
        return false;
    }

    m_spExplorerBrowser->SetOptions(EBO_NOBORDER);

    int headerH = ScaleForDpi(36, m_dpi);
    RECT rcBrowser{ 0, headerH, w, std::max(headerH + 10, h) };
    FOLDERSETTINGS fs{};
    fs.ViewMode = FVM_DETAILS;
    fs.fFlags   = FWF_AUTOARRANGE | FWF_SHOWSELALWAYS;

    hr = m_spExplorerBrowser->Initialize(m_hwndContainer, &rcBrowser, &fs);
    if (FAILED(hr)) {
        Destroy();
        return false;
    }

    auto* rawSink = new BrowserEventsSink(this);
    m_spEventsSink.Attach(rawSink); // Takes initial refCount=1
    m_spExplorerBrowser->Advise(m_spEventsSink.Get(), &m_adviseCookie);

    LayoutInternal();

    if (initialPidl) {
        NavigateToPidl(initialPidl);
    } else {
        UniquePidl fallback = QueryLeftPaneFolderPidl(hwndParentTab);
        if (fallback) NavigateToPidl(fallback.Get());
    }

    return true;
}

void RightPaneView::Destroy() {
    if (m_spEventsSink) {
        static_cast<BrowserEventsSink*>(m_spEventsSink.Get())->Detach();
    }

    if (m_spExplorerBrowser) {
        if (m_adviseCookie != 0) {
            m_spExplorerBrowser->Unadvise(m_adviseCookie);
            m_adviseCookie = 0;
        }
        m_spExplorerBrowser->Destroy();
        m_spExplorerBrowser.Reset();
    }
    m_spEventsSink.Reset();

    if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
        RemoveWindowSubclass(m_hwndPathEdit, PathEditSubclassProc, 1);
        DestroyWindow(m_hwndPathEdit);
    }
    m_hwndPathEdit = nullptr;

    if (m_hwndTooltip && IsWindow(m_hwndTooltip)) {
        DestroyWindow(m_hwndTooltip);
    }
    m_hwndTooltip = nullptr;

    if (m_hwndContainer && IsWindow(m_hwndContainer)) {
        SetWindowLongPtrW(m_hwndContainer, GWLP_USERDATA, 0);
        DestroyWindow(m_hwndContainer);
    }
    m_hwndContainer = nullptr;

    ReleaseFontsAndBrushes();
}

bool RightPaneView::ContainsFocus() const {
    if (!m_hwndContainer || !IsWindow(m_hwndContainer)) return false;
    HWND hFocus = GetFocus();
    return hFocus && (hFocus == m_hwndContainer || IsChild(m_hwndContainer, hFocus));
}

void RightPaneView::SetBounds(int x, int y, int width, int height) {
    if (!IsCreated()) return;
    SetWindowPos(
        m_hwndContainer,
        HWND_TOP,
        x, y,
        std::max(50, width),
        std::max(50, height),
        SWP_NOACTIVATE
    );
}

void RightPaneView::Show(bool visible) {
    if (!IsCreated()) return;
    ShowWindow(m_hwndContainer, visible ? SW_SHOW : SW_HIDE);
}

void RightPaneView::UpdateThemeAndDpi() {
    if (!IsCreated()) return;
    m_dpi = SafeGetDpiForWindow(m_hwndContainer);
    m_theme = ThemePalette::Current();
    RecreateFontsAndBrushes();
    if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
        SendMessageW(m_hwndPathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontText), TRUE);
        SetWindowTheme(m_hwndPathEdit, m_theme.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    }
    LayoutInternal();
    InvalidateRect(m_hwndContainer, nullptr, TRUE);
}

bool RightPaneView::NavigateToPidl(PCIDLIST_ABSOLUTE pidl) {
    if (!m_spExplorerBrowser || !pidl) return false;
    HRESULT hr = m_spExplorerBrowser->BrowseToIDList(pidl, SBSP_ABSOLUTE);
    if (SUCCEEDED(hr)) {
        m_currentPidl.Reset(ILCloneFull(pidl));
        if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
            std::wstring formatted = FormatPidlForAddressBar(pidl);
            SetWindowTextW(m_hwndPathEdit, formatted.c_str());
        }
        return true;
    }
    return false;
}

bool RightPaneView::NavigateToPathText() {
    if (!m_hwndPathEdit || !IsWindow(m_hwndPathEdit)) return false;
    int len = GetWindowTextLengthW(m_hwndPathEdit);
    std::wstring text(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
    if (len > 0) {
        GetWindowTextW(m_hwndPathEdit, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));
    } else {
        text.clear();
    }

    UniquePidl target = ParseUserPathToPidl(text);
    if (target && NavigateToPidl(target.Get())) {
        return true;
    }

    // Invalid path: flash gently via beep-free restore
    OnBrowserNavigationFailed();
    return false;
}

void RightPaneView::NavigateBack() {
    if (m_spExplorerBrowser) {
        m_spExplorerBrowser->BrowseToIDList(nullptr, SBSP_NAVIGATEBACK);
    }
}

void RightPaneView::NavigateForward() {
    if (m_spExplorerBrowser) {
        m_spExplorerBrowser->BrowseToIDList(nullptr, SBSP_NAVIGATEFORWARD);
    }
}

void RightPaneView::NavigateUp() {
    if (m_spExplorerBrowser) {
        m_spExplorerBrowser->BrowseToIDList(nullptr, SBSP_PARENT);
    }
}

void RightPaneView::RefreshView() {
    if (!m_spExplorerBrowser) return;
    ComPtr<IShellView> spView;
    if (SUCCEEDED(m_spExplorerBrowser->GetCurrentView(IID_PPV_ARGS(&spView))) && spView) {
        spView->Refresh();
    }
}

void RightPaneView::FocusShellView() {
    if (!m_spExplorerBrowser) return;
    ComPtr<IShellView> spView;
    if (SUCCEEDED(m_spExplorerBrowser->GetCurrentView(IID_PPV_ARGS(&spView))) && spView) {
        HWND hwndView = nullptr;
        if (SUCCEEDED(spView->GetWindow(&hwndView)) && hwndView && IsWindow(hwndView)) {
            SetFocus(hwndView);
        }
        spView->UIActivate(SVUIA_ACTIVATE_FOCUS);
    }
    if (m_onFocusActivity) m_onFocusActivity();
}

bool RightPaneView::PreTranslateMessage(MSG* pMsg) {
    if (!pMsg || !IsCreated()) return false;
    if (pMsg->message < WM_KEYFIRST || pMsg->message > WM_KEYLAST) return false;

    HWND hFocus = GetFocus();
    if (!hFocus || (hFocus != m_hwndContainer && !IsChild(m_hwndContainer, hFocus))) {
        return false;
    }

    // 1. Address bar Edit box has focus
    if (hFocus == m_hwndPathEdit) {
        if (pMsg->message == WM_KEYDOWN) {
            if (pMsg->wParam == VK_RETURN) {
                NavigateToPathText();
                FocusShellView();
                pMsg->message = WM_NULL;
                return true;
            }
            if (pMsg->wParam == VK_ESCAPE) {
                OnBrowserNavigationFailed();
                FocusShellView();
                pMsg->message = WM_NULL;
                return true;
            }
            if (pMsg->wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                SendMessageW(m_hwndPathEdit, EM_SETSEL, 0, -1);
                pMsg->message = WM_NULL;
                return true;
            }
        }
        TranslateMessage(pMsg);
        DispatchMessageW(pMsg);
        pMsg->message = WM_NULL;
        return true;
    }

    // 2. Navigation shortcuts while inside Right Pane file view (when not inline-renaming)
    wchar_t focusCls[64]{};
    GetClassNameW(hFocus, focusCls, static_cast<int>(std::size(focusCls)));
    bool isInlineEdit = (_wcsicmp(focusCls, L"Edit") == 0);

    if (!isInlineEdit) {
        if (pMsg->message == WM_SYSKEYDOWN) {
            if (pMsg->wParam == VK_LEFT) {
                NavigateBack();
                pMsg->message = WM_NULL;
                return true;
            }
            if (pMsg->wParam == VK_RIGHT) {
                NavigateForward();
                pMsg->message = WM_NULL;
                return true;
            }
            if (pMsg->wParam == VK_UP) {
                NavigateUp();
                pMsg->message = WM_NULL;
                return true;
            }
        } else if (pMsg->message == WM_KEYDOWN) {
            if (pMsg->wParam == VK_F5) {
                RefreshView();
                pMsg->message = WM_NULL;
                return true;
            }
            if (pMsg->wParam == 'L' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
                    SetFocus(m_hwndPathEdit);
                    SendMessageW(m_hwndPathEdit, EM_SETSEL, 0, -1);
                }
                pMsg->message = WM_NULL;
                return true;
            }
        }
    }

    // 3. Route ShellView accelerators (Delete, F2, Ctrl+C/X/V/A/Z, Enter) ONLY to Right Pane
    if (m_spExplorerBrowser) {
        ComPtr<IShellView> spView;
        if (SUCCEEDED(m_spExplorerBrowser->GetCurrentView(IID_PPV_ARGS(&spView))) && spView) {
            if (spView->TranslateAcceleratorW(pMsg) == S_OK) {
                pMsg->message = WM_NULL;
                return true;
            }
        }
    }

    if (!isInlineEdit && pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_BACK) {
        NavigateUp();
        pMsg->message = WM_NULL;
        return true;
    }

    // 4. Dispatch directly to focused Right Pane child so Left Pane NEVER steals keys
    TranslateMessage(pMsg);
    DispatchMessageW(pMsg);
    pMsg->message = WM_NULL;
    return true;
}

void RightPaneView::OnBrowserNavigationComplete(PCIDLIST_ABSOLUTE pidlFolder) {
    if (pidlFolder) {
        m_currentPidl.Reset(ILCloneFull(pidlFolder));
    }
    if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
        std::wstring formatted = FormatPidlForAddressBar(m_currentPidl.Get());
        SetWindowTextW(m_hwndPathEdit, formatted.c_str());
    }
}

void RightPaneView::OnBrowserNavigationFailed() {
    if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
        std::wstring formatted = FormatPidlForAddressBar(m_currentPidl.Get());
        SetWindowTextW(m_hwndPathEdit, formatted.c_str());
    }
}

LRESULT CALLBACK RightPaneView::PathEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData
) {
    UNREFERENCED_PARAMETER(uIdSubclass);
    auto* self = reinterpret_cast<RightPaneView*>(dwRefData);
    if (self) {
        if (msg == WM_SETFOCUS) {
            LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
            PostMessageW(hwnd, EM_SETSEL, 0, -1);
            if (self->m_hwndContainer) InvalidateRect(self->m_hwndContainer, &self->m_rcAddressBorder, FALSE);
            if (self->m_onFocusActivity) self->m_onFocusActivity();
            return res;
        }
        if (msg == WM_KILLFOCUS) {
            LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (self->m_hwndContainer) InvalidateRect(self->m_hwndContainer, &self->m_rcAddressBorder, FALSE);
            if (self->m_onFocusActivity) self->m_onFocusActivity();
            return res;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK RightPaneView::ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RightPaneView* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<RightPaneView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwndContainer = hwnd;
    } else {
        self = reinterpret_cast<RightPaneView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT RightPaneView::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        LayoutInternal();
        InvalidateRect(m_hwndContainer, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(m_hwndContainer, &ps);
        RECT rcClient{};
        GetClientRect(m_hwndContainer, &rcClient);
        PaintHeader(hdc, rcClient);
        EndPaint(m_hwndContainer, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdcEdit, m_theme.textPrimary);
        SetBkColor(hdcEdit, m_theme.editBg);
        return reinterpret_cast<LRESULT>(m_hbrEditBg);
    }

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwndContainer, 0 };
            TrackMouseEvent(&tme);
            m_trackingMouse = true;
        }
        HeaderBtnId hit = HitTestButton(pt);
        if (hit != m_hoverBtn) {
            m_hoverBtn = hit;
            RECT rcHdr{ 0, 0, 4096, ScaleForDpi(36, m_dpi) };
            InvalidateRect(m_hwndContainer, &rcHdr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        m_trackingMouse = false;
        if (m_hoverBtn != HeaderBtnId::None || m_pressBtn != HeaderBtnId::None) {
            m_hoverBtn = HeaderBtnId::None;
            m_pressBtn = HeaderBtnId::None;
            RECT rcHdr{ 0, 0, 4096, ScaleForDpi(36, m_dpi) };
            InvalidateRect(m_hwndContainer, &rcHdr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (m_onFocusActivity) m_onFocusActivity();
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HeaderBtnId hit = HitTestButton(pt);
        if (hit != HeaderBtnId::None) {
            m_pressBtn = hit;
            SetCapture(m_hwndContainer);
            RECT rcHdr{ 0, 0, 4096, ScaleForDpi(36, m_dpi) };
            InvalidateRect(m_hwndContainer, &rcHdr, FALSE);
        } else if (PtInRect(&m_rcAddressBorder, pt) && m_hwndPathEdit) {
            SetFocus(m_hwndPathEdit);
        } else {
            FocusShellView();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HeaderBtnId pressed = m_pressBtn;
        m_pressBtn = HeaderBtnId::None;
        if (GetCapture() == m_hwndContainer) {
            ReleaseCapture();
        }
        RECT rcHdr{ 0, 0, 4096, ScaleForDpi(36, m_dpi) };
        InvalidateRect(m_hwndContainer, &rcHdr, FALSE);

        HeaderBtnId hit = HitTestButton(pt);
        if (pressed != HeaderBtnId::None && pressed == hit) {
            ExecuteHeaderButton(hit);
        }
        return 0;
    }

    case WM_PARENTNOTIFY:
    case WM_MOUSEACTIVATE: {
        if (m_onFocusActivity) m_onFocusActivity();
        break;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DPICHANGED_AFTERPARENT: {
        UpdateThemeAndDpi();
        break;
    }
    }

    return DefWindowProcW(m_hwndContainer, msg, wParam, lParam);
}

void RightPaneView::LayoutInternal() {
    if (!m_hwndContainer) return;

    RECT rc{};
    GetClientRect(m_hwndContainer, &rc);
    int w = std::max<int>(60, rc.right - rc.left);
    int h = std::max<int>(60, rc.bottom - rc.top);

    int headerH = ScaleForDpi(36, m_dpi);
    int btnSize = ScaleForDpi(28, m_dpi);
    int padX    = ScaleForDpi(4, m_dpi);
    int gap     = ScaleForDpi(2, m_dpi);
    int btnTop  = (headerH - btnSize) / 2;

    // Left navigation buttons: Back, Forward, Up, Refresh
    int curX = padX;
    for (int i = 0; i <= static_cast<int>(HeaderBtnId::Refresh); ++i) {
        m_buttons[i].rc = { curX, btnTop, curX + btnSize, btnTop + btnSize };
        curX += btnSize + gap;
    }

    // Right buttons: Sync, Close
    int closeRight = w - padX;
    m_buttons[static_cast<int>(HeaderBtnId::Close)].rc = { closeRight - btnSize, btnTop, closeRight, btnTop + btnSize };
    int syncRight = closeRight - btnSize - gap;
    m_buttons[static_cast<int>(HeaderBtnId::Sync)].rc  = { syncRight - btnSize, btnTop, syncRight, btnTop + btnSize };

    // Address bar between Refresh and Sync
    int addrLeft  = curX + ScaleForDpi(4, m_dpi);
    int addrRight = std::max(addrLeft + ScaleForDpi(40, m_dpi), syncRight - btnSize - ScaleForDpi(6, m_dpi));
    int addrTop   = ScaleForDpi(4, m_dpi);
    int addrBot   = headerH - ScaleForDpi(4, m_dpi);
    m_rcAddressBorder = { addrLeft, addrTop, addrRight, addrBot };

    if (m_hwndPathEdit && IsWindow(m_hwndPathEdit)) {
        int innerPadX = ScaleForDpi(8, m_dpi);
        int editH     = ScaleForDpi(18, m_dpi);
        int editY     = addrTop + std::max(1, ((addrBot - addrTop) - editH) / 2);
        int editW     = std::max(10, (addrRight - addrLeft) - innerPadX * 2);
        SetWindowPos(m_hwndPathEdit, nullptr, addrLeft + innerPadX, editY, editW, editH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    UpdateTooltipRects();

    if (m_spExplorerBrowser) {
        RECT rcBrowser{ 0, headerH, w, std::max(headerH + 1, h) };
        m_spExplorerBrowser->SetRect(nullptr, rcBrowser);
    }
}

void RightPaneView::PaintHeader(HDC hdc, const RECT& rcClient) {
    int headerH = ScaleForDpi(36, m_dpi);
    int w = rcClient.right - rcClient.left;
    if (w <= 0) return;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, headerH);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    // Header background
    RECT rcHdr{ 0, 0, w, headerH };
    HBRUSH hbrBg = CreateSolidBrush(m_theme.headerBg);
    FillRect(memDC, &rcHdr, hbrBg);
    DeleteObject(hbrBg);

    // Subtle bottom divider line under header
    RECT rcDiv{ 0, headerH - 1, w, headerH };
    HBRUSH hbrDiv = CreateSolidBrush(m_theme.editBorder);
    FillRect(memDC, &rcDiv, hbrDiv);
    DeleteObject(hbrDiv);

    // Rounded Address Box container
    bool editFocused = (GetFocus() == m_hwndPathEdit);
    COLORREF borderCol = editFocused ? m_theme.editFocusBorder : m_theme.editBorder;
    HBRUSH hbrEdit = CreateSolidBrush(m_theme.editBg);
    HPEN hPenAddr = CreatePen(PS_SOLID, editFocused ? ScaleForDpi(2, m_dpi) : 1, borderCol);
    HGDIOBJ oldBr = SelectObject(memDC, hbrEdit);
    HGDIOBJ oldPen = SelectObject(memDC, hPenAddr);
    int radius = ScaleForDpi(6, m_dpi);
    RoundRect(memDC, m_rcAddressBorder.left, m_rcAddressBorder.top, m_rcAddressBorder.right, m_rcAddressBorder.bottom, radius, radius);
    SelectObject(memDC, oldBr);
    SelectObject(memDC, oldPen);
    DeleteObject(hbrEdit);
    DeleteObject(hPenAddr);

    // Paint Fluent Icon Buttons
    SetBkMode(memDC, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(memDC, m_hFontIcons);

    for (int i = 0; i < static_cast<int>(HeaderBtnId::Count); ++i) {
        auto id = static_cast<HeaderBtnId>(i);
        const RECT& br = m_buttons[i].rc;
        bool hovered = (m_hoverBtn == id);
        bool pressed = (m_pressBtn == id);

        COLORREF glyphColor = m_theme.textPrimary;
        if (hovered || pressed) {
            COLORREF fillCol = pressed ? m_theme.btnPressBg : m_theme.btnHoverBg;
            if (id == HeaderBtnId::Close) {
                fillCol = m_theme.closeHoverBg;
                glyphColor = RGB(255, 255, 255);
            }
            HBRUSH hbrBtn = CreateSolidBrush(fillCol);
            HPEN hPenNull = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ ob = SelectObject(memDC, hbrBtn);
            HGDIOBJ op = SelectObject(memDC, hPenNull);
            int btnRad = ScaleForDpi(5, m_dpi);
            RoundRect(memDC, br.left, br.top, br.right, br.bottom + 1, btnRad, btnRad);
            SelectObject(memDC, ob);
            SelectObject(memDC, op);
            DeleteObject(hbrBtn);
            DeleteObject(hPenNull);
        }

        SetTextColor(memDC, glyphColor);
        wchar_t glyphStr[2]{ m_buttons[i].glyph, L'\0' };
        RECT rcText = br;
        DrawTextW(memDC, glyphStr, 1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(memDC, oldFont);

    BitBlt(hdc, 0, 0, w, headerH, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

void RightPaneView::RecreateFontsAndBrushes() {
    ReleaseFontsAndBrushes();

    m_hbrEditBg = CreateSolidBrush(m_theme.editBg);

    m_hFontText = CreateFontW(
        -ScaleForDpi(13, m_dpi),
        0, 0, 0,
        FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Text"
    );
    if (!m_hFontText) {
        m_hFontText = CreateFontW(
            -ScaleForDpi(13, m_dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
        );
    }

    HDC screenDC = GetDC(nullptr);
    const wchar_t* iconFace = FontExists(screenDC, L"Segoe Fluent Icons")
        ? L"Segoe Fluent Icons"
        : L"Segoe MDL2 Assets";
    ReleaseDC(nullptr, screenDC);

    m_hFontIcons = CreateFontW(
        -ScaleForDpi(13, m_dpi),
        0, 0, 0,
        FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        iconFace
    );
}

void RightPaneView::ReleaseFontsAndBrushes() {
    if (m_hFontText) {
        DeleteObject(m_hFontText);
        m_hFontText = nullptr;
    }
    if (m_hFontIcons) {
        DeleteObject(m_hFontIcons);
        m_hFontIcons = nullptr;
    }
    if (m_hbrEditBg) {
        DeleteObject(m_hbrEditBg);
        m_hbrEditBg = nullptr;
    }
}

RightPaneView::HeaderBtnId RightPaneView::HitTestButton(POINT pt) const {
    for (int i = 0; i < static_cast<int>(HeaderBtnId::Count); ++i) {
        if (PtInRect(&m_buttons[i].rc, pt)) {
            return static_cast<HeaderBtnId>(i);
        }
    }
    return HeaderBtnId::None;
}

void RightPaneView::ExecuteHeaderButton(HeaderBtnId id) {
    switch (id) {
    case HeaderBtnId::Back:
        NavigateBack();
        FocusShellView();
        break;
    case HeaderBtnId::Forward:
        NavigateForward();
        FocusShellView();
        break;
    case HeaderBtnId::Up:
        NavigateUp();
        FocusShellView();
        break;
    case HeaderBtnId::Refresh:
        RefreshView();
        FocusShellView();
        break;
    case HeaderBtnId::Sync:
        if (m_onSyncFromLeftRequested) m_onSyncFromLeftRequested();
        FocusShellView();
        break;
    case HeaderBtnId::Close:
        if (m_onCloseRequested) m_onCloseRequested();
        break;
    default:
        break;
    }
}

void RightPaneView::EnsureTooltip(HINSTANCE hInst) {
    if (m_hwndTooltip || !m_hwndContainer) return;
    m_hwndTooltip = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hwndContainer,
        nullptr,
        hInst,
        nullptr
    );
    if (!m_hwndTooltip) return;

    SetWindowTheme(m_hwndTooltip, m_theme.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);

    for (int i = 0; i < static_cast<int>(HeaderBtnId::Count); ++i) {
        TTTOOLINFOW ti{};
        ti.cbSize   = sizeof(ti);
        ti.uFlags   = TTF_SUBCLASS;
        ti.hwnd     = m_hwndContainer;
        ti.uId      = static_cast<UINT_PTR>(i + 1);
        ti.rect     = m_buttons[i].rc;
        ti.lpszText = const_cast<LPWSTR>(m_buttons[i].tooltip);
        SendMessageW(m_hwndTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void RightPaneView::UpdateTooltipRects() {
    if (!m_hwndTooltip || !IsWindow(m_hwndTooltip)) return;
    for (int i = 0; i < static_cast<int>(HeaderBtnId::Count); ++i) {
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd   = m_hwndContainer;
        ti.uId    = static_cast<UINT_PTR>(i + 1);
        ti.rect   = m_buttons[i].rc;
        SendMessageW(m_hwndTooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

} // namespace ExplorerSplit
