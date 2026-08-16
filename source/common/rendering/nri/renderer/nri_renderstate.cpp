#include "nri_renderstate.h"

#include "../nri_output.h"
#include "../system/nri_hwbuffer.h"
#include "../system/nri_hwtexture.h"
#include "../system/nri_renderdevice.h"
#include "d_eventbase.h"
#include "hw_material.h"
#include "i_time.h"
#include "printf.h"
#include "textures.h"

#include <cstring>

namespace
{
	static bool ViewportEquals(const nri::Viewport& a, const nri::Viewport& b)
	{
		return a.x == b.x &&
			a.y == b.y &&
			a.width == b.width &&
			a.height == b.height &&
			a.depthMin == b.depthMin &&
			a.depthMax == b.depthMax;
	}

	static bool RectEquals(const nri::Rect& a, const nri::Rect& b)
	{
		return a.x == b.x &&
			a.y == b.y &&
			a.width == b.width &&
			a.height == b.height;
	}

	static uint32_t NextPowerOfTwo(uint32_t value)
	{
		if (value <= 1u)
		{
			return 1u;
		}

		value--;
		value |= value >> 1;
		value |= value >> 2;
		value |= value >> 4;
		value |= value >> 8;
		value |= value >> 16;
		return value + 1u;
	}

	static nri::StageBits NRIShaderStages()
	{
		return (nri::StageBits)((uint32_t)nri::StageBits::VERTEX_SHADER | (uint32_t)nri::StageBits::FRAGMENT_SHADER);
	}

	static nri::BlendFactor ToBlendFactor(int alpha)
	{
		switch (alpha)
		{
		default:
		case STYLEALPHA_Zero: return nri::BlendFactor::ZERO;
		case STYLEALPHA_One: return nri::BlendFactor::ONE;
		case STYLEALPHA_Src: return nri::BlendFactor::SRC_ALPHA;
		case STYLEALPHA_InvSrc: return nri::BlendFactor::ONE_MINUS_SRC_ALPHA;
		case STYLEALPHA_SrcCol: return nri::BlendFactor::SRC_COLOR;
		case STYLEALPHA_InvSrcCol: return nri::BlendFactor::ONE_MINUS_SRC_COLOR;
		case STYLEALPHA_DstCol: return nri::BlendFactor::DST_COLOR;
		case STYLEALPHA_InvDstCol: return nri::BlendFactor::ONE_MINUS_DST_COLOR;
		case STYLEALPHA_Dst: return nri::BlendFactor::DST_ALPHA;
		case STYLEALPHA_InvDst: return nri::BlendFactor::ONE_MINUS_DST_ALPHA;
		}
	}

	static nri::BlendOp ToBlendOp(int op)
	{
		switch (op)
		{
		default:
		case STYLEOP_Add: return nri::BlendOp::ADD;
		case STYLEOP_Sub: return nri::BlendOp::SUBTRACT;
		case STYLEOP_RevSub: return nri::BlendOp::REVERSE_SUBTRACT;
		}
	}

	static nri::Topology ToTopology(int dt)
	{
		switch (dt)
		{
		case DT_Points: return nri::Topology::POINT_LIST;
		case DT_Lines: return nri::Topology::LINE_LIST;
		case DT_TriangleStrip: return nri::Topology::TRIANGLE_STRIP;
		case DT_Triangles:
		default: return nri::Topology::TRIANGLE_LIST;
		}
	}

	static nri::ColorWriteBits ToColorMask(int mask)
	{
		uint32_t out = 0;
		if (mask & 0x1) out |= (uint32_t)nri::ColorWriteBits::R;
		if (mask & 0x2) out |= (uint32_t)nri::ColorWriteBits::G;
		if (mask & 0x4) out |= (uint32_t)nri::ColorWriteBits::B;
		if (mask & 0x8) out |= (uint32_t)nri::ColorWriteBits::A;
		return (nri::ColorWriteBits)out;
	}

	static nri::Format ToVertexFormat(int format)
	{
		switch (format)
		{
		case VFmt_Float4: return nri::Format::RGBA32_SFLOAT;
		case VFmt_Float3: return nri::Format::RGB32_SFLOAT;
		case VFmt_Float2: return nri::Format::RG32_SFLOAT;
		case VFmt_Float: return nri::Format::R32_SFLOAT;
		case VFmt_Byte4: return nri::Format::RGBA8_UNORM;
		case VFmt_Byte4_UInt: return nri::Format::RGBA8_UINT;
		case VFmt_Packed_A2R10G10B10: return nri::Format::R10_G10_B10_A2_UNORM;
		default: return nri::Format::UNKNOWN;
		}
	}

