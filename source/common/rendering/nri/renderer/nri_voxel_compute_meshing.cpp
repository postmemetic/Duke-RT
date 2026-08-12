#include "nri_voxel_compute_meshing.h"

#include "nri_voxel_compute_batch_plan.h"
#include "nri_voxel_compute_completion_ring.h"
#include "nri_voxel_compute_parallel_plan.h"
#include "nri_voxel_compute_raw_archive.h"
#include "nri_cvars.h"
#include "nri_renderer.h"
#include "nri_shader_contracts.h"
#include "nri_resources.h"
#include "../scene/nri_geometry_bridge.h"
#include "../system/nri_gpu_timing.h"
#include "../system/nri_renderdevice.h"
#include "common/models/model_kvx.h"
#include "printf.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr uint32_t RawArchiveSlabPageCapacity = 4u * 1024u * 1024u / sizeof(NRIVoxelComputeSlabRecord);
	constexpr uint32_t RawArchiveColorRunPageCapacity = 4u * 1024u * 1024u / sizeof(NRIVoxelComputeColorRunRecord);
	constexpr uint32_t RuntimeRawSourceScansPerFrame = 1;

	uint32_t AdjacencySurfacePrimitiveCount(const FVoxelRawMeshStats& stats)
	{
		return stats.adjacencySurfaceFaceCount * 2u;
	}

	double DurationMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	struct DirectPublishGeometrySummary
	{
		NRIVoxelComputeDirectPublishBounds bounds;
		float surfaceArea = 0.0f;
	};

	DirectPublishGeometrySummary BuildDirectPublishGeometrySummary(
		const FVoxelRawMeshStats& stats,
		const std::vector<NRIVoxelComputeSlabRecord>& slabs)
	{
		DirectPublishGeometrySummary summary = {};
		if (stats.sizeX == 0u || stats.sizeY == 0u || stats.sizeZ == 0u ||
			!std::isfinite(stats.pivotX) || !std::isfinite(stats.pivotY) || !std::isfinite(stats.pivotZ) ||
			slabs.empty())
		{
			return summary;
		}

		// Keep this in lockstep with TransformVoxelPoint in the emit shaders. The
		// raw archive already owns the occupied slabs, so publication can derive a
		// tight local bound and exact exposed unit-face area without geometry readback.
		for (const NRIVoxelComputeSlabRecord& slab : slabs)
		{
			if (slab.ZLength == 0u)
			{
				continue;
			}
			const float slabMin[3] = {
				(float)slab.X - stats.pivotX,
				stats.pivotZ - (float)(slab.ZTop + slab.ZLength),
				stats.pivotY - (float)(slab.Y + 1)
			};
			const float slabMax[3] = {
				(float)(slab.X + 1) - stats.pivotX,
				stats.pivotZ - (float)slab.ZTop,
				stats.pivotY - (float)slab.Y
			};
			if (!summary.bounds.valid)
			{
				for (uint32_t axis = 0; axis < 3; ++axis)
				{
					summary.bounds.min[axis] = slabMin[axis];
					summary.bounds.max[axis] = slabMax[axis];
				}
				summary.bounds.valid = true;
			}
			else
			{
				for (uint32_t axis = 0; axis < 3; ++axis)
				{
					summary.bounds.min[axis] = std::min(summary.bounds.min[axis], slabMin[axis]);
					summary.bounds.max[axis] = std::max(summary.bounds.max[axis], slabMax[axis]);
				}
			}

			const uint32_t topBottomFaceCount = ((slab.CullMask >> 4u) & 1u) + ((slab.CullMask >> 5u) & 1u);
			const uint32_t sideFaceCount =
				(slab.CullMask & 1u) + ((slab.CullMask >> 1u) & 1u) +
				((slab.CullMask >> 2u) & 1u) + ((slab.CullMask >> 3u) & 1u);
			summary.surfaceArea += (float)topBottomFaceCount + (float)(sideFaceCount * slab.ZLength);
		}
		if (!(summary.surfaceArea > 0.0f) || !std::isfinite(summary.surfaceArea))
		{
			summary = {};
		}
		return summary;
	}

	uint64_t HashRawArchiveValue(uint64_t hash, uint64_t value)
	{
		hash ^= value;
		hash *= 1099511628211ull;
		return hash;
	}

	uint64_t BuildRawArchiveContentHash(
		const FVoxelRawMeshStats& stats,
		const std::vector<NRIVoxelComputeSlabRecord>& slabs,
		const std::vector<NRIVoxelComputeColorRunRecord>& colorRuns)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashRawArchiveValue(hash, 0x5658524157483031ull); // VXRAWH01
		hash = HashRawArchiveValue(hash, stats.sizeX);
		hash = HashRawArchiveValue(hash, stats.sizeY);
		hash = HashRawArchiveValue(hash, stats.sizeZ);
		uint32_t pivotBits[3] = {};
		std::memcpy(&pivotBits[0], &stats.pivotX, sizeof(uint32_t));
		std::memcpy(&pivotBits[1], &stats.pivotY, sizeof(uint32_t));
		std::memcpy(&pivotBits[2], &stats.pivotZ, sizeof(uint32_t));
		for (uint32_t pivotBit : pivotBits)
		{
			hash = HashRawArchiveValue(hash, pivotBit);
		}
		for (const NRIVoxelComputeSlabRecord& slab : slabs)
		{
			hash = HashRawArchiveValue(hash, (uint32_t)slab.X);
			hash = HashRawArchiveValue(hash, (uint32_t)slab.Y);
			hash = HashRawArchiveValue(hash, slab.ZTop);
			hash = HashRawArchiveValue(hash, slab.ZLength);
			hash = HashRawArchiveValue(hash, slab.CullMask);
			hash = HashRawArchiveValue(hash, slab.ColorRunOffset);
			hash = HashRawArchiveValue(hash, slab.ColorRunCount);
		}
		for (const NRIVoxelComputeColorRunRecord& run : colorRuns)
		{
			hash = HashRawArchiveValue(hash, run.ZOffset);
			hash = HashRawArchiveValue(hash, run.ZLength);
			hash = HashRawArchiveValue(hash, run.Color);
		}
		return hash;
	}

	enum class VoxelComputeAdmissionState : uint32_t
	{
		Queued = 0,
		Counting,
		CountReady,
		Emitting,
		ReadyForBlas,
		BlasBuilding,
		BlasReady,
		Failed
	};

	struct PendingDirectPublishBinding
	{
		uint64_t meshResourceKey = 0;
		uint64_t directKey = 0;
		uint64_t generation = 0;
		uint64_t materialBindingKey = 0;
		uint32_t materialBase = 0;
		uint32_t materialCount = 0;
	};

	struct PendingVoxelComputeJob
	{
		FVoxelModel* model = nullptr;
		FVoxelRawMeshStats stats = {};
		uint64_t consumeKey = 0;
		uint64_t directMeshResourceKey = 0;
		uint64_t directKey = 0;
		uint64_t directGeneration = 0;
		uint64_t geometryKey = 0;
		uint64_t queuedFrame = 0;
		uint64_t reservationBytes = 0;
		uint64_t sourceArchiveSerial = 0;
		uint32_t cpuVertexCount = 0;
		uint32_t cpuIndexCount = 0;
		uint32_t jobId = 0;
		uint32_t materialBase = 0;
		uint32_t materialCount = 0;
		uint32_t priority = 0;
		uint64_t age = 0;
		bool oversizedExclusive = false;
		bool directPublication = false;
		NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::None;
		NRIVoxelComputeDirectPublishOutputBuffers outputBuffers;
		NRIVoxelComputeDirectPublishRange vertices;
		NRIVoxelComputeDirectPublishRange indices;
		NRIVoxelComputeDirectPublishRange primitives;
		std::vector<PendingDirectPublishBinding> directBindings;
		VoxelComputeAdmissionState admissionState = VoxelComputeAdmissionState::Queued;
		std::vector<NRIVoxelComputeSlabRecord> slabs;
		std::vector<NRIVoxelComputeFaceRecord> faces;
		std::vector<NRIVoxelComputeColorRunRecord> colorRuns;
	};

	struct RawVoxelSourceArchiveEntry
	{
		FVoxelRawMeshStats stats = {};
		std::vector<NRIVoxelComputeFaceRecord> faces;
		uint64_t recordSerial = 0;
		uint64_t recordedFrame = 0;
		uint32_t pageIndex = NRI_VOXEL_RAW_ARCHIVE_INVALID_PAGE;
		uint32_t slabOffset = 0;
		uint32_t slabCount = 0;
		uint32_t colorRunOffset = 0;
		uint32_t colorRunCount = 0;
		uint64_t slabBytes = 0;
		uint64_t faceBytes = 0;
		uint64_t colorRunBytes = 0;
		NRIVoxelComputeDirectPublishBounds bounds;
		float surfaceArea = 0.0f;
		bool uploadQueued = false;
		bool uploaded = false;
		bool failed = false;
		bool preloadRecorded = false;
	};

	struct RawVoxelSourceArchivePage
	{
		std::vector<NRIVoxelComputeSlabRecord> slabs;
		std::vector<NRIVoxelComputeColorRunRecord> colorRuns;
		NRIBufferResource slabUploadBuffer = {};
		NRIBufferResource colorRunUploadBuffer = {};
		NRIBufferResource slabBuffer = {};
		NRIBufferResource colorRunBuffer = {};
		uint32_t uploadedSlabCount = 0;
		uint32_t uploadedColorRunCount = 0;
		uint64_t uploadFenceValue = 0;
		bool uploadInFlight = false;
		bool failed = false;
	};

	struct PendingReadbackJob
	{
		uint32_t jobId = 0;
		uint32_t expectedFaces = 0;
		uint32_t expectedIndices = 0;
		uint32_t expectedVerticesNoDedupe = 0;
		uint32_t expectedVoxels = 0;
		uint32_t expectedPrimitives = 0;
		uint32_t cpuVertices = 0;
		uint32_t cpuIndices = 0;
		uint32_t vertexOffset = 0;
		uint32_t indexOffset = 0;
		uint32_t primitiveOffset = 0;
		uint64_t consumeKey = 0;
		uint64_t directMeshResourceKey = 0;
		uint64_t directKey = 0;
		uint64_t directGeneration = 0;
		uint64_t geometryKey = 0;
		uint64_t sourceArchiveSerial = 0;
		uint32_t materialBase = 0;
		uint32_t materialCount = 0;
		bool directPublication = false;
		NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::None;
		NRIVoxelComputeDirectPublishRange vertices;
		NRIVoxelComputeDirectPublishRange indices;
		NRIVoxelComputeDirectPublishRange primitives;
		NRIVoxelComputeDirectPublishBounds bounds;
		float surfaceArea = 0.0f;
		std::vector<PendingDirectPublishBinding> directBindings;
		VoxelComputeAdmissionState admissionState = VoxelComputeAdmissionState::Counting;
	};

	struct GeneratedVoxelGeometry
	{
		nri_scene::GeometryData geometry;
		uint32_t jobId = 0;
		uint32_t vertexHash = 0;
		uint32_t indexHash = 0;
		uint32_t primitiveHash = 0;
	};

	struct VoxelComputeCompletionSlot
	{
		std::vector<PendingReadbackJob> pendingJobs;
		uint64_t dispatchFrame = 0;
		uint64_t rawArchiveGeneration = 0;
		uint64_t statusBytes = 0;
		uint64_t fullGeometryBytes = 0;
		uint32_t pendingVertexCount = 0;
		uint32_t pendingIndexCount = 0;
		uint32_t pendingPrimitiveCount = 0;
		bool emit = false;
		bool buildBlas = false;
		bool fullGeneratedReadback = false;
		bool directProduction = false;
		const NRIBufferResource* outputVertexBuffer = nullptr;
		const NRIBufferResource* outputIndexBuffer = nullptr;
		const NRIBufferResource* outputPrimitiveBuffer = nullptr;
		nri::DescriptorSet* inputSet = nullptr;
		nri::DescriptorSet* outputSet = nullptr;

		NRIBufferResource jobUploadBuffer = {};
		NRIBufferResource slabUploadBuffer = {};
		NRIBufferResource faceUploadBuffer = {};
		NRIBufferResource colorRunUploadBuffer = {};
		NRIBufferResource jobBuffer = {};
		NRIBufferResource slabBuffer = {};
		NRIBufferResource faceBuffer = {};
		NRIBufferResource colorRunBuffer = {};
		NRIBufferResource resultBuffer = {};
		NRIBufferResource vertexBuffer = {};
		NRIBufferResource indexBuffer = {};
		NRIBufferResource primitiveBuffer = {};
		NRIBufferResource slabScratchBuffer = {};
		NRIBufferResource readbackBuffer = {};
		NRIBufferResource vertexReadbackBuffer = {};
		NRIBufferResource indexReadbackBuffer = {};
		NRIBufferResource primitiveReadbackBuffer = {};
	};

	struct VoxelComputeState
	{
		std::vector<PendingVoxelComputeJob> queuedJobs;
		uint32_t nextJobId = 1;
		uint32_t diagnosticBlasBuildsSubmitted = 0;
		NRIVoxelComputeCompletionRingState completionRing;
		std::array<VoxelComputeCompletionSlot, NRI_VOXEL_COMPUTE_COMPLETION_SLOT_COUNT> completionSlots;
		uint64_t completionPolls = 0;
		uint64_t completionHostWaits = 0;
		uint64_t rawSourceArchiveHits = 0;
		uint64_t rawSourceArchiveMisses = 0;
		uint64_t rawSourceArchiveRecords = 0;
		uint64_t rawSourceArchiveUploadBytes = 0;
		uint64_t rawSourceArchivePreloadUploadBytes = 0;
		uint64_t rawSourceArchiveRuntimeUploadBytes = 0;
		uint64_t rawSourceArchiveUploadFailures = 0;
		uint64_t rawSourceIngestScans = 0;
		uint64_t rawSourceIngestScanFailures = 0;
		uint64_t rawSourceWholeArchiveReuploads = 0;
		uint64_t rawSourceUploadLatencyFrames = 0;
		uint64_t rawSourceUploadLatencySamples = 0;
		uint64_t rawSourceUploadLatencyMaxFrames = 0;
		double rawSourceIngestScanMs = 0.0;
		double rawSourceIngestScanMaxMs = 0.0;
		uint64_t lastObservedFrame = 0;
		uint64_t totalStatusReadbackBytes = 0;
		uint64_t totalFullGeometryReadbackBytes = 0;
		uint64_t directBatchInputRequests = 0;
		uint64_t directBatchUniqueJobs = 0;
		uint64_t directBatchDedupeHits = 0;
		uint64_t directBatchMaterialBindings = 0;
		uint64_t directBatchRawStatScansAvoided = 0;
		uint64_t directBatchDispatches = 0;
		uint64_t directBatchJobsDispatched = 0;
		uint64_t directBatchReservationBytes = 0;
		uint64_t directBatchOversizedExclusive = 0;
		uint64_t diagnosticSidecarsSuppressed = 0;
		std::unordered_set<uint64_t> queuedConsumeKeys;
		std::unordered_set<uint64_t> pendingConsumeKeys;
		std::unordered_set<uint64_t> failedConsumeKeys;
		std::unordered_set<uint64_t> queuedDirectKeys;
		std::unordered_set<uint64_t> pendingDirectKeys;
		std::unordered_set<uint64_t> cancelledDirectKeys;
		std::unordered_set<uint64_t> failedDirectKeys;
		std::unordered_map<uint64_t, GeneratedVoxelGeometry> readyGeneratedGeometry;
		std::unordered_map<uint64_t, NRIVoxelComputeDirectPublishedMesh> readyDirectPublishedMeshes;
		std::unordered_map<FVoxelModel*, RawVoxelSourceArchiveEntry> rawSourceArchive;
		NRIVoxelComputeRawArchivePlan rawArchivePlan = {
			RawArchiveSlabPageCapacity,
			RawArchiveColorRunPageCapacity
		};
		std::vector<RawVoxelSourceArchivePage> rawArchivePages;
		NRIAccelerationStructureResource diagnosticBlas = {};
	};

	VoxelComputeState gVoxelComputeState;

	struct CompletionSlotRecordingGuard
	{
		NRIVoxelComputeCompletionRingState* ring = nullptr;
		uint32_t slotIndex = 0;
		bool submitted = false;
		uint64_t frameNumber = 0;
		const char* abortReason = "recording-incomplete";
		NRIVoxelComputeCompletionOutcome abortOutcome = NRIVoxelComputeCompletionOutcome::Abandoned;

		~CompletionSlotRecordingGuard()
		{
			if (ring != nullptr && !submitted)
			{
				const NRIVoxelComputeCompletionSlotToken token = ring->slots[slotIndex];
				ring->Release(slotIndex, abortOutcome);
				if ((int)nri_ptvoxelcomputetrace > 0)
				{
					Printf(
						"PERF pt voxel compute completion NRI: action=recording-abort submission=%llu slot=%u level_generation=%llu frame=%llu reason=%s occupancy=%u high_water=%u abandoned_attempts=%llu failed_attempts=%llu host_waits=0\n",
						(unsigned long long)token.submissionId, slotIndex,
						(unsigned long long)token.levelGeneration, (unsigned long long)frameNumber,
						abortReason != nullptr ? abortReason : "unknown", ring->occupancy, ring->highWater,
						(unsigned long long)ring->abandonedCount,
						(unsigned long long)ring->failedCount);
				}
			}
		}
	};

	bool IsTraceEnabled()
	{
		return (int)nri_ptvoxelcomputetrace > 0;
	}

	bool IsEmitEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 3;
	}

	NRIVoxelComputeAlgorithm SelectedComputeAlgorithm()
	{
		return (int)nri_ptvoxelcomputealgorithm > 0 ?
			NRIVoxelComputeAlgorithm::ParallelVoxelAdjacencyCoalescedV3 :
			NRIVoxelComputeAlgorithm::SerialV1;
	}

	bool IsBlasEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 5;
	}

	bool IsConsumptionEnabled()
	{
		return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 6;
	}

	bool IsDirectGpuPublicationEnabled()
	{
		return (bool)nri_ptvoxelcompute && (bool)nri_ptvoxelcomputedirectgpu;
	}

	bool IsDirectPublicationEnabled()
	{
		return (bool)nri_ptvoxelcompute && (bool)nri_ptvoxelcomputedirectpublish;
	}

	bool IsRawSourceArchiveEnabled()
	{
		return (bool)nri_ptvoxelcompute && (bool)nri_ptvoxelcomputerawarchive;
	}

	bool IsFullGeneratedReadbackEnabled()
	{
		return (bool)nri_ptvoxelcomputevalidatefullreadback;
	}

	const char* AdmissionStateName(VoxelComputeAdmissionState state)
	{
		switch (state)
		{
		case VoxelComputeAdmissionState::Queued: return "queued";
		case VoxelComputeAdmissionState::Counting: return "counting";
		case VoxelComputeAdmissionState::CountReady: return "count_ready";
		case VoxelComputeAdmissionState::Emitting: return "emitting";
		case VoxelComputeAdmissionState::ReadyForBlas: return "ready_for_blas";
		case VoxelComputeAdmissionState::BlasBuilding: return "blas_building";
		case VoxelComputeAdmissionState::BlasReady: return "blas_ready";
		case VoxelComputeAdmissionState::Failed: return "failed";
		default: return "unknown";
		}
	}

	const char* DirectPublishOutputKindName(NRIVoxelComputeDirectPublishOutputKind kind)
	{
		switch (kind)
		{
		case NRIVoxelComputeDirectPublishOutputKind::None: return "none";
		case NRIVoxelComputeDirectPublishOutputKind::SharedPersistentArena: return "shared_persistent_arena";
		case NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena: return "private_blas_inputs_and_shared_arena";
		case NRIVoxelComputeDirectPublishOutputKind::PrivateBuffers: return "private_buffers";
		default: return "unknown";
		}
	}

	uint64_t BuildDirectPublishKey(uint64_t meshResourceKey, uint64_t generation)
	{
		uint64_t hash = meshResourceKey;
		hash ^= generation + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return hash;
	}

	bool DirectPublishBufferReady(const NRIBufferResource* resource, nri::BufferUsageBits usage)
	{
		return
			resource != nullptr &&
			resource->buffer != nullptr &&
			resource->shaderView != nullptr &&
			resource->storageView != nullptr &&
			NRIResourceUsageIncludes(resource->usage, usage);
	}

	void CopySlabRecords(const TArray<FVoxelRawSlabRecord>& source, std::vector<NRIVoxelComputeSlabRecord>& target)
	{
		target.clear();
		target.reserve((size_t)source.Size());
		for (unsigned int i = 0; i < source.Size(); ++i)
		{
			const FVoxelRawSlabRecord& slab = source[i];
			NRIVoxelComputeSlabRecord record = {};
			record.X = slab.x;
			record.Y = slab.y;
			record.ZTop = slab.zTop;
			record.CullMask = slab.cullMask;
			record.ZLength = slab.zLength;
			record.ColorRunCount = slab.colorRunCount;
			record.ColorRunOffset = slab.colorRunOffset;
			target.push_back(record);
		}
	}

	void CopyColorRunRecords(const TArray<FVoxelRawColorRunRecord>& source, std::vector<NRIVoxelComputeColorRunRecord>& target)
	{
		target.clear();
		target.reserve((size_t)source.Size());
		for (unsigned int i = 0; i < source.Size(); ++i)
		{
			const FVoxelRawColorRunRecord& run = source[i];
			NRIVoxelComputeColorRunRecord record = {};
			record.ZOffset = run.zOffset;
			record.ZLength = run.zLength;
			record.Color = run.color;
			target.push_back(record);
		}
	}

	void CopyFaceRecords(const TArray<FVoxelRawFaceRecord>& source, std::vector<NRIVoxelComputeFaceRecord>& target)
	{
		target.clear();
		target.reserve((size_t)source.Size());
		for (unsigned int i = 0; i < source.Size(); ++i)
		{
			const FVoxelRawFaceRecord& face = source[i];
			NRIVoxelComputeFaceRecord record = {};
			for (uint32_t v = 0; v < 4; ++v)
			{
				record.X[v] = face.x[v];
				record.Y[v] = face.y[v];
				record.Z[v] = face.z[v];
			}
			record.Color = face.color;
			record.MaterialIndex = face.materialIndex;
			target.push_back(record);
		}
	}

	uint64_t RawSourceKey(FVoxelModel* model)
	{
		return (uint64_t)(uintptr_t)model;
	}

	RawVoxelSourceArchiveEntry* RecordRawSourceArchive(
		FVoxelModel* model,
		const FVoxelRawMeshStats& stats,
		const TArray<FVoxelRawSlabRecord>& slabs,
		const TArray<FVoxelRawFaceRecord>* faces,
		const TArray<FVoxelRawColorRunRecord>* colorRuns,
		bool preloadRecorded = false,
		uint64_t frameNumber = 0)
	{
		if (!IsRawSourceArchiveEnabled() || model == nullptr || stats.slabCount == 0 || slabs.Size() != stats.slabCount ||
			colorRuns == nullptr || colorRuns->Size() == 0)
		{
			return nullptr;
		}

		VoxelComputeState& state = gVoxelComputeState;
		auto found = state.rawSourceArchive.find(model);
		if (found != state.rawSourceArchive.end())
		{
			state.rawSourceArchiveHits++;
			found->second.preloadRecorded = found->second.preloadRecorded || preloadRecorded;
			return &found->second;
		}

		RawVoxelSourceArchiveEntry entry = {};
		entry.stats = stats;
		std::vector<NRIVoxelComputeSlabRecord> packedSlabs;
		std::vector<NRIVoxelComputeColorRunRecord> packedColorRuns;
		CopySlabRecords(slabs, packedSlabs);
		CopyColorRunRecords(*colorRuns, packedColorRuns);
		entry.stats.contentHash = BuildRawArchiveContentHash(entry.stats, packedSlabs, packedColorRuns);
		const DirectPublishGeometrySummary geometrySummary = BuildDirectPublishGeometrySummary(entry.stats, packedSlabs);
		entry.bounds = geometrySummary.bounds;
		entry.surfaceArea = geometrySummary.surfaceArea;
		NRIVoxelComputeRawArchiveSourceRange range = {};
		if (!state.rawArchivePlan.CommitSource(
			RawSourceKey(model),
			(uint32_t)packedSlabs.size(),
			(uint32_t)packedColorRuns.size(),
			preloadRecorded,
			range))
		{
			return nullptr;
		}
		entry.recordSerial = range.serial;
		entry.recordedFrame = frameNumber;
		entry.pageIndex = range.pageIndex;
		entry.slabOffset = range.slabOffset;
		entry.slabCount = range.slabCount;
		entry.colorRunOffset = range.colorRunOffset;
		entry.colorRunCount = range.colorRunCount;
		while (state.rawArchivePages.size() < state.rawArchivePlan.GetPageCount())
		{
			const uint32_t pageIndex = (uint32_t)state.rawArchivePages.size();
			const NRIVoxelComputeRawArchivePagePlan* pagePlan = state.rawArchivePlan.GetPage(pageIndex);
			if (pagePlan == nullptr)
			{
				return nullptr;
			}
			state.rawArchivePages.emplace_back();
			state.rawArchivePages.back().slabs.reserve(pagePlan->slabCapacity);
			state.rawArchivePages.back().colorRuns.reserve(pagePlan->colorRunCapacity);
		}
		RawVoxelSourceArchivePage& page = state.rawArchivePages[entry.pageIndex];
		if (page.slabs.size() != entry.slabOffset || page.colorRuns.size() != entry.colorRunOffset)
		{
			state.rawArchivePlan.FailSource(RawSourceKey(model));
			return nullptr;
		}
		for (NRIVoxelComputeSlabRecord& slab : packedSlabs)
		{
			slab.ColorRunOffset += entry.colorRunOffset;
		}
		page.slabs.insert(page.slabs.end(), packedSlabs.begin(), packedSlabs.end());
		page.colorRuns.insert(page.colorRuns.end(), packedColorRuns.begin(), packedColorRuns.end());
		if (faces != nullptr)
		{
			CopyFaceRecords(*faces, entry.faces);
		}
		entry.slabBytes = (uint64_t)entry.slabCount * sizeof(NRIVoxelComputeSlabRecord);
		entry.faceBytes = (uint64_t)entry.faces.size() * sizeof(NRIVoxelComputeFaceRecord);
		entry.colorRunBytes = (uint64_t)entry.colorRunCount * sizeof(NRIVoxelComputeColorRunRecord);
		entry.uploadQueued = true;
		entry.preloadRecorded = range.preloadRecorded;
		state.rawSourceArchiveMisses++;
		state.rawSourceArchiveRecords++;
		auto inserted = state.rawSourceArchive.emplace(model, std::move(entry));
		RawVoxelSourceArchiveEntry& archived = inserted.first->second;
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel raw source archive NRI: event=record phase=%s model=%p serial=%llu page=%u slab_offset=%u color_run_offset=%u raw_bytes=%llu slab_upload_bytes=%llu color_run_bytes=%llu transient_face_bytes=%llu slabs=%u color_runs=%u faces=%u adjacency_faces=%u unit_faces=%u voxels=%u\n",
				preloadRecorded ? "preload" : "runtime",
				(void*)model,
				(unsigned long long)archived.recordSerial,
				archived.pageIndex,
				archived.slabOffset,
				archived.colorRunOffset,
				(unsigned long long)archived.stats.rawByteCount,
				(unsigned long long)archived.slabBytes,
				(unsigned long long)archived.colorRunBytes,
				(unsigned long long)archived.faceBytes,
				archived.slabCount,
				archived.colorRunCount,
				archived.stats.coalescedFaceCount,
				archived.stats.adjacencySurfaceFaceCount,
				archived.stats.unitSurfaceFaceCount,
				archived.stats.voxelCount);
		}
		return &archived;
	}

	RawVoxelSourceArchivePage* FindRawArchivePage(const RawVoxelSourceArchiveEntry& entry)
	{
		VoxelComputeState& state = gVoxelComputeState;
		return entry.pageIndex < state.rawArchivePages.size() ? &state.rawArchivePages[entry.pageIndex] : nullptr;
	}

	RawVoxelSourceArchiveEntry* FindUploadedRawSourceArchive(FVoxelModel* model)
	{
		if (model == nullptr)
		{
			return nullptr;
		}
		VoxelComputeState& state = gVoxelComputeState;
		auto found = state.rawSourceArchive.find(model);
		if (found == state.rawSourceArchive.end() || !found->second.uploaded || found->second.failed)
		{
			return nullptr;
		}
		RawVoxelSourceArchivePage* page = FindRawArchivePage(found->second);
		if (page == nullptr || page->slabBuffer.shaderView == nullptr || page->colorRunBuffer.shaderView == nullptr ||
			found->second.slabOffset + found->second.slabCount > page->uploadedSlabCount ||
			found->second.colorRunOffset + found->second.colorRunCount > page->uploadedColorRunCount)
		{
			return nullptr;
		}
		state.rawSourceArchiveHits++;
		return &found->second;
	}

	void CopyRawArchiveRecords(
		const RawVoxelSourceArchiveEntry& archive,
		std::vector<NRIVoxelComputeSlabRecord>& outSlabs,
		std::vector<NRIVoxelComputeColorRunRecord>& outColorRuns)
	{
		const VoxelComputeState& state = gVoxelComputeState;
		if (archive.pageIndex >= state.rawArchivePages.size())
		{
			outSlabs.clear();
			outColorRuns.clear();
			return;
		}
		const RawVoxelSourceArchivePage& page = state.rawArchivePages[archive.pageIndex];
		outSlabs.assign(
			page.slabs.begin() + archive.slabOffset,
			page.slabs.begin() + archive.slabOffset + archive.slabCount);
		for (NRIVoxelComputeSlabRecord& slab : outSlabs)
		{
			slab.ColorRunOffset -= archive.colorRunOffset;
		}
		outColorRuns.assign(
			page.colorRuns.begin() + archive.colorRunOffset,
			page.colorRuns.begin() + archive.colorRunOffset + archive.colorRunCount);
	}

	bool CreateBuffer(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation,
		nri::BufferView viewType,
		bool createView)
	{
		const NRIResourceContext& context = services.context;
		if (context.device == nullptr || context.core == nullptr || size == 0)
		{
			return false;
		}

		services.DestroyBufferResource(resource);
		nri::BufferDesc desc = {};
		desc.size = size;
		desc.structureStride = stride;
		desc.usage = usage;
		if (context.core->CreateCommittedBuffer(*context.device, memoryLocation, 0.0f, desc, resource.buffer) != nri::Result::SUCCESS)
		{
			return false;
		}

		nri::MemoryDesc memoryDesc = {};
		context.core->GetBufferMemoryDesc(*resource.buffer, memoryLocation, memoryDesc);
		resource.size = desc.size;
		resource.memorySize = memoryDesc.size;
		resource.memoryLocation = memoryLocation;
		resource.usedSize = size;
		resource.stride = stride;
		resource.usage = usage;

		if (createView)
		{
			nri::BufferViewDesc viewDesc = {};
			viewDesc.buffer = resource.buffer;
			viewDesc.type = viewType;
			viewDesc.offset = 0;
			viewDesc.size = nri::WHOLE_SIZE;
			viewDesc.structureStride = stride;
			if (context.core->CreateBufferView(viewDesc, resource.shaderView) != nri::Result::SUCCESS)
			{
				services.DestroyBufferResource(resource);
				return false;
			}
		}

		return true;
	}

	bool EnsureBuffer(
		const NRIResourceServices& services,
		NRIBufferResource& resource,
		uint64_t size,
		uint32_t stride,
		nri::BufferUsageBits usage,
		nri::MemoryLocation memoryLocation,
		nri::BufferView viewType,
		bool createView)
	{
		if (resource.buffer != nullptr &&
			resource.size >= size &&
			resource.stride == stride &&
			resource.memoryLocation == memoryLocation &&
			NRIResourceUsageIncludes(resource.usage, usage))
		{
			return true;
		}
		return CreateBuffer(services, resource, size, stride, usage, memoryLocation, viewType, createView);
	}

	bool CopyToUploadBuffer(const NRIResourceContext& context, NRIBufferResource& upload, const void* data, uint64_t size)
	{
		void* mapped = context.core->MapBuffer(*upload.buffer, 0, size);
		if (mapped == nullptr)
		{
			return false;
		}
		std::memcpy(mapped, data, (size_t)size);
		context.core->UnmapBuffer(*upload.buffer);
		return true;
	}

	bool ImportGeneratedGeometry(
		const PendingReadbackJob& job,
		const NRIVoxelComputeResult& result,
		const NRIVoxelComputeSceneVertex* vertices,
		const uint32_t* indices,
		const NRIVoxelComputePrimitiveData* primitives,
		GeneratedVoxelGeometry& outGenerated)
	{
		if (vertices == nullptr || indices == nullptr || primitives == nullptr ||
			result.VertexCountNoDedupe != job.expectedVerticesNoDedupe ||
			result.IndexCount != job.expectedIndices ||
			result.PrimitiveCount != job.expectedPrimitives)
		{
			return false;
		}

		nri_scene::GeometryData geometry = {};
		geometry.vertices.resize(result.VertexCountNoDedupe);
		geometry.indices.resize(result.IndexCount);
		geometry.primitives.resize(result.PrimitiveCount);

		for (uint32_t i = 0; i < result.VertexCountNoDedupe; ++i)
		{
			const NRIVoxelComputeSceneVertex& source = vertices[(uint64_t)job.vertexOffset + i];
			nri_scene::SceneVertex& target = geometry.vertices[i];
			target.position[0] = source.Position[0];
			target.position[1] = source.Position[1];
			target.position[2] = source.Position[2];
			target.prevPosition[0] = source.PrevPosition[0];
			target.prevPosition[1] = source.PrevPosition[1];
			target.prevPosition[2] = source.PrevPosition[2];
			target.uv[0] = source.Uv[0];
			target.uv[1] = source.Uv[1];
		}

		for (uint32_t i = 0; i < result.IndexCount; ++i)
		{
			const uint32_t localIndex = indices[(uint64_t)job.indexOffset + i];
			if (localIndex >= result.VertexCountNoDedupe)
			{
				return false;
			}
			geometry.indices[i] = localIndex;
		}

		for (uint32_t i = 0; i < result.PrimitiveCount; ++i)
		{
			const NRIVoxelComputePrimitiveData& source = primitives[(uint64_t)job.primitiveOffset + i];
			nri_scene::PrimitiveData& target = geometry.primitives[i];
			for (uint32_t vertex = 0; vertex < 3; ++vertex)
			{
				if (source.Indices[vertex] < job.vertexOffset || source.Indices[vertex] >= job.vertexOffset + result.VertexCountNoDedupe)
				{
					return false;
				}
				target.indices[vertex] = source.Indices[vertex] - job.vertexOffset;
			}
			target.materialIndex = source.MaterialIndex;
			target.uv0[0] = source.Uv0[0];
			target.uv0[1] = source.Uv0[1];
			target.uv1[0] = source.Uv1[0];
			target.uv1[1] = source.Uv1[1];
			target.uv2[0] = source.Uv2[0];
			target.uv2[1] = source.Uv2[1];
			target.normal[0] = source.Normal[0];
			target.normal[1] = source.Normal[1];
			target.normal[2] = source.Normal[2];
			target.flags = source.Flags;
			target.portalIndex = source.PortalIndex;
			target.reserved0 = source.Reserved0;
			target.smoothNormals[0] = source.SmoothNormals[0];
			target.smoothNormals[1] = source.SmoothNormals[1];
		}

		outGenerated.geometry = std::move(geometry);
		outGenerated.jobId = job.jobId;
		outGenerated.vertexHash = result.VertexHash;
		outGenerated.indexHash = result.IndexHash;
		outGenerated.primitiveHash = result.PrimitiveHash;
		return true;
	}

	void TraceAdmission(uint64_t frameNumber, const PendingReadbackJob& job, VoxelComputeAdmissionState state, const char* reason)
	{
		if (!IsTraceEnabled())
		{
			return;
		}

		Printf(
			"PERF pt voxel compute admission NRI: frame=%llu job=%u consume_key=0x%llx state=%s reason=%s faces=%u vertices=%u indices=%u primitives=%u\n",
			(unsigned long long)frameNumber,
			job.jobId,
			(unsigned long long)job.consumeKey,
			AdmissionStateName(state),
			reason != nullptr ? reason : "unknown",
			job.expectedFaces,
			job.expectedVerticesNoDedupe,
			job.expectedIndices,
			job.expectedPrimitives);
	}

	NRIVoxelComputeRawSourceQueueResult QueueRuntimeRawSource(FVoxelModel* model, uint64_t frameNumber)
	{
		if (!IsRawSourceArchiveEnabled())
		{
			return NRIVoxelComputeRawSourceQueueResult::Invalid;
		}
		VoxelComputeState& state = gVoxelComputeState;
		const uint32_t maxPending = (uint32_t)std::max(0, (int)nri_ptvoxelcomputemaxjobs);
		const NRIVoxelComputeRawSourceQueueResult result = state.rawArchivePlan.QueueSource(
			RawSourceKey(model), frameNumber, maxPending);
		if (IsTraceEnabled() && result == NRIVoxelComputeRawSourceQueueResult::Queued)
		{
			const NRIVoxelComputeRawArchivePlanStats& stats = state.rawArchivePlan.GetStats();
			Printf(
				"PERF pt voxel raw source ingest NRI: event=queue model=%p source_pending=%u queued=%llu dedupe=%llu scans=%llu failures=%llu frame=%llu\n",
				(void*)model,
				stats.pendingSources,
				(unsigned long long)stats.queuedSources,
				(unsigned long long)stats.dedupeHits,
				(unsigned long long)stats.scansStarted,
				(unsigned long long)stats.failedSources,
				(unsigned long long)frameNumber);
		}
		return result;
	}

	void PumpPendingRawSources(uint64_t frameNumber)
	{
		VoxelComputeState& state = gVoxelComputeState;
		const std::vector<NRIVoxelComputeRawSourcePendingJob> jobs =
			state.rawArchivePlan.TakePendingSources(RuntimeRawSourceScansPerFrame);
		for (const NRIVoxelComputeRawSourcePendingJob& job : jobs)
		{
			FVoxelModel* model = reinterpret_cast<FVoxelModel*>((uintptr_t)job.sourceKey);
			FVoxelRawMeshStats rawStats = {};
			TArray<FVoxelRawSlabRecord> rawSlabs;
			TArray<FVoxelRawColorRunRecord> rawColorRuns;
			const auto start = std::chrono::steady_clock::now();
			model->BuildRawMeshStats(rawStats, &rawSlabs, nullptr, &rawColorRuns);
			const double scanMs = DurationMs(start, std::chrono::steady_clock::now());
			state.rawSourceIngestScans++;
			state.rawSourceIngestScanMs += scanMs;
			state.rawSourceIngestScanMaxMs = std::max(state.rawSourceIngestScanMaxMs, scanMs);
			RawVoxelSourceArchiveEntry* archived = RecordRawSourceArchive(
				model, rawStats, rawSlabs, nullptr, &rawColorRuns, false, frameNumber);
			if (archived == nullptr)
			{
				state.rawArchivePlan.FailSource(job.sourceKey);
				state.rawSourceIngestScanFailures++;
			}
			if (IsTraceEnabled())
			{
				const NRIVoxelComputeRawArchivePlanStats& stats = state.rawArchivePlan.GetStats();
				Printf(
					"PERF pt voxel raw source ingest NRI: event=scan model=%p outcome=%s source_pending=%u scans=%llu failures=%llu scan_ms=%.3f total_scan_ms=%.3f max_scan_ms=%.3f queue_latency_frames=%llu frame=%llu\n",
					(void*)model,
					archived != nullptr ? "recorded" : "failed",
					stats.pendingSources,
					(unsigned long long)state.rawSourceIngestScans,
					(unsigned long long)state.rawSourceIngestScanFailures,
					scanMs,
					state.rawSourceIngestScanMs,
					state.rawSourceIngestScanMaxMs,
					(unsigned long long)(frameNumber >= job.queuedFrame ? frameNumber - job.queuedFrame : 0u),
					(unsigned long long)frameNumber);
			}
		}
	}

	void CompleteRawArchiveUploads(NRIRenderer& renderer, const NRIResourceServices& services)
	{
		VoxelComputeState& state = gVoxelComputeState;
		for (RawVoxelSourceArchivePage& page : state.rawArchivePages)
		{
			if (!page.uploadInFlight || !renderer.IsCommandFenceValueComplete(page.uploadFenceValue))
			{
				continue;
			}
			services.DestroyBufferResource(page.slabUploadBuffer);
			services.DestroyBufferResource(page.colorRunUploadBuffer);
			page.uploadInFlight = false;
			page.uploadFenceValue = 0;
		}
	}

	void UploadPendingRawSourcePage(
		NRIRenderer& renderer,
		NRIRenderDevice* timingDevice,
		const NRIResourceServices& services,
		uint64_t frameNumber,
		uint32_t pageIndex)
	{
		VoxelComputeState& state = gVoxelComputeState;
		if (!IsRawSourceArchiveEnabled() || state.rawSourceArchive.empty() || services.context.core == nullptr ||
			services.context.commandBuffer == nullptr)
		{
			return;
		}
		const uint64_t recordingFenceValue = renderer.GetRecordingCommandFenceValue();
		if (recordingFenceValue == 0)
		{
			return;
		}

		if (pageIndex >= state.rawArchivePages.size())
		{
			return;
		}

		RawVoxelSourceArchivePage& page = state.rawArchivePages[pageIndex];
		if (page.failed || page.uploadInFlight ||
			(page.uploadedSlabCount == page.slabs.size() && page.uploadedColorRunCount == page.colorRuns.size()))
		{
			return;
		}
		const NRIVoxelComputeRawArchivePagePlan* pagePlan = state.rawArchivePlan.GetPage(pageIndex);
		if (pagePlan == nullptr)
		{
			return;
		}
		const uint32_t totalSlabCount = (uint32_t)page.slabs.size();
		const uint32_t totalColorRunCount = (uint32_t)page.colorRuns.size();
		const uint32_t firstSlab = page.uploadedSlabCount;
		const uint32_t firstColorRun = page.uploadedColorRunCount;
		const uint32_t uploadSlabCount = totalSlabCount - firstSlab;
		const uint32_t uploadColorRunCount = totalColorRunCount - firstColorRun;
		const uint64_t uploadSlabBytes = (uint64_t)uploadSlabCount * sizeof(NRIVoxelComputeSlabRecord);
		const uint64_t uploadColorRunBytes = (uint64_t)uploadColorRunCount * sizeof(NRIVoxelComputeColorRunRecord);

		const bool ok =
			(page.slabBuffer.buffer != nullptr || CreateBuffer(
				services,
				page.slabBuffer,
				(uint64_t)pagePlan->slabCapacity * sizeof(NRIVoxelComputeSlabRecord),
				sizeof(NRIVoxelComputeSlabRecord),
				nri::BufferUsageBits::SHADER_RESOURCE,
				nri::MemoryLocation::DEVICE,
				nri::BufferView::STRUCTURED_BUFFER,
				true)) &&
			(page.colorRunBuffer.buffer != nullptr || CreateBuffer(
				services,
				page.colorRunBuffer,
				(uint64_t)pagePlan->colorRunCapacity * sizeof(NRIVoxelComputeColorRunRecord),
				sizeof(NRIVoxelComputeColorRunRecord),
				nri::BufferUsageBits::SHADER_RESOURCE,
				nri::MemoryLocation::DEVICE,
				nri::BufferView::STRUCTURED_BUFFER,
				true)) &&
			EnsureBuffer(services, page.slabUploadBuffer, uploadSlabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) &&
			EnsureBuffer(services, page.colorRunUploadBuffer, uploadColorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) &&
			CopyToUploadBuffer(services.context, page.slabUploadBuffer, page.slabs.data() + firstSlab, uploadSlabBytes) &&
			CopyToUploadBuffer(services.context, page.colorRunUploadBuffer, page.colorRuns.data() + firstColorRun, uploadColorRunBytes);

		if (!ok)
		{
			page.failed = true;
			for (auto& archivePair : state.rawSourceArchive)
			{
				if (archivePair.second.pageIndex == pageIndex)
				{
					archivePair.second.failed = true;
					archivePair.second.uploaded = false;
					archivePair.second.uploadQueued = false;
				}
			}
			state.rawSourceArchiveUploadFailures++;
			return;
		}
		NRIScopedGpuTiming admissionGpuTiming(timingDevice, NRIGpuTimingScope::VoxelAdmission);
		NRIScopedGpuTiming uploadGpuTiming(timingDevice, NRIGpuTimingScope::VoxelUpload);

		nri::BufferBarrierDesc uploadBarriers[4] = {};
		uploadBarriers[0].buffer = page.slabUploadBuffer.buffer;
		uploadBarriers[0].after = NRIResourceCopySourceAccess();
		uploadBarriers[1].buffer = page.slabBuffer.buffer;
		uploadBarriers[1].before = firstSlab == 0 ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess();
		uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
		uploadBarriers[2].buffer = page.colorRunUploadBuffer.buffer;
		uploadBarriers[2].after = NRIResourceCopySourceAccess();
		uploadBarriers[3].buffer = page.colorRunBuffer.buffer;
		uploadBarriers[3].before = firstColorRun == 0 ? nri::AccessStage{} : NRIResourceComputeShaderResourceAccess();
		uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
		nri::BarrierDesc uploadBarrier = {};
		uploadBarrier.buffers = uploadBarriers;
		uploadBarrier.bufferNum = 4;
		services.context.core->CmdBarrier(*services.context.commandBuffer, uploadBarrier);
		services.context.core->CmdCopyBuffer(
			*services.context.commandBuffer,
			*page.slabBuffer.buffer,
			(uint64_t)firstSlab * sizeof(NRIVoxelComputeSlabRecord),
			*page.slabUploadBuffer.buffer,
			0,
			uploadSlabBytes);
		services.context.core->CmdCopyBuffer(
			*services.context.commandBuffer,
			*page.colorRunBuffer.buffer,
			(uint64_t)firstColorRun * sizeof(NRIVoxelComputeColorRunRecord),
			*page.colorRunUploadBuffer.buffer,
			0,
			uploadColorRunBytes);

		nri::BufferBarrierDesc shaderBarriers[2] = {};
		shaderBarriers[0].buffer = page.slabBuffer.buffer;
		shaderBarriers[0].before = NRIResourceCopyDestinationAccess();
		shaderBarriers[0].after = NRIResourceComputeShaderResourceAccess();
		shaderBarriers[1].buffer = page.colorRunBuffer.buffer;
		shaderBarriers[1].before = NRIResourceCopyDestinationAccess();
		shaderBarriers[1].after = NRIResourceComputeShaderResourceAccess();
		nri::BarrierDesc shaderBarrier = {};
		shaderBarrier.buffers = shaderBarriers;
		shaderBarrier.bufferNum = 2;
		services.context.core->CmdBarrier(*services.context.commandBuffer, shaderBarrier);

		uint32_t uploads = 0;
		uint32_t preloadUploads = 0;
		uint32_t runtimeUploads = 0;
		uint64_t preloadBytes = 0;
		uint64_t runtimeBytes = 0;
		for (auto& archivePair : state.rawSourceArchive)
		{
			RawVoxelSourceArchiveEntry& entry = archivePair.second;
			if (entry.pageIndex != pageIndex)
			{
				continue;
			}
			const bool wasUploaded = entry.uploaded;
			entry.uploaded = !entry.failed &&
				entry.slabOffset + entry.slabCount <= totalSlabCount &&
				entry.colorRunOffset + entry.colorRunCount <= totalColorRunCount;
			entry.uploadQueued = !entry.uploaded;
			if (!wasUploaded && entry.uploaded)
			{
				uploads++;
				if (entry.preloadRecorded)
				{
					preloadUploads++;
					preloadBytes += entry.slabBytes + entry.colorRunBytes;
				}
				else
				{
					const uint64_t latencyFrames = frameNumber >= entry.recordedFrame ? frameNumber - entry.recordedFrame : 0u;
					state.rawSourceUploadLatencyFrames += latencyFrames;
					state.rawSourceUploadLatencySamples++;
					state.rawSourceUploadLatencyMaxFrames = std::max(state.rawSourceUploadLatencyMaxFrames, latencyFrames);
					runtimeUploads++;
					runtimeBytes += entry.slabBytes + entry.colorRunBytes;
				}
			}
		}

		const uint64_t uploadedBytes = uploadSlabBytes + uploadColorRunBytes;
		page.uploadedSlabCount = totalSlabCount;
		page.uploadedColorRunCount = totalColorRunCount;
		page.uploadInFlight = true;
		page.uploadFenceValue = recordingFenceValue;
		state.rawSourceArchiveUploadBytes += uploadedBytes;
		state.rawSourceArchivePreloadUploadBytes += preloadBytes;
		state.rawSourceArchiveRuntimeUploadBytes += runtimeBytes;
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel raw source upload NRI: frame=%llu page=%u new_uploads=%u preload_uploads=%u runtime_uploads=%u failures=0 bytes=%llu preload_bytes=%llu runtime_bytes=%llu records=%llu hits=%llu misses=%llu total_upload_bytes=%llu total_preload_bytes=%llu total_runtime_bytes=%llu total_failures=%llu whole_archive_reuploads=%llu archive_pages=%u archive_descriptors=%u slab_records=%u color_run_records=%u slab_capacity_bytes=%llu color_run_capacity_bytes=%llu upload_latency_avg_frames=%.3f upload_latency_max_frames=%llu source_pending=%u scans=%llu scan_ms=%.3f\n",
				(unsigned long long)frameNumber,
				pageIndex,
				uploads,
				preloadUploads,
				runtimeUploads,
				(unsigned long long)uploadedBytes,
				(unsigned long long)preloadBytes,
				(unsigned long long)runtimeBytes,
				(unsigned long long)state.rawSourceArchiveRecords,
				(unsigned long long)state.rawSourceArchiveHits,
				(unsigned long long)state.rawSourceArchiveMisses,
				(unsigned long long)state.rawSourceArchiveUploadBytes,
				(unsigned long long)state.rawSourceArchivePreloadUploadBytes,
				(unsigned long long)state.rawSourceArchiveRuntimeUploadBytes,
				(unsigned long long)state.rawSourceArchiveUploadFailures,
				(unsigned long long)state.rawSourceWholeArchiveReuploads,
				(uint32_t)state.rawArchivePages.size(),
				(uint32_t)state.rawArchivePages.size() * 2u,
				totalSlabCount,
				totalColorRunCount,
				(unsigned long long)page.slabBuffer.size,
				(unsigned long long)page.colorRunBuffer.size,
				state.rawSourceUploadLatencySamples != 0 ?
					(double)state.rawSourceUploadLatencyFrames / (double)state.rawSourceUploadLatencySamples : 0.0,
				(unsigned long long)state.rawSourceUploadLatencyMaxFrames,
				state.rawArchivePlan.GetStats().pendingSources,
				(unsigned long long)state.rawSourceIngestScans,
				state.rawSourceIngestScanMs);
		}
	}

	void UploadPendingRawSources(
		NRIRenderer& renderer,
		NRIRenderDevice* timingDevice,
		const NRIResourceServices& services,
		uint64_t frameNumber)
	{
		const uint32_t pageCount = (uint32_t)gVoxelComputeState.rawArchivePages.size();
		for (uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex)
		{
			UploadPendingRawSourcePage(renderer, timingDevice, services, frameNumber, pageIndex);
		}
	}

	void ResetCompletionSlotMetadata(VoxelComputeCompletionSlot& slot)
	{
		slot.pendingJobs.clear();
		slot.dispatchFrame = 0;
		slot.rawArchiveGeneration = 0;
		slot.statusBytes = 0;
		slot.fullGeometryBytes = 0;
		slot.pendingVertexCount = 0;
		slot.pendingIndexCount = 0;
		slot.pendingPrimitiveCount = 0;
		slot.emit = false;
		slot.buildBlas = false;
		slot.fullGeneratedReadback = false;
		slot.directProduction = false;
		slot.outputVertexBuffer = nullptr;
		slot.outputIndexBuffer = nullptr;
		slot.outputPrimitiveBuffer = nullptr;
	}

	void FailCompletionSlotJobs(VoxelComputeState& state, VoxelComputeCompletionSlot& slot)
	{
		for (const PendingReadbackJob& job : slot.pendingJobs)
		{
			if (job.consumeKey != 0)
			{
				state.pendingConsumeKeys.erase(job.consumeKey);
				state.failedConsumeKeys.insert(job.consumeKey);
			}
			for (const PendingDirectPublishBinding& binding : job.directBindings)
			{
				state.cancelledDirectKeys.erase(binding.directKey);
				if (state.pendingDirectKeys.erase(binding.directKey) != 0)
				{
					state.failedDirectKeys.insert(binding.directKey);
				}
			}
		}
	}

	void ProcessCompletedSlot(
		const NRIResourceServices& services,
		uint32_t slotIndex,
		uint64_t observedFrame)
	{
		VoxelComputeState& state = gVoxelComputeState;
		VoxelComputeCompletionSlot& slot = state.completionSlots[slotIndex];
		const NRIVoxelComputeCompletionSlotToken token = state.completionRing.slots[slotIndex];
		const uint64_t resultByteSize = (uint64_t)slot.pendingJobs.size() * sizeof(NRIVoxelComputeResult);
		const void* mapped = slot.readbackBuffer.buffer != nullptr && resultByteSize != 0 ?
			services.context.core->MapBuffer(*slot.readbackBuffer.buffer, 0, resultByteSize) : nullptr;
		if (mapped == nullptr)
		{
			FailCompletionSlotJobs(state, slot);
			state.completionRing.Release(slotIndex, NRIVoxelComputeCompletionOutcome::Failed);
			if (IsTraceEnabled())
			{
				Printf(
					"PERF pt voxel compute completion NRI: action=retire submission=%llu slot=%u fence=%llu dispatch_frame=%llu observed_frame=%llu status=map_failed occupancy=%u high_water=%u backpressure=%llu completed=%llu stale=%llu failed=%llu status_bytes=%llu full_geometry_bytes=%llu host_waits=%llu\n",
					(unsigned long long)token.submissionId, slotIndex, (unsigned long long)token.fenceValue,
					(unsigned long long)slot.dispatchFrame, (unsigned long long)observedFrame,
					state.completionRing.occupancy, state.completionRing.highWater,
					(unsigned long long)state.completionRing.backpressureCount,
					(unsigned long long)state.completionRing.completedCount,
					(unsigned long long)state.completionRing.staleCount,
					(unsigned long long)state.completionRing.failedCount,
					(unsigned long long)resultByteSize, (unsigned long long)slot.fullGeometryBytes,
					(unsigned long long)state.completionHostWaits);
			}
			ResetCompletionSlotMetadata(slot);
			return;
		}

		const NRIVoxelComputeResult* results = static_cast<const NRIVoxelComputeResult*>(mapped);
		const NRIVoxelComputeSceneVertex* generatedVertices = nullptr;
		const uint32_t* generatedIndices = nullptr;
		const NRIVoxelComputePrimitiveData* generatedPrimitives = nullptr;
		if (slot.emit && slot.fullGeneratedReadback)
		{
			const uint64_t vertexBytes = (uint64_t)slot.pendingVertexCount * sizeof(NRIVoxelComputeSceneVertex);
			const uint64_t indexBytes = (uint64_t)slot.pendingIndexCount * sizeof(uint32_t);
			const uint64_t primitiveBytes = (uint64_t)slot.pendingPrimitiveCount * sizeof(NRIVoxelComputePrimitiveData);
			generatedVertices = slot.vertexReadbackBuffer.buffer != nullptr && vertexBytes != 0 ?
				static_cast<const NRIVoxelComputeSceneVertex*>(services.context.core->MapBuffer(*slot.vertexReadbackBuffer.buffer, 0, vertexBytes)) : nullptr;
			generatedIndices = slot.indexReadbackBuffer.buffer != nullptr && indexBytes != 0 ?
				static_cast<const uint32_t*>(services.context.core->MapBuffer(*slot.indexReadbackBuffer.buffer, 0, indexBytes)) : nullptr;
			generatedPrimitives = slot.primitiveReadbackBuffer.buffer != nullptr && primitiveBytes != 0 ?
				static_cast<const NRIVoxelComputePrimitiveData*>(services.context.core->MapBuffer(*slot.primitiveReadbackBuffer.buffer, 0, primitiveBytes)) : nullptr;
		}

		uint32_t okCount = 0;
		uint32_t mismatchCount = 0;
		uint32_t directStatusJobs = 0;
		uint32_t directStatusReady = 0;
		uint32_t directStatusFailed = 0;
		uint32_t staleBindings = 0;
		state.totalStatusReadbackBytes += resultByteSize;
		state.totalFullGeometryReadbackBytes += slot.fullGeometryBytes;
		for (size_t i = 0; i < slot.pendingJobs.size(); ++i)
		{
			PendingReadbackJob& job = slot.pendingJobs[i];
			const NRIVoxelComputeResult& result = results[i];
			const bool ok = result.MismatchMask == 0u &&
				((slot.emit && result.Status == NRI_VOXEL_COMPUTE_STATUS_EMIT_OK) ||
				(!slot.emit && result.Status == NRI_VOXEL_COMPUTE_STATUS_COUNT_OK));
			okCount += ok ? 1u : 0u;
			mismatchCount += ok ? 0u : 1u;
			job.admissionState = ok ? (slot.emit ? VoxelComputeAdmissionState::ReadyForBlas : VoxelComputeAdmissionState::CountReady) : VoxelComputeAdmissionState::Failed;
			if (job.consumeKey != 0)
			{
				state.pendingConsumeKeys.erase(job.consumeKey);
				GeneratedVoxelGeometry generated = {};
				if (ok && slot.emit && slot.fullGeneratedReadback &&
					ImportGeneratedGeometry(job, result, generatedVertices, generatedIndices, generatedPrimitives, generated))
				{
					state.readyGeneratedGeometry[job.consumeKey] = std::move(generated);
					state.failedConsumeKeys.erase(job.consumeKey);
				}
				else
				{
					state.failedConsumeKeys.insert(job.consumeKey);
				}
			}

			directStatusJobs += (uint32_t)job.directBindings.size();
			for (const PendingDirectPublishBinding& binding : job.directBindings)
			{
				const bool stillPending = state.pendingDirectKeys.erase(binding.directKey) != 0;
				state.cancelledDirectKeys.erase(binding.directKey);
				staleBindings += stillPending ? 0u : 1u;
				directStatusReady += ok && stillPending ? 1u : 0u;
				directStatusFailed += !ok && stillPending ? 1u : 0u;
				if (ok && slot.emit && stillPending)
				{
					NRIVoxelComputeDirectPublishedMesh published = {};
					published.meshResourceKey = binding.meshResourceKey;
					published.generation = binding.generation;
					published.sourceArchiveSerial = job.sourceArchiveSerial;
					published.jobId = job.jobId;
					published.readyFrame = (uint32_t)std::min<uint64_t>(observedFrame, UINT32_MAX);
					published.status = NRIVoxelComputeGeneratedGeometryStatus::Ready;
					published.failure = NRIVoxelComputeDirectPublishFailure::None;
					published.outputKind = job.outputKind;
					published.vertices = job.vertices;
					published.vertices.count = result.VertexCountNoDedupe;
					published.indices = job.indices;
					published.indices.count = result.IndexCount;
					published.primitives = job.primitives;
					published.primitives.count = result.PrimitiveCount;
					published.materialBase = binding.materialBase;
					published.materialCount = binding.materialCount;
					published.bounds = job.bounds;
					published.surfaceArea = job.surfaceArea;
					state.readyDirectPublishedMeshes[binding.directKey] = published;
					state.failedDirectKeys.erase(binding.directKey);
				}
				else if (stillPending)
				{
					state.failedDirectKeys.insert(binding.directKey);
				}
				if (IsTraceEnabled())
				{
					Printf(
						"PERF pt voxel compute direct publish NRI: action=status submission=%llu slot=%u fence=%llu dispatch_frame=%llu observed_frame=%llu job=%u mesh_resource=0x%llx generation=%llu status=%s mismatch=%u vertices=%u indices=%u primitives=%u source_serial=%llu status_readback_bytes=%u full_geometry_readback_bytes=%llu\n",
						(unsigned long long)token.submissionId, slotIndex, (unsigned long long)token.fenceValue,
						(unsigned long long)slot.dispatchFrame, (unsigned long long)observedFrame, job.jobId,
						(unsigned long long)binding.meshResourceKey, (unsigned long long)binding.generation,
						!stillPending ? "stale" : (ok ? "ready" : "failed"), result.MismatchMask,
						result.VertexCountNoDedupe, result.IndexCount, result.PrimitiveCount,
						(unsigned long long)job.sourceArchiveSerial, (uint32_t)sizeof(NRIVoxelComputeResult),
						(unsigned long long)slot.fullGeometryBytes);
				}
			}
			TraceAdmission(slot.dispatchFrame, job, job.admissionState, slot.emit ? "fence_emit" : "fence_count");
		}

		if (generatedVertices != nullptr) services.context.core->UnmapBuffer(*slot.vertexReadbackBuffer.buffer);
		if (generatedIndices != nullptr) services.context.core->UnmapBuffer(*slot.indexReadbackBuffer.buffer);
		if (generatedPrimitives != nullptr) services.context.core->UnmapBuffer(*slot.primitiveReadbackBuffer.buffer);
		services.context.core->UnmapBuffer(*slot.readbackBuffer.buffer);

		const NRIVoxelComputeCompletionOutcome outcome = mismatchCount != 0 ? NRIVoxelComputeCompletionOutcome::Failed :
			(directStatusJobs != 0 && staleBindings == directStatusJobs ? NRIVoxelComputeCompletionOutcome::Stale : NRIVoxelComputeCompletionOutcome::Completed);
		state.completionRing.Release(slotIndex, outcome);
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute completion NRI: action=retire submission=%llu slot=%u fence=%llu level_generation=%llu archive_generation=%llu dispatch_frame=%llu observed_frame=%llu jobs=%u ready=%u failed_jobs=%u stale_bindings=%u status=%s occupancy=%u high_water=%u backpressure=%llu completed=%llu stale=%llu abandoned=%llu failed=%llu status_bytes=%llu full_geometry_bytes=%llu host_waits=%llu\n",
				(unsigned long long)token.submissionId, slotIndex, (unsigned long long)token.fenceValue,
				(unsigned long long)token.levelGeneration, (unsigned long long)slot.rawArchiveGeneration,
				(unsigned long long)slot.dispatchFrame, (unsigned long long)observedFrame,
				(unsigned)slot.pendingJobs.size(), directStatusReady, directStatusFailed, staleBindings,
				outcome == NRIVoxelComputeCompletionOutcome::Completed ? "completed" :
					(outcome == NRIVoxelComputeCompletionOutcome::Stale ? "stale" : "failed"),
				state.completionRing.occupancy, state.completionRing.highWater,
				(unsigned long long)state.completionRing.backpressureCount,
				(unsigned long long)state.completionRing.completedCount,
				(unsigned long long)state.completionRing.staleCount,
				(unsigned long long)state.completionRing.abandonedCount,
				(unsigned long long)state.completionRing.failedCount,
				(unsigned long long)resultByteSize, (unsigned long long)slot.fullGeometryBytes,
				(unsigned long long)state.completionHostWaits);
		}
		ResetCompletionSlotMetadata(slot);
	}

	void PollCompletionSlots(NRIRenderer& renderer, const NRIResourceServices& services, uint64_t observedFrame)
	{
		VoxelComputeState& state = gVoxelComputeState;
		state.completionPolls++;
		for (uint32_t slotIndex = 0; slotIndex < state.completionSlots.size(); ++slotIndex)
		{
			const NRIVoxelComputeCompletionSlotToken& token = state.completionRing.slots[slotIndex];
			if (token.phase == NRIVoxelComputeCompletionSlotPhase::Submitted && renderer.IsCommandFenceValueComplete(token.fenceValue))
			{
				ProcessCompletedSlot(services, slotIndex, observedFrame);
			}
		}
	}
}

