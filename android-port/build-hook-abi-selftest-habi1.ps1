param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $portRoot "src\payload_hook_abi_selftest_habi1.cpp"
$header = Join-Path $portRoot "src\physics_token_gate.h"
$bootstrapSource = Join-Path $portRoot "src\bootstrap_hook_abi_selftest_habi1_build.cpp"
$bootstrapCore = Join-Path $portRoot "src\bootstrap.cpp"
$controllerSource = Join-Path $portRoot "src\habi1_one_shot_controller.cpp"
$carrierSource = Join-Path $portRoot "src\habi1_early_carrier.cpp"
$verifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1.py"
$verifierSelftest = Join-Path $portRoot "tools\test_verify_hook_abi_selftest_habi1.py"
$bootstrapVerifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1_bootstrap.py"
$controllerVerifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1_controller.py"
$controllerVerifierSelftest = Join-Path $portRoot "tools\test_verify_hook_abi_selftest_habi1_controller.py"
$carrierVerifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1_carrier.py"
$carrierVerifierSelftest = Join-Path $portRoot "tools\test_verify_hook_abi_selftest_habi1_carrier.py"
$liveRunner = Join-Path $portRoot "run-habi1-live.ps1"
$liveRunnerPolicy = Join-Path $portRoot "tools\test_run_habi1_live_policy.py"
$carrierLibcEvidence = Join-Path $portRoot "evidence\libc_ldplayer9_20260816.so"
$outDir = Join-Path $portRoot "build\habi-1-offline"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$compilerExe = Join-Path $toolBin "clang++.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$ndkProperties = Join-Path $NdkRoot "source.properties"

foreach ($path in @($source, $header, $bootstrapSource, $bootstrapCore,
                     $controllerSource, $carrierSource, $verifier, $verifierSelftest,
                     $bootstrapVerifier, $controllerVerifier,
                     $controllerVerifierSelftest,
                     $carrierVerifier, $carrierVerifierSelftest,
                     $liveRunner, $liveRunnerPolicy,
                     $carrierLibcEvidence,
                     $compiler, $x86Compiler, $compilerExe,
                     $objdump, $readelf, $ndkProperties)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing HABI-1 build input: $path"
    }
}

