#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <memory>

#include "lightoverlay_editor.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "d_net.h"
#include "filesystem.h"
#include "gamefuncs.h"
#include "gamestate.h"
#include "i_time.h"
#include "keydef.h"
#include "lightoverlay.h"
#include "mapinfo.h"
#include "printf.h"
#include "v_video.h"

CVAR(Bool, nri_ptactorlighteditmode, false, CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptmaplighteditmode, false, CVAR_GLOBALCONFIG)
CVAR(Bool, nri_ptemissivelighteditmode, false, 0)

namespace
{
	static ActorLightEditorState GActorLightEditorState;
	static constexpr uint64_t ActorLightEditorNotifyRepeatMs = 750;
	static constexpr uint64_t ActorLightEditorNotifyClearGraceMs = 250;
	static constexpr double MapLightEditorDefaultDistance = 128.0;
	static constexpr double MapLightEditorDistanceStep = 16.0;
	static constexpr double MapLightEditorMaxDistance = 4096.0;
	static constexpr float MapLightEditorColor[3] = { 1.0f, 1.0f, 1.0f };
	static constexpr float MapLightEditorIntensity = 1.0f;
	static constexpr float MapLightEditorRadius = 200.0f;
	static constexpr const char* MapLightEditorDirectionalRuleId = "EditorDirectional";
	static constexpr float SurfaceLightEditorDefaultWidth = 32.0f;
	static constexpr float SurfaceLightEditorDefaultHeight = 32.0f;
	static constexpr float SurfaceLightEditorDefaultOffset = 0.5f;
	static constexpr float SurfaceLightEditorDefaultIntensity = 1.0f;
	static constexpr float SurfaceLightEditorDefaultRadius = 256.0f;
	static constexpr float SurfaceLightEditorDefaultColor[3] = { 1.0f, 1.0f, 1.0f };
	static constexpr const char* SurfaceLightEditorDefaultTexture = "#00707";
	static constexpr float SurfaceLightEditorRotateStep = 15.0f;
	static constexpr float SurfaceLightEditorSizeStep = 4.0f;
	static constexpr float SurfaceLightEditorIntensityStep = 0.2f;
	static constexpr float SurfaceLightEditorRadiusStep = 32.0f;
	static constexpr const char* SurfaceLightEditorTextureOptions[] =
	{
		"#00124", "#00701", "#00702", "#00703", "#00707", "#00708", "#01206", "#00705", "#00706",
		"#00704", "#00126", "#00120", "#00121", "#00122", "#00123", "#00127", "#00128"
	};

	enum class SurfaceLightEditorEditAction : uint8_t
	{
		Rotate,
		SizeX,
		SizeY,
		Intensity,
		Radius,
		Texture,
	};

	enum class ActorLightEditorWritableSourceKind : uint8_t
	{
		None,
		Directory,
		StandaloneFile,
		ReadOnlyContainer,
	};

	static const char* GetActorLightEditorClassName(const DCoreActor* actor)
	{
		if (actor == nullptr || actor->GetClass() == nullptr)
		{
			return "(null)";
		}
		return actor->GetClass()->TypeName.GetChars();
	}

	static bool GetActorLightEditorSamplingContext(DCorePlayer*& outPlayer, DCoreActor*& outActor, DVector3& outOrigin, DRotator& outViewRotation, sectortype*& outStartSector)
	{
		outPlayer = nullptr;
		outActor = nullptr;
		outOrigin = {};
		outViewRotation = {};
		outStartSector = nullptr;

		if (gamestate != GS_LEVEL || myconnectindex < 0 || myconnectindex >= MAXPLAYERS)
		{
			return false;
		}

		outPlayer = PlayerArray[myconnectindex];
		if (outPlayer == nullptr)
		{
			return false;
		}

		outActor = outPlayer->GetActor();
		if (outActor == nullptr || !outActor->exists() || (outActor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			return false;
		}

		outOrigin = outPlayer->CameraPos;
		outStartSector = outActor->sector();
		updatesector(outOrigin, &outStartSector);
		if (outStartSector == nullptr)
		{
			outOrigin = outActor->getPosWithOffsetZ();
			outStartSector = outActor->sector();
			updatesector(outOrigin, &outStartSector);
			if (outStartSector == nullptr)
			{
				return false;
			}
		}

		outViewRotation = DRotator(
			outPlayer->getPitchWithView(),
			outActor->spr.Angles.Yaw + outPlayer->ViewAngles.Yaw,
			outActor->spr.Angles.Roll + outPlayer->ViewAngles.Roll);
		return true;
	}

	static void PrintActorLightEditorTarget(const ActorLightEditorTarget& target)
	{
		switch (target.kind)
		{
		case ActorLightEditorTargetKind::Actor:
			Printf(
				"NRI PT actor light editor target: kind=actor actor=%d class=%s sector=%d hitpos=(%.2f, %.2f, %.2f)\n",
				target.actorIndex,
				target.actorClassName.GetChars(),
				target.hitSector != nullptr ? sectindex(target.hitSector) : -1,
				target.hitX,
				target.hitY,
				target.hitZ);
			break;

		case ActorLightEditorTargetKind::Surface:
			Printf(
				"NRI PT actor light editor target: kind=surface sector=%d wall=%d hitpos=(%.2f, %.2f, %.2f)\n",
				target.hitSector != nullptr ? sectindex(target.hitSector) : -1,
				target.hitWall != nullptr ? wallindex(target.hitWall) : -1,
				target.hitX,
				target.hitY,
				target.hitZ);
			break;

		default:
			Printf("NRI PT actor light editor target: kind=none\n");
			break;
		}
	}

	static ActorLightEditorWritableSourceKind ClassifyActorLightEditorWritableSource(int lumpNum, FString& outPath)
	{
		outPath = "";
		if (lumpNum < 0)
		{
			return ActorLightEditorWritableSourceKind::None;
		}

		const int container = fileSystem.GetFileContainer(lumpNum);
		const char* entryName = fileSystem.GetFileFullName(lumpNum);
		const char* containerPath = fileSystem.GetResourceFileFullName(container);
		if (container < 0 || entryName == nullptr || entryName[0] == 0 || containerPath == nullptr || containerPath[0] == 0)
		{
			return ActorLightEditorWritableSourceKind::None;
		}

		FString normalizedContainerPath = containerPath;
		FixPathSeperator(normalizedContainerPath);

		if (DirExists(normalizedContainerPath.GetChars()))
		{
			outPath = normalizedContainerPath;
			if (outPath.IsEmpty() || outPath.Back() != '/')
			{
				outPath << '/';
			}
			outPath << entryName;
			FixPathSeperator(outPath);
			return ActorLightEditorWritableSourceKind::Directory;
		}

		if (!FileExists(normalizedContainerPath.GetChars()))
		{
			return ActorLightEditorWritableSourceKind::None;
		}

		const FString resourceBase = ExtractFileBase(normalizedContainerPath.GetChars(), true);
		const FString entryBase = ExtractFileBase(entryName, true);
		if (fileSystem.GetEntryCount(container) == 1 &&
			resourceBase.CompareNoCase("LIGHTOVR") == 0 &&
			entryBase.CompareNoCase("LIGHTOVR") == 0)
		{
			outPath = normalizedContainerPath;
			return ActorLightEditorWritableSourceKind::StandaloneFile;
		}

		return ActorLightEditorWritableSourceKind::ReadOnlyContainer;
	}

	static void PrintActorLightEditorWritableSourceFailure()
	{
		const auto& database = GetParsedLightOverlayDatabase();
		if (database.sourceFiles.Size() == 0)
		{
			Printf("NRI PT actor light editor: no mounted LIGHTOVR source is available.\n");
			return;
		}

		bool sawMountedSource = false;
		bool sawReadOnlySource = false;
		for (int i = database.sourceFiles.Size() - 1; i >= 0; --i)
		{
			const auto& sourceFile = database.sourceFiles[i];
			if (sourceFile.lumpNum < 0)
			{
				continue;
			}

			sawMountedSource = true;
			FString ignoredPath;
			const auto kind = ClassifyActorLightEditorWritableSource(sourceFile.lumpNum, ignoredPath);
			if (kind == ActorLightEditorWritableSourceKind::ReadOnlyContainer)
			{
				sawReadOnlySource = true;
				break;
			}
		}

		if (sawReadOnlySource)
		{
			Printf("NRI PT actor light editor: no writable mounted LIGHTOVR source is available; archive-backed sources are read-only.\n");
		}
		else if (sawMountedSource)
		{
			Printf("NRI PT actor light editor: no writable mounted LIGHTOVR source could be resolved from the current overlay metadata.\n");
		}
		else
		{
			Printf("NRI PT actor light editor: no mounted LIGHTOVR source is available.\n");
		}
	}

	static FString FindActorLightEditorSourceNameForLump(const ParsedLightOverlayDatabase& database, int lumpNum)
	{
		for (const auto& sourceFile : database.sourceFiles)
		{
			if (sourceFile.lumpNum == lumpNum)
			{
				return sourceFile.sourceName;
			}
		}

		const char* fullName = lumpNum >= 0 ? fileSystem.GetFileFullName(lumpNum) : nullptr;
		return fullName != nullptr ? FString(fullName) : FString("LIGHTOVR");
	}

	static bool HasActorLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& id)
	{
		for (const auto& rule : database.actorRules)
		{
			if (rule.id.CompareNoCase(id) == 0)
			{
				return true;
			}
		}

		return false;
	}

