$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$parser = Join-Path $root "tools\parse_m1_executor_report_v1.py"
$tests = Join-Path $root "tools\test_parse_m1_executor_report_v1.py"
foreach ($path in @($parser, $tests)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing M1 report parser input: $path"
    }
}
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source -B $tests
if ($LASTEXITCODE -ne 0) { throw "M1 report parser tests failed" }
$parserHash = (Get-FileHash -LiteralPath $parser -Algorithm SHA256).Hash.ToLowerInvariant()
$testsHash = (Get-FileHash -LiteralPath $tests -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "M1_EXECUTOR_REPORT_PARSER_BUILD passed=1 tests=5 strict_receipts=3 strict_cleanup=3 exact_delta_count=1 pre_next_tick_completion=1 device_access=0"
Write-Output "parser_sha256=$parserHash"
Write-Output "tests_sha256=$testsHash"
