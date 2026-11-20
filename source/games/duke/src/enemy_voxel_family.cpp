#include "ns.h"
#include "global.h"
#include "dukeactor.h"
#include "gamefuncs.h"
#include "enemy_voxel_family.h"
#include "g_mapinfo.h"
#include "actor_presentation_snapshot.h"
#include "buildtiles.h"
#include "tilesetbuilder.h"
#include "hw_voxels.h"
#include "v_video.h"
#include "gametexture.h"
#include "c_cvars.h"
#include "vm.h"
#include "md5.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <map>
#include <unordered_map>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)
EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)

BEGIN_DUKE_NS
namespace
{
bool stockConSources = false, stockGameSeen = false;
uint64_t mountGeneration = 0;
struct DirectionalStart
{
    int marker;
    const char* cls;
    const char* voxelFile;
    uint32_t markerCrc, frontCrc;
    bool useFront = false;
};
// The stay-put marker is rotation 1 of the first walk frame. Its mounted
// 3D front pose already handles that angle with the actor's transform.
DirectionalStart directionalStarts[] = {
    {2711, "DukeBoss2Stayput", "voxels/monsters/CYCLOID/2710_CYCLOID.kvx", 0x26e902b9u, 0x0ef47cd7u},
    {2761, "DukeBoss3Stayput", "voxels/monsters/OVERLORD/2760_OVERLORD.kvx", 0x5776bfb1u, 0x778bc212u},
    {2631, "DukeBoss1Stayput", "voxels/monsters/BATTLELORD/2630_BATTLELORD.kvx", 0xca790092u, 0xc1d01eb4u},
    {1921, "DukeCommanderStayput", "voxels/monsters/COMMANDER/1920_COMM.kvx", 0xe47375dcu, 0x21cb4655u},
    {2121, "DukeLizManStayput", "voxels/monsters/ENFORCER/2120_ENFORCER.kvx", 0xc7b799aau, 0x5b7ba499u},
    {2001, "DukePigCopStayput", "voxels/monsters/PIGCOP/2000_PIGCOP.kvx", 0xabf25e32u, 0xb4afddd7u},
    {1821, "DukeOctabrainStayput", "voxels/monsters/OCTA/1820_OCTA.kvx", 0xbcd65873u, 0xe3e8d9d1u},
};
const DirectionalStart* CanonicalStart(int marker)
{
    for (const auto& start : directionalStarts)
        if (start.marker == marker && start.useFront) return &start;
    return nullptr;
}
struct ActionShape { const char* name; int offset, frames, rotation, increment; };
const ActionShape SharkActions[] = {
    {"ASHARKCRUZING", 0, 8, 5, 1},
    {"ASHARKFLEE", 0, 8, 5, 1},
    {"ASHARKATACK", 0, 8, 5, 1},
    {"ASHARKSHRUNK", 0, 8, 5, 1},
    {"ASHARKGROW", 0, 8, 5, 1},
    {"ASHARKFROZEN", 0, 1, 5, 1},
};
const ActionShape TrooperActions[] = {
    {"ATROOPSTAND", 0, 1, 5, 1},
    {"ATROOPGROW", 0, 1, 5, 1},
    {"ATROOPSTAYSTAND", -2, 1, 5, 1},
    {"ATROOPWALKING", 0, 4, 5, 1},
    {"ATROOPWALKINGBACK", 15, 4, 5, -1},
    {"ATROOPRUNNING", 0, 4, 5, 1},
    {"ATROOPSHOOT", 35, 1, 5, 1},
    {"ATROOPJETPACK", 40, 1, 5, 1},
    {"ATROOPJETPACKILL", 40, 2, 5, 1},
    {"ATROOPFLINTCH", 50, 1, 1, 1},
    {"ATROOPFLINTCHFIRE", 50, 1, 1, 1},
    {"ATROOPDYING", 50, 5, 1, 1},
    {"ATROOPDEAD", 54, 0, 0, 0},
    {"ATROOPPLAYDEAD", 54, 0, 0, 0},
    {"ATROOPSUFFERDEAD", 58, 2, 1, -4},
    {"ATROOPSUFFERING", 59, 2, 1, 1},
    {"ATROOPDUCK", 64, 1, 5, 1},
    {"ATROOPDUCKSHOOT", 64, 2, 5, 1},
    {"ATROOPABOUTHIDE", 74, 1, 1, 1},
    {"ATROOPHIDE", 79, 1, 1, 1},
    {"ATROOPREAPPEAR", 74, 1, 1, 1},
    {"ATROOPFROZEN", 0, 1, 5, 0},
};
const ActionShape EnforcerActions[] = {
    {"ALIZSTAND", 0, 0, 0, 0},
    {"ALIZWALKING", 0, 4, 5, 1},
    {"ALIZRUNNING", 0, 4, 5, 1},
    {"ALIZTHINK", 20, 2, 5, 1},
    {"ALIZSCREAM", 30, 1, 5, 1},
    {"ALIZJUMP", 45, 3, 5, 1},
    {"ALIZFALL", 55, 1, 5, 0},
    {"ALIZSHOOTING", 70, 2, 5, 1},
    {"ALIZDYING", 60, 6, 1, 1},
    {"ALIZLYINGDEAD", 65, 1, 0, 0},
    {"ALIZFROZEN", 0, 1, 5, 0},
};
const ActionShape DroneActions[] = {
    {"DRONEFRAMES", 0, 1, 7, 1},
    {"DRONESCREAM", 0, 1, 7, 1},
};
const ActionShape OctabrainActions[] = {
    {"AOCTAWALK", 0, 3, 5, 1},
    {"AOCTASTAND", 0, 1, 5, 1},
    {"AOCTASCRATCH", 0, 4, 5, 1},
    {"AOCTAHIT", 30, 1, 1, 1},
    {"AOCTASHOOT", 20, 1, 5, 1},
    {"AOCTADYING", 30, 8, 1, 1},
    {"AOCTADEAD", 38, 1, 1, 1},
    {"AOCTAFROZEN", 0, 1, 5, 0},
};
const ActionShape PigCopActions[] = {
    {"APIGWALK", 0, 4, 5, 1},
    {"APIGRUN", 0, 4, 5, 1},
    {"APIGSHOOT", 30, 2, 5, 1},
    {"APIGCOCK", 25, 1, 5, 1},
    {"APIGSTAND", 30, 1, 5, 1},
    {"APIGDIVE", 40, 2, 5, 1},
    {"APIGDIVESHOOT", 45, 2, 5, 1},
    {"APIGDYING", 55, 5, 1, 1},
    {"APIGHIT", 55, 1, 1, 1},
    {"APIGDEAD", 60, 1, 1, 1},
    {"APIGFROZEN", 0, 1, 5, 0},
    {"APIGGROW", 0, 0, 0, 0},
};
const ActionShape CommanderActions[] = {
    {"ACOMMBREETH", 0, 3, 5, 1},
    {"ACOMMFROZEN", 0, 1, 5, 0},
    {"ACOMMSPIN", -5, 1, 5, 1},
    {"ACOMMGET", 0, 3, 5, 1},
    {"ACOMMSHOOT", 20, 1, 5, 1},
    {"ACOMMABOUTTOSHOOT", 20, 1, 5, 1},
    {"ACOMMDYING", 30, 8, 1, 1},
    {"ACOMMDEAD", 38, 1, 1, 1},
};
const ActionShape TurretActions[] = {
    {"ASATNSPIN", 0, 5, 1, 1},
    {"ASATSHOOTING", -10, 3, 5, 1},
    {"ASATWAIT", 0, 1, 5, 1},
};
const ActionShape EggActions[] = {
    {"EGGOPEN1", 1, 1, 1, 1},
    {"EGGOPEN2", 2, 1, 1, 1},
    {"EGGOPEN3", 2, 1, 1, 1},
    {"EGGWAIT", 0, 0, 0, 0},
    {"EGGFROZEN", 1, 0, 0, 0},
    {"EGGGROW", 1, 0, 0, 0},
    {"EGGSHRUNK", 1, 0, 0, 0},
};
const ActionShape BattlelordActions[] = {
    {"ABOSS1WALK", 0, 4, 5, 1},
    {"ABOSS1FROZEN", 30, 1, 5, 0},
    {"ABOSS1RUN", 0, 6, 5, 1},
    {"ABOSS1SHOOT", 30, 2, 5, 1},
    {"ABOSS1LOB", 40, 2, 5, 1},
    {"ABOSS1DYING", 50, 5, 1, 1},
    {"BOSS1FLINTCH", 50, 1, 1, 1},
    {"ABOSS1DEAD", 55, 0, 0, 0},
};
const ActionShape Boss2Actions[] = {
    {"ABOSS2WALK", 0, 4, 5, 1},
    {"ABOSS2FROZEN", 0, 1, 5, 0},
    {"ABOSS2RUN", 0, 4, 5, 1},
    {"ABOSS2SHOOT", 20, 2, 5, 1},
    {"ABOSS2LOB", 30, 2, 5, 1},
    {"ABOSS2DYING", 40, 8, 1, 1},
    {"BOSS2FLINTCH", 40, 1, 1, 1},
    {"ABOSS2DEAD", 48, 0, 0, 0},
    // Miniature bosses recover from shrinking via AITROOPSEEKENEMY.
    {"ATROOPWALKING", 0, 4, 5, 1},
};
const ActionShape Boss3Actions[] = {
    {"ABOSS3WALK", 0, 4, 5, 1},
    {"ABOSS3FROZEN", 0, 1, 5, 0},
    {"ABOSS3RUN", 0, 4, 5, 1},
    {"ABOSS3LOB", 20, 4, 5, 1},
    {"ABOSS3LOBBING", 30, 2, 5, 1},
    {"ABOSS3DYING", 40, 8, 1, 1},
    {"BOSS3FLINTCH", 40, 1, 1, 1},
    {"ABOSS3DEAD", 48, 0, 0, 0},
    {"ATROOPWALKING", 0, 4, 5, 1},
};
const ActionShape TankActions[] = {
    {"ATANKSPIN", 0, 1, 7, 1},
    {"ATANKSHOOTING", 7, 2, 7, 1},
    {"ATANKWAIT", 0, 1, 7, 1},
    {"ATANKDESTRUCT", 0, 1, 7, 1},
    {"ATANKDEAD", 0, 1, 7, 1},
};
const ActionShape CannonActions[] = {
    {"ACANNONWAIT", 0, 1, 7, 1},
    {"ACANNONSHOOTING", 0, 1, 7, 1},
};
struct FamilySpec { uint32_t id; const char* name; int baseTile; const ActionShape* actions; size_t count; unsigned spriteSetSize = 0; };
const FamilySpec Families[] = {
    {1, "Shark", 1550, SharkActions, sizeof(SharkActions) / sizeof(ActionShape)},
    {2, "Trooper", 1680, TrooperActions, sizeof(TrooperActions) / sizeof(ActionShape)},
    {3, "Enforcer", 2120, EnforcerActions, sizeof(EnforcerActions) / sizeof(ActionShape)},
    {4, "Drone", 1880, DroneActions, sizeof(DroneActions) / sizeof(ActionShape)},
    {5, "Octabrain", 1820, OctabrainActions, sizeof(OctabrainActions) / sizeof(ActionShape)},
    {6, "PigCop", 2000, PigCopActions, sizeof(PigCopActions) / sizeof(ActionShape)},
    {7, "Commander", 1920, CommanderActions, sizeof(CommanderActions) / sizeof(ActionShape)},
    {8, "Turret", 2360, TurretActions, sizeof(TurretActions) / sizeof(ActionShape)},
    {9, "Egg", 675, EggActions, sizeof(EggActions) / sizeof(ActionShape)},
    {10, "GreenSlime", 2370, nullptr, 0, 8},
    {11, "Battlelord", 2630, BattlelordActions, sizeof(BattlelordActions) / sizeof(ActionShape)},
    {12, "Boss2", 2710, Boss2Actions, sizeof(Boss2Actions) / sizeof(ActionShape)},
    {13, "Boss3", 2760, Boss3Actions, sizeof(Boss3Actions) / sizeof(ActionShape)},
    {14, "Tank", 1975, TankActions, sizeof(TankActions) / sizeof(ActionShape)},
    {15, "Cannon", 1810, CannonActions, sizeof(CannonActions) / sizeof(ActionShape)},
    {16, "Recon", 1960, nullptr, 0, 2},
};
struct ClassFamily { const char* name; uint32_t family; int startupTile = 0; };
const ClassFamily Classes[] = {
    {"DukeShark", 1},
    {"DukeLizTrooper", 2},
    {"DukeLizTrooperToilet", 2, 1741},
    {"DukeLizTrooperSitting", 2, 1742},
    {"DukeLizTrooperShoot", 2},
    {"DukeLizTrooperJetpack", 2},
    {"DukeLizTrooperDucking", 2},
    {"DukeLizTrooperRunning", 2},
    {"DukeLizTrooperStayput", 2},
    {"DukeLizMan", 3},
    {"DukeLizManSpitting", 3},
    {"DukeLizManFeeding", 3},
    {"DukeLizManJump", 3},
    {"DukeLizManStayput", 3, 2121},
    {"DukeDrone", 4},
    {"DukeOctabrain", 5},
    {"DukeOctabrainStayput", 5, 1821},
    {"DukePigCop", 6},
    {"DukePigCopStayput", 6, 2001},
    {"DukePigCopDive", 6},
    {"DukeCommander", 7},
    {"DukeCommanderStayput", 7, 1921},
    {"DukeRotateGun", 8},
    {"DukeEgg", 9},
    {"DukeGreenSlime", 10},
    {"DukeBoss1", 11},
    {"DukeBoss1Stayput", 11, 2631},
    {"DukeBoss2", 12},
    {"DukeBoss2Stayput", 12, 2711},
    {"DukeBoss3", 13},
    {"DukeBoss3Stayput", 13, 2761},
    {"DukeTank", 14},
    {"DukeCannon", 15},
    {"DukeRecon", 16},
};

bool Matches(const ActorAction& action, const ActionShape& shape)
{
    return !action.base.isValid() && action.offset == shape.offset &&
        action.numframes == shape.frames && action.rotationtype == shape.rotation &&
        action.increment == shape.increment;
}

std::shared_ptr<const ActorEnemyVoxelFamily> BuildFamily(const FamilySpec& spec, const PClassActor& cls, int startupTile)
{
    auto family = std::make_shared<ActorEnemyVoxelFamily>();
    family->id = spec.id; family->name = spec.name;
    family->reason = "action-table-mismatch";
    const int base = tileGetTextureID(spec.baseTile).GetIndex();
    if (base < 0) return family;
    std::map<int32_t, const char*> poses;
    // GreenSlime.Tick and Recon.Animate select world images directly from
    // their class sprite sets, independently of CON action frames.
    if (spec.spriteSetSize)
    {
        const auto& sprites = cls.ActorInfo()->SpriteSet;
        family->reason = "sprite-set-mismatch";
        if (sprites.Size() != spec.spriteSetSize) return family;
        for (auto texture : sprites)
        {
            const auto* art = TexMan.GetGameTexture(texture);
            if (!texture.isValid() || !art || !art->isValid()) return family;
            poses.try_emplace(texture.GetIndex(), "class-sprite-set");
        }
    }
    for (size_t index = 0; index < spec.count; ++index)
    {
        const auto& shape = spec.actions[index];
        const ActorAction* found = nullptr;
        for (const auto& action : actions)
            if (std::strcmp(action.name.GetChars(), shape.name) == 0)
            { if (found) return family; found = &action; }
        if (!found || !Matches(*found, shape)) return family;
        for (int frame = 0; frame < std::max(1, shape.frames); ++frame)
        {
            // STAYSTAND is the initial action on LIZTROOPSTAYPUT (1682),
            // before its first cactor LIZTROOP. Its -2 offset resolves1680.
            const int actionBase = spec.id == 2 && !std::strcmp(shape.name, "ATROOPSTAYSTAND") ?
                tileGetTextureID(1682).GetIndex() : base;
            int texture = actionBase + shape.offset + shape.rotation * shape.increment * frame;
            // Same empty-art backoff as ResolveSpriteAnimationFrame, bounded
            // for certification. Invisible states are not guessed from names.
            FGameTexture* art = TexMan.GameByIndex(texture);
            for (int back = 0; art && !art->isValid() && shape.rotation > 0 && back < 256; ++back)
            { texture -= shape.rotation; art = TexMan.GameByIndex(texture); }
            if (!art || !art->isValid()) { family->reason = "missing-original-art"; return family; }
            poses.try_emplace(texture, shape.name);
        }
    }
    int startupTexture = -1;
    if (startupTile)
    {
        // These CON actors start with no action on their own image. Their
        // cactor successor uses only the ordinary action set.
        const auto texture = tileGetTextureID(startupTile);
        const auto* defaults = GetDefaultByType(&cls);
        const auto* art = TexMan.GetGameTexture(texture);
        // initanimations reads the compiled CON actor header for these exact
        // classes. The ZScript startaction property can legitimately be unset.
        const auto address = gs.actorinfo[startupTile].scriptaddress;
        family->reason = "startup-pose-mismatch";
        if (!texture.isValid() || !defaults || defaults->spr.spritetexture() != texture ||
            cls.ActorInfo()->TypeNum != startupTile || !address || uint64_t(address) + 1 >= ScriptCode.Size() ||
            ScriptCode[address + 1] != 0) return family;
        if (!art || !art->isValid()) { family->reason = "missing-original-art"; return family; }
        startupTexture = texture.GetIndex();
        poses.try_emplace(startupTexture, "class-start-pose");
    }
    if (poses.empty() || poses.size() > 64) return family;
    uint64_t hash = UINT64_C(1469598103934665603) ^ spec.id;
    uint64_t successorHash = hash;
    for (const auto& [texture, source] : poses)
    {
        family->textureIds.push_back(texture);
        family->poseSources.push_back(source);
        hash = (hash ^ uint32_t(texture)) * UINT64_C(1099511628211);
        if (startupTile && texture != startupTexture)
            successorHash = (successorHash ^ uint32_t(texture)) * UINT64_C(1099511628211);
    }
    family->signature = hash;
    family->successorSignature = startupTile ? successorHash : 0;
    family->complete = true;
    family->reason = spec.spriteSetSize ? "stock-sprite-set" : "stock-action-set";
    return family;
}
}