	static FString BuildActorLightEditorUniqueRuleId(const ParsedLightOverlayDatabase& database, const FString& actorClassName)
	{
		FString baseId = actorClassName;
		if (!HasActorLightEditorRuleId(database, baseId))
		{
			return baseId;
		}

		for (int suffix = 2; suffix < 1000000; ++suffix)
		{
			FString candidate = FStringf("%s_%d", baseId.GetChars(), suffix);
			if (!HasActorLightEditorRuleId(database, candidate))
			{
				return candidate;
			}
		}

		return FStringf("%s_%u", baseId.GetChars(), (unsigned)I_msTime());
	}

	static bool HasMapLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (const auto& rule : database.mapLightRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0 && rule.id.CompareNoCase(id) == 0)
			{
				return true;
			}
		}

		return false;
	}

	static FString BuildMapLightEditorUniqueRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName)
	{
		const FString baseId = "MapLight";
		if (!HasMapLightEditorRuleId(database, mapName, baseId))
		{
			return baseId;
		}

		for (int suffix = 2; suffix < 1000000; ++suffix)
		{
			FString candidate = FStringf("%s_%d", baseId.GetChars(), suffix);
			if (!HasMapLightEditorRuleId(database, mapName, candidate))
			{
				return candidate;
			}
		}

		return FStringf("%s_%u", baseId.GetChars(), (unsigned)I_msTime());
	}

	static bool HasEmissiveLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (const auto& rule : database.emissiveOverrideRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0 && rule.id.CompareNoCase(id) == 0)
			{
				return true;
			}
		}

		return false;
	}

	static FString BuildEmissiveLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName, const PathTracingEmissiveLightEditTarget& target)
	{
		FString baseId = FStringf("Emitter_S%d_W%d_T%d", target.sectorIndex, target.wallIndex, target.textureId);
		if (!HasEmissiveLightEditorRuleId(database, mapName, baseId))
		{
			return baseId;
		}

		return baseId;
	}

	static int CountSurfaceLightEditorRulesForMap(const ParsedLightOverlayDatabase& database, const FString& mapName)
	{
		int count = 0;
		for (const auto& rule : database.surfaceLightRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				++count;
			}
		}

		return count;
	}

	static bool HasSurfaceLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (const auto& rule : database.surfaceLightRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0 && rule.id.CompareNoCase(id) == 0)
			{
				return true;
			}
		}

		return false;
	}

	static FString BuildSurfaceLightEditorTimestampSuffix()
	{
		const std::time_t now = std::time(nullptr);
		std::tm utcTime = {};
#if defined(_WIN32)
		gmtime_s(&utcTime, &now);
#else
		gmtime_r(&now, &utcTime);
#endif
		return FStringf(
			"%d-%d-%04d-%02d%02dZ",
			utcTime.tm_mon + 1,
			utcTime.tm_mday,
			utcTime.tm_year + 1900,
			utcTime.tm_hour,
			utcTime.tm_min);
	}

	static FString BuildSurfaceLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName, const PathTracingEmissiveLightEditTarget&)
	{
		const FString timestamp = BuildSurfaceLightEditorTimestampSuffix();

		for (int index = CountSurfaceLightEditorRulesForMap(database, mapName); index < 1000000; ++index)
		{
			FString candidate = FStringf("surfacelight_%d_%s", index, timestamp.GetChars());
			if (!HasSurfaceLightEditorRuleId(database, mapName, candidate))
			{
				return candidate;
			}
		}

		return FStringf("surfacelight_%u_%s", (unsigned)I_msTime(), timestamp.GetChars());
	}

	static uint64_t HashSurfaceLightEditorText(uint64_t hash, const char* text)
	{
		if (text == nullptr)
		{
			return hash;
		}

		for (const unsigned char* cursor = (const unsigned char*)text; *cursor != '\0'; ++cursor)
		{
			hash ^= (uint64_t)(*cursor);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t BuildSurfaceLightEditorResolvedRuleId(const ParsedLightOverlaySurfaceLightRule& rule)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = HashSurfaceLightEditorText(hash, rule.id.GetChars());
		hash = HashSurfaceLightEditorText(hash, rule.mapName.GetChars());
		hash = HashSurfaceLightEditorText(hash, rule.source.sourceName.GetChars());
		hash ^= (uint64_t)rule.source.orderIndex + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		const uint32_t ruleId = (uint32_t)(hash ^ (hash >> 32));
		return ruleId != 0 ? ruleId : 1u;
	}

	static int32_t FindSurfaceLightEditorRuleIndexById(const ParsedLightOverlayDatabase& database, const FString& mapName, uint32_t ruleId)
	{
		for (unsigned i = 0; i < (unsigned)database.surfaceLightRules.Size(); ++i)
		{
			const auto& rule = database.surfaceLightRules[i];
			if (rule.mapName.CompareNoCase(mapName) == 0 &&
				BuildSurfaceLightEditorResolvedRuleId(rule) == ruleId)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static int32_t FindSurfaceLightEditorRuleIndexByName(const ParsedLightOverlayDatabase& database, const FString& mapName, const FString& id)
	{
		for (unsigned i = 0; i < (unsigned)database.surfaceLightRules.Size(); ++i)
		{
			const auto& rule = database.surfaceLightRules[i];
			if (rule.mapName.CompareNoCase(mapName) == 0 && rule.id.CompareNoCase(id) == 0)
			{
				return (int32_t)i;
			}
		}
		return -1;
	}

	static void SetSurfaceLightEditorActiveRule(const ParsedLightOverlaySurfaceLightRule& rule)
	{
		GActorLightEditorState.activeSurfaceLightMapName = rule.mapName;
		GActorLightEditorState.activeSurfaceLightRuleName = rule.id;
		GActorLightEditorState.activeSurfaceLightRuleId = BuildSurfaceLightEditorResolvedRuleId(rule);
	}

	static int32_t FindSurfaceLightEditorActiveRuleIndex(const ParsedLightOverlayDatabase& database, const FString& mapName)
	{
		if (GActorLightEditorState.activeSurfaceLightMapName.CompareNoCase(mapName) != 0)
		{
			return -1;
		}

		if (GActorLightEditorState.activeSurfaceLightRuleId != 0u)
		{
			const int32_t byId = FindSurfaceLightEditorRuleIndexById(database, mapName, GActorLightEditorState.activeSurfaceLightRuleId);
			if (byId >= 0)
			{
				return byId;
			}
		}

		return GActorLightEditorState.activeSurfaceLightRuleName.IsNotEmpty() ?
			FindSurfaceLightEditorRuleIndexByName(database, mapName, GActorLightEditorState.activeSurfaceLightRuleName) :
			-1;
	}

	static void CopySurfaceLightEditorRepeatSettings(ParsedLightOverlaySurfaceLightRule& destination, const ParsedLightOverlaySurfaceLightRule& source)
	{
		destination.hasSize = source.hasSize;
		destination.size[0] = source.size[0];
		destination.size[1] = source.size[1];
		destination.hasRotation = source.hasRotation;
		destination.rotation = source.rotation;
		destination.hasOffset = source.hasOffset;
		destination.offset = source.offset;
		destination.hasFixtureTexture = source.hasFixtureTexture;
		destination.fixtureTexture = source.fixtureTexture;
		destination.hasFixtureMaterialResponse = source.hasFixtureMaterialResponse;
		destination.fixtureMaterialResponse = source.fixtureMaterialResponse;
		destination.lightType = source.lightType;
		destination.hasColor = source.hasColor;
		destination.color[0] = source.color[0];
		destination.color[1] = source.color[1];
		destination.color[2] = source.color[2];
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasSectorResponse = source.hasSectorResponse;
		destination.sectorResponse = source.sectorResponse;
		destination.colorResponse = source.colorResponse;
		destination.colorResponse.signalSector = -1;
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

	static float NormalizeSurfaceLightEditorRotation(float degrees)
	{
		while (degrees < 0.0f)
		{
			degrees += 360.0f;
		}
		while (degrees >= 360.0f)
		{
			degrees -= 360.0f;
		}
		return degrees;
	}

	static int FindSurfaceLightEditorTextureOption(const FString& texture)
	{
		for (unsigned i = 0; i < (unsigned)(sizeof(SurfaceLightEditorTextureOptions) / sizeof(SurfaceLightEditorTextureOptions[0])); ++i)
		{
			if (texture.CompareNoCase(SurfaceLightEditorTextureOptions[i]) == 0)
			{
				return (int)i;
			}
		}
		return -1;
	}

	static bool FindMapLightEditorActiveDirectionalRule(const ParsedLightOverlayDatabase& database, const FString& mapName, ParsedLightOverlayDirectionalRule& outRule)
	{
		bool found = false;
		for (const auto& rule : database.directionalRules)
		{
			if (rule.mapName.CompareNoCase(mapName) == 0)
			{
				outRule = rule;
				found = true;
			}
		}

		return found;
	}

	static bool WriteActorLightEditorTextFile(const FString& path, const FString& text)
	{
		std::unique_ptr<FileWriter> file(FileWriter::Open(path.GetChars()));
		if (file == nullptr)
		{
			return false;
		}

		const size_t bytesToWrite = text.Len();
		const size_t bytesWritten = file->Write(text.GetChars(), bytesToWrite);
		file->Close();
		return bytesWritten == bytesToWrite;
	}

	static bool ReloadActorLightEditorOverlays()
	{
		const bool ok = ParseLightOverlays(true);
		Printf("LIGHTOVR reload %s.\n", ok ? "completed" : "completed with parse errors");
		return ok;
	}

	static void PrintActorLightEditorActorData(const ActorLightEditorTarget& target)
	{
		const auto* actor = target.actor;
		if (actor == nullptr)
		{
			Printf("NRI PT actor light editor: sampled actor target is no longer valid.\n");
			return;
		}

		Printf(
			"NRI PT actor light editor: actor=%d class=%s stat=%d sector=%d type=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x pos=(%.2f, %.2f, %.2f) hitpos=(%.2f, %.2f, %.2f)\n",
			actor->GetIndex(),
			target.actorClassName.GetChars(),
			(int)actor->spr.statnum,
			actor->sectno(),
			(int)actor->spr.type,
			(int)actor->spr.pal,
			(int)actor->spr.shade,
			(unsigned int)actor->spr.cstat,
			(unsigned int)actor->spr.cstat2,
			actor->spr.pos.X,
			actor->spr.pos.Y,
			actor->spr.pos.Z,
			target.hitX,
			target.hitY,
			target.hitZ);

		Printf(
			"NRI PT actor light editor: lotag=%d hitag=%d extra=%d detail=%d owner=%d clipdist=%.2f clipdist_map=%u blend=%u pal=%u scale=(%.3f, %.3f) offset=(%d, %d) angles=(%.2f, %.2f, %.2f) vel=(%.2f, %.2f, %.2f) object_flags=0x%x\n",
			(int)actor->spr.lotag,
			(int)actor->spr.hitag,
			(int)actor->spr.extra,
			(int)actor->spr.detail,
			(int)actor->spr.intowner,
			actor->clipdist,
			(unsigned int)actor->spr.clipdist,
			(unsigned int)actor->spr.blend,
			(unsigned int)actor->spr.pal,
			actor->spr.scale.X,
			actor->spr.scale.Y,
			(int)actor->spr.xoffset,
			(int)actor->spr.yoffset,
			actor->spr.Angles.Yaw.Degrees(),
			actor->spr.Angles.Pitch.Degrees(),
			actor->spr.Angles.Roll.Degrees(),
			actor->vel.X,
			actor->vel.Y,
			actor->vel.Z,
			(unsigned int)actor->ObjectFlags);
	}

	static void PrintActorLightEditorSurfaceData()
	{
		if (screen == nullptr)
		{
			Printf("NRI PT actor light editor: surface probe status is unavailable because no screen backend is active.\n");
			return;
		}

		screen->PrintPathTracingSurfaceProbeStatus();
	}

	static bool IsActorLightEditorActionKey(const event_t* ev, char key)
	{
		if (ev == nullptr || (ev->type != EV_KeyDown && ev->type != EV_KeyUp))
		{
			return false;
		}

		const unsigned char ascii = static_cast<unsigned char>(ev->data2 & 0xff);
		return ascii != 0 && std::tolower(ascii) == key;
	}

	static bool GetMapLightEditorPreviewPosition(DVector3& outPosition)
	{
		DCorePlayer* player = nullptr;
		DCoreActor* actor = nullptr;
		DVector3 origin;
		DRotator viewRotation;
		sectortype* startSector = nullptr;
		if (!GetActorLightEditorSamplingContext(player, actor, origin, viewRotation, startSector))
		{
			outPosition = {};
			return false;
		}

		outPosition = origin + DVector3(viewRotation) * GActorLightEditorState.mapLightPreviewDistance;
		return true;
	}

	static bool GetMapLightEditorDirectionalLightDirection(float outDirection[3])
	{
		if (outDirection == nullptr)
		{
			return false;
		}

		DCorePlayer* player = nullptr;
		DCoreActor* actor = nullptr;
		DVector3 origin;
		DRotator viewRotation;
		sectortype* startSector = nullptr;
		if (!GetActorLightEditorSamplingContext(player, actor, origin, viewRotation, startSector))
		{
			return false;
		}

		DVector3 direction = DVector3(viewRotation);
		direction = DVector3(-direction.X, -direction.Y, -direction.Z);
		direction = DVector3(direction.X, -direction.Z, -direction.Y);
		direction.MakeUnit();

		outDirection[0] = (float)direction.X;
		outDirection[1] = (float)direction.Y;
		outDirection[2] = (float)direction.Z;
		return true;
	}

	static void UpdateMapLightEditorPreview()
	{
		if (screen == nullptr)
		{
			return;
		}

		DVector3 position;
		if (!GetMapLightEditorPreviewPosition(position))
		{
			screen->ClearPathTracingEditorPointLight();
			return;
		}

		screen->SetPathTracingEditorPointLight(position, MapLightEditorColor, MapLightEditorIntensity, MapLightEditorRadius);
	}

	static void PerformActorLightEditorPrintAction()
	{
		ActorLightEditorTarget target;
		if (!ActorLightEditorSampleTarget(target))
		{
			Printf("NRI PT actor light editor: no local gameplay sampling context is available.\n");
			return;
		}

		GActorLightEditorState.currentTarget = target;

		switch (target.kind)
		{
		case ActorLightEditorTargetKind::Actor:
			PrintActorLightEditorActorData(target);
			break;

		case ActorLightEditorTargetKind::Surface:
			PrintActorLightEditorSurfaceData();
			break;

		default:
			PrintActorLightEditorTarget(target);
			break;
		}
	}

	static void PerformActorLightEditorCreateRuleAction()
	{
		ActorLightEditorTarget target;
		if (!ActorLightEditorSampleTarget(target))
		{
			Printf("NRI PT actor light editor: no local gameplay sampling context is available.\n");
			return;
		}

		GActorLightEditorState.currentTarget = target;
		if (target.kind != ActorLightEditorTargetKind::Actor || target.actor == nullptr || target.actorClassName.IsEmpty())
		{
			Printf("NRI PT actor light editor: placeholder rule creation requires an actor target.\n");
			return;
		}

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlayActorRule rule = {};
		rule.id = BuildActorLightEditorUniqueRuleId(database, target.actorClassName);
		rule.actorClassName = target.actorClassName;
		rule.lightType = "point";
		rule.hasColor = true;
		rule.color[0] = 1.0f;
		rule.color[1] = 0.9f;
		rule.color[2] = 0.7f;
		rule.hasIntensity = true;
		rule.intensity = 8.0f;
		rule.hasRadius = true;
		rule.radius = 96.0f;
		rule.hasOffset = true;
		rule.offset[0] = 0.0f;
		rule.offset[1] = 0.0f;
		rule.offset[2] = 0.0f;
		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);

		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);
		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT actor light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return;
		}

		Printf(
			"NRI PT actor light editor: created placeholder actorrule '%s' for class '%s' and wrote %d bytes to %s.\n",
			rule.id.GetChars(),
			rule.actorClassName.GetChars(),
			serialized.Len(),
			writablePath.GetChars());

		ReloadActorLightEditorOverlays();
	}

	static void PerformActorLightEditorReloadAction()
	{
		ReloadActorLightEditorOverlays();
	}

	static void PerformMapLightEditorDirectionalAction()
	{
		if (currentLevel == nullptr || currentLevel->labelName.IsEmpty())
		{
			Printf("NRI PT map light editor: no current map name is available.\n");
			return;
		}

		float direction[3] = {};
		if (!GetMapLightEditorDirectionalLightDirection(direction))
		{
			Printf("NRI PT map light editor: no local gameplay sampling context is available.\n");
			return;
		}

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlayDirectionalRule rule = {};
		const bool replaced = FindMapLightEditorActiveDirectionalRule(database, currentLevel->labelName, rule);
		if (!replaced)
		{
			rule.mapName = currentLevel->labelName;
			rule.id = MapLightEditorDirectionalRuleId;
			rule.hasColor = true;
			rule.color[0] = 1.0f;
			rule.color[1] = 1.0f;
			rule.color[2] = 1.0f;
			rule.hasIntensity = true;
			rule.intensity = 1.0f;
			rule.hasAngularSize = true;
			rule.angularSize = 0.03f;
			rule.hasShadow = true;
			rule.shadow = true;
		}

		rule.hasDirection = true;
		rule.direction[0] = direction[0];
		rule.direction[1] = direction[1];
		rule.direction[2] = direction[2];
		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);

		bool addReplaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &addReplaced);

		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT map light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return;
		}

		Printf(
			"NRI PT map light editor: %s directional '%s' for map '%s' direction=(%.3f, %.3f, %.3f) and wrote %d bytes to %s.\n",
			(addReplaced || replaced) ? "updated" : "created",
			rule.id.GetChars(),
			rule.mapName.GetChars(),
			rule.direction[0],
			rule.direction[1],
			rule.direction[2],
			serialized.Len(),
			writablePath.GetChars());

		ReloadActorLightEditorOverlays();
	}

	static void PerformMapLightEditorPlaceAction()
	{
		if (currentLevel == nullptr || currentLevel->labelName.IsEmpty())
		{
			Printf("NRI PT map light editor: no current map name is available.\n");
			return;
		}

		DVector3 position;
		if (!GetMapLightEditorPreviewPosition(position))
		{
			Printf("NRI PT map light editor: no local gameplay sampling context is available.\n");
			return;
		}

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlayMapLightRule rule = {};
		rule.mapName = currentLevel->labelName;
		rule.id = BuildMapLightEditorUniqueRuleId(database, rule.mapName);
		rule.lightType = "point";
		rule.anchorType = LightOverlayAnchorType::Position;
		rule.hasAnchorPosition = true;
		rule.anchorPosition[0] = (float)position.X;
		rule.anchorPosition[1] = (float)position.Y;
		rule.anchorPosition[2] = (float)position.Z;
		rule.hasColor = true;
		rule.color[0] = MapLightEditorColor[0];
		rule.color[1] = MapLightEditorColor[1];
		rule.color[2] = MapLightEditorColor[2];
		rule.hasIntensity = true;
		rule.intensity = MapLightEditorIntensity;
		rule.hasRadius = true;
		rule.radius = MapLightEditorRadius;
		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);

		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);
		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT map light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return;
		}

		Printf(
			"NRI PT map light editor: created map light '%s' for map '%s' at (%.2f, %.2f, %.2f) and wrote %d bytes to %s.\n",
			rule.id.GetChars(),
			rule.mapName.GetChars(),
			position.X,
			position.Y,
			position.Z,
			serialized.Len(),
			writablePath.GetChars());

		ReloadActorLightEditorOverlays();
	}

	enum class SectorColorEdit { Auto, Off, Color, Strength, Gain };

	static void WriteSectorColorOverride(int sectorIndex, SectorColorEdit edit, const float* color = nullptr, float value = 0.0f)
	{
		if (currentLevel == nullptr || sectorIndex < 0 || (unsigned)sectorIndex >= sector.Size())
		{ Printf("NRI PT sector color: invalid current sector\n"); return; }
		if (!ReloadActorLightEditorOverlays()) return;
		FString writablePath;
		int lump = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &lump)) { PrintActorLightEditorWritableSourceFailure(); return; }
		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlaySectorColorRule rule;
		rule.mapName = currentLevel->labelName;
		rule.id.Format("SectorColor_%d", sectorIndex);
		for (const auto& existing : GetResolvedLightOverlaySet().sectorColorRules)
			if (existing.mapName.CompareNoCase(rule.mapName) == 0 && existing.sector == sectorIndex) rule = existing;
		rule.sector = sectorIndex;
		rule.enabled = edit != SectorColorEdit::Off;
		if (edit == SectorColorEdit::Auto) rule.hasColor = rule.hasStrength = rule.hasGain = false;
		if (edit == SectorColorEdit::Color)
		{
			rule.hasColor = true;
			for (int i = 0; i < 3; ++i) rule.color[i] = color[i];
		}
		if (edit == SectorColorEdit::Strength) { rule.hasStrength = true; rule.strength = value; }
		if (edit == SectorColorEdit::Gain) { rule.hasGain = true; rule.gain = value; }
		rule.source.lumpNum = lump;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, lump);
		AddOrReplaceLightOverlayRule(database, rule);
		if (!WriteActorLightEditorTextFile(writablePath, SerializeLightOverlayDatabase(database))) return;
		ReloadActorLightEditorOverlays();
		Printf("NRI PT sector color: wrote %s sector=%d enabled=%s source=%s to %s\n", rule.id.GetChars(), sectorIndex,
			rule.enabled ? "yes" : "no", rule.hasColor ? "RGB override" : "live palette", writablePath.GetChars());
	}

	static void PerformSectorColorEditorAction()
	{
		PathTracingEmissiveLightEditTarget target;
		if (screen == nullptr || !screen->BuildPathTracingSurfaceLightEditTarget(target)) return;
		// Creating an editable rule must not reset an existing authored sector correction.
		for (const auto& rule : GetResolvedLightOverlaySet().sectorColorRules)
			if (rule.sector == target.sectorIndex && rule.mapName.IsNotEmpty())
			{ Printf("NRI PT sector color: existing rule '%s' sector=%d; edit LIGHTOVR and reload with l\n", rule.id.GetChars(), target.sectorIndex); return; }
		WriteSectorColorOverride(target.sectorIndex, SectorColorEdit::Auto);
	}

	static void PerformEmissiveLightEditorCreateOverrideAction()
	{
		if (currentLevel == nullptr || currentLevel->labelName.IsEmpty())
		{
			Printf("NRI PT emissive light editor: no current map name is available.\n");
			return;
		}

		if (screen == nullptr)
		{
			Printf("NRI PT emissive light editor: no screen backend is active.\n");
			return;
		}

		PathTracingEmissiveLightEditTarget target = {};
		if (!screen->BuildPathTracingEmissiveLightEditTarget(target))
		{
			Printf(
				"NRI PT emissive light editor: cannot create override: %s.\n",
				target.failureReason.IsEmpty() ? "no active emissive surface is aimed" : target.failureReason.GetChars());
			return;
		}

		if (!ReloadActorLightEditorOverlays())
		{
			Printf("NRI PT emissive light editor: not writing emissiveoverride because the current LIGHTOVR reload has parse errors.\n");
			return;
		}

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlayEmissiveOverrideRule rule = {};
		rule.mapName = currentLevel->labelName;
		rule.id = BuildEmissiveLightEditorRuleId(database, rule.mapName, target);
		rule.hasSectorFilter = target.sectorIndex >= 0;
		rule.sectorFilter = target.sectorIndex;
		rule.hasWallFilter = target.wallIndex >= 0;
		rule.wallFilter = target.wallIndex;
		rule.hasTileFilter = target.textureId >= 0;
		rule.tileFilter = target.textureId;
		rule.hasIntensityScale = true;
		rule.intensityScale = 1.0f;
		rule.hasReachScale = true;
		rule.reachScale = 1.0f;
		rule.hasSectorResponse = true;
		rule.sectorResponse = true;
		rule.hasSignalSector = target.sectorIndex >= 0;
		rule.signalSector = target.sectorIndex;
		rule.hasResponseIntensity = true;
		rule.responseIntensity = target.sectorResponseIntensity;
		rule.hasResponseMin = true;
		rule.responseMin = target.sectorResponseMin;
		rule.hasResponseMax = true;
		rule.responseMax = target.sectorResponseMax;
		rule.colorResponse.hasResponse = true;
		for (const auto& existing : database.emissiveOverrideRules)
			if (existing.mapName.CompareNoCase(rule.mapName) == 0 && existing.id.CompareNoCase(rule.id) == 0)
				rule.colorResponse = existing.colorResponse;
		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);

		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);
		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT emissive light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return;
		}

		Printf(
			"NRI PT emissive light editor: %s emissiveoverride '%s' for map '%s' sector=%d wall=%d tile=%d signal_sector=%d response=%.3f/[%.3f,%.3f] and wrote %d bytes to %s.\n",
			replaced ? "updated" : "created",
			rule.id.GetChars(),
			rule.mapName.GetChars(),
			target.sectorIndex,
			target.wallIndex,
			target.textureId,
			rule.signalSector,
			rule.responseIntensity,
			rule.responseMin,
			rule.responseMax,
			serialized.Len(),
			writablePath.GetChars());

		ReloadActorLightEditorOverlays();
	}

	static void PerformEmissiveLightEditorCreateSurfaceLightAction(bool repeatActiveSettings)
	{
		if (currentLevel == nullptr || currentLevel->labelName.IsEmpty())
		{
			Printf("NRI PT emissive light editor: no current map name is available.\n");
			return;
		}

		if (screen == nullptr)
		{
			Printf("NRI PT emissive light editor: no screen backend is active.\n");
			return;
		}

		PathTracingEmissiveLightEditTarget target = {};
		if (!screen->BuildPathTracingSurfaceLightEditTarget(target))
		{
			Printf(
				"NRI PT emissive light editor: cannot create surfacelight: %s.\n",
				target.failureReason.IsEmpty() ? "no PT surface is aimed" : target.failureReason.GetChars());
			return;
		}

		if (!ReloadActorLightEditorOverlays())
		{
			Printf("NRI PT emissive light editor: not writing surfacelight because the current LIGHTOVR reload has parse errors.\n");
			return;
		}

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlaySurfaceLightRule repeatSource = {};
		bool hasRepeatSource = false;
		if (repeatActiveSettings)
		{
			const int32_t repeatIndex = FindSurfaceLightEditorActiveRuleIndex(database, currentLevel->labelName);
			if (repeatIndex < 0)
			{
				Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "No surfacelight template selected; place or edit one first.\n");
				return;
			}
			repeatSource = database.surfaceLightRules[repeatIndex];
			hasRepeatSource = true;
		}

		ParsedLightOverlaySurfaceLightRule rule = {};
		rule.mapName = currentLevel->labelName;
		rule.id = BuildSurfaceLightEditorRuleId(database, rule.mapName, target);
		rule.hasPosition = true;
		rule.position[0] = target.position[0];
		rule.position[1] = target.position[1];
		rule.position[2] = target.position[2];
		rule.hasNormal = true;
		rule.normal[0] = target.normal[0];
		rule.normal[1] = target.normal[1];
		rule.normal[2] = target.normal[2];
		rule.hasSize = true;
		rule.size[0] = SurfaceLightEditorDefaultWidth;
		rule.size[1] = SurfaceLightEditorDefaultHeight;
		rule.hasOffset = true;
		rule.offset = SurfaceLightEditorDefaultOffset;
		rule.hasSector = target.sectorIndex >= 0;
		rule.sector = target.sectorIndex;
		rule.hasWall = target.wallIndex >= 0;
		rule.wall = target.wallIndex;
		rule.hasTile = target.textureId >= 0;
		rule.tile = target.textureId;
		rule.hasFixtureTexture = true;
		rule.fixtureTexture = SurfaceLightEditorDefaultTexture;
		rule.hasFixtureMaterialResponse = true;
		rule.fixtureMaterialResponse = true;
		rule.lightType = "point";
		rule.hasColor = true;
		rule.color[0] = SurfaceLightEditorDefaultColor[0];
		rule.color[1] = SurfaceLightEditorDefaultColor[1];
		rule.color[2] = SurfaceLightEditorDefaultColor[2];
		rule.hasIntensity = true;
		rule.intensity = SurfaceLightEditorDefaultIntensity;
		rule.hasRadius = true;
		rule.radius = SurfaceLightEditorDefaultRadius;
		rule.hasSectorResponse = true;
		rule.sectorResponse = false;
		rule.hasSignalSector = target.sectorIndex >= 0;
		rule.signalSector = target.sectorIndex;
		rule.hasResponseIntensity = true;
		rule.responseIntensity = target.sectorResponseIntensity;
		rule.hasResponseMin = true;
		rule.responseMin = target.sectorResponseMin;
		rule.hasResponseMax = true;
		rule.responseMax = target.sectorResponseMax;
		if (hasRepeatSource)
		{
			CopySurfaceLightEditorRepeatSettings(rule, repeatSource);
		}
		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);

		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);
		const int32_t activeIndex = FindSurfaceLightEditorRuleIndexByName(database, rule.mapName, rule.id);
		if (activeIndex >= 0)
		{
			SetSurfaceLightEditorActiveRule(database.surfaceLightRules[activeIndex]);
		}

		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT emissive light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return;
		}

		Printf(
			"NRI PT emissive light editor: %s surfacelight '%s' for map '%s' sector=%d wall=%d tile=%d texture=%s pos=(%.2f, %.2f, %.2f) normal=(%.3f, %.3f, %.3f) size=(%.1f, %.1f) intensity=%.3f radius=%.1f signal_sector=%d and wrote %d bytes to %s.\n",
			replaced ? "updated" : "created",
			rule.id.GetChars(),
			rule.mapName.GetChars(),
			target.sectorIndex,
			target.wallIndex,
			target.textureId,
			rule.fixtureTexture.GetChars(),
			rule.position[0],
			rule.position[1],
			rule.position[2],
			rule.normal[0],
			rule.normal[1],
			rule.normal[2],
			rule.size[0],
			rule.size[1],
			rule.intensity,
			rule.radius,
			rule.signalSector,
			serialized.Len(),
			writablePath.GetChars());

		ReloadActorLightEditorOverlays();
	}

	static bool PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction action, float delta)
	{
		if (currentLevel == nullptr || currentLevel->labelName.IsEmpty() || screen == nullptr)
		{
			return false;
		}

		PathTracingEmissiveLightEditTarget target = {};
		const bool hasAimedSurfaceLight =
			screen->BuildPathTracingSurfaceLightEditTarget(target) &&
			target.surfaceLightOverlay &&
			target.surfaceLightRuleId != 0u;

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return true;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		int32_t index = hasAimedSurfaceLight ?
			FindSurfaceLightEditorRuleIndexById(database, currentLevel->labelName, target.surfaceLightRuleId) :
			-1;
		if (index < 0)
		{
			index = FindSurfaceLightEditorActiveRuleIndex(database, currentLevel->labelName);
			if (index < 0)
			{
				Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "No placed surfacelight selected; place one with o or aim at one.\n");
				return true;
			}
		}

		ParsedLightOverlaySurfaceLightRule rule = database.surfaceLightRules[index];
		SetSurfaceLightEditorActiveRule(rule);
		const bool hadSize = rule.hasSize;
		const bool hadRotation = rule.hasRotation;
		const bool hadIntensity = rule.hasIntensity;
		const bool hadRadius = rule.hasRadius;
		switch (action)
		{
		case SurfaceLightEditorEditAction::Rotate:
			rule.hasRotation = true;
			rule.rotation = NormalizeSurfaceLightEditorRotation((hadRotation ? rule.rotation : 0.0f) + delta);
			break;
		case SurfaceLightEditorEditAction::SizeX:
			rule.hasSize = true;
			rule.size[0] = std::max(1.0f, (hadSize ? rule.size[0] : SurfaceLightEditorDefaultWidth) + delta);
			break;
		case SurfaceLightEditorEditAction::SizeY:
			rule.hasSize = true;
			rule.size[1] = std::max(1.0f, (hadSize ? rule.size[1] : SurfaceLightEditorDefaultHeight) + delta);
			break;
		case SurfaceLightEditorEditAction::Intensity:
			rule.hasIntensity = true;
			rule.intensity = std::max(0.0f, (hadIntensity ? rule.intensity : SurfaceLightEditorDefaultIntensity) + delta);
			break;
		case SurfaceLightEditorEditAction::Radius:
			rule.hasRadius = true;
			rule.radius = std::max(0.0f, (hadRadius ? rule.radius : SurfaceLightEditorDefaultRadius) + delta);
			break;
		case SurfaceLightEditorEditAction::Texture:
		{
			const int optionCount = (int)(sizeof(SurfaceLightEditorTextureOptions) / sizeof(SurfaceLightEditorTextureOptions[0]));
			const int current = FindSurfaceLightEditorTextureOption(rule.fixtureTexture);
			const int next = current >= 0 ?
				(current + (delta >= 0.0f ? 1 : optionCount - 1)) % optionCount :
				(delta >= 0.0f ? 0 : optionCount - 1);
			rule.hasFixtureTexture = true;
			rule.fixtureTexture = SurfaceLightEditorTextureOptions[next];
			break;
		}
		default:
			return false;
		}

		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);
		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);
		const int32_t activeIndex = FindSurfaceLightEditorRuleIndexByName(database, rule.mapName, rule.id);
		if (activeIndex >= 0)
		{
			SetSurfaceLightEditorActiveRule(database.surfaceLightRules[activeIndex]);
		}

		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT emissive light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return true;
		}

		Printf(
			PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG,
			"NRI PT surfacelight '%s': size=(%.1f, %.1f) rotation=%.1f texture=%s intensity=%.2f radius=%.1f\n",
			rule.id.GetChars(),
			rule.hasSize ? rule.size[0] : SurfaceLightEditorDefaultWidth,
			rule.hasSize ? rule.size[1] : SurfaceLightEditorDefaultHeight,
			rule.hasRotation ? rule.rotation : 0.0f,
			(rule.hasFixtureTexture && rule.fixtureTexture.IsNotEmpty()) ? rule.fixtureTexture.GetChars() : "(default)",
			rule.hasIntensity ? rule.intensity : SurfaceLightEditorDefaultIntensity,
			rule.hasRadius ? rule.radius : SurfaceLightEditorDefaultRadius);

		ReloadActorLightEditorOverlays();
		return true;
	}

	static void MoveMapLightEditorPreview(double delta)
	{
		GActorLightEditorState.mapLightPreviewDistance += delta;
		if (GActorLightEditorState.mapLightPreviewDistance < 0.0)
		{
			GActorLightEditorState.mapLightPreviewDistance = 0.0;
		}
		else if (GActorLightEditorState.mapLightPreviewDistance > MapLightEditorMaxDistance)
		{
			GActorLightEditorState.mapLightPreviewDistance = MapLightEditorMaxDistance;
		}

		Printf(
			"NRI PT map light editor: preview distance %.1f\n",
			GActorLightEditorState.mapLightPreviewDistance);
		UpdateMapLightEditorPreview();
	}

	static void UpdateActorLightEditorNotify()
	{
		const uint64_t nowMs = I_msTime();
		const auto& target = GActorLightEditorState.currentTarget;

		if (target.kind == ActorLightEditorTargetKind::Actor && !target.actorClassName.IsEmpty())
		{
			GActorLightEditorState.lastActorSeenTimeMs = nowMs;

			const bool classChanged = GActorLightEditorState.lastNotifyActorClassName != target.actorClassName;
			const bool cooldownExpired =
				GActorLightEditorState.lastNotifyTimeMs == 0 ||
				(nowMs - GActorLightEditorState.lastNotifyTimeMs) >= ActorLightEditorNotifyRepeatMs;

			if (classChanged || cooldownExpired)
			{
				Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "%s\n", target.actorClassName.GetChars());
				GActorLightEditorState.lastNotifyActorClassName = target.actorClassName;
				GActorLightEditorState.lastNotifyTimeMs = nowMs;
			}

			return;
		}

		if (!GActorLightEditorState.lastNotifyActorClassName.IsEmpty() &&
			GActorLightEditorState.lastActorSeenTimeMs != 0 &&
			(nowMs - GActorLightEditorState.lastActorSeenTimeMs) >= ActorLightEditorNotifyClearGraceMs)
		{
			GActorLightEditorState.lastNotifyActorClassName = "";
			GActorLightEditorState.lastNotifyTimeMs = 0;
			GActorLightEditorState.lastActorSeenTimeMs = 0;
		}
	}
}

