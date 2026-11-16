#include "ShellHelpers.h"

namespace ExplorerSplit {

namespace {

constexpr UINT kWmGetIShellBrowser = WM_USER + 7;
constexpr size_t kBrowseObjectVtblIndex = 11; // IUnknown(3) + IOleWindow(2) + IShellBrowser(6) -> slot 11

using BrowseObjectFn = HRESULT(STDMETHODCALLTYPE*)(IShellBrowser* This, PCUIDLIST_RELATIVE pidl, UINT wFlags);

SRWLOCK g_browseHookLock = SRWLOCK_INIT;
void** g_hookedVtblSlot = nullptr;
BrowseObjectFn g_origBrowseObject = nullptr;
BrowseRedirectCallback g_redirectCallback = nullptr;

HRESULT STDMETHODCALLTYPE HookedBrowseObject(IShellBrowser* This, PCUIDLIST_RELATIVE pidl, UINT wFlags) {
    BrowseObjectFn orig = g_origBrowseObject;
    BrowseRedirectCallback cb = g_redirectCallback;

    if (This && cb) {
        HWND hwndTab = nullptr;
        if (SUCCEEDED(This->GetWindow(&hwndTab)) && hwndTab && IsWindow(hwndTab)) {
            if (cb(hwndTab, pidl, wFlags)) {
                return S_OK;
            }
        }
    }

    return orig ? orig(This, pidl, wFlags) : E_FAIL;
}

bool IsReadablePointer(const void* p, size_t minBytes) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kReadableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kReadableMask) == 0 || (mbi.Protect & PAGE_GUARD) != 0) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + minBytes) <= regionEnd;
}

ComPtr<IShellBrowser> TryQueryShellBrowserFromHwnd(HWND hwndTarget) {
    if (!hwndTarget || !IsWindow(hwndTarget)) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwndTarget, &pid);
    if (pid != GetCurrentProcessId()) return nullptr;

    LRESULT raw = SendMessageW(hwndTarget, kWmGetIShellBrowser, 0, 0);
    auto* candidate = reinterpret_cast<IUnknown*>(raw);
    if (!IsReadablePointer(candidate, sizeof(void*))) return nullptr;

    const void* vtbl = *reinterpret_cast<const void* const*>(candidate);
    if (!IsReadablePointer(vtbl, sizeof(void*) * 12)) return nullptr;

    ComPtr<IShellBrowser> spBrowser;
    if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&spBrowser))) && spBrowser) {
        return spBrowser;
    }
    return nullptr;
}

} // namespace

void EnsureBrowseObjectHookInstalled(IShellBrowser* sampleBrowser, BrowseRedirectCallback callback) {
    if (!sampleBrowser || !callback) return;

    AcquireSRWLockExclusive(&g_browseHookLock);
    g_redirectCallback = callback;

    if (!g_hookedVtblSlot) {
        void** vtbl = *reinterpret_cast<void***>(sampleBrowser);
        if (IsReadablePointer(vtbl, sizeof(void*) * (kBrowseObjectVtblIndex + 1))) {
            void** slot = &vtbl[kBrowseObjectVtblIndex];
            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                g_origBrowseObject = reinterpret_cast<BrowseObjectFn>(*slot);
                InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedBrowseObject));
                DWORD dummy = 0;
                VirtualProtect(slot, sizeof(void*), oldProtect, &dummy);
                g_hookedVtblSlot = slot;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_browseHookLock);
}

void RemoveBrowseObjectHook() {
    AcquireSRWLockExclusive(&g_browseHookLock);
    if (g_hookedVtblSlot && g_origBrowseObject) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_hookedVtblSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            InterlockedExchangePointer(g_hookedVtblSlot, reinterpret_cast<void*>(g_origBrowseObject));
            DWORD dummy = 0;
            VirtualProtect(g_hookedVtblSlot, sizeof(void*), oldProtect, &dummy);
        }
    }
    g_hookedVtblSlot = nullptr;
    g_origBrowseObject = nullptr;
    g_redirectCallback = nullptr;
    ReleaseSRWLockExclusive(&g_browseHookLock);
}

bool HasAncestorWithClass(HWND hwnd, HWND stopAt, const wchar_t* className) {
    if (!hwnd || !className) return false;
    wchar_t cls[64]{};
    HWND cur = hwnd;
    int safety = 0;
    while (cur && IsWindow(cur) && safety++ < 32) {
        if (GetClassNameW(cur, cls, static_cast<int>(std::size(cls))) > 0 &&
            wcscmp(cls, className) == 0) {
            return true;
        }
        if (cur == stopAt) break;
        cur = GetParent(cur);
    }
    return false;
}

