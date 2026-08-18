#include "nri_framegen.h"
#include "../renderer/nri_cvars.h"
#include "nri_ffx_api.h"

#include "../system/nri_renderdevice.h"
#include "i_mainwindow.h"
#include "c_cvars.h"
#include "printf.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#endif


namespace
{
#ifdef _WIN32
	struct NriFfxFunctionTable
	{
		PfnFfxCreateContext createContext = nullptr;
		PfnFfxDestroyContext destroyContext = nullptr;
		PfnFfxConfigure configure = nullptr;
		PfnFfxQuery query = nullptr;
		PfnFfxDispatch dispatch = nullptr;
	};

	static void* NriFfxAlloc(void*, uint64_t size)
	{
		return std::malloc((size_t)size);
	}

	static void NriFfxDealloc(void*, void* memory)
	{
		std::free(memory);
	}

	static const char* GetFfxLibraryName(NRIFrameGenerationProvider provider)
	{
		switch (provider)
		{
		default:
		case NRIFrameGenerationProvider::Off:
			return "";
		case NRIFrameGenerationProvider::FSR3:
			return "amd_fidelityfx_dx12.dll";
		}
	}

	static const char* GetFfxReturnCodeName(uint32_t result)
	{
		switch (result)
		{
		default: return "unknown";
		case NRI_FFX_API_RETURN_OK: return "ok";
		case NRI_FFX_API_RETURN_ERROR: return "error";
		case NRI_FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown-desctype";
		case NRI_FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime-error";
		case NRI_FFX_API_RETURN_NO_PROVIDER: return "no-provider";
		case NRI_FFX_API_RETURN_ERROR_MEMORY: return "memory-error";
		case NRI_FFX_API_RETURN_ERROR_PARAMETER: return "parameter-error";
		}
	}

	static D3D12_RESOURCE_STATES ConvertNriStateToD3D12State(const nri::AccessLayoutStage& state)
	{
		switch (state.layout)
		{
		case nri::Layout::COPY_DESTINATION:
			return D3D12_RESOURCE_STATE_COPY_DEST;
		case nri::Layout::COPY_SOURCE:
			return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case nri::Layout::COLOR_ATTACHMENT:
			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case nri::Layout::SHADER_RESOURCE_STORAGE:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case nri::Layout::PRESENT:
			return D3D12_RESOURCE_STATE_PRESENT;
		case nri::Layout::SHADER_RESOURCE:
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		default:
			break;
		}

		if ((state.access & nri::AccessBits::COPY_DESTINATION) != 0)
			return D3D12_RESOURCE_STATE_COPY_DEST;
		if ((state.access & nri::AccessBits::COPY_SOURCE) != 0)
			return D3D12_RESOURCE_STATE_COPY_SOURCE;
		if ((state.access & nri::AccessBits::COLOR_ATTACHMENT) != 0)
			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		if ((state.access & nri::AccessBits::SHADER_RESOURCE_STORAGE) != 0)
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		if ((state.access & nri::AccessBits::SHADER_RESOURCE) != 0)
			return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		return D3D12_RESOURCE_STATE_COMMON;
	}

	static ID3D12Resource* GetNativeTexture(const nri::CoreInterface& core, const NRITextureResource* texture)
	{
		if (texture == nullptr || texture->texture == nullptr || core.GetTextureNativeObject == nullptr)
			return nullptr;
		return reinterpret_cast<ID3D12Resource*>(core.GetTextureNativeObject(texture->texture));
	}

	static void* GetNativeCommandList(const nri::CoreInterface& core, nri::CommandBuffer* commandBuffer)
	{
		if (commandBuffer == nullptr || core.GetCommandBufferNativeObject == nullptr)
			return nullptr;
		return core.GetCommandBufferNativeObject(commandBuffer);
	}

	static uint32_t GetFfxSurfaceFormat(nri::Format format)
	{
		switch (format)
		{
		case nri::Format::RGBA16_SFLOAT: return NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		case nri::Format::RGBA8_UNORM: return NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case nri::Format::R32_SFLOAT: return NRI_FFX_API_SURFACE_FORMAT_R32_FLOAT;
		default: return NRI_FFX_API_SURFACE_FORMAT_UNKNOWN;
		}
	}

	static bool IsNativeTextureFormatCompatible(DXGI_FORMAT nativeFormat, uint32_t ffxFormat)
	{
		switch (ffxFormat)
		{
		case NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
			return nativeFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || nativeFormat == DXGI_FORMAT_R16G16B16A16_TYPELESS;
		case NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM:
			return nativeFormat == DXGI_FORMAT_R8G8B8A8_UNORM || nativeFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS;
		default:
			return false;
		}
	}

	static FfxApiResource GetFfxTextureResource(const nri::CoreInterface& core, const NRITextureResource* texture)
	{
		ID3D12Resource* nativeTexture = GetNativeTexture(core, texture);
		const D3D12_RESOURCE_STATES state = texture != nullptr ? ConvertNriStateToD3D12State(texture->state) : D3D12_RESOURCE_STATE_COMMON;
		FfxApiResource resource = NriFfxGetResourceDX12(nativeTexture, NriFfxGetResourceStateFromDx12State(state));
		resource.description.format = texture != nullptr ? GetFfxSurfaceFormat(texture->format) : NRI_FFX_API_SURFACE_FORMAT_UNKNOWN;
		return resource;
	}

	static uint32_t GetFfxCreateFlags(const NRIFrameGenerationPolicy& policy, const NRIFrameGenerationFrameDesc& desc, bool hdrInput, bool enableDebugChecking)
	{
		uint32_t flags = 0;
		if (policy.requestedAsync)
			flags |= NRI_FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
		if (desc.motionVectors != nullptr && desc.motionVectors->width == desc.outputWidth && desc.motionVectors->height == desc.outputHeight)
			flags |= NRI_FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
		if (desc.depthInverted)
			flags |= NRI_FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
		if (desc.depthInfinite)
			flags |= NRI_FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
		if (hdrInput)
			flags |= NRI_FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
		if (enableDebugChecking)
			flags |= NRI_FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
		return flags;
	}

	static void NriFfxMessageCallback(uint32_t type, const wchar_t* message)
	{
		if (message == nullptr)
			return;

		char narrow[1024] = {};
		const int converted = WideCharToMultiByte(CP_UTF8, 0, message, -1, narrow, (int)std::size(narrow), nullptr, nullptr);
		if (converted <= 0)
		{
			Printf("NRI FFX framegen: [%u] <failed to convert message>\n", type);
			return;
		}

		Printf("NRI FFX framegen: type=%u msg=%s\n", type, narrow);
	}

	static void CopyString(char* destination, size_t destinationSize, const char* source)
	{
		if (destination == nullptr || destinationSize == 0u)
			return;

		if (source == nullptr)
			source = "";

		std::strncpy(destination, source, destinationSize - 1u);
		destination[destinationSize - 1u] = '\0';
	}

	static nri::Result GetNriPresentResult(HRESULT hr)
	{
		if (SUCCEEDED(hr))
			return nri::Result::SUCCESS;
		if (hr == DXGI_STATUS_OCCLUDED)
			return nri::Result::SUCCESS;
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
			return nri::Result::DEVICE_LOST;
		return nri::Result::FAILURE;
	}

	static const char* GetPresentModeName(bool usedBridge, bool generatedFrame)
	{
		if (!usedBridge)
			return "native";
		return generatedFrame ? "proxy-generated" : "proxy-passthrough";
	}
#endif

	static const char* GetSafeResetReason(const char* reason)
	{
		return (reason != nullptr && *reason != '\0') ? reason : "unspecified";
	}

	static const char* GetApiName(nri::GraphicsAPI api)
	{
		switch (api)
		{
		case nri::GraphicsAPI::D3D12: return "d3d12";
		case nri::GraphicsAPI::VK: return "vulkan";
		default: return "unknown";
		}
	}

	static bool SupportsFrameGenerationPresentMode(NRIWindowPresentationMode mode, bool dxgiFullscreenKnown, bool dxgiFullscreen)
	{
		const bool engineModeSupported =
			mode == NRIWindowPresentationMode::Windowed ||
			mode == NRIWindowPresentationMode::BorderlessFullscreen;
		return engineModeSupported && (!dxgiFullscreenKnown || !dxgiFullscreen);
	}

	static NRIFrameGenerationUiMode GetRequestedUiMode()
	{
		switch ((int)nri_framegenui)
		{
		default:
		case 0: return NRIFrameGenerationUiMode::Auto;
		case 1: return NRIFrameGenerationUiMode::Hudless;
		case 2: return NRIFrameGenerationUiMode::UiTexture;
		case 3: return NRIFrameGenerationUiMode::PresentCallback;
		}
	}

	static NRIFrameGenerationUiMode ResolveUiMode(NRIFrameGenerationUiMode requested)
	{
		switch (requested)
		{
		default:
		case NRIFrameGenerationUiMode::Auto:
			return NRIFrameGenerationUiMode::UiTexture;
		case NRIFrameGenerationUiMode::PresentCallback:
			// Present callbacks stay out of scope until the proxy-swapchain path exists.
			return NRIFrameGenerationUiMode::UiTexture;
		case NRIFrameGenerationUiMode::Hudless:
		case NRIFrameGenerationUiMode::UiTexture:
			return requested;
		}
	}

	static bool ArePoliciesEquivalent(const NRIFrameGenerationPolicy& a, const NRIFrameGenerationPolicy& b)
	{
		return
			a.initialized == b.initialized &&
			a.requestedEnabled == b.requestedEnabled &&
			a.operational == b.operational &&
			a.apiSupported == b.apiSupported &&
			a.shaderModelSupported == b.shaderModelSupported &&
			a.providerRuntimeSupported == b.providerRuntimeSupported &&
			a.swapChainReady == b.swapChainReady &&
			a.windowPresentationMode == b.windowPresentationMode &&
			a.windowModeSupported == b.windowModeSupported &&
			a.dxgiFullscreenKnown == b.dxgiFullscreenKnown &&
			a.dxgiFullscreen == b.dxgiFullscreen &&
			a.ffxProxyPacing == b.ffxProxyPacing &&
			a.lowLatencyAvailable == b.lowLatencyAvailable &&
			a.lowLatencyInterfaceAvailable == b.lowLatencyInterfaceAvailable &&
			a.lowLatencySwapChainEnabled == b.lowLatencySwapChainEnabled &&
			a.waitableSwapChainAvailable == b.waitableSwapChainAvailable &&
			a.asyncWorkloadAvailable == b.asyncWorkloadAvailable &&
			a.nativeDeviceAvailable == b.nativeDeviceAvailable &&
			a.nativeGraphicsQueueAvailable == b.nativeGraphicsQueueAvailable &&
			a.nativeSwapChainAvailable == b.nativeSwapChainAvailable &&
			a.shaderModel == b.shaderModel &&
			a.selectedApiName == b.selectedApiName &&
			a.outputContractScope == b.outputContractScope &&
			a.resolvedReason == b.resolvedReason &&
			a.provider == b.provider &&
			a.requestedUiMode == b.requestedUiMode &&
			a.resolvedUiMode == b.resolvedUiMode &&
			a.requestedOutputMode == b.requestedOutputMode &&
			a.resolvedOutputMode == b.resolvedOutputMode &&
			a.resolvedOutputContract == b.resolvedOutputContract &&
			a.requestedAsync == b.requestedAsync &&
			a.resolvedAsync == b.resolvedAsync &&
			a.requestedLowLatency == b.requestedLowLatency &&
			a.resolvedLowLatency == b.resolvedLowLatency;
	}

}