	static const char* ToSemanticName(int location)
	{
		switch (location)
		{
		default:
		case VATTR_VERTEX: return "POSITION";
		case VATTR_TEXCOORD: return "TEXCOORD";
		case VATTR_COLOR: return "COLOR";
		case VATTR_LIGHTMAP: return "TEXCOORD";
		}
	}

	static uint32_t ToSemanticIndex(int location)
	{
		return location == VATTR_LIGHTMAP ? 1u : 0u;
	}
}

NRIRenderState::NRIRenderState(NRIRenderDevice* fb)
	: mFrameBuffer(fb)
{
	Reset();
}

void NRIRenderState::ClearScreen()
{
	mClearTargets |= CT_Color;
	mNeedsClear = true;
}

void NRIRenderState::Draw(int dt, int index, int count, bool)
{
	Apply(dt, false, index, count);
}

void NRIRenderState::DrawIndexed(int dt, int index, int count, bool)
{
	Apply(dt, true, index, count);
}

bool NRIRenderState::SetDepthClamp(bool on)
{
	return on;
}

void NRIRenderState::SetDepthMask(bool)
{
}

void NRIRenderState::SetDepthFunc(int)
{
}

void NRIRenderState::SetDepthRange(float, float)
{
}

void NRIRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	mColorMask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
}

void NRIRenderState::SetStencil(int, int, int)
{
}

void NRIRenderState::SetCulling(int)
{
}

void NRIRenderState::EnableClipDistance(int, bool)
{
}

void NRIRenderState::Clear(int targets)
{
	if (targets & CT_Color)
	{
		EndRendering();
		mNeedsClear = true;
		mClearTargets |= CT_Color;
	}
}

void NRIRenderState::EnableStencil(bool)
{
}

void NRIRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
}

void NRIRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
}

void NRIRenderState::EnableDepthTest(bool)
{
}

void NRIRenderState::EnableMultisampling(bool)
{
}

void NRIRenderState::EnableLineSmooth(bool)
{
}

void NRIRenderState::EnableDrawBuffers(int, bool)
{
}

void NRIRenderState::BeginFrame()
{
	mLastVertexBuffer = nullptr;
	mLastIndexBuffer = nullptr;
	mLastVertexStream = {};
	mLastIndexStream = {};
	mLastBoundViewport = {};
	mLastBoundScissor = {};
	mLastBoundConstants = {};
	mLastBoundSamplerSet = nullptr;
	mLastBoundTextureSet = nullptr;
	mLastBoundPipeline = nullptr;
	mLastBoundVertexBufferResource = nullptr;
	mLastBoundVertexOffset = 0;
	mLastBoundVertexStride = 0;
	mLastBoundIndexBufferResource = nullptr;
	mLastBoundIndexOffset = 0;
	mHasBoundViewport = false;
	mHasBoundScissor = false;
	mHasBoundConstants = false;
	mHasBoundSamplerSet = false;
	mHasBoundTextureSet = false;
	mHasBoundPipeline = false;
	mHasBoundVertexBuffer = false;
	mHasBoundIndexBuffer = false;
	mRendering = false;
	mNeedsClear = true;
	mClearTargets = CT_Color;
	mPerfTraceStats = {};
}

void NRIRenderState::EndFrame()
{
	EndRendering();
}

void NRIRenderState::NotifyExternalTargetWrite()
{
	// The PT path can populate the active target without going through BeginRenderingIfNeeded().
	// Subsequent translucent/HUD raster work must treat that target as already initialized.
	mNeedsClear = false;
}

void NRIRenderState::ResetPerfTraceStats()
{
	mPerfTraceStats = {};
}

NRIRenderState::PerfTraceStats NRIRenderState::GetPerfTraceStats() const
{
	return mPerfTraceStats;
}

