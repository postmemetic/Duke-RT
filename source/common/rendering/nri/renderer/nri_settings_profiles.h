#pragma once

struct NRISettingsCapabilities;

void NRIApplySafeSettingsProfileForRecovery();
void NRIReconcileSettingsProfileAvailability(const NRISettingsCapabilities& capabilities);
