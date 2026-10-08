// Standalone native memory test: compile this file only; it includes memutil.cpp.
// No game process or IPC is accessed. API instrumentation calls the real Windows
// APIs except for explicit failure/response fixtures. All memory is test-owned.
#include "../src/common.h"
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <vector>

static thread_local uint64_t queryCalls = 0;
static thread_local const void* faultAfterQuery = nullptr;
static thread_local bool failNextQuery = false;
static thread_local DWORD queryDelayMs = 0;
static thread_local uint64_t independentQueryTicks = 0;
static thread_local uint64_t residentCalls = 0;
// Run the original 67 checks with a failed resident query to exercise the exact
// pre-existing VirtualQuery fallback contract, then run the live resident path.
static thread_local bool forceResidentFallback = true;
static thread_local const void* faultAfterResidentQuery = nullptr;
enum class ResidentReply { Real, Failure, InvalidReadable, Bad, LargePage, Guard };
static thread_local ResidentReply nextResidentReply = ResidentReply::Real;
static BOOL WINAPI tracked_resident_query(HANDLE process, PVOID buffer, DWORD size) {
    ++residentCalls;
    if (forceResidentFallback) return FALSE;
    const auto reply = nextResidentReply;
    nextResidentReply = ResidentReply::Real;
    if (reply == ResidentReply::Failure) return FALSE;
    const BOOL result = K32QueryWorkingSetEx(process, buffer, size);
    auto* ws = (PSAPI_WORKING_SET_EX_INFORMATION*)buffer;
    if (result && size == sizeof(*ws)) {
        if (reply != ResidentReply::Real) {
            ws->VirtualAttributes.Flags = 0;
            ws->VirtualAttributes.Valid = reply != ResidentReply::InvalidReadable;
            ws->VirtualAttributes.Win32Protection = PAGE_READWRITE;
            if (reply == ResidentReply::Bad) ws->VirtualAttributes.Bad = 1;
            if (reply == ResidentReply::LargePage) ws->VirtualAttributes.LargePage = 1;
            if (reply == ResidentReply::Guard) ws->VirtualAttributes.Win32Protection |= PAGE_GUARD;
        }
        if (ws->VirtualAddress == faultAfterResidentQuery) {
            faultAfterResidentQuery = nullptr;
            DWORD old;
            if (!VirtualProtect(ws->VirtualAddress, 1, PAGE_NOACCESS, &old)) ExitProcess(2);
        }
    }
    return result;
}
static SIZE_T WINAPI tracked_query(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size) {
    ++queryCalls;
    if (failNextQuery) { failNextQuery = false; return 0; }
    LARGE_INTEGER begin = {}, end;
    if (queryDelayMs) {
        QueryPerformanceCounter(&begin);
        Sleep(queryDelayMs);  // fixture-only latency; never compiled into memutil
        queryDelayMs = 0;
    }
    SIZE_T result = VirtualQuery(address, info, size);
    if (begin.QuadPart) {
        QueryPerformanceCounter(&end);
        independentQueryTicks = (uint64_t)(end.QuadPart - begin.QuadPart);
    }
    if (result && address == faultAfterQuery) {
        faultAfterQuery = nullptr;
        DWORD old;
        if (!VirtualProtect(info->BaseAddress, info->RegionSize, PAGE_NOACCESS, &old)) ExitProcess(2);
    }
    return result;
}
#define VirtualQuery tracked_query
#define K32QueryWorkingSetEx tracked_resident_query
#include "../src/memutil.cpp"
#undef VirtualQuery
#undef K32QueryWorkingSetEx

#if !ERMC_NATIVE_MEMORY
#error This test requires the native MSVC memory backend.
#endif

namespace mb {
void log(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); putchar('\n');
}
}

static int failures = 0, checks = 0;
static void check(bool pass, const char* message) {
    ++checks;
    printf("%s: %s\n", pass ? "PASS" : "FAIL", message);
    if (!pass) ++failures;
}
static void require(bool pass, const char* message) {
    if (!pass) { printf("FATAL: %s (Windows error %lu)\n", message, GetLastError()); ExitProcess(2); }
}
static void protect(void* address, SIZE_T size, DWORD mode) {
    DWORD old; require(VirtualProtect(address, size, mode, &old) != FALSE, "fixture protection change");
}
static uint8_t* allocate(SIZE_T size, DWORD protection = PAGE_READWRITE) {
    auto* p = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, protection);
    require(p != nullptr, "fixture allocation");
    return p;
}

