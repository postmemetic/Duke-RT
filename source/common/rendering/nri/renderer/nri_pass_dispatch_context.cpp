#include "nri_pass_dispatch.h"

#include "nri_descriptor_sets.h"
#include "nri_scene_upload.h"
#include "../system/nri_renderdevice.h"

NRITextureResource& NRIPassDispatchContext::TextureService::Get(FrameTextureSlot slot) const
{
	return getFrameTexture(user, slot);
}

nri::Pipeline* NRIPassDispatchContext::PipelineService::Get(PipelineSlot slot) const
{
	return getPipeline(user, slot);
}

bool NRIPassDispatchContext::PipelineService::EnsureIndirectRadianceCachePipeline() const
{
	return ensureIndirectRadianceCachePipeline != nullptr && ensureIndirectRadianceCachePipeline(user);
}

nri::PipelineLayout* NRIPassDispatchContext::PipelineService::GetIndirectRadianceCachePipelineLayout() const
{
	return getIndirectRadianceCachePipelineLayout != nullptr ? getIndirectRadianceCachePipelineLayout(user) : nullptr;
}

bool NRIPassDispatchContext::DescriptorService::UpdateFrameTextureSet() const
{
	return updateFrameTextureSet(user);
}

bool NRIPassDispatchContext::DescriptorService::UpdateFrameTextureSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_INPUT_DESCRIPTOR_NUM>& descriptors) const
{
	return updateFrameTextureSetWithDescriptors(user, descriptorSet, descriptors);
}

bool NRIPassDispatchContext::DescriptorService::UpdateOutputSet() const
{
	return updateOutputSet(user);
}

bool NRIPassDispatchContext::DescriptorService::UpdateOutputSet(nri::DescriptorSet* descriptorSet, const std::array<nri::Descriptor*, NRI_OUTPUT_DESCRIPTOR_NUM>& descriptors) const
{
	return updateOutputSetWithDescriptors(user, descriptorSet, descriptors);
}

NRIResourceServices NRIPassDispatchContext::ResourceService::BuildResourceServices() const
{
	return buildResourceServices(user);
}

bool NRIPassDispatchContext::ResourceService::UpdateReprojectionBuffer() const
{
	return updateReprojectionBuffer(user);
}

NRIPTOutputPolicy NRIPassDispatchContext::ResourceService::GetOutputPolicy() const
{
	return getOutputPolicy(user);
}

void NRIPassDispatchContext::ResourceService::CopyFinalToActiveTarget() const
{
	copyFinalToActiveTarget(user);
}

void NRIPassDispatchContext::ResourceService::CopyTexture(NRITextureResource& source, NRITextureResource& destination) const
{
	copyTexture(user, source, destination);
}

void NRIPassDispatchContext::ResourceService::TransitionTexture(NRITextureResource& texture, nri::AccessLayoutStage after) const
{
	transitionTexture(user, texture, after);
}

nri::CommandBuffer* NRIPassDispatchContext::CommandService::GetCommandBuffer() const
{
	return commandBuffer;
}

void NRIPassDispatchContext::CommandService::RestoreDescriptorPool() const
{
	if (core != nullptr && commandBuffer != nullptr && descriptorPool != nullptr)
	{
		core->CmdSetDescriptorPool(*commandBuffer, *descriptorPool);
	}
}

void NRIPassDispatchContext::CommandService::SetPipelineLayout(nri::PipelineLayout* pipelineLayout) const
{
	core->CmdSetPipelineLayout(*commandBuffer, nri::BindPoint::COMPUTE, *pipelineLayout);
}

void NRIPassDispatchContext::CommandService::SetRootConstants(const void* data, uint32_t size) const
{
	core->CmdSetRootConstants(*commandBuffer, { 0, data, size, 0, nri::BindPoint::COMPUTE });
}

void NRIPassDispatchContext::CommandService::SetDescriptorSet(uint32_t setIndex, nri::DescriptorSet* descriptorSet) const
{
	core->CmdSetDescriptorSet(*commandBuffer, { setIndex, descriptorSet, nri::BindPoint::COMPUTE });
}

void NRIPassDispatchContext::CommandService::SetPipeline(nri::Pipeline* pipeline) const
{
	core->CmdSetPipeline(*commandBuffer, *pipeline);
}

void NRIPassDispatchContext::CommandService::Dispatch(uint32_t x, uint32_t y, uint32_t z) const
{
	core->CmdDispatch(*commandBuffer, { x, y, z });
}

void NRIPassDispatchContext::CommandService::UpdateDescriptorRanges(const nri::UpdateDescriptorRangeDesc* updates, uint32_t updateCount) const
{
	core->UpdateDescriptorRanges(updates, updateCount);
}

nri::DescriptorSet* NRIPassDispatchContext::SceneBindingService::GetCurrentSceneTextureSet() const
{
	return getCurrentSceneTextureSet(user);
}

