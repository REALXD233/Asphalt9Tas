param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$compiler = Join-Path $toolBin 'aarch64-linux-android24-clang++.cmd'
$readelf = Join-Path $toolBin 'llvm-readelf.exe'
$nm = Join-Path $toolBin 'llvm-nm.exe'
$source = Join-Path $portRoot 'src/native_arm64_g4_payload_resolver_build_only_v1.cpp'
$header = Join-Path $portRoot 'src/native_arm64_g4_payload_resolver_v1.h'
$payload = Join-Path $portRoot 'build/g4-multi-hook-runtime-v1/liba9tas_g4_multi_hook_runtime_v1.so'
$payloadBuild = Join-Path $portRoot 'build-g4-multi-hook-runtime-v1.ps1'
$output = Join-Path $portRoot 'build/native-arm64-g4-payload-resolver-v1'
$object = Join-Path $output 'native_arm64_g4_payload_resolver_v1.arm64.o'

foreach ($required in @($compiler,$readelf,$nm,$source,$header,$payloadBuild)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native ARM64 G4 resolver input: $required"
    }
}
if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
    & $payloadBuild -NdkRoot $NdkRoot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'G4 payload prerequisite failed' }
}
New-Item -ItemType Directory -Force -Path $output | Out-Null

& $compiler $source "-I$(Join-Path $portRoot 'src')" -c -O2 -std=c++20 `
    -Wall -Wextra -Werror -fno-exceptions -fno-rtti -o $object
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 G4 payload resolver build failed' }
$identity = (& $readelf -h $object) -join "`n"
if ($LASTEXITCODE -ne 0 -or $identity -notmatch 'Machine:\s+AArch64') {
    throw 'native ARM64 G4 payload resolver object identity failed'
}
$objectSymbols = (& $nm -C $object) -join "`n"
if ($objectSymbols -notmatch 'a9tas_native_arm64_g4_payload_resolve_build_only_v1') {
    throw 'native ARM64 G4 payload resolver entry missing'
}

$payloadHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $payload).Hash.ToLowerInvariant()
$payloadSize = (Get-Item -LiteralPath $payload).Length
if ($payloadHash -ne '2294dafdcee3f97ca0805d8123ba59bea9be90d13cc46374662c60b057b30a55' -or
    $payloadSize -ne 4152672) {
    throw 'pinned G4 payload identity drifted'
}
$symbols = (& $readelf -sW $payload) -join "`n"
$expectedSymbols = @{
    'a9tas_g4_command_v1' = '000000000000F688'
    'a9tas_g4_control_data_v1' = '0000000000024710'
    'a9tas_g4_evidence_data_v1' = '0000000000024718'
    'a9tas_g4_runtime_data_v1' = '0000000000024720'
    'a9tas_g4_build_profile_data_v1' = '0000000000024728'
    'a9tas_g4_replay_frames_data_v1' = '0000000000024730'
    'a9tas_g4_replay_intervals_data_v1' = '0000000000024738'
    'a9tas_g4_recorded_frames_data_v1' = '0000000000024740'
    'a9tas_g4_recorded_intervals_data_v1' = '0000000000024748'
}
foreach ($entry in $expectedSymbols.GetEnumerator()) {
    $pattern = '(?m)^\s*\d+:\s+' + $entry.Value + '\s+\d+\s+(?:FUNC|OBJECT)\s+GLOBAL\s+DEFAULT\s+\d+\s+' + [regex]::Escape($entry.Key) + '$'
    if ($symbols -notmatch $pattern) {
        throw "pinned G4 payload symbol drifted: $($entry.Key)"
    }
}
$allSymbols = (& $nm -n -S $payload) -join "`n"
$expectedStorageSymbols = @{
    '_ZN12_GLOBAL__N_19g_runtimeE' = @('0000000000024780','00000000003d8930')
    '_ZN12_GLOBAL__N_110g_evidenceE' = @('00000000003FD180','0000000000000340')
    '_ZN12_GLOBAL__N_19g_controlE' = @('00000000003FD4C0','0000000000000280')
    '_ZN12_GLOBAL__N_120g_recorded_intervalsE' = @('00000000003FD8C0','0000000000177000')
    '_ZN12_GLOBAL__N_117g_recorded_framesE' = @('0000000000574F00','000000000034bc00')
    '_ZN12_GLOBAL__N_115g_build_profileE' = @('00000000008C0B00','0000000000000180')
    '_ZN12_GLOBAL__N_115g_replay_framesE' = @('00000000008C0C80','000000000034bc00')
    '_ZN12_GLOBAL__N_118g_replay_intervalsE' = @('0000000000C0C880','0000000000177000')
}
foreach ($entry in $expectedStorageSymbols.GetEnumerator()) {
    $address = $entry.Value[0]
    $size = $entry.Value[1]
    $pattern = '(?m)^' + $address + '\s+' + $size + '\s+[bBdD]\s+' + [regex]::Escape($entry.Key) + '$'
    if ($allSymbols -notmatch $pattern) {
        throw "pinned G4 payload storage symbol drifted: $($entry.Key)"
    }
}
$programHeaders = (& $readelf -lW $payload) -join "`n"
if ($programHeaders -notmatch '(?m)^\s*LOAD\s+0x017d40\s+0x0000000000023D40\s+0x0000000000023D40\s+0x3d93a0\s+0xd5fc40\s+RW\s+0x4000\s*$') {
    throw 'pinned G4 payload final RW PT_LOAD drifted'
}
$buildIdLine = (& $readelf -n $payload | Select-String 'Build ID:' | Select-Object -Last 1).Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($buildId -ne '1da494291bc9556274daf27d100b175f0a352e7e') {
    throw 'pinned G4 payload Build ID drifted'
}

$text = Get-Content -LiteralPath $header -Raw
foreach ($forbidden in @('libhoudini','libnb.so','bootstrap_base','guest_trampoline')) {
    if ($text.Contains($forbidden)) {
        throw "native ARM64 resolver retained NativeBridge dependency: $forbidden"
    }
}
foreach ($token in @('kCommandRva = 0xF688',
                      'kRuntimeStorageRva = 0x24780',
                      'kEvidenceStorageRva = 0x3FD180',
                      'kControlStorageRva = 0x3FD4C0',
                      'kRecordedIntervalsStorageRva = 0x3FD8C0',
                      'kRecordedFramesStorageRva = 0x574F00',
                      'kBuildProfileStorageRva = 0x8C0B00',
                      'kReplayFramesStorageRva = 0x8C0C80',
                      'kReplayIntervalsStorageRva = 0xC0C880',
                      'kFinalRwRva = 0x23D40',
                      'kFinalRwLogicalEndRva = 0xD83980',
                      'relocated!=expected[index]',
                      'PrivateAnonymousBss',
                      'WritableRange',
                      'std::memcmp(hash,kExpectedSha256',
                      'std::memcmp(command_map->perms,"r-xp",4)')) {
    if (-not $text.Contains($token)) {
        throw "native ARM64 resolver invariant missing: $token"
    }
}

$objectHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $object).Hash.ToLowerInvariant()
Write-Output 'NATIVE_ARM64_G4_PAYLOAD_RESOLVER_BUILD passed=1 exact_payload=1 direct_command=1 locators=8 nativebridge_dependency=0 bss_split=1 backend_enabled=0 device_access=0'
Write-Output "resolver_object_sha256=$objectHash"
Write-Output "payload_sha256=$payloadHash"
Write-Output "payload_build_id=$buildId"