struct ThreadFixture { const void* source; uint64_t queries; bool pass; bool resident = false; uint64_t residentQueries = 0; };
static DWORD WINAPI thread_read(void* arg) {
    auto* fixture = (ThreadFixture*)arg;
    forceResidentFallback = !fixture->resident;
    uint64_t before = queryCalls, residentBefore = residentCalls, value = 0;
    fixture->pass = mb::mem_read(fixture->source, &value, sizeof(value));
    fixture->queries = queryCalls - before;
    fixture->residentQueries = residentCalls - residentBefore;
    return 0;
}

static void faulting_nested_helper(uint8_t* source, SIZE_T page) {
    mb::MemReadabilityScope nested;
    require(mb::mem_readable(source, 8), "prime nested callback cache");
    protect(source, page, PAGE_NOACCESS);
    volatile uint64_t value = *(volatile uint64_t*)source;  // deterministic streaming race
    (void)value;
}
// Like the game's native guard, this thunk owns no objects requiring C++ unwinding.
// Under /EHsc a hardware exception can skip the helper's RAII destructor. The
// external callback scope must still leave no active cache after returning.
static bool guarded_callback(uint8_t* source, SIZE_T page) {
    __try {
        faulting_nested_helper(source, page);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        mb::mem_readability_invalidate();
        return false;
    }
}
static void test_callback_guard(SIZE_T page) {
    auto* source = allocate(page);
    {
        mb::MemReadabilityScope callback;
        check(!guarded_callback(source, page), "native thunk catches a raw read fault in a nested RAII helper");
    }
    protect(source, page, PAGE_READWRITE);
    uint64_t before = queryCalls;
    check(mb::mem_readable(source, 8) && mb::mem_readable(source, 8) && queryCalls - before == 2,
          "outer callback exit disables caching even when SEH skips an inner destructor");
    VirtualFree(source, 0, MEM_RELEASE);
}

static void test_safety(SIZE_T page) {
    using namespace mb;
    auto* p = (uint8_t*)VirtualAlloc(nullptr, page * 4, MEM_RESERVE, PAGE_NOACCESS);
    require(p != nullptr, "mixed-protection reservation");
    require(VirtualAlloc(p, page * 3, MEM_COMMIT, PAGE_READWRITE) == p, "mixed-protection commit");
    memset(p, 0x5a, page * 3);
    protect(p + page, page, PAGE_READONLY);
    protect(p + page * 2, page, PAGE_NOACCESS);
    uint8_t out[32] = {};

    check(mem_read(p + page - 8, out, 16) && out[0] == 0x5a && out[15] == 0x5a,
          "copy crosses committed RW/RO regions successfully");
    check(!mem_readable(p + page * 2 - 8, 16) && !mem_read(p + page * 2 - 8, out, 16),
          "read crossing into NOACCESS is rejected");
    check(!mem_readable(p + page * 3, 8) && !mem_read(p + page * 3, out, 8),
          "reserved but uncommitted source is rejected");
    uint64_t before = queryCalls;
    check(!mem_readable(nullptr, 8) && !mem_readable((void*)0xffff, 8) &&
          !mem_readable((void*)(UINTPTR_MAX - 3), 8) && queryCalls == before,
          "null/low/overflowing ranges fail before VirtualQuery");
    check(mem_readable(p, 0), "empty valid-address range preserves existing behavior");

    protect(p + page, page, PAGE_READWRITE | PAGE_GUARD);
    check(!mem_readable(p + page, 8) && !mem_read(p + page, out, 8),
          "guard source is rejected without copying");
    MEMORY_BASIC_INFORMATION info = {};
    require(VirtualQuery(p + page, &info, sizeof(info)) != 0, "guard flag query");
    check((info.Protect & PAGE_GUARD) != 0, "rejection leaves the guard flag armed");
    protect(p + page, page, PAGE_READONLY);
    protect(p + page * 2, page, PAGE_EXECUTE);
    check(!mem_readable(p + page * 2, 8), "execute-only memory is not treated as readable");
    protect(p + page * 2, page, PAGE_NOACCESS);

    before = queryCalls;
    check(mem_readable(p, 8) && mem_readable(p, 8) && queryCalls - before == 2,
          "outside a scope every validation queries the current map");
    {
        MemReadabilityScope scope;
        before = queryCalls;
        check(mem_readable(p, 8) && mem_readable(p + 48, 8) && queryCalls - before == 1,
              "same queried region is reused inside the callback");
        before = queryCalls;
        check(mem_readable(p + page, 8) && queryCalls - before == 1,
              "an unknown readable region needs a fresh query");
        before = queryCalls;
        check(!mem_readable(p + page * 2, 8) && !mem_readable(p + page * 2, 8) && queryCalls - before == 1,
              "negative observations stay fail-closed within the callback");
        protect(p + page * 2, page, PAGE_READWRITE);
        check(!mem_readable(p + page * 2, 8), "newly readable region cannot bypass a cached denial");
        mem_readability_invalidate();
        check(mem_readable(p + page * 2, 8), "explicit invalidation refreshes negative observations");
        before = queryCalls;
        {
            MemReadabilityScope nested;
            check(mem_readable(p + page * 2, 8) && queryCalls == before,
                  "nested helper scope preserves the outer callback snapshot");
        }
        check(mem_readable(p + page * 2, 8) && queryCalls == before,
              "nested exit keeps the outer callback cache active");
        mem_readability_invalidate();
        require(mem_readable(p, 8), "prime caller thread cache");
        ThreadFixture fixture = {p, 0, false};
        HANDLE thread = CreateThread(nullptr, 0, thread_read, &fixture, 0, nullptr);
        require(thread != nullptr, "independent reader thread");
        require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "reader thread completion");
        CloseHandle(thread);
        check(fixture.pass && fixture.queries == 1, "worker thread cannot inherit another callback's cache");
    }
    protect(p, page, PAGE_NOACCESS);
    before = queryCalls;
    check(!mem_readable(p, 8) && queryCalls - before == 1, "scope exit discards a prior positive observation");
    protect(p, page, PAGE_READWRITE);
    {
        MemReadabilityScope scope;
        check(mem_readable(p, 8), "new callback refreshes a prior denial");
        protect(p, page, PAGE_NOACCESS);  // simulate streaming after the successful query
        check(!mem_read(p, out, 8), "SEH rejects source reprotection after a cached positive query");
        before = queryCalls;
        check(!mem_readable(p, 8) && queryCalls - before == 1,
              "copy fault invalidates the entire callback cache");
    }
    protect(p, page, PAGE_READWRITE);
    faultAfterQuery = p;
    check(!mem_read(p, out, 8), "SEH also closes the uncached query-to-copy race");
    protect(p, page, PAGE_READWRITE);
    check(!mem_read(p, p + page * 3, 8), "SEH rejects an invalid output buffer");

    {
        MemReadabilityScope scope;
        failNextQuery = true;
        before = queryCalls;
        check(!mem_readable(p, 8) && mem_readable(p, 8) && queryCalls - before == 2,
              "failed queries are not cached as readable");
        protect(p + page, page, PAGE_NOACCESS);
        check(!mem_readable(p + page, 8), "prime denied page before a protected write");
        uint64_t value = 17;
        check(mem_write(p + page, &value, sizeof(value)), "existing protected write succeeds");
        before = queryCalls;
        check(!mem_readable(p + page, 8) && queryCalls - before == 1,
              "mem_write invalidates observations and restores original protection");
    }
    VirtualFree(p, 0, MEM_RELEASE);

    auto* disposable = allocate(page);
    {
        MemReadabilityScope scope;
        require(mem_readable(disposable, 8), "prime streaming allocation");
        require(VirtualFree(disposable, page, MEM_DECOMMIT) != FALSE, "simulate streaming decommit");
        check(!mem_read(disposable, out, 8), "SEH contains decommit during an active callback");
        check(!mem_readable(disposable, 8), "decommit fault cannot leave a stale positive entry");
    }
    VirtualFree(disposable, 0, MEM_RELEASE);
}