bool IsActorLightEditorEnabled()
{
	return !!nri_ptactorlighteditmode;
}

static bool IsMapLightEditorEnabled()
{
	return !!nri_ptmaplighteditmode;
}

static bool IsEmissiveLightEditorEnabled()
{
	return !!nri_ptemissivelighteditmode;
}

void ResetActorLightEditorState()
{
	const bool enabled = IsActorLightEditorEnabled() || IsMapLightEditorEnabled() || IsEmissiveLightEditorEnabled();
	const double previousMapLightDistance = GActorLightEditorState.mapLightPreviewDistance > 0.0 ?
		GActorLightEditorState.mapLightPreviewDistance :
		MapLightEditorDefaultDistance;
	GActorLightEditorState = {};
	GActorLightEditorState.enabled = enabled;
	GActorLightEditorState.mapLightPreviewDistance = previousMapLightDistance;
}

const ActorLightEditorState& GetActorLightEditorState()
{
	return GActorLightEditorState;
}

void TickActorLightEditor()
{
	const bool mapLightEnabled = IsMapLightEditorEnabled();
	const bool actorLightEnabled = IsActorLightEditorEnabled();
	const bool emissiveLightEnabled = IsEmissiveLightEditorEnabled();
	const bool enabled = actorLightEnabled || mapLightEnabled || emissiveLightEnabled;
	if (!enabled)
	{
		if (GActorLightEditorState.enabled)
		{
			if (screen != nullptr)
			{
				screen->ClearPathTracingEditorPointLight();
			}
			ResetActorLightEditorState();
		}
		return;
	}

	if (!GActorLightEditorState.enabled)
	{
		ResetActorLightEditorState();
	}

	if (mapLightEnabled)
	{
		UpdateMapLightEditorPreview();
	}
	else if (screen != nullptr)
	{
		screen->ClearPathTracingEditorPointLight();
	}

	if (actorLightEnabled)
	{
		ActorLightEditorSampleTarget(GActorLightEditorState.currentTarget);
		UpdateActorLightEditorNotify();
	}
}

