param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d",
    [switch]$ExperimentalHighRefresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\payload_g4_multi_hook_runtime_v1.cpp"
$barrelCore = Join-Path $portRoot "src\barrel_stabilization_replay_core_v1.cpp"
$barrelHeader = Join-Path $portRoot "src\barrel_stabilization_replay_core_v1.h"
$barrelPrng = Join-Path $portRoot "src\barrel_prng_v1.h"
$protocol = Join-Path $portRoot "src\g4_multi_hook_runtime_v1.h"
$adapter = Join-Path $portRoot "src\g4_g3_adapter_v1.h"
$core = Join-Path $portRoot "src\g4_input_action_core_v1.h"
$g3Adapter = Join-Path $portRoot "src\g3_boundary_adapter_v1.h"
$coordinator = Join-Path $portRoot "src\g3_tick_coordinator_v1.h"
$recording = Join-Path $portRoot "src\unified_tick_recording_v1.h"
$game = Join-Path (Split-Path -Parent $portRoot) "apk-analysis\lib\arm64-v8a\libAsphalt9.so"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"
$alignmentPolicy = Join-Path $portRoot "tools\assert_elf_load_alignment_v1.ps1"
$outDir = Join-Path $portRoot "build\g4-multi-hook-runtime-v1"
if ($ExperimentalHighRefresh) {
    # Keep the candidate separate from the pinned, deployed payload until the
    # matching controller/resolver and APK have been built together.
    $outDir = Join-Path $portRoot "build\g4-multi-hook-runtime-high-refresh-v1"
}
$payload = Join-Path $outDir "liba9tas_g4_multi_hook_runtime_v1.so"
$disassembly = Join-Path $outDir "g4_multi_hook_runtime_v1.disasm.txt"

foreach ($path in @($source, $barrelCore, $barrelHeader, $barrelPrng, $protocol, $adapter, $core, $g3Adapter,
                     $coordinator, $recording, $game,
                     $baselineVerifier, $baseline, $compiler, $objdump,
                     $readelf, $nm, $alignmentPolicy)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G4 multi-hook build input: $path"
    }
}
. $alignmentPolicy
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$gameSha = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
if ($gameSha -ne '671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0') {
    throw "G4 exact game ELF identity mismatch"
}

