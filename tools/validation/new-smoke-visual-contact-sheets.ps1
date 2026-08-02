param(
    [Parameter(Mandatory = $true)] [string]$SourceDirectory,
    [Parameter(Mandatory = $true)] [string]$OutputDirectory,
    [string]$VariantFile,
    [string]$RepeatControlPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$source = (Resolve-Path -LiteralPath $SourceDirectory -ErrorAction Stop).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null

$items = @(
    [pscustomobject]@{ id='00-current-default'; label='Current default - history on' },
    [pscustomobject]@{ id='01-history-off-baseline'; label='Item 0 baseline - history off' },
    [pscustomobject]@{ id='02-low-albedo-soot'; label='Low albedo soot' },
    [pscustomobject]@{ id='03-high-albedo-lit'; label='High albedo lit' },
    [pscustomobject]@{ id='04-isotropic'; label='Isotropic phase - g 0' },
    [pscustomobject]@{ id='05-backward-phase'; label='Backward phase - g -0.25' },
    [pscustomobject]@{ id='06-forward-phase'; label='Forward phase - g 0.45' },
    [pscustomobject]@{ id='07-thin-luminous'; label='Thin luminous pair' },
    [pscustomobject]@{ id='08-dense-soot'; label='Dense soot pair' },
    [pscustomobject]@{ id='09-multiple-scatter-half'; label='Multiple scatter - scale 0.5' },
    [pscustomobject]@{ id='10-multiple-scatter-full'; label='Multiple scatter - scale 1.0' },
    [pscustomobject]@{ id='11-self-shadow-diagnostic'; label='Rejected self-shadow diagnostic' }
)
if (-not [string]::IsNullOrWhiteSpace($VariantFile)) {
    $resolvedVariantFile = (Resolve-Path -LiteralPath $VariantFile -ErrorAction Stop).Path
    $variantDocument = Get-Content -LiteralPath $resolvedVariantFile -Raw | ConvertFrom-Json
    $items = @($variantDocument.variants | ForEach-Object {
        [pscustomobject]@{ id=$_.id; label=$_.label }
    })
    if ($items.Count -eq 0) { throw 'VariantFile must contain a non-empty variants array.' }
}

foreach ($item in $items) {
    $inputPath = Join-Path (Join-Path (Join-Path $source $item.id) 'screenshots') ($item.id + '_0000.png')
    if (-not (Test-Path -LiteralPath $inputPath)) { throw "Missing screenshot: $inputPath" }
    Copy-Item -LiteralPath $inputPath -Destination (Join-Path $output ($item.id + '.png')) -Force
}

if (-not [string]::IsNullOrWhiteSpace($RepeatControlPath)) {
    $repeat = (Resolve-Path -LiteralPath $RepeatControlPath -ErrorAction Stop).Path
    Copy-Item -LiteralPath $repeat -Destination (Join-Path $output '12-current-default-repeat.png') -Force
}

function New-ContactSheet {
    param(
        [object[]]$SheetItems,
        [string]$Name
    )

    $columns = 3
    $tileWidth = 640
    $imageHeight = 360
    $labelHeight = 48
    $rows = [int][Math]::Ceiling($SheetItems.Count / [double]$columns)
    $bitmap = [Drawing.Bitmap]::new(
        $columns * $tileWidth,
        $rows * ($imageHeight + $labelHeight),
        [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $font = [Drawing.Font]::new('Segoe UI', 16, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $brush = [Drawing.SolidBrush]::new([Drawing.Color]::White)
    $format = [Drawing.StringFormat]::new()
    try {
        $graphics.Clear([Drawing.Color]::FromArgb(24, 24, 24))
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        for ($index = 0; $index -lt $SheetItems.Count; ++$index) {
            $item = $SheetItems[$index]
            $column = $index % $columns
            $row = [int][Math]::Floor($index / $columns)
            $x = $column * $tileWidth
            $y = $row * ($imageHeight + $labelHeight)
            $image = [Drawing.Image]::FromFile((Join-Path $output ($item.id + '.png')))
            try {
                $graphics.DrawImage($image, $x, $y, $tileWidth, $imageHeight)
            }
            finally {
                $image.Dispose()
            }
            $labelBounds = [Drawing.RectangleF]::new($x + 10, $y + $imageHeight + 8, $tileWidth - 20, $labelHeight - 8)
            $graphics.DrawString(($item.id + '  ' + $item.label), $font, $brush, $labelBounds, $format)
        }
        $bitmap.Save((Join-Path $output $Name), [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $format.Dispose()
        $brush.Dispose()
        $font.Dispose()
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

New-ContactSheet -SheetItems $items -Name 'contact-sheet-all.png'
if ([string]::IsNullOrWhiteSpace($VariantFile)) {
    New-ContactSheet -SheetItems @($items | Where-Object { $_.id -in @(
        '00-current-default', '01-history-off-baseline', '02-low-albedo-soot',
        '03-high-albedo-lit', '07-thin-luminous', '08-dense-soot') }) -Name 'contact-sheet-optics.png'
    New-ContactSheet -SheetItems @($items | Where-Object { $_.id -in @(
        '01-history-off-baseline', '04-isotropic', '05-backward-phase', '06-forward-phase',
        '09-multiple-scatter-half', '10-multiple-scatter-full', '11-self-shadow-diagnostic') }) -Name 'contact-sheet-phase-lighting.png'
} else {
    if (@($items | Where-Object { $_.id -eq '41-radiance-phase-identity' }).Count -gt 0) {
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^4[1-7]-|^5[5-7]-radiance' }) -Name 'contact-sheet-radiance.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^41-|^4[89]-phase|^5[0-2]-phase' }) -Name 'contact-sheet-phase.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^41-|^5[3-4]-combined' }) -Name 'contact-sheet-combined.png'
    } elseif (@($items | Where-Object { $_.id -eq '30-history-off-diagnostic' }).Count -gt 0) {
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^3[0-3]-' }) -Name 'contact-sheet-decisive-diagnostics.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^3[3-5]-' }) -Name 'contact-sheet-decisive-candidates.png'
    } elseif (@($items | Where-Object { $_.id -eq '20-followup-control' }).Count -gt 0) {
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^2[0-3]-' }) -Name 'contact-sheet-followup-radius.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^20-|^2[4-7]-' }) -Name 'contact-sheet-followup-combined.png'
    } elseif (@($items | Where-Object { $_.id -eq '00-motion-default' }).Count -gt 0) {
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^0[0-3]-(motion|pulse)' }) -Name 'contact-sheet-pulse-cadence.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^00-motion|^0[4-6]-random' }) -Name 'contact-sheet-source-random.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^00-motion|^(0[7-9]|1[01])-curl' }) -Name 'contact-sheet-curl.png'
    } elseif (@($items | Where-Object { $_.id -match '^1[2-9]-field|^2[01]-field' }).Count -gt 0) {
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^1[2-9]-field|^2[01]-field' }) -Name 'contact-sheet-field-diagnostics.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^2[2-7]-(shape|color)' }) -Name 'contact-sheet-optical-shaping.png'
    } else {
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^28-|^2[9]-thermal|^3[0-3]-thermal' }) -Name 'contact-sheet-thermal.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^28-|^3[4-8]-gradient' }) -Name 'contact-sheet-gradient.png'
        New-ContactSheet -SheetItems @($items | Where-Object { $_.id -match '^28-|^3[9]-combined|^40-combined' }) -Name 'contact-sheet-combined.png'
    }
}

if (-not [string]::IsNullOrWhiteSpace($RepeatControlPath)) {
    $controls = @(
        $items[0],
        [pscustomobject]@{ id='12-current-default-repeat'; label='Current default repeat control' }
    )
    New-ContactSheet -SheetItems $controls -Name 'contact-sheet-control-repeat.png'
}

Write-Host "Smoke visual contact sheets complete: output=$output"
