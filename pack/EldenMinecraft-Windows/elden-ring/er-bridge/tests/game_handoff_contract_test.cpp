// Actual native tick/camera/mailbox regressions. No live game, OS focus or IPC.
// The two engine entry RVAs target jumps in this executable's own reserved image.
#define main previous_visibility_fixture_main
#include "game_visibility_frame_contract_test.cpp"
#undef main

static unsigned killed = 0;
static void __fastcall fixture_kill(void* ins, uint8_t) {
    ++killed;
    auto mods = *(uint8_t**)((uint8_t*)ins + mb::kChrModules);
    auto data = *(uint8_t**)(mods + mb::kModData);
    put(data, mb::kDataHp, 0);
}
static void engine_stub(uint8_t* image, size_t rva, uintptr_t target) {
    auto page = image + (rva & ~size_t(4095));
    if (!VirtualAlloc(page, 4096, MEM_COMMIT, PAGE_READWRITE)) std::abort();
    uint8_t code[12] = {0x48, 0xB8}; // mov rax, imm64; jmp rax
    memcpy(code + 2, &target, 8); code[10] = 0xFF; code[11] = 0xE0;
    memcpy(image + rva, code, sizeof(code));
    DWORD old = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) std::abort();
    FlushInstructionCache(GetCurrentProcess(), image + rva, sizeof(code));
}
static ErmcHandoff* mailbox() {
    return (ErmcHandoff*)(fixtureSharedMemory.data() + ERMC_OFF_HANDOFF);
}
static void commit_handoff(bool automatic) {
    auto* e = mailbox();
    uint32_t seq = e->seq | 1u; e->seq = seq;
    e->version = ERMC_HANDOFF_VERSION;
    e->mcPid = headerMailbox.mcPid; e->mcStartMs = headerMailbox.mcStartMs;
    e->hostStartMs = headerMailbox.hostStartMs; e->hostLife = headerMailbox.hostLife;
    e->focusReq = headerMailbox.hostFocusReq + 1u;
    e->flags = automatic ? ERMC_HANDOFF_AUTOMATIC : 0;
    e->mcDeaths = headerMailbox.mcDeaths; e->resumeReq = headerMailbox.mcSwitchReq;
    e->seq = seq + 1u;
}
static void fly(Character& local) {
    using namespace mb;
    fixtureAbsoluteFloor = true; fixtureFloorY = 80;
    tick(local, 1, drive);
    controlMailbox.seq += 2; controlMailbox.flags = drive;
    raw_stable_pos(local.player(), controlMailbox.hunterPos); controlMailbox.hunterPos[1] = 110;
    fixtureMs += 300; ++headerMailbox.mcHeartbeat; task_tick_body(nullptr);
    tick(local, 1, ERMC_CTRL_HOLD_HUNTER);
    check(g_haveRecoveryAnchor && g_standing, "setup owns a supported Y80 anchor and airborne Y110 body");
}
static void first_move() {
    using namespace mb;
    Character local;
    for (bool hit : {true, false}) {
        start_tick_fixture(local); fixtureAbsoluteFloor = true; fixtureFloorY = 80; fixtureFloorHit = hit;
        controlMailbox.seq += 2; controlMailbox.flags = drive;
        raw_stable_pos(local.player(), controlMailbox.hunterPos); controlMailbox.hunterPos[1] -= 0.05f;
        ++fixtureMs; ++headerMailbox.mcHeartbeat; task_tick_body(nullptr);
        check(close(*(float*)(local.physics.data() + kPhysPos + 4), hit ? 80 : 79.95f) &&
              g_movementCorrected == hit, hit ? "N01 first acquisition clamps BEFORE native physics write" :
              "N01 a genuine ray miss keeps the real descent");
        if (hit) check(stateMailbox.flags & ERMC_STATE_MOVEMENT_CORRECTED, "first-owner correction reaches actual state publication");
    }
    start_tick_fixture(local); fixturePhysicsWorld = false;
    controlMailbox.seq += 2; controlMailbox.flags = drive;
    raw_stable_pos(local.player(), controlMailbox.hunterPos); controlMailbox.hunterPos[1] -= 0.05f;
    ++fixtureMs; ++headerMailbox.mcHeartbeat; task_tick_body(nullptr);
    check(!local.pinned() && close(*(float*)(local.physics.data() + kPhysPos + 4),80), "first-owner descent cannot bypass a missing physics world");
    start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, 0);
    fixtureAbsoluteFloor = true; fixtureFloorY = 80;
    controlMailbox.seq += 2; controlMailbox.flags = drive;
    raw_stable_pos(local.player(), controlMailbox.hunterPos); controlMailbox.hunterPos[1] -= 0.05f;
    ++fixtureMs; ++headerMailbox.mcHeartbeat; task_tick_body(nullptr);
    check(g_movementCorrected && close(*(float*)(local.physics.data() + kPhysPos + 4),80), "reacquisition also uses the current native floor");
}
static void lifecycle_transactions() {
    using namespace mb;
    Character local;
    start_tick_fixture(local); fly(local); commit_handoff(true);
    killed = 0; ++headerMailbox.mcDeaths;
    tick(local, 750, ERMC_CTRL_AUTO_RECOVERY);
    check(killed == 0 && g_life == LIFE_ALIVE && g_controlRevoked && !local.pinned() &&
          close(*(float*)(local.physics.data() + kPhysPos + 4),80), "N02 terminal event beats queued death BEFORE update_life in actual tick");
    check(g_mcDeathsSeen == headerMailbox.mcDeaths, "revoked queued death is consumed and cannot fire later");
    headerMailbox.hostFocusReq = mailbox()->focusReq; // Java finishes the already committed transaction
    tick(local, 1, 0); ++headerMailbox.mcDeaths; tick(local, 1, 0);
    check(killed == 0, "late ordinary MC death cannot kill the returned ER body");
    fixtureF8 = true; tick(local, 1, ERMC_CTRL_AUTO_RECOVERY); fixtureF8 = false;
    check(!g_controlRevoked && headerMailbox.mcSwitchReq == 1, "retained terminal event cannot revoke explicit native F8");
    foregroundWindow = (HWND)2; tick(local, 1, drive);
    check(local.pinned(), "explicit retry can acquire the newly synchronized MC body");
    start_tick_fixture(local); fly(local); commit_handoff(true); tick(local,1,ERMC_CTRL_AUTO_RECOVERY);
    foregroundWindow = hostWindow; // user has focused ER before pressing its F8
    fixtureF8=true; tick(local,1,ERMC_CTRL_AUTO_RECOVERY); fixtureF8=false;
    check(headerMailbox.mcSwitchReq == 1, "late-counter setup uses a genuine foreground native F8");
    foregroundWindow = (HWND)2; // client completes the explicit retry
    headerMailbox.hostFocusReq = mailbox()->focusReq; tick(local,1,drive);
    check(!g_controlRevoked && !g_hostFocusChanged && local.pinned(),
          "late legacy focus completion of old terminal event cannot revoke an explicit newer native F8");
    for (uint32_t activeFlags : {drive, uint32_t(ERMC_CTRL_HOLD_HUNTER)}) {
        start_tick_fixture(local); tick(local, 1, drive); tick(local, 1, activeFlags);
        killed = 0; ++headerMailbox.mcDeaths; tick(local, 1, activeFlags);
        check(killed == 1 && g_life == LIFE_DEAD, "genuine active-MC death / deliberate kill during valid HOLD remains authoritative");
        tick(local, 1, 0); check(killed == 1, "one ordinary death counter causes at most one engine kill");
    }
    start_tick_fixture(local); fly(local); commit_handoff(false);
    killed = 0; ++headerMailbox.mcDeaths; tick(local, 1, 0);
    check(!local.pinned() && !killed && close(*(float*)(local.physics.data() + kPhysPos + 4),110), "manual airborne F8 releases in place without automatic relocation");
    for (int identity = 0; identity < 4; ++identity) {
        start_tick_fixture(local); fly(local); commit_handoff(true);
        if (identity == 0) ++mailbox()->mcStartMs;
        if (identity == 1) ++mailbox()->hostStartMs;
        if (identity == 2) ++mailbox()->hostLife;
        if (identity == 3) ++mailbox()->resumeReq;
        tick(local, 1, ERMC_CTRL_HOLD_HUNTER);
        check(!g_controlRevoked && local.pinned() && close(*(float*)(local.physics.data() + kPhysPos + 4),110), "wrong process/life/retry generation cannot relocate or revoke current owner");
    }
}
static void torn_transactions() {
    using namespace mb;
    Character local;
    start_tick_fixture(local); fly(local); commit_handoff(true);
    ++headerMailbox.hostFocusReq; snapshotAvailable = false;
    tick(local, 1, ERMC_CTRL_AUTO_RECOVERY);
    check(close(*(float*)(local.physics.data() + kPhysPos + 4),80) && !local.pinned(), "N03 independent event survives torn continuous-control snapshot at focus commit");
    snapshotAvailable = true;
    for (bool completes : {true, false}) {
        start_tick_fixture(local); fly(local); commit_handoff(true); mailbox()->seq = 3;
        killed = 0; ++headerMailbox.mcDeaths;
        tick(local, 1, ERMC_CTRL_OVERRIDE_CAMERA);
        check(local.pinned() && g_haveRecoveryAnchor && g_controlRevoked && !killed,
              "torn event parks only the validated owner and revokes camera/death authority");
        tick(local, 99, ERMC_CTRL_OVERRIDE_CAMERA);
        check(local.pinned() && !killed, "progressing control packets cannot renew torn-event retry bound");
        if (completes) mailbox()->seq = 4;
        tick(local, 1, ERMC_CTRL_OVERRIDE_CAMERA);
        check(!local.pinned() && !killed && close(*(float*)(local.physics.data() + kPhysPos + 4), completes ? 80 : 110),
              completes ? "complete automatic event recovers within the bounded retry" : "100ms expiry releases without guessing manual/automatic relocation");
    }
}
static void first_consumption_retry() {
    using namespace mb;
    Character local;
    for (bool automatic : {false,true}) {
        start_tick_fixture(local); fly(local); commit_handoff(automatic);
        headerMailbox.hostFocusReq=mailbox()->focusReq; foregroundWindow=hostWindow; fixtureF8=true;
        tick(local,1,ERMC_CTRL_OVERRIDE_CAMERA); fixtureF8=false;
        check(headerMailbox.mcSwitchReq == 1 && !g_controlRevoked && !g_hostFocusChanged &&
              close(*(float*)(local.physics.data()+kPhysPos+4),110),
              "first consumption of an older manual/automatic event cannot revoke or relocate a newer native F8");
        foregroundWindow=(HWND)2;
        ++fixtureMs; ++headerMailbox.mcHeartbeat; controlMailbox.seq+=2; controlMailbox.flags=drive;
        to_stable((const float*)(local.physics.data()+kPhysPos),controlMailbox.hunterPos);
        task_tick_body(nullptr);
        check(local.pinned() && !g_controlRevoked, "synchronized MC can reacquire after first-consumption F8");
    }
    for (int invalid=0;invalid<4;++invalid) {
        start_tick_fixture(local); fly(local); commit_handoff(true);
        headerMailbox.hostFocusReq=mailbox()->focusReq;
        if (invalid==0) mailbox()->seq |= 1;
        if (invalid==1) ++mailbox()->mcStartMs;
        if (invalid==2) mailbox()->flags |= 0x80000000u;
        if (invalid==3) mailbox()->resumeReq=2; // future, not an older committed generation
        foregroundWindow=hostWindow; fixtureF8=true; tick(local,1,ERMC_CTRL_OVERRIDE_CAMERA); fixtureF8=false;
        check(g_controlRevoked && close(*(float*)(local.physics.data()+kPhysPos+4),110),
              "torn/untrusted/future terminal event stays fail closed without guessed recovery");
    }
}
static void identity_visibility() {
    using namespace mb;
    Character local;
    for (bool replaced : {true,false}) {
        start_tick_fixture(local); tick(local,1,drive);
        if (replaced) { put(local.instance.data(),kChrHandle,uint64_t(999)); local.instance[kChrRenderFlags]=0x20; }
        tick(local,1,0); tick(local,250,0);
        check(local.rendered() == !replaced, replaced ? "N04 new handle never inherits old render restoration" : "same owner still restores its render bit after bounded grace");
    }
}
struct CameraTaskFixture { HANDLE ready, next; };
static DWORD WINAPI independent_camera_task(void* raw) {
    auto* signals = (CameraTaskFixture*)raw;
    for (int phase = 0; phase < 4; ++phase) {
        mb::task_camera_body(nullptr);
        SetEvent(signals->ready);
        if (phase != 3 && WaitForSingleObject(signals->next,10000) != WAIT_OBJECT_0) std::abort();
    }
    return 0;
}
static void camera_revocation() {
    using namespace mb;
    Character local;
    start_tick_fixture(local);
    std::array<uint8_t,0x30> cs = {}; std::array<uint8_t,0x60> cam = {};
    put(cs.data(),kCamPers1,cam.data()); put(cam.data(),0,g_base+rva::kVtPersCam);
    FixtureImageSpan global{g_base+rva::kCSCamera,std::vector<uint8_t>(8)};
    auto ptr=cs.data();memcpy(global.bytes.data(),&ptr,8);fixtureImage.push_back(global);
    controlMailbox.camPos[1]=20;controlMailbox.camTarget[1]=20;controlMailbox.camTarget[2]=1;controlMailbox.camUp[1]=1;
    tick(local,1,ERMC_CTRL_OVERRIDE_CAMERA);
    CameraTaskFixture signals{CreateEventW(nullptr,FALSE,FALSE,nullptr),CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    auto worker=CreateThread(nullptr,0,independent_camera_task,&signals,0,nullptr);
    if (!signals.ready || !signals.next || !worker || WaitForSingleObject(signals.ready,10000) != WAIT_OBJECT_0) std::abort();
    ++headerMailbox.mcHeartbeat; fixtureAppliedPoses=0;
    SetEvent(signals.next);
    if (WaitForSingleObject(signals.ready,10000) != WAIT_OBJECT_0) std::abort();
    check(fixtureAppliedPoses == 1, "dedicated camera thread primes its own heartbeat and last-good control cache");
    memset(cam.data()+kCamMatrix,0,64); // fixture native camera resolves a different pose before handoff
    ++headerMailbox.hostFocusReq; snapshotAvailable=false;
    tick(local,1,ERMC_CTRL_OVERRIDE_CAMERA);
    const auto before=cam; fixtureAppliedPoses=0;
    SetEvent(signals.next);
    if (WaitForSingleObject(signals.ready,10000) != WAIT_OBJECT_0) std::abort();
    check(cam == before && !fixtureAppliedPoses && g_controlRevoked,
          "N05 manual handoff revokes cached camera-only control in actual camera task");
    fixtureF8=true; tick(local,1,ERMC_CTRL_OVERRIDE_CAMERA); fixtureF8=false;
    SetEvent(signals.next);
    if (WaitForSingleObject(signals.ready,10000) != WAIT_OBJECT_0 || WaitForSingleObject(worker,10000) != WAIT_OBJECT_0) std::abort();
    check(cam != before && fixtureAppliedPoses == 1 && !g_controlRevoked, "only explicit native F8 retry restores camera override authority");
    CloseHandle(worker); CloseHandle(signals.ready); CloseHandle(signals.next);
    snapshotAvailable=true;
}

static ErmcExplicitReset* reset_mailbox() {
    return (ErmcExplicitReset*)(fixtureSharedMemory.data() + ERMC_OFF_EXPLICIT_RESET);
}
static void reset_request(uint64_t connection, uint64_t event, uint32_t reason, int life = -1) {
    auto* e = reset_mailbox();
    uint32_t odd = e->seq | 1u; e->seq = odd;
    e->version = ERMC_EXPLICIT_RESET_VERSION;
    e->mcStartMs = headerMailbox.mcStartMs; e->hostStartMs = headerMailbox.hostStartMs;
    e->connection = connection; e->event = event; e->reason = reason;
    e->hostLife = life >= 0 ? life : int(headerMailbox.hostLife);
    e->issuedAtMs = fixtureUnixMs; e->seq = odd + 1u;
}
static bool reset_ack(uint32_t result) {
    return reset_mailbox()->ack == ((uint64_t(result) << 32) | reset_mailbox()->seq);
}
static void reset_setup(Character& local, uint64_t connection = 900) {
    using namespace mb;
    start_tick_fixture(local); headerMailbox.hostStartMs = 12345; killed = 0;
    reset_request(connection, 0, ERMC_EXPLICIT_RESET_INACTIVE, 0);
    tick(local, 1, ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_COMPOSITE);
    check(reset_ack(ERMC_EXPLICIT_RESET_CONSUMED) && g_explicitReset.connection == connection && !killed,
          "explicit binding ACK precedes any snapshot; binding itself never kills");
}
static void explicit_resets() {
    using namespace mb;
    Character local;
    reset_setup(local);
    ++headerMailbox.mcDeaths;
    tick(local,1,ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_COMPOSITE);
    check(!killed && g_life == LIFE_ALIVE, "ordinary passive fire/fall death counter remains powerless");
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL);
    tick(local,1,ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_COMPOSITE);
    check(killed == 1 && g_life == LIFE_DEAD && reset_ack(ERMC_EXPLICIT_RESET_CONSUMED) &&
          !headerMailbox.hostDeaths, "explicit admin kill works in ER/passive mode without stand-in and without ordinary death echo");
    tick(local,1,0); check(killed == 1, "exact retained explicit stamp executes at most once");
    // Emulate core reload after native ACK: no ledger memory, but same stamped request.
    g_explicitReset = {}; put(local.data.data(),kDataHp,100); g_life=LIFE_ALIVE;
    tick(local,1,0); check(killed == 1, "persisted exact ACK prevents duplicate kill across core reload");

    for (int invalid = 0; invalid < 12; ++invalid) {
        reset_setup(local);
        reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL);
        auto* e = reset_mailbox();
        if (invalid == 0) ++e->mcStartMs;
        if (invalid == 1) ++e->hostStartMs;
        if (invalid == 2) ++e->hostLife;
        if (invalid == 3) e->issuedAtMs = fixtureUnixMs - 2001;
        if (invalid == 4) e->issuedAtMs = fixtureUnixMs + 1;
        if (invalid == 5) e->connection = 100; // a numerically lower ID still needs binding
        if (invalid == 6) e->event = 0;
        if (invalid == 7) e->reason = 2; // no hazard/other reason is accepted
        if (invalid == 8) e->version = 2;
        if (invalid == 9) { g_life = LIFE_SETTLING; }
        if (invalid == 10) put(local.data.data(),kDataHp,0);
        if (invalid == 11) e->event = 1ull << 52;
        tick(local,1,0);
        check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED),
              "wrong process/life/time/binding/nonce/reason/schema or nonliving body is terminally rejected");
    }
    reset_setup(local); reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL);
    tick(local,2501,0,false);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "inactive producer cannot issue explicit reset");
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "producer resume cannot re-date the terminally rejected nonce");
    reset_setup(local); reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL);
    reset_mailbox()->seq |= 1;
    tick(local,1,0); check(!killed, "torn explicit mailbox cannot kill or block ordinary native tick");
    ++reset_mailbox()->seq; tick(local,1,0);
    check(killed == 1 && reset_ack(ERMC_EXPLICIT_RESET_CONSUMED), "completed coherent explicit request is consumed");

    reset_setup(local);
    reset_request(100,0,ERMC_EXPLICIT_RESET_INACTIVE,0); tick(local,1,0);
    check(reset_ack(ERMC_EXPLICIT_RESET_CONSUMED) && g_explicitReset.connection == 100,
          "explicit binding accepts a lower replacement epoch and retires old identity");
    reset_request(900,0,ERMC_EXPLICIT_RESET_INACTIVE,0); tick(local,1,0);
    check(reset_ack(ERMC_EXPLICIT_RESET_REJECTED) && g_explicitReset.connection == 100, "retired unordered epoch cannot rebind");
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "retired connection event cannot kill");
    reset_request(100,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(killed == 1 && reset_ack(ERMC_EXPLICIT_RESET_CONSUMED), "new bound lower epoch retains explicit positive control");

    reset_setup(local);
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); reset_mailbox()->issuedAtMs -= 2001; tick(local,1,0);
    reset_request(900,0,ERMC_EXPLICIT_RESET_INACTIVE,0); tick(local,1,0);
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "same binding cannot renew expired nonce through rebind/new stamp");
    reset_request(0,0,ERMC_EXPLICIT_RESET_INACTIVE,0); tick(local,1,0);
    reset_request(900,2,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "disconnect clear invalidates all old connection requests");
    reset_request(101,0,ERMC_EXPLICIT_RESET_INACTIVE,0); tick(local,1,0);
    reset_request(101,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); reset_mailbox()->issuedAtMs -= 2000; tick(local,1,0);
    check(killed == 1 && reset_ack(ERMC_EXPLICIT_RESET_CONSUMED), "exact 2000ms freshness boundary accepts genuinely living current life");

    reset_setup(local); tick(local,1,drive);
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); ++headerMailbox.mcDeaths; tick(local,1,drive);
    check(killed == 1 && g_life == LIFE_DEAD && reset_ack(ERMC_EXPLICIT_RESET_CONSUMED) &&
          !headerMailbox.hostDeaths, "explicit active-MC kill wins before coincident ordinary counter without double kill/echo");

    reset_setup(local); reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL);
    ++headerMailbox.mcStartMs; tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "process restart with reused native life cannot consume old request");
    reset_request(900,2,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "new process cannot inherit another process's binding");

    reset_setup(local);
    reset_request(100,0,ERMC_EXPLICIT_RESET_INACTIVE,0); reset_mailbox()->issuedAtMs -= 2001; tick(local,1,0);
    check(reset_ack(ERMC_EXPLICIT_RESET_REJECTED) && g_explicitReset.connection == 900 &&
          !g_explicitReset.is_retired(100), "expired nonlethal binding does not replace/retire valid or candidate epoch");
    reset_request(100,0,ERMC_EXPLICIT_RESET_INACTIVE,0); tick(local,1,0);
    check(reset_ack(ERMC_EXPLICIT_RESET_CONSUMED) && g_explicitReset.connection == 100,
          "same candidate binding may retry with fresh stamp without numeric ordering");
    reset_request(100,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(killed == 1 && reset_ack(ERMC_EXPLICIT_RESET_CONSUMED), "retried accepted binding permits real explicit kill");
    reset_setup(local);
    reset_request(0,0,ERMC_EXPLICIT_RESET_INACTIVE,0); reset_mailbox()->issuedAtMs -= 2001;
    tick(local,2501,0,false);
    check(reset_ack(ERMC_EXPLICIT_RESET_CONSUMED) && !g_explicitReset.connection,
          "disconnect clear revokes even with expired time/inactive heartbeat; it grants no authority");
    reset_request(900,1,ERMC_EXPLICIT_RESET_GENERIC_KILL); tick(local,1,0);
    check(!killed && reset_ack(ERMC_EXPLICIT_RESET_REJECTED), "late old connection cannot kill after delayed disconnect clear");
}
int main() {
    auto image=(uint8_t*)VirtualAlloc(nullptr,0x5000000,MEM_RESERVE,PAGE_NOACCESS);
    if (!image) return 2;
    engine_stub(image,mb::rva::kCastRay,(uintptr_t)&fixture_cast);
    engine_stub(image,mb::rva::kKill,(uintptr_t)&fixture_kill);
    mb::g_base=fixtureImageBase=(uintptr_t)image;
    worldChrGlobal=mb::g_base+mb::rva::kWorldChrMan;havokGlobal=mb::g_base+mb::rva::kCSHavokMan;mb::g_ok=true;
    first_move();lifecycle_transactions();torn_transactions();first_consumption_retry();identity_visibility();camera_revocation();explicit_resets();
    std::printf("Native handoff regressions: %d checks, %d failures. Real tick/camera/mailbox; all engine/OS calls are fixtures.\n",checks,failures);
    VirtualFree(image,0,MEM_RELEASE);
    return failures ? 1 : 0;
}
