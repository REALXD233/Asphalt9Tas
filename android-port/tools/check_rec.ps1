# check_rec.ps1 — 校验 a9tas bridge 录制文件：头格式、帧数、tick 连续性与输入活跃度。
#
# 用法: powershell -File check_rec.ps1 -Path <rec.bin>
param(
    [Parameter(Mandatory = $true)][string]$Path
)

$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -lt 48) { Write-Error "文件过小（$($bytes.Length) B）"; exit 2 }
$magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
if ($magic -ne 'A9RBFREC') { Write-Error "magic 不符（$magic）"; exit 3 }

$count = [BitConverter]::ToUInt32($bytes, 8)
$frameSize = [BitConverter]::ToUInt32($bytes, 12)
"magic=$magic count=$count frame_size=$frameSize file_size=$($bytes.Length)"

$expect = 48 + $count * $frameSize
if ($expect -ne $bytes.Length) {
    $real = [int](($bytes.Length - 48) / $frameSize)
    Write-Warning "长度不一致：header 声明 $count 帧，按文件大小实际 $real 帧（用 fix_rec_count.ps1 修正）"
    $count = $real
}

if ($count -eq 0) { Write-Error "0 帧（recorder 被 kill 且未封存？用 fix_rec_count.ps1）"; exit 4 }

$ticks = New-Object System.Collections.Generic.List[uint64]
$steerNonZero = 0; $brakeNonZero = 0; $validOnes = 0
$tickPrev = [uint64]0; $first = $true; $gaps = 0; $dups = 0; $desc = 0
for ($i = 0; $i -lt $count; $i++) {
    $off = 48 + $i * $frameSize
    $tick = [BitConverter]::ToUInt64($bytes, $off)
    $steer = [BitConverter]::ToUInt32($bytes, $off + 16)
    $brake = [BitConverter]::ToUInt32($bytes, $off + 20)
    $valid = [BitConverter]::ToUInt32($bytes, $off + 24)
    $ticks.Add($tick)
    if ($steer -ne 0) { $steerNonZero++ }
    if ($brake -ne 0) { $brakeNonZero++ }
    if ($valid -eq 1) { $validOnes++ }
    if (-not $first) {
        if ($tick -eq $tickPrev) { $dups++ }
        elseif ($tick -lt $tickPrev) { $desc++ }
        elseif ($tick -gt $tickPrev + 1) { $gaps += ($tick - $tickPrev - 1) }
    }
    $first = $false; $tickPrev = $tick
}

"tick 范围: first=$($ticks[0]) last=$($ticks[$count-1])"
"tick 单调: 重复=$dups 回退=$desc 缺号合计=$gaps"
"输入活跃: steer!=0 帧=$steerNonZero ($([math]::Round(100.0*$steerNonZero/$count,1))%)  brake!=0 帧=$brakeNonZero ($([math]::Round(100.0*$brakeNonZero/$count,1))%)  valid=1 帧=$validOnes"

if ($dups -gt 0 -or $desc -gt 0) { Write-Warning "tick 序列异常（重复/回退），回放匹配可能错位" }
if ($validOnes -lt $count) { Write-Warning "部分帧 valid=0（$($count-$validOnes) 帧），这些帧回放时会被忽略吗？——payload 侧语义需确认" }
