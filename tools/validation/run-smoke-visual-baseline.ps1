param(
    [string]$RazePath = 'build/terminal-ninja/raze.exe',
    [string]$SaveDirectory = 'M:/Raze/tools/perf-saves/Duke.WorldTour',
    [string]$SaveName = 'save0041',
    [string]$File = 'M:/Raze/full-voxel-overlay',
    [string]$GameGrp = 'C:/Program Files (x86)/Steam/steamapps/common/Duke Nukem 3D Twentieth Anniversary World Tour/DUKE3D.GRP',
    [string]$ConfigTemplate = 'C:/Users/brian/Documents/My Games/duke-rt/duke-rt.ini',
    [string]$VariantFile,
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
$resolvedVariantFile = if ([string]::IsNullOrWhiteSpace($VariantFile)) {
    $null
} else {
    Resolve-Path -LiteralPath $VariantFile -ErrorAction Stop
}
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
    nri_ptsmokeextinctionthreshold = '0'
    nri_ptsmokeextinctionknee = '0'
    nri_ptsmokeextinctiongamma = '1'
    nri_ptsmokeextinctionreference = '1'
    nri_ptsmokeextinctionshoulder = '0'
    nri_ptsmokethincolorr = '1'
    nri_ptsmokethincolorg = '1'
    nri_ptsmokethincolorb = '1'
    nri_ptsmokecorecolorr = '1'
    nri_ptsmokecorecolorg = '1'
    nri_ptsmokecorecolorb = '1'
    nri_ptsmokecolorpivot = '0.05'
    nri_ptsmokethermallow = '1'
    nri_ptsmokethermalhigh = '4'
    nri_ptsmokethermaltintr = '1'
    nri_ptsmokethermaltintg = '1'
    nri_ptsmokethermaltintb = '1'
    nri_ptsmokethermalglowr = '0'
    nri_ptsmokethermalglowg = '0'
    nri_ptsmokethermalglowb = '0'
    nri_ptsmokegradientpivot = '0.5'
    nri_ptsmokegradientwidth = '0.25'
    nri_ptsmokeedgesculpt = '0'
    nri_ptsmokeedgepowder = '0'
    nri_ptsmokeedgetintr = '1'
    nri_ptsmokeedgetintg = '1'
    nri_ptsmokeedgetintb = '1'
    nri_ptsmokeedgetintstrength = '0'
    nri_ptsmokeduallobeweight = '0'
    nri_ptsmokeduallobeg = '0.75'
    nri_ptsmokerimstrength = '0'
    nri_ptsmokerimgain = '0'
    nri_ptsmokeradianceedgechroma = '0'
    nri_ptsmokeradiancecavitycontrast = '0'
    nri_ptsmokeradiancedesaturation = '0'
    nri_ptsmokeradianceconfidence = '0.25'
    nri_ptsmokeradiancedirectionality = '0.15'
    nri_ptsmokethicknessstrength = '0'
    nri_ptsmokethicknesspivot = '0.12'
    nri_ptsmokethicknesssteps = '2'
    nri_ptsmokeflowhighlight = '0'
    nri_ptsmokeflowspeedreference = '32'
    nri_ptsmokecurlhighlight = '0'
    nri_ptsmokecurlreference = '1'
    nri_ptsmokecompressionsculpt = '0'
    nri_ptsmokedivergencereference = '1'
    nri_ptsmokebandstrength = '0'
    nri_ptsmokebandcount = '4'
    nri_ptsmokebandsoftness = '0.25'
    nri_ptsmokecontourstrength = '0'
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
    nri_ptsmokegridcurlevolution = '0'
    nri_ptsmokegridvorticity = '0'
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
    nri_ptloadingtrace = '0'
    nri_ptloadingvoxellist = 'false'
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

$styleDefaults = [ordered]@{
    density = 3.0
    extinction = 0.008
    albedo = @(0.30, 0.29, 0.28)
    anisotropy = 0.12
    radius = 7.0
    expansionVelocity = 10.0
    lifetime = 18.0
    densityHalfLife = 6.0
    riseVelocity = 32.0
    velocityRandom = 20.0
    velocityInherit = 0.0
    buoyancy = 7.0
    drag = 0.05
    turbulence = 42.0
    turbulenceScale = 24.0
    temperature = 5.5
    momentumScale = 1.0
    coolingHalfLife = 5.0
}

$sourceDefaults = [ordered]@{
    rule = 'duke_fire_sustained'
    actorClass = 'DukeFire'
    trigger = 'interval'
    activation = 'surface'
    representation = 'grid'
    queuePolicy = 'retry'
    analyticCarriers = 1
    emitterForeground = $true
    style = 'duke_fire_smoke'
    count = 9
    offset = @(0.0, 0.0, 0.0)
    spawnRadius = 4.0
    densityScale = 3.0
    radiusScale = 3.0
    velocityCone = 0.0
    velocityScale = 0.0
    intervalSeconds = 0.28
    pulseAmount = 0.90
    pulsePeriodCadences = 12
    pulsePhase = 0.7916667
    startTime = 0.0
    startDistance = 0.0
    spacing = 0.0
    maxSegmentsPerFrame = 12
}

$styleOverrideDefinitions = [ordered]@{
    radius = @{ type='number'; minimum=0.001 }
    expansionVelocity = @{ type='number'; minimum=0.0 }
    lifetime = @{ type='number'; minimum=0.001 }
    densityHalfLife = @{ type='number'; minimum=0.001 }
    riseVelocity = @{ type='number'; minimum=0.0 }
    velocityRandom = @{ type='number'; minimum=0.0 }
    velocityInherit = @{ type='number'; minimum=0.0 }
    buoyancy = @{ type='number'; minimum=0.0 }
    drag = @{ type='number'; minimum=0.0 }
    turbulence = @{ type='number'; minimum=0.0 }
    turbulenceScale = @{ type='number'; minimum=0.0 }
    temperature = @{ type='number'; minimum=0.0 }
    momentumScale = @{ type='number'; minimum=0.0 }
    coolingHalfLife = @{ type='number'; minimum=0.001 }
}

$sourceOverrideDefinitions = [ordered]@{
    rule = @{ type='rule' }
    actorClass = @{ type='actorClass' }
    count = @{ type='integer'; minimum=1; maximum=256 }
    offset = @{ type='vector3' }
    spawnRadius = @{ type='number'; minimum=0.0 }
    densityScale = @{ type='number'; minimum=0.0 }
    radiusScale = @{ type='number'; minimum=0.0 }
    velocityCone = @{ type='number'; minimum=0.0; maximum=180.0 }
    velocityScale = @{ type='number'; minimum=0.0 }
    intervalSeconds = @{ type='number'; minimum=0.001 }
    pulseAmount = @{ type='number'; minimum=0.0; maximum=1.0 }
    pulsePeriodCadences = @{ type='integer'; minimum=1; maximum=256 }
    pulsePhase = @{ type='phase' }
    startTime = @{ type='number'; minimum=0.0 }
    startDistance = @{ type='number'; minimum=0.0 }
    spacing = @{ type='number'; minimum=0.0 }
    maxSegmentsPerFrame = @{ type='integer'; minimum=1; maximum=256 }
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
if ($null -ne $resolvedVariantFile) {
    $variantDocument = Get-Content -LiteralPath $resolvedVariantFile.Path -Raw | ConvertFrom-Json
    if (($variantDocument.PSObject.Properties.Name -contains 'schema') -and
        ([int]$variantDocument.schema -notin @(1, 2))) {
        throw "VariantFile schema must be 1 or 2; found $($variantDocument.schema)."
    }
    if ($null -eq $variantDocument.variants -or @($variantDocument.variants).Count -eq 0) {
        throw 'VariantFile must contain a non-empty variants array.'
    }
    $variants = @($variantDocument.variants)
}

function Test-JsonNumber([object]$Value) {
    if ($null -eq $Value -or $Value -is [bool]) { return $false }
    return $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64] -or
        $Value -is [single] -or $Value -is [double] -or $Value -is [decimal]
}

function Convert-TypedValue {
    param([object]$Value, [Collections.IDictionary]$Definition, [string]$Context)
    switch ([string]$Definition.type) {
        'number' {
            if (-not (Test-JsonNumber $Value)) { throw "$Context must be a JSON number." }
            $normalized = [double]$Value
            if ([double]::IsNaN($normalized) -or [double]::IsInfinity($normalized)) {
                throw "$Context must be finite."
            }
        }
        'integer' {
            if (-not (Test-JsonNumber $Value) -or [double]$Value -ne [math]::Truncate([double]$Value)) {
                throw "$Context must be a JSON integer."
            }
            $normalized = [int]$Value
        }
        'phase' {
            if (-not (Test-JsonNumber $Value)) { throw "$Context must be a JSON number." }
            $phase = [double]$Value
            if ([double]::IsNaN($phase) -or [double]::IsInfinity($phase)) {
                throw "$Context must be finite."
            }
            return $phase - [math]::Floor($phase)
        }
        'vector3' {
            if ($Value -is [string] -or @($Value).Count -ne 3) { throw "$Context must be an array of three JSON numbers." }
            $normalized = @($Value | ForEach-Object {
                if (-not (Test-JsonNumber $_)) { throw "$Context must be an array of three JSON numbers." }
                [double]$_
            })
            return ,$normalized
        }
        'rule' {
            if ($Value -isnot [string] -or $Value -cne 'duke_fire_sustained') {
                throw "$Context must be the literal string 'duke_fire_sustained'."
            }
            return [string]$Value
        }
        'actorClass' {
            if ($Value -isnot [string] -or $Value -cnotin @('DukeFire', 'DukeFire2')) {
                throw "$Context must be either 'DukeFire' or 'DukeFire2'."
            }
            return [string]$Value
        }
        default { throw "Internal error: unsupported type '$($Definition.type)' for $Context." }
    }
    if ($Definition.Contains('minimum') -and $normalized -lt [double]$Definition.minimum) {
        throw "$Context must be at least $($Definition.minimum)."
    }
    if ($Definition.Contains('maximum') -and $normalized -gt [double]$Definition.maximum) {
        throw "$Context must be at most $($Definition.maximum)."
    }
    return $normalized
}

function Copy-Manifest([Collections.IDictionary]$Manifest) {
    $copy = [ordered]@{}
    foreach ($entry in $Manifest.GetEnumerator()) {
        $copy[$entry.Key] = if ($entry.Value -is [array]) { @($entry.Value) } else { $entry.Value }
    }
    return $copy
}

function Assert-KnownProperties {
    param([object]$Object, [string[]]$Allowed, [string]$Context)
    if ($null -eq $Object -or $Object -isnot [pscustomobject]) { throw "$Context must be a JSON object." }
    foreach ($property in $Object.PSObject.Properties) {
        if ($Allowed -notcontains $property.Name) { throw "$Context contains unknown field '$($property.Name)'." }
    }
}

function Apply-TypedOverrides {
    param(
        [Collections.IDictionary]$Target,
        [object]$Overrides,
        [Collections.IDictionary]$Definitions,
        [string]$Context
    )
    if ($null -eq $Overrides) { return }
    Assert-KnownProperties -Object $Overrides -Allowed @($Definitions.Keys) -Context $Context
    foreach ($property in $Overrides.PSObject.Properties) {
        $Target[$property.Name] = Convert-TypedValue -Value $property.Value -Definition $Definitions[$property.Name] -Context "$Context.$($property.Name)"
    }
}

function Test-NormalizedEqual([object]$Left, [object]$Right) {
    if ($Left -is [array] -or $Right -is [array]) {
        $leftValues = @($Left)
        $rightValues = @($Right)
        if ($leftValues.Count -ne $rightValues.Count) { return $false }
        for ($i = 0; $i -lt $leftValues.Count; ++$i) {
            if (-not (Test-NormalizedEqual $leftValues[$i] $rightValues[$i])) { return $false }
        }
        return $true
    }
    if ((Test-JsonNumber $Left) -and (Test-JsonNumber $Right)) { return [double]$Left -eq [double]$Right }
    return $Left -eq $Right
}

function Get-DefaultDiff {
    param([Collections.IDictionary]$Defaults, [Collections.IDictionary]$Effective)
    $diff = [ordered]@{}
    foreach ($entry in $Effective.GetEnumerator()) {
        if (-not $Defaults.Contains($entry.Key) -or -not (Test-NormalizedEqual $Defaults[$entry.Key] $entry.Value)) {
            $diff[$entry.Key] = [ordered]@{
                default = $(if ($Defaults.Contains($entry.Key)) { $Defaults[$entry.Key] } else { $null })
                effective = $entry.Value
            }
        }
    }
    return $diff
}

function Assert-ListedDiff {
    param([Collections.IDictionary]$Diff, [string[]]$ListedFields, [string]$Context)
    foreach ($field in $Diff.Keys) {
        if ($ListedFields -notcontains $field) { throw "$Context changed unlisted field '$field'." }
    }
}

function Get-NormalizedVariant([object]$Variant) {
    $allowedVariantFields = @(
        'id', 'label', 'extinction', 'albedo', 'anisotropy', 'history', 'multiple',
        'multipleScale', 'selfShadow', 'settings', 'styleOverrides', 'sourceOverrides',
        'expectedNominalMassRate'
    )
    Assert-KnownProperties -Object $Variant -Allowed $allowedVariantFields -Context 'variant'
    if ($Variant.id -isnot [string] -or $Variant.id -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') {
        throw 'variant.id must contain only safe path/console characters.'
    }
    if ($Variant.label -isnot [string] -or [string]::IsNullOrWhiteSpace($Variant.label)) {
        throw "variant '$($Variant.id)' must have a non-empty string label."
    }

    $style = Copy-Manifest $styleDefaults
    $declaredStyleFields = [Collections.Generic.List[string]]::new()
    foreach ($legacyField in @('extinction', 'albedo', 'anisotropy')) {
        if ($Variant.PSObject.Properties.Name -contains $legacyField) { $declaredStyleFields.Add($legacyField) }
    }
    if ($Variant.PSObject.Properties.Name -contains 'extinction') {
        $style.extinction = Convert-TypedValue $Variant.extinction @{ type='number'; minimum=0.0 } "variant '$($Variant.id)'.extinction"
    }
    if ($Variant.PSObject.Properties.Name -contains 'albedo') {
        $style.albedo = Convert-TypedValue $Variant.albedo @{ type='vector3' } "variant '$($Variant.id)'.albedo"
        foreach ($component in $style.albedo) {
            if ($component -lt 0.0 -or $component -gt 1.0) { throw "variant '$($Variant.id)'.albedo components must be between 0 and 1." }
        }
    }
    if ($Variant.PSObject.Properties.Name -contains 'anisotropy') {
        $style.anisotropy = Convert-TypedValue $Variant.anisotropy @{ type='number'; minimum=-0.99; maximum=0.99 } "variant '$($Variant.id)'.anisotropy"
    }
    if ($Variant.PSObject.Properties.Name -contains 'styleOverrides') {
        Apply-TypedOverrides $style $Variant.styleOverrides $styleOverrideDefinitions "variant '$($Variant.id)'.styleOverrides"
        foreach ($property in $Variant.styleOverrides.PSObject.Properties) { $declaredStyleFields.Add($property.Name) }
    }

    $source = Copy-Manifest $sourceDefaults
    $declaredSourceFields = [Collections.Generic.List[string]]::new()
    if ($Variant.PSObject.Properties.Name -contains 'sourceOverrides') {
        Apply-TypedOverrides $source $Variant.sourceOverrides $sourceOverrideDefinitions "variant '$($Variant.id)'.sourceOverrides"
        foreach ($property in $Variant.sourceOverrides.PSObject.Properties) { $declaredSourceFields.Add($property.Name) }
    }

    $history = $true
    $multiple = $false
    $multipleScale = 1.0
    $selfShadow = $false
    foreach ($field in @('history', 'multiple', 'selfShadow')) {
        if (($Variant.PSObject.Properties.Name -contains $field) -and $Variant.$field -isnot [bool]) {
            throw "variant '$($Variant.id)'.$field must be a JSON boolean."
        }
    }
    if ($Variant.PSObject.Properties.Name -contains 'history') { $history = [bool]$Variant.history }
    if ($Variant.PSObject.Properties.Name -contains 'multiple') { $multiple = [bool]$Variant.multiple }
    if ($Variant.PSObject.Properties.Name -contains 'selfShadow') { $selfShadow = [bool]$Variant.selfShadow }
    if ($Variant.PSObject.Properties.Name -contains 'multipleScale') {
        $multipleScale = Convert-TypedValue $Variant.multipleScale @{ type='number'; minimum=0.0 } "variant '$($Variant.id)'.multipleScale"
    }

    $settingsOverrides = [ordered]@{}
    if ($Variant.PSObject.Properties.Name -contains 'settings') {
        if ($null -eq $Variant.settings -or $Variant.settings -isnot [pscustomobject]) {
            throw "variant '$($Variant.id)'.settings must be a JSON object."
        }
        foreach ($property in $Variant.settings.PSObject.Properties) { $settingsOverrides[$property.Name] = [string]$property.Value }
    }

    $styleDiff = Get-DefaultDiff $styleDefaults $style
    $sourceDiff = Get-DefaultDiff $sourceDefaults $source
    Assert-ListedDiff $styleDiff $declaredStyleFields.ToArray() "variant '$($Variant.id)' style"
    Assert-ListedDiff $sourceDiff $declaredSourceFields.ToArray() "variant '$($Variant.id)' source"

    $nominalMassRate = [double]$source.count * [double]$source.densityScale / [double]$source.intervalSeconds
    if ($Variant.PSObject.Properties.Name -contains 'expectedNominalMassRate') {
        $expectedRate = Convert-TypedValue $Variant.expectedNominalMassRate @{ type='number'; minimum=0.0 } "variant '$($Variant.id)'.expectedNominalMassRate"
        $tolerance = [math]::Max(0.0001, [math]::Abs($expectedRate) * 0.00001)
        if ([math]::Abs($nominalMassRate - $expectedRate) -gt $tolerance) {
            throw "variant '$($Variant.id)' nominal mass rate $nominalMassRate does not match expected $expectedRate."
        }
    }

    return [pscustomobject]@{
        id = [string]$Variant.id
        label = [string]$Variant.label
        style = $style
        source = $source
        styleDiff = $styleDiff
        sourceDiff = $sourceDiff
        history = $history
        multiple = $multiple
        multipleScale = $multipleScale
        selfShadow = $selfShadow
        settingsOverrides = $settingsOverrides
        nominalMassRate = $nominalMassRate
    }
}

$normalizedVariants = @($variants | ForEach-Object { Get-NormalizedVariant $_ })
if (@($normalizedVariants.id | Sort-Object -Unique).Count -ne $normalizedVariants.Count) {
    throw 'Variant IDs must be unique.'
}

function Add-SettingArguments {
    param(
        [Collections.Generic.List[object]]$Arguments,
        [Collections.Generic.List[string]]$DeferredCommands,
        [Collections.IDictionary]$Settings
    )
    foreach ($entry in $Settings.GetEnumerator()) {
        $value = [string]$entry.Value
        if ($value.StartsWith('-', [StringComparison]::Ordinal)) {
            # Raze's startup parser treats a leading-minus CVar value as another
            # process option. Apply it through the console before smoke reset.
            $DeferredCommands.Add(('{0} {1}' -f $entry.Key, $value))
            continue
        }
        $Arguments.Add('+set'); $Arguments.Add([string]$entry.Key); $Arguments.Add($value)
    }
}

function Format-OverlayFloat([double]$Value) {
    return $Value.ToString('0.####', [Globalization.CultureInfo]::InvariantCulture)
}

function New-SmokeOverride {
    param([object]$Variant, [string]$Directory)
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $style = $Variant.style
    $source = $Variant.source
    $albedo = @($style.albedo | ForEach-Object { Format-OverlayFloat $_ }) -join ' '
    $offset = @($source.offset | ForEach-Object { Format-OverlayFloat $_ }) -join ' '
    $text = @"
LIGHTOVR
{
    smokestyle "duke_fire_smoke"
    {
        density $(Format-OverlayFloat $style.density)
        extinction $(Format-OverlayFloat $style.extinction)
        albedo $albedo
        anisotropy $(Format-OverlayFloat $style.anisotropy)
        radius $(Format-OverlayFloat $style.radius)
        expansionvelocity $(Format-OverlayFloat $style.expansionVelocity)
        lifetime $(Format-OverlayFloat $style.lifetime)
        densityhalflife $(Format-OverlayFloat $style.densityHalfLife)
        risevelocity $(Format-OverlayFloat $style.riseVelocity)
        velocityrandom $(Format-OverlayFloat $style.velocityRandom)
        velocityinherit $(Format-OverlayFloat $style.velocityInherit)
        buoyancy $(Format-OverlayFloat $style.buoyancy)
        drag $(Format-OverlayFloat $style.drag)
        turbulence $(Format-OverlayFloat $style.turbulence)
        turbulencescale $(Format-OverlayFloat $style.turbulenceScale)
        temperature $(Format-OverlayFloat $style.temperature)
        momentumscale $(Format-OverlayFloat $style.momentumScale)
        coolinghalflife $(Format-OverlayFloat $style.coolingHalfLife)
    }

    smokeactorrule "$($source.rule)"
    {
        actorclass "$($source.actorClass)"
        trigger $($source.trigger)
        activation $($source.activation)
        representation $($source.representation)
        queuepolicy $($source.queuePolicy)
        analyticcarriers $($source.analyticCarriers)
        emitterforeground $(if ($source.emitterForeground) { 'on' } else { 'off' })
        style "$($source.style)"
        count $($source.count)
        offset $offset
        spawnradius $(Format-OverlayFloat $source.spawnRadius)
        densityscale $(Format-OverlayFloat $source.densityScale)
        radiusscale $(Format-OverlayFloat $source.radiusScale)
        velocitycone $(Format-OverlayFloat $source.velocityCone)
        velocityscale $(Format-OverlayFloat $source.velocityScale)
        intervalseconds $(Format-OverlayFloat $source.intervalSeconds)
        pulseamount $(Format-OverlayFloat $source.pulseAmount)
        pulseperiodcadences $($source.pulsePeriodCadences)
        pulsephase $(Format-OverlayFloat $source.pulsePhase)
        starttime $(Format-OverlayFloat $source.startTime)
        startdistance $(Format-OverlayFloat $source.startDistance)
        spacing $(Format-OverlayFloat $source.spacing)
        maxsegmentsperframe $($source.maxSegmentsPerFrame)
    }
}
"@
    Set-Content -LiteralPath (Join-Path $Directory 'LIGHTOVR') -Value $text -Encoding ASCII
}

$artifacts = [ordered]@{}
foreach ($variant in $normalizedVariants) {
    $variantDirectory = Join-Path $resolvedOutput $variant.id
    $overrideDirectory = Join-Path $variantDirectory 'style-override'
    $shotDirectory = Join-Path $variantDirectory 'screenshots'
    $screenshotPath = Join-Path $shotDirectory ($variant.id + '_0000.png')
    if (Test-Path -LiteralPath $screenshotPath) {
        throw "Refusing to mix a new comparison with an existing artifact: $screenshotPath"
    }
    New-Item -ItemType Directory -Force -Path $shotDirectory | Out-Null
    New-SmokeOverride -Variant $variant -Directory $overrideDirectory

    $settings = [ordered]@{}
    foreach ($entry in $smokeDefaults.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }
    foreach ($entry in $captureDefaults.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }
    $settings.nri_ptsmokevolumehistory = if ($variant.history) { 'true' } else { 'false' }
    $settings.nri_ptsmokemultiplescatter = if ($variant.multiple) { 'true' } else { 'false' }
    $settings.nri_ptsmokemultiplescatterscale = Format-OverlayFloat $variant.multipleScale
    $settings.nri_ptsmokeselfshadow = if ($variant.selfShadow) { 'true' } else { 'false' }
    foreach ($entry in $variant.settingsOverrides.GetEnumerator()) { $settings[$entry.Key] = $entry.Value }

    $cvarDefaults = [ordered]@{}
    foreach ($entry in $smokeDefaults.GetEnumerator()) { $cvarDefaults[$entry.Key] = $entry.Value }
    foreach ($entry in $captureDefaults.GetEnumerator()) { $cvarDefaults[$entry.Key] = $entry.Value }
    $cvarDiff = Get-DefaultDiff $cvarDefaults $settings
    $listedCvarFields = [Collections.Generic.List[string]]::new()
    foreach ($field in @('nri_ptsmokevolumehistory', 'nri_ptsmokemultiplescatter', 'nri_ptsmokemultiplescatterscale', 'nri_ptsmokeselfshadow')) {
        $listedCvarFields.Add($field)
    }
    foreach ($field in $variant.settingsOverrides.Keys) { $listedCvarFields.Add([string]$field) }
    Assert-ListedDiff $cvarDiff $listedCvarFields.ToArray() "variant '$($variant.id)' CVars"
    $settings.screenshot_dir = $shotDirectory.Replace('\', '/')
    $settings.screenshotname = $variant.id

    $extraArguments = [Collections.Generic.List[object]]::new()
    $deferredSettingCommands = [Collections.Generic.List[string]]::new()
    $extraArguments.Add('-file')
    $extraArguments.Add($overrideDirectory.Replace('\', '/'))
    Add-SettingArguments -Arguments $extraArguments -DeferredCommands $deferredSettingCommands -Settings $settings
    $deferredSettings = if ($deferredSettingCommands.Count -gt 0) {
        ($deferredSettingCommands -join '; ') + '; '
    } else {
        ''
    }

    $scenario = [ordered]@{
        name = 'smoke-visual-baseline-' + $variant.id
        backend = 'd3d12'
        description = $variant.label
        config = $configSnapshot
        commands = "+wait 45; load $SaveName; wait 35; closemenu; ${deferredSettings}nri_ptautoexposurefreeze false; nri_ptautoexposurereset; nri_ptsmokereset; nri_ptreset; wait $SmokeEvolutionTics; nri_ptsmokestatus; nri_ptautoexposurefreeze true; wait 8; screenshot; wait 60"
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
            extinction = $variant.style.extinction
            albedo = $variant.style.albedo
            anisotropy = $variant.style.anisotropy
            volumeHistory = $variant.history
            multipleScatter = $variant.multiple
            multipleScatterScale = $variant.multipleScale
            selfShadow = $variant.selfShadow
            normalized = [ordered]@{
                style = $variant.style
                source = $variant.source
                cvars = $settings
            }
            defaultDiff = [ordered]@{
                style = $variant.styleDiff
                source = $variant.sourceDiff
                cvars = $cvarDiff
            }
            nominalMassRate = $variant.nominalMassRate
        }
    }
    $scenarioPath = Join-Path $variantDirectory 'scenario.json'
    $scenario | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $scenarioPath -Encoding UTF8
    $artifacts[$variant.id] = [ordered]@{
        label = $variant.label
        scenario = $scenarioPath
        screenshot = $screenshotPath
        normalized = [ordered]@{
            style = $variant.style
            source = $variant.source
            cvars = $settings
        }
        defaultDiff = [ordered]@{
            style = $variant.styleDiff
            source = $variant.sourceDiff
            cvars = $cvarDiff
        }
        nominalMassRate = $variant.nominalMassRate
    }
}

$preflight = [ordered]@{
    schema = 1
    save = [ordered]@{ path = (Join-Path $resolvedSaveDirectory.Path "$SaveName.dsave"); expectedTitle = 'LookingAtDumpsterFireSmoke' }
    smokeEvolutionTics = $SmokeEvolutionTics
    variantFile = $(if ($null -ne $resolvedVariantFile) { $resolvedVariantFile.Path } else { $null })
    smokeDefaultCount = $smokeDefaults.Count - 1
    sessionSafetySettings = @(
        'nri_ptmapsmokeeditmode=false',
        'nri_ptloadingtrace=0',
        'nri_ptloadingvoxellist=false'
    )
    styleDefaults = $styleDefaults
    sourceDefaults = $sourceDefaults
    smokeCVarDefaults = $smokeDefaults
    captureCVarDefaults = $captureDefaults
    variants = $artifacts
}
$preflight | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'preflight.json') -Encoding UTF8
if ($ValidateOnly) {
    Write-Host "Smoke visual baseline preflight passed: output=$resolvedOutput variants=$($normalizedVariants.Count)"
    return
}

$runner = Join-Path $PSScriptRoot 'run-nri-perf.ps1'
foreach ($variant in $normalizedVariants) {
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
Write-Host "Smoke visual baseline complete: output=$resolvedOutput variants=$($normalizedVariants.Count)"
