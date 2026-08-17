#include "nri_voxel_palette_policy.h"

#include "nri_hash.h"

#include "cmdlib.h"
#include "filesystem.h"
#include "model_kvx.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
	using namespace nri_scene;

	constexpr size_t MaxPolicyFileSize = 1024u * 1024u;
	constexpr size_t MaxPolicyDiagnostics = 32;

	void AddDiagnostic(
		std::vector<VoxelPalettePolicyDiagnostic>& diagnostics,
		VoxelPalettePolicyDiagnosticSeverity severity,
		const char* code,
		const std::string& message)
	{
		if (diagnostics.size() >= MaxPolicyDiagnostics)
		{
			return;
		}
		diagnostics.push_back({ severity, code, message });
	}

	std::string LowerAscii(std::string value)
	{
		for (char& character : value)
		{
			character = (char)std::tolower((unsigned char)character);
		}
		return value;
	}

	bool NormalizeVirtualPath(const std::string& input, std::string& output)
	{
		output.clear();
		std::string segment;
		for (size_t index = 0; index <= input.size(); ++index)
		{
			const char character = index < input.size() ? input[index] : '/';
			if (character == '/' || character == '\\')
			{
				if (segment.empty())
				{
					continue;
				}
				if (segment == "." || segment == ".." || segment.find(':') != std::string::npos)
				{
					output.clear();
					return false;
				}
				if (!output.empty())
				{
					output.push_back('/');
				}
				output += segment;
				segment.clear();
			}
			else
			{
				segment.push_back(character);
			}
		}
		return !output.empty();
	}

	std::string StripLastExtension(std::string path)
	{
		const size_t slash = path.find_last_of('/');
		const size_t dot = path.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			path.resize(dot);
		}
		return path;
	}

	bool SameVirtualPath(const std::string& left, const std::string& right)
	{
		std::string normalizedLeft;
		std::string normalizedRight;
		return NormalizeVirtualPath(left, normalizedLeft) &&
			NormalizeVirtualPath(right, normalizedRight) &&
			LowerAscii(normalizedLeft) == LowerAscii(normalizedRight);
	}

	std::string HexHash(uint64_t hash)
	{
		char text[32] = {};
		std::snprintf(text, sizeof(text), "fnv1a64:%016llx", (unsigned long long)hash);
		return text;
	}

	uint32_t ExpandPaletteRgb(const std::array<uint8_t, VoxelPalettePolicyPaletteSize>& palette, size_t index)
	{
		const size_t offset = index * 3;
		const uint32_t red = (palette[offset] << 2) | (palette[offset] >> 4);
		const uint32_t green = (palette[offset + 1] << 2) | (palette[offset + 1] >> 4);
		const uint32_t blue = (palette[offset + 2] << 2) | (palette[offset + 2] >> 4);
		return (red << 16) | (green << 8) | blue;
	}

	bool ParseRgb(const rapidjson::Value& value, uint32_t& output)
	{
		if (!value.IsString() || value.GetStringLength() != 7 || value.GetString()[0] != '#')
		{
			return false;
		}

		output = 0;
		for (size_t index = 1; index < 7; ++index)
		{
			const char character = value.GetString()[index];
			uint32_t digit = 0;
			if (character >= '0' && character <= '9') digit = (uint32_t)(character - '0');
			else if (character >= 'a' && character <= 'f') digit = (uint32_t)(character - 'a' + 10);
			else if (character >= 'A' && character <= 'F') digit = (uint32_t)(character - 'A' + 10);
			else return false;
			output = (output << 4) | digit;
		}
		return true;
	}

	std::string FormatRgb(uint32_t rgb)
	{
		char text[8] = {};
		std::snprintf(text, sizeof(text), "#%06X", rgb & 0xffffffu);
		return text;
	}

	bool TryParsePaletteIndex(const char* text, size_t length, size_t& output)
	{
		if (length == 0)
		{
			return false;
		}
		unsigned value = 0;
		for (size_t index = 0; index < length; ++index)
		{
			if (text[index] < '0' || text[index] > '9')
			{
				return false;
			}
			value = value * 10u + (unsigned)(text[index] - '0');
			if (value >= VoxelPalettePolicyEntryCount)
			{
				return false;
			}
		}
		output = value;
		return true;
	}

	uint64_t ComputeSemanticContentKey(
		const VoxelPalettePolicyDocument& document,
		const VoxelPalettePolicyRequest& request)
	{
		uint64_t hash = NRIHashFnv1a64OffsetBasis;
		const std::string sourceIdentity = LowerAscii(document.source);
		Fnv1a64Append(hash, &document.version, sizeof(document.version));
		Fnv1a64Append(hash, sourceIdentity.data(), sourceIdentity.size());
		Fnv1a64Append(hash, request.palette.data(), request.palette.size());
		uint64_t emissionScaleBits = 0;
		static_assert(sizeof(emissionScaleBits) == sizeof(document.emissionScale), "Unexpected double size");
		std::memcpy(&emissionScaleBits, &document.emissionScale, sizeof(emissionScaleBits));
		Fnv1a64Append(hash, &emissionScaleBits, sizeof(emissionScaleBits));
		Fnv1a64Append(hash, document.compiled.data(), document.compiled.size());
		return hash;
	}

	bool HasUniqueKnownMembers(
		const rapidjson::Value& object,
		const std::vector<const char*>& knownNames,
		const char* context,
		std::vector<VoxelPalettePolicyDiagnostic>& diagnostics)
	{
		std::vector<std::string> seen;
		for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member)
		{
			const std::string name(member->name.GetString(), member->name.GetStringLength());
			if (std::find(seen.begin(), seen.end(), name) != seen.end())
			{
				AddDiagnostic(diagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "duplicate-field", std::string(context) + " contains duplicate field '" + name + "'.");
				return false;
			}
			seen.push_back(name);
			bool known = false;
			for (const char* knownName : knownNames)
			{
				if (name == knownName)
				{
					known = true;
					break;
				}
			}
			if (!known)
			{
				AddDiagnostic(diagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "unknown-field", std::string(context) + " contains unknown field '" + name + "'.");
				return false;
			}
		}
		return true;
	}

	struct PolicySourceRead
	{
		bool found = false;
		bool success = false;
		VoxelPalettePolicyCandidate candidate;
		std::string text;
		std::string error;
	};

	std::string JoinPhysicalPath(const std::string& root, const std::string& relative)
	{
		std::string result = root;
		if (!result.empty() && result.back() != '/' && result.back() != '\\')
		{
			result.push_back('/');
		}
		result += relative;
		return result;
	}

	bool ReadPhysicalFile(const std::string& path, std::string& text, std::string& error)
	{
		FileSys::FileReader reader;
		if (!reader.OpenFile(path.c_str()))
		{
			error = "could not open loose policy file '" + path + "'.";
			return false;
		}
		const auto length = reader.GetLength();
		if (length < 0 || (size_t)length > MaxPolicyFileSize)
		{
			error = "policy file '" + path + "' exceeds the 1 MiB size limit.";
			return false;
		}
		text.resize((size_t)length);
		if (length > 0 && reader.Read(text.data(), length) != length)
		{
			error = "could not read the complete policy file '" + path + "'.";
			return false;
		}
		return true;
	}

	bool ReadMountedLump(int lump, std::string& text, std::string& error)
	{
		FileSys::FileReader reader = fileSystem.OpenFileReader(lump);
		if (!reader.isOpen())
		{
			error = "could not open mounted policy lump.";
			return false;
		}
		const auto length = reader.GetLength();
		if (length < 0 || (size_t)length > MaxPolicyFileSize)
		{
			error = "mounted policy exceeds the 1 MiB size limit.";
			return false;
		}
		text.resize((size_t)length);
		if (length > 0 && reader.Read(text.data(), length) != length)
		{
			error = "could not read the complete mounted policy lump.";
			return false;
		}
		return true;
	}

	PolicySourceRead ReadCandidate(const VoxelPalettePolicyCandidate& requested)
	{
		PolicySourceRead result;
		result.candidate = requested;
		for (int container = fileSystem.GetNumWads() - 1; container >= 0; --container)
		{
			const char* mountedSource = fileSystem.GetResourceFileFullName(container);
			const int lump = fileSystem.CheckNumForFullName(requested.virtualPath.c_str(), container);
			if (mountedSource != nullptr && DirExists(mountedSource))
			{
				const char* indexedName = lump >= 0 ? fileSystem.GetFileFullName(lump) : nullptr;
				const std::string relativeName = indexedName != nullptr && indexedName[0] != 0 ? indexedName : requested.virtualPath;
				const std::string physicalPath = JoinPhysicalPath(mountedSource, relativeName);
				if (!FileExists(physicalPath.c_str()))
				{
					continue;
				}

				result.found = true;
				result.candidate.found = true;
				result.candidate.looseFile = true;
				result.candidate.container = container;
				result.candidate.lump = lump;
				result.candidate.mountedSource = mountedSource;
				result.candidate.physicalPath = physicalPath;
				result.success = ReadPhysicalFile(physicalPath, result.text, result.error);
				return result;
			}

			if (lump >= 0)
			{
				result.found = true;
				result.candidate.found = true;
				result.candidate.container = container;
				result.candidate.lump = lump;
				result.candidate.mountedSource = mountedSource != nullptr ? mountedSource : "";
				result.success = ReadMountedLump(lump, result.text, result.error);
				return result;
			}
		}
		return result;
	}

	std::string MakeCacheKey(const VoxelPalettePolicyRequest& request)
	{
		std::string source;
		NormalizeVirtualPath(request.voxelSource, source);
		return LowerAscii(source) + "\n" + std::to_string(request.tileNumber) + "\n" +
			ComputeVoxelPalettePolicyPaletteHash(request.palette.data(), request.hasPalette ? request.palette.size() : 0);
	}
}

