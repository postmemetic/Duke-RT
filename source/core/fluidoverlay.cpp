#include "fluidoverlay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "c_dispatch.h"
#include "cmdlib.h"
#include "files.h"
#include "filesystem.h"
#include "mapinfo.h"
#include "printf.h"
#include "sc_man.h"

namespace
{
	static ParsedFluidOverlayDatabase GFluidOverlayDatabase;
	static uint32_t GFluidOverlayGeneration = 0;
	static ResolvedFluidOverlaySet GResolvedFluidOverlaySet;
	static uint32_t GResolvedFluidOverlayGeneration = 0;

	static constexpr uint64_t HashOffset = 1469598103934665603ull;
	static constexpr uint64_t HashPrime = 1099511628211ull;

	static uint64_t HashByte(uint64_t hash, uint8_t value)
	{
		hash ^= value;
		hash *= HashPrime;
		return hash;
	}

	template<typename T>
	static uint64_t HashValue(uint64_t hash, const T& value)
	{
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
		for (size_t i = 0; i < sizeof(T); ++i)
		{
			hash = HashByte(hash, bytes[i]);
		}
		return hash;
	}

	static uint64_t HashText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return HashByte(hash, 0);
		}
		for (const char* p = text; *p != 0; ++p)
		{
			hash = HashByte(hash, (uint8_t)*p);
		}
		return HashByte(hash, 0);
	}

	static uint32_t CanonicalFloatBits(float value)
	{
		if (value == 0.0f)
		{
			value = 0.0f;
		}
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static FString GetLumpDisplayName(int lumpNum)
	{
		const char* fullName = lumpNum >= 0 ? fileSystem.GetFileFullName(lumpNum) : nullptr;
		return fullName != nullptr ? FString(fullName) : FStringf("lump:%d", lumpNum);
	}

	static std::string MakeNormalizedKey(const FString& value)
	{
		return std::string(value.MakeLower().GetChars());
	}

	static std::string MakeMaterialRuleKey(const FString& mapName, const FString& id)
	{
		FString key;
		key << mapName.MakeLower() << "|" << id.MakeLower();
		return std::string(key.GetChars());
	}

	static std::string NormalizeTextureName(const char* value)
	{
		std::string normalized = value != nullptr ? value : "";
		for (char& c : normalized)
		{
			if (c == '\\') c = '/';
			else c = (char)std::tolower((unsigned char)c);
		}

		const size_t slash = normalized.find_last_of('/');
		const size_t dot = normalized.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			normalized.erase(dot);
		}
		return normalized;
	}

	static FString CanonicalizeMapName(const char* mapName)
	{
		if (mapName == nullptr || mapName[0] == 0)
		{
			return "";
		}
		if (auto* map = FindMapByName(mapName))
		{
			return map->labelName;
		}
		return ExtractFileBase(mapName);
	}

	static FString GetCurrentResolvedMapName()
	{
		return currentLevel != nullptr ? currentLevel->labelName : FString("");
	}

	static const char* StyleName(FluidOverlayStyle style)
	{
		switch (style)
		{
		case FluidOverlayStyle::Waves: return "waves";
		case FluidOverlayStyle::Flow: return "flow";
		case FluidOverlayStyle::Fall: return "fall";
		case FluidOverlayStyle::Viscous: return "viscous";
		case FluidOverlayStyle::Authored: return "authored";
		case FluidOverlayStyle::Lava: return "lava";
		default: return "still";
		}
	}

	static bool ParseStyle(const char* value, FluidOverlayStyle& outStyle)
	{
		if (!stricmp(value, "still")) outStyle = FluidOverlayStyle::Still;
		else if (!stricmp(value, "waves")) outStyle = FluidOverlayStyle::Waves;
		else if (!stricmp(value, "flow")) outStyle = FluidOverlayStyle::Flow;
		else if (!stricmp(value, "fall")) outStyle = FluidOverlayStyle::Fall;
		else if (!stricmp(value, "viscous")) outStyle = FluidOverlayStyle::Viscous;
		else if (!stricmp(value, "authored")) outStyle = FluidOverlayStyle::Authored;
		else if (!stricmp(value, "lava")) outStyle = FluidOverlayStyle::Lava;
		else return false;
		return true;
	}

	static const char* SurfaceClassName(FluidOverlaySurfaceClass surfaceClass)
	{
		switch (surfaceClass)
		{
		case FluidOverlaySurfaceClass::Flat: return "flat";
		case FluidOverlaySurfaceClass::Wall: return "wall";
		default: return "any";
		}
	}

	static const char* FoamBlendName(FluidOverlayFoamBlend blend)
	{
		return blend == FluidOverlayFoamBlend::Smooth ? "smooth" : "legacy";
	}

	static bool ParseFoamBlend(const char* value, FluidOverlayFoamBlend& outBlend)
	{
		if (!stricmp(value, "legacy")) outBlend = FluidOverlayFoamBlend::Legacy;
		else if (!stricmp(value, "smooth")) outBlend = FluidOverlayFoamBlend::Smooth;
		else return false;
		return true;
	}

	static bool ParseSurfaceClass(const char* value, FluidOverlaySurfaceClass& outClass)
	{
		if (!stricmp(value, "any")) outClass = FluidOverlaySurfaceClass::Any;
		else if (!stricmp(value, "flat")) outClass = FluidOverlaySurfaceClass::Flat;
		else if (!stricmp(value, "wall")) outClass = FluidOverlaySurfaceClass::Wall;
		else return false;
		return true;
	}

	static bool ParseOnOff(const char* value, bool& outValue)
	{
		if (!stricmp(value, "on") || !stricmp(value, "true") || !stricmp(value, "yes") || !strcmp(value, "1"))
		{
			outValue = true;
			return true;
		}
		if (!stricmp(value, "off") || !stricmp(value, "false") || !stricmp(value, "no") || !strcmp(value, "0"))
		{
			outValue = false;
			return true;
		}
		return false;
	}

	static FluidOverlayParameters MakeStyleDefaults(FluidOverlayStyle style)
	{
		FluidOverlayParameters result;
		result.presentMask = 0;
		result.style = style;
		switch (style)
		{
		case FluidOverlayStyle::Still:
			result.amplitude = 0.035f;
			result.speed = 0.08f;
			result.lacunarity = 1.70f;
			result.gain = 0.25f;
			result.warp = 0.35f;
			result.choppiness = 1.20f;
			result.octaveRotation = 29.0f;
			result.choppyRelaxation = 0.35f;
			result.noiseScale = 0.55f;
			result.maxSlope = 0.40f;
			result.crestGain = 0.20f;
			result.normalStrength = 0.42f;
			result.octaves = 2;
			break;
		case FluidOverlayStyle::Waves:
			result.amplitude = 0.18f;
			result.speed = 0.32f;
			result.warp = 0.80f;
			result.choppiness = 3.20f;
			result.maxSlope = 1.25f;
			result.normalStrength = 0.90f;
			result.octaves = 4;
			break;
		case FluidOverlayStyle::Flow:
			result.scale = 48.0f;
			result.amplitude = 0.12f;
			result.speed = 0.85f;
			result.anisotropy = 1.8f;
			result.warp = 0.70f;
			result.choppiness = 3.0f;
			result.octaveRotation = 24.0f;
			result.noiseScale = 0.70f;
			result.maxSlope = 1.40f;
			result.normalStrength = 0.95f;
			result.octaves = 4;
			break;
		case FluidOverlayStyle::Fall:
			result.scale = 40.0f;
			result.amplitude = 0.10f;
			result.speed = 1.15f;
			result.flow[0] = 0.0f;
			result.flow[1] = -1.0f;
			result.anisotropy = 2.4f;
			result.lacunarity = 2.0f;
			result.gain = 0.26f;
			result.warp = 0.65f;
			result.choppiness = 3.20f;
			result.octaveRotation = 12.0f;
			result.choppyRelaxation = 0.15f;
			result.noiseScale = 0.70f;
			result.maxSlope = 1.40f;
			result.crestGain = 0.28f;
			result.normalStrength = 0.90f;
			result.octaves = 4;
			break;
		case FluidOverlayStyle::Viscous:
			result.bodyColor[0] = 0.035f; result.bodyColor[1] = 0.16f; result.bodyColor[2] = 0.045f;
			result.undersideColor[0] = 0.018f; result.undersideColor[1] = 0.07f; result.undersideColor[2] = 0.022f;
			result.crestColor[0] = 0.12f; result.crestColor[1] = 0.28f; result.crestColor[2] = 0.10f;
			result.roughness = 0.28f;
			result.undersideRoughness = 0.38f;
			result.opacity = 0.98f;
			result.scale = 58.0f;
			result.amplitude = 0.09f;
			result.speed = 0.11f;
			result.anisotropy = 1.25f;
			result.lacunarity = 1.70f;
			result.gain = 0.35f;
			result.warp = 0.90f;
			result.choppiness = 1.60f;
			result.octaveRotation = 31.0f;
			result.choppyRelaxation = 0.35f;
			result.noiseScale = 0.45f;
			result.maxSlope = 0.80f;
			result.crestGain = 0.40f;
			result.normalStrength = 0.65f;
			result.octaves = 4;
			break;
		case FluidOverlayStyle::Authored:
			result.amplitude = 0.045f;
			result.speed = 0.0f;
			result.gain = 0.20f;
			result.warp = 0.30f;
			result.choppiness = 1.20f;
			result.octaveRotation = 29.0f;
			result.maxSlope = 0.45f;
			result.normalStrength = 0.50f;
			result.octaves = 2;
			break;
		case FluidOverlayStyle::Lava:
			result.bodyColor[0] = 0.12f; result.bodyColor[1] = 0.018f; result.bodyColor[2] = 0.004f;
			result.undersideColor[0] = 0.055f; result.undersideColor[1] = 0.006f; result.undersideColor[2] = 0.001f;
			result.crestColor[0] = 1.0f; result.crestColor[1] = 0.22f; result.crestColor[2] = 0.012f;
			result.emissionColor[0] = 1.0f; result.emissionColor[1] = 0.16f; result.emissionColor[2] = 0.006f;
			result.emission = 7.0f;
			result.roughness = 0.38f;
			result.undersideRoughness = 0.48f;
			result.opacity = 1.0f;
			result.scale = 52.0f;
			result.amplitude = 0.08f;
			result.speed = 0.10f;
			result.anisotropy = 1.2f;
			result.lacunarity = 1.80f;
			result.gain = 0.30f;
			result.warp = 1.0f;
			result.choppiness = 2.40f;
			result.octaveRotation = 41.0f;
			result.choppyRelaxation = 0.28f;
			result.noiseScale = 0.50f;
			result.maxSlope = 0.90f;
			result.crestGain = 0.70f;
			result.normalStrength = 0.70f;
			result.octaves = 4;
			break;
		}
		return result;
	}

	static void Copy3(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}

	static void ApplyParameterOverrides(FluidOverlayParameters& destination, const FluidOverlayParameters& source)
	{
		const uint64_t mask = source.presentMask;
		if (mask & FluidOverlayParameter_Style) destination.style = source.style;
		if (mask & FluidOverlayParameter_BodyColor) Copy3(source.bodyColor, destination.bodyColor);
		if (mask & FluidOverlayParameter_UndersideColor) Copy3(source.undersideColor, destination.undersideColor);
		if (mask & FluidOverlayParameter_CrestColor) Copy3(source.crestColor, destination.crestColor);
		if (mask & FluidOverlayParameter_FoamColor) Copy3(source.foamColor, destination.foamColor);
		if (mask & FluidOverlayParameter_EmissionColor) Copy3(source.emissionColor, destination.emissionColor);
		if (mask & FluidOverlayParameter_Emission) destination.emission = source.emission;
		if (mask & FluidOverlayParameter_Roughness) destination.roughness = source.roughness;
		if (mask & FluidOverlayParameter_UndersideRoughness) destination.undersideRoughness = source.undersideRoughness;
		if (mask & FluidOverlayParameter_Opacity) destination.opacity = source.opacity;
		if (mask & FluidOverlayParameter_TextureMix) destination.textureMix = source.textureMix;
		if (mask & FluidOverlayParameter_Scale) destination.scale = source.scale;
		if (mask & FluidOverlayParameter_Amplitude) destination.amplitude = source.amplitude;
		if (mask & FluidOverlayParameter_Frequency) destination.frequency = source.frequency;
		if (mask & FluidOverlayParameter_Lacunarity) destination.lacunarity = source.lacunarity;
		if (mask & FluidOverlayParameter_Gain) destination.gain = source.gain;
		if (mask & FluidOverlayParameter_TimeScale) destination.timeScale = source.timeScale;
		if (mask & FluidOverlayParameter_Speed) destination.speed = source.speed;
		if (mask & FluidOverlayParameter_FollowPan) destination.followPan = source.followPan;
		if (mask & FluidOverlayParameter_Flow)
		{
			destination.flow[0] = source.flow[0];
			destination.flow[1] = source.flow[1];
		}
		if (mask & FluidOverlayParameter_Anisotropy) destination.anisotropy = source.anisotropy;
		if (mask & FluidOverlayParameter_Warp) destination.warp = source.warp;
		if (mask & FluidOverlayParameter_Choppiness) destination.choppiness = source.choppiness;
		if (mask & FluidOverlayParameter_OctaveRotation) destination.octaveRotation = source.octaveRotation;
		if (mask & FluidOverlayParameter_ChoppyRelaxation) destination.choppyRelaxation = source.choppyRelaxation;
		if (mask & FluidOverlayParameter_NoiseScale) destination.noiseScale = source.noiseScale;
		if (mask & FluidOverlayParameter_MaxSlope) destination.maxSlope = source.maxSlope;
		if (mask & FluidOverlayParameter_CrestGain) destination.crestGain = source.crestGain;
		if (mask & FluidOverlayParameter_CrustSoftness) destination.crustSoftness = source.crustSoftness;
		if (mask & FluidOverlayParameter_CrustDarkening) destination.crustDarkening = source.crustDarkening;
		if (mask & FluidOverlayParameter_FoamAmount) destination.foamAmount = source.foamAmount;
		if (mask & FluidOverlayParameter_FoamRoughness) destination.foamRoughness = source.foamRoughness;
		if (mask & FluidOverlayParameter_FoamThreshold) destination.foamThreshold = source.foamThreshold;
		if (mask & FluidOverlayParameter_FoamSoftness) destination.foamSoftness = source.foamSoftness;
		if (mask & FluidOverlayParameter_FoamBreakupScale) destination.foamBreakupScale = source.foamBreakupScale;
		if (mask & FluidOverlayParameter_FoamBreakupStrength) destination.foamBreakupStrength = source.foamBreakupStrength;
		if (mask & FluidOverlayParameter_FoamLifetime) destination.foamLifetime = source.foamLifetime;
		if (mask & FluidOverlayParameter_FoamTrailGain) destination.foamTrailGain = source.foamTrailGain;
		if (mask & FluidOverlayParameter_FoamUndersideStrength) destination.foamUndersideStrength = source.foamUndersideStrength;
		if (mask & FluidOverlayParameter_FoamContrast) destination.foamContrast = source.foamContrast;
		if (mask & FluidOverlayParameter_FoamEdgeWidth) destination.foamEdgeWidth = source.foamEdgeWidth;
		if (mask & FluidOverlayParameter_FoamCutoutScale) destination.foamCutoutScale = source.foamCutoutScale;
		if (mask & FluidOverlayParameter_FoamCutoutStrength) destination.foamCutoutStrength = source.foamCutoutStrength;
		if (mask & FluidOverlayParameter_FoamBrightness) destination.foamBrightness = source.foamBrightness;
		if (mask & FluidOverlayParameter_FoamBlend) destination.foamBlend = source.foamBlend;
		if (mask & FluidOverlayParameter_FoamBlendWidth) destination.foamBlendWidth = source.foamBlendWidth;
		if (mask & FluidOverlayParameter_NormalStrength) destination.normalStrength = source.normalStrength;
		if (mask & FluidOverlayParameter_VirtualHeight) destination.virtualHeight = source.virtualHeight;
		if (mask & FluidOverlayParameter_Seed) destination.seed = source.seed;
		if (mask & FluidOverlayParameter_Octaves) destination.octaves = source.octaves;
		destination.presentMask = 0;
	}

	static void SanitizeParameters(FluidOverlayParameters& value)
	{
		auto finiteOr = [](float input, float fallback)
		{
			return std::isfinite(input) ? input : fallback;
		};
		for (uint32_t i = 0; i < 3; ++i)
		{
			value.bodyColor[i] = std::max(0.0f, finiteOr(value.bodyColor[i], 0.0f));
			value.undersideColor[i] = std::max(0.0f, finiteOr(value.undersideColor[i], 0.0f));
			value.crestColor[i] = std::max(0.0f, finiteOr(value.crestColor[i], 0.0f));
			value.foamColor[i] = std::max(0.0f, finiteOr(value.foamColor[i], 0.0f));
			value.emissionColor[i] = std::max(0.0f, finiteOr(value.emissionColor[i], 0.0f));
		}
		value.emission = std::max(0.0f, finiteOr(value.emission, 0.0f));
		value.roughness = std::clamp(finiteOr(value.roughness, 0.08f), 0.0f, 1.0f);
		value.undersideRoughness = std::clamp(finiteOr(value.undersideRoughness, 0.16f), 0.0f, 1.0f);
		value.opacity = std::clamp(finiteOr(value.opacity, 1.0f), 0.0f, 1.0f);
		value.textureMix = std::clamp(finiteOr(value.textureMix, -1.0f), -1.0f, 1.0f);
		value.scale = std::max(0.001f, finiteOr(value.scale, 96.0f));
		value.amplitude = std::max(0.0f, finiteOr(value.amplitude, 0.0f));
		value.frequency = std::max(0.0f, finiteOr(value.frequency, 1.0f));
		value.lacunarity = std::clamp(finiteOr(value.lacunarity, 1.90f), 1.01f, 4.0f);
		value.gain = std::clamp(finiteOr(value.gain, 0.28f), 0.0f, 1.0f);
		value.timeScale = finiteOr(value.timeScale, 1.0f);
		value.speed = finiteOr(value.speed, 0.0f);
		value.flow[0] = finiteOr(value.flow[0], 1.0f);
		value.flow[1] = finiteOr(value.flow[1], 0.0f);
		// followPan is already a canonical bool; keep this independent opt-in
		// across styles. Carrier support is decided by the renderer, not parsing.
		value.anisotropy = std::max(0.0f, finiteOr(value.anisotropy, 1.0f));
		value.warp = std::clamp(finiteOr(value.warp, 0.0f), 0.0f, 4.0f);
		value.choppiness = std::clamp(finiteOr(value.choppiness, 1.0f), 0.05f, 4.0f);
		value.octaveRotation = std::remainder(finiteOr(value.octaveRotation, 37.0f), 360.0f);
		value.choppyRelaxation = std::clamp(finiteOr(value.choppyRelaxation, 0.20f), 0.0f, 1.0f);
		value.noiseScale = std::clamp(finiteOr(value.noiseScale, 0.65f), 0.05f, 4.0f);
		value.maxSlope = std::clamp(finiteOr(value.maxSlope, 1.25f), 0.0f, 4.0f);
		value.crestGain = std::clamp(finiteOr(value.crestGain, 0.35f), 0.0f, 4.0f);
		value.crustSoftness = std::clamp(finiteOr(value.crustSoftness, 0.16f), 0.001f, 4.0f);
		value.crustDarkening = std::clamp(finiteOr(value.crustDarkening, 0.72f), 0.0f, 1.0f);
		value.foamAmount = std::clamp(finiteOr(value.foamAmount, 0.0f), 0.0f, 1.0f);
		value.foamRoughness = std::clamp(finiteOr(value.foamRoughness, 0.85f), 0.0f, 1.0f);
		value.foamThreshold = std::clamp(finiteOr(value.foamThreshold, 0.65f), 0.0f, 1.0f);
		value.foamSoftness = std::clamp(finiteOr(value.foamSoftness, 0.10f), 0.0f, 1.0f);
		value.foamBreakupScale = std::clamp(finiteOr(value.foamBreakupScale, 2.0f), 0.05f, 64.0f);
		value.foamBreakupStrength = std::clamp(finiteOr(value.foamBreakupStrength, 0.50f), 0.0f, 1.0f);
		value.foamLifetime = std::clamp(finiteOr(value.foamLifetime, 0.0f), 0.0f, 60.0f);
		value.foamTrailGain = std::clamp(finiteOr(value.foamTrailGain, 0.0f), 0.0f, 4.0f);
		value.foamUndersideStrength = std::clamp(finiteOr(value.foamUndersideStrength, 0.0f), 0.0f, 1.0f);
		value.foamContrast = std::clamp(finiteOr(value.foamContrast, 1.0f), 0.1f, 16.0f);
		value.foamEdgeWidth = std::clamp(finiteOr(value.foamEdgeWidth, 0.10f), 0.0f, 0.5f);
		value.foamCutoutScale = std::clamp(finiteOr(value.foamCutoutScale, 2.0f), 0.05f, 64.0f);
		value.foamCutoutStrength = std::clamp(finiteOr(value.foamCutoutStrength, 0.0f), 0.0f, 1.0f);
		value.foamBrightness = std::clamp(finiteOr(value.foamBrightness, 1.0f), 0.0f, 4.0f);
		if (value.foamBlend != FluidOverlayFoamBlend::Smooth) value.foamBlend = FluidOverlayFoamBlend::Legacy;
		value.foamBlendWidth = std::clamp(finiteOr(value.foamBlendWidth, 0.20f), 0.0f, 0.5f);
		value.normalStrength = std::max(0.0f, finiteOr(value.normalStrength, 0.0f));
		value.virtualHeight = std::max(0.0f, finiteOr(value.virtualHeight, 0.0f));
		value.octaves = std::clamp(value.octaves, 1u, 4u);
		value.presentMask = 0;
	}

	static uint64_t ComputeParameterContentKey(const FluidOverlayParameters& value)
	{
		uint64_t hash = HashOffset;
		hash = HashValue(hash, (uint8_t)value.style);
		hash = HashValue(hash, (uint8_t)value.followPan);
		hash = HashValue(hash, (uint8_t)value.foamBlend);
		for (float component : value.bodyColor) hash = HashValue(hash, CanonicalFloatBits(component));
		for (float component : value.undersideColor) hash = HashValue(hash, CanonicalFloatBits(component));
		for (float component : value.crestColor) hash = HashValue(hash, CanonicalFloatBits(component));
		for (float component : value.foamColor) hash = HashValue(hash, CanonicalFloatBits(component));
		for (float component : value.emissionColor) hash = HashValue(hash, CanonicalFloatBits(component));
		const float scalarValues[] = {
			value.emission, value.roughness, value.undersideRoughness, value.opacity, value.textureMix,
			value.scale, value.amplitude, value.frequency, value.lacunarity, value.gain,
			value.timeScale, value.speed,
			value.flow[0], value.flow[1], value.anisotropy, value.warp, value.choppiness,
			value.octaveRotation, value.choppyRelaxation, value.noiseScale, value.maxSlope,
			value.crestGain, value.crustSoftness, value.crustDarkening,
			value.foamAmount, value.foamRoughness, value.foamThreshold,
			value.foamSoftness, value.foamBreakupScale, value.foamBreakupStrength,
			value.foamLifetime, value.foamTrailGain, value.foamUndersideStrength,
			value.foamContrast, value.foamEdgeWidth, value.foamCutoutScale,
			value.foamCutoutStrength, value.foamBrightness, value.foamBlendWidth,
			value.normalStrength, value.virtualHeight,
		};
		for (float scalar : scalarValues) hash = HashValue(hash, CanonicalFloatBits(scalar));
		hash = HashValue(hash, value.seed);
		return HashValue(hash, value.octaves);
	}

	static bool ParametersEqual(const FluidOverlayParameters& a, const FluidOverlayParameters& b)
	{
		if (a.style != b.style || a.followPan != b.followPan || a.seed != b.seed || a.octaves != b.octaves) return false;
		if (a.foamBlend != b.foamBlend) return false;
		const float* aVectors[] = { a.bodyColor, a.undersideColor, a.crestColor, a.emissionColor, a.foamColor };
		const float* bVectors[] = { b.bodyColor, b.undersideColor, b.crestColor, b.emissionColor, b.foamColor };
		for (size_t vectorIndex = 0; vectorIndex < std::size(aVectors); ++vectorIndex)
		{
			for (size_t component = 0; component < 3; ++component)
			{
				if (CanonicalFloatBits(aVectors[vectorIndex][component]) != CanonicalFloatBits(bVectors[vectorIndex][component])) return false;
			}
		}
		const float aScalars[] = {
			a.emission, a.roughness, a.undersideRoughness, a.opacity, a.textureMix, a.scale, a.amplitude,
			a.frequency, a.lacunarity, a.gain, a.timeScale, a.speed, a.flow[0], a.flow[1],
			a.anisotropy, a.warp, a.choppiness, a.octaveRotation, a.choppyRelaxation,
			a.noiseScale, a.maxSlope, a.crestGain, a.crustSoftness, a.crustDarkening,
			a.foamAmount, a.foamRoughness,
			a.foamThreshold, a.foamSoftness, a.foamBreakupScale, a.foamBreakupStrength,
			a.foamLifetime, a.foamTrailGain, a.foamUndersideStrength,
			a.foamContrast, a.foamEdgeWidth, a.foamCutoutScale, a.foamCutoutStrength,
			a.foamBrightness, a.foamBlendWidth,
			a.normalStrength, a.virtualHeight,
		};
		const float bScalars[] = {
			b.emission, b.roughness, b.undersideRoughness, b.opacity, b.textureMix, b.scale, b.amplitude,
			b.frequency, b.lacunarity, b.gain, b.timeScale, b.speed, b.flow[0], b.flow[1],
			b.anisotropy, b.warp, b.choppiness, b.octaveRotation, b.choppyRelaxation,
			b.noiseScale, b.maxSlope, b.crestGain, b.crustSoftness, b.crustDarkening,
			b.foamAmount, b.foamRoughness,
			b.foamThreshold, b.foamSoftness, b.foamBreakupScale, b.foamBreakupStrength,
			b.foamLifetime, b.foamTrailGain, b.foamUndersideStrength,
			b.foamContrast, b.foamEdgeWidth, b.foamCutoutScale, b.foamCutoutStrength,
			b.foamBrightness, b.foamBlendWidth,
			b.normalStrength, b.virtualHeight,
		};
		for (size_t i = 0; i < std::size(aScalars); ++i)
		{
			if (CanonicalFloatBits(aScalars[i]) != CanonicalFloatBits(bScalars[i])) return false;
		}
		return true;
	}

	static bool ParametersLess(const FluidOverlayParameters& a, const FluidOverlayParameters& b)
	{
		if (a.style != b.style) return (uint8_t)a.style < (uint8_t)b.style;
		if (a.followPan != b.followPan) return a.followPan < b.followPan;
		if (a.foamBlend != b.foamBlend) return (uint8_t)a.foamBlend < (uint8_t)b.foamBlend;
		const auto getFloatBits = [](const FluidOverlayParameters& value)
		{
			std::array<uint32_t, 56> bits = {};
			size_t index = 0;
			const float* vectors[] = { value.bodyColor, value.undersideColor, value.crestColor, value.emissionColor, value.foamColor };
			for (const float* vector : vectors)
			{
				for (size_t component = 0; component < 3; ++component) bits[index++] = CanonicalFloatBits(vector[component]);
			}
			const float scalars[] = {
				value.emission, value.roughness, value.undersideRoughness, value.opacity, value.textureMix,
				value.scale, value.amplitude, value.frequency, value.lacunarity, value.gain,
				value.timeScale, value.speed,
				value.flow[0], value.flow[1], value.anisotropy, value.warp, value.choppiness,
				value.octaveRotation, value.choppyRelaxation, value.noiseScale, value.maxSlope,
				value.crestGain, value.crustSoftness, value.crustDarkening,
				value.foamAmount, value.foamRoughness,
				value.foamThreshold, value.foamSoftness, value.foamBreakupScale,
				value.foamBreakupStrength, value.foamLifetime, value.foamTrailGain,
				value.foamUndersideStrength,
				value.foamContrast, value.foamEdgeWidth, value.foamCutoutScale,
				value.foamCutoutStrength, value.foamBrightness, value.foamBlendWidth,
				value.normalStrength, value.virtualHeight,
			};
			for (float scalar : scalars) bits[index++] = CanonicalFloatBits(scalar);
			return bits;
		};
		const auto aBits = getFloatBits(a);
		const auto bBits = getFloatBits(b);
		if (aBits != bBits) return aBits < bBits;
		if (a.seed != b.seed) return a.seed < b.seed;
		return a.octaves < b.octaves;
	}

	static ResolvedFluidOverlayProfile MakeBuiltInProfile(const char* id, FluidOverlayStyle style, int32_t seed)
	{
		ResolvedFluidOverlayProfile profile;
		profile.id = id;
		profile.source.sourceName = "<builtin>";
		profile.source.orderIndex = 0;
		profile.builtIn = true;
		profile.parameters = MakeStyleDefaults(style);
		profile.parameters.seed = seed;
		SanitizeParameters(profile.parameters);
		profile.contentKey = ComputeParameterContentKey(profile.parameters);
		return profile;
	}

	static std::vector<ResolvedFluidOverlayProfile> BuildBuiltInProfiles()
	{
		std::vector<ResolvedFluidOverlayProfile> profiles;
		profiles.push_back(MakeBuiltInProfile("water.calm", FluidOverlayStyle::Still, 11));
		profiles.push_back(MakeBuiltInProfile("water.waves", FluidOverlayStyle::Waves, 13));
		profiles.push_back(MakeBuiltInProfile("water.flow", FluidOverlayStyle::Flow, 17));
		profiles.push_back(MakeBuiltInProfile("water.fall", FluidOverlayStyle::Fall, 19));
		profiles.push_back(MakeBuiltInProfile("sludge.viscous", FluidOverlayStyle::Viscous, 23));
		profiles.push_back(MakeBuiltInProfile("lava", FluidOverlayStyle::Lava, 29));
		profiles.push_back(MakeBuiltInProfile("authored.micro", FluidOverlayStyle::Authored, 31));
		return profiles;
	}

	struct ParsedFluidOverlayDatabaseBuilder
	{
		ParsedFluidOverlayDatabase database;
		uint32_t nextOrderIndex = 0;
		std::unordered_map<std::string, int> profileLookup;
		std::unordered_map<std::string, int> materialRuleLookup;

		void AddProfile(const ParsedFluidOverlayProfile& profile)
		{
			const std::string key = MakeNormalizedKey(profile.id);
			auto it = profileLookup.find(key);
			if (it == profileLookup.end())
			{
				profileLookup.emplace(key, database.profiles.Size());
				database.profiles.Push(profile);
				return;
			}
			auto& existing = database.profiles[it->second];
			if (existing.source.lumpNum == profile.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "FLUIDOVR warning: duplicate profile '%s' in %s; using the last whole definition.\n",
					profile.id.GetChars(), profile.source.sourceName.GetChars());
			}
			existing = profile;
		}

		void AddMaterialRule(const ParsedFluidOverlayMaterialRule& rule)
		{
			const std::string key = MakeMaterialRuleKey(rule.mapName, rule.id);
			auto it = materialRuleLookup.find(key);
			if (it == materialRuleLookup.end())
			{
				materialRuleLookup.emplace(key, database.materialRules.Size());
				database.materialRules.Push(rule);
				return;
			}
			auto& existing = database.materialRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "FLUIDOVR warning: duplicate material '%s' in scope '%s' in %s; using the last whole definition.\n",
					rule.id.GetChars(), rule.mapName.IsEmpty() ? "global" : rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}
	};

	class FluidOverlayParser
	{
	public:
		FluidOverlayParser(int lumpNum, ParsedFluidOverlayDatabaseBuilder& builder, ParsedFluidOverlaySourceFile& sourceFile)
			: sc(lumpNum), builder(builder), sourceFile(sourceFile), lumpNum(lumpNum)
		{
			sc.SetCMode(true);
			sc.SetNoFatalErrors(true);
			sc.SetPrependMessage("FLUIDOVR: ");
		}

		void Parse()
		{
			while (sc.GetString())
			{
				if (sc.Compare("FLUIDOVR")) ParseRootBlock();
				else
				{
					sc.ScriptMessage("Unknown top-level token '%s'; expected FLUIDOVR", sc.String);
					SkipUnknownDefinition();
				}
			}
			sourceFile.hadParseErrors = sc.ParseError;
		}

	private:
		FScanner sc;
		ParsedFluidOverlayDatabaseBuilder& builder;
		ParsedFluidOverlaySourceFile& sourceFile;
		int lumpNum = -1;

		FluidOverlaySourceLocation MakeSourceLocation(int lineStart)
		{
			FluidOverlaySourceLocation result;
			result.sourceName = sourceFile.sourceName;
			result.lumpNum = lumpNum;
			result.lineStart = lineStart;
			result.lineEnd = sc.GetMessageLine();
			result.orderIndex = ++builder.nextOrderIndex;
			return result;
		}

		void FinalizeSourceLocation(FluidOverlaySourceLocation& source)
		{
			source.lineEnd = sc.GetMessageLine();
		}

		void SkipUnknownDefinition()
		{
			while (sc.GetString())
			{
				if (sc.Compare("{"))
				{
					sc.SkipToEndOfBlock();
					return;
				}
				if (sc.Compare("}"))
				{
					sc.UnGet();
					return;
				}
			}
		}

		void SkipUnknownField(const char* context, const char* field)
		{
			sc.ScriptMessage("Unknown field '%s' in %s definition", field, context);
			if (sc.CheckString("{")) sc.SkipToEndOfBlock();
			else sc.MustGetAnyToken();
		}

		void MustGetVector(float* values, uint32_t count)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				sc.MustGetFloat();
				values[i] = (float)sc.Float;
			}
		}

		bool ParseParameterField(const char* context, FluidOverlayParameters& parameters)
		{
			if (sc.Compare("style") || sc.Compare("motion"))
			{
				sc.MustGetString();
				FluidOverlayStyle style;
				if (!ParseStyle(sc.String, style))
				{
					sc.ScriptMessage("Invalid fluid style '%s'; expected still, waves, flow, fall, viscous, authored, or lava", sc.String);
				}
				else
				{
					parameters.style = style;
					parameters.presentMask |= FluidOverlayParameter_Style;
				}
				return true;
			}
			if (sc.Compare("foamblend"))
			{
				sc.MustGetString();
				FluidOverlayFoamBlend blend;
				if (!ParseFoamBlend(sc.String, blend))
				{
					sc.ScriptMessage("Invalid foamblend value '%s'; expected legacy or smooth", sc.String);
				}
				else
				{
					parameters.foamBlend = blend;
					parameters.presentMask |= FluidOverlayParameter_FoamBlend;
				}
				return true;
			}
			if (sc.Compare("followpan"))
			{
				sc.MustGetString();
				bool followPan = false;
				if (!ParseOnOff(sc.String, followPan))
				{
					sc.ScriptMessage("Invalid followpan value '%s'; expected off/on", sc.String);
				}
				else
				{
					parameters.followPan = followPan;
					parameters.presentMask |= FluidOverlayParameter_FollowPan;
				}
				return true;
			}
			struct VectorField { const char* name; uint64_t bit; float* value; uint32_t count; };
			VectorField vectorFields[] = {
				{ "bodycolor", FluidOverlayParameter_BodyColor, parameters.bodyColor, 3 },
				{ "undersidecolor", FluidOverlayParameter_UndersideColor, parameters.undersideColor, 3 },
				{ "crestcolor", FluidOverlayParameter_CrestColor, parameters.crestColor, 3 },
				{ "foamcolor", FluidOverlayParameter_FoamColor, parameters.foamColor, 3 },
				{ "emissioncolor", FluidOverlayParameter_EmissionColor, parameters.emissionColor, 3 },
				{ "flow", FluidOverlayParameter_Flow, parameters.flow, 2 },
			};
			for (const auto& field : vectorFields)
			{
				if (sc.Compare(field.name))
				{
					MustGetVector(field.value, field.count);
					parameters.presentMask |= field.bit;
					return true;
				}
			}

			struct FloatField { const char* name; uint64_t bit; float* value; };
			FloatField floatFields[] = {
				{ "emission", FluidOverlayParameter_Emission, &parameters.emission },
				{ "roughness", FluidOverlayParameter_Roughness, &parameters.roughness },
				{ "undersideroughness", FluidOverlayParameter_UndersideRoughness, &parameters.undersideRoughness },
				{ "opacity", FluidOverlayParameter_Opacity, &parameters.opacity },
				{ "texturemix", FluidOverlayParameter_TextureMix, &parameters.textureMix },
				{ "scale", FluidOverlayParameter_Scale, &parameters.scale },
				{ "amplitude", FluidOverlayParameter_Amplitude, &parameters.amplitude },
				{ "frequency", FluidOverlayParameter_Frequency, &parameters.frequency },
				{ "lacunarity", FluidOverlayParameter_Lacunarity, &parameters.lacunarity },
				{ "gain", FluidOverlayParameter_Gain, &parameters.gain },
				{ "timescale", FluidOverlayParameter_TimeScale, &parameters.timeScale },
				{ "speed", FluidOverlayParameter_Speed, &parameters.speed },
				{ "anisotropy", FluidOverlayParameter_Anisotropy, &parameters.anisotropy },
				{ "warp", FluidOverlayParameter_Warp, &parameters.warp },
				{ "choppiness", FluidOverlayParameter_Choppiness, &parameters.choppiness },
				{ "octaverotation", FluidOverlayParameter_OctaveRotation, &parameters.octaveRotation },
				{ "choppyrelaxation", FluidOverlayParameter_ChoppyRelaxation, &parameters.choppyRelaxation },
				{ "noisescale", FluidOverlayParameter_NoiseScale, &parameters.noiseScale },
				{ "maxslope", FluidOverlayParameter_MaxSlope, &parameters.maxSlope },
				{ "crestgain", FluidOverlayParameter_CrestGain, &parameters.crestGain },
				{ "crustsoftness", FluidOverlayParameter_CrustSoftness, &parameters.crustSoftness },
				{ "crustdarkening", FluidOverlayParameter_CrustDarkening, &parameters.crustDarkening },
				{ "foamamount", FluidOverlayParameter_FoamAmount, &parameters.foamAmount },
				{ "foamroughness", FluidOverlayParameter_FoamRoughness, &parameters.foamRoughness },
				{ "foamthreshold", FluidOverlayParameter_FoamThreshold, &parameters.foamThreshold },
				{ "foamsoftness", FluidOverlayParameter_FoamSoftness, &parameters.foamSoftness },
				{ "foambreakupscale", FluidOverlayParameter_FoamBreakupScale, &parameters.foamBreakupScale },
				{ "foambreakupstrength", FluidOverlayParameter_FoamBreakupStrength, &parameters.foamBreakupStrength },
				{ "foamlifetime", FluidOverlayParameter_FoamLifetime, &parameters.foamLifetime },
				{ "foamtrailgain", FluidOverlayParameter_FoamTrailGain, &parameters.foamTrailGain },
				{ "foamundersidestrength", FluidOverlayParameter_FoamUndersideStrength, &parameters.foamUndersideStrength },
				{ "foamcontrast", FluidOverlayParameter_FoamContrast, &parameters.foamContrast },
				{ "foamedgewidth", FluidOverlayParameter_FoamEdgeWidth, &parameters.foamEdgeWidth },
				{ "foamcutoutscale", FluidOverlayParameter_FoamCutoutScale, &parameters.foamCutoutScale },
				{ "foamcutoutstrength", FluidOverlayParameter_FoamCutoutStrength, &parameters.foamCutoutStrength },
				{ "foambrightness", FluidOverlayParameter_FoamBrightness, &parameters.foamBrightness },
				{ "foamblendwidth", FluidOverlayParameter_FoamBlendWidth, &parameters.foamBlendWidth },
				{ "normalstrength", FluidOverlayParameter_NormalStrength, &parameters.normalStrength },
				{ "virtualheight", FluidOverlayParameter_VirtualHeight, &parameters.virtualHeight },
			};
			for (const auto& field : floatFields)
			{
				if (sc.Compare(field.name))
				{
					sc.MustGetFloat();
					*field.value = (float)sc.Float;
					parameters.presentMask |= field.bit;
					return true;
				}
			}
			if (sc.Compare("seed"))
			{
				sc.MustGetNumber();
				parameters.seed = sc.Number;
				parameters.presentMask |= FluidOverlayParameter_Seed;
				return true;
			}
			if (sc.Compare("octaves"))
			{
				sc.MustGetNumber();
				parameters.octaves = (uint32_t)std::clamp(sc.Number, 1, 4);
				parameters.presentMask |= FluidOverlayParameter_Octaves;
				return true;
			}
			(void)context;
			return false;
		}

		void ParseRootBlock()
		{
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("profile")) ParseProfile();
				else if (sc.Compare("material")) ParseMaterial("");
				else if (sc.Compare("map")) ParseMap();
				else
				{
					sc.ScriptMessage("Unknown FLUIDOVR block '%s'", sc.String);
					SkipUnknownDefinition();
				}
			}
		}

		void ParseProfile()
		{
			sc.MustGetString();
			ParsedFluidOverlayProfile profile;
			profile.id = sc.String;
			profile.source = MakeSourceLocation(sc.GetMessageLine());
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (!ParseParameterField("profile", profile.parameters)) SkipUnknownField("profile", sc.String);
			}
			if (profile.id.IsEmpty()) sc.ScriptMessage("Fluid profile id must not be empty");
			FinalizeSourceLocation(profile.source);
			builder.AddProfile(profile);
		}

		void ParseMap()
		{
			sc.MustGetString();
			const FString mapName = CanonicalizeMapName(sc.String);
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("material")) ParseMaterial(mapName);
				else
				{
					sc.ScriptMessage("Unknown map-local FLUIDOVR block '%s'", sc.String);
					SkipUnknownDefinition();
				}
			}
		}

		void ParseMaterial(const FString& mapName)
		{
			sc.MustGetString();
			ParsedFluidOverlayMaterialRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("tile"))
				{
					sc.MustGetNumber();
					rule.tileFilters.Push(sc.Number);
				}
				else if (sc.Compare("tilerange"))
				{
					sc.MustGetNumber();
					const int first = sc.Number;
					sc.MustGetNumber();
					const int last = sc.Number;
					rule.tileRanges.Push({ std::min(first, last), std::max(first, last) });
				}
				else if (sc.Compare("tilefamily"))
				{
					sc.MustGetNumber();
					rule.tileFamilies.Push(sc.Number);
				}
				else if (sc.Compare("texture"))
				{
					sc.MustGetString();
					rule.textureNames.Push(sc.String);
				}
				else if (sc.Compare("palette"))
				{
					sc.MustGetNumber();
					rule.hasPalette = true;
					rule.palette = sc.Number;
				}
				else if (sc.Compare("surfaceclass"))
				{
					sc.MustGetString();
					rule.hasSurfaceClass = true;
					if (!ParseSurfaceClass(sc.String, rule.surfaceClass))
					{
						sc.ScriptMessage("Invalid surfaceclass '%s'; expected any, flat, or wall", sc.String);
						rule.surfaceClass = FluidOverlaySurfaceClass::Any;
					}
				}
				else if (sc.Compare("profile"))
				{
					sc.MustGetString();
					rule.hasProfile = true;
					rule.profileId = sc.String;
				}
				else if (sc.Compare("disable"))
				{
					sc.MustGetString();
					rule.hasDisable = true;
					if (!ParseOnOff(sc.String, rule.disable))
					{
						sc.ScriptMessage("Invalid disable value '%s'; expected off/on", sc.String);
					}
				}
				else if (!ParseParameterField("material", rule.overrides))
				{
					SkipUnknownField("material", sc.String);
				}
			}

			if (rule.tileFilters.Size() == 0 && rule.tileRanges.Size() == 0 &&
				rule.tileFamilies.Size() == 0 && rule.textureNames.Size() == 0)
			{
				sc.ScriptMessage("Fluid material '%s' has no tile, tilerange, tilefamily, or texture selector", rule.id.GetChars());
			}
			if (!rule.disable && (!rule.hasProfile || rule.profileId.IsEmpty()))
			{
				sc.ScriptMessage("Fluid material '%s' requires a profile unless disable is on", rule.id.GetChars());
			}
			FinalizeSourceLocation(rule.source);
			builder.AddMaterialRule(rule);
		}
	};

	static FString FormatFloat(float value)
	{
		// Nine significant digits are sufficient to round-trip every finite
		// IEEE-754 float through the normalized text form.
		FString text = FStringf("%.9g", value);
		if (text.IndexOf('.') < 0 && text.IndexOf('e') < 0 && text.IndexOf('E') < 0) text << ".0";
		return text;
	}

	static FString QuoteString(const FString& value)
	{
		FString result = "\"";
		for (const char* p = value.GetChars(); *p != 0; ++p)
		{
			if (*p == '\\' || *p == '\"') result << '\\';
			result << *p;
		}
		result << '\"';
		return result;
	}

	static void AppendLine(FString& text, uint32_t indent, const FString& line)
	{
		for (uint32_t i = 0; i < indent; ++i) text << "    ";
		text << line << "\n";
	}

	static void AppendVector(FString& text, uint32_t indent, const char* name, const float* values, uint32_t count)
	{
		FString line = name;
		for (uint32_t i = 0; i < count; ++i) line << " " << FormatFloat(values[i]);
		AppendLine(text, indent, line);
	}

	static void AppendParameterFields(FString& text, uint32_t indent, const FluidOverlayParameters& parameters)
	{
		const uint64_t mask = parameters.presentMask;
		if (mask & FluidOverlayParameter_Style) AppendLine(text, indent, FStringf("style %s", StyleName(parameters.style)));
		if (mask & FluidOverlayParameter_FollowPan) AppendLine(text, indent, FStringf("followpan %s", parameters.followPan ? "on" : "off"));
		if (mask & FluidOverlayParameter_FoamBlend) AppendLine(text, indent, FStringf("foamblend %s", FoamBlendName(parameters.foamBlend)));
		if (mask & FluidOverlayParameter_BodyColor) AppendVector(text, indent, "bodycolor", parameters.bodyColor, 3);
		if (mask & FluidOverlayParameter_UndersideColor) AppendVector(text, indent, "undersidecolor", parameters.undersideColor, 3);
		if (mask & FluidOverlayParameter_CrestColor) AppendVector(text, indent, "crestcolor", parameters.crestColor, 3);
		if (mask & FluidOverlayParameter_FoamColor) AppendVector(text, indent, "foamcolor", parameters.foamColor, 3);
		if (mask & FluidOverlayParameter_EmissionColor) AppendVector(text, indent, "emissioncolor", parameters.emissionColor, 3);
		struct FloatField { uint64_t bit; const char* name; float value; };
		const FloatField fields[] = {
			{ FluidOverlayParameter_Emission, "emission", parameters.emission },
			{ FluidOverlayParameter_Roughness, "roughness", parameters.roughness },
			{ FluidOverlayParameter_UndersideRoughness, "undersideroughness", parameters.undersideRoughness },
			{ FluidOverlayParameter_Opacity, "opacity", parameters.opacity },
			{ FluidOverlayParameter_TextureMix, "texturemix", parameters.textureMix },
			{ FluidOverlayParameter_Scale, "scale", parameters.scale },
			{ FluidOverlayParameter_Amplitude, "amplitude", parameters.amplitude },
			{ FluidOverlayParameter_Frequency, "frequency", parameters.frequency },
			{ FluidOverlayParameter_Lacunarity, "lacunarity", parameters.lacunarity },
			{ FluidOverlayParameter_Gain, "gain", parameters.gain },
			{ FluidOverlayParameter_TimeScale, "timescale", parameters.timeScale },
			{ FluidOverlayParameter_Speed, "speed", parameters.speed },
		};
		for (const auto& field : fields)
		{
			if (mask & field.bit) AppendLine(text, indent, FStringf("%s %s", field.name, FormatFloat(field.value).GetChars()));
		}
		if (mask & FluidOverlayParameter_Flow) AppendVector(text, indent, "flow", parameters.flow, 2);
		const FloatField tailFields[] = {
			{ FluidOverlayParameter_Anisotropy, "anisotropy", parameters.anisotropy },
			{ FluidOverlayParameter_Warp, "warp", parameters.warp },
			{ FluidOverlayParameter_Choppiness, "choppiness", parameters.choppiness },
			{ FluidOverlayParameter_OctaveRotation, "octaverotation", parameters.octaveRotation },
			{ FluidOverlayParameter_ChoppyRelaxation, "choppyrelaxation", parameters.choppyRelaxation },
			{ FluidOverlayParameter_NoiseScale, "noisescale", parameters.noiseScale },
			{ FluidOverlayParameter_MaxSlope, "maxslope", parameters.maxSlope },
			{ FluidOverlayParameter_CrestGain, "crestgain", parameters.crestGain },
			{ FluidOverlayParameter_CrustSoftness, "crustsoftness", parameters.crustSoftness },
			{ FluidOverlayParameter_CrustDarkening, "crustdarkening", parameters.crustDarkening },
			{ FluidOverlayParameter_FoamAmount, "foamamount", parameters.foamAmount },
			{ FluidOverlayParameter_FoamRoughness, "foamroughness", parameters.foamRoughness },
			{ FluidOverlayParameter_FoamThreshold, "foamthreshold", parameters.foamThreshold },
			{ FluidOverlayParameter_FoamSoftness, "foamsoftness", parameters.foamSoftness },
			{ FluidOverlayParameter_FoamBreakupScale, "foambreakupscale", parameters.foamBreakupScale },
			{ FluidOverlayParameter_FoamBreakupStrength, "foambreakupstrength", parameters.foamBreakupStrength },
			{ FluidOverlayParameter_FoamLifetime, "foamlifetime", parameters.foamLifetime },
			{ FluidOverlayParameter_FoamTrailGain, "foamtrailgain", parameters.foamTrailGain },
			{ FluidOverlayParameter_FoamUndersideStrength, "foamundersidestrength", parameters.foamUndersideStrength },
			{ FluidOverlayParameter_FoamContrast, "foamcontrast", parameters.foamContrast },
			{ FluidOverlayParameter_FoamEdgeWidth, "foamedgewidth", parameters.foamEdgeWidth },
			{ FluidOverlayParameter_FoamCutoutScale, "foamcutoutscale", parameters.foamCutoutScale },
			{ FluidOverlayParameter_FoamCutoutStrength, "foamcutoutstrength", parameters.foamCutoutStrength },
			{ FluidOverlayParameter_FoamBrightness, "foambrightness", parameters.foamBrightness },
			{ FluidOverlayParameter_FoamBlendWidth, "foamblendwidth", parameters.foamBlendWidth },
			{ FluidOverlayParameter_NormalStrength, "normalstrength", parameters.normalStrength },
			{ FluidOverlayParameter_VirtualHeight, "virtualheight", parameters.virtualHeight },
		};
		for (const auto& field : tailFields)
		{
			if (mask & field.bit) AppendLine(text, indent, FStringf("%s %s", field.name, FormatFloat(field.value).GetChars()));
		}
		if (mask & FluidOverlayParameter_Seed) AppendLine(text, indent, FStringf("seed %d", parameters.seed));
		if (mask & FluidOverlayParameter_Octaves) AppendLine(text, indent, FStringf("octaves %u", parameters.octaves));
	}

	static void AppendProfile(FString& text, const ParsedFluidOverlayProfile& profile)
	{
		AppendLine(text, 1, FStringf("profile %s", QuoteString(profile.id).GetChars()));
		AppendLine(text, 1, "{");
		AppendParameterFields(text, 2, profile.parameters);
		AppendLine(text, 1, "}");
	}

	static void AppendMaterial(FString& text, uint32_t indent, const ParsedFluidOverlayMaterialRule& rule)
	{
		AppendLine(text, indent, FStringf("material %s", QuoteString(rule.id).GetChars()));
		AppendLine(text, indent, "{");
		for (int tile : rule.tileFilters) AppendLine(text, indent + 1, FStringf("tile %d", tile));
		for (const auto& range : rule.tileRanges) AppendLine(text, indent + 1, FStringf("tilerange %d %d", range.first, range.last));
		for (int family : rule.tileFamilies) AppendLine(text, indent + 1, FStringf("tilefamily %d", family));
		for (const auto& name : rule.textureNames) AppendLine(text, indent + 1, FStringf("texture %s", QuoteString(name).GetChars()));
		if (rule.hasPalette) AppendLine(text, indent + 1, FStringf("palette %d", rule.palette));
		if (rule.hasSurfaceClass) AppendLine(text, indent + 1, FStringf("surfaceclass %s", SurfaceClassName(rule.surfaceClass)));
		if (rule.hasProfile) AppendLine(text, indent + 1, FStringf("profile %s", QuoteString(rule.profileId).GetChars()));
		if (rule.hasDisable) AppendLine(text, indent + 1, FStringf("disable %s", rule.disable ? "on" : "off"));
		AppendParameterFields(text, indent + 1, rule.overrides);
		AppendLine(text, indent, "}");
	}

	template<typename T>
	static std::vector<const T*> SortByOrder(const TArray<T>& values)
	{
		std::vector<const T*> sorted;
		sorted.reserve(values.Size());
		for (const auto& value : values) sorted.push_back(&value);
		std::sort(sorted.begin(), sorted.end(), [](const T* a, const T* b)
		{
			if (a->source.orderIndex != b->source.orderIndex) return a->source.orderIndex < b->source.orderIndex;
			if (a->source.sourceName.CompareNoCase(b->source.sourceName) != 0) return a->source.sourceName.CompareNoCase(b->source.sourceName) < 0;
			return a->id.CompareNoCase(b->id) < 0;
		});
		return sorted;
	}

	static uint64_t ComputeTextHash(const FString& text)
	{
		uint64_t hash = HashOffset;
		for (const char* p = text.GetChars(); *p != 0; ++p) hash = HashByte(hash, (uint8_t)*p);
		return hash;
	}

	static bool RuleHasIdentitySelector(const ParsedFluidOverlayMaterialRule& rule)
	{
		return rule.tileFilters.Size() != 0 || rule.tileRanges.Size() != 0 ||
			rule.tileFamilies.Size() != 0 || rule.textureNames.Size() != 0;
	}

	static bool MaterialRuleMatches(const ResolvedFluidOverlayMaterialRule& rule, const FluidOverlayMaterialIdentity& identity)
	{
		if (!rule.valid || !RuleHasIdentitySelector(rule)) return false;
		if (rule.hasPalette && rule.palette != identity.palette) return false;
		if (rule.hasSurfaceClass && rule.surfaceClass != FluidOverlaySurfaceClass::Any && rule.surfaceClass != identity.surfaceClass) return false;

		for (int tile : rule.tileFilters) if (identity.tile >= 0 && tile == identity.tile) return true;
		for (const auto& range : rule.tileRanges)
		{
			if (identity.tile >= range.first && identity.tile <= range.last) return true;
		}
		for (int family : rule.tileFamilies) if (identity.tileFamily >= 0 && family == identity.tileFamily) return true;
		const std::string candidateName = NormalizeTextureName(identity.textureName.GetChars());
		for (const auto& textureName : rule.textureNames)
		{
			if (!candidateName.empty() && candidateName == NormalizeTextureName(textureName.GetChars())) return true;
		}
		return false;
	}

	static const ResolvedFluidOverlaySet& ResolveFluidOverlaysForMapInternal(const FString& mapName, bool currentMapAvailable)
	{
		const bool sameGeneration = GResolvedFluidOverlaySet.parsedGeneration == GFluidOverlayDatabase.generation;
		const bool sameMap = GResolvedFluidOverlaySet.activeMapName.CompareNoCase(mapName) == 0;
		if (sameGeneration && sameMap && GResolvedFluidOverlaySet.currentMapAvailable == currentMapAvailable)
		{
			return GResolvedFluidOverlaySet;
		}

		ResolvedFluidOverlaySet resolved;
		resolved.parsedGeneration = GFluidOverlayDatabase.generation;
		resolved.resolvedGeneration = ++GResolvedFluidOverlayGeneration;
		resolved.activeMapName = mapName;
		resolved.currentMapAvailable = currentMapAvailable;

		std::vector<ResolvedFluidOverlayProfile> profiles = BuildBuiltInProfiles();
		std::unordered_map<std::string, size_t> profileSlots;
		for (size_t i = 0; i < profiles.size(); ++i) profileSlots[MakeNormalizedKey(profiles[i].id)] = i;
		for (const ParsedFluidOverlayProfile* source : SortByOrder(GFluidOverlayDatabase.profiles))
		{
			ResolvedFluidOverlayProfile profile;
			profile.id = source->id;
			profile.source = source->source;
			profile.builtIn = false;
			const FluidOverlayStyle selectedStyle =
				(source->parameters.presentMask & FluidOverlayParameter_Style) != 0 ? source->parameters.style : FluidOverlayStyle::Still;
			profile.parameters = MakeStyleDefaults(selectedStyle);
			ApplyParameterOverrides(profile.parameters, source->parameters);
			SanitizeParameters(profile.parameters);
			profile.contentKey = ComputeParameterContentKey(profile.parameters);
			const std::string key = MakeNormalizedKey(profile.id);
			auto existing = profileSlots.find(key);
			if (existing == profileSlots.end())
			{
				profileSlots[key] = profiles.size();
				profiles.push_back(std::move(profile));
			}
			else profiles[existing->second] = std::move(profile);
		}
		std::sort(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) { return a.id.CompareNoCase(b.id) < 0; });
		for (auto& profile : profiles) resolved.profiles.Push(std::move(profile));

		std::unordered_map<std::string, uint32_t> resolvedProfileLookup;
		for (uint32_t i = 0; i < (uint32_t)resolved.profiles.Size(); ++i)
		{
			resolvedProfileLookup[MakeNormalizedKey(resolved.profiles[i].id)] = i;
		}

		const auto materialRules = SortByOrder(GFluidOverlayDatabase.materialRules);
		auto appendScope = [&](bool mapLocal)
		{
			for (const ParsedFluidOverlayMaterialRule* source : materialRules)
			{
				const bool sourceIsMapLocal = source->mapName.IsNotEmpty();
				if (sourceIsMapLocal != mapLocal) continue;
				if (mapLocal && (mapName.IsEmpty() || source->mapName.CompareNoCase(mapName) != 0)) continue;

				ResolvedFluidOverlayMaterialRule rule;
				static_cast<ParsedFluidOverlayMaterialRule&>(rule) = *source;
				rule.resolvedDisable = source->hasDisable && source->disable;
				rule.valid = RuleHasIdentitySelector(*source);
				if (rule.resolvedDisable)
				{
					resolved.materialRules.Push(std::move(rule));
					continue;
				}

				const auto profile = source->hasProfile ? resolvedProfileLookup.find(MakeNormalizedKey(source->profileId)) : resolvedProfileLookup.end();
				if (!source->hasProfile || profile == resolvedProfileLookup.end())
				{
					rule.valid = false;
					Printf(TEXTCOLOR_ORANGE "FLUIDOVR warning: material '%s' in scope '%s' references unresolved profile '%s'; rule skipped.\n",
						rule.id.GetChars(), rule.mapName.IsEmpty() ? "global" : rule.mapName.GetChars(), rule.profileId.GetChars());
					resolved.materialRules.Push(std::move(rule));
					continue;
				}

				rule.profileIndex = profile->second;
				rule.effectiveParameters = resolved.profiles[rule.profileIndex].parameters;
				ApplyParameterOverrides(rule.effectiveParameters, source->overrides);
				SanitizeParameters(rule.effectiveParameters);
				rule.bindingContentKey = ComputeParameterContentKey(rule.effectiveParameters);
				resolved.materialRules.Push(std::move(rule));
			}
		};
		appendScope(false);
		appendScope(true);

		struct UniqueBinding
		{
			uint64_t contentKey = 0;
			FluidOverlayParameters parameters;
		};
		std::vector<UniqueBinding> uniqueBindings;
		for (const auto& rule : resolved.materialRules)
		{
			if (!rule.valid || rule.resolvedDisable) continue;
			bool found = false;
			for (const auto& binding : uniqueBindings)
			{
				if (binding.contentKey == rule.bindingContentKey && ParametersEqual(binding.parameters, rule.effectiveParameters))
				{
					found = true;
					break;
				}
			}
			if (!found) uniqueBindings.push_back({ rule.bindingContentKey, rule.effectiveParameters });
		}
		std::sort(uniqueBindings.begin(), uniqueBindings.end(), [](const UniqueBinding& a, const UniqueBinding& b)
		{
			if (a.contentKey != b.contentKey) return a.contentKey < b.contentKey;
			return ParametersLess(a.parameters, b.parameters);
		});
		for (uint32_t index = 0; index < (uint32_t)uniqueBindings.size(); ++index)
		{
			ResolvedFluidOverlayBinding binding;
			binding.bindingIndex = index;
			binding.contentKey = uniqueBindings[index].contentKey;
			binding.parameters = uniqueBindings[index].parameters;
			resolved.bindings.Push(binding);
		}
		for (auto& rule : resolved.materialRules)
		{
			if (!rule.valid || rule.resolvedDisable) continue;
			for (const auto& binding : resolved.bindings)
			{
				if (binding.contentKey == rule.bindingContentKey && ParametersEqual(binding.parameters, rule.effectiveParameters))
				{
					rule.bindingIndex = binding.bindingIndex;
					break;
				}
			}
		}

		uint64_t resolvedHash = HashValue(HashOffset, GFluidOverlayDatabase.contentHash);
		resolvedHash = HashText(resolvedHash, mapName.MakeLower().GetChars());
		for (const auto& profile : resolved.profiles)
		{
			resolvedHash = HashText(resolvedHash, profile.id.MakeLower().GetChars());
			resolvedHash = HashValue(resolvedHash, profile.contentKey);
		}
		for (const auto& binding : resolved.bindings) resolvedHash = HashValue(resolvedHash, binding.contentKey);
		for (const auto& rule : resolved.materialRules)
		{
			resolvedHash = HashText(resolvedHash, rule.mapName.MakeLower().GetChars());
			resolvedHash = HashText(resolvedHash, rule.id.MakeLower().GetChars());
			resolvedHash = HashValue(resolvedHash, rule.source.orderIndex);
			resolvedHash = HashValue(resolvedHash, rule.bindingContentKey);
			resolvedHash = HashValue(resolvedHash, rule.valid);
			resolvedHash = HashValue(resolvedHash, rule.resolvedDisable);
		}
		resolved.contentHash = resolvedHash;
		GResolvedFluidOverlaySet = std::move(resolved);
		return GResolvedFluidOverlaySet;
	}

	static void DumpResolved(const ResolvedFluidOverlaySet& resolved)
	{
		Printf("FLUIDOVR resolved: parsed_generation=%u resolved_generation=%u map=%s profiles=%d materials=%d bindings=%d hash=%llu\n",
			resolved.parsedGeneration, resolved.resolvedGeneration,
			resolved.activeMapName.IsEmpty() ? "(global)" : resolved.activeMapName.GetChars(),
			resolved.profiles.Size(), resolved.materialRules.Size(), resolved.bindings.Size(),
			(unsigned long long)resolved.contentHash);
		for (uint32_t i = 0; i < (uint32_t)resolved.profiles.Size(); ++i)
		{
			const auto& profile = resolved.profiles[i];
			Printf("FLUIDOVR resolved profile: index=%u id=%s builtin=%s style=%s key=%llu source=%s:%d\n",
				i, profile.id.GetChars(), profile.builtIn ? "yes" : "no", StyleName(profile.parameters.style),
				(unsigned long long)profile.contentKey, profile.source.sourceName.GetChars(), profile.source.lineStart);
			const auto& p = profile.parameters;
			Printf("FLUIDOVR resolved pan: id=%s followpan=%s\n",
				profile.id.GetChars(), p.followPan ? "yes" : "no");
			Printf("FLUIDOVR resolved field: id=%s scale=%.4f amplitude=%.4f frequency=%.4f lacunarity=%.4f gain=%.4f speed=%.4f timescale=%.4f anisotropy=%.4f warp=%.4f choppiness=%.4f octaverotation=%.4f choppyrelaxation=%.4f noisescale=%.4f maxslope=%.4f crestgain=%.4f normalstrength=%.4f virtualheight=%.4f seed=%d octaves=%u\n",
				profile.id.GetChars(), p.scale, p.amplitude, p.frequency, p.lacunarity, p.gain,
				p.speed, p.timeScale, p.anisotropy, p.warp, p.choppiness, p.octaveRotation,
				p.choppyRelaxation, p.noiseScale, p.maxSlope, p.crestGain, p.normalStrength,
				p.virtualHeight, p.seed, p.octaves);
			Printf("FLUIDOVR resolved crust: id=%s softness=%.4f darkening=%.4f\n",
				profile.id.GetChars(), p.crustSoftness, p.crustDarkening);
			Printf("FLUIDOVR resolved texture mix: id=%s mix=%.4f policy=%s\n",
				profile.id.GetChars(), p.textureMix, p.textureMix < 0.0f ? "automatic" : "explicit");
			Printf("FLUIDOVR resolved foam: id=%s color=(%.4f,%.4f,%.4f) amount=%.4f roughness=%.4f threshold=%.4f softness=%.4f breakupscale=%.4f breakupstrength=%.4f lifetime=%.4f trailgain=%.4f undersidestrength=%.4f\n",
				profile.id.GetChars(), p.foamColor[0], p.foamColor[1], p.foamColor[2],
				p.foamAmount, p.foamRoughness, p.foamThreshold, p.foamSoftness,
				p.foamBreakupScale, p.foamBreakupStrength, p.foamLifetime,
				p.foamTrailGain, p.foamUndersideStrength);
			Printf("FLUIDOVR resolved foam style: id=%s contrast=%.4f edgewidth=%.4f cutoutscale=%.4f cutoutstrength=%.4f brightness=%.4f blend=%s blendwidth=%.4f\n",
				profile.id.GetChars(), p.foamContrast, p.foamEdgeWidth,
				p.foamCutoutScale, p.foamCutoutStrength, p.foamBrightness,
				FoamBlendName(p.foamBlend), p.foamBlendWidth);
		}
		for (uint32_t i = 0; i < (uint32_t)resolved.materialRules.Size(); ++i)
		{
			const auto& rule = resolved.materialRules[i];
			Printf("FLUIDOVR resolved material: index=%u scope=%s id=%s valid=%s disable=%s profile=%s profile_index=%u binding=%u key=%llu order=%u source=%s:%d followpan=%s foamblend=%s foamblendwidth=%.4f\n",
				i, rule.mapName.IsEmpty() ? "global" : rule.mapName.GetChars(), rule.id.GetChars(),
				rule.valid ? "yes" : "no", rule.resolvedDisable ? "yes" : "no",
				rule.hasProfile ? rule.profileId.GetChars() : "(none)", rule.profileIndex, rule.bindingIndex,
				(unsigned long long)rule.bindingContentKey, rule.source.orderIndex,
				rule.source.sourceName.GetChars(), rule.source.lineStart,
				rule.effectiveParameters.followPan ? "yes" : "no",
				FoamBlendName(rule.effectiveParameters.foamBlend), rule.effectiveParameters.foamBlendWidth);
		}
	}
}

