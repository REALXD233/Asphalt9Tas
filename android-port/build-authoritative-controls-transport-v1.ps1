param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$out = Join-Path $root "build\authoritative-controls-transport-v1"
$source = Join-Path $src "authoritative_controls_transport_v1.cpp"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing controls transport input: $path" }
}
New-Item -ItemType Directory -Path $out -Force | Out-Null
$binary = Join-Path $out "authoritative_controls_transport_v1_selftest"
$object = Join-Path $out "authoritative_controls_transport_v1.o"
$flags = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror")
& $compiler @flags "-static-libstdc++" "-DA9TAS_AUTHORITATIVE_CONTROLS_TRANSPORT_SELFTEST=1" $source "-o" $binary
if ($LASTEXITCODE -ne 0) { throw "controls transport selftest build failed" }
& $compiler @flags "-DA9TAS_AUTHORITATIVE_CONTROLS_TRANSPORT_NO_MAIN=1" "-c" $source "-o" $object
if ($LASTEXITCODE -ne 0) { throw "controls transport object build failed" }
Write-Host "AUTHORITATIVE_CONTROLS_TRANSPORT_BUILD passed=1 deployed=0"
foreach ($artifact in @($binary, $object)) {
    Write-Host "$([IO.Path]::GetFileName($artifact))_sha256=$((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower())"
}
