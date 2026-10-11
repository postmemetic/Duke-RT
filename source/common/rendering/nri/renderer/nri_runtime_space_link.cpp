#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../scene/nri_map_builder.h"
#include "../../hwrenderer/data/hw_clock.h"
#include "c_cvars.h"
#include "perf_capture.h"
#include "gamestruct.h"
#include "nri_render_geometry_helpers.h"

#include <chrono>
#include <cmath>


namespace
{
	static bool ShouldCollectRuntimeSpaceLinkPerfTiming()
	{
		const bool perfLoopTraceActive = (int)perf_looptraceframes > 0;
		const bool temporalTraceActive = !!nri_pttemporaltrace && (int)nri_pttraceframes > 0;
		return perfLoopTraceActive || temporalTraceActive || (bool)nri_ptslowdowntrace || (bool)nri_ptscenestats || PerfCompactCaptureTimingActive();
	}

	static double RuntimeSpaceLinkDurationMs(const std::chrono::steady_clock::time_point& start, const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	class ScopedPtPerfTimer
	{
	public:
		explicit ScopedPtPerfTimer(double& targetMs)
			: mTarget(ShouldCollectRuntimeSpaceLinkPerfTiming() ? &targetMs : nullptr)
		{
			if (mTarget != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedPtPerfTimer()
		{
			if (mTarget != nullptr)
			{
				*mTarget += RuntimeSpaceLinkDurationMs(mStart, std::chrono::steady_clock::now());
			}
		}

	private:
		double* mTarget = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};
}

bool NRIRenderer::BuildRuntimeSpaceLinkOverlay(HWDrawInfo& di, nri_scene::GeometryData& outGeometry, nri_scene::MaterialBridgeData& outMaterials)
{
	nri_scene::ClearGeometryRetainingCapacity(outGeometry);
	nri_scene::ClearMaterialBridgeRetainingCapacity(outMaterials);
	mRuntimeSpaceLinkLastFrame = {};
	mRuntimeSpaceLinkLastFrame.orphanLocalSpaceCount = CountOrphanLocalSpaces(mMapWorld);
	mRuntimeSpaceLinkLastFrame.unresolvedRuntimePortalCount = mMapWorld.stats.runtimePortalCount;

	const auto deactivateRuntimeLinkHistory = [&]()
	{
		if (!mRuntimeChunkTranslationHistory.empty())
		{
			mRuntimeSpaceLinkLastFrame.topologyChanged = true;
			RequestHistoryReset("runtime-link-deactivated", false, true);
		}
	};

	if (!mMapWorld.valid)
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	int effectSectorIndex = -1;
	if (di.Viewpoint.SectNums != nullptr)
	{
		if (di.Viewpoint.SectCount > 0)
		{
			effectSectorIndex = di.Viewpoint.SectNums[0];
			mRuntimeSpaceLinkLastFrame.viewRootSectorCount = (uint32_t)di.Viewpoint.SectCount;
		}
	}
	else
	{
		effectSectorIndex = di.Viewpoint.SectCount;
		mRuntimeSpaceLinkLastFrame.viewRootSectorCount = 1;
	}

	if (effectSectorIndex < 0 || (unsigned)effectSectorIndex >= sector.Size())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const BitArray& visibleSectors = di.GetVisibleSectors();
	for (unsigned sectorIndex = 0; sectorIndex < visibleSectors.Size(); ++sectorIndex)
	{
		if (visibleSectors.Check(sectorIndex))
		{
			mRuntimeSpaceLinkLastFrame.visibleSectorCount++;
		}
	}

	mRuntimeSpaceLinkLastFrame.candidateSectorIndex = effectSectorIndex;
	mRuntimeSpaceLinkLastFrame.candidateSectorLotag = sector[(unsigned)effectSectorIndex].lotag;
	mRuntimeSpaceLinkLastFrame.queryAttempted = true;

	GeoEffect effect = {};
	int providerSectorIndex = -1;
	if (gi != nullptr && gi->GetGeoEffect(&effect, &sector[effectSectorIndex]))
	{
		providerSectorIndex = effectSectorIndex;
	}
	else
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
	}

	const auto getLocalSpaceIndex = [&](int sectorIndex) -> uint32_t
	{
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return UINT32_MAX;
		}

		return mMapWorld.chunks[(unsigned)sectorIndex].localSpaceIndex;
	};

	const uint32_t candidateLocalSpaceIndex = getLocalSpaceIndex(effectSectorIndex);
	const auto sectorMatchesVisibleSet = [&](int sectorIndex) -> bool
	{
		return sectorIndex >= 0 &&
			(unsigned)sectorIndex < visibleSectors.Size() &&
			visibleSectors.Check((unsigned)sectorIndex);
	};
	auto groupMatchesCandidate = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesSector = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			if (sectorIndex < 0)
			{
				return false;
			}

			if (sectorIndex == effectSectorIndex)
			{
				return true;
			}

			if (candidateLocalSpaceIndex == UINT32_MAX)
			{
				return false;
			}

			return getLocalSpaceIndex(sectorIndex) == candidateLocalSpaceIndex;
		};