void NRIRenderState::Apply(int dt, bool indexed, int firstIndex, int count)
{
	const bool traceActive = PerfLoopTraceActive();
	const double applyStartMs = traceActive ? I_msTimeF() : 0.0;
	if (traceActive)
	{
		mPerfTraceStats.applyCalls++;
		if (indexed)
		{
			mPerfTraceStats.indexedCalls++;
		}
	}

	if (mFrameBuffer == nullptr || mFrameBuffer->mCommandBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return;
	}

	auto* vertexBuffer = static_cast<NRIHardwareVertexBuffer*>(mVertexBuffer);
	if (vertexBuffer == nullptr || vertexBuffer->Data() == nullptr || vertexBuffer->GetStride() == 0)
	{
		return;
	}

	if (indexed && mIndexBuffer == nullptr)
	{
		return;
	}

	int pipelineDt = dt;
	uint32_t drawFirstVertex = (uint32_t)firstIndex;
	uint32_t drawVertexCount = (uint32_t)count;
	std::vector<uint8_t> expandedTriangleFanVertices;
	const bool expandTriangleFan = !indexed && dt == DT_TriangleFan;
	if (expandTriangleFan)
	{
		const size_t stride = vertexBuffer->GetStride();
		const uint8_t* source = vertexBuffer->Data();
		const size_t bufferSize = vertexBuffer->Size();
		const size_t baseOffset = mVertexOffsets[0] >= 0 ? (size_t)mVertexOffsets[0] : 0u;
		if (stride == 0 || source == nullptr || count < 3 || firstIndex < 0 || baseOffset > bufferSize)
		{
			return;
		}

		const size_t firstVertex = (size_t)firstIndex;
		const size_t vertexCount = (size_t)count;
		const size_t availableVertexCount = (bufferSize - baseOffset) / stride;
		if (firstVertex > availableVertexCount || vertexCount > availableVertexCount - firstVertex)
		{
			return;
		}

		const size_t expandedVertexCount = (vertexCount - 2u) * 3u;
		expandedTriangleFanVertices.resize(expandedVertexCount * stride);
		uint8_t* dst = expandedTriangleFanVertices.data();
		const uint8_t* fanBase = source + baseOffset + firstVertex * stride;
		for (size_t triangleIndex = 0; triangleIndex < vertexCount - 2u; ++triangleIndex)
		{
			const uint8_t* v0 = fanBase;
			const uint8_t* v1 = fanBase + (triangleIndex + 1u) * stride;
			const uint8_t* v2 = fanBase + (triangleIndex + 2u) * stride;
			std::memcpy(dst, v0, stride);
			dst += stride;
			std::memcpy(dst, v1, stride);
			dst += stride;
			std::memcpy(dst, v2, stride);
			dst += stride;
		}

		pipelineDt = DT_Triangles;
		drawFirstVertex = 0;
		drawVertexCount = (uint32_t)expandedVertexCount;
	}

	double stageStartMs = traceActive ? I_msTimeF() : 0.0;
	if (traceActive)
	{
		mPerfTraceStats.pipelineLookups++;
	}
	nri::Pipeline* pipeline = GetPipeline(pipelineDt);
	if (traceActive)
	{
		mPerfTraceStats.pipelineMs += I_msTimeF() - stageStartMs;
	}
	if (pipeline == nullptr)
	{
		return;
	}

	stageStartMs = traceActive ? I_msTimeF() : 0.0;
	StreamedBuffer vertexStream = {};
	if (expandTriangleFan)
	{
		nri::DataSize dataChunk = { expandedTriangleFanVertices.data(), expandedTriangleFanVertices.size() };
		nri::StreamBufferDataDesc streamDesc = {};
		streamDesc.dataChunks = &dataChunk;
		streamDesc.dataChunkNum = 1;
		streamDesc.placementAlignment = NextPowerOfTwo((uint32_t)std::max<size_t>(vertexBuffer->GetStride(), 16));
		nri::BufferOffset bufferOffset = mFrameBuffer->mStreamer.StreamBufferData(*mFrameBuffer->mStreamerInstance, streamDesc);
		vertexStream.buffer = bufferOffset.buffer;
		vertexStream.offset = bufferOffset.offset;
		vertexStream.stride = (uint32_t)vertexBuffer->GetStride();
	}
	else
	{
		vertexStream = StreamVertices(vertexBuffer);
	}
	if (traceActive)
	{
		mPerfTraceStats.vertexStreamMs += I_msTimeF() - stageStartMs;
	}
	if (vertexStream.buffer == nullptr)
	{
		return;
	}

	NRIShaderConstants constants = {};
	FillShaderConstants(constants);

	nri::Viewport viewport = {};
	viewport.x = (float)mViewportX;
	viewport.y = (float)mViewportY;
	viewport.width = (float)(mViewportWidth > 0 ? mViewportWidth : (int)mFrameBuffer->mActiveTarget->width);
	viewport.height = (float)(mViewportHeight > 0 ? mViewportHeight : (int)mFrameBuffer->mActiveTarget->height);
	viewport.depthMin = 0.0f;
	viewport.depthMax = 1.0f;

	nri::Rect scissor = {};
	scissor.x = (int16_t)(mScissorX >= 0 ? mScissorX : 0);
	scissor.y = (int16_t)(mScissorY >= 0 ? mScissorY : 0);
	scissor.width = (uint32_t)(mScissorWidth >= 0 ? mScissorWidth : (mViewportWidth > 0 ? mViewportWidth : (int)mFrameBuffer->mActiveTarget->width));
	scissor.height = (uint32_t)(mScissorHeight >= 0 ? mScissorHeight : (mViewportHeight > 0 ? mViewportHeight : (int)mFrameBuffer->mActiveTarget->height));

	nri::VertexBufferDesc vertexDesc = {};
	vertexDesc.buffer = vertexStream.buffer;
	vertexDesc.offset = vertexStream.offset + (expandTriangleFan ? 0ull : (uint64_t)mVertexOffsets[0]);
	vertexDesc.stride = (uint32_t)vertexStream.stride;

	nri::DescriptorSet* textureSet = mFrameBuffer->mWhiteTextureSet;
	if (mTextureEnabled && mMaterial.mMaterial != nullptr)
	{
		MaterialLayerInfo* layer = nullptr;
		auto* hwTexture = static_cast<NRIHardwareTexture*>(mMaterial.mMaterial->GetLayer(0, mMaterial.mTranslation, &layer));
		if (hwTexture != nullptr && layer != nullptr && layer->layerTexture != nullptr)
		{
			// Lazy texture uploads use the helper upload path and can submit work immediately,
			// so they must happen before we open a rendering scope on the current command buffer.
			if (traceActive)
			{
				mPerfTraceStats.textureEnsureCalls++;
				stageStartMs = I_msTimeF();
			}
			hwTexture->EnsureTexture(layer->layerTexture, mMaterial.mTranslation, layer->scaleFlags);
			if (traceActive)
			{
				mPerfTraceStats.textureEnsureMs += I_msTimeF() - stageStartMs;
			}
			if (hwTexture->GetResource().textureSet != nullptr)
			{
				textureSet = hwTexture->GetResource().textureSet;
			}
		}
	}

	if (traceActive)
	{
		stageStartMs = I_msTimeF();
	}
	BeginRenderingIfNeeded();
	if (traceActive)
	{
		if (mRendering)
		{
			mPerfTraceStats.beginRenderingCalls++;
		}
		mPerfTraceStats.beginRenderingMs += I_msTimeF() - stageStartMs;
	}

	if (traceActive)
	{
		stageStartMs = I_msTimeF();
	}
	auto* samplerSet = mFrameBuffer->GetSamplerSet(GetSamplerMode());
	mFrameBuffer->mCore.CmdSetPipelineLayout(*mFrameBuffer->mCommandBuffer, nri::BindPoint::GRAPHICS, *mFrameBuffer->mPipelineLayout);
	mFrameBuffer->mCore.CmdSetViewports(*mFrameBuffer->mCommandBuffer, &viewport, 1);
	mFrameBuffer->mCore.CmdSetScissors(*mFrameBuffer->mCommandBuffer, &scissor, 1);
	mFrameBuffer->mCore.CmdSetRootConstants(*mFrameBuffer->mCommandBuffer, { 0, &constants, sizeof(constants), 0, nri::BindPoint::GRAPHICS });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 0, samplerSet, nri::BindPoint::GRAPHICS });
	mFrameBuffer->mCore.CmdSetDescriptorSet(*mFrameBuffer->mCommandBuffer, { 1, textureSet, nri::BindPoint::GRAPHICS });
	mFrameBuffer->mCore.CmdSetPipeline(*mFrameBuffer->mCommandBuffer, *pipeline);
	mFrameBuffer->mCore.CmdSetVertexBuffers(*mFrameBuffer->mCommandBuffer, 0, &vertexDesc, 1);
	if (traceActive)
	{
		if (!mHasBoundViewport || !ViewportEquals(mLastBoundViewport, viewport)) mPerfTraceStats.viewportChanges++;
		else mPerfTraceStats.viewportNoops++;
		if (!mHasBoundScissor || !RectEquals(mLastBoundScissor, scissor)) mPerfTraceStats.scissorChanges++;
		else mPerfTraceStats.scissorNoops++;
		if (!mHasBoundConstants || std::memcmp(&mLastBoundConstants, &constants, sizeof(constants)) != 0) mPerfTraceStats.rootConstantChanges++;
		else mPerfTraceStats.rootConstantNoops++;
		if (!mHasBoundSamplerSet || mLastBoundSamplerSet != samplerSet) mPerfTraceStats.samplerSetChanges++;
		else mPerfTraceStats.samplerSetNoops++;
		if (!mHasBoundTextureSet || mLastBoundTextureSet != textureSet) mPerfTraceStats.textureSetChanges++;
		else mPerfTraceStats.textureSetNoops++;
		if (!mHasBoundPipeline || mLastBoundPipeline != pipeline) mPerfTraceStats.pipelineChanges++;
		else mPerfTraceStats.pipelineNoops++;
		if (!mHasBoundVertexBuffer ||
			mLastBoundVertexBufferResource != vertexDesc.buffer ||
			mLastBoundVertexOffset != vertexDesc.offset ||
			mLastBoundVertexStride != vertexDesc.stride)
		{
			mPerfTraceStats.vertexBufferChanges++;
		}
		else
		{
			mPerfTraceStats.vertexBufferNoops++;
		}
	}
	mLastBoundViewport = viewport;
	mLastBoundScissor = scissor;
	mLastBoundConstants = constants;
	mLastBoundSamplerSet = samplerSet;
	mLastBoundTextureSet = textureSet;
	mLastBoundPipeline = pipeline;
	mLastBoundVertexBufferResource = vertexDesc.buffer;
	mLastBoundVertexOffset = vertexDesc.offset;
	mLastBoundVertexStride = vertexDesc.stride;
	mHasBoundViewport = true;
	mHasBoundScissor = true;
	mHasBoundConstants = true;
	mHasBoundSamplerSet = true;
	mHasBoundTextureSet = true;
	mHasBoundPipeline = true;
	mHasBoundVertexBuffer = true;
	if (traceActive)
	{
		mPerfTraceStats.bindStateMs += I_msTimeF() - stageStartMs;
	}

	if (indexed)
	{
		auto* indexBuffer = static_cast<NRIHardwareIndexBuffer*>(mIndexBuffer);
		if (traceActive)
		{
			stageStartMs = I_msTimeF();
		}
		StreamedBuffer indexStream = StreamIndices(indexBuffer, 0, (uint32_t)indexBuffer->Size() / 4u);
		if (traceActive)
		{
			mPerfTraceStats.indexStreamMs += I_msTimeF() - stageStartMs;
		}
		if (indexStream.buffer == nullptr)
		{
			return;
		}

		if (traceActive)
		{
			stageStartMs = I_msTimeF();
		}
		mFrameBuffer->mCore.CmdSetIndexBuffer(*mFrameBuffer->mCommandBuffer, *indexStream.buffer, indexStream.offset, nri::IndexType::UINT32);
		mFrameBuffer->mCore.CmdDrawIndexed(*mFrameBuffer->mCommandBuffer, { (uint32_t)count, 1, (uint32_t)firstIndex, 0, 0 });
		if (traceActive)
		{
			if (!mHasBoundIndexBuffer ||
				mLastBoundIndexBufferResource != indexStream.buffer ||
				mLastBoundIndexOffset != indexStream.offset)
			{
				mPerfTraceStats.indexBufferChanges++;
			}
			else
			{
				mPerfTraceStats.indexBufferNoops++;
			}
		}
		mLastBoundIndexBufferResource = indexStream.buffer;
		mLastBoundIndexOffset = indexStream.offset;
		mHasBoundIndexBuffer = true;
		if (traceActive)
		{
			mPerfTraceStats.drawCallMs += I_msTimeF() - stageStartMs;
		}
	}
	else
	{
		if (traceActive)
		{
			stageStartMs = I_msTimeF();
		}
		mFrameBuffer->mCore.CmdDraw(*mFrameBuffer->mCommandBuffer, { drawVertexCount, 1, drawFirstVertex, 0 });
		if (traceActive)
		{
			mPerfTraceStats.drawCallMs += I_msTimeF() - stageStartMs;
		}
	}

	if (traceActive)
	{
		mPerfTraceStats.applyMs += I_msTimeF() - applyStartMs;
	}
}

