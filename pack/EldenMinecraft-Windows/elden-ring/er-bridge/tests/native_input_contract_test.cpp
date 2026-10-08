// Production core.cpp input path with test-only key/window/SendInput fixtures.
// No real input, process launch, game, or IPC is used by this executable.
#include "../src/common.h"
#include "MinHook.h"
#include <vector>
#include <array>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
static ErmcHeader mailbox = {};
static HWND inputHost = (HWND)1, foreground = inputHost;
static std::array<bool, 256> keys = {};
static std::vector<std::vector<INPUT>> batches;
static UINT firstResult = 2, releaseResult = 1;
static int waits = 0, checks = 0, failures = 0;
static bool switchDuringKeys = false;
static HWND WINAPI fake_foreground() { return foreground; }
static SHORT WINAPI fake_key(int key) {
    if (switchDuringKeys) foreground = (HWND)2;
    return keys.at(key) ? SHORT(-32768) : 0;
}
static UINT WINAPI fake_send(UINT count, LPINPUT input, int size) {
    if (size != sizeof(INPUT)) return 0;
    batches.emplace_back(input, input + count);
    return batches.size() == 1 ? firstResult : releaseResult;
}
static void WINAPI fake_sleep(DWORD) { ++waits; }
namespace mb {
ErmcHeader* shm_header() { return &mailbox; }
HWND game_hwnd() { return inputHost; }
void log(const char*, ...) {}
void log_close() { std::abort(); }
bool shm_open() { std::abort(); }
void shm_close() { std::abort(); }
void debugcmd_poll() { std::abort(); }
void on_frame() { std::abort(); }
bool frame_source_is_pacer() { std::abort(); }
bool game_init() { std::abort(); }
void game_detach() { std::abort(); }
void game_shutdown() { std::abort(); }
bool compositor_init() { std::abort(); }
void compositor_poll() { std::abort(); }
void compositor_depth_poll() { std::abort(); }
void game_worker_poll() { std::abort(); }
void compositor_detach() { std::abort(); }
void compositor_shutdown() { std::abort(); }
}
extern "C" MH_STATUS WINAPI MH_Initialize() { std::abort(); }
extern "C" MH_STATUS WINAPI MH_Uninitialize() { std::abort(); }
extern "C" MH_STATUS WINAPI MH_DisableHook(LPVOID) { std::abort(); }
#define GetForegroundWindow fake_foreground
#define GetAsyncKeyState fake_key
#define SendInput fake_send
#define Sleep fake_sleep
#include "../src/core.cpp"
#undef Sleep
#undef SendInput
#undef GetAsyncKeyState
#undef GetForegroundWindow
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
static void reset() {
    mailbox = {}; inputHost = (HWND)1; foreground = inputHost; keys.fill(false);
    batches.clear(); firstResult = 2; releaseResult = 1; waits = 0; switchDuringKeys = false;
}
int main() {
    reset(); mb::dev_press_keys(); check(batches.empty(), "ordinary gameplay never injects keys");
    reset(); mailbox.debugFlags = 16 | 8; mb::dev_press_keys();
    check(mailbox.debugFlags == 8 && batches.size() == 1 && batches[0].size() == 2 && waits == 0,
          "one explicit debug request sends one consecutive batch without a held-key sleep");
    check(batches[0][0].type == INPUT_KEYBOARD && batches[0][1].type == INPUT_KEYBOARD &&
          batches[0][0].ki.wVk == 0 && batches[0][1].ki.wVk == 0 &&
          batches[0][0].ki.wScan == 1 && batches[0][1].ki.wScan == 1 &&
          batches[0][0].ki.dwFlags == KEYEVENTF_SCANCODE &&
          batches[0][1].ki.dwFlags == (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP),
          "only nonextended Escape scan 0x01 down/up is emitted, never a modifier or Win key");
    mb::dev_press_keys(); check(batches.size() == 1, "consumed request cannot repeat");
    for (int modifier : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                        VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN, VK_ESCAPE}) {
        reset(); keys[modifier] = true; mailbox.debugFlags = 16; mb::dev_press_keys();
        check(batches.empty() && keys[modifier] && mailbox.debugFlags == 0,
              "held key/modifier consumes the request without injecting or releasing physical input");
    }
    reset(); foreground = (HWND)2; mailbox.debugFlags = 16; mb::dev_press_keys();
    check(batches.empty() && mailbox.debugFlags == 0, "terminal/browser focus discards Escape instead of routing it globally");
    reset(); inputHost = nullptr; mailbox.debugFlags = 16; mb::dev_press_keys();
    check(batches.empty(), "absent game window fails closed");
    reset(); switchDuringKeys = true; mailbox.debugFlags = 16; mb::dev_press_keys();
    check(batches.empty(), "focus change during modifier scan fails the final foreground recheck");
    reset(); firstResult = 1; mailbox.debugFlags = 16; mb::dev_press_keys();
    check(batches.size() == 2 && batches[1].size() == 1 &&
          batches[1][0].ki.dwFlags == (KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP),
          "partial down-only SendInput result gets an immediate owned Escape release");
    reset(); firstResult = 0; mailbox.debugFlags = 16; mb::dev_press_keys();
    check(batches.size() == 1, "rejected pair cannot fabricate a key release without owning a key-down");
    std::printf("Native input contract: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}
