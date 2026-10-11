#include "texturemanager.h"
#include "nri_renderer.h"
#include "nri_cvars.h"

#include "../scene/nri_hash.h"
#include "../scene/nri_scene_math.h"
#include "../system/nri_renderdevice.h"
#include "c_cvars.h"
#include "image.h"
#include "mapinfo.h"
#include "printf.h"
#include "skyboxtexture.h"
#include "texturemanager.h"
#include "textures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <windows.h>
#endif


RendererSkyPerfTraceStats gRendererSkyPerfTraceStats = {};

bool ShouldTraceSkyPerf()
{
	return !!nri_pttemporaltrace && nri_pttraceframes > 0;
}

bool ShouldEmitRendererTemporalTraceLogs()
{
	return !!nri_pttemporaltrace && nri_pttraceframes > 0;
}

void ResetRendererSkyPerfTraceStats()
{
	gRendererSkyPerfTraceStats = {};
}

const char* GetSkyModeName(nri_scene::PTSkyMode mode)
{
	switch (mode)
	{
	case nri_scene::PTSkyMode::None:
		return "none";
	case nri_scene::PTSkyMode::SolidColor:
		return "solid";
	case nri_scene::PTSkyMode::Cubemap:
		return "cubemap";
	default:
		return "unknown";
	}
}

const char* GetSkySourceTypeName(nri_scene::PTSkySourceType sourceType)
{
	switch (sourceType)
	{
	case nri_scene::PTSkySourceType::None:
		return "none";
	case nri_scene::PTSkySourceType::Wall:
		return "wall";
	case nri_scene::PTSkySourceType::Flat:
		return "flat";
	case nri_scene::PTSkySourceType::Portal:
		return "portal";
	default:
		return "unknown";
	}
}

float GetSkyBrightnessMultiplier()
{
	return std::clamp((float)nri_ptskybrightness, 0.0f, 4.0f);
}

bool SkyBrightnessMatches(float a, float b)
{
	return std::fabs(a - b) <= 0.0001f;
}

ScopedSkyPerfTimer::ScopedSkyPerfTimer(uint64_t& targetUs)
	: mTarget(ShouldTraceSkyPerf() ? &targetUs : nullptr)
{
	if (mTarget != nullptr)
	{
		mStart = std::chrono::steady_clock::now();
	}
}

ScopedSkyPerfTimer::~ScopedSkyPerfTimer()
{
	if (mTarget != nullptr)
	{
		const auto elapsed = std::chrono::steady_clock::now() - mStart;
		*mTarget += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
	}
}

namespace
{
	static float ClampSky01(float value)
	{
		return std::clamp(value, 0.0f, 1.0f);
	}

	static uint64_t HashBytes64(const uint8_t* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= (uint64_t)data[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static uint64_t HashSkyBrightness(uint64_t hash)
	{
		return nri_scene::HashCombine64(hash, (uint64_t)FloatBits(GetSkyBrightnessMultiplier()));
	}

	struct SkyFaceUpload
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	struct SkyFaceProbe
	{
		FGameTexture* texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t contentId = 0;
	};

	struct SkyProbe
	{
		uint64_t key = 0;
		uint32_t width = 1;
		uint32_t height = 1;
		std::array<SkyFaceProbe, 6> faces = {};
	};

	struct PanoramaSkyProbe
	{
		FGameTexture* texture = nullptr;
		uint32_t width = 1;
		uint32_t height = 1;
		uint64_t key = 0;
	};

	struct SkyUpload
	{
		uint64_t key = 0;
		bool cubemap = false;
		uint32_t width = 1;
		uint32_t height = 1;
		std::array<SkyFaceUpload, 6> faces = {};
	};

	static bool IsUsableGameTexturePointer(FGameTexture* texture)
	{
#ifdef _WIN32
		const intptr_t value = (intptr_t)texture;
		return value > 0x10000 && value != -1;
#else
		return TexMan.OwnsTexture(texture);
#endif
	}

	static FTexture* TryGetBaseTexture(FGameTexture* texture)
	{
		if (!IsUsableGameTexturePointer(texture))
		{
			return nullptr;
		}

		FTexture* baseTexture = nullptr;
#ifdef _WIN32
		__try
#endif
		{
			baseTexture = texture->GetTexture();
		}
#ifdef _WIN32
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			baseTexture = nullptr;
		}
#endif

		return baseTexture;
	}

	static FGameTexture* TryGetSkyFace(FSkyBox* skybox, int index)
	{
		if (skybox == nullptr || index < 0 || index >= 6)
		{
			return nullptr;
		}

		FGameTexture* face = nullptr;
#ifdef _WIN32
		__try
#endif
		{
			face = skybox->GetSkyFace(index);
		}
#ifdef _WIN32
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			face = nullptr;
		}
#endif

		return IsUsableGameTexturePointer(face) ? face : nullptr;
	}

