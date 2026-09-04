# analyze_field_scan.ps1 - find the race tick counter in a field scan log.
# usage: powershell -File analyze_field_scan.ps1 -Path <field_scan.bin>
param(
    [Parameter(Mandatory = $true)][string]$Path
)

$bytes = [System.IO.File]::ReadAllBytes($Path)
$magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
if ($magic -ne 'A9FSCN1') { Write-Error "bad magic: $magic"; exit 2 }

$base = [BitConverter]::ToUInt64($bytes, 8)
$owner = [BitConverter]::ToUInt64($bytes, 16)
$startAddr = [BitConverter]::ToUInt64($bytes, 24)
$startMs = [BitConverter]::ToUInt64($bytes, 32)
$count = [BitConverter]::ToUInt64($bytes, 40)
$interval = [BitConverter]::ToUInt32($bytes, 48)
$window = [BitConverter]::ToUInt32($bytes, 52)
$hdr = 64
$recSize = 8 + $window
"count=$count interval_ms=$interval window=$window file_size=$($bytes.Length)"

# Parse all samples.
$samples = @()
for ($i = 0; $i -lt $count; $i++) {
    $off = $hdr + $i * $recSize
    if ($off + $recSize -gt $bytes.Length) { break }
    $t = [BitConverter]::ToUInt64($bytes, $off)
    $data = New-Object byte[] $window
    [Array]::Copy($bytes, $off + 8, $data, 0, $window)
    $samples += , @($t, $data)
}
"parsed samples: $($samples.Count)"

# Analyze each aligned u32 across the sample series.
$best = @()
for ($off = 0; $off + 4 -le $window; $off += 4) {
    $vals = New-Object System.Collections.Generic.List[uint32]
    foreach ($s in $samples) {
        $vals.Add([BitConverter]::ToUInt32($s[1], $off))
    }
    # Menu segment: first 25% of samples; race segment: last 60%.
    $menuEnd = [int]($vals.Count * 0.25)
    $raceStart = [int]($vals.Count * 0.40)
    $menuVals = $vals.GetRange(0, [Math]::Max(1, $menuEnd))
    $raceVals = $vals.GetRange($raceStart, $vals.Count - $raceStart)
    $menuMin = ($menuVals | Measure-Object -Minimum).Minimum
    $menuMax = ($menuVals | Measure-Object -Maximum).Maximum
    $raceMin = ($raceVals | Measure-Object -Minimum).Minimum
    $raceMax = ($raceVals | Measure-Object -Maximum).Maximum
    # Increment candidates: race range small-ish, monotonic, delta 0..4 per sample.
    $deltas = @()
    $mono = $true
    for ($i = 1; $i -lt $raceVals.Count; $i++) {
        $d = [int64]$raceVals[$i] - [int64]$raceVals[$i - 1]
        if ($d -lt 0) { $mono = $false; break }
        $deltas += $d
    }
    $maxDelta = if ($deltas.Count -gt 0) { ($deltas | Measure-Object -Maximum).Maximum } else { -1 }
    $raceSpan = $raceMax - $raceMin
    if ($mono -and $raceSpan -gt 5 -and $raceSpan -lt 20000 -and $maxDelta -le 8 -and $menuMax -le $raceMax) {
        $best += [PSCustomObject]@{
            Off = $off
            Abs = ('0x{0:x}' -f ($startAddr + $off))
            Menu = "$menuMin..$menuMax"
            Race = "$raceMin..$raceMax"
            Span = $raceSpan
            MaxDelta = $maxDelta
        }
    }
}
"--- candidates (monotonic u32 in race segment) ---"
if ($best.Count -eq 0) { 'none found' } else {
    $best | Sort-Object Span | Select-Object -First 25 | Format-Table -AutoSize
}
