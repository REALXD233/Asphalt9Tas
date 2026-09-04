param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = 'Stop'
$clang = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd'
$source = Join-Path $PSScriptRoot 'src\hwbp_scheduler_replay_v1.cpp'
$outDir = Join-Path $PSScriptRoot 'build\staging'
$output = Join-Path $outDir 'a9tas_hwbp_scheduler_replay_v1'

if (!(Test-Path -LiteralPath $clang)) { throw "missing compiler: $clang" }
if (!(Test-Path -LiteralPath $source)) { throw "missing source: $source" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& $clang '-O2' '-std=c++20' '-static-libstdc++' '-fPIE' '-pie' `
    '-Wall' '-Wextra' '-Werror' '-Wl,--build-id=sha1' $source '-o' $output
if ($LASTEXITCODE -ne 0) { throw "compile failed: $LASTEXITCODE" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "built=$output"
Write-Host "sha256=$hash"
