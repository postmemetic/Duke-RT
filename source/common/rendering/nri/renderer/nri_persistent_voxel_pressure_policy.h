#pragma once

#include <cstdint>

enum NRIPersistentVoxelPressureEvaluationReason : uint32_t
{
	NRIPersistentVoxelPressureEvaluationReason_None = 0,
	NRIPersistentVoxelPressureEvaluationReason_Trace = 1u << 0,
	NRIPersistentVoxelPressureEvaluationReason_Invalid = 1u << 1,
	NRIPersistentVoxelPressureEvaluationReason_Mutation = 1u << 2,
	NRIPersistentVoxelPressureEvaluationReason_Settings = 1u << 3,
	NRIPersistentVoxelPressureEvaluationReason_AdapterBudget = 1u << 4,
	NRIPersistentVoxelPressureEvaluationReason_ExternalPressure = 1u << 5,
	NRIPersistentVoxelPressureEvaluationReason_SafetyAudit = 1u << 6,
};

struct NRIPersistentVoxelPressureEvaluationInput
{
	bool traceEnabled = false;
	bool evaluationValid = false;
	bool externalPressure = false;
	uint64_t evaluationGeneration = 0;
	uint64_t maintenanceGeneration = 0;
	uint64_t evaluationSettingsSignature = 0;
	uint64_t settingsSignature = 0;
	uint64_t evaluationAdapterBudget = 0;
	uint64_t adapterBudget = 0;
	uint32_t evaluationFrame = 0;
	uint32_t frameIndex = 0;
	uint32_t safetyAuditFrames = 0;
};

struct NRIPersistentVoxelPressureEvaluationDecision
{
	bool evaluate = false;
	uint32_t reasonMask = NRIPersistentVoxelPressureEvaluationReason_None;
};

NRIPersistentVoxelPressureEvaluationDecision DecideNRIPersistentVoxelPressureEvaluation(
	const NRIPersistentVoxelPressureEvaluationInput& input);

bool ShouldInvalidateNRIPersistentVoxelPressureForMembershipChange(bool protectionBlocked);

// Counts come from the authoritative resource refresh, not a sampled census.
// Explicit trace/audit requests retain the complete candidate path.
bool CanSkipNRIPersistentVoxelPressureCandidates(
	uint32_t evaluationReasonMask, uint32_t coldMeshCount, uint32_t coldMaterialCount);
