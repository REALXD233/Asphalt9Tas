param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$baseSource = Join-Path $root "src\payload_final_writer_replay_v1.cpp"
$phaseSource = Join-Path $root "src\payload_vehicle_camera_phase_observer_v1.cpp"
$phaseProtocol = Join-Path $root "src\vehicle_camera_phase_observer_protocol_v1.h"
$protocolSelftest = Join-Path $root "src\vehicle_camera_phase_observer_protocol_selftest_v1.cpp"
$resolverSelftest = Join-Path $root "src\vehicle_camera_phase_observer_elf_resolver_selftest_v1.cpp"
$policy = Join-Path $root "tools\test_vehicle_camera_phase_observer_policy_v1.py"
$resolverPolicy = Join-Path $root "tools\test_vehicle_camera_phase_observer_elf_resolver_v1.py"
$transactionSource = Join-Path $root "src\vehicle_camera_phase_camera_transaction_controller_v1.cpp"
$transactionPolicy = Join-Path $root "tools\test_vehicle_camera_phase_transaction_policy_v1.py"
$unifiedSource = Join-Path $root "src\hwbp_vehicle_camera_phase_unified_replay_v1.cpp"
$unifiedPolicy = Join-Path $root "tools\test_vehicle_camera_phase_unified_policy_v1.py"
$bootstrapSource = Join-Path $root "src\bootstrap_vehicle_camera_phase_v1_build.cpp"
$analyzerTest = Join-Path $root "tools\test_analyze_vehicle_camera_phase_events_v1.py"
$gateValidator = Join-Path $root "tools\validate_vehicle_camera_phase_gate_v1.py"
$gateValidatorTest = Join-Path $root "tools\test_validate_vehicle_camera_phase_gate_v1.py"
$baseArtifact = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$baseBuild = Join-Path $root "build-final-writer-replay-v1.ps1"
$outDir = Join-Path $root "build\vehicle-camera-phase-observer-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$expectedBaseHash = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
$expectedBaseSourceHash = "112bd88773c69607b99d2f2e5ee930b2cdfcda342ed5652e618bbd3e57183abf"
$actualBaseSourceHash = (Get-FileHash -LiteralPath $baseSource -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualBaseSourceHash -ne $expectedBaseSourceHash) {
    throw "Immutable final-writer source changed: $actualBaseSourceHash"
}
if (-not (Test-Path -LiteralPath $baseArtifact -PathType Leaf) -or
    (Get-FileHash -LiteralPath $baseArtifact -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedBaseHash) {
    & $baseBuild -NdkRoot $NdkRoot
    if ($LASTEXITCODE -ne 0) { throw "Pinned final-writer rebuild failed" }
}
$actualBaseHash = (Get-FileHash -LiteralPath $baseArtifact -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualBaseHash -ne $expectedBaseHash) {
    throw "Immutable final-writer artifact hash mismatch: $actualBaseHash"
}

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$hostCompiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $hostCompiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$baseObject = Join-Path $outDir "final_writer.o"
$phaseObject = Join-Path $outDir "phase_observer.o"
$output = Join-Path $outDir "liba9tas_vehicle_camera_phase_observer_v1_build_only.so"
$disassembly = Join-Path $outDir "vehicle_camera_phase_observer_v1.disasm.txt"
$transaction = Join-Path $outDir "a9tas_vehicle_camera_phase_transaction_v1"
$unifiedObject = Join-Path $outDir "vehicle_camera_phase_unified_v1_review_only.o"
$candidate = Join-Path $outDir "a9tas_vehicle_camera_phase_live_candidate_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_vehicle_camera_phase_v1.so"
$common = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-fno-stack-protector", "-Wall", "-Wextra", "-Werror", "-fPIC")

& $compiler $baseSource @common "-I$(Join-Path $root 'src')" "-c" "-o" $baseObject
if ($LASTEXITCODE -ne 0) { throw "Immutable final-writer object build failed" }
& $compiler $phaseSource @common "-I$(Join-Path $root 'src')" "-c" "-o" $phaseObject
if ($LASTEXITCODE -ne 0) { throw "Phase-observer object build failed" }
& $compiler $baseObject $phaseObject "-static-libstdc++" "-shared" `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "Phase-observer ARM64 link failed" }
& $hostCompiler $transactionSource "-I$(Join-Path $root 'src')" "-O2" `
    "-std=c++20" "-static-libstdc++" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--no-undefined" "-o" $transaction
if ($LASTEXITCODE -ne 0) { throw "Phase-observer host transaction build failed" }
& $hostCompiler $unifiedSource "-I$(Join-Path $root 'src')" "-O2" `
    "-std=c++20" "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" `
    "-Werror" "-Wno-unused-function" `
    "-DA9TAS_FINAL_WRITER_LIVE_CANDIDATE=1" "-c" "-o" $unifiedObject
if ($LASTEXITCODE -ne 0) { throw "Phase-observer unified review-object build failed" }
& $hostCompiler $unifiedObject "-static-libstdc++" "-Wl,--no-undefined" `
    "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "Phase-observer local live-candidate link failed" }
& $hostCompiler $bootstrapSource "-I$(Join-Path $root 'src')" "-shared" `
    "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" "-Wall" "-Wextra" `
    "-Werror" "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Phase-observer preload bootstrap build failed" }