# Android tick-begin is the live-proven Physics Interval boundary. The same
# wrapper performs BeginTick and PrePhysics before resuming the natural method.
$expectedGamePrologues = [ordered]@{
    0x3695474 = 'ffc300d1f55301a9f37b02a935119152'
    0x367D66C = 'ec0f17fceb2b016de923026dfc6f03a9'
    0x38B78E4 = 'ffc300d1f40b00f9f37b02a9280040f9'
    0x36D8524 = 'f553bea9f37b01a908e44639c8000034'
    0x38B7AC4 = 'ffc300d1f40b00f9f37b02a908204739'
    0x369DE4C = 'ffc302d1ee2b00fdedb3056debab066d'
    0x369E30C = 'ff0305d1ef3b0c6ded330d6deb2b0e6d'
    0x37949C8 = 'ff4301d1f75b02a9f55303a9f37b04a9'
}
$expectedRandomPrologues = [ordered]@{
    0x38B7A00 = 'fe0f1ff808fce7d2c6190094'
    0x38B7A3C = 'fe0f1ff8280040f9b7190094'
}
$expectedSetterBodies = [ordered]@{
    0x36934B8 = '080040f9290040b908610bd1080140f90800088b09990cb9c0035fd6'
    0x36934E0 = '080040f9290040b908810bd1080140f90800088b099d0cb9c0035fd6'
}
$expectedSetterSlots = [ordered]@{
    0x2A8 = 0x36934B8
    0x2B0 = 0x36934E0
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
            throw ('G4 exact prologue mismatch at RVA 0x{0:X}: expected={1} actual={2}' -f `
                   $rva, $entry.Value, $actual)
        }
    }
    foreach ($entry in $expectedRandomPrologues.GetEnumerator()) {
        $rva = [Int64]$entry.Key
        $bytes = New-Object byte[] ($entry.Value.Length / 2)
        $gameStream.Position = $rva
        $read = $gameStream.Read($bytes, 0, $bytes.Length)
        $actual = ([System.BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
        if ($read -ne $bytes.Length -or $actual -ne $entry.Value) {
            throw ('G4 exact random prologue mismatch at RVA 0x{0:X}: expected={1} actual={2}' -f `
                   $rva, $entry.Value, $actual)
        }
    }
    foreach ($entry in $expectedSetterBodies.GetEnumerator()) {
        $rva = [Int64]$entry.Key
        $bytes = New-Object byte[] ($entry.Value.Length / 2)
        $gameStream.Position = $rva
        $read = $gameStream.Read($bytes, 0, $bytes.Length)
        $actual = ([System.BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
        if ($read -ne $bytes.Length -or $actual -ne $entry.Value) {
            throw ('G4 exact setter body mismatch at RVA 0x{0:X}: expected={1} actual={2}' -f `
                   $rva, $entry.Value, $actual)
        }
    }
    # The exact adjusted downstream vtable is in the second PT_LOAD whose file
    # offset is RVA-0x1000 in this pinned game ELF.
    foreach ($entry in $expectedSetterSlots.GetEnumerator()) {
        $slotFileOffset = [Int64](0x7EED9E0 - 0x1000 + [Int64]$entry.Key)
        $bytes = New-Object byte[] 8
        $gameStream.Position = $slotFileOffset
        $read = $gameStream.Read($bytes, 0, $bytes.Length)
        $actual = [System.BitConverter]::ToUInt64($bytes, 0)
        if ($read -ne 8 -or $actual -ne [UInt64]$entry.Value) {
            throw ('G4 exact setter vtable mismatch at slot +0x{0:X}: expected=0x{1:X} actual=0x{2:X}' -f `
                   [Int64]$entry.Key, [UInt64]$entry.Value, $actual)
        }
    }
} finally {
    $gameStream.Dispose()
}
$candidateFlags = @()
if ($ExperimentalHighRefresh) {
    $candidateFlags += '-DA9TAS_EXPERIMENTAL_HIGH_REFRESH=1'
}
& $compiler $source $barrelCore @candidateFlags "-I$(Join-Path $portRoot 'src')" "-shared" "-fPIC" `
    "-O2" "-std=c++20" "-static-libstdc++" "-fno-exceptions" `
    "-fno-rtti" "-fno-stack-protector" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-Wl,--no-undefined" "-llog" "-ldl" `
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" `
    "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "G4 multi-hook ARM64 payload build failed" }

Assert-A9TasElfLoadAlignment -ReadElf $readelf -ElfPath $payload `
    -ExpectedAlignment 0x4000 -Label 'G4 multi-hook ARM64 payload'

& $objdump "-d" "--demangle" "--no-show-raw-insn" $payload |
    Set-Content -LiteralPath $disassembly -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "G4 multi-hook disassembly failed" }

$symbols = (& $nm "-D" "--defined-only" $payload) -join "`n"
foreach ($symbol in @('a9tas_g4_command_v1', 'a9tas_g4_protocol_v1',
                       'a9tas_g4_control_data_v1',
                       'a9tas_g4_evidence_data_v1',
                       'a9tas_g4_runtime_data_v1',
                       'a9tas_g4_replay_frames_data_v1',
                       'a9tas_g4_replay_intervals_data_v1',
                       'a9tas_g4_recorded_frames_data_v1',
                       'a9tas_g4_recorded_intervals_data_v1',
                       'a9tas_g4_build_profile_data_v1')) {
    if ($symbols -notmatch [regex]::Escape($symbol)) {
        throw "Missing G4 payload symbol: $symbol"
    }
}

$dynamic = (& $readelf "--dyn-syms" $payload) -join "`n"
foreach ($forbidden in @('ptrace', 'process_vm_writev',
                          'PTRACE_POKEDATA')) {
    if ($dynamic -match $forbidden) {
        throw "Forbidden G4 hot-path dependency: $forbidden"
    }
}
$sourceText = Get-Content -LiteralPath $source -Raw
$randomPatchPolicy = @(
    'constexpr std::size_t kRandomPatchSize = kPatchSize;',
    'constexpr std::size_t kRandomIdentitySize = 12;',
    'std::uint8_t original[kRandomPatchSize]{};',
    'RandomOriginalIdentityValid',
    '(fourth_instruction & 0xfc000000u) == 0x94000000u',
    'AbsoluteJump(patch, g_random_hooks[index].wrapper);'
)
foreach ($token in $randomPatchPolicy) {
    if (-not $sourceText.Contains($token)) {
        throw "G4 NativeBridge-unbounded random jump policy token missing: $token"
    }
}
if ($sourceText.Contains('page_delta < -(1LL << 20)')) {
    throw 'G4 random hook must not retain the NativeBridge-incompatible ADRP range gate'
}
$barrelCoreText = Get-Content -LiteralPath $barrelCore -Raw
$barrelPrngText = Get-Content -LiteralPath $barrelPrng -Raw
if ([regex]::Matches($sourceText, '\bpwrite\s*\(').Count -ne 0) {
    throw 'G4 payload must not retain the obsolete permanent Brake writer'
}
$adapterText = (Get-Content -LiteralPath $adapter -Raw) +
               (Get-Content -LiteralPath $g3Adapter -Raw)
foreach ($token in @('kTickBeginRva = 0x38B7AC4',
                      'kPhysicsSubmitRva = kTickBeginRva',
                      'kPhysicsIntervalRva = 0x3695474',
                      'ObserveTickBeginAtSubmit',
                      'ObservePhysicsInterval',
                      'ObserveTickBeginAndPrePhysics',
                      'if (lifecycle_state == 2u) return Result::kWaitingForRace',
                      'qualified submit that observes phase 3 owns Android tick 0',
                      'state->bound_begin_owner = owner')) {
    if (-not $adapterText.Contains($token)) {
        throw "G4 exact race-call binding policy token missing: $token"
    }
}
foreach ($token in @('Command::kInstallPassive',
                      'build_profile::Valid(g_build_profile)',
                      'g_build_profile.hook_rvas[index]',
                      'g_build_profile.nitro_dispatch_rva',
                      'G4NitroOriginalV1',
                      'G4SubmitOriginalV1',
                      'G4SubmitBeforeV1',
                      'bridge::ObservePhysicsSubmitBeforeOriginal',
                      '__atomic_store_n(delta_token, receipt.decision.fixed_delta_us',
                      'bridge::ObserveNaturalNitroCall',
                      'bridge::ObserveIntervalAfterOriginal',
                      'bridge::IntervalAfterQualified(result)',
                      'g_control.mode != static_cast<std::uint32_t>(RunMode::kNeutral)',
                      'g_control.expected_interval_owner + kControlPairOffset',
                      'g4::CurrentControlPair(g_runtime.input_action)',
                      'G4BrakeSetterEntryV1',
                      'G4SteeringSetterEntryV1',
                      'G4AcceleratorSetterEntryV1',
                      'bridge::ObserveBrakeBeforeOriginal',
                      'bridge::ObserveSteeringBeforeOriginal',
                      'bridge::ObserveAcceleratorAfterOriginal',
                      'PublishSetterSlot',
                      'RestoreSetterSlot',
                      'RestoredTargetsValid',
                      'RestoredSessionReusable',
                      'ResetRestoredSessionMetadata',
                      'g_evidence.restore_calls == 1',
                      'expected = 2',
                      'g_runtime_lock.clear(std::memory_order_release)',
                      'str x8, [sp, #0x18]',
                      'ldr x0, [sp, #0x18]',
                      '__atomic_load_n(output, __ATOMIC_RELAXED)',
                      '__atomic_store_n(output, final_bits, __ATOMIC_RELAXED)',
                      'std::uint32_t published = 0',
                      'G4BarrelRandomBoolEntryV1',
                      'G4BarrelRandomLerpEntryV1',
                      'BuildRandomHookPatch',
                      'PublishRandomHook',
                      'RestoreRandomHook',
                      'ResetBarrelPrng',
                      'kDeterministicBarrelPrngBound',
                      'for (std::uint32_t index = kHookCount; index > 0')) {
    if (-not $sourceText.Contains($token)) {
        throw "G4 eight-session-hook transport token missing: $token"
    }
}
foreach ($forbidden in @('FindRetStubOffset', 'begin_stub', '35C37DC',
                          'fmov w0, s0', 'fmov s0, w20')) {
    if ($sourceText.Contains($forbidden)) {
        throw "Forbidden G4 in-image carrier token remains: $forbidden"
    }
}

$asm = Get-Content -LiteralPath $disassembly -Raw
foreach ($symbol in @('G4IntervalOriginalV1',
                       'G4FinalOriginalV1', 'G4FrameOriginalV1',
                       'G4NitroOriginalV1',
                       'G4SubmitOriginalV1',
                       'G4BarrelRollOriginalV1',
                       'G4BarrelYawOriginalV1',
                       'G4DispatcherOriginalV1',
                       'G4IntervalEntryV1',
                       'G4FinalEntryV1', 'G4FrameEntryV1',
                       'G4NitroEntryV1',
                       'G4SubmitEntryV1',
                       'G4BarrelRollEntryV1',
                       'G4BarrelYawEntryV1',
                       'G4DispatcherEntryV1',
                       'G4BarrelRandomBoolEntryV1',
                       'G4BarrelRandomLerpEntryV1',
                       'G4BrakeSetterEntryV1',
                       'G4SteeringSetterEntryV1',
                       'G4AcceleratorSetterEntryV1')) {
    if (-not $asm.Contains("<$symbol>")) {
        throw "Missing G4 wrapper/trampoline in disassembly: $symbol"
    }
}
foreach ($token in @('kSeed = 0', 'kStateWords = 624',
                      'kMiddleWord = 397', 'NextFloat01', 'NextDouble01',
                      'static_assert(SelfTest()')) {
    if (-not $barrelPrngText.Contains($token)) {
        throw "Deterministic barrel PRNG token missing: $token"
    }
}
if ($sourceText -notmatch '(?s)G4BarrelRandomBoolEntryV1.*?NextBool' -or
    $sourceText -notmatch '(?s)G4BarrelRandomLerpEntryV1.*?NextFloat01.*?\(high - low\) \+ low') {
    throw 'G4 deterministic barrel random wrappers do not match upstream semantics'
}
$pausedRearmBegin = $sourceText.IndexOf('bool RearmPausedReplayRecord()')
$pausedRearmEnd = $sourceText.IndexOf('bool ArchivedCompletedReceiptValid()', $pausedRearmBegin)
$archivedRearmBegin = $sourceText.IndexOf('bool RearmArchivedSession(bool replay, bool lifecycle_callback)')
$archivedRearmEnd = $sourceText.IndexOf('bool RearmArchivedReplay()', $archivedRearmBegin)
$freshArmBegin = $sourceText.IndexOf('bool ArmSession()')
$freshArmEnd = $sourceText.IndexOf('bool Restore()', $freshArmBegin)
if ($pausedRearmBegin -lt 0 -or $pausedRearmEnd -le $pausedRearmBegin -or
    $archivedRearmBegin -lt 0 -or $archivedRearmEnd -le $archivedRearmBegin -or
    $freshArmBegin -lt 0 -or $freshArmEnd -le $freshArmBegin) {
    throw 'G4 deterministic barrel PRNG lifecycle boundaries are missing'
}
$pausedRearm = $sourceText.Substring($pausedRearmBegin,
    $pausedRearmEnd - $pausedRearmBegin)
$archivedRearm = $sourceText.Substring($archivedRearmBegin,
    $archivedRearmEnd - $archivedRearmBegin)
$freshArm = $sourceText.Substring($freshArmBegin, $freshArmEnd - $freshArmBegin)
if ($pausedRearm.Contains('ResetBarrelPrng()')) {
    throw 'Paused replay-to-record continuation must preserve the consumed PRNG sequence'
}
if (-not $archivedRearm.Contains('ResetBarrelPrng()') -or
    -not $freshArm.Contains('ResetBarrelPrng()')) {
    throw 'Every new race session must reset the deterministic barrel PRNG'
}
if ($asm -notmatch '(?s)<G4IntervalEntryV1>:.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4IntervalBeforeV1>.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4IntervalOriginalV1>.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4IntervalAfterV1>') {
    throw 'G4 interval entry does not coordinate around the natural method'
}
if ($asm -notmatch '(?s)<G4SubmitEntryV1>:.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4SubmitBeforeV1>.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4SubmitOriginalV1>') {
    throw 'G4 submit entry does not write the source-faithful token before the natural method'
}
if ($asm -notmatch '(?s)<G4FinalEntryV1>:.{0,1600}?bl\s+0x[0-9a-f]+\s+<G4FinalOriginalV1>.{0,1800}?bl\s+0x[0-9a-f]+\s+<G4FinalAfterV1>') {
    throw 'G5 final writer does not capture after the natural method'
}
if ($asm -notmatch '(?s)<G4NitroEntryV1>:.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4NitroBeforeV1>.{0,2400}?cbz.{0,800}?bl\s+0x[0-9a-f]+\s+<G4NitroOriginalV1>') {
    throw 'G4 Nitro entry does not preserve natural-call suppression semantics'
}
if ($asm -notmatch '(?s)<G4BarrelRollEntryV1>:.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4BarrelRollOriginalV1>.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4BarrelRollAfterV1>' -or
    $asm -notmatch '(?s)<G4BarrelYawEntryV1>:.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4BarrelYawOriginalV1>.{0,2400}?bl\s+0x[0-9a-f]+\s+<G4BarrelYawAfterV1>') {
    throw 'G6 barrel hooks do not preserve original-before-tail order'
}
$dispatcherBody = [regex]::Match($asm,
    '(?ms)^[0-9a-f]+ <G4DispatcherEntryV1>:\r?\n(?<body>.*?)(?=^[0-9a-f]+ <|\z)')
if (-not $dispatcherBody.Success -or $dispatcherBody.Groups['body'].Value -notmatch
    'bl\s+0x[0-9a-f]+\s+<G4DispatcherOriginalV1>') {
    throw 'Fast replay dispatcher does not repeat the exact original complete-logic method'
}
& $compiler -std=c++17 -fsyntax-only (Join-Path $portRoot 'tests/realtime_tick_budget_test.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Real-time tick budget constexpr regression failed' }
& $compiler -std=c++20 -fsyntax-only -fconstexpr-steps=10000000 `
    (Join-Path $portRoot 'tests/slowmo_tick_budget_test.cpp') `
    (Join-Path $portRoot 'tests/slowmo_protocol_test.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Slowmo budget/protocol regression failed' }
& $compiler -std=c++20 -fsyntax-only (Join-Path $portRoot 'tests/long_recording_capacity_test.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Long recording capacity regression failed' }
foreach ($token in @('OnBarrelRollPostOriginal', 'OnBarrelYawPostOriginal',
                      'kSkipBarrelRbx', 'kSkipBarrelAngular',
                      'CopyBits(live_bits, target_bits)')) {
    if (-not $barrelCoreText.Contains($token)) {
        throw "G6 upstream barrel semantic token missing: $token"
    }
}
if ($sourceText -notmatch '(?s)G4BrakeSetterEntryV1\(.*?ObserveBrakeBeforeOriginal.*?original\(owner, value\)' -or
    $sourceText -notmatch '(?s)G4SteeringSetterEntryV1\(.*?ObserveSteeringBeforeOriginal.*?original\(owner, value\)' -or
    $sourceText -notmatch '(?s)G4AcceleratorSetterEntryV1\(.*?original\(owner, value\).*?ObserveAcceleratorAfterOriginal') {
    throw 'G4 setter wrappers do not preserve AluTasV2 before/after-original order'
}
if ($sourceText -notmatch '(?s)const bool source_attempt_still_active = active_retry.*?g_control\.enabled == 1.*?g_control\.completed == 0.*?g_evidence\.status == kArmed.*?if \(source_attempt_still_active\) return false;.*?if \(!BindObservedRaceObjects\(lifecycle_object\)\)') {
    throw 'G8 direct-Retry lifecycle must defer a healthy active attempt before object rebinding or rejection'
}
if ($sourceText -notmatch '(?s)const bool queued_for_this_generation =.*?g_evidence\.status == kQueued.*?g_control\.pending_state == 1u.*?g_control\.pending_mode == g_control\.mode.*?g_control\.pending_generation == g_control\.generation.*?g_control\.pending_reserved == 0u.*?g_evidence\.status == kComplete \|\| queued_for_this_generation') {
    throw 'G8 queued lifecycle activation must accept only its exact archived-generation queue receipt'
}
if ($sourceText -notmatch 'RunMode::kRecord' -or
    $sourceText -notmatch 'BuildPhysicsRecordingFrame' -or
    $sourceText -notmatch 'NativePhysicsIdentityAlive' -or
    $sourceText -notmatch '\+\+g_evidence\.recorded_frames') {
    throw 'G5 physics record path is not wired to Final Writer and the authoritative tick receipt'
}
foreach ($token in @('RunMode::kReplay',
                      'ReplayBuffersValid',
                      'g4::DecidePhysicsCorrection',
                      'auto diagnostic = g_runtime.input_action.packet',
                      'diagnostic.reserved = static_cast<std::uint32_t>(correction.kind)',
                      'g_recorded_frames[g_runtime.input_action.tick] = diagnostic',
                      'PhysicsCorrectionKindV1::kNaturalEqual',
                      'PhysicsCorrectionKindV1::kCorrectBoth',
                      'packet.transform_bits',
                      'packet.linear_velocity_bits',
                      'g_evidence.physics_correction_writes += 2')) {
    if (-not $sourceText.Contains($token)) {
        throw "G5 conditional replay token missing: $token"
    }
}
foreach ($token in @('mrs', 'NZCV', 'FPCR', 'FPSR',
                      'stp' , 'q6, q7', 'br')) {
    if (-not $asm.Contains($token)) {
        throw "G4 ABI preservation token missing: $token"
    }
}

python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'Historical 900-frame baseline no longer matches mutable build outputs. This candidate is NOT live-proven; historical manifest is unchanged.'
}

