#include "nri_renderer.h"

#include "nri_cvars.h"
#include "nri_diagnostic_names.h"
#include "nri_fluid_surface_gpu.h"
#include "../scene/nri_fluid_surface_normals.h"
#include "../system/nri_renderdevice.h"

#include "fluidoverlay.h"
#include "gamecontrol.h"
#include "printf.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	uint32_t ResolveStyleFlags(FluidOverlayStyle style)
	{
		switch (style)
		{
		case FluidOverlayStyle::Flow: return NRI_FLUID_STYLE_FLOW;
		case FluidOverlayStyle::Fall: return NRI_FLUID_STYLE_FALL;
		case FluidOverlayStyle::Viscous: return NRI_FLUID_STYLE_VISCOUS;
		case FluidOverlayStyle::Authored: return NRI_FLUID_STYLE_AUTHORED_MACRO;
		case FluidOverlayStyle::Lava: return NRI_FLUID_STYLE_VISCOUS | NRI_FLUID_STYLE_CRUST_EMISSION;
		case FluidOverlayStyle::Still:
		case FluidOverlayStyle::Waves:
		default: return NRI_FLUID_STYLE_NONE;
		}
	}

	float Saturate(float value)
	{
		return std::clamp(value, 0.0f, 1.0f);
	}

	NRIFluidBindingGpuData BuildGpuBinding(const ResolvedFluidOverlayBinding& binding)
	{
		const FluidOverlayParameters& source = binding.parameters;
		NRIFluidBindingGpuData result = {};
		result.flags = NRI_FLUID_BINDING_FLAG_ENABLED | NRI_FLUID_BINDING_FLAG_TWO_SIDED;
		result.styleFlags = ResolveStyleFlags(source.style);
		result.layerCount = std::clamp(source.octaves, 1u, NRI_FLUID_MAX_LAYERS);
		result.seed = (uint32_t)source.seed;

		result.domainOriginScale[3] = 1.0f / std::max(source.scale, 0.001f);
		result.domainAxisUFlowU[0] = 1.0f;
		result.domainAxisUFlowU[3] = source.flow[0] * source.speed;
		if (source.style == FluidOverlayStyle::Fall)
		{
			result.domainAxisVFlowV[1] = -1.0f;
		}
		else
		{
			result.domainAxisVFlowV[2] = 1.0f;
		}
		result.domainAxisVFlowV[3] = source.flow[1] * source.speed;

		const bool automaticTextureMix = source.textureMix < 0.0f;
		const float baseTextureMix = automaticTextureMix ?
			std::clamp(1.25f - source.opacity, 0.18f, 0.65f) : Saturate(source.textureMix);
		for (uint32_t channel = 0; channel < 3u; ++channel)
		{
			result.topColorBaseMix[channel] = source.bodyColor[channel];
			result.undersideColorBaseMix[channel] = source.undersideColor[channel];
			result.crestColorOpacity[channel] = source.crestColor[channel];
			result.emissionColorIntensity[channel] = source.emissionColor[channel];
		}
		result.topColorBaseMix[3] = baseTextureMix;
		result.undersideColorBaseMix[3] = automaticTextureMix ?
			std::max(0.12f, baseTextureMix * 0.70f) : baseTextureMix * 0.70f;
		result.crestColorOpacity[3] = Saturate(source.opacity);
		result.emissionColorIntensity[3] = std::max(source.emission, 0.0f);

		result.surfaceResponse[0] = Saturate(source.roughness);
		result.surfaceResponse[1] = Saturate(source.undersideRoughness);
		result.surfaceResponse[2] = std::clamp(source.normalStrength, 0.0f, 4.0f);
		result.surfaceResponse[3] = std::clamp(source.maxSlope, 0.0f, 4.0f);
		result.spectrum[0] = std::max(source.amplitude, 0.0f);
		result.spectrum[1] = std::max(source.frequency, 0.001f);
		result.spectrum[2] = std::clamp(source.lacunarity, 1.01f, 4.0f);
		result.spectrum[3] = std::clamp(source.gain, 0.0f, 1.0f);
		result.motion[0] = source.speed;
		result.motion[1] = source.timeScale;
		result.motion[2] = std::max(source.anisotropy, 0.05f);
		result.motion[3] = source.warp;
		result.shaping[0] = std::clamp(source.choppiness, 0.05f, 4.0f);
		result.shaping[1] = std::clamp(source.choppyRelaxation, 0.0f, 1.0f);
		result.shaping[2] = std::clamp(source.noiseScale, 0.05f, 4.0f);
		result.shaping[3] = source.octaveRotation * 0.01745329251994329577f;
		result.crust[0] = 0.54f;
		result.crust[1] = std::clamp(source.crustSoftness, 0.001f, 4.0f);
		result.crust[2] = 1.35f;
		result.crust[3] = source.style == FluidOverlayStyle::Lava ?
			std::clamp(source.crustDarkening, 0.0f, 1.0f) : 0.0f;

		constexpr float minimumVirtualViewCosine = 0.10f;
		const float virtualHalfHeight = std::max(source.virtualHeight, 0.0f);
		result.virtualParams[0] = virtualHalfHeight;
		result.virtualParams[1] = virtualHalfHeight / minimumVirtualViewCosine;
		result.virtualParams[2] = std::clamp(source.crestGain, 0.0f, 4.0f);
		result.virtualParams[3] = minimumVirtualViewCosine;
		for (uint32_t channel = 0; channel < 3u; ++channel)
		{
			result.foamColorAmount[channel] = source.foamColor[channel];
		}
		result.foamColorAmount[3] = Saturate(source.foamAmount);
		result.foamBirthParams[0] = Saturate(source.foamRoughness);
		result.foamBirthParams[1] = Saturate(source.foamThreshold);
		result.foamBirthParams[2] = Saturate(source.foamSoftness);
		result.foamBirthParams[3] = Saturate(source.foamUndersideStrength);
		result.foamTrailParams[0] = std::clamp(source.foamBreakupScale, 0.05f, 64.0f);
		result.foamTrailParams[1] = Saturate(source.foamBreakupStrength);
		result.foamTrailParams[2] = std::clamp(source.foamLifetime, 0.0f, 60.0f);
		result.foamTrailParams[3] = std::clamp(source.foamTrailGain, 0.0f, 4.0f);
		result.foamStyleParams[0] = std::clamp(source.foamContrast, 0.1f, 16.0f);
		result.foamStyleParams[1] = std::clamp(source.foamEdgeWidth, 0.0f, 0.5f);
		result.foamStyleParams[2] = std::clamp(source.foamCutoutScale, 0.05f, 64.0f);
		result.foamStyleParams[3] = Saturate(source.foamCutoutStrength);
		result.foamPresentationParams[0] = std::clamp(source.foamBrightness, 0.0f, 4.0f);
		return result;
	}
}

