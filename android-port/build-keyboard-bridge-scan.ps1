param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\keyboard_bridge_scan.cpp"
$outputDir = Join-Path $projectRoot "build\staging"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$output = Join-Path $outputDir "a9tas_keyboard_bridge_scan_v7"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "NDK compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-pie" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" $source "-o" $output
if ($LASTEXITCODE -ne 0) { throw "keyboard bridge scan v7 build failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Built $output"
Write-Output "SHA256 $hash"
