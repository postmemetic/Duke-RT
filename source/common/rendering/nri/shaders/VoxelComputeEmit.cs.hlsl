#include "Include/VoxelComputeConstants.hlsli"
#include "Include/VoxelNormals.hlsli"

StructuredBuffer<NRIVoxelComputeJob> VoxelComputeJobs : register(t0, space0);
StructuredBuffer<NRIVoxelComputeSlabRecord> VoxelComputeSlabs : register(t1, space0);
StructuredBuffer<NRIVoxelComputeFaceRecord> VoxelComputeFaces : register(t2, space0);
StructuredBuffer<NRIVoxelComputeColorRunRecord> VoxelComputeColorRuns : register(t3, space0);
RWStructuredBuffer<NRIVoxelComputeResult> VoxelComputeResults : register(u0, space1);
RWStructuredBuffer<NRIVoxelComputeSceneVertex> VoxelComputeVertices : register(u1, space1);
RWStructuredBuffer<uint> VoxelComputeIndices : register(u2, space1);
RWStructuredBuffer<NRIVoxelComputePrimitiveData> VoxelComputePrimitives : register(u3, space1);

uint HashCombine(uint hash, uint value)
{
	hash ^= value;
	hash *= 16777619u;
	return hash;
}

float2 VoxelUv(uint color)
{
	return float2(((color & 15u) + 0.5f) / 16.0f, ((color >> 4u) + 0.5f) / 16.0f);
}

float3 TransformVoxelPoint(NRIVoxelComputeJob job, int x, int y, int z)
{
	return float3((float)x - job.PivotX, -(float)z + job.PivotZ, -(float)y + job.PivotY);
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
	bool hasSlab,
	int voxelZ,
	uint faceCullBit,
	inout uint emittedFaces,
	inout uint vertexHash,
	inout uint indexHash,
	inout uint primitiveHash)
{
	const uint localVertexBase = emittedFaces * 4u;
	const uint localIndexBase = emittedFaces * 6u;
	const uint localPrimitiveBase = emittedFaces * 2u;
	if (localVertexBase + 4u > job.VertexCapacity ||
		localIndexBase + 6u > job.IndexCapacity ||
		localPrimitiveBase + 2u > job.PrimitiveCapacity)
	{
		return false;
	}

	const float2 uv = VoxelUv(color);
	const float3 p0 = TransformVoxelPoint(job, x.x, y.x, z.x);
	const float3 p1 = TransformVoxelPoint(job, x.y, y.y, z.y);
	const float3 p2 = TransformVoxelPoint(job, x.w, y.w, z.w);
	const float3 p3 = TransformVoxelPoint(job, x.z, y.z, z.z);
	const float3 rawNormal = cross(p1 - p0, p3 - p0);
	const float normalLengthSq = dot(rawNormal, rawNormal);
	const float3 faceNormal = normalLengthSq > 1.0e-12f ? rawNormal * rsqrt(normalLengthSq) : float3(0.0f, 1.0f, 0.0f);
	const uint shadingNormal = hasSlab ? BuildVoxelSurfaceNormal(voxelZ, faceCullBit, slab) : PackVoxelNormal(faceNormal);

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
		indexHash = HashCombine(indexHash, localIndices[i]);
	}

	VoxelComputePrimitives[primitiveBase + 0u] = MakePrimitive(uint3(primitiveIndices[0], primitiveIndices[1], primitiveIndices[2]), materialIndex, uv, uv, uv, faceNormal, uint3(shadingNormal, shadingNormal, shadingNormal));
	VoxelComputePrimitives[primitiveBase + 1u] = MakePrimitive(uint3(primitiveIndices[3], primitiveIndices[4], primitiveIndices[5]), materialIndex, uv, uv, uv, faceNormal, uint3(shadingNormal, shadingNormal, shadingNormal));

	[unroll]
	for (uint v = 0u; v < 4u; ++v)
	{
		const NRIVoxelComputeSceneVertex vertex = VoxelComputeVertices[vertexBase + v];
		vertexHash = HashCombine(vertexHash, asuint(vertex.Position.x));
		vertexHash = HashCombine(vertexHash, asuint(vertex.Position.y));
		vertexHash = HashCombine(vertexHash, asuint(vertex.Position.z));
		vertexHash = HashCombine(vertexHash, asuint(vertex.Uv.x));
		vertexHash = HashCombine(vertexHash, asuint(vertex.Uv.y));
	}
	primitiveHash = HashCombine(primitiveHash, materialIndex);
	primitiveHash = HashCombine(primitiveHash, primitiveIndices[0]);
	primitiveHash = HashCombine(primitiveHash, primitiveIndices[1]);
	primitiveHash = HashCombine(primitiveHash, primitiveIndices[2]);
	primitiveHash = HashCombine(primitiveHash, primitiveIndices[3]);
	primitiveHash = HashCombine(primitiveHash, primitiveIndices[4]);
	primitiveHash = HashCombine(primitiveHash, primitiveIndices[5]);
	primitiveHash = HashCombine(primitiveHash, shadingNormal);
	primitiveHash = HashCombine(primitiveHash, shadingNormal);
	primitiveHash = HashCombine(primitiveHash, shadingNormal);
	primitiveHash = HashCombine(primitiveHash, shadingNormal);
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
	inout uint emittedFaces,
	inout uint vertexHash,
	inout uint indexHash,
	inout uint primitiveHash)
{
	if (faceCullBit == 1u)
	{
		return EmitFace(job, int4(x, x, x, x), int4(y, y + 1, y, y + 1), int4(z0, z0, z1, z1), color, 0u, slab, true, z0, faceCullBit, emittedFaces, vertexHash, indexHash, primitiveHash);
	}
	if (faceCullBit == 2u)
	{
		return EmitFace(job, int4(x + 1, x + 1, x + 1, x + 1), int4(y + 1, y, y + 1, y), int4(z0, z0, z1, z1), color, 0u, slab, true, z0, faceCullBit, emittedFaces, vertexHash, indexHash, primitiveHash);
	}
	if (faceCullBit == 4u)
	{
		return EmitFace(job, int4(x + 1, x, x + 1, x), int4(y, y, y, y), int4(z0, z0, z1, z1), color, 0u, slab, true, z0, faceCullBit, emittedFaces, vertexHash, indexHash, primitiveHash);
	}
	return EmitFace(job, int4(x, x + 1, x, x + 1), int4(y + 1, y + 1, y + 1, y + 1), int4(z0, z0, z1, z1), color, 0u, slab, true, z0, faceCullBit, emittedFaces, vertexHash, indexHash, primitiveHash);
}

