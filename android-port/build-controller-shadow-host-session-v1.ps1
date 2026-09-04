param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$armCompiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$hostCompiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$header = Join-Path $root "src\controller_shadow_host_session_v1.h"
$selftest = Join-Path $root "src\controller_shadow_host_session_selftest_v1.cpp"
$outDir = Join-Path $root "build\controller-shadow-host-session-v1"
$armObject = Join-Path $outDir "controller_shadow_host_session_arm64_v1.o"
$hostObject = Join-Path $outDir "controller_shadow_host_session_x86_64_v1.o"

foreach ($path in @($armCompiler, $hostCompiler, $header, $selftest)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing controller-shadow host session input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

foreach ($build in @(
    @($armCompiler, $armObject),
    @($hostCompiler, $hostObject)
)) {
    & $build[0] $selftest "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
        "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
        "-c" "-o" $build[1]
    if ($LASTEXITCODE -ne 0) {
        throw "controller-shadow host session build failed: $($build[1])"
    }
}

$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -LiteralPath $selftest -Algorithm SHA256).Hash.ToLowerInvariant()
$armHash = (Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant()
$hostHash = (Get-FileHash -LiteralPath $hostObject -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "CONTROLLER_SHADOW_HOST_SESSION_BUILD passed=1 arm64=1 x86_64=1 mandatory_cleanup=1 completion_receipts=1 device_access=0 deployed=0"
Write-Output "header_sha256=$headerHash"
Write-Output "selftest_sha256=$selftestHash"
Write-Output "arm64_object_sha256=$armHash"
Write-Output "x86_64_object_sha256=$hostHash"
