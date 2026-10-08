#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mb { namespace action_detour {

// Pure snapshot validation: no process reads, execution, or patching. These are
// the only two entry points eligible for the cooperative compatibility path.
enum class Entry { ConsumePress, GetMsg };
struct Bytes {
    const uint8_t* data;
    size_t size;
};
struct Evidence {
    bool coopEnabled = false;
    bool seamlessLoaded = false;
    uint64_t imageBase = 0;
    uint64_t seamlessBase = 0;
    uint64_t relayAddress = 0;
    uint64_t handlerAddress = 0;
    Bytes live = {};
    Bytes relay = {};
    Bytes handler = {};
    // Read immediately before relayAddress: 20 bytes for ConsumePress, 33 for
    // GetMsg. Placement confirmed in coop/action-code-inspection.json.
    Bytes trampoline = {};
};

inline uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t u64(const uint8_t* p) {
    return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32);
}
inline bool available(Bytes b, size_t count) {
    return b.data && b.size == count;
}
inline bool abs_jump(const uint8_t* p, uint64_t target) {
    static const uint8_t opcode[] = {0xFF, 0x25, 0, 0, 0, 0};
    return memcmp(p, opcode, sizeof(opcode)) == 0 && u64(p + 6) == target;
}

inline uint64_t entry_rva(Entry e) {
    return e == Entry::ConsumePress ? 0xA64FA0 : e == Entry::GetMsg ? 0x266FC40 : 0;
}
inline size_t signature_size(Entry e) { return e == Entry::ConsumePress ? 25 : 15; }
inline size_t trampoline_size(Entry e) { return e == Entry::ConsumePress ? 20 : 33; }
inline uint64_t handler_rva(Entry e) { return e == Entry::ConsumePress ? 0x940F0 : 0x95D70; }

inline bool relative_target(uint64_t original, Bytes live, uint64_t* target) {
    if (!target || !live.data || live.size < 5 || live.data[0] != 0xE9 ||
        original > UINT64_MAX - 5) return false;
    const uint64_t next = original + 5;
    const uint32_t displacement = u32(live.data + 1);
    if (displacement <= 0x7FFFFFFF) {
        if (next > UINT64_MAX - displacement) return false;
        *target = next + displacement;
    } else {
        const uint64_t magnitude = UINT64_C(0x100000000) - displacement;
        if (next < magnitude) return false;
        *target = next - magnitude;
    }
    return true;
}

// Proves preservation of both known original signatures, relocated control
// flow, and the captured handler stub to its specific ersc.dll RVA. The runtime
// caller also verifies executable memory, PE section ownership, and all other
// action signatures. Never call any address obtained from these snapshots.
inline bool matches(Entry entry, const Evidence& e) {
    static const uint8_t consume[] = {
        0x4C,0x8B,0xC1,0x48,0x85,0xD2,0x74,0x61,0x0F,0xB6,0x4A,0x20,
        0xF6,0xC1,0x04,0x74,0x58,0x41,0x80,0xB8,0x81,0,0,0,0
    };
    static const uint8_t message[] = {
        0x3B,0x51,0x10,0x73,0x29,0x44,0x3B,0x41,0x14,0x73,0x23,
        0x48,0x8B,0x41,0x08
    };
    const bool isConsume = entry == Entry::ConsumePress;
    if (!isConsume && entry != Entry::GetMsg) return false;
    const uint64_t rva = entry_rva(entry);
    const uint8_t* signature = isConsume ? consume : message;
    const size_t signatureSize = isConsume ? sizeof(consume) : sizeof(message);
    const size_t trampolineSize = trampoline_size(entry);
    if (!e.coopEnabled || !e.seamlessLoaded || !e.imageBase ||
        !e.seamlessBase || e.seamlessBase > UINT64_MAX - handler_rva(entry) ||
        e.imageBase > UINT64_MAX - rva - 64 || e.relayAddress < trampolineSize ||
        !available(e.live, signatureSize) ||
        !available(e.relay, 14) || !available(e.handler, 28) ||
        !available(e.trampoline, trampolineSize)) return false;
    if (memcmp(e.live.data + 5, signature + 5, signatureSize - 5) != 0) return false;

    const uint64_t original = e.imageBase + rva;
    uint64_t target;
    if (!relative_target(original, e.live, &target) || target != e.relayAddress || !e.handlerAddress ||
        !abs_jump(e.relay.data, e.handlerAddress)) return false;
    const uint8_t* h = e.handler.data;
    static const uint8_t transfer[] = {0x66,0x48,0x0F,0x6E,0xE8,0x49,0xBB};
    if (h[0] != 0x48 || h[1] != 0xB8 || !u64(h + 2) ||
        memcmp(h + 10, transfer, sizeof(transfer)) != 0 ||
        u64(h + 17) != e.seamlessBase + handler_rva(entry) ||
        h[25] != 0x41 || h[26] != 0xFF || h[27] != 0xE3) return false;

    const uint8_t* t = e.trampoline.data;
    if (isConsume) {
        // mov r8,rcx; test rdx,rdx; jmp original+6 (original JE remains there).
        return memcmp(t, consume, 6) == 0 && abs_jump(t + 6, original + 6);
    }
    // cmp edx,[rcx+10]; original JAE original+0x2e becomes inverse JB +14
    // over JMP_ABS(original+0x2e), then JMP_ABS(original+5).
    return memcmp(t, message, 3) == 0 && t[3] == 0x72 && t[4] == 0x0E &&
           abs_jump(t + 5, original + 0x2E) && abs_jump(t + 19, original + 5);
}

}}  // namespace mb::action_detour
