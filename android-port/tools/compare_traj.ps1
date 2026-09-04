# compare_traj.ps1 - compare recorded result-state trajectory vs a replay
# trajectory capture (A9TRAJV1). Aligns both to the first significant motion
# sample, then reports per-sample position deltas.
#
# usage:
#   powershell -File compare_traj.ps1 -Rec <hwbp_rec.bin> -Traj <traj_capture.bin>
param(
    [Parameter(Mandatory = $true)][string]$Rec,
    [Parameter(Mandatory = $true)][string]$Traj
)

# --- Load recording frames (A9HDPV2, 112-byte frames) ---
$rb = [System.IO.File]::ReadAllBytes($Rec)
$rmagic = [System.Text.Encoding]::ASCII.GetString($rb, 0, 8)
if ($rmagic -ne 'A9HDPV2') { Write-Error "rec magic: $rmagic"; exit 2 }
$rCount = [BitConverter]::ToUInt64($rb, 40)
$recTraj = @()  # list of position vec3 (transform[12..14] translation)
for ($i = 0; $i -lt $rCount; $i++) {
    $o = 64 + $i * 112
    if ($o + 112 -gt $rb.Length) { break }
    $t = [BitConverter]::ToUInt64($rb, $o)
    $seq = [BitConverter]::ToUInt32($rb, $o + 8)
    $tr = @(
        [BitConverter]::ToSingle($rb, $o + 24 + 48),
        [BitConverter]::ToSingle($rb, $o + 24 + 52),
        [BitConverter]::ToSingle($rb, $o + 24 + 56)
    )
    $recTraj += , @($t, $seq, $tr)
}
"rec frames: $($recTraj.Count) (with transform: $(($recTraj | Where-Object { $_.Count -gt 0 -and $_.[2][0] -ne 0 }).Count))"

# --- Load trajectory capture ---
$tb = [System.IO.File]::ReadAllBytes($Traj)
$tmagic = [System.Text.Encoding]::ASCII.GetString($tb, 0, 8)
if ($tmagic -ne 'A9TRAJV1') { Write-Error "traj magic: $tmagic"; exit 3 }
$tCount = [BitConverter]::ToUInt64($tb, 32)
$traj = @()
for ($i = 0; $i -lt $tCount; $i++) {
    $o = 48 + $i * 88
    if ($o + 88 -gt $tb.Length) { break }
    $t = [BitConverter]::ToUInt64($tb, $o)
    $pos = @(
        [BitConverter]::ToSingle($tb, $o + 8 + 48),
        [BitConverter]::ToSingle($tb, $o + 8 + 52),
        [BitConverter]::ToSingle($tb, $o + 8 + 56)
    )
    $traj += , @($t, $pos)
}
"traj samples: $($traj.Count)"

# --- Align at first motion (translation moves > 0.05 from origin sample) ---
function Get-MotionStart($seq) {
    for ($i = 1; $i -lt $seq.Count; $i++) {
        $d = [math]::Sqrt(
            [math]::Pow($seq[$i][2][0] - $seq[0][2][0], 2) +
            [math]::Pow($seq[$i][2][1] - $seq[0][2][1], 2) +
            [math]::Pow($seq[$i][2][2] - $seq[0][2][2], 2))
        if ($d -gt 0.05) { return $i }
    }
    return 0
}
$recStart = Get-MotionStart $recTraj
$trajStart = Get-MotionStart $traj
"rec motion start: sample $recStart, traj motion start: sample $trajStart"

# --- Compare aligned segments (use min length) ---
$len = [Math]::Min($recTraj.Count - $recStart, $traj.Count - $trajStart)
$maxErr = 0.0; $sumErr = 0.0; $n = 0
for ($i = 0; $i -lt $len; $i++) {
    $a = $recTraj[$recStart + $i][2]
    $b = $traj[$trajStart + $i][2]
    $d = [math]::Sqrt([math]::Pow($a[0] - $b[0], 2) + [math]::Pow($a[1] - $b[1], 2) + [math]::Pow($a[2] - $b[2], 2))
    if ($d -gt $maxErr) { $maxErr = $d }
    $sumErr += $d; $n++
}
if ($n -gt 0) {
    $avgErr = $sumErr / $n
    "compared $n aligned samples: avg pos err = $('{0:0.000}' -f $avgErr), max pos err = $('{0:0.000}' -f $maxErr)"
    if ($avgErr -lt 1.0) { 'VERDICT: HIGH consistency (avg err < 1.0)' }
    elseif ($avgErr -lt 5.0) { 'VERDICT: MEDIUM consistency (1.0 <= avg err < 5.0)' }
    else { 'VERDICT: LOW consistency (avg err >= 5.0)' }
} else {
    'nothing to compare'
}
