// Compile with /std:c++17 /Zs: static assertions execute the real policy at compile
// time. No binary, game process, IPC file, or synthetic window is needed.
#include "../src/consumer_liveness.h"

constexpr bool idle_session_is_not_live() {
    mb::McHeartbeatWatch watch;
    return !watch.update(0, 0, 42, 0) && !watch.update(7, 10, 42, 1) &&
        !watch.update(7, 10, 42, 500) && !watch.update(7, 10, 42, 5000);
}
constexpr bool progress_expires_and_recovers() {
    mb::McHeartbeatWatch watch;
    return !watch.update(7, 10, 42, 10) && watch.update(7, 10, 43, 20) &&
        watch.update(7, 10, 43, 1020) && !watch.update(7, 10, 43, 1021) &&
        watch.update(7, 10, 44, 1022);
}
constexpr bool new_session_needs_its_own_progress() {
    mb::McHeartbeatWatch watch;
    return !watch.update(7, 10, 42, 1) && watch.update(7, 10, 43, 2) &&
        !watch.update(8, 10, 43, 3) && watch.update(8, 10, 44, 4) &&
        !watch.update(8, 11, 45, 5) && watch.update(8, 11, 46, 6) &&
        !watch.update(0, 11, 46, 7) && !watch.update(8, 11, 47, 8);
}
constexpr bool zero_reset_is_inactive() {
    mb::McHeartbeatWatch watch;
    return !watch.update(7, 10, 0, 1) && watch.update(7, 10, 1, 2) &&
        !watch.update(7, 10, 0, 3) && watch.update(7, 10, 1, 4);
}
static_assert(idle_session_is_not_live(), "Persisted PID/counter must not activate work");
static_assert(progress_expires_and_recovers(), "Only recent observed progress is live");
static_assert(new_session_needs_its_own_progress(), "Session changes revoke old liveness");
static_assert(zero_reset_is_inactive(), "A heartbeat reset cannot activate stale work");