void NRIRenderer::SyncFluidSurfaceOverlayState()
{
	const ResolvedFluidOverlaySet& overlays = GetResolvedFluidOverlaySet();
	if (overlays.resolvedGeneration != mLastResolvedFluidOverlayGeneration)
	{
		const bool hadPreviousGeneration = mLastResolvedFluidOverlayGeneration != 0u;
		mLastResolvedFluidOverlayGeneration = overlays.resolvedGeneration;
		if (hadPreviousGeneration)
		{
			QueueStaticMapSceneLightingInvalidation();
			// Newly enabled fluid floors need their carrier-normal geometry payload
			// rebuilt too. Existing fluid floors remain watched when disabled.
			for (const auto& chunk : mMapWorld.chunks)
			{
				if (nri_scene::ComputeFluidFloorNormalSignature(chunk.sectorIndex) != 0)
					mRuntimeMutation.SeedSignatureWatchlist(chunk.chunkIndex);
			}
			RequestHistoryReset("fluidoverlay-resolve");
		}
		Printf("NRI PT fluid overlays: generation=%u bindings=%u rules=%u map=%s\n",
			overlays.resolvedGeneration,
			(unsigned)overlays.bindings.Size(),
			(unsigned)overlays.materialRules.Size(),
			overlays.activeMapName.GetChars());
	}

	const bool virtualHeightEnabled = (bool)nri_ptfluidvirtualheight;
	if (!mFluidVirtualHeightStateInitialized)
	{
		mFluidVirtualHeightStateInitialized = true;
		mLastFluidVirtualHeightEnabled = virtualHeightEnabled;
	}
	else if (virtualHeightEnabled != mLastFluidVirtualHeightEnabled)
	{
		mLastFluidVirtualHeightEnabled = virtualHeightEnabled;
		RequestHistoryReset(virtualHeightEnabled ? "fluid-virtual-height-on" : "fluid-virtual-height-off");
		Printf("NRI PT fluid virtual height: enabled=%u steps=%d\n",
			virtualHeightEnabled ? 1u : 0u,
			(int)nri_ptfluidvirtualheightsteps);
	}

	const int foamMode = std::clamp((int)nri_ptfluidfoam, 0, 2);
	if (!mFluidFoamModeStateInitialized)
	{
		mFluidFoamModeStateInitialized = true;
		mLastFluidFoamMode = foamMode;
		const bool foamSupported = mFrameBuffer != nullptr &&
			mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
		Printf("NRI PT fluid foam: mode=%d capability=%s\n",
			foamMode, foamSupported ? "d3d12-procedural" : "unsupported-backend-disabled");
	}
	else if (foamMode != mLastFluidFoamMode)
	{
		mLastFluidFoamMode = foamMode;
		RequestHistoryReset("fluid-foam-mode");
		const bool foamSupported = mFrameBuffer != nullptr &&
			mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
		Printf("NRI PT fluid foam: mode=%d capability=%s (0=off 1=instantaneous 2=analytic-trails)\n",
			foamMode, foamSupported ? "d3d12-procedural" : "unsupported-backend-disabled");
	}

	const int foamStyleMode = std::clamp((int)nri_ptfluidfoamstyle, 0, 1);
	if (!mFluidFoamStyleModeStateInitialized)
	{
		mFluidFoamStyleModeStateInitialized = true;
		mLastFluidFoamStyleMode = foamStyleMode;
		const bool foamStyleSupported = mFrameBuffer != nullptr &&
			mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
		Printf("NRI PT fluid foam style: mode=%d capability=%s (0=natural 1=graphic-cutouts)\n",
			foamStyleMode, foamStyleSupported ? "d3d12-procedural" : "unsupported-backend-disabled");
	}
	else if (foamStyleMode != mLastFluidFoamStyleMode)
	{
		mLastFluidFoamStyleMode = foamStyleMode;
		RequestHistoryReset("fluid-foam-style-mode");
		const bool foamStyleSupported = mFrameBuffer != nullptr &&
			mFrameBuffer->GetSelectedAPI() == nri::GraphicsAPI::D3D12;
		Printf("NRI PT fluid foam style: mode=%d capability=%s (0=natural 1=graphic-cutouts)\n",
			foamStyleMode, foamStyleSupported ? "d3d12-procedural" : "unsupported-backend-disabled");
	}
}