bool ActorLightEditorResponder(event_t* ev)
{
	if ((!IsActorLightEditorEnabled() && !IsMapLightEditorEnabled() && !IsEmissiveLightEditorEnabled()) || ev == nullptr)
	{
		return false;
	}

	if ((ev->type == EV_KeyDown || ev->type == EV_KeyUp) &&
		(ev->data1 == KEY_LCTRL || ev->data1 == KEY_RCTRL))
	{
		GActorLightEditorState.controlKeyDown = ev->type == EV_KeyDown;
		return false;
	}

	if (IsEmissiveLightEditorEnabled())
	{
		if (ev->type == EV_KeyDown)
		{
			switch (ev->data1)
			{
			case KEY_MWHEELUP:
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Rotate, SurfaceLightEditorRotateStep);
			case KEY_MWHEELDOWN:
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Rotate, -SurfaceLightEditorRotateStep);
			case KEY_LEFTARROW:
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::SizeX, -SurfaceLightEditorSizeStep);
			case KEY_RIGHTARROW:
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::SizeX, SurfaceLightEditorSizeStep);
			case KEY_UPARROW:
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::SizeY, SurfaceLightEditorSizeStep);
			case KEY_DOWNARROW:
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::SizeY, -SurfaceLightEditorSizeStep);
			default:
				break;
			}

			if (IsActorLightEditorActionKey(ev, ','))
			{
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Intensity, -SurfaceLightEditorIntensityStep);
			}
			if (IsActorLightEditorActionKey(ev, '.'))
			{
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Intensity, SurfaceLightEditorIntensityStep);
			}
			if (IsActorLightEditorActionKey(ev, ';'))
			{
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Radius, -SurfaceLightEditorRadiusStep);
			}
			if (IsActorLightEditorActionKey(ev, '\''))
			{
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Radius, SurfaceLightEditorRadiusStep);
			}
			if (IsActorLightEditorActionKey(ev, '['))
			{
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Texture, -1.0f);
			}
			if (IsActorLightEditorActionKey(ev, ']'))
			{
				return PerformEmissiveLightEditorModifySurfaceLightAction(SurfaceLightEditorEditAction::Texture, 1.0f);
			}
		}

		if (IsActorLightEditorActionKey(ev, 'c'))
		{
			if (ev->type == EV_KeyDown && !GActorLightEditorState.sectorColorActionPressed) PerformSectorColorEditorAction();
			GActorLightEditorState.sectorColorActionPressed = ev->type == EV_KeyDown;
			return true;
		}

		if (IsActorLightEditorActionKey(ev, 'p'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.createEmissiveOverrideActionPressed)
				{
					GActorLightEditorState.createEmissiveOverrideActionPressed = true;
					PerformEmissiveLightEditorCreateOverrideAction();
				}
			}
			else
			{
				GActorLightEditorState.createEmissiveOverrideActionPressed = false;
			}

			return true;
		}

		if (IsActorLightEditorActionKey(ev, 'o'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.createSurfaceLightActionPressed)
				{
					GActorLightEditorState.createSurfaceLightActionPressed = true;
					PerformEmissiveLightEditorCreateSurfaceLightAction(GActorLightEditorState.controlKeyDown || (ev->data3 & GKM_CTRL) != 0);
				}
			}
			else
			{
				GActorLightEditorState.createSurfaceLightActionPressed = false;
			}

			return true;
		}

		if (IsActorLightEditorActionKey(ev, 'l'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.reloadActionPressed)
				{
					GActorLightEditorState.reloadActionPressed = true;
					PerformActorLightEditorReloadAction();
				}
			}
			else
			{
				GActorLightEditorState.reloadActionPressed = false;
			}

			return true;
		}
	}

	if (IsMapLightEditorEnabled())
	{
		if (IsActorLightEditorActionKey(ev, '['))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.moveMapLightCloserActionPressed)
				{
					GActorLightEditorState.moveMapLightCloserActionPressed = true;
					MoveMapLightEditorPreview(-MapLightEditorDistanceStep);
				}
			}
			else
			{
				GActorLightEditorState.moveMapLightCloserActionPressed = false;
			}

			return true;
		}

		if (IsActorLightEditorActionKey(ev, ']'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.moveMapLightFartherActionPressed)
				{
					GActorLightEditorState.moveMapLightFartherActionPressed = true;
					MoveMapLightEditorPreview(MapLightEditorDistanceStep);
				}
			}
			else
			{
				GActorLightEditorState.moveMapLightFartherActionPressed = false;
			}

			return true;
		}

		if (IsActorLightEditorActionKey(ev, 'p'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.placeMapLightActionPressed)
				{
					GActorLightEditorState.placeMapLightActionPressed = true;
					PerformMapLightEditorPlaceAction();
				}
			}
			else
			{
				GActorLightEditorState.placeMapLightActionPressed = false;
			}

			return true;
		}

		if (IsActorLightEditorActionKey(ev, 'o'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.setMapDirectionalActionPressed)
				{
					GActorLightEditorState.setMapDirectionalActionPressed = true;
					PerformMapLightEditorDirectionalAction();
				}
			}
			else
			{
				GActorLightEditorState.setMapDirectionalActionPressed = false;
			}

			return true;
		}

		if (IsActorLightEditorActionKey(ev, 'l'))
		{
			if (ev->type == EV_KeyDown)
			{
				if (!GActorLightEditorState.reloadActionPressed)
				{
					GActorLightEditorState.reloadActionPressed = true;
					PerformActorLightEditorReloadAction();
				}
			}
			else
			{
				GActorLightEditorState.reloadActionPressed = false;
			}

			return true;
		}
	}

	if (!IsActorLightEditorEnabled())
	{
		return false;
	}

	if (IsActorLightEditorActionKey(ev, 'p'))
	{
		if (ev->type == EV_KeyDown)
		{
			if (!GActorLightEditorState.printActionPressed)
			{
				GActorLightEditorState.printActionPressed = true;
				PerformActorLightEditorPrintAction();
			}
		}
		else
		{
			GActorLightEditorState.printActionPressed = false;
		}

		return true;
	}

	if (IsActorLightEditorActionKey(ev, 'o'))
	{
		if (ev->type == EV_KeyDown)
		{
			if (!GActorLightEditorState.createRuleActionPressed)
			{
				GActorLightEditorState.createRuleActionPressed = true;
				PerformActorLightEditorCreateRuleAction();
			}
		}
		else
		{
			GActorLightEditorState.createRuleActionPressed = false;
		}

		return true;
	}

	if (IsActorLightEditorActionKey(ev, 'l'))
	{
		if (ev->type == EV_KeyDown)
		{
			if (!GActorLightEditorState.reloadActionPressed)
			{
				GActorLightEditorState.reloadActionPressed = true;
				PerformActorLightEditorReloadAction();
			}
		}
		else
		{
			GActorLightEditorState.reloadActionPressed = false;
		}

		return true;
	}

	return false;
}