static void test_profiling(SIZE_T page) {
    using namespace mb;
    auto* source = allocate(page * 3);
    memset(source, 0x19, page * 3);
    protect(source + page, page, PAGE_READONLY);
    protect(source + page * 2, page, PAGE_NOACCESS);
    {
        MemReadabilityScope callback;
        uint64_t before = queryCalls;
        require(mem_readable(source, 8) && mem_readable(source, 8), "ordinary profiled fixture reads");
        auto stats = mem_readability_stats();
        check(queryCalls - before == 1 && stats.checks == 0 && stats.queries == 0 && stats.validationTicks == 0,
              "profiling is off by default while ordinary scoped caching still works");
    }
    MemReadabilityStats completed;
    {
        MemReadabilityScope callback(true);
        uint64_t before = queryCalls, value = 0;
        require(mem_readable(source, 8) && mem_read(source + 8, &value, 8), "profile readable fixture");
        {
            MemReadabilityScope nested;  // inherit outer profiling, don't reset stage totals
            require(mem_readable(source + 16, 8), "nested profile read");
        }
        auto firstStage = mem_readability_stats();
        require(mem_readable(source + page, 8) && !mem_readable(source + page * 2, 8) &&
                !mem_readable(source + page * 2, 8), "profile mixed-protection fixture");
        auto secondStage = mem_readability_stats();
        check(secondStage.queries == queryCalls - before && secondStage.queries == 3 &&
              secondStage.checks == 6 && secondStage.cacheHits == 3 && secondStage.queryFailures == 0,
              "profile counts agree with independent real-query interception, including negative cache hits");
        check(secondStage.queries - firstStage.queries == 2 && secondStage.checks - firstStage.checks == 3 &&
              secondStage.cacheHits - firstStage.cacheHits == 1,
              "nested stage snapshots isolate fresh queries without resetting the callback");
        check(secondStage.queryTicks > 0 && secondStage.validationTicks >= secondStage.queryTicks &&
              secondStage.queryMaxTicks <= secondStage.queryTicks,
              "query timing is contained within total readability validation timing");

        mem_readability_invalidate();
        require(mem_readable(source, 8), "prime profiled streaming source");
        protect(source, page, PAGE_NOACCESS);
        check(!mem_read(source, &value, 8), "profiling preserves SEH protection after reprotection");
        auto fault = mem_readability_stats();
        check(fault.copyFaults == 1 && fault.invalidations == 2 && fault.queries == 4,
              "copy faults and explicit invalidations are counted without hiding previous query cost");
        require(!mem_readable(source, 8), "refresh protected source after profiled fault");
        failNextQuery = true;
        require(!mem_readable(source + page, 8), "inject metadata query failure");
        require(mem_readable(source + page, 8), "retry real query after failure");
        check(mem_readability_stats().queryFailures == 1,
              "failed metadata query is reported, retried and never accepted as readable");

        mem_readability_invalidate();
        queryDelayMs = 5;
        auto delayedBefore = mem_readability_stats();
        require(mem_readable(source + page, 8), "deliberately slow fresh fixture query");
        auto delayedAfter = mem_readability_stats();
        check(independentQueryTicks > 0 && delayedAfter.queryTicks - delayedBefore.queryTicks >= independentQueryTicks &&
              delayedAfter.queryMaxTicks >= independentQueryTicks,
              "profiling captures independently timed slow-query latency");
        require(mem_readable(source + page, 8), "cached slow-query result");
        check(mem_readability_stats().queryTicks == delayedAfter.queryTicks,
              "cache hits do not attribute extra time to VirtualQuery");
        completed = mem_readability_stats();
    }
    auto retained = mem_readability_stats();
    check(retained.checks == completed.checks && retained.queries == completed.queries &&
          retained.queryTicks == completed.queryTicks && retained.invalidations == completed.invalidations,
          "callback exit retains diagnostic totals while retiring all mapping observations");
    {
        MemReadabilityScope next(true);
        check(mem_readability_stats().checks == 0 && mem_readability_stats().queries == 0,
              "a new callback starts a fresh profile without inheriting prior query costs");
    }
    protect(source, page, PAGE_READWRITE);
    VirtualFree(source, 0, MEM_RELEASE);
}