NRIRenderState::StreamedBuffer NRIRenderState::StreamVertices(NRIHardwareVertexBuffer* vertexBuffer)
{
	if (vertexBuffer == nullptr || vertexBuffer->Data() == nullptr || vertexBuffer->Size() == 0)
	{
		return {};
	}

	if (mLastVertexBuffer == vertexBuffer &&
		mLastVertexStream.objectId == vertexBuffer->ObjectId() &&
		mLastVertexStream.generation == vertexBuffer->Generation())
	{
		return mLastVertexStream;
	}

	nri::DataSize dataChunk = { vertexBuffer->Data(), vertexBuffer->Size() };
	nri::StreamBufferDataDesc streamDesc = {};
	streamDesc.dataChunks = &dataChunk;
	streamDesc.dataChunkNum = 1;
	// NRI's internal alignment helper assumes power-of-two alignment. Some raster
	// vertex formats, including the skydome path, use non-power-of-two strides such as 36 bytes.
	streamDesc.placementAlignment = NextPowerOfTwo((uint32_t)std::max<size_t>(vertexBuffer->GetStride(), 16));

	nri::BufferOffset bufferOffset = mFrameBuffer->mStreamer.StreamBufferData(*mFrameBuffer->mStreamerInstance, streamDesc);

	mLastVertexBuffer = vertexBuffer;
	mLastVertexStream.buffer = bufferOffset.buffer;
	mLastVertexStream.offset = bufferOffset.offset;
	mLastVertexStream.stride = (uint32_t)vertexBuffer->GetStride();
	mLastVertexStream.objectId = vertexBuffer->ObjectId();
	mLastVertexStream.generation = vertexBuffer->Generation();
	return mLastVertexStream;
}