	static uint64_t HashSkyColor(const float* color)
	{
		const uint8_t rgba[4] = {
			(uint8_t)std::clamp((int)std::lround(ClampSky01(color[0]) * 255.0f), 0, 255),
			(uint8_t)std::clamp((int)std::lround(ClampSky01(color[1]) * 255.0f), 0, 255),
			(uint8_t)std::clamp((int)std::lround(ClampSky01(color[2]) * 255.0f), 0, 255),
			255
		};
		return HashBytes64(rgba, sizeof(rgba));
	}

	static void ScaleSkyUploadBrightness(SkyUpload& upload)
	{
		const float brightness = GetSkyBrightnessMultiplier();
		if (SkyBrightnessMatches(brightness, 1.0f))
		{
			return;
		}

		for (SkyFaceUpload& face : upload.faces)
		{
			for (size_t i = 0; i + 3 < face.pixels.size(); i += 4)
			{
				face.pixels[i + 0] = (uint8_t)std::clamp((int)std::lround((float)face.pixels[i + 0] * brightness), 0, 255);
				face.pixels[i + 1] = (uint8_t)std::clamp((int)std::lround((float)face.pixels[i + 1] * brightness), 0, 255);
				face.pixels[i + 2] = (uint8_t)std::clamp((int)std::lround((float)face.pixels[i + 2] * brightness), 0, 255);
			}
		}
	}

	static void FlipImageHorizontal(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
	{
		for (uint32_t y = 0; y < height; ++y)
		{
			uint8_t* row = pixels.data() + (size_t)y * width * 4u;
			for (uint32_t x = 0; x < width / 2; ++x)
			{
				uint8_t* a = row + x * 4u;
				uint8_t* b = row + (width - 1 - x) * 4u;
				for (uint32_t c = 0; c < 4; ++c)
				{
					std::swap(a[c], b[c]);
				}
			}
		}
	}

	static void FlipImageVertical(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
	{
		const size_t rowSize = (size_t)width * 4u;
		std::vector<uint8_t> temp(rowSize);
		for (uint32_t y = 0; y < height / 2; ++y)
		{
			uint8_t* a = pixels.data() + (size_t)y * rowSize;
			uint8_t* b = pixels.data() + (size_t)(height - 1 - y) * rowSize;
			std::memcpy(temp.data(), a, rowSize);
			std::memcpy(a, b, rowSize);
			std::memcpy(b, temp.data(), rowSize);
		}
	}

	static bool CopyFacePixels(FGameTexture* texture, SkyFaceUpload& outFace)
	{
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		FTextureBuffer texBuffer = baseTexture->CreateTexBuffer(0, CTF_ProcessData);
		if (texBuffer.mBuffer == nullptr || texBuffer.mWidth <= 0 || texBuffer.mHeight <= 0)
		{
			return false;
		}

		outFace.width = (uint32_t)texBuffer.mWidth;
		outFace.height = (uint32_t)texBuffer.mHeight;
		outFace.pixels.assign(texBuffer.mBuffer, texBuffer.mBuffer + (size_t)texBuffer.mWidth * (size_t)texBuffer.mHeight * 4u);
		return true;
	}

	static bool ProbeFace(FGameTexture* texture, SkyFaceProbe& outFace)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeFaceCalls++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.probeFaceTimeUs);
		FTexture* baseTexture = TryGetBaseTexture(texture);
		if (baseTexture == nullptr || baseTexture->GetImage() == nullptr)
		{
			return false;
		}

		const int width = baseTexture->GetWidth();
		const int height = baseTexture->GetHeight();
		if (width <= 0 || height <= 0)
		{
			return false;
		}

		FContentIdBuilder contentId = {};
		contentId.imageID = baseTexture->GetImage()->GetId();
		contentId.translation = 0;
		contentId.expand = 0;
		contentId.scaler = 0;
		contentId.scalefactor = 0;

		outFace.texture = texture;
		outFace.width = (uint32_t)width;
		outFace.height = (uint32_t)height;
		outFace.contentId = contentId.id != 0 ? contentId.id : (uint64_t)(uintptr_t)texture;
		return true;
	}

