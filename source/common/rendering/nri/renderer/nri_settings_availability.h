#pragma once

#include <string>

class NRIRenderDevice;

// A capability snapshot, independent of requested settings and lazily created
// feature contexts. This also keeps menu policy testable without a GPU.
struct NRISettingsCapabilities
{
	bool deviceKnown = false;
	bool presentationKnown = false;
	bool amd = false;
	bool d3d12 = false;
	bool dlss = false;
	bool dlrr = false;
	bool fsr = false;
	bool hdr = false;
	bool framegenRuntime = false;
	bool framegenShaderModel = false;
	bool framegenNativeDevice = false;
	bool framegenNativeQueue = false;
	bool framegenWindowMode = true;
	bool smokeEnabled = true;
};

std::string NRIEvaluateSettingAvailability(const NRISettingsCapabilities& capabilities, const char* setting, int value);
struct NRISettingChangeDecision
{
	int value;
	std::string unavailableReason;
};
NRISettingChangeDecision NRIResolveSettingChange(const NRISettingsCapabilities& capabilities, const char* setting, int requested, int previous);
std::string NRIGetSettingUnavailableReason(const char* setting, int value);
NRISettingsCapabilities NRIGetSettingsCapabilities();
bool NRIIsSmokeSupported();

// Startup loads CVARs before a device exists; validation is deferred until the
// device publishes its capabilities, before renderer/optional shader creation.
void NRIRefreshSettingsAvailability(const NRIRenderDevice& device);
void NRIResetSettingsAvailability();
int NRIValidateSettingChange(const char* setting, int requested, int& previous);
