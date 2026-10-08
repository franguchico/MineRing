#include "common.h"
#include <stdio.h>
#include <string.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 1;
    bool eac = wcscmp(argv[2], L"eac") == 0;
    SetEnvironmentVariableW(L"ERBRIDGE", eac ? L"1" : nullptr);
    if (eac && (argc < 4 || !LoadLibraryW(argv[3]))) return 2;
    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) return 3;
    const char* exports[] = {"DirectInput8Create", "DllCanUnloadNow", "DllGetClassObject",
        "DllRegisterServer", "DllUnregisterServer", "GetdfDIJoystick"};
    for (auto name : exports) if (!GetProcAddress(proxy, name)) return 4;
    Sleep(1000); // give the loader thread time to make its eligibility decision
    auto file = mb::ipc_path(L"bridge.shm");
    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetModuleHandleW(L"erbridge_core.dll")) {
        fprintf(stderr, "Loader activated in a forbidden launch\n"); return 5;
    }
    auto canUnload = (HRESULT(WINAPI*)())GetProcAddress(proxy, "DllCanUnloadNow");
    canUnload(); // exercises the forwarding path to Windows' real DirectInput
    puts(eac ? "EAC_GUARD_PASS" : "PASSIVE_STEAM_LAUNCH_GUARD_PASS");
    return 0;
}
