#include "perf_route.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "c_console.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "d_eventbase.h"
#include "d_net.h"
#include "gameinput.h"
#include "gamecontrol.h"
#include "gamefuncs.h"
#include "gamestate.h"
#include "i_time.h"
#include "keydef.h"
#include "mapinfo.h"
#include "m_random.h"
#include "menu.h"
#include "perf_audit.h"
#include "savegamehelp.h"
#include "serializer.h"

EXTERN_CVAR(Bool, cl_syncinput)
EXTERN_CVAR(Bool, cl_latelatchmouse)
EXTERN_CVAR(Int, perf_fixedsimulationframes)
EXTERN_CVAR(Int, perf_tileanimationtime_ms)
CVAR(Int, perf_route_smokeframes, 0, 0)
CVAR(Bool, perf_route_smokemotion, false, 0)
CVAR(Bool, perf_route_actorcountchange, false, 0)
CVAR(Bool, perf_route_freshscriptrng, false, 0)
CUSTOM_CVAR(String, perf_route_next, "", 0)
{
    const char* name = self;
    if (!name[0]) return;
    bool valid = !UnsafeExecutionContext && !ParsingKeyConf;
    size_t length = 0;
    for (const char* p = name; *p; ++p)
    {
        if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '_') valid = false;
        if (++length > 63) { valid = false; break; }
    }
    if (!valid) { self = ""; Printf("PERF route: completion alias rejected\n"); }
}
extern bool demoplayback;
extern int gametic, oldentertics, entertic;
extern DCorePlayer* PlayerArray[MAXPLAYERS];

namespace
{
constexpr uint32_t Version = 1, FrameTag = 0x4d415246, DoneTag = 0x454e4f44;
constexpr uint32_t MaxTicks = 128;
constexpr uint64_t HashBasis = 14695981039346656037ull;

// This deliberately local format specifies packed little-endian IEEE754 data.
// Never serialize InputPacket's native layout, pointers, padding or float casts.
#pragma pack(push, 1)
struct Witness
{
    int32_t clock, seed, actors, sector, health;
    double x, y, z, yaw, pitch;
};
struct Header
{
    char magic[8];
    uint32_t version, tickRate;
    int32_t initialGametic;
    char map[64], save[1024];
    Witness initial;
    uint64_t checksum;
};
struct Command
{
    uint64_t timeNS;
    double x, y, z, yaw, pitch, roll;
    uint32_t actions;
    uint8_t sync;
};
struct Frame
{
    uint64_t index, startNS, displayNS, frameTimeMS;
    uint32_t ticks;
    char marker[96];
    Witness state;
};
struct Footer { uint64_t frames, ticks, checksum; };
#pragma pack(pop)

enum class Mode { Idle, PreparePending, RecordPending, PlayPending, Record, Play };
Mode mode = Mode::Idle;
FILE* stream = nullptr;
Header header{};
Frame frame{};
std::array<Command, MaxTicks> commands{};
uint32_t commandIndex = 0;
uint64_t frames = 0, ticks = 0, streamHash = HashBasis;
bool loaded = false, ready = false, frameOpen = false, stopRequested = false;
bool savedSync = false, savedLate = false, frameTimeSet = false;
std::string routePath, pendingMarker, pendingNextAlias;
std::string prepareMap;
bool prepareMapRequested = false;
bool actorCountException = false;
// The legacy fixtures omit named script RNG state. This opt-in baseline belongs
// to this process, survives Finish/stop, and is only suitable for replay fixtures
// with the same fresh-process startup. It is not a general savegame repair.
std::string freshScriptRngBaseline;
uint64_t freshScriptRngAppliedRoutes = 0;

uint64_t Hash(const void* data, size_t size, uint64_t hash = HashBasis)
{
    auto bytes = static_cast<const uint8_t*>(data);
    while (size--) { hash ^= *bytes++; hash *= 1099511628211ull; }
    return hash;
}
Witness State()
{
    Witness result{};
    result.clock = PlayClock;
    result.seed = randomseed;
    result.actors = Numsprites;
    result.sector = -1;
    auto player = PlayerArray[myconnectindex];
    auto actor = player ? player->GetActor() : nullptr;
    if (actor)
    {
        result.x = actor->spr.pos.X; result.y = actor->spr.pos.Y; result.z = actor->spr.pos.Z;
        result.yaw = actor->spr.Angles.Yaw.Degrees(); result.pitch = actor->spr.Angles.Pitch.Degrees();
        result.sector = actor->sector() ? sectindex(actor->sector()) : -1;
        result.health = actor->spr.extra;
    }
    return result;
}
uint64_t HashWithoutActors(Witness witness)
{
    witness.actors = 0;
    return Hash(&witness, sizeof(witness));
}
bool SameState(const Witness& a, const Witness& b, bool allowActorCountChange = false)
{
    // These are intentionally lightweight witnesses, not a complete world hash.
    return a.clock == b.clock && a.seed == b.seed && (allowActorCountChange || a.actors == b.actors) &&
        a.sector == b.sector && a.health == b.health &&
        std::abs(a.x-b.x) < 1e-8 && std::abs(a.y-b.y) < 1e-8 && std::abs(a.z-b.z) < 1e-8 &&
        std::abs(a.yaw-b.yaw) < 1e-8 && std::abs(a.pitch-b.pitch) < 1e-8;
}
bool Write(const void* data, size_t size) { return stream && fwrite(data, 1, size, stream) == size; }
bool Read(void* data, size_t size) { return stream && fread(data, 1, size, stream) == size; }

FILE* CreateRecording(const char* path)
{
    // Exclusive creation also protects a previous recording when commands are
    // invoked directly, outside the runner's fresh-output-directory guard.
#ifdef _WIN32
    const int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return nullptr;
    FILE* output = _fdopen(fd, "wb");
    if (!output) _close(fd);
#else
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) return nullptr;
    FILE* output = fdopen(fd, "wb");
    if (!output) close(fd);
#endif
    return output;
}