$sourceSha = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$headerSha = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierSha = (Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierSelftestSha = (Get-FileHash -LiteralPath $verifierSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapSourceSha = (Get-FileHash -LiteralPath $bootstrapSource -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapCoreSha = (Get-FileHash -LiteralPath $bootstrapCore -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapVerifierSha = (Get-FileHash -LiteralPath $bootstrapVerifier -Algorithm SHA256).Hash.ToLowerInvariant()
$controllerSourceSha = (Get-FileHash -LiteralPath $controllerSource -Algorithm SHA256).Hash.ToLowerInvariant()
$controllerVerifierSha = (Get-FileHash -LiteralPath $controllerVerifier -Algorithm SHA256).Hash.ToLowerInvariant()
$controllerVerifierSelftestSha = (Get-FileHash -LiteralPath $controllerVerifierSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierSourceSha = (Get-FileHash -LiteralPath $carrierSource -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierVerifierSha = (Get-FileHash -LiteralPath $carrierVerifier -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierVerifierSelftestSha = (Get-FileHash -LiteralPath $carrierVerifierSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
$liveRunnerSha = (Get-FileHash -LiteralPath $liveRunner -Algorithm SHA256).Hash.ToLowerInvariant()
$liveRunnerPolicySha = (Get-FileHash -LiteralPath $liveRunnerPolicy -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierLibcSha = (Get-FileHash -LiteralPath $carrierLibcEvidence -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedCarrierLibcSha = "0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc"
$carrierLibcBuildId = "f81ba81cf6ceedcdca60be54ad95b51f"
$carrierLibcDevicePath = "/system/lib64/libc.so"
$carrierLibcTrapRva = "0x5b0"
if ($carrierLibcSha -ne $expectedCarrierLibcSha) {
    throw "HABI-1 pinned LDPlayer9 libc evidence drift"
}
$prefix = $sourceSha.Substring(0, 16)
$fileName = "liba9tas_habi1_self_hook_$prefix.so"
$payload = Join-Path $outDir $fileName
$map = Join-Path $outDir "liba9tas_habi1_self_hook_$prefix.map"
$policyReport = Join-Path $outDir "habi1_payload_$prefix.policy.json"
$bootstrap = Join-Path $outDir "liba9tas_habi1_bootstrap_$prefix.so"
$bootstrapPolicyReport = Join-Path $outDir "habi1_bootstrap_$prefix.policy.json"
$controller = Join-Path $outDir "a9tas_habi1_one_shot_controller_$prefix"
$controllerPolicyReport = Join-Path $outDir "habi1_controller_$prefix.policy.json"
$carrier = Join-Path $outDir "a9tas_habi1_early_carrier_$prefix"
$carrierPolicyReport = Join-Path $outDir "habi1_carrier_$prefix.policy.json"
$manifestPath = Join-Path $outDir "habi1_manifest_$prefix.json"
$devicePath = "/data/local/tmp/$fileName"

New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$compileFlags = @(
    "-shared", "-fPIC", "-O2", "-std=c++20", "-static-libstdc++",
    "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
    "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1",
    "-Wl,--no-undefined", "-Wl,-Map,$map",
    "-DA9TAS_HABI1_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_HABI1_HEADER_SHA256=$headerSha",
    "-DA9TAS_HABI1_EXPECTED_PAYLOAD_PATH=$devicePath",
    $source, "-llog", "-ldl", "-o", $payload
)

& $compiler @compileFlags
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 ARM64 payload build failed with exit code $LASTEXITCODE"
}

python -B $verifier `
    --source $source `
    --header $header `
    --elf $payload `
    --objdump $objdump `
    --expected-device-path $devicePath `
    --report $policyReport
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 payload policy failed with exit code $LASTEXITCODE"
}

python -B $verifierSelftest `
    --verifier $verifier `
    --source $source `
    --header $header `
    --elf $payload `
    --objdump $objdump `
    --expected-device-path $devicePath
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 payload verifier selftest failed with exit code $LASTEXITCODE"
}

$policy = Get-Content -LiteralPath $policyReport -Raw | ConvertFrom-Json
if ($policy.passed -ne 1 -or $policy.source_sha256 -ne $sourceSha -or
    $policy.header_sha256 -ne $headerSha -or
    $policy.expected_device_path -ne $devicePath) {
    throw "HABI-1 policy report identity mismatch"
}

$bootstrapFlags = @(
    $bootstrapSource, "-I$(Join-Path $portRoot 'src')",
    "-shared", "-fPIC", "-O2", "-std=c++20", "-static-libstdc++",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
    "-Wl,--build-id=sha1",
    "-DA9TAS_HABI1_DEVICE_PAYLOAD_PATH=$devicePath",
    "-DA9TAS_HABI1_PAYLOAD_SHA256=$($policy.elf_sha256)",
    "-DA9TAS_HABI1_PAYLOAD_BUILD_ID=$($policy.build_id)",
    "-DA9TAS_HABI1_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-llog", "-ldl", "-o", $bootstrap
)
& $x86Compiler @bootstrapFlags
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 x86_64 bootstrap build failed with exit code $LASTEXITCODE"
}

python -B $bootstrapVerifier `
    --source $bootstrapSource `
    --bootstrap-core $bootstrapCore `
    --elf $bootstrap `
    --payload-sha256 $policy.elf_sha256 `
    --payload-build-id $policy.build_id `
    --payload-source-sha256 $sourceSha `
    --payload-device-path $devicePath `
    --report $bootstrapPolicyReport
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 bootstrap policy failed with exit code $LASTEXITCODE"
}
$bootstrapPolicy = Get-Content -LiteralPath $bootstrapPolicyReport -Raw |
    ConvertFrom-Json
if ($bootstrapPolicy.passed -ne 1 -or
    $bootstrapPolicy.payload_sha256 -ne $policy.elf_sha256 -or
    $bootstrapPolicy.payload_build_id -ne $policy.build_id) {
    throw "HABI-1 bootstrap policy report identity mismatch"
}

$bootstrapDevicePath = "/data/local/tmp/$([IO.Path]::GetFileName($bootstrap))"
$controllerFlags = @(
    $controllerSource, "-O2", "-std=c++20", "-static-libstdc++",
    "-fPIE", "-pie", "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1",
    "-DA9TAS_HABI1_BOOTSTRAP_PATH=$bootstrapDevicePath",
    "-DA9TAS_HABI1_BOOTSTRAP_SHA256=$($bootstrapPolicy.elf_sha256)",
    "-DA9TAS_HABI1_BOOTSTRAP_BUILD_ID=$($bootstrapPolicy.build_id)",
    "-DA9TAS_HABI1_PAYLOAD_PATH=$devicePath",
    "-DA9TAS_HABI1_PAYLOAD_SHA256=$($policy.elf_sha256)",
    "-DA9TAS_HABI1_PAYLOAD_BUILD_ID=$($policy.build_id)",
    "-DA9TAS_HABI1_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_HABI1_PAYLOAD_HEADER_SHA256=$headerSha",
    "-o", $controller
)
& $x86Compiler @controllerFlags
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 one-shot controller build failed with exit code $LASTEXITCODE"
}

$controllerPolicyArgs = @(
    "--source", $controllerSource,
    "--payload-source-file", $source,
    "--elf", $controller,
    "--bootstrap-path", $bootstrapDevicePath,
    "--bootstrap-sha256", $bootstrapPolicy.elf_sha256,
    "--bootstrap-build-id", $bootstrapPolicy.build_id,
    "--payload-path", $devicePath,
    "--payload-sha256", $policy.elf_sha256,
    "--payload-build-id", $policy.build_id,
    "--payload-source-sha256", $sourceSha,
    "--payload-header-sha256", $headerSha
)
python -B $controllerVerifier @controllerPolicyArgs --report $controllerPolicyReport
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 controller policy failed with exit code $LASTEXITCODE"
}
python -B $controllerVerifierSelftest `
    --verifier $controllerVerifier @controllerPolicyArgs
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 controller verifier selftest failed with exit code $LASTEXITCODE"
}
$controllerPolicy = Get-Content -LiteralPath $controllerPolicyReport -Raw |
    ConvertFrom-Json
if ($controllerPolicy.passed -ne 1 -or
    $controllerPolicy.bootstrap_sha256 -ne $bootstrapPolicy.elf_sha256 -or
    $controllerPolicy.payload_sha256 -ne $policy.elf_sha256) {
    throw "HABI-1 controller policy report identity mismatch"
}

$carrierFlags = @(
    $carrierSource, "-I$(Join-Path $portRoot 'src')",
    "-O2", "-std=c++20", "-static-libstdc++", "-fPIE", "-pie",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
    "-Wl,--build-id=sha1",
    "-DA9TAS_HABI1_BOOTSTRAP_PATH=$bootstrapDevicePath",
    "-DA9TAS_HABI1_BOOTSTRAP_SHA256=$($bootstrapPolicy.elf_sha256)",
    "-DA9TAS_HABI1_BOOTSTRAP_BUILD_ID=$($bootstrapPolicy.build_id)",
    "-DA9TAS_HABI1_PAYLOAD_PATH=$devicePath",
    "-DA9TAS_HABI1_PAYLOAD_SHA256=$($policy.elf_sha256)",
    "-DA9TAS_HABI1_PAYLOAD_BUILD_ID=$($policy.build_id)",
    "-DA9TAS_HABI1_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_HABI1_PAYLOAD_HEADER_SHA256=$headerSha",
    "-DA9TAS_HABI1_LIBC_PATH=$carrierLibcDevicePath",
    "-DA9TAS_HABI1_LIBC_SHA256=$carrierLibcSha",
    "-DA9TAS_HABI1_LIBC_BUILD_ID=$carrierLibcBuildId",
    "-DA9TAS_HABI1_LIBC_TRAP_RVA=$carrierLibcTrapRva",
    "-o", $carrier
)
& $x86Compiler @carrierFlags
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 fixed early carrier build failed with exit code $LASTEXITCODE"
}

$carrierPolicyArgs = @(
    "--source", $carrierSource,
    "--controller-source", $controllerSource,
    "--elf", $carrier,
    "--bootstrap-path", $bootstrapDevicePath,
    "--bootstrap-sha256", $bootstrapPolicy.elf_sha256,
    "--bootstrap-build-id", $bootstrapPolicy.build_id,
    "--payload-path", $devicePath,
    "--payload-sha256", $policy.elf_sha256,
    "--payload-build-id", $policy.build_id,
    "--payload-source-sha256", $sourceSha,
    "--libc-file", $carrierLibcEvidence,
    "--libc-device-path", $carrierLibcDevicePath,
    "--libc-sha256", $carrierLibcSha,
    "--libc-build-id", $carrierLibcBuildId,
    "--libc-trap-rva", $carrierLibcTrapRva
)
python -B $carrierVerifier @carrierPolicyArgs --report $carrierPolicyReport
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 carrier policy failed with exit code $LASTEXITCODE"
}
python -B $carrierVerifierSelftest `
    --verifier $carrierVerifier @carrierPolicyArgs
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 carrier verifier selftest failed with exit code $LASTEXITCODE"
}
$carrierPolicy = Get-Content -LiteralPath $carrierPolicyReport -Raw |
    ConvertFrom-Json
if ($carrierPolicy.passed -ne 1 -or
    $carrierPolicy.bootstrap_sha256 -ne $bootstrapPolicy.elf_sha256 -or
    $carrierPolicy.payload_sha256 -ne $policy.elf_sha256 -or
    $carrierPolicy.libc_sha256 -ne $carrierLibcSha) {
    throw "HABI-1 carrier policy report identity mismatch"
}
python -B $liveRunnerPolicy $liveRunner
if ($LASTEXITCODE -ne 0) {
    throw "HABI-1 live runner policy failed with exit code $LASTEXITCODE"
}

$ndkRevisionLine = Get-Content -LiteralPath $ndkProperties |
    Where-Object { $_ -match '^Pkg\.Revision\s*=' } |
    Select-Object -First 1
if (-not $ndkRevisionLine) { throw "NDK revision is missing" }
$ndkRevision = ($ndkRevisionLine -split '=', 2)[1].Trim()

$payloadSha = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$mapSha = (Get-FileHash -LiteralPath $map -Algorithm SHA256).Hash.ToLowerInvariant()
$reportSha = (Get-FileHash -LiteralPath $policyReport -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapSha = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapReportSha = (Get-FileHash -LiteralPath $bootstrapPolicyReport -Algorithm SHA256).Hash.ToLowerInvariant()
$controllerSha = (Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLowerInvariant()
$controllerReportSha = (Get-FileHash -LiteralPath $controllerPolicyReport -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierSha = (Get-FileHash -LiteralPath $carrier -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierReportSha = (Get-FileHash -LiteralPath $carrierPolicyReport -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerSha = (Get-FileHash -LiteralPath $compilerExe -Algorithm SHA256).Hash.ToLowerInvariant()
$readelfSha = (Get-FileHash -LiteralPath $readelf -Algorithm SHA256).Hash.ToLowerInvariant()
$objdumpSha = (Get-FileHash -LiteralPath $objdump -Algorithm SHA256).Hash.ToLowerInvariant()

$manifest = [ordered]@{
    schema = "a9tas-habi1-offline-manifest"
    revision = 3
    gate = "HABI-1"
    generated_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffffffZ")
    source = [ordered]@{
        path = "android-port/src/payload_hook_abi_selftest_habi1.cpp"
        sha256 = $sourceSha
    }
    physics_token_gate = [ordered]@{
        path = "android-port/src/physics_token_gate.h"
        sha256 = $headerSha
    }
    verifier = [ordered]@{
        path = "android-port/tools/verify_hook_abi_selftest_habi1.py"
        sha256 = $verifierSha
        selftest_path = "android-port/tools/test_verify_hook_abi_selftest_habi1.py"
        selftest_sha256 = $verifierSelftestSha
        selftest_negative_cases = 7
        report_path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($policyReport))"
        report_sha256 = $reportSha
    }
    bootstrap = [ordered]@{
        source_path = "android-port/src/bootstrap_hook_abi_selftest_habi1_build.cpp"
        source_sha256 = $bootstrapSourceSha
        core_path = "android-port/src/bootstrap.cpp"
        core_sha256 = $bootstrapCoreSha
        path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($bootstrap))"
        sha256 = $bootstrapSha
        build_id = $bootstrapPolicy.build_id
        machine = "x86_64"
        type = "ET_DYN"
        locator_rva = $bootstrapPolicy.locator_rva
        locator_size = $bootstrapPolicy.locator_size
        return_trap_rva = $bootstrapPolicy.return_trap_rva
        calibrate_tid_rva = $bootstrapPolicy.calibrate_tid_rva
        policy_path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($bootstrapPolicyReport))"
        policy_sha256 = $bootstrapReportSha
        verifier_path = "android-port/tools/verify_hook_abi_selftest_habi1_bootstrap.py"
        verifier_sha256 = $bootstrapVerifierSha
        passive = 0
        disposable_process_only = 1
    }
    controller = [ordered]@{
        source_path = "android-port/src/habi1_one_shot_controller.cpp"
        source_sha256 = $controllerSourceSha
        path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($controller))"
        sha256 = $controllerSha
        build_id = $controllerPolicy.build_id
        machine = "x86_64"
        type = "ET_DYN-PIE"
        cli = @("PID", "START_TICKS", "NONCE")
        guest_calls = 1
        verifier_path = "android-port/tools/verify_hook_abi_selftest_habi1_controller.py"
        verifier_sha256 = $controllerVerifierSha
        verifier_selftest_path = "android-port/tools/test_verify_hook_abi_selftest_habi1_controller.py"
        verifier_selftest_sha256 = $controllerVerifierSelftestSha
        verifier_negative_cases = 7
        policy_path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($controllerPolicyReport))"
        policy_sha256 = $controllerReportSha
    }
    carrier = [ordered]@{
        source_path = "android-port/src/habi1_early_carrier.cpp"
        source_sha256 = $carrierSourceSha
        controller_core_path = "android-port/src/habi1_one_shot_controller.cpp"
        controller_core_sha256 = $controllerSourceSha
        path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($carrier))"
        sha256 = $carrierSha
        build_id = $carrierPolicy.build_id
        machine = "x86_64"
        type = "ET_DYN-PIE"
        cli = @()
        fixed_bootstrap_loads = 1
        libc_evidence_path = "android-port/evidence/libc_ldplayer9_20260816.so"
        libc_sha256 = $carrierLibcSha
        libc_build_id = $carrierLibcBuildId
        libc_device_path = $carrierLibcDevicePath
        libc_trap_rva = $carrierLibcTrapRva
        verifier_path = "android-port/tools/verify_hook_abi_selftest_habi1_carrier.py"
        verifier_sha256 = $carrierVerifierSha
        verifier_selftest_path = "android-port/tools/test_verify_hook_abi_selftest_habi1_carrier.py"
        verifier_selftest_sha256 = $carrierVerifierSelftestSha
        verifier_negative_cases = 6
        policy_path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($carrierPolicyReport))"
        policy_sha256 = $carrierReportSha
        disposable_process_only = 1
    }
    live_runner = [ordered]@{
        path = "android-port/run-habi1-live.ps1"
        sha256 = $liveRunnerSha
        policy_path = "android-port/tools/test_run_habi1_live_policy.py"
        policy_sha256 = $liveRunnerPolicySha
        default_mode = "OfflineValidate"
        live_mode = "ExecuteOnce"
        attempts = 1
        requires_explicit_acknowledgements = 5
    }
    payload = [ordered]@{
        path = "android-port/build/habi-1-offline/$fileName"
        sha256 = $payloadSha
        build_id = $policy.build_id
        machine = "AArch64"
        type = "ET_DYN"
        target_rva = $policy.target_rva
        target_page_end_rva = $policy.target_page_end_rva
        expected_device_path = $devicePath
    }
    linker_map = [ordered]@{
        path = "android-port/build/habi-1-offline/$([IO.Path]::GetFileName($map))"
        sha256 = $mapSha
    }
    toolchain = [ordered]@{
        ndk = "android-ndk-r27d"
        ndk_revision = $ndkRevision
        api = 24
        clang_sha256 = $compilerSha
        llvm_readelf_sha256 = $readelfSha
        llvm_objdump_sha256 = $objdumpSha
    }
    compile_arguments = $compileFlags
    bootstrap_compile_arguments = $bootstrapFlags
    controller_compile_arguments = $controllerFlags
    carrier_compile_arguments = $carrierFlags
    policy = [ordered]@{
        logical_guest_executable_proof = "ELF_PT_LOAD_R_X"
        accepted_houdini_host_views = @("r--p", "r-xp")
        target_writable_view = "rw-p"
        project_exports = $policy.project_exports
        compiler_runtime_weak_export = $policy.compiler_runtime_weak_export
        selftest_constructor_trigger = 0
        guest_calls_planned = 1
    }
    status = [ordered]@{
        offline_payload_policy = "PASS"
        dynamic = "NOT_RUN"
        deployed = 0
        device_access = 0
        live_authorized = 0
    }
}

$manifestJson = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($manifestPath, $manifestJson + "`n",
                        [Text.UTF8Encoding]::new($false))
$manifestSha = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

Write-Output "HABI1_OFFLINE_BUILD passed=1 revision=3 device_access=0 deployed=0 dynamic=NOT_RUN"
Write-Output "source_sha256=$sourceSha"
Write-Output "payload_sha256=$payloadSha"
Write-Output "build_id=$($policy.build_id)"
Write-Output "bootstrap_sha256=$bootstrapSha"
Write-Output "bootstrap_build_id=$($bootstrapPolicy.build_id)"
Write-Output "controller_sha256=$controllerSha"
Write-Output "controller_build_id=$($controllerPolicy.build_id)"
Write-Output "carrier_sha256=$carrierSha"
Write-Output "carrier_build_id=$($carrierPolicy.build_id)"
Write-Output "manifest=$manifestPath"
Write-Output "manifest_sha256=$manifestSha"
