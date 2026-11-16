#include "../../include/SharedProtocol.h"
#include "TabSplitManager.h"
#include <atomic>
#include <unordered_map>
#include <vector>

namespace {

HINSTANCE g_hDllInstance = nullptr;
std::atomic<int> g_totalAttachedTabs{0};

struct ThreadHookState {
    ExplorerSplit::RegisteredMessages msgs{};
    std::unordered_map<HWND, std::unique_ptr<ExplorerSplit::TabSplitManager>> tabs;
    bool initializedComControls = false;
    bool isShutDown = false;

    void EnsureInit() {
        msgs.Initialize();
        if (!initializedComControls) {
            INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES };
            InitCommonControlsEx(&icc);
            initializedComControls = true;
        }
    }
};

thread_local ThreadHookState t_state;

bool GlobalBrowseRedirectRouter(HWND hwndShellTab, PCUIDLIST_RELATIVE pidl, UINT wFlags) {
    auto it = t_state.tabs.find(hwndShellTab);
    if (it != t_state.tabs.end() && it->second) {
        return it->second->TryRedirectBrowseToActivePane(pidl, wFlags);
    }
    return false;
}

bool IsWindowClass(HWND hwnd, const wchar_t* expectedClass) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0) return false;
    return wcscmp(cls, expectedClass) == 0;
}

ExplorerSplit::TabSplitManager* EnsureTabAttached(HWND hwndTab) {
    if (!hwndTab || !IsWindow(hwndTab)) return nullptr;
    auto it = t_state.tabs.find(hwndTab);
    if (it != t_state.tabs.end()) {
        return it->second.get();
    }

    auto mgr = std::make_unique<ExplorerSplit::TabSplitManager>(
        g_hDllInstance,
        hwndTab,
        &GlobalBrowseRedirectRouter
    );
    if (!mgr->Attach()) {
        return nullptr;
    }
    auto* raw = mgr.get();
    t_state.tabs.emplace(hwndTab, std::move(mgr));
    ++g_totalAttachedTabs;
    return raw;
}

void EraseTabFromThread(HWND hwndTab) {
    auto it = t_state.tabs.find(hwndTab);
    if (it != t_state.tabs.end()) {
        if (it->second) {
            it->second->DetachAndRestore();
        }
        t_state.tabs.erase(it);
        if (--g_totalAttachedTabs <= 0) {
            g_totalAttachedTabs = 0;
            ExplorerSplit::RemoveBrowseObjectHook();
        }
    }
}

HWND FindActiveShellTab(HWND hwndCabinetOrChild) {
    if (!hwndCabinetOrChild || !IsWindow(hwndCabinetOrChild)) return nullptr;
    if (IsWindowClass(hwndCabinetOrChild, L"ShellTabWindowClass")) {
        return hwndCabinetOrChild;
    }

    HWND hwndRoot = GetAncestor(hwndCabinetOrChild, GA_ROOT);
    if (!hwndRoot || !IsWindowClass(hwndRoot, L"CabinetWClass")) return nullptr;

    HWND hwndChild = nullptr;
    HWND firstTab = nullptr;
    while ((hwndChild = FindWindowExW(hwndRoot, hwndChild, L"ShellTabWindowClass", nullptr)) != nullptr) {
        if (!firstTab) firstTab = hwndChild;
        if (IsWindowVisible(hwndChild)) {
            return hwndChild;
        }
    }
    return firstTab;
}

void AttachAllTabsInCabinet(HWND hwndCabinet) {
    if (!hwndCabinet || !IsWindow(hwndCabinet)) return;
    if (IsWindowClass(hwndCabinet, L"ShellTabWindowClass")) {
        EnsureTabAttached(hwndCabinet);
        return;
    }
    if (!IsWindowClass(hwndCabinet, L"CabinetWClass")) return;

    HWND hwndTab = nullptr;
    while ((hwndTab = FindWindowExW(hwndCabinet, hwndTab, L"ShellTabWindowClass", nullptr)) != nullptr) {
        EnsureTabAttached(hwndTab);
    }
}