void Finish(const char* status, const char* reason)
{
    const bool wasPlayback = mode == Mode::Play;
    char audit[384];
    snprintf(audit, sizeof(audit), "\"status\":\"%s\",\"reason\":\"%s\",\"route_frames\":%llu,\"route_ticks\":%llu",
        status, reason, (unsigned long long)frames, (unsigned long long)ticks);
    PerfAuditWriteEvent("route_end", PerfAuditCurrentToken(), audit);
    if (!strcmp(status, "prepared") || !strcmp(status, "recorded") || !strcmp(status, "played"))
    {
        const char* next = perf_route_next;
        bool valid = next && *next && strlen(next) < 128;
        if (valid) for (const char* p = next; *p; ++p)
            if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '_') { valid = false; break; }
        if (valid) pendingNextAlias = next;
    }
    if (stream) fclose(stream);
    stream = nullptr;
    mode = Mode::Idle;
    frameOpen = ready = loaded = stopRequested = false;
    prepareMapRequested = false;
    I_SetSimulationTimeOverride(false, 0);
    cl_syncinput = savedSync;
    cl_latelatchmouse = savedLate;
    gameInput.Clear();
    if (wasPlayback)
    {
        Net_ClearBuffers();
        I_SetFrameTime();
        oldentertics = entertic = I_GetTime();
        Net_ClearFifo();
    }
    Printf("PERF route: status=%s reason=%s frames=%llu ticks=%llu path=\"%s\"\n",
        status, reason, (unsigned long long)frames, (unsigned long long)ticks, routePath.c_str());
}
void Invalid(const char* reason) { Finish("invalid", reason); }

bool SerializeNamedScriptRng(std::string& state)
{
    FSerializer archive;
    if (!archive.OpenWriter(false)) return false;
    FRandom::StaticWriteRNGState(archive);
    unsigned length = 0;
    const char* output = archive.GetOutput(&length);
    if (!output || !length) return false;
    state.assign(output, length);
    archive.Close();
    return true;
}

