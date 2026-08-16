#include "nri_framegen_present_dx12.h"
#include "nri_ffx_api.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>

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
}
#endif

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
	mSnapshot.lastCreateResult = 0;
	mSnapshot.lastQueryResult = 0;
	mSnapshot.totalUsageBytes = 0;
	mSnapshot.aliasableUsageBytes = 0;

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
		++mSnapshot.createGeneration;
		if (mSwapChain != nullptr)
		{
			const HRESULT associationResult = factory->MakeWindowAssociation(static_cast<HWND>(desc.windowHandle), DXGI_MWA_NO_WINDOW_CHANGES);
			mSnapshot.windowAssociationKnown = true;
			mSnapshot.windowAssociationSucceeded = SUCCEEDED(associationResult);
			RefreshFullscreenState();
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

	factory->Release();
	return IsActive();
#endif
}

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
	mDrainRequired = false;
	mSnapshot.contextCreated = false;
	mSnapshot.swapChainCreated = false;
	mSnapshot.dxgiFullscreenKnown = false;
	mSnapshot.dxgiFullscreen = false;
	mSnapshot.windowAssociationKnown = false;
	mSnapshot.windowAssociationSucceeded = false;
	return result;
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
