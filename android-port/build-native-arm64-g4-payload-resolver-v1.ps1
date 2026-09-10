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
if ($payloadHash -ne '766f2f9815486a71702c3b31c8b6edb08cbc93edd55781daaa63348aebb1f1b9' -or
    $payloadSize -ne 4149656) {
    throw 'pinned G4 payload identity drifted'
}
$symbols = (& $readelf -sW $payload) -join "`n"
$expectedSymbols = @{
    'a9tas_g4_command_v1' = '000000000000ee04'
    'a9tas_g4_control_data_v1' = '0000000000023CD0'
    'a9tas_g4_evidence_data_v1' = '0000000000023CD8'
    'a9tas_g4_runtime_data_v1' = '0000000000023ce0'
    'a9tas_g4_build_profile_data_v1' = '0000000000023ce8'
    'a9tas_g4_replay_frames_data_v1' = '0000000000023cf0'
    'a9tas_g4_replay_intervals_data_v1' = '0000000000023cf8'
    'a9tas_g4_recorded_frames_data_v1' = '0000000000023d00'
    'a9tas_g4_recorded_intervals_data_v1' = '0000000000023d08'
}
foreach ($entry in $expectedSymbols.GetEnumerator()) {
    $pattern = '(?m)^\s*\d+:\s+' + $entry.Value + '\s+\d+\s+(?:FUNC|OBJECT)\s+GLOBAL\s+DEFAULT\s+\d+\s+' + [regex]::Escape($entry.Key) + '$'
    if ($symbols -notmatch $pattern) {
        throw "pinned G4 payload symbol drifted: $($entry.Key)"
    }
}
$allSymbols = (& $nm -n -S $payload) -join "`n"
$expectedStorageSymbols = @{
    '_ZN12_GLOBAL__N_19g_runtimeE' = @('0000000000023D40','00000000003d8930')
    '_ZN12_GLOBAL__N_110g_evidenceE' = @('00000000003fc700','0000000000000340')
    '_ZN12_GLOBAL__N_19g_controlE' = @('00000000003fca40','0000000000000240')
    '_ZN12_GLOBAL__N_120g_recorded_intervalsE' = @('00000000003fce00','0000000000177000')
    '_ZN12_GLOBAL__N_117g_recorded_framesE' = @('0000000000574440','000000000034bc00')
    '_ZN12_GLOBAL__N_115g_build_profileE' = @('00000000008c0040','0000000000000180')
    '_ZN12_GLOBAL__N_115g_replay_framesE' = @('00000000008c01c0','000000000034bc00')
    '_ZN12_GLOBAL__N_118g_replay_intervalsE' = @('0000000000c0bdc0','0000000000177000')
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
if ($programHeaders -notmatch '(?m)^\s*LOAD\s+0x017300\s+0x0000000000023300\s+0x0000000000023300\s+0x3d9370\s+0xd5fbc0\s+RW\s+0x4000\s*$') {
    throw 'pinned G4 payload final RW PT_LOAD drifted'
}
$buildIdLine = (& $readelf -n $payload | Select-String 'Build ID:' | Select-Object -Last 1).Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($buildId -ne 'cc9356cd6a7ffe082942342e6c34bd23ec90deed') {
    throw 'pinned G4 payload Build ID drifted'
}

$text = Get-Content -LiteralPath $header -Raw
foreach ($forbidden in @('libhoudini','libnb.so','bootstrap_base','guest_trampoline')) {
    if ($text.Contains($forbidden)) {
        throw "native ARM64 resolver retained NativeBridge dependency: $forbidden"
    }
}
foreach ($token in @('kCommandRva = 0xEE04',
                      'kRuntimeStorageRva = 0x23D40',
                      'kEvidenceStorageRva = 0x3FC700',
                      'kControlStorageRva = 0x3FCA40',
                      'kRecordedIntervalsStorageRva = 0x3FCE00',
                      'kRecordedFramesStorageRva = 0x574440',
                      'kBuildProfileStorageRva = 0x8C0040',
                      'kReplayFramesStorageRva = 0x8C01C0',
                      'kReplayIntervalsStorageRva = 0xC0BDC0',
                      'kFinalRwRva = 0x23300',
                      'kFinalRwLogicalEndRva = 0xD82EC0',
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
