#include "nri_map_mover_rigid_route_policy.h"

#include <algorithm>
#include <cmath>

namespace
{
	using nri_scene::MapMoverLocalToWorldTransform;

	constexpr double TransformTolerance = 1.0e-6;
	constexpr uint32_t GeometryMemberFlags = RuntimeMapMoverMember_OwnsWalls |
		RuntimeMapMoverMember_OwnsFloor | RuntimeMapMoverMember_OwnsCeiling;
	constexpr uint32_t SupportedBoundaryMask = NRIMapMoverShadowQuarantine_AdjacencyUnproven;

	bool IsFiniteRigidTransform(const MapMoverLocalToWorldTransform& transform)
	{
		for (int row = 0; row < 3; ++row)
		{
			if (!std::isfinite(transform.translation[row])) return false;
			for (int column = 0; column < 3; ++column)
				if (!std::isfinite(transform.basis[row][column])) return false;
		}

		for (int row = 0; row < 3; ++row)
		{
			for (int other = row; other < 3; ++other)
			{
				double dot = 0.0;
				for (int component = 0; component < 3; ++component)
					dot += transform.basis[row][component] * transform.basis[other][component];
				const double expected = row == other ? 1.0 : 0.0;
				if (std::fabs(dot - expected) > TransformTolerance) return false;
			}
		}

		const double determinant =
			transform.basis[0][0] * (transform.basis[1][1] * transform.basis[2][2] -
				transform.basis[1][2] * transform.basis[2][1]) -
			transform.basis[0][1] * (transform.basis[1][0] * transform.basis[2][2] -
				transform.basis[1][2] * transform.basis[2][0]) +
			transform.basis[0][2] * (transform.basis[1][0] * transform.basis[2][1] -
				transform.basis[1][1] * transform.basis[2][0]);
		return std::fabs(determinant - 1.0) <= TransformTolerance;
	}

	bool BuildRelativeTransform(
		const MapMoverLocalToWorldTransform& capturedBase,
		const MapMoverLocalToWorldTransform& absolute,
		float (&outTransform)[12])
	{
		if (!IsFiniteRigidTransform(capturedBase) || !IsFiniteRigidTransform(absolute)) return false;

		double relativeBasis[3][3] = {};
		for (int row = 0; row < 3; ++row)
		{
			for (int column = 0; column < 3; ++column)
			{
				for (int component = 0; component < 3; ++component)
					relativeBasis[row][column] +=
						absolute.basis[row][component] * capturedBase.basis[column][component];
				const double expected = row == column ? 1.0 : 0.0;
				// The first routed family is translation-only. A relative rotation is
				// a contract failure even if both absolute transforms are rigid.
				if (std::fabs(relativeBasis[row][column] - expected) > TransformTolerance) return false;
			}
		}

		for (int row = 0; row < 3; ++row)
		{
			double translatedBase = 0.0;
			for (int column = 0; column < 3; ++column)
				translatedBase += relativeBasis[row][column] * capturedBase.translation[column];
			const double relativeTranslation = absolute.translation[row] - translatedBase;
			if (!std::isfinite(relativeTranslation)) return false;

			for (int column = 0; column < 3; ++column)
				outTransform[row * 4 + column] = (float)relativeBasis[row][column];
			outTransform[row * 4 + 3] = (float)relativeTranslation;
		}
		return true;
	}

	bool SameNonTransformGenerations(
		const NRIMapMoverShadowGenerations& expected,
		const NRIMapMoverShadowGenerations& actual)
	{
		return expected.topology == actual.topology &&
			expected.geometry == actual.geometry &&
			expected.material == actual.material &&
			expected.visibility == actual.visibility &&
			expected.light == actual.light;
	}

	NRIMapMoverShadowGenerations SnapshotGenerations(const RuntimeMapMoverSnapshot& snapshot)
	{
		return { snapshot.topologyGeneration, snapshot.geometryGeneration,
			snapshot.materialGeneration, snapshot.transformGeneration,
			snapshot.visibilityGeneration, snapshot.lightGeneration };
	}

	bool HasOneWholeGeometryMember(const RuntimeMapMoverSnapshot& snapshot)
	{
		if (snapshot.members.Size() != 1) return false;
		const RuntimeMapMoverMember& member = snapshot.members[0];
		return (member.flags & RuntimeMapMoverMember_ControlOnly) == 0 &&
			(member.flags & GeometryMemberFlags) == GeometryMemberFlags;
	}
}

