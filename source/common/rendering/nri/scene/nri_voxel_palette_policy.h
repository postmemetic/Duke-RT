#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FVoxelModel;

namespace nri_scene
{
constexpr size_t VoxelPalettePolicyEntryCount = 256;
constexpr size_t VoxelPalettePolicyPaletteSize = VoxelPalettePolicyEntryCount * 3;

enum VoxelPalettePolicyFlag : uint8_t
{
	VoxelPalettePolicyFlag_None = 0,
	VoxelPalettePolicyFlag_EmissionEnabled = 1u << 0,
	VoxelPalettePolicyFlag_Fullbright = 1u << 1,
	VoxelPalettePolicyFlag_NoShadowCast = 1u << 2,
	VoxelPalettePolicyFlag_NoShadowReceive = 1u << 3,
};

enum class VoxelPalettePolicyDiagnosticSeverity
{
	Info,
	Warning,
	Error,
};

struct VoxelPalettePolicyDiagnostic
{
	VoxelPalettePolicyDiagnosticSeverity severity = VoxelPalettePolicyDiagnosticSeverity::Info;
	std::string code;
	std::string message;
};

struct VoxelPalettePolicyEntry
{
	bool authored = false;
	std::string label;
	bool hasExpectedRgb = false;
	uint32_t expectedRgb = 0;
	uint8_t flags = VoxelPalettePolicyFlag_None;
};

struct VoxelPalettePolicyDocument
{
	uint32_t version = 1;
	std::string source;
	std::string paletteHash;
	double emissionScale = 1.0;
	std::array<VoxelPalettePolicyEntry, VoxelPalettePolicyEntryCount> entries = {};
	std::array<uint8_t, VoxelPalettePolicyEntryCount> compiled = {};
	uint64_t semanticContentKey = 0;
};

struct VoxelPalettePolicyRequest
{
	std::string voxelSource;
	int tileNumber = -1;
	std::array<uint8_t, VoxelPalettePolicyPaletteSize> palette = {};
	bool hasPalette = false;
};

enum class VoxelPalettePolicyScope
{
	Resource,
	Tile,
};

struct VoxelPalettePolicyCandidate
{
	VoxelPalettePolicyScope scope = VoxelPalettePolicyScope::Resource;
	std::string virtualPath;
	bool found = false;
	bool looseFile = false;
	int container = -1;
	int lump = -1;
	std::string mountedSource;
	std::string physicalPath;
};

enum class VoxelPalettePolicyResolveStatus
{
	NotFound,
	Resolved,
	Invalid,
};

struct VoxelPalettePolicyResolution
{
	VoxelPalettePolicyResolveStatus status = VoxelPalettePolicyResolveStatus::NotFound;
	bool usingLastValid = false;
	uint64_t generation = 0;
	uint64_t rawSourceHash = 0;
	std::shared_ptr<const VoxelPalettePolicyDocument> policy;
	std::vector<VoxelPalettePolicyCandidate> candidates;
	std::vector<VoxelPalettePolicyDiagnostic> diagnostics;
};

std::string ComputeVoxelPalettePolicyPaletteHash(const uint8_t* palette, size_t paletteSize);
bool BuildVoxelPalettePolicyRequest(const FVoxelModel* voxelModel, int tileNumber, VoxelPalettePolicyRequest& outRequest);
std::vector<VoxelPalettePolicyCandidate> BuildVoxelPalettePolicyCandidates(const std::string& voxelSource, int tileNumber);

bool ParseVoxelPalettePolicy(
	const char* json,
	size_t jsonSize,
	const VoxelPalettePolicyRequest& request,
	VoxelPalettePolicyDocument& outDocument,
	std::vector<VoxelPalettePolicyDiagnostic>& outDiagnostics);

std::string SerializeVoxelPalettePolicy(const VoxelPalettePolicyDocument& document);

class VoxelPalettePolicyCache
{
public:
	VoxelPalettePolicyCache();
	~VoxelPalettePolicyCache();

	VoxelPalettePolicyCache(const VoxelPalettePolicyCache&) = delete;
	VoxelPalettePolicyCache& operator=(const VoxelPalettePolicyCache&) = delete;

	VoxelPalettePolicyResolution Resolve(const VoxelPalettePolicyRequest& request);
	VoxelPalettePolicyResolution Reload(const VoxelPalettePolicyRequest& request);
	std::vector<VoxelPalettePolicyResolution> ReloadAll();
	void Invalidate(const VoxelPalettePolicyRequest& request);
	void Clear();
	uint64_t GetGeneration() const;

private:
	class Impl;
	std::unique_ptr<Impl> mImpl;
};

VoxelPalettePolicyCache& GetVoxelPalettePolicyCache();
}
