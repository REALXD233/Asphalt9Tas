param(
    [string]$SourceRecording = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($SourceRecording -eq "") {
    $SourceRecording = Join-Path $root "evidence\a9tas_lifecycle_source_900f_20260822_121419_985.a9utk1"
}
if ($OutputDirectory -eq "") {
    $OutputDirectory = Join-Path $root "build\m1-five-frame-gate-v1"
}
$SourceRecording = [IO.Path]::GetFullPath($SourceRecording)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$tool = Join-Path $root "tools\make_m1_gate_prefix_v1.py"
$expectedSourceHash = "727e850fac73cdcf69d5eff1684f946dd129b542e72cf7d34a67fd379d47229b"
$recording = Join-Path $OutputDirectory "a9tas_m1_gate_5f.a9utk1"
$target = Join-Path $OutputDirectory "a9tas_m1_gate_5f.a9fwt1"

foreach ($path in @($SourceRecording, $tool)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing M1 gate prefix input: $path"
    }
}
$sourceHash = (Get-FileHash -LiteralPath $SourceRecording -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sourceHash -ne $expectedSourceHash) {
    throw "M1 gate source hash mismatch"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
foreach ($path in @($recording, $target)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source -B $tool $SourceRecording 5 $recording $target
if ($LASTEXITCODE -ne 0) { throw "M1 five-frame prefix generation failed" }

$recordingBytes = [IO.File]::ReadAllBytes($recording)
$targetBytes = [IO.File]::ReadAllBytes($target)
if ($recordingBytes.Length -ne 96 + 5 * 144 -or
    [BitConverter]::ToUInt32($recordingBytes, 20) -ne 5 -or
    [BitConverter]::ToUInt64($recordingBytes, 96) -ne 0 -or
    $targetBytes.Length -ne 128 + 5 * 80 -or
    [BitConverter]::ToUInt32($targetBytes, 20) -ne 5) {
    throw "M1 five-frame artifact shape mismatch"
}
$recordingHash = (Get-FileHash -LiteralPath $recording -Algorithm SHA256).Hash.ToLowerInvariant()
$targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
$digest = New-Object byte[] 32
for ($index = 0; $index -lt $digest.Length; ++$index) {
    $digest[$index] = [Convert]::ToByte($recordingHash.Substring($index * 2, 2), 16)
}
for ($index = 0; $index -lt 32; ++$index) {
    if ($targetBytes[40 + $index] -ne $digest[$index]) {
        throw "M1 target is not SHA-bound to its five-frame recording"
    }
}
Write-Output "M1_FIVE_FRAME_GATE_ARTIFACTS passed=1 frames=5 first_tick=0 fixed_interval_us=16667 target_bound=1 device_access=0"
Write-Output "recording=$recording"
Write-Output "recording_sha256=$recordingHash"
Write-Output "target=$target"
Write-Output "target_sha256=$targetHash"
