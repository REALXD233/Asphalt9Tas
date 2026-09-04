param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $projectRoot "src"
$outputDir = Join-Path $projectRoot "build\h0"
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"

if (-not (Test-Path $x64Compiler)) {
    Write-Error "NDK x86_64 compiler not found: $x64Compiler"
    exit 1
}

$targetExe = Join-Path $outputDir "remote_call_selftest_target"
$controllerExe = Join-Path $outputDir "h0_remote_call_selftest"

# Build the target executable (Linux x86_64 via NDK)
Write-Host "Building H0 target: $targetExe"
$targetCmd = "`"$x64Compiler`" `"$sourceDir\remote_call_selftest_target.cpp`" -O2 -std=c++20 -static -fPIE -o `"$targetExe`" -ldl"
Write-Host "CMD: $targetCmd"
& $x64Compiler "$sourceDir\remote_call_selftest_target.cpp" -O2 -std=c++20 -static -fPIE -o "$targetExe" -ldl
$targetExit = $LASTEXITCODE
if ($targetExit -ne 0) { throw "H0 target build failed with exit=$targetExit" }

# Build the controller executable (Linux x86_64 via NDK)
Write-Host "Building H0 controller: $controllerExe"
$controllerCmd = "`"$x64Compiler`" `"$projectRoot\h0_remote_call_selftest.cpp`" -O2 -std=c++20 -static -fPIE -I`"$sourceDir`" -o `"$controllerExe`" -ldl"
Write-Host "CMD: $controllerCmd"
& $x64Compiler "$projectRoot\h0_remote_call_selftest.cpp" -O2 -std=c++20 -static -fPIE -I"$sourceDir" -o "$controllerExe" -ldl
$controllerExit = $LASTEXITCODE
if ($controllerExit -ne 0) { throw "H0 controller build failed with exit=$controllerExit" }

# Compute SHA-256
function Get-FileHash256($path) {
    if (Test-Path $path) {
        $hash = Get-FileHash -Path $path -Algorithm SHA256 | Select-Object -ExpandProperty Hash
        return $hash.ToLower()
    }
    return "FILE_NOT_FOUND"
}

$targetHash = Get-FileHash256 $targetExe
$controllerHash = Get-FileHash256 $controllerExe

Write-Output "BUILD target_exit=$targetExit controller_exit=$controllerExit"
Write-Output "SHA256 target=$targetHash"
Write-Output "SHA256 controller=$controllerHash"
Write-Output "H0 build complete: target=$targetExe controller=$controllerExe"
