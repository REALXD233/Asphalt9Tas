# check_final_rec.ps1 - validate final_recorder A9FRECV1 recording file.
# usage: powershell -File check_final_rec.ps1 -Path <rec.bin>
param(
    [Parameter(Mandatory = $true)][string]$Path
)

$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -lt 88) { Write-Error "file too small ($($bytes.Length) B)"; exit 2 }
$magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
if ($magic -ne 'A9FRECV1') { Write-Error "bad magic ($magic)"; exit 3 }

$base = [BitConverter]::ToUInt64($bytes, 8)
$inner = [BitConverter]::ToUInt64($bytes, 16)
$owner = [BitConverter]::ToUInt64($bytes, 24)
$startMs = [BitConverter]::ToUInt64($bytes, 32)
$interval = [BitConverter]::ToUInt32($bytes, 40)
$count = [BitConverter]::ToUInt32($bytes, 44)
$c9cOff = [BitConverter]::ToUInt32($bytes, 48)
$c98Off = [BitConverter]::ToUInt32($bytes, 52)

"magic=$magic base=0x$('{0:x}' -f $base) inner=0x$('{0:x}' -f $inner) final_owner=0x$('{0:x}' -f $owner)"
"interval_ms=$interval count=$count c9c_off=0x$('{0:x}' -f $c9cOff) c98_off=0x$('{0:x}' -f $c98Off) file_size=$($bytes.Length)"
$expect = 88 + $count * 16
if ($expect -ne $bytes.Length) {
    $real = [int](($bytes.Length - 88) / 16)
    Write-Warning "size mismatch: header says $count frames, file holds $real (truncated?)"
    $count = $real
}
if ($count -eq 0) { Write-Error "0 frames"; exit 4 }

$tFirst = [BitConverter]::ToUInt64($bytes, 88)
$tLast = [BitConverter]::ToUInt64($bytes, 88 + ($count - 1) * 16)
$c9cMin = [float]::MaxValue; $c9cMax = [float]::MinValue
$c98Min = [float]::MaxValue; $c98Max = [float]::MinValue
$nonZero = 0; $firstDrive = -1; $lastDrive = -1
for ($i = 0; $i -lt $count; $i++) {
    $off = 88 + $i * 16
    $f9 = [BitConverter]::ToSingle($bytes, $off + 8)
    $f8 = [BitConverter]::ToSingle($bytes, $off + 12)
    if ($f9 -lt $c9cMin) { $c9cMin = $f9 }; if ($f9 -gt $c9cMax) { $c9cMax = $f9 }
    if ($f8 -lt $c98Min) { $c98Min = $f8 }; if ($f8 -gt $c98Max) { $c98Max = $f8 }
    if ($f9 -ne 0 -or $f8 -ne 0) {
        $nonZero++
        if ($firstDrive -lt 0) { $firstDrive = $i }
        $lastDrive = $i
    }
}
"t_ms range: $tFirst .. $tLast (span $($tLast - $tFirst) ms)"
"C9C range: $c9cMin .. $c9cMax | C98 range: $c98Min .. $c98Max"
"driving frames (nonzero): #$firstDrive .. #$lastDrive (total $nonZero, $([math]::Round(100.0*$nonZero/$count,1))%)"
if ($nonZero -eq 0) { Write-Warning "all zero - window may not cover the race, or field address invalid" }
