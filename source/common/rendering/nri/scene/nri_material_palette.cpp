#include "nri_material_palette.h"
#include "nri_hash.h"
#include "../../../engine/perf_audit.h"

#include "palette.h"
#include "palettecontainer.h"
#include "printf.h"

#include <cassert>
#include <mutex>

namespace
{
	using namespace nri_scene;

	struct PaletteLookupCache
	{
		uint64_t sourceSignature = 0;
		ImmutableBytePayload payload;
	};

	PaletteLookupCache gPaletteLookupCache;
	std::mutex gPaletteLookupCacheMutex;
	thread_local ScopedMaterialPaletteSnapshot* gMaterialPaletteScope = nullptr;

	uint64_t BuildPaletteSourceSignature()
	{
		PerfAuditScope scanScope(PerfAuditMetric::MaterialPaletteSourceScanNs);
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteSourceScans);
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteSourceBytes, 256u * 3u + MAXPALOOKUPS * 256u);
		uint64_t signature = 1469598103934665603ull;
		for (uint32_t colorIndex = 0; colorIndex < 256; ++colorIndex)
		{
			const PalEntry color = GPalette.BaseColors[colorIndex];
			Fnv1a64Append(signature, &color.b, sizeof(color.b));
			Fnv1a64Append(signature, &color.g, sizeof(color.g));
			Fnv1a64Append(signature, &color.r, sizeof(color.r));
		}
		for (uint32_t paletteIndex = 0; paletteIndex < MAXPALOOKUPS; ++paletteIndex)
		{
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			if (table != nullptr)
			{
				Fnv1a64Append(signature, table, 256);
			}
			else
			{
				for (uint32_t colorIndex = 0; colorIndex < 256; ++colorIndex)
				{
					const uint8_t identity = (uint8_t)colorIndex;
					Fnv1a64Append(signature, &identity, sizeof(identity));
				}
			}
		}
		return signature;
	}

	bool PalettePayloadMatchesLiveSamples(const ImmutableBytePayload& payload)
	{
		if (payload.size() != (size_t)256 * MAXPALOOKUPS * 4u)
		{
			return false;
		}
		for (uint32_t sample = 0; sample < 32; ++sample)
		{
			const uint32_t paletteIndex = (sample * 67u) & 255u;
			const uint32_t colorIndex = (sample * 149u) & 255u;
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			const uint8_t remappedIndex = table != nullptr ? table[colorIndex] : (uint8_t)colorIndex;
			const PalEntry color = GPalette.BaseColors[remappedIndex];
			const size_t pixelIndex = ((size_t)paletteIndex * 256u + colorIndex) * 4u;
			const uint8_t* bytes = payload.data();
			if (bytes[pixelIndex + 0] != color.b ||
				bytes[pixelIndex + 1] != color.g ||
				bytes[pixelIndex + 2] != color.r ||
				bytes[pixelIndex + 3] != 255)
			{
				return false;
			}
		}
		return true;
	}

	// Deliberately independent of source hashes and the small legacy sample set.
	// Only used by the opt-in validator; no full comparison on the normal hit path.
	bool PalettePayloadMatchesLive(const ImmutableBytePayload& payload)
	{
		PerfAuditScope validationScope(PerfAuditMetric::MaterialPaletteValidationNs);
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteValidationChecks);
		if (payload.size() != (size_t)256 * MAXPALOOKUPS * 4u)
		{
			return false;
		}
		const uint8_t* bytes = payload.data();
		for (uint32_t paletteIndex = 0; paletteIndex < MAXPALOOKUPS; ++paletteIndex)
		{
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			for (uint32_t colorIndex = 0; colorIndex < 256; ++colorIndex)
			{
				const PalEntry color = GPalette.BaseColors[table != nullptr ? table[colorIndex] : colorIndex];
				const size_t offset = ((size_t)paletteIndex * 256u + colorIndex) * 4u;
				if (bytes[offset] != color.b || bytes[offset + 1] != color.g ||
					bytes[offset + 2] != color.r || bytes[offset + 3] != 255)
				{
					PerfAuditAdd(PerfAuditMetric::MaterialPaletteValidationBytes, offset + 4u);
					return false;
				}
			}
		}
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteValidationBytes, payload.size());
		return true;
	}

	bool ResolveLivePalette(MaterialBridgeData& outMaterials, bool forceBuild)
	{
		const uint64_t sourceSignature = BuildPaletteSourceSignature();
		std::lock_guard<std::mutex> lock(gPaletteLookupCacheMutex);
		if (!forceBuild && gPaletteLookupCache.sourceSignature == sourceSignature &&
			PalettePayloadMatchesLiveSamples(gPaletteLookupCache.payload))
		{
			outMaterials.paletteLookup = gPaletteLookupCache.payload;
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteCacheReuses);
			return false;
		}

		PerfAuditScope buildScope(PerfAuditMetric::MaterialPalettePayloadBuildNs);
		std::vector<uint8_t> paletteLookup(
			(size_t)outMaterials.paletteWidth * (size_t)outMaterials.paletteHeight * 4u);

		for (uint32_t paletteIndex = 0; paletteIndex < outMaterials.paletteHeight; ++paletteIndex)
		{
			const uint8_t* table = lookups.getTable((int)paletteIndex);
			for (uint32_t colorIndex = 0; colorIndex < outMaterials.paletteWidth; ++colorIndex)
			{
				const uint8_t remappedIndex = table != nullptr ? table[colorIndex] : (uint8_t)colorIndex;
				const PalEntry color = GPalette.BaseColors[remappedIndex];
				const size_t pixelIndex = ((size_t)paletteIndex * outMaterials.paletteWidth + colorIndex) * 4u;
				paletteLookup[pixelIndex + 0] = color.b;
				paletteLookup[pixelIndex + 1] = color.g;
				paletteLookup[pixelIndex + 2] = color.r;
				paletteLookup[pixelIndex + 3] = 255;
			}
		}
		const uint64_t payloadSignature = Fnv1a64(paletteLookup.data(), paletteLookup.size());
		gPaletteLookupCache.sourceSignature = sourceSignature;
		gPaletteLookupCache.payload.Assign(std::move(paletteLookup), payloadSignature);
		outMaterials.paletteLookup = gPaletteLookupCache.payload;
		PerfAuditAdd(PerfAuditMetric::MaterialPalettePayloadBuilds);
		PerfAuditAdd(PerfAuditMetric::MaterialPalettePayloadBytesBuilt, outMaterials.paletteLookup.size());
		return true;
	}
}

