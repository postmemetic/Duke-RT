param(
    [string]$RazePath = 'build/terminal-ninja/raze.exe',
    [string]$SaveDirectory = 'M:/Raze/tools/perf-saves/Duke.WorldTour',
    [string]$SaveName = 'save0041',
    [string]$File = 'M:/Raze/full-voxel-overlay',
    [string]$GameGrp = 'C:/Program Files (x86)/Steam/steamapps/common/Duke Nukem 3D Twentieth Anniversary World Tour/DUKE3D.GRP',
    [string]$ConfigTemplate = 'C:/Users/brian/Documents/My Games/duke-rt/duke-rt.ini',
    [string]$OutputDirectory,
    [int]$SmokeEvolutionTics = 600,
    [int]$TimeoutSeconds = 300,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($SaveName -notmatch '^[A-Za-z0-9_.-]+$') { throw 'SaveName contains unsupported console characters.' }
if ($SmokeEvolutionTics -lt 300 -or $SmokeEvolutionTics -gt 900) {
    throw 'SmokeEvolutionTics must stay within the supported 10-30 second comparison window (300-900 tics).'
}

$resolvedRaze = Resolve-Path -LiteralPath $RazePath -ErrorAction Stop
$resolvedSaveDirectory = Resolve-Path -LiteralPath $SaveDirectory -ErrorAction Stop
$resolvedFile = Resolve-Path -LiteralPath $File -ErrorAction Stop
$resolvedConfigTemplate = Resolve-Path -LiteralPath $ConfigTemplate -ErrorAction Stop
if (-not (Test-Path -LiteralPath (Join-Path $resolvedSaveDirectory.Path "$SaveName.dsave"))) {
    throw "Save not found: $SaveName.dsave"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Get-Location) ('tools/logs/smoke-visual-baseline/{0}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
$configSnapshot = Join-Path $resolvedOutput 'capture-config-template.ini'
Copy-Item -LiteralPath $resolvedConfigTemplate.Path -Destination $configSnapshot -Force

$smokeDefaults = [ordered]@{
    nri_ptsmoke = 'true'
    nri_ptsmokequality = '2'
    nri_ptsmokeworkprofile = '2'
    nri_ptsmokeparticles = '8192'
    nri_ptsmokefroxelpixels = '16'
    nri_ptsmokefroxelz = '48'
    nri_ptsmokefroxelmaxdistance = '4096'
    nri_ptsmokecolumncapacity = '64'
    nri_ptsmokesimrate = '60'
    nri_ptsmokemaxsubsteps = '4'
    nri_ptsmoketimescale = '1'
    nri_ptsmokewindx = '5'
    nri_ptsmokewindy = '20'
    nri_ptsmokewindz = '5'
    nri_ptsmokedensityscale = '5'
    nri_ptsmokeradiancescale = '1'
    nri_ptsmokepointlights = 'true'
    nri_ptsmokedirectionallight = 'true'
    nri_ptsmokeemissivelights = 'true'
    nri_ptsmokeemissivereuse = '1'
    nri_ptsmokeemissivereference = 'false'
    nri_ptsmokeemissivepoints = '4'
    nri_ptsmokeemissivecandidate = '-1'
    nri_ptsmokeemissivebackend = '2'
    nri_ptsmokeemissiveworldfilter = 'false'
    nri_ptsmokeemissivelocal = 'true'
    nri_ptsmokeemissiveworlddebug = '0'
    nri_ptsmokeworldpartitions = '1'
    nri_ptsmokeworldnewcells = '8192'
    nri_ptsmokeworldmaintenancecells = '32768'
    nri_ptsmokeworldradiancemaxage = '16'
    nri_ptsmokeemissivelegacygatherdisable = 'false'
    nri_ptsmokeemissivequarterkey = 'false'
    nri_ptsmokeemissiveclamp = '32'
    nri_ptsmokedirectreuse = '2'
    nri_ptsmokedirectreference = '1'
    nri_ptsmokevolumehistory = 'true'
    nri_ptsmokedlrrmode = '1'
    nri_ptsmokeindirect = 'false'
    nri_ptsmokeindirectcache = '3'
    nri_ptsmokemultiplescatter = 'false'
    nri_ptsmokemultiplescatterscale = '1'
    nri_ptsmokemultiplescatteriterations = '0'
    nri_ptsmokemultiplescatterdebug = '0'
    nri_ptsmokeselfshadow = 'false'
    nri_ptsmokeselfshadowdebug = '0'
    nri_ptsmokelightmode = '3'
    nri_ptsmokelightsamples = '4'
    nri_ptsmokemaxlightcandidates = '8'
    nri_ptsmokefilteredvisibility = 'true'
    nri_ptsmokedebug = '0'
    nri_ptsmoketrace = '0'
    nri_ptsmokereadback = 'false'
    nri_ptsmokeviewcompare = 'false'
    nri_ptsmokeviewroute = '0'
    nri_ptsmokeindirectscale = '1'
    nri_ptsmokerepresentation = '1'
    nri_ptsmokegridbricks = '512'
    nri_ptsmokedormantgrid = 'true'
    nri_ptsmokegridcellsize = '8'
    nri_ptsmokegridbuoyancy = '1'
    nri_ptsmokegridvelocitydamping = '0.15'
    nri_ptsmokegridwindcoupling = '0.5'
    nri_ptsmokegriddensityhalflifescale = '1'
    nri_ptsmokegridcoolingscale = '1'
    nri_ptsmokegridmaxvelocity = '128'
    nri_ptsmokegridmaxbacktrace = '32'
    nri_ptsmokegridactivethreshold = '0.0001'
    nri_ptsmokegridreclaimgrace = '120'
    nri_ptmapsmokeeditmode = 'false'
}

$captureDefaults = [ordered]@{
    nri_ptwaitpresent = 'false'
    nri_validation = 'false'
    nri_apivalidation = 'false'
    nri_dred = 'true'
    nri_ptbloom = 'true'
    nri_renderscale = '1'
    nri_upscaler = '0'
    nri_upscalermode = '0'
    nri_denoise = 'true'
    nri_nrddenoiser = '1'
    nri_pttaa = 'false'
    nri_ptdebug = '0'
    nri_ptautoexposure = 'true'
    nri_ptautoexposurefreeze = 'false'
    nri_pttraceframes = '0'
    nri_ptscenestats = 'false'
    nri_voxelstats = 'false'
    r_voxels = 'true'
    use_mouse = 'false'
    use_joystick = 'false'
    cl_viewbob = '0'
    vid_defwidth = '1920'
    vid_defheight = '1080'
    vid_fullscreen = 'false'
    vid_hdr = 'false'
    vid_activeinbackground = 'true'
    vid_vsync = 'false'
    vid_maxfps = '60'
}

$variants = @(
    [pscustomobject]@{ id='00-current-default'; label='Current default: history on'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=.12; history=$true; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='01-history-off-baseline'; label='Item 0 baseline: history off'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=.12; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='02-low-albedo-soot'; label='Low albedo soot'; extinction=.008; albedo=@(.17,.165,.16); anisotropy=.12; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='03-high-albedo-lit'; label='High albedo lit'; extinction=.008; albedo=@(.50,.47,.43); anisotropy=.12; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='04-isotropic'; label='Isotropic phase: g=0'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=0; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='05-backward-phase'; label='Backward phase: g=-0.25'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=-.25; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='06-forward-phase'; label='Forward phase: g=0.45'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=.45; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='07-thin-luminous'; label='Thin luminous pair'; extinction=.0055; albedo=@(.42,.40,.37); anisotropy=.12; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='08-dense-soot'; label='Dense soot pair'; extinction=.0115; albedo=@(.22,.21,.20); anisotropy=.12; history=$false; multiple=$false; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='09-multiple-scatter-half'; label='Multiple scatter: scale 0.5'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=.12; history=$false; multiple=$true; multipleScale=.5; selfShadow=$false },
    [pscustomobject]@{ id='10-multiple-scatter-full'; label='Multiple scatter: scale 1.0'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=.12; history=$false; multiple=$true; multipleScale=1; selfShadow=$false },
    [pscustomobject]@{ id='11-self-shadow-diagnostic'; label='Rejected self-shadow diagnostic'; extinction=.008; albedo=@(.30,.29,.28); anisotropy=.12; history=$false; multiple=$false; multipleScale=1; selfShadow=$true }
)

function Add-SettingArguments {
    param([Collections.Generic.List[object]]$Arguments, [Collections.IDictionary]$Settings)
    foreach ($entry in $Settings.GetEnumerator()) {
        $Arguments.Add('+set'); $Arguments.Add([string]$entry.Key); $Arguments.Add([string]$entry.Value)
    }
}

function Format-OverlayFloat([double]$Value) {
    return $Value.ToString('0.####', [Globalization.CultureInfo]::InvariantCulture)
}

function New-StyleOverride {
    param([object]$Variant, [string]$Directory)
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $a = @($Variant.albedo | ForEach-Object { Format-OverlayFloat $_ }) -join ' '
    $text = @"
LIGHTOVR
{
    smokestyle "duke_fire_smoke"
    {
        density 3.0
        extinction $(Format-OverlayFloat $Variant.extinction)
        albedo $a
        anisotropy $(Format-OverlayFloat $Variant.anisotropy)
        radius 7.0
        expansionvelocity 10.0
        lifetime 18.0
        densityhalflife 6.0
        risevelocity 32.0
        velocityrandom 20.0
        velocityinherit 0.0
        buoyancy 7.0
        drag 0.05
        turbulence 42.0
        turbulencescale 24.0
        temperature 5.5
        momentumscale 1.0
        coolinghalflife 5.0
    }
}
"@
    Set-Content -LiteralPath (Join-Path $Directory 'LIGHTOVR') -Value $text -Encoding ASCII
}

$artifacts = [ordered]@{}
foreach ($variant in $variants) {
    $variantDirectory = Join-Path $resolvedOutput $variant.id
    $overrideDirectory = Join-Path $variantDirectory 'style-override'
    $shotDirectory = Join-Path $variantDirectory 'screenshots'
    $screenshotPath = Join-Path $shotDirectory ($variant.id + '_0000.png')
    if (Test-Path -LiteralPath $screenshotPath) {
        throw "Refusing to mix a new comparison with an existing artifact: $screenshotPath"
    }
    New-Item -ItemType Directory -Force -Path $shotDirectory | Out-Null
    New-StyleOverride -Variant $variant -Directory $overrideDirectory

    $settings = [ordered]@{}
    foreach ($entry in $smokeDefaults.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }
    foreach ($entry in $captureDefaults.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }
    $settings.nri_ptsmokevolumehistory = if ($variant.history) { 'true' } else { 'false' }
    $settings.nri_ptsmokemultiplescatter = if ($variant.multiple) { 'true' } else { 'false' }
    $settings.nri_ptsmokemultiplescatterscale = Format-OverlayFloat $variant.multipleScale
    $settings.nri_ptsmokeselfshadow = if ($variant.selfShadow) { 'true' } else { 'false' }
    $settings.screenshot_dir = $shotDirectory.Replace('\', '/')
    $settings.screenshotname = $variant.id

    $extraArguments = [Collections.Generic.List[object]]::new()
    $extraArguments.Add('-file')
    $extraArguments.Add($overrideDirectory.Replace('\', '/'))
    Add-SettingArguments -Arguments $extraArguments -Settings $settings

    $scenario = [ordered]@{
        name = 'smoke-visual-baseline-' + $variant.id
        backend = 'd3d12'
        description = $variant.label
        config = $configSnapshot
        commands = "+wait 45; load $SaveName; wait 35; closemenu; nri_ptautoexposurefreeze false; nri_ptautoexposurereset; nri_ptreset; wait $SmokeEvolutionTics; nri_ptsmokestatus; nri_ptautoexposurefreeze true; wait 8; screenshot; wait 60"
        save = [ordered]@{ dir = $resolvedSaveDirectory.Path.Replace('\', '/'); name = $SaveName }
        capture = [ordered]@{ loopTraceFrames = 0; timeoutSeconds = $TimeoutSeconds; runs = 1; stopWhenLoopTraceFramesCaptured = $false; stopWhenPrefix = 'screenshot saved'; stopWhenPrefixCount = 1 }
        launch = [ordered]@{ file = $resolvedFile.Path.Replace('\', '/'); gameGrp = $GameGrp; extraArgs = $extraArguments.ToArray() }
        requiredPrefixes = @('screenshot saved')
        forbiddenPatterns = @(
            'Device removed', 'device lost', 'DXGI_ERROR_DEVICE', 'QueueSubmit failed',
            'QueuePresent failed', 'AcquireNextTexture(): failed', 'NRI render failed',
            'validation error', 'failed to create', 'assertion failed', 'fatal error',
            'NRI screenshot failed', 'Failed writing screenshot', 'LIGHTOVR parse error'
        )
        smokeVisual = [ordered]@{
            label = $variant.label
            extinction = $variant.extinction
            albedo = $variant.albedo
            anisotropy = $variant.anisotropy
            volumeHistory = $variant.history
            multipleScatter = $variant.multiple
            multipleScatterScale = $variant.multipleScale
            selfShadow = $variant.selfShadow
        }
    }
    $scenarioPath = Join-Path $variantDirectory 'scenario.json'
    $scenario | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $scenarioPath -Encoding UTF8
    $artifacts[$variant.id] = [ordered]@{
        label = $variant.label
        scenario = $scenarioPath
        screenshot = $screenshotPath
    }
}

$preflight = [ordered]@{
    schema = 1
    save = [ordered]@{ path = (Join-Path $resolvedSaveDirectory.Path "$SaveName.dsave"); expectedTitle = 'LookingAtDumpsterFireSmoke' }
    smokeEvolutionTics = $SmokeEvolutionTics
    smokeDefaultCount = $smokeDefaults.Count - 1
    sessionSafetySettings = @('nri_ptmapsmokeeditmode=false')
    variants = $artifacts
}
$preflight | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'preflight.json') -Encoding UTF8
if ($ValidateOnly) {
    Write-Host "Smoke visual baseline preflight passed: output=$resolvedOutput variants=$($variants.Count)"
    return
}

$runner = Join-Path $PSScriptRoot 'run-nri-perf.ps1'
foreach ($variant in $variants) {
    $artifact = $artifacts[$variant.id]
    $runDirectory = Join-Path (Join-Path $resolvedOutput $variant.id) 'run'
    & $runner -ScenarioPath $artifact.scenario -RazePath $resolvedRaze.Path -Runs 1 `
        -TimeoutSeconds $TimeoutSeconds -OutputDirectory $runDirectory `
        -SummaryOutput (Join-Path $runDirectory 'summary.json')
    if (-not $?) { throw "Smoke visual capture failed for $($variant.id)." }
    if (-not (Test-Path -LiteralPath $artifact.screenshot)) {
        throw "Smoke visual capture did not publish $($artifact.screenshot)."
    }
}

$result = [ordered]@{
    schema = 1
    ok = $true
    outputDirectory = $resolvedOutput
    smokeEvolutionTics = $SmokeEvolutionTics
    artifacts = $artifacts
}
$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'result.json') -Encoding UTF8
Write-Host "Smoke visual baseline complete: output=$resolvedOutput variants=$($variants.Count)"
