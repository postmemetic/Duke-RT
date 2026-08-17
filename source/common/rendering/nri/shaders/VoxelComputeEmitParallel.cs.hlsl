#include "Include/VoxelComputeConstants.hlsli"
#include "Include/VoxelNormals.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
StructuredBuffer<NRIVoxelComputeSlabRecord> VoxelComputeSlabs : register(t1, space0);
StructuredBuffer<NRIVoxelComputeColorRunRecord> VoxelComputeColorRuns : register(t3, space0);
RWStructuredBuffer<NRIVoxelComputeResult> VoxelComputeResults : register(u0, space1);
RWStructuredBuffer<NRIVoxelComputeSceneVertex> VoxelComputeVertices : register(u1, space1);
RWStructuredBuffer<uint> VoxelComputeIndices : register(u2, space1);
RWStructuredBuffer<NRIVoxelComputePrimitiveData> VoxelComputePrimitives : register(u3, space1);
RWStructuredBuffer<NRIVoxelComputeSlabScratch> VoxelComputeScratch : register(u4, space1);

float2 VoxelUv(uint color)
{
	return float2(((color & 15u) + 0.5f) / 16.0f, ((color >> 4u) + 0.5f) / 16.0f);
}

float3 TransformVoxelPoint(NRIVoxelComputeJob job, int x, int y, int z)
{
	return float3(float(x) - job.PivotX, -(float(z) - job.PivotZ), -(float(y) - job.PivotY));
}

NRIVoxelComputeSceneVertex MakeVertex(float3 position, float2 uv)
{
	NRIVoxelComputeSceneVertex vertex;
	vertex.Position = position;
	vertex.PrevPosition = position;
	vertex.Uv = uv;
	return vertex;
}

NRIVoxelComputePrimitiveData MakePrimitive(uint3 indices, uint materialIndex, float2 uv0, float2 uv1, float2 uv2, float3 normal, uint3 smoothNormals)
{
	NRIVoxelComputePrimitiveData primitive;
	primitive.Indices = indices;
	primitive.MaterialIndex = materialIndex;
	primitive.Uv0 = uv0;
	primitive.Uv1 = uv1;
	primitive.Uv2 = uv2;
	primitive.Normal = normal;
	primitive.Flags = 0u;
	primitive.PortalIndex = 0xffffffffu;
	primitive.Reserved0 = 0xffffffffu;
	primitive.SmoothNormals = uint2(
		smoothNormals.x | (smoothNormals.y << 16u),
		smoothNormals.z | 0x80000000u);
	return primitive;
}

