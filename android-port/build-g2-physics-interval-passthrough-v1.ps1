param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\payload_g2_physics_interval_passthrough_v1.cpp"
$header = Join-Path $portRoot "src\g2_physics_interval_passthrough_v1.h"
$verifier = Join-Path $portRoot "tools\verify_g2_physics_interval_passthrough_v1.py"
$verifierSelftest = Join-Path $portRoot "tools\test_verify_g2_physics_interval_passthrough_v1.py"
$bootstrapSource = Join-Path $portRoot "src\bootstrap_g2_physics_interval_passthrough_v1_build.cpp"
$bootstrapCore = Join-Path $portRoot "src\bootstrap.cpp"
$controllerSource = Join-Path $portRoot "src\g2_physics_interval_controller_v1.cpp"
$controllerVerifier = Join-Path $portRoot "tools\verify_g2_physics_interval_controller_v1.py"
$controllerVerifierSelftest = Join-Path $portRoot "tools\test_verify_g2_physics_interval_controller_v1.py"
$carrierSource = Join-Path $portRoot "src\habi1_early_carrier.cpp"
$carrierVerifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1_carrier.py"
$carrierVerifierSelftest = Join-Path $portRoot "tools\test_verify_hook_abi_selftest_habi1_carrier.py"
$carrierLibc = Join-Path $portRoot "evidence\libc_ldplayer9_20260816.so"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$outDir = Join-Path $portRoot "build\g2-physics-interval-passthrough-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"

foreach ($path in @($source, $header, $verifier, $verifierSelftest,
                     $bootstrapSource, $bootstrapCore, $controllerSource,
                     $controllerVerifier, $controllerVerifierSelftest,
                     $carrierSource, $carrierVerifier,
                     $carrierVerifierSelftest, $carrierLibc, $baselineVerifier,
                     $baseline, $compiler, $x86Compiler, $objdump,
                     $readelf, $nm)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G2 build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$sourceSha = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$headerSha = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$devicePath = "/data/local/tmp/liba9tas_g2_physics_interval_passthrough_v1.so"
$payload = Join-Path $outDir "liba9tas_g2_physics_interval_passthrough_v1.so"
$disassembly = Join-Path $outDir "g2_physics_interval_passthrough_v1.disasm.txt"
$policyReport = Join-Path $outDir "g2_physics_interval_passthrough_v1.policy.json"

$compileFlags = @(
    $source, "-I$(Join-Path $portRoot 'src')", "-shared", "-fPIC", "-O2",
    "-std=c++20", "-static-libstdc++", "-fno-exceptions", "-fno-rtti",
    "-fno-stack-protector", "-Wall", "-Wextra", "-Werror",
    "-Wl,--build-id=sha1", "-Wl,--no-undefined",
    "-DA9TAS_G2_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_G2_EXPECTED_PAYLOAD_PATH=$devicePath",
    "-llog", "-ldl", "-o", $payload
)
& $compiler @compileFlags
if ($LASTEXITCODE -ne 0) { throw "G2 ARM64 payload build failed" }

& $objdump "-d" "--demangle" "--no-show-raw-insn" $payload |
    Set-Content -LiteralPath $disassembly -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "G2 disassembly failed" }

python -B $verifier --header $header --source $source --elf $payload `
    --objdump $objdump --readelf $readelf --nm $nm `
    --expected-device-path $devicePath --report $policyReport
if ($LASTEXITCODE -ne 0) { throw "G2 payload policy failed" }

python -B $verifierSelftest --verifier $verifier --header $header `
    --source $source --disassembly $disassembly
if ($LASTEXITCODE -ne 0) { throw "G2 policy selftest failed" }

python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "known-good v2 baseline drift" }

$policy = Get-Content -LiteralPath $policyReport -Raw | ConvertFrom-Json
if ($policy.passed -ne 1 -or $policy.source_sha256 -ne $sourceSha -or
    $policy.header_sha256 -ne $headerSha -or
    $policy.expected_device_path -ne $devicePath) {
    throw "G2 policy report identity mismatch"
}

$bootstrapDevicePath = "/data/local/tmp/liba9tas_g2_physics_interval_bootstrap_v1.so"
$bootstrap = Join-Path $outDir "liba9tas_g2_physics_interval_bootstrap_v1.so"
$bootstrapFlags = @(
    $bootstrapSource, "-I$(Join-Path $portRoot 'src')", "-shared", "-fPIC",
    "-O2", "-std=c++20", "-static-libstdc++", "-fno-exceptions",
    "-fno-rtti", "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1",
    "-DA9TAS_G2_DEVICE_PAYLOAD_PATH=$devicePath",
    "-DA9TAS_G2_PAYLOAD_SHA256=$($policy.elf_sha256)",
    "-DA9TAS_G2_PAYLOAD_BUILD_ID=$($policy.build_id)",
    "-DA9TAS_G2_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-llog", "-ldl", "-o", $bootstrap
)
& $x86Compiler @bootstrapFlags
if ($LASTEXITCODE -ne 0) { throw "G2 bootstrap build failed" }
$bootstrapSha = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapBuildIdLine = (& $readelf "-n" $bootstrap |
    Select-String "Build ID:" | Select-Object -Last 1).Line
$bootstrapBuildId = ($bootstrapBuildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($bootstrapBuildId -notmatch '^[0-9a-f]{40}$') {
    throw "G2 bootstrap build-id missing"
}

$identityFlags = @(
    "-DA9TAS_HABI1_BOOTSTRAP_PATH=$bootstrapDevicePath",
    "-DA9TAS_HABI1_BOOTSTRAP_SHA256=$bootstrapSha",
    "-DA9TAS_HABI1_BOOTSTRAP_BUILD_ID=$bootstrapBuildId",
    "-DA9TAS_HABI1_PAYLOAD_PATH=$devicePath",
    "-DA9TAS_HABI1_PAYLOAD_SHA256=$($policy.elf_sha256)",
    "-DA9TAS_HABI1_PAYLOAD_BUILD_ID=$($policy.build_id)",
    "-DA9TAS_HABI1_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_HABI1_PAYLOAD_HEADER_SHA256=$headerSha"
)

$controller = Join-Path $outDir "a9tas_g2_physics_interval_controller_v1"
$controllerFlags = @(
    $controllerSource, "-I$(Join-Path $portRoot 'src')", "-O2",
    "-std=c++20", "-static-libstdc++", "-fPIE", "-pie",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
    "-Wl,--build-id=sha1"
) + $identityFlags + @("-o", $controller)
& $x86Compiler @controllerFlags
if ($LASTEXITCODE -ne 0) { throw "G2 controller build failed" }
$controllerPolicyReport = Join-Path $outDir `
    "g2_physics_interval_controller_v1.policy.json"
