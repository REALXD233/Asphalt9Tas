param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$validator = Join-Path $root "tools\validate_natural_action_replay_sequence_report_v1.py"
$audit = Join-Path $root "tools\audit_natural_action_replay_sequence_controller_v1.py"
$payload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$outDir = Join-Path $root "build\natural-action-replay-sequence-controller-v1"
$object = Join-Path $outDir "natural_action_replay_sequence_controller_v1_review_only.o"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($path in @($source, $validator, $audit, $payload, $compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing replay-sequence controller input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_NAL_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_ACTION_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW=1" `
    "-I$(Join-Path $root 'src')" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "replay-sequence controller build failed" }
python -B $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "replay-sequence validator failed" }
python -B $audit $object $payload $readelf $source $validator $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "replay-sequence controller audit failed" }
$hash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_REPLAY_SEQUENCE_CONTROLLER_BUILD passed=1 frames=5 sequence=0_1_0_2_0 action_calls=3 linked_live=0 deployed=0 device_access=0"
Write-Output "review_object_sha256=$hash"
