#pragma once

#include <cstdint>
#include <vector>

class NRIPassDispatchContext;
class NRIRenderer;
struct HWDrawInfo;

namespace nri_scene
{
	struct GeometryData;
	struct MaterialData;
}

enum class NRIFramePass : uint32_t
{
	TraceOpaque,
	Denoiser,
	Composition,
	TraceTransparent,
	Exposure,
	UpscaleChain,
	RawPresent,
	FinalPresent,
	Final,
	CopyFinalToTarget,
};

enum class NRIPresentRouteKind : uint32_t
{
	BootstrapFinal,
	ResolvedBeauty,
	ComposedDebug,
	UpscalerTraceTransparentProbe,
	ValidationRaw,
	DenoisedRaw,
	ShadowFinal,
	FinalDebug,
	RawTraceDebug,
	FallbackFinal,
};

struct NRIPresentRouteInfo
{
	NRIPresentRouteKind kind = NRIPresentRouteKind::FallbackFinal;
	const char* routeName = "fallback_final";
	const char* presenterName = "Final";
	const char* ownerName = "fallback";
	const char* passListName = "TraceOpaque,Final,CopyFinal";
	bool denoiserRun = false;
	bool upscalerRun = false;
	bool exposureRun = false;
	bool compositionPath = false;
	bool rawTraceDebug = false;
};

struct NRIFrameRouteRequest
{
	uint32_t debugMode = 0;
	bool bootstrap = false;
	uint32_t bootstrapMode = 0;
};

struct NRIFrameGraphExecutionRequest
{
	int ptDebugMode = 0;
	bool denoise = false;
	NRIPresentRouteInfo presentRoute = {};
};

const char* GetNRIFramePassName(NRIFramePass pass);
bool IsNRIFrameGraphSupportedDebugMode(uint32_t debugMode);
bool IsNRIFrameGraphRawTraceDebugMode(uint32_t debugMode);
// Only these raw presenters are independent of all hit-lighting/radiance outputs.
bool IsNRIFrameGraphLightingIndependentDebugMode(uint32_t debugMode);
bool IsNRIFrameGraphFinalShaderDebugMode(uint32_t debugMode);
NRIPresentRouteInfo ResolveNRIFrameRoute(const NRIFrameRouteRequest& request);
bool ExecuteNRIFrameGraph(
	NRIPassDispatchContext& context,
	HWDrawInfo& di,
	const nri_scene::GeometryData& geometry,
	const std::vector<nri_scene::MaterialData>& materials,
	const NRIFrameGraphExecutionRequest& request);
