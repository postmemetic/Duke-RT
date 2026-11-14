#include "nri_renderdevice.h"
#include "../renderer/nri_renderer.h"
#include "../renderer/nri_renderstate.h"
#include "printf.h"
#include "version.h"

#include <algorithm>
#include <cstring>

namespace
{
	bool GetCaptureLayout(uint32_t width, uint32_t height, uint32_t bytesPerPixel,
		const nri::DeviceDesc& device, uint32_t& rowPitch, uint32_t& slicePitch)
	{
		if (width == 0 || height == 0 || bytesPerPixel == 0)
			return false;
		auto align = [](uint64_t value, uint32_t alignment)
		{
			const uint64_t a = std::max(alignment, 1u);
			return ((value + a - 1u) / a) * a;
		};
		const uint64_t row = align((uint64_t)width * bytesPerPixel, device.memoryAlignment.uploadBufferTextureRow);
		if (row > UINT32_MAX)
			return false;
		const uint64_t slice = align(row * height, device.memoryAlignment.uploadBufferTextureSlice);
		if (slice > UINT32_MAX || (uint64_t)width * height * 3u > INT32_MAX)
			return false;
		rowPitch = (uint32_t)row;
		slicePitch = (uint32_t)slice;
		return true;
	}
}

NRICaptureColorInfo NRIRenderDevice::GetCaptureColorInfo(const NRITextureResource& source) const
{
	NRICaptureColorInfo info = {};
	info.format = source.format;
	const NRIPTOutputPolicy policy = GetPathTracingOutputPolicy();
	// Read presentation pixels, not scene radiance: active FP16 HDR targets use
	// scRGB transport. Follow that transport even for legacy debug views whose
	// shaders write normalized values; Windows displays those as linear scRGB too.
	if (policy.hdrSwapChainActive && source.format == nri::Format::RGBA16_SFLOAT)
	{
		info.encoding = NRICaptureEncoding::LinearScRGB;
		info.paperWhiteNits = GetNRIPTOutputClampedPaperWhiteNits(policy.paperWhiteNits,
			policy.displaySdrLuminance, policy.displayMaxLuminance);
		info.peakNits = std::max(GetNRIPTOutputSafeDisplayMaxLuminance(policy.displaySdrLuminance,
			policy.displayMaxLuminance), GetNRIPTHdrUiWhiteScale(policy) * 80.0f);
	}
	return info;
}

bool NRIRenderDevice::EnsureSaveTarget(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
	{
		return false;
	}

	const nri::Format format = GetSceneOutputFormat();
	if (mSaveTarget.texture != nullptr && mSaveTarget.width == width && mSaveTarget.height == height && mSaveTarget.format == format)
	{
		return true;
	}

	DestroyTextureResource(mSaveTarget);
	return CreateOwnedTexture(mSaveTarget, width, height, format, nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::COLOR_ATTACHMENT);
}

bool NRIRenderDevice::QueueScreenshot(FileWriter* file, const char* filename)
{
	if (file == nullptr)
	{
		return false;
	}

	if (!mInitialized)
	{
		return false;
	}

	PendingScreenshotCapture capture;
	capture.file.reset(file);
	capture.fileName = filename != nullptr ? filename : "";
	capture.serial = mNextScreenshotCaptureSerial++;
	capture.width = (uint32_t)GetWidth();
	capture.height = (uint32_t)GetHeight();
	Printf("NRI screenshot queued: serial=%llu path=\"%s\"\n",
		(unsigned long long)capture.serial,
		capture.fileName.GetChars());
	mPendingScreenshotCaptures.push_back(std::move(capture));
	return true;
}

TArray<uint8_t> NRIRenderDevice::GetScreenshotBuffer(int& pitch, ESSType& color_type, float& gamma)
{
	const int w = SCREENWIDTH;
	const int h = SCREENHEIGHT;

	TArray<uint8_t> buffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, buffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return buffer;
}