nri::DescriptorSet* NRIPassDispatchContext::SceneBindingService::GetCurrentSceneDataSet() const
{
	return getCurrentSceneDataSet(user);
}

bool NRIPassDispatchContext::SceneBindingService::BindSceneRootDescriptors() const
{
	return bindSceneRootDescriptors != nullptr && bindSceneRootDescriptors(user);
}

void NRIPassDispatchContext::ExposureService::ReadbackAutoExposureStats() const
{
	readbackAutoExposureStats(user);
}

bool NRIPassDispatchContext::ExposureService::EnsureAutoExposureResources(const NRIAutoExposureSettings& settings) const
{
	return ensureAutoExposureResources(user, settings);
}

void NRIPassDispatchContext::ExposureService::RequestAutoExposureReset(const char* reason) const
{
	requestAutoExposureReset(user, reason);
}

bool NRIPassDispatchContext::ExposureService::DispatchAutoExposure(FrameTextureSlot sourceSlot) const
{
	return dispatchAutoExposure(user, sourceSlot);
}

NRIPassDispatchContext::ExposureRoute NRIPassDispatchContext::ExposureService::ResolveExposureRoute(FrameTextureSlot inputSlot, const NRIPTOutputPolicy& outputPolicy, NRIMainUpscalerKind mainKind, NRIPostSharpenKind postSharpenKind) const
{
	return resolveExposureRoute(user, inputSlot, outputPolicy, mainKind, postSharpenKind);
}

NRIMainUpscalerKind NRIPassDispatchContext::UpscalerService::ResolveMainUpscalerKind(bool logFallback) const
{
	return resolveMainUpscalerKind(user, logFallback);
}

NRIPostSharpenKind NRIPassDispatchContext::UpscalerService::ResolvePostSharpenKind(bool logFallback) const
{
	return resolvePostSharpenKind(user, logFallback);
}

nri::UpscalerMode NRIPassDispatchContext::UpscalerService::GetSelectedUpscalerMode() const
{
	return getSelectedUpscalerMode(user);
}

bool NRIPassDispatchContext::UpscalerService::ShouldRunAppTaaForFrameGraph(NRIMainUpscalerKind kind) const
{
	return shouldRunAppTaaForFrameGraph(user, kind);
}

void NRIPassDispatchContext::UpscalerService::TraceTemporalState(const char* stage, NRIMainUpscalerKind resolvedMainUpscaler, NRIPostSharpenKind resolvedPostSharpen, bool runAppTaa, FrameTextureSlot primarySlot, FrameTextureSlot secondarySlot) const
{
	traceTemporalState(user, stage, resolvedMainUpscaler, resolvedPostSharpen, runAppTaa, primarySlot, secondarySlot);
}

bool NRIPassDispatchContext::UpscalerService::EnsureMainUpscaler(NRIMainUpscalerKind kind, nri::UpscalerMode mode, uint32_t outputWidth, uint32_t outputHeight, bool exposure, bool reactive) const
{
	return ensureMainUpscaler(user, kind, mode, outputWidth, outputHeight, exposure, reactive);
}

bool NRIPassDispatchContext::UpscalerService::DispatchMainUpscaler(NRIMainUpscalerKind kind, const NRIUpscalerDispatchDesc& desc) const
{
	return dispatchMainUpscaler(user, kind, desc);
}

bool NRIPassDispatchContext::UpscalerService::EnsurePostSharpen(NRIPostSharpenKind kind, uint32_t outputWidth, uint32_t outputHeight) const
{
	return ensurePostSharpen(user, kind, outputWidth, outputHeight);
}

bool NRIPassDispatchContext::UpscalerService::DispatchPostSharpen(NRIPostSharpenKind kind, const NRIUpscalerDispatchDesc& desc) const
{
	return dispatchPostSharpen(user, kind, desc);
}

void NRIPassDispatchContext::SelfTestService::ResetSelfTestRouteSnapshot() const
{
	resetSelfTestRouteSnapshot(user);
}

void NRIPassDispatchContext::SelfTestService::SetSelfTestRouteSnapshot(const char* routeName, const char* presenterName, const char* ownerName, const char* passListName, bool denoiserRun, bool upscalerRun, bool exposureRun) const
{
	setSelfTestRouteSnapshot(user, routeName, presenterName, ownerName, passListName, denoiserRun, upscalerRun, exposureRun);
}

bool NRIPassDispatchContext::SmokeService::PrepareFrame(bool mainViewEligible) const
{
	static const TArray<PathTracingWeaponLightEvent> emptyWeaponEvents;
	return prepareFrame == nullptr || prepareFrame(user, mainViewEligible, weaponEvents != nullptr ? *weaponEvents : emptyWeaponEvents);
}

bool NRIPassDispatchContext::SmokeService::DispatchRoute(const NRISmokeRouteDesc& route) const
{
	return dispatchRoute == nullptr || dispatchRoute(user, route);
}