NRIRenderState::StreamedBuffer NRIRenderState::StreamIndices(NRIHardwareIndexBuffer* indexBuffer, uint32_t, uint32_t)
{
	if (indexBuffer == nullptr || indexBuffer->Data() == nullptr || indexBuffer->Size() == 0)
	{
		return {};
	}

	if (mLastIndexBuffer == indexBuffer &&
		mLastIndexStream.objectId == indexBuffer->ObjectId() &&
		mLastIndexStream.generation == indexBuffer->Generation())
	{
		return mLastIndexStream;
	}

	nri::DataSize dataChunk = { indexBuffer->Data(), indexBuffer->Size() };
	nri::StreamBufferDataDesc streamDesc = {};
	streamDesc.dataChunks = &dataChunk;
	streamDesc.dataChunkNum = 1;
	streamDesc.placementAlignment = 16;

	nri::BufferOffset bufferOffset = mFrameBuffer->mStreamer.StreamBufferData(*mFrameBuffer->mStreamerInstance, streamDesc);

	mLastIndexBuffer = indexBuffer;
	mLastIndexStream.buffer = bufferOffset.buffer;
	mLastIndexStream.offset = bufferOffset.offset;
	mLastIndexStream.stride = 4;
	mLastIndexStream.objectId = indexBuffer->ObjectId();
	mLastIndexStream.generation = indexBuffer->Generation();
	return mLastIndexStream;
}