const char* NRIFrameGenerationContext::GetProviderName(NRIFrameGenerationProvider provider)
{
	switch (provider)
	{
	default:
	case NRIFrameGenerationProvider::Off: return "off";
	case NRIFrameGenerationProvider::FSR3: return "fsr3";
	}
}

const char* NRIFrameGenerationContext::GetUiModeName(NRIFrameGenerationUiMode mode)
{
	switch (mode)
	{
	default:
	case NRIFrameGenerationUiMode::Auto: return "auto";
	case NRIFrameGenerationUiMode::Hudless: return "hudless";
	case NRIFrameGenerationUiMode::UiTexture: return "ui_texture";
	case NRIFrameGenerationUiMode::PresentCallback: return "present_callback";
	}
}

const char* NRIFrameGenerationContext::GetColorSourceName(NRIFrameGenerationColorSource source)
{
	switch (source)
	{
	default:
	case NRIFrameGenerationColorSource::Unknown: return "unknown";
	case NRIFrameGenerationColorSource::Final: return "final";
	}
}

const char* NRIFrameGenerationContext::GetMotionVectorSpaceName(NRIFrameGenerationMotionVectorSpace space)
{
	switch (space)
	{
	default:
	case NRIFrameGenerationMotionVectorSpace::Unknown: return "unknown";
	case NRIFrameGenerationMotionVectorSpace::ScreenPixels: return "screen_pixels";
	}
}

const char* NRIFrameGenerationContext::GetMotionVectorDirectionName(NRIFrameGenerationMotionVectorDirection direction)
{
	switch (direction)
	{
	default:
	case NRIFrameGenerationMotionVectorDirection::Unknown: return "unknown";
	case NRIFrameGenerationMotionVectorDirection::CurrentToPrevious: return "current_to_previous";
	case NRIFrameGenerationMotionVectorDirection::PreviousToCurrent: return "previous_to_current";
	}
}

const char* NRIFrameGenerationContext::GetDepthTypeName(NRIFrameGenerationDepthType type)
{
	switch (type)
	{
	default:
	case NRIFrameGenerationDepthType::Unknown: return "unknown";
	case NRIFrameGenerationDepthType::ClipDepth: return "clip_depth";
	case NRIFrameGenerationDepthType::ViewZ: return "view_z";
	}
}

const char* NRIFrameGenerationContext::GetAdapterRequirementName(NRIFrameGenerationAdapterRequirement requirement)
{
	switch (requirement)
	{
	default:
	case NRIFrameGenerationAdapterRequirement::None: return "none";
	case NRIFrameGenerationAdapterRequirement::MotionVectors: return "motion";
	case NRIFrameGenerationAdapterRequirement::Depth: return "depth";
	case NRIFrameGenerationAdapterRequirement::MotionAndDepth: return "motion+depth";
	}
}

const char* NRIFrameGenerationContext::GetOutputContractName(NRIFrameGenerationOutputContract contract)
{
	switch (contract)
	{
	default:
	case NRIFrameGenerationOutputContract::None: return "none";
	case NRIFrameGenerationOutputContract::SDRDisplayReady: return "sdr-display-ready";
	case NRIFrameGenerationOutputContract::ScRGBDisplayReady: return "scrgb-display-ready";
	case NRIFrameGenerationOutputContract::Unsupported: return "unsupported";
	}
}

const char* NRIFrameGenerationContext::GetPresentTransferFunctionName(NRIFrameGenerationPresentTransferFunction transferFunction)
{
	switch (transferFunction)
	{
	default:
	case NRIFrameGenerationPresentTransferFunction::Unknown: return "unknown";
	case NRIFrameGenerationPresentTransferFunction::SRGB: return "srgb";
	case NRIFrameGenerationPresentTransferFunction::PQ: return "pq";
	case NRIFrameGenerationPresentTransferFunction::ScRGB: return "scrgb";
	}
}

const char* NRIFrameGenerationContext::GetSwapChainFormatName(nri::SwapChainFormat format)
{
	switch (format)
	{
	default: return "unknown";
	case nri::SwapChainFormat::BT709_G10_16BIT: return "BT709_G10_16BIT";
	case nri::SwapChainFormat::BT709_G22_8BIT: return "BT709_G22_8BIT";
	case nri::SwapChainFormat::BT709_G22_10BIT: return "BT709_G22_10BIT";
	case nri::SwapChainFormat::BT2020_G2084_10BIT: return "BT2020_G2084_10BIT";
	}
}

const char* NRIFrameGenerationContext::GetNriFormatName(nri::Format format)
{
	switch (format)
	{
	default: return "unknown";
	case nri::Format::BGRA8_UNORM: return "BGRA8_UNORM";
	case nri::Format::RGBA8_UNORM: return "RGBA8_UNORM";
	case nri::Format::BGRA8_SRGB: return "BGRA8_SRGB";
	case nri::Format::RGBA8_SRGB: return "RGBA8_SRGB";
	case nri::Format::R10_G10_B10_A2_UNORM: return "R10_G10_B10_A2_UNORM";
	case nri::Format::RGBA16_SFLOAT: return "RGBA16_SFLOAT";
	case nri::Format::UNKNOWN: return "UNKNOWN";
	}
}

const char* NRIFrameGenerationContext::GetDxgiFormatName(uint32_t format)
{
#ifdef _WIN32
	switch ((DXGI_FORMAT)format)
	{
	default: return "unknown";
	case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
	}
#else
	(void)format;
	return "unsupported";
#endif
}

const char* NRIFrameGenerationContext::GetDxgiColorSpaceName(uint32_t colorSpace)
{
#ifdef _WIN32
	switch (static_cast<DXGI_COLOR_SPACE_TYPE>(colorSpace))
	{
	case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709";
	case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709";
	case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020";
	default: return "unknown";
	}
#else
	(void)colorSpace;
	return "unknown";
#endif
}

const char* NRIFrameGenerationContext::GetFfxSurfaceFormatName(uint32_t format)
{
	switch (format)
	{
	case NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
	case NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case NRI_FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
	case NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
	case NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case NRI_FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
	case NRI_FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
	case NRI_FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
	case NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
	case NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
	default: return "unknown";
	}
}

const char* NRIFrameGenerationContext::GetWindowModeName(NRIWindowPresentationMode mode)
{
	return mode == NRIWindowPresentationMode::BorderlessFullscreen ? "borderless" : "windowed";
}

const char* NRIFrameGenerationContext::GetDxgiFullscreenStateName(bool known, bool fullscreen)
{
	if (!known)
		return "unavailable";
	return fullscreen ? "fullscreen" : "windowed";
}

const char* NRIFrameGenerationContext::GetPacingModeName(const NRIFrameGenerationPolicy& policy)
{
	if (policy.ffxProxyPacing)
		return "ffx-proxy";
	if (policy.resolvedLowLatency)
		return "nri-native";
	return "none";
}

const char* NRIFrameGenerationContext::GetAvailabilityName(bool available)
{
	return available ? "yes" : "no";
}

const char* NRIFrameGenerationContext::GetProviderReturnCodeName(uint32_t result)
{
#ifdef _WIN32
	return GetFfxReturnCodeName(result);
#else
	(void)result;
	return "unsupported";
#endif
}

const char* NRIFrameGenerationContext::GetPresentResultName(nri::Result result)
{
	switch (result)
	{
	default: return "unknown";
	case nri::Result::SUCCESS: return "success";
	case nri::Result::FAILURE: return "failure";
	case nri::Result::INVALID_ARGUMENT: return "invalid-argument";
	case nri::Result::OUT_OF_MEMORY: return "out-of-memory";
	case nri::Result::UNSUPPORTED: return "unsupported";
	case nri::Result::DEVICE_LOST: return "device-lost";
	case nri::Result::OUT_OF_DATE: return "out-of-date";
	case nri::Result::INVALID_SDK: return "invalid-sdk";
	}
}

NRIFrameGenerationSettings NRIFrameGenerationContext::CaptureSettings()
{
	NRIFrameGenerationSettings settings = {};
	settings.enabled = !!nri_framegen;
	settings.uiMode = GetRequestedUiMode();
	settings.async = !!nri_framegenasync;
	settings.lowLatency = !!nri_framegenlatency;
	return settings;
}

bool NRIFrameGenerationContext::IsPresentBridgeActive() const
{
	return mPresentBridge.IsActive();
}

bool NRIFrameGenerationContext::ShouldUsePresentBridge() const
{
	return
		IsPresentBridgeActive() &&
		mProviderState.presentBridgeReady &&
		mPresentContract.proxyAllowed &&
		mPolicy.operational;
}

#ifdef _WIN32
IDXGISwapChain4* NRIFrameGenerationContext::GetPresentSwapChain() const
{
	return mPresentBridge.GetSwapChain();
}
#endif

void NRIFrameGenerationContext::Initialize(const NRIRenderDevice& frameBuffer)
{
	mInitialized = true;
	mSwapChainReady = frameBuffer.mSwapChain != nullptr;
	ResetLowLatencyState();
	ResetProviderState();
	EnsureProviderRuntime(frameBuffer);
	RefreshPolicy(frameBuffer, false);
	RefreshPolicy(frameBuffer, true);
}

bool NRIFrameGenerationContext::CreatePresentBridge(const NRIRenderDevice& frameBuffer)
{
	mSwapChainReady = true;
	EnsureProviderRuntime(frameBuffer);
	RefreshPolicy(frameBuffer, false);
	const bool created = CreateProviderPresentBridge(frameBuffer);
	RefreshPolicy(frameBuffer, true);
	return created;
}

void NRIFrameGenerationContext::Shutdown()
{
	ShutdownProvider();
	mInitialized = false;
	mSwapChainReady = false;
	mHasFrameDesc = false;
	mHasLoggedPolicy = false;
	mPolicy = {};
	mPresentContract = {};
	mLastFrameDesc = {};
	mLastInputAudit = {};
	mResetNextFrame = false;
	CopyString(mPendingResetReason, std::size(mPendingResetReason), "none");
	ResetLowLatencyState();
	ResetProviderState();
}