FString SerializeFluidOverlayDatabase(const ParsedFluidOverlayDatabase& database)
{
	FString text;
	AppendLine(text, 0, "FLUIDOVR");
	AppendLine(text, 0, "{");

	std::vector<const ParsedFluidOverlayProfile*> profiles;
	for (const auto& profile : database.profiles) profiles.push_back(&profile);
	std::sort(profiles.begin(), profiles.end(), [](const auto* a, const auto* b) { return a->id.CompareNoCase(b->id) < 0; });
	for (size_t i = 0; i < profiles.size(); ++i)
	{
		if (i != 0) text << "\n";
		AppendProfile(text, *profiles[i]);
	}

	const auto orderedRules = SortByOrder(database.materialRules);
	bool wroteRule = false;
	for (const auto* rule : orderedRules)
	{
		if (rule->mapName.IsNotEmpty()) continue;
		if (!profiles.empty() || wroteRule) text << "\n";
		AppendMaterial(text, 1, *rule);
		wroteRule = true;
	}

	std::vector<FString> mapNames;
	for (const auto& rule : database.materialRules)
	{
		if (rule.mapName.IsEmpty()) continue;
		bool found = false;
		for (const auto& name : mapNames) if (name.CompareNoCase(rule.mapName) == 0) { found = true; break; }
		if (!found) mapNames.push_back(rule.mapName);
	}
	std::sort(mapNames.begin(), mapNames.end(), [](const FString& a, const FString& b) { return a.CompareNoCase(b) < 0; });
	for (const auto& mapName : mapNames)
	{
		if (!profiles.empty() || wroteRule) text << "\n";
		AppendLine(text, 1, FStringf("map %s", QuoteString(mapName).GetChars()));
		AppendLine(text, 1, "{");
		bool wroteMapRule = false;
		for (const auto* rule : orderedRules)
		{
			if (rule->mapName.CompareNoCase(mapName) != 0) continue;
			if (wroteMapRule) text << "\n";
			AppendMaterial(text, 2, *rule);
			wroteMapRule = true;
		}
		AppendLine(text, 1, "}");
		wroteRule = true;
	}

	AppendLine(text, 0, "}");
	return text;
}

