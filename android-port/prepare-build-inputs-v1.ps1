param([Parameter(Mandatory=$true)][string]$ReferenceWorkspace)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$workspace = Split-Path -Parent $port
$reference = (Resolve-Path -LiteralPath $ReferenceWorkspace).Path
$manifest = Get-Content -Raw -LiteralPath (Join-Path $port 'baselines/known_good_900_exact_interval_v2.json') | ConvertFrom-Json
# These fixtures are external build inputs, not redistributable source assets.
$inputs = @(
    @{ path='apk-analysis/lib/arm64-v8a/libAsphalt9.so'; sha='671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0' },
    @{ path='android-port/evidence/libc_ldplayer9_20260816.so'; sha='0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc' },
    @{ path=('android-port/' + $manifest.live_pass_document); sha=$manifest.live_pass_sha256 }
)
foreach ($item in @($manifest.files) + @($manifest.source_snapshot)) {
    $inputs += @{ path=('android-port/' + $item.path); sha=$item.sha256 }
}
foreach ($item in $inputs) {
    $destination = [IO.Path]::GetFullPath((Join-Path $workspace $item.path))
    $scope = [IO.Path]::GetFullPath($workspace).TrimEnd('\') + '\'
    if (-not $destination.StartsWith($scope, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build fixture destination escapes workspace'
    }
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash -ne $item.sha) {
            throw "Existing file differs; not overwriting: $destination"
        }
        continue
    }
    $source = Join-Path $reference $item.path
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne $item.sha) {
        throw "Reference fixture mismatch: $source"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
Write-Output "BUILD_INPUTS_READY files=$($inputs.Count) source_overwrites=0"
