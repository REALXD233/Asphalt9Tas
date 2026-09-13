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
$strings = Join-Path $toolBin 'llvm-strings.exe'
$alignmentPolicy = Join-Path $portRoot 'tools/assert_elf_load_alignment_v1.ps1'
$g4 = Join-Path $portRoot 'src/g4_input_action_controller_v1.cpp'
$g2 = Join-Path $portRoot 'src/g2_physics_interval_controller_v1.cpp'
$habi = Join-Path $portRoot 'src/habi1_one_shot_controller.cpp'
$command = Join-Path $portRoot 'src/native_arm64_command_backend_v1.cpp'
$remote = Join-Path $portRoot 'src/native_arm64_remote_call_v1.cpp'
$trap = Join-Path $portRoot 'src/native_arm64_immutable_trap_resolver_v1.cpp'
$resolver = Join-Path $portRoot 'src/native_arm64_g4_payload_resolver_v1.h'
$policy = Join-Path $portRoot 'tools/test_native_arm64_g4_controller_policy_v1.py'
$payload = Join-Path $portRoot 'build/g4-multi-hook-runtime-v1/liba9tas_g4_multi_hook_runtime_v1.so'
$payloadBuild = Join-Path $portRoot 'build-g4-multi-hook-runtime-v1.ps1'
$resolverBuild = Join-Path $portRoot 'build-native-arm64-g4-payload-resolver-v1.ps1'
$output = Join-Path $portRoot 'build/native-arm64-g4-controller-v1'
$controller = Join-Path $output 'a9tas_native_arm64_g4_input_action_controller_v1'

foreach ($required in @($compiler,$readelf,$nm,$strings,$alignmentPolicy,$g4,$g2,$habi,
                         $command,$remote,$trap,$resolver,$policy,$payloadBuild,
                         $resolverBuild)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native ARM64 G4 controller input: $required"
    }
}
. $alignmentPolicy
if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
    & $payloadBuild -NdkRoot $NdkRoot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'native G4 payload prerequisite failed' }
}
$payloadHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $payload).Hash.ToLowerInvariant()
$payloadSize = (Get-Item -LiteralPath $payload).Length
if ($payloadHash -ne 'c129ee69ee618942af9e0c218b1fa27a42ddd05511e4850d222c3cd968b682ad' -or
    $payloadSize -ne 4152872) {
    throw 'native G4 controller payload identity drifted'
}
# Compile and verify the exact resolver against the same final payload before
# embedding it in the controller.  This prevents a newly rebuilt payload from
# being packaged together with stale hash/RVA pins.
& $resolverBuild -NdkRoot $NdkRoot | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw 'native G4 payload resolver/payload consistency gate failed'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
& $compiler -std=c++17 -fsyntax-only (Join-Path $portRoot 'tests/diagnostic_tick_row_test.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic tick-row regression failed' }

& $compiler $g4 $command $remote $trap "-I$(Join-Path $portRoot 'src')" `
    -O2 -std=c++20 -static-libstdc++ -fPIE -pie `
    -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti `
    -Wall -Wextra -Werror `
    '-Wl,--build-id=sha1,--gc-sections,-z,relro,-z,now' `
    '-Wl,-z,max-page-size=16384' '-Wl,-z,common-page-size=16384' `
    '-DA9TAS_G4_NATIVE_ARM64_CONTROLLER=1' -o $controller
if ($LASTEXITCODE -ne 0) { throw 'full native ARM64 G4 controller build failed' }

$header = (& $readelf -h $controller) -join "`n"
if ($LASTEXITCODE -ne 0 -or $header -notmatch 'Machine:\s+AArch64' -or
    $header -notmatch 'Type:\s+DYN') {
    throw 'native ARM64 G4 controller ELF identity failed'
}
$programs = (& $readelf -l $controller) -join "`n"
if ($LASTEXITCODE -ne 0 -or $programs -notmatch 'GNU_RELRO' -or
    $programs -match 'LOAD\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+RWE') {
    throw 'native ARM64 G4 controller hardening failed'
}
Assert-A9TasElfLoadAlignment -ReadElf $readelf -ElfPath $controller `
    -ExpectedAlignment 0x4000 -Label 'native ARM64 G4 controller'
$dynamic = (& $readelf -d $controller) -join "`n"
if ($LASTEXITCODE -ne 0 -or $dynamic -match 'libc[+][+]_shared[.]so') {
    throw 'native ARM64 G4 controller gained a deployed C++ runtime dependency'
}
$symbols = (& $nm -C $controller) -join "`n"
foreach ($symbol in @(' main','InvokeStopped','GetRegisters','SetRegistersExact')) {
    if ($symbols -notmatch [regex]::Escape($symbol)) {
        throw "native ARM64 G4 controller symbol missing: $symbol"
    }
}
$binaryText = (& $strings $controller) -join "`n"
foreach ($forbidden in @('libhoudini','libnb.so','guest_trampoline','bootstrap')) {
    if ($binaryText.Contains($forbidden)) {
        throw "native ARM64 G4 controller retained bridge dependency: $forbidden"
    }
}
foreach ($receipt in @('G4_STATUS complete=','G4_REPLAY_LOAD passed=0',
                        'G4_PASSIVE installed=','G4_ACTION action=')) {
    if (-not $binaryText.Contains($receipt)) {
        throw "native ARM64 G4 action/receipt missing: $receipt"
    }
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 G4 controller policy failed' }

$buildIdLine = (& $readelf -n $controller |
    Select-String 'Build ID:' | Select-Object -Last 1).Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($buildId -notmatch '^[0-9a-f]{40}$') {
    throw 'native ARM64 G4 controller Build ID missing'
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $controller).Hash.ToLowerInvariant()
Write-Output 'NATIVE_ARM64_G4_CONTROLLER_BUILD passed=1 full_controller=1 shared_g4_core=1 actions=29 direct_payload=1 stopped_reresolve=1 native_command=1 nativebridge_dependency=0 backend_enabled=0 device_access=0'
Write-Output "controller_sha256=$hash"
Write-Output "controller_build_id=$buildId"
Write-Output "payload_sha256=$payloadHash"
