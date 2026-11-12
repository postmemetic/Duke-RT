#pragma once

#include "nri_frame_resources.h"
#include "nri_diagnostic_names.h"
#include "nri_world_tlas_slots.h"
#include "nri_runtime_mutation_cause_trace.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_hash.h"
#include "printf.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <limits>
#include <vector>

// Passive diagnostic snapshots of the chosen CPU submission, never a producer,
// reference builder, discovery source, or GPU-memory/pixel oracle. In particular,
// do not build a live map view here: that may update engine geometry caches.
class NRIRuntimeMutationPublicationTrace
{
public:
	struct Context
	{
		uint64_t epoch = 0, build = 0, frame = 0, tick = 0;
		uint64_t simulation = 0, presentation = 0;
		uint64_t vertexBufferHash = 0, primitiveBufferHash = 0, materialBufferHash = 0;
		uint32_t frameSlot = 0;
		int focusChunk = -1;
		const char* focusList = "";
		bool selectedStatic = false;
		const std::vector<uint32_t>* visibleWords = nullptr;
	};
	struct Replacement
	{
		uint64_t baseline = 0, preparedBaseline = 0, revision = 0;
		uint64_t materialDeferredFrame = 0, structuralDeferredFrame = 0;
		uint32_t source = 0, reason = 0;
		bool valid = false, active = false, authoritative = false;
		bool materialDeferred = false, structuralDeferred = false;
	};
	struct CommitChunk { uint32_t chunk = 0; bool material = false, geometry = false; };
	template<class StaticScene>
	static std::vector<CommitChunk> CaptureCommitChunks(const StaticScene& scene,
		const std::vector<uint32_t>& materials, const std::vector<uint32_t>& geometry, int focusChunk, const char* focusList)
	{
		std::vector<CommitChunk> result;
		const auto append = [&](uint32_t index, bool material)
		{
			if (index >= scene.chunks.size()) return;
			const uint32_t chunk = scene.chunks[index].chunkIndex;
			if (!nri_mutation_cause::IsFocusedChunk(chunk, focusChunk, focusList)) return;
			auto it = std::find_if(result.begin(), result.end(), [&](const CommitChunk& item) { return item.chunk == chunk; });
			if (it == result.end()) { result.push_back({ chunk }); it = result.end() - 1; }
			(material ? it->material : it->geometry) = true;
		};
		for (uint32_t index : materials) append(index, true);
		for (uint32_t index : geometry) append(index, false);
		return result;
	}
	static void TraceCommit(uint64_t epoch, uint64_t build, uint64_t frame, uint64_t tick,
		const std::vector<CommitChunk>& chunks, bool success, const char* failureReason)
	{
		for (const auto& chunk : chunks)
			Printf("PERF pt mutation publication commit NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu chunk=%u material=%u geometry=%u success=%u failure=%s boundary=resident_upload_and_blas_recording selected_tlas_proven=0\n",
				(unsigned long long)epoch, (unsigned long long)build, (unsigned long long)frame,
				(unsigned long long)(frame + 1), (unsigned long long)tick, chunk.chunk, chunk.material, chunk.geometry,
				success, failureReason != nullptr ? failureReason : "none");
	}

	void Reset()
	{
		mEpoch = mBuild = mLastFrame = 0;
		mChunks.clear();
	}

