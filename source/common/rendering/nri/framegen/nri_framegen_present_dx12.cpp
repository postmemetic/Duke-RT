#include "nri_framegen_present_dx12.h"
#include "nri_ffx_api.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#include <vector>


namespace
{
	bool IsDxgiTearingSupported(IDXGIFactory* factory)
	{
		if (factory == nullptr)
			return false;

		IDXGIFactory5* factory5 = nullptr;
		if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))) || factory5 == nullptr)
			return false;

		BOOL allowTearing = FALSE;
		const HRESULT result = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
		factory5->Release();
		return SUCCEEDED(result) && allowTearing == TRUE;
	}

	HRESULT CreateDxgiFactoryForFrameGeneration(IDXGIFactory7** factory)
	{
		if (factory == nullptr)
			return E_POINTER;

		*factory = nullptr;
		HMODULE dxgiModule = GetModuleHandleA("dxgi.dll");
		if (dxgiModule == nullptr)
		{
			dxgiModule = LoadLibraryA("dxgi.dll");
			if (dxgiModule == nullptr)
				return HRESULT_FROM_WIN32(GetLastError());
		}

		using PfnCreateDXGIFactory2 = HRESULT (WINAPI*)(UINT flags, REFIID riid, void** factory);
		auto createFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(GetProcAddress(dxgiModule, "CreateDXGIFactory2"));
		return createFactory2 != nullptr ? createFactory2(0u, IID_PPV_ARGS(factory)) : E_NOINTERFACE;
	}

	struct NriDisplayConfigPathSourceInfo
	{
		LUID adapterId;
		UINT32 id;
		UINT32 modeInfoIdx;
		UINT32 statusFlags;
	};

	struct NriDisplayConfigRational
	{
		UINT32 numerator;
		UINT32 denominator;
	};

	struct NriDisplayConfigPathTargetInfo
	{
		LUID adapterId;
		UINT32 id;
		UINT32 modeInfoIdx;
		UINT32 outputTechnology;
		UINT32 rotation;
		UINT32 scaling;
		NriDisplayConfigRational refreshRate;
		UINT32 scanLineOrdering;
		BOOL targetAvailable;
		UINT32 statusFlags;
	};

	struct NriDisplayConfigPathInfo
	{
		NriDisplayConfigPathSourceInfo sourceInfo;
		NriDisplayConfigPathTargetInfo targetInfo;
		UINT32 flags;
	};

	struct alignas(8) NriDisplayConfigModeInfo
	{
		uint8_t bytes[64];
	};

	struct NriDisplayConfigDeviceInfoHeader
	{
		UINT32 type;
		UINT32 size;
		LUID adapterId;
		UINT32 id;
	};

	struct NriDisplayConfigSourceDeviceName
	{
		NriDisplayConfigDeviceInfoHeader header;
		WCHAR viewGdiDeviceName[CCHDEVICENAME];
	};

	struct NriDisplayConfigSdrWhiteLevel
	{
		NriDisplayConfigDeviceInfoHeader header;
		ULONG sdrWhiteLevel;
	};

	static_assert(sizeof(NriDisplayConfigPathSourceInfo) == 20);
	static_assert(sizeof(NriDisplayConfigPathTargetInfo) == 48);
	static_assert(sizeof(NriDisplayConfigPathInfo) == 72);
	static_assert(sizeof(NriDisplayConfigModeInfo) == 64);
	static_assert(sizeof(NriDisplayConfigSourceDeviceName) == 84);
	static_assert(sizeof(NriDisplayConfigSdrWhiteLevel) == 24);

	bool IsInternalOutputTechnology(UINT32 outputTechnology)
	{
		return outputTechnology == 0x80000000u || outputTechnology == 11u || outputTechnology == 13u;
	}

	float GetSdrLuminanceForMonitor(HMONITOR monitor)
	{
		constexpr UINT32 QueryOnlyActivePaths = 0x00000002u;
		constexpr UINT32 GetSourceName = 1u;
		constexpr UINT32 GetSdrWhiteLevel = 11u;
		float nits = 80.0f;

		MONITORINFOEXW monitorInfo = {};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
			return nits;

		HMODULE user32Module = GetModuleHandleW(L"user32.dll");
		if (user32Module == nullptr)
			return nits;

		using PfnGetDisplayConfigBufferSizes = LONG (WINAPI*)(UINT32, UINT32*, UINT32*);
		using PfnQueryDisplayConfig = LONG (WINAPI*)(UINT32, UINT32*, void*, UINT32*, void*, void*);
		using PfnDisplayConfigGetDeviceInfo = LONG (WINAPI*)(void*);
		const auto getBufferSizes = reinterpret_cast<PfnGetDisplayConfigBufferSizes>(GetProcAddress(user32Module, "GetDisplayConfigBufferSizes"));
		const auto queryDisplayConfig = reinterpret_cast<PfnQueryDisplayConfig>(GetProcAddress(user32Module, "QueryDisplayConfig"));
		const auto getDeviceInfo = reinterpret_cast<PfnDisplayConfigGetDeviceInfo>(GetProcAddress(user32Module, "DisplayConfigGetDeviceInfo"));
		if (getBufferSizes == nullptr || queryDisplayConfig == nullptr || getDeviceInfo == nullptr)
			return nits;

		std::vector<NriDisplayConfigPathInfo> paths;
		std::vector<NriDisplayConfigModeInfo> modes;
		LONG queryResult = ERROR_INSUFFICIENT_BUFFER;
		UINT32 pathCount = 0;
		UINT32 modeCount = 0;
		for (uint32_t attempt = 0; attempt < 3u && queryResult == ERROR_INSUFFICIENT_BUFFER; ++attempt)
		{
			if (getBufferSizes(QueryOnlyActivePaths, &pathCount, &modeCount) != ERROR_SUCCESS)
				return nits;

			paths.resize(pathCount);
			modes.resize(modeCount);
			queryResult = queryDisplayConfig(QueryOnlyActivePaths, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
		}
		if (queryResult != ERROR_SUCCESS)
			return nits;

		const NriDisplayConfigPathTargetInfo* selectedTarget = nullptr;
		for (UINT32 pathIndex = 0; pathIndex < pathCount; ++pathIndex)
		{
			NriDisplayConfigSourceDeviceName sourceName = {};
			sourceName.header.type = GetSourceName;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = paths[pathIndex].sourceInfo.adapterId;
			sourceName.header.id = paths[pathIndex].sourceInfo.id;
			if (getDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
				wcscmp(monitorInfo.szDevice, sourceName.viewGdiDeviceName) != 0)
			{
				continue;
			}

			if (selectedTarget == nullptr || IsInternalOutputTechnology(paths[pathIndex].targetInfo.outputTechnology))
				selectedTarget = &paths[pathIndex].targetInfo;
		}
		if (selectedTarget == nullptr)
			return nits;

		NriDisplayConfigSdrWhiteLevel whiteLevel = {};
		whiteLevel.header.type = GetSdrWhiteLevel;
		whiteLevel.header.size = sizeof(whiteLevel);
		whiteLevel.header.adapterId = selectedTarget->adapterId;
		whiteLevel.header.id = selectedTarget->id;
		if (getDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS && whiteLevel.sdrWhiteLevel != 0u)
			nits = (static_cast<float>(whiteLevel.sdrWhiteLevel) * 80.0f) / 1000.0f;

		return nits;
	}
}
#endif

