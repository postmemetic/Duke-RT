#pragma once

#include <cstdint>
#include <string>
#include <vector>

class DCoreActor;

struct ActorPresentationOwnerKey
{
	uint64_t ownerWorldEpoch = 0;
	int64_t ownerLifetimeGeneration = -1;

	bool IsValid() const { return ownerWorldEpoch != 0 && ownerLifetimeGeneration >= 0; }
	bool operator==(const ActorPresentationOwnerKey& other) const
	{
		return ownerWorldEpoch == other.ownerWorldEpoch &&
			ownerLifetimeGeneration == other.ownerLifetimeGeneration;
	}
};

struct ActorPresentationVector2
{
	double x = 0.0;
	double y = 0.0;
};

struct ActorPresentationVector3
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct ActorPresentationAngles
{
	double yawDegrees = 0.0;
	double pitchDegrees = 0.0;
	double rollDegrees = 0.0;
};

// Read-only game animation inputs, captured once with simulation authority.
// Script callbacks are deliberately not evaluated by renderer refreshes.
struct ActorSpriteAnimationState
{
	bool available = false;
	bool enabled = false;
	bool actionActive = false;
	bool rrZombieRotation = false;
	int32_t rotationType = 0;
	int32_t actionOffset = 0;
	int32_t currentFrame = 0;
	int32_t forcedRotation = 0;
	int32_t fullbrightKind = 0;
	// Certified built-in Duke presentation recipe. These inputs are deliberately
	// outside the placement/witness hash and never execute script callbacks.
	bool recipeCertified = false;
	bool recipeHandled = false;
	bool recipeHidden = false;
	bool forceSphereChild = false;
	bool heatVision = false;
	int32_t recipeShade = 0;
	int32_t recipePalette = 0;
	int32_t recipeFloorPalette = -1;
	int32_t recipeTextureId = -1;
	ActorPresentationVector2 sphereOwnerPosition;
	ActorPresentationVector2 viewPlayerPosition;
};

// A renderer-safe copy of the authoritative actor placement at the end of a
// completed simulation tick. It deliberately contains no actor pointer.
struct ActorPresentationState
{
	ActorPresentationOwnerKey owner;
	int32_t actorIndex = -1;
	uint64_t placementGeneration = 0;
	uint64_t placementStateHash = 0;
	int32_t statNumber = -1;
	int32_t physicalSectorIndex = -1;
	int32_t picnum = -1;
	int32_t textureId = -1;
	int32_t displayTextureId = -1;
	ActorPresentationVector3 position;
	ActorPresentationVector3 previousPosition;
	ActorPresentationAngles angles;
	ActorPresentationAngles previousAngles;
	ActorPresentationVector2 scale;
	ActorPresentationAngles modelRotation;
	ActorPresentationVector3 modelPositionOffset;
	uint32_t cstat = 0;
	uint16_t cstat2 = 0;
	uint8_t renderFlags = 0;
	float alpha = 0.0f;
	ActorSpriteAnimationState spriteAnimation;
	int32_t spriteShade = 0;
	int32_t spritePalette = 0;
	int32_t spriteXOffset = 0;
	int32_t spriteYOffset = 0;
	bool authorityCurrent = false;
	bool publicationEligible = false;
};

struct ActorPresentationSnapshot
{
	uint64_t snapshotGeneration = 0;
	uint64_t worldEpoch = 0;
	uint64_t simulationGeneration = 0;
	uint64_t lifecycleSerial = 0;
	uint64_t stateHash = 0;
	bool complete = false;
	bool fullReconcileRequired = false;
	std::vector<ActorPresentationState> actors;
};

// Publishes a new snapshot only after a simulation tick completes. Zero-tic
// presentations therefore keep observing the exact same immutable snapshot.
void PublishActorPresentationSnapshot(uint64_t simulationGeneration);
void ResetActorPresentationSnapshot(uint64_t worldEpoch);

// Returned references/pointers remain valid until the next completed
// simulation tick. Callers must persist only owner + placementGeneration.
const ActorPresentationSnapshot& GetActorPresentationSnapshot();
const ActorPresentationState* LookupActorPresentationByOwnerKey(const ActorPresentationOwnerKey& owner);
const ActorPresentationState* LookupActorPresentationByActor(const DCoreActor* actor);

bool RunActorPresentationSnapshotSelfTests(std::string* failureReason = nullptr);

// Optional renderer consumer demand. Disabled frames skip game recipe capture;
// toggling this does not alter placement authority or gameplay witness hashes.
void SetActorSpriteFallbackRecipesRequested(bool requested);
