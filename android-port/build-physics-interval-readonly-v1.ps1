param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\physics_interval_readonly_observer_v1.cpp"
$g8Source = Join-Path $root "src\g8_profile_physics_interval_readonly_observer_v1.cpp"
$runner = Join-Path $root "run-physics-interval-readonly-v1.ps1"
$policy = Join-Path $root "tools\test_physics_interval_readonly_policy_v1.py"
$parserTests = Join-Path $root "tools\test_parse_physics_interval_readonly_v1.py"
$outDir = Join-Path $root "build\physics-interval-readonly-v1"
$output = Join-Path $outDir "a9tas_physics_interval_readonly_observer_v1"
$g8Output = Join-Path $outDir "a9tas_g8_profile_physics_interval_readonly_observer_v1"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "x86_64 compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" $source "-o" $output
if ($LASTEXITCODE -ne 0) { throw "physics interval read-only build failed" }
& $compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" $g8Source "-o" $g8Output
if ($LASTEXITCODE -ne 0) { throw "G8 profile physics interval read-only build failed" }
python -B $policy $source $runner
if ($LASTEXITCODE -ne 0) { throw "physics interval read-only policy failed" }
python -B $parserTests
if ($LASTEXITCODE -ne 0) { throw "physics interval parser tests failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
$g8Hash = (Get-FileHash -LiteralPath $g8Output -Algorithm SHA256).Hash.ToLower()
Write-Output "a9tas_physics_interval_readonly_observer_v1 sha256=$hash"
Write-Output "a9tas_g8_profile_physics_interval_readonly_observer_v1 sha256=$g8Hash"
Write-Output "PHYSICS_INTERVAL_READONLY_BUILD passed=1 deployed=0 device_access=0 ptrace=0 game_writes=0"
