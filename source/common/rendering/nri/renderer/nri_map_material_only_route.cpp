#include "nri_map_material_only_route.h"
#include "nri_cpu_detail_audit.h"

#include "../scene/nri_texture_signature.h"

#include "texturemanager.h"

#include <cstring>
#include <optional>

namespace
{
	constexpr uint64_t AnimatedChunkOwnerDomain = 0xa11ca7ed00000000ull;
	constexpr int32_t DukeSE12LightSwitch = 12;

	bool ResolveTextureIdentity(const FGameTexture* texture, uint64_t& outStableIdentity, void*)
	{
		if (texture == nullptr)
		{
			outStableIdentity = 0;
			return true;
		}
		const FTextureID id = texture->GetID();
		if (!id.isValid())
		{
			return false;
		}
		outStableIdentity = (uint64_t)(uint32_t)id.GetIndex() + 1ull;
		return true;
	}

	template<class Container>
	bool UsesMutableCanvas(const Container& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			const auto isNonPersistentTexture = [](FGameTexture* texture)
			{
				return texture != nullptr &&
					!nri_scene::IsTexturePersistentSignatureEligible(texture);
			};
			if (isNonPersistentTexture(surface.material.texture) ||
				isNonPersistentTexture(surface.material.emissiveSourceTexture))
			{
				return true;
			}
		}
		return false;
	}

	bool SceneUsesMutableCanvas(const nri_scene::SceneView& view)
	{
		return UsesMutableCanvas(view.opaqueWalls) || UsesMutableCanvas(view.opaqueFlats);
	}

	bool SameTerminalMaterialCarrier(const nri_scene::MaterialRef& retained, const nri_scene::MaterialRef& current)
	{
		uint64_t retainedTexture = 0;
		uint64_t currentTexture = 0;
		uint64_t retainedEmissive = 0;
		uint64_t currentEmissive = 0;
		return ResolveTextureIdentity(retained.texture, retainedTexture, nullptr) &&
			ResolveTextureIdentity(current.texture, currentTexture, nullptr) &&
			ResolveTextureIdentity(retained.emissiveSourceTexture, retainedEmissive, nullptr) &&
			ResolveTextureIdentity(current.emissiveSourceTexture, currentEmissive, nullptr) &&
			retainedTexture == currentTexture &&
			retainedEmissive == currentEmissive &&
			retained.flags == current.flags &&
			std::memcmp(&retained.alpha, &current.alpha, sizeof(retained.alpha)) == 0;
	}

	template<class Container>
	bool OnlyTerminalPaletteShadeChanged(const Container& retained, const Container& current)
	{
		if (retained.size() != current.size())
		{
			return false;
		}
		for (size_t index = 0; index < retained.size(); ++index)
		{
			if (!SameTerminalMaterialCarrier(retained[index].material, current[index].material))
			{
				return false;
			}
		}
		return true;
	}

	bool ResolveTerminalSE12Authority(
		const NRIMapMoverSystem& movers,
		const nri_scene::PTMapChunk& chunk,
		uint64_t buildSerial,
		uint64_t mapEpoch,
		uint64_t& outStableId)
	{
		outStableId = UINT64_MAX;
		if (buildSerial == 0 || mapEpoch == 0 ||
			movers.GetBuildSerial() != buildSerial || movers.GetMapEpoch() != mapEpoch)
		{
			return false;
		}
		const std::vector<uint64_t>* groups = movers.FindGroupsForSector(chunk.sectorIndex);
		if (groups == nullptr || groups->size() != 1)
		{
			return false;
		}
		const RuntimeMapMoverSnapshot* group = movers.FindGroup((*groups)[0]);
		if (group == nullptr || group->mapEpoch != mapEpoch ||
			group->lifecycle != RuntimeMapMoverLifecycle::Terminal ||
			group->capability != RuntimeMapMoverCapability::MaterialOrLightOnly ||
			group->effectorLotag != DukeSE12LightSwitch ||
			group->ownerSectorIndex != chunk.sectorIndex ||
			group->members.Size() != 1)
		{
			return false;
		}
		const RuntimeMapMoverMember& member = group->members[0];
		if (member.sectorIndex != chunk.sectorIndex || member.canonicalWallOffset != -1 ||
			member.wallCount <= 0 || member.flags != RuntimeMapMoverMember_ControlOnly)
		{
			return false;
		}
		outStableId = group->stableGroupId;
		return outStableId != UINT64_MAX;
	}
}