struct BenchResult { double ms; uint64_t queries; uint64_t checksum; uint64_t residentQueries; };
static BenchResult sample(const std::vector<uint8_t*>& sources, unsigned callbacks, bool cached, bool profile = false) {
    LARGE_INTEGER frequency, begin, end;
    QueryPerformanceFrequency(&frequency);
    uint64_t before = queryCalls, residentBefore = residentCalls, checksum = 0;
    QueryPerformanceCounter(&begin);
    for (unsigned tick = 0; tick < callbacks; ++tick) {
        unsigned previousDepth = cached ? mb::mem_readability_scope_begin(profile) : 0;
        for (const auto* p : sources) {
            uint64_t value = 0;
            if (!mb::mem_read(p, &value, sizeof(value))) ExitProcess(2);
            checksum += value;
        }
        if (cached) mb::mem_readability_scope_end(previousDepth);
    }
    QueryPerformanceCounter(&end);
    return {(end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart,
            queryCalls - before, checksum, residentCalls - residentBefore};
}
static void benchmark(const char* name, const std::vector<uint8_t*>& sources, uint64_t uniqueRegions) {
    auto plain = sample(sources, 1, false), cached = sample(sources, 1, true);
    check(plain.queries == sources.size() && cached.queries == uniqueRegions && plain.checksum == cached.checksum,
          name);
    unsigned callbacks = (unsigned)std::max(1.0, std::min(5000.0, 40.0 / std::max(plain.ms, 0.001)));
    std::vector<double> plainMs, cachedMs, profileMs;
    for (unsigned i = 0; i < 5; ++i) {
        plain = sample(sources, callbacks, false);
        cached = sample(sources, callbacks, true);
        check(plain.queries == uint64_t(callbacks) * sources.size() &&
              cached.queries == uint64_t(callbacks) * uniqueRegions && plain.checksum == cached.checksum,
              "benchmark keeps callback invalidation and copies identical bytes");
        plainMs.push_back(plain.ms); cachedMs.push_back(cached.ms);
        auto instrumented = sample(sources, callbacks, true, true);
        auto stats = mb::mem_readability_stats();
        check(instrumented.queries == cached.queries && instrumented.checksum == cached.checksum &&
              stats.queries == uniqueRegions && stats.checks == sources.size(),
              "optional profiling preserves benchmark reads and fresh-per-callback query counts");
        profileMs.push_back(instrumented.ms);
    }
    std::sort(plainMs.begin(), plainMs.end()); std::sort(cachedMs.begin(), cachedMs.end());
    std::sort(profileMs.begin(), profileMs.end());
    printf("BENCH %s: %u callbacks, %zu reads/callback, queries/callback %zu -> %llu; "
           "median native uncached %.3f ms, scoped %.3f ms, %.2fx; %.3f -> %.3f us/callback\n",
           name, callbacks, sources.size(), sources.size(), (unsigned long long)uniqueRegions,
           plainMs[2], cachedMs[2], plainMs[2] / std::max(cachedMs[2], 0.000001),
           plainMs[2] * 1000 / callbacks, cachedMs[2] * 1000 / callbacks);
    printf("PROFILE %s: median profiled %.3f us/callback versus default %.3f; "
           "diagnostic overhead %.3f us/callback (five samples).\n", name,
           profileMs[2] * 1000 / callbacks, cachedMs[2] * 1000 / callbacks,
           (profileMs[2] - cachedMs[2]) * 1000 / callbacks);
}

static void test_cache_capacity_and_benchmark(SIZE_T page) {
    using namespace mb;
    // Twenty-one candidate objects, five validations/copies each, in one owned
    // committed heap-like allocation. Every callback must query that region again.
    auto* heap = allocate(page * 32);
    memset(heap, 0x31, page * 32);
    std::vector<uint8_t*> sources;
    for (unsigned candidate = 0; candidate < 21; ++candidate)
        for (unsigned field = 0; field < 5; ++field) sources.push_back(heap + page * candidate + field * 64);
    benchmark("same heap region / 21 candidates x 5", sources, 1);
    VirtualFree(heap, 0, MEM_RELEASE);

    // Sixty-five genuinely distinct readable regions separated by NOACCESS pages.
    // Exercise full-cache lookup, deterministic bounded eviction and fresh misses.
    auto* split = allocate(page * 130);
    memset(split, 0x42, page * 130);
    for (unsigned i = 0; i < 65; ++i) protect(split + page * (i * 2 + 1), page, PAGE_NOACCESS);
    {
        MemReadabilityScope scope;
        uint64_t before = queryCalls;
        bool valid = true;
        for (unsigned i = 0; i < 65; ++i) valid &= mem_readable(split + page * i * 2, 8);
        check(valid && queryCalls - before == 65, "unknown regions all receive fresh queries at cache capacity");
        before = queryCalls;
        check(mem_readable(split, 8) && queryCalls - before == 1, "evicted region is re-queried instead of guessed");
        check(g_readability.count == 64, "cache remains bounded to 64 region entries");
    }
    sources.clear();
    for (unsigned pass = 0; pass < 5; ++pass)
        for (unsigned i = 0; i < 64; ++i) sources.push_back(split + page * i * 2);
    benchmark("64 regions / bounded linear lookup", sources, 64);
    VirtualFree(split, 0, MEM_RELEASE);
}

static void test_resident_path(SIZE_T page) {
    using namespace mb;
    forceResidentFallback = false;
    auto* p = allocate(page * 4);
    memset(p, 0x59, page * 4);
    protect(p + page, page, PAGE_READONLY);
    protect(p + page * 2, page, PAGE_NOACCESS);
    uint8_t out[32] = {};
    {
        MemReadabilityScope scope(true);
        uint64_t vq = queryCalls, ws = residentCalls;
        check(mem_read(p + page - 8, out, 16) && out[0] == 0x59 && out[15] == 0x59 &&
              queryCalls == vq && residentCalls - ws == 2,
              "resident RW/RO crossing validates both pages without VirtualQuery");
        check(mem_readable(p + 64, 8) && residentCalls - ws == 2 &&
              mem_readability_stats().residentQueries == 2 && mem_readability_stats().queries == 0,
              "resident page observations are reused and separately profiled");
        auto stats = mem_readability_stats();
        check(stats.residentQueryTicks > 0 && stats.residentQueryMaxTicks <= stats.residentQueryTicks &&
              stats.validationTicks >= stats.residentQueryTicks && stats.residentFallbacks == 0,
              "resident query timing is included in total validation timing");
        check(!mem_readable(p + page * 2 - 8, 16) && queryCalls - vq == 1 &&
              mem_readability_stats().residentFallbacks == 1,
              "adjacent NOACCESS page cannot inherit a resident neighbor's permission");
        protect(p + page * 2, page, PAGE_READWRITE);
        check(!mem_readable(p + page * 2, 8), "resident backend preserves cached denials until invalidation");
        mem_readability_invalidate();
        check(mem_read(p + page * 2, out, 8), "resident backend refreshes a denial after explicit invalidation");
    }
    protect(p + page, page, PAGE_READWRITE | PAGE_GUARD);
    check(!mem_read(p + page - 8, out, 16), "resident-to-guard crossing is rejected before any copy");
    MEMORY_BASIC_INFORMATION info = {};
    require(VirtualQuery(p + page, &info, sizeof(info)) != 0, "resident guard comparison");
    check((info.Protect & PAGE_GUARD) != 0, "resident query fallback does not consume a guard alarm");
    protect(p + page, page, PAGE_READONLY);
    protect(p + page * 2, page, PAGE_EXECUTE);
    check(!mem_readable(p + page * 2, 8), "resident execute-only permission remains unreadable");
    protect(p + page * 2, page, PAGE_READWRITE);
    {
        MemReadabilityScope scope(true);
        require(mem_read(p, out, 8), "prime resident copy race");
        protect(p, page, PAGE_NOACCESS);
        check(!mem_read(p, out, 8) && mem_readability_stats().copyFaults == 1,
              "SEH contains reprotection after a cached resident observation");
        check(!mem_readable(p, 8) && mem_readability_stats().queries == 1 &&
              mem_readability_stats().invalidations == 1,
              "resident copy fault discards all cached permissions before retry");
    }
    protect(p, page, PAGE_READWRITE);
    p[0] = 0x59;
    faultAfterResidentQuery = p;
    check(!mem_read(p, out, 8), "SEH closes the uncached resident-query-to-copy race");
    protect(p, page, PAGE_READWRITE);
    p[0] = 0x59;
    {
        MemReadabilityScope scope;
        require(mem_readable(p, 8), "prime resident callback scope");
        { MemReadabilityScope nested; require(mem_readable(p, 8), "nested resident scope"); }
        ThreadFixture fixture = {p, 0, false, true, 0};
        HANDLE thread = CreateThread(nullptr, 0, thread_read, &fixture, 0, nullptr);
        require(thread != nullptr, "resident reader thread");
        require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "resident thread completion");
        CloseHandle(thread);
        check(fixture.pass && fixture.residentQueries == 1 && fixture.queries == 0,
              "another thread performs its own resident query despite the caller's cache");
    }
    uint64_t ws = residentCalls;
    check(mem_readable(p, 8) && mem_readable(p, 8) && residentCalls - ws == 2,
          "resident permissions do not persist outside the callback");
    {
        MemReadabilityScope scope;
        require(mem_readable(p, 8), "prime default resident profile");
        auto stats = mem_readability_stats();
        check(stats.residentQueries == 0 && stats.residentQueryTicks == 0,
              "resident instrumentation is off by default");
    }
    protect(p, page, PAGE_NOACCESS);
    check(!mem_readable(p, 8), "callback exit prevents reuse after resident page reprotection");
    protect(p, page, PAGE_READWRITE);
    p[0] = 0x59;
    uint64_t vq = queryCalls;
    nextResidentReply = ResidentReply::Failure;
    check(mem_readable(p, 8) && queryCalls - vq == 1, "failed working-set API falls back to a fresh VirtualQuery");
    nextResidentReply = ResidentReply::Failure;
    failNextQuery = true;
    check(!mem_readable(p, 8), "failure of both metadata APIs fails closed");
    nextResidentReply = ResidentReply::Bad;
    check(!mem_readable(p, 8), "a reported bad resident page is never readable");
    nextResidentReply = ResidentReply::Guard;
    check(!mem_readable(p, 8), "a valid response carrying PAGE_GUARD remains denied");
    vq = queryCalls;
    nextResidentReply = ResidentReply::LargePage;
    check(mem_readable(p, 8) && queryCalls - vq == 1, "large-page response uses the full-region fallback");
    require(VirtualLock(p, page) != FALSE, "lock owned fixture page");
    vq = queryCalls;
    check(mem_readable(p, 8) && queryCalls - vq == 1, "locked resident pages retain explicit committed-region validation");
    require(VirtualUnlock(p, page) != FALSE, "unlock owned fixture page");
    protect(p, page, PAGE_NOACCESS);
    nextResidentReply = ResidentReply::InvalidReadable;
    check(!mem_readable(p, 8), "Valid=0 forbids trusting fabricated readable protection bits");
    protect(p, page, PAGE_READWRITE);
    uint64_t beforeWs = residentCalls, beforeVq = queryCalls;
    check(!mem_readable((void*)(UINTPTR_MAX - 3), 8) && !mem_readable(nullptr, 8) &&
          !mem_readable((void*)0xffff, 8) && mem_readable(p, 0) &&
          residentCalls == beforeWs && queryCalls == beforeVq,
          "empty/low/overflowing ranges do not issue resident queries");
    protect(p + page * 3, page, PAGE_NOACCESS);
    check(!mem_read(p, p + page * 3, 8), "resident guarded copy rejects an invalid output buffer");
    VirtualFree(p, 0, MEM_RELEASE);

    auto* cold = allocate(page * 2);
    require(VirtualFree(cold + page, page, MEM_DECOMMIT) != FALSE, "cold reserved neighbor");
    PSAPI_WORKING_SET_EX_INFORMATION coldInfo = {}; coldInfo.VirtualAddress = cold;
    require(K32QueryWorkingSetEx(GetCurrentProcess(), &coldInfo, sizeof(coldInfo)) != FALSE, "cold page observation");
    check(!coldInfo.VirtualAttributes.Valid, "untouched demand-zero committed page starts nonresident");
    vq = queryCalls;
    check(mem_readable(cold, 8) && queryCalls - vq == 1, "nonresident committed readable memory is accepted via VirtualQuery");
    check(!mem_readable(cold + page - 8, 16), "nonresident range crossing into an uncommitted page is denied");
    check(mem_read(cold, out, 8) && out[0] == 0, "guarded copy can fault in a valid nonresident page");
    {
        MemReadabilityScope scope;
        require(mem_readable(cold, 8), "prime resident decommit race");
        require(VirtualFree(cold, page, MEM_DECOMMIT) != FALSE, "resident decommit race");
        check(!mem_read(cold, out, 8) && !mem_readable(cold, 8),
              "resident decommit fault invalidates cached permission and denies retry");
    }
    VirtualFree(cold, 0, MEM_RELEASE);

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)page, nullptr);
    require(mapping != nullptr, "pagefile mapping fixture");
    auto* view = (uint8_t*)MapViewOfFile(mapping, FILE_MAP_COPY, 0, 0, page);
    require(view != nullptr, "copy-on-write view fixture");
    volatile uint8_t mappedValue = *view; (void)mappedValue;  // make the mapping resident
    check(mem_read(view, out, 8) && out[0] == 0, "resident copy-on-write mapped page is readable");
    view[0] = 0x27;  // make this page private without changing its virtual address
    check(mem_read(view, out, 8) && out[0] == 0x27, "resident page remains readable after copy-on-write");
    UnmapViewOfFile(view); CloseHandle(mapping);

    auto* many = allocate(page * 65); memset(many, 0x42, page * 65);
    {
        MemReadabilityScope scope(true);
        bool valid = true;
        for (unsigned i = 0; i < 65; ++i) valid &= mem_readable(many + page * i, 8);
        ws = residentCalls;
        check(valid && mem_readable(many, 8) && residentCalls - ws == 1 &&
              g_readability.count == 64 && mem_readability_stats().evictions == 2,
              "resident cache stays bounded and re-queries evicted pages");
    }
    ws = residentCalls; vq = queryCalls;
    check(mem_readable(many, page * 65) && residentCalls == ws && queryCalls - vq == 1,
          "bulk ranges use one full-region query instead of an unbounded page walk");
    VirtualFree(many, 0, MEM_RELEASE);
    // Repeat the actual /EHsc nested fault contract with resident metadata.
    auto* nested = allocate(page); nested[0] = 1;
    { MemReadabilityScope scope; check(!guarded_callback(nested, page), "resident callback contains a raw nested read fault"); }
    protect(nested, page, PAGE_READWRITE); nested[0] = 1;
    ws = residentCalls;
    check(mem_readable(nested, 8) && mem_readable(nested, 8) && residentCalls - ws == 2,
          "resident outer scope retires observations despite a skipped nested SEH destructor");
    VirtualFree(nested, 0, MEM_RELEASE);
}

