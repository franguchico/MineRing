#include "common.h"
#include <filesystem>

namespace mb {
bool running_under_wine() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version");
}

static std::wstring env(const wchar_t* key) {
    DWORD n = GetEnvironmentVariableW(key, nullptr, 0);
    if (!n) return {};
    std::wstring value(n, L'\0');
    DWORD got = GetEnvironmentVariableW(key, &value[0], n);
    if (!got || got >= n) return {};
    value.resize(got);
    return value;
}

// Pure resolver: tests supply synthetic environments without creating IPC files.
static std::filesystem::path resolve_ipc_dir(const std::wstring& override,
                                           const std::wstring& userProfile, bool wine,
                                           const wchar_t** error) {
    *error = nullptr;
    std::filesystem::path dir;
    const wchar_t* variable;
    if (!override.empty()) {
        dir = override;
        variable = L"ERMC_DIR must be an absolute directory";
    }
    else if (wine) return std::filesystem::path(L"Z:\\tmp\\ermc");
    else {
        if (userProfile.empty()) {
            *error = L"Set an absolute ERMC_DIR or USERPROFILE on Windows";
            return {};
        }
        // Packaged launchers can virtualize LOCALAPPDATA into a different physical file.
        dir = userProfile;
        variable = L"USERPROFILE must be an absolute directory";
    }
    // Never silently resolve a drive-relative path against the game's working directory.
    if (!dir.is_absolute()) {
        *error = variable;
        return {};
    }
    if (override.empty()) dir /= std::filesystem::path(L"Documents") / L"EldenMinecraft" / L"ipc";
    return dir.lexically_normal();
}

static std::filesystem::path ipc_dir() {
    const wchar_t* error;
    auto dir = resolve_ipc_dir(env(L"ERMC_DIR"), env(L"USERPROFILE"), running_under_wine(), &error);
    if (error) {
        // log() calls ipc_ensure_dir() while opening its file, so it cannot report this error.
        OutputDebugStringW(L"erbridge IPC: ");
        OutputDebugStringW(error);
        OutputDebugStringW(L"\n");
        SetLastError(ERROR_BAD_ENVIRONMENT);
    }
    return dir;
}

std::wstring ipc_path(const wchar_t* filename) {
    auto dir = ipc_dir();
    return dir.empty() ? std::wstring{} : (dir / filename).wstring();
}

bool ipc_ensure_dir() {
    auto dir = ipc_dir();
    if (dir.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec && std::filesystem::is_directory(dir, ec) && !ec;
}
} // namespace mb
