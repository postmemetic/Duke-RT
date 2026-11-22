#include "nri_enemy_voxel_retained.h"
#include "nri_scene_bridge.h"
#include "actor_presentation_snapshot.h"
#include "gamestruct.h"
#include "buildtiles.h"
#include "hw_voxels.h"
#include "hw_drawinfo.h"
#include "hw_drawstructs.h"
#include "texinfo.h"
#include "c_cvars.h"
#include "perf_audit.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>
#include <memory>

EXTERN_CVAR(Bool, nri_ptenemystablerepresentation)
EXTERN_CVAR(Bool, nri_ptenemyrepresentationtrace)

namespace nri_scene
{
namespace
{
using Owner = std::pair<uint64_t, uint64_t>;
enum class Mode { Candidate, Voxel, Sprite };
struct Source
{
	Mode mode = Mode::Candidate;
	uint64_t frame = 0, signature = 0, successorSignature = 0;
	bool root = false;
	int failedTexture = -1;
	std::unique_ptr<tspritetype> sprite;
};
constexpr size_t MaximumSources = 512;
std::map<Owner, Source> sources;
bool Certified(const ActorPresentationState& state)
{
	const auto& animation = state.spriteAnimation;
	const auto& family = animation.enemyVoxelFamily;
	return state.owner.IsValid() && animation.available && animation.enemy && animation.recipeCertified &&
		family && family->complete && family->signature != 0 && !family->textureIds.empty() && family->textureIds.size() <= 64;
}

const char* PrepareSource(HWDrawInfo& view, const ActorPresentationState& state, tspritetype& sprite, HWSprite& prepared, int* resolvedTexture = nullptr)
{
	if (state.physicalSectorIndex < 0 || unsigned(state.physicalSectorIndex) >= sector.Size()) return "invalid-sector";
	sprite.ownerActor = nullptr;
	sprite.sectp = &sector[state.physicalSectorIndex];
	if (!gi || !gi->ResolveActorRetainedVoxelFrame(state, sprite, view.Viewpoint.TicFrac,
		DVector2(view.Viewpoint.Pos.X, -view.Viewpoint.Pos.Y), DAngle::fromBam(view.Viewpoint.RotAngle))) return "recipe-unavailable";
	if (sprite.scale.X == 0 || sprite.scale.Y == 0) return "authored-hidden";
	auto texture = sprite.spritetexture();
	if (!(sprite.cstat2 & CSTAT2_SPRITE_NOANIMATE)) tileUpdatePicnum(texture, state.actorIndex & 16383);
	sprite.setspritetexture(texture);
	if (sprite.cstat2 & CSTAT2_SPRITE_FULLBRIGHT) sprite.shade = -127;
	if (resolvedTexture) *resolvedTexture = texture.GetIndex();
	if (!texture.isValid()) return "invalid-texture";
	if (sprite.cstat & CSTAT_SPRITE_INVISIBLE) return "authored-hidden";
	if (state.spriteAnimation.enemy)
	{
		const auto& poses = state.spriteAnimation.enemyVoxelFamily->textureIds;
		if (std::find(poses.begin(), poses.end(), texture.GetIndex()) == poses.end()) return "outside-enemy-family";
	}
	const int voxel = GetExtInfo(texture).tiletovox;
	if (voxel < 0) return "unmapped-voxel-art";
	if (voxel >= MAXVOXELS || !voxmodels[voxel]) return "voxel-model-unavailable";
	HWVoxelActorInputs inputs;
	inputs.modelRotation.Yaw = DAngle::fromDeg(state.modelRotation.yawDegrees);
	inputs.modelRotation.Pitch = DAngle::fromDeg(state.modelRotation.pitchDegrees);
	inputs.modelRotation.Roll = DAngle::fromDeg(state.modelRotation.rollDegrees);
	inputs.modelPositionOffset = DVector3(state.modelPositionOffset.x, state.modelPositionOffset.y, state.modelPositionOffset.z);
	inputs.cstat = int(state.cstat); inputs.alpha = state.alpha;
	return prepared.PrepareVoxel(&view, voxmodels[voxel], &sprite, sprite.sectp, voxrotate[voxel], inputs) ? nullptr : "voxel-transform-refused";
}

bool ValidateCapturedSource(HWDrawInfo& view, const HWSprite& captured, const ActorPresentationState& state, tspritetype sprite)
{
	HWSprite prepared = {};
	if (PrepareSource(view, state, sprite, prepared) || prepared.voxel != captured.voxel ||
		sprite.spritetexture() != captured.Sprite->spritetexture() || prepared.palette != captured.palette ||
		prepared.shade != captured.shade || std::abs(prepared.alpha - captured.alpha) > 0.0001f) return false;
	for (int i = 0; i < 16; ++i)
		if (std::abs(prepared.rotmat.get()[i] - captured.rotmat.get()[i]) > 0.001f) return false;
	return true;
}
}

bool ValidateGenericVoxelTemplate(HWDrawInfo& view, const HWSprite& captured, const ActorPresentationState& state,
	const tspritetype& sprite)
{
	return !state.spriteAnimation.enemy && state.spriteAnimation.recipeCertified &&
		ValidateCapturedSource(view, captured, state, sprite);
}

const char* PublishGenericVoxelTemplate(HWDrawInfo& view, const ActorPresentationState& state, const tspritetype& source,
	EnemyVoxelSourceSink sink, int& texture)
{
	if (state.spriteAnimation.enemy || !state.spriteAnimation.recipeCertified) return "recipe-unavailable";
	tspritetype sprite = source;
	HWSprite prepared = {};
	if (const char* refusal = PrepareSource(view, state, sprite, prepared, &texture)) return refusal;
	return sink(state, prepared) ? "published" : "source-publication-refused";
}

void ResetEnemyVoxelSources() { sources.clear(); }

void BeginEnemyVoxelSourceFrame()
{
	if (!(bool)nri_ptenemystablerepresentation) { ResetEnemyVoxelSources(); return; }
	const auto& snapshot = GetActorPresentationSnapshot();
	if (!snapshot.complete) return;
	for (auto it = sources.begin(); it != sources.end();)
	{
		if (it->first.first != snapshot.worldEpoch ||
			!LookupActorPresentationByOwnerKey({it->first.first, int64_t(it->first.second)})) it = sources.erase(it);
		else ++it;
	}
}

void CaptureEnemyVoxelSource(HWDrawInfo& view, const HWSprite& sprite, const ActorPresentationState& state, uint64_t frame)
{
	if (!(bool)nri_ptenemystablerepresentation || !Certified(state) || !sprite.Sprite) return;
	const Owner owner{state.owner.ownerWorldEpoch, uint64_t(state.owner.ownerLifetimeGeneration)};
	auto found = sources.find(owner);
	if (found == sources.end())
	{
		if (sources.size() >= MaximumSources) return;
		found = sources.emplace(owner, Source{}).first;
	}
	auto& source = found->second;
	if (source.mode == Mode::Sprite) return;
	if (source.frame == frame && source.root && view.outer != nullptr) return;
	source.frame = frame; source.root = view.outer == nullptr;
	if (!ValidateCapturedSource(view, sprite, state, *sprite.Sprite))
	{
		source.sprite.reset();
		const int texture = sprite.Sprite->spritetexture().GetIndex();
		if ((bool)nri_ptenemyrepresentationtrace && source.failedTexture != texture)
		{
			char fields[224];
			std::snprintf(fields, sizeof(fields), "\"actor\":%d,\"world\":%llu,\"lifetime\":%llu,\"texture\":%d,\"reason\":\"captured-recipe-mismatch\"",
				state.actorIndex, (unsigned long long)owner.first, (unsigned long long)owner.second, texture);
			PerfAuditWriteEvent("enemy_retained_voxel_refusal", PerfAuditCurrentToken(), fields);
		}
		source.failedTexture = texture;
		return;
	}
	source.failedTexture = -1;
	if (!source.sprite) source.sprite = std::make_unique<tspritetype>();
	*source.sprite = *sprite.Sprite;
	source.sprite->ownerActor = nullptr;
	source.signature = state.spriteAnimation.enemyVoxelFamily->signature;
	source.successorSignature = state.spriteAnimation.enemyVoxelFamily->successorSignature;
}

bool HasEnemyVoxelSource(const ActorPresentationState& state)
{
	const auto found = sources.find({state.owner.ownerWorldEpoch, uint64_t(state.owner.ownerLifetimeGeneration)});
	return Certified(state) && found != sources.end() && found->second.mode != Mode::Sprite && found->second.sprite &&
		state.spriteAnimation.enemyVoxelFamily->CoveredBy(found->second.signature, found->second.successorSignature);
}

void PrepareEnemyVoxelSource(uint64_t world, uint64_t lifetime)
{
    const auto found = sources.find({world, lifetime});
    if (found != sources.end() && found->second.mode == Mode::Sprite) found->second.mode = Mode::Candidate;
}

void SetEnemyVoxelSourceMode(uint64_t world, uint64_t lifetime, bool voxel)
{
	auto found = sources.find({world, lifetime});
	if (found == sources.end())
	{
		if (voxel || sources.size() >= MaximumSources) return;
		found = sources.emplace(Owner{world, lifetime}, Source{}).first;
	}
	found->second.mode = voxel ? Mode::Voxel : Mode::Sprite;
	if (!voxel) found->second.sprite.reset();
}

void RefreshEnemyVoxelSources(HWDrawInfo& view, uint64_t frame, EnemyVoxelSourceSink sink)
{
	if (!(bool)nri_ptenemystablerepresentation || !sink || !GetActorPresentationSnapshot().complete) return;
	uint32_t refreshed = 0, rejected = 0;
	for (auto& pair : sources)
	{
		auto& source = pair.second;
		if (source.mode != Mode::Voxel || source.frame == frame || !source.sprite) continue;
		const auto* state = LookupActorPresentationByOwnerKey({pair.first.first, int64_t(pair.first.second)});
		if (!state || !state->authorityCurrent || !state->publicationEligible || !Certified(*state) ||
			!state->spriteAnimation.enemyVoxelFamily->CoveredBy(source.signature, source.successorSignature) ||
			state->physicalSectorIndex < 0 || unsigned(state->physicalSectorIndex) >= sector.Size())
		{ ++rejected; continue; }
		tspritetype sprite = *source.sprite;
		HWSprite prepared = {};
		const bool ready = !PrepareSource(view, *state, sprite, prepared) && sink(*state, prepared);
		if (ready) ++refreshed; else ++rejected;
	}
	if ((bool)nri_ptenemyrepresentationtrace && (refreshed || rejected))
	{
		char fields[192];
		std::snprintf(fields, sizeof(fields), "\"capture_frame\":%llu,\"registry_records\":%zu,\"refreshed\":%u,\"rejected\":%u",
			(unsigned long long)frame, sources.size(), refreshed, rejected);
		PerfAuditWriteEvent("enemy_retained_voxel_sources", PerfAuditCurrentToken(), fields);
	}
}
}