void NRIMapMaterialOnlyRoute::BeginFrame(uint64_t frameIndex)
{
	if (mFrameIndex != frameIndex)
	{
		mFrameIndex = frameIndex;
		mFrameStats = {};
	}
}

void NRIMapMaterialOnlyRoute::NotePreflightReject(
	NRIMapMaterialOnlyRoutePreflightReject reason)
{
	mFrameStats.candidates++;
	const uint32_t bit = (uint32_t)reason;
	if (bit < 64)
	{
		mFrameStats.preflightRejectMask |= 1ull << bit;
	}
}

void NRIMapMaterialOnlyRoute::Reject(
	NRIMapMaterialOnlyRouteResult& result,
	NRIMapMaterialOnlyRouteReject reason)
{
	result.reject = reason;
	const uint32_t bit = (uint32_t)reason;
	if (bit < 64)
	{
		mFrameStats.rejectMask |= 1ull << bit;
	}
	const uint32_t validationBit = (uint32_t)result.validation.failure;
	if (validationBit > 0 && validationBit < 64)
	{
		mFrameStats.validationFailureMask |= 1ull << validationBit;
	}
}

NRIMapMaterialOnlyRouteResult NRIMapMaterialOnlyRoute::TryPrepare(
	const NRIMapMaterialOnlyRouteInput& input)
{
	const bool detail = PerfAuditCpuDetailActive();
	std::optional<NRICpuDetailAuditScope> authorityAudit(std::in_place, PerfAuditMetric::MutationCanonicalAuthorityNs);
	if (detail) PerfAuditAdd(PerfAuditMetric::MutationCanonicalObservedCalls);
	BeginFrame(input.frameIndex);
	mFrameStats.candidates++;
	NRIMapMaterialOnlyRouteResult result;
	if (input.movers == nullptr || input.retainedWorld == nullptr || input.retainedChunk == nullptr ||
		input.retainedSceneView == nullptr || input.currentWorld == nullptr || input.currentChunk == nullptr ||
		input.currentSceneView == nullptr || input.buildSerial == 0 || input.mapEpoch == 0 ||
		!input.retainedWorld->valid || !input.currentWorld->valid ||
		input.retainedWorld->level == nullptr || input.retainedWorld->level != input.currentWorld->level ||
		input.retainedWorld->buildSerial != input.buildSerial ||
		input.currentWorld->buildSerial != input.buildSerial ||
		input.retainedChunk->chunkIndex != input.currentChunk->chunkIndex ||
		input.retainedChunk->sectorIndex != input.currentChunk->sectorIndex)
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::InvalidInput);
		return result;
	}

	uint64_t terminalStableId = UINT64_MAX;
	const bool terminalSE12 = ResolveTerminalSE12Authority(
		*input.movers, *input.retainedChunk, input.buildSerial, input.mapEpoch, terminalStableId);
	if (terminalSE12)
	{
		result.kind = NRIMapMaterialOnlyRouteKind::TerminalSE12;
		result.ownerStableId = terminalStableId;
	}
	else if (input.allowAnimatedMaterialState)
	{
		result.kind = NRIMapMaterialOnlyRouteKind::AnimatedChunk;
		result.ownerStableId = AnimatedChunkOwnerDomain ^ input.retainedChunk->chunkIndex;
	}
	else
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::InvalidAuthority);
		return result;
	}

	if (SceneUsesMutableCanvas(*input.retainedSceneView) || SceneUsesMutableCanvas(*input.currentSceneView))
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::UnsupportedTexture);
		return result;
	}

	authorityAudit.reset();
	nri_scene::PTMapMaterialLayoutOptions layoutOptions;
	layoutOptions.resolveTextureIdentity = ResolveTextureIdentity;
	{
		NRICpuDetailAuditScope retainedAudit(PerfAuditMetric::MutationCanonicalRetainedLayoutNs);
		if (input.retainedLayout != nullptr && input.retainedLayout->valid)
		{
			if (detail)
			{
				const auto& source = *input.retainedLayout;
				PerfAuditAdd(PerfAuditMetric::MutationCanonicalRetainedLayoutCopies);
				PerfAuditAdd(PerfAuditMetric::MutationCanonicalRetainedLayoutCopiedBytes,
					source.canonicalSlotIds.size() * sizeof(uint64_t) +
					source.canonicalSemanticSlots.size() * sizeof(nri_scene::PTMapMaterialSemanticSlot) +
					source.canonicalMaterialStateKeys.size() * sizeof(uint64_t) +
					(source.emittedToCanonical.size() + source.canonicalToEmitted.size()) * sizeof(uint32_t));
			}
			result.retainedLayout = *input.retainedLayout;
		}
		else if ([&]()
		{
			if (detail) PerfAuditAdd(PerfAuditMetric::MutationCanonicalRetainedLayoutBuilds);
			return !nri_scene::BuildCanonicalPTMapMaterialLayoutForChunkSceneView(
			*input.retainedWorld, *input.retainedChunk, *input.retainedSceneView,
			result.retainedLayout, result.validation, layoutOptions);
		}())
		{
			Reject(result, NRIMapMaterialOnlyRouteReject::RetainedLayout);
			return result;
		}
	}
	if ([&]()
	{
		NRICpuDetailAuditScope currentAudit(PerfAuditMetric::MutationCanonicalCurrentLayoutNs);
		if (detail) PerfAuditAdd(PerfAuditMetric::MutationCanonicalCurrentLayoutBuilds);
		return !nri_scene::BuildCanonicalPTMapMaterialLayoutForChunkSceneView(
			*input.currentWorld, *input.currentChunk, *input.currentSceneView,
			result.currentLayout, result.validation, layoutOptions);
	}())
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::CurrentLayout);
		return result;
	}
	if ([&]()
	{
		NRICpuDetailAuditScope remapAudit(PerfAuditMetric::MutationCanonicalRemapNs);
		if (detail)
		{
			const auto& current = *input.currentSceneView;
			PerfAuditAdd(PerfAuditMetric::MutationCanonicalRemapSurfaceRows, current.opaqueWalls.size() + current.opaqueFlats.size());
			uint64_t vertices = 0, indices = 0;
			const auto count = [&](const auto& rows)
			{
				for (const auto& row : rows) { vertices += row.vertices.size(); indices += row.indices.size(); }
			};
			count(current.opaqueWalls); count(current.opaqueFlats);
			PerfAuditAdd(PerfAuditMetric::MutationCanonicalRemapVertexBytes, vertices * sizeof(nri_scene::CapturedVertex));
			PerfAuditAdd(PerfAuditMetric::MutationCanonicalRemapIndexBytes, indices * sizeof(uint32_t));
		}
		return !nri_scene::RemapPTMapMaterialSceneViewToRetainedLayout(
			result.retainedLayout, result.currentLayout, *input.currentSceneView,
			result.residentOrderSceneView, result.validation);
	}())
	{
		Reject(result, result.validation.failure == nri_scene::PTMapMaterialVariantRouteFailure::LayoutMismatch ?
			NRIMapMaterialOnlyRouteReject::LayoutMismatch : NRIMapMaterialOnlyRouteReject::SceneRemap);
		return result;
	}
	if ([&]()
	{
		NRICpuDetailAuditScope validationAudit(PerfAuditMetric::MutationCanonicalGeometryValidationNs);
		return !nri_scene::ValidatePTMapMaterialVariantGeometryLayout(
			*input.retainedSceneView, result.residentOrderSceneView, result.validation);
	}())
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::GeometryMismatch);
		return result;
	}
	if (terminalSE12 && [&]()
	{
		NRICpuDetailAuditScope terminalAudit(PerfAuditMetric::MutationCanonicalTerminalValidationNs);
		return !OnlyTerminalPaletteShadeChanged(input.retainedSceneView->opaqueWalls, result.residentOrderSceneView.opaqueWalls) ||
			!OnlyTerminalPaletteShadeChanged(input.retainedSceneView->opaqueFlats, result.residentOrderSceneView.opaqueFlats);
	}())
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::TerminalStateMismatch);
		return result;
	}

	nri_scene::PTMapMaterialStateVariantIdentity identity;
	identity.mapEpoch = input.mapEpoch;
	identity.buildSerial = input.buildSerial;
	identity.moverStableId = result.ownerStableId;
	identity.chunkIndex = input.retainedChunk->chunkIndex;
	{
		NRICpuDetailAuditScope variantAudit(PerfAuditMetric::MutationCanonicalVariantNs);
		result.variant = mVariants.Resolve(identity, result.currentLayout);
	}
	if (!result.variant.eligible)
	{
		Reject(result, NRIMapMaterialOnlyRouteReject::VariantRejected);
		return result;
	}

	result.admitted = true;
	result.layoutKey = result.currentLayout.layoutKey;
	result.stateKey = result.currentLayout.stateKey;
	result.reject = NRIMapMaterialOnlyRouteReject::None;
	mFrameStats.admitted++;
	if (result.kind == NRIMapMaterialOnlyRouteKind::TerminalSE12) mFrameStats.terminalAdmissions++;
	if (result.kind == NRIMapMaterialOnlyRouteKind::AnimatedChunk) mFrameStats.animatedAdmissions++;
	if (result.variant.decision == nri_scene::PTMapMaterialStateVariantDecision::Hit) mFrameStats.variantHits++;
	if (result.variant.inserted) mFrameStats.variantInserts++;
	if (result.variant.evicted) mFrameStats.variantEvictions++;
	if (result.variant.recordEvicted) mFrameStats.recordEvictions++;
	return result;
}

