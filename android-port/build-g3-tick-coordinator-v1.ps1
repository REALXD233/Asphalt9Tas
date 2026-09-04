param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\g3_tick_coordinator_selftest_v1.cpp"
$header = Join-Path $portRoot "src\g3_tick_coordinator_v1.h"
$adapterSource = Join-Path $portRoot "src\g3_boundary_adapter_selftest_v1.cpp"
$adapterHeader = Join-Path $portRoot "src\g3_boundary_adapter_v1.h"
$recording = Join-Path $portRoot "src\unified_tick_recording_v1.h"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$outDir = Join-Path $portRoot "build\g3-tick-coordinator-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$armCompiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$objdump = Join-Path $toolBin "llvm-objdump.exe"

foreach ($path in @($source, $header, $adapterSource, $adapterHeader,
                     $recording, $baselineVerifier,
                     $baseline, $armCompiler, $x86Compiler, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G3 coordinator build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$common = @(
    $source, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror"
)
$armObject = Join-Path $outDir "g3_tick_coordinator_selftest_v1.arm64.o"
$x86Object = Join-Path $outDir "g3_tick_coordinator_selftest_v1.x86_64.o"
$x86Selftest = Join-Path $outDir "g3_tick_coordinator_selftest_v1"
$adapterArmObject = Join-Path $outDir "g3_boundary_adapter_selftest_v1.arm64.o"
$adapterSelftest = Join-Path $outDir "g3_boundary_adapter_selftest_v1"
& $armCompiler @common "-c" "-o" $armObject
if ($LASTEXITCODE -ne 0) { throw "G3 ARM64 coordinator build failed" }
& $x86Compiler @common "-c" "-o" $x86Object
if ($LASTEXITCODE -ne 0) { throw "G3 x86_64 review build failed" }
& $x86Compiler @common "-static-libstdc++" "-Wl,--build-id=sha1" `
    "-o" $x86Selftest
if ($LASTEXITCODE -ne 0) { throw "G3 x86_64 selftest link failed" }
$adapterCommon = @(
    $adapterSource, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror"
)
& $armCompiler @adapterCommon "-c" "-o" $adapterArmObject
if ($LASTEXITCODE -ne 0) { throw "G3 boundary adapter ARM64 build failed" }
& $x86Compiler @adapterCommon "-static-libstdc++" "-Wl,--build-id=sha1" `
    "-o" $adapterSelftest
if ($LASTEXITCODE -ne 0) { throw "G3 boundary adapter selftest link failed" }

$disassembly = Join-Path $outDir "g3_tick_coordinator_selftest_v1.arm64.disasm.txt"
& $objdump "-d" "--demangle" "--no-show-raw-insn" $armObject |
    Set-Content -LiteralPath $disassembly -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "G3 coordinator disassembly failed" }

python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "known-good v2 baseline drift" }

$forbidden = Select-String -LiteralPath @($header, $source, $adapterHeader,
                                           $adapterSource) -Pattern `
    'ptrace|process_vm_writev|/proc/|mprotect|adb|system\(|CreateProcess|WriteProcessMemory'
if ($forbidden) { throw "G3 pure coordinator contains runtime transport" }

$required = @(
    'Lifecycle::kArmed', 'Lifecycle::kInRace', 'Lifecycle::kEnding',
    'ReplayMode::kActiveBlock', 'kNeedReplayData', 'kPausedNoAdvance',
    'OnRaceStart', 'BeginTick', 'OnPrePhysics', 'OnFinalWriter',
    'OnTickEnd', 'BeginRaceEnd', 'FinishRaceEnd', 'ActivePacket'
)
$combined = (Get-Content -LiteralPath $header -Raw) +
            (Get-Content -LiteralPath $source -Raw)
foreach ($token in $required) {
    if (-not $combined.Contains($token)) { throw "Missing G3 semantic token: $token" }
}

$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterHeaderHash = (Get-FileHash -LiteralPath $adapterHeader -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterSourceHash = (Get-FileHash -LiteralPath $adapterSource -Algorithm SHA256).Hash.ToLowerInvariant()
$armHash = (Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant()
$x86Hash = (Get-FileHash -LiteralPath $x86Object -Algorithm SHA256).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -LiteralPath $x86Selftest -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterArmHash = (Get-FileHash -LiteralPath $adapterArmObject -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterSelftestHash = (Get-FileHash -LiteralPath $adapterSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "G3_TICK_COORDINATOR_BUILD passed=1 arm64=1 x86_64=1 device_access=0 deployed=0"
Write-Output "lifecycle=Inactive-Armed-InRace-Ending-Inactive faulted=1"
Write-Output "boundaries=race,tick_begin,pre_physics,final_writer,tick_end"
Write-Output "neutral_selftest_cases=9 runtime_selftest=NOT_RUN"
Write-Output "header_sha256=$headerHash"
Write-Output "selftest_sha256=$sourceHash"
Write-Output "arm64_object_sha256=$armHash"
Write-Output "x86_64_object_sha256=$x86Hash"
Write-Output "x86_64_selftest_sha256=$selftestHash"
Write-Output "adapter_header_sha256=$adapterHeaderHash"
Write-Output "adapter_selftest_source_sha256=$adapterSourceHash"
Write-Output "adapter_arm64_object_sha256=$adapterArmHash"
Write-Output "adapter_x86_64_selftest_sha256=$adapterSelftestHash"
