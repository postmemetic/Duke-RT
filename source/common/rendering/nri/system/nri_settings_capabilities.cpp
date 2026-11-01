#include "nri_renderdevice.h"
#include "../renderer/nri_settings_availability.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
	bool FidelityFxRuntimePresent(bool d3d12)
	{
#ifdef _WIN32
		// No DLL load, provider context, GPU allocation or shader compilation.
		// Use the same filenames/search environment as the existing loaders.
		char path[MAX_PATH];
		const char* name = d3d12 ? "amd_fidelityfx_dx12.dll" : "amd_fidelityfx_vk.dll";
		return GetModuleHandleA(name) != nullptr || SearchPathA(nullptr, name, nullptr, MAX_PATH, path, nullptr) != 0;
#else
		(void)d3d12;
		// Linux builds advertise FSR through NRI only when that provider is built.
		return true;
#endif
	}
}

NRISettingsCapabilities NRIRenderDevice::BuildSettingsCapabilities() const
{
	NRISettingsCapabilities caps;
	if (mDevice == nullptr || mCore.GetDeviceDesc == nullptr)
		return caps;
	const auto& desc = mCore.GetDeviceDesc(*mDevice);
	const auto backend = BuildBackendCapabilities();
	caps.deviceKnown = true;
	caps.amd = desc.adapterDesc.vendor == nri::Vendor::AMD;
	caps.d3d12 = backend.d3d12;
	caps.presentationKnown = mHasSwapChainDisplayDesc || mSwapChain != nullptr || !mFrameGenerationPresentImages.empty();
	caps.hdr = mHasSwapChainDisplayDesc && mSwapChainDisplayDesc.isHDR;
	// Runtime files are immutable during a session. Cache the read-only probe
	// because display capabilities are refreshed on the normal frame path.
	static const bool dx12Runtime = FidelityFxRuntimePresent(true);
	static const bool vkRuntime = FidelityFxRuntimePresent(false);
	if (mUpscaler.IsUpscalerSupported != nullptr)
	{
		caps.dlss = mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLSR);
		caps.dlrr = mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::DLRR);
		caps.fsr = mUpscaler.IsUpscalerSupported(*mDevice, nri::UpscalerType::FSR) && (caps.d3d12 ? dx12Runtime : vkRuntime);
	}
	caps.framegenShaderModel = desc.shaderModel >= 62;
	caps.framegenNativeDevice = backend.nativeD3D12DeviceAvailable;
	caps.framegenNativeQueue = backend.nativeD3D12GraphicsQueueAvailable;
	caps.framegenRuntime = dx12Runtime;
	const auto& provider = mFrameGeneration.GetProviderState();
	if (provider.runtimeLoaded)
		caps.framegenRuntime = provider.runtimeFunctionsLoaded;
	const auto& policy = mFrameGeneration.GetPolicy();
	if (policy.initialized)
		caps.framegenWindowMode = policy.windowModeSupported;
	return caps;
}
