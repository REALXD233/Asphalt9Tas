param(
    [ValidateSet("OfflineValidateOnly", "Capture")]
    [string]$Mode = "OfflineValidateOnly",
    [int]$DurationMs = 3000,
    [int]$WorkerTid = 0,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [switch]$AcknowledgeNaturallyRunningRace
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$parserTests = Join-Path $projectRoot `
    "tools\test_parse_hwbp_executor_stack_affinity_v1.py"
$policyTests = Join-Path $projectRoot `
    "tools\test_executor_stack_affinity_build_policy_v1.py"

if ($Mode -eq "OfflineValidateOnly") {
    python.exe $parserTests
    if ($LASTEXITCODE -ne 0) { throw "executor parser selftests failed" }
    python.exe $policyTests
    if ($LASTEXITCODE -ne 0) { throw "executor policy selftests failed" }
    Write-Output "EXECUTOR_STACK_AFFINITY_V1_OFFLINE_VALIDATION_OK device_access=0 attached=0"
    return
}

if (-not $AcknowledgeNaturallyRunningRace) {
    throw "Capture requires -AcknowledgeNaturallyRunningRace"
}

$runner = Join-Path $projectRoot "run-hwbp-worker-stack-scope-v1.ps1"
& $runner -DurationMs $DurationMs -WorkerTid $WorkerTid -Device $Device `
    -AdbPath $AdbPath -OutputPath $OutputPath -Mode ExecutorAffinity
if ($LASTEXITCODE -ne 0) {
    throw "executor stack-affinity runner failed (exit=$LASTEXITCODE)"
}
