# Offline-only bridge from a proven A9USR5/A9UTK1 source pair to the exact
# SHA-bound A9FWT1 consumed by lifecycle final-writer replay.

param(
    [ValidateSet("ValidateTooling", "PrepareArtifacts")]
    [string]$Mode = "ValidateTooling",
    [string]$RecordingPath = "",
    [string]$SourceReportPath = "",
    [string]$TargetPath = "",
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$validator = Join-Path $root "tools\lifecycle_source_recording_v1.py"
$physicalValidator = Join-Path $root "tools\synchronized_brake_recording_v1.py"
$generator = Join-Path $root "tools\make_final_writer_target_blob_v1.py"
$pins = @{
    $validator = "0185f9ceca7b720bdc6d3a9caab20a47c4dba76d272d8b59eaa8b2d0e0937b55"
    $physicalValidator = "01a66fe65543d0a5882920aa3dde94251e75057ce1e11da5e5abea189f09fc22"
    $generator = "fab3756e85a6933d92c97bafd2a8792b83cd2c24d0fcb83a7013ada74efcddf8"
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Sha $path) -ne $pins[$path]) {
        throw "Lifecycle replay artifact tooling pin mismatch: $path"
    }
}
if ($Mode -eq "ValidateTooling") {
    Write-Output "LIFECYCLE_REPLAY_ARTIFACT_TOOLING_VALID passed=1 writes=0 device_access=0"
    return
}

if ($RecordingPath -eq "" -or $SourceReportPath -eq "") {
    throw "PrepareArtifacts requires RecordingPath and SourceReportPath"
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$SourceReportPath = [IO.Path]::GetFullPath($SourceReportPath)
foreach ($path in @($RecordingPath, $SourceReportPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing source artifact: $path" }
}
if ($TargetPath -eq "") { $TargetPath = [IO.Path]::ChangeExtension($RecordingPath, ".a9fwt1") }
if ($ManifestPath -eq "") { $ManifestPath = "$TargetPath.manifest.json" }
$TargetPath = [IO.Path]::GetFullPath($TargetPath)
$ManifestPath = [IO.Path]::GetFullPath($ManifestPath)
foreach ($path in @($TargetPath, $ManifestPath)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing output: $path" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
}

python -B $validator $SourceReportPath $RecordingPath
if ($LASTEXITCODE -ne 0) { throw "A9USR5/A9UTK1 lifecycle source validation failed" }
python -B $generator $RecordingPath $TargetPath
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $TargetPath -PathType Leaf)) {
    throw "A9FWT1 lifecycle replay target generation failed"
}
$recordingHash = Get-Sha $RecordingPath
$sourceReportHash = Get-Sha $SourceReportPath
$targetHash = Get-Sha $TargetPath
$recordingBytes = [IO.File]::ReadAllBytes($RecordingPath)
$targetBytes = [IO.File]::ReadAllBytes($TargetPath)
if ($recordingBytes.Length -ne 96 + 360 * 144 -or
    [BitConverter]::ToUInt32($recordingBytes, 20) -ne 360 -or
    $targetBytes.Length -ne 128 + 360 * 80 -or
    [BitConverter]::ToUInt32($targetBytes, 20) -ne 360) {
    Remove-Item -LiteralPath $TargetPath -Force
    throw "Lifecycle replay artifacts are not an exact 360-frame pair"
}
$recordingDigest = [Convert]::FromHexString($recordingHash)
for ($index = 0; $index -lt 32; ++$index) {
    if ($targetBytes[40 + $index] -ne $recordingDigest[$index]) {
        Remove-Item -LiteralPath $TargetPath -Force
        throw "Generated A9FWT1 is not SHA-bound to A9UTK1"
    }
}
$manifest = [ordered]@{
    version = 1
    semantic_origin = "authoritative_race_lifecycle_2_to_3"
    frames = 360
    fixed_interval_us = 16667
    recording_path = $RecordingPath
    recording_sha256 = $recordingHash
    source_report_path = $SourceReportPath
    source_report_sha256 = $sourceReportHash
    target_path = $TargetPath
    target_sha256 = $targetHash
    device_access = 0
}
[IO.File]::WriteAllText(
    $ManifestPath,
    ($manifest | ConvertTo-Json -Depth 3) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
Write-Output "LIFECYCLE_REPLAY_ARTIFACTS_PREPARED passed=1 frames=360 device_access=0"
Write-Output "recording_sha256=$recordingHash"
Write-Output "source_report_sha256=$sourceReportHash"
Write-Output "target_sha256=$targetHash"
Write-Output "manifest=$ManifestPath"
