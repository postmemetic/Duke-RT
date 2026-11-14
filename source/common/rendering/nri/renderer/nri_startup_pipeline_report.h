#pragma once

#include "NRI.h"
#include "common/platform/startup_progress.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Startup reporting only. Pipeline ownership, descriptors, creation order and
// failure cleanup remain with the caller. Runtime lazy creation stays silent.
class NRIStartupPipelineReport
{
public:
	NRIStartupPipelineReport(const char* shaderName, const char* label, nri::GraphicsAPI api)
		: mShaderName(shaderName), mApi(api), mActive(StartupProgress::IsActive())
	{
		if (mActive)
		{
			StartupProgress::CheckCancellation();
			StartupProgress::BeginTask(label);
		}
	}

	~NRIStartupPipelineReport()
	{
		if (mActive && !mFinished)
			StartupProgress::EndTask(false);
	}

	NRIStartupPipelineReport(const NRIStartupPipelineReport&) = delete;
	NRIStartupPipelineReport& operator=(const NRIStartupPipelineReport&) = delete;

	template<typename LoadShader>
	bool Read(LoadShader&& loadShader, std::vector<uint8_t>& blob)
	{
		bool loaded;
		{
			StartupProgress::Operation operation("shader-read", mShaderName);
			loaded = loadShader();
			operation.Finish(loaded);
		}
		// Hashing and log formatting are deliberately outside both measured calls.
		if (loaded && mActive && StartupProgress::IsTracing())
		{
			uint64_t hash = UINT64_C(14695981039346656037);
			for (uint8_t byte : blob)
			{
				hash ^= byte;
				hash *= UINT64_C(1099511628211);
			}
			StartupProgress::TraceDetail("shader=%s api=%s bytes=%llu fnv1a64=%016llx",
				mShaderName, mApi == nri::GraphicsAPI::D3D12 ? "d3d12" : "vulkan",
				static_cast<unsigned long long>(blob.size()), static_cast<unsigned long long>(hash));
		}
		return loaded;
	}

	template<typename CreatePipeline>
	nri::Result Create(CreatePipeline&& createPipeline)
	{
		if (mActive)
			StartupProgress::CheckCancellation();
		StartupProgress::Operation operation("shader-pipeline", mShaderName);
		const nri::Result result = createPipeline();
		const bool success = result == nri::Result::SUCCESS;
		operation.Finish(success, static_cast<int64_t>(result));
		if (mActive)
			StartupProgress::EndTask(success);
		mFinished = true;
		return result;
	}

private:
	const char* mShaderName;
	nri::GraphicsAPI mApi;
	bool mActive;
	bool mFinished = false;
};
