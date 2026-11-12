#include "nri_renderer.h"
#include "../system/nri_renderdevice.h"
#include "perf_audit.h"

void NRIRenderer::ReportResidentBlasScratchMemory()
{
	if (!PerfAuditActive()) return;
	PerfAuditSet(PerfAuditMetric::ResidentBlasScratchRetainedCount, mResidentBlasScratchRetirement.RetainedCount());
	PerfAuditSet(PerfAuditMetric::ResidentBlasScratchRetainedBytes, mResidentBlasScratchRetirement.RetainedBytes());
	PerfAuditSet(PerfAuditMetric::ResidentBlasScratchPeakRetainedBytes, mResidentBlasScratchRetirement.PeakRetainedBytes());
	PerfAuditSet(PerfAuditMetric::ResidentBlasScratchLiveAllocatedBytes,
		mResidentStaticBlasScratchBuffer.memorySize + mResidentBlasScratchRetirement.RetainedBytes());
	PerfAuditSet(PerfAuditMetric::ResidentBlasScratchPeakLiveAllocatedBytes, mResidentBlasScratchRetirement.PeakLiveBytes());
}

NRIResidentBlasScratchRetirement::Result NRIRenderer::EnsureResidentBlasScratch(uint64_t requiredBytes, bool optional)
{
	using Result = NRIResidentBlasScratchRetirement::Result;
	const uint64_t oldBytes = mResidentStaticBlasScratchBuffer.memorySize;
	const bool hadBuffer = mResidentStaticBlasScratchBuffer.buffer != nullptr;
	const uint64_t fence = mFrameBuffer != nullptr && mFrameBuffer->mCommandBuffer != nullptr
		? GetRecordingCommandFenceValue() : 0;
	const auto result = mResidentBlasScratchRetirement.Ensure(mResidentStaticBlasScratchBuffer, requiredBytes, fence,
		[this](NRIBufferResource& resource, uint64_t bytes) {
			return CreateBufferWithoutView(resource, bytes, 16, nri::BufferUsageBits::SCRATCH_BUFFER);
		}, [this](NRIBufferResource& resource) { DestroyBufferResource(resource); });
	if (PerfAuditActive())
	{
		if (result == Result::Grown)
		{
			PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchGrowths);
			if (!optional) PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchMandatoryGrowths);
			if (hadBuffer)
			{
				PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchRetirements);
				PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchRetiredBytes, oldBytes);
			}
		}
		else if (result == Result::AllocationFailed) PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchAllocationFailures);
		else if (result == Result::NoRecordingOwner) PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchNoOwnerFailures);
		if (result != Result::Unchanged)
		{
			const FString fields = FStringf("\"optional\":%s,\"result\":%u,\"command_fence\":%llu,\"requested_bytes\":%llu,\"old_bytes\":%llu,\"active_bytes\":%llu,\"retained_bytes\":%llu,\"retained_count\":%llu",
				optional ? "true" : "false", (unsigned)result, (unsigned long long)fence,
				(unsigned long long)requiredBytes, (unsigned long long)oldBytes,
				(unsigned long long)mResidentStaticBlasScratchBuffer.memorySize,
				(unsigned long long)mResidentBlasScratchRetirement.RetainedBytes(),
				(unsigned long long)mResidentBlasScratchRetirement.RetainedCount());
			PerfAuditWriteEvent("resident_blas_scratch_growth", PerfAuditCurrentToken(), fields.GetChars());
		}
		ReportResidentBlasScratchMemory();
	}
	return result;
}

void NRIRenderer::CollectResidentBlasScratch()
{
	if (mResidentBlasScratchRetirement.RetainedCount() != 0)
	{
		mResidentBlasScratchRetirement.Collect(
			[this](uint64_t fence) { return IsCommandFenceValueComplete(fence); },
			[this](NRIBufferResource& resource) {
				if (PerfAuditActive())
				{
					PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchReleases);
					PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchReleasedBytes, resource.memorySize);
				}
				DestroyBufferResource(resource);
			});
	}
	ReportResidentBlasScratchMemory();
}

void NRIRenderer::ReleaseResidentBlasScratchAfterWaitAndDiscard()
{
	mResidentBlasScratchRetirement.ReleaseAfterWaitAndDiscard([this](NRIBufferResource& resource) {
		if (PerfAuditActive())
		{
			PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchReleases);
			PerfAuditAdd(PerfAuditMetric::ResidentBlasScratchReleasedBytes, resource.memorySize);
		}
		DestroyBufferResource(resource);
	});
	ReportResidentBlasScratchMemory();
}
