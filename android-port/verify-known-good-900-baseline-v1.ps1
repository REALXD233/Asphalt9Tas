param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifest = Join-Path $root "baselines\known_good_900_v1.json"
$verifier = Join-Path $root "tools\verify_known_good_900_baseline_v1.py"

foreach ($path in @($manifest, $verifier)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing known-good baseline input: $path"
    }
}

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $verifier $manifest
if ($LASTEXITCODE -ne 0) {
    throw "Known-good 900-frame baseline verification failed"
}
