#pragma once

#include <cstdint>

#ifdef _WIN32
struct IDXGIFactory7;
struct IDXGIOutput6;
struct IDXGISwapChain4;
#endif

struct NRIFsr3Dx12PresentBridgeCreateDesc
{
	void* windowHandle = nullptr;
	void* gameQueue = nullptr;
	void* createContextFn = nullptr;
	void* queryFn = nullptr;
	void* allocationCallbacks = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t format = 0;
	uint32_t colorSpace = 0;
	uint32_t bufferCount = 0;
};

struct NRIFsr3Dx12DisplayDesc
{
	float redPrimary[2] = {};
	float greenPrimary[2] = {};
	float bluePrimary[2] = {};
	float whitePoint[2] = {};
	float minLuminance = 0.0f;
	float maxLuminance = 80.0f;
	float maxFullFrameLuminance = 80.0f;
	float sdrLuminance = 80.0f;
	bool isHdr = false;
};

struct NRIFsr3Dx12PresentBridgeSnapshot
{
	bool contextCreated = false;
	bool swapChainCreated = false;
	bool tearingSupported = false;
	bool windowAssociationKnown = false;
	bool windowAssociationSucceeded = false;
	bool dxgiFullscreenKnown = false;
	bool dxgiFullscreen = false;
	bool memoryUsageValid = false;
	bool waitableObjectAvailable = false;
	bool colorSpaceSupportValid = false;
	bool colorSpaceSet = false;
	bool observedColorSpaceValid = false;
	uint32_t lastCreateResult = 0;
	uint32_t lastQueryResult = 0;
	uint32_t lastDrainResult = 0;
	int64_t lastPresentHresult = 0;
	uint32_t lastPresentSyncInterval = 0;
	uint32_t lastPresentFlags = 0;
	uint32_t lastWaitResult = 0;
	uint32_t requestedColorSpace = 0;
	uint32_t observedColorSpace = 0;
	uint32_t colorSpaceSupport = 0;
	int64_t colorSpaceCheckHresult = 0;
	int64_t colorSpaceSetHresult = 0;
	int64_t lastExitFullscreenHresult = 0;
	uint64_t createGeneration = 0;
	uint64_t createAttemptCount = 0;
	uint64_t drainCount = 0;
	uint64_t pacingWaitCount = 0;
	uint64_t pacingWaitTimeoutCount = 0;
	uint64_t totalUsageBytes = 0;
	uint64_t aliasableUsageBytes = 0;
};

class NRIFsr3Dx12PresentBridge
{
public:
	~NRIFsr3Dx12PresentBridge();

	bool Create(const NRIFsr3Dx12PresentBridgeCreateDesc& desc);
	uint32_t Drain(void* dispatchFn);
	uint32_t Destroy(void* dispatchFn, void* destroyContextFn, void* allocationCallbacks);
	int64_t Present(bool vsync, bool allowTearing);
	bool WaitForPacing(uint32_t timeoutMs);
	bool QueryDisplayDesc(void* windowHandle, NRIFsr3Dx12DisplayDesc& outDesc);

	bool IsActive() const;
	void* GetContext() const { return mContext; }
	void** GetContextAddress() { return &mContext; }
	const NRIFsr3Dx12PresentBridgeSnapshot& GetSnapshot() const { return mSnapshot; }
#ifdef _WIN32
	IDXGISwapChain4* GetSwapChain() const { return mSwapChain; }
#endif

private:
	void RefreshFullscreenState();
#ifdef _WIN32
	void ReleaseDisplayQueryCache();
	bool EnsureDisplayOutput(void* monitor);
#endif

	void* mContext = nullptr;
#ifdef _WIN32
	IDXGISwapChain4* mSwapChain = nullptr;
	void* mFrameLatencyWaitableObject = nullptr;
	IDXGIFactory7* mDisplayFactory = nullptr;
	IDXGIOutput6* mDisplayOutput = nullptr;
	void* mDisplayMonitor = nullptr;
	uint64_t mSdrLuminanceQueryTick = 0;
	float mCachedSdrLuminance = 80.0f;
	bool mCachedSdrLuminanceValid = false;
#endif
	bool mDrainRequired = false;
	NRIFsr3Dx12PresentBridgeSnapshot mSnapshot = {};
};