bool ApplyParsedFluidOverlayDatabase(const ParsedFluidOverlayDatabase& database, bool verbose)
{
	ParsedFluidOverlayDatabase next = database;
	next.contentHash = ComputeTextHash(SerializeFluidOverlayDatabase(next));
	const bool changed = next.contentHash != GFluidOverlayDatabase.contentHash;
	next.generation = changed ? ++GFluidOverlayGeneration : GFluidOverlayDatabase.generation;
	GFluidOverlayDatabase = std::move(next);

	if (verbose)
	{
		Printf("FLUIDOVR: parsed generation=%u files=%d profiles=%d materials=%d parse_errors=%s changed=%s hash=%llu\n",
			GFluidOverlayDatabase.generation, GFluidOverlayDatabase.sourceFiles.Size(),
			GFluidOverlayDatabase.profiles.Size(), GFluidOverlayDatabase.materialRules.Size(),
			GFluidOverlayDatabase.hadParseErrors ? "yes" : "no", changed ? "yes" : "no",
			(unsigned long long)GFluidOverlayDatabase.contentHash);
	}
	ResolveFluidOverlaysForMapInternal(GetCurrentResolvedMapName(), currentLevel != nullptr);
	return !GFluidOverlayDatabase.hadParseErrors;
}

const ParsedFluidOverlayDatabase& GetParsedFluidOverlayDatabase()
{
	return GFluidOverlayDatabase;
}