void ConfigureEnemyVoxelDirectionalStarts(const TilesetBuildInfo& info, bool worldTour)
{
    ++mountGeneration;
    for (auto& start : directionalStarts)
    {
        const auto& marker = info.tile[start.marker];
        const auto& front = info.tile[start.marker - 1];
        // A DEF tile range can explicitly share one voxel slot across the
        // front and marker angles. A separately bound slot is its own pose.
        const bool sharesFront = marker.extinfo.tiletovox < 0 || marker.extinfo.tiletovox == front.extinfo.tiletovox;
        start.useFront = worldTour && sharesFront && !marker.imported && !front.imported &&
            voxUsesFile(front.extinfo.tiletovox, start.voxelFile) &&
            uint32_t(tileGetCRC32(marker.tileimage)) == start.markerCrc &&
            uint32_t(tileGetCRC32(front.tileimage)) == start.frontCrc;
    }
}

int ResolveEnemyVoxelDirectionalStart(const DDukeActor& actor)
{
    if (!stockConSources || !stockGameSeen || !(bool)nri_ptenemystablerepresentation || !r_voxels) return -1;
    const auto* start = CanonicalStart(actor.spr.picnum);
    if (!start || actor.GetClass() != PClass::FindClass(start->cls) || V_GetBackend() != 4) return -1;
    return tileGetTextureID(start->marker - 1).GetIndex();
}

