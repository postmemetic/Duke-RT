#include "nri_hwtexture.h"

#include "../scene/nri_texture_signature.h"
#include "nri_renderdevice.h"
#include "printf.h"
#include "textures.h"
#include "i_safe_memory.h"

namespace
{
	bool TryBuildHardwareTextureSignature(FTexture* texture, int translation, int flags, nri_scene::TextureSignature& outSignature)
	{
		outSignature = {};
		if (texture == nullptr || !nri_scene::IsTexturePersistentSignatureEligible(texture))
		{
			return false;
		}

		nri_scene::TextureSignatureRequest request = {};
		request.contentKind = nri_scene::TextureSignatureContentKind::ProcessedBGRA;
		request.translation = translation;
		request.flags = nri_scene::TextureSignatureRequestFlag_None;
		if ((flags & CTF_Expand) != 0)
		{
			request.flags |= nri_scene::TextureSignatureRequestFlag_Expand;
		}
		if ((flags & CTF_Upscale) != 0)
		{
			request.flags |= nri_scene::TextureSignatureRequestFlag_Upscale;
		}

		return nri_scene::TryBuildImageTextureSignature(texture, request, outSignature) &&
			outSignature.valid &&
			outSignature.persistentEligible;
	}

	bool TryMemcpyTexturePixels(void* dst, const void* src, size_t size)
	{
		return I_TryReadMemory(dst, src, size);
	}
}

NRIHardwareTexture::NRIHardwareTexture(NRIRenderDevice* fb, int numchannels)
	: mFrameBuffer(fb), mChannels(numchannels)
{
}

NRIHardwareTexture::~NRIHardwareTexture()
{
	Reset();
}

void NRIHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (w <= 0 || h <= 0 || texelsize <= 0)
	{
		mStagingPixels.clear();
		bufferpitch = 0;
		return;
	}

	mChannels = texelsize;
	bufferpitch = w;
	mStagingPixels.resize((size_t)w * (size_t)h * (size_t)texelsize);

	const nri::Format format = texelsize == 1 ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	CreateTextureResource((uint32_t)w, (uint32_t)h, format, nri::TextureUsageBits::SHADER_RESOURCE);
	mContentId = 0;
}

uint8_t* NRIHardwareTexture::MapBuffer()
{
	return mStagingPixels.empty() ? nullptr : mStagingPixels.data();
}

unsigned int NRIHardwareTexture::CreateTexture(unsigned char* buffer, int w, int h, int, bool, const char*)
{
	if (w <= 0 || h <= 0)
	{
		return 0;
	}

	const nri::Format format = mChannels == 1 ? nri::Format::R8_UNORM : nri::Format::BGRA8_UNORM;
	CreateTextureResource((uint32_t)w, (uint32_t)h, format, nri::TextureUsageBits::SHADER_RESOURCE);

	if (buffer != nullptr)
	{
		UploadTextureData(buffer, (uint32_t)w, (uint32_t)h, format, (uint32_t)w * (uint32_t)mChannels);
	}
	else if (!mStagingPixels.empty())
	{
		UploadTextureData(mStagingPixels.data(), (uint32_t)w, (uint32_t)h, format, (uint32_t)w * (uint32_t)mChannels);
	}

	mContentId = 0;
	return 0;
}