namespace nri_scene
{
ScopedMaterialPaletteSnapshot::ScopedMaterialPaletteSnapshot(bool enabled, bool validate)
	: mPrevious(gMaterialPaletteScope), mEnabled(enabled), mValidate(validate)
{
	gMaterialPaletteScope = this;
	PerfAuditAdd(PerfAuditMetric::MaterialPaletteOperations);
	if (mPrevious != nullptr)
	{
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteNestedOperations);
	}
}

ScopedMaterialPaletteSnapshot::~ScopedMaterialPaletteSnapshot()
{
	assert(gMaterialPaletteScope == this);
	gMaterialPaletteScope = mPrevious;
	if (mPrevious != nullptr && !mPrevious->mPayload.empty())
	{
		// A nested view/preload owns a fresh source snapshot. Do not resume an old
		// outer snapshot if palette sources changed while that nested work ran.
		mPrevious->mPayload.clear();
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteParentInvalidations);
	}
}

bool ResolveMaterialPalette(MaterialBridgeData& outMaterials)
{
	PerfAuditScope resolveScope(PerfAuditMetric::MaterialPaletteResolveNs);
	PerfAuditAdd(PerfAuditMetric::MaterialPaletteRequests);
	outMaterials.paletteWidth = 256;
	outMaterials.paletteHeight = MAXPALOOKUPS;
	ScopedMaterialPaletteSnapshot* scope = gMaterialPaletteScope;
	bool forceBuild = false;
	if (scope != nullptr && scope->mEnabled && !scope->mPayload.empty())
	{
		if (!scope->mValidate || PalettePayloadMatchesLive(scope->mPayload))
		{
			outMaterials.paletteLookup = scope->mPayload;
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteSnapshotReuses);
			return false;
		}
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteValidationMismatches);
		Printf("NRI PT palette snapshot mismatch: rebuilding live palette and disabling reuse for this operation\n");
		scope->mPayload.clear();
		scope->mEnabled = false;
		forceBuild = true;
	}
	if (scope == nullptr)
	{
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteUnscopedRequests);
	}
	else if (!scope->mEnabled)
	{
		PerfAuditAdd(PerfAuditMetric::MaterialPaletteReferenceRequests);
	}
	const bool built = ResolveLivePalette(outMaterials, forceBuild);
	if (scope != nullptr && scope->mEnabled)
	{
		scope->mPayload = outMaterials.paletteLookup;
	}
	return built;
}
}