bool NRIRenderer::UpdateFluidSurfaceResources(SceneUploadBufferRingSlot& uploadSlot)
{
	const ResolvedFluidOverlaySet& overlays = GetResolvedFluidOverlaySet();
	std::vector<NRIFluidBindingGpuData> gpuBindings;
	if (overlays.bindings.Size() > 0)
	{
		gpuBindings.resize(overlays.bindings.Size());
		for (unsigned index = 0; index < overlays.bindings.Size(); ++index)
		{
			const ResolvedFluidOverlayBinding& binding = overlays.bindings[index];
			if (binding.bindingIndex < gpuBindings.size())
			{
				gpuBindings[binding.bindingIndex] = BuildGpuBinding(binding);
			}
		}
	}
	if (gpuBindings.empty())
	{
		gpuBindings.emplace_back();
	}
	mBoundFluidBindingCount = (uint32_t)overlays.bindings.Size();

	const float currentGameplaySeconds = PlayClock > 0 ? (float)((double)PlayClock / 120.0) : 0.0f;
	if (!mFluidTimeInitialized)
	{
		mPreviousFluidGameplaySeconds = currentGameplaySeconds;
		mFluidTimeInitialized = true;
	}
	NRIFluidFrameGpuData frame = {};
	frame.currentGameplaySeconds = currentGameplaySeconds;
	frame.previousGameplaySeconds = mPreviousFluidGameplaySeconds;
	frame.flags = mBoundFluidBindingCount != 0u ? NRI_FLUID_FRAME_FLAG_ENABLED : NRI_FLUID_FRAME_FLAG_NONE;
	if ((bool)nri_ptfluidvirtualheight)
	{
		frame.flags |= NRI_FLUID_FRAME_FLAG_VIRTUAL_HEIGHT;
	}
	frame.virtualStepCount = 1u;
	frame.debugMode = (uint32_t)std::clamp((int)nri_ptdebug, 0, (int)nri_diag::PtDebugMaximum);
	frame.foamMode = (uint32_t)std::clamp((int)nri_ptfluidfoam, 0, 2);
	frame.foamStyleMode = (uint32_t)std::clamp((int)nri_ptfluidfoamstyle, 0, 1);

	const bool bindingsReady = EnsureStructuredBuffer(
		uploadSlot.fluidBindingBuffer,
		uploadSlot.fluidBindingStats,
		gpuBindings.data(),
		gpuBindings.size() * sizeof(NRIFluidBindingGpuData),
		sizeof(NRIFluidBindingGpuData),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		true,
		"fluid_binding_upload");
	const bool frameReady = bindingsReady && EnsureStructuredBuffer(
		uploadSlot.fluidFrameBuffer,
		uploadSlot.fluidFrameStats,
		&frame,
		sizeof(frame),
		sizeof(frame),
		nri::BufferUsageBits::SHADER_RESOURCE,
		NRIResourceComputeShaderResourceAccess(),
		true,
		"fluid_frame_upload");
	if (frameReady)
	{
		mPreviousFluidGameplaySeconds = currentGameplaySeconds;
	}
	return frameReady;
}