bool EmitFace(
	NRIVoxelComputeJob job,
	int4 x,
	int4 y,
	int4 z,
	uint color,
	uint materialIndex,
	NRIVoxelComputeSlabRecord slab,
	int voxelZ,
	uint faceCullBit,
	inout uint emittedFaces)
{
	const uint localVertexBase = emittedFaces * 4u;
	const uint localIndexBase = emittedFaces * 6u;
	const uint localPrimitiveBase = emittedFaces * 2u;
	if (localVertexBase > job.VertexCapacity || job.VertexCapacity - localVertexBase < 4u ||
		localIndexBase > job.IndexCapacity || job.IndexCapacity - localIndexBase < 6u ||
		localPrimitiveBase > job.PrimitiveCapacity || job.PrimitiveCapacity - localPrimitiveBase < 2u)
	{
		return false;
	}

	const float3 p0 = TransformVoxelPoint(job, x.x, y.x, z.x);
	const float3 p1 = TransformVoxelPoint(job, x.y, y.y, z.y);
	const float3 p2 = TransformVoxelPoint(job, x.w, y.w, z.w);
	const float3 p3 = TransformVoxelPoint(job, x.z, y.z, z.z);
	const float3 rawNormal = cross(p1 - p0, p3 - p0);
	const float normalLengthSq = dot(rawNormal, rawNormal);
	const float3 faceNormal = normalLengthSq > 1.0e-12f ? rawNormal * rsqrt(normalLengthSq) : float3(0.0f, 1.0f, 0.0f);
	const uint shadingNormal = BuildVoxelSurfaceNormal(voxelZ, faceCullBit, slab);
	const float2 uv = VoxelUv(color);
	const uint vertexBase = job.VertexOffset + localVertexBase;
	const uint indexBase = job.IndexOffset + localIndexBase;
	const uint primitiveBase = job.PrimitiveOffset + localPrimitiveBase;
	VoxelComputeVertices[vertexBase + 0u] = MakeVertex(p0, uv);
	VoxelComputeVertices[vertexBase + 1u] = MakeVertex(p1, uv);
	VoxelComputeVertices[vertexBase + 2u] = MakeVertex(p2, uv);
	VoxelComputeVertices[vertexBase + 3u] = MakeVertex(p3, uv);

	const uint localIndices[6] = {
		localVertexBase + 0u,
		localVertexBase + 1u,
		localVertexBase + 3u,
		localVertexBase + 1u,
		localVertexBase + 2u,
		localVertexBase + 3u
	};
	const uint primitiveIndices[6] = {
		vertexBase + 0u,
		vertexBase + 1u,
		vertexBase + 3u,
		vertexBase + 1u,
		vertexBase + 2u,
		vertexBase + 3u
	};
	[unroll]
	for (uint i = 0u; i < 6u; ++i)
	{
		VoxelComputeIndices[indexBase + i] = localIndices[i];
	}

	VoxelComputePrimitives[primitiveBase + 0u] = MakePrimitive(uint3(primitiveIndices[0], primitiveIndices[1], primitiveIndices[2]), materialIndex, uv, uv, uv, faceNormal, uint3(shadingNormal, shadingNormal, shadingNormal));
	VoxelComputePrimitives[primitiveBase + 1u] = MakePrimitive(uint3(primitiveIndices[3], primitiveIndices[4], primitiveIndices[5]), materialIndex, uv, uv, uv, faceNormal, uint3(shadingNormal, shadingNormal, shadingNormal));
	++emittedFaces;
	return true;
}

bool EmitVoxelSideSpan(
	NRIVoxelComputeJob job,
	int x,
	int y,
	int z0,
	int z1,
	uint color,
	uint faceCullBit,
	NRIVoxelComputeSlabRecord slab,
	inout uint emittedFaces)
{
	if (faceCullBit == 1u)
	{
		return EmitFace(job, int4(x, x, x, x), int4(y, y + 1, y, y + 1), int4(z0, z0, z1, z1), color, color, slab, z0, faceCullBit, emittedFaces);
	}
	if (faceCullBit == 2u)
	{
		return EmitFace(job, int4(x + 1, x + 1, x + 1, x + 1), int4(y + 1, y, y + 1, y), int4(z0, z0, z1, z1), color, color, slab, z0, faceCullBit, emittedFaces);
	}
	if (faceCullBit == 4u)
	{
		return EmitFace(job, int4(x + 1, x, x + 1, x), int4(y, y, y, y), int4(z0, z0, z1, z1), color, color, slab, z0, faceCullBit, emittedFaces);
	}
	return EmitFace(job, int4(x, x + 1, x, x + 1), int4(y + 1, y + 1, y + 1, y + 1), int4(z0, z0, z1, z1), color, color, slab, z0, faceCullBit, emittedFaces);
}

bool EmitVoxelSideSpansForRun(
	NRIVoxelComputeJob job,
	int x,
	int y,
	NRIVoxelComputeSlabRecord slab,
	NRIVoxelComputeColorRunRecord run,
	uint faceCullBit,
	inout uint emittedFaces)
{
	NRIVoxelAdjacencySpan spans[3];
	const uint spanCount = BuildVoxelAdjacencyNormalSpans(slab, run, faceCullBit, spans);
	for (uint spanIndex = 0u; spanIndex < spanCount; ++spanIndex)
	{
		const int z0 = int(slab.ZTop + spans[spanIndex].ZOffset);
		const int z1 = z0 + int(spans[spanIndex].ZLength);
		if (!EmitVoxelSideSpan(job, x, y, z0, z1, run.Color, faceCullBit, slab, emittedFaces))
		{
			return false;
		}
	}
	return true;
}