bool ShouldTraceNRIVoxelComputeMeshing()
{
	return IsTraceEnabled();
}

bool ShouldRunNRIVoxelComputeMeshing()
{
	return (bool)nri_ptvoxelcompute && (int)nri_ptvoxelcomputemode >= 2;
}

bool ShouldEmitNRIVoxelComputeMeshing()
{
	return IsEmitEnabled();
}

bool ShouldConsumeNRIVoxelComputeMeshing()
{
	return IsConsumptionEnabled() && !(bool)nri_ptvoxelcomputeforcecpu;
}

bool ShouldDirectPublishNRIVoxelComputeMeshing()
{
	return
		ShouldConsumeNRIVoxelComputeMeshing() &&
		IsDirectGpuPublicationEnabled() &&
		IsDirectPublicationEnabled() &&
		IsRawSourceArchiveEnabled() &&
		!IsFullGeneratedReadbackEnabled();
}

NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, FVoxelModel* model)
{
	constexpr uint32_t MaxConsumptionPrimitives = 8192;
	VoxelComputeState& state = gVoxelComputeState;
	if (!ShouldConsumeNRIVoxelComputeMeshing() || requestKey == 0 || model == nullptr)
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}
	if (state.readyGeneratedGeometry.find(requestKey) != state.readyGeneratedGeometry.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Ready;
	}
	if (state.failedConsumeKeys.find(requestKey) != state.failedConsumeKeys.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Failed;
	}
	if (state.queuedConsumeKeys.find(requestKey) != state.queuedConsumeKeys.end() ||
		state.pendingConsumeKeys.find(requestKey) != state.pendingConsumeKeys.end())
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Queued;
	}

	const uint32_t maxJobs = std::max(0, (int)nri_ptvoxelcomputemaxjobs);
	if (maxJobs == 0)
	{
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}
	if (state.queuedJobs.size() >= maxJobs)
	{
		auto diagnosticJob = std::find_if(state.queuedJobs.rbegin(), state.queuedJobs.rend(), [](const PendingVoxelComputeJob& job)
		{
			return job.consumeKey == 0;
		});
		if (diagnosticJob == state.queuedJobs.rend())
		{
			return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
		}
		state.queuedJobs.erase(std::next(diagnosticJob).base());
	}

	FVoxelRawMeshStats rawStats = {};
	TArray<FVoxelRawSlabRecord> rawSlabs;
	TArray<FVoxelRawFaceRecord> rawFaces;
	TArray<FVoxelRawColorRunRecord> rawColorRuns;
	model->BuildRawMeshStats(rawStats, &rawSlabs, &rawFaces, &rawColorRuns);
	RawVoxelSourceArchiveEntry* archivedSource = RecordRawSourceArchive(model, rawStats, rawSlabs, &rawFaces, &rawColorRuns);
	if (rawStats.slabCount == 0 || rawStats.coalescedFaceCount == 0 ||
		rawFaces.Size() != rawStats.coalescedFaceCount ||
		rawStats.coalescedFaceCount > MaxConsumptionPrimitives / 2u)
	{
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute consume NRI: action=fallback reason=%s consume_key=0x%llx faces=%u primitives=%u max_primitives=%u\n",
				rawStats.coalescedFaceCount > MaxConsumptionPrimitives / 2u ? "primitive_budget" : "invalid_raw",
				(unsigned long long)requestKey,
				rawStats.coalescedFaceCount,
				rawStats.coalescedFaceCount * 2u,
				MaxConsumptionPrimitives);
		}
		return NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	}

	PendingVoxelComputeJob job = {};
	job.model = model;
	job.stats = rawStats;
	job.consumeKey = requestKey;
	job.jobId = state.nextJobId++;
	if (archivedSource != nullptr)
	{
		CopyRawArchiveRecords(*archivedSource, job.slabs, job.colorRuns);
		job.faces = archivedSource->faces;
	}
	else
	{
		CopySlabRecords(rawSlabs, job.slabs);
		CopyFaceRecords(rawFaces, job.faces);
		CopyColorRunRecords(rawColorRuns, job.colorRuns);
	}
	state.queuedConsumeKeys.insert(requestKey);
	state.queuedJobs.push_back(std::move(job));
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute consume NRI: action=queue consume_key=0x%llx job=%u faces=%u vertices=%u indices=%u primitives=%u\n",
			(unsigned long long)requestKey,
			state.queuedJobs.back().jobId,
			rawStats.coalescedFaceCount,
			rawStats.noDedupeVertexCount,
			rawStats.indexCount,
			rawStats.coalescedFaceCount * 2u);
	}
	return NRIVoxelComputeGeneratedGeometryStatus::Queued;
}

