# analyze_field_scan3.ps1 - wide-window (32KB) transform/velocity locator.
# Finds float blocks that stay stable in the menu segment and change
# continuously during the race segment (racer transform = 16 consecutive
# floats, velocity = 3 consecutive floats).
# usage: powershell -File analyze_field_scan3.ps1 -Path <field_scan3.bin>
param(
    [Parameter(Mandatory = $true)][string]$Path
)

$bytes = [System.IO.File]::ReadAllBytes($Path)
$magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
if ($magic -ne 'A9FSCN1') { Write-Error "bad magic: $magic"; exit 2 }
$base = [BitConverter]::ToUInt64($bytes, 8)
$owner = [BitConverter]::ToUInt64($bytes, 16)
$startA = [BitConverter]::ToUInt64($bytes, 24)
$count = [BitConverter]::ToUInt64($bytes, 40)
$window = [BitConverter]::ToUInt32($bytes, 52)
$ctrl = [BitConverter]::ToUInt64($bytes, 56)
$stride = 8 + $window
$recSize = if ($ctrl -ne 0) { $stride * 2 } else { $stride }
"count=$count window=$window owner=0x$('{0:x}' -f $owner) startA=0x$('{0:x}' -f $startA) hasCtrl=$($ctrl -ne 0)"

# Decimate by 3 for speed: every 3rd sample.
$idx = @()
for ($i = 0; $i -lt $count; $i += 3) { $idx += $i }
$n = $idx.Count
"decimated samples: $n"

# Sample one float column across the decimated series.
function Get-FloatSeries([int]$off) {
    $s = New-Object System.Collections.Generic.List[float]
    foreach ($i in $idx) {
        $o = 64 + $i * $recSize + 8 + $off
        if ($o + 4 -gt $bytes.Length) { break }
        $s.Add([BitConverter]::ToSingle($bytes, $o))
    }
    return , $s
}

$menuEnd = [Math]::Max(1, [int]($n * 0.25))
$raceStart = [Math]::Max($menuEnd, [int]($n * 0.40))

$candidates = @()
$step = 4
for ($off = 0; $off + 4 -le $window; $off += $step) {
    $series = Get-FloatSeries $off
    if ($series.Count -lt $raceStart + 2) { continue }
    # Menu stability: max abs deviation from median over menu segment.
    $menuVals = $series.GetRange(0, $menuEnd)
    $menuMin = ($menuVals | Measure-Object -Minimum).Minimum
    $menuMax = ($menuVals | Measure-Object -Maximum).Maximum
    $menuSpan = [float]$menuMax - [float]$menuMin
    # Race activity: count of large per-sample deltas in race segment.
    $big = 0
    $raceSpan = [float]0
    $first = $null
    foreach ($i in ($raceStart..($series.Count - 1))) {
        $v = $series[$i]
        if ($null -eq $first) { $first = $v } else {
            $d = [math]::Abs($v - $first)
            if ($d -gt 0.01) { $big++ }
            $first = $v
        }
        $raceSpan = [math]::Max($raceSpan, [math]::Abs($v - $series[$raceStart]))
    }
    if ($menuSpan -lt 0.5 -and $big -gt ($series.Count - $raceStart) * 0.5) {
        $candidates += [PSCustomObject]@{
            Off = ('0x{0:x}' -f $off)
            Abs = ('0x{0:x}' -f ($startA + $off))
            MenuSpan = ('{0:0.000}' -f $menuSpan)
            RaceSpan = ('{0:0.0}' -f $raceSpan)
            BigDeltas = $big
        }
    }
}

"--- active float candidates (menu-stable, race-changing) ---"
if ($candidates.Count -eq 0) { 'none' } else {
    $candidates | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    # Group consecutive offsets into blocks (transform/velocity runs).
    $sorted = $candidates | ForEach-Object { [Convert]::ToUInt64($_.Off, 16) } | Sort-Object
    $blocks = @()
    $blockStart = $sorted[0]; $prev = $sorted[0]
    for ($i = 1; $i -lt $sorted.Count; $i++) {
        if ($sorted[$i] -ne $prev + 4) {
            $blocks += [PSCustomObject]@{ Start = ('0x{0:x}' -f $blockStart); End = ('0x{0:x}' -f $prev); Len = (($prev - $blockStart) / 4 + 1) }
            $blockStart = $sorted[$i]
        }
        $prev = $sorted[$i]
    }
    $blocks += [PSCustomObject]@{ Start = ('0x{0:x}' -f $blockStart); End = ('0x{0:x}' -f $prev); Len = (($prev - $blockStart) / 4 + 1) }
    '--- contiguous float blocks (len>=3 candidates: transform/velocity) ---'
    $blocks | Where-Object { $_.Len -ge 3 } | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
}