	static bool ProbeCubemapSky(const nri_scene::SceneView& sceneView, SkyProbe& outProbe)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeAttempts++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.probeCubemapTimeUs);
		if (sceneView.sky.mode != nri_scene::PTSkyMode::Cubemap || !IsUsableGameTexturePointer(sceneView.sky.texture))
		{
			return false;
		}

		auto* skybox = dynamic_cast<FSkyBox*>(TryGetBaseTexture(sceneView.sky.texture));
		if (skybox == nullptr)
		{
			return false;
		}

		struct FaceMapping
		{
			int sourceIndex;
			bool flipHorizontal;
			bool flipVertical;
		};

		// Build sky faces are ordered north, east, south, west, top, bottom.
		// The PT cubemap follows the conventional +X, -X, +Y, -Y, +Z, -Z order.
		// Top and bottom need explicit flips to match the ray-space basis used by the PT shaders.
		static const FaceMapping mappings[6] = {
			{ 3, false, false }, // +X = west
			{ 1, false, false }, // -X = east
			{ 4, true, false },  // +Y = top
			{ 5, true, true },   // -Y = bottom
			{ 2, false, false }, // +Z = south
			{ 0, false, false }  // -Z = north
		};

		uint64_t key = nri_scene::HashCombine64(1469598103934665603ull, (uint64_t)(uintptr_t)sceneView.sky.texture);
		key = nri_scene::HashCombine64(key, (uint64_t)sceneView.sky.faceMask);
		key = nri_scene::HashCombine64(key, sceneView.sky.flipTop ? 1ull : 0ull);
		for (uint32_t i = 0; i < 6; ++i)
		{
			if (!ProbeFace(TryGetSkyFace(skybox, mappings[i].sourceIndex), outProbe.faces[i]))
			{
				return false;
			}

			key = nri_scene::HashCombine64(key, (uint64_t)(uintptr_t)outProbe.faces[i].texture);
			key = nri_scene::HashCombine64(key, outProbe.faces[i].contentId);
			key = nri_scene::HashCombine64(key, ((uint64_t)outProbe.faces[i].width << 32) | outProbe.faces[i].height);
		}
		key = HashSkyBrightness(key);

		outProbe.width = outProbe.faces[0].width;
		outProbe.height = outProbe.faces[0].height;
		for (uint32_t i = 1; i < 6; ++i)
		{
			if (outProbe.faces[i].width != outProbe.width || outProbe.faces[i].height != outProbe.height)
			{
				return false;
			}
		}

		outProbe.key = key;
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.probeSuccesses++;
		}
		return true;
	}

	static bool BuildCubemapUpload(const nri_scene::SceneView& sceneView, const SkyProbe& probe, SkyUpload& outUpload)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.buildCubemapUploadCalls++;
		}
		ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.buildCubemapUploadTimeUs);
		struct FaceMapping
		{
			bool flipHorizontal;
			bool flipVertical;
		};

		// Build sky faces are ordered north, east, south, west, top, bottom.
		// The PT cubemap follows the conventional +X, -X, +Y, -Y, +Z, -Z order.
		// Top and bottom need explicit flips to match the ray-space basis used by the PT shaders.
		static const FaceMapping mappings[6] = {
			{ false, false }, // +X = west
			{ false, false }, // -X = east
			{ true, false },  // +Y = top
			{ true, true },   // -Y = bottom
			{ false, false }, // +Z = south
			{ false, false }  // -Z = north
		};

		for (uint32_t i = 0; i < 6; ++i)
		{
			if (!CopyFacePixels(probe.faces[i].texture, outUpload.faces[i]))
			{
				return false;
			}

			if (i == 2 && sceneView.sky.flipTop)
			{
				FlipImageVertical(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
			if (mappings[i].flipHorizontal)
			{
				FlipImageHorizontal(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
			if (mappings[i].flipVertical)
			{
				FlipImageVertical(outUpload.faces[i].pixels, outUpload.faces[i].width, outUpload.faces[i].height);
			}
		}

		outUpload.key = probe.key;
		outUpload.width = probe.width;
		outUpload.height = probe.height;
		outUpload.cubemap = true;
		return true;
	}

	static bool ProbePanoramaSky(const nri_scene::SceneView& sceneView, PanoramaSkyProbe& outProbe)
	{
		if (sceneView.sky.mode != nri_scene::PTSkyMode::SolidColor || !IsUsableGameTexturePointer(sceneView.sky.texture))
		{
			return false;
		}

		auto* skybox = dynamic_cast<FSkyBox*>(TryGetBaseTexture(sceneView.sky.texture));
		if (skybox != nullptr)
		{
			return false;
		}

		SkyFaceProbe textureProbe = {};
		if (!ProbeFace(sceneView.sky.texture, textureProbe))
		{
			return false;
		}

		uint64_t key = nri_scene::HashCombine64(1469598103934665603ull, 0x50414e4f534b5955ull);
		key = nri_scene::HashCombine64(key, (uint64_t)(uintptr_t)sceneView.sky.texture);
		key = nri_scene::HashCombine64(key, textureProbe.contentId);
		key = nri_scene::HashCombine64(key, ((uint64_t)textureProbe.width << 32) | textureProbe.height);
		key = HashSkyBrightness(key);
		outProbe.texture = sceneView.sky.texture;
		outProbe.width = textureProbe.width;
		outProbe.height = textureProbe.height;
		outProbe.key = key;
		return true;
	}

	static uint32_t GetPanoramaSkyFaceSize(uint32_t sourceHeight)
	{
		return std::clamp(sourceHeight, 64u, 1024u);
	}

	struct PanoramaDirection
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 1.0f;
	};

	static PanoramaDirection NormalizePanoramaDirection(PanoramaDirection dir)
	{
		const float lengthSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
		if (lengthSq <= 0.000001f)
		{
			return {};
		}
		const float invLength = 1.0f / std::sqrt(lengthSq);
		dir.x *= invLength;
		dir.y *= invLength;
		dir.z *= invLength;
		return dir;
	}

	static PanoramaDirection DirectionForPanoramaSkyFace(uint32_t faceIndex, float x, float y)
	{
		switch (faceIndex)
		{
		case 0:
			return NormalizePanoramaDirection({ 1.0f, -y, -x });
		case 1:
			return NormalizePanoramaDirection({ -1.0f, -y, x });
		case 2:
			return NormalizePanoramaDirection({ x, 1.0f, y });
		case 3:
			return NormalizePanoramaDirection({ x, -1.0f, -y });
		case 4:
			return NormalizePanoramaDirection({ x, -y, 1.0f });
		default:
			return NormalizePanoramaDirection({ -x, -y, -1.0f });
		}
	}

	static void SamplePanoramaSkyPixel(const SkyFaceUpload& source, float u, float v, uint8_t* outPixel)
	{
		if (source.pixels.empty() || source.width == 0 || source.height == 0)
		{
			outPixel[0] = outPixel[1] = outPixel[2] = 0;
			outPixel[3] = 255;
			return;
		}

		u = u - std::floor(u);
		v = std::clamp(v, 0.0f, 1.0f);
		const uint32_t x = std::min<uint32_t>((uint32_t)std::floor(u * (float)source.width), source.width - 1u);
		const uint32_t y = std::min<uint32_t>((uint32_t)std::floor(v * (float)source.height), source.height - 1u);
		const uint8_t* src = source.pixels.data() + ((size_t)y * source.width + x) * 4u;
		outPixel[0] = src[0];
		outPixel[1] = src[1];
		outPixel[2] = src[2];
		outPixel[3] = src[3];
	}

	static bool BuildPanoramaSkyUpload(const PanoramaSkyProbe& probe, SkyUpload& outUpload)
	{
		SkyFaceUpload source = {};
		if (!CopyFacePixels(probe.texture, source))
		{
			return false;
		}

		const uint32_t faceSize = GetPanoramaSkyFaceSize(source.height);
		outUpload = {};
		outUpload.key = probe.key;
		outUpload.width = faceSize;
		outUpload.height = faceSize;
		outUpload.cubemap = true;

		constexpr float kInvPi = 0.31830988618f;
		constexpr float kInvTwoPi = 0.15915494309f;
		for (uint32_t face = 0; face < 6; ++face)
		{
			SkyFaceUpload& outFace = outUpload.faces[face];
			outFace.width = faceSize;
			outFace.height = faceSize;
			outFace.pixels.resize((size_t)faceSize * faceSize * 4u);
			for (uint32_t y = 0; y < faceSize; ++y)
			{
				for (uint32_t x = 0; x < faceSize; ++x)
				{
					const float fx = (((float)x + 0.5f) / (float)faceSize) * 2.0f - 1.0f;
					const float fy = (((float)y + 0.5f) / (float)faceSize) * 2.0f - 1.0f;
					const PanoramaDirection dir = DirectionForPanoramaSkyFace(face, fx, fy);
					const float u = 0.5f + std::atan2(dir.x, dir.z) * kInvTwoPi;
					const float v = 0.5f - std::asin(std::clamp(dir.y, -1.0f, 1.0f)) * kInvPi;
					SamplePanoramaSkyPixel(source, u, v, outFace.pixels.data() + ((size_t)y * faceSize + x) * 4u);
				}
			}
		}

		return true;
	}

	static void BuildSolidSkyUpload(const float* skyColor, SkyUpload& outUpload)
	{
		outUpload = {};
		outUpload.key = HashSkyBrightness(HashSkyColor(skyColor) ^ 0x53594b59554c4c45ull);
		for (auto& face : outUpload.faces)
		{
			face.width = 1;
			face.height = 1;
			face.pixels = {
				(uint8_t)std::clamp((int)std::lround(ClampSky01(skyColor[2]) * 255.0f), 0, 255),
				(uint8_t)std::clamp((int)std::lround(ClampSky01(skyColor[1]) * 255.0f), 0, 255),
				(uint8_t)std::clamp((int)std::lround(ClampSky01(skyColor[0]) * 255.0f), 0, 255),
				255
			};
		}
	}
}

bool NRIRenderer::EnsureSkyTexture(const nri_scene::SceneView& sceneView, bool preserveExistingSky)
{
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.ensureSkyCalls++;
	}
	ScopedSkyPerfTimer timer(gRendererSkyPerfTraceStats.ensureSkyTimeUs);
	if (mSkyEnvironment.Level() != currentLevel)
	{
		mSkyEnvironment.ResetActiveForLevel(currentLevel);
	}

	const float skyBrightness = GetSkyBrightnessMultiplier();

	auto findCachedSkyTexture = [this](uint64_t key, uint32_t width, uint32_t height) -> uint32_t
	{
		return mSkyEnvironment.FindCachedTexture(key, width, height);
	};

	auto activateCachedSky = [this, skyBrightness](uint32_t index, uint64_t key, const nri_scene::SceneView& sourceView, nri_scene::PTSkyMode mode)
	{
		mSkyEnvironment.Activate(index, key, sourceView, mode, skyBrightness);
	};

	auto createCachedSky = [this, &findCachedSkyTexture](SkyUpload upload, nri_scene::PTSkyMode mode) -> uint32_t
	{
		const uint32_t existing = findCachedSkyTexture(upload.key, upload.width, upload.height);
		if (existing != UINT32_MAX)
		{
			return existing;
		}

		ScaleSkyUploadBrightness(upload);

		NRICachedSkyTexture cacheEntry = {};
		cacheEntry.key = upload.key;
		cacheEntry.mode = mode;
		if (!mFrameBuffer->CreateOwnedTexture(cacheEntry.resource, upload.width, upload.height, nri::Format::BGRA8_UNORM, nri::TextureUsageBits::SHADER_RESOURCE, nri::TextureType::TEXTURE_2D, 6, nri::TextureView::TEXTURE_CUBE))
		{
			return UINT32_MAX;
		}

		std::array<nri::TextureSubresourceUploadDesc, 6> subresources = {};
		for (uint32_t i = 0; i < 6; ++i)
		{
			subresources[i].slices = upload.faces[i].pixels.data();
			subresources[i].sliceNum = 1;
			subresources[i].rowPitch = upload.faces[i].width * 4u;
			subresources[i].slicePitch = upload.faces[i].width * upload.faces[i].height * 4u;
		}

		if (!mFrameBuffer->UploadTextureSubresources(cacheEntry.resource, subresources.data(), (uint32_t)subresources.size(), upload.width, upload.height))
		{
			mFrameBuffer->DestroyTextureResource(cacheEntry.resource);
			return UINT32_MAX;
		}

		return mSkyEnvironment.AddCachedTexture(std::move(cacheEntry));
	};

	const NRITextureResource* activeSkyTexture = GetActiveSkyTexture();
	if (preserveExistingSky && activeSkyTexture != nullptr && SkyBrightnessMatches(mSkyEnvironment.ActiveState().brightness, skyBrightness))
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.preserveExistingHits++;
		}
		TraceSkyState(sceneView, "preserve-existing", mSkyEnvironment.ActiveKey());
		return true;
	}

	if (sceneView.sky.mode == nri_scene::PTSkyMode::Cubemap &&
		activeSkyTexture != nullptr &&
		mSkyEnvironment.ActiveState().mode == nri_scene::PTSkyMode::Cubemap &&
		mSkyEnvironment.ActiveState().texture == sceneView.sky.texture &&
		mSkyEnvironment.ActiveState().faceMask == sceneView.sky.faceMask &&
		SkyBrightnessMatches(mSkyEnvironment.ActiveState().brightness, skyBrightness) &&
		mSkyEnvironment.ActiveState().flipTop == sceneView.sky.flipTop)
	{
		mSkyEnvironment.Level() = currentLevel;
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.reuseActiveCubemapHits++;
		}
		TraceSkyState(sceneView, "reuse-active-cubemap", mSkyEnvironment.ActiveKey());
		return true;
	}

	SkyProbe probe = {};
	if (ProbeCubemapSky(sceneView, probe))
	{
		if (activeSkyTexture != nullptr &&
			mSkyEnvironment.ActiveKey() == probe.key &&
			activeSkyTexture->width == probe.width &&
			activeSkyTexture->height == probe.height)
		{
			mSkyEnvironment.Level() = currentLevel;
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.reuseActiveProbeHits++;
			}
			TraceSkyState(sceneView, "reuse-active-probe", probe.key);
			return true;
		}

		const uint32_t cachedIndex = findCachedSkyTexture(probe.key, probe.width, probe.height);
		if (cachedIndex != UINT32_MAX)
		{
			activateCachedSky(cachedIndex, probe.key, sceneView, nri_scene::PTSkyMode::Cubemap);
			mSkyEnvironment.Level() = currentLevel;
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.activateCachedCubemapHits++;
			}
			TraceSkyState(sceneView, "activate-cached-cubemap", probe.key);
			return true;
		}

		SkyUpload upload = {};
		if (!BuildCubemapUpload(sceneView, probe, upload))
		{
			return false;
		}

		const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::Cubemap);
		if (createdIndex == UINT32_MAX)
		{
			return false;
		}

		activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::Cubemap);
		mSkyEnvironment.Level() = currentLevel;
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.createCachedCubemapHits++;
		}
		TraceSkyState(sceneView, "create-cached-cubemap", upload.key);
		return true;
	}

	const bool shouldKeepLastCubemap =
		activeSkyTexture != nullptr &&
		mSkyEnvironment.ActiveState().mode == nri_scene::PTSkyMode::Cubemap &&
		SkyBrightnessMatches(mSkyEnvironment.ActiveState().brightness, skyBrightness) &&
		(sceneView.sky.mode == nri_scene::PTSkyMode::None ||
			sceneView.sky.texture == mSkyEnvironment.ActiveState().texture ||
			(sceneView.sky.texture == nullptr && sceneView.stats.skySurfaces > 0) ||
			(mSkyEnvironment.Level() == currentLevel &&
				sceneView.sky.mode == nri_scene::PTSkyMode::SolidColor &&
				sceneView.sky.sourceType != nri_scene::PTSkySourceType::Portal &&
				sceneView.stats.skySurfaces > 0));
	if (shouldKeepLastCubemap)
	{
		if (sceneView.sky.mode == nri_scene::PTSkyMode::SolidColor &&
			sceneView.sky.sourceType != nri_scene::PTSkySourceType::Portal)
		{
			if (ShouldTraceSkyPerf())
			{
				gRendererSkyPerfTraceStats.holdLevelCubemapHits++;
			}
			TraceSkyState(sceneView, "hold-level-cubemap", mSkyEnvironment.ActiveKey());
			return true;
		}

		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.keepLastCubemapHits++;
		}
		TraceSkyState(sceneView, "keep-last-cubemap", mSkyEnvironment.ActiveKey());
		return true;
	}

	PanoramaSkyProbe panoramaProbe = {};
	if (ProbePanoramaSky(sceneView, panoramaProbe))
	{
		const uint32_t panoramaFaceSize = GetPanoramaSkyFaceSize(panoramaProbe.height);
		const uint32_t cachedIndex = findCachedSkyTexture(panoramaProbe.key, panoramaFaceSize, panoramaFaceSize);
		if (cachedIndex != UINT32_MAX)
		{
			activateCachedSky(cachedIndex, panoramaProbe.key, sceneView, nri_scene::PTSkyMode::Cubemap);
			mSkyEnvironment.Level() = currentLevel;
			TraceSkyState(sceneView, "activate-cached-panorama", panoramaProbe.key);
			return true;
		}

		SkyUpload upload = {};
		if (BuildPanoramaSkyUpload(panoramaProbe, upload))
		{
			const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::Cubemap);
			if (createdIndex != UINT32_MAX)
			{
				activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::Cubemap);
				mSkyEnvironment.Level() = currentLevel;
				TraceSkyState(sceneView, "create-cached-panorama", upload.key);
				return true;
			}
		}
	}

	SkyUpload upload = {};
	BuildSolidSkyUpload(sceneView.skyColor, upload);
	if (activeSkyTexture != nullptr &&
		mSkyEnvironment.ActiveKey() == upload.key &&
		activeSkyTexture->width == upload.width &&
		activeSkyTexture->height == upload.height)
	{
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.solidReuseHits++;
		}
		TraceSkyState(sceneView, "reuse-active-solid", upload.key);
		return true;
	}

	const uint32_t cachedIndex = findCachedSkyTexture(upload.key, upload.width, upload.height);
	if (cachedIndex != UINT32_MAX)
	{
		activateCachedSky(cachedIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
		if (ShouldTraceSkyPerf())
		{
			gRendererSkyPerfTraceStats.solidActivateHits++;
		}
		TraceSkyState(sceneView, "activate-cached-solid", upload.key);
		return true;
	}

	const uint32_t createdIndex = createCachedSky(upload, nri_scene::PTSkyMode::SolidColor);
	if (createdIndex == UINT32_MAX)
	{
		return false;
	}

	activateCachedSky(createdIndex, upload.key, sceneView, nri_scene::PTSkyMode::SolidColor);
	if (ShouldTraceSkyPerf())
	{
		gRendererSkyPerfTraceStats.solidCreateHits++;
	}
	TraceSkyState(sceneView, "create-cached-solid", upload.key);
	return true;
}

