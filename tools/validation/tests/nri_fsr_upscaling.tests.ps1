Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))

function Read-RepoFile([string]$relativePath)
{
	return Get-Content -LiteralPath (Join-Path $root $relativePath) -Raw
}

function Assert-Match([string]$text, [string]$pattern, [string]$message)
{
	if ($text -notmatch $pattern)
	{
		throw $message
	}
}

function Assert-NotMatch([string]$text, [string]$pattern, [string]$message)
{
	if ($text -match $pattern)
	{
		throw $message
	}
}

$upscalerHeader = Read-RepoFile 'source/common/rendering/nri/renderer/nri_upscaler.h'
$upscaler = Read-RepoFile 'source/common/rendering/nri/renderer/nri_upscaler.cpp'
$frameResources = Read-RepoFile 'source/common/rendering/nri/renderer/nri_frame_resources.cpp'
$passDispatch = Read-RepoFile 'source/common/rendering/nri/renderer/nri_pass_dispatch.cpp'
$frameGraph = Read-RepoFile 'source/common/rendering/nri/renderer/nri_frame_graph.cpp'
$renderDevice = Read-RepoFile 'source/common/rendering/nri/system/nri_renderdevice.cpp'
$nriUpscaler = Read-RepoFile 'libraries/NRIFramework/External/NRI/Source/Shared/UpscalerInterface.hpp'
$nriCmake = Read-RepoFile 'libraries/NRIFramework/External/NRI/CMakeLists.txt'
$razeCmake = Read-RepoFile 'source/CMakeLists.txt'
$menu = Read-RepoFile 'wadsrc/static/menudef.txt'

# Persisted identity and provider semantics.
Assert-Match $upscalerHeader 'FSR\s*=\s*4' 'FSR must retain persisted main-upscaler value 4.'
Assert-Match $upscaler 'case\s+NRIMainUpscalerKind::FSR:\s*return\s+nri::UpscalerType::FSR' 'FSR must translate to the NRI FSR provider.'
Assert-Match $upscaler 'NRIIsStandardSuperResolutionMain[\s\S]*NRIMainUpscalerKind::DLSR\s*\|\|\s*kind\s*==\s*NRIMainUpscalerKind::FSR' 'FSR must share the standard-SR route with DLSS-SR.'
Assert-Match $upscaler 'kind\s*==\s*NRIMainUpscalerKind::FSR\s*&&\s*requestedMode\s*==\s*nri::UpscalerMode::ULTRA_QUALITY[\s\S]*return\s+nri::UpscalerMode::QUALITY' 'The generic Ultra Quality choice must resolve to the official FSR Quality mode.'
Assert-Match $upscaler 'case\s+NRIMainUpscalerKind::FSR:\s*slot\s*=\s*&mFsr' 'FSR must own an independent NRI context slot.'

# Context creation must happen before reduced resources are selected, and a
# failed real context must leave this frame at native resolution.
Assert-Match $frameResources 'requestedMainUpscalerKind\s*==\s*NRIMainUpscalerKind::FSR[\s\S]*EnsureMainUpscaler\([\s\S]*outputWidth,[\s\S]*outputHeight,[\s\S]*false,[\s\S]*false\)' 'FSR must preflight a real auto-exposure context before frame sizing.'
Assert-Match $frameResources 'fsrPreparationFailed\s*\?\s*1\.0f\s*:\s*NRIResolveRenderScaleForMain' 'FSR context failure must select native-sized resources.'
Assert-Match $upscaler 'creationFailed\s*&&\s*matchingConfiguration' 'A failed context configuration must not be retried every frame.'
Assert-Match $upscaler 'SubmitWaitAndRestartCommandList\("upscaler-recreate"\)[\s\S]*DestroyUpscaler\(frameBuffer, slot\.instance\)' 'Context replacement must submit, drain, and restart before destruction.'

