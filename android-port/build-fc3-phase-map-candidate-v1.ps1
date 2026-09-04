param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$controllerSource = Join-Path $root "src\fc2_frame_callback_transaction_controller_v1.cpp"
$identitySource = Join-Path $root "src\fc3_replay_observer_identity_resolver_v1.cpp"
$eventSource = Join-Path $root "src\fc3_observer_event_core_v1.cpp"
$protocolSource = Join-Path $root "src\fc3_replay_observer_state_machine_v1.cpp"
$phaseSource = Join-Path $root "src\fc3_phase_map_v1.cpp"
$outDir = Join-Path $root "build\fc3-phase-map-candidate-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$objcopy = Join-Path $toolBin "llvm-objcopy.exe"
foreach ($tool in @($compiler, $readelf, $objdump, $objcopy)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$passive = Join-Path $outDir "fc3_phase_map_candidate_v1_build_only"
$review = Join-Path $outDir "fc3_phase_map_candidate_v1_review_only.o"
$disassembly = Join-Path $outDir "fc3_phase_map_candidate_v1_review_only.disasm.txt"
$live = Join-Path $outDir "a9tas_fc3_phase_map_controller_v1"

& $compiler $controllerSource "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map passive build failed" }

& $compiler $controllerSource "-O2" "-std=c++20" "-fno-exceptions" `
    "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_FC2_LIVE_CANDIDATE=1" `
    "-DA9TAS_FC3_TAIL_CANDIDATE=1" `
    "-DA9TAS_FC3_PHASE_MAP_CANDIDATE=1" "-c" "-o" $review
if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map review compile failed" }

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$tempDir = [System.IO.Path]::GetFullPath(
    (Join-Path $tempRoot ("a9tas-fc3-phase-map-linkcheck-{0}" -f $PID)))
if (-not $tempDir.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "FC-3 phase-map link-check path escaped system temp"
}
try {
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    $controllerObject = Join-Path $tempDir "controller.o"
    $identityObject = Join-Path $tempDir "identity.o"
    $eventObject = Join-Path $tempDir "event.o"
    $protocolObject = Join-Path $tempDir "protocol.o"
    $phaseObject = Join-Path $tempDir "phase.o"
    $linkCheck = Join-Path $tempDir "linkcheck.so"
    Copy-Item -LiteralPath $review -Destination $controllerObject

    & $compiler $identitySource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
        "-DA9TAS_FC3_IDENTITY_REVIEW=1" "-c" "-o" $identityObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map identity compile failed" }
    & $compiler $eventSource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" `
        "-DA9TAS_FC3_EVENT_CORE_REVIEW=1" "-c" "-o" $eventObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map legacy event compile failed" }
    & $compiler $protocolSource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" `
        "-DA9TAS_FC3_PROTOCOL_NO_MAIN=1" "-c" "-o" $protocolObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map legacy protocol compile failed" }
    & $compiler $phaseSource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" "-c" "-o" $phaseObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map core compile failed" }

    foreach ($object in @($controllerObject, $identityObject)) {
        & $objcopy `
            "--localize-symbol=_Z34A9TasSchedulerObserverMain_NotUsediPPc" `
            $object
        if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map symbol isolation failed" }
    }
    & $compiler $controllerObject $identityObject $eventObject $protocolObject `
        $phaseObject "-shared" "-static-libstdc++" "-Wl,--gc-sections" `
        "-Wl,--no-undefined" "-o" $linkCheck
    if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map ephemeral link failed" }

    & $compiler $controllerObject $identityObject $eventObject $protocolObject `
        $phaseObject "-static-libstdc++" "-Wl,--gc-sections" `
        "-Wl,--no-undefined" "-o" $live
    if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map controller link failed" }

    $header = (& $readelf -h $linkCheck) -join "`n"
    if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
        $header -notmatch "Type:\s+DYN") {
        throw "FC-3 phase-map ephemeral linked image is invalid"
    }
    $liveHeader = (& $readelf -h $live) -join "`n"
    if ($liveHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
        $liveHeader -notmatch "Type:\s+DYN") {
        throw "FC-3 phase-map linked controller is invalid"
    }
    $symbols = (& $readelf "--dyn-syms" "--wide" $linkCheck) -join "`n"
    foreach ($required in @("ptrace", "pwrite", "pread", "waitpid")) {
        if ($symbols -notmatch "\b$required(?:@|\b)") {
            throw "FC-3 phase-map linked review lacks required transport: $required"
        }
    }
    foreach ($forbidden in @("process_vm_writev", "dlopen", "mprotect", "socket")) {
        if ($symbols -match "\b$forbidden(?:@|\b)") {
            throw "FC-3 phase-map linked review imports forbidden primitive: $forbidden"
        }
    }
} finally {
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force
    }
}

& $objdump "-d" "--demangle" $review | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source (Join-Path $root "tools\validate_fc3_phase_map_v1.py") `
    "--selftest"
if ($LASTEXITCODE -ne 0) { throw "FC-3 phase-map validator selftest failed" }
& $python.Source (Join-Path $root "tools\validate_fc3_entry_stability_v1.py") `
    "--selftest"
if ($LASTEXITCODE -ne 0) { throw "FC-3 entry-stability validator selftest failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$reviewHash = (Get-FileHash -LiteralPath $review -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
$liveHash = (Get-FileHash -LiteralPath $live -Algorithm SHA256).Hash.ToLower()
Write-Output "FC3_PHASE_MAP_BUILD passed=1 runtime=guarded linked_live_artifact=1 deployed=0 device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_object_sha256=$reviewHash"
Write-Output "disassembly_sha256=$disasmHash"
Write-Output "controller_sha256=$liveHash"
