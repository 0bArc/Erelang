# Build erelang.ico (multi-size) + icons/erelang.png for VS Code language / marketplace icons.
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$extDir = Join-Path (Join-Path $PSScriptRoot '..') 'erevos-language' | Resolve-Path
$iconsDir = Join-Path $extDir 'icons'
$null = New-Item -ItemType Directory -Force -Path $iconsDir

function New-ErelangBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    $bg = [System.Drawing.Color]::FromArgb(255, 24, 26, 34)
    $g.Clear($bg)

    $pad = [Math]::Max(2, [int]($size * 0.12))
    $rect = New-Object System.Drawing.Rectangle $pad, $pad, ($size - 2 * $pad), ($size - 2 * $pad)
    $accent = [System.Drawing.Color]::FromArgb(255, 86, 195, 255)
    $penW = [Math]::Max(1.0, ($size / 16.0))
    $border = New-Object System.Drawing.Pen $accent, $penW
    $g.DrawRectangle($border, $rect)

    $fontSize = [float]([Math]::Max(6, $size * 0.52))
    $font = New-Object System.Drawing.Font 'Segoe UI', $fontSize, ([System.Drawing.FontStyle]::Bold)
    $brush = New-Object System.Drawing.SolidBrush $accent
    $format = New-Object System.Drawing.StringFormat
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $g.DrawString('E', $font, $brush, ([System.Drawing.RectangleF]$rect), $format)

    $font.Dispose()
    $brush.Dispose()
    $border.Dispose()
    $g.Dispose()
    return $bmp
}

$sizes = @(16, 32, 48, 128, 256)
$pngPaths = @()
foreach ($s in $sizes) {
    $bmp = New-ErelangBitmap $s
    $png = Join-Path $iconsDir ("erelang-$s.png")
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngPaths += $png
}

# 32px PNG used by package.json language icon (VS Code prefers PNG over ICO in explorer)
$light = Join-Path $iconsDir 'erelang-light.png'
$dark = Join-Path $iconsDir 'erelang-dark.png'
Copy-Item -Force (Join-Path $iconsDir 'erelang-32.png') $light
Copy-Item -Force (Join-Path $iconsDir 'erelang-32.png') $dark

$icoOut = Join-Path $extDir 'erelang.ico'
$market = Join-Path $extDir 'icon.png'
Copy-Item -Force (Join-Path $iconsDir 'erelang-128.png') $market

# Multi-resolution .ico via npx png-to-ico (no package.json dep; one-off download ok)
$pngArg = ($pngPaths | ForEach-Object { "`"$_`"" }) -join ' '
$cmd = "npx --yes png-to-ico@1.0.0 $pngArg > `"$icoOut`""
Push-Location $extDir
try {
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "png-to-ico failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

Write-Host "wrote $icoOut"
Write-Host "wrote $light , $dark , $market"
