#ifndef NRI_STATIC_SHADING_DATA_HLSLI
#define NRI_STATIC_SHADING_DATA_HLSLI
struct StaticTangentData
{
    float3 tangentRaw;
    uint valid;
    float3 bitangentRaw;
    uint reserved;
};

bool ResolveRawTriangleTangents(float3 p0, float3 p1, float3 p2,
    float2 uv0, float2 uv1, float2 uv2, out float3 tangentRaw, out float3 bitangentRaw)
{
    const float3 edge1 = p1 - p0;
    const float3 edge2 = p2 - p0;
    const float2 duv1 = uv1 - uv0;
    const float2 duv2 = uv2 - uv0;
    const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
    if (abs(determinant) <= 1e-6)
    {
        tangentRaw = 0.0;
        bitangentRaw = 0.0;
        return false;
    }
    tangentRaw = (edge1 * duv2.y - edge2 * duv1.y) / determinant;
    bitangentRaw = (edge2 * duv1.x - edge1 * duv2.x) / determinant;
    return true;
}

bool FinishTriangleTangentFrame(float3 tangentRaw, float3 bitangentRaw,
    float3 geometricNormal, out float3 tangent, out float3 bitangent)
{
    tangentRaw -= geometricNormal * dot(geometricNormal, tangentRaw);
    const float tangentLengthSq = dot(tangentRaw, tangentRaw);
    const float bitangentLengthSq = dot(bitangentRaw, bitangentRaw);
    if (tangentLengthSq <= 1e-8 || bitangentLengthSq <= 1e-8)
    {
        tangent = 0.0;
        bitangent = 0.0;
        return false;
    }
    tangent = tangentRaw * rsqrt(tangentLengthSq);
    bitangent = normalize(cross(geometricNormal, tangent));
    const float handedness = dot(cross(geometricNormal, tangent), bitangentRaw) < 0.0 ? -1.0 : 1.0;
    // Match the existing raster/PT normal-map Y orientation.
    bitangent *= -handedness;
    return true;
}
#endif
