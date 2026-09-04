# fix_rec_count.ps1 — 修正 a9tas bridge 录制文件的头计数（count 字段）。
# 背景：recorder 只有在自然跑满 MAX_FRAMES 退出时才写 header.count；
# 若比赛提前结束被 kill，count 仍为 0，player 会拒绝加载（"bad recording header"）。
# 本脚本按文件大小重建 count = (filesize - 48) / 32（RecHeader 48B + Frame 32B）。
#
# 用法: powershell -File fix_rec_count.ps1 -Path <rec.bin> [-DryRun]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [switch]$DryRun
)

$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -lt 48) {
    Write-Error "文件过小（$($bytes.Length) B），不是有效录制文件"
    exit 2
}
$magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
if ($magic -ne 'A9RBFREC') {
    Write-Error "magic 不符（$magic），不是 a9tas bridge 录制文件"
    exit 3
}
$frameSize = [BitConverter]::ToUInt32($bytes, 12)
if ($frameSize -ne 32) {
    Write-Error "frame_size 异常（$frameSize），预期 32"
    exit 4
}
$count = [int](($bytes.Length - 48) / 32)
$oldCount = [BitConverter]::ToUInt32($bytes, 8)
if (($bytes.Length - 48) % 32 -ne 0) {
    Write-Warning "文件长度非 32 对齐：$($bytes.Length)（尾 $((($bytes.Length - 48) % 32)) B 将被忽略）"
}
if ($DryRun) {
    "DRY-RUN: $Path old_count=$oldCount new_count=$count size=$($bytes.Length)"
    exit 0
}
[BitConverter]::GetBytes([uint32]$count).CopyTo($bytes, 8)
[System.IO.File]::WriteAllBytes($Path, $bytes)
"FIXED: $Path count $oldCount -> $count (size=$($bytes.Length))"
