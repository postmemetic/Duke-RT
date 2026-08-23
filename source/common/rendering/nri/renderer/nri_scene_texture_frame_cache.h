#pragma once

#include "../scene/nri_material_bridge.h"

#include <cstdint>
#include <vector>

namespace nri
{
	struct Descriptor;
}

struct NRISceneTextureFrameReuseInputs
{
	uint64_t engineUpdateGeneration = 0;
	uint64_t mapBuildSerial = 0;
	uint32_t ticksExecutedThisPresentation = 0;
	bool allowReuse = false;
	bool validateReuse = false;
};

struct NRISceneTextureFrameProductKey
{
	uint64_t engineUpdateGeneration = 0;
	uint64_t mapBuildSerial = 0;
	uint64_t slotMappingRevision = 0;
	uintptr_t activeCanvasSourcePointer = 0;
	uint32_t textureCount = 0;
	bool residentStaticUsesStableSlots = false;
};

struct NRISceneTextureDynamicDependency
{
	uint32_t uploadIndex = 0;
	uint32_t descriptorIndex = 0;
};

struct NRISceneTextureFrameProductTelemetry
{
	uint32_t requestedTextureCount = 0;
	uint32_t referencedActorMaterialCount = 0;
	uint32_t referencedBaseCount = 0;
	uint32_t referencedGlowCount = 0;
	uint32_t referencedNormalCount = 0;
	uint32_t referencedMetallicCount = 0;
	uint32_t referencedRoughnessCount = 0;
	uint32_t referencedEmissiveCount = 0;
	uint32_t truncatedTextureCount = 0;
	uint32_t stableDescriptorHits = 0;
};

struct NRISceneTextureFrameProduct
{
	NRISceneTextureFrameProductKey key;
	std::vector<uint64_t> sourceTextureKeys;
	std::vector<uint32_t> sourceTextureWidths;
	std::vector<uint32_t> sourceTextureHeights;
	std::vector<uint32_t> sourceTextureMipCounts;
	std::vector<uint64_t> sourceTexturePayloadSignatures;
	std::vector<uintptr_t> sourceTexturePointers;
	std::vector<uint8_t> sourceTextureIndexed;
	std::vector<nri::Descriptor*> descriptorTemplate;
	std::vector<NRISceneTextureDynamicDependency> dynamicDependencies;
	NRISceneTextureFrameProductTelemetry telemetry;
	uint64_t traceKey = 0;
	bool stableSlotMode = false;
	bool valid = false;
};

enum NRISceneTextureFrameMissReason : uint32_t
{
	NRISceneTextureFrameMiss_None = 0,
	NRISceneTextureFrameMiss_NoProduct = 1u << 0,
	NRISceneTextureFrameMiss_EngineGeneration = 1u << 1,
	NRISceneTextureFrameMiss_MapSerial = 1u << 2,
	NRISceneTextureFrameMiss_SlotMapping = 1u << 3,
	NRISceneTextureFrameMiss_ActiveCanvas = 1u << 4,
	NRISceneTextureFrameMiss_Counts = 1u << 5,
	NRISceneTextureFrameMiss_Namespace = 1u << 6,
	NRISceneTextureFrameMiss_TextureSources = 1u << 7,
};

class NRISceneTextureFrameCache
{
public:
	const NRISceneTextureFrameProduct* Find(
		const NRISceneTextureFrameProductKey& key,
		const nri_scene::MaterialBridgeData& materials,
		uint32_t* outMissReasonMask = nullptr) const;
	void Store(
		const NRISceneTextureFrameProductKey& key,
		const nri_scene::MaterialBridgeData& materials,
		const std::vector<nri::Descriptor*>& descriptorTemplate,
		const std::vector<NRISceneTextureDynamicDependency>& dynamicDependencies,
		const NRISceneTextureFrameProductTelemetry& telemetry,
		bool stableSlotMode);
	void Reset();
	uint64_t LastTraceKey() const { return mProduct.valid ? mProduct.traceKey : 0; }

	static uint64_t BuildTraceKey(
		const NRISceneTextureFrameProductKey& key,
		const nri_scene::MaterialBridgeData& materials);

private:
	NRISceneTextureFrameProduct mProduct;
};