void ResetEnemyVoxelFamilyContentCertificate()
{
    stockConSources = true;
    stockGameSeen = false;
}

void RecordEnemyVoxelFamilyConSource(const char* name, const char* bytes, unsigned length, bool root)
{
    // Certify the actual compiler inputs, including recursive includes and
    // appended CON. Unknown control flow cannot borrow stock coverage.
    // These are the shipped World Tour scripts. Supporting another script
    // revision requires auditing its complete family action transitions.
    struct Source { const char* name; const char* md5; };
    static const Source known[] = {
        {"game.con", "3d9032d10f8ac867bba5c4cef69a3c62"},
        {"defs.con", "a586e0d47b83bf591f38852dbac37c18"},
        {"user.con", "b8079724b493be6f304f8264285e1471"},
        {"episode5boss.con", "76f74645904d01c2e183e0ed3925f75f"},
        {"fireflytrooper.con", "257a6d78f2c085d549b4f02daa5a9d08"},
        {"flamethrower.con", "bf62179519bb018a89dc72916889a2df"},
    };
    MD5Context digest; uint8_t raw[16]; char hex[33];
    digest.Update(reinterpret_cast<const uint8_t*>(bytes), length); digest.Final(raw);
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) { hex[i * 2] = digits[raw[i] >> 4]; hex[i * 2 + 1] = digits[raw[i] & 15]; }
    hex[32] = 0;
    bool recognized = false;
    for (const auto& source : known)
        if (!stricmp(name, source.name) && !std::strcmp(hex, source.md5))
        { recognized = !root || !stricmp(name, "game.con"); break; }
    if (root && !stricmp(name, "game.con") && recognized) stockGameSeen = true;
    stockConSources = stockConSources && recognized;
}