bool ApplyFreshScriptRngBaseline()
{
    const bool capture = freshScriptRngBaseline.empty();
    std::string before, after;
    bool clientRngReset = false;
    bool serialized = SerializeNamedScriptRng(before);
    if (serialized && capture)
    {
        freshScriptRngBaseline = before;
    }
    else if (serialized)
    {
        FSerializer archive;
        serialized = archive.OpenReader(freshScriptRngBaseline.data(), freshScriptRngBaseline.size());
        if (serialized)
        {
            // This existing API also resets client streams to the stored seed.
            // Only named gameplay streams are serialized and verified below.
            FRandom::StaticReadRNGState(archive);
            clientRngReset = true;
            archive.Close();
        }
    }
    serialized = serialized && SerializeNamedScriptRng(after);
    const bool verified = serialized && !freshScriptRngBaseline.empty() && after == freshScriptRngBaseline;
    const uint64_t appliedRoutes = freshScriptRngAppliedRoutes + 1;
    const auto beforeHash = Hash(before.data(), before.size());
    const auto afterHash = Hash(after.data(), after.size());
    const auto baselineHash = Hash(freshScriptRngBaseline.data(), freshScriptRngBaseline.size());
    char audit[768];
    snprintf(audit, sizeof(audit),
        "\"action\":\"%s\",\"boundary\":\"actual_pre_load\",\"before_hash\":\"%016llx\",\"after_hash\":\"%016llx\",\"baseline_hash\":\"%016llx\","
        "\"before_bytes\":%llu,\"after_bytes\":%llu,\"baseline_bytes\":%llu,\"verified\":%s,\"exact_match\":%s,\"applied_routes\":%llu,\"client_rng_reset\":%s",
        capture ? "capture" : "restore", (unsigned long long)beforeHash, (unsigned long long)afterHash,
        (unsigned long long)baselineHash, (unsigned long long)before.size(), (unsigned long long)after.size(),
        (unsigned long long)freshScriptRngBaseline.size(), verified ? "true" : "false", verified ? "true" : "false",
        (unsigned long long)appliedRoutes, clientRngReset ? "true" : "false");
    PerfAuditWriteEvent("route_script_rng", PerfAuditCurrentToken(), audit);
    Printf("PERF route script RNG: action=%s boundary=actual_pre_load before=%016llx after=%016llx baseline=%016llx bytes=%llu/%llu/%llu verified=%d applied_routes=%llu client_rng_reset=%d\n",
        capture ? "capture" : "restore", (unsigned long long)beforeHash, (unsigned long long)afterHash,
        (unsigned long long)baselineHash, (unsigned long long)before.size(), (unsigned long long)after.size(),
        (unsigned long long)freshScriptRngBaseline.size(), verified ? 1 : 0, (unsigned long long)appliedRoutes,
        clientRngReset ? 1 : 0);
    if (!verified) { Invalid("script-rng-baseline"); return false; }
    freshScriptRngAppliedRoutes = appliedRoutes;
    return true;
}

void CompleteRecording()
{
    const uint32_t tag = DoneTag;
    const Footer footer{frames, ticks, streamHash};
    if (!Write(&tag, sizeof(tag)) || !Write(&footer, sizeof(footer)) || fflush(stream) != 0)
        Invalid("write-footer");
    else Finish("recorded", "complete");
}
void Start(const char* path, const char* save, bool playback)
{
    if (mode != Mode::Idle) { Printf("PERF route: busy; stop the active route first\n"); return; }
    if (netgame || demoplayback || !isDuke() || ticdup != 1 || (int)perf_fixedsimulationframes > 0 ||
        (int)perf_tileanimationtime_ms >= 0)
    { Printf("PERF route: requires single-player Duke, ticdup=1 and live animation/simulation clocks\n"); return; }
    routePath = path;
    stream = playback ? fopen(path, "rb") : CreateRecording(path);
    if (!stream)
    {
        Printf("PERF route: status=invalid reason=open-failed-or-existing-recording path=\"%s\" (recording requires a new file)\n", path);
        return;
    }
    frames = ticks = 0; streamHash = HashBasis;
    actorCountException = playback && perf_route_actorcountchange;
    frameOpen = ready = loaded = stopRequested = false;
    pendingMarker.clear();
    savedSync = cl_syncinput; savedLate = cl_latelatchmouse;
    if (playback)
    {
        if (!Read(&header, sizeof(header)) || memcmp(header.magic, "P_ROUTE1", 8) != 0 ||
            header.version != Version || header.tickRate != uint32_t(GameTicRate) ||
            header.checksum != Hash(&header, sizeof(header)-sizeof(header.checksum)) ||
            !memchr(header.save, 0, sizeof(header.save)) || !memchr(header.map, 0, sizeof(header.map)))
        { Invalid("header"); return; }
        if (!save || !*save) save = header.save;
    }
    else
    {
        header = {};
        memcpy(header.magic, "P_ROUTE1", 8);
        header.version = Version; header.tickRate = GameTicRate;
        if (!save || !*save || strlen(save) >= sizeof(header.save)) { Invalid("save-path"); return; }
        strcpy(header.save, save);
    }
    FILE* saveFile = fopen(save, "rb");
    if (!saveFile) { Invalid("missing-save"); return; }
    fclose(saveFile);
    cl_syncinput = true; cl_latelatchmouse = false;
    M_ClearMenus(); C_HideConsole();
    mode = playback ? Mode::PlayPending : Mode::RecordPending;
    G_LoadGame(save, true);
    Printf("PERF route: status=armed mode=%s path=\"%s\" save=\"%s\"\n", playback ? "play" : "record", path, save);
}