$sourceSha = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolSha = (Get-FileHash -LiteralPath $protocol -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterSha = (Get-FileHash -LiteralPath $adapter -Algorithm SHA256).Hash.ToLowerInvariant()
$coreSha = (Get-FileHash -LiteralPath $core -Algorithm SHA256).Hash.ToLowerInvariant()
$barrelCoreSha = (Get-FileHash -LiteralPath $barrelCore -Algorithm SHA256).Hash.ToLowerInvariant()
$barrelPrngSha = (Get-FileHash -LiteralPath $barrelPrng -Algorithm SHA256).Hash.ToLowerInvariant()
$g3AdapterSha = (Get-FileHash -LiteralPath $g3Adapter -Algorithm SHA256).Hash.ToLowerInvariant()
$coordinatorSha = (Get-FileHash -LiteralPath $coordinator -Algorithm SHA256).Hash.ToLowerInvariant()
$payloadSha = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$buildIdLine = (& $readelf "-n" $payload | Select-String 'Build ID:' |
    Select-Object -Last 1).Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($buildId -notmatch '^[0-9a-f]{40}$') { throw "G4 payload build-id missing" }

Write-Output "G4_MULTI_HOOK_BUILD passed=1 build_only=1 deployed=0 device_access=0"
Write-Output "reference_fixture_targets=3695474,367D66C,38B78E4,36D8524,38B7AC4,369DE4C,369E30C,37949C8 runtime_targets=build_profile"
Write-Output "exact_target_prologues=8/8"
Write-Output "barrel_random_identity=2/2 first_12_exact_plus_fourth_BL patch_bytes=16 exact_restore_bytes=16 deterministic_prng=mt19937_seed_0"
Write-Output "exact_setter_bodies=2/2 runtime_setter_vtable=build_profile setter_vtable_slots=2A8,2B0"
Write-Output "tick_transport=physics_submit_before_then_interval_before_plus_original_plus_after"
Write-Output "tick_targets=build_profile original_target=PhysicsContextSubmit"
Write-Output "tick_qualifiers=exact_profile_PhysicsContext_owner,armed_at_phase2,first_submit_after_lifecycle_2_to_3_starts_race"
Write-Output "install_timing=eight_session_hooks_plus_two_barrel_random_hooks_plus_two_proven_adjusted_setter_slots_at_stopped_countdown"
Write-Output "payload_preload_code_patch_bytes=0 lobby_patch_bytes=0 ordinary_code_hook_count=8 random_code_hook_count=2 total_code_hook_count=10 setter_slot_count=2"
Write-Output "accelerator_setter=disabled_pending_android_after_original_proof"
Write-Output "live_modes=neutral,record,replay replay_live=NOT_RUN"
Write-Output "record_scope=adjusted_brake,adjusted_steering,natural_nitro,interval_diagnostics,barrel_rbx,barrel_angular,final_transform,linear_velocity skip_mask=48 physics_valid=1"
Write-Output "replay_scope=adjusted_brake,adjusted_steering,game_owned_nitro,alu_default_interval_passthrough,after_original_barrel_tails,conditional_final_64_plus_12,complete_logic_dispatch_speed_1_2_4_8"
Write-Output "normal_hot_path_ptrace=0 neutral_gameplay_state_writes=0 record_fixed_delta_writes=one_per_tick"
Write-Output "normal_hot_path_pwrite=0 passive_install_pwrite=0 restore_pwrite=0"
Write-Output "reference_prologue_fixture_sha256=$gameSha runtime_game_identity=build_profile_sha256_plus_build_id"
Write-Output "source_sha256=$sourceSha"
Write-Output "protocol_sha256=$protocolSha"
Write-Output "adapter_sha256=$adapterSha"
Write-Output "core_sha256=$coreSha"
Write-Output "barrel_core_sha256=$barrelCoreSha"
Write-Output "barrel_prng_sha256=$barrelPrngSha"
Write-Output "g3_adapter_sha256=$g3AdapterSha"
Write-Output "coordinator_sha256=$coordinatorSha"
Write-Output "payload_sha256=$payloadSha"
Write-Output "build_id=$buildId"
