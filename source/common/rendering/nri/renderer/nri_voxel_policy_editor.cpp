#include "nri_renderer.h"

#include "../scene/nri_hash.h"
#include "../scene/nri_voxel_material_slots.h"
#include "../scene/nri_voxel_palette_policy.h"

#include "cmdlib.h"
#include "filesystem.h"
#include "model_kvx.h"
#include "v_video.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace
{
	struct VoxelProbeMesh
	{
		std::vector<std::array<float, 3>> vertices;
		std::vector<uint32_t> indices;
		std::vector<uint8_t> primitiveSlots;
		std::array<uint8_t, 256> used = {};
		uint32_t usedCount = 0;
	};

	std::unordered_map<const FVoxelModel*, std::shared_ptr<const VoxelProbeMesh>> gVoxelProbeMeshes;

	std::shared_ptr<const VoxelProbeMesh> GetVoxelProbeMesh(const FVoxelModel* model)
	{
		auto found = gVoxelProbeMeshes.find(model);
		if (found != gVoxelProbeMeshes.end())
		{
			return found->second;
		}
		if (model == nullptr)
		{
			return nullptr;
		}

		FVoxelMeshData source;
		model->BuildCpuMesh(source);
		auto mesh = std::make_shared<VoxelProbeMesh>();
		mesh->vertices.reserve(source.vertices.Size());
		for (const FModelVertex& vertex : source.vertices)
		{
			mesh->vertices.push_back({ vertex.x, vertex.y, vertex.z });
		}
		mesh->indices.assign(source.indices.begin(), source.indices.end());
		mesh->primitiveSlots.reserve(mesh->indices.size() / 3u);
		for (size_t i = 0; i + 2u < mesh->indices.size(); i += 3u)
		{
			const uint32_t vertexIndex = mesh->indices[i];
			uint8_t slot = 0;
			if (vertexIndex < source.vertices.Size())
			{
				nri_scene::DecodeVoxelPaletteMaterialSlot(source.vertices[vertexIndex].u, source.vertices[vertexIndex].v, slot);
			}
			mesh->primitiveSlots.push_back(slot);
			if (mesh->used[slot] == 0)
			{
				mesh->used[slot] = 1;
				++mesh->usedCount;
			}
		}
		gVoxelProbeMeshes[model] = mesh;
		return mesh;
	}

	std::array<float, 3> TransformPoint(const std::array<float, 12>& transform, const std::array<float, 3>& point)
	{
		return {
			transform[0] * point[0] + transform[1] * point[1] + transform[2] * point[2] + transform[3],
			transform[4] * point[0] + transform[5] * point[1] + transform[6] * point[2] + transform[7],
			transform[8] * point[0] + transform[9] * point[1] + transform[10] * point[2] + transform[11]
		};
	}

	std::array<float, 3> Subtract(const std::array<float, 3>& a, const std::array<float, 3>& b)
	{
		return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
	}

	std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b)
	{
		return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	}

	float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b)
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	}

	bool IntersectTriangle(
		const std::array<float, 3>& origin,
		const std::array<float, 3>& direction,
		const std::array<float, 3>& v0,
		const std::array<float, 3>& v1,
		const std::array<float, 3>& v2,
		float& outDistance)
	{
		constexpr float epsilon = 1.0e-6f;
		const auto edge1 = Subtract(v1, v0);
		const auto edge2 = Subtract(v2, v0);
		const auto p = Cross(direction, edge2);
		const float determinant = Dot(edge1, p);
		if (std::abs(determinant) <= epsilon)
		{
			return false;
		}
		const float inverse = 1.0f / determinant;
		const auto t = Subtract(origin, v0);
		const float u = Dot(t, p) * inverse;
		if (u < 0.0f || u > 1.0f)
		{
			return false;
		}
		const auto q = Cross(t, edge1);
		const float v = Dot(direction, q) * inverse;
		if (v < 0.0f || u + v > 1.0f)
		{
			return false;
		}
		const float distance = Dot(edge2, q) * inverse;
		if (distance <= epsilon)
		{
			return false;
		}
		outDistance = distance;
		return true;
	}

	uint32_t ExpandedPaletteRgb(const nri_scene::VoxelPalettePolicyRequest& request, uint32_t index)
	{
		const size_t offset = (size_t)index * 3u;
		const uint32_t r = (request.palette[offset] << 2u) | (request.palette[offset] >> 4u);
		const uint32_t g = (request.palette[offset + 1u] << 2u) | (request.palette[offset + 1u] >> 4u);
		const uint32_t b = (request.palette[offset + 2u] << 2u) | (request.palette[offset + 2u] >> 4u);
		return (r << 16u) | (g << 8u) | b;
	}

	void CopyRgb(uint32_t rgb, uint8_t target[3])
	{
		target[0] = (uint8_t)(rgb >> 16u);
		target[1] = (uint8_t)(rgb >> 8u);
		target[2] = (uint8_t)rgb;
	}

	bool FindWritablePolicyPath(
		const nri_scene::VoxelPalettePolicyRequest& request,
		const nri_scene::VoxelPalettePolicyResolution& resolution,
		std::string& outPath,
		std::string& outError)
	{
		for (const auto& candidate : resolution.candidates)
		{
			if (candidate.found)
			{
				if (!candidate.looseFile || candidate.physicalPath.empty())
				{
					outError = "The active policy is archive-backed and cannot be edited in place.";
					return false;
				}
				outPath = candidate.physicalPath;
				return true;
			}
		}

		const auto candidates = nri_scene::BuildVoxelPalettePolicyCandidates(request.voxelSource, request.tileNumber);
		if (candidates.empty())
		{
			outError = "The voxel resource cannot be mapped to a policy sidecar path.";
			return false;
		}
		for (int container = fileSystem.GetNumWads() - 1; container >= 0; --container)
		{
			const char* root = fileSystem.GetResourceFileFullName(container);
			if (root != nullptr && DirExists(root))
			{
				outPath = (std::filesystem::path(root) / std::filesystem::path(candidates.front().virtualPath)).string();
				return true;
			}
		}
		outError = "No writable loose mounted overlay is available for policy creation.";
		return false;
	}

	bool WritePolicyAtomically(const std::string& path, const std::string& text, std::string& outError)
	{
		std::error_code error;
		const std::filesystem::path target(path);
		std::filesystem::create_directories(target.parent_path(), error);
		if (error)
		{
			outError = "Could not create the policy directory: " + error.message();
			return false;
		}
		const std::filesystem::path temporary = target.string() + ".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream || !stream.write(text.data(), (std::streamsize)text.size()) || !stream.flush())
			{
				outError = "Could not write the temporary policy file.";
				return false;
			}
		}
