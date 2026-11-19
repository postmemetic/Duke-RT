#pragma once

#include "tarray.h"

#include <cstdint>
#include <vector>

class FVoxelModel;
class NRIRenderer;
struct NRIBufferResource;
struct FVoxelRawMeshStats;
struct FVoxelRawSlabRecord;
struct FVoxelRawFaceRecord;
struct FVoxelRawColorRunRecord;
struct FVoxelMeshData;
namespace nri_scene { struct GeometryData; }

enum class NRIVoxelComputeGeneratedGeometryStatus : uint8_t
{
	Unavailable,
	Queued,
	Ready,
	Failed,
};

enum class NRIVoxelComputeDirectPublishOutputKind : uint8_t
{
	None,
	SharedPersistentArena,
	PrivateBlasInputsAndSharedArena,
	PrivateBuffers,
};

enum class NRIVoxelComputeDirectPublishFailure : uint8_t
{
	None,
	Disabled,
	InvalidRequest,
	UnsupportedOutputKind,
	QueueFull,
	PrimitiveBudget,
	AllocationUnavailable,
	DispatchFailed,
	StatusFailed,
	StaleGeneration,
	Cancelled,
};

struct NRIVoxelComputeDirectPublishRange
{
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t capacity = 0;
};

struct NRIVoxelComputeDirectPublishBounds
{
	float min[3] = {};
	float max[3] = {};
	bool valid = false;
};

struct NRIVoxelComputeDirectPublishOutputBuffers
{
	const NRIBufferResource* vertices = nullptr;
	const NRIBufferResource* indices = nullptr;
	const NRIBufferResource* primitives = nullptr;
};

struct NRIVoxelComputeDirectPublishRequest
{
	uint64_t overflowGeneration = 0;
	uint64_t meshResourceKey = 0;
	uint64_t geometryKey = 0;
	uint64_t materialBindingKey = 0;
	uint64_t generation = 0;
	FVoxelModel* model = nullptr;
	NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::PrivateBlasInputsAndSharedArena;
	NRIVoxelComputeDirectPublishOutputBuffers outputBuffers;
	NRIVoxelComputeDirectPublishRange vertices;
	NRIVoxelComputeDirectPublishRange indices;
	NRIVoxelComputeDirectPublishRange primitives;
	uint32_t materialBase = 0;
	uint32_t materialCount = 0;
	uint32_t priority = 0;
	uint64_t age = 0;
	bool validationReadbackAllowed = false;
};

struct NRIVoxelComputeDirectPublishBatchResult
{
	NRIVoxelComputeGeneratedGeometryStatus status = NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	NRIVoxelComputeDirectPublishFailure failure = NRIVoxelComputeDirectPublishFailure::None;
	uint32_t inputRequests = 0;
	uint32_t uniqueJobs = 0;
	uint32_t dedupeHits = 0;
	uint32_t materialBindings = 0;
	uint32_t repeatedRawStatScansAvoided = 0;
};

struct NRIVoxelComputeDirectPublishedMesh
{
	uint64_t overflowGeneration = 0;
	uint64_t meshResourceKey = 0;
	uint64_t generation = 0;
	uint64_t sourceArchiveSerial = 0;
	uint32_t jobId = 0;
	uint32_t readyFrame = 0;
	NRIVoxelComputeGeneratedGeometryStatus status = NRIVoxelComputeGeneratedGeometryStatus::Unavailable;
	NRIVoxelComputeDirectPublishFailure failure = NRIVoxelComputeDirectPublishFailure::None;
	NRIVoxelComputeDirectPublishOutputKind outputKind = NRIVoxelComputeDirectPublishOutputKind::None;
	NRIVoxelComputeDirectPublishRange vertices;
	NRIVoxelComputeDirectPublishRange indices;
	NRIVoxelComputeDirectPublishRange primitives;
	uint32_t materialBase = 0;
	uint32_t materialCount = 0;
	NRIVoxelComputeDirectPublishBounds bounds;
	float surfaceArea = 0.0f;
};

struct NRIVoxelComputeRawSourcePreloadStats
{
	uint32_t requested = 0;
	uint32_t recorded = 0;
	uint32_t alreadyResident = 0;
	uint32_t skipped = 0;
	uint32_t failed = 0;
	uint32_t slabRecords = 0;
	uint32_t colorRunRecords = 0;
	uint64_t rawBytes = 0;
	uint64_t uploadBytes = 0;
	double buildMs = 0.0;
};

// Immutable CPU copy of the already-decoded raw archive records. Optional
// consumers must use this snapshot instead of rescanning FVoxelModel data.
struct NRIVoxelComputeRawSlabSnapshot
{
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t zTop = 0;
	uint32_t cullMask = 0;
	uint32_t zLength = 0;
};

