param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_nitro_thread_affinity_observer_v1.cpp"
$output = Join-Path $root "build\staging\a9tas_hwbp_nitro_thread_affinity_observer_v1"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"

if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "x86_64 compiler not found: $compiler"
}
New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force |
    Out-Null

& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-pie" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" $source "-o" $output
if ($LASTEXITCODE -ne 0) {
    throw "nitro thread-affinity observer build failed (exit=$LASTEXITCODE)"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Built $output"
Write-Host "SHA256 $hash"
