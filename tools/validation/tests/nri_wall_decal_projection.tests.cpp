#include "nri_wall_decal_projection.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>

namespace
{

using nri_scene::WallDecalGpuData;

struct Quad
{
	float x0 = 10.0f;
	float z0 = -20.0f;
	float x1 = 18.0f;
	float z1 = -20.0f;
	float bottom = -12.0f;
	float top = 4.0f;
	float leftU = 0.0f;
	float rightU = 1.0f;
	float bottomV = 1.0f;
	float topV = 0.0f;
};

bool Build(const Quad& quad, WallDecalGpuData& record)
{
	return nri_scene::BuildWallDecalProjection(quad.x0, quad.z0, quad.x1, quad.z1,
		quad.bottom, quad.top, quad.leftU, quad.rightU, quad.bottomV, quad.topV, record);
}

float Evaluate(const float (&row)[4], float x, float y, float z)
{
	return row[0] * x + row[1] * y + row[2] * z + row[3];
}

bool Near(float actual, float expected)
{
	return std::isfinite(actual) && std::abs(actual - expected) <= 0.00002f;
}

bool MapsQuad(const Quad& quad)
{
	WallDecalGpuData record = {};
	if (!Build(quad, record)) return false;
	// Independent geometric oracle: corners and interior points on the supplied
	// quad must receive their interpolated authored UVs, regardless of direction.
	constexpr float fractions[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
	for (float s : fractions)
	{
		for (float t : fractions)
		{
			const float x = quad.x0 + s * (quad.x1 - quad.x0);
			const float z = quad.z0 + s * (quad.z1 - quad.z0);
			const float y = quad.bottom + t * (quad.top - quad.bottom);
			if (!Near(Evaluate(record.worldToU, x, y, z), quad.leftU + s * (quad.rightU - quad.leftU)) ||
				!Near(Evaluate(record.worldToV, x, y, z), quad.bottomV + t * (quad.topV - quad.bottomV)))
				return false;
		}
	}
	return true;
}

bool DefaultRecordsHaveZeroProjectionAndInvalidIdentity()
{
	const WallDecalGpuData record = {};
	const nri_scene::WallDecalHeaderGpuData header = {};
	for (std::size_t i = 0; i < 4; ++i)
	{
		if (record.worldToU[i] != 0.0f || record.worldToV[i] != 0.0f ||
			record.plane[i] != 0.0f || record.uvBounds[i] != 0.0f)
			return false;
	}
	return header.first == 0u && header.count == 0u && record.alpha == 0.0f &&
		record.lightLevel == 1.0f && record.paletteIndex == 0u && record.flags == 0u &&
		record.textureIndex == UINT32_MAX && record.actorIndex == UINT32_MAX &&
		record.wallIndex == UINT32_MAX && record.sectorIndex == UINT32_MAX;
}

bool MapsEveryCardinalAndDiagonalDirection()
{
	constexpr float directions[][2] = {
		{ 8.0f, 0.0f }, { -8.0f, 0.0f }, { 0.0f, 8.0f }, { 0.0f, -8.0f },
		{ 6.0f, 8.0f }, { -6.0f, 8.0f }, { 6.0f, -8.0f }, { -6.0f, -8.0f }
	};
	for (const auto& direction : directions)
	{
		Quad quad;
		quad.x1 = quad.x0 + direction[0];
		quad.z1 = quad.z0 + direction[1];
		if (!MapsQuad(quad)) return false;
	}
	return true;
}

bool PreservesIndependentXYFlips()
{
	for (unsigned flags = 0; flags < 4; ++flags)
	{
		Quad quad;
		quad.x1 = 16.0f;
		quad.z1 = -12.0f;
		quad.leftU = (flags & 1u) ? 1.0f : 0.0f;
		quad.rightU = 1.0f - quad.leftU;
		quad.bottomV = (flags & 2u) ? 0.0f : 1.0f;
		quad.topV = 1.0f - quad.bottomV;
		if (!MapsQuad(quad)) return false;
	}
	return true;
}

bool ClippedQuadKeepsAuthoredUvBounds()
{
	Quad quad;
	quad.bottom = -7.0f;
	quad.top = 1.0f;
	quad.leftU = 0.875f;
	quad.rightU = 0.25f;
	quad.bottomV = 0.75f;
	quad.topV = 0.125f;
	WallDecalGpuData record = {};
	if (!Build(quad, record) || !MapsQuad(quad)) return false;
	// Clipping cannot stretch the surviving pixels back to the full texture.
	return Near(record.uvBounds[0], 0.25f) && Near(record.uvBounds[1], 0.125f) &&
		Near(record.uvBounds[2], 0.875f) && Near(record.uvBounds[3], 0.75f) &&
		Near(Evaluate(record.worldToU, 14.0f, -3.0f, -20.0f), 0.5625f) &&
		Near(Evaluate(record.worldToV, 14.0f, -3.0f, -20.0f), 0.4375f);
}

bool WallNormalOffsetDoesNotShiftTexture()
{
	Quad quad;
	quad.x1 = 16.0f;
	quad.z1 = -12.0f;
	WallDecalGpuData record = {};
	if (!Build(quad, record)) return false;
	// Unit perpendicular to a 6,8 edge. The real crack moves 0.3125 units
	// from its wall during initialization; projection must keep its UV center.
	const float x = 13.0f - 0.8f * 0.3125f;
	const float z = -16.0f + 0.6f * 0.3125f;
	return Near(Evaluate(record.worldToU, x, -4.0f, z), 0.5f) &&
		Near(Evaluate(record.worldToV, x, -4.0f, z), 0.5f);
}

bool ProjectionPreservesReceiverAndMaterialMetadata()
{
	WallDecalGpuData record = {};
	record.wallIndex = 347u;
	record.sectorIndex = 65u;
	record.actorIndex = 414u;
	record.textureIndex = 17u;
	record.paletteIndex = 8u;
	record.flags = 3u;
	record.alpha = 0.666f;
	record.lightLevel = 0.75f;
	record.plane[0] = -1.0f;
	record.plane[3] = -16.0f;
	if (!Build(Quad{}, record)) return false;
	return record.wallIndex == 347u && record.sectorIndex == 65u && record.actorIndex == 414u &&
		record.textureIndex == 17u && record.paletteIndex == 8u && record.flags == 3u &&
		record.alpha == 0.666f && record.lightLevel == 0.75f &&
		record.plane[0] == -1.0f && record.plane[3] == -16.0f &&
		record.worldToU[1] == 0.0f && record.worldToV[0] == 0.0f && record.worldToV[2] == 0.0f;
}

bool RebuildingClearsUnusedCoefficients()
{
	WallDecalGpuData record = {};
	for (std::size_t i = 0; i < 4; ++i)
	{
		record.worldToU[i] = 300.0f;
		record.worldToV[i] = -700.0f;
	}
	Quad quad;
	if (!Build(quad, record)) return false;
	return record.worldToU[1] == 0.0f && record.worldToV[0] == 0.0f && record.worldToV[2] == 0.0f &&
		Near(Evaluate(record.worldToU, 14.0f, -4.0f, -20.0f), 0.5f) &&
		Near(Evaluate(record.worldToV, 14.0f, -4.0f, -20.0f), 0.5f);
}

bool RejectsDegenerateGeometry()
{
	std::array<Quad, 5> quads = {};
	quads[0].x1 = quads[0].x0;
	quads[0].z1 = quads[0].z0;
	quads[1].top = quads[1].bottom;
	quads[2].top = quads[2].bottom - 1.0f;
	quads[3].x0 = quads[3].z0 = quads[3].z1 = 0.0f;
	quads[3].x1 = 1.e-9f;
	quads[4].bottom = 0.0f;
	quads[4].top = 1.e-9f;
	for (const auto& quad : quads)
	{
		WallDecalGpuData record = {};
		if (Build(quad, record)) return false;
	}
	return true;
}

bool RejectsEveryNonFiniteInput()
{
	constexpr float Quad::* fields[] = { &Quad::x0, &Quad::z0, &Quad::x1, &Quad::z1,
		&Quad::bottom, &Quad::top, &Quad::leftU, &Quad::rightU, &Quad::bottomV, &Quad::topV };
	const float values[] = { std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
	bool ok = true;
	for (std::size_t i = 0; i < std::size(fields); ++i)
	{
		for (float value : values)
		{
			Quad quad;
			quad.*fields[i] = value;
			WallDecalGpuData record = {};
			if (Build(quad, record))
			{
				std::cerr << "Accepted nonfinite projection input at field " << i << '\n';
				ok = false;
			}
		}
	}
	return ok;
}

bool RejectsFiniteInputsWhoseProjectionOverflows()
{
	const float limit = std::numeric_limits<float>::max();
	std::array<Quad, 4> quads = {};
	quads[0].leftU = -limit;
	quads[0].rightU = limit;
	quads[1].bottomV = -limit;
	quads[1].topV = limit;
	quads[2].x0 = 10000.0f;
	quads[2].x1 = 10001.0f;
	quads[2].rightU = limit / 2.0f;
	quads[3].x0 = -limit;
	quads[3].x1 = limit;
	for (const auto& quad : quads)
	{
		WallDecalGpuData record = {};
		if (Build(quad, record)) return false;
	}
	return true;
}

}

int main()
{
	struct Test { const char* name; bool (*run)(); };
	const Test tests[] = {
		{ "zero projection defaults and invalid identities", DefaultRecordsHaveZeroProjectionAndInvalidIdentity },
		{ "cardinal and diagonal affine UV mapping", MapsEveryCardinalAndDiagonalDirection },
		{ "independent X/Y flips", PreservesIndependentXYFlips },
		{ "clipped non-full UV bounds", ClippedQuadKeepsAuthoredUvBounds },
		{ "wall-normal offset invariance", WallNormalOffsetDoesNotShiftTexture },
		{ "receiver and material metadata preserved", ProjectionPreservesReceiverAndMaterialMetadata },
		{ "rebuilding clears unused coefficients", RebuildingClearsUnusedCoefficients },
		{ "degenerate geometry rejected", RejectsDegenerateGeometry },
		{ "NaN and infinity rejected in every input", RejectsEveryNonFiniteInput },
		{ "finite-input arithmetic overflow rejected", RejectsFiniteInputsWhoseProjectionOverflows },
	};
	unsigned failures = 0;
	for (const auto& test : tests)
	{
		const bool passed = test.run();
		std::cout << (passed ? "PASS: " : "FAIL: ") << test.name << '\n';
		failures += passed ? 0u : 1u;
	}
	return failures == 0u ? 0 : 1;
}