bool ActorLightEditorSampleTarget(ActorLightEditorTarget& outTarget)
{
	outTarget.Clear();

	DCorePlayer* player = nullptr;
	DCoreActor* actor = nullptr;
	DVector3 origin;
	DRotator viewRotation;
	sectortype* startSector = nullptr;
	if (!GetActorLightEditorSamplingContext(player, actor, origin, viewRotation, startSector))
	{
		return false;
	}

	HitInfoBase hit;
	hitscan(origin, startSector, DVector3(viewRotation), hit, CLIPMASK1, 65536.0);
	outTarget.hitSector = hit.hitSector;
	outTarget.hitWall = hit.hitWall;
	outTarget.hitX = hit.hitpos.X;
	outTarget.hitY = hit.hitpos.Y;
	outTarget.hitZ = hit.hitpos.Z;

	if (hit.hitActor != nullptr &&
		hit.hitActor->exists() &&
		(hit.hitActor->ObjectFlags & OF_EuthanizeMe) == 0)
	{
		outTarget.kind = ActorLightEditorTargetKind::Actor;
		outTarget.actor = hit.hitActor;
		outTarget.actorIndex = hit.hitActor->GetIndex();
		outTarget.actorClassName = GetActorLightEditorClassName(hit.hitActor);
	}
	else if (hit.hitWall != nullptr || hit.hitSector != nullptr)
	{
		outTarget.kind = ActorLightEditorTargetKind::Surface;
	}

	return true;
}