void NRIMapMaterialOnlyRoute::Reset()
{
	mFrameIndex = UINT64_MAX;
	mFrameStats = {};
	mVariants.Reset();
}

const char* GetNRIMapMaterialOnlyRouteKindName(NRIMapMaterialOnlyRouteKind kind)
{
	switch (kind)
	{
	case NRIMapMaterialOnlyRouteKind::None: return "none";
	case NRIMapMaterialOnlyRouteKind::TerminalSE12: return "terminal-se12";
	case NRIMapMaterialOnlyRouteKind::AnimatedChunk: return "animated-chunk";
	}
	return "unknown";
}

const char* GetNRIMapMaterialOnlyRouteRejectName(NRIMapMaterialOnlyRouteReject reject)
{
	switch (reject)
	{
	case NRIMapMaterialOnlyRouteReject::None: return "none";
	case NRIMapMaterialOnlyRouteReject::InvalidInput: return "invalid-input";
	case NRIMapMaterialOnlyRouteReject::InvalidAuthority: return "invalid-authority";
	case NRIMapMaterialOnlyRouteReject::RetainedLayout: return "retained-layout";
	case NRIMapMaterialOnlyRouteReject::CurrentLayout: return "current-layout";
	case NRIMapMaterialOnlyRouteReject::LayoutMismatch: return "layout-mismatch";
	case NRIMapMaterialOnlyRouteReject::SceneRemap: return "scene-remap";
	case NRIMapMaterialOnlyRouteReject::GeometryMismatch: return "geometry-mismatch";
	case NRIMapMaterialOnlyRouteReject::UnsupportedTexture: return "unsupported-texture";
	case NRIMapMaterialOnlyRouteReject::TerminalStateMismatch: return "terminal-state-mismatch";
	case NRIMapMaterialOnlyRouteReject::VariantRejected: return "variant-rejected";
	}
	return "unknown";
}
