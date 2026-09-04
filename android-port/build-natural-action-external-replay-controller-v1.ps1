param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_natural_action_external_replay_v1_build.cpp"
$payload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$audit = Join-Path $root "tools\audit_natural_action_external_replay_controller_v1.py"
$outDir = Join-Path $root "build\natural-action-external-replay-controller-v1"
$object = Join-Path $outDir "natural_action_external_replay_controller_v1_review_only.o"
$candidate = Join-Path $outDir "natural_action_external_replay_controller_v1_review_only"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_nar6_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($path in @($source, $bootstrapSource, $payload, $audit, $compiler,
                     $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing external replay controller input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_NAL_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_ACTION_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW=1" `
    "-I$(Join-Path $root 'src')" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "external replay controller object build failed" }
& $compiler $bootstrapSource "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-I$(Join-Path $root 'src')" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "external replay controller candidate build failed" }
& $compiler $bootstrapSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "external replay bootstrap build failed" }
python -B $audit $object $candidate $bootstrap $payload $readelf $source $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "external replay controller audit failed" }
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_EXTERNAL_REPLAY_CONTROLLER_BUILD passed=1 mailbox_initially_empty=1 external_frame_count=1 natural_cleanup=1 deployed=0 device_access=0"
Write-Output "review_object_sha256=$objectHash"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