bool TakeNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, nri_scene::GeometryData& outGeometry, uint32_t* outJobId)
{
	VoxelComputeState& state = gVoxelComputeState;
	auto found = state.readyGeneratedGeometry.find(requestKey);
	if (found == state.readyGeneratedGeometry.end())
	{
		return false;
	}
	if (outJobId != nullptr)
	{
		*outJobId = found->second.jobId;
	}
	outGeometry = std::move(found->second.geometry);
	state.readyGeneratedGeometry.erase(found);
	return true;
}

bool RequestNRIVoxelComputeDirectPublicationBatch(
	const NRIVoxelComputeDirectPublishRequest* requests,
	uint32_t requestCount,
	NRIVoxelComputeDirectPublishBatchResult& outResult)
{
	outResult = {};
	outResult.inputRequests = requestCount;
	VoxelComputeState& state = gVoxelComputeState;
	if (!ShouldDirectPublishNRIVoxelComputeMeshing())
	{
		outResult.failure = NRIVoxelComputeDirectPublishFailure::Disabled;
		return false;
	}
	if (requests == nullptr || requestCount == 0)
	{
		outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Failed;
		outResult.failure = NRIVoxelComputeDirectPublishFailure::InvalidRequest;
		return false;
	}

	struct PreparedRequest
	{
		const NRIVoxelComputeDirectPublishRequest* request = nullptr;
		RawVoxelSourceArchiveEntry* archive = nullptr;
		uint64_t directKey = 0;
		uint64_t geometryKey = 0;
		uint64_t levelGeneration = 0;
	};
	std::vector<PreparedRequest> prepared;
	std::vector<NRIVoxelComputeBatchPlanRequest> planRequests;
	prepared.reserve(requestCount);
	planRequests.reserve(requestCount);
	uint32_t alreadyQueued = 0;
	uint32_t alreadyReady = 0;
	uint32_t sourcePending = 0;
	const uint32_t maxDirectPublishPrimitives = std::max(0, (int)nri_ptvoxelcomputedirectmaxprimitives);
	const uint32_t maxJobs = (uint32_t)std::max(0, (int)nri_ptvoxelcomputemaxjobs);

	for (uint32_t requestIndex = 0; requestIndex < requestCount; ++requestIndex)
	{
		const NRIVoxelComputeDirectPublishRequest& request = requests[requestIndex];
		const uint64_t directKey = BuildDirectPublishKey(request.meshResourceKey, request.generation);
		if (request.meshResourceKey == 0 || request.generation == 0 || request.model == nullptr ||
			request.outputKind == NRIVoxelComputeDirectPublishOutputKind::None ||
			!DirectPublishBufferReady(request.outputBuffers.vertices, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT)) ||
			!DirectPublishBufferReady(request.outputBuffers.indices, NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT)) ||
			!DirectPublishBufferReady(request.outputBuffers.primitives, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE))
		{
			outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Failed;
			outResult.failure = NRIVoxelComputeDirectPublishFailure::InvalidRequest;
			return false;
		}
		if (request.outputKind != NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena)
		{
			outResult.failure = NRIVoxelComputeDirectPublishFailure::UnsupportedOutputKind;
			return false;
		}
		if (state.failedDirectKeys.find(directKey) != state.failedDirectKeys.end())
		{
			outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Failed;
			outResult.failure = NRIVoxelComputeDirectPublishFailure::StatusFailed;
			return false;
		}
		if (state.readyDirectPublishedMeshes.find(directKey) != state.readyDirectPublishedMeshes.end())
		{
			alreadyReady++;
			continue;
		}
		if (state.queuedDirectKeys.find(directKey) != state.queuedDirectKeys.end() ||
			state.pendingDirectKeys.find(directKey) != state.pendingDirectKeys.end() ||
			state.cancelledDirectKeys.find(directKey) != state.cancelledDirectKeys.end())
		{
			alreadyQueued++;
			continue;
		}

		RawVoxelSourceArchiveEntry* archivedSource = nullptr;
		auto archived = state.rawSourceArchive.find(request.model);
		if (archived != state.rawSourceArchive.end() && !archived->second.failed)
		{
			archivedSource = &archived->second;
			outResult.repeatedRawStatScansAvoided++;
			state.rawSourceArchiveHits++;
		}
		else
		{
			const NRIVoxelComputeRawSourceState sourceState =
				state.rawArchivePlan.GetSourceState(RawSourceKey(request.model));
			if (sourceState == NRIVoxelComputeRawSourceState::Missing &&
				(maxJobs == 0 || state.queuedJobs.size() >= maxJobs))
			{
				outResult.failure = NRIVoxelComputeDirectPublishFailure::QueueFull;
				return false;
			}
			const NRIVoxelComputeRawSourceQueueResult sourceResult =
				QueueRuntimeRawSource(request.model, state.lastObservedFrame);
			if (sourceResult == NRIVoxelComputeRawSourceQueueResult::Queued ||
				sourceResult == NRIVoxelComputeRawSourceQueueResult::AlreadyPending)
			{
				outResult.repeatedRawStatScansAvoided +=
					sourceResult == NRIVoxelComputeRawSourceQueueResult::AlreadyPending ? 1u : 0u;
				sourcePending++;
				continue;
			}
			outResult.status = sourceResult == NRIVoxelComputeRawSourceQueueResult::Full ?
				NRIVoxelComputeGeneratedGeometryStatus::Unavailable :
				NRIVoxelComputeGeneratedGeometryStatus::Failed;
			outResult.failure = sourceResult == NRIVoxelComputeRawSourceQueueResult::Full ?
				NRIVoxelComputeDirectPublishFailure::QueueFull :
				NRIVoxelComputeDirectPublishFailure::InvalidRequest;
			return false;
		}
		if (archivedSource == nullptr || archivedSource->failed)
		{
			outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Failed;
			outResult.failure = NRIVoxelComputeDirectPublishFailure::InvalidRequest;
			return false;
		}

		const FVoxelRawMeshStats& rawStats = archivedSource->stats;
		const uint32_t expectedPrimitives = AdjacencySurfacePrimitiveCount(rawStats);
		const bool overPrimitiveBudget = maxDirectPublishPrimitives != 0 && expectedPrimitives > maxDirectPublishPrimitives;
		if (rawStats.slabCount == 0 || rawStats.adjacencySurfaceFaceCount == 0 || overPrimitiveBudget ||
			rawStats.adjacencySurfaceVertexCount > request.vertices.capacity ||
			rawStats.adjacencySurfaceIndexCount > request.indices.capacity ||
			expectedPrimitives > request.primitives.capacity)
		{
			outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Failed;
			outResult.failure = overPrimitiveBudget ?
				NRIVoxelComputeDirectPublishFailure::PrimitiveBudget :
				NRIVoxelComputeDirectPublishFailure::AllocationUnavailable;
			return false;
		}

		PreparedRequest item = {};
		item.request = &request;
		item.archive = archivedSource;
		item.directKey = directKey;
		item.geometryKey = request.geometryKey != 0 ? request.geometryKey : request.meshResourceKey;
		item.levelGeneration = request.generation >> 32;
		if (item.levelGeneration == 0)
		{
			item.levelGeneration = request.generation;
		}
		prepared.push_back(item);

		NRIVoxelComputeBatchPlanRequest planRequest = {};
		planRequest.meshKey = item.geometryKey;
		planRequest.levelGeneration = item.levelGeneration;
		planRequest.requestKey = directKey;
		planRequest.materialBindingKey = request.materialBindingKey;
		planRequest.source = {
			archivedSource->slabOffset,
			archivedSource->slabCount,
			archivedSource->colorRunOffset,
			archivedSource->colorRunCount
		};
		planRequest.vertices = { request.vertices.offset, rawStats.adjacencySurfaceVertexCount, request.vertices.capacity };
		planRequest.indices = { request.indices.offset, rawStats.adjacencySurfaceIndexCount, request.indices.capacity };
		planRequest.primitives = { request.primitives.offset, expectedPrimitives, request.primitives.capacity };
		planRequest.reservationBytes =
			(uint64_t)request.vertices.capacity * sizeof(NRIVoxelComputeSceneVertex) +
			(uint64_t)request.indices.capacity * sizeof(uint32_t) +
			(uint64_t)request.primitives.capacity * sizeof(NRIVoxelComputePrimitiveData);
		planRequest.priority = request.priority;
		planRequest.age = request.age;
		planRequests.push_back(planRequest);
	}

	if (sourcePending != 0)
	{
		outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Queued;
		outResult.uniqueJobs = 0;
		outResult.dedupeHits = alreadyQueued;
		outResult.materialBindings = requestCount;
		state.directBatchInputRequests += requestCount;
		state.directBatchDedupeHits += outResult.dedupeHits;
		state.directBatchMaterialBindings += outResult.materialBindings;
		state.directBatchRawStatScansAvoided += outResult.repeatedRawStatScansAvoided;
		return true;
	}

	if (prepared.empty())
	{
		outResult.status = alreadyQueued != 0 ?
			NRIVoxelComputeGeneratedGeometryStatus::Queued :
			NRIVoxelComputeGeneratedGeometryStatus::Ready;
		outResult.uniqueJobs = 0;
		outResult.dedupeHits = alreadyQueued + (alreadyReady > 0 ? alreadyReady - 1u : 0u);
		outResult.materialBindings = requestCount;
		state.directBatchInputRequests += requestCount;
		state.directBatchDedupeHits += outResult.dedupeHits;
		state.directBatchMaterialBindings += outResult.materialBindings;
		return true;
	}

	NRIVoxelComputeBatchPlanSettings settings = {};
	settings.activeLevelGeneration = planRequests.front().levelGeneration;
	settings.maxJobsPerBatch = maxJobs;
	settings.maxBytesPerBatch = (uint64_t)std::max(0, (int)nri_ptvoxeladmitmaxbytesruntime);
	const NRIVoxelComputeBatchPlan plan = BuildNRIVoxelComputeBatchPlan(planRequests, settings);
	if (!plan.rejected.empty())
	{
		outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Failed;
		const NRIVoxelComputeBatchRejectReason reason = plan.rejected.front().reason;
		outResult.failure = reason == NRIVoxelComputeBatchRejectReason::StaleGeneration ?
			NRIVoxelComputeDirectPublishFailure::StaleGeneration :
			(reason == NRIVoxelComputeBatchRejectReason::Capacity ?
				NRIVoxelComputeDirectPublishFailure::AllocationUnavailable :
				NRIVoxelComputeDirectPublishFailure::InvalidRequest);
		return false;
	}

	std::vector<int32_t> queuedOwners(plan.jobs.size(), -1);
	struct PendingOwner
	{
		int32_t slotIndex = -1;
		int32_t jobIndex = -1;
	};
	std::vector<PendingOwner> pendingOwners(plan.jobs.size());
	uint32_t newJobs = 0;
	for (uint32_t planJobIndex = 0; planJobIndex < plan.jobs.size(); ++planJobIndex)
	{
		const NRIVoxelComputeBatchPlannedJob& planned = plan.jobs[planJobIndex];
		for (uint32_t queuedIndex = 0; queuedIndex < state.queuedJobs.size(); ++queuedIndex)
		{
			const PendingVoxelComputeJob& queued = state.queuedJobs[queuedIndex];
			if (queued.directPublication && queued.geometryKey == planned.meshKey &&
				((queued.directGeneration >> 32) != 0 ? queued.directGeneration >> 32 : queued.directGeneration) == planned.levelGeneration)
			{
				queuedOwners[planJobIndex] = (int32_t)queuedIndex;
				break;
			}
		}
		if (queuedOwners[planJobIndex] < 0)
		{
			for (uint32_t slotIndex = 0; slotIndex < state.completionSlots.size(); ++slotIndex)
			{
				if (state.completionRing.slots[slotIndex].phase == NRIVoxelComputeCompletionSlotPhase::Free)
				{
					continue;
				}
				const std::vector<PendingReadbackJob>& pendingJobs = state.completionSlots[slotIndex].pendingJobs;
				for (uint32_t pendingIndex = 0; pendingIndex < pendingJobs.size(); ++pendingIndex)
				{
					const PendingReadbackJob& pending = pendingJobs[pendingIndex];
					if (pending.directPublication && pending.geometryKey == planned.meshKey &&
						((pending.directGeneration >> 32) != 0 ? pending.directGeneration >> 32 : pending.directGeneration) == planned.levelGeneration)
					{
						pendingOwners[planJobIndex] = { (int32_t)slotIndex, (int32_t)pendingIndex };
						break;
					}
				}
				if (pendingOwners[planJobIndex].slotIndex >= 0)
				{
					break;
				}
			}
		}
		newJobs += queuedOwners[planJobIndex] < 0 && pendingOwners[planJobIndex].slotIndex < 0 ? 1u : 0u;
	}
	if (maxJobs == 0 || state.queuedJobs.size() + newJobs > maxJobs)
	{
		outResult.failure = NRIVoxelComputeDirectPublishFailure::QueueFull;
		return false;
	}

	for (uint32_t planJobIndex = 0; planJobIndex < plan.jobs.size(); ++planJobIndex)
	{
		const NRIVoxelComputeBatchPlannedJob& planned = plan.jobs[planJobIndex];
		auto appendBindings = [&](std::vector<PendingDirectPublishBinding>& target)
		{
			for (const NRIVoxelComputeBatchMaterialBinding& binding : planned.bindings)
			{
				const PreparedRequest& item = prepared[binding.requestIndex];
				const NRIVoxelComputeDirectPublishRequest& request = *item.request;
				target.push_back({
					request.meshResourceKey,
					item.directKey,
					request.generation,
					request.materialBindingKey,
					request.materialBase,
					request.materialCount
				});
				state.queuedDirectKeys.insert(item.directKey);
			}
		};

		if (queuedOwners[planJobIndex] >= 0)
		{
			appendBindings(state.queuedJobs[(uint32_t)queuedOwners[planJobIndex]].directBindings);
			continue;
		}
		if (pendingOwners[planJobIndex].slotIndex >= 0)
		{
			const PendingOwner owner = pendingOwners[planJobIndex];
			PendingReadbackJob& pending = state.completionSlots[(uint32_t)owner.slotIndex].pendingJobs[(uint32_t)owner.jobIndex];
			appendBindings(pending.directBindings);
			for (const PendingDirectPublishBinding& binding : pending.directBindings)
			{
				if (state.queuedDirectKeys.erase(binding.directKey) != 0)
				{
					state.pendingDirectKeys.insert(binding.directKey);
				}
			}
			continue;
		}

		const PreparedRequest& owner = prepared[planned.ownerRequestIndex];
		const NRIVoxelComputeDirectPublishRequest& request = *owner.request;
		PendingVoxelComputeJob job = {};
		job.model = request.model;
		job.stats = owner.archive->stats;
		job.directPublication = true;
		job.directMeshResourceKey = request.meshResourceKey;
		job.directKey = owner.directKey;
		job.directGeneration = request.generation;
		job.geometryKey = planned.meshKey;
		job.reservationBytes = planned.reservationBytes;
		job.sourceArchiveSerial = owner.archive->recordSerial;
		job.jobId = state.nextJobId++;
		job.materialBase = request.materialBase;
		job.materialCount = request.materialCount;
		job.priority = planned.priority;
		job.age = planned.age;
		job.oversizedExclusive = planned.oversizedExclusive;
		job.outputKind = request.outputKind;
		job.outputBuffers = request.outputBuffers;
		job.vertices = request.vertices;
		job.indices = request.indices;
		job.primitives = request.primitives;
		appendBindings(job.directBindings);
		state.queuedJobs.push_back(std::move(job));
	}

	outResult.status = NRIVoxelComputeGeneratedGeometryStatus::Queued;
	outResult.uniqueJobs = plan.stats.uniqueJobs;
	outResult.dedupeHits = plan.stats.dedupeHits + alreadyQueued;
	outResult.materialBindings = plan.stats.materialBindings + alreadyQueued + alreadyReady;
	state.directBatchInputRequests += requestCount;
	state.directBatchUniqueJobs += plan.stats.uniqueJobs;
	state.directBatchDedupeHits += outResult.dedupeHits;
	state.directBatchMaterialBindings += outResult.materialBindings;
	state.directBatchRawStatScansAvoided += outResult.repeatedRawStatScansAvoided;
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute batch queue NRI: input_requests=%u unique_jobs=%u dedupe_hits=%u material_bindings=%u raw_stat_scans_avoided=%u queued_jobs=%u new_jobs=%u oversized_exclusive=%u atomic=1\n",
			outResult.inputRequests,
			outResult.uniqueJobs,
			outResult.dedupeHits,
			outResult.materialBindings,
			outResult.repeatedRawStatScansAvoided,
			(uint32_t)state.queuedJobs.size(),
			newJobs,
			plan.stats.oversizedExclusiveBatches);
	}
	return true;
}

NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeDirectPublication(const NRIVoxelComputeDirectPublishRequest& request)
{
	NRIVoxelComputeDirectPublishBatchResult result = {};
	RequestNRIVoxelComputeDirectPublicationBatch(&request, 1, result);
	return result.status;
}

bool TakeNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation, NRIVoxelComputeDirectPublishedMesh& outMesh)
{
	VoxelComputeState& state = gVoxelComputeState;
	const uint64_t directKey = BuildDirectPublishKey(meshResourceKey, generation);
	auto found = state.readyDirectPublishedMeshes.find(directKey);
	if (found == state.readyDirectPublishedMeshes.end())
	{
		outMesh = {};
		outMesh.meshResourceKey = meshResourceKey;
		outMesh.generation = generation;
		outMesh.status = state.failedDirectKeys.find(directKey) != state.failedDirectKeys.end() ?
			NRIVoxelComputeGeneratedGeometryStatus::Failed :
			NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
		return false;
	}
	outMesh = found->second;
	return true;
}

void CancelNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation)
{
	VoxelComputeState& state = gVoxelComputeState;
	const uint64_t directKey = BuildDirectPublishKey(meshResourceKey, generation);
	state.queuedDirectKeys.erase(directKey);
	if (state.pendingDirectKeys.erase(directKey) != 0)
	{
		state.cancelledDirectKeys.insert(directKey);
	}
	state.readyDirectPublishedMeshes.erase(directKey);
	for (PendingVoxelComputeJob& job : state.queuedJobs)
	{
		job.directBindings.erase(
			std::remove_if(job.directBindings.begin(), job.directBindings.end(), [directKey](const PendingDirectPublishBinding& binding)
			{
				return binding.directKey == directKey;
			}),
			job.directBindings.end());
	}
	state.queuedJobs.erase(
		std::remove_if(state.queuedJobs.begin(), state.queuedJobs.end(), [](const PendingVoxelComputeJob& job)
		{
			return job.directPublication && job.directBindings.empty();
		}),
		state.queuedJobs.end());
	if (IsTraceEnabled() && meshResourceKey != 0)
	{
		Printf(
			"PERF pt voxel compute direct publish NRI: action=cancel mesh_resource=0x%llx generation=%llu\n",
			(unsigned long long)meshResourceKey,
			(unsigned long long)generation);
	}
}

