# analyze_field_scan2.ps1 - analyze dual-window field scan (owner + controller).
# usage: powershell -File analyze_field_scan2.ps1 -Path <field_scan2.bin>
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
"count=$count window=$window owner=0x$('{0:x}' -f $owner) startA=0x$('{0:x}' -f $startA) controller=0x$('{0:x}' -f $ctrl) size=$($bytes.Length)"

$hasCtrl = $ctrl -ne 0
$stride = 8 + $window
$recSize = if ($hasCtrl) { $stride * 2 } else { $stride }
"hasCtrl=$hasCtrl recSize=$recSize"

# Parse sample times and both window buffers.
$times = @()
$winA = @()
$winB = @()
for ($i = 0; $i -lt $count; $i++) {
    $off = 64 + $i * $recSize
    if ($off + $recSize -gt $bytes.Length) { break }
    $times += [BitConverter]::ToUInt64($bytes, $off)
    $a = New-Object byte[] $window
    [Array]::Copy($bytes, $off + 8, $a, 0, $window)
    $winA += , $a
    if ($hasCtrl) {
        $b = New-Object byte[] $window
        [Array]::Copy($bytes, $off + $stride, $b, 0, $window)
        $winB += , $b
    }
}
$n = $winA.Count
"parsed samples: $n"

function Analyze-Window([string]$Name, [byte[][]]$Wins, [uint64]$BaseAddr) {
    "=== $Name (base 0x$('{0:x}' -f $BaseAddr)) ==="
    $cands = @()
    $menuEnd = [Math]::Max(1, [int]($n * 0.25))
    $raceStart = [Math]::Max($menuEnd, [int]($n * 0.40))
    for ($off = 0; $off + 4 -le $window; $off += 4) {
        $menuMin = [uint32]::MaxValue; $menuMax = [uint32]::MinValue
        $raceMin = [uint32]::MaxValue; $raceMax = [uint32]::MinValue
        $menuMono = $true; $raceMono = $true
        $prevMenu = [uint32]0; $prevRace = [uint32]0
        $raceDeltas = @()
        $menuDeltas = @()
        for ($i = 0; $i -lt $n; $i++) {
            $v = [BitConverter]::ToUInt32($Wins[$i], $off)
            if ($i -lt $menuEnd) {
                if ($v -lt $menuMin) { $menuMin = $v }
                if ($v -gt $menuMax) { $menuMax = $v }
                if ($i -gt 0) {
                    $d = [int64]$v - [int64]$prevMenu
                    if ($d -lt 0) { $menuMono = $false }
                    $menuDeltas += $d
                }
                $prevMenu = $v
            } elseif ($i -ge $raceStart) {
                if ($v -lt $raceMin) { $raceMin = $v }
                if ($v -gt $raceMax) { $raceMax = $v }
                if ($i -gt $raceStart) {
                    $d = [int64]$v - [int64]$prevRace
                    if ($d -lt 0) { $raceMono = $false }
                    $raceDeltas += $d
                }
                $prevRace = $v
            }
        }
        $menuSpread = [uint64]$menuMax - [uint64]$menuMin
        $raceSpread = [uint64]$raceMax - [uint64]$raceMin
        $raceMaxD = if ($raceDeltas.Count -gt 0) { ($raceDeltas | Measure-Object -Maximum).Maximum } else { -1 }
        $menuMaxD = if ($menuDeltas.Count -gt 0) { ($menuDeltas | Measure-Object -Maximum).Maximum } else { -1 }
        # Race tick candidate: menu flat, race monotonic small increments.
        if ($menuSpread -le 2 -and $raceMono -and $raceSpread -ge 10 -and $raceSpread -lt 100000 -and $raceMaxD -le 8) {
            $cands += [PSCustomObject]@{ Kind = 'TICK'; Off = ('0x{0:x}' -f $off); Abs = ('0x{0:x}' -f ($BaseAddr + $off)); Menu = "$menuMin..$menuMax"; Race = "$raceMin..$raceMax"; Span = $raceSpread; MaxD = $raceMaxD }
        }
        # State flip candidate: menu flat low, race jumps to a different stable value.
        elseif ($menuSpread -le 1 -and $raceSpread -le 3 -and $raceMax -ne $menuMax) {
            $cands += [PSCustomObject]@{ Kind = 'STATE'; Off = ('0x{0:x}' -f $off); Abs = ('0x{0:x}' -f ($BaseAddr + $off)); Menu = "$menuMin..$menuMax"; Race = "$raceMin..$raceMax"; Span = $raceSpread; MaxD = $raceMaxD }
        }
        # Race counter candidate: increments in both but rate changes, or starts at 0 in race.
        elseif ($raceMono -and $raceSpread -ge 5 -and $raceSpread -lt 100000 -and $raceMaxD -le 8 -and $menuMaxD -le 8) {
            $cands += [PSCustomObject]@{ Kind = 'CNT?'; Off = ('0x{0:x}' -f $off); Abs = ('0x{0:x}' -f ($BaseAddr + $off)); Menu = "$menuMin..$menuMax"; Race = "$raceMin..$raceMax"; Span = $raceSpread; MaxD = $raceMaxD }
        }
    }
    if ($cands.Count -eq 0) { '  no candidates' } else {
        $cands | Sort-Object Kind | Format-Table -AutoSize | Out-String | Write-Host
    }
}

Analyze-Window 'OWNER' $winA $startA
if ($hasCtrl) { Analyze-Window 'CONTROLLER' $winB ($ctrl - 0x400) }
