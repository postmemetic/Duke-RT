#ifndef NRI_SMOKE_CONSTANTS_HLSLI
#define NRI_SMOKE_CONSTANTS_HLSLI

#define NRI_SMOKE_VISIBILITY_FILTERED_EFFECTIVE 0x1u
#define NRI_SMOKE_VISIBILITY_FILTERED_RESOURCES_READY 0x2u
#define NRI_SMOKE_VISIBILITY_TLAS_READY 0x4u
#define NRI_SMOKE_VISIBILITY_FILTERED_REQUESTED 0x8u
#define NRI_SMOKE_EMISSIVE_POINT_CANDIDATE_SHIFT 4u
#define NRI_SMOKE_EMISSIVE_POINT_CANDIDATE_MASK 0xfu

#define NRI_SMOKE_DIRECT_REUSE_SHIFT 14u
#define NRI_SMOKE_DIRECT_REUSE_MASK 3u
#define NRI_SMOKE_FLAG_COMPARE_REPRESENTATION 0x10000u
#define NRI_SMOKE_FLAG_GRID_REPRESENTATION 0x20000u
#define NRI_SMOKE_DIRECT_GRID_ENABLED 0x20000u
#define NRI_SMOKE_DIRECT_HISTORY_VALID 0x40000u
#define NRI_SMOKE_DIRECT_REFERENCE_SHIFT 19u
#define NRI_SMOKE_DIRECT_REFERENCE_MASK 3u
#define NRI_SMOKE_GRID_LIGHT_WORLD_ENABLED 0x200000u
#define NRI_SMOKE_FLAG_VIEW_MASK 0x40000000u

struct SmokeConstants
{
    uint Pass;
    uint FrameIndex;
    uint SimulationEpoch;
    uint ParticleCapacity;

    uint CommandCount;
    uint StyleCount;
    uint FroxelWidth;
    uint FroxelHeight;

    uint FroxelDepth;
    uint DirectionalColorPacked;
    uint RenderWidth;
    uint RenderHeight;

    uint OutputWidth;
    uint OutputHeight;
    uint DebugMode;
    uint Flags;

    float DeltaTime;
    float IndirectScale;
    float FroxelMaxDistance;
    float DepthExponent;

    float DensityScale;
    float RadianceScale;
    float TanHalfFovX;
    float TanHalfFovY;

    float3 CameraPosition;
    float TimeScale;

    float3 CameraForward;
    float DirectionalDirectionX;

    float3 CameraRight;
    float DirectionalDirectionY;

    float3 CameraUp;
    float DirectionalDirectionZ;

    float3 Wind;
    float DirectionalAngularSize;

    uint LightMode;
    uint LightSamples;
    uint MaxLightCandidates;
    uint RuntimeLightCount;

    uint RuntimeLightTileCountX;
    uint RuntimeLightTileCountY;
    uint LightSourceFlags;
    uint FilteredVisibilityEnabled;

    float2 CurrentJitter;

    uint4 Visuals;
};

uint SmokeDebugMode(uint packedDebugMode) { return packedDebugMode & 0xffu; }
uint SmokeMultipleScatterDebug(uint packedDebugMode) { return (packedDebugMode >> 8u) & 3u; }
bool SmokeSelfShadowEnabled(uint packedDebugMode) { return (packedDebugMode & (1u << 10u)) != 0u; }
uint SmokeSelfShadowDebug(uint packedDebugMode) { return (packedDebugMode >> 11u) & 3u; }
float SmokeMultipleScatterScale(uint packedDebugMode)
{
    return (float)((packedDebugMode >> 16u) & 0xffffu) * (16.0 / 65535.0);
}

#endif