NRIFsr3Dx12PresentBridge::~NRIFsr3Dx12PresentBridge()
{
#ifdef _WIN32
	ReleaseDisplayQueryCache();
#endif
}

bool NRIFsr3Dx12PresentBridge::Create(const NRIFsr3Dx12PresentBridgeCreateDesc& desc)
{
#ifndef _WIN32
	(void)desc;
	return false;
#else
	if (IsActive() || desc.windowHandle == nullptr || desc.gameQueue == nullptr || desc.createContextFn == nullptr)
		return false;

	++mSnapshot.createAttemptCount;
	mSnapshot.contextCreated = false;
	mSnapshot.swapChainCreated = false;
	mSnapshot.tearingSupported = false;
	mSnapshot.windowAssociationKnown = false;
	mSnapshot.windowAssociationSucceeded = false;
	mSnapshot.dxgiFullscreenKnown = false;
	mSnapshot.dxgiFullscreen = false;
	mSnapshot.memoryUsageValid = false;
	mSnapshot.waitableObjectAvailable = false;
	mSnapshot.colorSpaceSupportValid = false;
	mSnapshot.colorSpaceSet = false;
	mSnapshot.observedColorSpaceValid = false;
	mSnapshot.lastCreateResult = 0;
	mSnapshot.lastQueryResult = 0;
	mSnapshot.totalUsageBytes = 0;
	mSnapshot.aliasableUsageBytes = 0;
	mSnapshot.requestedColorSpace = desc.colorSpace;
	mSnapshot.observedColorSpace = 0;
	mSnapshot.colorSpaceSupport = 0;
	mSnapshot.colorSpaceCheckHresult = 0;
	mSnapshot.colorSpaceSetHresult = 0;

	IDXGIFactory7* factory = nullptr;
	if (FAILED(CreateDxgiFactoryForFrameGeneration(&factory)) || factory == nullptr)
		return false;

	mSnapshot.tearingSupported = IsDxgiTearingSupported(factory);
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = desc.width;
	swapChainDesc.Height = desc.height;
	swapChainDesc.Format = static_cast<DXGI_FORMAT>(desc.format);
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
	swapChainDesc.BufferCount = desc.bufferCount;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	swapChainDesc.Flags = mSnapshot.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
	fullscreenDesc.Windowed = TRUE;

	ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 createDesc = {};
	NriFfxInitHeader(createDesc.header, NRI_FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12);
	createDesc.swapchain = &mSwapChain;
	createDesc.hwnd = static_cast<HWND>(desc.windowHandle);
	createDesc.desc = &swapChainDesc;
	createDesc.fullscreenDesc = &fullscreenDesc;
	createDesc.dxgiFactory = factory;
	createDesc.gameQueue = static_cast<ID3D12CommandQueue*>(desc.gameQueue);

	const auto createContext = reinterpret_cast<PfnFfxCreateContext>(desc.createContextFn);
	ffxContext context = nullptr;
	mSnapshot.lastCreateResult = createContext(&context, &createDesc.header, reinterpret_cast<ffxAllocationCallbacks*>(desc.allocationCallbacks));
	mContext = context;
	mSnapshot.contextCreated = mContext != nullptr;
	mSnapshot.swapChainCreated = mSwapChain != nullptr;
	if (mSnapshot.lastCreateResult == NRI_FFX_API_RETURN_OK)
	{
		mDrainRequired = IsActive();
		if (mSwapChain != nullptr)
		{
			UINT colorSpaceSupport = 0;
			const DXGI_COLOR_SPACE_TYPE requestedColorSpace = static_cast<DXGI_COLOR_SPACE_TYPE>(desc.colorSpace);
			const HRESULT colorSpaceCheckResult = mSwapChain->CheckColorSpaceSupport(requestedColorSpace, &colorSpaceSupport);
			mSnapshot.colorSpaceCheckHresult = static_cast<int64_t>(colorSpaceCheckResult);
			mSnapshot.colorSpaceSupport = colorSpaceSupport;
			mSnapshot.colorSpaceSupportValid = SUCCEEDED(colorSpaceCheckResult);
			if (SUCCEEDED(colorSpaceCheckResult) && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0u)
			{
				const HRESULT colorSpaceSetResult = mSwapChain->SetColorSpace1(requestedColorSpace);
				mSnapshot.colorSpaceSetHresult = static_cast<int64_t>(colorSpaceSetResult);
				mSnapshot.colorSpaceSet = SUCCEEDED(colorSpaceSetResult);
			}
			else
			{
				mSnapshot.colorSpaceSetHresult = static_cast<int64_t>(E_FAIL);
			}
			mSnapshot.observedColorSpace = desc.colorSpace;
			mSnapshot.observedColorSpaceValid = mSnapshot.colorSpaceSet;

			const HRESULT associationResult = factory->MakeWindowAssociation(static_cast<HWND>(desc.windowHandle), DXGI_MWA_NO_WINDOW_CHANGES);
			mSnapshot.windowAssociationKnown = true;
			mSnapshot.windowAssociationSucceeded = SUCCEEDED(associationResult);
			RefreshFullscreenState();
			mFrameLatencyWaitableObject = mSwapChain->GetFrameLatencyWaitableObject();
			mSnapshot.waitableObjectAvailable = mFrameLatencyWaitableObject != nullptr;
		}

		const auto query = reinterpret_cast<PfnFfxQuery>(desc.queryFn);
		if (query != nullptr && mContext != nullptr)
		{
			FfxApiEffectMemoryUsage memoryUsage = {};
			ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 memoryQuery = {};
			NriFfxInitHeader(memoryQuery.header, NRI_FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12);
			memoryQuery.gpuMemoryUsageFrameGenerationSwapchain = &memoryUsage;
			mSnapshot.lastQueryResult = query(reinterpret_cast<ffxContext*>(&mContext), &memoryQuery.header);
			if (mSnapshot.lastQueryResult == NRI_FFX_API_RETURN_OK)
			{
				mSnapshot.memoryUsageValid = true;
				mSnapshot.totalUsageBytes = memoryUsage.totalUsageInBytes;
				mSnapshot.aliasableUsageBytes = memoryUsage.aliasableUsageInBytes;
			}
		}
	}

	const bool success = IsActive() && mSnapshot.colorSpaceSet &&
		mSnapshot.observedColorSpaceValid && mSnapshot.observedColorSpace == mSnapshot.requestedColorSpace;
	if (success)
		++mSnapshot.createGeneration;
	factory->Release();
	return success;
#endif
}

