#include "NRI.hlsl"
#include "Include/SceneShadowContracts.hlsli"
#include "Include/EmissiveLightContracts.hlsli"
#include "Include/StaticShadingData.hlsli"
StructuredBuffer<SceneVertex> Vertices : register(t0, space0);
StructuredBuffer<PrimitiveData> Primitives : register(t1, space0);
RWStructuredBuffer<StaticTangentData> Tangents : register(u2, space0);
struct StaticShadingBuildConstants
{
    uint PrimitiveCount;
    uint VertexCount;
    uint Reserved0;
    uint Reserved1;
};
NRI_ROOT_CONSTANTS(StaticShadingBuildConstants, gBuild, 0, 0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint PrimitiveCount = gBuild.PrimitiveCount;
    const uint VertexCount = gBuild.VertexCount;
    const uint index = id.x; // CPU64MiB cap bounds the one-dimensional dispatch.
    if (index >= PrimitiveCount) return;
    StaticTangentData record = (StaticTangentData)0;
    record.valid = 2u; // Unsupported input: require the exact legacy path.
    const PrimitiveData primitive = Primitives[index];
    if (all(primitive.indices < VertexCount))
    {
        record.valid = ResolveRawTriangleTangents(
            Vertices[primitive.indices.x].position,
            Vertices[primitive.indices.y].position,
            Vertices[primitive.indices.z].position,
            primitive.uv0, primitive.uv1, primitive.uv2,
            record.tangentRaw, record.bitangentRaw) ? 1u : 0u;
    }
    Tangents[index] = record;
}
