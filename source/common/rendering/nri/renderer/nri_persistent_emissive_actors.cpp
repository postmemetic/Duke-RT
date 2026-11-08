#include "nri_scene_lights.h"
#include "nri_cvars.h"
#include "nri_diagnostic_names.h"
#include "actor_lifecycle_journal.h"
#include "coreactor.h"
#include "hw_voxels.h"
#include "buildtiles.h"
#include "texturemanager.h"
#include "printf.h"
#include "perf_audit.h"

namespace
{
	enum class ActorSpriteLiveMatchResult : uint32_t
	{
		Match = 0,
		NullLiveTexture,
		TextureMismatch,
		PaletteMismatch
	};

	struct ActorSpriteLiveMatchDetails
	{
		ActorSpriteLiveMatchResult result = ActorSpriteLiveMatchResult::Match;
		FGameTexture* liveTexture = nullptr;
		int32_t liveTextureId = -1;
		int32_t surfaceTextureId = -1;
		int32_t livePalette = 0;
		int32_t surfacePalette = 0;
	};

	const char* GetActorSpriteLiveMatchResultName(ActorSpriteLiveMatchResult result)
	{
		switch (result)
		{
		case ActorSpriteLiveMatchResult::Match: return "match";
		case ActorSpriteLiveMatchResult::NullLiveTexture: return "null_live_texture";
		case ActorSpriteLiveMatchResult::TextureMismatch: return "texture_mismatch";
		case ActorSpriteLiveMatchResult::PaletteMismatch: return "palette_mismatch";
		default: return "unknown";
		}
	}

	FTextureID GetLiveActorDisplayTextureId(const DCoreActor& actor)
	{
		return actor.dispictex.isValid() ? actor.dispictex : actor.spr.spritetexture();
	}

	FGameTexture* GetLiveActorSurfaceTexture(const DCoreActor& actor, nri_scene::SurfaceSourceType sourceType)
	{
		switch (sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::FacingSprite:
			return TexMan.GetGameTexture(GetLiveActorDisplayTextureId(actor));

		case nri_scene::SurfaceSourceType::VoxelProxySprite:
		{
			if (!r_voxels)
			{
				return nullptr;
			}

			const int voxelIndex = GetExtInfo(actor.spr.spritetexture()).tiletovox;
			if (voxelIndex < 0 || voxelIndex >= MAXVOXELS || voxmodels[voxelIndex] == nullptr || voxmodels[voxelIndex]->model == nullptr)
			{
				return nullptr;
			}

			return TexMan.GetGameTexture(voxmodels[voxelIndex]->model->GetPaletteTexture());
		}

		default:
			return nullptr;
		}
	}

	bool SurfaceUsesLiveActorTextureValidation(const nri_scene::SurfaceRef& surface)
	{
		if (surface.provenance.actorIndex < 0)
		{
			return false;
		}

		switch (surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
			return (surface.material.flags & nri_scene::MaterialFlag_Sprite) != 0;
		case nri_scene::SurfaceSourceType::FacingSprite:
		case nri_scene::SurfaceSourceType::VoxelProxySprite:
			return true;
		default:
			return false;
		}
	}

	ActorSpriteLiveMatchDetails EvaluateCachedSurfaceMatchAgainstLiveActor(const nri_scene::SurfaceRef& surface, const DCoreActor& actor)
	{
		ActorSpriteLiveMatchDetails details = {};
		details.surfaceTextureId = surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1;
		details.surfacePalette = surface.material.palette;
		details.livePalette = actor.spr.pal;

		switch (surface.provenance.sourceType)
		{
		case nri_scene::SurfaceSourceType::DrawListWall:
		case nri_scene::SurfaceSourceType::FacingSprite:
		case nri_scene::SurfaceSourceType::VoxelProxySprite:
		{
			details.liveTexture = GetLiveActorSurfaceTexture(actor, surface.provenance.sourceType);
			details.liveTextureId = details.liveTexture != nullptr ? details.liveTexture->GetID().GetIndex() : -1;
			if (details.liveTexture == nullptr)
			{
				details.result = ActorSpriteLiveMatchResult::NullLiveTexture;
			}
			else if (surface.material.texture != details.liveTexture)
			{
				details.result = ActorSpriteLiveMatchResult::TextureMismatch;
			}
			else if (surface.material.palette != actor.spr.pal)
			{
				details.result = ActorSpriteLiveMatchResult::PaletteMismatch;
			}
			return details;
		}

		default:
			return details;
		}
	}