void DetachAllOnCurrentThread() {
    t_state.isShutDown = true;
    auto localMap = std::move(t_state.tabs);
    t_state.tabs.clear();
    int removed = static_cast<int>(localMap.size());
    for (auto& kv : localMap) {
        if (kv.second) {
            kv.second->DetachAndRestore();
        }
    }
    if ((g_totalAttachedTabs -= removed) <= 0) {
        g_totalAttachedTabs = 0;
        ExplorerSplit::RemoveBrowseObjectHook();
    }
}

} // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK CallWndProcHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam != 0) {
        auto* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
        t_state.EnsureInit();

        if (cwp->message == t_state.msgs.msgShutdown && t_state.msgs.msgShutdown != 0) {
            DetachAllOnCurrentThread();
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (cwp->message == t_state.msgs.msgInit && t_state.msgs.msgInit != 0) {
            t_state.isShutDown = false;
            AttachAllTabsInCabinet(cwp->hwnd);
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (t_state.isShutDown) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (cwp->message == t_state.msgs.msgToggle && t_state.msgs.msgToggle != 0) {
            if (InSendMessage()) {
                PostMessageW(cwp->hwnd, t_state.msgs.msgToggle, 0, 0);
            } else {
                HWND activeTab = FindActiveShellTab(cwp->hwnd);
                if (activeTab) {
                    if (auto* mgr = EnsureTabAttached(activeTab)) {
                        mgr->ToggleSplit();
                    }
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (cwp->message == t_state.msgs.msgResetRatio && t_state.msgs.msgResetRatio != 0) {
            if (InSendMessage()) {
                PostMessageW(cwp->hwnd, t_state.msgs.msgResetRatio, 0, 0);
            } else {
                HWND activeTab = FindActiveShellTab(cwp->hwnd);
                if (activeTab) {
                    if (auto* mgr = EnsureTabAttached(activeTab)) {
                        mgr->ResetSplitRatio();
                    }
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        switch (cwp->message) {
        case WM_CREATE:
        case WM_SHOWWINDOW:
        case WM_SIZE: {
            if (IsWindowClass(cwp->hwnd, L"ShellTabWindowClass")) {
                EnsureTabAttached(cwp->hwnd);
            } else if (IsWindowClass(cwp->hwnd, L"DUIViewWndClassName")) {
                HWND parentTab = GetParent(cwp->hwnd);
                if (parentTab && IsWindowClass(parentTab, L"ShellTabWindowClass")) {
                    if (auto* mgr = EnsureTabAttached(parentTab)) {
                        mgr->OnChildCreatedOrSized();
                    }
                }
            }
            break;
        }
        case WM_MOUSEACTIVATE: {
            for (auto& kv : t_state.tabs) {
                if (kv.second) {
                    kv.second->NotifyPointerDownOnHwnd(cwp->hwnd);
                    kv.second->RefreshFocusIndicator();
                }
            }
            break;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            for (auto& kv : t_state.tabs) {
                if (kv.second) kv.second->RefreshFocusIndicator();
            }
            break;
        }
        case WM_NCDESTROY: {
            EraseTabFromThread(cwp->hwnd);
            break;
        }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProcHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam & PM_REMOVE) && lParam != 0) {
        auto* pMsg = reinterpret_cast<MSG*>(lParam);
        t_state.EnsureInit();

        if (t_state.isShutDown) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (pMsg->hwnd && pMsg->message == t_state.msgs.msgToggle && t_state.msgs.msgToggle != 0) {
            HWND activeTab = FindActiveShellTab(pMsg->hwnd);
            if (activeTab) {
                if (auto* mgr = EnsureTabAttached(activeTab)) {
                    mgr->ToggleSplit();
                }
            }
            pMsg->message = WM_NULL;
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (pMsg->hwnd && pMsg->message == t_state.msgs.msgResetRatio && t_state.msgs.msgResetRatio != 0) {
            HWND activeTab = FindActiveShellTab(pMsg->hwnd);
            if (activeTab) {
                if (auto* mgr = EnsureTabAttached(activeTab)) {
                    mgr->ResetSplitRatio();
                }
            }
            pMsg->message = WM_NULL;
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        // Track mouse clicks to accurately distinguish Left File View vs Right File View vs Shared Left Sidebar
        if (pMsg->hwnd && (pMsg->message == WM_LBUTTONDOWN || pMsg->message == WM_RBUTTONDOWN || pMsg->message == WM_MBUTTONDOWN)) {
            for (auto& kv : t_state.tabs) {
                if (kv.second && kv.second->IsSplitActive()) {
                    kv.second->NotifyPointerDownOnHwnd(pMsg->hwnd);
                }
            }
        }

        if (pMsg->hwnd && pMsg->message >= WM_KEYFIRST && pMsg->message <= WM_KEYLAST) {
            if (pMsg->message == WM_KEYDOWN && pMsg->wParam == 'E' && !(pMsg->lParam & (1LL << 30))) {
                bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
                bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
                if (ctrl && shift && !alt) {
                    HWND activeTab = FindActiveShellTab(pMsg->hwnd);
                    if (activeTab) {
                        if (auto* mgr = EnsureTabAttached(activeTab)) {
                            mgr->ToggleSplit();
                            pMsg->message = WM_NULL;
                            return CallNextHookEx(nullptr, nCode, wParam, lParam);
                        }
                    }
                }
            }

            for (auto& kv : t_state.tabs) {
                if (kv.second && kv.second->IsSplitActive()) {
                    if (kv.second->PreTranslateMessage(pMsg)) {
                        break;
                    }
                }
            }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    UNREFERENCED_PARAMETER(lpReserved);
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hDllInstance = hModule;
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH && lpReserved == nullptr) {
        ExplorerSplit::RemoveBrowseObjectHook();
    }
    return TRUE;
}