ComPtr<IShellBrowser> FindShellBrowserForTab(HWND hwndShellTab) {
    if (!hwndShellTab || !IsWindow(hwndShellTab)) return nullptr;

    // 1. Direct in-process query on ShellTabWindowClass (zero RPC, works with multi-process Explorer)
    if (auto sp = TryQueryShellBrowserFromHwnd(hwndShellTab)) {
        return sp;
    }

    // 2. Direct in-process query on top-level CabinetWClass
    HWND hwndRoot = GetAncestor(hwndShellTab, GA_ROOT);
    if (hwndRoot && hwndRoot != hwndShellTab) {
        if (auto sp = TryQueryShellBrowserFromHwnd(hwndRoot)) {
            return sp;
        }
    }

    // 3. Fallback via CLSID_ShellWindows
    ComPtr<IShellWindows> spShellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&spShellWindows))) || !spShellWindows) {
        return nullptr;
    }

    long count = 0;
    if (FAILED(spShellWindows->get_Count(&count)) || count <= 0) {
        return nullptr;
    }

    ComPtr<IShellBrowser> fallbackBrowser;
    for (long i = 0; i < count; ++i) {
        VARIANT idx{};
        idx.vt = VT_I4;
        idx.lVal = i;

        ComPtr<IDispatch> spDisp;
        if (FAILED(spShellWindows->Item(idx, &spDisp)) || !spDisp) {
            continue;
        }

        ComPtr<IShellBrowser> spBrowser;
        if (FAILED(IUnknown_QueryService(spDisp.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&spBrowser))) || !spBrowser) {
            continue;
        }

        HWND hwndBrowser = nullptr;
        if (SUCCEEDED(spBrowser->GetWindow(&hwndBrowser)) && hwndBrowser) {
            if (hwndBrowser == hwndShellTab) {
                return spBrowser;
            }
            if (!fallbackBrowser && hwndRoot && GetAncestor(hwndBrowser, GA_ROOT) == hwndRoot && IsWindowVisible(hwndBrowser)) {
                fallbackBrowser = spBrowser;
            }
        }
    }

    return fallbackBrowser;
}

UniquePidl QueryLeftPaneFolderPidl(HWND hwndShellTab) {
    ComPtr<IShellBrowser> spBrowser = FindShellBrowserForTab(hwndShellTab);
    if (spBrowser) {
        ComPtr<IShellView> spShellView;
        if (SUCCEEDED(spBrowser->QueryActiveShellView(&spShellView)) && spShellView) {
            ComPtr<IFolderView2> spFolderView;
            if (SUCCEEDED(spShellView.As(&spFolderView)) && spFolderView) {
                ComPtr<IPersistFolder2> spPersistFolder;
                if (SUCCEEDED(spFolderView->GetFolder(IID_PPV_ARGS(&spPersistFolder))) && spPersistFolder) {
                    UniquePidl pidl;
                    if (SUCCEEDED(spPersistFolder->GetCurFolder(pidl.ReleaseAndGetAddressOf())) && pidl) {
                        return pidl;
                    }
                }
            }
        }
    }

    UniquePidl fallback;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, fallback.ReleaseAndGetAddressOf())) && fallback) {
        return fallback;
    }
    SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, fallback.ReleaseAndGetAddressOf());
    return fallback;
}

std::wstring FormatPidlForAddressBar(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return L"";

    PWSTR pszName = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &pszName)) && pszName) {
        std::wstring result(pszName);
        CoTaskMemFree(pszName);
        return result;
    }

    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &pszName)) && pszName) {
        std::wstring result(pszName);
        CoTaskMemFree(pszName);
        return result;
    }

    return L"";
}

UniquePidl ParseUserPathToPidl(const std::wstring& rawInput) {
    std::wstring trimmed = rawInput;
    while (!trimmed.empty() && std::iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::iswspace(trimmed.back())) trimmed.pop_back();

    if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    if (trimmed.empty()) return UniquePidl{};

    wchar_t expanded[MAX_PATH * 4]{};
    DWORD len = ExpandEnvironmentStringsW(trimmed.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    std::wstring candidate = (len > 0 && len < std::size(expanded)) ? std::wstring(expanded) : trimmed;

    UniquePidl pidl;
    SFGAOF sfgao = 0;
    if (SUCCEEDED(SHParseDisplayName(candidate.c_str(), nullptr, pidl.ReleaseAndGetAddressOf(), 0, &sfgao)) && pidl) {
        return pidl;
    }
    return UniquePidl{};
}

} // namespace ExplorerSplit