void NRIRenderDevice::RecordPendingScreenshotReadbacks()
{
	if (mPendingScreenshotCaptures.empty())
	{
		return;
	}

	if (!mFrameBegun || mCommandBuffer == nullptr || mCurrentPresentTarget == nullptr || mCurrentPresentTarget->texture == nullptr)
	{
		return;
	}

	NRITextureResource& source = *mCurrentPresentTarget;
	const uint32_t bytesPerPixel = NRIGetCaptureBytesPerPixel(source.format);
	if (bytesPerPixel == 0)
	{
		Printf(TEXTCOLOR_RED "NRI screenshot failed: unsupported source format %u.\n", (unsigned)source.format);
		ClearPendingScreenshotReadbacks();
		return;
	}

	mRenderState->EndFrame();
	TransitionTexture(source, NRICopySourceState());

	const nri::DeviceDesc& deviceDesc = mCore.GetDeviceDesc(*mDevice);
	for (auto& capture : mPendingScreenshotCaptures)
	{
		if (capture.readbackRecorded || capture.readbackBuffer != nullptr)
		{
			continue;
		}

		capture.width = (std::min)(capture.width, source.width);
		capture.height = (std::min)(capture.height, source.height);
		if (capture.width == 0 || capture.height == 0)
		{
			continue;
		}

		capture.colorInfo = GetCaptureColorInfo(source);
		if (!GetCaptureLayout(capture.width, capture.height, bytesPerPixel, deviceDesc, capture.rowPitch, capture.slicePitch))
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: invalid readback dimensions.\n");
			continue;
		}

		nri::BufferDesc readbackDesc = {};
		readbackDesc.size = capture.slicePitch;
		if (mCore.CreateCommittedBuffer(*mDevice, nri::MemoryLocation::HOST_READBACK, 0.0f, readbackDesc, capture.readbackBuffer) != nri::Result::SUCCESS)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: could not create readback buffer.\n");
			continue;
		}

		nri::TextureRegionDesc region = {};
		region.width = capture.width;
		region.height = capture.height;
		region.depth = 1;
		region.planes = nri::PlaneBits::COLOR;

		nri::TextureDataLayoutDesc layout = {};
		layout.rowPitch = capture.rowPitch;
		layout.slicePitch = capture.slicePitch;

		mCore.CmdReadbackTextureToBuffer(*mCommandBuffer, *capture.readbackBuffer, layout, *source.texture, region);
		capture.readbackRecorded = true;
		capture.shellFrameIndex = mFrameIndex;
		capture.rendererFrameIndex = mRenderer != nullptr ? mRenderer->GetLastCompletedFrameIndex() : ~0u;
		Printf("NRI screenshot frame: serial=%llu renderer_frame=%u shell_frame=%llu path=\"%s\"\n",
			(unsigned long long)capture.serial,
			capture.rendererFrameIndex,
			(unsigned long long)capture.shellFrameIndex,
			capture.fileName.GetChars());
	}
}

void NRIRenderDevice::FinishPendingScreenshotReadbacks(bool submitted, uint64_t submittedFenceValue)
{
	if (mPendingScreenshotCaptures.empty())
	{
		return;
	}

	if (!submitted || mFrameFence == nullptr)
	{
		Printf(TEXTCOLOR_RED "NRI screenshot failed: frame submission did not complete.\n");
		ClearPendingScreenshotReadbacks();
		return;
	}

	if (!WaitForFenceValue(*mFrameFence, submittedFenceValue, "Wait(ScreenshotReadback)"))
	{
		Printf(TEXTCOLOR_RED "NRI screenshot failed: frame-fence wait did not complete.\n");
		ClearPendingScreenshotReadbacks();
		return;
	}

	for (auto& capture : mPendingScreenshotCaptures)
	{
		if (!capture.readbackRecorded || capture.readbackBuffer == nullptr || capture.file == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: readback was not recorded.\n");
			continue;
		}

		const uint8_t* pixels = (const uint8_t*)mCore.MapBuffer(*capture.readbackBuffer, 0, capture.slicePitch);
		if (pixels == nullptr)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: could not map readback buffer.\n");
			continue;
		}

		TArray<uint8_t> image(capture.width * capture.height * 3, true);
		const bool converted = NRIConvertCaptureToSdr(pixels, capture.slicePitch,
			capture.width, capture.height, capture.rowPitch, capture.colorInfo,
			image.Data(), image.Size(), (size_t)capture.width * 3u);

		mCore.UnmapBuffer(*capture.readbackBuffer);

		if (!converted)
		{
			Printf(TEXTCOLOR_RED "NRI screenshot failed: invalid pixel format or color encoding.\n");
			continue;
		}

		FStringf software(GAMENAME " %s", GetVersionString());
		if (!M_CreatePNG(capture.file.get(), image.Data(), nullptr, SS_RGB, capture.width, capture.height, capture.width * 3, 1.0f, true) ||
			!M_AppendPNGText(capture.file.get(), "Software", software.GetChars()) ||
			!M_FinishPNG(capture.file.get()))
		{
			Printf("Failed writing screenshot\n");
		}
		else
		{
			Printf("NRI screenshot completed: serial=%llu renderer_frame=%u shell_frame=%llu path=\"%s\"\n",
				(unsigned long long)capture.serial,
				capture.rendererFrameIndex,
				(unsigned long long)capture.shellFrameIndex,
				capture.fileName.GetChars());
			Printf("screenshot saved\n");
		}
	}

	ClearPendingScreenshotReadbacks();
}

