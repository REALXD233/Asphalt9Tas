param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\fc2_frame_callback_transaction_controller_v1.cpp"
$policy = Join-Path $root "tools\test_fc2_frame_callback_transaction_policy_v1.py"
$payload = Join-Path $root "build\frame-callback-deferred-registration-v1\liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$outDir = Join-Path $root "build\fc2-frame-callback-transaction-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}
if (-not (Test-Path -LiteralPath $payload)) {
    throw "FC-2 payload must be built before transaction review"
}

$passive = Join-Path $outDir "fc2_frame_callback_transaction_v1_build_only"
$reviewObject = Join-Path $outDir "fc2_frame_callback_transaction_v1_review_only.o"
$disassembly = Join-Path $outDir "fc2_frame_callback_transaction_v1_review_only.disasm.txt"

& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "FC-2 passive controller build failed" }

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_FC2_LIVE_CANDIDATE=1" "-c" "-o" $reviewObject
if ($LASTEXITCODE -ne 0) { throw "FC-2 review object build failed" }

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$linkCheck = [System.IO.Path]::GetFullPath(
    (Join-Path $tempRoot ("a9tas-fc2-linkcheck-{0}.so" -f $PID)))
if (-not $linkCheck.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "FC-2 link-check path escaped the system temp directory"
}
try {
    & $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
        "-fno-exceptions" "-fno-rtti" "-fPIC" "-shared" `
        "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
        "-DA9TAS_FC2_LIVE_CANDIDATE=1" "-Wl,--no-undefined" `
        "-o" $linkCheck
    if ($LASTEXITCODE -ne 0) { throw "FC-2 ephemeral link check failed" }
    $linkHeader = (& $readelf -h $linkCheck) -join "`n"
    if ($linkHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
        $linkHeader -notmatch "Type:\s+DYN") {
        throw "FC-2 ephemeral link-check ELF is invalid"
    }
} finally {
    if (Test-Path -LiteralPath $linkCheck) {
        Remove-Item -LiteralPath $linkCheck -Force
    }
}
if (Test-Path -LiteralPath $linkCheck) {
    throw "FC-2 ephemeral link-check artifact was retained"
}

& $objdump "-d" "--demangle" $reviewObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-2 controller disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $reviewObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-2 transaction policy failed" }
& $python.Source (Join-Path $root "tools\test_frame_callback_deferred_registration_policy_v1.py") `
    $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-2 payload policy regression failed" }
& $python.Source (Join-Path $root "tools\test_fc2_payload_elf_resolver_v1.py") $payload
if ($LASTEXITCODE -ne 0) { throw "FC-2 resolver regression failed" }
& $python.Source (Join-Path $root "tools\validate_fc2_report_v1.py") "--selftest"
if ($LASTEXITCODE -ne 0) { throw "FC-2 report-validator regression failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$objectHash = (Get-FileHash -LiteralPath $reviewObject -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FC2_TRANSACTION_BUILD passed=1 runtime=disabled live_object=unlinked link_check=ephemeral_pass device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disasmHash"
