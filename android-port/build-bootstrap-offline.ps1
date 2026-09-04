param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $projectRoot "src"
$outputDir = Join-Path $projectRoot "build\offline"
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
if (-not (Test-Path $x64Compiler)) { throw "NDK compiler not found: $x64Compiler" }

# 1. The real bootstrap injector (v8 int3 session path).
$injectorExe = Join-Path $projectRoot "build\a9tas_injector"
Write-Host "Building injector: $injectorExe"
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    (Join-Path $sourceDir "injector.cpp") "-ldl" -o $injectorExe
if ($LASTEXITCODE -ne 0) { throw "injector build failed" }

# 2. Dynamic PIE target (must NOT be static: the whole point is that the
#    target maps /system/lib64/libc.so and libdl.so like app_process64).
$targetExe = Join-Path $outputDir "bootstrap_selftest_target"
Write-Host "Building target: $targetExe"
& $x64Compiler "-O2" "-std=c++20" "-fPIE" "-static-libstdc++" `
    (Join-Path $sourceDir "bootstrap_selftest_target.cpp") "-ldl" -o $targetExe
if ($LASTEXITCODE -ne 0) { throw "target build failed" }

# 3. Marker library whose constructor writes the marker file.
$markerLib = Join-Path $outputDir "libbootstrap_marker_x86_64.so"
Write-Host "Building marker lib: $markerLib"
& $x64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    (Join-Path $sourceDir "bootstrap_marker_x86_64.cpp") -o $markerLib
if ($LASTEXITCODE -ne 0) { throw "marker lib build failed" }

function Get-Hash256($path) {
    if (Test-Path $path) {
        return (Get-FileHash -Path $path -Algorithm SHA256).Hash.ToLower()
    }
    return "FILE_NOT_FOUND"
}

Write-Output "BUILD injector=$injectorExe"
Write-Output "SHA256 injector=$(Get-Hash256 $injectorExe)"
Write-Output "SHA256 target=$(Get-Hash256 $targetExe)"
Write-Output "SHA256 marker=$(Get-Hash256 $markerLib)"
Write-Output "Offline bootstrap build complete"
