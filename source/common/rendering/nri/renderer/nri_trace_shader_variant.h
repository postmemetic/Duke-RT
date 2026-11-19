#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// Each artifact family has its own diagnostic authorization. A production
// fallback for one PSO must never enable audit copies on a different PSO.
enum class NRITraceShaderFamily : uint32_t { Full = 1, Cache = 2, Lean = 4 };

inline uint32_t NRITraceAuditBit(NRITraceShaderFamily family, bool page)
{
	return static_cast<uint32_t>(family) << (page ? 3 : 0);
}

inline const char* NRITraceShaderStem(NRITraceShaderFamily family, uint32_t spatialFormat)
{
	if (family == NRITraceShaderFamily::Lean) return "TraceOpaqueLeanDebug";
	if (family == NRITraceShaderFamily::Cache)
		return spatialFormat == 1 ? "TraceOpaqueTypedCache" : spatialFormat == 2 ? "TraceOpaqueCompareCache" : "TraceOpaqueCache";
	return spatialFormat == 1 ? "TraceOpaqueTyped" : spatialFormat == 2 ? "TraceOpaqueCompare" : "TraceOpaque";
}

inline std::string NRITraceShaderFile(NRITraceShaderFamily family, uint32_t spatialFormat, bool page, bool d3d12)
{
	std::string name = spatialFormat == 2 ? "variants/diagnostic/" : "";
	name += NRITraceShaderStem(family, spatialFormat);
	if (page) name += "Page";
	return name + (d3d12 ? ".cs.dxil" : ".cs.spirv");
}

inline uint32_t NRITraceShaderFileAuditBit(std::string_view name)
{
	const auto suffix = name.find('.');
	if (suffix == name.npos || (name.substr(suffix) != ".cs.dxil" && name.substr(suffix) != ".cs.spirv")) return 0;
	auto stem = name.substr(0, suffix);
	const bool page = stem.size() >= 4 && stem.substr(stem.size() - 4) == "Page";
	if (page) stem.remove_suffix(4);
	for (auto family : {NRITraceShaderFamily::Full, NRITraceShaderFamily::Cache, NRITraceShaderFamily::Lean})
		for (uint32_t format = 0; format <= 2; ++format)
			if (stem == NRITraceShaderStem(family, format)) return NRITraceAuditBit(family, page);
	return 0;
}
