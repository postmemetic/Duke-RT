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
};

void NRIPopulateSmokeVisualConstants(const NRISmokeVisualSettings& settings,
	NRISmokeConstants& constants);
uint64_t NRIHashSmokeVisualSettings(const NRISmokeVisualSettings& settings);
const char* NRIGetSmokeFieldDebugName(uint32_t mode);