static void benchmark_resident_path(SIZE_T page) {
    // Compare the actual old scoped VQ behavior and new scoped page behavior.
    // Failure injection skips ONLY the new API, not real VirtualQuery latency.
    for (SIZE_T bytes : {page * 32, SIZE_T(64 * 1024 * 1024)}) {
        auto* heap = allocate(bytes); memset(heap, 0x31, bytes);
        std::vector<uint8_t*> sources;
        for (unsigned candidate = 0; candidate < 21; ++candidate)
            for (unsigned field = 0; field < 5; ++field)
                sources.push_back(heap + page * candidate + field * 64);
        const unsigned callbacks = 100;
        std::vector<double> legacyMs, residentMs;
        bool equivalent = true;
        for (unsigned i = 0; i < 5; ++i) {
            forceResidentFallback = true;
            auto legacy = sample(sources, callbacks, true);
            forceResidentFallback = false;
            auto resident = sample(sources, callbacks, true);
            equivalent &= legacy.checksum == resident.checksum && legacy.queries == callbacks &&
                          resident.queries == 0 && resident.residentQueries == uint64_t(callbacks) * 21;
            legacyMs.push_back(legacy.ms); residentMs.push_back(resident.ms);
        }
        check(equivalent, "resident benchmark preserves bytes and refreshes every page on each callback");
        std::sort(legacyMs.begin(), legacyMs.end()); std::sort(residentMs.begin(), residentMs.end());
        printf("RESIDENT BENCH %zu KiB owned region, 21 pages x 5 reads, five samples of %u callbacks: "
               "scoped VQ %.3f us/callback -> resident %.3f us/callback, %.2fx; "
               "VQ queries/callback 1 -> 0, WS queries/callback 21.\n",
               bytes / 1024, callbacks, legacyMs[2] * 1000 / callbacks,
               residentMs[2] * 1000 / callbacks, legacyMs[2] / std::max(residentMs[2], 0.000001));
        VirtualFree(heap, 0, MEM_RELEASE);
    }
}