NRIMapMoverRigidRouteAdmissionResult EvaluateNRIMapMoverRigidRouteAdmission(
	const NRIMapMoverRigidRouteAdmissionInput& input)
{
	NRIMapMoverRigidRouteAdmissionResult result;
	if (input.snapshot == nullptr || input.shadowRecord == nullptr || input.resource == nullptr)
	{
		result.rejectMask |= NRIMapMoverRigidRouteReject_MissingInput;
		return result;
	}

	const RuntimeMapMoverSnapshot& snapshot = *input.snapshot;
	const NRIMapMoverShadowRecord& record = *input.shadowRecord;
	const NRIMapMoverRigidRouteResource& resource = *input.resource;
	if (snapshot.stableGroupId == 0 ||
		record.key.stableGroupId != snapshot.stableGroupId ||
		resource.key.stableGroupId != snapshot.stableGroupId ||
		resource.key.chunkIndex != record.key.chunkIndex ||
		resource.mapEpoch != snapshot.mapEpoch ||
		resource.canonicalResourceKey == 0 ||
		resource.canonicalResourceKey != record.canonical.resourceKey)
	{
		result.rejectMask |= NRIMapMoverRigidRouteReject_IdentityMismatch;
	}

	if (snapshot.effectorLotag != 15 ||
		snapshot.capability != RuntimeMapMoverCapability::RigidTranslation)
	{
		result.rejectMask |= NRIMapMoverRigidRouteReject_UnsupportedFamily;
	}
	if (!HasOneWholeGeometryMember(snapshot))
		result.rejectMask |= NRIMapMoverRigidRouteReject_AmbiguousMembership;
	if (!input.wholeChunkOwnershipProven)
		result.rejectMask |= NRIMapMoverRigidRouteReject_PartialChunkOwnership;

	const NRIMapMoverShadowGenerations snapshotGenerations = SnapshotGenerations(snapshot);
	if (!SameNonTransformGenerations(resource.capturedGenerations, snapshotGenerations) ||
		!SameNonTransformGenerations(resource.capturedGenerations, record.generations))
	{
		result.rejectMask |= NRIMapMoverRigidRouteReject_GenerationChanged;
	}

	if (!record.canonical.valid)
		result.rejectMask |= NRIMapMoverRigidRouteReject_InvalidCanonical;
	if (record.consecutiveRigidCount == 0 ||
		record.lastComparison.classification != nri_scene::MapMoverGeometryClassification::RigidTranslation)
	{
		result.rejectMask |= NRIMapMoverRigidRouteReject_RigidEvidenceMissing;
	}

	if ((input.provenSafeBoundaryMask & ~SupportedBoundaryMask) != 0 ||
		(input.provenSafeBoundaryMask & ~record.quarantineMask) != 0)
		result.rejectMask |= NRIMapMoverRigidRouteReject_InvalidBoundaryProof;
	if ((record.quarantineMask & ~input.provenSafeBoundaryMask) != 0)
		result.rejectMask |= NRIMapMoverRigidRouteReject_Quarantined;
	if (input.hasOverlappingOwner)
		result.rejectMask |= NRIMapMoverRigidRouteReject_OverlappingOwner;
	if (input.hasPortalSurface)
		result.rejectMask |= NRIMapMoverRigidRouteReject_PortalSurface;
	if (input.hasEmissiveSurface)
		result.rejectMask |= NRIMapMoverRigidRouteReject_EmissiveSurface;
	if (record.canonicalReplacementCount != 0)
		result.rejectMask |= NRIMapMoverRigidRouteReject_CanonicalReplacement;
	if (!resource.atlasResident)
		result.rejectMask |= NRIMapMoverRigidRouteReject_AtlasNotResident;
	if (!resource.blasResident)
		result.rejectMask |= NRIMapMoverRigidRouteReject_BlasNotResident;
	if (!resource.registryResident)
		result.rejectMask |= NRIMapMoverRigidRouteReject_RegistryNotResident;
	if (!input.hasCurrentPresentationTransforms ||
		!BuildNRIMapMoverRigidRouteRelativeTransforms(
		resource.capturedBaseTransform,
		input.currentPreviousTransform,
		input.currentCurrentTransform,
		result.transforms))
	{
		result.rejectMask |= NRIMapMoverRigidRouteReject_InvalidTransform;
	}

	result.admitted = result.rejectMask == NRIMapMoverRigidRouteReject_None;
	return result;
}

bool BuildNRIMapMoverRigidRouteRelativeTransforms(
	const MapMoverLocalToWorldTransform& capturedBase,
	const MapMoverLocalToWorldTransform& previous,
	const MapMoverLocalToWorldTransform& current,
	NRIMapMoverRigidRouteTransforms& outTransforms)
{
	outTransforms = {};
	return BuildRelativeTransform(capturedBase, previous, outTransforms.previous) &&
		BuildRelativeTransform(capturedBase, current, outTransforms.current);
}

void PatchNRIMapMoverRigidRouteInstanceTransforms(
	const NRIMapMoverRigidRouteTransforms& transforms,
	float (&tlasTransform)[3][4],
	float (&sceneCurrentTransform)[12],
	float (&scenePreviousTransform)[12])
{
	for (int index = 0; index < 12; ++index)
	{
		tlasTransform[index / 4][index % 4] = transforms.current[index];
		sceneCurrentTransform[index] = transforms.current[index];
		scenePreviousTransform[index] = transforms.previous[index];
	}
}

