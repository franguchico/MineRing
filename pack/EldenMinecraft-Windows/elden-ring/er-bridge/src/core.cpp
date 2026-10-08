// erbridge_core.dll entry points, called by the dinput8 loader. Must be able to shut down
// cleanly: every hook is removed and no game thread may still be executing our code when
// the loader calls FreeLibrary.
#include "common.h"
#include "MinHook.h"

namespace mb {

volatile LONG g_inflight = 0;
static HANDLE g_worker = nullptr;
static volatile LONG g_stop = 0;

static HANDLE g_pacer = nullptr;

// Dev (debugFlags bit4, set by erctl): one Escape tap for a tutorial popup.
// SendInput uses the GLOBAL queue; never send a gameplay/debug key to the shell,
// a different foreground window, or a held Ctrl/Alt/Shift/Win chord. Do not
// release physical modifiers: the bridge never owns those keys.
static bool dev_escape_allowed(HWND host) {
    if (!host || GetForegroundWindow() != host) return false;
    const int held[] = {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
        VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN, VK_ESCAPE};
    for (int key : held) if (GetAsyncKeyState(key) & 0x8000) return false;
    return GetForegroundWindow() == host;  // recheck after reading key state
}
static void dev_press_keys() {
    ErmcHeader* h = shm_header();
    if (!(h->debugFlags & 16)) return;
    h->debugFlags = h->debugFlags & ~16u;
    if (!dev_escape_allowed(game_hwnd())) {
        log("dev: Escape discarded (host not focused or a key/modifier is held)");
        return;
    }
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = 0x01;
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    // One batch inserts a consecutive pair; no 150 ms focus-change window.
    UINT sent = SendInput(2, in, sizeof(INPUT));
    UINT released = sent == 1 ? SendInput(1, &in[1], sizeof(INPUT)) : 0;
    log("dev: Escape batch %u/2, partial-release %u", sent, released);
}

static DWORD WINAPI worker(LPVOID) {
    while (!g_stop) {
        dev_press_keys();
        debugcmd_poll();
        compositor_poll();
        compositor_depth_poll();
        game_worker_poll();
        Sleep(1);
    }
    return 0;
}

// Publishes the state block at ~60 Hz until a real per-frame hook takes over.
static DWORD WINAPI pacer(LPVOID) {
    while (!g_stop) {
        if (frame_source_is_pacer()) on_frame();
        Sleep(16);
    }
    return 0;
}

}  // namespace mb

extern "C" __declspec(dllexport) bool erb_core_init() {
    using namespace mb;
    if (!shm_open()) return false;
    log("core: init");
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log("core: MH_Initialize failed %d", st);
        return false;
    }
    bool ok = game_init();
    if (ok) compositor_init();  // optional: without it Minecraft shows in its own overlay window
    g_stop = 0;
    g_worker = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    g_pacer = CreateThread(nullptr, 0, pacer, nullptr, 0, nullptr);
    return ok;
}

extern "C" __declspec(dllexport) void erb_core_shutdown() {
    using namespace mb;
    log("core: shutdown");
    g_stop = 1;
    HANDLE* threads[] = {&g_worker, &g_pacer};
    for (HANDLE* t : threads) {
        if (*t) {
            WaitForSingleObject(*t, 2000);
            CloseHandle(*t);
            *t = nullptr;
        }
    }
    // Stop every entry into this core: game tasks, Present stubs, MinHook detours.
    game_detach();
    compositor_detach();
    MH_DisableHook(MH_ALL_HOOKS);
    // A thread may have jumped into a detour just before the prologue was restored; give
    // it time to register itself, then wait for every detour to return.
    Sleep(100);
    for (int i = 0; i < 400 && g_inflight > 0; i++) Sleep(5);
    if (g_inflight > 0) log("core: %ld hooks still in flight at shutdown", g_inflight);
    MH_Uninitialize();
    compositor_shutdown();
    game_shutdown();
    shm_close();
    log("core: shutdown complete");
    log_close();
}