void NRIFrameGenerationContext::RefreshPolicy(const NRIRenderDevice& frameBuffer, bool logChanges)
{
	const NRIFrameGenerationPresentContract newPresentContract = BuildPresentContract(frameBuffer);
	const NRIFrameGenerationPolicy newPolicy = BuildPolicy(frameBuffer, newPresentContract);
	const bool changed = !ArePoliciesEquivalent(mPolicy, newPolicy);
	mPolicy = newPolicy;
	mPresentContract = newPresentContract;
	mLowLatencyState.interfaceAvailable = mPolicy.lowLatencyInterfaceAvailable;
	mLowLatencyState.swapChainEnabled = mPolicy.lowLatencySwapChainEnabled;

	if (logChanges && (!mHasLoggedPolicy || changed))
	{
		Printf("NRI frame generation policy: request=%s provider=%s operational=%s owner=%s output=%s->%s contract=%s scope=%s api=%s shader_model=%u.%u window=%s dxgi=%s supported=%s low_latency=%s->%s(avail=%s iface=%s swapchain=%s pacing=%s) async=%s->%s(avail=%s) ui=%s->%s swapchain=%s native=device:%s queue:%s swapchain:%s waitable=%s runtime=%s reason=%s\n",
			mPolicy.requestedEnabled ? "on" : "off",
			GetProviderName(mPolicy.provider),
			mPolicy.operational ? "yes" : "no",
			mProviderState.presentBridgeReady ? "proxy" : (frameBuffer.mSwapChain != nullptr ? "native" : "none"),
			GetNRIPTOutputModeName(mPolicy.requestedOutputMode),
			GetNRIPTOutputModeName(mPolicy.resolvedOutputMode),
			GetOutputContractName(mPolicy.resolvedOutputContract),
			mPolicy.outputContractScope,
			mPolicy.selectedApiName,
			mPolicy.shaderModel / 10u,
			mPolicy.shaderModel % 10u,
			GetWindowModeName(mPolicy.windowPresentationMode),
			GetDxgiFullscreenStateName(mPolicy.dxgiFullscreenKnown, mPolicy.dxgiFullscreen),
			GetAvailabilityName(mPolicy.windowModeSupported),
			mPolicy.requestedLowLatency ? "on" : "off",
			mPolicy.resolvedLowLatency ? "on" : "off",
			GetAvailabilityName(mPolicy.lowLatencyAvailable),
			GetAvailabilityName(mPolicy.lowLatencyInterfaceAvailable),
			GetAvailabilityName(mPolicy.lowLatencySwapChainEnabled),
			GetPacingModeName(mPolicy),
			mPolicy.requestedAsync ? "on" : "off",
			mPolicy.resolvedAsync ? "on" : "off",
			GetAvailabilityName(mPolicy.asyncWorkloadAvailable),
			GetUiModeName(mPolicy.requestedUiMode),
			GetUiModeName(mPolicy.resolvedUiMode),
			mPolicy.swapChainReady ? "ready" : "cold",
			mPolicy.nativeDeviceAvailable ? "ok" : "missing",
			mPolicy.nativeGraphicsQueueAvailable ? "ok" : "missing",
			mPolicy.nativeSwapChainAvailable ? "ok" : "missing",
			GetAvailabilityName(mPolicy.waitableSwapChainAvailable),
			GetAvailabilityName(mPolicy.providerRuntimeSupported),
			mPolicy.resolvedReason);
	}

	mHasLoggedPolicy = true;
}

NRIFrameGenerationPolicy NRIFrameGenerationContext::BuildPolicy(const NRIRenderDevice& frameBuffer, const NRIFrameGenerationPresentContract& presentContract) const
{
	const NRIFrameGenerationSettings settings = CaptureSettings();
	NRIFrameGenerationPolicy policy = {};
	policy.initialized = true;
	policy.requestedEnabled = settings.enabled;
	policy.provider = NRIFrameGenerationProvider::FSR3;
	policy.requestedUiMode = settings.uiMode;
	policy.requestedAsync = settings.async;
	policy.requestedLowLatency = settings.lowLatency;
	policy.resolvedUiMode = ResolveUiMode(policy.requestedUiMode);
	policy.requestedOutputMode = presentContract.requestedOutputMode;
	policy.resolvedOutputMode = presentContract.resolvedOutputMode;
	policy.outputContractScope = "d3d12-windowed-or-borderless-sdr-or-scrgb";
	policy.swapChainReady = mSwapChainReady;
	policy.windowPresentationMode = frameBuffer.IsFullscreenModeActive() ? NRIWindowPresentationMode::BorderlessFullscreen : NRIWindowPresentationMode::Windowed;
	const NRIFsr3Dx12PresentBridgeSnapshot& presentSnapshot = mPresentBridge.GetSnapshot();
	policy.dxgiFullscreenKnown = presentSnapshot.dxgiFullscreenKnown;
	policy.dxgiFullscreen = presentSnapshot.dxgiFullscreen;
	policy.windowModeSupported = SupportsFrameGenerationPresentMode(policy.windowPresentationMode, policy.dxgiFullscreenKnown, policy.dxgiFullscreen);
	policy.ffxProxyPacing = mPresentBridge.IsActive();

	const NRIBackendCapabilities backendCapabilities = frameBuffer.BuildBackendCapabilities();
	policy.selectedApiName = GetApiName(backendCapabilities.liveApi);
	policy.apiSupported = backendCapabilities.d3d12;
	policy.nativeDeviceAvailable = backendCapabilities.nativeD3D12DeviceAvailable;
	policy.nativeGraphicsQueueAvailable = backendCapabilities.nativeD3D12GraphicsQueueAvailable;
	policy.nativeSwapChainAvailable = backendCapabilities.nativeD3D12SwapChainAvailable;
	policy.shaderModel = backendCapabilities.shaderModel;
	policy.lowLatencyInterfaceAvailable = backendCapabilities.lowLatencyInterfaceAvailable;
	policy.lowLatencyAvailable = backendCapabilities.lowLatencyAvailable;
	policy.lowLatencySwapChainEnabled = backendCapabilities.lowLatencySwapChainEnabled;
	policy.waitableSwapChainAvailable = backendCapabilities.waitableSwapChainAvailable;
	policy.shaderModelSupported = frameBuffer.mDevice != nullptr && policy.shaderModel >= 62u;
	policy.providerRuntimeSupported = mProviderState.runtimeFunctionsLoaded;
	policy.asyncWorkloadAvailable = false;
	policy.resolvedAsync = false;
	policy.resolvedLowLatency =
		policy.requestedEnabled &&
		policy.requestedLowLatency &&
		policy.apiSupported &&
		policy.windowModeSupported &&
		policy.lowLatencyAvailable &&
		policy.lowLatencySwapChainEnabled;

	if (!policy.requestedEnabled)
	{
		policy.resolvedReason = "disabled-by-cvar";
		return policy;
	}

	if (frameBuffer.mDevice == nullptr)
	{
		policy.resolvedReason = "device-unavailable";
		return policy;
	}

	if (!presentContract.proxyAllowed)
	{
		policy.resolvedOutputContract = NRIFrameGenerationOutputContract::Unsupported;
		policy.resolvedReason = presentContract.resolvedReason;
		return policy;
	}

	policy.resolvedOutputContract = presentContract.hdrContext ?
		NRIFrameGenerationOutputContract::ScRGBDisplayReady :
		NRIFrameGenerationOutputContract::SDRDisplayReady;

	if (!policy.apiSupported)
	{
		policy.resolvedReason = "api-not-d3d12";
		return policy;
	}

	if (!policy.windowModeSupported)
	{
		policy.resolvedReason = "dxgi-fullscreen-unsupported";
		return policy;
	}

	if (!policy.shaderModelSupported)
	{
		policy.resolvedReason = "shader-model-below-6.2";
		return policy;
	}

	if (!policy.nativeDeviceAvailable)
	{
		policy.resolvedReason = "native-device-unavailable";
		return policy;
	}

	if (!policy.nativeGraphicsQueueAvailable)
	{
		policy.resolvedReason = "native-queue-unavailable";
		return policy;
	}

	if (!policy.swapChainReady)
	{
		policy.resolvedReason = "swapchain-cold";
		return policy;
	}

	if (!policy.providerRuntimeSupported)
	{
		policy.resolvedReason = "provider-runtime-unavailable";
		return policy;
	}

	if (!mProviderState.presentBridgeReady)
	{
		policy.resolvedReason = "provider-present-unavailable";
		return policy;
	}

	policy.operational = true;
	policy.resolvedAsync = policy.requestedAsync && policy.asyncWorkloadAvailable;
	policy.resolvedLowLatency = policy.requestedLowLatency && policy.lowLatencyAvailable && policy.lowLatencySwapChainEnabled;
	policy.resolvedReason = "enabled";
	return policy;
}

