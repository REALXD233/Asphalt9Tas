param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $projectRoot "src"
$outputDir = Join-Path $projectRoot "build"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$common = @(
    "-shared", "-fPIC", "-O2", "-std=c++20",
    "-static-libstdc++",
    "-fvisibility=hidden", "-fno-exceptions", "-fno-rtti",
    "-Wl,--build-id=sha1", "-Wl,--gc-sections", "-llog", "-ldl"
)

$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$arm64Compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"

& $x64Compiler @common (Join-Path $sourceDir "bootstrap.cpp") `
    -o (Join-Path $outputDir "liba9tas_bootstrap.so")
if ($LASTEXITCODE -ne 0) { throw "x86_64 bootstrap build failed" }

& $arm64Compiler @common (Join-Path $sourceDir "payload.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload.so")
if ($LASTEXITCODE -ne 0) { throw "AArch64 payload build failed" }

& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" (Join-Path $sourceDir "loader.cpp") "-ldl" `
    -o (Join-Path $outputDir "a9tas_loader_x86_64")
if ($LASTEXITCODE -ne 0) { throw "x86_64 loader build failed" }

& $arm64Compiler "-O2" "-std=c++20" "-static-libstdc++" (Join-Path $sourceDir "loader.cpp") "-ldl" `
    -o (Join-Path $outputDir "a9tas_loader_arm64")
if ($LASTEXITCODE -ne 0) { throw "AArch64 loader build failed" }

& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    (Join-Path $sourceDir "ptrace_probe.cpp") `
    -o (Join-Path $outputDir "a9tas_ptrace_probe")
if ($LASTEXITCODE -ne 0) { throw "x86_64 ptrace probe build failed" }

& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    (Join-Path $sourceDir "injector.cpp") "-ldl" `
    -o (Join-Path $outputDir "a9tas_injector")
if ($LASTEXITCODE -ne 0) { throw "x86_64 injector build failed" }

Write-Output "Built $outputDir"