bool QueryNRIVoxelComputeRawSourceArchiveStats(FVoxelModel* model, FVoxelRawMeshStats& outStats)
{
	outStats = {};
	if (model == nullptr)
	{
		return false;
	}

	VoxelComputeState& state = gVoxelComputeState;
	auto archived = state.rawSourceArchive.find(model);
	if (archived == state.rawSourceArchive.end() ||
		archived->second.failed ||
		archived->second.stats.slabCount == 0 ||
		archived->second.stats.adjacencySurfaceFaceCount == 0)
	{
		return false;
	}

	outStats = archived->second.stats;
	return true;
}

bool CopyNRIVoxelComputeRawSourceArchiveSnapshot(
	FVoxelModel* model,
	NRIVoxelComputeRawSourceArchiveSnapshot& outSnapshot)
{
	outSnapshot = {};
	if (model == nullptr)
	{
		return false;
	}

	const VoxelComputeState& state = gVoxelComputeState;
	const auto archived = state.rawSourceArchive.find(model);
	if (archived == state.rawSourceArchive.end() || archived->second.failed)
	{
		return false;
	}
	const RawVoxelSourceArchiveEntry& entry = archived->second;
	if (entry.stats.slabCount == 0 || entry.stats.adjacencySurfaceFaceCount == 0 ||
		entry.pageIndex >= state.rawArchivePages.size())
	{
		return false;
	}
	const RawVoxelSourceArchivePage& page = state.rawArchivePages[entry.pageIndex];
	if ((uint64_t)entry.slabOffset + entry.slabCount > page.slabs.size())
	{
		return false;
	}

	outSnapshot.recordSerial = entry.recordSerial;
	outSnapshot.contentHash = entry.stats.contentHash;
	outSnapshot.sizeX = entry.stats.sizeX;
	outSnapshot.sizeY = entry.stats.sizeY;
	outSnapshot.sizeZ = entry.stats.sizeZ;
	outSnapshot.exactFaceCount = entry.stats.adjacencySurfaceFaceCount;
	outSnapshot.exactPrimitiveCount = AdjacencySurfacePrimitiveCount(entry.stats);
	outSnapshot.pivotX = entry.stats.pivotX;
	outSnapshot.pivotY = entry.stats.pivotY;
	outSnapshot.pivotZ = entry.stats.pivotZ;
	outSnapshot.slabs.reserve(entry.slabCount);
	for (uint32_t slabIndex = 0; slabIndex < entry.slabCount; ++slabIndex)
	{
		const NRIVoxelComputeSlabRecord& source = page.slabs[entry.slabOffset + slabIndex];
		outSnapshot.slabs.push_back({ source.X, source.Y, source.ZTop, source.CullMask, source.ZLength });
	}
	return outSnapshot.slabs.size() == entry.slabCount;
}

bool QueryNRIVoxelComputeRawSourceStats(FVoxelModel* model, FVoxelRawMeshStats& outStats)
{
	outStats = {};
	if (model == nullptr)
	{
		return false;
	}

	VoxelComputeState& state = gVoxelComputeState;
	auto archived = state.rawSourceArchive.find(model);
	if (archived != state.rawSourceArchive.end() &&
		!archived->second.failed &&
		archived->second.stats.slabCount != 0 &&
		archived->second.stats.adjacencySurfaceFaceCount != 0)
	{
		outStats = archived->second.stats;
		return true;
	}

	QueueRuntimeRawSource(model, state.lastObservedFrame);
	return false;
}

bool PreloadNRIVoxelComputeRawSource(FVoxelModel* model, NRIVoxelComputeRawSourcePreloadStats* outStats)
{
	if (outStats != nullptr)
	{
		outStats->requested++;
	}
	if (!IsRawSourceArchiveEnabled() || model == nullptr)
	{
		if (outStats != nullptr)
		{
			outStats->skipped++;
		}
		return false;
	}

	VoxelComputeState& state = gVoxelComputeState;
	auto found = state.rawSourceArchive.find(model);
	if (found != state.rawSourceArchive.end())
	{
		found->second.preloadRecorded = true;
		if (outStats != nullptr)
		{
			outStats->alreadyResident++;
			outStats->rawBytes += found->second.stats.rawByteCount;
			outStats->uploadBytes += found->second.slabBytes + found->second.colorRunBytes;
			outStats->slabRecords += found->second.stats.slabCount;
			outStats->colorRunRecords += found->second.colorRunCount;
		}
		return true;
	}

	FVoxelRawMeshStats rawStats = {};
	TArray<FVoxelRawSlabRecord> rawSlabs;
	TArray<FVoxelRawColorRunRecord> rawColorRuns;
	const auto start = std::chrono::steady_clock::now();
	model->BuildRawMeshStats(rawStats, &rawSlabs, nullptr, &rawColorRuns);
	const double buildMs = DurationMs(start, std::chrono::steady_clock::now());
	RawVoxelSourceArchiveEntry* archivedSource = RecordRawSourceArchive(model, rawStats, rawSlabs, nullptr, &rawColorRuns, true);
	if (archivedSource == nullptr)
	{
		if (outStats != nullptr)
		{
			outStats->failed++;
			outStats->buildMs += buildMs;
		}
		return false;
	}

	if (outStats != nullptr)
	{
		outStats->recorded++;
		outStats->rawBytes += rawStats.rawByteCount;
		outStats->uploadBytes += archivedSource->slabBytes + archivedSource->colorRunBytes;
		outStats->slabRecords += rawStats.slabCount;
		outStats->colorRunRecords += archivedSource->colorRunCount;
		outStats->buildMs += buildMs;
	}
	return true;
}