void NRIHardwareTexture::EnsureTexture(FTexture* tex, int translation, int flags)
{
	static int sUploadFailureLogCount = 0;

	if (tex == nullptr)
	{
		return;
	}

	if (tex->isHardwareCanvas())
	{
		if (mFrameBuffer != nullptr)
		{
			mFrameBuffer->Note2DTextureEnsure(true);
		}
		EnsureCanvas(tex);
		return;
	}

	// Wrapper-backed SWCanvas textures already own a live GPU resource.
	// Do not fall back to CreateTexBuffer(), which for FWrapperTexture has no software pixels.
	if (dynamic_cast<FWrapperTexture*>(tex) != nullptr)
	{
		if (mFrameBuffer != nullptr)
		{
			mFrameBuffer->Note2DTextureEnsure(false);
			mFrameBuffer->Note2DTextureCacheHit();
		}
		return;
	}

	if (mFrameBuffer != nullptr)
	{
		mFrameBuffer->Note2DTextureEnsure(false);
	}

	nri_scene::TextureSignature signature = {};
	const bool hasMetadataSignature = TryBuildHardwareTextureSignature(tex, translation, flags, signature);
	if (hasMetadataSignature &&
		mResource.texture != nullptr &&
		mContentId == signature.key &&
		mResource.width == signature.width &&
		mResource.height == signature.height &&
		mResource.format == nri::Format::BGRA8_UNORM)
	{
		if (mFrameBuffer != nullptr)
		{
			mFrameBuffer->Note2DTextureCacheHit();
		}
		return;
	}

	FTextureBuffer texBuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
	if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
	{
		return;
	}

	if (mResource.texture != nullptr &&
		mContentId == texBuffer.mContentId &&
		mResource.width == (uint32_t)texBuffer.mWidth &&
		mResource.height == (uint32_t)texBuffer.mHeight &&
		mResource.format == nri::Format::BGRA8_UNORM)
	{
		if (mFrameBuffer != nullptr)
		{
			mFrameBuffer->Note2DTextureCacheHit();
		}
		return;
	}

	if (mFrameBuffer != nullptr)
	{
		mFrameBuffer->Note2DTextureCacheMiss();
	}

	if (mResource.texture != nullptr)
	{
		mFrameBuffer->WaitForCommands(true);
	}

	const size_t uploadSize = (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u;
	std::vector<uint8_t> uploadPixels(uploadSize);
	if (!TryMemcpyTexturePixels(uploadPixels.data(), texBuffer.mBuffer, uploadSize))
	{
		if (mFrameBuffer != nullptr)
		{
			mFrameBuffer->Note2DTextureUploadAttempt((uint64_t)uploadSize, false);
		}
		if (sUploadFailureLogCount < 16)
		{
			Printf(TEXTCOLOR_RED "NRI texture upload skipped: invalid source buffer (lump=%d size=%dx%d translation=%d flags=0x%x image=%s content=%llu).\n",
				tex->GetSourceLump(),
				texBuffer.mWidth,
				texBuffer.mHeight,
				translation,
				flags,
				tex->GetImage() != nullptr ? "yes" : "no",
				(unsigned long long)texBuffer.mContentId);
			sUploadFailureLogCount++;
		}
		return;
	}

	CreateTextureResource((uint32_t)texBuffer.mWidth, (uint32_t)texBuffer.mHeight, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE);
	UploadTextureData(uploadPixels.data(), (uint32_t)texBuffer.mWidth, (uint32_t)texBuffer.mHeight, nri::Format::BGRA8_UNORM, (uint32_t)texBuffer.mWidth * 4u);
	mContentId = hasMetadataSignature ? signature.key : texBuffer.mContentId;
}

void NRIHardwareTexture::EnsureCanvas(FTexture* tex, nri::Format format)
{
	if (tex == nullptr)
	{
		return;
	}

	const uint32_t width = (uint32_t)tex->GetWidth();
	const uint32_t height = (uint32_t)tex->GetHeight();
	if (width == 0 || height == 0)
	{
		return;
	}

	if (mResource.texture != nullptr &&
		mResource.width == width &&
		mResource.height == height &&
		mResource.format == format &&
		mResource.colorAttachmentView != nullptr)
	{
		return;
	}

	CreateTextureResource(width, height, format, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::COLOR_ATTACHMENT);
	mContentId = 0;
}

void NRIHardwareTexture::CreateWipeTexture(int w, int h, const char*)
{
	if (w <= 0 || h <= 0)
	{
		return;
	}

	CreateTextureResource((uint32_t)w, (uint32_t)h, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::COLOR_ATTACHMENT);
	mFrameBuffer->CopyCurrentTargetToTexture(mResource);
	mContentId = 0;
}

void NRIHardwareTexture::Reset()
{
	if (mFrameBuffer != nullptr)
	{
		const uint64_t oldBytes = mResource.memorySize;
		mFrameBuffer->RetireTextureResource(mResource);
		mFrameBuffer->Note2DTextureResidentBytesChanged(oldBytes, 0);
	}

	mStagingPixels.clear();
	bufferpitch = 0;
	mContentId = 0;
}

void NRIHardwareTexture::CreateTextureResource(uint32_t width, uint32_t height, nri::Format format, nri::TextureUsageBits usage)
{
	if (mFrameBuffer == nullptr)
	{
		return;
	}

	if (mResource.texture != nullptr &&
		mResource.width == width &&
		mResource.height == height &&
		mResource.format == format &&
		((((uint32_t)usage & (uint32_t)nri::TextureUsageBits::COLOR_ATTACHMENT) == 0) || mResource.colorAttachmentView != nullptr))
	{
		return;
	}

	const bool recreated = mResource.texture != nullptr;
	const uint64_t oldBytes = mResource.memorySize;
	if (recreated)
	{
		mFrameBuffer->WaitForCommands(true);
	}
	mFrameBuffer->DestroyTextureResource(mResource);
	if (mFrameBuffer->CreateOwnedTexture(mResource, width, height, format, usage))
	{
		mFrameBuffer->Note2DTextureResourceCreate(recreated);
	}
	mFrameBuffer->Note2DTextureResidentBytesChanged(oldBytes, mResource.memorySize);
}

bool NRIHardwareTexture::UploadTextureData(const void* data, uint32_t width, uint32_t height, nri::Format format, uint32_t rowPitch)
{
	if (mFrameBuffer == nullptr || mResource.texture == nullptr || data == nullptr)
	{
		return false;
	}

	const uint64_t bytes = (uint64_t)rowPitch * (uint64_t)height;
	const bool success = mFrameBuffer->UploadTextureData(mResource, data, width, height, rowPitch);
	mFrameBuffer->Note2DTextureUploadAttempt(bytes, success);
	if (success)
	{
		mResource.format = format;
	}
	return success;
}
