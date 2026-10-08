// Standalone pure-path regression check: no directory creation, IPC mapping or game access.
// Include the implementation so its resolver can remain private to paths.cpp in production.
#include "../src/paths.cpp"
#include <stdio.h>
#include <wchar.h>

static int failures;

static void check(bool passed, const char* message) {
    if (!passed) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void expect_path(const wchar_t* override, const wchar_t* profile, bool wine,
                        const wchar_t* expected, const char* message) {
    const wchar_t* error;
    auto actual = mb::resolve_ipc_dir(override, profile, wine, &error);
    check(!error && actual == std::filesystem::path(expected), message);
}

static void expect_error(const wchar_t* override, const wchar_t* profile, bool wine,
                         const wchar_t* variable, const char* message) {
    const wchar_t* error;
    auto actual = mb::resolve_ipc_dir(override, profile, wine, &error);
    check(actual.empty() && error && wcsstr(error, variable), message);
}

int main() {
    const wchar_t* profile = L"C:\\Users\\Profile with spaces and \u00E7";
    const wchar_t* expected = L"C:\\Users\\Profile with spaces and \u00E7\\Documents\\EldenMinecraft\\ipc";
    expect_path(L"", profile, false, expected, "Windows defaults to USERPROFILE Documents");
    expect_path(L"", L"C:\\Users\\Profile with spaces and \u00E7\\unused\\..", false, expected,
                "Profile normalization preserves spaces and Unicode");
    expect_path(L"D:\\Explicit IPC", L"", false, L"D:\\Explicit IPC", "Override without USERPROFILE");
    expect_path(L"D:\\Explicit IPC", L"relative", false, L"D:\\Explicit IPC", "Override beats invalid USERPROFILE");
    expect_path(L"D:\\unused\\..\\IPC with spaces", profile, false, L"D:\\IPC with spaces", "Override normalization");
    expect_path(L"D:\\Explicit IPC", profile, true, L"D:\\Explicit IPC", "Override beats Wine default");
    expect_path(L"", L"", true, L"Z:\\tmp\\ermc", "Wine default without USERPROFILE");
    expect_path(L"", L"relative", true, L"Z:\\tmp\\ermc", "Wine ignores Windows USERPROFILE");
    expect_error(L"", L"", false, L"USERPROFILE", "Missing USERPROFILE has a named error");
    expect_error(L"", L"relative", false, L"USERPROFILE", "Relative USERPROFILE rejected");
    expect_error(L"", L"C:relative", false, L"USERPROFILE", "Drive-relative USERPROFILE rejected");
    expect_error(L"", L"\\root-relative", false, L"USERPROFILE", "Root-relative USERPROFILE rejected");
    expect_error(L"relative", profile, false, L"ERMC_DIR", "Invalid override cannot fall back to profile");
    expect_error(L"C:relative", profile, false, L"ERMC_DIR", "Drive-relative override rejected");
    expect_error(L"\\root-relative", profile, false, L"ERMC_DIR", "Root-relative override rejected");
    expect_error(L"relative", profile, true, L"ERMC_DIR", "Invalid override cannot fall back to Wine");
    expect_path(L"\\\\server\\share\\IPC with spaces", L"", false, L"\\\\server\\share\\IPC with spaces",
                "UNC override preserved without network I/O");
    expect_path(L"", L"\\\\server\\share\\Profile", false,
                L"\\\\server\\share\\Profile\\Documents\\EldenMinecraft\\ipc", "UNC profile without network I/O");
    if (failures) return 1;
    puts("PASS: native IPC profile default, overrides, Wine, normalization and named path errors (no file I/O)");
    return 0;
}
