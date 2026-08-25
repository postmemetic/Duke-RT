#include "lightoverlay.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cmdlib.h"
#include "c_dispatch.h"
#include "filesystem.h"
#include "coreactor.h"
#include "files.h"
#include "mapinfo.h"
#include "printf.h"
#include "sc_man.h"
#include "vectors.h"

namespace
{
	static ParsedLightOverlayDatabase GLightOverlayDatabase;
	static uint32_t GLightOverlayGeneration = 0;
	static ResolvedLightOverlaySet GResolvedLightOverlaySet;
	static uint32_t GResolvedLightOverlayGeneration = 0;

	static FString GetLumpDisplayName(int lumpNum)
	{
		const char* fullName = lumpNum >= 0 ? fileSystem.GetFileFullName(lumpNum) : nullptr;
		return fullName != nullptr ? FString(fullName) : FStringf("lump:%d", lumpNum);
	}

	static std::string MakeNormalizedKey(const FString& value)
	{
		return std::string(value.MakeLower().GetChars());
	}

	static std::string MakeMapScopedKey(const FString& mapName, const char* kind, const FString& id)
	{
		FString normalized;
		normalized << mapName.MakeLower() << "|" << kind << "|" << id.MakeLower();
		return std::string(normalized.GetChars());
	}

	static FString SourceLocationText(const LightOverlaySourceLocation& source)
	{
		return FStringf("%s:%d", source.sourceName.GetChars(), source.lineStart);
	}

	static const char* AnchorTypeName(LightOverlayAnchorType type)
	{
		switch (type)
		{
		case LightOverlayAnchorType::Position: return "position";
		case LightOverlayAnchorType::Sector: return "sector";
		case LightOverlayAnchorType::Wall: return "wall";
		default: return "none";
		}
	}

	static const char* ActorActivationPolicyName(LightOverlayActorActivationPolicy policy)
	{
		switch (policy)
		{
		case LightOverlayActorActivationPolicy::Immediate: return "immediate";
		default: return "surface";
		}
	}

	static const char* SmokeTriggerName(LightOverlaySmokeTrigger trigger)
	{
		switch (trigger)
		{
		case LightOverlaySmokeTrigger::Interval: return "interval";
		default: return "spawn";
		}
	}

	static const char* SmokeDirectionPolicyName(LightOverlaySmokeDirectionPolicy policy)
	{
		switch (policy)
		{
		case LightOverlaySmokeDirectionPolicy::Normal: return "normal";
		case LightOverlaySmokeDirectionPolicy::Incoming: return "incoming";
		default: return "aim";
		}
	}

	static const char* SmokeRepresentationName(LightOverlaySmokeRepresentation representation)
	{
		switch (representation)
		{
		case LightOverlaySmokeRepresentation::Analytic: return "analytic";
		default: return "grid";
		}
	}

	static const char* SmokeQueuePolicyName(LightOverlaySmokeQueuePolicy policy)
	{
		switch (policy)
		{
		case LightOverlaySmokeQueuePolicy::Drop: return "drop";
		case LightOverlaySmokeQueuePolicy::Latest: return "latest";
		default: return "retry";
		}
	}

	static void CopyVector3(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}

	static uint64_t HashCombineLightOverlayText(uint64_t hash, uint8_t value)
	{
		hash ^= value;
		hash *= 1099511628211ull;
		return hash;
	}

	static FString FormatLightOverlayFloat(float value)
	{
		FString text = FStringf("%.6g", value);
		if (text.IndexOf('.') < 0 && text.IndexOf('e') < 0 && text.IndexOf('E') < 0)
		{
			text << ".0";
		}
		return text;
	}

