param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\android_identity_probe_v1.cpp"
$include = Join-Path $root "src"
$outDir = Join-Path $root "build\android-identity-probe-v1"
$assetDir = Join-Path $root "A9TasAndroid\app\src\main\assets\runtime"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$alignmentPolicy = Join-Path $root "tools\assert_elf_load_alignment_v1.ps1"
$targets = @(
    @{ Machine = "arm64"; Compiler = "aarch64-linux-android24-clang++.cmd"; Elf = "AArch64" },
    @{ Machine = "x86_64"; Compiler = "x86_64-linux-android24-clang++.cmd"; Elf = "Advanced Micro Devices X86-64" }
)

foreach ($path in @($source, $readelf, $alignmentPolicy)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing identity probe input: $path" }
}
. $alignmentPolicy
New-Item -ItemType Directory -Force -Path $outDir, $assetDir | Out-Null

foreach ($target in $targets) {
    $compiler = Join-Path $toolBin $target.Compiler
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "Missing NDK compiler: $compiler" }
    $name = "a9tas_identity_probe_$($target.Machine)_v1"
    $output = Join-Path $outDir $name
    $pageFlags = @()
    if ($target.Machine -eq "arm64") {
        $pageFlags = @("-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384")
    }
    & $compiler $source "-I$include" "-O2" "-std=c++20" "-fPIE" "-pie" `
        "-fno-exceptions" "-fno-rtti" "-static-libstdc++" `
        "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" @pageFlags "-o" $output
    if ($LASTEXITCODE -ne 0) { throw "Identity probe build failed: $($target.Machine)" }
    $header = (& $readelf "-h" $output) -join "`n"
    if ($header -notmatch "Type:\s+DYN" -or $header -notmatch "Machine:\s+$([regex]::Escape($target.Elf))") {
        throw "Identity probe ABI mismatch: $($target.Machine)"
    }
    if ($target.Machine -eq "arm64") {
        Assert-A9TasElfLoadAlignment -ReadElf $readelf -ElfPath $output `
            -ExpectedAlignment 0x4000 -Label 'Android identity probe arm64'
    }
    Copy-Item -LiteralPath $output -Destination (Join-Path $assetDir $name) -Force
    $sha = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "IDENTITY_PROBE machine=$($target.Machine) sha256=$sha"
}