bool NRIFsr3Dx12PresentBridge::QueryDisplayDesc(void* windowHandle, NRIFsr3Dx12DisplayDesc& outDesc)
{
	outDesc = {};
#ifndef _WIN32
	(void)windowHandle;
	return false;
#else
	if (windowHandle == nullptr)
		return false;

	const HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(windowHandle), MONITOR_DEFAULTTONEAREST);
	if (monitor == nullptr || !EnsureDisplayOutput(monitor))
		return false;

	bool success = false;
	if (mDisplayOutput != nullptr)
	{
		DXGI_OUTPUT_DESC1 desc = {};
		if (SUCCEEDED(mDisplayOutput->GetDesc1(&desc)))
		{
			outDesc.redPrimary[0] = desc.RedPrimary[0];
			outDesc.redPrimary[1] = desc.RedPrimary[1];
			outDesc.greenPrimary[0] = desc.GreenPrimary[0];
			outDesc.greenPrimary[1] = desc.GreenPrimary[1];
			outDesc.bluePrimary[0] = desc.BluePrimary[0];
			outDesc.bluePrimary[1] = desc.BluePrimary[1];
			outDesc.whitePoint[0] = desc.WhitePoint[0];
			outDesc.whitePoint[1] = desc.WhitePoint[1];
			outDesc.minLuminance = desc.MinLuminance;
			outDesc.maxLuminance = desc.MaxLuminance;
			outDesc.maxFullFrameLuminance = desc.MaxFullFrameLuminance;
			const uint64_t queryTick = GetTickCount64();
			if (!mCachedSdrLuminanceValid || queryTick - mSdrLuminanceQueryTick >= 1000u)
			{
				mCachedSdrLuminance = GetSdrLuminanceForMonitor(desc.Monitor);
				mCachedSdrLuminanceValid = true;
				mSdrLuminanceQueryTick = queryTick;
			}
			outDesc.sdrLuminance = mCachedSdrLuminance;
			outDesc.isHdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
			success = true;
		}
	}
	return success;
