param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_natural_action_replay_sequence_v1_build.cpp"
$payload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$validator = Join-Path $root "tools\validate_natural_action_replay_sequence_report_v1.py"
$audit = Join-Path $root "tools\audit_natural_action_replay_sequence_candidate_v1.py"
$outDir = Join-Path $root "build\natural-action-replay-sequence-live-candidate-v1"
$candidate = Join-Path $outDir "natural_action_replay_sequence_candidate_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_nar5_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($path in @($source, $bootstrapSource, $payload, $validator, $audit,
                     $compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing replay-sequence live-candidate input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-DA9TAS_NAL_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_ACTION_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW=1" `
    "-I$(Join-Path $root 'src')" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "linked replay-sequence candidate build failed" }
& $compiler $bootstrapSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "replay-sequence bootstrap build failed" }
python -B $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "replay-sequence validator failed" }
python -B $audit $candidate $bootstrap $payload $readelf $source `
    $bootstrapSource $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "replay-sequence candidate audit failed" }
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_REPLAY_SEQUENCE_LIVE_CANDIDATE_BUILD passed=1 linked=1 frames=5 sequence=0_1_0_2_0 action_calls=3 deployed=0 device_access=0"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
