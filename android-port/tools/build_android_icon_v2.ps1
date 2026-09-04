param(
    [string]$Source = (Join-Path $PSScriptRoot '..\A9TasAndroid\design\app-icon-v2-master.png'),
    [string]$ResRoot = (Join-Path $PSScriptRoot '..\A9TasAndroid\app\src\main\res'),
    [switch]$UseExistingMaster
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Write-MinimalMaster([string]$path) {
    $size = 1024
    $canvas = [System.Drawing.Bitmap]::new(
        $size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $font = [System.Drawing.Font]::new(
        'Arial', 260, [System.Drawing.FontStyle]::Bold,
        [System.Drawing.GraphicsUnit]::Pixel)
    $brush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.ColorTranslator]::FromHtml('#F5F5F7'))
    $format = [System.Drawing.StringFormat]::new()
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingQuality =
            [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.SmoothingMode =
            [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.TextRenderingHint =
            [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $format.Alignment = [System.Drawing.StringAlignment]::Center
        $format.LineAlignment = [System.Drawing.StringAlignment]::Center
        # Keep the entire word inside Android's adaptive-icon safe zone.
        $bounds = [System.Drawing.RectangleF]::new(112, 112, 800, 800)
        $graphics.DrawString('TAS', $font, $brush, $bounds, $format)
        $directory = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $directory | Out-Null
        }
        $canvas.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $format.Dispose()
        $brush.Dispose()
        $font.Dispose()
        $graphics.Dispose()
        $canvas.Dispose()
    }
}

if (-not $UseExistingMaster) {
    Write-MinimalMaster $Source
}

$sourcePath = (Resolve-Path -LiteralPath $Source).Path
$sourceImage = [System.Drawing.Bitmap]::FromFile($sourcePath)
$densities = [ordered]@{
    'mdpi' = 48
    'hdpi' = 72
    'xhdpi' = 96
    'xxhdpi' = 144
    'xxxhdpi' = 192
}

function New-Canvas([int]$size) {
    return [System.Drawing.Bitmap]::new(
        $size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
}

function Configure-Graphics([System.Drawing.Graphics]$graphics) {
    $graphics.CompositingQuality =
        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
}

function New-RoundedPath([int]$size, [float]$radius) {
    $diameter = $radius * 2
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc(0, 0, $diameter, $diameter, 180, 90)
    $path.AddArc($size - $diameter, 0, $diameter, $diameter, 270, 90)
    $path.AddArc($size - $diameter, $size - $diameter,
        $diameter, $diameter, 0, 90)
    $path.AddArc(0, $size - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function Save-ResizedForeground([string]$path, [int]$size) {
    $canvas = New-Canvas $size
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    try {
        Configure-Graphics $graphics
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.DrawImage($sourceImage, 0, 0, $size, $size)
        $canvas.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $canvas.Dispose()
    }
}

function Save-LegacyIcon([string]$path, [int]$size, [bool]$round) {
    $canvas = New-Canvas $size
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $clip = if ($round) {
        $ellipse = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $ellipse.AddEllipse(0, 0, $size, $size)
        $ellipse
    } else {
        New-RoundedPath $size ($size * 0.22)
    }
    $background = [System.Drawing.SolidBrush]::new(
        [System.Drawing.ColorTranslator]::FromHtml('#151820'))
    try {
        Configure-Graphics $graphics
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.SetClip($clip)
        $graphics.FillRectangle($background, 0, 0, $size, $size)
        $graphics.DrawImage($sourceImage, 0, 0, $size, $size)
        $graphics.ResetClip()
        $canvas.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $background.Dispose()
        $clip.Dispose()
        $graphics.Dispose()
        $canvas.Dispose()
    }
}

try {
    foreach ($entry in $densities.GetEnumerator()) {
        $density = $entry.Key
        $legacySize = [int]$entry.Value
        $foregroundSize = [int][Math]::Round($legacySize * 2.25)
        $directory = Join-Path $ResRoot "mipmap-$density"
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        Save-LegacyIcon (Join-Path $directory 'ic_launcher.png') $legacySize $false
        Save-LegacyIcon (Join-Path $directory 'ic_launcher_round.png') $legacySize $true
        Save-ResizedForeground (Join-Path $directory 'ic_launcher_foreground.png') `
            $foregroundSize
    }
} finally {
    $sourceImage.Dispose()
}

Write-Output "A9TAS_ANDROID_ICON_V2 passed=1 mark=TAS source=$sourcePath densities=$($densities.Count)"
