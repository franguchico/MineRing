#pragma once
#include <stdint.h>

// A persisted PID/heartbeat is not proof of a running consumer. Each calling thread
// must witness progress in the same MC session before doing work for it. Callers use
// a monotonic clock; no process handles, waits, IPC writes or path changes are needed.
namespace mb {
struct McHeartbeatWatch {
    uint32_t pid = 0;
    uint64_t start = 0, heartbeat = 0, changedAt = 0;
    bool progressed = false;

    constexpr bool update(uint32_t nextPid, uint64_t nextStart, uint64_t nextHeartbeat, uint64_t now) {
        if (!nextPid || nextPid != pid || nextStart != start) {
            pid = nextPid; start = nextStart; heartbeat = nextHeartbeat;
            changedAt = now; progressed = false;
            return false;
        }
        if (nextHeartbeat != heartbeat) {
            heartbeat = nextHeartbeat; changedAt = now;
            progressed = nextHeartbeat != 0;
        }
        return progressed && now - changedAt <= 1000;
    }
};
// Explicit opt-in diagnostics, shared by the game tick and native compositor.
// Existing debugFlags bits 0..4 keep their meanings. Bit 5 never changes gameplay.
constexpr uint32_t kNativePerfDebugFlag = 1u << 5;
}