#endif
}

#ifdef _WIN32
void NRIFsr3Dx12PresentBridge::ReleaseDisplayQueryCache()
{
	if (mDisplayOutput != nullptr)
	{
		mDisplayOutput->Release();
		mDisplayOutput = nullptr;
	}
	if (mDisplayFactory != nullptr)
	{
		mDisplayFactory->Release();
		mDisplayFactory = nullptr;
	}
	mDisplayMonitor = nullptr;
	mCachedSdrLuminanceValid = false;
	mSdrLuminanceQueryTick = 0;
}

bool NRIFsr3Dx12PresentBridge::EnsureDisplayOutput(void* monitorHandle)
{
	const HMONITOR monitor = static_cast<HMONITOR>(monitorHandle);
	if (mDisplayFactory != nullptr && (!mDisplayFactory->IsCurrent() || mDisplayMonitor != monitor))
		ReleaseDisplayQueryCache();
	if (mDisplayOutput != nullptr)
		return true;

	if (mDisplayFactory == nullptr &&
		(FAILED(CreateDxgiFactoryForFrameGeneration(&mDisplayFactory)) || mDisplayFactory == nullptr))
	{
		return false;
	}

	for (UINT adapterIndex = 0; mDisplayOutput == nullptr; ++adapterIndex)
	{
		IDXGIAdapter1* adapter = nullptr;
		const HRESULT adapterResult = mDisplayFactory->EnumAdapters1(adapterIndex, &adapter);
		if (adapterResult == DXGI_ERROR_NOT_FOUND)
			break;
		if (FAILED(adapterResult) || adapter == nullptr)
			continue;

		for (UINT outputIndex = 0; mDisplayOutput == nullptr; ++outputIndex)
		{
			IDXGIOutput* output = nullptr;
			const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
			if (outputResult == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(outputResult) || output == nullptr)
				continue;

			DXGI_OUTPUT_DESC outputDesc = {};
			if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.Monitor == monitor)
				output->QueryInterface(IID_PPV_ARGS(&mDisplayOutput));
			output->Release();
		}
		adapter->Release();
	}

	if (mDisplayOutput == nullptr)
	{
		ReleaseDisplayQueryCache();
		return false;
	}

	mDisplayMonitor = monitor;
	return true;
}
#endif