NRIFrameGenerationPresentContract NRIFrameGenerationContext::BuildPresentContract(const NRIRenderDevice& frameBuffer) const
{
	NRIFrameGenerationPresentContract contract = {};
	const NRIPTOutputPolicy outputPolicy = frameBuffer.GetPathTracingOutputPolicy();
	const nri::GraphicsAPI api = frameBuffer.GetLiveAPI();
	contract.initialized = true;
	contract.requestedOutputMode = outputPolicy.requestedMode;
	contract.resolvedOutputMode = outputPolicy.resolvedMode;
	contract.createdSwapChainFormat = frameBuffer.mCreatedSwapChainFormat;
	contract.resolvedTextureFormat = frameBuffer.mResolvedSwapChainTextureFormat;
	contract.usesHdrSwapChain = outputPolicy.hdrSwapChainActive;
	contract.hdrPaperWhiteScale = GetNRIPTHdrPaperWhiteScale(outputPolicy);
	const float safeSdrNits = GetNRIPTOutputSafeDisplaySdrLuminance(outputPolicy.displaySdrLuminance);
	const float safeMaxNits = GetNRIPTOutputSafeDisplayMaxLuminance(outputPolicy.displaySdrLuminance, outputPolicy.displayMaxLuminance);
	contract.minLuminanceNits =
		std::isfinite(outputPolicy.displayMinLuminance) && outputPolicy.displayMinLuminance >= 0.0f && outputPolicy.displayMinLuminance < safeMaxNits ?
			outputPolicy.displayMinLuminance : 0.0f;
	contract.maxLuminanceNits = outputPolicy.hdrSwapChainActive ? safeMaxNits : safeSdrNits;

#ifdef _WIN32
	if (api != nri::GraphicsAPI::D3D12)
	{
		contract.proxyAllowed = false;
		contract.resolvedReason = "api-not-d3d12";
		return contract;
	}

	switch (contract.createdSwapChainFormat)
	{
	case nri::SwapChainFormat::BT709_G22_8BIT:
		contract.resolvedDxgiFormat = (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM;
		contract.resolvedDxgiFormatValid = true;
		contract.requestedDxgiColorSpace = (uint32_t)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		contract.requestedDxgiColorSpaceValid = true;
		contract.transferFunction = NRIFrameGenerationPresentTransferFunction::SRGB;
		if (contract.resolvedTextureFormat == nri::Format::UNKNOWN)
			contract.resolvedTextureFormat = nri::Format::RGBA8_UNORM;
		break;
	case nri::SwapChainFormat::BT709_G22_10BIT:
		contract.resolvedDxgiFormat = (uint32_t)DXGI_FORMAT_R10G10B10A2_UNORM;
		contract.resolvedDxgiFormatValid = true;
		contract.requestedDxgiColorSpace = (uint32_t)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		contract.requestedDxgiColorSpaceValid = true;
		contract.transferFunction = NRIFrameGenerationPresentTransferFunction::SRGB;
		break;
	case nri::SwapChainFormat::BT709_G10_16BIT:
		contract.resolvedDxgiFormat = (uint32_t)DXGI_FORMAT_R16G16B16A16_FLOAT;
		contract.resolvedDxgiFormatValid = true;
		contract.requestedDxgiColorSpace = (uint32_t)DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
		contract.requestedDxgiColorSpaceValid = true;
		contract.transferFunction = NRIFrameGenerationPresentTransferFunction::ScRGB;
		contract.hdrContext = true;
		if (contract.resolvedTextureFormat == nri::Format::UNKNOWN)
			contract.resolvedTextureFormat = nri::Format::RGBA16_SFLOAT;
		break;
	case nri::SwapChainFormat::BT2020_G2084_10BIT:
		contract.resolvedDxgiFormat = (uint32_t)DXGI_FORMAT_R10G10B10A2_UNORM;
		contract.resolvedDxgiFormatValid = true;
		contract.requestedDxgiColorSpace = (uint32_t)DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		contract.requestedDxgiColorSpaceValid = true;
		contract.transferFunction = NRIFrameGenerationPresentTransferFunction::PQ;
		contract.hdrContext = true;
		break;
	default:
		break;
	}

	if (frameBuffer.mCurrentPresentTarget != nullptr)
	{
		contract.activePresentTargetFormat = frameBuffer.mCurrentPresentTarget->format;
		if (ID3D12Resource* nativePresentTarget = GetNativeTexture(frameBuffer.mCore, frameBuffer.mCurrentPresentTarget))
		{
			contract.activePresentTargetDxgiFormat = (uint32_t)nativePresentTarget->GetDesc().Format;
			contract.activePresentTargetDxgiFormatValid = true;
		}
	}

	const NRIFsr3Dx12PresentBridgeSnapshot& presentSnapshot = mPresentBridge.GetSnapshot();
	if (mPresentBridge.IsActive() && presentSnapshot.observedColorSpaceValid)
	{
		contract.observedDxgiColorSpace = presentSnapshot.observedColorSpace;
		contract.observedDxgiColorSpaceValid = true;
	}
#else
	(void)outputPolicy;
#endif

	if (!contract.resolvedDxgiFormatValid)
	{
		contract.proxyAllowed = false;
		contract.resolvedReason = "swapchain-format-unmapped";
		return contract;
	}

	if (!contract.requestedDxgiColorSpaceValid)
	{
		contract.proxyAllowed = false;
		contract.resolvedReason = "swapchain-color-space-unmapped";
		return contract;
	}

	if (contract.requestedOutputMode == NRIPTOutputMode::HDR10PQ ||
		contract.createdSwapChainFormat == nri::SwapChainFormat::BT2020_G2084_10BIT)
	{
		contract.proxyAllowed = false;
		contract.resolvedReason = "direct-pq-deferred";
		return contract;
	}

	if (contract.createdSwapChainFormat == nri::SwapChainFormat::BT709_G22_8BIT)
	{
		if (contract.requestedOutputMode != NRIPTOutputMode::SDR ||
			contract.resolvedOutputMode != NRIPTOutputMode::SDR || contract.usesHdrSwapChain)
		{
			contract.proxyAllowed = false;
			contract.resolvedReason = "sdr-contract-mismatch";
			return contract;
		}
		if (frameBuffer.mResolvedSwapChainTextureFormat != nri::Format::UNKNOWN &&
			frameBuffer.mResolvedSwapChainTextureFormat != nri::Format::RGBA8_UNORM)
		{
			contract.proxyAllowed = false;
			contract.resolvedReason = "sdr-present-format-mismatch";
			return contract;
		}
		contract.proxyAllowed = true;
		contract.resolvedReason = "proxy-sdr-display-ready";
		return contract;
	}

	if (contract.createdSwapChainFormat == nri::SwapChainFormat::BT709_G10_16BIT)
	{
		const bool requestedScRgb = contract.requestedOutputMode == NRIPTOutputMode::HDR ||
			contract.requestedOutputMode == NRIPTOutputMode::HDRLinear16;
		if (!requestedScRgb || contract.resolvedOutputMode != NRIPTOutputMode::HDRLinear16 || !contract.usesHdrSwapChain)
		{
			contract.proxyAllowed = false;
			contract.resolvedReason = "scrgb-contract-mismatch";
			return contract;
		}
		if (frameBuffer.mResolvedSwapChainTextureFormat != nri::Format::UNKNOWN &&
			frameBuffer.mResolvedSwapChainTextureFormat != nri::Format::RGBA16_SFLOAT)
		{
			contract.proxyAllowed = false;
			contract.resolvedReason = "scrgb-present-format-mismatch";
			return contract;
		}
		contract.proxyAllowed = true;
		contract.resolvedReason = "proxy-scrgb-display-ready";
		return contract;
	}

	contract.proxyAllowed = false;
	contract.resolvedReason = "swapchain-contract-unsupported";
	return contract;
}

void NRIFrameGenerationContext::OnSwapChainCreated(const NRIRenderDevice& frameBuffer)
{
	mSwapChainReady = true;
	ResetLowLatencyState();
	EnsureProviderRuntime(frameBuffer);
	RefreshPolicy(frameBuffer, false);
	ConfigureLowLatencyMode(frameBuffer);
	RefreshPolicy(frameBuffer, true);
}

void NRIFrameGenerationContext::OnSwapChainDestroyed(const NRIRenderDevice& frameBuffer)
{
	(void)frameBuffer;
	mSwapChainReady = false;
	DestroyProviderPresentBridge();
	ResetLowLatencyState();
	RefreshPolicy(frameBuffer, false);
}

void NRIFrameGenerationContext::BeginFrame(const NRIRenderDevice& frameBuffer)
{
	EnsureProviderRuntime(frameBuffer);
	RefreshPolicy(frameBuffer, false);
	RefreshPolicy(frameBuffer, true);
	mLowLatencyState.sleepInvoked = false;
	mLowLatencyState.presentBoundarySeen = false;
	mLowLatencyState.latencySleepResult = nri::Result::FAILURE;
	mLowLatencyState.simulationStartMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.simulationEndMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.renderSubmitStartMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.renderSubmitEndMarkerResult = nri::Result::FAILURE;
	mLowLatencyState.latencyReportResult = nri::Result::FAILURE;
	mLowLatencyState.latencyReport = {};
	mProviderState.configuredThisFrame = false;
	mProviderState.prepareDispatchedThisFrame = false;
	mProviderState.prepareCameraInfoProvided = false;
	mProviderState.presentUsedBridgeThisFrame = false;
	mProviderState.presentGeneratedThisFrame = false;
	ConfigureLowLatencyMode(frameBuffer);
	if (!IsLowLatencyOperational(frameBuffer))
	{
		return;
	}

	if (frameBuffer.mSwapChain != nullptr)
	{
		mLowLatencyState.latencySleepResult = frameBuffer.mLowLatency.LatencySleep(*frameBuffer.mSwapChain);
		mLowLatencyState.sleepInvoked = mLowLatencyState.latencySleepResult == nri::Result::SUCCESS;
		if (mLowLatencyState.sleepInvoked)
		{
			mLowLatencyState.latencySleepCount++;
		}
	}

	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::SIMULATION_START, mLowLatencyState.simulationStartMarkerResult);
}

void NRIFrameGenerationContext::EndFrame(const NRIRenderDevice& frameBuffer)
{
	RefreshPolicy(frameBuffer, false);
}

void NRIFrameGenerationContext::SetFrameDesc(const NRIRenderDevice& frameBuffer, const NRIFrameGenerationFrameDesc& desc)
{
	(void)frameBuffer;
	mLastFrameDesc = desc;
	const bool forcedReset = mResetNextFrame;
	if (forcedReset)
	{
		mLastFrameDesc.resetHistory = true;
		CopyString(mLastFrameDesc.resetReason, std::size(mLastFrameDesc.resetReason), mPendingResetReason);
		mResetNextFrame = false;
		CopyString(mPendingResetReason, std::size(mPendingResetReason), "none");
	}
	mLastInputAudit = BuildInputAudit(mLastFrameDesc);
	mHasFrameDesc = true;
	if (desc.resetHistory && !forcedReset)
	{
		NoteReset(desc.resetReason);
	}
}

void NRIFrameGenerationContext::SetUiTexture(const NRITextureResource* uiTexture)
{
	if (!mHasFrameDesc)
	{
		return;
	}

	mLastFrameDesc.uiTexture = uiTexture;
}

void NRIFrameGenerationContext::ConfigureAndDispatchFrame(const NRIRenderDevice& frameBuffer)
{
	if (!mHasFrameDesc)
	{
		return;
	}

	ConfigureAndPrepareProvider(frameBuffer, mLastFrameDesc);
}

bool NRIFrameGenerationContext::Present(const NRIRenderDevice& frameBuffer, bool vsync, bool allowTearing, nri::Result& outResult)
{
	(void)frameBuffer;
	outResult = nri::Result::FAILURE;
#ifndef _WIN32
	return false;
#else
	if (!ShouldUsePresentBridge())
	{
		return false;
	}

	const HRESULT hr = static_cast<HRESULT>(mPresentBridge.Present(vsync, allowTearing));
	RefreshPresentBridgeSnapshot();
	outResult = GetNriPresentResult(hr);
	mProviderState.lastPresentResult = outResult;
	mProviderState.presentUsedBridgeThisFrame = true;
	mProviderState.presentGeneratedThisFrame = mProviderState.frameGenerationDispatchedThisFrame;
	CopyString(mProviderState.lastPresentMode, std::size(mProviderState.lastPresentMode),
		GetPresentModeName(true, mProviderState.frameGenerationDispatchedThisFrame));
	++mProviderState.presentCount;
	if (FAILED(hr))
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "proxy-present-failed");
	}
	return true;
#endif
}

void NRIFrameGenerationContext::NoteReset(const char* reason)
{
	CopyString(mProviderState.lastResetReason, std::size(mProviderState.lastResetReason), GetSafeResetReason(reason));
	++mProviderState.resetCount;
}

void NRIFrameGenerationContext::RequestHistoryReset(const char* reason)
{
	NoteReset(reason);
	mResetNextFrame = true;
	CopyString(mPendingResetReason, std::size(mPendingResetReason), GetSafeResetReason(reason));
}

bool NRIFrameGenerationContext::DrainPresentBridge()
{
	if (!IsPresentBridgeActive())
		return true;
	const uint32_t result = mPresentBridge.Drain(mFfxDispatchFn);
	bool detached = true;
#ifdef _WIN32
	const auto configure = reinterpret_cast<PfnFfxConfigure>(mFfxConfigureFn);
	if (configure != nullptr && mFfxContext != nullptr)
	{
		ffxConfigureDescFrameGeneration disableDesc = {};
		NriFfxInitHeader(disableDesc.header, NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
		disableDesc.swapChain = mPresentBridge.GetSwapChain();
		disableDesc.frameGenerationEnabled = false;
		disableDesc.frameID = mProviderState.lastConfiguredFrameId + 1u;
		mProviderState.lastConfigureResult = configure(reinterpret_cast<ffxContext*>(&mFfxContext), &disableDesc.header);
		detached = mProviderState.lastConfigureResult == NRI_FFX_API_RETURN_OK;
	}
	if (configure != nullptr && mPresentBridge.GetContext() != nullptr)
	{
		ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiDesc = {};
		NriFfxInitHeader(uiDesc.header, NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12);
		mProviderState.lastSwapChainConfigureResult = configure(reinterpret_cast<ffxContext*>(mPresentBridge.GetContextAddress()), &uiDesc.header);
		detached = detached && mProviderState.lastSwapChainConfigureResult == NRI_FFX_API_RETURN_OK;
	}
#endif
	mProviderState.presentBridgeDetached = detached;
	mProviderState.uiResourceRegisteredThisFrame = false;
	RefreshPresentBridgeSnapshot();
#ifdef _WIN32
	return result == NRI_FFX_API_RETURN_OK && detached;
#else
	return false;
#endif
}

bool NRIFrameGenerationContext::WaitForPresentPacing()
{
	if (!IsPresentBridgeActive())
		return true;
	const bool ready = mPresentBridge.WaitForPacing(1000u);
	RefreshPresentBridgeSnapshot();
	return ready;
}

void NRIFrameGenerationContext::RequestNativeFallback(const char* reason)
{
	mProviderState.nativeFallbackRequested = true;
	NoteReset(reason);
	CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), GetSafeResetReason(reason));
}

bool NRIFrameGenerationContext::ConsumeNativeFallbackRequest()
{
	const bool requested = mProviderState.nativeFallbackRequested;
	mProviderState.nativeFallbackRequested = false;
	return requested;
}