namespace
{
const ClassFamily* FindStockFamily(const PClassActor* cls)
{
    if (!cls) return nullptr;
    for (const auto& entry : Classes)
        if (cls == PClass::FindClass(entry.name)) return &entry;
    return nullptr; // Unknown subclasses/scripts never inherit a certificate.
}

std::shared_ptr<const ActorEnemyVoxelFamily> ResolveStockFamily(const PClassActor* cls, const ClassFamily& entry)
{
    const unsigned animate = GetVirtualIndex(RUNTIME_CLASS(DDukeActor), "animate");
    const unsigned runState = GetVirtualIndex(RUNTIME_CLASS(DDukeActor), "RunState");
    // Recon.Animate is a supported two-frame SpriteSet recipe in animatesprites.cpp.
    // FindStockFamily requires the exact class. In particular, none of the
    // Redneck UFO subclasses inherit this certificate.
    static const auto* recon = PClass::FindClass("DukeRecon");
    if (animate == ~0u || runState == ~0u || animate >= cls->Virtuals.Size() || runState >= cls->Virtuals.Size() ||
        (cls != recon && cls->Virtuals[animate] != RUNTIME_CLASS(DDukeActor)->Virtuals[animate]) ||
        cls->Virtuals[runState] != RUNTIME_CLASS(DDukeActor)->Virtuals[runState]) return nullptr;
    // Parsed action storage remains fixed during a game. Rebuild certificates
    // after a script reload/reallocation, rather than retaining stale pointers.
    static const ActorAction* actionBase = nullptr;
    static unsigned actionCount = 0;
    static uint64_t cachedMount = 0;
    static std::unordered_map<const PClassActor*, std::shared_ptr<const ActorEnemyVoxelFamily>> cached;
    const auto* currentBase = actions.Size() ? &actions[0] : nullptr;
    if (actionBase != currentBase || actionCount != actions.Size() || cachedMount != mountGeneration)
    { cached.clear(); actionBase = currentBase; actionCount = actions.Size(); cachedMount = mountGeneration; }
    auto found = cached.find(cls);
    if (found == cached.end())
    {
        // An explicitly authored marker voxel is an extra pose until cactor.
        // The guarded stock mapping uses the family's existing first walk pose.
        const int extraStart = CanonicalStart(entry.startupTile) ? 0 : entry.startupTile;
        found = cached.emplace(cls, BuildFamily(Families[entry.family - 1], *cls, extraStart)).first;
    }
    return found->second;
}
}