NRIVoxelComputeMemoryUsage GetNRIVoxelComputeMemoryUsage()
{
	const VoxelComputeState& state = gVoxelComputeState;
	NRIVoxelComputeMemoryUsage usage = {};
	const auto bufferBytes = [](const NRIBufferResource& resource) -> uint64_t
	{
		return resource.memorySize;
	};

	usage.rawSourceCount = (uint32_t)state.rawSourceArchive.size();
	usage.rawSourcePendingCount = state.rawArchivePlan.GetStats().pendingSources;
	usage.rawArchivePageCount = (uint32_t)state.rawArchivePages.size();
	usage.totalRawSourceScans = state.rawSourceIngestScans;
	usage.totalRawSourceScanFailures = state.rawSourceIngestScanFailures;
	usage.totalRawSourceNewUploadBytes = state.rawSourceArchiveUploadBytes;
	usage.totalRawSourceWholeArchiveReuploads = state.rawSourceWholeArchiveReuploads;
	usage.rawSourceUploadLatencyMaxFrames = state.rawSourceUploadLatencyMaxFrames;
	usage.totalRawSourceScanMs = state.rawSourceIngestScanMs;
	usage.rawSourceScanMaxMs = state.rawSourceIngestScanMaxMs;
	usage.rawSourceUploadLatencyAverageFrames = state.rawSourceUploadLatencySamples != 0 ?
		(double)state.rawSourceUploadLatencyFrames / (double)state.rawSourceUploadLatencySamples : 0.0;
	for (const auto& archivePair : state.rawSourceArchive)
	{
		const RawVoxelSourceArchiveEntry& entry = archivePair.second;
		usage.rawSourceUploadedCount += entry.uploaded ? 1u : 0u;
		usage.rawCpuBytes += (uint64_t)entry.faces.capacity() * sizeof(NRIVoxelComputeFaceRecord);
	}
	for (const RawVoxelSourceArchivePage& page : state.rawArchivePages)
	{
		usage.rawCpuBytes +=
			(uint64_t)page.slabs.capacity() * sizeof(NRIVoxelComputeSlabRecord) +
			(uint64_t)page.colorRuns.capacity() * sizeof(NRIVoxelComputeColorRunRecord);
		usage.rawDeviceBytes += bufferBytes(page.slabBuffer) + bufferBytes(page.colorRunBuffer);
		usage.rawUploadBytes += bufferBytes(page.slabUploadBuffer) + bufferBytes(page.colorRunUploadBuffer);
	}

	usage.queuedJobCount = (uint32_t)state.queuedJobs.size();
	usage.readyDirectMeshCount = (uint32_t)state.readyDirectPublishedMeshes.size();
	for (const VoxelComputeCompletionSlot& slot : state.completionSlots)
	{
		usage.pendingJobCount += (uint32_t)slot.pendingJobs.size();
		usage.transientInputUploadBytes +=
			bufferBytes(slot.jobUploadBuffer) +
			bufferBytes(slot.slabUploadBuffer) +
			bufferBytes(slot.faceUploadBuffer) +
			bufferBytes(slot.colorRunUploadBuffer);
		usage.transientInputDeviceBytes +=
			bufferBytes(slot.jobBuffer) +
			bufferBytes(slot.slabBuffer) +
			bufferBytes(slot.faceBuffer) +
			bufferBytes(slot.colorRunBuffer) +
			bufferBytes(slot.resultBuffer);
		usage.transientGeneratedBytes +=
			bufferBytes(slot.vertexBuffer) +
			bufferBytes(slot.indexBuffer) +
			bufferBytes(slot.primitiveBuffer) +
			bufferBytes(slot.slabScratchBuffer);
		usage.statusReadbackBytes += bufferBytes(slot.readbackBuffer);
		usage.geometryReadbackBufferBytes +=
			bufferBytes(slot.vertexReadbackBuffer) +
			bufferBytes(slot.indexReadbackBuffer) +
			bufferBytes(slot.primitiveReadbackBuffer);
	}
	usage.diagnosticAsBytes = state.diagnosticBlas.memorySize;
	usage.totalStatusReadbackBytes = state.totalStatusReadbackBytes;
	usage.totalFullGeometryReadbackBytes = state.totalFullGeometryReadbackBytes;
	return usage;
}

uint32_t GetNRIVoxelComputeQueuedJobCount()
{
	return (uint32_t)gVoxelComputeState.queuedJobs.size();
}

void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const TArray<FVoxelRawColorRunRecord>* colorRuns,
	const FVoxelMeshData& cpuMesh)
{
	if (!ShouldRunNRIVoxelComputeMeshing() || model == nullptr || slabs == nullptr || stats.slabCount == 0)
	{
		return;
	}
	if (IsEmitEnabled() && (faces == nullptr || faces->Size() != stats.coalescedFaceCount))
	{
		return;
	}

	const uint32_t maxJobs = std::max(0, (int)nri_ptvoxelcomputemaxjobs);
	if (maxJobs == 0 || gVoxelComputeState.queuedJobs.size() >= maxJobs)
	{
		return;
	}

	RawVoxelSourceArchiveEntry* archivedSource = colorRuns != nullptr ? RecordRawSourceArchive(model, stats, *slabs, faces, colorRuns) : nullptr;
	if (archivedSource != nullptr && ShouldDirectPublishNRIVoxelComputeMeshing())
	{
		VoxelComputeState& state = gVoxelComputeState;
		state.diagnosticSidecarsSuppressed++;
		if (IsTraceEnabled() && state.diagnosticSidecarsSuppressed == 1)
		{
			Printf(
				"PERF pt voxel compute diagnostic NRI: action=suppress reason=production-direct archive_serial=%llu faces=%u primitives=%u bounded=1\n",
				(unsigned long long)archivedSource->recordSerial,
				stats.coalescedFaceCount,
				stats.coalescedFaceCount * 2u);
		}
		return;
	}
	PendingVoxelComputeJob job = {};
	job.model = model;
	job.stats = stats;
	job.cpuVertexCount = (uint32_t)cpuMesh.vertices.Size();
	job.cpuIndexCount = (uint32_t)cpuMesh.indices.Size();
	job.jobId = gVoxelComputeState.nextJobId++;
	if (archivedSource != nullptr)
	{
		CopyRawArchiveRecords(*archivedSource, job.slabs, job.colorRuns);
		job.faces = archivedSource->faces;
	}
	else
	{
		CopySlabRecords(*slabs, job.slabs);
		if (faces != nullptr)
		{
			CopyFaceRecords(*faces, job.faces);
		}
		if (colorRuns != nullptr)
		{
			CopyColorRunRecords(*colorRuns, job.colorRuns);
		}
	}
	gVoxelComputeState.queuedJobs.push_back(std::move(job));
}

