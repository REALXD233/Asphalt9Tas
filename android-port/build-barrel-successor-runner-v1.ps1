param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = Join-Path $root "run-final-writer-natural-action-composite-v1.ps1"
$generator = Join-Path $root "tools\generate_barrel_successor_runner_v1.py"
$policy = Join-Path $root "tools\test_run_barrel_successor_replay_policy_v1.py"
$outDir = Join-Path $root "build\barrel-successor-runner-v1"
$runner = Join-Path $outDir "run-barrel-successor-replay-v1.ps1"
foreach ($path in @($base, $generator, $policy)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Barrel successor runner input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
python -B $generator $base $runner
if ($LASTEXITCODE -ne 0) { throw "Barrel successor runner generation failed" }
$tokens = $null
$errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $runner, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) {
    throw "Generated Barrel successor runner has PowerShell syntax errors: $($errors[0])"
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "Barrel successor runner policy failed" }
$hash = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "BARREL_SUCCESSOR_RUNNER_BUILD passed=1 base_unchanged=1 live_default=0 device_access=0"
Write-Output "runner=$runner"
Write-Output "runner_sha256=$hash"