namespace nri_scene
{
std::string ComputeVoxelPalettePolicyPaletteHash(const uint8_t* palette, size_t paletteSize)
{
	if (palette == nullptr || paletteSize != VoxelPalettePolicyPaletteSize)
	{
		return {};
	}
	return HexHash(Fnv1a64(palette, paletteSize));
}

bool BuildVoxelPalettePolicyRequest(const FVoxelModel* voxelModel, int tileNumber, VoxelPalettePolicyRequest& outRequest)
{
	outRequest = {};
	if (voxelModel == nullptr)
	{
		return false;
	}

	const int sourceLump = voxelModel->GetVoxelSourceLump();
	const char* sourceName = sourceLump >= 0 ? fileSystem.GetFileFullName(sourceLump) : nullptr;
	if (sourceName == nullptr || sourceName[0] == 0)
	{
		return false;
	}

	outRequest.voxelSource = sourceName;
	outRequest.tileNumber = tileNumber;
	outRequest.hasPalette = voxelModel->CopyVoxelPalette(outRequest.palette.data(), outRequest.palette.size());
	return outRequest.hasPalette;
}

std::vector<VoxelPalettePolicyCandidate> BuildVoxelPalettePolicyCandidates(const std::string& voxelSource, int tileNumber)
{
	std::vector<VoxelPalettePolicyCandidate> result;
	std::string normalizedSource;
	if (NormalizeVirtualPath(voxelSource, normalizedSource))
	{
		VoxelPalettePolicyCandidate resource;
		resource.scope = VoxelPalettePolicyScope::Resource;
		resource.virtualPath = "materials/voxelpolicies/" + StripLastExtension(normalizedSource) + ".policy.json";
		result.push_back(std::move(resource));
	}
	if (tileNumber >= 0)
	{
		char tileName[64] = {};
		std::snprintf(tileName, sizeof(tileName), "materials/voxelpolicies/auto/#%05d.policy.json", tileNumber);
		VoxelPalettePolicyCandidate tile;
		tile.scope = VoxelPalettePolicyScope::Tile;
		tile.virtualPath = tileName;
		result.push_back(std::move(tile));
	}
	return result;
}

bool ParseVoxelPalettePolicy(
	const char* json,
	size_t jsonSize,
	const VoxelPalettePolicyRequest& request,
	VoxelPalettePolicyDocument& outDocument,
	std::vector<VoxelPalettePolicyDiagnostic>& outDiagnostics)
{
	outDiagnostics.clear();
	if (json == nullptr || jsonSize == 0)
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "empty-document", "Voxel palette policy is empty.");
		return false;
	}
	if (jsonSize > MaxPolicyFileSize)
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "document-too-large", "Voxel palette policy exceeds the 1 MiB size limit.");
		return false;
	}
	if (!request.hasPalette)
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "missing-palette", "The resolved voxel has no 768-byte source palette for policy validation.");
		return false;
	}

	rapidjson::Document root;
	root.Parse(json, jsonSize);
	if (root.HasParseError())
	{
		std::ostringstream message;
		message << "JSON parse error at byte " << root.GetErrorOffset() << ": " << rapidjson::GetParseError_En(root.GetParseError()) << '.';
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "json-parse", message.str());
		return false;
	}
	if (!root.IsObject())
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "root-type", "Voxel palette policy root must be an object.");
		return false;
	}
	if (!HasUniqueKnownMembers(root, { "version", "source", "paletteHash", "emissionScale", "entries" }, "Policy root", outDiagnostics))
	{
		return false;
	}

	const auto version = root.FindMember("version");
	const auto source = root.FindMember("source");
	const auto paletteHash = root.FindMember("paletteHash");
	const auto emissionScale = root.FindMember("emissionScale");
	const auto entries = root.FindMember("entries");
	if (version == root.MemberEnd() || !version->value.IsUint() || version->value.GetUint() != 1)
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "schema-version", "Policy field 'version' must be integer 1.");
		return false;
	}
	if (source == root.MemberEnd() || !source->value.IsString())
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "source-type", "Policy field 'source' must be a voxel resource path string.");
		return false;
	}
	if (paletteHash == root.MemberEnd() || !paletteHash->value.IsString())
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "palette-hash-type", "Policy field 'paletteHash' must be an FNV-1a identity string.");
		return false;
	}
	if (emissionScale == root.MemberEnd() || !emissionScale->value.IsNumber() || !std::isfinite(emissionScale->value.GetDouble()) || emissionScale->value.GetDouble() < 0.0)
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "emission-scale", "Policy field 'emissionScale' must be a finite non-negative number.");
		return false;
	}
	if (entries == root.MemberEnd() || !entries->value.IsObject())
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "entries-type", "Policy field 'entries' must be an object keyed by palette index.");
		return false;
	}

	std::string normalizedSource;
	const std::string authoredSource(source->value.GetString(), source->value.GetStringLength());
	if (!NormalizeVirtualPath(authoredSource, normalizedSource) || !SameVirtualPath(normalizedSource, request.voxelSource))
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "source-mismatch", "Policy source '" + authoredSource + "' does not match resolved voxel '" + request.voxelSource + "'.");
		return false;
	}

	const std::string expectedPaletteHash = ComputeVoxelPalettePolicyPaletteHash(request.palette.data(), request.palette.size());
	const std::string authoredPaletteHash(paletteHash->value.GetString(), paletteHash->value.GetStringLength());
	if (LowerAscii(authoredPaletteHash) != expectedPaletteHash)
	{
		AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "palette-hash-mismatch", "Policy paletteHash '" + authoredPaletteHash + "' does not match resolved palette '" + expectedPaletteHash + "'.");
		return false;
	}

	VoxelPalettePolicyDocument candidate;
	candidate.source = normalizedSource;
	candidate.paletteHash = expectedPaletteHash;
	candidate.emissionScale = emissionScale->value.GetDouble();
	std::array<bool, VoxelPalettePolicyEntryCount> seenEntries = {};
	for (auto member = entries->value.MemberBegin(); member != entries->value.MemberEnd(); ++member)
	{
		size_t paletteIndex = 0;
		if (!TryParsePaletteIndex(member->name.GetString(), member->name.GetStringLength(), paletteIndex))
		{
			AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "palette-index", "Entry key '" + std::string(member->name.GetString(), member->name.GetStringLength()) + "' is not an integer from 0 through 255.");
			return false;
		}
		if (seenEntries[paletteIndex])
		{
			AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "duplicate-index", "Palette index " + std::to_string(paletteIndex) + " is declared more than once.");
			return false;
		}
		seenEntries[paletteIndex] = true;
		if (!member->value.IsObject())
		{
			AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "entry-type", "Palette index " + std::to_string(paletteIndex) + " must contain an object.");
			return false;
		}
		const std::string context = "Palette index " + std::to_string(paletteIndex);
		if (!HasUniqueKnownMembers(member->value, { "label", "expectedRgb", "emission", "fullbright", "shadowCast", "shadowReceive" }, context.c_str(), outDiagnostics))
		{
			return false;
		}

		VoxelPalettePolicyEntry& entry = candidate.entries[paletteIndex];
		entry.authored = true;
		const auto label = member->value.FindMember("label");
		if (label != member->value.MemberEnd())
		{
			if (!label->value.IsString() || label->value.GetStringLength() > 256)
			{
				AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "label-type", context + " label must be a string no longer than 256 bytes.");
				return false;
			}
			entry.label.assign(label->value.GetString(), label->value.GetStringLength());
		}

		const auto expectedRgb = member->value.FindMember("expectedRgb");
		if (expectedRgb != member->value.MemberEnd())
		{
			entry.hasExpectedRgb = ParseRgb(expectedRgb->value, entry.expectedRgb);
			if (!entry.hasExpectedRgb)
			{
				AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "expected-rgb-type", context + " expectedRgb must use '#RRGGBB' format.");
				return false;
			}
			const uint32_t resolvedRgb = ExpandPaletteRgb(request.palette, paletteIndex);
			if (entry.expectedRgb != resolvedRgb)
			{
				AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "expected-rgb-mismatch", context + " expectedRgb " + FormatRgb(entry.expectedRgb) + " does not match resolved palette color " + FormatRgb(resolvedRgb) + ".");
				return false;
			}
		}

		bool emission = false;
		bool fullbright = false;
		bool shadowCast = true;
		bool shadowReceive = true;
		struct BooleanField { const char* name; bool* output; };
		const BooleanField booleanFields[] = {
			{ "emission", &emission },
			{ "fullbright", &fullbright },
			{ "shadowCast", &shadowCast },
			{ "shadowReceive", &shadowReceive },
		};
		for (const auto& field : booleanFields)
		{
			const auto value = member->value.FindMember(field.name);
			if (value == member->value.MemberEnd()) continue;
			if (!value->value.IsBool())
			{
				AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "boolean-type", context + " field '" + field.name + "' must be boolean.");
				return false;
			}
			*field.output = value->value.GetBool();
		}

		entry.flags = (uint8_t)((emission ? VoxelPalettePolicyFlag_EmissionEnabled : 0) |
			(fullbright ? VoxelPalettePolicyFlag_Fullbright : 0) |
			(!shadowCast ? VoxelPalettePolicyFlag_NoShadowCast : 0) |
			(!shadowReceive ? VoxelPalettePolicyFlag_NoShadowReceive : 0));
		candidate.compiled[paletteIndex] = entry.flags;
		if (fullbright && !emission)
		{
			AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Warning, "fullbright-without-emission", context + " is fullbright without emission; this is valid but unusual.");
		}
		if (fullbright && !shadowReceive)
		{
			AddDiagnostic(outDiagnostics, VoxelPalettePolicyDiagnosticSeverity::Warning, "redundant-shadow-receive", context + " disables shadow receiving while fullbright; the bit is preserved but currently redundant.");
		}
	}

	candidate.semanticContentKey = ComputeSemanticContentKey(candidate, request);
	outDocument = std::move(candidate);
	return true;
}

