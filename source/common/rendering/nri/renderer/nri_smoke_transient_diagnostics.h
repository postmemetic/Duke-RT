#pragma once

#include "nri_smoke_contracts.h"
#include "nri_smoke_transient_clouds.h"

// CPU population and GPU observations refer to the same completed command slot.
struct NRISmokeTransientTelemetry
{
	bool valid = false;
	uint64_t rendererFrame = UINT64_MAX;
	uint32_t epoch = 0;
	uint32_t profile = 0;
	uint64_t residentBytes = 0;
	NRISmokeTransientSnapshot cpu = {};
	NRISmokeControlGpu gpu = {};
};

void NRIPrintSmokeTransientTelemetry(const NRISmokeTransientTelemetry& telemetry);
