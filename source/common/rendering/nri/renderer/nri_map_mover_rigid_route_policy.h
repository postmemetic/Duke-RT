#pragma once

#include "nri_map_mover_shadow_state.h"
#include "runtime_map_mover.h"

#include <cstdint>
#include <string>

enum NRIMapMoverRigidRouteRejectBits : uint32_t
{
	NRIMapMoverRigidRouteReject_None = 0,
	NRIMapMoverRigidRouteReject_MissingInput = 1u << 0,
	NRIMapMoverRigidRouteReject_IdentityMismatch = 1u << 1,
	NRIMapMoverRigidRouteReject_UnsupportedFamily = 1u << 2,
	NRIMapMoverRigidRouteReject_AmbiguousMembership = 1u << 3,
	NRIMapMoverRigidRouteReject_PartialChunkOwnership = 1u << 4,
	NRIMapMoverRigidRouteReject_GenerationChanged = 1u << 5,
	NRIMapMoverRigidRouteReject_InvalidCanonical = 1u << 6,
	NRIMapMoverRigidRouteReject_RigidEvidenceMissing = 1u << 7,
	NRIMapMoverRigidRouteReject_Quarantined = 1u << 8,
	NRIMapMoverRigidRouteReject_InvalidBoundaryProof = 1u << 9,
	NRIMapMoverRigidRouteReject_OverlappingOwner = 1u << 10,
	NRIMapMoverRigidRouteReject_PortalSurface = 1u << 11,
	NRIMapMoverRigidRouteReject_EmissiveSurface = 1u << 12,
	NRIMapMoverRigidRouteReject_CanonicalReplacement = 1u << 13,
	NRIMapMoverRigidRouteReject_AtlasNotResident = 1u << 14,
	NRIMapMoverRigidRouteReject_BlasNotResident = 1u << 15,
	NRIMapMoverRigidRouteReject_RegistryNotResident = 1u << 16,
	NRIMapMoverRigidRouteReject_InvalidTransform = 1u << 17,
};

struct NRIMapMoverRigidRouteResource
{
	NRIMapMoverShadowRecordKey key;
	uint64_t mapEpoch = 0;
	uint64_t canonicalResourceKey = 0;
	NRIMapMoverShadowGenerations capturedGenerations;
	nri_scene::MapMoverLocalToWorldTransform capturedBaseTransform;
	bool atlasResident = false;
	bool blasResident = false;
	bool registryResident = false;
};

struct NRIMapMoverRigidRouteAdmissionInput
{
	const RuntimeMapMoverSnapshot* snapshot = nullptr;
	const NRIMapMoverShadowRecord* shadowRecord = nullptr;
	const NRIMapMoverRigidRouteResource* resource = nullptr;
	bool wholeChunkOwnershipProven = false;
	bool hasOverlappingOwner = false;
	bool hasPortalSurface = false;
	bool hasEmissiveSurface = false;
	// Current-frame presentation transforms converted by the caller from game
	// authority. Shadow-record transforms are evidence and may lag on the exact
	// transform frame that this route is preflighting.
	bool hasCurrentPresentationTransforms = false;
	nri_scene::MapMoverLocalToWorldTransform currentPreviousTransform;
	nri_scene::MapMoverLocalToWorldTransform currentCurrentTransform;
	// Only quarantine facts that were independently proven safe may be named
	// here. The policy currently recognizes the adjacency-boundary fact only.
	uint32_t provenSafeBoundaryMask = 0;
};

struct NRIMapMoverRigidRouteTransforms
{
	float current[12] = {};
	float previous[12] = {};
};

struct NRIMapMoverRigidRouteAdmissionResult
{
	bool admitted = false;
	uint32_t rejectMask = NRIMapMoverRigidRouteReject_None;
	NRIMapMoverRigidRouteTransforms transforms;
};

NRIMapMoverRigidRouteAdmissionResult EvaluateNRIMapMoverRigidRouteAdmission(
	const NRIMapMoverRigidRouteAdmissionInput& input);

bool BuildNRIMapMoverRigidRouteRelativeTransforms(
	const nri_scene::MapMoverLocalToWorldTransform& capturedBase,
	const nri_scene::MapMoverLocalToWorldTransform& previous,
	const nri_scene::MapMoverLocalToWorldTransform& current,
	NRIMapMoverRigidRouteTransforms& outTransforms);

void PatchNRIMapMoverRigidRouteInstanceTransforms(
	const NRIMapMoverRigidRouteTransforms& transforms,
	float (&tlasTransform)[3][4],
	float (&sceneCurrentTransform)[12],
	float (&scenePreviousTransform)[12]);

template<class TlasInstanceType, class SceneInstanceType>
void PatchNRIMapMoverRigidRouteInstance(
	const NRIMapMoverRigidRouteTransforms& transforms,
	TlasInstanceType& tlasInstance,
	SceneInstanceType& sceneInstance)
{
	PatchNRIMapMoverRigidRouteInstanceTransforms(
		transforms,
		tlasInstance.transform,
		sceneInstance.currentTransform,
		sceneInstance.previousTransform);
}

const char* GetNRIMapMoverRigidRouteRejectName(uint32_t rejectBit);
bool RunNRIMapMoverRigidRoutePolicySelfTests(std::string* failureReason = nullptr);
