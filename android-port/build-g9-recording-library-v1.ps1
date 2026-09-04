param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$codec = Join-Path $root 'tools\a9tas_recording_v1.py'
$library = Join-Path $root 'tools\a9tas_library_v1.py'
$tests = Join-Path $root 'tools\test_a9tas_recording_v1.py'
$schema = Join-Path $root 'docs\A9TAS1_RECORDING_SCHEMA.md'

foreach ($path in @($codec,$library,$tests,$schema)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G9 recording input: $path"
    }
}

$source = (Get-Content -LiteralPath $codec -Raw) +
          (Get-Content -LiteralPath $library -Raw)
foreach ($token in @('MAGIC = b"A9TAS1\0\0"',
                      'canonical_manifest_bytes',
                      'decode_a9g4r2',
                      'hmac.compare_digest',
                      'archive.recording_hash',
                      'manifest.recording_identity',
                      'os.replace',
                      'delete.requires_yes',
                      'delete.outside_library')) {
    if (-not $source.Contains($token)) {
        throw "G9 schema policy token missing: $token"
    }
}
foreach ($forbidden in @('subprocess', 'adb.exe', 'ptrace', '/proc/')) {
    if ($source.Contains($forbidden)) {
        throw "G9 offline recording library contains forbidden runtime token: $forbidden"
    }
}

python -m py_compile $codec $library $tests
if ($LASTEXITCODE -ne 0) { throw 'G9 Python syntax check failed' }
python -B $tests
if ($LASTEXITCODE -ne 0) { throw 'G9 recording selftest failed' }

$codecSha = (Get-FileHash -LiteralPath $codec -Algorithm SHA256).Hash.ToLowerInvariant()
$librarySha = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "G9_RECORDING_LIBRARY_BUILD passed=1 schema=A9TAS1 version=1 header=160 raw_a9g4r2_roundtrip=1 atomic_writes=1 device_access=0"
Write-Output "codec_sha256=$codecSha"
Write-Output "library_sha256=$librarySha"
