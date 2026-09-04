# Staging build for the PhysicsDispatchProbe first-round safe test.
# Builds ONLY the modified payload variant into build/staging/ with an
# independent name. Never overwrites build/liba9tas_payload.so.
param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $projectRoot "src"
$outputDir = Join-Path $projectRoot "build\staging"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$common = @(
    "-shared", "-fPIC", "-O2", "-std=c++20",
    "-static-libstdc++",
    "-fvisibility=hidden", "-fno-exceptions", "-fno-rtti",
    "-Wl,--build-id=sha1", "-Wl,--gc-sections", "-llog", "-ldl"
)

$arm64Compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"

& $arm64Compiler @common (Join-Path $sourceDir "payload_probe_dev.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload_probe_dev.so")
if ($LASTEXITCODE -ne 0) { throw "staging payload build failed" }

& $arm64Compiler @common (Join-Path $sourceDir "payload_control_values_probe.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload_control_values_probe.so")
if ($LASTEXITCODE -ne 0) { throw "control-values probe build failed" }

& $arm64Compiler @common (Join-Path $sourceDir "payload_action_subscriber_probe.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload_action_subscriber_probe.so")
if ($LASTEXITCODE -ne 0) { throw "action-subscriber probe build failed" }

& $arm64Compiler @common (Join-Path $sourceDir "payload_input_receiver_probe.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload_input_receiver_probe.so")
if ($LASTEXITCODE -ne 0) { throw "input-receiver probe build failed" }

& $arm64Compiler @common (Join-Path $sourceDir "payload_keyboard_action_probe.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload_keyboard_action_probe.so")
if ($LASTEXITCODE -ne 0) { throw "keyboard-action probe build failed" }

# Phase A13: the main TAS payload (keyboard injection + sequence + recording + RNG + interval)
& $arm64Compiler @common (Join-Path $sourceDir "payload_phase_a13.cpp") `
    -o (Join-Path $outputDir "liba9tas_payload_a13.so")
if ($LASTEXITCODE -ne 0) { throw "a13 payload build failed" }

Write-Host "a13 SHA256: $((Get-FileHash (Join-Path $outputDir 'liba9tas_payload_a13.so') -Algorithm SHA256).Hash.ToLower())"

# Isolated ARM64 self-test of the probe mechanism (runs directly on the guest
# via Houdini; never touches the game process).
& $arm64Compiler "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-fno-exceptions" "-fno-rtti" `
    "-Wl,--build-id=sha1" (Join-Path $sourceDir "probe_selftest.cpp") `
    -o (Join-Path $outputDir "a9tas_probe_selftest")
if ($LASTEXITCODE -ne 0) { throw "selftest build failed" }

Write-Output "Built $outputDir"