	bool CachedSurfaceMatchesLiveActor(const nri_scene::SurfaceRef& surface, const DCoreActor& actor)
	{
		return EvaluateCachedSurfaceMatchAgainstLiveActor(surface, actor).result == ActorSpriteLiveMatchResult::Match;
	}

}

void SceneLightSystem::PrunePersistentDynamicEmissiveCacheToLiveActors(const PersistentDynamicEmissiveCacheBuildServices& services)
{
	PerfAuditScope auditTimer(PerfAuditMetric::EmissiveRebuildNs);
	mActorSpriteDebugStats = {};
	if (!mPersistentDynamicEmissiveCache.valid)
	{
		return;
	}

	auto& lookup = mEmissiveActorLookup;
	NRIScopedEmissiveActorLookup borrowedActors{ lookup };
	std::unordered_map<int32_t, DCoreActor*> fullActors;
	bool useFull = false;
	{
		PerfAuditScope lookupTimer(PerfAuditMetric::EmissiveActorLookupNs);
		lookup.Begin(GetActorLifecycleJournal().WorldEpoch());
		auto requestOwners = [&lookup](const auto& surfaces)
		{
			for (const auto& surface : surfaces) lookup.Request(surface.provenance.actorIndex);
		};
		requestOwners(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
		requestOwners(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
		requestOwners(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
		lookup.Prepare();
		PerfAuditAdd(PerfAuditMetric::EmissiveActorLookupOwners, lookup.Owners().size());
		PerfAuditSet(PerfAuditMetric::EmissiveActorLookupScratchBytes, lookup.CapacityBytes());
		useFull = !nri_ptactorlightmaintenance || lookup.quarantined;
		uint64_t visits = 0;
		if (!useFull && !lookup.Owners().empty())
		{
			TSpriteIterator<DCoreActor> it;
			while (auto actor = it.Next())
			{
				++visits;
				if (actor->exists() && (actor->ObjectFlags & OF_EuthanizeMe) == 0)
					lookup.Set((int32_t)actor->GetIndex(), actor);
			}
		}
		if (useFull || nri_ptactorlightvalidate)
		{
			// Independent original full lookup. Do not share the selected-owner
			// membership decision with the validation reference.
			fullActors.reserve(256);
			uint64_t referenceVisits = 0;
			TSpriteIterator<DCoreActor> it;
			while (auto actor = it.Next())
			{
				++referenceVisits;
				if (actor->exists() && (actor->ObjectFlags & OF_EuthanizeMe) == 0)
					fullActors[(int32_t)actor->GetIndex()] = actor;
			}
			if (useFull) visits = referenceVisits;
			else
			{
				PerfAuditAdd(PerfAuditMetric::EmissiveActorLookupReferenceScanRows, referenceVisits);
				PerfAuditAdd(PerfAuditMetric::EmissiveActorLookupValidations);
				bool equal = true;
				auto compareOwners = [&](const auto& surfaces)
				{
					for (const auto& surface : surfaces)
					{
						const int32_t owner = surface.provenance.actorIndex;
						if (owner < 0) continue;
						const auto full = fullActors.find(owner);
						const auto actor = full != fullActors.end() ? full->second : nullptr;
						equal = equal && lookup.Find(owner) == actor;
					}
				};
				compareOwners(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
				compareOwners(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
				compareOwners(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
				if (!equal)
				{
					PerfAuditAdd(PerfAuditMetric::EmissiveActorLookupMismatches);
					Printf("NRI PT emissive actor lookup mismatch: frame=%llu action=full-lookup-quarantine\n",
						(unsigned long long)mFrameSerial);
					lookup.quarantined = true;
					useFull = true;
				}
			}
		}
		PerfAuditAdd(PerfAuditMetric::EmissiveActorScanRows, visits);
	}
	const auto findActor = [&](int32_t owner) -> DCoreActor*
	{
		if (!useFull) return static_cast<DCoreActor*>(lookup.Find(owner));
		const auto found = fullActors.find(owner);
		return found != fullActors.end() ? found->second : nullptr;
	};

	bool needsPrune = false;
	auto detectStaleActorOwnership = [this, &services, &needsPrune, &findActor](const auto& surfaces)
	{
		for (const auto& surface : surfaces)
		{
			if (SurfaceUsesLiveActorTextureValidation(surface))
			{
				mActorSpriteDebugStats.lastPruneChecks++;
				if (surface.provenance.actorIndex < 0)
				{
					mActorSpriteDebugStats.lastPruneDroppedMissingActorIndex++;
					if (services.traceActorSpriteMismatch)
					{
						Printf("NRI PT actor-sprite cache: action=drop reason=missing_actor_index source=%s actor=%d surface_tex=%d surface_ptr=%p surface_pal=%d\n",
							nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1,
							surface.material.texture,
							surface.material.palette);
					}
					needsPrune = true;
					continue;
				}

				auto liveActor = findActor(surface.provenance.actorIndex);
				if (liveActor == nullptr)
				{
					mActorSpriteDebugStats.lastPruneDroppedMissingActor++;
					if (services.traceActorSpriteMismatch)
					{
						Printf("NRI PT actor-sprite cache: action=drop reason=missing_actor source=%s actor=%d surface_tex=%d surface_ptr=%p surface_pal=%d\n",
							nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							surface.material.texture != nullptr ? surface.material.texture->GetID().GetIndex() : -1,
							surface.material.texture,
							surface.material.palette);
					}
					needsPrune = true;
					continue;
				}

				const ActorSpriteLiveMatchDetails match = EvaluateCachedSurfaceMatchAgainstLiveActor(surface, *liveActor);
				if (match.result == ActorSpriteLiveMatchResult::Match)
				{
					mActorSpriteDebugStats.lastPruneMatches++;
					if (services.traceActorSpriteVerbose)
					{
						Printf("NRI PT actor-sprite cache: action=keep reason=%s source=%s actor=%d surface_tex=%d surface_ptr=%p live_tex=%d live_ptr=%p surface_pal=%d live_pal=%d\n",
							GetActorSpriteLiveMatchResultName(match.result),
							nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
							surface.provenance.actorIndex,
							match.surfaceTextureId,
							surface.material.texture,
							match.liveTextureId,
							match.liveTexture,
							match.surfacePalette,
							match.livePalette);
					}
					continue;
				}

				switch (match.result)
				{
				case ActorSpriteLiveMatchResult::NullLiveTexture: mActorSpriteDebugStats.lastPruneDroppedNullLiveTexture++; break;
				case ActorSpriteLiveMatchResult::TextureMismatch: mActorSpriteDebugStats.lastPruneDroppedTextureMismatch++; break;
				case ActorSpriteLiveMatchResult::PaletteMismatch: mActorSpriteDebugStats.lastPruneDroppedPaletteMismatch++; break;
				default: break;
				}
				if (services.traceActorSpriteMismatch)
				{
					Printf("NRI PT actor-sprite cache: action=drop reason=%s source=%s actor=%d surface_tex=%d surface_ptr=%p live_tex=%d live_ptr=%p surface_pal=%d live_pal=%d\n",
						GetActorSpriteLiveMatchResultName(match.result),
						nri_diag::GetSurfaceSourceTypeName(surface.provenance.sourceType),
						surface.provenance.actorIndex,
						match.surfaceTextureId,
						surface.material.texture,
						match.liveTextureId,
						match.liveTexture,
						match.surfacePalette,
						match.livePalette);
				}
				needsPrune = true;
			}
			else if (surface.provenance.actorIndex >= 0 &&
				findActor(surface.provenance.actorIndex) == nullptr)
			{
				needsPrune = true;
			}
		}
	};

	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats);
	detectStaleActorOwnership(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites);
	if (!needsPrune)
	{
		return;
	}
	PerfAuditAdd(PerfAuditMetric::EmissivePruneRebuilds);

	PersistentDynamicEmissiveCache next = {};
	next.sceneView.drawInfo = mPersistentDynamicEmissiveCache.sceneView.drawInfo;
	next.sceneView.sky = mPersistentDynamicEmissiveCache.sceneView.sky;
	std::copy_n(mPersistentDynamicEmissiveCache.sceneView.skyColor, 3, next.sceneView.skyColor);
	std::copy_n(mPersistentDynamicEmissiveCache.sceneView.groundColor, 3, next.sceneView.groundColor);

	auto appendLiveOwnedSurfaces = [&findActor](const auto& source, auto& destination)
	{
		for (const auto& surface : source)
		{
			if (SurfaceUsesLiveActorTextureValidation(surface))
			{
				if (surface.provenance.actorIndex < 0)
				{
					continue;
				}

				auto liveActor = findActor(surface.provenance.actorIndex);
				if (liveActor == nullptr || !CachedSurfaceMatchesLiveActor(surface, *liveActor))
				{
					continue;
				}
			}
			else if (surface.provenance.actorIndex >= 0 &&
				findActor(surface.provenance.actorIndex) == nullptr)
			{
				continue;
			}

			destination.push_back(surface);
			PerfAuditAdd(PerfAuditMetric::EmissiveSurfaceCopyBytes, sizeof(surface) +
				surface.vertices.size() * sizeof(nri_scene::CapturedVertex) + surface.indices.size() * sizeof(uint32_t) + surface.primitiveLocalMaterialSlots.size());
		}
	};

	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueWalls, next.sceneView.opaqueWalls);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueFlats, next.sceneView.opaqueFlats);
	appendLiveOwnedSurfaces(mPersistentDynamicEmissiveCache.sceneView.opaqueSprites, next.sceneView.opaqueSprites);

	next.surfaceCount =
		(uint32_t)next.sceneView.opaqueWalls.size() +
		(uint32_t)next.sceneView.opaqueFlats.size() +
		(uint32_t)next.sceneView.opaqueSprites.size();
	if (next.surfaceCount == 0)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	services.BuildGeometry(next.sceneView, next.geometry, "persistent_emissive_cache_prune");
	services.BuildMaterials(next.sceneView, next.materialBridge, "persistent_emissive_cache_prune");

	next.primitiveCount = (uint32_t)next.geometry.primitives.size();
	next.materialCount = (uint32_t)next.materialBridge.materials.size();
	next.surfaceStats = BuildPersistentDynamicSurfaceStats(next.sceneView);
	next.sceneView.stats.totalDrawItems = next.surfaceCount;
	next.sceneView.stats.wallDrawItems = (uint32_t)next.sceneView.opaqueWalls.size();
	next.sceneView.stats.flatDrawItems = (uint32_t)next.sceneView.opaqueFlats.size();
	next.sceneView.stats.spriteDrawItems = (uint32_t)next.sceneView.opaqueSprites.size();
	next.sceneView.stats.triangleEstimate = next.primitiveCount;
	next.sceneView.stats.materialRefs = next.materialCount;
	next.valid = next.primitiveCount > 0 && next.materialCount > 0;
	if (!next.valid)
	{
		mPersistentDynamicEmissiveCache = {};
		return;
	}

	mPersistentDynamicEmissiveCache = std::move(next);
}