# Dispatch contract: standard HDR SR, render-pixel motion, clip depth, applied
# jitter sign, camera data, frame time, unit scale, and RCAS sharpness.
Assert-Match $upscaler 'jitterScale\s*=\s*kind\s*==\s*NRIMainUpscalerKind::FSR\s*\?\s*1\.0f\s*:\s*-1\.0f' 'FSR must receive the applied jitter rather than the DLSS-specific inverse.'
Assert-Match $upscaler 'settings\.fsr\.zNear\s*=\s*desc\.zNear[\s\S]*settings\.fsr\.zFar\s*=\s*desc\.zFar[\s\S]*settings\.fsr\.verticalFov\s*=\s*desc\.verticalFov[\s\S]*settings\.fsr\.frameTime\s*=\s*desc\.frameTimeMs[\s\S]*settings\.fsr\.viewSpaceToMetersFactor\s*=\s*desc\.viewSpaceToMetersFactor[\s\S]*settings\.fsr\.sharpness\s*=\s*desc\.sharpness' 'FSR dispatch must populate every NRI FSR setting.'
Assert-Match $passDispatch 'standardSuperResolution\s*\?\s*NRIRenderer::FrameTextureSlot::SrInput\s*:\s*GetDlrrMainInputSlot\(\)' 'FSR must dispatch from the standard SrInput texture.'
Assert-Match $passDispatch 'const\s+bool\s+fsr\s*=\s*mainKind\s*==\s*NRIMainUpscalerKind::FSR[\s\S]*volumeReactive\s*=\s*!fsr\s*&&[\s\S]*if\s*\(!fsr\s*&&\s*context\.mExposure\.GetSettings\(\)\.enabled\)' 'Basic FSR must use its own automatic exposure and no mislabeled Raze reactive texture.'
Assert-Match $frameGraph 'compositionConsumesNrd\s*=\s*useCompositionPath\s*&&\s*denoise\s*&&\s*!NRIIsRayReconstructionMain' 'FSR must retain NRD; only ray reconstruction may bypass it.'

# The pinned NRI implementation must support and package both APIs safely.
Assert-Match $nriCmake 'FidelityFX-SDK-v1\.1\.4\.zip' 'NRI must pin FidelityFX SDK 1.1.4.'
Assert-Match $nriUpscaler 'type\s*==\s*UpscalerType::FSR[\s\S]*GraphicsAPI::D3D12\s*\|\|\s*deviceDesc\.graphicsAPI\s*==\s*GraphicsAPI::VK' 'NRI FSR must advertise D3D12 and Vulkan support.'
Assert-Match $nriUpscaler 'amd_fidelityfx_dx12\.dll[\s\S]*amd_fidelityfx_vk\.dll' 'NRI must load the API-matched FidelityFX backend.'
Assert-Match $nriUpscaler 'FfxUnregisterDevice\(m\.ffx->registeredVkDevice\)' 'Vulkan FSR teardown must release the process registry entry.'
Assert-Match $nriUpscaler 'if\s*\(m\.ffx->context\s*&&\s*m\.ffx->DestroyContext\)' 'Partial FidelityFX construction must be safe to destroy.'
Assert-Match $razeCmake 'RAZE_NRI_RUNTIME_NAMES[\s\S]*amd_fidelityfx_dx12\.dll[\s\S]*amd_fidelityfx_vk\.dll' 'Release staging must include both FidelityFX runtime DLLs.'
Assert-Match $razeCmake 'FidelityFX-SDK-LICENSE\.txt' 'Release staging must include the FidelityFX SDK license.'
Assert-NotMatch $razeCmake 'file\(GLOB\s+RAZE_NRI_RUNTIME_DLLS' 'Release staging must not copy an unpinned directory-wide DLL glob.'

# User-visible selection and diagnostics must distinguish upscaling from the
# independently controlled D3D12-only frame-generation feature.
Assert-Match $menu '4,\s*"AMD FSR 3 \(Upscaling\)"' 'The advanced menu must expose AMD FSR 3 upscaling.'
Assert-Match $menu 'Option\s+"AMD FSR 3 Frame Generation"' 'The separate frame-generation toggle must remain explicit.'
Assert-Match $renderDevice 'FSR-3\.1\.4=%s\s+ffx_sdk=1\.1\.4' 'Startup diagnostics must report the exact NRI FSR and FidelityFX SDK versions.'
Assert-Match $upscaler 'provider=FSR-3\.1\.4\s+ffx_sdk=1\.1\.4[\s\S]*result=success' 'Successful FSR context creation must be observable.'

Write-Host 'NRI FSR upscaling contract passed.'
