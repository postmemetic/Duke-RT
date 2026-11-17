#pragma once

#include <cstdint>

namespace nri_scene
{
// Resource identity is stable across shade/palette/alpha changes; the separate
// material signature still validates every current payload. World/lifetime
// qualification prevents a recycled actor index from inheriting these rows.
constexpr uint64_t BuildEnemyStableMaterialKey(uint64_t world, uint64_t lifetime, int32_t textureId)
{
    if (!world || textureId < 0) return 0;
    uint64_t hash = UINT64_C(0x454e454d594d4154);
    const auto mix = [](uint64_t a, uint64_t b)
    { return (a ^ b) * UINT64_C(1099511628211); };
    hash = mix(hash, world);
    hash = mix(hash, lifetime);
    hash = mix(hash, uint32_t(textureId));
    return hash ? hash : 1;
}
}