bool ActorLightEditorResolveWritableSource(FString& outPath, int* outLumpNum)
{
	outPath = "";
	if (outLumpNum != nullptr)
	{
		*outLumpNum = -1;
	}

	const auto& database = GetParsedLightOverlayDatabase();
	for (int i = database.sourceFiles.Size() - 1; i >= 0; --i)
	{
		const auto& sourceFile = database.sourceFiles[i];
		if (sourceFile.lumpNum < 0)
		{
			continue;
		}

		FString candidatePath;
		const auto kind = ClassifyActorLightEditorWritableSource(sourceFile.lumpNum, candidatePath);
		if (kind != ActorLightEditorWritableSourceKind::Directory &&
			kind != ActorLightEditorWritableSourceKind::StandaloneFile)
		{
			continue;
		}

		outPath = candidatePath;
		if (outLumpNum != nullptr)
		{
			*outLumpNum = sourceFile.lumpNum;
		}
		GActorLightEditorState.writableLightOvrPath = candidatePath;
		GActorLightEditorState.writableLightOvrLumpNum = sourceFile.lumpNum;
		return true;
	}

	GActorLightEditorState.writableLightOvrPath = "";
	GActorLightEditorState.writableLightOvrLumpNum = -1;
	return false;
}

CCMD(nri_ptactorlightedittarget)
{
	ActorLightEditorTarget target;
	if (!ActorLightEditorSampleTarget(target))
	{
		Printf("nri_ptactorlightedittarget: no local gameplay sampling context is available.\n");
		return;
	}

	PrintActorLightEditorTarget(target);
}