nri::Pipeline* NRIRenderState::GetPipeline(int dt)
{
	auto* vertexBuffer = static_cast<NRIHardwareVertexBuffer*>(mVertexBuffer);
	if (vertexBuffer == nullptr || mFrameBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return nullptr;
	}

	PipelineKey key = {};
	key.blendOp = mRenderStyle.BlendOp;
	key.srcBlend = mRenderStyle.SrcAlpha;
	key.dstBlend = mRenderStyle.DestAlpha;
	key.topology = (uint32_t)ToTopology(dt);
	key.format = (uint32_t)mFrameBuffer->mActiveTarget->format;
	key.colorMask = (uint32_t)mColorMask;
	key.vertexLayout = vertexBuffer->GetLayoutHash();

	auto it = mPipelines.find(key);
	if (it != mPipelines.end())
	{
		return it->second;
	}

	nri::VertexAttributeDesc attributes[VATTR_MAX] = {};
	uint8_t attributeCount = 0;
	bool hasPosition = false;
	bool hasTexCoord = false;
	bool hasColor = false;
	for (int i = 0; i < vertexBuffer->GetAttributeCount(); ++i)
	{
		const FVertexBufferAttribute& attr = vertexBuffer->GetAttributes()[i];
		if (attr.location != VATTR_VERTEX && attr.location != VATTR_TEXCOORD && attr.location != VATTR_COLOR)
		{
			continue;
		}

		if (attr.binding != 0)
		{
			continue;
		}

		nri::Format format = ToVertexFormat(attr.format);
		if (format == nri::Format::UNKNOWN)
		{
			continue;
		}

		nri::VertexAttributeDesc& outAttr = attributes[attributeCount++];
		outAttr.d3d.semanticName = ToSemanticName(attr.location);
		outAttr.d3d.semanticIndex = ToSemanticIndex(attr.location);
		outAttr.vk.location = (uint32_t)attr.location;
		outAttr.offset = attr.offset;
		outAttr.format = format;
		outAttr.streamIndex = 0;

		hasPosition |= attr.location == VATTR_VERTEX;
		hasTexCoord |= attr.location == VATTR_TEXCOORD;
		hasColor |= attr.location == VATTR_COLOR;
	}

	if (!hasPosition || !hasTexCoord || !hasColor)
	{
		static bool sLoggedUnsupportedLayout = false;
		if (!sLoggedUnsupportedLayout)
		{
			Printf("NRI 2D graphics pipeline skipped unsupported vertex layout: attrs=%u position=%u texcoord=%u color=%u layout=0x%llx\n",
				(uint32_t)vertexBuffer->GetAttributeCount(),
				hasPosition ? 1u : 0u,
				hasTexCoord ? 1u : 0u,
				hasColor ? 1u : 0u,
				(unsigned long long)vertexBuffer->GetLayoutHash());
			sLoggedUnsupportedLayout = true;
		}
		return nullptr;
	}

	nri::VertexStreamDesc streamDesc = {};
	streamDesc.bindingSlot = 0;
	streamDesc.stepRate = nri::VertexStreamStepRate::PER_VERTEX;

	nri::VertexInputDesc vertexInput = {};
	vertexInput.attributes = attributes;
	vertexInput.attributeNum = attributeCount;
	vertexInput.streams = &streamDesc;
	vertexInput.streamNum = 1;

	size_t vsSize = 0;
	size_t psSize = 0;
	const void* vsBytecode = mFrameBuffer->GetVertexShaderBytecode(vsSize);
	const void* psBytecode = mFrameBuffer->GetPixelShaderBytecode(psSize);
	if (vsBytecode == nullptr || psBytecode == nullptr)
	{
		return nullptr;
	}

	nri::ShaderDesc shaders[2] = {};
	shaders[0].stage = nri::StageBits::VERTEX_SHADER;
	shaders[0].bytecode = vsBytecode;
	shaders[0].size = vsSize;
	shaders[0].entryPointName = "main";
	shaders[1].stage = nri::StageBits::FRAGMENT_SHADER;
	shaders[1].bytecode = psBytecode;
	shaders[1].size = psSize;
	shaders[1].entryPointName = "main";

	nri::InputAssemblyDesc inputAssembly = {};
	inputAssembly.topology = ToTopology(dt);
	inputAssembly.primitiveRestart = nri::PrimitiveRestart::DISABLED;

	nri::RasterizationDesc rasterization = {};
	rasterization.fillMode = nri::FillMode::SOLID;
	rasterization.cullMode = nri::CullMode::NONE;

	nri::BlendDesc blend = {};
	blend.srcFactor = ToBlendFactor(mRenderStyle.SrcAlpha);
	blend.dstFactor = ToBlendFactor(mRenderStyle.DestAlpha);
	blend.op = ToBlendOp(mRenderStyle.BlendOp);

	nri::ColorAttachmentDesc colorAttachment = {};
	colorAttachment.format = mFrameBuffer->mActiveTarget->format;
	colorAttachment.colorBlend = blend;
	colorAttachment.alphaBlend = blend;
	colorAttachment.colorWriteMask = ToColorMask(mColorMask);
	colorAttachment.blendEnabled = !(mRenderStyle.SrcAlpha == STYLEALPHA_One && mRenderStyle.DestAlpha == STYLEALPHA_Zero && mRenderStyle.BlendOp == STYLEOP_Add);

	nri::OutputMergerDesc outputMerger = {};
	outputMerger.colors = &colorAttachment;
	outputMerger.colorNum = 1;
	outputMerger.depth.compareOp = nri::CompareOp::NONE;
	outputMerger.depth.write = false;
	outputMerger.depthStencilFormat = nri::Format::UNKNOWN;

	nri::GraphicsPipelineDesc pipelineDesc = {};
	pipelineDesc.pipelineLayout = mFrameBuffer->mPipelineLayout;
	pipelineDesc.vertexInput = &vertexInput;
	pipelineDesc.inputAssembly = inputAssembly;
	pipelineDesc.rasterization = rasterization;
	pipelineDesc.outputMerger = outputMerger;
	pipelineDesc.shaders = shaders;
	pipelineDesc.shaderNum = 2;

	nri::Pipeline* pipeline = nullptr;
	const nri::Result result = mFrameBuffer->mCore.CreateGraphicsPipeline(*mFrameBuffer->mDevice, pipelineDesc, pipeline);
	if (result != nri::Result::SUCCESS)
	{
		Printf(TEXTCOLOR_RED "NRI 2D graphics pipeline creation failed for target format %u with %u vertex attributes (result=%d).\n",
			(uint32_t)mFrameBuffer->mActiveTarget->format,
			(uint32_t)vertexBuffer->GetAttributeCount(),
			(int)result);
		return nullptr;
	}

	if (PerfLoopTraceActive())
	{
		mPerfTraceStats.pipelineCreates++;
	}
	mPipelines.emplace(key, pipeline);
	return pipeline;
}

