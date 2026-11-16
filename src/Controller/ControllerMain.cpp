#include "../../include/SharedProtocol.h"
#include <objbase.h>
#include <shellapi.h>
#include <unordered_map>
#include <vector>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace ExplorerSplit;

constexpr UINT WM_TRAYICON          = WM_APP + 10;
constexpr UINT WM_DEFERRED_SCAN     = WM_APP + 11;
constexpr UINT WM_FOREGROUND_CHANGE = WM_APP + 12;
constexpr UINT_PTR TIMER_CLEANUP    = 1;
constexpr UINT_PTR TIMER_FG_SYNC    = 2;
constexpr int HOTKEY_SPLIT_TOGGLE   = 0x5350;

enum TrayMenuCmd : UINT {
    CMD_TOGGLE_SPLIT = 2001,
    CMD_RESET_RATIO  = 2002,
    CMD_OPEN_EXPLORER= 2003,
    CMD_AUTORUN      = 2004,
    CMD_EXIT         = 2005
};

struct HookPair {
    HHOOK hCallWnd = nullptr;
    HHOOK hGetMsg  = nullptr;
};

class SplitControllerApp {
public:
    bool Initialize(HINSTANCE hInst, bool silentStartup) {
        m_hInst = hInst;
        m_msgs.Initialize();
        m_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        if (!LoadHookLibrary()) {
            MessageBoxW(
                nullptr,
                L"Failed to load ExplorerSplitHook.dll. Please keep it in the same directory as ExplorerSplitController.exe.",
                L"Explorer Split View",
                MB_ICONERROR | MB_OK
            );
            return false;
        }

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = HiddenWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kControllerWndClass;
        RegisterClassExW(&wc);

        m_hwndHidden = CreateWindowExW(
            0, kControllerWndClass, L"ExplorerSplitViewController",
            WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, hInst, this
        );
        if (!m_hwndHidden) return false;

        m_hTrayIcon = CreateSplitTrayIcon();
        AddTrayIcon(!silentStartup);

        g_instance = this;
        m_winEventCreateShow = SetWinEventHook(
            EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
            nullptr, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );
        m_winEventForeground = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );

        ScanAndHookExplorerWindows();
        UpdateForegroundHotkey();
        SetTimer(m_hwndHidden, TIMER_CLEANUP, 10000, nullptr);
        SetTimer(m_hwndHidden, TIMER_FG_SYNC, 350, nullptr);

        if (!silentStartup) {
            HWND target = FindBestTargetCabinet(m_lastActiveCabinet);
            if (target) {
                EnsureCabinetHooked(target);
                PostMessageW(target, m_msgs.msgToggle, 0, 0);
            } else {
                m_autoSplitNextNewWindow = true;
                ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        return true;
    }

    int RunMessageLoop() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    void Shutdown() {
        if (m_shuttingDown) return;
        m_shuttingDown = true;

        if (m_hwndHidden && IsWindow(m_hwndHidden)) {
            KillTimer(m_hwndHidden, TIMER_CLEANUP);
            KillTimer(m_hwndHidden, TIMER_FG_SYNC);
            if (m_hotkeyRegistered) {
                UnregisterHotKey(m_hwndHidden, HOTKEY_SPLIT_TOGGLE);
                m_hotkeyRegistered = false;
            }
        }

        if (m_winEventCreateShow) {
            UnhookWinEvent(m_winEventCreateShow);
            m_winEventCreateShow = nullptr;
        }
        if (m_winEventForeground) {
            UnhookWinEvent(m_winEventForeground);
            m_winEventForeground = nullptr;
        }

        // 1. Ask all open Explorer windows to synchronously restore single-pane layout & release COM
        std::vector<HWND> cabinets = EnumerateExplorerCabinets();
        for (HWND hwndCab : cabinets) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(hwndCab, m_msgs.msgShutdown, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1500, &res);
        }

        // 2. Remove all thread hooks
        std::vector<DWORD> hookedTids;
        hookedTids.reserve(m_threadHooks.size());
        for (auto& kv : m_threadHooks) {
            hookedTids.push_back(kv.first);
            if (kv.second.hCallWnd) UnhookWindowsHookEx(kv.second.hCallWnd);
            if (kv.second.hGetMsg)  UnhookWindowsHookEx(kv.second.hGetMsg);
        }
        m_threadHooks.clear();

        // 3. Pump a harmless queued + sent message to wake GetMessageW & WndProc so Windows unmaps ExplorerSplitHook.dll immediately
        for (DWORD tid : hookedTids) {
            PostThreadMessageW(tid, WM_NULL, 0, 0);
        }
        for (HWND hwndCab : cabinets) {
            PostMessageW(hwndCab, WM_NULL, 0, 0);
            DWORD_PTR res = 0;
            SendMessageTimeoutW(hwndCab, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 300, &res);
        }

        RemoveTrayIcon();
        if (m_hTrayIcon) {
            DestroyIcon(m_hTrayIcon);
            m_hTrayIcon = nullptr;
        }

        if (m_hHookDll) {
            FreeLibrary(m_hHookDll);
            m_hHookDll = nullptr;
        }
        g_instance = nullptr;
    }