CCMD(nri_ptactorlighteditwritable)
{
	FString path;
	int lumpNum = -1;
	if (!ActorLightEditorResolveWritableSource(path, &lumpNum))
	{
		PrintActorLightEditorWritableSourceFailure();
		return;
	}

	Printf(
		"NRI PT actor light editor writable LIGHTOVR: path=%s lump=%d source=%s\n",
		path.GetChars(),
		lumpNum,
		lumpNum >= 0 ? fileSystem.GetFileFullPath(lumpNum).c_str() : "(none)");
}

CCMD(nri_ptsectorcoloredit)
{
	if (argv.argc() < 3 || argv.argc() > 5)
	{ Printf("usage: nri_ptsectorcoloredit <sector> <auto|off> OR <sector> <r> <g> <b> OR <sector> <strength|gain> <value>\n"); return; }
	char* end = nullptr;
	const long index = strtol(argv[1], &end, 10);
	if (end == argv[1] || *end != 0 || index < 0 || (unsigned long)index >= sector.Size()) { Printf("NRI PT sector color: invalid sector\n"); return; }
	if (argv.argc() == 3)
	{
		if (stricmp(argv[2], "auto") && stricmp(argv[2], "off")) { Printf("Expected auto or off\n"); return; }
		WriteSectorColorOverride((int)index, stricmp(argv[2], "off") != 0 ? SectorColorEdit::Auto : SectorColorEdit::Off);
		return;
	}
	if (argv.argc() == 4)
	{
		const bool strength = stricmp(argv[2], "strength") == 0;
		if (!strength && stricmp(argv[2], "gain") != 0) { Printf("Expected strength or gain\n"); return; }
		const float value = strtof(argv[3], &end);
		if (end == argv[3] || *end != 0 || !std::isfinite(value) || value < 0 || (strength && value > 1))
		{ Printf("Expected finite strength in [0,1] or nonnegative gain\n"); return; }
		WriteSectorColorOverride((int)index, strength ? SectorColorEdit::Strength : SectorColorEdit::Gain, nullptr, value);
		return;
	}
	float color[3];
	for (int i = 0; i < 3; ++i)
	{
		color[i] = strtof(argv[i + 2], &end);
		if (end == argv[i + 2] || *end != 0 || !std::isfinite(color[i]) || color[i] < 0) { Printf("Expected finite nonnegative RGB\n"); return; }
	}
	if (color[0] + color[1] + color[2] <= 0) { Printf("RGB must have positive luminance\n"); return; }
	WriteSectorColorOverride((int)index, SectorColorEdit::Color, color);
}