#ifdef _WIN32
		if (!MoveFileExA(temporary.string().c_str(), target.string().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			outError = "Could not atomically replace the policy file.";
			std::filesystem::remove(temporary, error);
			return false;
		}
#else
		std::filesystem::rename(temporary, target, error);
		if (error)
		{
			outError = "Could not atomically replace the policy file: " + error.message();
			std::filesystem::remove(temporary, error);
			return false;
		}
#endif
		return true;
	}

	const PersistentVoxelBatch::ActorEntry* FindEditableActor(
		const NRIPersistentVoxelResidency& voxels,
		const PathTracingVoxelPolicyEditRequest& request)
	{
		for (const auto& actor : voxels.batch.actors)
		{
			if (actor.actorIndex == request.actorIndex && actor.sourcePicnum == request.sourcePicnum &&
				actor.resolvedVoxelIndex == request.resolvedVoxelIndex && actor.active && actor.authorityCurrent &&
				actor.publicationEligible && !actor.pendingRemoval && !actor.indirectOnly)
			{
				return &actor;
			}
		}
		return nullptr;
	}
}

bool NRIRenderer::BuildVoxelPolicyEditTarget(PathTracingVoxelPolicyEditTarget& outTarget) const
{
	outTarget = {};
	const std::array<float, 3> origin = { mCurrentCameraPos[0], mCurrentCameraPos[1], mCurrentCameraPos[2] };
	std::array<float, 3> direction = { mCurrentCameraForward[0], mCurrentCameraForward[1], mCurrentCameraForward[2] };
	const float directionLength = std::sqrt(Dot(direction, direction));
	if (!(directionLength > 0.0f))
	{
		outTarget.failureReason = "Voxel policy probe has no valid camera ray.";
		return false;
	}
	for (float& component : direction) component /= directionLength;

	float nearestDistance = std::numeric_limits<float>::max();
	const PersistentVoxelBatch::ActorEntry* nearestActor = nullptr;
	const PersistentVoxelMeshVariantResource* nearestResource = nullptr;
	std::shared_ptr<const VoxelProbeMesh> nearestMesh;
	uint32_t nearestPrimitive = UINT32_MAX;
	for (const auto& actor : mPersistentVoxels.batch.actors)
	{
		if (!actor.active || !actor.authorityCurrent || !actor.publicationEligible || actor.pendingRemoval || actor.indirectOnly)
		{
			continue;
		}
		auto resourceIt = mPersistentVoxels.meshVariantResources.find(actor.meshResourceKey);
		if (resourceIt == mPersistentVoxels.meshVariantResources.end() || resourceIt->second.sourceModel == nullptr)
		{
			continue;
		}
		const auto mesh = GetVoxelProbeMesh(resourceIt->second.sourceModel);
		if (mesh == nullptr)
		{
			continue;
		}
		for (uint32_t primitive = 0; (size_t)primitive * 3u + 2u < mesh->indices.size(); ++primitive)
		{
			const uint32_t i0 = mesh->indices[(size_t)primitive * 3u];
			const uint32_t i1 = mesh->indices[(size_t)primitive * 3u + 1u];
			const uint32_t i2 = mesh->indices[(size_t)primitive * 3u + 2u];
			if (i0 >= mesh->vertices.size() || i1 >= mesh->vertices.size() || i2 >= mesh->vertices.size()) continue;
			float distance = 0.0f;
			if (IntersectTriangle(origin, direction,
				TransformPoint(actor.instanceTransform, mesh->vertices[i0]),
				TransformPoint(actor.instanceTransform, mesh->vertices[i1]),
				TransformPoint(actor.instanceTransform, mesh->vertices[i2]), distance) && distance < nearestDistance)
			{
				nearestDistance = distance;
				nearestActor = &actor;
				nearestResource = &resourceIt->second;
				nearestMesh = mesh;
				nearestPrimitive = primitive;
			}
		}
	}
	if (nearestActor == nullptr || nearestResource == nullptr || nearestMesh == nullptr || nearestPrimitive >= nearestMesh->primitiveSlots.size())
	{
		outTarget.failureReason = "Aim at a resident voxel actor surface.";
		return false;
	}
	const NRISurfaceProbeResult& ordinarySurface = mSurfaceProbe.Last();
	if (ordinarySurface.valid && ordinarySurface.hit && ordinarySurface.distance > 0.0f && ordinarySurface.distance + 0.01f < nearestDistance)
	{
		outTarget.failureReason = "A map surface is in front of the nearest voxel actor.";
		return false;
	}

	nri_scene::VoxelPalettePolicyRequest request;
	if (!nri_scene::BuildVoxelPalettePolicyRequest(nearestResource->sourceModel, nearestActor->sourcePicnum, request))
	{
		outTarget.failureReason = "The aimed voxel has no resolvable source palette.";
		return false;
	}
	const nri_scene::VoxelPalettePolicyResolution resolution = nri_scene::GetVoxelPalettePolicyCache().Resolve(request);
	const uint32_t paletteIndex = nearestMesh->primitiveSlots[nearestPrimitive];
	outTarget.valid = true;
	outTarget.hit = true;
	outTarget.actorIndex = nearestActor->actorIndex;
	outTarget.sourcePicnum = nearestActor->sourcePicnum;
	outTarget.resolvedVoxelIndex = nearestActor->resolvedVoxelIndex;
	outTarget.paletteIndex = paletteIndex;
	outTarget.meshVariantHash = nearestActor->meshVariantHash;
	outTarget.materialVariantHash = nearestActor->materialVariantHash;
	outTarget.sourceHash = nearestResource->geometryContentHash;
	outTarget.paletteHash = nri_scene::Fnv1a64(request.palette.data(), request.palette.size());
	outTarget.voxelResource = request.voxelSource.c_str();
	const uint32_t rgb = ExpandedPaletteRgb(request, paletteIndex);
	CopyRgb(rgb, outTarget.rawPaletteRgb);
	CopyRgb(rgb, outTarget.resolvedPaletteRgb);
	for (uint32_t index = 0; index < 256u; ++index)
	{
		CopyRgb(ExpandedPaletteRgb(request, index), outTarget.rawPaletteRgbByIndex[index]);
	}
	outTarget.usedPaletteIndexCount = nearestMesh->usedCount;
	uint32_t usedCursor = 0;
	for (uint32_t index = 0; index < 256u; ++index)
	{
		if (nearestMesh->used[index] != 0) outTarget.usedPaletteIndices[usedCursor++] = (uint8_t)index;
	}
	std::array<float, 3> selectedMin = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
	std::array<float, 3> selectedMax = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
	bool selectedBoundsValid = false;
	for (uint32_t primitive = 0; primitive < nearestMesh->primitiveSlots.size(); ++primitive)
	{
		if (nearestMesh->primitiveSlots[primitive] != paletteIndex) continue;
		for (uint32_t corner = 0; corner < 3u; ++corner)
		{
			const size_t indexOffset = (size_t)primitive * 3u + corner;
			if (indexOffset >= nearestMesh->indices.size()) continue;
			const uint32_t vertexIndex = nearestMesh->indices[indexOffset];
			if (vertexIndex >= nearestMesh->vertices.size()) continue;
			const auto& point = nearestMesh->vertices[vertexIndex];
			for (uint32_t axis = 0; axis < 3u; ++axis)
			{
				selectedMin[axis] = std::min(selectedMin[axis], point[axis]);
				selectedMax[axis] = std::max(selectedMax[axis], point[axis]);
			}
			selectedBoundsValid = true;
		}
	}
	if (selectedBoundsValid)
	{
		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			outTarget.selectedCellBoundsMin[axis] = (int32_t)std::floor(selectedMin[axis]);
			outTarget.selectedCellBoundsMax[axis] = (int32_t)std::ceil(selectedMax[axis]);
		}
		outTarget.selectedCellComponentCount = 1;
	}
	if (resolution.policy != nullptr)
	{
		std::copy(resolution.policy->compiled.begin(), resolution.policy->compiled.end(), outTarget.policyByPaletteIndex);
		outTarget.policyBits = resolution.policy->compiled[paletteIndex];
		outTarget.policyContentHash = resolution.policy->semanticContentKey;
		for (const auto& candidate : resolution.candidates)
		{
			if (candidate.found)
			{
				outTarget.policySource = candidate.virtualPath.c_str();
				break;
			}
		}
	}
	return true;
}