static void probe_working_set(SIZE_T page) {
    auto* p = allocate(page * 8);
    memset(p, 0x41, page * 6);
    const DWORD modes[] = {PAGE_READWRITE, PAGE_READONLY, PAGE_EXECUTE_READ,
                           PAGE_READWRITE | PAGE_GUARD, PAGE_NOACCESS, PAGE_EXECUTE};
    for (unsigned i = 0; i < 6; ++i) protect(p + page * i, page, modes[i]);
    require(VirtualFree(p + page * 7, page, MEM_DECOMMIT) != FALSE, "probe reserved page");
    for (unsigned i = 0; i < 8; ++i) {
        PSAPI_WORKING_SET_EX_INFORMATION ws = {};
        ws.VirtualAddress = p + page * i;
        MEMORY_BASIC_INFORMATION mbi = {};
        require(K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof(ws)) != FALSE, "working-set probe");
        require(VirtualQuery(ws.VirtualAddress, &mbi, sizeof(mbi)) != 0, "working-set comparison");
        printf("API page %u: VQ state=0x%lx protect=0x%lx; WS valid=%llu protect=0x%llx bad=%llu\n",
               i, mbi.State, mbi.Protect, (unsigned long long)ws.VirtualAttributes.Valid,
               (unsigned long long)ws.VirtualAttributes.Win32Protection,
               (unsigned long long)ws.VirtualAttributes.Bad);
    }
    VirtualFree(p, 0, MEM_RELEASE);
    const SIZE_T bytes = 64 * 1024 * 1024;
    auto* big = allocate(bytes);
    memset(big, 0x31, bytes);
    LARGE_INTEGER frequency, begin, end; QueryPerformanceFrequency(&frequency);
    MEMORY_BASIC_INFORMATION mbi = {};
    PSAPI_WORKING_SET_EX_INFORMATION ws = {}; ws.VirtualAddress = big + page * 17;
    const unsigned samples = 100;
    QueryPerformanceCounter(&begin);
    for (unsigned i = 0; i < samples; ++i) require(VirtualQuery(ws.VirtualAddress, &mbi, sizeof(mbi)) != 0, "VQ benchmark");
    QueryPerformanceCounter(&end);
    double vqUs = (end.QuadPart - begin.QuadPart) * 1e6 / frequency.QuadPart / samples;
    QueryPerformanceCounter(&begin);
    for (unsigned i = 0; i < samples; ++i) require(K32QueryWorkingSetEx(GetCurrentProcess(), &ws, sizeof(ws)) != FALSE, "WS benchmark");
    QueryPerformanceCounter(&end);
    double wsUs = (end.QuadPart - begin.QuadPart) * 1e6 / frequency.QuadPart / samples;
    printf("API BENCH 64 MiB owned resident region: %u queries, VQ %.3f us/query, WS %.3f us/query, %.2fx.\n",
           samples, vqUs, wsUs, vqUs / std::max(wsUs, 0.000001));
    VirtualFree(big, 0, MEM_RELEASE);
}

int main(int argc, char** argv) {
    SYSTEM_INFO system; GetSystemInfo(&system);
    printf("Native memory tests: owned allocations, page size %lu; no game-runtime benchmark.\n", system.dwPageSize);
    if (argc == 2 && strcmp(argv[1], "--probe-working-set") == 0) {
        probe_working_set(system.dwPageSize);
        return 0;
    }
    test_safety(system.dwPageSize);
    test_cache_capacity_and_benchmark(system.dwPageSize);
    test_callback_guard(system.dwPageSize);
    test_profiling(system.dwPageSize);
    printf("Legacy fallback contract: %d checks completed, %d failures.\n", checks, failures);
    test_resident_path(system.dwPageSize);
    benchmark_resident_path(system.dwPageSize);
    printf("Memory contract result: %s (%d checks, %d failures).\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
