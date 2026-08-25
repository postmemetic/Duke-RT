#pragma once

#include <cstdint>

namespace nri_scene
{
	enum class PTMapSurfaceKind : uint32_t;
	enum class SurfaceSourceType : uint32_t;
}

namespace nri_diag
{
	constexpr uint32_t PtDebugAnalyticDirect = 26u;
	constexpr uint32_t PtDebugEmissiveTags = 27u;
	constexpr uint32_t PtDebugEmissiveDirect = 28u;
	constexpr uint32_t PtDebugSectorAmbient = 29u;
	constexpr uint32_t PtDebugEmissiveSampleVisibility = 33u;
	constexpr uint32_t PtDebugUpscalerTraceTransparent = 34u;
	constexpr uint32_t PtDebugTaaPreExposedInput = 45u;
	constexpr uint32_t PtDebugIndirectLobeSelection = 46u;
	constexpr uint32_t PtDebugMotionValidity = 47u;

	constexpr uint32_t SceneDataSourceStatic = 0u;
	constexpr uint32_t SceneDataSourceDynamic = 1u;
	constexpr uint32_t SceneDataSourcePersistentVoxel = 2u;

	constexpr uint32_t SurfaceProbeOwnerUnknown = 0u;
	constexpr uint32_t SurfaceProbeOwnerStaticMap = 1u;
	constexpr uint32_t SurfaceProbeOwnerCapturedScene = 2u;
	constexpr uint32_t SurfaceProbeOwnerRuntimeLink = 3u;
	constexpr uint32_t SurfaceProbeOwnerRuntimeMutation = 4u;
	constexpr uint32_t SurfaceProbeOwnerDynamicOverlay = 5u;

	const char* GetSceneDataSourceName(uint32_t dataSource);
	const char* GetSurfaceProbeSceneOwnerName(uint32_t owner);
	const char* GetSurfaceSourceTypeName(nri_scene::SurfaceSourceType sourceType);
	const char* GetMapSurfaceKindName(nri_scene::PTMapSurfaceKind kind);
	const char* GetMaterialEmissiveModeName(uint32_t mode);
	const char* GetDrawListTypeName(uint32_t drawListType);
}