std::string SerializeVoxelPalettePolicy(const VoxelPalettePolicyDocument& document)
{
	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	writer.SetIndent(' ', 2);
	writer.StartObject();
	writer.Key("version");
	writer.Uint(document.version);
	writer.Key("source");
	writer.String(document.source.c_str(), (rapidjson::SizeType)document.source.size());
	writer.Key("paletteHash");
	writer.String(document.paletteHash.c_str(), (rapidjson::SizeType)document.paletteHash.size());
	writer.Key("emissionScale");
	writer.Double(document.emissionScale);
	writer.Key("entries");
	writer.StartObject();
	for (size_t index = 0; index < document.entries.size(); ++index)
	{
		const VoxelPalettePolicyEntry& entry = document.entries[index];
		if (!entry.authored)
		{
			continue;
		}
		const std::string indexText = std::to_string(index);
		writer.Key(indexText.c_str(), (rapidjson::SizeType)indexText.size());
		writer.StartObject();
		if (!entry.label.empty())
		{
			writer.Key("label");
			writer.String(entry.label.c_str(), (rapidjson::SizeType)entry.label.size());
		}
		if (entry.hasExpectedRgb)
		{
			const std::string rgb = FormatRgb(entry.expectedRgb);
			writer.Key("expectedRgb");
			writer.String(rgb.c_str(), (rapidjson::SizeType)rgb.size());
		}
		writer.Key("emission");
		writer.Bool((entry.flags & VoxelPalettePolicyFlag_EmissionEnabled) != 0);
		writer.Key("fullbright");
		writer.Bool((entry.flags & VoxelPalettePolicyFlag_Fullbright) != 0);
		writer.Key("shadowCast");
		writer.Bool((entry.flags & VoxelPalettePolicyFlag_NoShadowCast) == 0);
		writer.Key("shadowReceive");
		writer.Bool((entry.flags & VoxelPalettePolicyFlag_NoShadowReceive) == 0);
		writer.EndObject();
	}
	writer.EndObject();
	writer.EndObject();
	return std::string(buffer.GetString(), buffer.GetSize()) + '\n';
}

