param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_startline_unified_replay_v1.cpp"
$protocol = Join-Path $root "src\startline_prearm_protocol_v1.cpp"
$outputDirectory = Join-Path $root "build\startline-unified-replay-v1"
$output = Join-Path $outputDirectory "a9tas_hwbp_startline_unified_replay_v1"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $protocol, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing unified startline build input: $path" }
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" "-DA9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN=1" $source $protocol "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "startline unified replay build failed" }
Write-Host "STARTLINE_UNIFIED_REPLAY_BUILD passed=1 deployed=0"
Write-Host "binary=$output"
Write-Host "sha256=$((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower())"