void NRIRenderer::TraceSkyState(const nri_scene::SceneView& sceneView, const char* action, uint64_t resolvedKey)
{
	if (!ShouldEmitRendererTemporalTraceLogs())
	{
		return;
	}

	const NRISkyState tracedState = {
		sceneView.sky.mode,
		sceneView.sky.sourceType,
		sceneView.sky.texture,
		sceneView.sky.faceMask,
		GetSkyBrightnessMultiplier(),
		sceneView.sky.flipTop
	};

	const bool changed =
		!mSkyEnvironment.HasTracedState() ||
		mSkyEnvironment.LastTracedState().mode != tracedState.mode ||
		mSkyEnvironment.LastTracedState().sourceType != tracedState.sourceType ||
		mSkyEnvironment.LastTracedState().texture != tracedState.texture ||
		mSkyEnvironment.LastTracedState().faceMask != tracedState.faceMask ||
		!SkyBrightnessMatches(mSkyEnvironment.LastTracedState().brightness, tracedState.brightness) ||
		mSkyEnvironment.LastTracedState().flipTop != tracedState.flipTop ||
		mSkyEnvironment.LastTracedResolvedKey() != resolvedKey;

	if (!changed && action == nullptr)
	{
		return;
	}

	const NRITextureResource* activeSkyTexture = GetActiveSkyTexture();
	Printf("NRI PT sky: captured_mode=%s source=%s texture=%p face_mask=0x%x flip_top=%s skies=%u color=(%.3f, %.3f, %.3f) sky_brightness=%.3f action=%s resolved_key=0x%llx active_mode=%s active_key=0x%llx active_size=%ux%u\n",
		GetSkyModeName(sceneView.sky.mode),
		GetSkySourceTypeName(sceneView.sky.sourceType),
		sceneView.sky.texture,
		sceneView.sky.faceMask,
		sceneView.sky.flipTop ? "true" : "false",
		sceneView.stats.skySurfaces,
		sceneView.skyColor[0],
		sceneView.skyColor[1],
		sceneView.skyColor[2],
		tracedState.brightness,
		action != nullptr ? action : "unchanged",
		(unsigned long long)resolvedKey,
		GetSkyModeName(mSkyEnvironment.ActiveState().mode),
		(unsigned long long)mSkyEnvironment.ActiveKey(),
		activeSkyTexture != nullptr ? activeSkyTexture->width : 0,
		activeSkyTexture != nullptr ? activeSkyTexture->height : 0);

	mSkyEnvironment.LastTracedState() = tracedState;
	mSkyEnvironment.LastTracedResolvedKey() = resolvedKey;
	mSkyEnvironment.HasTracedState() = true;
}
NRITextureResource* NRISkyEnvironment::ActiveTexture()
{
	return mActiveIndex < mCache.size() ? &mCache[mActiveIndex].resource : nullptr;
}

