param([string]$NdkRoot="C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference="Stop"
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$source=Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$output=Join-Path $root "build\staging\a9tas_hwbp_natural_preroll_search_only_v1"
$compiler=Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
if(-not(Test-Path -LiteralPath $compiler)){throw "x86_64 compiler not found"}
New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force|Out-Null
& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_REPLAY_ALIGNMENT_GUARD_V1" "-DA9TAS_NATURAL_PREROLL_REPLAY_V1" `
    "-DA9TAS_NATURAL_PREROLL_SEARCH_ONLY_V1" $source "-o" $output
if($LASTEXITCODE -ne 0){throw "natural pre-roll search-only build failed"}
$hash=(Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash.ToLowerInvariant()
Write-Host "Built $output";Write-Host "SHA256 $hash"