python -B $controllerVerifier --source $controllerSource --elf $controller `
    --readelf $readelf --nm $nm --payload-path $devicePath `
    --payload-sha256 $($policy.elf_sha256) `
    --payload-build-id $($policy.build_id) `
    --bootstrap-path $bootstrapDevicePath `
    --bootstrap-sha256 $bootstrapSha `
    --bootstrap-build-id $bootstrapBuildId --report $controllerPolicyReport
if ($LASTEXITCODE -ne 0) { throw "G2 controller policy failed" }
python -B $controllerVerifierSelftest --verifier $controllerVerifier `
    --source $controllerSource
if ($LASTEXITCODE -ne 0) { throw "G2 controller policy selftest failed" }

$carrier = Join-Path $outDir `
    "a9tas_habi1_early_carrier_$($sourceSha.Substring(0, 16))"
$carrierFlags = @(
    $carrierSource, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-static-libstdc++", "-fPIE", "-pie", "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1"
) + $identityFlags + @(
    "-DA9TAS_HABI1_LIBC_PATH=/system/lib64/libc.so",
    "-DA9TAS_HABI1_LIBC_SHA256=0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc",
    "-DA9TAS_HABI1_LIBC_BUILD_ID=f81ba81cf6ceedcdca60be54ad95b51f",
    "-DA9TAS_HABI1_LIBC_TRAP_RVA=0x5b0",
    "-o", $carrier
)
& $x86Compiler @carrierFlags
if ($LASTEXITCODE -ne 0) { throw "G2 early carrier build failed" }
$carrierPolicyReport = Join-Path $outDir "g2_early_carrier_v1.policy.json"
$carrierPolicyArgs = @(
    "--source", $carrierSource,
    "--controller-source", (Join-Path $portRoot "src\habi1_one_shot_controller.cpp"),
    "--elf", $carrier,
    "--bootstrap-path", $bootstrapDevicePath,
    "--bootstrap-sha256", $bootstrapSha,
    "--bootstrap-build-id", $bootstrapBuildId,
    "--payload-path", $devicePath,
    "--payload-sha256", $policy.elf_sha256,
    "--payload-build-id", $policy.build_id,
    "--payload-source-sha256", $sourceSha,
    "--libc-file", $carrierLibc,
    "--libc-device-path", "/system/lib64/libc.so",
    "--libc-sha256", "0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc",
    "--libc-build-id", "f81ba81cf6ceedcdca60be54ad95b51f",
    "--libc-trap-rva", "0x5b0"
)
python -B $carrierVerifier @carrierPolicyArgs --report $carrierPolicyReport
if ($LASTEXITCODE -ne 0) { throw "G2 early carrier policy failed" }
python -B $carrierVerifierSelftest --verifier $carrierVerifier `
    @carrierPolicyArgs
if ($LASTEXITCODE -ne 0) { throw "G2 early carrier policy selftest failed" }

$payloadSha = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$controllerSha = (Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierSha = (Get-FileHash -LiteralPath $carrier -Algorithm SHA256).Hash.ToLowerInvariant()
foreach ($artifact in @($bootstrap, $controller, $carrier)) {
    $identity = (& $readelf "-h" $artifact) -join "`n"
    if ($identity -notmatch 'Machine:\s+Advanced Micro Devices X86-64' -or
        $identity -notmatch 'Type:\s+DYN') {
        throw "G2 x86_64 artifact identity failed: $artifact"
    }
}
Write-Output "G2_OFFLINE_BUILD passed=1 deployed=0 dynamic=NOT_RUN"
Write-Output "source_sha256=$sourceSha"
Write-Output "header_sha256=$headerSha"
Write-Output "payload_sha256=$payloadSha"
Write-Output "build_id=$($policy.build_id)"
Write-Output "bootstrap_sha256=$bootstrapSha"
Write-Output "bootstrap_build_id=$bootstrapBuildId"
Write-Output "controller_sha256=$controllerSha"
Write-Output "carrier_sha256=$carrierSha"
Write-Output "policy=$policyReport"
