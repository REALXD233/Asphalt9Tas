param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\payload_tick_observer_v1.cpp"
$bootstrapSource = Join-Path $portRoot "src\bootstrap_g3_begin_identity_v1_build.cpp"
$bootstrapCore = Join-Path $portRoot "src\bootstrap.cpp"
$carrierSource = Join-Path $portRoot "src\habi1_early_carrier.cpp"
$controllerCore = Join-Path $portRoot "src\habi1_one_shot_controller.cpp"
$carrierVerifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1_carrier.py"
$carrierVerifierSelftest = Join-Path $portRoot "tools\test_verify_hook_abi_selftest_habi1_carrier.py"
$carrierLibc = Join-Path $portRoot "evidence\libc_ldplayer9_20260816.so"
$game = Join-Path (Split-Path -Parent $portRoot) "apk-analysis\lib\arm64-v8a\libAsphalt9.so"
$parser = Join-Path $portRoot "tools\parse_g3_begin_identity_v1.py"
$parserTest = Join-Path $portRoot "tools\test_parse_g3_begin_identity_v1.py"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$outDir = Join-Path $portRoot "build\g3-begin-identity-v1"
$payload = Join-Path $outDir "liba9tas_g3_begin_identity_v1.so"
$bootstrap = Join-Path $outDir "liba9tas_g3_begin_identity_bootstrap_v1.so"
$disassembly = Join-Path $outDir "g3_begin_identity_v1.disasm.txt"
$carrierPolicy = Join-Path $outDir "g3_begin_identity_carrier_v1.policy.json"
$devicePayload = "/data/local/tmp/liba9tas_g3_begin_identity_v1.so"
$deviceBootstrap = "/data/local/tmp/liba9tas_g3_begin_identity_bootstrap_v1.so"

foreach ($path in @($source,$bootstrapSource,$bootstrapCore,$carrierSource,
                     $controllerCore,$carrierVerifier,$carrierVerifierSelftest,
                     $carrierLibc,$game,$parser,$parserTest,$baselineVerifier,
                     $baseline,$compiler,$x86Compiler,$objdump,$readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G3 begin-identity input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$gameSha = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
if ($gameSha -ne '671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0') {
    throw "G3 begin-identity exact game ELF mismatch"
}
$stream = [System.IO.File]::OpenRead($game)
try {
    $stream.Position = 0x386B1E0
    $bytes = New-Object byte[] 12
    if ($stream.Read($bytes,0,$bytes.Length) -ne $bytes.Length) {
        throw "Cannot read UpdatePerTick prologue"
    }
    $actual = ([BitConverter]::ToString($bytes)).Replace('-','').ToLowerInvariant()
    if ($actual -ne 'ff4301d1f41b00f9f37b04a9') {
        throw "UpdatePerTick exact prologue mismatch"
    }
} finally {
    $stream.Dispose()
}

& $compiler $source "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" `
    "-fno-stack-protector" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-Wl,--no-undefined" "-llog" "-ldl" `
    "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity payload build failed" }

& $objdump "-d" "--demangle" "--no-show-raw-insn" $payload |
    Set-Content -LiteralPath $disassembly -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity disassembly failed" }
$asm = Get-Content -LiteralPath $disassembly -Raw
foreach ($token in @('<TickObserverCapture','<ObserverCommon>',
                      'DumpBeginIdentity','q30, q31','br\s+x17')) {
    if ($asm -notmatch $token) {
        throw "G3 begin-identity ABI token missing: $token"
    }
}
$sourceText = Get-Content -LiteralPath $source -Raw
foreach ($token in @('A9G3BI1','g_identity_state','object \+ 0x48',
                      'vptr \+ 0x40','vptr \+ 0x70')) {
    if ($sourceText -notmatch $token) {
        throw "G3 begin-identity receipt token missing: $token"
    }
}

python -B $parserTest
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity parser selftest failed" }
python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "Known-good 900-frame baseline drift" }

$payloadSha = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceSha = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$buildIdLine = (& $readelf "-n" $payload | Select-String 'Build ID:' |
    Select-Object -Last 1).Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($buildId -notmatch '^[0-9a-f]{40}$') { throw "Payload build-id missing" }

$bootstrapFlags = @(
    $bootstrapSource, "-I$(Join-Path $portRoot 'src')", "-shared", "-fPIC",
    "-O2", "-std=c++20", "-static-libstdc++", "-fno-exceptions",
    "-fno-rtti", "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1",
    "-DA9TAS_G3_BEGIN_IDENTITY_DEVICE_PAYLOAD_PATH=$devicePayload",
    "-DA9TAS_G3_BEGIN_IDENTITY_PAYLOAD_SHA256=$payloadSha",
    "-DA9TAS_G3_BEGIN_IDENTITY_PAYLOAD_BUILD_ID=$buildId",
    "-DA9TAS_G3_BEGIN_IDENTITY_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-llog", "-ldl", "-o", $bootstrap
)
& $x86Compiler @bootstrapFlags
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity bootstrap build failed" }
$bootstrapSha = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapBuildIdLine = (& $readelf "-n" $bootstrap |
    Select-String 'Build ID:' | Select-Object -Last 1).Line
