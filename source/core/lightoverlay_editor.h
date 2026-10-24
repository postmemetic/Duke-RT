#pragma once

#include <cstdint>

#include "d_eventbase.h"
#include "zstring.h"

class DCoreActor;
struct sectortype;
struct walltype;

enum class ActorLightEditorTargetKind : uint8_t
{
	None,
	Actor,
	Surface,
};

struct ActorLightEditorTarget
{
	ActorLightEditorTargetKind kind = ActorLightEditorTargetKind::None;
	DCoreActor* actor = nullptr;
	sectortype* hitSector = nullptr;
	walltype* hitWall = nullptr;
	FString actorClassName;
	int actorIndex = -1;
	double hitX = 0.0;
	double hitY = 0.0;
	double hitZ = 0.0;

	void Clear()
	{
		kind = ActorLightEditorTargetKind::None;
		actor = nullptr;
		hitSector = nullptr;
		hitWall = nullptr;
		actorClassName = "";
		actorIndex = -1;
		hitX = 0.0;
		hitY = 0.0;
		hitZ = 0.0;
	}
};

struct ActorLightEditorState
{
	bool enabled = false;
	ActorLightEditorTarget currentTarget;
	FString lastNotifyActorClassName;
	uint64_t lastNotifyTimeMs = 0;
	uint64_t lastActorSeenTimeMs = 0;
	FString writableLightOvrPath;
	int writableLightOvrLumpNum = -1;
	bool printActionPressed = false;
	bool createRuleActionPressed = false;
	bool reloadActionPressed = false;
	bool placeMapLightActionPressed = false;
	bool setMapDirectionalActionPressed = false;
	bool moveMapLightCloserActionPressed = false;
	bool moveMapLightFartherActionPressed = false;
	bool createEmissiveOverrideActionPressed = false;
	bool sectorColorActionPressed = false;
	bool createSurfaceLightActionPressed = false;
	bool controlKeyDown = false;
	double mapLightPreviewDistance = 128.0;
	FString activeSurfaceLightMapName;
	FString activeSurfaceLightRuleName;
	uint32_t activeSurfaceLightRuleId = 0;
};

bool IsActorLightEditorEnabled();
void TickActorLightEditor();
void ResetActorLightEditorState();
const ActorLightEditorState& GetActorLightEditorState();
bool ActorLightEditorResponder(event_t* ev);
bool ActorLightEditorSampleTarget(ActorLightEditorTarget& outTarget);
bool ActorLightEditorResolveWritableSource(FString& outPath, int* outLumpNum = nullptr);
