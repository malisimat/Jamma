<#
.SYNOPSIS
    Box-filter downsample a supersampled RGBA PNG and export a pixel-sharp,
    straight-alpha TGA matching Jamma's existing HUD button textures.

.DESCRIPTION
    Splits the source PNG into RGB and alpha, downsamples each independently
    with a box filter (avoids AA halo/ringing from Lanczos/cubic on straight
    edges), then recombines. Requires the source PNG's RGB to already be
    filled across fully-transparent regions (see SKILL.md's transparency
    rules) so downsampling never bleeds a black/default fringe into the
    silhouette edge.

.PARAMETER InputPng
    Path to the supersampled source PNG (with alpha channel).

.PARAMETER OutputTga
    Path to write the final TGA.

.PARAMETER TargetSize
    ImageMagick geometry string for the final size, e.g. "64x64".

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File render-tga-icon.ps1 -InputPng plus_8x.png -OutputTga Jamma\resources\textures\trigger_add.tga -TargetSize 64x64
#>
param(
    [Parameter(Mandatory = $true)][string]$InputPng,
    [Parameter(Mandatory = $true)][string]$OutputTga,
    [Parameter(Mandatory = $true)][string]$TargetSize
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command magick -ErrorAction SilentlyContinue)) {
    throw "ImageMagick 'magick' CLI not found on PATH."
}
if (-not (Test-Path $InputPng)) {
    throw "InputPng not found: $InputPng"
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("tga-icon-gen-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

try {
    $rgbFull = Join-Path $tempDir "rgb_full.png"
    $maskFull = Join-Path $tempDir "mask_full.png"
    $rgbSmall = Join-Path $tempDir "rgb_small.png"
    $maskSmall = Join-Path $tempDir "mask_small.png"

    # Drop alpha but keep the (pre-filled) RGB as authored.
    & magick $InputPng -alpha off $rgbFull
    # Extract the silhouette as a grayscale mask.
    & magick $InputPng -alpha extract $maskFull

    # Box filter = simple pixel averaging; keeps straight edges crisp and
    # gives natural AA only on curved corners.
    & magick $rgbFull -filter box -resize $TargetSize $rgbSmall
    & magick $maskFull -filter box -resize $TargetSize $maskSmall

    $outDir = Split-Path -Parent $OutputTga
    if ($outDir -and -not (Test-Path $outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }

    & magick $rgbSmall $maskSmall -alpha off -compose CopyOpacity -composite $OutputTga

    Write-Host "Wrote $OutputTga"
}
finally {
    Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
}