		return
			matchesSector(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesSector(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};
	auto groupMatchesVisibleSectors = [&](const GeoEffect& candidateEffect, int groupIndex) -> bool
	{
		auto matchesVisible = [&](sectortype* sect) -> bool
		{
			if (sect == nullptr)
			{
				return false;
			}

			const int sectorIndex = sector.IndexOf(sect);
			return sectorMatchesVisibleSet(sectorIndex);
		};

		return
			matchesVisible(candidateEffect.geosector != nullptr ? candidateEffect.geosector[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp != nullptr ? candidateEffect.geosectorwarp[groupIndex] : nullptr) ||
			matchesVisible(candidateEffect.geosectorwarp2 != nullptr ? candidateEffect.geosectorwarp2[groupIndex] : nullptr);
	};

	if (gi != nullptr)
	{
		for (unsigned sectorIndex = 0; sectorIndex < sector.Size(); ++sectorIndex)
		{
			if (sector[sectorIndex].lotag != 848)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.providerSectorCount++;

			GeoEffect candidateEffect = {};
			if (!gi->GetGeoEffect(&candidateEffect, &sector[sectorIndex]) || candidateEffect.geocnt <= 0)
			{
				continue;
			}

			mRuntimeSpaceLinkLastFrame.geoProviderCount++;
			mRuntimeSpaceLinkLastFrame.providerGroupCount += (uint32_t)candidateEffect.geocnt;

			bool matched = false;
			bool visibleMatched = false;
			for (int i = 0; i < candidateEffect.geocnt; ++i)
			{
				if (groupMatchesCandidate(candidateEffect, i))
				{
					matched = true;
				}
				if (groupMatchesVisibleSectors(candidateEffect, i))
				{
					visibleMatched = true;
				}
			}

			if (matched)
			{
				mRuntimeSpaceLinkLastFrame.localSpaceMatchedProviderCount++;
			}
			if (visibleMatched)
			{
				mRuntimeSpaceLinkLastFrame.visibleMatchedProviderCount++;
			}

			if (providerSectorIndex >= 0 || !matched)
			{
				continue;
			}

			effect = candidateEffect;
			providerSectorIndex = (int)sectorIndex;
			break;
		}
	}

	if (providerSectorIndex < 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	mRuntimeSpaceLinkLastFrame.sourceSectorIndex = providerSectorIndex;
	mRuntimeSpaceLinkLastFrame.reportedGeoCount = effect.geocnt;
	if (effect.geocnt <= 0)
	{
		mRuntimeSpaceLinkLastFrame.queryRejected = true;
		deactivateRuntimeLinkHistory();
		return false;
	}

	struct RuntimeGeoLink
	{
		uint32_t chunkIndex = UINT32_MAX;
		float dx = 0.0f;
		float dz = 0.0f;
		float prevDx = 0.0f;
		float prevDz = 0.0f;
	};

	std::vector<RuntimeGeoLink> links;
	links.reserve((size_t)effect.geocnt * 2u);

	auto appendLink = [&](sectortype* warpedSector, double mapDx, double mapDy)
	{
		if (warpedSector == nullptr)
		{
			return;
		}

		const int32_t sectorIndex = sector.IndexOf(warpedSector);
		if (sectorIndex < 0 || (unsigned)sectorIndex >= mMapWorld.chunks.size())
		{
			return;
		}

		RuntimeGeoLink link = {};
		link.chunkIndex = (uint32_t)sectorIndex;
		link.dx = (float)mapDx;
		link.dz = (float)-mapDy;
		for (const RuntimeGeoLink& existing : links)
		{
			if (existing.chunkIndex == link.chunkIndex &&
				std::fabs(existing.dx - link.dx) < 0.001f &&
				std::fabs(existing.dz - link.dz) < 0.001f)
			{
				return;
			}
		}

		links.push_back(link);
	};

	for (int i = 0; i < effect.geocnt; ++i)
	{
		if (!groupMatchesCandidate(effect, i))
		{
			continue;
		}

		appendLink(effect.geosectorwarp != nullptr ? effect.geosectorwarp[i] : nullptr,
			effect.geox != nullptr ? effect.geox[i] : 0.0,
			effect.geoy != nullptr ? effect.geoy[i] : 0.0);
		appendLink(effect.geosectorwarp2 != nullptr ? effect.geosectorwarp2[i] : nullptr,
			effect.geox2 != nullptr ? effect.geox2[i] : 0.0,
			effect.geoy2 != nullptr ? effect.geoy2[i] : 0.0);
	}

	if (links.empty())
	{
		deactivateRuntimeLinkHistory();
		return false;
	}

	const auto findPreviousTranslation = [&](uint32_t chunkIndex, float& outPrevDx, float& outPrevDz) -> bool
	{
		for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
		{
			if (previous.chunkIndex == chunkIndex)
			{
				outPrevDx = previous.dx;
				outPrevDz = previous.dz;
				return true;
			}
		}

		return false;
	};

	for (RuntimeGeoLink& link : links)
	{
		findPreviousTranslation(link.chunkIndex, link.prevDx, link.prevDz);
	}

	const auto runtimeLinkTopologyChanged = [&]() -> bool
	{
		if (links.size() != mRuntimeChunkTranslationHistory.size())
		{
			return true;
		}

		for (const RuntimeGeoLink& link : links)
		{
			bool found = false;
			for (const RuntimeChunkTranslationState& previous : mRuntimeChunkTranslationHistory)
			{
				if (previous.chunkIndex == link.chunkIndex)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				return true;
			}
		}

		return false;
	};

	mRuntimeSpaceLinkLastFrame.geoEffectActive = true;
	mRuntimeSpaceLinkLastFrame.linkCount = (uint32_t)links.size();
	mRuntimeSpaceLinkLastFrame.topologyChanged = runtimeLinkTopologyChanged();
	if (mRuntimeSpaceLinkLastFrame.topologyChanged)
	{
		RequestHistoryReset("runtime-link-topology");
	}

	std::vector<RuntimeChunkTranslationState> nextRuntimeChunkTranslationHistory;
	nextRuntimeChunkTranslationHistory.reserve(links.size());

	for (const RuntimeGeoLink& link : links)
	{
		if (link.chunkIndex >= mMapWorld.chunks.size())
		{
			continue;
		}

		nri_scene::SceneView liveChunkView;
		nri_scene::PTMapWorldStats liveStats = {};
		nri_scene::PTMapBuildOptions mapBuildOptions = {};
		if (!nri_scene::BuildLiveMapChunkSceneView(mMapWorld.chunks[link.chunkIndex], liveChunkView, &liveStats, mapBuildOptions))
		{
			continue;
		}
		if (nri_ptceilingnudge)
		{
			NudgeMapCeilingSections(liveChunkView, (float)nri_ptceilingnudgedistance);
		}

		nri_scene::GeometryData chunkGeometry;
		nri_scene::MaterialBridgeData chunkMaterials;
		{
			Clocker clock(NriPTGeometryBuild);
			ScopedPtPerfTimer perfTimer(mLastPerfShellTraceStats.geometryBuildRuntimeSpaceLinkMs);
			nri_scene::BuildGeometry(liveChunkView, chunkGeometry);
			AssignGeometryPortalIndices(mMapWorld, chunkGeometry);
			TranslateGeometry(chunkGeometry, link.dx, 0.0f, link.dz, link.prevDx, 0.0f, link.prevDz);
		}
		mLastPerfShellTraceStats.geometryBuildRuntimeSpaceLinkCalls++;
		mLastPerfShellTraceStats.geometryBuildRuntimeSpaceLinkPrimitives += (uint32_t)chunkGeometry.primitives.size();
		{
			Clocker clock(NriPTMaterialBuild);
			BuildMaterialsWithActorOverrides(liveChunkView, chunkMaterials, "runtime_space_link_chunk");
		}

		if (!chunkGeometry.primitives.empty())
		{
			AppendGeometry(chunkGeometry, (uint32_t)outMaterials.materials.size(), outGeometry);
		}
		nri_scene::AppendMaterialBridge(chunkMaterials, outMaterials);

		mRuntimeSpaceLinkLastFrame.translatedChunkCount++;
		mRuntimeSpaceLinkLastFrame.surfaceCount += liveStats.surfaceCount;
		mRuntimeSpaceLinkLastFrame.triangleCount += liveStats.triangleCount;
		mRuntimeSpaceLinkLastFrame.materialCount += (uint32_t)chunkMaterials.materials.size();
		nextRuntimeChunkTranslationHistory.push_back({ link.chunkIndex, link.dx, link.dz });
	}

	mRuntimeChunkTranslationHistory = std::move(nextRuntimeChunkTranslationHistory);
	mRuntimeSpaceLinkLastFrame.active = !outGeometry.primitives.empty();
	return mRuntimeSpaceLinkLastFrame.active;
}