void CaptureEnemyVoxelFamily(const DDukeActor& actor, ActorSpriteAnimationState& animation)
{
    if (!(bool)nri_ptenemystablerepresentation && !(bool)nri_ptenemyrepresentationtrace) return;
    animation.enemy = !isRR() && !actor.isPlayer() && badguy(&actor);
    if (!animation.enemy) return;
    if (!stockConSources || !stockGameSeen)
    {
        static const auto unsupported = [] { auto family = std::make_shared<ActorEnemyVoxelFamily>(); family->reason = "uncertified-con-content"; return family; }();
        animation.enemyVoxelFamily = unsupported;
        return;
    }
    const auto* cls = static_cast<PClassActor*>(actor.GetClass());
    const auto* entry = FindStockFamily(cls);
    if (!entry) return;
    animation.enemyVoxelFamily = ResolveStockFamily(cls, *entry);
    if (actor.curAction && actor.curAction->name != NAME_None)
    {
        bool recognized = false;
        const auto& spec = Families[entry->family - 1];
        for (size_t i = 0; i < spec.count; ++i)
            if (std::strcmp(actor.curAction->name.GetChars(), spec.actions[i].name) == 0 && Matches(*actor.curAction, spec.actions[i]))
            { recognized = true; break; }
        if (!recognized) animation.enemyVoxelFamily.reset();
    }
}