$bootstrapBuildId = ($bootstrapBuildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($bootstrapBuildId -notmatch '^[0-9a-f]{40}$') {
    throw "G3 begin-identity bootstrap build-id missing"
}

$carrier = Join-Path $outDir "a9tas_habi1_early_carrier_$($sourceSha.Substring(0,16))"
$identityFlags = @(
    "-DA9TAS_HABI1_BOOTSTRAP_PATH=$deviceBootstrap",
    "-DA9TAS_HABI1_BOOTSTRAP_SHA256=$bootstrapSha",
    "-DA9TAS_HABI1_BOOTSTRAP_BUILD_ID=$bootstrapBuildId",
    "-DA9TAS_HABI1_PAYLOAD_PATH=$devicePayload",
    "-DA9TAS_HABI1_PAYLOAD_SHA256=$payloadSha",
    "-DA9TAS_HABI1_PAYLOAD_BUILD_ID=$buildId",
    "-DA9TAS_HABI1_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_HABI1_PAYLOAD_HEADER_SHA256=$sourceSha"
)
$carrierFlags = @(
    $carrierSource, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-static-libstdc++", "-fPIE", "-pie", "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1"
) + $identityFlags + @(
    "-DA9TAS_HABI1_LIBC_PATH=/system/lib64/libc.so",
    "-DA9TAS_HABI1_LIBC_SHA256=0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc",
    "-DA9TAS_HABI1_LIBC_BUILD_ID=f81ba81cf6ceedcdca60be54ad95b51f",
    "-DA9TAS_HABI1_LIBC_TRAP_RVA=0x5b0", "-o", $carrier
)
& $x86Compiler @carrierFlags
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity early carrier build failed" }

$carrierPolicyArgs = @(
    "--source", $carrierSource,
    "--controller-source", $controllerCore,
    "--elf", $carrier,
    "--bootstrap-path", $deviceBootstrap,
    "--bootstrap-sha256", $bootstrapSha,
    "--bootstrap-build-id", $bootstrapBuildId,
    "--payload-path", $devicePayload,
    "--payload-sha256", $payloadSha,
    "--payload-build-id", $buildId,
    "--payload-source-sha256", $sourceSha,
    "--libc-file", $carrierLibc,
    "--libc-device-path", "/system/lib64/libc.so",
    "--libc-sha256", "0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc",
    "--libc-build-id", "f81ba81cf6ceedcdca60be54ad95b51f",
    "--libc-trap-rva", "0x5b0"
)
python -B $carrierVerifier @carrierPolicyArgs --report $carrierPolicy
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity carrier policy failed" }
python -B $carrierVerifierSelftest --verifier $carrierVerifier @carrierPolicyArgs
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity carrier policy selftest failed" }

$bootstrapIdentity = (& $readelf "-h" $bootstrap) -join "`n"
$carrierIdentity = (& $readelf "-h" $carrier) -join "`n"
if ($bootstrapIdentity -notmatch 'Machine:\s+Advanced Micro Devices X86-64' -or
    $bootstrapIdentity -notmatch 'Type:\s+DYN' -or
    $carrierIdentity -notmatch 'Machine:\s+Advanced Micro Devices X86-64' -or
    $carrierIdentity -notmatch 'Type:\s+DYN') {
    throw "G3 begin-identity x86_64 loader identity failed"
}
$carrierSha = (Get-FileHash -LiteralPath $carrier -Algorithm SHA256).Hash.ToLowerInvariant()

Write-Output "G3_BEGIN_IDENTITY_BUILD passed=1 build_only=1 device_access=0 deployed=0"
Write-Output "transport=live_proven_tick_only_observer target=386B1E0"
Write-Output "capture=x0,x1,caller,tid,vptr,source,source_vptr,slot40,slot70"
Write-Output "gameplay_state_writes=0 normal_capture_ptrace=0"
Write-Output "game_sha256=$gameSha"
Write-Output "source_sha256=$sourceSha"
Write-Output "payload_sha256=$payloadSha"
Write-Output "build_id=$buildId"
Write-Output "bootstrap_sha256=$bootstrapSha"
Write-Output "bootstrap_build_id=$bootstrapBuildId"
Write-Output "carrier_sha256=$carrierSha"
Write-Output "carrier=$carrier"
Write-Output "carrier_policy=$carrierPolicy"