NRIFrameGenerationInputAudit NRIFrameGenerationContext::BuildInputAudit(const NRIFrameGenerationFrameDesc& desc) const
{
	NRIFrameGenerationInputAudit audit = {};
	audit.renderRectValid =
		desc.renderRect.left == 0 &&
		desc.renderRect.top == 0 &&
		desc.renderRect.width == desc.renderWidth &&
		desc.renderRect.height == desc.renderHeight &&
		desc.renderRect.width <= 0x7fffffffu &&
		desc.renderRect.height <= 0x7fffffffu;
	const int64_t outputRectRight = (int64_t)desc.outputRect.left + desc.outputRect.width;
	const int64_t outputRectBottom = (int64_t)desc.outputRect.top + desc.outputRect.height;
	audit.outputRectValid =
		desc.outputRect.width > 0u &&
		desc.outputRect.height > 0u &&
		desc.outputRect.width <= 0x7fffffffu &&
		desc.outputRect.height <= 0x7fffffffu &&
		desc.outputRect.left >= -32768 &&
		desc.outputRect.left <= 32767 &&
		desc.outputRect.top >= -32768 &&
		desc.outputRect.top <= 32767 &&
		(int64_t)desc.outputRect.left < desc.outputWidth &&
		(int64_t)desc.outputRect.top < desc.outputHeight &&
		outputRectRight > 0 &&
		outputRectBottom > 0 &&
		outputRectRight <= 0x7fffffffll &&
		outputRectBottom <= 0x7fffffffll;
	audit.currentJitterValid = true;
	audit.previousJitterValid = desc.hasPreviousCamera;
	audit.hudlessColorAvailable = desc.hudlessColor != nullptr;
	audit.motionVectorsAvailable = desc.motionVectors != nullptr;
	audit.depthAvailable = desc.depth != nullptr;
	audit.motionResolutionMatchesRender =
		audit.motionVectorsAvailable &&
		desc.motionVectors->width == desc.renderWidth &&
		desc.motionVectors->height == desc.renderHeight;
	audit.depthResolutionMatchesRender =
		audit.depthAvailable &&
		desc.depth->width == desc.renderWidth &&
		desc.depth->height == desc.renderHeight;
	audit.fsr3MotionCompatible =
		audit.motionResolutionMatchesRender &&
		desc.motionVectorSpace == NRIFrameGenerationMotionVectorSpace::ScreenPixels &&
		desc.motionVectorDirection == NRIFrameGenerationMotionVectorDirection::CurrentToPrevious &&
		desc.motionVectorScale[0] == 1.0f &&
		desc.motionVectorScale[1] == 1.0f;
	audit.fsr3DepthCompatible =
		audit.depthResolutionMatchesRender &&
		desc.depthType == NRIFrameGenerationDepthType::ClipDepth;
	audit.fsr3PrepareInputsRequired = true;
	audit.complete =
		audit.renderRectValid &&
		audit.outputRectValid &&
		audit.currentJitterValid &&
		audit.hudlessColorAvailable &&
		audit.motionVectorsAvailable &&
		audit.depthAvailable;

	if (!audit.complete)
	{
		const char* statusReason = !audit.outputRectValid ? "invalid-generation-rect" : "missing-required-input";
		std::strncpy(audit.statusReason, statusReason, std::size(audit.statusReason) - 1u);
		audit.statusReason[std::size(audit.statusReason) - 1u] = '\0';
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::MotionAndDepth;
		return audit;
	}

	if (!audit.fsr3MotionCompatible && !audit.fsr3DepthCompatible)
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::MotionAndDepth;
		std::strncpy(audit.statusReason, "fsr3-motion-depth-adapter", std::size(audit.statusReason) - 1u);
	}
	else if (!audit.fsr3MotionCompatible)
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::MotionVectors;
		std::strncpy(audit.statusReason, "fsr3-motion-adapter", std::size(audit.statusReason) - 1u);
	}
	else if (!audit.fsr3DepthCompatible)
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::Depth;
		std::strncpy(audit.statusReason, "fsr3-depth-adapter", std::size(audit.statusReason) - 1u);
	}
	else
	{
		audit.adapterRequirement = NRIFrameGenerationAdapterRequirement::None;
		std::strncpy(audit.statusReason, "fsr3-prepare-pass-pending", std::size(audit.statusReason) - 1u);
	}

	audit.statusReason[std::size(audit.statusReason) - 1u] = '\0';
	return audit;
}

void NRIFrameGenerationContext::OnSimulationEnd(const NRIRenderDevice& frameBuffer)
{
	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::SIMULATION_END, mLowLatencyState.simulationEndMarkerResult);
}

void NRIFrameGenerationContext::OnRenderSubmitStart(const NRIRenderDevice& frameBuffer)
{
	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::RENDER_SUBMIT_START, mLowLatencyState.renderSubmitStartMarkerResult);
}

void NRIFrameGenerationContext::OnRenderSubmitEnd(const NRIRenderDevice& frameBuffer)
{
	SetLowLatencyMarker(frameBuffer, nri::LatencyMarker::RENDER_SUBMIT_END, mLowLatencyState.renderSubmitEndMarkerResult);
}

void NRIFrameGenerationContext::OnPresentStart(const NRIRenderDevice&)
{
	mLowLatencyState.presentBoundarySeen = true;
}

void NRIFrameGenerationContext::OnPresentEnd(const NRIRenderDevice& frameBuffer, nri::Result presentResult)
{
	mProviderState.lastPresentResult = presentResult;
	if (presentResult == nri::Result::SUCCESS && !mProviderState.presentUsedBridgeThisFrame)
	{
		CopyString(mProviderState.lastPresentMode, std::size(mProviderState.lastPresentMode), "native");
		++mProviderState.presentCount;
	}
	mLowLatencyState.presentBoundarySeen = mLowLatencyState.presentBoundarySeen || presentResult == nri::Result::SUCCESS;
	if (!IsLowLatencyOperational(frameBuffer) || presentResult != nri::Result::SUCCESS || frameBuffer.mSwapChain == nullptr)
	{
		return;
	}

	mLowLatencyState.latencyReportResult = frameBuffer.mLowLatency.GetLatencyReport(*frameBuffer.mSwapChain, mLowLatencyState.latencyReport);
}

bool NRIFrameGenerationContext::IsLowLatencyOperational(const NRIRenderDevice& frameBuffer) const
{
	return
		mPolicy.resolvedLowLatency &&
		frameBuffer.mSwapChain != nullptr &&
		frameBuffer.mLowLatency.SetLatencySleepMode != nullptr &&
		frameBuffer.mLowLatency.SetLatencyMarker != nullptr &&
		frameBuffer.mLowLatency.LatencySleep != nullptr &&
		frameBuffer.mLowLatency.GetLatencyReport != nullptr;
}

void NRIFrameGenerationContext::ConfigureLowLatencyMode(const NRIRenderDevice& frameBuffer)
{
	if (frameBuffer.mSwapChain == nullptr || frameBuffer.mLowLatency.SetLatencySleepMode == nullptr || !mPolicy.lowLatencySwapChainEnabled)
	{
		return;
	}

	nri::LatencySleepMode sleepMode = {};
	sleepMode.minIntervalUs = 0;
	sleepMode.lowLatencyMode = mPolicy.resolvedLowLatency;
	sleepMode.lowLatencyBoost = false;
	mLowLatencyState.configuredSleepMode = sleepMode;
	mLowLatencyState.setSleepModeResult = frameBuffer.mLowLatency.SetLatencySleepMode(*frameBuffer.mSwapChain, sleepMode);
	mLowLatencyState.sleepModeConfigured = mLowLatencyState.setSleepModeResult == nri::Result::SUCCESS;
}

void NRIFrameGenerationContext::SetLowLatencyMarker(const NRIRenderDevice& frameBuffer, nri::LatencyMarker marker, nri::Result& resultSlot)
{
	resultSlot = nri::Result::FAILURE;
	if (!IsLowLatencyOperational(frameBuffer))
	{
		return;
	}

	resultSlot = frameBuffer.mLowLatency.SetLatencyMarker(*frameBuffer.mSwapChain, marker);
	if (resultSlot == nri::Result::SUCCESS)
	{
		mLowLatencyState.markerCount++;
	}
}

void NRIFrameGenerationContext::ResetLowLatencyState()
{
	mLowLatencyState = {};
}

void NRIFrameGenerationContext::ResetProviderState()
{
	mProviderState = {};
	mProviderState.noSwapChainNotify = true;
	mProviderState.lastPresentResult = nri::Result::FAILURE;
	std::strncpy(mProviderState.runtimeLibrary, "unloaded", std::size(mProviderState.runtimeLibrary) - 1u);
	std::strncpy(mProviderState.providerVersion, "unknown", std::size(mProviderState.providerVersion) - 1u);
	std::strncpy(mProviderState.lastResetReason, "none", std::size(mProviderState.lastResetReason) - 1u);
	std::strncpy(mProviderState.lastPresentMode, "none", std::size(mProviderState.lastPresentMode) - 1u);
	std::strncpy(mProviderState.lastStatusReason, "not-loaded", std::size(mProviderState.lastStatusReason) - 1u);
	mProviderState.runtimeLibrary[std::size(mProviderState.runtimeLibrary) - 1u] = '\0';
	mProviderState.providerVersion[std::size(mProviderState.providerVersion) - 1u] = '\0';
	mProviderState.lastResetReason[std::size(mProviderState.lastResetReason) - 1u] = '\0';
	mProviderState.lastPresentMode[std::size(mProviderState.lastPresentMode) - 1u] = '\0';
	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
}

void NRIFrameGenerationContext::RefreshPresentBridgeSnapshot()
{
	const NRIFsr3Dx12PresentBridgeSnapshot& snapshot = mPresentBridge.GetSnapshot();
	mProviderState.dxgiFullscreenKnown = snapshot.dxgiFullscreenKnown;
	mProviderState.dxgiFullscreen = snapshot.dxgiFullscreen;
	mProviderState.tearingSupported = snapshot.tearingSupported;
	mProviderState.bridgeCreateGeneration = snapshot.createGeneration;
	mProviderState.bridgeCreateAttemptCount = snapshot.createAttemptCount;
	mProviderState.bridgeDrainCount = snapshot.drainCount;
	mProviderState.waitableObjectAvailable = snapshot.waitableObjectAvailable;
	mProviderState.pacingWaitCount = snapshot.pacingWaitCount;
	mProviderState.pacingWaitTimeoutCount = snapshot.pacingWaitTimeoutCount;
	mProviderState.lastPacingWaitResult = snapshot.lastWaitResult;
	mProviderState.lastBridgeDrainResult = snapshot.lastDrainResult;
	mProviderState.lastPresentHresult = snapshot.lastPresentHresult;
	mProviderState.lastPresentSyncInterval = snapshot.lastPresentSyncInterval;
	mProviderState.lastPresentFlags = snapshot.lastPresentFlags;
}

void NRIFrameGenerationContext::DestroyProviderPresentBridge()
{
#ifdef _WIN32
	mPresentBridge.Destroy(mFfxDispatchFn, mFfxDestroyContextFn, mFfxAllocCallbacks);
	RefreshPresentBridgeSnapshot();
#endif

	mProviderState.swapChainContextCreated = false;
	mProviderState.presentBridgeReady = false;
	mProviderState.uiResourceRegisteredThisFrame = false;
	mProviderState.swapChainMemoryUsageValid = false;
	mProviderState.swapChainTotalUsageBytes = 0;
	mProviderState.swapChainAliasableUsageBytes = 0;
}