const ResolvedFluidOverlaySet& GetResolvedFluidOverlaySet()
{
	return ResolveFluidOverlaysForMapInternal(GetCurrentResolvedMapName(), currentLevel != nullptr);
}

const ResolvedFluidOverlaySet& ResolveFluidOverlaysForMap(const char* mapName)
{
	const FString canonical = CanonicalizeMapName(mapName);
	return ResolveFluidOverlaysForMapInternal(canonical, canonical.IsNotEmpty());
}

bool ResolveFluidOverlayMaterial(
	const ResolvedFluidOverlaySet& resolved,
	const FluidOverlayMaterialIdentity& identity,
	FluidOverlayMaterialMatch& outMatch)
{
	outMatch = {};
	for (uint32_t ruleIndex = 0; ruleIndex < (uint32_t)resolved.materialRules.Size(); ++ruleIndex)
	{
		const auto& rule = resolved.materialRules[ruleIndex];
		if (!MaterialRuleMatches(rule, identity)) continue;
		outMatch.matched = true;
		outMatch.disabled = rule.resolvedDisable;
		outMatch.ruleIndex = ruleIndex;
		outMatch.profileIndex = rule.profileIndex;
		outMatch.bindingIndex = rule.bindingIndex;
		outMatch.bindingContentKey = rule.bindingContentKey;
	}
	return outMatch.matched && !outMatch.disabled && outMatch.bindingIndex != UINT32_MAX;
}

