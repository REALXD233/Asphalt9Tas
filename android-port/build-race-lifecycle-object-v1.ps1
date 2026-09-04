param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\race_lifecycle_object_check_v1.cpp"
$policy = Join-Path $root "tools\test_race_lifecycle_object_resolver_policy_v1.py"
$outDir = Join-Path $root "build\race-lifecycle-object-v1"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$output = Join-Path $outDir "a9tas_race_lifecycle_object_check_v1_review_only"

foreach ($path in @($source, $policy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing race-lifecycle object input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-pie" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "Race-lifecycle object build failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $output
if ($LASTEXITCODE -ne 0) { throw "Race-lifecycle object policy failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "RACE_LIFECYCLE_OBJECT_BUILD passed=1 review_only=1 deployed=0 device_access=0"
Write-Output "review_binary_sha256=$hash"