class VoxelPalettePolicyCache::Impl
{
public:
	VoxelPalettePolicyResolution Resolve(const VoxelPalettePolicyRequest& request, bool forceReload)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return ResolveLocked(request, forceReload);
	}

	std::vector<VoxelPalettePolicyResolution> ReloadAll()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		std::vector<VoxelPalettePolicyRequest> requests;
		requests.reserve(mEntries.size());
		for (const auto& pair : mEntries)
		{
			requests.push_back(pair.second.request);
		}
		std::vector<VoxelPalettePolicyResolution> results;
		results.reserve(requests.size());
		for (const auto& request : requests)
		{
			results.push_back(ResolveLocked(request, true));
		}
		return results;
	}

	void Invalidate(const VoxelPalettePolicyRequest& request)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mEntries.erase(MakeCacheKey(request));
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (!mEntries.empty())
		{
			++mGeneration;
		}
		mEntries.clear();
	}

	uint64_t GetGeneration() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mGeneration;
	}

private:
	struct CacheEntry
	{
		VoxelPalettePolicyRequest request;
		VoxelPalettePolicyResolution result;
	};

	VoxelPalettePolicyResolution ResolveLocked(const VoxelPalettePolicyRequest& request, bool forceReload)
	{
		const std::string key = MakeCacheKey(request);
		auto existing = mEntries.find(key);
		if (!forceReload && existing != mEntries.end())
		{
			return existing->second.result;
		}

		VoxelPalettePolicyResolution result;
		const std::shared_ptr<const VoxelPalettePolicyDocument> previousPolicy =
			existing != mEntries.end() ? existing->second.result.policy : nullptr;
		const auto requestedCandidates = BuildVoxelPalettePolicyCandidates(request.voxelSource, request.tileNumber);
		if (requestedCandidates.empty())
		{
			result.status = VoxelPalettePolicyResolveStatus::Invalid;
			AddDiagnostic(result.diagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "invalid-source", "Voxel source path cannot be mapped into materials/voxelpolicies/.");
		}
		else if (!request.hasPalette)
		{
			result.status = VoxelPalettePolicyResolveStatus::Invalid;
			AddDiagnostic(result.diagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "missing-palette", "Voxel palette policy resolution requires the raw 768-byte KVX palette.");
		}
		else
		{
			for (const auto& requestedCandidate : requestedCandidates)
			{
				PolicySourceRead source = ReadCandidate(requestedCandidate);
				result.candidates.push_back(source.candidate);
				if (!source.found)
				{
					continue;
				}
				if (!source.success)
				{
					result.status = VoxelPalettePolicyResolveStatus::Invalid;
					AddDiagnostic(result.diagnostics, VoxelPalettePolicyDiagnosticSeverity::Error, "source-read", source.error);
					break;
				}

				VoxelPalettePolicyDocument document;
				result.rawSourceHash = Fnv1a64((const uint8_t*)source.text.data(), source.text.size());
				if (!ParseVoxelPalettePolicy(source.text.data(), source.text.size(), request, document, result.diagnostics))
				{
					result.status = VoxelPalettePolicyResolveStatus::Invalid;
					break;
				}
				result.status = VoxelPalettePolicyResolveStatus::Resolved;
				result.policy = std::make_shared<const VoxelPalettePolicyDocument>(std::move(document));
				AddDiagnostic(result.diagnostics, VoxelPalettePolicyDiagnosticSeverity::Info, "resolved", "Resolved voxel palette policy from '" + source.candidate.virtualPath + "'.");
				break;
			}
		}

		if (result.status == VoxelPalettePolicyResolveStatus::Invalid && previousPolicy != nullptr)
		{
			result.policy = previousPolicy;
			result.usingLastValid = true;
			AddDiagnostic(result.diagnostics, VoxelPalettePolicyDiagnosticSeverity::Warning, "using-last-valid", "Policy reload failed; the last valid compiled policy remains active.");
		}

		const uint64_t previousKey = previousPolicy != nullptr ? previousPolicy->semanticContentKey : 0;
		const uint64_t resolvedKey = result.policy != nullptr ? result.policy->semanticContentKey : 0;
		if (result.status != VoxelPalettePolicyResolveStatus::Invalid && previousKey != resolvedKey)
		{
			++mGeneration;
		}
		result.generation = mGeneration;
		mEntries[key] = { request, result };
		return result;
	}

	mutable std::mutex mMutex;
	std::unordered_map<std::string, CacheEntry> mEntries;
	uint64_t mGeneration = 0;
};

VoxelPalettePolicyCache::VoxelPalettePolicyCache()
	: mImpl(std::make_unique<Impl>())
{
}

VoxelPalettePolicyCache::~VoxelPalettePolicyCache() = default;

VoxelPalettePolicyResolution VoxelPalettePolicyCache::Resolve(const VoxelPalettePolicyRequest& request)
{
	return mImpl->Resolve(request, false);
}

VoxelPalettePolicyResolution VoxelPalettePolicyCache::Reload(const VoxelPalettePolicyRequest& request)
{
	return mImpl->Resolve(request, true);
}

std::vector<VoxelPalettePolicyResolution> VoxelPalettePolicyCache::ReloadAll()
{
	return mImpl->ReloadAll();
}

void VoxelPalettePolicyCache::Invalidate(const VoxelPalettePolicyRequest& request)
{
	mImpl->Invalidate(request);
}

void VoxelPalettePolicyCache::Clear()
{
	mImpl->Clear();
}

uint64_t VoxelPalettePolicyCache::GetGeneration() const
{
	return mImpl->GetGeneration();
}

VoxelPalettePolicyCache& GetVoxelPalettePolicyCache()
{
	static VoxelPalettePolicyCache cache;
	return cache;
}
}