bool ParseFluidOverlays(bool verbose)
{
	ParsedFluidOverlayDatabaseBuilder builder;
	int workingLump = -1;
	int lastLump = 0;
	static const char* fluidOverlayNames[] = { "FLUIDOVR", nullptr };
	while ((workingLump = fileSystem.FindLumpMulti(fluidOverlayNames, &lastLump)) != -1)
	{
		fileSystem.RefreshFile(workingLump);
		ParsedFluidOverlaySourceFile sourceFile;
		sourceFile.lumpNum = workingLump;
		sourceFile.sourceName = GetLumpDisplayName(workingLump);
		builder.database.sourceFiles.Push(sourceFile);
		auto& storedSource = builder.database.sourceFiles.Last();
		FluidOverlayParser parser(workingLump, builder, storedSource);
		parser.Parse();
		builder.database.hadParseErrors = builder.database.hadParseErrors || storedSource.hadParseErrors;
	}
	return ApplyParsedFluidOverlayDatabase(builder.database, verbose);
}

CCMD(fluidoverlay_reload)
{
	const bool ok = ParseFluidOverlays(true);
	Printf("FLUIDOVR reload %s.\n", ok ? "completed" : "completed with parse errors");
}

CCMD(fluidoverlay_dump)
{
	const auto& database = GetParsedFluidOverlayDatabase();
	Printf("FLUIDOVR parsed: generation=%u files=%d profiles=%d materials=%d hash=%llu parse_errors=%s\n",
		database.generation, database.sourceFiles.Size(), database.profiles.Size(), database.materialRules.Size(),
		(unsigned long long)database.contentHash, database.hadParseErrors ? "yes" : "no");
	Printf("%s", SerializeFluidOverlayDatabase(database).GetChars());
}

CCMD(fluidoverlay_dumpresolved)
{
	if (argv.argc() > 2)
	{
		Printf("usage: fluidoverlay_dumpresolved [mapname]\n");
		return;
	}
	DumpResolved(argv.argc() == 2 ? ResolveFluidOverlaysForMap(argv[1]) : GetResolvedFluidOverlaySet());
}

CCMD(fluidoverlay_dumpnormalized)
{
	Printf("%s", SerializeFluidOverlayDatabase(GetParsedFluidOverlayDatabase()).GetChars());
}

CCMD(fluidoverlay_export)
{
	if (argv.argc() != 2)
	{
		Printf("usage: fluidoverlay_export <path>\n");
		return;
	}
	std::unique_ptr<FileWriter> file(FileWriter::Open(argv[1]));
	if (file == nullptr)
	{
		Printf("FLUIDOVR export failed: could not open '%s' for writing.\n", argv[1]);
		return;
	}
	const FString serialized = SerializeFluidOverlayDatabase(GetParsedFluidOverlayDatabase());
	file->Write(serialized.GetChars(), serialized.Len());
	Printf("FLUIDOVR export wrote %d bytes to %s.\n", serialized.Len(), argv[1]);
}
