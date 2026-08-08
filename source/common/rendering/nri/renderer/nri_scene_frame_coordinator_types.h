#pragma once

#include "nri_runtime_mutation.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

struct HWDrawInfo;

struct RenderSceneHistorySnapshot
{
	uint32_t frameIndex = 0;
	float currentCameraPos[3] = {};
	float currentCameraForward[3] = {};
	float currentCameraRight[3] = {};
	float currentCameraUp[3] = {};
	float previousCameraPos[3] = {};
	float previousCameraForward[3] = {};
	float previousCameraRight[3] = {};
	float previousCameraUp[3] = {};
	float currentJitter[2] = {};
	float previousJitter[2] = {};
	float currentViewToClip[16] = {};
	float previousViewToClip[16] = {};
	float currentWorldToView[16] = {};
	float previousWorldToView[16] = {};
	float currentTanHalfFovX = 0.0f;
	float currentTanHalfFovY = 0.0f;
	float previousTanHalfFovX = 0.0f;
	float previousTanHalfFovY = 0.0f;
	bool hasPreviousCameraState = false;
	bool resetHistory = false;
};

struct RenderSceneDispatchInputs
{
	bool bootstrapCapturedView = false;
	bool buffersReady = false;
	bool accelerationReady = false;
	bool mainViewEligible = false;
	HWDrawInfo* drawInfo = nullptr;
	const nri_scene::GeometryData* activeGeometry = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	int drawmode = 0;
};

struct RenderSceneCompletionInputs
{
	bool success = false;
	bool preserveHistory = false;
	bool bootstrapCapturedView = false;
	uint32_t traceFrameIndex = 0;
	int drawmode = 0;
	bool portal = false;
	const nri_scene::GeometryData* activeGeometry = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	bool usingPersistentDynamicEmissiveCache = false;
};

struct RenderSceneFrameBuildInputs
{
	uint64_t simulationGeneration = 0;
	uint64_t engineUpdateGeneration = 0;
	uint64_t presentationGeneration = 0;
	uint32_t ticksExecutedThisPresentation = 0;
	uint32_t bootstrapMode = 0;
	bool bootstrapCapturedView = false;
	bool bootstrapCapturedDiagnostics = false;
	bool bootstrapCapturedFlat = false;
	bool bootstrapCapturedBaseColor = false;
	bool rawTraceDirectScene = false;
	bool preserveHistory = false;
};

struct RenderSceneFrameBuildResult
{
	nri_scene::SceneView capturedSceneView;
	nri_scene::SceneView dynamicSceneView;
	nri_scene::SceneView localPlayerReflectionSceneView;
	nri_scene::SceneView surfaceLightSceneView;
	nri_scene::SceneView sceneLightMergedDynamicSceneView;
	nri_scene::SceneView mergedDynamicSceneView;
	nri_scene::GeometryData capturedGeometry;
	nri_scene::GeometryData runtimeSpaceLinkGeometry;
	nri_scene::GeometryData dynamicGeometry;
	nri_scene::GeometryData mergedDynamicGeometry;
	nri_scene::GeometryData actorFilteredDynamicGeometry;
	nri_scene::GeometryData debugSphereGeometry;
	nri_scene::GeometryData surfaceLightGeometry;
	NRIRuntimeMutationFrameOutput runtimeMutationFrame;
	nri_scene::MaterialBridgeData materialBridge;
	nri_scene::MaterialBridgeData runtimeSpaceLinkMaterialBridge;
	nri_scene::MaterialBridgeData dynamicMaterialBridge;
	nri_scene::MaterialBridgeData localPlayerReflectionMaterialBridge;
	nri_scene::MaterialBridgeData sceneLightMergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData mergedDynamicMaterialBridge;
	nri_scene::MaterialBridgeData debugSphereMaterialBridge;
	nri_scene::MaterialBridgeData surfaceLightMaterialBridge;
	nri_scene::MaterialBridgeData combinedMaterialBridge;
	const nri_scene::SceneView* activeSceneView = nullptr;
	const nri_scene::GeometryData* activeGeometry = nullptr;
	const std::vector<nri_scene::MaterialData>* activeGpuMaterials = nullptr;
	const nri_scene::MaterialBridgeData* activeMaterialBridge = nullptr;
	const nri_scene::SceneView* activeDynamicSceneView = nullptr;
	const nri_scene::GeometryData* activeDynamicGeometry = nullptr;
	const nri_scene::MaterialBridgeData* activeDynamicMaterials = nullptr;
	nri_scene::SceneDebugStats activeStats = {};
	bool paletteReady = true;
	bool texturesReady = true;
	bool buffersReady = true;
	bool accelerationReady = true;
	bool usingPersistentDynamicEmissiveCache = false;
};