void Prepare(const char* map, const char* save)
{
    if (mode != Mode::Idle) { Printf("PERF route: busy; stop the active route first\n"); return; }
    if (netgame || demoplayback || !isDuke() ||
        (strcmp(map, "e2l6") != 0 && strcmp(map, "e2l8") != 0 && strcmp(map, "E2L6") != 0 && strcmp(map, "E2L8") != 0))
    { Printf("PERF route: status=invalid reason=prepare-requires-single-player-E2L6-or-E2L8\n"); return; }
    FILE* reservation = CreateRecording(save);
    if (!reservation)
    { Printf("PERF route: status=invalid reason=prepare-save-open-failed-or-already-exists\n"); return; }
    fclose(reservation);
    // Reserve a fresh save path before requesting a map. WriteSavegame later
    // replaces only this owned placeholder; an existing user's save is refused.
    routePath = save;
    prepareMap = map;
    prepareMapRequested = false;
    ready = loaded = frameOpen = stopRequested = false;
    frames = ticks = 0;
    savedSync = cl_syncinput; savedLate = cl_latelatchmouse;
    cl_syncinput = true; cl_latelatchmouse = false;
    mode = Mode::PreparePending;
    Printf("PERF route: status=armed mode=prepare map=%s save=\"%s\"\n", map, save);
}
}

