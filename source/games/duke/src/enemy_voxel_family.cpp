#include "ns.h"
#include "global.h"
#include "dukeactor.h"
#include "gamefuncs.h"
#include "enemy_voxel_family.h"
#include "actor_presentation_snapshot.h"
#include "buildtiles.h"
#include "gametexture.h"
#include "c_cvars.h"
#include "vm.h"
#include "md5.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)
EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)

BEGIN_DUKE_NS
namespace
{
bool stockConSources = false, stockGameSeen = false;
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
struct FamilySpec { uint32_t id; const char* name; int baseTile; const ActionShape* actions; size_t count; };
const FamilySpec Families[] = {
    {1, "Shark", 1550, SharkActions, sizeof(SharkActions) / sizeof(ActionShape)},
    {2, "Trooper", 1680, TrooperActions, sizeof(TrooperActions) / sizeof(ActionShape)},
    {3, "Enforcer", 2120, EnforcerActions, sizeof(EnforcerActions) / sizeof(ActionShape)},
    {4, "Drone", 1880, DroneActions, sizeof(DroneActions) / sizeof(ActionShape)},
    {5, "Octabrain", 1820, OctabrainActions, sizeof(OctabrainActions) / sizeof(ActionShape)},
    {6, "PigCop", 2000, PigCopActions, sizeof(PigCopActions) / sizeof(ActionShape)},
    {7, "Commander", 1920, CommanderActions, sizeof(CommanderActions) / sizeof(ActionShape)},
    {8, "Turret", 2360, TurretActions, sizeof(TurretActions) / sizeof(ActionShape)},
};
struct ClassFamily { const char* name; uint32_t family; };
const ClassFamily Classes[] = {
    {"DukeShark", 1},
    {"DukeLizTrooper", 2},
    {"DukeLizTrooperToilet", 2},
    {"DukeLizTrooperSitting", 2},
    {"DukeLizTrooperShoot", 2},
    {"DukeLizTrooperJetpack", 2},
    {"DukeLizTrooperDucking", 2},
    {"DukeLizTrooperRunning", 2},
    {"DukeLizTrooperStayput", 2},
    {"DukeLizMan", 3},
    {"DukeLizManSpitting", 3},
    {"DukeLizManFeeding", 3},
    {"DukeLizManJump", 3},
    {"DukeLizManStayput", 3},
    {"DukeDrone", 4},
    {"DukeOctabrain", 5},
    {"DukeOctabrainStayput", 5},
    {"DukePigCop", 6},
    {"DukePigCopStayput", 6},
    {"DukePigCopDive", 6},
    {"DukeCommander", 7},
    {"DukeCommanderStayput", 7},
    {"DukeRotateGun", 8},
};

bool Matches(const ActorAction& action, const ActionShape& shape)
{
    return !action.base.isValid() && action.offset == shape.offset &&
        action.numframes == shape.frames && action.rotationtype == shape.rotation &&
        action.increment == shape.increment;
}

std::shared_ptr<const ActorEnemyVoxelFamily> BuildFamily(const FamilySpec& spec)
{
    auto family = std::make_shared<ActorEnemyVoxelFamily>();
    family->id = spec.id; family->name = spec.name;
    family->reason = "action-table-mismatch";
    const int base = tileGetTextureID(spec.baseTile).GetIndex();
    if (base < 0) return family;
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
            family->textureIds.push_back(texture);
        }
    }
    std::sort(family->textureIds.begin(), family->textureIds.end());
    family->textureIds.erase(std::unique(family->textureIds.begin(), family->textureIds.end()), family->textureIds.end());
    if (family->textureIds.empty() || family->textureIds.size() > 64) return family;
    uint64_t hash = UINT64_C(1469598103934665603) ^ spec.id;
    for (int32_t texture : family->textureIds) hash = (hash ^ uint32_t(texture)) * UINT64_C(1099511628211);
    family->signature = hash;
    family->complete = true;
    family->reason = "stock-action-set";
    return family;
}
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
    const auto* cls = actor.GetClass();
    const FamilySpec* spec = nullptr;
    for (const auto& entry : Classes)
        if (cls == PClass::FindClass(entry.name)) { spec = &Families[entry.family - 1]; break; }
    if (!spec) return; // Unknown subclasses/scripts never inherit a certificate.
    const unsigned animate = GetVirtualIndex(RUNTIME_CLASS(DDukeActor), "animate");
    const unsigned runState = GetVirtualIndex(RUNTIME_CLASS(DDukeActor), "RunState");
    if (animate == ~0u || runState == ~0u || animate >= cls->Virtuals.Size() || runState >= cls->Virtuals.Size() ||
        cls->Virtuals[animate] != RUNTIME_CLASS(DDukeActor)->Virtuals[animate] ||
        cls->Virtuals[runState] != RUNTIME_CLASS(DDukeActor)->Virtuals[runState]) return;
    // Parsed action storage remains fixed during a game. Rebuild certificates
    // after a script reload/reallocation, rather than retaining stale pointers.
    static const ActorAction* actionBase = nullptr;
    static unsigned actionCount = 0;
    static std::unordered_map<uint32_t, std::shared_ptr<const ActorEnemyVoxelFamily>> cached;
    const auto* currentBase = actions.Size() ? &actions[0] : nullptr;
    if (actionBase != currentBase || actionCount != actions.Size())
    { cached.clear(); actionBase = currentBase; actionCount = actions.Size(); }
    auto found = cached.find(spec->id);
    if (found == cached.end()) found = cached.emplace(spec->id, BuildFamily(*spec)).first;
    animation.enemyVoxelFamily = found->second;
    if (actor.curAction && actor.curAction->name != NAME_None)
    {
        bool recognized = false;
        for (size_t i = 0; i < spec->count; ++i)
            if (std::strcmp(actor.curAction->name.GetChars(), spec->actions[i].name) == 0 && Matches(*actor.curAction, spec->actions[i]))
            { recognized = true; break; }
        if (!recognized) animation.enemyVoxelFamily.reset();
    }
}
END_DUKE_NS
