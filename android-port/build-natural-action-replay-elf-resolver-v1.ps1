param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_replay_elf_resolver_build_only.cpp"
$header = Join-Path $root "src\natural_action_lifecycle_elf_resolver_v1.h"
$payload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$policy = Join-Path $root "tools\test_natural_action_replay_elf_resolver_v1.py"
$outDir = Join-Path $root "build\natural-action-replay-elf-resolver-v1"
$output = Join-Path $outDir "natural_action_replay_elf_resolver_build_only.o"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $header, $payload, $policy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing natural-action replay resolver input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-I$(Join-Path $root 'src')" `
    "-c" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "natural-action replay resolver build failed" }
python -B $policy $payload
if ($LASTEXITCODE -ne 0) { throw "natural-action replay resolver policy failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_REPLAY_ELF_RESOLVER_BUILD passed=1 hash_pinned=1 build_id_pinned=1 deployed=0 device_access=0"
Write-Output "review_object_sha256=$hash"
