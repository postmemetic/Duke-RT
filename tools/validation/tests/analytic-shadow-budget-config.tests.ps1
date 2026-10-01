[CmdletBinding()]
param(
    [switch]$RunRuntime,
    [string]$RazePath,
    [string]$GameGrp,
    [string]$OutputDirectory,
    [switch]$AllowOtherInstances,
    [ValidateSet('fresh', 'global', 'global-unknown', 'game-console', 'game-unknown', 'mixed-sections', 'explicit-command-line', 'explicit-autoexec')]
    [string[]]$Cases = @('fresh', 'global', 'global-unknown', 'game-console', 'game-unknown', 'mixed-sections', 'explicit-command-line', 'explicit-autoexec'),
    [ValidateRange(10, 300)][int]$TimeoutSeconds = 60
)

# Behavioral config tests. No source-text matching or gameplay map:
# query the actual CVar, save the private INI, and restart one migrated fixture.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $RunRuntime) {
    Write-Host 'Use -RunRuntime -GameGrp <DUKE3D.GRP> to run isolated startup config tests.'
    return
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Quote-NativeArgument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Write-Utf8([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Get-IniSection([string]$Text, [string]$Section) {
    $match = [regex]::Match($Text, '(?ims)^\[' + [regex]::Escape($Section) + '\][ \t]*\r?\n(.*?)(?=^\[|\z)')
    Require $match.Success "Missing saved section: $Section"
    return $match.Groups[1].Value
}

function Get-QueryNumber([string]$Log, [string]$Name, [string]$Marker) {
    $window = [regex]::Match($Log, '(?s)' + [regex]::Escape($Marker + '_BEGIN') + '(.*?)' + [regex]::Escape($Marker + '_END'))
    Require $window.Success "Missing completed query window: $Marker"
    $query = [regex]::Match($window.Groups[1].Value, '"' + [regex]::Escape($Name) + '"\s+is\s+"([^"\r\n]+)"')
    Require $query.Success "Missing $Name query in $Marker"
    return [double]::Parse($query.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
}

if (-not $RazePath) { $RazePath = Join-Path $repo 'build/terminal-ninja/raze.exe' }
Require (-not [string]::IsNullOrWhiteSpace($GameGrp)) '-GameGrp is required with -RunRuntime.'
$RazePath = (Resolve-Path -LiteralPath $RazePath).Path
$GameGrp = (Resolve-Path -LiteralPath $GameGrp).Path
if (-not $AllowOtherInstances) {
    Require (@(Get-Process -Name raze, duke-rt -ErrorAction SilentlyContinue).Count -eq 0) 'Close existing game processes or use -AllowOtherInstances for these private, non-performance config tests.'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('tools/logs/validation/analytic-shadow-budget-config-' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
Require (-not (Test-Path -LiteralPath $OutputDirectory)) "Refusing to overwrite output directory: $OutputDirectory"
$null = New-Item -ItemType Directory -Path $OutputDirectory
$results = [Collections.Generic.List[object]]::new()
$executableHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $RazePath).Hash

function Write-Fixture([string]$CaseName, [string]$CaseDirectory) {
    $sections = [ordered]@{
        LastRun = @('Version=11')
        GlobalSettings = @('vid_preferbackend=0', 'fullscreen=false', 'vid_defwidth=640', 'vid_defheight=480',
            'nri_ptsmoketimescale=0.75', 'nri_ptanalyticsoftshadowradius=7')
        'GlobalSettings.Unknown' = @('analytic_shadow_config_fixture=preserved')
        'Duke.UnknownConsoleVariables' = @()
        'Duke.ConsoleVariables' = @()
        'Duke.LocalServerInfo' = @()
        'Duke.Player' = @()
        'Duke.Autoexec' = @()
    }
    switch ($CaseName) {
        'fresh' { $sections.Remove('LastRun') }
        'global' { $sections['GlobalSettings'] += 'nri_ptanalyticshadowbudget=8' }
        'global-unknown' {
            $sections.Remove('LastRun')
            $sections['GlobalSettings.Unknown'] += 'nri_ptanalyticshadowbudget=8'
        }
        'game-console' { $sections['Duke.ConsoleVariables'] += 'nri_ptanalyticshadowbudget=8' }
        'game-unknown' { $sections['Duke.UnknownConsoleVariables'] += 'nri_ptanalyticshadowbudget=8' }
        'mixed-sections' {
            foreach ($section in @('GlobalSettings.Unknown', 'GlobalSettings', 'Duke.UnknownConsoleVariables',
                'Duke.ConsoleVariables', 'Duke.LocalServerInfo', 'Duke.Player')) {
                $sections[$section] += @('NRI_PTANALYTICSHADOWBUDGET=0', 'nri_ptanalyticshadowbudget=8', 'Nri_PtAnalyticShadowBudget=16')
            }
        }
        'explicit-command-line' { $sections['GlobalSettings'] += 'nri_ptanalyticshadowbudget=1' }
        'explicit-autoexec' { $sections['Duke.ConsoleVariables'] += 'nri_ptanalyticshadowbudget=1' }
    }
    if ($CaseName -in @('explicit-command-line', 'explicit-autoexec')) {
        $autoexec = Join-Path $CaseDirectory 'autoexec.cfg'
        $value = if ($CaseName -eq 'explicit-command-line') { 4 } else { 8 }
        Write-Utf8 $autoexec "set nri_ptanalyticshadowbudget $value`n"
        $sections['Duke.Autoexec'] = @('Path=' + $autoexec.Replace('\', '/'))
    }
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($section in $sections.GetEnumerator()) {
        $lines.Add('[' + $section.Key + ']')
        foreach ($line in $section.Value) { $lines.Add($line) }
        $lines.Add('')
    }
    $configPath = Join-Path $CaseDirectory 'config.ini'
    Write-Utf8 $configPath ($lines -join "`n")
    Copy-Item -LiteralPath $configPath -Destination (Join-Path $CaseDirectory 'input.ini')
    return $configPath
}

function Invoke-Fixture([string]$Name, [string]$CaseDirectory, [string]$ConfigPath, [int]$Expected,
    [bool]$EnableAutoexec = $false, [bool]$ExplicitCommandLine = $false, [bool]$CheckReload = $false,
    [int]$ExpectedSoftRadius = 7) {
    $record = [ordered]@{ case = $Name; passed = $false; expected = $Expected; observed = $null;
        observed_after_reload = $null; config = $ConfigPath; log = (Join-Path $CaseDirectory ($Name + '.log')) }
    try {
        $savePath = Join-Path $CaseDirectory ($Name + '-saves')
        $null = New-Item -ItemType Directory -Path $savePath
        $arguments = @('-config', $ConfigPath, '-savedir', $savePath, '-gamegrp', $GameGrp,
            '-nosound', '-nologo', '-width', '640', '-height', '480',
            '+set', 'vid_preferbackend', '0', '+set', 'fullscreen', 'false',
            '+set', 'use_mouse', 'false', '+set', 'use_joystick', 'false',
            '+set', 'i_pauseinbackground', 'false', '+set', 'vid_activeinbackground', 'true',
            '+logfile', $record.log.Replace('\', '/'))
        if (-not $EnableAutoexec) { $arguments += '-noautoexec' }
        if ($ExplicitCommandLine) { $arguments += @('+set', 'nri_ptanalyticshadowbudget', '8') }
        $commands = 'wait 20; echo SHADOW_CONFIG_QUERY_BEGIN; nri_ptanalyticshadowbudget; nri_ptsmoketimescale; nri_ptanalyticsoftshadowradius; echo SHADOW_CONFIG_QUERY_END; '
        if ($CheckReload) {
            $commands += 'reset2saved; echo SHADOW_CONFIG_RELOAD_BEGIN; nri_ptanalyticshadowbudget; echo SHADOW_CONFIG_RELOAD_END; '
        }
        $commands += 'echo SHADOW_CONFIG_DONE; quit'
        $arguments += '+' + $commands
        Write-Utf8 (Join-Path $CaseDirectory ($Name + '.arguments.json')) ($arguments | ConvertTo-Json)
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $RazePath
        $start.Arguments = ($arguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' '
        $start.WorkingDirectory = Split-Path -Parent $RazePath
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $start
        try {
            Require ($process.Start()) "Failed to start: $Name"
            $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
            while (-not $process.WaitForExit(1000)) {
                if ([DateTime]::UtcNow -ge $deadline) {
                    $process.Kill()
                    $null = $process.WaitForExit(5000)
                    throw "Timed out after $TimeoutSeconds seconds: $Name (owned PID $($process.Id))"
                }
            }
            $record['exit_code'] = $process.ExitCode
            Require ($process.ExitCode -eq 0) "Process exited $($process.ExitCode): $Name"
        }
        finally { $process.Dispose() }
        Require (Test-Path -LiteralPath $record.log) "Missing query log: $Name"
        $log = Get-Content -Raw -LiteralPath $record.log
        Require ($log.Contains('SHADOW_CONFIG_DONE')) "Missing completion marker: $Name"
        Require (-not [regex]::IsMatch($log, 'Device removed|device lost|NRI render failed|assertion failed|fatal error|unknown command', 'IgnoreCase')) "Runtime failure in $($record.log)"
        $record.observed = Get-QueryNumber $log 'nri_ptanalyticshadowbudget' 'SHADOW_CONFIG_QUERY'
        Require ($record.observed -eq $Expected) "Live budget expected $Expected, observed $($record.observed): $Name"
        Require ((Get-QueryNumber $log 'nri_ptsmoketimescale' 'SHADOW_CONFIG_QUERY') -eq 0.75) "Unrelated archived setting changed: $Name"
        Require ((Get-QueryNumber $log 'nri_ptanalyticsoftshadowradius' 'SHADOW_CONFIG_QUERY') -eq $ExpectedSoftRadius) "Unrelated session CVar INI behavior changed: $Name"
        if ($CheckReload) {
            $record.observed_after_reload = Get-QueryNumber $log 'nri_ptanalyticshadowbudget' 'SHADOW_CONFIG_RELOAD'
            Require ($record.observed_after_reload -eq $Expected) "Saved config replaced the explicit live override: $Name"
        }
        $saved = Get-Content -Raw -LiteralPath $ConfigPath
        Require (-not [regex]::IsMatch($saved, '(?im)^\s*nri_ptanalyticshadowbudget\s*=')) "Stale budget key remains in saved fixture: $Name"
        $globals = Get-IniSection $saved 'GlobalSettings'
        Require ([regex]::IsMatch($globals, '(?m)^nri_ptsmoketimescale=0\.75\s*$')) "Unrelated archived setting was not preserved: $Name"
        $unknown = Get-IniSection $saved 'GlobalSettings.Unknown'
        Require ([regex]::IsMatch($unknown, '(?m)^analytic_shadow_config_fixture=preserved\s*$')) "Unrelated unknown setting was not preserved: $Name"
        $record.passed = $true
        Write-Host "Analytic shadow config passed: $Name (budget $Expected)"
    }
    catch {
        $record['error'] = $_.Exception.Message
        throw
    }
    finally {
        $results.Add([pscustomobject]$record)
        $summary = [ordered]@{ executable = $RazePath; executable_sha256 = $executableHash; game_grp = $GameGrp;
            note = 'CPU config behavior only; private startup, no gameplay map or performance measurement.'; results = @($results.ToArray()) }
        Write-Utf8 (Join-Path $OutputDirectory 'summary.json') ($summary | ConvertTo-Json -Depth 5)
    }
}

foreach ($caseName in $Cases) {
    $caseDirectory = Join-Path $OutputDirectory $caseName
    $null = New-Item -ItemType Directory -Path $caseDirectory
    $configPath = Write-Fixture $caseName $caseDirectory
    $explicit = $caseName -in @('explicit-command-line', 'explicit-autoexec')
    $expected = if ($explicit) { 8 } else { 64 }
    Invoke-Fixture $caseName $caseDirectory $configPath $expected $explicit ($caseName -eq 'explicit-command-line') $explicit
    if ($caseName -eq 'mixed-sections') {
        # Restart the INI saved by the real process; do not synthesize the result.
        # The unrelated non-archived radius returns to its default on restart.
        Invoke-Fixture 'mixed-sections-restart' $caseDirectory $configPath 64 $false $false $false 4
    }
}
Write-Host "All $($results.Count) analytic shadow config runtime cases passed: $OutputDirectory"