bool EmitVoxelSideSpansForRun(
	NRIVoxelComputeJob job,
	int x,
	int y,
	NRIVoxelComputeSlabRecord slab,
	NRIVoxelComputeColorRunRecord run,
	uint faceCullBit,
	inout uint emittedFaces,
	inout uint vertexHash,
	inout uint indexHash,
	inout uint primitiveHash)
{
	NRIVoxelAdjacencySpan spans[3];
	const uint spanCount = BuildVoxelAdjacencyNormalSpans(slab, run, faceCullBit, spans);
	for (uint spanIndex = 0u; spanIndex < spanCount; ++spanIndex)
	{
		const int z0 = int(slab.ZTop + spans[spanIndex].ZOffset);
		const int z1 = z0 + int(spans[spanIndex].ZLength);
		if (!EmitVoxelSideSpan(job, x, y, z0, z1, run.Color, faceCullBit, slab, emittedFaces, vertexHash, indexHash, primitiveHash))
		{
			return false;
		}
	}
	return true;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint jobIndex = dispatchThreadId.x;
	if (jobIndex >= gVoxelComputeConstants.JobCount)
	{
		return;
	}

	const NRIVoxelComputeJob job = VoxelComputeJobs[jobIndex];
	uint vertexHash = 2166136261u;
	uint indexHash = 2166136261u;
	uint primitiveHash = 2166136261u;
	uint emittedFaces = 0u;
	bool outputOverflow = false;

	if (gVoxelComputeConstants.ColorRunRecordCount != 0u)
	{
		for (uint localSlab = 0u; localSlab < job.SlabCount; ++localSlab)
		{
			const uint slabIndex = job.SlabOffset + localSlab;
			if (slabIndex >= gVoxelComputeConstants.SlabRecordCount)
			{
				break;
			}
			const NRIVoxelComputeSlabRecord slab = VoxelComputeSlabs[slabIndex];
			const int x = int(slab.X);
			const int y = int(slab.Y);
			const int zTop = int(slab.ZTop);
			if ((slab.CullMask & 16u) != 0u && slab.ColorRunCount != 0u)
			{
				const uint runIndex = slab.ColorRunOffset;
				if (runIndex < gVoxelComputeConstants.ColorRunRecordCount)
				{
					const uint color = VoxelComputeColorRuns[runIndex].Color;
					outputOverflow = !EmitFace(job, int4(x, x + 1, x, x + 1), int4(y, y, y + 1, y + 1), int4(zTop, zTop, zTop, zTop), color, 0u, slab, true, zTop, 16u, emittedFaces, vertexHash, indexHash, primitiveHash);
				}
			}
			for (uint localRun = 0u; localRun < slab.ColorRunCount && !outputOverflow; ++localRun)
			{
				const uint runIndex = slab.ColorRunOffset + localRun;
				if (runIndex >= gVoxelComputeConstants.ColorRunRecordCount)
				{
					break;
				}
				const NRIVoxelComputeColorRunRecord run = VoxelComputeColorRuns[runIndex];
				[unroll]
				for (uint faceCullBit = 1u; faceCullBit <= 8u && !outputOverflow; faceCullBit <<= 1u)
				{
					if ((slab.CullMask & faceCullBit) != 0u)
					{
						outputOverflow = !EmitVoxelSideSpansForRun(job, x, y, slab, run, faceCullBit, emittedFaces, vertexHash, indexHash, primitiveHash);
					}
				}
			}
			if (!outputOverflow && (slab.CullMask & 32u) != 0u && slab.ColorRunCount != 0u)
			{
				const uint runIndex = slab.ColorRunOffset + slab.ColorRunCount - 1u;
				if (runIndex < gVoxelComputeConstants.ColorRunRecordCount)
				{
					const uint color = VoxelComputeColorRuns[runIndex].Color;
					const int z = zTop + int(slab.ZLength);
					outputOverflow = !EmitFace(job, int4(x + 1, x, x + 1, x), int4(y, y, y + 1, y + 1), int4(z, z, z, z), color, 0u, slab, true, z - 1, 32u, emittedFaces, vertexHash, indexHash, primitiveHash);
				}
			}
			if (outputOverflow)
			{
				break;
			}
		}
	}
	else
	{
		for (uint localFace = 0u; localFace < job.ExpectedFaces && !outputOverflow; ++localFace)
		{
			const uint faceIndex = job.FaceOffset + localFace;
			if (faceIndex >= gVoxelComputeConstants.FaceRecordCount)
			{
				break;
			}
			const NRIVoxelComputeFaceRecord face = VoxelComputeFaces[faceIndex];
			outputOverflow = !EmitFace(job, int4(face.X[0], face.X[1], face.X[2], face.X[3]), int4(face.Y[0], face.Y[1], face.Y[2], face.Y[3]), int4(face.Z[0], face.Z[1], face.Z[2], face.Z[3]), face.Color, face.MaterialIndex, (NRIVoxelComputeSlabRecord)0, false, face.Z[0], 0u, emittedFaces, vertexHash, indexHash, primitiveHash);
		}
	}

	NRIVoxelComputeResult result;
	result.FaceCount = emittedFaces;
	result.IndexCount = emittedFaces * 6u;
	result.VertexCountNoDedupe = emittedFaces * 4u;
	result.VoxelCount = job.ExpectedVoxels;
	result.SlabCount = job.SlabCount;
	result.PrimitiveCount = emittedFaces * 2u;
	result.MismatchMask = 0u;
	result.MismatchMask |= result.FaceCount == job.ExpectedFaces ? 0u : 1u;
	result.MismatchMask |= result.IndexCount == job.ExpectedIndices ? 0u : 2u;
	result.MismatchMask |= result.VertexCountNoDedupe == job.ExpectedVerticesNoDedupe ? 0u : 4u;
	result.MismatchMask |= result.PrimitiveCount == job.ExpectedFaces * 2u ? 0u : 16u;
	result.MismatchMask |= outputOverflow ? 32u : 0u;
	result.JobId = job.JobId;
	result.Status = result.MismatchMask == 0u ? NRI_VOXEL_COMPUTE_STATUS_EMIT_OK : NRI_VOXEL_COMPUTE_STATUS_EMIT_MISMATCH;
	result.VertexHash = vertexHash;
	result.IndexHash = indexHash;
	result.PrimitiveHash = primitiveHash;
	VoxelComputeResults[jobIndex] = result;
}
