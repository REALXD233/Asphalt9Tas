param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_startline_tick_recorder_v1.cpp"
$protocol = Join-Path $root "src\startline_prearm_protocol_v1.cpp"
$outputDirectory = Join-Path $root "build\startline-tick-recorder-v1"
$output = Join-Path $outputDirectory "a9tas_hwbp_startline_tick_recorder_v1"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"

foreach ($path in @($source, $protocol, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing startline build input: $path"
    }
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$arguments = @(
    "-O2", "-std=c++20", "-static-libstdc++",
    "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
    "-DA9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN",
    $source, $protocol, "-Wl,--no-undefined", "-o", $output
)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    throw "startline tick recorder build failed (exit=$LASTEXITCODE)"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
Write-Host "STARTLINE_TICK_RECORDER_BUILD passed=1 deployed=0"
Write-Host "binary=$output"
Write-Host "sha256=$hash"