void NRIFrameGenerationContext::ShutdownProvider()
{
#ifdef _WIN32
	const auto destroyContext = reinterpret_cast<PfnFfxDestroyContext>(mFfxDestroyContextFn);
	const auto allocationCallbacks = reinterpret_cast<ffxAllocationCallbacks*>(mFfxAllocCallbacks);
	DestroyProviderPresentBridge();
	if (mFfxContext != nullptr && destroyContext != nullptr)
	{
		ffxContext context = mFfxContext;
		mProviderState.lastDestroyResult = destroyContext(&context, allocationCallbacks);
		mFfxContext = context;
	}
	else
	{
		mProviderState.lastDestroyResult = NRI_FFX_API_RETURN_OK;
	}

	if (allocationCallbacks != nullptr)
	{
		delete allocationCallbacks;
		mFfxAllocCallbacks = nullptr;
	}

	if (mFfxModule != nullptr)
	{
		FreeLibrary(reinterpret_cast<HMODULE>(mFfxModule));
		mFfxModule = nullptr;
	}
#endif

	mFfxContext = nullptr;
	mFfxCreateContextFn = nullptr;
	mFfxDestroyContextFn = nullptr;
	mFfxConfigureFn = nullptr;
	mFfxQueryFn = nullptr;
	mFfxDispatchFn = nullptr;
	ResetProviderState();
	std::strncpy(mProviderState.lastStatusReason, "shutdown", std::size(mProviderState.lastStatusReason) - 1u);
	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
}