void NRIRenderDevice::ClearPendingScreenshotReadbacks()
{
	for (auto& capture : mPendingScreenshotCaptures)
	{
		if (capture.readbackBuffer != nullptr && mDevice != nullptr)
		{
			mCore.DestroyBuffer(capture.readbackBuffer);
			capture.readbackBuffer = nullptr;
		}
	}
	mPendingScreenshotCaptures.clear();
}

void NRIRenderDevice::CopyScreenToBuffer(int width, int height, uint8_t* buffer)
{
	if (buffer == nullptr || width <= 0 || height <= 0)
		return;

	const size_t outputSize = (size_t)width * height * 3u;
	memset(buffer, 0, outputSize);
	NRITextureResource* source = mActiveTarget != nullptr ? mActiveTarget : mCurrentPresentTarget;
	if (source == nullptr || source->texture == nullptr || (uint32_t)width > source->width || (uint32_t)height > source->height)
		return;

	const NRICaptureColorInfo colorInfo = GetCaptureColorInfo(*source);
	uint32_t rowPitch = 0, slicePitch = 0;
	if (!GetCaptureLayout((uint32_t)width, (uint32_t)height, NRIGetCaptureBytesPerPixel(source->format),
		mCore.GetDeviceDesc(*mDevice), rowPitch, slicePitch))
		return;

	const bool useStandaloneSavePicFrame = mStandaloneSavePicFrame && mFrameBegun && mCommandBuffer != nullptr && mCommandBufferOpen;
	if (mFrameBegun)
		mRenderState->EndFrame();
	if (!useStandaloneSavePicFrame && !BeginCommandList("CopyScreenToBuffer", !mFrameBegun))
		return;

	TransitionTexture(*source, NRICopySourceState());
	nri::BufferDesc readbackDesc = {};
	readbackDesc.size = slicePitch;
	nri::Buffer* readbackBuffer = nullptr;
	if (mCore.CreateCommittedBuffer(*mDevice, nri::MemoryLocation::HOST_READBACK, 0.0f, readbackDesc, readbackBuffer) != nri::Result::SUCCESS)
		return;

	nri::TextureRegionDesc region = {};
	region.width = (uint32_t)width;
	region.height = (uint32_t)height;
	region.depth = 1;
	region.planes = nri::PlaneBits::COLOR;
	nri::TextureDataLayoutDesc layout = {};
	layout.rowPitch = rowPitch;
	layout.slicePitch = slicePitch;
	mCore.CmdReadbackTextureToBuffer(*mCommandBuffer, *readbackBuffer, layout, *source->texture, region);
	if (!SubmitAndWaitCurrentCommandBuffer())
	{
		mCore.DestroyBuffer(readbackBuffer);
		return;
	}

	const void* pixels = mCore.MapBuffer(*readbackBuffer, 0, slicePitch);
	if (pixels != nullptr)
	{
		if (!NRIConvertCaptureToSdr(pixels, slicePitch, (uint32_t)width, (uint32_t)height,
			rowPitch, colorInfo, buffer, outputSize, (size_t)width * 3u))
			Printf(TEXTCOLOR_RED "NRI save picture failed: invalid pixel format or color encoding.\n");
		mCore.UnmapBuffer(*readbackBuffer);
	}
	else
		Printf(TEXTCOLOR_RED "NRI save picture failed: could not map readback buffer.\n");
	mCore.DestroyBuffer(readbackBuffer);
}
