#include "common.h"
#include <string.h>
#include <stdlib.h>
#include <vector>

#if defined(_MSC_VER) && (!defined(ERMC_NATIVE_WINDOWS) || ERMC_NATIVE_WINDOWS)
#define ERMC_NATIVE_MEMORY 1
#else
#define ERMC_NATIVE_MEMORY 0
#endif

#if ERMC_NATIVE_MEMORY
#include <psapi.h>
#endif
namespace mb {

static bool protect_readable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

#if ERMC_NATIVE_MEMORY
struct ReadableRegion {
    uintptr_t begin, end;
    bool readable;
};
// Bounded storage, no allocation or locks on game/render/worker callbacks. Each
// entry covers only a VirtualQuery region or one validated resident base page.
// Never carry a positive observation into another callback or another thread.
static thread_local struct {
    ReadableRegion regions[64];
    unsigned depth, count, next, recent;
    bool profile;
    MemReadabilityStats stats;
} g_readability = {};
#endif

MemReadabilityStats mem_readability_stats() {
#if ERMC_NATIVE_MEMORY
    return g_readability.stats;
#else
    return {};
#endif
}

void mem_readability_invalidate() {
#if ERMC_NATIVE_MEMORY
    if (g_readability.profile) ++g_readability.stats.invalidations;
    g_readability.count = g_readability.next = g_readability.recent = 0;
#endif
}

unsigned mem_readability_scope_begin(bool profile) {
#if ERMC_NATIVE_MEMORY
    unsigned previous = g_readability.depth;
    if (!previous) {
        mem_readability_invalidate();
        g_readability.stats = {};
        g_readability.profile = profile;
    }
    ++g_readability.depth;
    return previous;
#else
    (void)profile;
    return 0;
#endif
}

void mem_readability_scope_end(unsigned previousDepth) {
#if ERMC_NATIVE_MEMORY
    g_readability.depth = previousDepth;
    if (!previousDepth) {
        // Scope boundaries retire the map, but aren't operational invalidations.
        // Keep the completed profile available to the caller after destruction.
        g_readability.profile = false;
        mem_readability_invalidate();
    }
#else
    (void)previousDepth;
#endif
}

#if ERMC_NATIVE_MEMORY
static SIZE_T resident_page_size() {
    // Windows' page size is immutable, unlike mapping/protection observations.
    // Keep the existing VirtualQuery backend on Wine and unusual page sizes.
    static const SIZE_T size = []() -> SIZE_T {
        if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return 0;
        SYSTEM_INFO info; GetSystemInfo(&info);
        return info.dwPageSize && !(info.dwPageSize & (info.dwPageSize - 1)) ? info.dwPageSize : 0;
    }();
    return size;
}

static void cache_readable_region(const ReadableRegion& region) {
    if (!g_readability.depth) return;
    if (g_readability.profile && g_readability.count == 64) ++g_readability.stats.evictions;
    unsigned slot = g_readability.count < 64 ? g_readability.count++ : g_readability.next++ % 64;
    g_readability.regions[slot] = region;
    g_readability.recent = slot;
}

static bool readable_region(uintptr_t address, ReadableRegion* region, bool residentProbe) {
    const bool profile = g_readability.profile;
    if (g_readability.depth && g_readability.count) {
        const auto& recent = g_readability.regions[g_readability.recent];
        if (profile) ++g_readability.stats.cacheProbes;
        if (address >= recent.begin && address < recent.end) {
            if (profile) ++g_readability.stats.cacheHits;
            *region = recent;
            return true;
        }
        for (unsigned i = 0; i < g_readability.count; ++i) {
            if (i == g_readability.recent) continue;
            const auto& cached = g_readability.regions[i];
            if (profile) ++g_readability.stats.cacheProbes;
            if (address >= cached.begin && address < cached.end) {
                if (profile) ++g_readability.stats.cacheHits;
                g_readability.recent = i;
                *region = cached;
                return true;
            }
        }
    }
    if (residentProbe) {
        const SIZE_T page = resident_page_size();
        const uintptr_t begin = address & ~(uintptr_t(page) - 1);
        if (page && begin <= UINTPTR_MAX - page) {
            PSAPI_WORKING_SET_EX_INFORMATION ws = {};
            ws.VirtualAddress = (void*)address;
            LARGE_INTEGER beginQuery = {}, endQuery;
            if (profile) QueryPerformanceCounter(&beginQuery);
            const BOOL queried = K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof(ws));
            if (profile) {
                QueryPerformanceCounter(&endQuery);
                const uint64_t elapsed = (uint64_t)(endQuery.QuadPart - beginQuery.QuadPart);
                ++g_readability.stats.residentQueries;
                g_readability.stats.residentQueryTicks += elapsed;
                if (elapsed > g_readability.stats.residentQueryMaxTicks)
                    g_readability.stats.residentQueryMaxTicks = elapsed;
            }
            // Only Valid=1 makes Win32Protection meaningful. A valid resident
            // base page is backed/mapped; its actual protection applies to the
            // whole base page, not to adjacent pages or the allocation region.
            // Locked/large pages can include special nonpageable mappings, so
            // they retain VirtualQuery's explicit MEM_COMMIT check as well.
            // Nonresident and failed responses likewise need VirtualQuery.
            if (queried && ws.VirtualAttributes.Valid &&
                (ws.VirtualAttributes.Bad || (!ws.VirtualAttributes.LargePage && !ws.VirtualAttributes.Locked))) {
                *region = {begin, begin + page, !ws.VirtualAttributes.Bad &&
                           protect_readable((DWORD)ws.VirtualAttributes.Win32Protection)};
                cache_readable_region(*region);
                return true;
            }
            if (profile) ++g_readability.stats.residentFallbacks;
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    LARGE_INTEGER beginQuery = {}, endQuery;
    if (profile) QueryPerformanceCounter(&beginQuery);
    SIZE_T queried = VirtualQuery((void*)address, &mbi, sizeof(mbi));
    if (profile) {
        QueryPerformanceCounter(&endQuery);
        uint64_t elapsed = (uint64_t)(endQuery.QuadPart - beginQuery.QuadPart);
        ++g_readability.stats.queries;
        g_readability.stats.queryTicks += elapsed;
        if (elapsed > g_readability.stats.queryMaxTicks) g_readability.stats.queryMaxTicks = elapsed;
    }
    if (!queried) {
        if (profile) ++g_readability.stats.queryFailures;
        return false;
    }
    uintptr_t begin = (uintptr_t)mbi.BaseAddress, end = begin + mbi.RegionSize;
    if (begin > address || end <= address || end < begin) {
        if (profile) ++g_readability.stats.queryFailures;
        return false;
    }
    *region = {begin, end, mbi.State == MEM_COMMIT && protect_readable(mbi.Protect)};
    cache_readable_region(*region);
    return true;
}

static int memory_read_exception(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
           code == EXCEPTION_GUARD_PAGE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool range_readable(const void* p, size_t len) {
    uintptr_t a = (uintptr_t)p, end = a + len;
    if (a < 0x10000 || end < a) return false;
#if ERMC_NATIVE_MEMORY
    const SIZE_T page = resident_page_size();
    // Bound per-page queries for bulk scans/copies. VirtualQuery can validate a
    // large uniform region in one call; pointer/object reads take the fast path.
    const bool residentProbe = page && len && (end - 1) / page - a / page < 16;
#endif
    while (a < end) {
#if ERMC_NATIVE_MEMORY
        ReadableRegion region;
        if (!readable_region(a, &region, residentProbe) || !region.readable) return false;
        a = region.end;
#else
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !protect_readable(mbi.Protect)) return false;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) return false;
        a = next;
#endif
    }
    return true;
}

bool mem_readable(const void* p, size_t len) {
#if ERMC_NATIVE_MEMORY
    if (g_readability.profile) {
        LARGE_INTEGER begin, end;
        ++g_readability.stats.checks;
        QueryPerformanceCounter(&begin);
        bool readable = range_readable(p, len);
        QueryPerformanceCounter(&end);
        g_readability.stats.validationTicks += (uint64_t)(end.QuadPart - begin.QuadPart);
        return readable;
    }
#endif
    return range_readable(p, len);
}

bool mem_read(const void* p, void* out, size_t len) {
    if (!mem_readable(p, len)) return false;
#if ERMC_NATIVE_MEMORY
    // Neither metadata query is a lifetime guarantee, even without caching. Streaming
    // can unmap/reprotect the source after validation. SEH guards the actual copy
    // without a self ReadProcessMemory kernel copy for every pointer-sized read.
    __try {
        memcpy(out, p, len);
        return true;
    } __except (memory_read_exception(GetExceptionCode())) {
        if (g_readability.profile) ++g_readability.stats.copyFaults;
        mem_readability_invalidate();
        return false;
    }
#else
    memcpy(out, p, len);
    return true;
#endif
}

bool mem_write(void* p, const void* src, size_t len) {
    mem_readability_invalidate();
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    DWORD old;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, src, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}

static bool region_wanted(const MEMORY_BASIC_INFORMATION& mbi, uint32_t flags) {
    if (mbi.State != MEM_COMMIT || !protect_readable(mbi.Protect)) return false;
    if (flags & ERMC_SCAN_EXEC_ONLY) {
        if (!(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
    }
    if (mbi.Type == MEM_IMAGE) return (flags & ERMC_SCAN_IMAGE) != 0;
    if (mbi.Type == MEM_PRIVATE) return (flags & ERMC_SCAN_PRIVATE) != 0;
    if (mbi.Type == MEM_MAPPED) return (flags & ERMC_SCAN_MAPPED) != 0;
    return false;
}

size_t mem_scan(uintptr_t start, uintptr_t end, const uint8_t* pat, const uint8_t* mask,
                size_t len, uint32_t flags, uintptr_t* out, size_t maxOut) {
    if (len == 0) return 0;
    // Anchor on the first fully-specified byte to use memchr.
    size_t anchor = 0;
    while (anchor < len && mask[anchor] != 0xFF) anchor++;
    size_t found = 0;
    uintptr_t a = start < 0x10000 ? 0x10000 : start;
    while (a < end && found < maxOut) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        uintptr_t rbase = (uintptr_t)mbi.BaseAddress, rend = rbase + mbi.RegionSize;
        if (rend <= a) break;
        if (region_wanted(mbi, flags)) {
            uintptr_t s = a > rbase ? a : rbase;
            uintptr_t e = rend < end ? rend : end;
            if (e - s >= len) {
                const uint8_t* p = (const uint8_t*)s;
                const uint8_t* last = (const uint8_t*)(e - len);
                while (p <= last && found < maxOut) {
                    if (anchor < len) {
                        const uint8_t* q = (const uint8_t*)memchr(p + anchor, pat[anchor],
                                                                (size_t)(last - p) + 1);
                        if (!q) break;
                        p = q - anchor;
                    }
                    size_t i = 0;
                    for (; i < len; i++)
                        if ((p[i] & mask[i]) != (pat[i] & mask[i])) break;
                    if (i == len) out[found++] = (uintptr_t)p;
                    p++;
                }
            }
        }
        a = rend;
    }
    return found;
}

uintptr_t main_module_base() { return (uintptr_t)GetModuleHandleA(nullptr); }

size_t main_module_size() {
    uint8_t* base = (uint8_t*)main_module_base();
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

static bool parse_sig(const char* sig, std::vector<uint8_t>& pat, std::vector<uint8_t>& mask) {
    const char* s = sig;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') {
            pat.push_back(0);
            mask.push_back(0);
            s += (s[1] == '?') ? 2 : 1;
        } else {
            char hex[3] = {s[0], s[1], 0};
            char* endp;
            long v = strtol(hex, &endp, 16);
            if (endp != hex + 2) return false;
            pat.push_back((uint8_t)v);
            mask.push_back(0xFF);
            s += 2;
        }
    }
    return !pat.empty();
}

uintptr_t find_pattern(const char* sig) {
    std::vector<uint8_t> pat, mask;
    if (!parse_sig(sig, pat, mask)) return 0;
    uintptr_t base = main_module_base();
    uintptr_t hits[2];
    size_t n = mem_scan(base, base + main_module_size(), pat.data(), mask.data(), pat.size(),
                        ERMC_SCAN_IMAGE | ERMC_SCAN_EXEC_ONLY, hits, 2);
    if (n != 1) {
        log("find_pattern: %zu matches for '%s'", n, sig);
        return n ? hits[0] : 0;
    }
    return hits[0];
}

}  // namespace mb
