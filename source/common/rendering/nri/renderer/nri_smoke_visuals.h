#pragma once

#include <cstdint>

struct NRISmokeConstants;

struct NRISmokeVisualSettings
{
	float extinctionThreshold = 0.0f;
	float extinctionKnee = 0.0f;
	float extinctionGamma = 1.0f;
	float extinctionReference = 1.0f;
	float extinctionShoulder = 0.0f;
	float thinColor[3] = { 1.0f, 1.0f, 1.0f };
	float coreColor[3] = { 1.0f, 1.0f, 1.0f };
	float colorPivot = 0.05f;
	float thermalLow = 1.0f;
	float thermalHigh = 4.0f;
	float thermalTint[3] = { 1.0f, 1.0f, 1.0f };
	float thermalGlow[3] = {};
	float gradientPivot = 0.5f;
	float gradientWidth = 0.25f;
	float edgeSculpt = 0.0f;
	float edgePowder = 0.0f;
	float edgeTint[3] = { 1.0f, 1.0f, 1.0f };
	float edgeTintStrength = 0.0f;
	float dualLobeWeight = 0.0f;
	float dualLobeG = 0.75f;
	float rimStrength = 0.0f;
	float rimGain = 0.0f;
	float radianceEdgeChroma = 0.0f;
	float radianceCavityContrast = 0.0f;
	float radianceDesaturation = 0.0f;
	float radianceConfidence = 0.25f;
	float radianceDirectionality = 0.15f;
	float thicknessStrength = 0.0f;
	float thicknessPivot = 0.12f;
	float thicknessSteps = 2.0f;
	float flowHighlight = 0.0f;
	float flowSpeedReference = 32.0f;
	float curlHighlight = 0.0f;
	float curlReference = 1.0f;
	float compressionSculpt = 0.0f;
	float divergenceReference = 1.0f;
	float bandStrength = 0.0f;
	float bandCount = 4.0f;
	float bandSoftness = 0.25f;
	float contourStrength = 0.0f;
};

void NRIPopulateSmokeVisualConstants(const NRISmokeVisualSettings& settings,
	NRISmokeConstants& constants);
void NRIPopulateSmokeVisualMaterializationConstants(const NRISmokeVisualSettings& settings,
	NRISmokeConstants& constants);
void NRIPopulateSmokeVisualPhaseConstants(const NRISmokeVisualSettings& settings,
	NRISmokeConstants& constants);
uint64_t NRIHashSmokeVisualSettings(const NRISmokeVisualSettings& settings);
const char* NRIGetSmokeFieldDebugName(uint32_t mode);
