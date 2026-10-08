#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "bridge_protocol.h"

namespace mb {
inline bool valid_peer_steam(uint64_t id) {
    // Same public individual desktop-account range as Java ModeSnapshot.
    return id >= 0x0110000100000001ull && id <= 0x01100001ffffffffull;
}
// The sidecar's IPC seq is not network freshness: both streams must progress.
// A new identity/connection starts untrusted, including after core hot reload.
struct PeerModeLease {
    uint32_t pid = 0, seq = 0;
    uint64_t start = 0, steam = 0, epoch = 0, roster = 0;
    uint64_t ipcAt = 0, rosterAt = 0;
    bool ipcProgress = false, rosterProgress = false;
    ErmcPeerMode lastMode = {};
    uint64_t snapshotAt = 0;
    bool haveMode = false;
    void reset() { *this = {}; }
    bool update(const ErmcPeerMode& p, uint32_t mcPid, uint64_t mcStart,
                bool consumer, uint64_t now) {
        if (!consumer || !mcPid || !mcStart || p.version != ERMC_PEER_VERSION ||
            !p.seq || (p.seq & 1) || p.mcStartMs != mcStart || !valid_peer_steam(p.remoteSteamId) ||
            !p.peerEpoch || !p.rosterSequence || p.mode > ERMC_PEER_ELDEN_RING || p.canRenderPeer > 1) {
            reset(); return false;
        }
        if (pid != mcPid || start != mcStart || steam != p.remoteSteamId || epoch != p.peerEpoch ||
            p.rosterSequence < roster || now < ipcAt || now < rosterAt) {
            pid = mcPid; start = mcStart; steam = p.remoteSteamId; epoch = p.peerEpoch;
            seq = p.seq; roster = p.rosterSequence; ipcAt = rosterAt = now;
            ipcProgress = rosterProgress = false;
            return false;
        }
        if (seq != p.seq) { seq = p.seq; ipcAt = now; ipcProgress = true; }
        if (roster != p.rosterSequence) { roster = p.rosterSequence; rosterAt = now; rosterProgress = true; }
        return ipcProgress && rosterProgress && now - ipcAt <= 1500 && now - rosterAt <= 1500;
    }
    // A writer in its odd seqlock phase is not a mode change. Reuse only the
    // last validated snapshot briefly; neither freshness clock is renewed.
    bool observe(const ErmcPeerMode* snapshot, uint32_t mcPid, uint64_t mcStart,
                 bool consumer, uint64_t now, ErmcPeerMode* out) {
        if (!snapshot) {
            if (!consumer || !haveMode || pid != mcPid || start != mcStart ||
                now < snapshotAt || now - snapshotAt >= 250) { reset(); return false; }
            *out = lastMode;
            return update(lastMode, mcPid, mcStart, consumer, now);
        }
        bool fresh = update(*snapshot, mcPid, mcStart, consumer, now);
        if (!pid) return false;  // invalid snapshot already revoked the lease
        lastMode = *snapshot; snapshotAt = now; haveMode = true;
        *out = lastMode;
        return fresh;
    }
};

// Only a transient missing frame receives grace. Explicit loss of composition
// intent/focus/life/consumer, or peer eligibility, must bypass it at the caller.
struct PeerCompositeLease {
    static constexpr uint64_t graceMs = 250;
    uint64_t submittedAt = 0;
    bool submitted = false;
    void reset() { *this = {}; }
    bool update(uint64_t now, bool eligible, bool frameAvailable) {
        if (!eligible) { reset(); return false; }
        if (frameAvailable) { submittedAt = now; submitted = true; return true; }
        return submitted && now >= submittedAt && now - submittedAt < graceMs;
    }
};

struct NativePeerIdentity {
    uintptr_t ins = 0, session = 0, entry = 0, slot = 0;
    uint64_t handle = 0, steam = 0;
    bool same_body(const NativePeerIdentity& b) const {
        return ins == b.ins && handle == b.handle && steam == b.steam;
    }
    bool same(const NativePeerIdentity& b) const {
        return same_body(b) && session == b.session && entry == b.entry && slot == b.slot;
    }
};

inline bool peer_chr_type(int32_t type) {
    // Only human multiplayer roles. NPC, NPC summon/invader, ghost and unknown
    // roles are excluded; a permitted role alone never establishes identity.
    switch (type) {
    case 0: case 1: case 2: case 8: case 13: case 15: case 16: case 17: case 18: return true;
    default: return false;
    }
}
inline bool peer_pointer(uintptr_t p) { return p >= 0x10000 && p < 0x0000800000000000ull && !(p & 7); }

template<class Read> bool exact_peer_rtti(uintptr_t object, uintptr_t base, size_t imageSize,
                                         const char* expected, Read read) {
    auto image = [=](uintptr_t p, size_t n) { return p >= base && p - base < imageSize && n <= imageSize - (p - base); };
    uintptr_t vt = 0, col = 0;
    uint32_t info[6] = {};
    if (!peer_pointer(object) || !read(object, &vt, 8) || vt < 8 || !image(vt - 8, 16) ||
        !read(vt - 8, &col, 8) || !image(col, sizeof(info)) || !read(col, info, sizeof(info)) ||
        info[0] != 1 || info[1] != 0 || info[2] != 0 || col - base != info[5]) return false;
    size_t n = strlen(expected) + 1;
    char name[64] = {};
    uintptr_t desc = base + (uintptr_t)info[3];
    if (n > sizeof(name) || !image(desc, 16 + n) || !read(desc + 16, name, n)) return false;
    return memcmp(name, expected, n) == 0;
}

// ER 2.7.1.0 layout confirmed from executable accessors/RTTI and the source schema.
// Every use still requires exact live types and two matching valid identities;
// offsets alone never establish that an object is a multiplayer peer.
// https://github.com/vswarte/fromsoftware-rs/tree/main/crates/eldenring/src/cs
template<class Read> bool read_native_peer(uintptr_t ins, uintptr_t local, uintptr_t base, size_t imageSize,
                                           uintptr_t playerVtableRva, Read read, NativePeerIdentity* out) {
    if (!peer_pointer(ins) || ins == local) return false;
    NativePeerIdentity p; p.ins = ins;
    uintptr_t vt = 0;
    int32_t type = -1;
    uint64_t entrySteam = 0;
    if (!read(ins, &vt, 8) || vt != base + playerVtableRva ||
        !read(ins + 8, &p.handle, 8) || uint32_t(p.handle) == 0xffffffffu ||
        !read(ins + 0x68, &type, 4) || !peer_chr_type(type) ||
        !read(ins + 0x5B0, &p.session, 8) || !peer_pointer(p.session) ||
        !read(ins + 0x6B8, &p.entry, 8) || !peer_pointer(p.entry) ||
        !exact_peer_rtti(ins, base, imageSize, ".?AVPlayerIns@CS@@", read) ||
        !exact_peer_rtti(p.session, base, imageSize, ".?AVPlayerNetworkSession@CS@@", read) ||
        !read(p.session + 8, &p.steam, 8) || !valid_peer_steam(p.steam) ||
        !read(p.entry + 0x10, &entrySteam, 8) || entrySteam != p.steam) return false;
    *out = p;
    return true;
}

// Ownership contains keys, never dereferenceable capabilities. Callers may
// supply flags only from a freshly enumerated and revalidated matching object.
// Missing observations keep bounded pending restores; no stale pointer reads.
struct PeerRenderOwners {
    static constexpr uint8_t kAbsentObservationsBeforeEviction = 3;
    struct Slot {
        NativePeerIdentity id;
        bool used = false, restoreRender = false;
        uint8_t absentObservations = 0;
    } slots[8];
    void reset() { *this = {}; }
    // Supply this tick's freshly verified player set before apply(). Complete
    // must mean a full, unambiguous enumeration, not a partial/unreadable list.
    // This compares keys only; retired instance/session/slot pointers are never
    // followed. Absence alone keeps pending restores until capacity is needed.
    void observe(const NativePeerIdentity* current, size_t count, bool complete) {
        if ((!current && count) || count > 8) { count = 0; complete = false; }
        for (auto& s : slots) {
            if (!s.used) continue;
            const NativePeerIdentity* live = nullptr;
            bool addressReused = false;
            size_t steamMatches = 0;
            for (size_t i = 0; i < count; ++i) {
                if (s.id.same_body(current[i])) live = &current[i];
                else if (s.id.ins == current[i].ins) addressReused = true;
                if (s.id.steam == current[i].steam) ++steamMatches;
            }
            if (addressReused) {
                s = {};  // another handle/account at this address never inherits
            } else if (live) {
                s.id = *live;  // same live body may rebind session, entry or slot
                s.absentObservations = 0;
            } else if (complete && steamMatches == 1) {
                s = {};  // this account has exactly one freshly verified new body
            } else if (!complete) {
                s.absentObservations = 0;
            } else if (s.absentObservations < kAbsentObservationsBeforeEviction) {
                ++s.absentObservations;
            }
        }
    }
    bool apply(const NativePeerIdentity& current, uint8_t& flags, bool hide) {
        Slot* free = nullptr;
        Slot* owned = nullptr;
        Slot* absent = nullptr;
        for (auto& s : slots) {
            if (s.used && s.id.ins == current.ins && !s.id.same_body(current)) s = {};
            if (s.used && s.id.same_body(current)) {
                owned = &s;
                s.id = current;
                s.absentObservations = 0;
            }
            if (!s.used && !free) free = &s;
            if (s.used && s.absentObservations >= kAbsentObservationsBeforeEviction && !absent) absent = &s;
        }
        if (!hide) {
            if (owned) {
                if (owned->restoreRender) flags |= 8;
                *owned = {};
            }
            return false;
        }
        if (!owned) {
            if (!free) free = absent;
            if (!free) return false;  // no verified replacement/absence: keep visible
            *free = {};
            owned = free; owned->id = current; owned->used = true; owned->restoreRender = (flags & 8) != 0;
        }
        flags &= 0xf7u;
        return true;
    }
};
}  // namespace mb
