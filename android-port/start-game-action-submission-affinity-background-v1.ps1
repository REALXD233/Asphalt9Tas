# Starts the guarded observation-only runner in a hidden background process.
# This exists only to return the ARMED acknowledgement to the operator without
# consuming the manual-input window. It never sends input itself.
param(
    [int]$DurationMs = 45000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [int]$ArmTimeoutMs = 15000,
    [switch]$AcknowledgeNaturallyRunningRace,
    [switch]$AcknowledgeExactlyOneManualSpacePress,
    [switch]$AcknowledgeNoPausedAttach,
    [switch]$AcknowledgeGuestMemoryReadOnly,
    [switch]$AcknowledgeDebugRegistersOnly,
    [switch]$AcknowledgeNoAutomatedInput,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$runner = Join-Path $root "run-game-action-submission-affinity-v1.ps1"
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw "Guarded runner unavailable"
}
foreach ($gate in @(
    @($AcknowledgeNaturallyRunningRace, "a naturally running race"),
    @($AcknowledgeExactlyOneManualSpacePress, "exactly one manual Space press"),
    @($AcknowledgeNoPausedAttach, "that attach while paused is forbidden"),
    @($AcknowledgeGuestMemoryReadOnly, "that game memory is read-only"),
    @($AcknowledgeDebugRegistersOnly, "debug-register-only target writes"),
    @($AcknowledgeNoAutomatedInput, "that no automated input is sent"),
    @($AcknowledgeShortPtraceStallRisk, "the short ptrace stall risk")
)) {
    if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" }
}
if ($DurationMs -lt 15000 -or $DurationMs -gt 60000) {
    throw "DurationMs must be 15000..60000"
}
if ($ArmTimeoutMs -lt 1000 -or $ArmTimeoutMs -gt 30000) {
    throw "ArmTimeoutMs must be 1000..30000"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$evidence = Join-Path $root "evidence"
$stdout = Join-Path $evidence "submission_affinity_background_$stamp.stdout.txt"
$stderr = Join-Path $evidence "submission_affinity_background_$stamp.stderr.txt"
$report = Join-Path $evidence "a9tas_game_action_submission_affinity_$stamp.a9asa1.txt"
$powershell = Join-Path $PSHOME "pwsh.exe"
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    $powershell = Join-Path $env:SystemRoot `
        "System32\WindowsPowerShell\v1.0\powershell.exe"
}
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    throw "PowerShell host unavailable"
}
$arguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $runner,
    "-Mode", "Capture",
    "-DurationMs", $DurationMs.ToString(),
    "-Device", $Device,
    "-AdbPath", $AdbPath,
    "-ReportOutputPath", $report,
    "-AcknowledgeNaturallyRunningRace",
    "-AcknowledgeExactlyOneManualSpacePress",
    "-AcknowledgeNoPausedAttach",
    "-AcknowledgeGuestMemoryReadOnly",
    "-AcknowledgeDebugRegistersOnly",
    "-AcknowledgeNoAutomatedInput",
    "-AcknowledgeShortPtraceStallRisk"
)
$process = Start-Process -FilePath $powershell -ArgumentList $arguments `
    -WindowStyle Hidden -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr -PassThru

$deadline = [DateTime]::UtcNow.AddMilliseconds($ArmTimeoutMs)
do {
    $process.Refresh()
    $text = if (Test-Path -LiteralPath $stdout) {
        Get-Content -LiteralPath $stdout -Raw
    } else { "" }
    if ($text -match "GAME_ACTION_SUBMISSION_AFFINITY_V1_ARMED") {
        Write-Host "GAME_ACTION_SUBMISSION_BACKGROUND_ARMED process_id=$($process.Id) duration_ms=$DurationMs input_sent=0"
        Write-Host "stdout=$stdout"
        Write-Host "stderr=$stderr"
        Write-Host "report=$report"
        return
    }
    if ($process.HasExited) {
        $errorText = if (Test-Path -LiteralPath $stderr) {
            Get-Content -LiteralPath $stderr -Raw
        } else { "" }
        throw "Background runner exited before ARMED (exit=$($process.ExitCode)): $errorText"
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)

throw "Background runner did not report ARMED within $ArmTimeoutMs ms; inspect $stdout and $stderr"
