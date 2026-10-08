#pragma once
#include <windows.h>
#include <stdint.h>
#include <atomic>
#include <string>
#include "bridge_protocol.h"

namespace mb {

// log.cpp
#if defined(__GNUC__)
void log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#else
void log(const char* fmt, ...);
#endif
void log_close();
uint64_t now_ms();

// Native Windows and the Java mod must resolve the same absolute IPC directory.
// Wine keeps its original Z:\tmp\ermc path unless ERMC_DIR is explicitly set.
bool running_under_wine();
std::wstring ipc_path(const wchar_t* filename);
bool ipc_ensure_dir();
inline void compiler_barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }

// shm.cpp
bool shm_open();
void shm_close();
ErmcHeader* shm_header();
ErmcGameState* shm_state();
ErmcControl* shm_control();
ErmcCmdBlock* shm_cmd();
uint8_t* shm_cmd_resp();
ErmcRayHeader* shm_rays();
ErmcEntityTable* shm_entities();
ErmcPassageTable* shm_passages();
ErmcHunterEvents* shm_hunter();
ErmcDamageQueue* shm_damage();

// Seqlock writer for the state block (ER is the only writer).
void state_begin_write();
void state_end_write();
// Consistent snapshot of the control block written by Minecraft. Returns false if the
// writer never published anything.
bool control_snapshot(ErmcControl* out);

// memutil.cpp
// Native MSVC: use resident page protection or VirtualQuery's committed/readable
// regions. Observations are reused only during this thread's callback; the
// outermost scope discards them on entry/exit. Wine retains VirtualQuery.
// This is a map snapshot, not object lifetime protection: use mem_read for reads
// that can race streaming, or guard the caller's direct accesses with SEH.
// Optional counters/timings for this thread's outermost callback. Timings are QPC
// ticks; divide by QueryPerformanceFrequency to obtain seconds. Snapshots can be
// differenced between game-perf stages, including nested helpers. The completed
// callback's totals remain available until the next outer scope starts.
struct MemReadabilityStats {
    uint64_t checks = 0, cacheHits = 0, cacheProbes = 0;
    uint64_t queries = 0, queryFailures = 0, evictions = 0;
    uint64_t invalidations = 0, copyFaults = 0;
    uint64_t queryTicks = 0, queryMaxTicks = 0, validationTicks = 0;
    // Existing queries/queryTicks remain VirtualQuery-only for profile continuity.
    // validationTicks includes both APIs. These fields measure the resident path.
    uint64_t residentQueries = 0, residentFallbacks = 0;
    uint64_t residentQueryTicks = 0, residentQueryMaxTicks = 0;
};
MemReadabilityStats mem_readability_stats();
unsigned mem_readability_scope_begin(bool profile = false);
void mem_readability_scope_end(unsigned previousDepth);
void mem_readability_invalidate();  // after a known mapping/protection change
struct MemReadabilityScope {
    // Restore the entry depth, rather than decrementing: /EHsc SEH can skip a
    // nested helper's destructor before the outer callback guard returns.
    unsigned previousDepth;
    explicit MemReadabilityScope(bool profile = false) : previousDepth(mem_readability_scope_begin(profile)) {}
    ~MemReadabilityScope() { mem_readability_scope_end(previousDepth); }
    MemReadabilityScope(const MemReadabilityScope&) = delete;
    MemReadabilityScope& operator=(const MemReadabilityScope&) = delete;
};
bool mem_readable(const void* p, size_t len);
// False on an unreadable source or a native access/in-page fault while copying.
// On failure the destination may be partially modified; callers must ignore it.
bool mem_read(const void* p, void* out, size_t len);
bool mem_write(void* p, const void* src, size_t len);
// Scan [start, end) for a masked byte pattern. mask byte 0xFF = must match, 0x00 = wildcard.
size_t mem_scan(uintptr_t start, uintptr_t end, const uint8_t* pat, const uint8_t* mask,
                size_t len, uint32_t flags, uintptr_t* out, size_t maxOut);
// Parse an IDA-style signature ("48 8B ?? 05") and scan the main module's executable pages.
uintptr_t find_pattern(const char* sig);
uintptr_t main_module_base();
size_t main_module_size();

// debugcmd.cpp
void debugcmd_poll();

// frame.cpp: per-frame glue. on_frame() publishes the state block and the heartbeat. Until a
// real per-frame hook calls frame_claim_source(), the core's pacer thread calls it at ~60 Hz.
void on_frame();
void frame_claim_source();
bool frame_source_is_pacer();
HWND game_hwnd();
void perf_add_tick(double ms);

// game.cpp: Elden Ring-specific memory access and hooks.
bool game_init();
void game_detach();    // shutdown step 1: our tasks stop calling into this core
void game_shutdown();  // step 2, once nothing runs our code: hand the character back
void game_fill_state(ErmcGameState* st);

// compositor.cpp: draws Minecraft's frames into Elden Ring's at Present (D3D12).
bool compositor_init();
void compositor_poll();       // worker thread: retries compositor_init until the game has a swapchain
void compositor_depth_poll(); // worker thread: runs a requested scan for the game's depth buffers
void game_worker_poll();      // worker thread: lists doors, levers... near the player (reach)
void compositor_detach();    // shutdown step 1: the Present stubs go straight to the originals
void compositor_shutdown();  // step 2: release D3D12 objects
void compositor_note_applied_pose(uint64_t poseId);  // camera task: the pose this game frame uses
bool compositor_active();
bool compositor_world_active(bool passive);

// core.cpp: number of threads currently inside one of our detours. The loader only
// unloads the core once this drops to zero.
extern volatile LONG g_inflight;
struct InflightGuard {
    InflightGuard() { InterlockedIncrement(&g_inflight); }
    ~InflightGuard() { InterlockedDecrement(&g_inflight); }
};

}  // namespace mb
