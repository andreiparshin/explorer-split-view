#pragma once

#include "../../include/SharedProtocol.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <wrl/client.h>
#include <functional>
#include <string>

namespace ExplorerSplit {

using Microsoft::WRL::ComPtr;

// RAII wrapper for PIDLIST_ABSOLUTE allocated by Shell COM allocator (CoTaskMemFree / ILFree)
class UniquePidl {
public:
    UniquePidl() = default;
    explicit UniquePidl(PIDLIST_ABSOLUTE p) : m_pidl(p) {}
    ~UniquePidl() { Reset(); }

    UniquePidl(const UniquePidl&) = delete;
    UniquePidl& operator=(const UniquePidl&) = delete;

    UniquePidl(UniquePidl&& other) noexcept : m_pidl(other.m_pidl) {
        other.m_pidl = nullptr;
    }
    UniquePidl& operator=(UniquePidl&& other) noexcept {
        if (this != &other) {
            Reset();
            m_pidl = other.m_pidl;
            other.m_pidl = nullptr;
        }
        return *this;
    }

    void Reset(PIDLIST_ABSOLUTE p = nullptr) {
        if (m_pidl) {
            CoTaskMemFree(m_pidl);
        }
        m_pidl = p;
    }

    PIDLIST_ABSOLUTE Get() const { return m_pidl; }
    PIDLIST_ABSOLUTE* ReleaseAndGetAddressOf() {
        Reset();
        return &m_pidl;
    }
    explicit operator bool() const { return m_pidl != nullptr; }

    UniquePidl Clone() const {
        if (!m_pidl) return UniquePidl{};
        return UniquePidl(ILCloneFull(m_pidl));
    }

private:
    PIDLIST_ABSOLUTE m_pidl = nullptr;
};

// Queries IShellBrowser associated with a given ShellTabWindowClass HWND
ComPtr<IShellBrowser> FindShellBrowserForTab(HWND hwndShellTab);

// Reads the current folder PIDL of the Left Pane (ShellTabWindowClass), falling back to This PC
UniquePidl QueryLeftPaneFolderPidl(HWND hwndShellTab);

// Formats a PIDL as a filesystem path (if available) or localized Shell display name
std::wstring FormatPidlForAddressBar(PCIDLIST_ABSOLUTE pidl);

// Parses user-entered path (supports quotes, %ENV_VARS%, shell: URIs, UNC, drive paths) into a PIDL
UniquePidl ParseUserPathToPidl(const std::wstring& rawInput);

// Callback invoked on the UI thread when IShellBrowser::BrowseObject is called for a tab.
// Return true if navigation was redirected to the Right Pane (suppresses Left Pane navigation).
using BrowseRedirectCallback = bool(*)(HWND hwndShellTab, PCUIDLIST_RELATIVE pidl, UINT wFlags);

// Installs (if not yet installed) the process-wide IShellBrowser::BrowseObject hook using sampleBrowser's vtable
void EnsureBrowseObjectHookInstalled(IShellBrowser* sampleBrowser, BrowseRedirectCallback callback);

// Restores the original IShellBrowser::BrowseObject vtable pointer before DLL unload
void RemoveBrowseObjectHook();

// Checks whether hwnd or any of its ancestors (up to stopAt) has the given window class name
bool HasAncestorWithClass(HWND hwnd, HWND stopAt, const wchar_t* className);

} // namespace ExplorerSplit
