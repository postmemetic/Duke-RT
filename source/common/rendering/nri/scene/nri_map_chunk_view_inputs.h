#pragma once
#include "nri_map_mutation_inputs.h"

namespace nri_scene
{
// Exact inputs for a deliberately narrow plain-map producer. No hash equality
// certifies view reuse; unsupported producers retain full reconstruction.
struct PTMapChunkViewInputs
{
	PTMapChunkMutationBaseline authored;
	std::vector<uint64_t> facts;
	bool valid = false;
};
bool CapturePlainMapChunkViewInputs(const PTMapChunk& chunk, PTMapChunkViewInputs& out);
bool SamePlainMapChunkViewInputs(const PTMapChunkViewInputs& lhs, const PTMapChunkViewInputs& rhs);
}