bool NRIFrameGenerationContext::EnsureProviderRuntime(const NRIRenderDevice& frameBuffer)
{
#ifndef _WIN32
	(void)frameBuffer;
	std::strncpy(mProviderState.lastStatusReason, "win32-only", std::size(mProviderState.lastStatusReason) - 1u);
	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
	return false;
#else
	const NRIFrameGenerationSettings settings = CaptureSettings();
	if (!settings.enabled)
	{
		std::strncpy(mProviderState.lastStatusReason, "disabled-by-cvar", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (frameBuffer.GetLiveAPI() != nri::GraphicsAPI::D3D12)
	{
		std::strncpy(mProviderState.lastStatusReason, "api-not-d3d12", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (mProviderState.runtimeFunctionsLoaded)
	{
		return true;
	}

	const char* libraryName = GetFfxLibraryName(NRIFrameGenerationProvider::FSR3);
	CopyString(mProviderState.runtimeLibrary, std::size(mProviderState.runtimeLibrary), libraryName);

	if (mFfxModule == nullptr)
	{
		mFfxModule = LoadLibraryA(libraryName);
		if (mFfxModule == nullptr)
		{
			mProviderState.runtimeLoaded = false;
			mProviderState.runtimeFunctionsLoaded = false;
			std::strncpy(mProviderState.lastStatusReason, "runtime-dll-missing", std::size(mProviderState.lastStatusReason) - 1u);
			mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
			return false;
		}

		mProviderState.runtimeLoaded = true;
	}

	if (mFfxAllocCallbacks == nullptr)
	{
		auto* allocationCallbacks = new ffxAllocationCallbacks{};
		allocationCallbacks->pUserData = nullptr;
		allocationCallbacks->alloc = &NriFfxAlloc;
		allocationCallbacks->dealloc = &NriFfxDealloc;
		mFfxAllocCallbacks = allocationCallbacks;
	}

	mFfxCreateContextFn = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mFfxModule), "ffxCreateContext"));
	mFfxDestroyContextFn = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mFfxModule), "ffxDestroyContext"));
	mFfxConfigureFn = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mFfxModule), "ffxConfigure"));
	mFfxQueryFn = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mFfxModule), "ffxQuery"));
	mFfxDispatchFn = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mFfxModule), "ffxDispatch"));

	mProviderState.runtimeFunctionsLoaded =
		mFfxCreateContextFn != nullptr &&
		mFfxDestroyContextFn != nullptr &&
		mFfxConfigureFn != nullptr &&
		mFfxQueryFn != nullptr &&
		mFfxDispatchFn != nullptr;

	if (!mProviderState.runtimeFunctionsLoaded)
	{
		std::strncpy(mProviderState.lastStatusReason, "runtime-symbol-missing", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	std::strncpy(mProviderState.lastStatusReason, "runtime-loaded", std::size(mProviderState.lastStatusReason) - 1u);
	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
	return true;
#endif
}

bool NRIFrameGenerationContext::CreateProviderPresentBridge(const NRIRenderDevice& frameBuffer)
{
#ifndef _WIN32
	(void)frameBuffer;
	return false;
#else
	const NRIFrameGenerationSettings settings = CaptureSettings();
	if (!EnsureProviderRuntime(frameBuffer))
	{
		return false;
	}

	if (!settings.enabled)
	{
		std::strncpy(mProviderState.lastStatusReason, "disabled-by-cvar", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (frameBuffer.GetLiveAPI() != nri::GraphicsAPI::D3D12)
	{
		std::strncpy(mProviderState.lastStatusReason, "api-not-d3d12", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (frameBuffer.mSwapChain != nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "native-swapchain-active", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (!mPresentContract.initialized)
	{
		std::strncpy(mProviderState.lastStatusReason, "present-contract-uninitialized", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	const NRIFrameGenerationPresentContract& presentContract = mPresentContract;
	if (!presentContract.proxyAllowed)
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), presentContract.resolvedReason);
		return false;
	}

	if (frameBuffer.GetNativeD3D12GraphicsQueue() == nullptr || mainwindow.GetHandle() == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "present-bridge-prereq-missing", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (IsPresentBridgeActive())
	{
		mProviderState.swapChainContextCreated = true;
		mProviderState.presentBridgeReady = true;
		return true;
	}

	if (mFfxCreateContextFn == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "runtime-symbol-missing", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	auto& mutableFrameBuffer = const_cast<NRIRenderDevice&>(frameBuffer);
	NRIFsr3Dx12PresentBridgeCreateDesc createDesc = {};
	createDesc.windowHandle = mainwindow.GetHandle();
	createDesc.gameQueue = frameBuffer.GetNativeD3D12GraphicsQueue();
	createDesc.createContextFn = mFfxCreateContextFn;
	createDesc.queryFn = mFfxQueryFn;
	createDesc.allocationCallbacks = mFfxAllocCallbacks;
	createDesc.width = (uint32_t)(std::max)(mutableFrameBuffer.GetClientWidth(), 1);
	createDesc.height = (uint32_t)(std::max)(mutableFrameBuffer.GetClientHeight(), 1);
	createDesc.format = presentContract.resolvedDxgiFormat;
	createDesc.colorSpace = presentContract.requestedDxgiColorSpace;
	createDesc.bufferCount = (std::max<uint32_t>)(frameBuffer.mSwapChainTextureCount != 0u ? frameBuffer.mSwapChainTextureCount : 3u, 2u);
	bool created = mPresentBridge.Create(createDesc);
	RefreshPresentBridgeSnapshot();
	const NRIFsr3Dx12PresentBridgeSnapshot& snapshot = mPresentBridge.GetSnapshot();
	mProviderState.lastSwapChainCreateResult = snapshot.lastCreateResult;
	mProviderState.lastSwapChainQueryResult = snapshot.lastQueryResult;
	mProviderState.swapChainMemoryUsageValid = snapshot.memoryUsageValid;
	mProviderState.swapChainTotalUsageBytes = snapshot.totalUsageBytes;
	mProviderState.swapChainAliasableUsageBytes = snapshot.aliasableUsageBytes;
	if (!created)
	{
		if (!snapshot.colorSpaceSupportValid)
			CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "color-space-check-failed");
		else if ((snapshot.colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0u)
			CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "color-space-present-unsupported");
		else if (!snapshot.colorSpaceSet)
			CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "color-space-set-failed");
		else if (!snapshot.observedColorSpaceValid || snapshot.observedColorSpace != snapshot.requestedColorSpace)
			CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "color-space-observation-mismatch");
	}
	if (created && (!snapshot.windowAssociationKnown || !snapshot.windowAssociationSucceeded || !snapshot.dxgiFullscreenKnown || snapshot.dxgiFullscreen))
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason),
			!snapshot.windowAssociationSucceeded ? "window-association-failed" : "dxgi-fullscreen-unsupported");
		mPresentBridge.Destroy(mFfxDispatchFn, mFfxDestroyContextFn, mFfxAllocCallbacks);
		RefreshPresentBridgeSnapshot();
		created = false;
	}
	if (created)
	{
		mProviderState.swapChainContextCreated = snapshot.contextCreated;
		mProviderState.presentBridgeReady = snapshot.swapChainCreated;
		mProviderState.presentBridgeDetached = false;
		std::strncpy(mProviderState.lastStatusReason, "present-bridge-ready", std::size(mProviderState.lastStatusReason) - 1u);
	}
	else
	{
		if (mPresentBridge.GetContext() != nullptr || mPresentBridge.GetSwapChain() != nullptr)
		{
			mPresentBridge.Destroy(mFfxDispatchFn, mFfxDestroyContextFn, mFfxAllocCallbacks);
			RefreshPresentBridgeSnapshot();
		}
		mProviderState.swapChainContextCreated = false;
		mProviderState.presentBridgeReady = false;
		if (std::strcmp(mProviderState.lastStatusReason, "window-association-failed") != 0 &&
			std::strcmp(mProviderState.lastStatusReason, "dxgi-fullscreen-unsupported") != 0 &&
			std::strncmp(mProviderState.lastStatusReason, "color-space-", 12u) != 0)
		{
			std::strncpy(mProviderState.lastStatusReason, "present-bridge-create-failed", std::size(mProviderState.lastStatusReason) - 1u);
		}
	}

	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
	return mProviderState.presentBridgeReady;
#endif
}

bool NRIFrameGenerationContext::EnsureProviderContext(const NRIRenderDevice& frameBuffer, const NRIFrameGenerationFrameDesc& desc)
{
#ifndef _WIN32
	(void)frameBuffer;
	(void)desc;
	return false;
#else
	if (!EnsureProviderRuntime(frameBuffer))
	{
		return false;
	}

	if (frameBuffer.GetNativeD3D12Device() == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "native-device-unavailable", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	if (desc.outputWidth == 0u || desc.outputHeight == 0u || desc.renderWidth == 0u || desc.renderHeight == 0u ||
		desc.outputWidth > 0x7fffffffu || desc.outputHeight > 0x7fffffffu ||
		desc.renderWidth > 0x7fffffffu || desc.renderHeight > 0x7fffffffu)
	{
		std::strncpy(mProviderState.lastStatusReason, "invalid-dimensions", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	ID3D12Resource* nativeHudlessColor = GetNativeTexture(frameBuffer.mCore, desc.hudlessColor);
	if (nativeHudlessColor == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "hudless-color-unavailable", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	ID3D12Resource* nativePresentColor = GetNativeTexture(frameBuffer.mCore, frameBuffer.mCurrentPresentTarget);
	if (nativePresentColor == nullptr)
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "present-color-unavailable");
		return false;
	}

	const D3D12_RESOURCE_DESC presentDesc = nativePresentColor->GetDesc();
	const D3D12_RESOURCE_DESC hudlessDesc = nativeHudlessColor->GetDesc();
	const bool logicalExtentMismatch =
		frameBuffer.mCurrentPresentTarget->width != desc.outputWidth ||
		frameBuffer.mCurrentPresentTarget->height != desc.outputHeight ||
		desc.hudlessColor->width != desc.outputWidth ||
		desc.hudlessColor->height != desc.outputHeight;
	const bool nativeExtentMismatch =
		presentDesc.Width != desc.outputWidth ||
		presentDesc.Height != desc.outputHeight ||
		hudlessDesc.Width != desc.outputWidth ||
		hudlessDesc.Height != desc.outputHeight;
	if (logicalExtentMismatch || nativeExtentMismatch)
	{
		Printf("NRI framegen extent mismatch: output=%ux%u present_logical=%ux%u present_native=%llux%u hudless_logical=%ux%u hudless_native=%llux%u\n",
			desc.outputWidth,
			desc.outputHeight,
			frameBuffer.mCurrentPresentTarget->width,
			frameBuffer.mCurrentPresentTarget->height,
			(unsigned long long)presentDesc.Width,
			presentDesc.Height,
			desc.hudlessColor->width,
			desc.hudlessColor->height,
			(unsigned long long)hudlessDesc.Width,
			hudlessDesc.Height);
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "presentation-extent-mismatch");
		return false;
	}
	const uint32_t backBufferFormat = GetFfxSurfaceFormat(frameBuffer.mCurrentPresentTarget->format);
	const uint32_t hudlessFormat = GetFfxSurfaceFormat(desc.hudlessColor->format);
	if (backBufferFormat == NRI_FFX_API_SURFACE_FORMAT_UNKNOWN || hudlessFormat == NRI_FFX_API_SURFACE_FORMAT_UNKNOWN)
	{
		std::strncpy(mProviderState.lastStatusReason, "unsupported-backbuffer-format", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}
	const NRIFrameGenerationPresentContract& presentContract = mPresentContract;
	if (presentDesc.Format != static_cast<DXGI_FORMAT>(presentContract.resolvedDxgiFormat))
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "present-format-contract-mismatch");
		return false;
	}
	if (!IsNativeTextureFormatCompatible(presentDesc.Format, backBufferFormat) ||
		!IsNativeTextureFormatCompatible(hudlessDesc.Format, hudlessFormat))
	{
		Printf("NRI framegen format mismatch: present_native=%s present_logical=%s present_ffx=%s hudless_native=%s hudless_logical=%s hudless_ffx=%s\n",
			GetDxgiFormatName((uint32_t)presentDesc.Format),
			GetNriFormatName(frameBuffer.mCurrentPresentTarget->format),
			GetFfxSurfaceFormatName(backBufferFormat),
			GetDxgiFormatName((uint32_t)hudlessDesc.Format),
			GetNriFormatName(desc.hudlessColor->format),
			GetFfxSurfaceFormatName(hudlessFormat));
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "native-logical-format-mismatch");
		return false;
	}
	if (presentContract.hdrContext &&
		(backBufferFormat != NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT ||
		 hudlessFormat != NRI_FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT))
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "scrgb-input-format-mismatch");
		return false;
	}
	const uint32_t effectiveCreateFlags = GetFfxCreateFlags(mPolicy, desc, presentContract.hdrContext, !!nri_validation);

	const bool contextIdentityChanged =
		mProviderState.contextCreated &&
		(mProviderState.contextDisplayWidth != desc.outputWidth ||
		 mProviderState.contextDisplayHeight != desc.outputHeight ||
		 mProviderState.contextRenderWidth != desc.renderWidth ||
		 mProviderState.contextRenderHeight != desc.renderHeight ||
		 mProviderState.contextBackBufferFormat != backBufferFormat ||
		 mProviderState.contextHudlessFormat != hudlessFormat ||
		 mProviderState.contextCreateFlags != effectiveCreateFlags ||
		 mProviderState.contextTransferFunction != presentContract.transferFunction ||
		 mProviderState.contextHdr != presentContract.hdrContext ||
		 mProviderState.contextDisplayGeneration != frameBuffer.mCreatedSwapChainDisplayGeneration);
	if (contextIdentityChanged)
	{
		NoteReset("output-contract-change");
		const auto destroyContext = reinterpret_cast<PfnFfxDestroyContext>(mFfxDestroyContextFn);
		const auto allocationCallbacks = reinterpret_cast<ffxAllocationCallbacks*>(mFfxAllocCallbacks);
		if (destroyContext != nullptr && mFfxContext != nullptr)
		{
			ffxContext context = mFfxContext;
			mProviderState.lastDestroyResult = destroyContext(&context, allocationCallbacks);
			mFfxContext = nullptr;
		}
		mProviderState.contextCreated = false;
		mProviderState.contextDimensionsValid = false;
		mProviderState.debugConfigured = false;
		mProviderState.configuredThisFrame = false;
		mProviderState.prepareDispatchedThisFrame = false;
		mProviderState.prepareCameraInfoProvided = false;
		mProviderState.memoryUsageValid = false;
		mProviderState.totalUsageBytes = 0;
		mProviderState.aliasableUsageBytes = 0;
		std::strncpy(mProviderState.lastStatusReason, "context-recreate-pending", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
	}

	if (mProviderState.contextCreated)
	{
		return true;
	}

	ffxCreateBackendDX12Desc backendDesc = {};
	NriFfxInitHeader(backendDesc.header, NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12);
	backendDesc.device = frameBuffer.GetNativeD3D12Device();

	ffxCreateContextDescFrameGeneration createDesc = {};
	NriFfxInitHeader(createDesc.header, NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
	createDesc.flags = effectiveCreateFlags;
	createDesc.displaySize = { desc.outputWidth, desc.outputHeight };
	createDesc.maxRenderSize = { desc.renderWidth, desc.renderHeight };
	createDesc.backBufferFormat = backBufferFormat;

	ffxCreateContextDescFrameGenerationHudless hudlessFormatDesc = {};
	NriFfxInitHeader(hudlessFormatDesc.header, NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS);
	hudlessFormatDesc.hudlessBackBufferFormat = hudlessFormat;

	createDesc.header.pNext = &hudlessFormatDesc.header;
	hudlessFormatDesc.header.pNext = &backendDesc.header;

	const auto createContext = reinterpret_cast<PfnFfxCreateContext>(mFfxCreateContextFn);
	const auto configure = reinterpret_cast<PfnFfxConfigure>(mFfxConfigureFn);
	const auto query = reinterpret_cast<PfnFfxQuery>(mFfxQueryFn);
	const auto allocationCallbacks = reinterpret_cast<ffxAllocationCallbacks*>(mFfxAllocCallbacks);

	ffxContext context = nullptr;
	mProviderState.lastCreateResult = createContext(&context, &createDesc.header, allocationCallbacks);
	if (mProviderState.lastCreateResult != NRI_FFX_API_RETURN_OK)
	{
		std::strncpy(mProviderState.lastStatusReason, "context-create-failed", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return false;
	}

	mFfxContext = context;
	mProviderState.contextCreated = true;
	mProviderState.contextDimensionsValid = true;
	mProviderState.contextDisplayWidth = desc.outputWidth;
	mProviderState.contextDisplayHeight = desc.outputHeight;
	mProviderState.contextRenderWidth = desc.renderWidth;
	mProviderState.contextRenderHeight = desc.renderHeight;
	mProviderState.contextBackBufferFormat = backBufferFormat;
	mProviderState.contextHudlessFormat = hudlessFormat;
	mProviderState.contextCreateFlags = effectiveCreateFlags;
	mProviderState.contextTransferFunction = presentContract.transferFunction;
	mProviderState.contextHdr = presentContract.hdrContext;
	mProviderState.contextDisplayGeneration = frameBuffer.mCreatedSwapChainDisplayGeneration;
	++mProviderState.contextGeneration;

	if (nri_validation)
	{
		ffxConfigureDescGlobalDebug1 debugDesc = {};
		NriFfxInitHeader(debugDesc.header, NRI_FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1);
		debugDesc.fpMessage = &NriFfxMessageCallback;
		debugDesc.debugLevel = NRI_FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
		mProviderState.debugConfigured = configure(reinterpret_cast<ffxContext*>(&mFfxContext), &debugDesc.header) == NRI_FFX_API_RETURN_OK;
	}
	else
	{
		mProviderState.debugConfigured = false;
	}

	ffxQueryGetProviderVersion versionQuery = {};
	NriFfxInitHeader(versionQuery.header, NRI_FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION);
	mProviderState.lastQueryResult = query(reinterpret_cast<ffxContext*>(&mFfxContext), &versionQuery.header);
	if (mProviderState.lastQueryResult == NRI_FFX_API_RETURN_OK && versionQuery.versionName != nullptr)
	{
		CopyString(mProviderState.providerVersion, std::size(mProviderState.providerVersion), versionQuery.versionName);
	}

	FfxApiEffectMemoryUsage memoryUsage = {};
	ffxQueryDescFrameGenerationGetGPUMemoryUsage memoryQuery = {};
	NriFfxInitHeader(memoryQuery.header, NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE);
	memoryQuery.gpuMemoryUsageFrameGeneration = &memoryUsage;
	mProviderState.lastQueryResult = query(reinterpret_cast<ffxContext*>(&mFfxContext), &memoryQuery.header);
	if (mProviderState.lastQueryResult == NRI_FFX_API_RETURN_OK)
	{
		mProviderState.memoryUsageValid = true;
		mProviderState.totalUsageBytes = memoryUsage.totalUsageInBytes;
		mProviderState.aliasableUsageBytes = memoryUsage.aliasableUsageInBytes;
	}

	std::strncpy(mProviderState.lastStatusReason, "context-created", std::size(mProviderState.lastStatusReason) - 1u);
	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
	return true;
#endif
}

void NRIFrameGenerationContext::ConfigureAndPrepareProvider(const NRIRenderDevice& frameBuffer, const NRIFrameGenerationFrameDesc& desc)
{
#ifndef _WIN32
	(void)frameBuffer;
	(void)desc;
	return;
#else
	mProviderState.frameGenerationDispatchedThisFrame = false;
	mProviderState.uiResourceRegisteredThisFrame = false;
	const auto failToNative = [&](const char* reason)
	{
		char stableReason[96] = {};
		CopyString(stableReason, std::size(stableReason), reason);
		RequestNativeFallback(stableReason);
		const_cast<NRIRenderDevice&>(frameBuffer).RequestSwapChainRefresh(stableReason, true);
	};

	if (!mInitialized || !mPolicy.operational)
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), mPolicy.resolvedReason);
		return;
	}

	if (!mPresentContract.initialized || !mPresentContract.proxyAllowed)
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason),
			mPresentContract.initialized ? mPresentContract.resolvedReason : "present-contract-uninitialized");
		if (IsPresentBridgeActive())
			failToNative(mProviderState.lastStatusReason);
		return;
	}

	if (!mLastInputAudit.complete || mLastInputAudit.adapterRequirement != NRIFrameGenerationAdapterRequirement::None)
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), mLastInputAudit.statusReason);
		if (IsPresentBridgeActive())
			failToNative(mProviderState.lastStatusReason);
		return;
	}

	if (!IsPresentBridgeActive())
	{
		CopyString(mProviderState.lastStatusReason, std::size(mProviderState.lastStatusReason), "present-bridge-not-selected");
		return;
	}

	if (!EnsureProviderContext(frameBuffer, desc))
	{
		failToNative(mProviderState.lastStatusReason);
		return;
	}

	ID3D12GraphicsCommandList* nativeCommandList = reinterpret_cast<ID3D12GraphicsCommandList*>(GetNativeCommandList(frameBuffer.mCore, frameBuffer.mCommandBuffer));
	if (nativeCommandList == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "native-commandlist-unavailable", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		failToNative("native-commandlist-unavailable");
		return;
	}

	auto& mutableFrameBuffer = const_cast<NRIRenderDevice&>(frameBuffer);
	if (desc.hudlessColor != nullptr)
	{
		mutableFrameBuffer.TransitionTexture(*const_cast<NRITextureResource*>(desc.hudlessColor), NRIComputeShaderResourceState());
	}
	if (desc.motionVectors != nullptr)
	{
		mutableFrameBuffer.TransitionTexture(*const_cast<NRITextureResource*>(desc.motionVectors), NRIComputeShaderResourceState());
	}
	if (desc.depth != nullptr)
	{
		mutableFrameBuffer.TransitionTexture(*const_cast<NRITextureResource*>(desc.depth), NRIComputeShaderResourceState());
	}
	if (frameBuffer.mCurrentPresentTarget != nullptr)
	{
		mutableFrameBuffer.TransitionTexture(*frameBuffer.mCurrentPresentTarget, NRIComputeShaderResourceState());
	}

	const auto configure = reinterpret_cast<PfnFfxConfigure>(mFfxConfigureFn);
	const auto query = reinterpret_cast<PfnFfxQuery>(mFfxQueryFn);
	const auto dispatch = reinterpret_cast<PfnFfxDispatch>(mFfxDispatchFn);
	const bool usePresentBridge = ShouldUsePresentBridge();
	const NRIFrameGenerationPresentContract& presentContract = mPresentContract;
	const uint32_t frameGenFlags = usePresentBridge ? 0u : NRI_FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;

	ffxConfigureDescFrameGeneration configureDesc = {};
	NriFfxInitHeader(configureDesc.header, NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
	configureDesc.swapChain = usePresentBridge ? mPresentBridge.GetSwapChain() : nullptr;
	configureDesc.presentCallback = nullptr;
	configureDesc.presentCallbackUserContext = nullptr;
	configureDesc.frameGenerationCallback = nullptr;
	configureDesc.frameGenerationCallbackUserContext = nullptr;
	configureDesc.frameGenerationEnabled = mPolicy.operational && usePresentBridge;
	configureDesc.allowAsyncWorkloads = false;
	configureDesc.HUDLessColor = GetFfxTextureResource(frameBuffer.mCore, desc.hudlessColor);
	configureDesc.flags = frameGenFlags;
	configureDesc.onlyPresentGenerated = false;
	configureDesc.generationRect = {
		(int32_t)desc.outputRect.left,
		(int32_t)desc.outputRect.top,
		(int32_t)desc.outputRect.width,
		(int32_t)desc.outputRect.height
	};
	configureDesc.frameID = desc.frameId;

	mProviderState.lastConfigureResult = configure(reinterpret_cast<ffxContext*>(&mFfxContext), &configureDesc.header);
	if (mProviderState.lastConfigureResult != NRI_FFX_API_RETURN_OK)
	{
		std::strncpy(mProviderState.lastStatusReason, "configure-failed", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		failToNative("configure-failed");
		return;
	}

	mProviderState.configuredThisFrame = true;
	mProviderState.lastConfiguredFrameId = desc.frameId;
	++mProviderState.configureCount;
	mProviderState.noSwapChainNotify = !usePresentBridge;

	if (usePresentBridge && mPresentBridge.GetContext() != nullptr)
	{
		assert(desc.uiTexture != nullptr || mPolicy.resolvedUiMode != NRIFrameGenerationUiMode::UiTexture);
		if (desc.uiTexture == nullptr && mPolicy.resolvedUiMode == NRIFrameGenerationUiMode::UiTexture)
		{
			std::strncpy(mProviderState.lastStatusReason, "ui-texture-missing", std::size(mProviderState.lastStatusReason) - 1u);
			mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
			failToNative("ui-texture-missing");
			return;
		}
		if (desc.uiTexture != nullptr && frameBuffer.mCurrentPresentTarget != nullptr &&
			desc.uiTexture->format != frameBuffer.mCurrentPresentTarget->format)
		{
			failToNative("ui-format-contract-mismatch");
			return;
		}
		ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig = {};
		NriFfxInitHeader(uiConfig.header, NRI_FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12);
		uiConfig.uiResource = GetFfxTextureResource(frameBuffer.mCore, desc.uiTexture);
		uiConfig.flags = desc.uiTexture != nullptr ? NRI_FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING : 0u;
		mProviderState.lastSwapChainConfigureResult = configure(reinterpret_cast<ffxContext*>(mPresentBridge.GetContextAddress()), &uiConfig.header);
		mProviderState.uiResourceRegisteredThisFrame = mProviderState.lastSwapChainConfigureResult == NRI_FFX_API_RETURN_OK && desc.uiTexture != nullptr;
		if (mProviderState.lastSwapChainConfigureResult != NRI_FFX_API_RETURN_OK)
		{
			std::strncpy(mProviderState.lastStatusReason, "ui-register-failed", std::size(mProviderState.lastStatusReason) - 1u);
			mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
			failToNative("ui-register-failed");
			return;
		}
	}

	ffxDispatchDescFrameGenerationPrepareCameraInfo cameraInfoDesc = {};
	NriFfxInitHeader(cameraInfoDesc.header, NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO);
	std::memcpy(cameraInfoDesc.cameraPosition, desc.cameraPosition, sizeof(cameraInfoDesc.cameraPosition));
	std::memcpy(cameraInfoDesc.cameraUp, desc.cameraUp, sizeof(cameraInfoDesc.cameraUp));
	std::memcpy(cameraInfoDesc.cameraRight, desc.cameraRight, sizeof(cameraInfoDesc.cameraRight));
	std::memcpy(cameraInfoDesc.cameraForward, desc.cameraForward, sizeof(cameraInfoDesc.cameraForward));

	ffxDispatchDescFrameGenerationPrepare prepareDesc = {};
	NriFfxInitHeader(prepareDesc.header, NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE);
	prepareDesc.header.pNext = reinterpret_cast<ffxApiHeader*>(&cameraInfoDesc.header);
	prepareDesc.frameID = desc.frameId;
	prepareDesc.flags = frameGenFlags;
	prepareDesc.commandList = nativeCommandList;
	prepareDesc.renderSize = { desc.renderWidth, desc.renderHeight };
	prepareDesc.jitterOffset = { desc.cameraJitter[0], desc.cameraJitter[1] };
	prepareDesc.motionVectorScale = { desc.motionVectorScale[0], desc.motionVectorScale[1] };
	prepareDesc.frameTimeDelta = desc.hasRealFrameTimeMs && desc.realFrameTimeMs > 0.0f ? desc.realFrameTimeMs : (1000.0f / 60.0f);
	prepareDesc.unused_reset = desc.resetHistory;
	prepareDesc.cameraNear = desc.cameraNear;
	prepareDesc.cameraFar = desc.cameraFar;
	prepareDesc.cameraFovAngleVertical = desc.cameraFovVerticalRadians;
	prepareDesc.viewSpaceToMetersFactor = desc.viewSpaceToMetersFactor > 0.0f ? desc.viewSpaceToMetersFactor : 1.0f;
	prepareDesc.depth = GetFfxTextureResource(frameBuffer.mCore, desc.depth);
	prepareDesc.motionVectors = GetFfxTextureResource(frameBuffer.mCore, desc.motionVectors);

	mProviderState.lastPrepareResult = dispatch(reinterpret_cast<ffxContext*>(&mFfxContext), &prepareDesc.header);
	if (mProviderState.lastPrepareResult != NRI_FFX_API_RETURN_OK)
	{
		std::strncpy(mProviderState.lastStatusReason, "prepare-dispatch-failed", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		failToNative("prepare-dispatch-failed");
		return;
	}

	mProviderState.prepareDispatchedThisFrame = true;
	mProviderState.prepareCameraInfoProvided = true;
	mProviderState.lastPreparedFrameId = desc.frameId;
	++mProviderState.prepareCount;

	if (!usePresentBridge || !configureDesc.frameGenerationEnabled || frameBuffer.mCurrentPresentTarget == nullptr || mPresentBridge.GetContext() == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "prepare-dispatched", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		return;
	}

	void* interpolationCommandList = nullptr;
	ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12 commandListQuery = {};
	NriFfxInitHeader(commandListQuery.header, NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12);
	commandListQuery.pOutCommandList = &interpolationCommandList;
	mProviderState.lastSwapChainQueryResult = query(reinterpret_cast<ffxContext*>(mPresentBridge.GetContextAddress()), &commandListQuery.header);
	if (mProviderState.lastSwapChainQueryResult != NRI_FFX_API_RETURN_OK || interpolationCommandList == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "interpolation-commandlist-unavailable", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		failToNative("interpolation-commandlist-unavailable");
		return;
	}

	FfxApiResource interpolationOutput = {};
	ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12 textureQuery = {};
	NriFfxInitHeader(textureQuery.header, NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12);
	textureQuery.pOutTexture = &interpolationOutput;
	mProviderState.lastSwapChainQueryResult = query(reinterpret_cast<ffxContext*>(mPresentBridge.GetContextAddress()), &textureQuery.header);
	if (mProviderState.lastSwapChainQueryResult != NRI_FFX_API_RETURN_OK || interpolationOutput.resource == nullptr)
	{
		std::strncpy(mProviderState.lastStatusReason, "interpolation-output-unavailable", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		failToNative("interpolation-output-unavailable");
		return;
	}

	ffxDispatchDescFrameGeneration dispatchDesc = {};
	NriFfxInitHeader(dispatchDesc.header, NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION);
	dispatchDesc.commandList = interpolationCommandList;
	dispatchDesc.presentColor = GetFfxTextureResource(frameBuffer.mCore, frameBuffer.mCurrentPresentTarget);
	dispatchDesc.outputs[0] = interpolationOutput;
	dispatchDesc.numGeneratedFrames = 1u;
	dispatchDesc.reset = desc.resetHistory;
	switch (presentContract.transferFunction)
	{
	default:
	case NRIFrameGenerationPresentTransferFunction::Unknown:
	case NRIFrameGenerationPresentTransferFunction::SRGB:
		dispatchDesc.backbufferTransferFunction = NRI_FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		break;
	case NRIFrameGenerationPresentTransferFunction::PQ:
		dispatchDesc.backbufferTransferFunction = NRI_FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
		break;
	case NRIFrameGenerationPresentTransferFunction::ScRGB:
		dispatchDesc.backbufferTransferFunction = NRI_FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
		break;
	}
	dispatchDesc.minMaxLuminance[0] = presentContract.minLuminanceNits;
	dispatchDesc.minMaxLuminance[1] = presentContract.maxLuminanceNits;
	dispatchDesc.generationRect = configureDesc.generationRect;
	dispatchDesc.frameID = desc.frameId;

	mProviderState.lastDispatchResult = dispatch(reinterpret_cast<ffxContext*>(&mFfxContext), &dispatchDesc.header);
	if (mProviderState.lastDispatchResult != NRI_FFX_API_RETURN_OK)
	{
		std::strncpy(mProviderState.lastStatusReason, "framegen-dispatch-failed", std::size(mProviderState.lastStatusReason) - 1u);
		mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
		failToNative("framegen-dispatch-failed");
		return;
	}

	mProviderState.frameGenerationDispatchedThisFrame = true;
	++mProviderState.dispatchCount;
	std::strncpy(mProviderState.lastStatusReason, "framegen-dispatched", std::size(mProviderState.lastStatusReason) - 1u);
	mProviderState.lastStatusReason[std::size(mProviderState.lastStatusReason) - 1u] = '\0';
#endif
}
