#pragma once

struct NRISettingsCapabilities;

void NRIApplySafeSettingsProfileForRecovery();
void NRIInitializeSettingsDefaults(const NRISettingsCapabilities& capabilities);
void NRIReconcileSettingsProfileAvailability(const NRISettingsCapabilities& capabilities);