uint32_t NRIFsr3Dx12PresentBridge::Drain(void* dispatchFn)
{
#ifndef _WIN32
	(void)dispatchFn;
	return 0;
#else
	if (!mDrainRequired || mContext == nullptr || dispatchFn == nullptr)
		return mSnapshot.lastDrainResult;

	ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 waitDesc = {};
	NriFfxInitHeader(waitDesc.header, NRI_FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12);
	mSnapshot.lastDrainResult = reinterpret_cast<PfnFfxDispatch>(dispatchFn)(reinterpret_cast<ffxContext*>(&mContext), &waitDesc.header);
	mDrainRequired = mSnapshot.lastDrainResult != NRI_FFX_API_RETURN_OK;
	++mSnapshot.drainCount;
	return mSnapshot.lastDrainResult;
#endif
}

uint32_t NRIFsr3Dx12PresentBridge::Destroy(void* dispatchFn, void* destroyContextFn, void* allocationCallbacks)
{
#ifndef _WIN32
	(void)dispatchFn;
	(void)destroyContextFn;
	(void)allocationCallbacks;
	return 0;
#else
	Drain(dispatchFn);
	RefreshFullscreenState();
	if (mSwapChain != nullptr && mSnapshot.dxgiFullscreenKnown && mSnapshot.dxgiFullscreen)
	{
		mSnapshot.lastExitFullscreenHresult = static_cast<int64_t>(mSwapChain->SetFullscreenState(FALSE, nullptr));
	}
	if (mSwapChain != nullptr)
	{
		mSwapChain->Release();
		mSwapChain = nullptr;
	}

	uint32_t result = NRI_FFX_API_RETURN_OK;
	if (mContext != nullptr && destroyContextFn != nullptr)
	{
		ffxContext context = mContext;
		result = reinterpret_cast<PfnFfxDestroyContext>(destroyContextFn)(&context, reinterpret_cast<ffxAllocationCallbacks*>(allocationCallbacks));
		mContext = nullptr;
	}
	if (mFrameLatencyWaitableObject != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(mFrameLatencyWaitableObject));
		mFrameLatencyWaitableObject = nullptr;
	}
	mDrainRequired = false;
	mSnapshot.contextCreated = false;
	mSnapshot.swapChainCreated = false;
	mSnapshot.dxgiFullscreenKnown = false;
	mSnapshot.dxgiFullscreen = false;
	mSnapshot.windowAssociationKnown = false;
	mSnapshot.windowAssociationSucceeded = false;
	mSnapshot.waitableObjectAvailable = false;
	return result;
