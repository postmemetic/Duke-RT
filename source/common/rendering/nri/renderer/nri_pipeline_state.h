#pragma once

#include <cstdint>

class NRIRenderer;
namespace nri { struct PipelineLayout; }

enum class NRIPipelineSlot : uint32_t
{
	TraceOpaque,
	TraceOpaqueCache,
	TraceOpaqueLeanDebug,
	Composition,
	TraceTransparent,
	ExposureHistogramClear,
	ExposureHistogramBuild,
	ExposureResolve,
	Taa,
	TemporalReactiveCombine,
	RawPresent,
	FinalPresent,
	DlssSrBefore,
	DlssBefore,
	DlssAfter,
	Final,
	BloomCopy,
	VoxelComputeCount,
	VoxelComputeEmit,
	VoxelComputeClassify,
	VoxelComputeScan,
	VoxelComputeEmitParallel,
	VoxelComputeFinalize,
	BloomDownsample,
	BloomUpsample,
	BloomComposite,
	TraceOpaquePage,
	TraceOpaqueCachePage,
	TraceOpaqueLeanDebugPage,
	Count
};

class NRIPipelineStateManager
{
public:
	static bool CreatePipelineLayout(NRIRenderer& renderer);
	static bool EnsureIndirectRadianceCachePipeline(NRIRenderer& renderer, bool page = false);
	static void PrepareVoxelPageTracePipelines(NRIRenderer& renderer);
	static bool CreateTaaPipelineLayout(NRIRenderer& renderer);
	static bool CreatePresentPipelineLayout(NRIRenderer& renderer);
	static bool CreateExposurePipelineLayout(NRIRenderer& renderer);
	static bool CreateBloomPipelineLayout(NRIRenderer& renderer);
	static bool CreateVoxelComputePipelineLayout(NRIRenderer& renderer);
	static bool CreatePipelines(NRIRenderer& renderer);

private:
	static bool CreateComputePipeline(NRIRenderer& renderer, const char* fileName, const char* label,
		NRIPipelineSlot slot, nri::PipelineLayout* layout);
};
