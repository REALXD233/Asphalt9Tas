param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_game_action_submission_affinity_v1.cpp"
$outputDir = Join-Path $root "build\game-action-submission-affinity-v1"
$output = Join-Path $outputDir "a9tas_hwbp_game_action_submission_affinity_v1"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"

if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "x86_64 compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-pie" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" $source "-o" $output
if ($LASTEXITCODE -ne 0) {
    throw "game action submission-affinity observer build failed (exit=$LASTEXITCODE)"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Built $output"
Write-Host "scope=observe-only game_calls=0 input_writes=0 guest_patches=0"
Write-Host "SHA256 $hash"