#endif
}

bool NRIFsr3Dx12PresentBridge::WaitForPacing(uint32_t timeoutMs)
{
#ifndef _WIN32
	(void)timeoutMs;
	return false;
#else
	if (!IsActive() || mFrameLatencyWaitableObject == nullptr)
		return false;

	mSnapshot.lastWaitResult = WaitForSingleObject(static_cast<HANDLE>(mFrameLatencyWaitableObject), timeoutMs);
	++mSnapshot.pacingWaitCount;
	if (mSnapshot.lastWaitResult == WAIT_TIMEOUT)
		++mSnapshot.pacingWaitTimeoutCount;
	return mSnapshot.lastWaitResult == WAIT_OBJECT_0;
#endif
}

int64_t NRIFsr3Dx12PresentBridge::Present(bool vsync, bool allowTearing)
{
#ifndef _WIN32
	(void)vsync;
	(void)allowTearing;
	return -1;
#else
	if (mSwapChain == nullptr)
		return static_cast<int64_t>(E_POINTER);

	RefreshFullscreenState();
	const UINT syncInterval = vsync ? 1u : 0u;
	const UINT flags = !vsync && allowTearing && mSnapshot.tearingSupported && mSnapshot.dxgiFullscreenKnown && !mSnapshot.dxgiFullscreen ? DXGI_PRESENT_ALLOW_TEARING : 0u;
	const HRESULT result = mSwapChain->Present(syncInterval, flags);
	mSnapshot.lastPresentHresult = static_cast<int64_t>(result);
	mSnapshot.lastPresentSyncInterval = syncInterval;
	mSnapshot.lastPresentFlags = flags;
	mDrainRequired = true;
	return static_cast<int64_t>(result);
#endif
}

bool NRIFsr3Dx12PresentBridge::IsActive() const
{
#ifdef _WIN32
	return mContext != nullptr && mSwapChain != nullptr;
#else
	return false;
#endif
}

void NRIFsr3Dx12PresentBridge::RefreshFullscreenState()
{
#ifdef _WIN32
	mSnapshot.dxgiFullscreenKnown = false;
	mSnapshot.dxgiFullscreen = false;
	if (mSwapChain == nullptr)
		return;

	BOOL fullscreen = FALSE;
	IDXGIOutput* output = nullptr;
	const HRESULT result = mSwapChain->GetFullscreenState(&fullscreen, &output);
	if (output != nullptr)
		output->Release();
	mSnapshot.dxgiFullscreenKnown = SUCCEEDED(result);
	mSnapshot.dxgiFullscreen = SUCCEEDED(result) && fullscreen == TRUE;
#endif
}