void NRIRenderState::BeginRenderingIfNeeded()
{
	if (mRendering || mFrameBuffer == nullptr || mFrameBuffer->mActiveTarget == nullptr)
	{
		return;
	}

	NRITextureResource& target = *mFrameBuffer->mActiveTarget;
	if (target.colorAttachmentView == nullptr)
	{
		return;
	}

	mFrameBuffer->PrepareTargetForRendering(target, mNeedsClear);

	nri::AttachmentDesc colorAttachment = {};
	colorAttachment.descriptor = target.colorAttachmentView;
	colorAttachment.loadOp = mNeedsClear ? nri::LoadOp::CLEAR : nri::LoadOp::LOAD;
	colorAttachment.storeOp = nri::StoreOp::STORE;
	colorAttachment.clearValue.color.f.x = mNeedsClear ? mFrameBuffer->mSceneClearColor[0] : 0.0f;
	colorAttachment.clearValue.color.f.y = mNeedsClear ? mFrameBuffer->mSceneClearColor[1] : 0.0f;
	colorAttachment.clearValue.color.f.z = mNeedsClear ? mFrameBuffer->mSceneClearColor[2] : 0.0f;
	colorAttachment.clearValue.color.f.w = mNeedsClear ? mFrameBuffer->mSceneClearColor[3] : 1.0f;

	nri::RenderingDesc renderingDesc = {};
	renderingDesc.colors = &colorAttachment;
	renderingDesc.colorNum = 1;
	mFrameBuffer->mCore.CmdBeginRendering(*mFrameBuffer->mCommandBuffer, renderingDesc);
	mRendering = true;
	mNeedsClear = false;
}