bool NRIRenderer::ApplyVoxelPolicyEdit(
	const PathTracingVoxelPolicyEditRequest& edit,
	PathTracingVoxelPolicyEditResult& outResult)
{
	outResult = {};
	const PersistentVoxelBatch::ActorEntry* actor = FindEditableActor(mPersistentVoxels, edit);
	if (actor == nullptr)
	{
		outResult.message = "The selected voxel actor is no longer current; aim again.";
		return false;
	}
	auto resourceIt = mPersistentVoxels.meshVariantResources.find(actor->meshResourceKey);
	if (resourceIt == mPersistentVoxels.meshVariantResources.end() || resourceIt->second.sourceModel == nullptr)
	{
		outResult.message = "The selected voxel mesh resource is no longer resident.";
		return false;
	}
	nri_scene::VoxelPalettePolicyRequest request;
	if (!nri_scene::BuildVoxelPalettePolicyRequest(resourceIt->second.sourceModel, actor->sourcePicnum, request))
	{
		outResult.message = "The selected voxel source or palette cannot be resolved.";
		return false;
	}
	if (edit.paletteIndex >= 256u || edit.sourceHash != resourceIt->second.geometryContentHash ||
		edit.paletteHash != nri_scene::Fnv1a64(request.palette.data(), request.palette.size()))
	{
		outResult.message = "The selected voxel source changed; aim again before editing.";
		return false;
	}

	auto& cache = nri_scene::GetVoxelPalettePolicyCache();
	const nri_scene::VoxelPalettePolicyResolution current = cache.Reload(request);
	if (current.status == nri_scene::VoxelPalettePolicyResolveStatus::Invalid)
	{
		outResult.message = "Policy reload failed; the last valid policy was preserved and no file was changed.";
		return false;
	}
	if (edit.action == PathTracingVoxelPolicyEditAction::Reload || edit.action == PathTracingVoxelPolicyEditAction::DiscardAndReload)
	{
		outResult.reloaded = true;
		outResult.policyBits = current.policy != nullptr ? current.policy->compiled[edit.paletteIndex] : 0u;
		outResult.message = current.policy != nullptr ? "Policy reloaded." : "No policy sidecar is currently resolved.";
		return true;
	}
	if (current.policy != nullptr && edit.contentHash != current.policy->semanticContentKey)
	{
		outResult.message = "The policy changed since selection; reload or aim again before writing.";
		return false;
	}

	nri_scene::VoxelPalettePolicyDocument document;
	if (current.policy != nullptr)
	{
		document = *current.policy;
	}
	else
	{
		document.source = request.voxelSource;
		document.paletteHash = nri_scene::ComputeVoxelPalettePolicyPaletteHash(request.palette.data(), request.palette.size());
	}
	if (edit.action != PathTracingVoxelPolicyEditAction::CreateSidecar)
	{
		auto& entry = document.entries[edit.paletteIndex];
		entry.authored = true;
		entry.hasExpectedRgb = true;
		entry.expectedRgb = ExpandedPaletteRgb(request, edit.paletteIndex);
		switch (edit.action)
		{
		case PathTracingVoxelPolicyEditAction::ToggleEmission: entry.flags ^= nri_scene::VoxelPalettePolicyFlag_EmissionEnabled; break;
		case PathTracingVoxelPolicyEditAction::ToggleFullbright: entry.flags ^= nri_scene::VoxelPalettePolicyFlag_Fullbright; break;
		case PathTracingVoxelPolicyEditAction::ToggleShadowCast: entry.flags ^= nri_scene::VoxelPalettePolicyFlag_NoShadowCast; break;
		case PathTracingVoxelPolicyEditAction::ToggleShadowReceive: entry.flags ^= nri_scene::VoxelPalettePolicyFlag_NoShadowReceive; break;
		case PathTracingVoxelPolicyEditAction::AdjustEmissionScale:
			document.emissionScale = std::max(0.0, document.emissionScale + (double)edit.emissionScaleDelta);
			break;
		default: break;
		}
	}
	std::string path;
	std::string error;
	if (!FindWritablePolicyPath(request, current, path, error))
	{
		outResult.message = error.c_str();
		return false;
	}
	if (!WritePolicyAtomically(path, nri_scene::SerializeVoxelPalettePolicy(document), error))
	{
		outResult.message = error.c_str();
		return false;
	}
	const nri_scene::VoxelPalettePolicyResolution reloaded = cache.Reload(request);
	if (reloaded.status != nri_scene::VoxelPalettePolicyResolveStatus::Resolved || reloaded.policy == nullptr)
	{
		outResult.message = "The sidecar was written but failed validation on reload; the last valid runtime policy remains active.";
		outResult.writablePath = path.c_str();
		return false;
	}
	outResult.changed = true;
	outResult.reloaded = true;
	outResult.policyBits = reloaded.policy->compiled[edit.paletteIndex];
	outResult.writablePath = path.c_str();
	outResult.message = "Policy written and reloaded.";
	return true;
}