	static FString QuoteLightOverlayString(const FString& value)
	{
		FString quoted = "\"";
		for (const char* p = value.GetChars(); *p != 0; ++p)
		{
			if (*p == '\\' || *p == '"')
			{
				quoted << '\\';
			}
			quoted << *p;
		}
		quoted << '"';
		return quoted;
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

	struct ParsedLightOverlayDatabaseBuilder
	{
		ParsedLightOverlayDatabase database;
		uint32_t nextOrderIndex = 0;
		std::unordered_map<std::string, int> actorRuleLookup;
		std::unordered_map<std::string, int> directionalRuleLookup;
		std::unordered_map<std::string, int> muzzleFlashRuleLookup;
		std::unordered_map<std::string, int> mapLightRuleLookup;
		std::unordered_map<std::string, int> emissiveOverrideLookup;
		std::unordered_map<std::string, int> emissiveMaterialResponseLookup;
		std::unordered_map<std::string, int> surfaceLightLookup;
		std::unordered_map<std::string, int> actorOverrideLookup;
		std::unordered_map<std::string, int> smokeStyleLookup;
		std::unordered_map<std::string, int> smokeActorRuleLookup;
		std::unordered_map<std::string, int> smokeEventRuleLookup;
		std::unordered_map<std::string, int> mapSmokeEmitterRuleLookup;

		void SetDefaults(const ParsedLightOverlayDefaults& defaults)
		{
			database.defaults = defaults;
		}

		void AddActorRule(const ParsedLightOverlayActorRule& rule)
		{
			const std::string key = MakeNormalizedKey(rule.id);
			auto it = actorRuleLookup.find(key);
			if (it == actorRuleLookup.end())
			{
				actorRuleLookup.emplace(key, database.actorRules.Size());
				database.actorRules.Push(rule);
				return;
			}

			auto& existing = database.actorRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate actorrule '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddDirectionalRule(const ParsedLightOverlayDirectionalRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "directional", rule.id);
			auto it = directionalRuleLookup.find(key);
			if (it == directionalRuleLookup.end())
			{
				directionalRuleLookup.emplace(key, database.directionalRules.Size());
				database.directionalRules.Push(rule);
				return;
			}

			auto& existing = database.directionalRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate directional rule '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddMuzzleFlashRule(const ParsedLightOverlayMuzzleFlashRule& rule)
		{
			const std::string key = MakeNormalizedKey(rule.id);
			auto it = muzzleFlashRuleLookup.find(key);
			if (it == muzzleFlashRuleLookup.end())
			{
				muzzleFlashRuleLookup.emplace(key, database.muzzleFlashRules.Size());
				database.muzzleFlashRules.Push(rule);
				return;
			}

			auto& existing = database.muzzleFlashRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate muzzleflashrule '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddMapLightRule(const ParsedLightOverlayMapLightRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "light", rule.id);
			auto it = mapLightRuleLookup.find(key);
			if (it == mapLightRuleLookup.end())
			{
				mapLightRuleLookup.emplace(key, database.mapLightRules.Size());
				database.mapLightRules.Push(rule);
				return;
			}

			auto& existing = database.mapLightRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate light rule '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddEmissiveOverrideRule(const ParsedLightOverlayEmissiveOverrideRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "emissiveoverride", rule.id);
			auto it = emissiveOverrideLookup.find(key);
			if (it == emissiveOverrideLookup.end())
			{
				emissiveOverrideLookup.emplace(key, database.emissiveOverrideRules.Size());
				database.emissiveOverrideRules.Push(rule);
				return;
			}

			auto& existing = database.emissiveOverrideRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate emissiveoverride '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddEmissiveMaterialResponseRule(const ParsedLightOverlayEmissiveMaterialResponseRule& rule)
		{
			const std::string key = MakeNormalizedKey(rule.id);
			auto it = emissiveMaterialResponseLookup.find(key);
			if (it == emissiveMaterialResponseLookup.end())
			{
				emissiveMaterialResponseLookup.emplace(key, database.emissiveMaterialResponseRules.Size());
				database.emissiveMaterialResponseRules.Push(rule);
				return;
			}

			auto& existing = database.emissiveMaterialResponseRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate emissivematerialresponse '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddSurfaceLightRule(const ParsedLightOverlaySurfaceLightRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "surfacelight", rule.id);
			auto it = surfaceLightLookup.find(key);
			if (it == surfaceLightLookup.end())
			{
				surfaceLightLookup.emplace(key, database.surfaceLightRules.Size());
				database.surfaceLightRules.Push(rule);
				return;
			}

			auto& existing = database.surfaceLightRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate surfacelight '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddActorOverrideRule(const ParsedLightOverlayActorOverrideRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "actoroverride", rule.id);
			auto it = actorOverrideLookup.find(key);
			if (it == actorOverrideLookup.end())
			{
				actorOverrideLookup.emplace(key, database.actorOverrideRules.Size());
				database.actorOverrideRules.Push(rule);
				return;
			}

			auto& existing = database.actorOverrideRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate actoroverride '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		template<class T>
		void AddSmokeDefinition(const T& rule, TArray<T>& rules, std::unordered_map<std::string, int>& lookup, const char* kind)
		{
			const std::string key = MakeNormalizedKey(rule.id);
			auto it = lookup.find(key);
			if (it == lookup.end())
			{
				lookup.emplace(key, rules.Size());
				rules.Push(rule);
				return;
			}
			auto& existing = rules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate %s '%s' in %s; using the last definition.\n",
					kind, rule.id.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddSmokeStyle(const ParsedLightOverlaySmokeStyle& rule)
		{
			AddSmokeDefinition(rule, database.smokeStyles, smokeStyleLookup, "smokestyle");
		}

		void AddSmokeActorRule(const ParsedLightOverlaySmokeActorRule& rule)
		{
			AddSmokeDefinition(rule, database.smokeActorRules, smokeActorRuleLookup, "smokeactorrule");
		}

		void AddSmokeEventRule(const ParsedLightOverlaySmokeEventRule& rule)
		{
			AddSmokeDefinition(rule, database.smokeEventRules, smokeEventRuleLookup, "smokeeventrule");
		}

		void AddMapSmokeEmitterRule(const ParsedLightOverlayMapSmokeEmitterRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "smokeemitter", rule.id);
			auto it = mapSmokeEmitterRuleLookup.find(key);
			if (it == mapSmokeEmitterRuleLookup.end())
			{
				mapSmokeEmitterRuleLookup.emplace(key, database.mapSmokeEmitterRules.Size());
				database.mapSmokeEmitterRules.Push(rule);
				return;
			}

			auto& existing = database.mapSmokeEmitterRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate smokeemitter '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}
	};

	class LightOverlayParser
	{
	public:
		LightOverlayParser(int lumpNum, ParsedLightOverlayDatabaseBuilder& builder, ParsedLightOverlaySourceFile& sourceFile)
			: sc(lumpNum), builder(builder), sourceFile(sourceFile), lumpNum(lumpNum)
		{
			sc.SetCMode(true);
			sc.SetNoFatalErrors(true);
			sc.SetPrependMessage("LIGHTOVR: ");
		}

		void Parse()
		{
			while (sc.GetString())
			{
				if (sc.Compare("LIGHTOVR"))
				{
					ParseRootBlock();
				}
				else
				{
					sc.ScriptMessage("Unknown top-level token '%s'; expected LIGHTOVR", sc.String);
					SkipUnknownDefinition();
				}
			}
			sourceFile.hadParseErrors = sc.ParseError;
		}

	private:
		FScanner sc;
		ParsedLightOverlayDatabaseBuilder& builder;
		ParsedLightOverlaySourceFile& sourceFile;
		int lumpNum = -1;

		LightOverlaySourceLocation MakeSourceLocation(int lineStart)
		{
			LightOverlaySourceLocation location;
			location.sourceName = sourceFile.sourceName;
			location.lumpNum = lumpNum;
			location.lineStart = lineStart;
			location.lineEnd = sc.GetMessageLine();
			location.orderIndex = ++builder.nextOrderIndex;
			return location;
		}

		void FinalizeSourceLocation(LightOverlaySourceLocation& location)
		{
			location.lineEnd = sc.GetMessageLine();
		}

		static bool ParseOnOffToken(const char* token, bool& outValue)
		{
			if (!stricmp(token, "on") || !stricmp(token, "true") || !strcmp(token, "1"))
			{
				outValue = true;
				return true;
			}
			if (!stricmp(token, "off") || !stricmp(token, "false") || !strcmp(token, "0"))
			{
				outValue = false;
				return true;
			}
			return false;
		}

		void MustParseVector3(float outValue[3])
		{
			sc.MustGetFloat();
			outValue[0] = (float)sc.Float;
			sc.MustGetFloat();
			outValue[1] = (float)sc.Float;
			sc.MustGetFloat();
			outValue[2] = (float)sc.Float;
		}

		void MustParseFloatRange(float outRange[2], bool allowSingleValue, bool singleValueIsPlusMinus, const char* fieldName)
		{
			sc.MustGetFloat();
			const float first = (float)sc.Float;
			if (sc.CheckFloat())
			{
				const float second = (float)sc.Float;
				outRange[0] = std::min(first, second);
				outRange[1] = std::max(first, second);
				return;
			}

			if (!allowSingleValue)
			{
				sc.ScriptMessage("Field '%s' requires two float values", fieldName);
				outRange[0] = first;
				outRange[1] = first;
				return;
			}

			if (singleValueIsPlusMinus)
			{
				const float plusMinus = fabsf(first);
				outRange[0] = -plusMinus;
				outRange[1] = plusMinus;
			}
			else
			{
				outRange[0] = first;
				outRange[1] = first;
			}
		}

		void SkipUnknownField(const char* context, const char* fieldName)
		{
			sc.ScriptMessage("Unknown field '%s' in %s definition", fieldName, context);
			if (sc.CheckString("{"))
			{
				sc.SkipToEndOfBlock();
				return;
			}
			sc.MustGetAnyToken();
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

		void ParseRootBlock()
		{
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("defaults"))
				{
					ParseDefaultsBlock();
				}
				else if (sc.Compare("actorrule"))
				{
					ParseActorRule();
				}
				else if (sc.Compare("muzzleflashrule"))
				{
					ParseMuzzleFlashRule();
				}
				else if (sc.Compare("emissivematerialresponse"))
				{
					ParseEmissiveMaterialResponseRule();
				}
				else if (sc.Compare("smokestyle"))
				{
					ParseSmokeStyle();
				}
				else if (sc.Compare("smokeactorrule"))
				{
					ParseSmokeActorRule();
				}
				else if (sc.Compare("smokeeventrule"))
				{
					ParseSmokeEventRule();
				}
				else if (sc.Compare("map"))
				{
					ParseMapBlock();
				}
				else
				{
					sc.ScriptMessage("Unknown LIGHTOVR block '%s'", sc.String);
					SkipUnknownDefinition();
				}
			}
		}

		void ParseDefaultsBlock()
		{
			const int lineStart = sc.GetMessageLine();
			ParsedLightOverlayDefaults defaults;
			defaults.present = true;
			defaults.source = MakeSourceLocation(lineStart);

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				SkipUnknownField("defaults", sc.String);
			}

			FinalizeSourceLocation(defaults.source);
			builder.SetDefaults(defaults);
		}

		void ParseActorRule()
		{
			sc.MustGetString();
			ParsedLightOverlayActorRule rule;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("actorclass"))
				{
					sc.MustGetString();
					rule.actorClassName = sc.String;
				}
				else if (sc.Compare("shadowreceive"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "off"))
					{
						rule.hasShadowReceive = true;
						rule.shadowReceive = false;
					}
					else if (!stricmp(sc.String, "default") || !stricmp(sc.String, "on"))
					{
						rule.hasShadowReceive = true;
						rule.shadowReceive = true;
					}
					else
					{
						sc.ScriptMessage("Invalid shadowreceive value '%s'; expected off/default/on", sc.String);
					}
				}
				else if (sc.Compare("shadowcast"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "off"))
					{
						rule.hasShadowCast = true;
						rule.shadowCast = false;
					}
					else if (!stricmp(sc.String, "default") || !stricmp(sc.String, "on"))
					{
						rule.hasShadowCast = true;
						rule.shadowCast = true;
					}
					else
					{
						sc.ScriptMessage("Invalid shadowcast value '%s'; expected off/default/on", sc.String);
					}
				}
				else if (sc.Compare("lightshadowcast"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "off"))
					{
						rule.hasLightShadowCast = true;
						rule.lightShadowCast = false;
					}
					else if (!stricmp(sc.String, "default") || !stricmp(sc.String, "on"))
					{
						rule.hasLightShadowCast = true;
						rule.lightShadowCast = true;
					}
					else
					{
						sc.ScriptMessage("Invalid lightshadowcast value '%s'; expected off/default/on", sc.String);
					}
				}
				else if (sc.Compare("fullbright"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "on"))
					{
						rule.hasFullbright = true;
						rule.fullbright = true;
					}
					else if (!stricmp(sc.String, "off") || !stricmp(sc.String, "default"))
					{
						rule.hasFullbright = true;
						rule.fullbright = false;
					}
					else
					{
						sc.ScriptMessage("Invalid fullbright value '%s'; expected off/default/on", sc.String);
					}
				}
				else if (sc.Compare("emissivestableframes"))
				{
					sc.MustGetNumber();
					rule.hasEmissiveStableFrames = true;
					rule.emissiveStableFrames = (uint32_t)std::max(sc.Number, 0);
				}
				else if (sc.Compare("activation"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "surface") || !stricmp(sc.String, "default"))
					{
						rule.hasActivationPolicy = true;
						rule.activationPolicy = LightOverlayActorActivationPolicy::Surface;
					}
					else if (!stricmp(sc.String, "immediate"))
					{
						rule.hasActivationPolicy = true;
						rule.activationPolicy = LightOverlayActorActivationPolicy::Immediate;
					}
					else
					{
						sc.ScriptMessage("Invalid activation value '%s'; expected surface/default/immediate", sc.String);
					}
				}
				else if (sc.Compare("tile"))
				{
					sc.MustGetNumber();
					rule.hasTileFilter = true;
					rule.tileFilter = sc.Number;
				}
				else if (sc.Compare("type"))
				{
					sc.MustGetString();
					rule.lightType = FString(sc.String).MakeLower();
				}
				else if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("radius"))
				{
					sc.MustGetFloat();
					rule.hasRadius = true;
					rule.radius = (float)sc.Float;
				}
				else if (sc.Compare("range"))
				{
					sc.MustGetFloat();
					rule.hasRange = true;
					rule.range = (float)sc.Float;
				}
				else if (sc.Compare("offset"))
				{
					rule.hasOffset = true;
					MustParseVector3(rule.offset);
				}
				else if (sc.Compare("nudgefromsurface"))
				{
					sc.MustGetFloat();
					rule.hasNudgeFromSurface = true;
					rule.nudgeFromSurfaceDistance = (float)sc.Float;
				}
				else if (sc.Compare("direction"))
				{
					rule.hasDirection = true;
					MustParseVector3(rule.direction);
				}
				else if (sc.Compare("flicker"))
				{
					sc.MustGetNumber();
					rule.hasFlicker = true;
					rule.flickerFrames = std::max(sc.Number, 0);
					rule.hasRandom = false;
					rule.randomIntensityRange[0] = 0.0f;
					rule.randomIntensityRange[1] = 0.0f;
				}
				else if (sc.Compare("random"))
				{
					rule.hasRandom = true;
					MustParseFloatRange(rule.randomIntensityRange, false, false, "random");
					rule.hasFlicker = false;
					rule.flickerFrames = 0;
				}
				else if (sc.Compare("localspace"))
				{
					sc.MustGetString();
					rule.hasLocalSpacePolicy = true;
					rule.localSpacePolicy = sc.String;
				}
				else
				{
					SkipUnknownField("actorrule", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddActorRule(rule);
		}

		void ParseMuzzleFlashRule()
		{
			sc.MustGetString();
			ParsedLightOverlayMuzzleFlashRule rule;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("intensityrandom"))
				{
					rule.hasIntensityRandom = true;
					MustParseFloatRange(rule.intensityRandomRange, false, false, "intensityrandom");
				}
				else if (sc.Compare("radius"))
				{
					sc.MustGetFloat();
					rule.hasRadius = true;
					rule.radius = (float)sc.Float;
				}
				else if (sc.Compare("radiusrandom"))
				{
					rule.hasRadiusRandom = true;
					MustParseFloatRange(rule.radiusRandomRange, false, false, "radiusrandom");
				}
				else if (sc.Compare("delayseconds"))
				{
					sc.MustGetFloat();
					rule.hasDelaySeconds = true;
					rule.delaySeconds = std::max(0.0f, (float)sc.Float);
				}
				else if (sc.Compare("delayrandomseconds"))
				{
					rule.hasDelayRandomSeconds = true;
					MustParseFloatRange(rule.delayRandomSecondsRange, false, false, "delayrandomseconds");
				}
				else if (sc.Compare("durationseconds"))
				{
					sc.MustGetFloat();
					rule.hasDurationSeconds = true;
					rule.durationSeconds = std::max(0.0f, (float)sc.Float);
				}
				else if (sc.Compare("durationrandomseconds"))
				{
					rule.hasDurationRandomSeconds = true;
					MustParseFloatRange(rule.durationRandomSecondsRange, true, true, "durationrandomseconds");
				}
				else if (sc.Compare("offset"))
				{
					rule.hasOffset = true;
					MustParseVector3(rule.offset);
				}
				else
				{
					SkipUnknownField("muzzleflashrule", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddMuzzleFlashRule(rule);
		}

		void ParseSmokeStyle()
		{
			sc.MustGetString();
			ParsedLightOverlaySmokeStyle rule;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("albedo"))
				{
					MustParseVector3(rule.albedo);
					for (float& value : rule.albedo) value = std::clamp(value, 0.0f, 1.0f);
					continue;
				}

				float* value = nullptr;
				float minimum = 0.0f;
				float maximum = 0.0f;
				bool boundedMaximum = false;
				if (sc.Compare("density")) value = &rule.density;
				else if (sc.Compare("extinction")) value = &rule.extinction;
				else if (sc.Compare("anisotropy")) { value = &rule.anisotropy; minimum = -0.99f; maximum = 0.99f; boundedMaximum = true; }
				else if (sc.Compare("radius")) { value = &rule.radius; minimum = 0.001f; }
				else if (sc.Compare("expansionvelocity")) value = &rule.expansionVelocity;
				else if (sc.Compare("lifetime")) { value = &rule.lifetime; minimum = 0.001f; }
				else if (sc.Compare("densityhalflife")) { value = &rule.densityHalfLife; minimum = 0.001f; }
				else if (sc.Compare("risevelocity")) value = &rule.riseVelocity;
				else if (sc.Compare("velocityrandom")) value = &rule.velocityRandom;
				else if (sc.Compare("velocityinherit")) value = &rule.velocityInherit;
				else if (sc.Compare("buoyancy")) value = &rule.buoyancy;
				else if (sc.Compare("drag")) value = &rule.drag;
				else if (sc.Compare("turbulence")) value = &rule.turbulence;
				else if (sc.Compare("turbulencescale")) value = &rule.turbulenceScale;
				else if (sc.Compare("temperature")) value = &rule.temperature;
				else if (sc.Compare("momentumscale")) value = &rule.momentumScale;
				else if (sc.Compare("coolinghalflife")) { value = &rule.coolingHalfLife; minimum = 0.001f; }
				else
				{
					SkipUnknownField("smokestyle", sc.String);
					continue;
				}
				sc.MustGetFloat();
				*value = boundedMaximum ? std::clamp((float)sc.Float, minimum, maximum) : std::max(minimum, (float)sc.Float);
			}
			FinalizeSourceLocation(rule.source);
			builder.AddSmokeStyle(rule);
		}

		void ParseSmokeActorRule()
		{
			sc.MustGetString();
			ParsedLightOverlaySmokeActorRule rule;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("actorclass")) { sc.MustGetString(); rule.actorClassName = sc.String; }
				else if (sc.Compare("ownerclass")) { sc.MustGetString(); rule.ownerClassName = sc.String; }
				else if (sc.Compare("excludeownerclass")) { sc.MustGetString(); rule.excludeOwnerClassName = sc.String; }
				else if (sc.Compare("trigger"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "spawn")) rule.trigger = LightOverlaySmokeTrigger::Spawn;
					else if (!stricmp(sc.String, "interval")) rule.trigger = LightOverlaySmokeTrigger::Interval;
					else sc.ScriptMessage("Invalid smoke trigger '%s'; expected spawn or interval", sc.String);
				}
				else if (sc.Compare("activation"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "surface")) rule.activationPolicy = LightOverlayActorActivationPolicy::Surface;
					else if (!stricmp(sc.String, "immediate")) rule.activationPolicy = LightOverlayActorActivationPolicy::Immediate;
					else sc.ScriptMessage("Invalid smoke actor activation '%s'; expected surface or immediate", sc.String);
				}
				else if (sc.Compare("representation"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "grid")) rule.representation = LightOverlaySmokeRepresentation::Grid;
					else if (!stricmp(sc.String, "analytic")) rule.representation = LightOverlaySmokeRepresentation::Analytic;
					else sc.ScriptMessage("Invalid smoke representation '%s'; expected grid or analytic", sc.String);
				}
				else if (sc.Compare("queuepolicy"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "retry")) rule.queuePolicy = LightOverlaySmokeQueuePolicy::Retry;
					else if (!stricmp(sc.String, "drop")) rule.queuePolicy = LightOverlaySmokeQueuePolicy::Drop;
					else if (!stricmp(sc.String, "latest")) rule.queuePolicy = LightOverlaySmokeQueuePolicy::Latest;
					else sc.ScriptMessage("Invalid smoke queue policy '%s'; expected retry, drop, or latest", sc.String);
				}
				else if (sc.Compare("maxlatencyseconds"))
				{
					sc.MustGetFloat();
					rule.hasMaxLatencySeconds = true;
					rule.maxLatencySeconds = std::max(0.0f, (float)sc.Float);
				}
				else if (sc.Compare("analyticcarriers")) { sc.MustGetNumber(); rule.analyticCarrierCount = (uint32_t)std::clamp(sc.Number, 1, 8); }
				else if (sc.Compare("emitterforeground"))
				{
					sc.MustGetString();
					if (!ParseOnOffToken(sc.String, rule.emitterForeground))
					{
						sc.ScriptMessage("Invalid smoke emitter foreground value '%s'; expected on or off", sc.String);
					}
				}
				else if (sc.Compare("style")) { sc.MustGetString(); rule.styleId = sc.String; }
				else if (sc.Compare("count")) { sc.MustGetNumber(); rule.count = (uint32_t)std::clamp(sc.Number, 1, 256); }
				else if (sc.Compare("offset")) MustParseVector3(rule.offset);
				else if (sc.Compare("spawnradius")) { sc.MustGetFloat(); rule.spawnRadius = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("densityscale")) { sc.MustGetFloat(); rule.densityScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("radiusscale")) { sc.MustGetFloat(); rule.radiusScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("velocitycone")) { sc.MustGetFloat(); rule.velocityCone = std::clamp((float)sc.Float, 0.0f, 180.0f); }
				else if (sc.Compare("velocityscale")) { sc.MustGetFloat(); rule.velocityScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("intervalseconds")) { sc.MustGetFloat(); rule.intervalSeconds = std::max(0.001f, (float)sc.Float); }
				else if (sc.Compare("pulseamount"))
				{
					sc.MustGetFloat();
					const float amount = std::isfinite((float)sc.Float) ? (float)sc.Float : 0.0f;
					rule.pulseAmount = std::clamp(amount, 0.0f, 1.0f);
				}
				else if (sc.Compare("pulseperiodcadences")) { sc.MustGetNumber(); rule.pulsePeriodCadences = (uint32_t)std::clamp(sc.Number, 1, 256); }
				else if (sc.Compare("pulsephase"))
				{
					sc.MustGetFloat();
					const float phase = std::isfinite((float)sc.Float) ? (float)sc.Float : 0.0f;
					rule.pulsePhase = phase - std::floor(phase);
				}
				else if (sc.Compare("starttime")) { sc.MustGetFloat(); rule.startTime = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("startdistance")) { sc.MustGetFloat(); rule.startDistance = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("spacing")) { sc.MustGetFloat(); rule.spacing = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("maxsegmentsperframe")) { sc.MustGetNumber(); rule.maxSegmentsPerFrame = (uint32_t)std::clamp(sc.Number, 1, 256); }
				else SkipUnknownField("smokeactorrule", sc.String);
			}
			FinalizeSourceLocation(rule.source);
			builder.AddSmokeActorRule(rule);
		}

		void ParseSmokeEventRule()
		{
			sc.MustGetString();
			ParsedLightOverlaySmokeEventRule rule;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("style")) { sc.MustGetString(); rule.styleId = sc.String; }
				else if (sc.Compare("representation"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "grid")) rule.representation = LightOverlaySmokeRepresentation::Grid;
					else if (!stricmp(sc.String, "analytic")) rule.representation = LightOverlaySmokeRepresentation::Analytic;
					else sc.ScriptMessage("Invalid smoke representation '%s'; expected grid or analytic", sc.String);
				}
				else if (sc.Compare("queuepolicy"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "retry")) rule.queuePolicy = LightOverlaySmokeQueuePolicy::Retry;
					else if (!stricmp(sc.String, "drop")) rule.queuePolicy = LightOverlaySmokeQueuePolicy::Drop;
					else if (!stricmp(sc.String, "latest")) rule.queuePolicy = LightOverlaySmokeQueuePolicy::Latest;
					else sc.ScriptMessage("Invalid smoke queue policy '%s'; expected retry, drop, or latest", sc.String);
				}
				else if (sc.Compare("maxlatencyseconds"))
				{
					sc.MustGetFloat();
					rule.hasMaxLatencySeconds = true;
					rule.maxLatencySeconds = std::max(0.0f, (float)sc.Float);
				}
				else if (sc.Compare("analyticcarriers")) { sc.MustGetNumber(); rule.analyticCarrierCount = (uint32_t)std::clamp(sc.Number, 1, 8); }
				else if (sc.Compare("count")) { sc.MustGetNumber(); rule.count = (uint32_t)std::clamp(sc.Number, 1, 256); }
				else if (sc.Compare("offset")) MustParseVector3(rule.offset);
				else if (sc.Compare("offsetrandom"))
				{
					MustParseVector3(rule.offsetRandom);
					for (float& value : rule.offsetRandom)
						value = std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
				}
				else if (sc.Compare("spawnradius")) { sc.MustGetFloat(); rule.spawnRadius = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("densityscale")) { sc.MustGetFloat(); rule.densityScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("radiusscale")) { sc.MustGetFloat(); rule.radiusScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("velocitycone")) { sc.MustGetFloat(); rule.velocityCone = std::clamp((float)sc.Float, 0.0f, 180.0f); }
				else if (sc.Compare("velocityscale")) { sc.MustGetFloat(); rule.velocityScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("normaloffset")) { sc.MustGetFloat(); rule.normalOffset = (float)sc.Float; }
				else if (sc.Compare("direction"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "aim")) rule.directionPolicy = LightOverlaySmokeDirectionPolicy::Aim;
					else if (!stricmp(sc.String, "normal")) rule.directionPolicy = LightOverlaySmokeDirectionPolicy::Normal;
					else if (!stricmp(sc.String, "incoming")) rule.directionPolicy = LightOverlaySmokeDirectionPolicy::Incoming;
					else sc.ScriptMessage("Invalid smoke direction '%s'; expected aim, normal, or incoming", sc.String);
				}
				else SkipUnknownField("smokeeventrule", sc.String);
			}
			FinalizeSourceLocation(rule.source);
			builder.AddSmokeEventRule(rule);
		}

		void ParseMapSmokeEmitterRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayMapSmokeEmitterRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("style")) { sc.MustGetString(); rule.styleId = sc.String; }
				else if (sc.Compare("position")) { rule.hasPosition = true; MustParseVector3(rule.position); }
				else if (sc.Compare("normal")) { rule.hasNormal = true; MustParseVector3(rule.normal); }
				else if (sc.Compare("size"))
				{
					sc.MustGetFloat();
					rule.size[0] = std::clamp((float)sc.Float, 1.0f, 256.0f);
					sc.MustGetFloat();
					rule.size[1] = std::clamp((float)sc.Float, 1.0f, 256.0f);
					rule.hasSize = true;
				}
				else if (sc.Compare("rotation")) { sc.MustGetFloat(); rule.rotation = (float)sc.Float; }
				else if (sc.Compare("offset")) { sc.MustGetFloat(); rule.offset = (float)sc.Float; }
				else if (sc.Compare("count")) { sc.MustGetNumber(); rule.count = (uint32_t)std::clamp(sc.Number, 1, 256); }
				else if (sc.Compare("intervalseconds")) { sc.MustGetFloat(); rule.intervalSeconds = std::max(0.001f, (float)sc.Float); }
				else if (sc.Compare("spawnradius")) { sc.MustGetFloat(); rule.spawnRadius = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("densityscale")) { sc.MustGetFloat(); rule.densityScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("radiusscale")) { sc.MustGetFloat(); rule.radiusScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("velocityscale")) { sc.MustGetFloat(); rule.velocityScale = std::max(0.0f, (float)sc.Float); }
				else if (sc.Compare("velocitycone")) { sc.MustGetFloat(); rule.velocityCone = std::clamp((float)sc.Float, 0.0f, 180.0f); }
				else if (sc.Compare("maxsegmentsperframe")) { sc.MustGetNumber(); rule.maxSegmentsPerFrame = (uint32_t)std::clamp(sc.Number, 1, 256); }
				else SkipUnknownField("smokeemitter", sc.String);
			}
			if (rule.styleId.IsEmpty()) sc.ScriptMessage("smokeemitter '%s' requires style", rule.id.GetChars());
			if (!rule.hasPosition) sc.ScriptMessage("smokeemitter '%s' requires position", rule.id.GetChars());
			if (!rule.hasNormal) sc.ScriptMessage("smokeemitter '%s' requires normal", rule.id.GetChars());
			if (!rule.hasSize) sc.ScriptMessage("smokeemitter '%s' requires size", rule.id.GetChars());
			FinalizeSourceLocation(rule.source);
			builder.AddMapSmokeEmitterRule(rule);
		}

		void ParseMapBlock()
		{
			sc.MustGetString();
			FString mapName = sc.String;

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("directional"))
				{
					ParseDirectionalRule(mapName);
				}
				else if (sc.Compare("light"))
				{
					ParseMapLightRule(mapName);
				}
				else if (sc.Compare("emissiveoverride"))
				{
					ParseEmissiveOverrideRule(mapName);
				}
				else if (sc.Compare("surfacelight"))
				{
					ParseSurfaceLightRule(mapName);
				}
				else if (sc.Compare("actoroverride"))
				{
					ParseActorOverrideRule(mapName);
				}
				else if (sc.Compare("smokeemitter"))
				{
					ParseMapSmokeEmitterRule(mapName);
				}
				else
				{
					sc.ScriptMessage("Unknown map-local LIGHTOVR block '%s'", sc.String);
					SkipUnknownDefinition();
				}
			}
		}

		void ParseDirectionalRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayDirectionalRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("direction"))
				{
					rule.hasDirection = true;
					MustParseVector3(rule.direction);
				}
				else if (sc.Compare("angularsize"))
				{
					sc.MustGetFloat();
					rule.hasAngularSize = true;
					rule.angularSize = (float)sc.Float;
				}
				else if (sc.Compare("shadow"))
				{
					sc.MustGetString();
					bool parsed = false;
					rule.hasShadow = ParseOnOffToken(sc.String, rule.shadow);
					parsed = rule.hasShadow;
					if (!parsed)
					{
						sc.ScriptMessage("Invalid shadow value '%s'; expected on/off", sc.String);
					}
				}
				else
				{
					SkipUnknownField("directional", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddDirectionalRule(rule);
		}

		void ParseMapLightRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayMapLightRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("type"))
				{
					sc.MustGetString();
					rule.lightType = FString(sc.String).MakeLower();
				}
				else if (sc.Compare("anchor"))
				{
					sc.MustGetString();
					if (sc.Compare("position"))
					{
						rule.anchorType = LightOverlayAnchorType::Position;
						rule.hasAnchorPosition = true;
						MustParseVector3(rule.anchorPosition);
					}
					else if (sc.Compare("sector"))
					{
						rule.anchorType = LightOverlayAnchorType::Sector;
						sc.MustGetNumber();
						rule.anchorIndex = sc.Number;
					}
					else if (sc.Compare("wall"))
					{
						rule.anchorType = LightOverlayAnchorType::Wall;
						sc.MustGetNumber();
						rule.anchorIndex = sc.Number;
					}
					else
					{
						sc.ScriptMessage("Unknown anchor type '%s' in light definition", sc.String);
					}
				}
				else if (sc.Compare("offset"))
				{
					rule.hasOffset = true;
					MustParseVector3(rule.offset);
				}
				else if (sc.Compare("direction"))
				{
					rule.hasDirection = true;
					MustParseVector3(rule.direction);
				}
				else if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("radius"))
				{
					sc.MustGetFloat();
					rule.hasRadius = true;
					rule.radius = (float)sc.Float;
				}
				else if (sc.Compare("range"))
				{
					sc.MustGetFloat();
					rule.hasRange = true;
					rule.range = (float)sc.Float;
				}
				else if (sc.Compare("flicker"))
				{
					sc.MustGetNumber();
					rule.hasFlicker = true;
					rule.flickerFrames = std::max(sc.Number, 0);
				}
				else
				{
					SkipUnknownField("light", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddMapLightRule(rule);
		}

		void ParseEmissiveOverrideRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayEmissiveOverrideRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("sector"))
				{
					sc.MustGetNumber();
					rule.hasSectorFilter = true;
					rule.sectorFilter = sc.Number;
				}
				else if (sc.Compare("wall"))
				{
					sc.MustGetNumber();
					rule.hasWallFilter = true;
					rule.wallFilter = sc.Number;
				}
				else if (sc.Compare("tile"))
				{
					sc.MustGetNumber();
					rule.hasTileFilter = true;
					rule.tileFilter = sc.Number;
				}
				else if (sc.Compare("intensityscale"))
				{
					sc.MustGetFloat();
					rule.hasIntensityScale = true;
					rule.intensityScale = (float)sc.Float;
				}
				else if (sc.Compare("reachscale"))
				{
					sc.MustGetFloat();
					rule.hasReachScale = true;
					rule.reachScale = (float)sc.Float;
				}
				else if (sc.Compare("responseintensity"))
				{
					sc.MustGetFloat();
					rule.hasResponseIntensity = true;
					rule.responseIntensity = (float)sc.Float;
				}
				else if (sc.Compare("responsemin"))
				{
					sc.MustGetFloat();
					rule.hasResponseMin = true;
					rule.responseMin = (float)sc.Float;
				}
				else if (sc.Compare("responsemax"))
				{
					sc.MustGetFloat();
					rule.hasResponseMax = true;
					rule.responseMax = (float)sc.Float;
				}
				else if (sc.Compare("responseinputmin"))
				{
					sc.MustGetFloat();
					rule.hasResponseInputMin = true;
					rule.responseInputMin = (float)sc.Float;
				}
				else if (sc.Compare("responseinputmax"))
				{
					sc.MustGetFloat();
					rule.hasResponseInputMax = true;
					rule.responseInputMax = (float)sc.Float;
				}
				else if (sc.Compare("responseintensitymin"))
				{
					sc.MustGetFloat();
					rule.hasResponseIntensityMin = true;
					rule.responseIntensityMin = (float)sc.Float;
				}
				else if (sc.Compare("responseintensitymax"))
				{
					sc.MustGetFloat();
					rule.hasResponseIntensityMax = true;
					rule.responseIntensityMax = (float)sc.Float;
				}
				else if (sc.Compare("responsereachmin"))
				{
					sc.MustGetFloat();
					rule.hasResponseReachMin = true;
					rule.responseReachMin = (float)sc.Float;
				}
				else if (sc.Compare("responsereachmax"))
				{
					sc.MustGetFloat();
					rule.hasResponseReachMax = true;
					rule.responseReachMax = (float)sc.Float;
				}
				else if (sc.Compare("materialresponse"))
				{
					sc.MustGetString();
					rule.hasMaterialResponse = true;
					if (sc.Compare("on") || sc.Compare("true") || sc.Compare("yes"))
					{
						rule.materialResponse = true;
					}
					else if (sc.Compare("off") || sc.Compare("false") || sc.Compare("no"))
					{
						rule.materialResponse = false;
					}
					else
					{
						sc.ScriptMessage("Invalid materialresponse value '%s'; expected off/on", sc.String);
					}
				}
				else if (sc.Compare("materialresponsemin"))
				{
					sc.MustGetFloat();
					rule.hasMaterialResponseMin = true;
					rule.materialResponseMin = (float)sc.Float;
				}
				else if (sc.Compare("materialresponsemax"))
				{
					sc.MustGetFloat();
					rule.hasMaterialResponseMax = true;
					rule.materialResponseMax = (float)sc.Float;
				}
				else if (sc.Compare("sectorresponse"))
				{
					sc.MustGetString();
					rule.hasSectorResponse = true;
					if (sc.Compare("on") || sc.Compare("true") || sc.Compare("yes"))
					{
						rule.sectorResponse = true;
					}
					else if (sc.Compare("off") || sc.Compare("false") || sc.Compare("no"))
					{
						rule.sectorResponse = false;
					}
					else
					{
						sc.ScriptMessage("Invalid sectorresponse value '%s'; expected off/on", sc.String);
					}
				}
				else if (sc.Compare("signal"))
				{
					sc.MustGetString();
					if (sc.Compare("sector"))
					{
						sc.MustGetNumber();
						rule.hasSignalSector = true;
						rule.signalSector = sc.Number;
					}
					else
					{
						sc.ScriptMessage("Unknown emissiveoverride signal target '%s'; expected sector", sc.String);
					}
				}
				else
				{
					SkipUnknownField("emissiveoverride", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddEmissiveOverrideRule(rule);
		}

		void ParseSurfaceLightRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlaySurfaceLightRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("anchor"))
				{
					sc.MustGetString();
					if (!sc.Compare("surface"))
					{
						sc.ScriptMessage("Unknown surfacelight anchor '%s'; expected surface", sc.String);
					}
				}
				else if (sc.Compare("position"))
				{
					rule.hasPosition = true;
					MustParseVector3(rule.position);
				}
				else if (sc.Compare("normal"))
				{
					rule.hasNormal = true;
					MustParseVector3(rule.normal);
				}
				else if (sc.Compare("size"))
				{
					sc.MustGetFloat();
					rule.hasSize = true;
					rule.size[0] = (float)sc.Float;
					sc.MustGetFloat();
					rule.size[1] = (float)sc.Float;
				}
				else if (sc.Compare("rotation"))
				{
					sc.MustGetFloat();
					rule.hasRotation = true;
					rule.rotation = (float)sc.Float;
				}
				else if (sc.Compare("offset"))
				{
					sc.MustGetFloat();
					rule.hasOffset = true;
					rule.offset = (float)sc.Float;
				}
				else if (sc.Compare("sector"))
				{
					sc.MustGetNumber();
					rule.hasSector = true;
					rule.sector = sc.Number;
				}
				else if (sc.Compare("wall"))
				{
					sc.MustGetNumber();
					rule.hasWall = true;
					rule.wall = sc.Number;
				}
				else if (sc.Compare("tile"))
				{
					sc.MustGetNumber();
					rule.hasTile = true;
					rule.tile = sc.Number;
				}
				else if (sc.Compare("fixture"))
				{
					sc.MustGetString();
					if (sc.Compare("texture"))
					{
						sc.MustGetString();
						rule.hasFixtureTexture = true;
						rule.fixtureTexture = sc.String;
					}
					else
					{
						sc.ScriptMessage("Unknown surfacelight fixture field '%s'; expected texture", sc.String);
					}
				}
				else if (sc.Compare("fixturetexture"))
				{
					sc.MustGetString();
					rule.hasFixtureTexture = true;
					rule.fixtureTexture = sc.String;
				}
				else if (sc.Compare("fixturematerialresponse"))
				{
					sc.MustGetString();
					rule.hasFixtureMaterialResponse = ParseOnOffToken(sc.String, rule.fixtureMaterialResponse);
					if (!rule.hasFixtureMaterialResponse)
					{
						sc.ScriptMessage("Invalid fixturematerialresponse value '%s'; expected off/on", sc.String);
					}
				}
				else if (sc.Compare("type"))
				{
					sc.MustGetString();
					rule.lightType = FString(sc.String).MakeLower();
				}
				else if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("radius"))
				{
					sc.MustGetFloat();
					rule.hasRadius = true;
					rule.radius = (float)sc.Float;
				}
				else if (sc.Compare("sectorresponse"))
				{
					sc.MustGetString();
					rule.hasSectorResponse = ParseOnOffToken(sc.String, rule.sectorResponse);
					if (!rule.hasSectorResponse)
					{
						sc.ScriptMessage("Invalid sectorresponse value '%s'; expected off/on", sc.String);
					}
				}
				else if (sc.Compare("signal"))
				{
					sc.MustGetString();
					if (sc.Compare("sector"))
					{
						sc.MustGetNumber();
						rule.hasSignalSector = true;
						rule.signalSector = sc.Number;
					}
					else
					{
						sc.ScriptMessage("Unknown surfacelight signal target '%s'; expected sector", sc.String);
					}
				}
				else if (sc.Compare("responseintensity"))
				{
					sc.MustGetFloat();
					rule.hasResponseIntensity = true;
					rule.responseIntensity = (float)sc.Float;
				}
				else if (sc.Compare("responsemin"))
				{
					sc.MustGetFloat();
					rule.hasResponseMin = true;
					rule.responseMin = (float)sc.Float;
				}
				else if (sc.Compare("responsemax"))
				{
					sc.MustGetFloat();
					rule.hasResponseMax = true;
					rule.responseMax = (float)sc.Float;
				}
				else if (sc.Compare("responseinputmin"))
				{
					sc.MustGetFloat();
					rule.hasResponseInputMin = true;
					rule.responseInputMin = (float)sc.Float;
				}
				else if (sc.Compare("responseinputmax"))
				{
					sc.MustGetFloat();
					rule.hasResponseInputMax = true;
					rule.responseInputMax = (float)sc.Float;
				}
				else if (sc.Compare("materialresponsemin"))
				{
					sc.MustGetFloat();
					rule.hasMaterialResponseMin = true;
					rule.materialResponseMin = (float)sc.Float;
				}
				else if (sc.Compare("materialresponsemax"))
				{
					sc.MustGetFloat();
					rule.hasMaterialResponseMax = true;
					rule.materialResponseMax = (float)sc.Float;
				}
				else
				{
					SkipUnknownField("surfacelight", sc.String);
				}
			}

			if (!rule.hasPosition)
			{
				sc.ScriptMessage("surfacelight '%s' is missing position", rule.id.GetChars());
			}
			if (!rule.hasNormal)
			{
				sc.ScriptMessage("surfacelight '%s' is missing normal", rule.id.GetChars());
			}

			FinalizeSourceLocation(rule.source);
			builder.AddSurfaceLightRule(rule);
		}

		void ParseEmissiveMaterialResponseRule()
		{
			sc.MustGetString();
			ParsedLightOverlayEmissiveMaterialResponseRule rule;
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
					const int second = sc.Number;
					LightOverlayTileRange range;
					range.first = std::min(first, second);
					range.last = std::max(first, second);
					rule.tileRanges.Push(range);
				}
				else if (sc.Compare("texture"))
				{
					sc.MustGetString();
					rule.textureNames.Push(sc.String);
				}
				else if (sc.Compare("materialresponse"))
				{
					sc.MustGetString();
					rule.hasMaterialResponse = true;
					if (!ParseOnOffToken(sc.String, rule.materialResponse))
					{
						sc.ScriptMessage("Invalid materialresponse value '%s'; expected off/on", sc.String);
					}
				}
				else if (sc.Compare("materialresponsemin"))
				{
					sc.MustGetFloat();
					rule.hasMaterialResponseMin = true;
					rule.materialResponseMin = (float)sc.Float;
				}
				else if (sc.Compare("materialresponsemax"))
				{
					sc.MustGetFloat();
					rule.hasMaterialResponseMax = true;
					rule.materialResponseMax = (float)sc.Float;
				}
				else if (sc.Compare("visibleglowblend") || sc.Compare("glowblend"))
				{
					sc.MustGetFloat();
					rule.hasVisibleGlowBlend = true;
					rule.visibleGlowBlend = (float)sc.Float;
				}
				else
				{
					SkipUnknownField("emissivematerialresponse", sc.String);
				}
			}

			if (rule.tileFilters.Size() == 0 && rule.tileRanges.Size() == 0 && rule.textureNames.Size() == 0)
			{
				sc.ScriptMessage("emissivematerialresponse '%s' has no tile, tilerange, or texture selector", rule.id.GetChars());
			}

			FinalizeSourceLocation(rule.source);
			builder.AddEmissiveMaterialResponseRule(rule);
		}

		void ParseActorOverrideRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayActorOverrideRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("actorclass"))
				{
					sc.MustGetString();
					rule.actorClassName = sc.String;
				}
				else if (sc.Compare("shadowreceive"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "off"))
					{
						rule.hasShadowReceive = true;
						rule.shadowReceive = false;
					}
					else if (!stricmp(sc.String, "default") || !stricmp(sc.String, "on"))
					{
						rule.hasShadowReceive = true;
						rule.shadowReceive = true;
					}
					else
					{
						sc.ScriptMessage("Invalid shadowreceive value '%s'; expected off/default/on", sc.String);
					}
				}
				else if (sc.Compare("shadowcast"))
				{
					sc.MustGetString();
					if (!stricmp(sc.String, "off"))
					{
						rule.hasShadowCast = true;
						rule.shadowCast = false;
					}
					else if (!stricmp(sc.String, "default") || !stricmp(sc.String, "on"))
					{
						rule.hasShadowCast = true;
						rule.shadowCast = true;
					}
					else
					{
						sc.ScriptMessage("Invalid shadowcast value '%s'; expected off/default/on", sc.String);
					}
				}
				else
				{
					SkipUnknownField("actoroverride", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddActorOverrideRule(rule);
		}
	};

	template <typename RuleType>
	static std::vector<const RuleType*> SortRulesByOrder(const TArray<RuleType>& rules)
	{
		std::vector<const RuleType*> sorted;
		sorted.reserve(rules.Size());
		for (auto& rule : rules)
		{
			sorted.push_back(&rule);
		}
		std::sort(sorted.begin(), sorted.end(), [](const RuleType* left, const RuleType* right)
		{
			if (left->source.orderIndex != right->source.orderIndex)
			{
				return left->source.orderIndex < right->source.orderIndex;
			}
			if (left->source.sourceName.CompareNoCase(right->source.sourceName) != 0)
			{
				return left->source.sourceName.CompareNoCase(right->source.sourceName) < 0;
			}
			return left->id.CompareNoCase(right->id) < 0;
		});
		return sorted;
	}

	template <typename RuleType>
	static std::vector<const RuleType*> SortRulesById(const TArray<RuleType>& rules)
	{
		std::vector<const RuleType*> sorted;
		sorted.reserve(rules.Size());
		for (auto& rule : rules)
		{
			sorted.push_back(&rule);
		}
		std::sort(sorted.begin(), sorted.end(), [](const RuleType* left, const RuleType* right)
		{
			const int idCompare = left->id.CompareNoCase(right->id);
			if (idCompare != 0)
			{
				return idCompare < 0;
			}
			return left->source.orderIndex < right->source.orderIndex;
		});
		return sorted;
	}

	static std::vector<FString> CollectSortedMapNames(const ParsedLightOverlayDatabase& database)
	{
		std::vector<FString> mapNames;
		auto addMap = [&mapNames](const FString& mapName)
		{
			for (const auto& existing : mapNames)
			{
				if (existing.CompareNoCase(mapName) == 0)
				{
					return;
				}
			}
			mapNames.push_back(mapName);
		};

		for (const auto& rule : database.directionalRules) addMap(rule.mapName);
		for (const auto& rule : database.mapLightRules) addMap(rule.mapName);
		for (const auto& rule : database.emissiveOverrideRules) addMap(rule.mapName);
		for (const auto& rule : database.surfaceLightRules) addMap(rule.mapName);
		for (const auto& rule : database.actorOverrideRules) addMap(rule.mapName);
		for (const auto& rule : database.mapSmokeEmitterRules) addMap(rule.mapName);

		std::sort(mapNames.begin(), mapNames.end(), [](const FString& left, const FString& right)
		{
			return left.CompareNoCase(right) < 0;
		});
		return mapNames;
	}

	static void AppendLine(FString& text, int indentLevel, const FString& line)
	{
		for (int i = 0; i < indentLevel; ++i)
		{
			text << "    ";
		}
		text << line << "\n";
	}

	static void AppendVector3Field(FString& text, int indentLevel, const char* name, const float value[3])
	{
		AppendLine(text, indentLevel, FStringf("%s %s %s %s",
			name,
			FormatLightOverlayFloat(value[0]).GetChars(),
			FormatLightOverlayFloat(value[1]).GetChars(),
			FormatLightOverlayFloat(value[2]).GetChars()));
	}

	static void AppendShadowStateField(FString& text, int indentLevel, const char* name, bool enabled)
	{
		AppendLine(text, indentLevel, FStringf("%s %s", name, enabled ? "on" : "off"));
	}

	static void AppendFloatRangeField(FString& text, int indentLevel, const char* name, const float range[2])
	{
		AppendLine(text, indentLevel, FStringf("%s %s %s",
			name,
			FormatLightOverlayFloat(range[0]).GetChars(),
			FormatLightOverlayFloat(range[1]).GetChars()));
	}

	static void AppendActorRuleBlock(FString& text, const ParsedLightOverlayActorRule& rule)
	{
		AppendLine(text, 1, FStringf("actorrule %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 1, "{");
		AppendLine(text, 2, FStringf("actorclass %s", QuoteLightOverlayString(rule.actorClassName).GetChars()));
		if (rule.hasShadowReceive) AppendShadowStateField(text, 2, "shadowreceive", rule.shadowReceive);
		if (rule.hasShadowCast) AppendShadowStateField(text, 2, "shadowcast", rule.shadowCast);
		if (rule.hasLightShadowCast) AppendShadowStateField(text, 2, "lightshadowcast", rule.lightShadowCast);
		if (rule.hasFullbright) AppendShadowStateField(text, 2, "fullbright", rule.fullbright);
		if (rule.hasEmissiveStableFrames) AppendLine(text, 2, FStringf("emissivestableframes %u", rule.emissiveStableFrames));
		if (rule.hasActivationPolicy) AppendLine(text, 2, FStringf("activation %s", ActorActivationPolicyName(rule.activationPolicy)));
		if (rule.hasTileFilter) AppendLine(text, 2, FStringf("tile %d", rule.tileFilter));
		AppendLine(text, 2, FStringf("type %s", QuoteLightOverlayString(rule.lightType).GetChars()));
		if (rule.hasColor) AppendVector3Field(text, 2, "color", rule.color);
		if (rule.hasIntensity) AppendLine(text, 2, FStringf("intensity %s", FormatLightOverlayFloat(rule.intensity).GetChars()));
		if (rule.hasRadius) AppendLine(text, 2, FStringf("radius %s", FormatLightOverlayFloat(rule.radius).GetChars()));
		if (rule.hasRange) AppendLine(text, 2, FStringf("range %s", FormatLightOverlayFloat(rule.range).GetChars()));
		if (rule.hasOffset) AppendVector3Field(text, 2, "offset", rule.offset);
		if (rule.hasNudgeFromSurface) AppendLine(text, 2, FStringf("nudgefromsurface %s", FormatLightOverlayFloat(rule.nudgeFromSurfaceDistance).GetChars()));
		if (rule.hasDirection) AppendVector3Field(text, 2, "direction", rule.direction);
		if (rule.hasFlicker) AppendLine(text, 2, FStringf("flicker %u", rule.flickerFrames));
		if (rule.hasRandom) AppendFloatRangeField(text, 2, "random", rule.randomIntensityRange);
		if (rule.hasLocalSpacePolicy) AppendLine(text, 2, FStringf("localspace %s", QuoteLightOverlayString(rule.localSpacePolicy).GetChars()));
		AppendLine(text, 1, "}");
	}

	static void AppendMuzzleFlashRuleBlock(FString& text, const ParsedLightOverlayMuzzleFlashRule& rule)
	{
		AppendLine(text, 1, FStringf("muzzleflashrule %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 1, "{");
		if (rule.hasColor) AppendVector3Field(text, 2, "color", rule.color);
		if (rule.hasIntensity) AppendLine(text, 2, FStringf("intensity %s", FormatLightOverlayFloat(rule.intensity).GetChars()));
		if (rule.hasIntensityRandom) AppendFloatRangeField(text, 2, "intensityrandom", rule.intensityRandomRange);
		if (rule.hasRadius) AppendLine(text, 2, FStringf("radius %s", FormatLightOverlayFloat(rule.radius).GetChars()));
		if (rule.hasRadiusRandom) AppendFloatRangeField(text, 2, "radiusrandom", rule.radiusRandomRange);
		if (rule.hasDelaySeconds) AppendLine(text, 2, FStringf("delayseconds %s", FormatLightOverlayFloat(rule.delaySeconds).GetChars()));
		if (rule.hasDelayRandomSeconds) AppendFloatRangeField(text, 2, "delayrandomseconds", rule.delayRandomSecondsRange);
		if (rule.hasDurationSeconds) AppendLine(text, 2, FStringf("durationseconds %s", FormatLightOverlayFloat(rule.durationSeconds).GetChars()));
		if (rule.hasDurationRandomSeconds) AppendFloatRangeField(text, 2, "durationrandomseconds", rule.durationRandomSecondsRange);
		if (rule.hasOffset) AppendVector3Field(text, 2, "offset", rule.offset);
		AppendLine(text, 1, "}");
	}

	static void AppendSmokeStyleBlock(FString& text, const ParsedLightOverlaySmokeStyle& rule)
	{
		AppendLine(text, 1, FStringf("smokestyle %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 1, "{");
		AppendLine(text, 2, FStringf("density %s", FormatLightOverlayFloat(rule.density).GetChars()));
		AppendLine(text, 2, FStringf("extinction %s", FormatLightOverlayFloat(rule.extinction).GetChars()));
		AppendVector3Field(text, 2, "albedo", rule.albedo);
		AppendLine(text, 2, FStringf("anisotropy %s", FormatLightOverlayFloat(rule.anisotropy).GetChars()));
		AppendLine(text, 2, FStringf("radius %s", FormatLightOverlayFloat(rule.radius).GetChars()));
		AppendLine(text, 2, FStringf("expansionvelocity %s", FormatLightOverlayFloat(rule.expansionVelocity).GetChars()));
		AppendLine(text, 2, FStringf("lifetime %s", FormatLightOverlayFloat(rule.lifetime).GetChars()));
		AppendLine(text, 2, FStringf("densityhalflife %s", FormatLightOverlayFloat(rule.densityHalfLife).GetChars()));
		AppendLine(text, 2, FStringf("risevelocity %s", FormatLightOverlayFloat(rule.riseVelocity).GetChars()));
		AppendLine(text, 2, FStringf("velocityrandom %s", FormatLightOverlayFloat(rule.velocityRandom).GetChars()));
		AppendLine(text, 2, FStringf("velocityinherit %s", FormatLightOverlayFloat(rule.velocityInherit).GetChars()));
		AppendLine(text, 2, FStringf("buoyancy %s", FormatLightOverlayFloat(rule.buoyancy).GetChars()));
		AppendLine(text, 2, FStringf("drag %s", FormatLightOverlayFloat(rule.drag).GetChars()));
		AppendLine(text, 2, FStringf("turbulence %s", FormatLightOverlayFloat(rule.turbulence).GetChars()));
		AppendLine(text, 2, FStringf("turbulencescale %s", FormatLightOverlayFloat(rule.turbulenceScale).GetChars()));
		AppendLine(text, 2, FStringf("temperature %s", FormatLightOverlayFloat(rule.temperature).GetChars()));
		AppendLine(text, 2, FStringf("momentumscale %s", FormatLightOverlayFloat(rule.momentumScale).GetChars()));
		AppendLine(text, 2, FStringf("coolinghalflife %s", FormatLightOverlayFloat(rule.coolingHalfLife).GetChars()));
		AppendLine(text, 1, "}");
	}

	static void AppendSmokeActorRuleBlock(FString& text, const ParsedLightOverlaySmokeActorRule& rule)
	{
		AppendLine(text, 1, FStringf("smokeactorrule %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 1, "{");
		AppendLine(text, 2, FStringf("actorclass %s", QuoteLightOverlayString(rule.actorClassName).GetChars()));
		if (rule.ownerClassName.IsNotEmpty()) AppendLine(text, 2, FStringf("ownerclass %s", QuoteLightOverlayString(rule.ownerClassName).GetChars()));
		if (rule.excludeOwnerClassName.IsNotEmpty()) AppendLine(text, 2, FStringf("excludeownerclass %s", QuoteLightOverlayString(rule.excludeOwnerClassName).GetChars()));
		AppendLine(text, 2, FStringf("trigger %s", SmokeTriggerName(rule.trigger)));
		AppendLine(text, 2, FStringf("activation %s", ActorActivationPolicyName(rule.activationPolicy)));
		AppendLine(text, 2, FStringf("representation %s", SmokeRepresentationName(rule.representation)));
		AppendLine(text, 2, FStringf("queuepolicy %s", SmokeQueuePolicyName(rule.queuePolicy)));
		if (rule.hasMaxLatencySeconds) AppendLine(text, 2, FStringf("maxlatencyseconds %s", FormatLightOverlayFloat(rule.maxLatencySeconds).GetChars()));
		AppendLine(text, 2, FStringf("analyticcarriers %u", rule.analyticCarrierCount));
		AppendLine(text, 2, FStringf("emitterforeground %s", rule.emitterForeground ? "on" : "off"));
		AppendLine(text, 2, FStringf("style %s", QuoteLightOverlayString(rule.styleId).GetChars()));
		AppendLine(text, 2, FStringf("count %u", rule.count));
		AppendVector3Field(text, 2, "offset", rule.offset);
		AppendLine(text, 2, FStringf("spawnradius %s", FormatLightOverlayFloat(rule.spawnRadius).GetChars()));
		AppendLine(text, 2, FStringf("densityscale %s", FormatLightOverlayFloat(rule.densityScale).GetChars()));
		AppendLine(text, 2, FStringf("radiusscale %s", FormatLightOverlayFloat(rule.radiusScale).GetChars()));
		AppendLine(text, 2, FStringf("velocitycone %s", FormatLightOverlayFloat(rule.velocityCone).GetChars()));
		AppendLine(text, 2, FStringf("velocityscale %s", FormatLightOverlayFloat(rule.velocityScale).GetChars()));
		AppendLine(text, 2, FStringf("intervalseconds %s", FormatLightOverlayFloat(rule.intervalSeconds).GetChars()));
		AppendLine(text, 2, FStringf("pulseamount %s", FormatLightOverlayFloat(rule.pulseAmount).GetChars()));
		AppendLine(text, 2, FStringf("pulseperiodcadences %u", rule.pulsePeriodCadences));
		AppendLine(text, 2, FStringf("pulsephase %s", FormatLightOverlayFloat(rule.pulsePhase).GetChars()));
		AppendLine(text, 2, FStringf("starttime %s", FormatLightOverlayFloat(rule.startTime).GetChars()));
		AppendLine(text, 2, FStringf("startdistance %s", FormatLightOverlayFloat(rule.startDistance).GetChars()));
		AppendLine(text, 2, FStringf("spacing %s", FormatLightOverlayFloat(rule.spacing).GetChars()));
		AppendLine(text, 2, FStringf("maxsegmentsperframe %u", rule.maxSegmentsPerFrame));
		AppendLine(text, 1, "}");
	}

	static void AppendSmokeEventRuleBlock(FString& text, const ParsedLightOverlaySmokeEventRule& rule)
	{
		AppendLine(text, 1, FStringf("smokeeventrule %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 1, "{");
		AppendLine(text, 2, FStringf("style %s", QuoteLightOverlayString(rule.styleId).GetChars()));
		AppendLine(text, 2, FStringf("representation %s", SmokeRepresentationName(rule.representation)));
		AppendLine(text, 2, FStringf("queuepolicy %s", SmokeQueuePolicyName(rule.queuePolicy)));
		if (rule.hasMaxLatencySeconds) AppendLine(text, 2, FStringf("maxlatencyseconds %s", FormatLightOverlayFloat(rule.maxLatencySeconds).GetChars()));
		AppendLine(text, 2, FStringf("analyticcarriers %u", rule.analyticCarrierCount));
		AppendLine(text, 2, FStringf("count %u", rule.count));
		AppendVector3Field(text, 2, "offset", rule.offset);
		AppendVector3Field(text, 2, "offsetrandom", rule.offsetRandom);
		AppendLine(text, 2, FStringf("spawnradius %s", FormatLightOverlayFloat(rule.spawnRadius).GetChars()));
		AppendLine(text, 2, FStringf("densityscale %s", FormatLightOverlayFloat(rule.densityScale).GetChars()));
		AppendLine(text, 2, FStringf("radiusscale %s", FormatLightOverlayFloat(rule.radiusScale).GetChars()));
		AppendLine(text, 2, FStringf("velocitycone %s", FormatLightOverlayFloat(rule.velocityCone).GetChars()));
		AppendLine(text, 2, FStringf("velocityscale %s", FormatLightOverlayFloat(rule.velocityScale).GetChars()));
		AppendLine(text, 2, FStringf("normaloffset %s", FormatLightOverlayFloat(rule.normalOffset).GetChars()));
		AppendLine(text, 2, FStringf("direction %s", SmokeDirectionPolicyName(rule.directionPolicy)));
		AppendLine(text, 1, "}");
	}

	static void AppendMapSmokeEmitterRuleBlock(FString& text, const ParsedLightOverlayMapSmokeEmitterRule& rule)
	{
		AppendLine(text, 2, FStringf("smokeemitter %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 2, "{");
		AppendLine(text, 3, FStringf("style %s", QuoteLightOverlayString(rule.styleId).GetChars()));
		if (rule.hasPosition) AppendVector3Field(text, 3, "position", rule.position);
		if (rule.hasNormal) AppendVector3Field(text, 3, "normal", rule.normal);
		if (rule.hasSize)
		{
			AppendLine(text, 3, FStringf("size %s %s",
				FormatLightOverlayFloat(rule.size[0]).GetChars(),
				FormatLightOverlayFloat(rule.size[1]).GetChars()));
		}
		AppendLine(text, 3, FStringf("rotation %s", FormatLightOverlayFloat(rule.rotation).GetChars()));
		AppendLine(text, 3, FStringf("offset %s", FormatLightOverlayFloat(rule.offset).GetChars()));
		AppendLine(text, 3, FStringf("count %u", rule.count));
		AppendLine(text, 3, FStringf("intervalseconds %s", FormatLightOverlayFloat(rule.intervalSeconds).GetChars()));
		AppendLine(text, 3, FStringf("spawnradius %s", FormatLightOverlayFloat(rule.spawnRadius).GetChars()));
		AppendLine(text, 3, FStringf("densityscale %s", FormatLightOverlayFloat(rule.densityScale).GetChars()));
		AppendLine(text, 3, FStringf("radiusscale %s", FormatLightOverlayFloat(rule.radiusScale).GetChars()));
		AppendLine(text, 3, FStringf("velocityscale %s", FormatLightOverlayFloat(rule.velocityScale).GetChars()));
		AppendLine(text, 3, FStringf("velocitycone %s", FormatLightOverlayFloat(rule.velocityCone).GetChars()));
		AppendLine(text, 3, FStringf("maxsegmentsperframe %u", rule.maxSegmentsPerFrame));
		AppendLine(text, 2, "}");
	}

	static void AppendDirectionalRuleBlock(FString& text, const ParsedLightOverlayDirectionalRule& rule)
	{
		AppendLine(text, 2, FStringf("directional %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 2, "{");
		if (rule.hasColor) AppendVector3Field(text, 3, "color", rule.color);
		if (rule.hasIntensity) AppendLine(text, 3, FStringf("intensity %s", FormatLightOverlayFloat(rule.intensity).GetChars()));
		if (rule.hasDirection) AppendVector3Field(text, 3, "direction", rule.direction);
		if (rule.hasAngularSize) AppendLine(text, 3, FStringf("angularsize %s", FormatLightOverlayFloat(rule.angularSize).GetChars()));
		if (rule.hasShadow) AppendShadowStateField(text, 3, "shadow", rule.shadow);
		AppendLine(text, 2, "}");
	}

	static void AppendMapLightRuleBlock(FString& text, const ParsedLightOverlayMapLightRule& rule)
	{
		AppendLine(text, 2, FStringf("light %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 2, "{");
		AppendLine(text, 3, FStringf("type %s", QuoteLightOverlayString(rule.lightType).GetChars()));
		switch (rule.anchorType)
		{
		case LightOverlayAnchorType::Position:
			AppendLine(text, 3, FStringf("anchor position %s %s %s",
				FormatLightOverlayFloat(rule.anchorPosition[0]).GetChars(),
				FormatLightOverlayFloat(rule.anchorPosition[1]).GetChars(),
				FormatLightOverlayFloat(rule.anchorPosition[2]).GetChars()));
			break;
		case LightOverlayAnchorType::Sector:
			AppendLine(text, 3, FStringf("anchor sector %d", rule.anchorIndex));
			break;
		case LightOverlayAnchorType::Wall:
			AppendLine(text, 3, FStringf("anchor wall %d", rule.anchorIndex));
			break;
		default:
			break;
		}
		if (rule.hasOffset) AppendVector3Field(text, 3, "offset", rule.offset);
		if (rule.hasDirection) AppendVector3Field(text, 3, "direction", rule.direction);
		if (rule.hasColor) AppendVector3Field(text, 3, "color", rule.color);
		if (rule.hasIntensity) AppendLine(text, 3, FStringf("intensity %s", FormatLightOverlayFloat(rule.intensity).GetChars()));
		if (rule.hasRadius) AppendLine(text, 3, FStringf("radius %s", FormatLightOverlayFloat(rule.radius).GetChars()));
		if (rule.hasRange) AppendLine(text, 3, FStringf("range %s", FormatLightOverlayFloat(rule.range).GetChars()));
		if (rule.hasFlicker) AppendLine(text, 3, FStringf("flicker %u", rule.flickerFrames));
		AppendLine(text, 2, "}");
	}

	static void AppendEmissiveOverrideRuleBlock(FString& text, const ParsedLightOverlayEmissiveOverrideRule& rule)
	{
		AppendLine(text, 2, FStringf("emissiveoverride %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 2, "{");
		if (rule.hasSectorFilter) AppendLine(text, 3, FStringf("sector %d", rule.sectorFilter));
		if (rule.hasWallFilter) AppendLine(text, 3, FStringf("wall %d", rule.wallFilter));
		if (rule.hasTileFilter) AppendLine(text, 3, FStringf("tile %d", rule.tileFilter));
		if (rule.hasIntensityScale) AppendLine(text, 3, FStringf("intensityscale %s", FormatLightOverlayFloat(rule.intensityScale).GetChars()));
		if (rule.hasReachScale) AppendLine(text, 3, FStringf("reachscale %s", FormatLightOverlayFloat(rule.reachScale).GetChars()));
		if (rule.hasSectorResponse) AppendShadowStateField(text, 3, "sectorresponse", rule.sectorResponse);
		if (rule.hasSignalSector) AppendLine(text, 3, FStringf("signal sector %d", rule.signalSector));
		if (rule.hasResponseIntensity) AppendLine(text, 3, FStringf("responseintensity %s", FormatLightOverlayFloat(rule.responseIntensity).GetChars()));
		if (rule.hasResponseMin) AppendLine(text, 3, FStringf("responsemin %s", FormatLightOverlayFloat(rule.responseMin).GetChars()));
		if (rule.hasResponseMax) AppendLine(text, 3, FStringf("responsemax %s", FormatLightOverlayFloat(rule.responseMax).GetChars()));
		if (rule.hasResponseInputMin) AppendLine(text, 3, FStringf("responseinputmin %s", FormatLightOverlayFloat(rule.responseInputMin).GetChars()));
		if (rule.hasResponseInputMax) AppendLine(text, 3, FStringf("responseinputmax %s", FormatLightOverlayFloat(rule.responseInputMax).GetChars()));
		if (rule.hasResponseIntensityMin) AppendLine(text, 3, FStringf("responseintensitymin %s", FormatLightOverlayFloat(rule.responseIntensityMin).GetChars()));
		if (rule.hasResponseIntensityMax) AppendLine(text, 3, FStringf("responseintensitymax %s", FormatLightOverlayFloat(rule.responseIntensityMax).GetChars()));
		if (rule.hasResponseReachMin) AppendLine(text, 3, FStringf("responsereachmin %s", FormatLightOverlayFloat(rule.responseReachMin).GetChars()));
		if (rule.hasResponseReachMax) AppendLine(text, 3, FStringf("responsereachmax %s", FormatLightOverlayFloat(rule.responseReachMax).GetChars()));
		if (rule.hasMaterialResponse) AppendShadowStateField(text, 3, "materialresponse", rule.materialResponse);
		if (rule.hasMaterialResponseMin) AppendLine(text, 3, FStringf("materialresponsemin %s", FormatLightOverlayFloat(rule.materialResponseMin).GetChars()));
		if (rule.hasMaterialResponseMax) AppendLine(text, 3, FStringf("materialresponsemax %s", FormatLightOverlayFloat(rule.materialResponseMax).GetChars()));
		AppendLine(text, 2, "}");
	}

	static void AppendSurfaceLightRuleBlock(FString& text, const ParsedLightOverlaySurfaceLightRule& rule)
	{
		AppendLine(text, 2, FStringf("surfacelight %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 2, "{");
		AppendLine(text, 3, "anchor surface");
		if (rule.hasPosition) AppendVector3Field(text, 3, "position", rule.position);
		if (rule.hasNormal) AppendVector3Field(text, 3, "normal", rule.normal);
		if (rule.hasSize)
		{
			AppendLine(text, 3, FStringf("size %s %s",
				FormatLightOverlayFloat(rule.size[0]).GetChars(),
				FormatLightOverlayFloat(rule.size[1]).GetChars()));
		}
		if (rule.hasRotation) AppendLine(text, 3, FStringf("rotation %s", FormatLightOverlayFloat(rule.rotation).GetChars()));
		if (rule.hasOffset) AppendLine(text, 3, FStringf("offset %s", FormatLightOverlayFloat(rule.offset).GetChars()));
		if (rule.hasSector) AppendLine(text, 3, FStringf("sector %d", rule.sector));
		if (rule.hasWall) AppendLine(text, 3, FStringf("wall %d", rule.wall));
		if (rule.hasTile) AppendLine(text, 3, FStringf("tile %d", rule.tile));
		if (rule.hasFixtureTexture) AppendLine(text, 3, FStringf("fixture texture %s", QuoteLightOverlayString(rule.fixtureTexture).GetChars()));
		if (rule.hasFixtureMaterialResponse) AppendShadowStateField(text, 3, "fixturematerialresponse", rule.fixtureMaterialResponse);
		if (rule.lightType.IsNotEmpty()) AppendLine(text, 3, FStringf("type %s", QuoteLightOverlayString(rule.lightType).GetChars()));
		if (rule.hasColor) AppendVector3Field(text, 3, "color", rule.color);
		if (rule.hasIntensity) AppendLine(text, 3, FStringf("intensity %s", FormatLightOverlayFloat(rule.intensity).GetChars()));
		if (rule.hasRadius) AppendLine(text, 3, FStringf("radius %s", FormatLightOverlayFloat(rule.radius).GetChars()));
		if (rule.hasSectorResponse) AppendShadowStateField(text, 3, "sectorresponse", rule.sectorResponse);
		if (rule.hasSignalSector) AppendLine(text, 3, FStringf("signal sector %d", rule.signalSector));
		if (rule.hasResponseIntensity) AppendLine(text, 3, FStringf("responseintensity %s", FormatLightOverlayFloat(rule.responseIntensity).GetChars()));
		if (rule.hasResponseMin) AppendLine(text, 3, FStringf("responsemin %s", FormatLightOverlayFloat(rule.responseMin).GetChars()));
		if (rule.hasResponseMax) AppendLine(text, 3, FStringf("responsemax %s", FormatLightOverlayFloat(rule.responseMax).GetChars()));
		if (rule.hasResponseInputMin) AppendLine(text, 3, FStringf("responseinputmin %s", FormatLightOverlayFloat(rule.responseInputMin).GetChars()));
		if (rule.hasResponseInputMax) AppendLine(text, 3, FStringf("responseinputmax %s", FormatLightOverlayFloat(rule.responseInputMax).GetChars()));
		if (rule.hasMaterialResponseMin) AppendLine(text, 3, FStringf("materialresponsemin %s", FormatLightOverlayFloat(rule.materialResponseMin).GetChars()));
		if (rule.hasMaterialResponseMax) AppendLine(text, 3, FStringf("materialresponsemax %s", FormatLightOverlayFloat(rule.materialResponseMax).GetChars()));
		AppendLine(text, 2, "}");
	}

	static void AppendEmissiveMaterialResponseRuleBlock(FString& text, const ParsedLightOverlayEmissiveMaterialResponseRule& rule)
	{
		AppendLine(text, 1, FStringf("emissivematerialresponse %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 1, "{");
		for (int tile : rule.tileFilters)
		{
			AppendLine(text, 2, FStringf("tile %d", tile));
		}
		for (const auto& range : rule.tileRanges)
		{
			AppendLine(text, 2, FStringf("tilerange %d %d", range.first, range.last));
		}
		for (const auto& textureName : rule.textureNames)
		{
			AppendLine(text, 2, FStringf("texture %s", QuoteLightOverlayString(textureName).GetChars()));
		}
		if (rule.hasMaterialResponse) AppendShadowStateField(text, 2, "materialresponse", rule.materialResponse);
		if (rule.hasMaterialResponseMin) AppendLine(text, 2, FStringf("materialresponsemin %s", FormatLightOverlayFloat(rule.materialResponseMin).GetChars()));
		if (rule.hasMaterialResponseMax) AppendLine(text, 2, FStringf("materialresponsemax %s", FormatLightOverlayFloat(rule.materialResponseMax).GetChars()));
		if (rule.hasVisibleGlowBlend) AppendLine(text, 2, FStringf("visibleglowblend %s", FormatLightOverlayFloat(rule.visibleGlowBlend).GetChars()));
		AppendLine(text, 1, "}");
	}

	static void AppendActorOverrideRuleBlock(FString& text, const ParsedLightOverlayActorOverrideRule& rule)
	{
		AppendLine(text, 2, FStringf("actoroverride %s", QuoteLightOverlayString(rule.id).GetChars()));
		AppendLine(text, 2, "{");
		AppendLine(text, 3, FStringf("actorclass %s", QuoteLightOverlayString(rule.actorClassName).GetChars()));
		if (rule.hasShadowReceive) AppendShadowStateField(text, 3, "shadowreceive", rule.shadowReceive);
		if (rule.hasShadowCast) AppendShadowStateField(text, 3, "shadowcast", rule.shadowCast);
		AppendLine(text, 2, "}");
	}

	static uint64_t ComputeLightOverlayTextHash(const FString& text)
	{
		uint64_t hash = 1469598103934665603ull;
		for (const char* p = text.GetChars(); *p != 0; ++p)
		{
			hash = HashCombineLightOverlayText(hash, (uint8_t)*p);
		}
		return hash;
	}

	static int32_t FindActorRuleIndex(const ParsedLightOverlayDatabase& database, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.actorRules.Size(); ++i)
		{
			if (database.actorRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	template<class T>
	static int32_t FindGlobalRuleIndex(const TArray<T>& rules, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)rules.Size(); ++i)
		{
			if (rules[i].id.CompareNoCase(id) == 0) return (int32_t)i;
		}
		return -1;
	}

	static int32_t FindDirectionalRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.directionalRules.Size(); ++i)
		{
			if (database.directionalRules[i].mapName.CompareNoCase(mapName) == 0 &&
				database.directionalRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindMuzzleFlashRuleIndex(const ParsedLightOverlayDatabase& database, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.muzzleFlashRules.Size(); ++i)
		{
			if (database.muzzleFlashRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindMapLightRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.mapLightRules.Size(); ++i)
		{
			if (database.mapLightRules[i].mapName.CompareNoCase(mapName) == 0 &&
				database.mapLightRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindEmissiveOverrideRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.emissiveOverrideRules.Size(); ++i)
		{
			if (database.emissiveOverrideRules[i].mapName.CompareNoCase(mapName) == 0 &&
				database.emissiveOverrideRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindEmissiveMaterialResponseRuleIndex(const ParsedLightOverlayDatabase& database, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.emissiveMaterialResponseRules.Size(); ++i)
		{
			if (database.emissiveMaterialResponseRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindSurfaceLightRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.surfaceLightRules.Size(); ++i)
		{
			if (database.surfaceLightRules[i].mapName.CompareNoCase(mapName) == 0 &&
				database.surfaceLightRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindActorOverrideRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.actorOverrideRules.Size(); ++i)
		{
			if (database.actorOverrideRules[i].mapName.CompareNoCase(mapName) == 0 &&
				database.actorOverrideRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindMapSmokeEmitterRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.mapSmokeEmitterRules.Size(); ++i)
		{
			if (database.mapSmokeEmitterRules[i].mapName.CompareNoCase(mapName) == 0 &&
				database.mapSmokeEmitterRules[i].id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static uint32_t FindNextLightOverlayOrderIndex(const ParsedLightOverlayDatabase& database)
	{
		uint32_t nextOrderIndex = 0;
		auto update = [&nextOrderIndex](const LightOverlaySourceLocation& source)
		{
			nextOrderIndex = std::max(nextOrderIndex, source.orderIndex);
		};

		if (database.defaults.present) update(database.defaults.source);
		for (const auto& rule : database.actorRules) update(rule.source);
		for (const auto& rule : database.directionalRules) update(rule.source);
		for (const auto& rule : database.muzzleFlashRules) update(rule.source);
		for (const auto& rule : database.mapLightRules) update(rule.source);
		for (const auto& rule : database.emissiveOverrideRules) update(rule.source);
		for (const auto& rule : database.emissiveMaterialResponseRules) update(rule.source);
		for (const auto& rule : database.surfaceLightRules) update(rule.source);
		for (const auto& rule : database.actorOverrideRules) update(rule.source);
		for (const auto& rule : database.smokeStyles) update(rule.source);
		for (const auto& rule : database.smokeActorRules) update(rule.source);
		for (const auto& rule : database.smokeEventRules) update(rule.source);
		for (const auto& rule : database.mapSmokeEmitterRules) update(rule.source);
		return nextOrderIndex + 1;
	}

	static void EnsureEditableSource(ParsedLightOverlayDatabase& database, LightOverlaySourceLocation& source, const LightOverlaySourceLocation* existingSource = nullptr)
	{
		if (source.sourceName.IsEmpty())
		{
			source.sourceName = "editor";
		}
		if (source.orderIndex == 0)
		{
			source.orderIndex = existingSource != nullptr && existingSource->orderIndex != 0 ?
				existingSource->orderIndex :
				FindNextLightOverlayOrderIndex(database);
		}

		bool found = false;
		for (const auto& sourceFile : database.sourceFiles)
		{
			if (sourceFile.lumpNum == source.lumpNum &&
				sourceFile.sourceName.CompareNoCase(source.sourceName) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			ParsedLightOverlaySourceFile sourceFile;
			sourceFile.lumpNum = source.lumpNum;
			sourceFile.sourceName = source.sourceName;
			database.sourceFiles.Push(sourceFile);
		}
	}

	static void DumpParsedLightOverlayDatabase(const ParsedLightOverlayDatabase& database)
	{
		Printf("LIGHTOVR: generation=%u files=%d actor_rules=%d muzzle_flashes=%d map_lights=%d emissive_overrides=%d emissive_material_responses=%d surface_lights=%d directional=%d actor_overrides=%d smoke_styles=%d smoke_actor_rules=%d smoke_event_rules=%d map_smoke_emitters=%d parse_errors=%s\n",
			database.generation,
			database.sourceFiles.Size(),
			database.actorRules.Size(),
			database.muzzleFlashRules.Size(),
			database.mapLightRules.Size(),
			database.emissiveOverrideRules.Size(),
			database.emissiveMaterialResponseRules.Size(),
			database.surfaceLightRules.Size(),
			database.directionalRules.Size(),
			database.actorOverrideRules.Size(),
			database.smokeStyles.Size(),
			database.smokeActorRules.Size(),
			database.smokeEventRules.Size(),
			database.mapSmokeEmitterRules.Size(),
			database.hadParseErrors ? "yes" : "no");

		for (const auto& sourceFile : database.sourceFiles)
		{
			Printf("LIGHTOVR file: %s parse_errors=%s\n",
				sourceFile.sourceName.GetChars(),
				sourceFile.hadParseErrors ? "yes" : "no");
		}

		if (database.defaults.present)
		{
			Printf("LIGHTOVR defaults: source=%s\n", SourceLocationText(database.defaults.source).GetChars());
		}

		for (const ParsedLightOverlayActorRule* rule : SortRulesByOrder(database.actorRules))
		{
			Printf("LIGHTOVR actorrule %s: actorclass=%s type=%s shadowreceive=%s shadowcast=%s lightshadowcast=%s fullbright=%s activation=%s source=%s\n",
				rule->id.GetChars(),
				rule->actorClassName.GetChars(),
				rule->lightType.GetChars(),
				rule->hasShadowReceive ? (rule->shadowReceive ? "default" : "off") : "(unset)",
				rule->hasShadowCast ? (rule->shadowCast ? "default" : "off") : "(unset)",
				rule->hasLightShadowCast ? (rule->lightShadowCast ? "default" : "off") : "(unset)",
				rule->hasFullbright ? (rule->fullbright ? "on" : "off") : "(unset)",
				rule->hasActivationPolicy ? ActorActivationPolicyName(rule->activationPolicy) : "(default)",
				SourceLocationText(rule->source).GetChars());
			if (rule->hasTileFilter) Printf("  tile=%d\n", rule->tileFilter);
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasRadius) Printf("  radius=%.3f\n", rule->radius);
			if (rule->hasRange) Printf("  range=%.3f\n", rule->range);
			if (rule->hasOffset) Printf("  offset=(%.3f, %.3f, %.3f)\n", rule->offset[0], rule->offset[1], rule->offset[2]);
			if (rule->hasNudgeFromSurface) Printf("  nudgefromsurface=%.3f\n", rule->nudgeFromSurfaceDistance);
			if (rule->hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule->direction[0], rule->direction[1], rule->direction[2]);
			if (rule->hasEmissiveStableFrames) Printf("  emissivestableframes=%u\n", rule->emissiveStableFrames);
			if (rule->hasFlicker) Printf("  flicker_frames=%u\n", rule->flickerFrames);
			if (rule->hasRandom) Printf("  random=(%.3f, %.3f)\n", rule->randomIntensityRange[0], rule->randomIntensityRange[1]);
			if (rule->hasLocalSpacePolicy) Printf("  localspace=%s\n", rule->localSpacePolicy.GetChars());
		}

		for (const ParsedLightOverlayMuzzleFlashRule* rule : SortRulesByOrder(database.muzzleFlashRules))
		{
			Printf("LIGHTOVR muzzleflashrule %s: source=%s\n",
				rule->id.GetChars(),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasIntensityRandom) Printf("  intensityrandom=(%.3f, %.3f)\n", rule->intensityRandomRange[0], rule->intensityRandomRange[1]);
			if (rule->hasRadius) Printf("  radius=%.3f\n", rule->radius);
			if (rule->hasRadiusRandom) Printf("  radiusrandom=(%.3f, %.3f)\n", rule->radiusRandomRange[0], rule->radiusRandomRange[1]);
			if (rule->hasDelaySeconds) Printf("  delayseconds=%.3f\n", rule->delaySeconds);
			if (rule->hasDelayRandomSeconds) Printf("  delayrandomseconds=(%.3f, %.3f)\n", rule->delayRandomSecondsRange[0], rule->delayRandomSecondsRange[1]);
			if (rule->hasDurationSeconds) Printf("  durationseconds=%.3f\n", rule->durationSeconds);
			if (rule->hasDurationRandomSeconds) Printf("  durationrandomseconds=(%.3f, %.3f)\n", rule->durationRandomSecondsRange[0], rule->durationRandomSecondsRange[1]);
			if (rule->hasOffset) Printf("  offset=(%.3f, %.3f, %.3f)\n", rule->offset[0], rule->offset[1], rule->offset[2]);
		}

		for (const auto* rule : SortRulesByOrder(database.smokeStyles))
		{
			Printf("LIGHTOVR smokestyle %s: density=%.3f extinction=%.4f radius=%.3f lifetime=%.3f half_life=%.3f source=%s\n",
				rule->id.GetChars(), rule->density, rule->extinction, rule->radius, rule->lifetime, rule->densityHalfLife,
				SourceLocationText(rule->source).GetChars());
		}
		for (const auto* rule : SortRulesByOrder(database.smokeActorRules))
		{
			Printf("LIGHTOVR smokeactorrule %s: actorclass=%s ownerclass=%s excludeownerclass=%s trigger=%s activation=%s representation=%s queuepolicy=%s maxlatency=%s analyticcarriers=%u emitterforeground=%s style=%s "
				"count=%u offset=(%.3f,%.3f,%.3f) spawnradius=%.3f densityscale=%.3f radiusscale=%.3f "
				"velocitycone=%.3f velocityscale=%.3f intervalseconds=%.3f pulseamount=%.3f pulseperiodcadences=%u pulsephase=%.3f starttime=%.3f startdistance=%.3f spacing=%.3f maxsegmentsperframe=%u source=%s\n",
				rule->id.GetChars(), rule->actorClassName.GetChars(),
				rule->ownerClassName.IsNotEmpty() ? rule->ownerClassName.GetChars() : "none",
				rule->excludeOwnerClassName.IsNotEmpty() ? rule->excludeOwnerClassName.GetChars() : "none",
				SmokeTriggerName(rule->trigger), ActorActivationPolicyName(rule->activationPolicy), SmokeRepresentationName(rule->representation), SmokeQueuePolicyName(rule->queuePolicy),
				rule->hasMaxLatencySeconds ? FormatLightOverlayFloat(rule->maxLatencySeconds).GetChars() : "none", rule->analyticCarrierCount, rule->emitterForeground ? "on" : "off", rule->styleId.GetChars(), rule->count,
				rule->offset[0], rule->offset[1], rule->offset[2], rule->spawnRadius, rule->densityScale,
				rule->radiusScale, rule->velocityCone, rule->velocityScale, rule->intervalSeconds,
				rule->pulseAmount, rule->pulsePeriodCadences, rule->pulsePhase,
				rule->startTime, rule->startDistance, rule->spacing,
				rule->maxSegmentsPerFrame, SourceLocationText(rule->source).GetChars());
		}
		for (const auto* rule : SortRulesByOrder(database.smokeEventRules))
		{
			Printf("LIGHTOVR smokeeventrule %s: style=%s representation=%s queuepolicy=%s maxlatency=%s analyticcarriers=%u count=%u offsetrandom=(%.3f,%.3f,%.3f) spawnradius=%.3f velocityscale=%.3f "
				"normaloffset=%.3f direction=%s source=%s\n",
				rule->id.GetChars(), rule->styleId.GetChars(), SmokeRepresentationName(rule->representation), SmokeQueuePolicyName(rule->queuePolicy),
				rule->hasMaxLatencySeconds ? FormatLightOverlayFloat(rule->maxLatencySeconds).GetChars() : "none", rule->analyticCarrierCount, rule->count,
				rule->offsetRandom[0], rule->offsetRandom[1], rule->offsetRandom[2], rule->spawnRadius, rule->velocityScale,
				rule->normalOffset, SmokeDirectionPolicyName(rule->directionPolicy),
				SourceLocationText(rule->source).GetChars());
		}
		for (const auto* rule : SortRulesByOrder(database.mapSmokeEmitterRules))
		{
			Printf("LIGHTOVR smokeemitter %s: map=%s style=%s position=(%.3f,%.3f,%.3f) normal=(%.3f,%.3f,%.3f) "
				"size=(%.3f,%.3f) rotation=%.3f offset=%.3f count=%u intervalseconds=%.3f spawnradius=%.3f "
				"densityscale=%.3f radiusscale=%.3f velocityscale=%.3f velocitycone=%.3f maxsegmentsperframe=%u source=%s\n",
				rule->id.GetChars(), rule->mapName.GetChars(), rule->styleId.GetChars(),
				rule->position[0], rule->position[1], rule->position[2],
				rule->normal[0], rule->normal[1], rule->normal[2], rule->size[0], rule->size[1],
				rule->rotation, rule->offset, rule->count, rule->intervalSeconds, rule->spawnRadius,
				rule->densityScale, rule->radiusScale, rule->velocityScale, rule->velocityCone,
				rule->maxSegmentsPerFrame, SourceLocationText(rule->source).GetChars());
		}

		for (const ParsedLightOverlayDirectionalRule* rule : SortRulesByOrder(database.directionalRules))
		{
			Printf("LIGHTOVR directional %s map=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule->direction[0], rule->direction[1], rule->direction[2]);
			if (rule->hasAngularSize) Printf("  angularsize=%.3f\n", rule->angularSize);
			if (rule->hasShadow) Printf("  shadow=%s\n", rule->shadow ? "on" : "off");
		}

		for (const ParsedLightOverlayMapLightRule* rule : SortRulesByOrder(database.mapLightRules))
		{
			Printf("LIGHTOVR light %s map=%s type=%s anchor=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				rule->lightType.GetChars(),
				AnchorTypeName(rule->anchorType),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasAnchorPosition) Printf("  anchor_position=(%.3f, %.3f, %.3f)\n", rule->anchorPosition[0], rule->anchorPosition[1], rule->anchorPosition[2]);
			if (rule->anchorType == LightOverlayAnchorType::Sector || rule->anchorType == LightOverlayAnchorType::Wall) Printf("  anchor_index=%d\n", rule->anchorIndex);
			if (rule->hasOffset) Printf("  offset=(%.3f, %.3f, %.3f)\n", rule->offset[0], rule->offset[1], rule->offset[2]);
			if (rule->hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule->direction[0], rule->direction[1], rule->direction[2]);
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasRadius) Printf("  radius=%.3f\n", rule->radius);
			if (rule->hasRange) Printf("  range=%.3f\n", rule->range);
			if (rule->hasFlicker) Printf("  flicker_frames=%u\n", rule->flickerFrames);
		}

		for (const ParsedLightOverlayEmissiveOverrideRule* rule : SortRulesByOrder(database.emissiveOverrideRules))
		{
			Printf("LIGHTOVR emissiveoverride %s map=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasSectorFilter) Printf("  sector=%d\n", rule->sectorFilter);
			if (rule->hasWallFilter) Printf("  wall=%d\n", rule->wallFilter);
			if (rule->hasTileFilter) Printf("  tile=%d\n", rule->tileFilter);
			if (rule->hasIntensityScale) Printf("  intensityscale=%.3f\n", rule->intensityScale);
			if (rule->hasReachScale) Printf("  reachscale=%.3f\n", rule->reachScale);
			if (rule->hasSectorResponse) Printf("  sectorresponse=%s\n", rule->sectorResponse ? "on" : "off");
			if (rule->hasSignalSector) Printf("  signal_sector=%d\n", rule->signalSector);
			if (rule->hasResponseIntensity) Printf("  responseintensity=%.3f\n", rule->responseIntensity);
			if (rule->hasResponseMin) Printf("  responsemin=%.3f\n", rule->responseMin);
			if (rule->hasResponseMax) Printf("  responsemax=%.3f\n", rule->responseMax);
			if (rule->hasResponseInputMin) Printf("  responseinputmin=%.3f\n", rule->responseInputMin);
			if (rule->hasResponseInputMax) Printf("  responseinputmax=%.3f\n", rule->responseInputMax);
			if (rule->hasResponseIntensityMin) Printf("  responseintensitymin=%.3f\n", rule->responseIntensityMin);
			if (rule->hasResponseIntensityMax) Printf("  responseintensitymax=%.3f\n", rule->responseIntensityMax);
			if (rule->hasResponseReachMin) Printf("  responsereachmin=%.3f\n", rule->responseReachMin);
			if (rule->hasResponseReachMax) Printf("  responsereachmax=%.3f\n", rule->responseReachMax);
			if (rule->hasMaterialResponse) Printf("  materialresponse=%s\n", rule->materialResponse ? "on" : "off");
			if (rule->hasMaterialResponseMin) Printf("  materialresponsemin=%.3f\n", rule->materialResponseMin);
			if (rule->hasMaterialResponseMax) Printf("  materialresponsemax=%.3f\n", rule->materialResponseMax);
		}

		for (const ParsedLightOverlayEmissiveMaterialResponseRule* rule : SortRulesByOrder(database.emissiveMaterialResponseRules))
		{
			Printf("LIGHTOVR emissivematerialresponse %s: tiles=%d ranges=%d textures=%d materialresponse=%s source=%s\n",
				rule->id.GetChars(),
				rule->tileFilters.Size(),
				rule->tileRanges.Size(),
				rule->textureNames.Size(),
				rule->hasMaterialResponse ? (rule->materialResponse ? "on" : "off") : "(default)",
				SourceLocationText(rule->source).GetChars());
			if (rule->hasMaterialResponseMin) Printf("  materialresponsemin=%.3f\n", rule->materialResponseMin);
			if (rule->hasMaterialResponseMax) Printf("  materialresponsemax=%.3f\n", rule->materialResponseMax);
		}

		for (const ParsedLightOverlayActorOverrideRule* rule : SortRulesByOrder(database.actorOverrideRules))
		{
			Printf("LIGHTOVR actoroverride %s map=%s actorclass=%s shadowreceive=%s shadowcast=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				rule->actorClassName.GetChars(),
				rule->hasShadowReceive ? (rule->shadowReceive ? "default" : "off") : "(unset)",
				rule->hasShadowCast ? (rule->shadowCast ? "default" : "off") : "(unset)",
				SourceLocationText(rule->source).GetChars());
		}
	}

	static void DumpResolvedLightOverlaySet(const ResolvedLightOverlaySet& resolved)
	{
		Printf("LIGHTOVR resolved: parsed_generation=%u resolved_generation=%u map=%s current_map=%s actor_rules=%d muzzle_flashes=%d map_lights=%d emissive_overrides=%d emissive_material_responses=%d directional=%d actor_overrides=%d smoke_styles=%d smoke_actor_rules=%d smoke_event_rules=%d map_smoke_emitters=%d\n",
			resolved.parsedGeneration,
			resolved.resolvedGeneration,
			resolved.activeMapName.IsNotEmpty() ? resolved.activeMapName.GetChars() : "(none)",
			resolved.currentMapAvailable ? "yes" : "no",
			resolved.actorRules.Size(),
			resolved.muzzleFlashRules.Size(),
			resolved.mapLightRules.Size(),
			resolved.emissiveOverrideRules.Size(),
			resolved.emissiveMaterialResponseRules.Size(),
			resolved.directionalRules.Size(),
			resolved.actorOverrideRules.Size(),
			resolved.smokeStyles.Size(),
			resolved.smokeActorRules.Size(),
			resolved.smokeEventRules.Size(),
			resolved.mapSmokeEmitterRules.Size());

		for (const auto& rule : resolved.actorRules)
		{
			Printf("LIGHTOVR resolved actorrule %s: actorclass=%s resolved=%s type=%s shadowreceive=%s shadowcast=%s lightshadowcast=%s fullbright=%s activation=%s source=%s\n",
				rule.id.GetChars(),
				rule.actorClassName.GetChars(),
				rule.actorClassResolved ? "yes" : "no",
				rule.lightType.GetChars(),
				rule.hasShadowReceive ? (rule.shadowReceive ? "default" : "off") : "(unset)",
				rule.hasShadowCast ? (rule.shadowCast ? "default" : "off") : "(unset)",
				rule.hasLightShadowCast ? (rule.lightShadowCast ? "default" : "off") : "(unset)",
				rule.hasFullbright ? (rule.fullbright ? "on" : "off") : "(unset)",
				rule.hasActivationPolicy ? ActorActivationPolicyName(rule.activationPolicy) : "(default)",
				SourceLocationText(rule.source).GetChars());
			if (rule.hasTileFilter) Printf("  tile=%d\n", rule.tileFilter);
			if (rule.hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule.color[0], rule.color[1], rule.color[2]);
			if (rule.hasIntensity) Printf("  intensity=%.3f\n", rule.intensity);
			if (rule.hasRadius) Printf("  radius=%.3f\n", rule.radius);
			if (rule.hasRange) Printf("  range=%.3f\n", rule.range);
			if (rule.hasOffset) Printf("  offset=(%.3f, %.3f, %.3f)\n", rule.offset[0], rule.offset[1], rule.offset[2]);
			if (rule.hasNudgeFromSurface) Printf("  nudgefromsurface=%.3f\n", rule.nudgeFromSurfaceDistance);
			if (rule.hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule.direction[0], rule.direction[1], rule.direction[2]);
			if (rule.hasEmissiveStableFrames) Printf("  emissivestableframes=%u\n", rule.emissiveStableFrames);
			if (rule.hasFlicker) Printf("  flicker_frames=%u\n", rule.flickerFrames);
			if (rule.hasRandom) Printf("  random=(%.3f, %.3f)\n", rule.randomIntensityRange[0], rule.randomIntensityRange[1]);
			if (rule.hasLocalSpacePolicy) Printf("  localspace=%s\n", rule.localSpacePolicy.GetChars());
		}

		for (const auto& rule : resolved.muzzleFlashRules)
		{
			Printf("LIGHTOVR resolved muzzleflashrule %s source=%s\n",
				rule.id.GetChars(),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.smokeStyles)
		{
			Printf("LIGHTOVR resolved smokestyle %s: style_index=%u source=%s\n",
				rule.id.GetChars(), rule.styleIndex, SourceLocationText(rule.source).GetChars());
		}
		for (const auto& rule : resolved.smokeActorRules)
		{
			Printf("LIGHTOVR resolved smokeactorrule %s: actorclass=%s resolved=%s ownerclass=%s owner_resolved=%s "
				"excludeownerclass=%s exclude_owner_resolved=%s trigger=%s activation=%s emitterforeground=%s starttime=%.3f style=%s style_resolved=%s style_index=%u source=%s\n",
				rule.id.GetChars(), rule.actorClassName.GetChars(), rule.actorClassResolved ? "yes" : "no",
				rule.ownerClassName.IsNotEmpty() ? rule.ownerClassName.GetChars() : "none",
				rule.ownerClassName.IsEmpty() ? "n/a" : (rule.ownerClassResolved ? "yes" : "no"),
				rule.excludeOwnerClassName.IsNotEmpty() ? rule.excludeOwnerClassName.GetChars() : "none",
				rule.excludeOwnerClassName.IsEmpty() ? "n/a" : (rule.excludeOwnerClassResolved ? "yes" : "no"),
				SmokeTriggerName(rule.trigger), ActorActivationPolicyName(rule.activationPolicy), rule.emitterForeground ? "on" : "off", rule.startTime, rule.styleId.GetChars(), rule.styleResolved ? "yes" : "no", rule.styleIndex,
				SourceLocationText(rule.source).GetChars());
			Printf("  smoke_policy representation=%s queuepolicy=%s maxlatency=%s analyticcarriers=%u pulseamount=%.3f pulseperiodcadences=%u pulsephase=%.3f\n",
				SmokeRepresentationName(rule.representation), SmokeQueuePolicyName(rule.queuePolicy),
				rule.hasMaxLatencySeconds ? FormatLightOverlayFloat(rule.maxLatencySeconds).GetChars() : "none", rule.analyticCarrierCount,
				rule.pulseAmount, rule.pulsePeriodCadences, rule.pulsePhase);
		}
		for (const auto& rule : resolved.smokeEventRules)
		{
			Printf("LIGHTOVR resolved smokeeventrule %s: style=%s style_resolved=%s style_index=%u representation=%s queuepolicy=%s maxlatency=%s analyticcarriers=%u "
				"offsetrandom=(%.3f,%.3f,%.3f) velocityscale=%.3f normaloffset=%.3f direction=%s source=%s\n",
				rule.id.GetChars(), rule.styleId.GetChars(), rule.styleResolved ? "yes" : "no", rule.styleIndex,
				SmokeRepresentationName(rule.representation), SmokeQueuePolicyName(rule.queuePolicy),
				rule.hasMaxLatencySeconds ? FormatLightOverlayFloat(rule.maxLatencySeconds).GetChars() : "none", rule.analyticCarrierCount,
				rule.offsetRandom[0], rule.offsetRandom[1], rule.offsetRandom[2], rule.velocityScale, rule.normalOffset, SmokeDirectionPolicyName(rule.directionPolicy),
				SourceLocationText(rule.source).GetChars());
		}
		for (const auto& rule : resolved.mapSmokeEmitterRules)
		{
			Printf("LIGHTOVR resolved smokeemitter %s: map=%s style=%s style_resolved=%s style_index=%u "
				"position_set=%s normal_set=%s size_set=%s size=(%.3f,%.3f) source=%s\n",
				rule.id.GetChars(), rule.mapName.GetChars(), rule.styleId.GetChars(),
				rule.styleResolved ? "yes" : "no", rule.styleIndex,
				rule.hasPosition ? "yes" : "no", rule.hasNormal ? "yes" : "no", rule.hasSize ? "yes" : "no",
				rule.size[0], rule.size[1], SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.directionalRules)
		{
			Printf("LIGHTOVR resolved directional %s map=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.mapLightRules)
		{
			Printf("LIGHTOVR resolved light %s map=%s type=%s anchor=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				rule.lightType.GetChars(),
				AnchorTypeName(rule.anchorType),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.emissiveOverrideRules)
		{
			Printf("LIGHTOVR resolved emissiveoverride %s map=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.emissiveMaterialResponseRules)
		{
			Printf("LIGHTOVR resolved emissivematerialresponse %s: tiles=%d ranges=%d textures=%d source=%s\n",
				rule.id.GetChars(),
				rule.tileFilters.Size(),
				rule.tileRanges.Size(),
				rule.textureNames.Size(),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.actorOverrideRules)
		{
			Printf("LIGHTOVR resolved actoroverride %s map=%s actorclass=%s resolved=%s shadowreceive=%s shadowcast=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				rule.actorClassName.GetChars(),
				rule.actorClassResolved ? "yes" : "no",
				rule.hasShadowReceive ? (rule.shadowReceive ? "default" : "off") : "(unset)",
				rule.hasShadowCast ? (rule.shadowCast ? "default" : "off") : "(unset)",
				SourceLocationText(rule.source).GetChars());
		}
	}

	static void CopyActorRule(const ParsedLightOverlayActorRule& source, ResolvedLightOverlayActorRule& destination)
	{
		destination.id = source.id;
		destination.source = source.source;
		destination.actorClassName = source.actorClassName;
		destination.actorClass = PClass::FindActor(source.actorClassName);
		destination.actorClassResolved = destination.actorClass != nullptr;
		destination.hasShadowReceive = source.hasShadowReceive;
		destination.shadowReceive = source.shadowReceive;
		destination.hasShadowCast = source.hasShadowCast;
		destination.shadowCast = source.shadowCast;
		destination.hasLightShadowCast = source.hasLightShadowCast;
		destination.lightShadowCast = source.lightShadowCast;
		destination.hasFullbright = source.hasFullbright;
		destination.fullbright = source.fullbright;
		destination.hasEmissiveStableFrames = source.hasEmissiveStableFrames;
		destination.emissiveStableFrames = source.emissiveStableFrames;
		destination.hasActivationPolicy = source.hasActivationPolicy;
		destination.activationPolicy = source.activationPolicy;
		destination.hasTileFilter = source.hasTileFilter;
		destination.tileFilter = source.tileFilter;
		destination.lightType = source.lightType;
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasRange = source.hasRange;
		destination.range = source.range;
		destination.hasOffset = source.hasOffset;
		CopyVector3(source.offset, destination.offset);
		destination.hasNudgeFromSurface = source.hasNudgeFromSurface;
		destination.nudgeFromSurfaceDistance = source.nudgeFromSurfaceDistance;
		destination.hasDirection = source.hasDirection;
		CopyVector3(source.direction, destination.direction);
		destination.hasFlicker = source.hasFlicker;
		destination.flickerFrames = source.flickerFrames;
		destination.hasRandom = source.hasRandom;
		destination.randomIntensityRange[0] = source.randomIntensityRange[0];
		destination.randomIntensityRange[1] = source.randomIntensityRange[1];
		destination.hasLocalSpacePolicy = source.hasLocalSpacePolicy;
		destination.localSpacePolicy = source.localSpacePolicy;
	}

	static void CopyDirectionalRule(const ParsedLightOverlayDirectionalRule& source, ResolvedLightOverlayDirectionalRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasDirection = source.hasDirection;
		CopyVector3(source.direction, destination.direction);
		destination.hasAngularSize = source.hasAngularSize;
		destination.angularSize = source.angularSize;
		destination.hasShadow = source.hasShadow;
		destination.shadow = source.shadow;
	}

	static void CopyMuzzleFlashRule(const ParsedLightOverlayMuzzleFlashRule& source, ResolvedLightOverlayMuzzleFlashRule& destination)
	{
		destination.id = source.id;
		destination.source = source.source;
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasIntensityRandom = source.hasIntensityRandom;
		destination.intensityRandomRange[0] = source.intensityRandomRange[0];
		destination.intensityRandomRange[1] = source.intensityRandomRange[1];
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasRadiusRandom = source.hasRadiusRandom;
		destination.radiusRandomRange[0] = source.radiusRandomRange[0];
		destination.radiusRandomRange[1] = source.radiusRandomRange[1];
		destination.hasDelaySeconds = source.hasDelaySeconds;
		destination.delaySeconds = source.delaySeconds;
		destination.hasDelayRandomSeconds = source.hasDelayRandomSeconds;
		destination.delayRandomSecondsRange[0] = source.delayRandomSecondsRange[0];
		destination.delayRandomSecondsRange[1] = source.delayRandomSecondsRange[1];
		destination.hasDurationSeconds = source.hasDurationSeconds;
		destination.durationSeconds = source.durationSeconds;
		destination.hasDurationRandomSeconds = source.hasDurationRandomSeconds;
		destination.durationRandomSecondsRange[0] = source.durationRandomSecondsRange[0];
		destination.durationRandomSecondsRange[1] = source.durationRandomSecondsRange[1];
		destination.hasOffset = source.hasOffset;
		CopyVector3(source.offset, destination.offset);
	}

	static void CopyMapLightRule(const ParsedLightOverlayMapLightRule& source, ResolvedLightOverlayMapLightRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.lightType = source.lightType;
		destination.anchorType = source.anchorType;
		destination.hasAnchorPosition = source.hasAnchorPosition;
		CopyVector3(source.anchorPosition, destination.anchorPosition);
		destination.anchorIndex = source.anchorIndex;
		destination.hasOffset = source.hasOffset;
		CopyVector3(source.offset, destination.offset);
		destination.hasDirection = source.hasDirection;
		CopyVector3(source.direction, destination.direction);
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasRange = source.hasRange;
		destination.range = source.range;
		destination.hasFlicker = source.hasFlicker;
		destination.flickerFrames = source.flickerFrames;
	}

	static void CopyEmissiveOverrideRule(const ParsedLightOverlayEmissiveOverrideRule& source, ResolvedLightOverlayEmissiveOverrideRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.hasSectorFilter = source.hasSectorFilter;
		destination.sectorFilter = source.sectorFilter;
		destination.hasWallFilter = source.hasWallFilter;
		destination.wallFilter = source.wallFilter;
		destination.hasTileFilter = source.hasTileFilter;
		destination.tileFilter = source.tileFilter;
		destination.hasIntensityScale = source.hasIntensityScale;
		destination.intensityScale = source.intensityScale;
		destination.hasReachScale = source.hasReachScale;
		destination.reachScale = source.reachScale;
		destination.hasSectorResponse = source.hasSectorResponse;
		destination.sectorResponse = source.sectorResponse;
		destination.hasSignalSector = source.hasSignalSector;
		destination.signalSector = source.signalSector;
		destination.hasResponseIntensity = source.hasResponseIntensity;
		destination.responseIntensity = source.responseIntensity;
		destination.hasResponseMin = source.hasResponseMin;
		destination.responseMin = source.responseMin;
		destination.hasResponseMax = source.hasResponseMax;
		destination.responseMax = source.responseMax;
		destination.hasResponseInputMin = source.hasResponseInputMin;
		destination.responseInputMin = source.responseInputMin;
		destination.hasResponseInputMax = source.hasResponseInputMax;
		destination.responseInputMax = source.responseInputMax;
		destination.hasResponseIntensityMin = source.hasResponseIntensityMin;
		destination.responseIntensityMin = source.responseIntensityMin;
		destination.hasResponseIntensityMax = source.hasResponseIntensityMax;
		destination.responseIntensityMax = source.responseIntensityMax;
		destination.hasResponseReachMin = source.hasResponseReachMin;
		destination.responseReachMin = source.responseReachMin;
		destination.hasResponseReachMax = source.hasResponseReachMax;
		destination.responseReachMax = source.responseReachMax;
		destination.hasMaterialResponse = source.hasMaterialResponse;
		destination.materialResponse = source.materialResponse;
		destination.hasMaterialResponseMin = source.hasMaterialResponseMin;
		destination.materialResponseMin = source.materialResponseMin;
		destination.hasMaterialResponseMax = source.hasMaterialResponseMax;
		destination.materialResponseMax = source.materialResponseMax;
	}

	static void CopyEmissiveMaterialResponseRule(const ParsedLightOverlayEmissiveMaterialResponseRule& source, ResolvedLightOverlayEmissiveMaterialResponseRule& destination)
	{
		destination.id = source.id;
		destination.source = source.source;
		destination.tileFilters = source.tileFilters;
		destination.tileRanges = source.tileRanges;
		destination.textureNames = source.textureNames;
		destination.hasMaterialResponse = source.hasMaterialResponse;
		destination.materialResponse = source.materialResponse;
		destination.hasMaterialResponseMin = source.hasMaterialResponseMin;
		destination.materialResponseMin = source.materialResponseMin;
		destination.hasMaterialResponseMax = source.hasMaterialResponseMax;
		destination.materialResponseMax = source.materialResponseMax;
		destination.hasVisibleGlowBlend = source.hasVisibleGlowBlend;
		destination.visibleGlowBlend = source.visibleGlowBlend;
	}

	static void CopySurfaceLightRule(const ParsedLightOverlaySurfaceLightRule& source, ResolvedLightOverlaySurfaceLightRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.hasPosition = source.hasPosition;
		CopyVector3(source.position, destination.position);
		destination.hasNormal = source.hasNormal;
		CopyVector3(source.normal, destination.normal);
		destination.hasSize = source.hasSize;
		destination.size[0] = source.size[0];
		destination.size[1] = source.size[1];
		destination.hasRotation = source.hasRotation;
		destination.rotation = source.rotation;
		destination.hasOffset = source.hasOffset;
		destination.offset = source.offset;
		destination.hasSector = source.hasSector;
		destination.sector = source.sector;
		destination.hasWall = source.hasWall;
		destination.wall = source.wall;
		destination.hasTile = source.hasTile;
		destination.tile = source.tile;
		destination.hasFixtureTexture = source.hasFixtureTexture;
		destination.fixtureTexture = source.fixtureTexture;
		destination.hasFixtureMaterialResponse = source.hasFixtureMaterialResponse;
		destination.fixtureMaterialResponse = source.fixtureMaterialResponse;
		destination.lightType = source.lightType;
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasSectorResponse = source.hasSectorResponse;
		destination.sectorResponse = source.sectorResponse;
		destination.hasSignalSector = source.hasSignalSector;
		destination.signalSector = source.signalSector;
		destination.hasResponseIntensity = source.hasResponseIntensity;
		destination.responseIntensity = source.responseIntensity;
		destination.hasResponseMin = source.hasResponseMin;
		destination.responseMin = source.responseMin;
		destination.hasResponseMax = source.hasResponseMax;
		destination.responseMax = source.responseMax;
		destination.hasResponseInputMin = source.hasResponseInputMin;
		destination.responseInputMin = source.responseInputMin;
		destination.hasResponseInputMax = source.hasResponseInputMax;
		destination.responseInputMax = source.responseInputMax;
		destination.hasMaterialResponseMin = source.hasMaterialResponseMin;
		destination.materialResponseMin = source.materialResponseMin;
		destination.hasMaterialResponseMax = source.hasMaterialResponseMax;
		destination.materialResponseMax = source.materialResponseMax;
	}

	static void CopyActorOverrideRule(const ParsedLightOverlayActorOverrideRule& source, ResolvedLightOverlayActorOverrideRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.actorClassName = source.actorClassName;
		destination.actorClass = PClass::FindActor(source.actorClassName);
		destination.actorClassResolved = destination.actorClass != nullptr;
		destination.hasShadowReceive = source.hasShadowReceive;
		destination.shadowReceive = source.shadowReceive;
		destination.hasShadowCast = source.hasShadowCast;
		destination.shadowCast = source.shadowCast;
	}

	static const ResolvedLightOverlaySet& ResolveLightOverlaysForMapInternal(const FString& mapName, bool currentMapAvailable)
	{
		const bool sameGeneration = GResolvedLightOverlaySet.parsedGeneration == GLightOverlayDatabase.generation;
		const bool sameMap = GResolvedLightOverlaySet.activeMapName.CompareNoCase(mapName) == 0;
		if (sameGeneration && sameMap && GResolvedLightOverlaySet.currentMapAvailable == currentMapAvailable)
		{
			return GResolvedLightOverlaySet;
		}

		ResolvedLightOverlaySet resolved;
		resolved.parsedGeneration = GLightOverlayDatabase.generation;
		resolved.resolvedGeneration = ++GResolvedLightOverlayGeneration;
		resolved.activeMapName = mapName;
		resolved.currentMapAvailable = currentMapAvailable;

		std::unordered_map<std::string, uint32_t> smokeStyleLookup;
		for (const auto* source : SortRulesById(GLightOverlayDatabase.smokeStyles))
		{
			ResolvedLightOverlaySmokeStyle destination;
			static_cast<ParsedLightOverlaySmokeStyle&>(destination) = *source;
			destination.styleIndex = (uint32_t)resolved.smokeStyles.Size();
			smokeStyleLookup[MakeNormalizedKey(source->id)] = destination.styleIndex;
			resolved.smokeStyles.Push(std::move(destination));
		}

		for (const auto& source : GLightOverlayDatabase.smokeActorRules)
		{
			ResolvedLightOverlaySmokeActorRule destination;
			static_cast<ParsedLightOverlaySmokeActorRule&>(destination) = source;
			destination.actorClass = PClass::FindActor(source.actorClassName);
			destination.actorClassResolved = destination.actorClass != nullptr;
			if (source.ownerClassName.IsNotEmpty())
			{
				destination.ownerClass = PClass::FindActor(source.ownerClassName);
				destination.ownerClassResolved = destination.ownerClass != nullptr;
			}
			if (source.excludeOwnerClassName.IsNotEmpty())
			{
				destination.excludeOwnerClass = PClass::FindActor(source.excludeOwnerClassName);
				destination.excludeOwnerClassResolved = destination.excludeOwnerClass != nullptr;
			}
			const auto style = smokeStyleLookup.find(MakeNormalizedKey(source.styleId));
			destination.styleResolved = style != smokeStyleLookup.end();
			if (destination.styleResolved) destination.styleIndex = style->second;
			resolved.smokeActorRules.Push(std::move(destination));
		}

		for (const auto& source : GLightOverlayDatabase.smokeEventRules)
		{
			ResolvedLightOverlaySmokeEventRule destination;
			static_cast<ParsedLightOverlaySmokeEventRule&>(destination) = source;
			const auto style = smokeStyleLookup.find(MakeNormalizedKey(source.styleId));
			destination.styleResolved = style != smokeStyleLookup.end();
			if (destination.styleResolved) destination.styleIndex = style->second;
			resolved.smokeEventRules.Push(std::move(destination));
		}

		for (const auto& source : GLightOverlayDatabase.mapSmokeEmitterRules)
		{
			if (mapName.IsEmpty() || source.mapName.CompareNoCase(mapName) != 0)
			{
				continue;
			}
			ResolvedLightOverlayMapSmokeEmitterRule destination;
			static_cast<ParsedLightOverlayMapSmokeEmitterRule&>(destination) = source;
			const auto style = smokeStyleLookup.find(MakeNormalizedKey(source.styleId));
			destination.styleResolved = style != smokeStyleLookup.end();
			if (destination.styleResolved) destination.styleIndex = style->second;
			resolved.mapSmokeEmitterRules.Push(std::move(destination));
		}

		for (const auto& source : GLightOverlayDatabase.actorRules)
		{
			ResolvedLightOverlayActorRule destination;
			CopyActorRule(source, destination);
			resolved.actorRules.Push(destination);
		}

		for (const auto& source : GLightOverlayDatabase.muzzleFlashRules)
		{
			ResolvedLightOverlayMuzzleFlashRule destination;
			CopyMuzzleFlashRule(source, destination);
			resolved.muzzleFlashRules.Push(destination);
		}

		for (const auto& source : GLightOverlayDatabase.directionalRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayDirectionalRule destination;
				CopyDirectionalRule(source, destination);
				resolved.directionalRules.Push(destination);
			}
		}

		for (const auto& source : GLightOverlayDatabase.mapLightRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayMapLightRule destination;
				CopyMapLightRule(source, destination);
				resolved.mapLightRules.Push(destination);
			}
		}

		for (const auto& source : GLightOverlayDatabase.emissiveOverrideRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayEmissiveOverrideRule destination;
				CopyEmissiveOverrideRule(source, destination);
				resolved.emissiveOverrideRules.Push(destination);
			}
		}

		for (const auto& source : GLightOverlayDatabase.emissiveMaterialResponseRules)
		{
			ResolvedLightOverlayEmissiveMaterialResponseRule destination;
			CopyEmissiveMaterialResponseRule(source, destination);
			resolved.emissiveMaterialResponseRules.Push(destination);
		}

		for (const auto& source : GLightOverlayDatabase.surfaceLightRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlaySurfaceLightRule destination;
				CopySurfaceLightRule(source, destination);
				resolved.surfaceLightRules.Push(destination);
			}
		}

		for (const auto& source : GLightOverlayDatabase.actorOverrideRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayActorOverrideRule destination;
				CopyActorOverrideRule(source, destination);
				resolved.actorOverrideRules.Push(destination);
			}
		}

		GResolvedLightOverlaySet = std::move(resolved);
		return GResolvedLightOverlaySet;
	}
}

bool BuildLightOverlayMapSmokeEmitterRectangle(
	const ParsedLightOverlayMapSmokeEmitterRule& rule,
	LightOverlayMapSmokeEmitterRectangle& outRectangle)
{
	outRectangle = {};
	if (!rule.hasPosition || !rule.hasNormal || !rule.hasSize ||
		!std::isfinite(rule.rotation) || !std::isfinite(rule.offset) ||
		!std::isfinite(rule.size[0]) || !std::isfinite(rule.size[1]))
	{
		return false;
	}

	DVector3 center(rule.position[0], rule.position[1], rule.position[2]);
	DVector3 normal(rule.normal[0], rule.normal[1], rule.normal[2]);
	if (!std::isfinite(center.X) || !std::isfinite(center.Y) || !std::isfinite(center.Z) ||
		!std::isfinite(normal.X) || !std::isfinite(normal.Y) || !std::isfinite(normal.Z) || normal.isZero())
	{
		return false;
	}
	normal.MakeUnit();

	const DVector3 reference = std::abs(normal.Z) < 0.999 ?
		DVector3(0.0, 0.0, 1.0) : DVector3(0.0, 1.0, 0.0);
	DVector3 baseU = reference ^ normal;
	if (baseU.isZero())
	{
		return false;
	}
	baseU.MakeUnit();
	DVector3 baseV = normal ^ baseU;
	baseV.MakeUnit();

	const double radians = (double)rule.rotation * (3.14159265358979323846 / 180.0);
	const double cosine = std::cos(radians);
	const double sine = std::sin(radians);
	const DVector3 halfAxisU = (baseU * cosine + baseV * sine) * (std::max(0.0f, rule.size[0]) * 0.5);
	const DVector3 halfAxisV = (baseV * cosine - baseU * sine) * (std::max(0.0f, rule.size[1]) * 0.5);
	center += normal * rule.offset;

	const DVector3 values[] = { center, normal, halfAxisU, halfAxisV };
	float* outputs[] = { outRectangle.center, outRectangle.normal, outRectangle.halfAxisU, outRectangle.halfAxisV };
	for (int i = 0; i < 4; ++i)
	{
		outputs[i][0] = (float)values[i].X;
		outputs[i][1] = (float)values[i].Y;
		outputs[i][2] = (float)values[i].Z;
	}
	return true;
}

FString SerializeLightOverlayDatabase(const ParsedLightOverlayDatabase& database)
{
	FString text;
	AppendLine(text, 0, "LIGHTOVR");
	AppendLine(text, 0, "{");

	if (database.defaults.present)
	{
		AppendLine(text, 1, "defaults");
		AppendLine(text, 1, "{");
		AppendLine(text, 1, "}");
		if (database.actorRules.Size() > 0 || database.muzzleFlashRules.Size() > 0 || database.smokeStyles.Size() > 0 || database.smokeActorRules.Size() > 0 || database.smokeEventRules.Size() > 0 || database.emissiveMaterialResponseRules.Size() > 0 || database.directionalRules.Size() > 0 || database.mapLightRules.Size() > 0 || database.emissiveOverrideRules.Size() > 0 || database.surfaceLightRules.Size() > 0 || database.actorOverrideRules.Size() > 0 || database.mapSmokeEmitterRules.Size() > 0)
		{
			text << "\n";
		}
	}

	const auto actorRules = SortRulesById(database.actorRules);
	for (size_t i = 0; i < actorRules.size(); ++i)
	{
		if (i > 0)
		{
			text << "\n";
		}
		AppendActorRuleBlock(text, *actorRules[i]);
	}

	const auto muzzleFlashRules = SortRulesById(database.muzzleFlashRules);
	if (!actorRules.empty() && !muzzleFlashRules.empty())
	{
		text << "\n";
	}
	for (size_t i = 0; i < muzzleFlashRules.size(); ++i)
	{
		if (i > 0)
		{
			text << "\n";
		}
		AppendMuzzleFlashRuleBlock(text, *muzzleFlashRules[i]);
	}

	const auto smokeStyles = SortRulesById(database.smokeStyles);
	const auto smokeActorRules = SortRulesById(database.smokeActorRules);
	const auto smokeEventRules = SortRulesById(database.smokeEventRules);
	auto appendSmokeFamily = [&text](const auto& rules, auto appendRule, bool& hasPrior)
	{
		if (hasPrior && !rules.empty()) text << "\n";
		for (size_t i = 0; i < rules.size(); ++i)
		{
			if (i > 0) text << "\n";
			appendRule(text, *rules[i]);
		}
		if (!rules.empty()) hasPrior = true;
	};
	bool hasGlobalRules = !actorRules.empty() || !muzzleFlashRules.empty();
	appendSmokeFamily(smokeStyles, AppendSmokeStyleBlock, hasGlobalRules);
	appendSmokeFamily(smokeActorRules, AppendSmokeActorRuleBlock, hasGlobalRules);
	appendSmokeFamily(smokeEventRules, AppendSmokeEventRuleBlock, hasGlobalRules);

	const auto emissiveMaterialResponseRules = SortRulesById(database.emissiveMaterialResponseRules);
	if (hasGlobalRules && !emissiveMaterialResponseRules.empty())
	{
		text << "\n";
	}
	for (size_t i = 0; i < emissiveMaterialResponseRules.size(); ++i)
	{
		if (i > 0)
		{
			text << "\n";
		}
		AppendEmissiveMaterialResponseRuleBlock(text, *emissiveMaterialResponseRules[i]);
	}

	const auto mapNames = CollectSortedMapNames(database);
	if ((hasGlobalRules || !emissiveMaterialResponseRules.empty()) && !mapNames.empty())
	{
		text << "\n";
	}

	for (size_t mapIndex = 0; mapIndex < mapNames.size(); ++mapIndex)
	{
		const FString& mapName = mapNames[mapIndex];
		AppendLine(text, 1, FStringf("map %s", QuoteLightOverlayString(mapName).GetChars()));
		AppendLine(text, 1, "{");

		std::vector<const ParsedLightOverlayDirectionalRule*> directionalRules;
		for (const auto& rule : database.directionalRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				directionalRules.push_back(&rule);
			}
		}
		std::sort(directionalRules.begin(), directionalRules.end(), [](const auto* left, const auto* right)
		{
			const int idCompare = left->id.CompareNoCase(right->id);
			return idCompare != 0 ? idCompare < 0 : left->source.orderIndex < right->source.orderIndex;
		});
		for (const auto* rule : directionalRules)
		{
			AppendDirectionalRuleBlock(text, *rule);
		}

		std::vector<const ParsedLightOverlayMapLightRule*> mapLightRules;
		for (const auto& rule : database.mapLightRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				mapLightRules.push_back(&rule);
			}
		}
		std::sort(mapLightRules.begin(), mapLightRules.end(), [](const auto* left, const auto* right)
		{
			const int idCompare = left->id.CompareNoCase(right->id);
			return idCompare != 0 ? idCompare < 0 : left->source.orderIndex < right->source.orderIndex;
		});
		for (const auto* rule : mapLightRules)
		{
			AppendMapLightRuleBlock(text, *rule);
		}

		std::vector<const ParsedLightOverlayMapSmokeEmitterRule*> mapSmokeEmitters;
		for (const auto& rule : database.mapSmokeEmitterRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				mapSmokeEmitters.push_back(&rule);
			}
		}
		std::sort(mapSmokeEmitters.begin(), mapSmokeEmitters.end(), [](const auto* left, const auto* right)
		{
			const int idCompare = left->id.CompareNoCase(right->id);
			return idCompare != 0 ? idCompare < 0 : left->source.orderIndex < right->source.orderIndex;
		});
		for (const auto* rule : mapSmokeEmitters)
		{
			AppendMapSmokeEmitterRuleBlock(text, *rule);
		}

		std::vector<const ParsedLightOverlayEmissiveOverrideRule*> emissiveOverrides;
		for (const auto& rule : database.emissiveOverrideRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				emissiveOverrides.push_back(&rule);
			}
		}
		std::sort(emissiveOverrides.begin(), emissiveOverrides.end(), [](const auto* left, const auto* right)
		{
			const int idCompare = left->id.CompareNoCase(right->id);
			return idCompare != 0 ? idCompare < 0 : left->source.orderIndex < right->source.orderIndex;
		});
		for (const auto* rule : emissiveOverrides)
		{
			AppendEmissiveOverrideRuleBlock(text, *rule);
		}

		std::vector<const ParsedLightOverlaySurfaceLightRule*> surfaceLights;
		for (const auto& rule : database.surfaceLightRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				surfaceLights.push_back(&rule);
			}
		}
		for (const auto* rule : surfaceLights)
		{
			AppendSurfaceLightRuleBlock(text, *rule);
		}

		std::vector<const ParsedLightOverlayActorOverrideRule*> actorOverrides;
		for (const auto& rule : database.actorOverrideRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				actorOverrides.push_back(&rule);
			}
		}
		std::sort(actorOverrides.begin(), actorOverrides.end(), [](const auto* left, const auto* right)
		{
			const int idCompare = left->id.CompareNoCase(right->id);
			return idCompare != 0 ? idCompare < 0 : left->source.orderIndex < right->source.orderIndex;
		});
		for (const auto* rule : actorOverrides)
		{
			AppendActorOverrideRuleBlock(text, *rule);
		}

		AppendLine(text, 1, "}");
		if (mapIndex + 1 < mapNames.size())
		{
			text << "\n";
		}
	}

	AppendLine(text, 0, "}");
	return text;
}

bool ApplyParsedLightOverlayDatabase(const ParsedLightOverlayDatabase& database, bool verbose)
{
	ParsedLightOverlayDatabase nextDatabase = database;
	nextDatabase.contentHash = ComputeLightOverlayTextHash(SerializeLightOverlayDatabase(nextDatabase));

	const bool contentChanged = nextDatabase.contentHash != GLightOverlayDatabase.contentHash;
	nextDatabase.generation = contentChanged ? ++GLightOverlayGeneration : GLightOverlayDatabase.generation;
	GLightOverlayDatabase = std::move(nextDatabase);

	if (verbose)
	{
		Printf("LIGHTOVR: parsed generation=%u files=%d actor_rules=%d muzzle_flashes=%d map_lights=%d emissive_overrides=%d emissive_material_responses=%d surface_lights=%d directional=%d actor_overrides=%d smoke_styles=%d smoke_actor_rules=%d smoke_event_rules=%d map_smoke_emitters=%d parse_errors=%s changed=%s\n",
			GLightOverlayDatabase.generation,
			GLightOverlayDatabase.sourceFiles.Size(),
			GLightOverlayDatabase.actorRules.Size(),
			GLightOverlayDatabase.muzzleFlashRules.Size(),
			GLightOverlayDatabase.mapLightRules.Size(),
			GLightOverlayDatabase.emissiveOverrideRules.Size(),
			GLightOverlayDatabase.emissiveMaterialResponseRules.Size(),
			GLightOverlayDatabase.surfaceLightRules.Size(),
			GLightOverlayDatabase.directionalRules.Size(),
			GLightOverlayDatabase.actorOverrideRules.Size(),
			GLightOverlayDatabase.smokeStyles.Size(),
			GLightOverlayDatabase.smokeActorRules.Size(),
			GLightOverlayDatabase.smokeEventRules.Size(),
			GLightOverlayDatabase.mapSmokeEmitterRules.Size(),
			GLightOverlayDatabase.hadParseErrors ? "yes" : "no",
			contentChanged ? "yes" : "no");
	}

	ResolveLightOverlaysForMapInternal(GetCurrentResolvedMapName(), currentLevel != nullptr);
	return !GLightOverlayDatabase.hadParseErrors;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayActorRule& rule, bool* outReplaced)
{
	ParsedLightOverlayActorRule nextRule = rule;
	const int32_t index = FindActorRuleIndex(database, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.actorRules[index].source : nullptr);
	if (index >= 0)
	{
		database.actorRules[index] = std::move(nextRule);
	}
	else
	{
		database.actorRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayDirectionalRule& rule, bool* outReplaced)
{
	ParsedLightOverlayDirectionalRule nextRule = rule;
	const int32_t index = FindDirectionalRuleIndex(database, nextRule.mapName, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.directionalRules[index].source : nullptr);
	if (index >= 0)
	{
		database.directionalRules[index] = std::move(nextRule);
	}
	else
	{
		database.directionalRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMuzzleFlashRule& rule, bool* outReplaced)
{
	ParsedLightOverlayMuzzleFlashRule nextRule = rule;
	const int32_t index = FindMuzzleFlashRuleIndex(database, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.muzzleFlashRules[index].source : nullptr);
	if (index >= 0)
	{
		database.muzzleFlashRules[index] = std::move(nextRule);
	}
	else
	{
		database.muzzleFlashRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMapLightRule& rule, bool* outReplaced)
{
	ParsedLightOverlayMapLightRule nextRule = rule;
	const int32_t index = FindMapLightRuleIndex(database, nextRule.mapName, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.mapLightRules[index].source : nullptr);
	if (index >= 0)
	{
		database.mapLightRules[index] = std::move(nextRule);
	}
	else
	{
		database.mapLightRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayEmissiveOverrideRule& rule, bool* outReplaced)
{
	ParsedLightOverlayEmissiveOverrideRule nextRule = rule;
	const int32_t index = FindEmissiveOverrideRuleIndex(database, nextRule.mapName, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.emissiveOverrideRules[index].source : nullptr);
	if (index >= 0)
	{
		database.emissiveOverrideRules[index] = std::move(nextRule);
	}
	else
	{
		database.emissiveOverrideRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayEmissiveMaterialResponseRule& rule, bool* outReplaced)
{
	ParsedLightOverlayEmissiveMaterialResponseRule nextRule = rule;
	const int32_t index = FindEmissiveMaterialResponseRuleIndex(database, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.emissiveMaterialResponseRules[index].source : nullptr);
	if (index >= 0)
	{
		database.emissiveMaterialResponseRules[index] = std::move(nextRule);
	}
	else
	{
		database.emissiveMaterialResponseRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySurfaceLightRule& rule, bool* outReplaced)
{
	ParsedLightOverlaySurfaceLightRule nextRule = rule;
	const int32_t index = FindSurfaceLightRuleIndex(database, nextRule.mapName, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.surfaceLightRules[index].source : nullptr);
	if (index >= 0)
	{
		database.surfaceLightRules[index] = std::move(nextRule);
	}
	else
	{
		database.surfaceLightRules.Push(std::move(nextRule));
	}
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayActorOverrideRule& rule, bool* outReplaced)
{
	ParsedLightOverlayActorOverrideRule nextRule = rule;
	const int32_t index = FindActorOverrideRuleIndex(database, nextRule.mapName, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.actorOverrideRules[index].source : nullptr);
	if (index >= 0)
	{
		database.actorOverrideRules[index] = std::move(nextRule);
	}
	else
	{
		database.actorOverrideRules.Push(std::move(nextRule));
	}
	return true;
}

template<class T>
static bool AddOrReplaceGlobalLightOverlayRule(ParsedLightOverlayDatabase& database, TArray<T>& rules, const T& rule, bool* outReplaced)
{
	T nextRule = rule;
	const int32_t index = FindGlobalRuleIndex(rules, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &rules[index].source : nullptr);
	if (index >= 0) rules[index] = std::move(nextRule);
	else rules.Push(std::move(nextRule));
	return true;
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySmokeStyle& rule, bool* outReplaced)
{
	return AddOrReplaceGlobalLightOverlayRule(database, database.smokeStyles, rule, outReplaced);
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySmokeActorRule& rule, bool* outReplaced)
{
	return AddOrReplaceGlobalLightOverlayRule(database, database.smokeActorRules, rule, outReplaced);
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlaySmokeEventRule& rule, bool* outReplaced)
{
	return AddOrReplaceGlobalLightOverlayRule(database, database.smokeEventRules, rule, outReplaced);
}

bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMapSmokeEmitterRule& rule, bool* outReplaced)
{
	ParsedLightOverlayMapSmokeEmitterRule nextRule = rule;
	const int32_t index = FindMapSmokeEmitterRuleIndex(database, nextRule.mapName, nextRule.id);
	if (outReplaced != nullptr) *outReplaced = index >= 0;
	EnsureEditableSource(database, nextRule.source, index >= 0 ? &database.mapSmokeEmitterRules[index].source : nullptr);
	if (index >= 0) database.mapSmokeEmitterRules[index] = std::move(nextRule);
	else database.mapSmokeEmitterRules.Push(std::move(nextRule));
	return true;
}

bool RemoveLightOverlayRule(ParsedLightOverlayDatabase& database, LightOverlayRuleKind kind, const char* id, const char* mapName)
{
	const FString ruleId = id != nullptr ? FString(id) : FString("");
	const FString scopedMapName = mapName != nullptr ? FString(mapName) : FString("");
	int32_t index = -1;
	switch (kind)
	{
	case LightOverlayRuleKind::ActorRule:
		index = FindActorRuleIndex(database, ruleId);
		if (index >= 0) database.actorRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::Directional:
		index = FindDirectionalRuleIndex(database, scopedMapName, ruleId);
		if (index >= 0) database.directionalRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::MuzzleFlash:
		index = FindMuzzleFlashRuleIndex(database, ruleId);
		if (index >= 0) database.muzzleFlashRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::MapLight:
		index = FindMapLightRuleIndex(database, scopedMapName, ruleId);
		if (index >= 0) database.mapLightRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::EmissiveOverride:
		index = FindEmissiveOverrideRuleIndex(database, scopedMapName, ruleId);
		if (index >= 0) database.emissiveOverrideRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::EmissiveMaterialResponse:
		index = FindEmissiveMaterialResponseRuleIndex(database, ruleId);
		if (index >= 0) database.emissiveMaterialResponseRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::SurfaceLight:
		index = FindSurfaceLightRuleIndex(database, scopedMapName, ruleId);
		if (index >= 0) database.surfaceLightRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::ActorOverride:
		index = FindActorOverrideRuleIndex(database, scopedMapName, ruleId);
		if (index >= 0) database.actorOverrideRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::SmokeStyle:
		index = FindGlobalRuleIndex(database.smokeStyles, ruleId);
		if (index >= 0) database.smokeStyles.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::SmokeActorRule:
		index = FindGlobalRuleIndex(database.smokeActorRules, ruleId);
		if (index >= 0) database.smokeActorRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::SmokeEventRule:
		index = FindGlobalRuleIndex(database.smokeEventRules, ruleId);
		if (index >= 0) database.smokeEventRules.Delete(index);
		return index >= 0;
	case LightOverlayRuleKind::MapSmokeEmitter:
		index = FindMapSmokeEmitterRuleIndex(database, scopedMapName, ruleId);
		if (index >= 0) database.mapSmokeEmitterRules.Delete(index);
		return index >= 0;
	default:
		return false;
	}
}

const ParsedLightOverlayDatabase& GetParsedLightOverlayDatabase()
{
	return GLightOverlayDatabase;
}

const ResolvedLightOverlaySet& GetResolvedLightOverlaySet()
{
	return ResolveLightOverlaysForMapInternal(GetCurrentResolvedMapName(), currentLevel != nullptr);
}

const ResolvedLightOverlaySet& ResolveLightOverlaysForMap(const char* mapName)
{
	const FString requestedMapName = CanonicalizeMapName(mapName);
	return ResolveLightOverlaysForMapInternal(requestedMapName, requestedMapName.IsNotEmpty());
}

bool ParseLightOverlays(bool verbose)
{
	ParsedLightOverlayDatabaseBuilder builder;

	int workingLump = -1;
	int lastLump = 0;
	static const char* lightOverlayNames[] = { "LIGHTOVR", nullptr };
	while ((workingLump = fileSystem.FindLumpMulti(lightOverlayNames, &lastLump)) != -1)
	{
		fileSystem.RefreshFile(workingLump);

		ParsedLightOverlaySourceFile sourceFile;
		sourceFile.lumpNum = workingLump;
		sourceFile.sourceName = GetLumpDisplayName(workingLump);
		builder.database.sourceFiles.Push(sourceFile);
		auto& storedSourceFile = builder.database.sourceFiles.Last();

		LightOverlayParser parser(workingLump, builder, storedSourceFile);
		parser.Parse();
		builder.database.hadParseErrors = builder.database.hadParseErrors || storedSourceFile.hadParseErrors;
	}

	return ApplyParsedLightOverlayDatabase(builder.database, verbose);
}

CCMD(lightoverlay_reload)
{
	const bool ok = ParseLightOverlays(true);
	Printf("LIGHTOVR reload %s.\n", ok ? "completed" : "completed with parse errors");
}

CCMD(lightoverlay_dump)
{
	DumpParsedLightOverlayDatabase(GetParsedLightOverlayDatabase());
}

CCMD(lightoverlay_dumpresolved)
{
	if (argv.argc() > 2)
	{
		Printf("usage: lightoverlay_dumpresolved [mapname]\n");
		return;
	}

	const ResolvedLightOverlaySet& resolved = argv.argc() == 2 ?
		ResolveLightOverlaysForMap(argv[1]) :
		GetResolvedLightOverlaySet();
	DumpResolvedLightOverlaySet(resolved);
}

CCMD(lightoverlay_dumpnormalized)
{
	Printf("%s", SerializeLightOverlayDatabase(GetParsedLightOverlayDatabase()).GetChars());
}

CCMD(lightoverlay_export)
{
	if (argv.argc() != 2)
	{
		Printf("usage: lightoverlay_export <path>\n");
		return;
	}

	std::unique_ptr<FileWriter> file(FileWriter::Open(argv[1]));
	if (file == nullptr)
	{
		Printf("LIGHTOVR export failed: could not open '%s' for writing.\n", argv[1]);
		return;
	}

	const FString serialized = SerializeLightOverlayDatabase(GetParsedLightOverlayDatabase());
	file->Write(serialized.GetChars(), serialized.Len());
	Printf("LIGHTOVR export wrote %d bytes to %s.\n", serialized.Len(), argv[1]);
}