void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber)
{
	if (!ShouldRunNRIVoxelComputeMeshing())
	{
		return;
	}

	NRIResourceServices services = renderer.BuildResourceServices();
	const NRIResourceContext& context = services.context;
	if (context.device == nullptr || context.core == nullptr || context.commandBuffer == nullptr ||
		renderer.mVoxelComputePipelineLayout == nullptr || renderer.mVoxelComputeInputSets[0] == nullptr || renderer.mVoxelComputeOutputSets[0] == nullptr)
	{
		return;
	}

	VoxelComputeState& state = gVoxelComputeState;
	state.lastObservedFrame = frameNumber;
	PollCompletionSlots(renderer, services, frameNumber);
	CompleteRawArchiveUploads(renderer, services);
	PumpPendingRawSources(frameNumber);
	UploadPendingRawSources(renderer, renderer.mFrameBuffer, services, frameNumber);

	if (state.queuedJobs.empty())
	{
		return;
	}

	const bool emit = IsEmitEnabled();
	const bool buildBlas = emit && IsBlasEnabled();
	const bool fullGeneratedReadback = emit && IsFullGeneratedReadbackEnabled();
	const bool directSource =
		emit && IsDirectGpuPublicationEnabled() && IsRawSourceArchiveEnabled() &&
		state.queuedJobs.front().directPublication;
	RawVoxelSourceArchivePage* directArchivePage = nullptr;
	uint32_t directArchivePageIndex = NRI_VOXEL_RAW_ARCHIVE_INVALID_PAGE;
	if (directSource)
	{
		auto firstArchive = state.rawSourceArchive.find(state.queuedJobs.front().model);
		if (firstArchive != state.rawSourceArchive.end() && firstArchive->second.uploaded && !firstArchive->second.failed)
		{
			directArchivePageIndex = firstArchive->second.pageIndex;
			directArchivePage = FindRawArchivePage(firstArchive->second);
		}
	}
	const int requestedAlgorithm = (int)nri_ptvoxelcomputealgorithm;
	bool parallelArchivedEmit =
		directSource && SelectedComputeAlgorithm() == NRIVoxelComputeAlgorithm::ParallelVoxelAdjacencyCoalescedV3;
	std::vector<NRIVoxelComputeJob> gpuJobs;
	std::vector<NRIVoxelComputeSlabRecord> gpuSlabs;
	std::vector<NRIVoxelComputeFaceRecord> gpuFaces;
	std::vector<NRIVoxelComputeColorRunRecord> gpuColorRuns;
	std::vector<PendingReadbackJob> pendingJobs;
	gpuJobs.reserve(state.queuedJobs.size());
	uint32_t slabOffset = 0;
	uint32_t faceOffset = 0;
	uint32_t colorRunOffset = 0;
	uint32_t vertexOffset = 0;
	uint32_t indexOffset = 0;
	uint32_t primitiveOffset = 0;
	uint32_t emittedVertexCount = 0;
	uint32_t emittedIndexCount = 0;
	uint32_t emittedPrimitiveCount = 0;
	bool directOutput = false;
	const NRIBufferResource* outputVertexBuffer = nullptr;
	const NRIBufferResource* outputIndexBuffer = nullptr;
	const NRIBufferResource* outputPrimitiveBuffer = nullptr;
	size_t jobsToProcess = std::min<size_t>(state.queuedJobs.size(), (size_t)std::max(1, (int)nri_ptvoxelcomputemaxjobs));
	if (directSource)
	{
		const PendingVoxelComputeJob& first = state.queuedJobs.front();
		if (directArchivePage == nullptr)
		{
			jobsToProcess = 0;
		}
		else if (first.oversizedExclusive)
		{
			jobsToProcess = 1;
		}
		else
		{
			jobsToProcess = 0;
			uint64_t dispatchReservationBytes = 0;
			const uint64_t dispatchByteLimit = (uint64_t)std::max(0, (int)nri_ptvoxeladmitmaxbytesruntime);
			while (jobsToProcess < state.queuedJobs.size() && jobsToProcess < (size_t)std::max(1, (int)nri_ptvoxelcomputemaxjobs))
			{
				const PendingVoxelComputeJob& candidate = state.queuedJobs[jobsToProcess];
				if (!candidate.directPublication || candidate.oversizedExclusive ||
					candidate.outputKind != first.outputKind ||
					candidate.outputBuffers.vertices != first.outputBuffers.vertices ||
					candidate.outputBuffers.indices != first.outputBuffers.indices ||
					candidate.outputBuffers.primitives != first.outputBuffers.primitives)
				{
					break;
				}
				if (dispatchByteLimit != 0 && jobsToProcess != 0 &&
					(candidate.reservationBytes > dispatchByteLimit - std::min(dispatchReservationBytes, dispatchByteLimit)))
				{
					break;
				}
				auto archived = state.rawSourceArchive.find(candidate.model);
				if (archived == state.rawSourceArchive.end() || !archived->second.uploaded || archived->second.failed ||
					archived->second.pageIndex != directArchivePageIndex)
				{
					break;
				}
				dispatchReservationBytes += candidate.reservationBytes;
				jobsToProcess++;
			}
		}
	}
	else
	{
		jobsToProcess = 0;
		while (jobsToProcess < state.queuedJobs.size() &&
			jobsToProcess < (size_t)std::max(1, (int)nri_ptvoxelcomputemaxjobs) &&
			!state.queuedJobs[jobsToProcess].directPublication)
		{
			jobsToProcess++;
		}
	}
	if (jobsToProcess == 0)
	{
		return;
	}
	const uint64_t recordingFenceValue = renderer.GetRecordingCommandFenceValue();
	if (recordingFenceValue == 0)
	{
		return;
	}
	const uint64_t directGeneration = directSource ? state.queuedJobs.front().directGeneration : 0u;
	const uint64_t levelGeneration = directGeneration >> 32 != 0 ? directGeneration >> 32 : directGeneration;
	const int32_t acquiredSlot = state.completionRing.Acquire(levelGeneration);
	if (acquiredSlot < 0)
	{
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute completion NRI: action=backpressure frame=%llu occupancy=%u high_water=%u backpressure=%llu queued=%u host_waits=%llu\n",
				(unsigned long long)frameNumber, state.completionRing.occupancy, state.completionRing.highWater,
				(unsigned long long)state.completionRing.backpressureCount, (uint32_t)state.queuedJobs.size(),
				(unsigned long long)state.completionHostWaits);
		}
		return;
	}
	const uint32_t slotIndex = (uint32_t)acquiredSlot;
	VoxelComputeCompletionSlot& slot = state.completionSlots[slotIndex];
	ResetCompletionSlotMetadata(slot);
	slot.inputSet = renderer.mVoxelComputeInputSets[slotIndex];
	slot.outputSet = renderer.mVoxelComputeOutputSets[slotIndex];
	CompletionSlotRecordingGuard recordingGuard = { &state.completionRing, slotIndex, false, frameNumber };
	NRIScopedGpuTiming admissionGpuTiming(renderer.mFrameBuffer, NRIGpuTimingScope::VoxelAdmission);
	for (size_t queuedIndex = 0; queuedIndex < jobsToProcess; ++queuedIndex)
	{
		PendingVoxelComputeJob& queued = state.queuedJobs[queuedIndex];
		RawVoxelSourceArchiveEntry* jobArchive = nullptr;
		if (emit && !queued.directPublication && queued.faces.size() != queued.stats.coalescedFaceCount)
		{
			if (queued.consumeKey != 0)
			{
				state.queuedConsumeKeys.erase(queued.consumeKey);
				state.failedConsumeKeys.insert(queued.consumeKey);
			}
			continue;
		}
		if (directSource)
		{
			jobArchive = FindUploadedRawSourceArchive(queued.model);
			if (jobArchive == nullptr)
			{
				recordingGuard.abortReason = "raw-source-not-uploaded";
				return;
			}
		}

		NRIVoxelComputeJob gpuJob = {};
		gpuJob.SlabOffset = directSource ? jobArchive->slabOffset : slabOffset;
		gpuJob.SlabCount = directSource ? jobArchive->slabCount : (uint32_t)queued.slabs.size();
		gpuJob.FaceOffset = directSource ? 0u : faceOffset;
		gpuJob.ExpectedFaces = queued.directPublication ? queued.stats.adjacencySurfaceFaceCount : queued.stats.coalescedFaceCount;
		gpuJob.ExpectedIndices = queued.directPublication ? queued.stats.adjacencySurfaceIndexCount : queued.stats.indexCount;
		gpuJob.ExpectedVerticesNoDedupe = queued.directPublication ? queued.stats.adjacencySurfaceVertexCount : queued.stats.noDedupeVertexCount;
		gpuJob.ExpectedVoxels = queued.stats.voxelCount;
		gpuJob.JobId = queued.jobId;
		gpuJob.VertexOffset = queued.directPublication ? queued.vertices.offset : vertexOffset;
		gpuJob.IndexOffset = queued.directPublication ? queued.indices.offset : indexOffset;
		gpuJob.PrimitiveOffset = queued.directPublication ? queued.primitives.offset : primitiveOffset;
		gpuJob.PivotX = queued.stats.pivotX;
		gpuJob.PivotY = queued.stats.pivotY;
		gpuJob.PivotZ = queued.stats.pivotZ;
		gpuJob.MaterialBase = queued.directPublication ? queued.materialBase : 0u;
		gpuJob.VertexCapacity = queued.directPublication ? queued.vertices.capacity : gpuJob.ExpectedVerticesNoDedupe;
		gpuJob.IndexCapacity = queued.directPublication ? queued.indices.capacity : gpuJob.ExpectedIndices;
		gpuJob.PrimitiveCapacity = queued.directPublication ? queued.primitives.capacity : queued.stats.coalescedFaceCount * 2u;
		gpuJobs.push_back(gpuJob);

		PendingReadbackJob pending = {};
		pending.jobId = gpuJob.JobId;
		pending.expectedFaces = gpuJob.ExpectedFaces;
		pending.expectedIndices = gpuJob.ExpectedIndices;
		pending.expectedVerticesNoDedupe = gpuJob.ExpectedVerticesNoDedupe;
		pending.expectedVoxels = gpuJob.ExpectedVoxels;
		pending.expectedPrimitives = gpuJob.ExpectedFaces * 2u;
		pending.cpuVertices = queued.cpuVertexCount;
		pending.cpuIndices = queued.cpuIndexCount;
		pending.vertexOffset = gpuJob.VertexOffset;
		pending.indexOffset = gpuJob.IndexOffset;
		pending.primitiveOffset = gpuJob.PrimitiveOffset;
		pending.consumeKey = queued.consumeKey;
		pending.directPublication = queued.directPublication;
		pending.directMeshResourceKey = queued.directMeshResourceKey;
		pending.directKey = queued.directKey;
		pending.directGeneration = queued.directGeneration;
		pending.geometryKey = queued.geometryKey;
		pending.sourceArchiveSerial = jobArchive != nullptr ? jobArchive->recordSerial : queued.sourceArchiveSerial;
		pending.materialBase = queued.materialBase;
		pending.materialCount = queued.materialCount;
		pending.outputKind = queued.outputKind;
		pending.vertices = queued.vertices;
		pending.indices = queued.indices;
		pending.primitives = queued.primitives;
		if (jobArchive != nullptr)
		{
			pending.bounds = jobArchive->bounds;
			pending.surfaceArea = jobArchive->surfaceArea;
		}
		else
		{
			const DirectPublishGeometrySummary geometrySummary = BuildDirectPublishGeometrySummary(queued.stats, queued.slabs);
			pending.bounds = geometrySummary.bounds;
			pending.surfaceArea = geometrySummary.surfaceArea;
		}
		pending.directBindings = queued.directBindings;
		pending.admissionState = emit ? VoxelComputeAdmissionState::Emitting : VoxelComputeAdmissionState::Counting;
		pendingJobs.push_back(pending);
		if (queued.directPublication)
		{
			directOutput = true;
			outputVertexBuffer = queued.outputBuffers.vertices;
			outputIndexBuffer = queued.outputBuffers.indices;
			outputPrimitiveBuffer = queued.outputBuffers.primitives;
			emittedVertexCount += queued.stats.adjacencySurfaceVertexCount;
			emittedIndexCount += queued.stats.adjacencySurfaceIndexCount;
			emittedPrimitiveCount += AdjacencySurfacePrimitiveCount(queued.stats);
		}
		TraceAdmission(frameNumber, pending, pending.admissionState, "dispatch");

		if (!directSource)
		{
			for (NRIVoxelComputeSlabRecord slab : queued.slabs)
			{
				slab.ColorRunOffset += colorRunOffset;
				gpuSlabs.push_back(slab);
			}
			gpuFaces.insert(gpuFaces.end(), queued.faces.begin(), queued.faces.end());
			gpuColorRuns.insert(gpuColorRuns.end(), queued.colorRuns.begin(), queued.colorRuns.end());
		}
		slabOffset += gpuJob.SlabCount;
		faceOffset += gpuJob.ExpectedFaces;
		colorRunOffset += (uint32_t)queued.colorRuns.size();
		if (!queued.directPublication)
		{
			vertexOffset += gpuJob.ExpectedVerticesNoDedupe;
			indexOffset += gpuJob.ExpectedIndices;
			primitiveOffset += gpuJob.ExpectedFaces * 2u;
			emittedVertexCount += gpuJob.ExpectedVerticesNoDedupe;
			emittedIndexCount += gpuJob.ExpectedIndices;
			emittedPrimitiveCount += gpuJob.ExpectedFaces * 2u;
		}
	}
	NRIVoxelComputeParallelDispatchPlan parallelPlan = {};
	if (parallelArchivedEmit)
	{
		std::vector<NRIVoxelComputeParallelJobShape> shapes;
		shapes.reserve(gpuJobs.size());
		for (const NRIVoxelComputeJob& gpuJob : gpuJobs)
		{
			shapes.push_back({ gpuJob.SlabCount, gpuJob.ExpectedFaces * 2u });
		}
		parallelPlan = BuildNRIVoxelComputeParallelDispatchPlan(shapes);
		if (parallelPlan.valid)
		{
			for (size_t jobIndex = 0; jobIndex < gpuJobs.size(); ++jobIndex)
			{
				gpuJobs[jobIndex].ScratchOffset = parallelPlan.jobs[jobIndex].scratchOffset;
				gpuJobs[jobIndex].ScratchCount = parallelPlan.jobs[jobIndex].scratchCount;
			}
		}
		else
		{
			parallelArchivedEmit = false;
		}
	}
	if (gpuJobs.empty() || (!directSource && gpuSlabs.empty()))
	{
		state.queuedJobs.erase(state.queuedJobs.begin(), state.queuedJobs.begin() + jobsToProcess);
		recordingGuard.abortReason = "empty-batch";
		return;
	}

	const uint64_t jobBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeJob);
	const uint64_t slabBytes = directSource ? (uint64_t)directArchivePage->slabs.size() * sizeof(NRIVoxelComputeSlabRecord) : (uint64_t)gpuSlabs.size() * sizeof(NRIVoxelComputeSlabRecord);
	const uint64_t faceBytes = directSource ? 0 : (uint64_t)gpuFaces.size() * sizeof(NRIVoxelComputeFaceRecord);
	const uint64_t colorRunBytes = directSource ? (uint64_t)directArchivePage->colorRuns.size() * sizeof(NRIVoxelComputeColorRunRecord) : (uint64_t)gpuColorRuns.size() * sizeof(NRIVoxelComputeColorRunRecord);
	const uint64_t resultBytes = (uint64_t)gpuJobs.size() * sizeof(NRIVoxelComputeResult);
	const uint64_t vertexBytes = (uint64_t)emittedVertexCount * sizeof(NRIVoxelComputeSceneVertex);
	const uint64_t indexBytes = (uint64_t)emittedIndexCount * sizeof(uint32_t);
	const uint64_t primitiveBytes = (uint64_t)emittedPrimitiveCount * sizeof(NRIVoxelComputePrimitiveData);
	const uint64_t scratchBytes = parallelArchivedEmit ?
		(uint64_t)parallelPlan.scratchRecordCount * sizeof(NRIVoxelComputeSlabScratch) : 0u;
	if (!EnsureBuffer(services, slot.jobUploadBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
		!EnsureBuffer(services, slot.jobBuffer, jobBytes, sizeof(NRIVoxelComputeJob), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
		(!directSource &&
			(!EnsureBuffer(services, slot.slabUploadBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, slot.slabBuffer, slabBytes, sizeof(NRIVoxelComputeSlabRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true) ||
			!EnsureBuffer(services, slot.colorRunUploadBuffer, colorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
			!EnsureBuffer(services, slot.colorRunBuffer, colorRunBytes, sizeof(NRIVoxelComputeColorRunRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true))) ||
		!EnsureBuffer(services, slot.resultBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
		(parallelArchivedEmit &&
			!EnsureBuffer(services, slot.slabScratchBuffer, scratchBytes, sizeof(NRIVoxelComputeSlabScratch), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true)) ||
		!EnsureBuffer(services, slot.readbackBuffer, resultBytes, sizeof(NRIVoxelComputeResult), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))
	{
		recordingGuard.abortReason = "core-buffer-prepare";
		return;
	}
	if (emit)
	{
		if ((!directSource && gpuFaces.empty()) ||
			(!directSource &&
				(!EnsureBuffer(services, slot.faceUploadBuffer, faceBytes, sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::NONE, nri::MemoryLocation::DEVICE_UPLOAD, nri::BufferView::STRUCTURED_BUFFER, false) ||
				!EnsureBuffer(services, slot.faceBuffer, faceBytes, sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true))) ||
			(directSource &&
				!EnsureBuffer(services, slot.faceBuffer, sizeof(NRIVoxelComputeFaceRecord), sizeof(NRIVoxelComputeFaceRecord), nri::BufferUsageBits::SHADER_RESOURCE, nri::MemoryLocation::DEVICE, nri::BufferView::STRUCTURED_BUFFER, true)) ||
			(!directOutput &&
				(!EnsureBuffer(services, slot.vertexBuffer, vertexBytes, sizeof(NRIVoxelComputeSceneVertex), NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
				!EnsureBuffer(services, slot.indexBuffer, indexBytes, sizeof(uint32_t), NRIResourceFlags(nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT), nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true) ||
				!EnsureBuffer(services, slot.primitiveBuffer, primitiveBytes, sizeof(NRIVoxelComputePrimitiveData), nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, nri::MemoryLocation::DEVICE, nri::BufferView::STORAGE_STRUCTURED_BUFFER, true))) ||
			(fullGeneratedReadback &&
				(!EnsureBuffer(services, slot.vertexReadbackBuffer, vertexBytes, sizeof(NRIVoxelComputeSceneVertex), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false) ||
				!EnsureBuffer(services, slot.indexReadbackBuffer, indexBytes, sizeof(uint32_t), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false) ||
				!EnsureBuffer(services, slot.primitiveReadbackBuffer, primitiveBytes, sizeof(NRIVoxelComputePrimitiveData), nri::BufferUsageBits::NONE, nri::MemoryLocation::HOST_READBACK, nri::BufferView::STRUCTURED_BUFFER, false))))
		{
			recordingGuard.abortReason = "emit-buffer-prepare";
			return;
		}
	}

	if (!CopyToUploadBuffer(context, slot.jobUploadBuffer, gpuJobs.data(), jobBytes) ||
		(!directSource &&
			(!CopyToUploadBuffer(context, slot.slabUploadBuffer, gpuSlabs.data(), slabBytes) ||
			!CopyToUploadBuffer(context, slot.colorRunUploadBuffer, gpuColorRuns.data(), colorRunBytes) ||
			(emit && !CopyToUploadBuffer(context, slot.faceUploadBuffer, gpuFaces.data(), faceBytes)))))
	{
		recordingGuard.abortReason = "upload-map";
		return;
	}
	state.queuedJobs.erase(state.queuedJobs.begin(), state.queuedJobs.begin() + jobsToProcess);
	for (const PendingReadbackJob& pending : pendingJobs)
	{
		if (pending.consumeKey != 0)
		{
			state.queuedConsumeKeys.erase(pending.consumeKey);
		}
		for (const PendingDirectPublishBinding& binding : pending.directBindings)
		{
			state.queuedDirectKeys.erase(binding.directKey);
		}
	}

	{
		NRIScopedGpuTiming uploadGpuTiming(renderer.mFrameBuffer, NRIGpuTimingScope::VoxelUpload);
		std::vector<nri::BufferBarrierDesc> uploadBarriers;
		uploadBarriers.resize(directSource ? 2 : (emit ? 8 : 6));
		uploadBarriers[0].buffer = slot.jobUploadBuffer.buffer;
		uploadBarriers[0].after = NRIResourceCopySourceAccess();
		uploadBarriers[1].buffer = slot.jobBuffer.buffer;
		uploadBarriers[1].after = NRIResourceCopyDestinationAccess();
		if (!directSource)
		{
			uploadBarriers[2].buffer = slot.slabUploadBuffer.buffer;
			uploadBarriers[2].after = NRIResourceCopySourceAccess();
			uploadBarriers[3].buffer = slot.slabBuffer.buffer;
			uploadBarriers[3].after = NRIResourceCopyDestinationAccess();
			uploadBarriers[4].buffer = slot.colorRunUploadBuffer.buffer;
			uploadBarriers[4].after = NRIResourceCopySourceAccess();
			uploadBarriers[5].buffer = slot.colorRunBuffer.buffer;
			uploadBarriers[5].after = NRIResourceCopyDestinationAccess();
			if (emit)
			{
				uploadBarriers[6].buffer = slot.faceUploadBuffer.buffer;
				uploadBarriers[6].after = NRIResourceCopySourceAccess();
				uploadBarriers[7].buffer = slot.faceBuffer.buffer;
				uploadBarriers[7].after = NRIResourceCopyDestinationAccess();
			}
		}
		nri::BarrierDesc uploadBarrier = {};
		uploadBarrier.buffers = uploadBarriers.data();
		uploadBarrier.bufferNum = (uint32_t)uploadBarriers.size();
		context.core->CmdBarrier(*context.commandBuffer, uploadBarrier);
		context.core->CmdCopyBuffer(*context.commandBuffer, *slot.jobBuffer.buffer, 0, *slot.jobUploadBuffer.buffer, 0, jobBytes);
		if (!directSource)
		{
			context.core->CmdCopyBuffer(*context.commandBuffer, *slot.slabBuffer.buffer, 0, *slot.slabUploadBuffer.buffer, 0, slabBytes);
			context.core->CmdCopyBuffer(*context.commandBuffer, *slot.colorRunBuffer.buffer, 0, *slot.colorRunUploadBuffer.buffer, 0, colorRunBytes);
			if (emit)
			{
				context.core->CmdCopyBuffer(*context.commandBuffer, *slot.faceBuffer.buffer, 0, *slot.faceUploadBuffer.buffer, 0, faceBytes);
			}
		}
	}

	const NRIBufferResource& emitVertexBuffer = directOutput ? *outputVertexBuffer : slot.vertexBuffer;
	const NRIBufferResource& emitIndexBuffer = directOutput ? *outputIndexBuffer : slot.indexBuffer;
	const NRIBufferResource& emitPrimitiveBuffer = directOutput ? *outputPrimitiveBuffer : slot.primitiveBuffer;

	std::vector<nri::BufferBarrierDesc> computeBarriers;
	computeBarriers.resize((directSource ? (emit ? 7 : 4) : (emit ? 8 : 4)) + (parallelArchivedEmit ? 1u : 0u));
	computeBarriers[0].buffer = slot.jobBuffer.buffer;
	computeBarriers[0].before = NRIResourceCopyDestinationAccess();
	computeBarriers[0].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[1].buffer = directSource ? directArchivePage->slabBuffer.buffer : slot.slabBuffer.buffer;
	computeBarriers[1].before = directSource ? NRIResourceComputeShaderResourceAccess() : NRIResourceCopyDestinationAccess();
	computeBarriers[1].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[2].buffer = directSource ? directArchivePage->colorRunBuffer.buffer : slot.colorRunBuffer.buffer;
	computeBarriers[2].before = directSource ? NRIResourceComputeShaderResourceAccess() : NRIResourceCopyDestinationAccess();
	computeBarriers[2].after = NRIResourceComputeShaderResourceAccess();
	computeBarriers[3].buffer = slot.resultBuffer.buffer;
	computeBarriers[3].before = NRIResourceCopySourceAccess();
	computeBarriers[3].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	if (emit)
	{
		computeBarriers[4].buffer = emitVertexBuffer.buffer;
		computeBarriers[4].before = directOutput ? NRIResourceComputeShaderResourceAccess() : (buildBlas ? NRIResourceAccelerationStructureBuildInputAccess() : NRIResourceCopySourceAccess());
		computeBarriers[4].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		computeBarriers[5].buffer = emitIndexBuffer.buffer;
		computeBarriers[5].before = directOutput ? NRIResourceComputeShaderResourceAccess() : (buildBlas ? NRIResourceAccelerationStructureBuildInputAccess() : NRIResourceCopySourceAccess());
		computeBarriers[5].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		computeBarriers[6].buffer = emitPrimitiveBuffer.buffer;
		computeBarriers[6].before = directOutput ? NRIResourceComputeShaderResourceAccess() : NRIResourceCopySourceAccess();
		computeBarriers[6].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		if (!directSource)
		{
			computeBarriers[7].buffer = slot.faceBuffer.buffer;
			computeBarriers[7].before = NRIResourceCopyDestinationAccess();
			computeBarriers[7].after = NRIResourceComputeShaderResourceAccess();
		}
	}
	if (parallelArchivedEmit)
	{
		nri::BufferBarrierDesc& scratchBarrier = computeBarriers.back();
		scratchBarrier.buffer = slot.slabScratchBuffer.buffer;
		scratchBarrier.before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		scratchBarrier.after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	}
	nri::BarrierDesc computeBarrier = {};
	computeBarrier.buffers = computeBarriers.data();
	computeBarrier.bufferNum = (uint32_t)computeBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, computeBarrier);

	const nri::Descriptor* inputDescriptors[2] = {
		slot.jobBuffer.shaderView,
		directSource ? directArchivePage->slabBuffer.shaderView : slot.slabBuffer.shaderView
	};
	nri::UpdateDescriptorRangeDesc inputUpdate = {};
	inputUpdate.descriptorSet = slot.inputSet;
	inputUpdate.rangeIndex = 0;
	inputUpdate.descriptors = inputDescriptors;
	inputUpdate.descriptorNum = 2;
	context.core->UpdateDescriptorRanges(&inputUpdate, 1);
	if (emit)
	{
		const nri::Descriptor* faceDescriptor[2] = {
			slot.faceBuffer.shaderView,
			directSource ? directArchivePage->colorRunBuffer.shaderView : slot.colorRunBuffer.shaderView
		};
		nri::UpdateDescriptorRangeDesc faceUpdate = {};
		faceUpdate.descriptorSet = slot.inputSet;
		faceUpdate.rangeIndex = 1;
		faceUpdate.descriptors = faceDescriptor;
		faceUpdate.descriptorNum = 2;
		context.core->UpdateDescriptorRanges(&faceUpdate, 1);
	}

	const nri::Descriptor* resultDescriptor[1] = { slot.resultBuffer.shaderView };
	nri::UpdateDescriptorRangeDesc resultUpdate = {};
	resultUpdate.descriptorSet = slot.outputSet;
	resultUpdate.rangeIndex = 0;
	resultUpdate.descriptors = resultDescriptor;
	resultUpdate.descriptorNum = 1;
	context.core->UpdateDescriptorRanges(&resultUpdate, 1);
	if (emit)
	{
		const nri::Descriptor* emitDescriptors[3] = {
			directOutput ? emitVertexBuffer.storageView : emitVertexBuffer.shaderView,
			directOutput ? emitIndexBuffer.storageView : emitIndexBuffer.shaderView,
			directOutput ? emitPrimitiveBuffer.storageView : emitPrimitiveBuffer.shaderView
		};
		nri::UpdateDescriptorRangeDesc emitUpdate = {};
		emitUpdate.descriptorSet = slot.outputSet;
		emitUpdate.rangeIndex = 1;
		emitUpdate.descriptors = emitDescriptors;
		emitUpdate.descriptorNum = 3;
		context.core->UpdateDescriptorRanges(&emitUpdate, 1);
	}
	if (parallelArchivedEmit)
	{
		const nri::Descriptor* scratchDescriptor[1] = { slot.slabScratchBuffer.shaderView };
		nri::UpdateDescriptorRangeDesc scratchUpdate = {};
		scratchUpdate.descriptorSet = slot.outputSet;
		scratchUpdate.rangeIndex = 2;
		scratchUpdate.descriptors = scratchDescriptor;
		scratchUpdate.descriptorNum = 1;
		context.core->UpdateDescriptorRanges(&scratchUpdate, 1);
	}

	NRIVoxelComputeConstants constants = {};
	constants.JobCount = (uint32_t)gpuJobs.size();
	constants.SlabRecordCount = directSource ? (uint32_t)directArchivePage->slabs.size() : (uint32_t)gpuSlabs.size();
	constants.FaceRecordCount = directSource ? 0u : (uint32_t)gpuFaces.size();
	constants.ColorRunRecordCount = directSource ? (uint32_t)directArchivePage->colorRuns.size() : 0u;
	constants.ScratchRecordCount = parallelArchivedEmit ? parallelPlan.scratchRecordCount : 0u;
	constants.MaxSlabsPerJob = parallelArchivedEmit ? parallelPlan.maxSlabsPerJob : 0u;
	constants.AlgorithmVersion = parallelArchivedEmit ? (uint32_t)NRIVoxelComputeAlgorithm::ParallelVoxelAdjacencyCoalescedV3 : (uint32_t)NRIVoxelComputeAlgorithm::SerialV1;
	constants.VertexRecordCount = emit ? (uint32_t)std::min<uint64_t>(emitVertexBuffer.size / sizeof(NRIVoxelComputeSceneVertex), UINT32_MAX) : 0u;
	constants.IndexRecordCount = emit ? (uint32_t)std::min<uint64_t>(emitIndexBuffer.size / sizeof(uint32_t), UINT32_MAX) : 0u;
	constants.PrimitiveRecordCount = emit ? (uint32_t)std::min<uint64_t>(emitPrimitiveBuffer.size / sizeof(NRIVoxelComputePrimitiveData), UINT32_MAX) : 0u;
	context.core->CmdSetPipelineLayout(*context.commandBuffer, nri::BindPoint::COMPUTE, *renderer.mVoxelComputePipelineLayout);
	context.core->CmdSetRootConstants(*context.commandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 0, slot.inputSet, nri::BindPoint::COMPUTE });
	context.core->CmdSetDescriptorSet(*context.commandBuffer, { 1, slot.outputSet, nri::BindPoint::COMPUTE });
	double classifyRecordMs = 0.0;
	double scanRecordMs = 0.0;
	double emitRecordMs = 0.0;
	double finalizeRecordMs = 0.0;
	double transitionRecordMs = 0.0;
	if (parallelArchivedEmit)
	{
		auto dispatchStage = [&](NRIRenderer::PipelineSlot slot, NRIGpuTimingScope timingScope, const char* annotation, const nri::DispatchDesc& dispatch, double& elapsedMs)
		{
			const auto start = std::chrono::steady_clock::now();
			NRIScopedGpuTiming stageGpuTiming(renderer.mFrameBuffer, timingScope);
			context.core->CmdBeginAnnotation(*context.commandBuffer, annotation, nri::BGRA_UNUSED);
			context.core->CmdSetPipeline(*context.commandBuffer, *renderer.GetPipeline(slot));
			context.core->CmdDispatch(*context.commandBuffer, dispatch);
			context.core->CmdEndAnnotation(*context.commandBuffer);
			elapsedMs = DurationMs(start, std::chrono::steady_clock::now());
		};
		auto storageBarrier = [&](nri::Buffer* const* buffers, uint32_t bufferCount)
		{
			const auto start = std::chrono::steady_clock::now();
			std::vector<nri::BufferBarrierDesc> barriers(bufferCount);
			for (uint32_t i = 0; i < bufferCount; ++i)
			{
				barriers[i].buffer = buffers[i];
				barriers[i].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
				barriers[i].after = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
			}
			nri::BarrierDesc barrier = {};
			barrier.buffers = barriers.data();
			barrier.bufferNum = bufferCount;
			context.core->CmdBarrier(*context.commandBuffer, barrier);
			transitionRecordMs += DurationMs(start, std::chrono::steady_clock::now());
		};

		dispatchStage(
			NRIRenderer::PipelineSlot::VoxelComputeClassify,
			NRIGpuTimingScope::VoxelClassify,
			"Raze.VoxelCompute.Classify",
			{ parallelPlan.classifyEmitGroupCountX, parallelPlan.jobCount, 1 },
			classifyRecordMs);
		nri::Buffer* classifyBuffers[] = { slot.slabScratchBuffer.buffer };
		storageBarrier(classifyBuffers, (uint32_t)std::size(classifyBuffers));
		dispatchStage(
			NRIRenderer::PipelineSlot::VoxelComputeScan,
			NRIGpuTimingScope::VoxelScan,
			"Raze.VoxelCompute.Scan",
			{ parallelPlan.jobCount, 1, 1 },
			scanRecordMs);
		nri::Buffer* scanBuffers[] = { slot.slabScratchBuffer.buffer, slot.resultBuffer.buffer };
		storageBarrier(scanBuffers, (uint32_t)std::size(scanBuffers));
		dispatchStage(
			NRIRenderer::PipelineSlot::VoxelComputeEmitParallel,
			NRIGpuTimingScope::VoxelEmit,
			"Raze.VoxelCompute.EmitParallel",
			{ parallelPlan.classifyEmitGroupCountX, parallelPlan.jobCount, 1 },
			emitRecordMs);
		nri::Buffer* emitBuffers[] = { slot.resultBuffer.buffer };
		storageBarrier(emitBuffers, (uint32_t)std::size(emitBuffers));
		dispatchStage(
			NRIRenderer::PipelineSlot::VoxelComputeFinalize,
			NRIGpuTimingScope::VoxelFinalize,
			"Raze.VoxelCompute.Finalize",
			{ parallelPlan.jobCount, 1, 1 },
			finalizeRecordMs);
	}
	else
	{
		const auto start = std::chrono::steady_clock::now();
		NRIScopedGpuTiming stageGpuTiming(
			renderer.mFrameBuffer,
			emit ? NRIGpuTimingScope::VoxelEmit : NRIGpuTimingScope::VoxelScan);
		context.core->CmdBeginAnnotation(
			*context.commandBuffer,
			emit ? "Raze.VoxelCompute.EmitSerial" : "Raze.VoxelCompute.CountSerial",
			nri::BGRA_UNUSED);
		context.core->CmdSetPipeline(*context.commandBuffer, *renderer.GetPipeline(emit ? NRIRenderer::PipelineSlot::VoxelComputeEmit : NRIRenderer::PipelineSlot::VoxelComputeCount));
		context.core->CmdDispatch(*context.commandBuffer, { (uint32_t)gpuJobs.size(), 1, 1 });
		context.core->CmdEndAnnotation(*context.commandBuffer);
		emitRecordMs = DurationMs(start, std::chrono::steady_clock::now());
	}
	if (IsTraceEnabled())
	{
		const bool requestedParallel = requestedAlgorithm > 0;
		const char* fallback = parallelArchivedEmit ? "none" :
			(!requestedParallel ? (requestedAlgorithm == (int)NRIVoxelComputeAlgorithm::SerialV1 ? "serial_requested" : "unsupported_version") :
				(directSource ? "invalid_parallel_plan" : "non_archive_work"));
		Printf(
			"PERF pt voxel compute stages NRI: frame=%llu algorithm=%s requested_version=%d active_version=%u fallback=%s jobs=%u scratch_records=%u scratch_bytes=%llu max_slabs_per_job=%u classify_dispatch=%u,%u,1 scan_dispatch=%u,1,1 emit_dispatch=%u,%u,1 finalize_dispatch=%u,1,1 classify_record_ms=%.6f scan_record_ms=%.6f emit_record_ms=%.6f finalize_record_ms=%.6f transition_record_ms=%.6f hashes=%s\n",
			(unsigned long long)frameNumber,
			parallelArchivedEmit ? "parallel_voxel_adjacency_coalesced_v3" : "serial_v1",
			requestedAlgorithm,
			parallelArchivedEmit ? (uint32_t)NRIVoxelComputeAlgorithm::ParallelVoxelAdjacencyCoalescedV3 : (uint32_t)NRIVoxelComputeAlgorithm::SerialV1,
			fallback,
			(uint32_t)gpuJobs.size(),
			parallelArchivedEmit ? parallelPlan.scratchRecordCount : 0u,
			(unsigned long long)scratchBytes,
			parallelArchivedEmit ? parallelPlan.maxSlabsPerJob : 0u,
			parallelArchivedEmit ? parallelPlan.classifyEmitGroupCountX : 0u,
			parallelArchivedEmit ? parallelPlan.jobCount : 0u,
			parallelArchivedEmit ? parallelPlan.jobCount : 0u,
			parallelArchivedEmit ? parallelPlan.classifyEmitGroupCountX : (uint32_t)gpuJobs.size(),
			parallelArchivedEmit ? parallelPlan.jobCount : 1u,
			parallelArchivedEmit ? parallelPlan.jobCount : 0u,
			classifyRecordMs,
			scanRecordMs,
			emitRecordMs,
			finalizeRecordMs,
			transitionRecordMs,
			parallelArchivedEmit ? "zero" : (fullGeneratedReadback ? "validation" : "serial"));
	}

	std::vector<nri::BufferBarrierDesc> readbackBarriers;
	readbackBarriers.resize((fullGeneratedReadback ? 4 : 1) + (directOutput ? 3 : 0));
	readbackBarriers[0].buffer = slot.resultBuffer.buffer;
	readbackBarriers[0].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
	readbackBarriers[0].after = NRIResourceCopySourceAccess();
	if (fullGeneratedReadback)
	{
		readbackBarriers[1].buffer = slot.vertexBuffer.buffer;
		readbackBarriers[1].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[1].after = NRIResourceCopySourceAccess();
		readbackBarriers[2].buffer = slot.indexBuffer.buffer;
		readbackBarriers[2].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[2].after = NRIResourceCopySourceAccess();
		readbackBarriers[3].buffer = slot.primitiveBuffer.buffer;
		readbackBarriers[3].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[3].after = NRIResourceCopySourceAccess();
	}
	if (directOutput)
	{
		const uint32_t directBarrierOffset = fullGeneratedReadback ? 4u : 1u;
		readbackBarriers[directBarrierOffset + 0u].buffer = emitVertexBuffer.buffer;
		readbackBarriers[directBarrierOffset + 0u].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[directBarrierOffset + 0u].after = NRIResourceComputeShaderResourceAccess();
		readbackBarriers[directBarrierOffset + 1u].buffer = emitIndexBuffer.buffer;
		readbackBarriers[directBarrierOffset + 1u].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[directBarrierOffset + 1u].after = NRIResourceComputeShaderResourceAccess();
		readbackBarriers[directBarrierOffset + 2u].buffer = emitPrimitiveBuffer.buffer;
		readbackBarriers[directBarrierOffset + 2u].before = { nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		readbackBarriers[directBarrierOffset + 2u].after = NRIResourceComputeShaderResourceAccess();
	}
	nri::BarrierDesc readbackBarrierDesc = {};
	readbackBarrierDesc.buffers = readbackBarriers.data();
	readbackBarrierDesc.bufferNum = (uint32_t)readbackBarriers.size();
	context.core->CmdBarrier(*context.commandBuffer, readbackBarrierDesc);
	context.core->CmdCopyBuffer(*context.commandBuffer, *slot.readbackBuffer.buffer, 0, *slot.resultBuffer.buffer, 0, resultBytes);
	if (fullGeneratedReadback)
	{
		context.core->CmdCopyBuffer(*context.commandBuffer, *slot.vertexReadbackBuffer.buffer, 0, *slot.vertexBuffer.buffer, 0, vertexBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *slot.indexReadbackBuffer.buffer, 0, *slot.indexBuffer.buffer, 0, indexBytes);
		context.core->CmdCopyBuffer(*context.commandBuffer, *slot.primitiveReadbackBuffer.buffer, 0, *slot.primitiveBuffer.buffer, 0, primitiveBytes);
	}

	constexpr uint32_t MaxDiagnosticBlasPrimitives = 8192;
	const bool allowDiagnosticBlas =
		buildBlas &&
		!directOutput &&
		state.diagnosticBlasBuildsSubmitted == 0 &&
		vertexOffset > 0 &&
		indexOffset > 0 &&
		primitiveOffset > 0 &&
		primitiveOffset <= MaxDiagnosticBlasPrimitives;
	if (allowDiagnosticBlas)
	{
		nri::BufferBarrierDesc blasBarriers[2] = {};
		blasBarriers[0].buffer = slot.vertexBuffer.buffer;
		blasBarriers[0].before = fullGeneratedReadback ? NRIResourceCopySourceAccess() : nri::AccessStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		blasBarriers[0].after = NRIResourceAccelerationStructureBuildInputAccess();
		blasBarriers[1].buffer = slot.indexBuffer.buffer;
		blasBarriers[1].before = fullGeneratedReadback ? NRIResourceCopySourceAccess() : nri::AccessStage{ nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER };
		blasBarriers[1].after = NRIResourceAccelerationStructureBuildInputAccess();
		nri::BarrierDesc blasBarrierDesc = {};
		blasBarrierDesc.buffers = blasBarriers;
		blasBarrierDesc.bufferNum = 2;
		context.core->CmdBarrier(*context.commandBuffer, blasBarrierDesc);

		for (PendingReadbackJob& job : pendingJobs)
		{
			job.admissionState = VoxelComputeAdmissionState::BlasBuilding;
			TraceAdmission(frameNumber, job, job.admissionState, "blas_build");
		}
		const bool blasOk = renderer.BuildBottomLevelAccelerationStructure(
			slot.vertexBuffer,
			slot.indexBuffer,
			0u,
			vertexOffset,
			0,
			indexOffset,
			primitiveOffset,
			state.diagnosticBlas,
			false);
		for (PendingReadbackJob& job : pendingJobs)
		{
			job.admissionState = blasOk ? VoxelComputeAdmissionState::BlasReady : VoxelComputeAdmissionState::Failed;
			TraceAdmission(frameNumber, job, job.admissionState, blasOk ? "blas_ready" : "blas_failed");
		}
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute blas NRI: frame=%llu jobs=%u success=%u vertices=%u indices=%u primitives=%u blas_bytes=%llu scratch_bytes=%llu direct_source=%u full_readback=%u\n",
				(unsigned long long)frameNumber,
				(unsigned)gpuJobs.size(),
				blasOk ? 1u : 0u,
				vertexOffset,
				indexOffset,
				primitiveOffset,
				(unsigned long long)state.diagnosticBlas.memorySize,
				(unsigned long long)state.diagnosticBlas.buildScratchSize,
				directSource ? 1u : 0u,
				fullGeneratedReadback ? 1u : 0u);
		}
		if (blasOk)
		{
			state.diagnosticBlasBuildsSubmitted++;
		}
	}
	else if (buildBlas && IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute blas NRI: frame=%llu jobs=%u success=0 skipped=1 reason=%s vertices=%u indices=%u primitives=%u max_primitives=%u submitted=%u direct_source=%u full_readback=%u\n",
			(unsigned long long)frameNumber,
			(unsigned)gpuJobs.size(),
			state.diagnosticBlasBuildsSubmitted != 0 ? "single_build_budget" : "primitive_budget",
			vertexOffset,
			indexOffset,
			primitiveOffset,
			MaxDiagnosticBlasPrimitives,
			state.diagnosticBlasBuildsSubmitted,
			directSource ? 1u : 0u,
			fullGeneratedReadback ? 1u : 0u);
	}

	slot.dispatchFrame = frameNumber;
	slot.rawArchiveGeneration = directSource ? (uint64_t)directArchivePageIndex + 1u : 0u;
	slot.statusBytes = resultBytes;
	slot.fullGeometryBytes = fullGeneratedReadback ? vertexBytes + indexBytes + primitiveBytes : 0u;
	slot.pendingVertexCount = emittedVertexCount;
	slot.pendingIndexCount = emittedIndexCount;
	slot.pendingPrimitiveCount = emittedPrimitiveCount;
	slot.emit = emit;
	slot.buildBlas = buildBlas;
	slot.fullGeneratedReadback = fullGeneratedReadback;
	slot.directProduction = parallelArchivedEmit && directOutput && !fullGeneratedReadback;
	assert(!slot.directProduction || (slot.fullGeometryBytes == 0 && state.completionHostWaits == 0));
	slot.outputVertexBuffer = outputVertexBuffer;
	slot.outputIndexBuffer = outputIndexBuffer;
	slot.outputPrimitiveBuffer = outputPrimitiveBuffer;
	slot.pendingJobs = std::move(pendingJobs);
	if (!state.completionRing.Submit(slotIndex, recordingFenceValue))
	{
		recordingGuard.abortReason = "ring-submit";
		FailCompletionSlotJobs(state, slot);
		return;
	}
	recordingGuard.submitted = true;
	for (const PendingReadbackJob& job : slot.pendingJobs)
	{
		if (job.consumeKey != 0)
		{
			state.pendingConsumeKeys.insert(job.consumeKey);
		}
		if (job.directPublication)
		{
			for (const PendingDirectPublishBinding& binding : job.directBindings)
			{
				state.pendingDirectKeys.insert(binding.directKey);
			}
		}
	}
	if (directSource)
	{
		uint32_t materialBindings = 0;
		for (const PendingReadbackJob& job : slot.pendingJobs)
		{
			materialBindings += (uint32_t)job.directBindings.size();
		}
		const uint64_t reservationBytes = vertexBytes + indexBytes + primitiveBytes;
		state.directBatchDispatches++;
		state.directBatchJobsDispatched += gpuJobs.size();
		state.directBatchReservationBytes += reservationBytes;
		const uint64_t batchByteLimit = (uint64_t)std::max(0, (int)nri_ptvoxeladmitmaxbytesruntime);
		const bool oversizedExclusive = batchByteLimit != 0 && gpuJobs.size() == 1 && reservationBytes > batchByteLimit;
		state.directBatchOversizedExclusive += oversizedExclusive ? 1u : 0u;
		if (IsTraceEnabled())
		{
			Printf(
				"PERF pt voxel compute batch dispatch NRI: frame=%llu input_requests=%u unique_jobs=%u material_bindings=%u jobs_per_batch=%u bytes_per_batch=%llu archive_pages=1 archive_descriptors=2 oversized_exclusive=%u command_lists=1 submits=1 queued_remaining=%u cumulative_requests=%llu cumulative_jobs=%llu cumulative_dedupe_hits=%llu cumulative_bindings=%llu cumulative_raw_stat_scans_avoided=%llu cumulative_dispatches=%llu cumulative_dispatch_jobs=%llu cumulative_bytes=%llu\n",
				(unsigned long long)frameNumber,
				materialBindings,
				(uint32_t)gpuJobs.size(),
				materialBindings,
				(uint32_t)gpuJobs.size(),
				(unsigned long long)reservationBytes,
				oversizedExclusive ? 1u : 0u,
				(uint32_t)state.queuedJobs.size(),
				(unsigned long long)state.directBatchInputRequests,
				(unsigned long long)state.directBatchUniqueJobs,
				(unsigned long long)state.directBatchDedupeHits,
				(unsigned long long)state.directBatchMaterialBindings,
				(unsigned long long)state.directBatchRawStatScansAvoided,
				(unsigned long long)state.directBatchDispatches,
				(unsigned long long)state.directBatchJobsDispatched,
				(unsigned long long)state.directBatchReservationBytes);
		}
	}
	if (IsTraceEnabled())
	{
		const NRIVoxelComputeCompletionSlotToken& token = state.completionRing.slots[slotIndex];
		Printf(
			"PERF pt voxel compute completion NRI: action=submit submission=%llu slot=%u fence=%llu level_generation=%llu archive_generation=%llu frame=%llu jobs=%u occupancy=%u high_water=%u backpressure=%llu status_bytes=%llu full_geometry_bytes=%llu host_waits=%llu\n",
			(unsigned long long)token.submissionId, slotIndex, (unsigned long long)token.fenceValue,
			(unsigned long long)token.levelGeneration, (unsigned long long)slot.rawArchiveGeneration,
			(unsigned long long)frameNumber, (uint32_t)slot.pendingJobs.size(),
			state.completionRing.occupancy, state.completionRing.highWater,
			(unsigned long long)state.completionRing.backpressureCount,
			(unsigned long long)slot.statusBytes, (unsigned long long)slot.fullGeometryBytes,
			(unsigned long long)state.completionHostWaits);
	}
	if (IsTraceEnabled())
	{
		Printf(
			"PERF pt voxel compute dispatch NRI: frame=%llu mode=%s algorithm=%s source=%s jobs=%u slab_records=%u color_run_records=%u face_records=%u scratch_records=%u job_bytes=%llu slab_bytes=%llu color_run_bytes=%llu face_bytes=%llu scratch_bytes=%llu result_bytes=%llu vertex_bytes=%llu index_bytes=%llu primitive_bytes=%llu production_readback_bytes=%llu validation_readback_bytes=%llu direct_gpu=%u raw_archive=%u direct_publish=%u\n",
			(unsigned long long)frameNumber,
			emit ? (buildBlas ? "emit_blas" : "emit") : "count",
			parallelArchivedEmit ? "parallel_voxel_adjacency_coalesced_v3" : "serial_v1",
			directSource ? "archive_decode" : (emit ? "face_records" : "slab_count"),
			(unsigned)gpuJobs.size(),
			directSource ? (uint32_t)directArchivePage->slabs.size() : (uint32_t)gpuSlabs.size(),
			directSource ? (uint32_t)directArchivePage->colorRuns.size() : 0u,
			directSource ? 0u : (uint32_t)gpuFaces.size(),
			parallelArchivedEmit ? parallelPlan.scratchRecordCount : 0u,
			(unsigned long long)jobBytes,
			(unsigned long long)slabBytes,
			(unsigned long long)colorRunBytes,
			(unsigned long long)faceBytes,
			(unsigned long long)scratchBytes,
			(unsigned long long)resultBytes,
			(unsigned long long)vertexBytes,
			(unsigned long long)indexBytes,
			(unsigned long long)primitiveBytes,
			(unsigned long long)resultBytes,
			(unsigned long long)(fullGeneratedReadback ? vertexBytes + indexBytes + primitiveBytes : 0),
			IsDirectGpuPublicationEnabled() ? 1u : 0u,
			IsRawSourceArchiveEnabled() ? 1u : 0u,
			directOutput ? 1u : 0u);
	}
}

void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer)
{
	NRIResourceServices services = renderer.BuildResourceServices();
	VoxelComputeState& state = gVoxelComputeState;
	for (RawVoxelSourceArchivePage& page : state.rawArchivePages)
	{
		services.DestroyBufferResource(page.slabUploadBuffer);
		services.DestroyBufferResource(page.colorRunUploadBuffer);
		services.DestroyBufferResource(page.slabBuffer);
		services.DestroyBufferResource(page.colorRunBuffer);
	}
	for (VoxelComputeCompletionSlot& slot : state.completionSlots)
	{
		services.DestroyBufferResource(slot.jobUploadBuffer);
		services.DestroyBufferResource(slot.slabUploadBuffer);
		services.DestroyBufferResource(slot.faceUploadBuffer);
		services.DestroyBufferResource(slot.colorRunUploadBuffer);
		services.DestroyBufferResource(slot.jobBuffer);
		services.DestroyBufferResource(slot.slabBuffer);
		services.DestroyBufferResource(slot.faceBuffer);
		services.DestroyBufferResource(slot.colorRunBuffer);
		services.DestroyBufferResource(slot.resultBuffer);
		services.DestroyBufferResource(slot.vertexBuffer);
		services.DestroyBufferResource(slot.indexBuffer);
		services.DestroyBufferResource(slot.primitiveBuffer);
		services.DestroyBufferResource(slot.slabScratchBuffer);
		services.DestroyBufferResource(slot.readbackBuffer);
		services.DestroyBufferResource(slot.vertexReadbackBuffer);
		services.DestroyBufferResource(slot.indexReadbackBuffer);
		services.DestroyBufferResource(slot.primitiveReadbackBuffer);
	}
	renderer.DestroyAccelerationStructureResource(state.diagnosticBlas);
	state = {};
}
