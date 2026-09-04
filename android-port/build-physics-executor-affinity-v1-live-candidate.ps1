param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_physics_executor_affinity_v1.cpp"
$outDir = Join-Path $root "build\physics-executor-affinity-v1-live-candidate"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $x64Compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$output = Join-Path $outDir "liba9tas_physics_executor_affinity_v1_live_candidate.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_physics_executor_affinity_v1_live_candidate.so"
$controller = Join-Path $outDir "a9tas_physics_executor_affinity_controller_v1"
$bootstrapDiag = Join-Path $outDir "a9tas_physics_executor_affinity_bootstrap_diag_v1"
$disassembly = Join-Path $outDir "physics_executor_affinity_v1_live_candidate.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-DA9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING=1" `
    "-fPIC" "-shared" "-Wl,--no-undefined" "-llog" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "P1 live-candidate ARM64 observer build failed" }
& $x64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    (Join-Path $root "src\bootstrap_physics_executor_affinity_v1_live_candidate.cpp") `
    "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "P1 live-candidate x86_64 bootstrap build failed" }
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    (Join-Path $root "src\physics_executor_affinity_controller_v1.cpp") `
    "-ldl" "-o" $controller
if ($LASTEXITCODE -ne 0) { throw "P1 live-candidate controller build failed" }
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    (Join-Path $root "src\physics_executor_affinity_bootstrap_diag_v1.cpp") `
    "-ldl" "-o" $bootstrapDiag
if ($LASTEXITCODE -ne 0) { throw "P1 bootstrap readiness diagnostic build failed" }

$header = (& $readelf -h $output) -join "`n"
if ($header -notmatch "Machine:\s+AArch64") {
    throw "P1 live-candidate observer is not ARM64"
}
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64") {
    throw "P1 live-candidate bootstrap is not x86_64"
}
$controllerHeader = (& $readelf -h $controller) -join "`n"
if ($controllerHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64") {
    throw "P1 live-candidate controller is not x86_64"
}
$bootstrapDiagHeader = (& $readelf -h $bootstrapDiag) -join "`n"
if ($bootstrapDiagHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64") {
    throw "P1 bootstrap readiness diagnostic is not x86_64"
}
$symbols = (& $readelf --dyn-syms --wide $output) -join "`n"
$dynamic = (& $readelf -d $output) -join "`n"
if ($dynamic -match "libc\+\+_shared\.so") {
    throw "P1 live candidate must not depend on namespace-sensitive libc++_shared.so"
}
foreach ($name in @(
    "a9tas_physics_executor_affinity_v1_protocol",
    "a9tas_physics_executor_affinity_v1_status",
    "a9tas_physics_executor_affinity_v1_report",
    "a9tas_physics_executor_affinity_v1_report_size",
    "a9tas_physics_executor_affinity_v1_arm",
    "a9tas_physics_executor_affinity_v1_report_storage",
    "a9tas_physics_executor_affinity_v1_control_storage",
    "a9tas_physics_executor_affinity_v1_control_size")) {
    if ($symbols -notmatch [regex]::Escape($name)) {
        throw "Missing required P1 live-candidate export: $name"
    }
}
if ($symbols -match "PhysicsExecutorAffinityEntryV1|PhysicsExecutorAffinityCaptureV1|InstallInternal") {
    throw "P1 live-candidate hidden implementation symbols leaked"
}
$bootstrapSymbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
foreach ($name in @(
    "a9tas_bootstrap_stage",
    "a9tas_bootstrap_same_thread_probe_status",
    "a9tas_bootstrap_same_thread_probe_trampoline")) {
    if ($bootstrapSymbols -notmatch [regex]::Escape($name)) {
        throw "Missing required P1 live-candidate bootstrap export: $name"
    }
}

& $objdump "-d" "--demangle" $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "P1 live-candidate disassembly failed" }
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source (Join-Path $root "tools\verify_physics_executor_affinity_v1.py") `
    $disassembly --expect-live-candidate
if ($LASTEXITCODE -ne 0) { throw "P1 live-candidate disassembly audit failed" }
& $python.Source (Join-Path $root "tools\test_parse_physics_executor_affinity_v1.py")
if ($LASTEXITCODE -ne 0) { throw "P1 report parser tests failed" }

$controllerSource = Get-Content -Raw -LiteralPath `
    (Join-Path $root "src\physics_executor_affinity_controller_v1.cpp")
foreach ($forbidden in @(
    "WriteRemote(", "Nitro", "nitro", "mailbox", "control pair",
    "physics correction")) {
    if ($controllerSource.Contains($forbidden)) {
        throw "P1 controller source contains forbidden operation: $forbidden"
    }
}
if (([regex]::Matches($controllerSource, "RemoteCallSessionCall\(")).Count -ne 6) {
    throw "P1 controller must contain exactly status/getter/preflight-low/preflight-high/permission-preflight/arm remote calls"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLower()
$controllerHash = (Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLower()
$bootstrapDiagHash = (Get-FileHash -LiteralPath $bootstrapDiag -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "PHYSICS_EXECUTOR_AFFINITY_V1_LIVE_CANDIDATE_BUILD passed=1 deployed=0 armed=0 capture_limit=300"
Write-Output "sha256=$hash"
Write-Output "bootstrap_sha256=$bootstrapHash"
Write-Output "controller_sha256=$controllerHash"
Write-Output "bootstrap_diag_sha256=$bootstrapDiagHash"
Write-Output "disassembly_sha256=$disasmHash"