bool PerfRoutePlaying() { return mode == Mode::Play; }
bool PerfRouteActive() { return mode == Mode::Record || mode == Mode::Play; }
bool PerfRouteAwaitingBoundary() { return ready && (mode == Mode::PreparePending || mode == Mode::RecordPending || mode == Mode::PlayPending); }
bool PerfRouteBeforeLoad()
{
    // G_LoadGame only queues an action. Delayed console commands can request it
    // after GameTicker's action loop, leaving an old-world tick/render before
    // actual loading. Restore only after that world has finished unloading.
    return mode != Mode::PlayPending || !perf_route_freshscriptrng || ApplyFreshScriptRngBaseline();
}
void PerfRouteLoaded() { if (mode == Mode::RecordPending || mode == Mode::PlayPending) loaded = true; }
bool PerfRouteHoldAtBoundary()
{
    if (mode == Mode::PreparePending && prepareMapRequested && currentLevel &&
        stricmp(currentLevel->LabelName(), prepareMap.c_str()) == 0)
    { ready = true; return true; }
    if (loaded && (mode == Mode::RecordPending || mode == Mode::PlayPending)) { ready = true; return true; }
    return false;
}
void PerfRouteBeginPresentation()
{
    if (!pendingNextAlias.empty())
    {
        const std::string name = pendingNextAlias;
        pendingNextAlias.clear();
        auto command = FConsoleCommand::FindByName(name.c_str());
        if (command && command->IsAlias()) C_DoCommand(name.c_str());
        else Printf("PERF route: completion alias missing or not an alias: %s\n", name.c_str());
    }
    if (mode == Mode::PreparePending)
    {
        if (!prepareMapRequested && gameaction == ga_nothing &&
            (gamestate == GS_MENUSCREEN || gamestate == GS_LEVEL || gamestate == GS_FULLCONSOLE))
        {
            // Console waits may expire while startup PSOs or a loading screen
            // are still active. Request the map only after startup is ready.
            M_ClearMenus(); C_HideConsole();
            prepareMapRequested = true;
            const std::string command = "map " + prepareMap;
            C_DoCommand(command.c_str());
        }
        if (ready)
        {
            M_ClearMenus(); C_HideConsole();
            if (!PerfRoutePrepareDukeFixture()) { Invalid("prepare-fixture-setup"); return; }
            if (!WriteSavegame(routePath.c_str(), "Performance route fixture")) { Invalid("prepare-save-write"); return; }
            Finish("prepared", "complete");
        }
        return;
    }
    if (PerfRouteAwaitingBoundary())
    {
        const char* map = currentLevel ? currentLevel->LabelName() : "";
        if (strcmp(map, "E2L6") != 0 && strcmp(map, "E2L8") != 0 &&
            strcmp(map, "e2l6") != 0 && strcmp(map, "e2l8") != 0)
        { Invalid("unsupported-map-use-E2L6-or-E2L8"); return; }
        if (!PerfRouteApplyDukeFixture()) { Invalid("fixture-needs-god-and-all-weapons-save"); return; }
        M_ClearMenus(); C_HideConsole();
        if (mode == Mode::RecordPending)
        {
            header.initialGametic = gametic;
            snprintf(header.map, sizeof(header.map), "%s", map);
            header.initial = State();
            header.checksum = Hash(&header, sizeof(header)-sizeof(header.checksum));
            if (!Write(&header, sizeof(header))) { Invalid("write-header"); return; }
            mode = Mode::Record;
        }
        else
        {
            const auto actual = State();
            if (strcmp(map, header.map) || !SameState(header.initial, actual))
            {
                Printf("PERF route initial divergence: map=%s/%s clock=%d/%d seed=%u/%u actors=%d/%d sector=%d/%d health=%d/%d pos=%.9g,%.9g,%.9g expected=%.9g,%.9g,%.9g\n",
                    map, header.map, actual.clock, header.initial.clock, (unsigned)actual.seed, (unsigned)header.initial.seed,
                    actual.actors, header.initial.actors, actual.sector, header.initial.sector, actual.health, header.initial.health,
                    actual.x, actual.y, actual.z, header.initial.x, header.initial.y, header.initial.z);
                Invalid("initial-state"); return;
            }
            gametic = header.initialGametic;
            mode = Mode::Play;
        }
        Printf("PERF route: status=started mode=%s map=%s level_clock=%d random_seed=%u gametic=%d\n",
            PerfRoutePlaying() ? "play" : "record", map, PlayClock, (unsigned)randomseed, gametic);
        char audit[384];
        snprintf(audit, sizeof(audit), "\"mode\":\"%s\",\"route_frame\":0,\"route_tic\":0,\"level_clock\":%d,\"seed\":%u,\"initial_witness\":\"%016llx\",\"actor_count_exception\":%s",
            PerfRoutePlaying() ? "play" : "record", PlayClock, (unsigned)randomseed,
            (unsigned long long)Hash(&header.initial, sizeof(header.initial)), actorCountException ? "true" : "false");
        PerfAuditAlignSampling();
        PerfAuditWriteEvent("route_begin", PerfAuditCurrentToken(), audit);
    }
    if (!PerfRouteActive()) return;
    if (gamestate != GS_LEVEL || gameaction != ga_nothing || !cl_syncinput || cl_latelatchmouse)
    { Invalid("state-or-input-mode-change"); return; }
    frameOpen = true; commandIndex = 0; frameTimeSet = false;
    if (mode == Mode::Record)
    {
        frame = {};
        frame.index = frames; frame.startNS = I_GetTimeNS();
    }
    else
    {
        uint32_t tag = 0;
        if (!Read(&tag, sizeof(tag))) { Invalid("truncated-stream"); return; }
        if (tag == DoneTag)
        {
            Footer footer{};
            if (!Read(&footer, sizeof(footer)) || footer.frames != frames || footer.ticks != ticks ||
                footer.checksum != streamHash || fgetc(stream) != EOF) Invalid("footer");
            else Finish("played", "complete");
            return;
        }
        uint64_t checksum = 0;
        if (tag != FrameTag || !Read(&frame, sizeof(frame)) || frame.ticks > MaxTicks || frame.index != frames ||
            !memchr(frame.marker, 0, sizeof(frame.marker)) ||
            !Read(commands.data(), frame.ticks*sizeof(Command)) || !Read(&checksum, sizeof(checksum)) ||
            checksum != Hash(commands.data(), frame.ticks*sizeof(Command), Hash(&frame, sizeof(frame))))
        { Invalid("frame-or-checksum"); return; }
        streamHash = Hash(&checksum, sizeof(checksum), streamHash);
        I_SetSimulationTimeOverride(true, frame.startNS);
    }
}
int PerfRoutePlaybackTicks() { return PerfRoutePlaying() && frameOpen ? int(frame.ticks) : 0; }
void PerfRouteBeforePlaybackTick()
{
    if (PerfRoutePlaying() && commandIndex < frame.ticks)
        I_SetSimulationTimeOverride(true, commands[commandIndex].timeNS);
}
bool PerfRouteAllowSpecials(int player, int buffer)
{
    if (PerfRoutePlaying()) return false;
    if (mode == Mode::Record)
    {
        int size = 0; NetSpecs[player][buffer].GetData(&size);
        if (size) { Invalid("unsupported-special-command"); return false; }
    }
    return true;
}
void PerfRouteCommand(InputPacket& input, bool& synchronous)
{
    if (!PerfRouteActive() || !frameOpen) return;
    if (commandIndex >= MaxTicks || (PerfRoutePlaying() && commandIndex >= frame.ticks))
    { Invalid("command-overflow"); return; }
    if (mode == Mode::Record)
    {
        // Optional short smoke driver. User recordings leave it disabled.
        if ((int)perf_route_smokeframes > 0 && perf_route_smokemotion)
        {
            input = {};
            if (frames < uint64_t((int)perf_route_smokeframes / 2)) input.vel.X = 3.0;
            input.ang.Yaw = DAngle::fromDeg(0.25);
        }
        auto& c = commands[commandIndex]; c = {};
        c.timeNS = I_GetTimeNS(); c.x = input.vel.X; c.y = input.vel.Y; c.z = input.vel.Z;
        c.yaw = input.ang.Yaw.Degrees(); c.pitch = input.ang.Pitch.Degrees(); c.roll = input.ang.Roll.Degrees();
        c.actions = input.actions.GetValue(); c.sync = synchronous;
        ++frame.ticks;
    }
    else
    {
        const auto& c = commands[commandIndex];
        input.vel = {c.x, c.y, c.z};
        input.ang.Yaw = DAngle::fromDeg(c.yaw); input.ang.Pitch = DAngle::fromDeg(c.pitch); input.ang.Roll = DAngle::fromDeg(c.roll);
        input.actions = ESyncBits::FromInt(c.actions); synchronous = c.sync != 0;
    }
    ++commandIndex;
}
uint64_t PerfRouteFrameTime(uint64_t realFrameTime)
{
    if (!PerfRouteActive() || !frameOpen) return realFrameTime;
    if (mode == Mode::Record && !frameTimeSet) { frame.frameTimeMS = realFrameTime; frameTimeSet = true; }
    return frame.frameTimeMS;
}
void PerfRouteBeforeDisplay()
{
    if (!PerfRouteActive() || !frameOpen) return;
    if (mode == Mode::Record) frame.displayNS = I_GetTimeNS();
    else I_SetSimulationTimeOverride(true, frame.displayNS);
}
void PerfRouteEndPresentation()
{
    if (!PerfRouteActive() || !frameOpen) return;
    if (gamestate != GS_LEVEL || paused || gameaction != ga_nothing)
    { Invalid("pause-or-level-transition"); return; }
    if (mode == Mode::Record)
    {
        frame.state = State();
        if ((int)perf_route_smokeframes > 0 && pendingMarker.empty())
        {
            if (frames == 0) pendingMarker = "smoke-start";
            else if (frames == uint64_t((int)perf_route_smokeframes / 2)) pendingMarker = "smoke-midpoint";
        }
        if (!pendingMarker.empty()) { snprintf(frame.marker, sizeof(frame.marker), "%s", pendingMarker.c_str()); pendingMarker.clear(); }
        const uint32_t tag = FrameTag;
        const auto checksum = Hash(commands.data(), frame.ticks*sizeof(Command), Hash(&frame, sizeof(frame)));
        if (!Write(&tag, sizeof(tag)) || !Write(&frame, sizeof(frame)) ||
            !Write(commands.data(), frame.ticks*sizeof(Command)) || !Write(&checksum, sizeof(checksum)))
        { Invalid("write-frame"); return; }
        streamHash = Hash(&checksum, sizeof(checksum), streamHash);
    }
    else if (commandIndex != frame.ticks || !SameState(frame.state, State(), actorCountException))
    {
        const auto state = State();
        Printf("PERF route divergence: frame=%llu commands=%u/%u clock=%d/%d seed=%u/%u actors=%d/%d pos=%.9g,%.9g,%.9g expected=%.9g,%.9g,%.9g\n",
            (unsigned long long)frames, commandIndex, frame.ticks, state.clock, frame.state.clock,
            (unsigned)state.seed, (unsigned)frame.state.seed, state.actors, frame.state.actors,
            state.x, state.y, state.z, frame.state.x, frame.state.y, frame.state.z);
        Invalid("state-divergence"); return;
    }
    if (frame.marker[0])
    {
        const auto actual = State();
        PerfAuditMark(frame.marker);
        Printf("PERF route checkpoint: frame=%llu tic=%llu label=\"%s\" level_clock=%d seed=%u actors=%d\n",
            (unsigned long long)frames, (unsigned long long)(ticks+frame.ticks), frame.marker,
            actual.clock, (unsigned)actual.seed, actual.actors);
    }
    if (PerfAuditActive())
    {
        const auto actualState = State();
        char audit[768];
        snprintf(audit, sizeof(audit), "\"route_frame\":%llu,\"route_tic\":%llu,\"ticks\":%u,\"level_clock\":%d,\"seed\":%u,\"actors\":%d,\"witness\":\"%016llx\",\"expected_witness\":\"%016llx\",\"inputs\":\"%016llx\",\"expected_actors\":%d,\"actor_count_exception\":%s,\"witness_without_actors\":\"%016llx\",\"expected_witness_without_actors\":\"%016llx\"",
            (unsigned long long)frames, (unsigned long long)ticks, frame.ticks, actualState.clock,
            (unsigned)actualState.seed, actualState.actors, (unsigned long long)Hash(&actualState, sizeof(actualState)),
            (unsigned long long)Hash(&frame.state, sizeof(frame.state)),
            (unsigned long long)Hash(commands.data(), frame.ticks*sizeof(Command)), frame.state.actors,
            actorCountException ? "true" : "false", (unsigned long long)HashWithoutActors(actualState),
            (unsigned long long)HashWithoutActors(frame.state));
        PerfAuditWriteEvent("route_frame", PerfAuditCurrentToken(), audit);
    }
    ticks += frame.ticks; ++frames; frameOpen = false;
    if (mode == Mode::Record && (stopRequested || ((int)perf_route_smokeframes > 0 && frames >= uint64_t((int)perf_route_smokeframes)))) CompleteRecording();
}
bool PerfRouteFilterEvent(event_t* event)
{
    if (mode != Mode::Play && mode != Mode::PlayPending) return false;
    if (event->type == EV_KeyDown && event->data1 == KEY_ESCAPE) Finish("aborted", "escape");
    // Events are consumed before console/menu/game responders. Window-system
    // close requests remain outside this filter and still close the application.
    return true;
}

CCMD(perf_route_record)
{
    if (argv.argc() != 3) Printf("usage: perf_route_record <route-file> <full-save-path>\n");
    else Start(argv[1], argv[2], false);
}
CCMD(perf_route_prepare)
{
    if (argv.argc() != 3) Printf("usage: perf_route_prepare <e2l6|e2l8> <full-new-save-path>\n");
    else Prepare(argv[1], argv[2]);
}
CCMD(perf_route_play)
{
    if (argv.argc() < 2 || argv.argc() > 3) Printf("usage: perf_route_play <route-file> [full-save-path-override]\n");
    else Start(argv[1], argv.argc() == 3 ? argv[2] : nullptr, true);
}
CCMD(perf_route_mark)
{
    if (mode != Mode::Record || argv.argc() != 2 || strlen(argv[1]) >= sizeof(frame.marker))
        Printf("usage while recording: perf_route_mark <label-under-96-bytes>\n");
    else pendingMarker = argv[1];
}
CCMD(perf_route_stop)
{
    if (mode == Mode::Record) { if (frameOpen) stopRequested = true; else CompleteRecording(); }
    else if (mode != Mode::Idle) Finish("aborted", "command");
}
