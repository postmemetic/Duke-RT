#pragma once

#include "../system/nri_local.h"
#include "hwrenderer/data/hw_renderstate.h"

#include <unordered_map>
#include <vector>

class NRIHardwareVertexBuffer;
class NRIHardwareIndexBuffer;
class NRIRenderDevice;

class NRIRenderState : public FRenderState
{
public:
	struct PerfTraceStats
	{
		uint32_t applyCalls = 0;
		uint32_t indexedCalls = 0;
		uint32_t pipelineLookups = 0;
		uint32_t pipelineCreates = 0;
		uint32_t textureEnsureCalls = 0;
		uint32_t beginRenderingCalls = 0;
		uint32_t viewportChanges = 0;
		uint32_t viewportNoops = 0;
		uint32_t scissorChanges = 0;
		uint32_t scissorNoops = 0;
		uint32_t rootConstantChanges = 0;
		uint32_t rootConstantNoops = 0;
		uint32_t samplerSetChanges = 0;
		uint32_t samplerSetNoops = 0;
		uint32_t textureSetChanges = 0;
		uint32_t textureSetNoops = 0;
		uint32_t pipelineChanges = 0;
		uint32_t pipelineNoops = 0;
		uint32_t vertexBufferChanges = 0;
		uint32_t vertexBufferNoops = 0;
		uint32_t indexBufferChanges = 0;
		uint32_t indexBufferNoops = 0;
		double applyMs = 0.0;
		double pipelineMs = 0.0;
		double vertexStreamMs = 0.0;
		double indexStreamMs = 0.0;
		double textureEnsureMs = 0.0;
		double beginRenderingMs = 0.0;
		double bindStateMs = 0.0;
		double drawCallMs = 0.0;
	};

	explicit NRIRenderState(NRIRenderDevice* fb);
	// Call after GPU completion, while the device and its pipeline layout still exist.
	void DestroyPipelines();

	void ClearScreen() override;
	void Draw(int dt, int index, int count, bool apply = true) override;
	void DrawIndexed(int dt, int index, int count, bool apply = true) override;
	bool SetDepthClamp(bool on) override;
	void SetDepthMask(bool on) override;
	void SetDepthFunc(int func) override;
	void SetDepthRange(float min, float max) override;
	void SetColorMask(bool r, bool g, bool b, bool a) override;
	void SetStencil(int offs, int op, int flags = -1) override;
	void SetCulling(int mode) override;
	void EnableClipDistance(int num, bool state) override;
	void Clear(int targets) override;
	void EnableStencil(bool on) override;
	void SetScissor(int x, int y, int w, int h) override;
	void SetViewport(int x, int y, int w, int h) override;
	void EnableDepthTest(bool on) override;
	void EnableMultisampling(bool on) override;
	void EnableLineSmooth(bool on) override;
	void EnableDrawBuffers(int count, bool apply = false) override;

	void BeginFrame();
	void EndFrame();
	void NotifyExternalTargetWrite();
	bool NeedsClear() const { return mNeedsClear; }
	void ResetPerfTraceStats();
	PerfTraceStats GetPerfTraceStats() const;

private:
	struct PipelineKey
	{
		uint32_t blendOp = 0;
		uint32_t srcBlend = 0;
		uint32_t dstBlend = 0;
		uint32_t topology = 0;
		uint32_t format = 0;
		uint32_t colorMask = 0;
		uint64_t vertexLayout = 0;

		bool operator==(const PipelineKey& other) const
		{
			return blendOp == other.blendOp &&
				srcBlend == other.srcBlend &&
				dstBlend == other.dstBlend &&
				topology == other.topology &&
				format == other.format &&
				colorMask == other.colorMask &&
				vertexLayout == other.vertexLayout;
		}
	};

	struct PipelineKeyHash
	{
		size_t operator()(const PipelineKey& key) const
		{
			return (size_t)key.blendOp ^
				((size_t)key.srcBlend << 8) ^
				((size_t)key.dstBlend << 16) ^
				((size_t)key.topology << 24) ^
				((size_t)key.format << 32) ^
				((size_t)key.colorMask << 40) ^
				(key.vertexLayout << 48);
		}
	};

	struct StreamedBuffer
	{
		nri::Buffer* buffer = nullptr;
		uint64_t offset = 0;
		uint32_t stride = 0;
		uint64_t generation = 0;
		uint64_t objectId = 0;
	};

	void Apply(int dt, bool indexed, int firstIndex, int count);
	StreamedBuffer StreamVertices(NRIHardwareVertexBuffer* vertexBuffer);
	StreamedBuffer StreamIndices(NRIHardwareIndexBuffer* indexBuffer, uint32_t firstIndex, uint32_t count);
	nri::Pipeline* GetPipeline(int dt);
	void BeginRenderingIfNeeded();
	void EndRendering();
	void FillShaderConstants(NRIShaderConstants& constants) const;
	NRISamplerMode GetSamplerMode() const;

	NRIRenderDevice* mFrameBuffer = nullptr;
	std::unordered_map<PipelineKey, nri::Pipeline*, PipelineKeyHash> mPipelines;
	StreamedBuffer mLastVertexStream;
	StreamedBuffer mLastIndexStream;
	NRIHardwareVertexBuffer* mLastVertexBuffer = nullptr;
	NRIHardwareIndexBuffer* mLastIndexBuffer = nullptr;
	nri::Viewport mLastBoundViewport = {};
	nri::Rect mLastBoundScissor = {};
	NRIShaderConstants mLastBoundConstants = {};
	nri::DescriptorSet* mLastBoundSamplerSet = nullptr;
	nri::DescriptorSet* mLastBoundTextureSet = nullptr;
	nri::Pipeline* mLastBoundPipeline = nullptr;
	const nri::Buffer* mLastBoundVertexBufferResource = nullptr;
	uint64_t mLastBoundVertexOffset = 0;
	uint32_t mLastBoundVertexStride = 0;
	const nri::Buffer* mLastBoundIndexBufferResource = nullptr;
	uint64_t mLastBoundIndexOffset = 0;
	bool mHasBoundViewport = false;
	bool mHasBoundScissor = false;
	bool mHasBoundConstants = false;
	bool mHasBoundSamplerSet = false;
	bool mHasBoundTextureSet = false;
	bool mHasBoundPipeline = false;
	bool mHasBoundVertexBuffer = false;
	bool mHasBoundIndexBuffer = false;
	int mViewportX = 0;
	int mViewportY = 0;
	int mViewportWidth = 0;
	int mViewportHeight = 0;
	int mScissorX = -1;
	int mScissorY = -1;
	int mScissorWidth = -1;
	int mScissorHeight = -1;
	int mColorMask = 0xF;
	int mClearTargets = CT_Color;
	bool mRendering = false;
	bool mNeedsClear = true;
	PerfTraceStats mPerfTraceStats;
};
