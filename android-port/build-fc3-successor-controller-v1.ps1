param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$controllerSource = Join-Path $root "src\fc2_frame_callback_transaction_controller_v1.cpp"
$identitySource = Join-Path $root "src\fc3_replay_observer_identity_resolver_v1.cpp"
$eventSource = Join-Path $root "src\fc3_observer_event_core_v1.cpp"
$protocolSource = Join-Path $root "src\fc3_replay_observer_state_machine_v1.cpp"
$policy = Join-Path $root "tools\test_fc3_successor_controller_policy_v1.py"
$outDir = Join-Path $root "build\fc3-successor-controller-v1"
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

$passive = Join-Path $outDir "fc3_successor_controller_v1_build_only"
$review = Join-Path $outDir "fc3_successor_controller_v1_review_only.o"
$disassembly = Join-Path $outDir "fc3_successor_controller_v1_review_only.disasm.txt"

& $compiler $controllerSource "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "FC-3 successor passive build failed" }

& $compiler $controllerSource "-O2" "-std=c++20" "-fno-exceptions" `
    "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_FC2_LIVE_CANDIDATE=1" `
    "-DA9TAS_FC3_TAIL_CANDIDATE=1" "-c" "-o" $review
if ($LASTEXITCODE -ne 0) { throw "FC-3 successor review compile failed" }

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$tempDir = [System.IO.Path]::GetFullPath(
    (Join-Path $tempRoot ("a9tas-fc3-successor-linkcheck-{0}" -f $PID)))
if (-not $tempDir.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "FC-3 successor link-check path escaped system temp"
}
$identityObject = Join-Path $tempDir "identity.o"
$eventObject = Join-Path $tempDir "event.o"
$protocolObject = Join-Path $tempDir "protocol.o"
$controllerObject = Join-Path $tempDir "controller.o"
$linkCheck = Join-Path $tempDir "linkcheck.so"
try {
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    Copy-Item -LiteralPath $review -Destination $controllerObject
    & $compiler $identitySource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
        "-DA9TAS_FC3_IDENTITY_REVIEW=1" "-c" "-o" $identityObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 successor identity compile failed" }
    & $compiler $eventSource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" `
        "-DA9TAS_FC3_EVENT_CORE_REVIEW=1" "-c" "-o" $eventObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 successor event-core compile failed" }
    & $compiler $protocolSource "-O2" "-std=c++20" "-fno-exceptions" `
        "-fno-rtti" "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" `
        "-DA9TAS_FC3_PROTOCOL_NO_MAIN=1" "-c" "-o" $protocolObject
    if ($LASTEXITCODE -ne 0) { throw "FC-3 successor protocol compile failed" }

    foreach ($object in @($controllerObject, $identityObject)) {
        & $objcopy `
            "--localize-symbol=_Z34A9TasSchedulerObserverMain_NotUsediPPc" `
            $object
        if ($LASTEXITCODE -ne 0) { throw "FC-3 successor symbol isolation failed" }
    }
    & $compiler $controllerObject $identityObject $eventObject $protocolObject `
        "-shared" "-static-libstdc++" "-Wl,--gc-sections" `
        "-Wl,--no-undefined" "-o" $linkCheck
    if ($LASTEXITCODE -ne 0) { throw "FC-3 successor ephemeral link failed" }
    $header = (& $readelf -h $linkCheck) -join "`n"
    if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
        $header -notmatch "Type:\s+DYN") {
        throw "FC-3 successor ephemeral linked image is invalid"
    }
    $dynamicSymbols = (& $readelf "--dyn-syms" "--wide" $linkCheck) -join "`n"
    foreach ($required in @("ptrace", "pwrite", "pread", "waitpid")) {
        if ($dynamicSymbols -notmatch "\b$required(?:@|\b)") {
            throw "FC-3 successor linked review lacks required FC-2 transport: $required"
        }
    }
    foreach ($forbidden in @("process_vm_writev", "dlopen", "mprotect", "socket")) {
        if ($dynamicSymbols -match "\b$forbidden(?:@|\b)") {
            throw "FC-3 successor linked review imports forbidden primitive: $forbidden"
        }
    }
    $allSymbols = (& $readelf "--symbols" "--wide" $linkCheck) -join "`n"
    foreach ($legacyBlocking in @(
        "A9TasSchedulerObserverMain_NotUsed", "AttachNewThreads", "ClearAndDetach")) {
        if ($allSymbols -match [regex]::Escape($legacyBlocking)) {
            throw "FC-3 successor link retained unreachable legacy blocking path: $legacyBlocking"
        }
    }
} finally {
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force
    }
}
if (Test-Path -LiteralPath $tempDir) {
    throw "FC-3 successor temporary link directory was retained"
}

& $objdump "-d" "--demangle" $review | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-3 successor disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $review $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-3 successor policy failed" }
& $python.Source (Join-Path $root "tools\validate_fc3_report_v1.py") "--selftest"
if ($LASTEXITCODE -ne 0) { throw "FC-3 report-validator selftest failed" }
& $python.Source (Join-Path $root "tools\audit_fc3_successor_candidate_v1.py") `
    $passive $review `
    (Join-Path $root "build\fc3-replay-observer-identity-v1\liba9tas_fc3_identity_resolver_review_only.so") `
    (Join-Path $root "build\fc3-observer-event-core-v1\fc3_observer_event_core_v1_review_only.o") `
    (Join-Path $root "build\fc3-replay-observer-protocol-v1\fc3_replay_observer_protocol_v1_selftest.o") `
    (Join-Path $root "build\fc2-frame-callback-transaction-v1\fc2_frame_callback_transaction_v1_review_only.o") `
    $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-3 successor closure audit failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$reviewHash = (Get-FileHash -LiteralPath $review -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FC3_SUCCESSOR_CONTROLLER_BUILD passed=1 runtime=disabled linked_live_artifact=0 device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_object_sha256=$reviewHash"
Write-Output "disassembly_sha256=$disasmHash"