void MarkMismatch(uint jobIndex, uint mismatch)
{
	uint unused;
	InterlockedOr(VoxelComputeResults[jobIndex].MismatchMask, mismatch, unused);
}

[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint localSlab = dispatchThreadId.x;
	const uint jobIndex = dispatchThreadId.y;
	if (jobIndex >= gVoxelComputeConstants.JobCount)
	{
		return;
	}

	const NRIVoxelComputeJob job = VoxelComputeJobs[jobIndex];
	if (localSlab >= job.SlabCount || localSlab >= job.ScratchCount)
	{
		return;
	}
	const NRIVoxelComputeResult scanResult = VoxelComputeResults[jobIndex];
	if (scanResult.Status != NRI_VOXEL_COMPUTE_STATUS_SCAN_OK || scanResult.MismatchMask != 0u)
	{
		return;
	}
	if (job.ScratchOffset > gVoxelComputeConstants.ScratchRecordCount ||
		localSlab >= gVoxelComputeConstants.ScratchRecordCount - job.ScratchOffset ||
		job.SlabOffset > gVoxelComputeConstants.SlabRecordCount ||
		localSlab >= gVoxelComputeConstants.SlabRecordCount - job.SlabOffset)
	{
		MarkMismatch(jobIndex, NRI_VOXEL_COMPUTE_MISMATCH_SCRATCH_RANGE | NRI_VOXEL_COMPUTE_MISMATCH_SOURCE_RANGE);
		return;
	}

	const NRIVoxelComputeSlabScratch scratch = VoxelComputeScratch[job.ScratchOffset + localSlab];
	const NRIVoxelComputeSlabRecord slab = VoxelComputeSlabs[job.SlabOffset + localSlab];
	const int x = int(slab.X);
	const int y = int(slab.Y);
	const int zTop = int(slab.ZTop);
	uint emittedFaces = scratch.FaceOffset;
	const uint firstFace = emittedFaces;
	bool overflow = false;

	if ((slab.CullMask & 16u) != 0u && slab.ColorRunCount != 0u)
	{
		const uint color = VoxelComputeColorRuns[slab.ColorRunOffset].Color;
		overflow = !EmitFace(job, int4(x, x + 1, x, x + 1), int4(y, y, y + 1, y + 1), int4(zTop, zTop, zTop, zTop), color, color, slab, zTop, 16u, emittedFaces);
	}
	for (uint localRun = 0u; localRun < slab.ColorRunCount && !overflow; ++localRun)
	{
		const NRIVoxelComputeColorRunRecord run = VoxelComputeColorRuns[slab.ColorRunOffset + localRun];
		[unroll]
		for (uint faceCullBit = 1u; faceCullBit <= 8u && !overflow; faceCullBit <<= 1u)
		{
			if ((slab.CullMask & faceCullBit) != 0u)
			{
				overflow = !EmitVoxelSideSpansForRun(job, x, y, slab, run, faceCullBit, emittedFaces);
			}
		}
	}
	if (!overflow && (slab.CullMask & 32u) != 0u && slab.ColorRunCount != 0u)
	{
		const uint color = VoxelComputeColorRuns[slab.ColorRunOffset + slab.ColorRunCount - 1u].Color;
		const int z = zTop + int(slab.ZLength);
		overflow = !EmitFace(job, int4(x + 1, x, x + 1, x), int4(y, y, y + 1, y + 1), int4(z, z, z, z), color, color, slab, z - 1, 32u, emittedFaces);
	}

	if (overflow)
	{
		MarkMismatch(jobIndex, NRI_VOXEL_COMPUTE_MISMATCH_OUTPUT_OVERFLOW);
	}
	else if (emittedFaces - firstFace != scratch.FaceCount)
	{
		MarkMismatch(jobIndex, NRI_VOXEL_COMPUTE_MISMATCH_SLAB_EMIT_COUNT);
	}
}