void GameInterface::CollectSpawnableEnemyVoxelFamilies(const DCoreActor& spawner,
    std::vector<std::shared_ptr<const ActorEnemyVoxelFamily>>& out) const
{
    if (isRR() || ud.monsters_off || !stockConSources || !stockGameSeen) return;
    const auto* egg = PClass::FindClass("DukeEgg");
    const auto* cls = spawner.GetClass();
    if (cls == DukeRespawnControllerClass && spawner.spr.statnum == STAT_FX)
    {
        // DukeRespawnController.Tick calls spawnsprite(hitag). A record with
        // a custom base image can change the target's action texture base.
        const auto* target = spawnMap.CheckKey(spawner.spr.hitag);
        if (!target || target->basetex.isValid()) return;
        const auto* spec = FindStockFamily(target->cls);
        if (!spec) return;
        auto family = ResolveStockFamily(target->cls, *spec);
        if (!family || !family->complete) return;
        out.push_back(family);
        cls = target->cls;
    }
    const PClassActor* child = nullptr;
    if (cls == egg || cls == PClass::FindClass("DukeTank"))
    {
        // EGGOPEN2 hatches GREENSLIME. ATANKDEAD may eject PIGCOP.
        const auto* record = spawnMap.CheckKey(cls == egg ? 2370 : 2000);
        if (!record || record->basetex.isValid() ||
            record->cls != PClass::FindClass(cls == egg ? "DukeGreenSlime" : "DukePigCop")) return;
        child = record->cls;
    }
    else if (cls == PClass::FindClass("DukeRecon"))
    {
        // A crash spawns the class in this Recon's initialized field. An
        // unspawned marker target will receive the stock Initialize value.
        child = static_cast<PClassActor*>(PClass::FindClass("DukePigCop"));
        if (cls == spawner.GetClass())
        {
            static const FName spawnType("spawntype");
            const auto* field = static_cast<PClassActor* const*>(const_cast<DCoreActor&>(spawner).ScriptVar(spawnType, nullptr));
            if (!field || *field != child) return;
        }
    }
    const auto* spec = FindStockFamily(child);
    if (!spec) return;
    auto family = ResolveStockFamily(child, *spec);
    if (family && family->complete) out.push_back(family);
}
END_DUKE_NS