& $objdump "-d" "--demangle" $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Phase-observer disassembly failed" }
& $compiler $protocolSelftest "-I$(Join-Path $root 'src')" "-std=c++20" `
    "-Wall" "-Wextra" "-Werror" "-fsyntax-only"
if ($LASTEXITCODE -ne 0) { throw "Phase-observer protocol ABI proof failed" }
& $compiler $resolverSelftest "-I$(Join-Path $root 'src')" "-std=c++20" `
    "-Wall" "-Wextra" "-Werror" "-fsyntax-only"
if ($LASTEXITCODE -ne 0) { throw "Phase-observer resolver syntax proof failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $output $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Phase-observer policy verification failed" }
& $python.Source $resolverPolicy $output
if ($LASTEXITCODE -ne 0) { throw "Phase-observer resolver policy failed" }
& $python.Source $transactionPolicy $transaction $readelf
if ($LASTEXITCODE -ne 0) { throw "Phase-observer transaction policy failed" }
& $python.Source $unifiedPolicy $unifiedObject $readelf $objdump $candidate
if ($LASTEXITCODE -ne 0) { throw "Phase-observer unified policy failed" }
& $python.Source "-m" "unittest" $analyzerTest "-q"
if ($LASTEXITCODE -ne 0) { throw "Phase-observer analyzer test failed" }
& $python.Source "-m" "py_compile" $gateValidator
if ($LASTEXITCODE -ne 0) { throw "Phase-observer gate validator syntax failed" }
& $python.Source "-m" "unittest" $gateValidatorTest "-q"
if ($LASTEXITCODE -ne 0) { throw "Phase-observer gate validator test failed" }

$payloadHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $phaseSource -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolHash = (Get-FileHash -LiteralPath $phaseProtocol -Algorithm SHA256).Hash.ToLowerInvariant()
$policyHash = (Get-FileHash -LiteralPath $policy -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
$transactionHash = (Get-FileHash -LiteralPath $transaction -Algorithm SHA256).Hash.ToLowerInvariant()
$unifiedHash = (Get-FileHash -LiteralPath $unifiedObject -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "VEHICLE_CAMERA_PHASE_OBSERVER_BUILD passed=1 build_only=1 base_pinned=1 camera_write=0 resolver=1 transaction=1 unified_review=1 local_candidate=1 analyzer=1 deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "phase_source_sha256=$sourceHash"
Write-Output "phase_protocol_sha256=$protocolHash"
Write-Output "policy_sha256=$policyHash"
Write-Output "disassembly_sha256=$disasmHash"
Write-Output "transaction_sha256=$transactionHash"
Write-Output "unified_review_object_sha256=$unifiedHash"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
Write-Output "immutable_base_payload_sha256=$actualBaseHash"