const NRITextureResource* NRISkyEnvironment::ActiveTexture() const
{
	return mActiveIndex < mCache.size() ? &mCache[mActiveIndex].resource : nullptr;
}

uint32_t NRISkyEnvironment::FindCachedTexture(uint64_t key, uint32_t width, uint32_t height) const
{
	for (uint32_t i = 0; i < (uint32_t)mCache.size(); ++i)
	{
		const NRICachedSkyTexture& cached = mCache[i];
		if (cached.key == key &&
			cached.resource.width == width &&
			cached.resource.height == height)
		{
			return i;
		}
	}

	return UINT32_MAX;
}

uint32_t NRISkyEnvironment::AddCachedTexture(NRICachedSkyTexture&& texture)
{
	mCache.push_back(std::move(texture));
	return (uint32_t)mCache.size() - 1;
}

void NRISkyEnvironment::Activate(uint32_t index, uint64_t key, const nri_scene::SceneView& sourceView, nri_scene::PTSkyMode mode, float brightness)
{
	mActiveIndex = index;
	mActiveKey = key;
	mActiveState.mode = mode;
	mActiveState.sourceType = sourceView.sky.sourceType;
	mActiveState.texture = sourceView.sky.texture;
	mActiveState.faceMask = sourceView.sky.faceMask;
	mActiveState.brightness = brightness;
	mActiveState.flipTop = sourceView.sky.flipTop;
}

void NRISkyEnvironment::ResetActiveForLevel(MapRecord* level)
{
	mActiveIndex = UINT32_MAX;
	mActiveKey = 0;
	mActiveState = {};
	mLevel = level;
}

void NRISkyEnvironment::ResetTrace()
{
	mLastTracedState = {};
	mLastTracedResolvedKey = 0;
	mHasTracedState = false;
}

void NRISkyEnvironment::ClearCache()
{
	mCache.clear();
	mActiveIndex = UINT32_MAX;
	mActiveKey = 0;
	mLevel = nullptr;
	mActiveState = {};
	ResetTrace();
}