NRIPassDispatchContext::FrameTextureSlot NRIPassDispatchContext::SmokeService::GetVolumeSlot(bool metadata) const
{
	return getVolumeSlot != nullptr ? (FrameTextureSlot)getVolumeSlot(user, metadata) : FrameTextureSlot::Count;
}

NRIIndirectRadianceCachePrepareResult NRIPassDispatchContext::IndirectRadianceCacheService::Prepare(bool enabled) const
{
	return prepare != nullptr ? prepare(user, enabled) : NRIIndirectRadianceCachePrepareResult{};
}

bool NRIPassDispatchContext::IndirectRadianceCacheService::RecordPendingClear() const
{
	return recordPendingClear == nullptr || recordPendingClear(user);
}

void NRIPassDispatchContext::IndirectRadianceCacheService::AdvanceFrame() const
{
	if (advanceFrame != nullptr) advanceFrame(user);
}

void NRIPassDispatchContext::IndirectRadianceCacheService::CopyTelemetry(uint64_t frameNumber) const
{
	if (copyTelemetry != nullptr) copyTelemetry(user, frameNumber);
}

void NRIPassDispatchContext::IndirectRadianceCacheService::ReadbackTelemetry(bool enabled) const
{
	if (readbackTelemetry != nullptr) readbackTelemetry(user, enabled);
}

const NRIIndirectRadianceCacheTelemetrySnapshot& NRIPassDispatchContext::IndirectRadianceCacheService::GetTelemetry() const
{
	static const NRIIndirectRadianceCacheTelemetrySnapshot empty = {};
	return getTelemetry != nullptr ? getTelemetry(user) : empty;
}

NRIPassDispatchContext::NRIPassDispatchContext(const Init& init)
	: mTextures(init.textures),
	mPipelines(init.pipelines),
	mDescriptors(init.descriptors),
	mResources(init.resources),
	mCommands(init.commands),
	mSceneBinding(init.sceneBinding),
	mExposureService(init.exposureService),
	mUpscalerService(init.upscalerService),
	mSelfTest(init.selfTest),
	mSmokeService(init.smokeService),
	mIndirectRadianceCacheService(init.indirectRadianceCacheService),
	mPipelineLayout(*init.pipelineLayout),
	mTaaPipelineLayout(*init.taaPipelineLayout),
	mPresentPipelineLayout(*init.presentPipelineLayout),
	mBloomPipelineLayout(*init.bloomPipelineLayout),
	mSamplerSet(*init.samplerSet),
	mFrameTextureSet(*init.frameTextureSet),
	mOutputSet(*init.outputSet),
	mCompositionFrameTextureSet(*init.compositionFrameTextureSet),
	mCompositionOutputSet(*init.compositionOutputSet),
	mUpscalerPrepassFrameTextureSet(*init.upscalerPrepassFrameTextureSet),
	mUpscalerPrepassOutputSet(*init.upscalerPrepassOutputSet),
	mTaaFrameTextureSet(*init.taaFrameTextureSet),
	mTaaOutputSet(*init.taaOutputSet),
	mRawPresentFrameTextureSet(*init.rawPresentFrameTextureSet),
	mRawPresentOutputSet(*init.rawPresentOutputSet),
	mFinalPresentFrameTextureSet(*init.finalPresentFrameTextureSet),
	mFinalPresentOutputSet(*init.finalPresentOutputSet),
	mBloomInputSets(*init.bloomInputSets),
	mBloomOutputSets(*init.bloomOutputSets),
	mFrameInputDescriptors(*init.frameInputDescriptors),
	mOutputDescriptors(*init.outputDescriptors),
	mExposure(*init.exposure),
	mTraceShaderStats(*init.traceShaderStats),
	mNrd(*init.nrd),
	mUpscaler(*init.upscaler),
	mPersistentVoxels(*init.persistentVoxels),
	mBoundSceneInstances(*init.boundSceneInstances),
	mDirectionalLightState(*init.directionalLightState),
	mNightVisionState(*init.nightVisionState),
	mLastPerfShellTraceStats(*init.lastPerfShellTraceStats),
	mLastPerfTraceShaderStats(*init.lastPerfTraceShaderStats),
	mLastAutoExposureSettings(*init.lastAutoExposureSettings),
	mFrame(init.frame),
	mSceneStats(init.sceneStats),
	mHasAutoExposureSettingsState(*init.hasAutoExposureSettingsState),
	mUseUpscaledInFinal(*init.useUpscaledInFinal),
	mUseDenoisedCompositionInputs(*init.useDenoisedCompositionInputs),
	mUseSplitShadowDenoiser(*init.useSplitShadowDenoiser),
	mHistoryInputSlot(*init.historyInputSlot),
	mHistoryOutputSlot(*init.historyOutputSlot),
	mUpscaledInputSlot(*init.upscaledInputSlot)
{
}
