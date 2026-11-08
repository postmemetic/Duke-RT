#include "nri_persistent_voxel_pressure_policy.h"

NRIPersistentVoxelPressureEvaluationDecision DecideNRIPersistentVoxelPressureEvaluation(
	const NRIPersistentVoxelPressureEvaluationInput& input)
{
	NRIPersistentVoxelPressureEvaluationDecision decision = {};
	if (input.traceEnabled)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Trace;
	}
	if (!input.evaluationValid)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Invalid;
	}
	if (input.evaluationGeneration != input.maintenanceGeneration)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Mutation;
	}
	if (input.evaluationSettingsSignature != input.settingsSignature)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Settings;
	}
	if (input.evaluationAdapterBudget != input.adapterBudget)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_AdapterBudget;
	}
	if (input.externalPressure)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_ExternalPressure;
	}
	if (input.evaluationValid && input.safetyAuditFrames != 0u &&
		input.frameIndex - input.evaluationFrame >= input.safetyAuditFrames)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_SafetyAudit;
	}
	decision.evaluate = decision.reasonMask != NRIPersistentVoxelPressureEvaluationReason_None;
	return decision;
}

bool ShouldInvalidateNRIPersistentVoxelPressureForMembershipChange(bool protectionBlocked)
{
	return protectionBlocked;
}

bool CanSkipNRIPersistentVoxelPressureCandidates(
	uint32_t evaluationReasonMask, uint32_t coldMeshCount, uint32_t coldMaterialCount)
{
	constexpr uint32_t fullScanReasons = NRIPersistentVoxelPressureEvaluationReason_Trace |
		NRIPersistentVoxelPressureEvaluationReason_SafetyAudit;
	return coldMeshCount == 0 && coldMaterialCount == 0 &&
		(evaluationReasonMask & fullScanReasons) == 0;
}
