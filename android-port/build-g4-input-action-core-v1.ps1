param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\g4_input_action_core_selftest_v1.cpp"
$header = Join-Path $portRoot "src\g4_input_action_core_v1.h"
$adapterSource = Join-Path $portRoot "src\g4_g3_adapter_selftest_v1.cpp"
$adapterHeader = Join-Path $portRoot "src\g4_g3_adapter_v1.h"
$protocolSource = Join-Path $portRoot "src\g4_multi_hook_runtime_selftest_v1.cpp"
$protocolHeader = Join-Path $portRoot "src\g4_multi_hook_runtime_v1.h"
$g3Adapter = Join-Path $portRoot "src\g3_boundary_adapter_v1.h"
$g3Coordinator = Join-Path $portRoot "src\g3_tick_coordinator_v1.h"
$recording = Join-Path $portRoot "src\unified_tick_recording_v1.h"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$outDir = Join-Path $portRoot "build\g4-input-action-core-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$armCompiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"

foreach ($path in @($source, $header, $adapterSource, $adapterHeader,
                     $protocolSource, $protocolHeader,
                     $g3Adapter, $g3Coordinator, $recording,
                     $baselineVerifier, $baseline, $armCompiler,
                     $x86Compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G4 input/action build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$common = @(
    $source, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror"
)
$armObject = Join-Path $outDir "g4_input_action_core_selftest_v1.arm64.o"
$x86Selftest = Join-Path $outDir "g4_input_action_core_selftest_v1"
$adapterArmObject = Join-Path $outDir "g4_g3_adapter_selftest_v1.arm64.o"
$adapterSelftest = Join-Path $outDir "g4_g3_adapter_selftest_v1"
$protocolArmObject = Join-Path $outDir "g4_multi_hook_runtime_selftest_v1.arm64.o"
$protocolSelftest = Join-Path $outDir "g4_multi_hook_runtime_selftest_v1"
& $armCompiler @common "-c" "-o" $armObject
if ($LASTEXITCODE -ne 0) { throw "G4 ARM64 semantic core build failed" }
& $x86Compiler @common "-static-libstdc++" "-Wl,--build-id=sha1" `
    "-o" $x86Selftest
if ($LASTEXITCODE -ne 0) { throw "G4 x86_64 semantic selftest link failed" }
$adapterCommon = @(
    $adapterSource, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror"
)
& $armCompiler @adapterCommon "-c" "-o" $adapterArmObject
if ($LASTEXITCODE -ne 0) { throw "G4/G3 adapter ARM64 build failed" }
& $x86Compiler @adapterCommon "-static-libstdc++" "-Wl,--build-id=sha1" `
    "-o" $adapterSelftest
if ($LASTEXITCODE -ne 0) { throw "G4/G3 adapter x86_64 link failed" }
$protocolCommon = @(
    $protocolSource, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror"
)
& $armCompiler @protocolCommon "-c" "-o" $protocolArmObject
if ($LASTEXITCODE -ne 0) { throw "G4 multi-hook protocol ARM64 build failed" }
& $x86Compiler @protocolCommon "-static-libstdc++" "-Wl,--build-id=sha1" `
    "-o" $protocolSelftest
if ($LASTEXITCODE -ne 0) { throw "G4 multi-hook protocol x86_64 link failed" }

python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "known-good v2 baseline drift" }

$forbidden = Select-String -LiteralPath @($header, $source, $adapterHeader,
                                           $adapterSource, $protocolHeader,
                                           $protocolSource) -Pattern `
    '\bptrace\b|\bprocess_vm_writev\b|/proc/|\bmprotect\b|\badb\b|\bWriteProcessMemory\b'
if ($forbidden) { throw "G4 pure semantic core contains runtime transport" }

$required = @(
    'IntervalSampleV1', 'BeginTick', 'ConsumeAcceleratorAfterOriginal',
    'OnNaturalNitroCall', 'AccountInjectedNitroCalls',
    'OnPhysicsIntervalAfterOriginal', 'IntervalAfterQualified', 'EndTick',
    'CapturePhysicsSnapshot', 'BuildPhysicsRecordingFrame',
    'PhysicsRecordingFrameValid', 'ComponentFloatRangesEqual',
    'DecidePhysicsCorrection', 'kSkipSteer',
    'kSkipBrake', 'kSkipNitroActivation', 'kSkipAccelerator'
)
$combined = (Get-Content -LiteralPath $header -Raw) +
            (Get-Content -LiteralPath $source -Raw) +
            (Get-Content -LiteralPath $adapterHeader -Raw) +
            (Get-Content -LiteralPath $adapterSource -Raw) +
            (Get-Content -LiteralPath $protocolHeader -Raw) +
            (Get-Content -LiteralPath $protocolSource -Raw)
foreach ($token in $required) {
    if (-not $combined.Contains($token)) {
        throw "Missing G4 semantic token: $token"
    }
}

$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterHeaderHash = (Get-FileHash -LiteralPath $adapterHeader -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterSourceHash = (Get-FileHash -LiteralPath $adapterSource -Algorithm SHA256).Hash.ToLowerInvariant()
$armHash = (Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -LiteralPath $x86Selftest -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterArmHash = (Get-FileHash -LiteralPath $adapterArmObject -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterSelftestHash = (Get-FileHash -LiteralPath $adapterSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolHeaderHash = (Get-FileHash -LiteralPath $protocolHeader -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolSourceHash = (Get-FileHash -LiteralPath $protocolSource -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolArmHash = (Get-FileHash -LiteralPath $protocolArmObject -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolSelftestHash = (Get-FileHash -LiteralPath $protocolSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "G4_INPUT_ACTION_CORE_BUILD passed=1 arm64=1 x86_64=1 runtime_selftest=NOT_RUN device_access=0 deployed=0"
Write-Output "semantics=controls,fixed_delta,interval_stream,nitro,accelerator"
Write-Output "header_sha256=$headerHash"
Write-Output "selftest_sha256=$sourceHash"
Write-Output "arm64_object_sha256=$armHash"
Write-Output "x86_64_selftest_sha256=$selftestHash"
Write-Output "adapter_header_sha256=$adapterHeaderHash"
Write-Output "adapter_selftest_sha256=$adapterSourceHash"
Write-Output "adapter_arm64_object_sha256=$adapterArmHash"
Write-Output "adapter_x86_64_selftest_sha256=$adapterSelftestHash"
Write-Output "protocol_header_sha256=$protocolHeaderHash"
Write-Output "protocol_selftest_sha256=$protocolSourceHash"
Write-Output "protocol_arm64_object_sha256=$protocolArmHash"
Write-Output "protocol_x86_64_selftest_sha256=$protocolSelftestHash"