    static std::vector<HWND> EnumerateExplorerCabinets() {
        std::vector<HWND> result;
        EnumWindows(
            [](HWND hwnd, LPARAM lParam) -> BOOL {
                wchar_t cls[64]{};
                if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) > 0 &&
                    wcscmp(cls, L"CabinetWClass") == 0) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (IsExplorerProcessId(pid)) {
                        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&result)
        );
        return result;
    }

    static HWND FindBestTargetCabinet(HWND preferred) {
        if (preferred && IsWindow(preferred) && IsWindowVisible(preferred)) {
            wchar_t cls[64]{};
            GetClassNameW(preferred, cls, static_cast<int>(std::size(cls)));
            if (wcscmp(cls, L"CabinetWClass") == 0) return preferred;
        }
        HWND fg = GetForegroundWindow();
        if (fg) {
            wchar_t cls[64]{};
            GetClassNameW(fg, cls, static_cast<int>(std::size(cls)));
            if (wcscmp(cls, L"CabinetWClass") == 0) return fg;
        }
        auto cabs = EnumerateExplorerCabinets();
        for (HWND h : cabs) {
            if (IsWindowVisible(h) && !IsIconic(h)) return h;
        }
        return cabs.empty() ? nullptr : cabs.front();
    }

private:
    static inline SplitControllerApp* g_instance = nullptr;

    HINSTANCE m_hInst = nullptr;
    HWND m_hwndHidden = nullptr;
    HICON m_hTrayIcon = nullptr;
    HMODULE m_hHookDll = nullptr;
    HOOKPROC m_fnCallWnd = nullptr;
    HOOKPROC m_fnGetMsg = nullptr;

    HWINEVENTHOOK m_winEventCreateShow = nullptr;
    HWINEVENTHOOK m_winEventForeground = nullptr;

    RegisteredMessages m_msgs{};
    UINT m_msgTaskbarCreated = 0;
    std::unordered_map<DWORD, HookPair> m_threadHooks;
    HWND m_lastActiveCabinet = nullptr;
    bool m_hotkeyRegistered = false;
    bool m_autoSplitNextNewWindow = false;
    bool m_shuttingDown = false;

    void UpdateForegroundHotkey() {
        if (!m_hwndHidden || m_shuttingDown) return;
        HWND fg = GetForegroundWindow();
        if (fg) {
            HWND root = GetAncestor(fg, GA_ROOT);
            if (root) fg = root;
        }
        bool isExplorerActive = false;
        if (fg && IsWindow(fg)) {
            wchar_t cls[64]{};
            if (GetClassNameW(fg, cls, static_cast<int>(std::size(cls))) > 0 &&
                wcscmp(cls, L"CabinetWClass") == 0) {
                DWORD pid = 0;
                GetWindowThreadProcessId(fg, &pid);
                if (IsExplorerProcessId(pid)) {
                    isExplorerActive = true;
                    m_lastActiveCabinet = fg;
                    EnsureCabinetHooked(fg);
                }
            }
        }

        if (isExplorerActive && !m_hotkeyRegistered) {
            if (RegisterHotKey(m_hwndHidden, HOTKEY_SPLIT_TOGGLE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'E')) {
                m_hotkeyRegistered = true;
            }
        } else if (!isExplorerActive && m_hotkeyRegistered) {
            UnregisterHotKey(m_hwndHidden, HOTKEY_SPLIT_TOGGLE);
            m_hotkeyRegistered = false;
        }
    }

    bool LoadHookLibrary() {
        wchar_t exePath[MAX_PATH * 2]{};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        std::wstring dllPath(exePath);
        size_t slash = dllPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dllPath.resize(slash + 1);
        }
        dllPath += L"ExplorerSplitHook.dll";

        m_hHookDll = LoadLibraryW(dllPath.c_str());
        if (!m_hHookDll) return false;

        m_fnCallWnd = reinterpret_cast<HOOKPROC>(GetProcAddress(m_hHookDll, "CallWndProcHook"));
        m_fnGetMsg  = reinterpret_cast<HOOKPROC>(GetProcAddress(m_hHookDll, "GetMsgProcHook"));
        return (m_fnCallWnd != nullptr && m_fnGetMsg != nullptr);
    }

    void EnsureCabinetHooked(HWND hwndCabinet) {
        if (!hwndCabinet || !IsWindow(hwndCabinet) || m_shuttingDown) return;
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(hwndCabinet, &pid);
        if (tid == 0 || !IsExplorerProcessId(pid)) return;

        if (m_threadHooks.find(tid) == m_threadHooks.end()) {
            HookPair hp{};
            hp.hCallWnd = SetWindowsHookExW(WH_CALLWNDPROC, m_fnCallWnd, m_hHookDll, tid);
            hp.hGetMsg  = SetWindowsHookExW(WH_GETMESSAGE,  m_fnGetMsg,  m_hHookDll, tid);
            if (hp.hCallWnd && hp.hGetMsg) {
                m_threadHooks.emplace(tid, hp);
            } else {
                if (hp.hCallWnd) UnhookWindowsHookEx(hp.hCallWnd);
                if (hp.hGetMsg)  UnhookWindowsHookEx(hp.hGetMsg);
                return;
            }
        }

        DWORD_PTR res = 0;
        SendMessageTimeoutW(hwndCabinet, m_msgs.msgInit, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, &res);

        if (m_autoSplitNextNewWindow && IsWindowVisible(hwndCabinet)) {
            m_autoSplitNextNewWindow = false;
            PostMessageW(hwndCabinet, m_msgs.msgToggle, 0, 0);
        }
    }

    void ScanAndHookExplorerWindows() {
        auto cabinets = EnumerateExplorerCabinets();
        for (HWND h : cabinets) {
            EnsureCabinetHooked(h);
            if (!m_lastActiveCabinet && IsWindowVisible(h)) {
                m_lastActiveCabinet = h;
            }
        }
    }

    void PruneDeadThreads() {
        for (auto it = m_threadHooks.begin(); it != m_threadHooks.end(); ) {
            HANDLE hThread = OpenThread(SYNCHRONIZE, FALSE, it->first);
            bool dead = (!hThread || WaitForSingleObject(hThread, 0) == WAIT_OBJECT_0);
            if (hThread) CloseHandle(hThread);

            if (dead) {
                if (it->second.hCallWnd) UnhookWindowsHookEx(it->second.hCallWnd);
                if (it->second.hGetMsg)  UnhookWindowsHookEx(it->second.hGetMsg);
                it = m_threadHooks.erase(it);
            } else {
                ++it;
            }
        }
    }

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK, DWORD event, HWND hwnd,
        LONG idObject, LONG idChild, DWORD, DWORD
    ) {
        if (!g_instance) return;
        if (event == EVENT_SYSTEM_FOREGROUND) {
            PostMessageW(g_instance->m_hwndHidden, WM_FOREGROUND_CHANGE, 0, 0);
        }
        if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0) return;

        if (wcscmp(cls, L"CabinetWClass") == 0) {
            if (event == EVENT_SYSTEM_FOREGROUND) {
                g_instance->m_lastActiveCabinet = hwnd;
            }
            PostMessageW(g_instance->m_hwndHidden, WM_DEFERRED_SCAN, reinterpret_cast<WPARAM>(hwnd), 0);
        } else if (wcscmp(cls, L"ShellTabWindowClass") == 0) {
            HWND root = GetAncestor(hwnd, GA_ROOT);
            if (root) {
                PostMessageW(g_instance->m_hwndHidden, WM_DEFERRED_SCAN, reinterpret_cast<WPARAM>(root), 0);
            }
        }
    }

    static HICON CreateSplitTrayIcon() {
        constexpr int sz = 32;
        BITMAPV5HEADER bi{};
        bi.bV5Size        = sizeof(bi);
        bi.bV5Width       = sz;
        bi.bV5Height      = -sz; // Top-down
        bi.bV5Planes      = 1;
        bi.bV5BitCount    = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask     = 0x00FF0000;
        bi.bV5GreenMask   = 0x0000FF00;
        bi.bV5BlueMask    = 0x000000FF;
        bi.bV5AlphaMask   = 0xFF000000;

        void* bits = nullptr;
        HDC screenDC = GetDC(nullptr);
        HBITMAP hColor = CreateDIBSection(screenDC, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screenDC);
        if (!hColor || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

        auto* px = static_cast<DWORD*>(bits);
        for (int y = 0; y < sz; ++y) {
            for (int x = 0; x < sz; ++x) {
                px[y * sz + x] = 0x00000000;
                if (x >= 2 && x <= 29 && y >= 4 && y <= 27) {
                    // Rounded corner clip
                    bool corner = ((x == 2 || x == 29) && (y == 4 || y == 27));
                    if (corner) continue;

                    if (y <= 8) {
                        // Top header bar (Windows 11 accent blue)
                        px[y * sz + x] = 0xFF0078D4;
                    } else if (x == 2 || x == 29 || y == 27 || x == 15 || x == 16) {
                        // Outer border & central split divider
                        px[y * sz + x] = 0xFF0078D4;
                    } else if (x < 15) {
                        // Left pane surface
                        px[y * sz + x] = 0xFFF5F9FF;
                    } else {
                        // Right pane surface
                        px[y * sz + x] = 0xFFDCEEFF;
                    }
                }
            }
        }

        HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, nullptr);
        ICONINFO ii{};
        ii.fIcon    = TRUE;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
        HICON hIcon = CreateIconIndirect(&ii);
        DeleteObject(hColor);
        DeleteObject(hMask);
        return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
    }

    void AddTrayIcon(bool showStartupBalloon = false) {
        NOTIFYICONDATAW nid{};
        nid.cbSize           = sizeof(nid);
        nid.hWnd             = m_hwndHidden;
        nid.uID              = 1;
        nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | (showStartupBalloon ? NIF_INFO : 0);
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon            = m_hTrayIcon;
        wcsncpy_s(nid.szTip, L"Explorer Split View (Ctrl+Shift+E)", _TRUNCATE);
        if (showStartupBalloon) {
            nid.dwInfoFlags = NIIF_INFO;
            wcsncpy_s(nid.szInfoTitle, L"Explorer Split View is active", _TRUNCATE);
            wcsncpy_s(nid.szInfo, L"Press Ctrl+Shift+E or click [Split] inside any File Explorer window.", _TRUNCATE);
        }
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    void RemoveTrayIcon() {
        if (!m_hwndHidden) return;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = m_hwndHidden;
        nid.uID    = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    static bool IsAutorunEnabled() {
        wchar_t val[MAX_PATH * 2]{};
        DWORD cb = sizeof(val);
        LSTATUS st = RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"ExplorerSplitView",
            RRF_RT_REG_SZ,
            nullptr, val, &cb
        );
        return st == ERROR_SUCCESS && val[0] != L'\0';
    }

    static void SetAutorunEnabled(bool enable) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
            return;
        }
        if (enable) {
            wchar_t exePath[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
            std::wstring quoted = L"\"" + std::wstring(exePath) + L"\" --autorun";
            RegSetValueExW(
                hKey, L"ExplorerSplitView", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(quoted.c_str()),
                static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t))
            );
        } else {
            RegDeleteValueW(hKey, L"ExplorerSplitView");
        }
        RegCloseKey(hKey);
    }

    void ShowTrayMenu() {
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;

        AppendMenuW(hMenu, MF_STRING, CMD_TOGGLE_SPLIT,  L"Toggle Split View\tCtrl+Shift+E");
        AppendMenuW(hMenu, MF_STRING, CMD_RESET_RATIO,   L"Reset Split Ratio (50/50)");
        AppendMenuW(hMenu, MF_STRING, CMD_OPEN_EXPLORER, L"Open New File Explorer Window");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING | (IsAutorunEnabled() ? MF_CHECKED : 0), CMD_AUTORUN, L"Launch at Windows Startup");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, CMD_EXIT,          L"Exit (Unload from Explorer)");

        SetMenuDefaultItem(hMenu, CMD_TOGGLE_SPLIT, FALSE);

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwndHidden);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwndHidden, nullptr);
        PostMessageW(m_hwndHidden, WM_NULL, 0, 0);
        DestroyMenu(hMenu);
    }

    void SendCommandToActiveExplorer(UINT registeredMsg) {
        ScanAndHookExplorerWindows();
        HWND target = FindBestTargetCabinet(m_lastActiveCabinet);
        if (target) {
            EnsureCabinetHooked(target);
            SetForegroundWindow(target);
            PostMessageW(target, registeredMsg, 0, 0);
        }
    }

    static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        SplitControllerApp* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<SplitControllerApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<SplitControllerApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

        if (self->m_msgTaskbarCreated != 0 && msg == self->m_msgTaskbarCreated) {
            self->AddTrayIcon();
            self->ScanAndHookExplorerWindows();
            return 0;
        }

        switch (msg) {
        case WM_FOREGROUND_CHANGE:
            self->UpdateForegroundHotkey();
            return 0;

        case WM_HOTKEY:
            if (wParam == HOTKEY_SPLIT_TOGGLE) {
                self->SendCommandToActiveExplorer(self->m_msgs.msgToggle);
            }
            return 0;

        case WM_DEFERRED_SCAN: {
            HWND hwndCab = reinterpret_cast<HWND>(wParam);
            if (hwndCab && IsWindow(hwndCab)) {
                self->EnsureCabinetHooked(hwndCab);
            } else {
                self->ScanAndHookExplorerWindows();
            }
            self->UpdateForegroundHotkey();
            return 0;
        }

        case WM_TIMER:
            if (wParam == TIMER_CLEANUP) {
                self->PruneDeadThreads();
                self->ScanAndHookExplorerWindows();
            } else if (wParam == TIMER_FG_SYNC) {
                self->UpdateForegroundHotkey();
            }
            return 0;

        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                self->ShowTrayMenu();
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                self->SendCommandToActiveExplorer(self->m_msgs.msgToggle);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case CMD_TOGGLE_SPLIT:
                self->SendCommandToActiveExplorer(self->m_msgs.msgToggle);
                break;
            case CMD_RESET_RATIO:
                self->SendCommandToActiveExplorer(self->m_msgs.msgResetRatio);
                break;
            case CMD_OPEN_EXPLORER:
                ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case CMD_AUTORUN:
                SetAutorunEnabled(!IsAutorunEnabled());
                break;
            case CMD_EXIT:
                DestroyWindow(hwnd);
                break;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            self->Shutdown();
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring args = lpCmdLine ? lpCmdLine : L"";
    RegisteredMessages msgs{};
    msgs.Initialize();

    // CLI control flags (--stop, --toggle, --reset)
    if (args.find(L"--stop") != std::wstring::npos || args.find(L"--quit") != std::wstring::npos) {
        HWND hwndRunning = FindWindowW(kControllerWndClass, nullptr);
        if (hwndRunning) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(hwndRunning, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 4000, &res);
        }
        return 0;
    }

    if (args.find(L"--toggle") != std::wstring::npos) {
        HWND target = SplitControllerApp::FindBestTargetCabinet(nullptr);
        if (target) {
            PostMessageW(target, msgs.msgToggle, 0, 0);
        }
        return 0;
    }

    if (args.find(L"--reset") != std::wstring::npos) {
        HWND target = SplitControllerApp::FindBestTargetCabinet(nullptr);
        if (target) {
            PostMessageW(target, msgs.msgResetRatio, 0, 0);
        }
        return 0;
    }

    // Ensure single controller instance per desktop; if already running, toggle split on active Explorer window
    std::wstring mutexName = GetDesktopScopedMutexName();
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!hMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        HWND target = SplitControllerApp::FindBestTargetCabinet(nullptr);
        if (target) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(target, msgs.msgToggle, 0, 0, SMTO_ABORTIFHUNG, 1500, &res);
        }
        return 0;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool silentStartup = (args.find(L"--silent") != std::wstring::npos || args.find(L"--autorun") != std::wstring::npos);

    SplitControllerApp app;
    int exitCode = 1;
    if (app.Initialize(hInstance, silentStartup)) {
        exitCode = app.RunMessageLoop();
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return exitCode;
}