void NRIRenderState::EndRendering()
{
	if (!mRendering || mFrameBuffer == nullptr)
	{
		return;
	}

	mFrameBuffer->mCore.CmdEndRendering(*mFrameBuffer->mCommandBuffer);
	mRendering = false;
}

void NRIRenderState::FillShaderConstants(NRIShaderConstants& constants) const
{
	const int viewportWidth = mViewportWidth > 0 ? mViewportWidth : (mFrameBuffer->mActiveTarget != nullptr ? (int)mFrameBuffer->mActiveTarget->width : 1);
	const int viewportHeight = mViewportHeight > 0 ? mViewportHeight : (mFrameBuffer->mActiveTarget != nullptr ? (int)mFrameBuffer->mActiveTarget->height : 1);

	constants.InvViewportSize[0] = viewportWidth > 0 ? 1.0f / viewportWidth : 1.0f;
	constants.InvViewportSize[1] = viewportHeight > 0 ? 1.0f / viewportHeight : 1.0f;
	constants.ScreenFade = mStreamData.uDynLightColor.W;
	constants.Flags = 0;
	constants.OutputInfo[0] = 1.0f;
	constants.OutputInfo[1] = 1.0f;
	constants.OutputInfo[2] = 0.0f;
	constants.OutputInfo[3] = 0.0f;

	if (mTextureEnabled && mMaterial.mMaterial != nullptr)
	{
		constants.Flags |= NRI2D_Textured;
	}

	if (mTextureMode == TM_ALPHATEXTURE || (mRenderStyle.Flags & STYLEF_RedIsAlpha))
	{
		constants.Flags |= NRI2D_AlphaFromRed;
	}

	if (mTextureMode == TM_INVERSE || mTextureMode == TM_INVERTOPAQUE || (mRenderStyle.Flags & STYLEF_InvertSource))
	{
		constants.Flags |= NRI2D_Invert;
	}

	if (mFrameBuffer != nullptr && mFrameBuffer->mActiveTarget != nullptr)
	{
		const bool isPresentTarget = mFrameBuffer->mActiveTarget == mFrameBuffer->mCurrentPresentTarget;
		const bool isFrameGenerationUiTarget =
			mFrameBuffer->IsFrameGenerationPresentPathActive() &&
			mFrameBuffer->mActiveTarget == mFrameBuffer->GetFrameGenerationUiTargetResource();
		const NRIPTOutputPolicy outputPolicy = mFrameBuffer->GetPathTracingOutputPolicy();
		if ((isPresentTarget || isFrameGenerationUiTarget) && outputPolicy.hdrSwapChainActive)
		{
			constants.Flags |= NRI2D_OutputHdrLinear;
			constants.OutputInfo[0] = GetNRIPTHdrPaperWhiteScale(outputPolicy);
			constants.OutputInfo[1] = 2.2f;
		}
	}

	constants.ObjectColor[0] = mStreamData.uObjectColor.r;
	constants.ObjectColor[1] = mStreamData.uObjectColor.g;
	constants.ObjectColor[2] = mStreamData.uObjectColor.b;
	constants.ObjectColor[3] = mStreamData.uObjectColor.a;

	constants.AddColor[0] = mStreamData.uAddColor.r;
	constants.AddColor[1] = mStreamData.uAddColor.g;
	constants.AddColor[2] = mStreamData.uAddColor.b;
	constants.AddColor[3] = mStreamData.uAddColor.a;

	constants.VertexColor[0] = mStreamData.uVertexColor.X;
	constants.VertexColor[1] = mStreamData.uVertexColor.Y;
	constants.VertexColor[2] = mStreamData.uVertexColor.Z;
	constants.VertexColor[3] = mStreamData.uVertexColor.W;

	std::memcpy(constants.ModelMatrix, mModelMatrix.get(), sizeof(constants.ModelMatrix));
	std::memcpy(constants.TexMatrix, mTextureMatrix.get(), sizeof(constants.TexMatrix));
}

NRISamplerMode NRIRenderState::GetSamplerMode() const
{
	return mFrameBuffer->GetSamplerMode(mMaterial.mClampMode);
}
