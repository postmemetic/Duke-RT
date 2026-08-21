#pragma once

bool System_WantGuiCapture();	// During playing this tells us whether the game must be paused due to active GUI elememts.

#include <stdint.h>
#include "vectors.h"
#include "engineerrors.h"
#include "stats.h"
#include "serializer.h"
#include "inputstate.h"
#include "maptypes.h"
#include "runtime_map_mover.h"

class FSerializer;
struct FRenderViewpoint;
struct sectortype;
struct tspritetype;
struct MapRecord;

struct GameStats
{
	int kill, tkill;
	int secret, tsecret;
	int timesecnd;
	int frags;
};

struct FNewGameStartup
{
	MapRecord* Map;
	int Episode;
	int Level;
	int Skill;
};

struct FSavegameInfo
{
	const char *savesig;
	int minsavever;
	int currentsavever;
};

enum EMenuSounds : int;
struct MapRecord;
struct TilesetBuildInfo;

struct GeoEffect
{
	sectortype** geosectorwarp;
	sectortype** geosectorwarp2;
	sectortype** geosector;
	double* geox;
	double* geoy;
	double* geox2;
	double* geoy2;
	int geocnt;

};

struct RuntimeLinkDebugState
{
	bool available = false;
	bool specialWaterSector = false;
	int32_t playerSectorIndex = -1;
	int32_t playerSectorLotag = 0;
	int32_t playerSectorHitag = 0;
	int32_t effectiveSectorLotag = 0;
	int32_t actorSectorIndex = -1;
	int32_t actorSectorLotag = 0;
	int32_t actorSectorHitag = 0;
	int32_t onWarpingSector = 0;
	int32_t transporterHold = 0;
	int32_t rrGeoCount = 0;
};

enum class RuntimeNightVisionMode : uint32_t
{
	None = 0,
	Duke = 1,
};

struct RuntimeNightVisionState
{
	bool available = false;
	RuntimeNightVisionMode mode = RuntimeNightVisionMode::None;
	bool viewEligible = false;
	bool enabled = false;
	float strength01 = 0.0f;
	float remainingSeconds = 0.0f;
};

struct RuntimeTaggedSectorDebugInfo
{
	bool available = false;
	int32_t sectorIndex = -1;
	int32_t lotag = 0;
	int32_t hitag = 0;
	uint32_t effectorCount = 0;
	int32_t effectorLotags[4] = {};
	int32_t effectorHitags[4] = {};
};

struct GameInterface
{
	virtual const char* Name() { return "$"; }
	virtual ~GameInterface() {}
	virtual bool GenerateSavePic() { return false; }
	virtual void app_init() = 0;
	virtual void FinalizeSetup() {}
	virtual void LoadTextureInfo(TilesetBuildInfo& info) {}
	virtual void SetupSpecialTextures(TilesetBuildInfo&) = 0;
	virtual void loadPalette() = 0;
	virtual void FreeLevelData();
	virtual void PlayHudSound() {}
	virtual void MenuOpened() {}
	virtual void MenuClosed() {}
	virtual void MenuSound(EMenuSounds snd) {}
	virtual bool CanSave() { return true; }
	virtual bool StartGame(FNewGameStartup& gs) { return true; }
	virtual FSavegameInfo GetSaveSig() { return { "", 0, 0}; }
	virtual double SmallFontScale() { return 1; }
	virtual void SerializeGameState(FSerializer& arc) = 0;
	virtual void DrawPlayerSprite(const DVector2& origin, bool onteam) {}
	virtual void SetAmbience(bool on) {}
	virtual void ExitFromMenu() { throw CExitEvent(0); }
	virtual void UpdateSounds() = 0;
	virtual void ErrorCleanup() {}
	virtual void Startup() = 0;
	virtual void DrawBackground() = 0;
	virtual void Render() = 0;
	virtual void Ticker() = 0;
	virtual int GetPlayerChecksum(int pnum) { return 0x12345678 + pnum; }
	virtual const char *CheckCheatMode() { return nullptr; }
	virtual const char* GenericCheat(int player, int cheat) = 0;
	virtual void NextLevel(MapRecord* map, int skill) = 0;
	virtual void NewGame(MapRecord* map, int skill, bool special = false) = 0;
	virtual void LevelCompleted(MapRecord* map, int skill) = 0;
	virtual bool DrawAutomapPlayer(const DVector2& mxy, const DVector2& cpos, const DAngle cang, const DVector2& xydim, const double czoom, double const interpfrac) { return false; }
	virtual DAngle playerPitchMin() { return DAngle::fromDeg(57.375); }
	virtual DAngle playerPitchMax() { return DAngle::fromDeg(-57.375); }
	virtual void ToggleThirdPerson() = 0;
	virtual void SwitchCoopView() { Printf("Unsupported command\n"); }
	virtual void ToggleShowWeapon() { Printf("Unsupported command\n"); }
	virtual void processSprites(tspriteArray& tsprites, const DVector3& view, DAngle viewang, double interpfrac) = 0;
	virtual void UpdateCameras(double smoothratio) {}
	virtual void EnterPortal(DCoreActor* viewer, int type) {}
	virtual void LeavePortal(DCoreActor* viewer, int type) {}
	virtual void SetMirrorPlayerVisibilityCaptureOverride(bool enabled) {}
	virtual bool GetMirrorPlayerVisibilityCaptureOverride() const { return false; }
	virtual bool GetGeoEffect(GeoEffect* eff, sectortype* viewsector) { return false; }
	virtual bool GetRuntimeLinkDebugState(RuntimeLinkDebugState* state) { return false; }
	virtual bool GetNightVisionState(RuntimeNightVisionState* state) { return false; }
	virtual bool GetRuntimeLinkDebugTaggedSectorInfo(int sectorIndex, RuntimeTaggedSectorDebugInfo* info) { return false; }
	virtual RuntimeMapMoverAuthorityState GetRuntimeMapMoverAuthorityState() const { return {}; }
	virtual void CaptureRuntimeMapMovers(TArray<RuntimeMapMoverSnapshot>& out) const { out.Clear(); }
	virtual bool IsPortalClosureSector(int sectorIndex) const { return false; }
	virtual int Voxelize(int sprnum) { return -1; }
	virtual void AddExcludedEpisode(const FString& episode) {}
	virtual int GetCurrentSkill() { return -1; }
	virtual bool IsQAVInterpTypeValid(const FString& type) { return false; }
	virtual void AddQAVInterpProps(const int res_id, const FString& interptype, const bool loopable, const TMap<int, TArray<int>>&& ignoredata) { }
	virtual void RemoveQAVInterpProps(const int res_id) { }
	virtual bool WantEscape() { return false; }
	virtual void StartSoundEngine() = 0;
	virtual void doPlayerMovement();
};

extern GameInterface* gi;
