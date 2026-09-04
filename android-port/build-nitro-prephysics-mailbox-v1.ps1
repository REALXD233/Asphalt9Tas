param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\nitro_prephysics_mailbox_selftest_v1.cpp"
$outDir = Join-Path $root "build\nitro-prephysics-mailbox-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$x64 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$arm64 = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
if (-not (Test-Path -LiteralPath $x64) -or
    -not (Test-Path -LiteralPath $arm64)) {
    throw "Required NDK compilers are unavailable"
}

$common = @("-O2", "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-I$(Join-Path $root 'src')")
$x64Out = Join-Path $outDir "a9tas_nitro_prephysics_mailbox_selftest_v1"
$arm64Object = Join-Path $outDir "nitro_prephysics_mailbox_selftest_v1.arm64.o"

& $x64 $source @common "-static" "-fPIE" "-o" $x64Out
if ($LASTEXITCODE -ne 0) { throw "x86_64 mailbox selftest build failed" }
& $arm64 $source @common "-c" "-o" $arm64Object
if ($LASTEXITCODE -ne 0) { throw "ARM64 mailbox compile check failed" }

$readelf = Join-Path $toolBin "llvm-readelf.exe"
$x64Header = (& $readelf -h $x64Out) -join "`n"
$arm64Header = (& $readelf -h $arm64Object) -join "`n"
if ($x64Header -notmatch "Machine:\s+Advanced Micro Devices X86-64") {
    throw "Unexpected x86_64 ELF architecture"
}
if ($arm64Header -notmatch "Machine:\s+AArch64") {
    throw "Unexpected ARM64 object architecture"
}

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source (Join-Path $root "tools\test_nitro_prephysics_mailbox_v1.py")
if ($LASTEXITCODE -ne 0) { throw "Mailbox reference-model tests failed" }

$x64Hash = (Get-FileHash -LiteralPath $x64Out -Algorithm SHA256).Hash.ToLower()
$arm64Hash = (Get-FileHash -LiteralPath $arm64Object -Algorithm SHA256).Hash.ToLower()
Write-Output "NITRO_PREPHYSICS_MAILBOX_V1_BUILD passed=1 python_tests=5"
Write-Output "x86_64_sha256=$x64Hash"
Write-Output "arm64_object_sha256=$arm64Hash"
