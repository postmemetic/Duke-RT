#ifndef RAZE_NRI_VOXEL_COMPUTE_CONSTANTS_HLSLI
#define RAZE_NRI_VOXEL_COMPUTE_CONSTANTS_HLSLI

#include "NRI.hlsl"

#define NRI_VOXEL_COMPUTE_SET_INPUTS 0
#define NRI_VOXEL_COMPUTE_SET_OUTPUTS 1
#define NRI_VOXEL_COMPUTE_SET_ROOT 2
#define NRI_VOXEL_COMPUTE_ROOT_REGISTER 0
#define NRI_VOXEL_COMPUTE_STATUS_COUNT_OK 1u
#define NRI_VOXEL_COMPUTE_STATUS_COUNT_MISMATCH 2u
#define NRI_VOXEL_COMPUTE_STATUS_EMIT_OK 3u
#define NRI_VOXEL_COMPUTE_STATUS_EMIT_MISMATCH 4u
#define NRI_VOXEL_COMPUTE_STATUS_SCAN_OK 5u
#define NRI_VOXEL_COMPUTE_STATUS_SCAN_MISMATCH 6u
#define NRI_VOXEL_COMPUTE_MISMATCH_OUTPUT_OVERFLOW (1u << 5u)
#define NRI_VOXEL_COMPUTE_MISMATCH_SOURCE_RANGE (1u << 6u)
#define NRI_VOXEL_COMPUTE_MISMATCH_COLOR_RUN_RANGE (1u << 7u)
#define NRI_VOXEL_COMPUTE_MISMATCH_SCRATCH_RANGE (1u << 8u)
#define NRI_VOXEL_COMPUTE_MISMATCH_ARITHMETIC_OVERFLOW (1u << 9u)
#define NRI_VOXEL_COMPUTE_MISMATCH_SLAB_EMIT_COUNT (1u << 10u)
#define NRI_VOXEL_COMPUTE_MISMATCH_ALGORITHM (1u << 11u)

struct NRIVoxelComputeConstants
{
	uint JobCount;
	uint SlabRecordCount;
	uint FaceRecordCount;
	uint ColorRunRecordCount;
	uint ScratchRecordCount;
	uint MaxSlabsPerJob;
	uint AlgorithmVersion;
	uint Reserved0;
	uint VertexRecordCount;
	uint IndexRecordCount;
	uint PrimitiveRecordCount;
	uint Reserved1;
};

struct NRIVoxelComputeJob
{
	uint SlabOffset;
	uint SlabCount;
	uint FaceOffset;
	uint ExpectedFaces;
	uint ExpectedIndices;
	uint ExpectedVerticesNoDedupe;
	uint ExpectedVoxels;
	uint JobId;
	uint VertexOffset;
	uint IndexOffset;
	uint PrimitiveOffset;
	float PivotX;
	float PivotY;
	float PivotZ;
	uint MaterialBase;
	uint VertexCapacity;
	uint IndexCapacity;
	uint PrimitiveCapacity;
	uint ScratchOffset;
	uint ScratchCount;
};

struct NRIVoxelComputeSlabRecord
{
	uint X;
	uint Y;
	uint ZTop;
	uint CullMask;
	uint ZLength;
	uint ColorRunCount;
	uint ColorRunOffset;
	uint Reserved0;
};

struct NRIVoxelComputeColorRunRecord
{
	uint ZOffset;
	uint ZLength;
	uint Color;
	uint Reserved0;
};

struct NRIVoxelComputeResult
{
	uint FaceCount;
	uint IndexCount;
	uint VertexCountNoDedupe;
	uint VoxelCount;
	uint SlabCount;
	uint PrimitiveCount;
	uint MismatchMask;
	uint JobId;
	uint Status;
	uint VertexHash;
	uint IndexHash;
	uint PrimitiveHash;
};

struct NRIVoxelComputeSlabScratch
{
	uint FaceCount;
	uint FaceOffset;
	uint VoxelCount;
	uint StatusMask;
};

struct NRIVoxelComputeFaceRecord
{
	int X[4];
	int Y[4];
	int Z[4];
	uint Color;
	uint MaterialIndex;
};

struct NRIVoxelComputeSceneVertex
{
	float3 Position;
	float3 PrevPosition;
	float2 Uv;
};

struct NRIVoxelComputePrimitiveData
{
	uint3 Indices;
	uint MaterialIndex;
	float2 Uv0;
	float2 Uv1;
	float2 Uv2;
	float3 Normal;
	uint Flags;
	uint PortalIndex;
	uint Reserved0;
	uint2 SmoothNormals;
};

NRI_ROOT_CONSTANTS(NRIVoxelComputeConstants, gVoxelComputeConstants, NRI_VOXEL_COMPUTE_ROOT_REGISTER, NRI_VOXEL_COMPUTE_SET_ROOT);

#endif
