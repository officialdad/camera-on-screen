#include "paths.h"

#define NOMINMAX
#include <windows.h>
#include <vector>

namespace {
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

std::string ShimModuleDir() {
    HMODULE hmod = nullptr;
    // Resolve the module that contains THIS function's code (the shim DLL at runtime, or the
    // smoke exe when linked into a test). UNCHANGED_REFCOUNT: do not bump the module's refcount.
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ShimModuleDir), &hmod)) {
        return std::string();
    }
    std::vector<wchar_t> buf(1024);
    DWORD n = GetModuleFileNameW(hmod, buf.data(), (DWORD)buf.size());
    if (n == 0) return std::string();
    // Grow once if the path was truncated (ERROR_INSUFFICIENT_BUFFER).
    while (n == buf.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buf.resize(buf.size() * 2);
        n = GetModuleFileNameW(hmod, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::string();
    }
    std::wstring path(buf.data(), n);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    return Utf8(path.substr(0, slash));
}

bool DirExists(const std::string& path) {
    if (path.empty()) return false;
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &w[0], wn);
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