const char* GetNRIMapMoverRigidRouteRejectName(uint32_t rejectBit)
{
	switch (rejectBit)
	{
	case NRIMapMoverRigidRouteReject_None: return "none";
	case NRIMapMoverRigidRouteReject_MissingInput: return "missing-input";
	case NRIMapMoverRigidRouteReject_IdentityMismatch: return "identity-mismatch";
	case NRIMapMoverRigidRouteReject_UnsupportedFamily: return "unsupported-family";
	case NRIMapMoverRigidRouteReject_AmbiguousMembership: return "ambiguous-membership";
	case NRIMapMoverRigidRouteReject_PartialChunkOwnership: return "partial-chunk-ownership";
	case NRIMapMoverRigidRouteReject_GenerationChanged: return "generation-changed";
	case NRIMapMoverRigidRouteReject_InvalidCanonical: return "invalid-canonical";
	case NRIMapMoverRigidRouteReject_RigidEvidenceMissing: return "rigid-evidence-missing";
	case NRIMapMoverRigidRouteReject_Quarantined: return "quarantined";
	case NRIMapMoverRigidRouteReject_InvalidBoundaryProof: return "invalid-boundary-proof";
	case NRIMapMoverRigidRouteReject_OverlappingOwner: return "overlapping-owner";
	case NRIMapMoverRigidRouteReject_PortalSurface: return "portal-surface";
	case NRIMapMoverRigidRouteReject_EmissiveSurface: return "emissive-surface";
	case NRIMapMoverRigidRouteReject_CanonicalReplacement: return "canonical-replacement";
	case NRIMapMoverRigidRouteReject_AtlasNotResident: return "atlas-not-resident";
	case NRIMapMoverRigidRouteReject_BlasNotResident: return "blas-not-resident";
	case NRIMapMoverRigidRouteReject_RegistryNotResident: return "registry-not-resident";
	case NRIMapMoverRigidRouteReject_InvalidTransform: return "invalid-transform";
	default: return "multiple-or-unknown";
	}
}

bool RunNRIMapMoverRigidRoutePolicySelfTests(std::string* failureReason)
{
	auto fail = [&](const char* reason)
	{
		if (failureReason != nullptr) *failureReason = reason;
		return false;
	};
	RuntimeMapMoverSnapshot snapshot;
	snapshot.stableGroupId = 7u;
	snapshot.mapEpoch = 3u;
	snapshot.effectorLotag = 15;
	snapshot.capability = RuntimeMapMoverCapability::RigidTranslation;
	RuntimeMapMoverMember member;
	member.flags = GeometryMemberFlags;
	snapshot.members.Push(member);

	NRIMapMoverShadowRecord record;
	record.key = { snapshot.stableGroupId, 11u };
	record.quarantineMask = NRIMapMoverShadowQuarantine_AdjacencyUnproven;
	record.canonical.valid = true;
	record.canonical.resourceKey = 99u;
	record.consecutiveRigidCount = 1u;
	record.lastComparison.classification = nri_scene::MapMoverGeometryClassification::RigidTranslation;

	NRIMapMoverRigidRouteResource resource;
	resource.key = record.key;
	resource.mapEpoch = snapshot.mapEpoch;
	resource.canonicalResourceKey = record.canonical.resourceKey;
	resource.atlasResident = true;
	resource.blasResident = true;
	resource.registryResident = true;
	resource.capturedBaseTransform = nri_scene::MakeIdentityMapMoverLocalToWorldTransform();

	NRIMapMoverRigidRouteAdmissionInput input;
	input.snapshot = &snapshot;
	input.shadowRecord = &record;
	input.resource = &resource;
	input.wholeChunkOwnershipProven = true;
	input.hasCurrentPresentationTransforms = true;
	input.currentPreviousTransform = resource.capturedBaseTransform;
	input.currentCurrentTransform = resource.capturedBaseTransform;
	input.currentCurrentTransform.translation[0] = 2.0;
	input.provenSafeBoundaryMask = NRIMapMoverShadowQuarantine_AdjacencyUnproven;
	const NRIMapMoverRigidRouteAdmissionResult admitted = EvaluateNRIMapMoverRigidRouteAdmission(input);
	if (!admitted.admitted || admitted.rejectMask != 0u) return fail("certified SE15 route rejected");
	if (std::fabs(admitted.transforms.current[3] - 2.0f) > 1.0e-6f) return fail("relative translation mismatch");

	input.hasCurrentPresentationTransforms = false;
	const NRIMapMoverRigidRouteAdmissionResult missingPresentation = EvaluateNRIMapMoverRigidRouteAdmission(input);
	if (missingPresentation.admitted ||
		(missingPresentation.rejectMask & NRIMapMoverRigidRouteReject_InvalidTransform) == 0u)
		return fail("missing presentation transforms accepted");
	input.hasCurrentPresentationTransforms = true;
	input.wholeChunkOwnershipProven = false;
	input.provenSafeBoundaryMask = 0u;
	const NRIMapMoverRigidRouteAdmissionResult partial = EvaluateNRIMapMoverRigidRouteAdmission(input);
	if (partial.admitted || (partial.rejectMask & NRIMapMoverRigidRouteReject_PartialChunkOwnership) == 0u)
		return fail("partial chunk ownership accepted");
	if (failureReason != nullptr) failureReason->clear();
	return true;
}
