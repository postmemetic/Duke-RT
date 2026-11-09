#pragma once
#include "../scene/nri_map_chunk_view_inputs.h"

// A last raw producer result per current chunk, not retained runtime authority.
// Never store adjusted motion/provenance/material-wrapper output here.
class NRIRuntimeMutationViewInputs
{
public:
	void Reset();
	void Begin(uint64_t buildSerial, size_t chunkCount);
	nri_scene::PTMapChunkViewInputs& Scratch() { return m_probe; }
	bool TryReuse(const nri_scene::PTMapChunk& chunk, nri_scene::PTMapChunkViewInputs& current,
		nri_scene::SceneView& outRawView, nri_scene::PTMapWorldStats& outStats, bool validate);
	void Observe(const nri_scene::PTMapChunk& chunk, const nri_scene::PTMapChunkViewInputs& current,
		const nri_scene::SceneView& rawView, const nri_scene::PTMapWorldStats& stats);
private:
	struct Record
	{
		nri_scene::PTMapChunkViewInputs inputs;
		nri_scene::SceneView rawView;
		nri_scene::PTMapWorldStats stats;
		uint64_t capacityBytes = 0;
	};
	std::vector<Record> m_records;
	nri_scene::PTMapChunkViewInputs m_probe;
	uint64_t m_buildSerial = 0;
	uint64_t m_capacityBytes = 0;
	bool m_quarantined = false;
};
