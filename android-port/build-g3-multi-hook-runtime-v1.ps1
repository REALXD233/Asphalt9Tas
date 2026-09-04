param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\payload_g3_multi_hook_runtime_v1.cpp"
$protocol = Join-Path $portRoot "src\g3_multi_hook_runtime_v1.h"
$adapter = Join-Path $portRoot "src\g3_boundary_adapter_v1.h"
$coordinator = Join-Path $portRoot "src\g3_tick_coordinator_v1.h"
$game = Join-Path (Split-Path -Parent $portRoot) "apk-analysis\lib\arm64-v8a\libAsphalt9.so"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"
$outDir = Join-Path $portRoot "build\g3-multi-hook-runtime-v1"
$payload = Join-Path $outDir "liba9tas_g3_multi_hook_runtime_v1.so"
$disassembly = Join-Path $outDir "g3_multi_hook_runtime_v1.disasm.txt"

foreach ($path in @($source, $protocol, $adapter, $coordinator, $game,
                     $baselineVerifier, $baseline, $compiler, $objdump,
                     $readelf, $nm)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G3 multi-hook build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$gameSha = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
if ($gameSha -ne '671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0') {
    throw "G3 exact game ELF identity mismatch"
}

# Android tick-begin is the live-proven Physics Interval boundary. The same
# wrapper performs BeginTick and PrePhysics before resuming the natural method.
$expectedGamePrologues = [ordered]@{
    0x3695474 = 'ffc300d1f55301a9f37b02a935119152'
    0x367D66C = 'ec0f17fceb2b016de923026dfc6f03a9'
    0x38B78E4 = 'ffc300d1f40b00f9f37b02a9280040f9'
}
$gameStream = [System.IO.File]::OpenRead($game)
try {
    foreach ($entry in $expectedGamePrologues.GetEnumerator()) {
        $rva = [Int64]$entry.Key
        $bytes = New-Object byte[] ($entry.Value.Length / 2)
        $gameStream.Position = $rva
        $read = $gameStream.Read($bytes, 0, $bytes.Length)
        $actual = ([System.BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
        if ($read -ne $bytes.Length -or $actual -ne $entry.Value) {
            throw ('G3 exact prologue mismatch at RVA 0x{0:X}: expected={1} actual={2}' -f `
                   $rva, $entry.Value, $actual)
        }
    }
} finally {
    $gameStream.Dispose()
}
& $compiler $source "-I$(Join-Path $portRoot 'src')" "-shared" "-fPIC" `
    "-O2" "-std=c++20" "-static-libstdc++" "-fno-exceptions" `
    "-fno-rtti" "-fno-stack-protector" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-Wl,--no-undefined" "-llog" "-ldl" `
    "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "G3 multi-hook ARM64 payload build failed" }

& $objdump "-d" "--demangle" "--no-show-raw-insn" $payload |
    Set-Content -LiteralPath $disassembly -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "G3 multi-hook disassembly failed" }

$symbols = (& $nm "-D" "--defined-only" $payload) -join "`n"
foreach ($symbol in @('a9tas_g3_command_v1', 'a9tas_g3_protocol_v1',
                       'a9tas_g3_control_data_v1',
                       'a9tas_g3_evidence_data_v1',
                       'a9tas_g3_runtime_data_v1')) {
    if ($symbols -notmatch [regex]::Escape($symbol)) {
        throw "Missing G3 payload symbol: $symbol"
    }
}

$dynamic = (& $readelf "--dyn-syms" $payload) -join "`n"
foreach ($forbidden in @('ptrace', 'process_vm_writev',
                          'PTRACE_POKEDATA')) {
    if ($dynamic -match $forbidden) {
        throw "Forbidden G3 hot-path dependency: $forbidden"
    }
}
$sourceText = Get-Content -LiteralPath $source -Raw
if ([regex]::Matches($sourceText, '\bpwrite\s*\(').Count -ne 0) {
    throw 'G3 payload must not retain the obsolete permanent Brake writer'
}
$adapterText = Get-Content -LiteralPath $adapter -Raw
foreach ($token in @('kTickBeginRva = 0x3695474',
                      'kPhysicsIntervalRva = kTickBeginRva',
                      'ObserveTickBeginAndPrePhysics',
                      'if (lifecycle_state == 2u) return Result::kWaitingForRace',
                      'first qualified interval that observes phase 3',
                      'state->bound_begin_owner = owner')) {
    if (-not $adapterText.Contains($token)) {
        throw "G3 exact race-call binding policy token missing: $token"
    }
}
foreach ($token in @('Command::kInstallPassive',
                      'g_control.begin_branch_destination != 0',
                      'std::uint32_t published = 0',
                      'for (std::uint32_t index = kHookCount; index > 0')) {
    if (-not $sourceText.Contains($token)) {
        throw "G3 three-session-hook transport token missing: $token"
    }
}
foreach ($forbidden in @('FindRetStubOffset', 'begin_stub', '35C37DC')) {
    if ($sourceText.Contains($forbidden)) {
        throw "Forbidden G3 in-image carrier token remains: $forbidden"
    }
}

$asm = Get-Content -LiteralPath $disassembly -Raw
foreach ($symbol in @('G3IntervalOriginalV1',
                       'G3FinalOriginalV1', 'G3FrameOriginalV1',
                       'G3IntervalEntryV1',
                       'G3FinalEntryV1', 'G3FrameEntryV1')) {
    if (-not $asm.Contains("<$symbol>")) {
        throw "Missing G3 wrapper/trampoline in disassembly: $symbol"
    }
}
if ($asm -notmatch '(?s)<G3IntervalEntryV1>:.{0,1600}?bl\s+0x[0-9a-f]+\s+<G3IntervalBeforeV1>.{0,1600}?b\s+0x[0-9a-f]+\s+<G3IntervalOriginalV1>') {
    throw 'G3 interval entry does not coordinate then resume the natural method'
}
foreach ($token in @('mrs', 'NZCV', 'FPCR', 'FPSR',
                      'stp' , 'q6, q7', 'br')) {
    if (-not $asm.Contains($token)) {
        throw "G3 ABI preservation token missing: $token"
    }
}

python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "known-good v2 baseline drift" }

$sourceSha = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolSha = (Get-FileHash -LiteralPath $protocol -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterSha = (Get-FileHash -LiteralPath $adapter -Algorithm SHA256).Hash.ToLowerInvariant()
$coordinatorSha = (Get-FileHash -LiteralPath $coordinator -Algorithm SHA256).Hash.ToLowerInvariant()
$payloadSha = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$buildIdLine = (& $readelf "-n" $payload | Select-String 'Build ID:' |
    Select-Object -Last 1).Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($buildId -notmatch '^[0-9a-f]{40}$') { throw "G3 payload build-id missing" }

Write-Output "G3_MULTI_HOOK_BUILD passed=1 build_only=1 deployed=0 device_access=0"
Write-Output "targets=3695474,367D66C,38B78E4"
Write-Output "exact_target_prologues=3/3"
Write-Output "tick_transport=physics_interval_begin_plus_prephysics_then_natural_method"
Write-Output "tick_target_rva=3695474 original_target=PhysicsInterval"
Write-Output "tick_qualifiers=exact_CarPhysicsComponent_owner,vptr_7EECD88,armed_at_phase2,first_interval_after_lifecycle_2_to_3_starts_race"
Write-Output "install_timing=all_three_session_hooks_at_stopped_countdown"
Write-Output "payload_preload_code_patch_bytes=0 lobby_patch_bytes=0 session_hook_count=3"
Write-Output "normal_hot_path_ptrace=0 gameplay_state_writes=0"
Write-Output "normal_hot_path_pwrite=0 passive_install_pwrite=0 restore_pwrite=0"
Write-Output "game_sha256=$gameSha"
Write-Output "source_sha256=$sourceSha"
Write-Output "protocol_sha256=$protocolSha"
Write-Output "adapter_sha256=$adapterSha"
Write-Output "coordinator_sha256=$coordinatorSha"
Write-Output "payload_sha256=$payloadSha"
Write-Output "build_id=$buildId"
