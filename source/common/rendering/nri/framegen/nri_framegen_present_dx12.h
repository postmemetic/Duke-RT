#pragma once

#include <cstdint>

#ifdef _WIN32
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
	uint32_t bufferCount = 0;
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
	uint32_t lastCreateResult = 0;
	uint32_t lastQueryResult = 0;
	uint32_t lastDrainResult = 0;
	int64_t lastPresentHresult = 0;
	uint32_t lastPresentSyncInterval = 0;
	uint32_t lastPresentFlags = 0;
	uint32_t lastWaitResult = 0;
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
	bool Create(const NRIFsr3Dx12PresentBridgeCreateDesc& desc);
	uint32_t Drain(void* dispatchFn);
	uint32_t Destroy(void* dispatchFn, void* destroyContextFn, void* allocationCallbacks);
	int64_t Present(bool vsync, bool allowTearing);
	bool WaitForPacing(uint32_t timeoutMs);

	bool IsActive() const;
	void* GetContext() const { return mContext; }
	void** GetContextAddress() { return &mContext; }
	const NRIFsr3Dx12PresentBridgeSnapshot& GetSnapshot() const { return mSnapshot; }
#ifdef _WIN32
	IDXGISwapChain4* GetSwapChain() const { return mSwapChain; }
#endif

private:
	void RefreshFullscreenState();

	void* mContext = nullptr;
#ifdef _WIN32
	IDXGISwapChain4* mSwapChain = nullptr;
	void* mFrameLatencyWaitableObject = nullptr;
#endif
	bool mDrainRequired = false;
	NRIFsr3Dx12PresentBridgeSnapshot mSnapshot = {};
};
