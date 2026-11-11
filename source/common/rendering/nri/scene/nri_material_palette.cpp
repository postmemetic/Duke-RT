#include "nri_material_palette.h"
#include "nri_hash.h"
#include "../../../engine/perf_audit.h"

#include "palette.h"
#include "palettecontainer.h"
#include "printf.h"
#include "c_cvars.h"

#include <cassert>
#include <array>
#include <cstring>
#include <mutex>

CVAR(Bool, nri_ptpalettecompare, false, 0)

namespace
{
	using namespace nri_scene;

	// Canonical source bytes match BuildPaletteSourceSignature's input exactly:
	// B/G/R only and identity bytes for a missing table. Pointer identity and
	// palette alpha never certify content. Storage is bounded and shares the
	// existing payload mutex; no new per-view or thread-local retained arrays.
	struct PaletteSourceSnapshot
	{
		std::array<uint8_t, 256u * 3u> colors;
		std::array<std::array<uint8_t, 256>, MAXPALOOKUPS> tables;
		bool valid = false;
		bool quarantined = false;

		static const std::array<uint8_t, 256>& Identity()
		{
			static const auto bytes = [] { std::array<uint8_t, 256> result{};
				for (size_t i = 0; i < result.size(); ++i) result[i] = (uint8_t)i;
				return result; }();
			return bytes;
		}
		bool MatchesLive() const
		{
			PerfAuditScope timer(PerfAuditMetric::MaterialPaletteCompareSourceNs);
			uint64_t compared = 0;
			bool same = valid;
			for (uint32_t i = 0; same && i < 256; ++i)
			{
				const PalEntry color = GPalette.BaseColors[i];
				compared += 3;
				same = colors[i * 3] == color.b && colors[i * 3 + 1] == color.g && colors[i * 3 + 2] == color.r;
			}
			for (uint32_t i = 0; same && i < MAXPALOOKUPS; ++i)
			{
				const auto* live = lookups.getTable((int)i);
				compared += 256;
				same = std::memcmp(tables[i].data(), live != nullptr ? live : Identity().data(), 256) == 0;
			}
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareExtentBytes, compared);
			return same;
		}
		void CaptureLive()
		{
			PerfAuditScope timer(PerfAuditMetric::MaterialPaletteCompareCaptureNs);
			for (uint32_t i = 0; i < 256; ++i)
			{
				const PalEntry color = GPalette.BaseColors[i];
				colors[i * 3] = color.b; colors[i * 3 + 1] = color.g; colors[i * 3 + 2] = color.r;
			}
			for (uint32_t i = 0; i < MAXPALOOKUPS; ++i)
			{
				const auto* live = lookups.getTable((int)i);
				std::memcpy(tables[i].data(), live != nullptr ? live : Identity().data(), 256);
			}
			valid = true;
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareCapturedBytes, colors.size() + sizeof(tables));
		}
	};

	struct PaletteLookupCache
	{
		uint64_t sourceSignature = 0;
		ImmutableBytePayload payload;
		PaletteSourceSnapshot source;
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

	bool ResolveLivePalette(MaterialBridgeData& outMaterials, bool forceBuild, bool validateCompare)
	{
		const bool compare = (bool)nri_ptpalettecompare;
		std::unique_lock<std::mutex> lock(gPaletteLookupCacheMutex, std::defer_lock);
		// Retain the original off-path hash-before-lock ordering. Enabled reads
		// and publishes the exact snapshot under the same payload-owner lock.
		uint64_t sourceSignature = compare ? 0 : BuildPaletteSourceSignature();
		lock.lock();
		auto& source = gPaletteLookupCache.source;
		if (compare && !source.quarantined && !forceBuild)
		{
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareRequests);
			if (source.MatchesLive() && PalettePayloadMatchesLiveSamples(gPaletteLookupCache.payload))
			{
				bool referenceMatches = true;
				if (validateCompare)
				{
					PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareHitValidationChecks);
					// Original source identity plus complete actual returned payload;
					// covers the first request of a new view, not only scope hits.
					referenceMatches = BuildPaletteSourceSignature() == gPaletteLookupCache.sourceSignature &&
						PalettePayloadMatchesLive(gPaletteLookupCache.payload);
				}
				if (referenceMatches)
				{
					outMaterials.paletteLookup = gPaletteLookupCache.payload;
					PerfAuditAdd(PerfAuditMetric::MaterialPaletteCacheReuses);
					PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareHits);
					return false;
				}
				PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareValidationMismatches);
				PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareQuarantines);
				source.quarantined = true;
				forceBuild = true;
				Printf("NRI PT palette source comparison mismatch: rebuilding original output and disabling comparison reuse\n");
			}
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteCompareMisses);
		}
		if (compare) sourceSignature = BuildPaletteSourceSignature();
		// A fallback/build may replace the payload even while comparison is off.
		// Never leave a source snapshot certified for a different publication.
		source.valid = false;
		if (!forceBuild && gPaletteLookupCache.sourceSignature == sourceSignature &&
			PalettePayloadMatchesLiveSamples(gPaletteLookupCache.payload))
		{
			outMaterials.paletteLookup = gPaletteLookupCache.payload;
			PerfAuditAdd(PerfAuditMetric::MaterialPaletteCacheReuses);
			// A legacy hash/sample hit alone cannot certify a new exact snapshot:
			// its payload must first match every byte of the current source.
			if (compare && !source.quarantined && PalettePayloadMatchesLive(gPaletteLookupCache.payload))
				source.CaptureLive();
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
		// Publish the signature only after immutable-payload allocation succeeds.
		gPaletteLookupCache.payload.Assign(std::move(paletteLookup), payloadSignature);
		gPaletteLookupCache.sourceSignature = sourceSignature;
		if (compare && !source.quarantined) source.CaptureLive();
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
	const bool built = ResolveLivePalette(outMaterials, forceBuild, scope != nullptr && scope->mValidate);
	if (scope != nullptr && scope->mEnabled)
	{
		scope->mPayload = outMaterials.paletteLookup;
	}
	return built;
}
}
