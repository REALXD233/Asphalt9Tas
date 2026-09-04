param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\camera_raceview_phase_sampler_v1.cpp"
$policy = Join-Path $root "tools\test_camera_raceview_phase_policy_v1.py"
$outDir = Join-Path $root "build\camera-raceview-phase-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$x86 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$output = Join-Path $outDir "a9tas_camera_raceview_phase_sampler_v1_review_only"

foreach ($path in @($source, $policy, $x86)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing RaceView phase input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $x86 $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "RaceView phase sampler build failed" }

python -B $policy $output
if ($LASTEXITCODE -ne 0) { throw "RaceView phase policy failed" }

Write-Output "CAMERA_RACEVIEW_PHASE_BUILD passed=1 read_only=1 x86_64=1 device_access=0 deployed=0"
Write-Output "source_sha256=$((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "review_binary_sha256=$((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant())"
