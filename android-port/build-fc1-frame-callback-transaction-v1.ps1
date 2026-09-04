param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\fc1_frame_callback_transaction_controller_v1.cpp"
$policy = Join-Path $root "tools\test_fc1_frame_callback_transaction_policy_v1.py"
$outDir = Join-Path $root "build\fc1-frame-callback-transaction-v1"
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

$passive = Join-Path $outDir "fc1_frame_callback_transaction_v1_build_only"
$liveObject = Join-Path $outDir "fc1_frame_callback_transaction_v1_review_only.o"
$liveDisassembly = Join-Path $outDir "fc1_frame_callback_transaction_v1_review_only.disasm.txt"
$payload = Join-Path $root "build\frame-callback-bootstrap-v1\liba9tas_frame_callback_bootstrap_v1_build_only.so"
if (-not (Test-Path -LiteralPath $payload)) {
    throw "FC-0 passive payload must be built before FC-1 review"
}

& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "FC-1 passive controller build failed" }

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_FC1_LIVE_CANDIDATE=1" "-c" "-o" $liveObject
if ($LASTEXITCODE -ne 0) { throw "FC-1 live review object build failed" }

# Link the complete path only into a uniquely named temporary shared object so
# --no-undefined validates the whole dependency closure.  Verify the resolved
# path is inside the system temp directory and remove it in all cases; no
# runnable/live-linked FC-1 artifact is retained in the workspace.
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$linkCheck = [System.IO.Path]::GetFullPath(
    (Join-Path $tempRoot ("a9tas-fc1-linkcheck-{0}.so" -f $PID)))
if (-not $linkCheck.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "FC-1 link-check path escaped the system temp directory"
}
try {
    & $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
        "-fno-exceptions" "-fno-rtti" "-fPIC" "-shared" `
        "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
        "-DA9TAS_FC1_LIVE_CANDIDATE=1" "-Wl,--no-undefined" `
        "-o" $linkCheck
    if ($LASTEXITCODE -ne 0) { throw "FC-1 ephemeral link check failed" }
    $linkHeader = (& $readelf -h $linkCheck) -join "`n"
    if ($linkHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
        $linkHeader -notmatch "Type:\s+DYN") {
        throw "FC-1 ephemeral link-check ELF is invalid"
    }
} finally {
    if (Test-Path -LiteralPath $linkCheck) {
        Remove-Item -LiteralPath $linkCheck -Force
    }
}
if (Test-Path -LiteralPath $linkCheck) {
    throw "FC-1 ephemeral link-check artifact was retained"
}

& $objdump "-d" "--demangle" $liveObject | Set-Content -LiteralPath $liveDisassembly
if ($LASTEXITCODE -ne 0) { throw "FC-1 review-object disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $liveObject $readelf $objdump $payload
if ($LASTEXITCODE -ne 0) { throw "FC-1 policy verification failed" }
& $python.Source (Join-Path $root "tools\test_frame_callback_bootstrap_policy_v1.py") `
    $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-0 payload policy regression failed" }
& $python.Source (Join-Path $root "tools\test_fc1_payload_elf_resolver_v1.py") `
    $payload
if ($LASTEXITCODE -ne 0) { throw "FC-1 payload ELF resolver audit failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$objectHash = (Get-FileHash -LiteralPath $liveObject -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $liveDisassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FC1_TRANSACTION_BUILD passed=1 runtime=disabled live_object=unlinked link_check=ephemeral_pass device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disasmHash"