	// World/static/registry types are dependent to keep their ownership headers
	// out of the mutation header's include graph.
	template<class World, class StaticScene, class Atlas, class Registry, class ReadReplacement>
	void TraceSelected(const Context& context, const World& world, const StaticScene& scene,
		const Atlas& atlas, const Registry& registry, const std::vector<SceneInstanceData>& bound,
		const NRIWorldTlasFrameSlot& tlas, const nri_scene::GeometryData& runtimeLink,
		const nri_scene::GeometryData& runtimeMutation, const nri_scene::GeometryData* dynamic,
		ReadReplacement readReplacement)
	{
		if (mEpoch != context.epoch || mBuild != context.build || mChunks.size() != world.chunks.size() || context.frame < mLastFrame)
			mChunks.assign(world.chunks.size(), {});
		mEpoch = context.epoch; mBuild = context.build; mLastFrame = context.frame;
		uint32_t visited = 0, emitted = 0, missingSlices = 0, invalidTlasIds = 0;
		for (const auto& instance : tlas.publishedInstances)
			invalidTlasIds += instance.instanceId >= bound.size();
		for (uint32_t index = 0; index < world.chunks.size(); ++index)
		{
			const auto& mapChunk = world.chunks[index];
			const uint32_t chunk = mapChunk.chunkIndex;
			if (!nri_mutation_cause::IsFocusedChunk(chunk, context.focusChunk, context.focusList)) continue;
			++visited;
			const Replacement replacement = readReplacement(index);
			const auto* entry = chunk < registry.entries.size() ? &registry.entries[chunk] : nullptr;
			const uint32_t slot = entry != nullptr ? entry->staticSceneChunkListIndex : UINT32_MAX;
			const auto* slice = slot < atlas.chunks.size() ? &atlas.chunks[slot] : nullptr;
			const bool sliceValid = context.selectedStatic && entry != nullptr && entry->valid && entry->active &&
				entry->mappedInStaticScene && slice != nullptr && slice->valid && slice->chunkIndex == chunk &&
				Range(slice->primitiveOffset, slice->primitiveCount, scene.geometry.primitives.size()) &&
				Range(slice->materialOffset, slice->materialCount, scene.gpuMaterials.size()) &&
				Range(slice->materialOffset, slice->materialCount, scene.materialBridge.lightMetadata.size());
			missingSlices += !sliceValid;
			uint64_t instanceHash = Seed, geometryHash = Seed, gpuHash = Seed, semanticHash = Seed;
			uint32_t instances = 0, masks = 0, baseMatches = 0;
			uint64_t firstBlas = 0;
			for (const auto& instance : tlas.publishedInstances)
			{
				if (instance.instanceId >= bound.size()) continue;
				const auto& record = bound[instance.instanceId];
				if (record.dataSource != nri_diag::SceneDataSourceStatic || record.metadata0 != chunk) continue;
				++instances; masks |= instance.mask;
				baseMatches += slice != nullptr && record.primitiveBase == slice->primitiveOffset;
				if (firstBlas == 0) firstBlas = instance.accelerationStructureHandle;
				Hash(instanceHash, &record, sizeof(record));
				Hash(instanceHash, &instance, sizeof(instance));
			}
			std::vector<Row> rows;
			uint32_t invalidGeometry = 0;
			if (sliceValid)
			{
				rows.resize(slice->materialCount);
				for (uint32_t i = 0; i < slice->materialCount; ++i)
				{
					const uint32_t material = slice->materialOffset + i;
					const auto& gpu = scene.gpuMaterials[material];
					const auto& metadata = scene.materialBridge.lightMetadata[material];
					Hash(gpuHash, &gpu, sizeof(gpu));
					Hash(rows[i].hash, &gpu, sizeof(gpu));
					const uint64_t identity[] = { (uint64_t)metadata.sourceType, (uint64_t)(uint32_t)metadata.sectorIndex,
						metadata.authoredTextureId, metadata.textureId, (uint64_t)(uint32_t)metadata.shade, metadata.paletteIndex };
					Hash(semanticHash, identity, sizeof(identity));
					Hash(rows[i].hash, identity, sizeof(identity));
				}
				for (uint32_t i = 0; i < slice->primitiveCount; ++i)
				{
					const uint32_t primitiveIndex = slice->primitiveOffset + i;
					const auto& primitive = scene.geometry.primitives[primitiveIndex];
					if (primitive.materialIndex < slice->materialOffset || primitive.materialIndex - slice->materialOffset >= rows.size())
					{ ++invalidGeometry; continue; }
					auto& row = rows[primitive.materialIndex - slice->materialOffset];
					++row.primitives;
					Hash(geometryHash, &primitive, sizeof(primitive));
					Hash(row.hash, &primitive, sizeof(primitive));
					if (primitiveIndex < scene.geometry.primitiveProvenance.size())
					{
						const auto& provenance = scene.geometry.primitiveProvenance[primitiveIndex];
						const uint64_t provenanceFields[] = { (uint64_t)provenance.sourceType, (uint64_t)(uint32_t)provenance.sectorIndex,
							(uint64_t)(uint32_t)provenance.wallIndex, (uint64_t)(uint32_t)provenance.sectionIndex,
							(uint64_t)(uint32_t)provenance.mapChunkIndex, provenance.cstat, provenance.materialFlags };
						Hash(geometryHash, provenanceFields, sizeof(provenanceFields));
						if (row.primitives == 1) { row.wall = provenance.wallIndex; row.section = provenance.sectionIndex; }
						Hash(row.hash, &provenance.wallIndex, sizeof(provenance.wallIndex));
						Hash(row.hash, &provenance.sectionIndex, sizeof(provenance.sectionIndex));
						row.provenanceMismatch |= provenance.mapChunkIndex != (int32_t)chunk;
					}
					else ++invalidGeometry;
					for (uint32_t corner = 0; corner < 3; ++corner)
					{
						if (primitive.indices[corner] >= scene.geometry.vertices.size()) { ++invalidGeometry; continue; }
						const auto& vertex = scene.geometry.vertices[primitive.indices[corner]];
						Hash(geometryHash, vertex.position, sizeof(vertex.position));
						Hash(geometryHash, vertex.uv, sizeof(vertex.uv));
						Hash(row.hash, vertex.position, sizeof(vertex.position));
						Hash(row.hash, vertex.uv, sizeof(vertex.uv));
						for (uint32_t axis = 0; axis < 3; ++axis)
						{ row.minimum[axis] = std::min(row.minimum[axis], vertex.position[axis]); row.maximum[axis] = std::max(row.maximum[axis], vertex.position[axis]); }
						for (uint32_t axis = 0; axis < 2; ++axis)
						{ row.uvMinimum[axis] = std::min(row.uvMinimum[axis], vertex.uv[axis]); row.uvMaximum[axis] = std::max(row.uvMaximum[axis], vertex.uv[axis]); }
					}
				}
			}
			const uint32_t links = CountProvenance(runtimeLink, chunk), mutations = CountProvenance(runtimeMutation, chunk);
			const uint32_t dynamics = dynamic != nullptr ? CountProvenance(*dynamic, chunk) : 0u;
			const bool visible = context.visibleWords != nullptr && chunk / 32u < context.visibleWords->size() &&
				((*context.visibleWords)[chunk / 32u] & (1u << (chunk % 32u))) != 0;
			uint64_t stateHash = Seed;
			const uint64_t identity[] = { replacement.baseline, replacement.preparedBaseline, replacement.revision,
				replacement.source, replacement.reason, replacement.valid, replacement.active, replacement.authoritative,
				replacement.materialDeferred, replacement.structuralDeferred, replacement.materialDeferredFrame, replacement.structuralDeferredFrame,
				sliceValid, slot, instances, masks, baseMatches, instanceHash, geometryHash, gpuHash, semanticHash,
				links, mutations, dynamics, visible, invalidGeometry, scene.contentBuildSerial,
				entry != nullptr ? entry->baselineSignature : 0 };
			Hash(stateHash, identity, sizeof(identity));
			auto& previous = mChunks[index];
			if (previous.hash == stateHash) continue;
			++emitted;
			Printf("PERF pt mutation publication NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu simulation=%llu presentation=%llu chunk=%u sector=%d stage=selected_cpu_submission visible=%u source=0x%x reason=0x%x baseline=%llu prepared_baseline=%llu baseline_revision=%llu registry_baseline=%llu active=%u valid=%u authoritative=%u material_deferred=%u structural_deferred=%u material_deferred_frame=%llu structural_deferred_frame=%llu static_selected=%u slice_valid=%u slot=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u bound_static_instances=%u bound_mask=0x%x base_matches=%u first_blas=%llu instance_hash=%llu geometry_hash=%llu gpu_material_hash=%llu semantic_hash=%llu invalid_geometry=%u alternate_link_cpu_rows=%u alternate_mutation_cpu_rows=%u alternate_dynamic_cpu_rows=%u content_build=%llu material_generation=%llu geometry_generation=%llu\n",
				(unsigned long long)context.epoch, (unsigned long long)context.build, (unsigned long long)context.frame,
				(unsigned long long)(context.frame + 1), (unsigned long long)context.tick, (unsigned long long)context.simulation,
				(unsigned long long)context.presentation, chunk, mapChunk.sectorIndex, visible, replacement.source, replacement.reason,
				(unsigned long long)replacement.baseline, (unsigned long long)replacement.preparedBaseline,
				(unsigned long long)replacement.revision, (unsigned long long)(entry != nullptr ? entry->baselineSignature : 0),
				replacement.active, replacement.valid, replacement.authoritative, replacement.materialDeferred, replacement.structuralDeferred,
				(unsigned long long)replacement.materialDeferredFrame, (unsigned long long)replacement.structuralDeferredFrame,
				context.selectedStatic, sliceValid, slot, slice != nullptr ? slice->primitiveOffset : 0, slice != nullptr ? slice->primitiveCount : 0,
				slice != nullptr ? slice->materialOffset : 0, slice != nullptr ? slice->materialCount : 0,
				instances, masks, baseMatches, (unsigned long long)firstBlas, (unsigned long long)instanceHash,
				(unsigned long long)geometryHash, (unsigned long long)gpuHash, (unsigned long long)semanticHash, invalidGeometry,
				links, mutations, dynamics, (unsigned long long)scene.contentBuildSerial,
				(unsigned long long)scene.materialGeneration, (unsigned long long)scene.geometryGeneration);
			for (uint32_t instanceIndex = 0; instanceIndex < tlas.publishedInstances.size(); ++instanceIndex)
			{
				const auto& instance = tlas.publishedInstances[instanceIndex];
				if (instance.instanceId >= bound.size()) continue;
				const auto& record = bound[instance.instanceId];
				if (record.dataSource != nri_diag::SceneDataSourceStatic || record.metadata0 != chunk) continue;
				Printf("PERF pt mutation publication instance NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu chunk=%u tlas_index=%u record=%u data_source=%u primitive_base=%u material_base=%u material_count=%u visibility_chunk=%u metadata=(%u,%u,%u) mask=0x%x flags=0x%x blas=%llu transform=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g)\n",
					(unsigned long long)context.epoch, (unsigned long long)context.build, (unsigned long long)context.frame,
					(unsigned long long)(context.frame + 1), chunk, instanceIndex, (uint32_t)instance.instanceId, record.dataSource,
					record.primitiveBase, record.materialBase, record.materialCount, record.visibilityChunk,
					record.metadata0, record.metadata1, record.metadata2, (uint32_t)instance.mask, (uint32_t)instance.flags,
					(unsigned long long)instance.accelerationStructureHandle,
					record.currentTransform[0], record.currentTransform[1], record.currentTransform[2], record.currentTransform[3],
					record.currentTransform[4], record.currentTransform[5], record.currentTransform[6], record.currentTransform[7],
					record.currentTransform[8], record.currentTransform[9], record.currentTransform[10], record.currentTransform[11]);
			}
			for (uint32_t i = 0; i < rows.size(); ++i)
			{
				const auto& row = rows[i];
				if (i < previous.rows.size() && previous.rows[i] == row.hash) continue;
				const uint32_t material = slice->materialOffset + i;
				const auto& gpu = scene.gpuMaterials[material];
				const auto& metadata = scene.materialBridge.lightMetadata[material];
				Printf("PERF pt mutation publication row NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu chunk=%u row=%u material=%u source_type=%u sector=%d wall=%d section=%d primitives=%u provenance_mismatch=%u authored_texture=%u resolved_texture=%u shade=%d palette=%u gpu_texture=%u gpu_palette=%u light_level=%.9g row_hash=%llu min=(%.9g,%.9g,%.9g) max=(%.9g,%.9g,%.9g) uv_min=(%.9g,%.9g) uv_max=(%.9g,%.9g)\n",
					(unsigned long long)context.epoch, (unsigned long long)context.build, (unsigned long long)context.frame,
					(unsigned long long)(context.frame + 1), (unsigned long long)context.tick, chunk, i, material,
					(uint32_t)metadata.sourceType, metadata.sectorIndex, row.wall, row.section, row.primitives, row.provenanceMismatch,
					metadata.authoredTextureId, metadata.textureId, metadata.shade, metadata.paletteIndex, gpu.textureIndex, gpu.paletteIndex,
					gpu.lightLevel, (unsigned long long)row.hash, row.minimum[0], row.minimum[1], row.minimum[2], row.maximum[0], row.maximum[1], row.maximum[2],
					row.uvMinimum[0], row.uvMinimum[1], row.uvMaximum[0], row.uvMaximum[1]);
			}
			previous.hash = stateHash;
			previous.rows.clear(); previous.rows.reserve(rows.size());
			for (const auto& row : rows) previous.rows.push_back(row.hash);
		}
		// Every selected invocation emits coverage. A missing row means no output
		// change only inside an uninterrupted coverage interval with the same epoch.
		Printf("PERF pt mutation publication coverage NRI: epoch=%llu build=%llu frame=%llu renderer_frame=%llu tick=%llu visited=%u emitted=%u missing_slices=%u invalid_tlas_ids=%u queued_slot=%u tlas_valid=%u tlas_map=%llu tlas_build=%llu recording_fence=%llu blas_generation=%llu vertex_buffer_hash=%llu primitive_buffer_hash=%llu material_buffer_hash=%llu gpu_readback=0 recovery=0\n",
			(unsigned long long)context.epoch, (unsigned long long)context.build, (unsigned long long)context.frame,
			(unsigned long long)(context.frame + 1), (unsigned long long)context.tick, visited, emitted, missingSlices,
			invalidTlasIds, context.frameSlot, tlas.publicationValid, (unsigned long long)tlas.publishedMapEpoch,
			(unsigned long long)tlas.publishedBuildEpoch, (unsigned long long)tlas.publishedRecordingFence,
			(unsigned long long)tlas.publishedBlasGeneration, (unsigned long long)context.vertexBufferHash,
			(unsigned long long)context.primitiveBufferHash, (unsigned long long)context.materialBufferHash);
	}

private:
	static constexpr uint64_t Seed = 1469598103934665603ull;
	struct Chunk { uint64_t hash = 0; std::vector<uint64_t> rows; };
	struct Row
	{
		uint64_t hash = Seed;
		uint32_t primitives = 0;
		int32_t wall = -1, section = -1;
		bool provenanceMismatch = false;
		float minimum[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, maximum[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		float uvMinimum[2] = { FLT_MAX, FLT_MAX }, uvMaximum[2] = { -FLT_MAX, -FLT_MAX };
	};
	uint64_t mEpoch = 0, mBuild = 0, mLastFrame = 0;
	std::vector<Chunk> mChunks;
	static bool Range(uint32_t first, uint32_t count, size_t size) { return first <= size && count <= size - first; }
	static void Hash(uint64_t& hash, const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
	}
	static uint32_t CountProvenance(const nri_scene::GeometryData& geometry, uint32_t chunk)
	{
		uint32_t count = 0;
		for (const auto& provenance : geometry.primitiveProvenance) count += provenance.mapChunkIndex == (int32_t)chunk;
		return count;
	}
};