struct NRIVoxelComputeRawSourceArchiveSnapshot
{
	uint64_t recordSerial = 0;
	uint64_t contentHash = 0;
	uint32_t sizeX = 0;
	uint32_t sizeY = 0;
	uint32_t sizeZ = 0;
	uint32_t exactFaceCount = 0;
	uint32_t exactPrimitiveCount = 0;
	float pivotX = 0.0f;
	float pivotY = 0.0f;
	float pivotZ = 0.0f;
	std::vector<NRIVoxelComputeRawSlabSnapshot> slabs;
};

struct NRIVoxelComputeMemoryUsage
{
	uint32_t rawSourceCount = 0;
	uint32_t rawSourceUploadedCount = 0;
	uint32_t rawSourcePendingCount = 0;
	uint32_t rawArchivePageCount = 0;
	uint32_t queuedJobCount = 0;
	uint32_t pendingJobCount = 0;
	uint32_t readyDirectMeshCount = 0;
	uint64_t rawCpuBytes = 0;
	uint64_t rawDeviceBytes = 0;
	uint64_t rawUploadBytes = 0;
	uint64_t totalRawSourceScans = 0;
	uint64_t totalRawSourceScanFailures = 0;
	uint64_t totalRawSourceNewUploadBytes = 0;
	uint64_t totalRawSourceWholeArchiveReuploads = 0;
	uint64_t rawSourceUploadLatencyMaxFrames = 0;
	double totalRawSourceScanMs = 0.0;
	double rawSourceScanMaxMs = 0.0;
	double rawSourceUploadLatencyAverageFrames = 0.0;
	uint64_t transientInputDeviceBytes = 0;
	uint64_t transientInputUploadBytes = 0;
	uint64_t transientGeneratedBytes = 0;
	uint64_t statusReadbackBytes = 0;
	uint64_t geometryReadbackBufferBytes = 0;
	uint64_t diagnosticAsBytes = 0;
	uint64_t totalStatusReadbackBytes = 0;
	uint64_t totalFullGeometryReadbackBytes = 0;
};

bool ShouldTraceNRIVoxelComputeMeshing();
bool ShouldRunNRIVoxelComputeMeshing();
bool ShouldEmitNRIVoxelComputeMeshing();
bool ShouldConsumeNRIVoxelComputeMeshing();
bool ShouldDirectPublishNRIVoxelComputeMeshing();
void QueueNRIVoxelComputeCountJob(
	FVoxelModel* model,
	const FVoxelRawMeshStats& stats,
	const TArray<FVoxelRawSlabRecord>* slabs,
	const TArray<FVoxelRawFaceRecord>* faces,
	const TArray<FVoxelRawColorRunRecord>* colorRuns,
	const FVoxelMeshData& cpuMesh);
NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, FVoxelModel* model);
bool TakeNRIVoxelComputeGeneratedGeometry(uint64_t requestKey, nri_scene::GeometryData& outGeometry, uint32_t* outJobId = nullptr);
bool RequestNRIVoxelComputeDirectPublicationBatch(
	const NRIVoxelComputeDirectPublishRequest* requests,
	uint32_t requestCount,
	NRIVoxelComputeDirectPublishBatchResult& outResult);
NRIVoxelComputeGeneratedGeometryStatus RequestNRIVoxelComputeDirectPublication(const NRIVoxelComputeDirectPublishRequest& request);
bool TakeNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation, NRIVoxelComputeDirectPublishedMesh& outMesh);
void CancelNRIVoxelComputeDirectPublication(uint64_t meshResourceKey, uint64_t generation);
// Consumes a bounded provider-budget retry signal; never marks source failure.
bool TakeNRIVoxelComputeBudgetDeferral(uint64_t meshResourceKey, uint64_t generation);
bool QueryNRIVoxelComputeRawSourceArchiveStats(FVoxelModel* model, FVoxelRawMeshStats& outStats);
bool CopyNRIVoxelComputeRawSourceArchiveSnapshot(
	FVoxelModel* model,
	NRIVoxelComputeRawSourceArchiveSnapshot& outSnapshot);
bool QueryNRIVoxelComputeRawSourceStats(FVoxelModel* model, FVoxelRawMeshStats& outStats);
bool PreloadNRIVoxelComputeRawSource(FVoxelModel* model, NRIVoxelComputeRawSourcePreloadStats* outStats = nullptr);
NRIVoxelComputeMemoryUsage GetNRIVoxelComputeMemoryUsage();
uint32_t GetNRIVoxelComputeQueuedJobCount();
void DispatchNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer, uint64_t frameNumber);
void DestroyNRIVoxelComputeMeshingDiagnostics(NRIRenderer& renderer);
