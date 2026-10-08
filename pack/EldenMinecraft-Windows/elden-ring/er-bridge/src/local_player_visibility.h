#pragma once
#include <stdint.h>

namespace mb {
struct LocalPlayerVisibilityIdentity {
    uintptr_t ins = 0, data = 0, phys = 0;
    uint64_t handle = 0;
    bool same(const LocalPlayerVisibilityIdentity& b) const {
        return ins == b.ins && data == b.data && phys == b.phys && handle == b.handle;
    }
};
// Only the already-validated main player's render byte is passed here. A short
// co-op readiness gap may keep it hidden, but never keeps its physics pinned.
// A gap does not renew the deadline. Explicit release/lifecycle changes bypass
// the grace, and a replacement character drops ownership without touching it.
struct LocalPlayerVisibility {
    static constexpr uint8_t renderBit = 1u << 3;
    static constexpr uint64_t graceMs = 250;
    bool hidden = false;
    bool restoreRender = false;
    uint64_t requestedAt = 0;
    LocalPlayerVisibilityIdentity identity;

    // A freshly validated replacement never inherits the old saved render bit.
    void observe(const LocalPlayerVisibilityIdentity& current) {
        if (!identity.same(current)) reset();
        identity = current;
    }

    void reset() { hidden = false; restoreRender = false; requestedAt = 0; identity = {}; }

    void release(uint8_t& flags) {
        if (hidden && restoreRender) flags |= renderBit;
        reset();
    }

    void apply(uint8_t& flags, uint64_t now, bool requested, bool allowGrace) {
        bool keep = requested || (hidden && allowGrace && now >= requestedAt &&
                                  now - requestedAt < graceMs);
        if (!keep) { release(flags); return; }
        if (!hidden) restoreRender = (flags & renderBit) != 0;
        if (requested) requestedAt = now;
        // The engine/mod may reassert rendering while our request is unchanged.
        flags &= (uint8_t)~renderBit;
        hidden = true;
    }
};
}  // namespace mb