CCMD(lightoverlay_generatesectorcolors)
{
	if (!isDuke()) { Printf("LIGHTOVR sector color generation currently supports Duke palettes only\n"); return; }
	if (!ReloadActorLightEditorOverlays()) return;
	FString path;
	int lump = -1;
	if (!ActorLightEditorResolveWritableSource(path, &lump)) { PrintActorLightEditorWritableSourceFailure(); return; }
	const auto& database = GetParsedLightOverlayDatabase();
	ParsedLightOverlayDatabase additions;
	const int palettes[] = { 1, 2, 7, 8 };
	const float colors[][3] = { { 0.5f, 0.65f, 1.0f }, { 1.0f, 0.10f, 0.025f }, { 1.0f, 0.55f, 0.10f }, { 0.5f, 1.0f, 0.2f } };
	unsigned added = 0;
	for (int i = 0; i < 4; ++i)
	{
		bool authored = false;
		for (const auto& rule : database.sectorColorRules) if (rule.mapName.IsEmpty() && rule.palette == palettes[i]) authored = true;
		if (authored) continue;
		ParsedLightOverlaySectorColorRule rule;
		rule.id.Format("DukeEnvironmentPalette%d", palettes[i]);
		rule.palette = palettes[i];
		rule.hasColor = true;
		rule.hasStrength = rule.hasGain = true;
		rule.strength = 0.35f;
		rule.gain = 0.80f;
		for (int channel = 0; channel < 3; ++channel) rule.color[channel] = colors[i][channel];
		rule.source.lumpNum = lump;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, lump);
		AddOrReplaceLightOverlayRule(additions, rule);
		++added;
	}
	if (added != 0)
	{
		FileReader reader;
		if (!reader.OpenFile(path.GetChars())) { Printf("Cannot read writable LIGHTOVR for generation\n"); return; }
		const auto original = reader.ReadPadded(1);
		reader.Close();
		FString text((const char*)original.bytes());
		text << "\n// Initial environmental sector-color profiles. Existing authored text is preserved.\n";
		text << SerializeLightOverlayDatabase(additions);
		if (!WriteActorLightEditorTextFile(path, text)) return;
	}
	ReloadActorLightEditorOverlays();
	Printf("LIGHTOVR sector color generation: added=%u preserved authored profiles and overrides; path=%s\n", added, path.GetChars());
}

CCMD(nri_ptsectorcolorpreview)
{
	if (!nri_ptemissivelighteditmode || argv.argc() != 4)
	{ Printf("Enable nri_ptemissivelighteditmode, then use nri_ptsectorcolorpreview <sector> <floorpal> <ceilingpal>; reload the save to restore gameplay palettes.\n"); return; }
	int values[3];
	for (int i = 0; i < 3; ++i)
	{
		char* end = nullptr;
		const long value = strtol(argv[i + 1], &end, 10);
		const unsigned limit = i == 0 ? sector.Size() : 256u;
		if (end == argv[i + 1] || *end != 0 || value < 0 || (unsigned long)value >= limit)
		{ Printf("Invalid sector or palette\n"); return; }
		values[i] = (int)value;
	}
	sector[values[0]].floorpal = (uint8_t)values[1];
	sector[values[0]].ceilingpal = (uint8_t)values[2];
	Printf("NRI PT sector color preview: sector=%d floorpal=%d ceilingpal=%d; live state changed, overlay unchanged\n", values[0], values[1], values[2]);
}
