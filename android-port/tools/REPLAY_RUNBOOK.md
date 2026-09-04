# A9TAS HWBP 录/放 Runbook（2026-08-16 v4：全自动锚定，零代码修改）

环境：LDPlayer9 `emulator-5554`，ADB `D:\leidian\LDPlayer9\adb.exe`，PowerShell。

## 核心机制（对照上游 AluTasV2）
- 上游 setter hook（SteeringValue/BrakeValue `*p = 帧值`）≈ 我们的 **HWBP 写监视**
  `final_owner+0xC9C/0xC98`（DR0/DR1，w4）+ trap 时单步回写。
- 上游 `m_race_frame_tick` 索引 ≈ 写事件序号（实测游戏每 tick 写一次两个字段，59Hz）：
  帧 `seq` 从 0 连续，回放端校验连续性。
- 上游 OnRaceStarted/OnRaceEnded ≈ **race 状态字段 `final_owner+0x2C0` 0→1 翻转**
  （DR2 写监视）：录/放都自动等待翻转开始，**无需人工同步**。
- 自动油门：`accel` 恒 1.0 记录，不覆盖。
- 结果状态：帧含 racer transform[16]/velocity[3]（地址可选），用于回放一致性验证。

## 工具与帧格式
- `a9tas_hwbp_dual`（v4）：`record|replay`，帧 112B：
  `t_ms u64 | seq u32 | c9c u32 | c98 u32 | flags u32 | accel u32 | transform f32×16 | velocity f32×3 | reserved u32`
- `a9tas_traj_capture`：回放时并行采 transform/velocity（50ms）→ 一致性对比。
- `a9tas_field_scan`：宽窗口（owner ±16KB）字段扫描 → transform/velocity 定位。
- host 侧：`tools/analyze_field_scan3.ps1`（定位）、`tools/compare_traj.ps1`（对比）。

## 标准流程
```powershell
$adb='D:\leidian\LDPlayer9\adb.exe'
$gpid = (& $adb -s emulator-5554 shell "pidof com.aligames.kuang.kybc.aligames").Trim()
$base = ((& $adb -s emulator-5554 shell "su -c 'grep libAsphalt9 /proc/$gpid/maps | head -1'") -split '[- ]')[0]
# 0) 定位 transform/velocity（一次扫描比赛，90s 窗口覆盖大厅→比赛→驾驶）
& $adb -s emulator-5554 shell "su -c 'nohup /data/local/tmp/a9tas_field_scan $gpid $base 90000 /data/local/tmp/fs.bin > /data/local/tmp/fs.log 2>&1 &'"
# 用户开 30s 比赛；跑完后拉取分析（找比赛段连续变化的 float 块）
powershell -File tools\analyze_field_scan3.ps1 -Path fs.bin   # → TRANSFORM_ADDR / VELOCITY_ADDR（相对 owner 偏移）
# 1) 录制（自动锚定：比赛开始翻转即帧 0；duration 从翻转起算；最长等 5 分钟）
& $adb -s emulator-5554 shell "su -c 'nohup /data/local/tmp/a9tas_hwbp_dual $gpid $base record 40000 /data/local/tmp/rec.bin <TRANSFORM_ADDR> <VELOCITY_ADDR> > /data/local/tmp/rec.log 2>&1 &'"
# 用户开 30s；等待 HWBP_REC_DONE frames=...
# 2) 回放（自动锚定；用户新开比赛，手离开键盘）
& $adb -s emulator-5554 shell "su -c 'nohup /data/local/tmp/a9tas_hwbp_dual $gpid $base replay /data/local/tmp/rec.bin 0 <TRANSFORM_ADDR> <VELOCITY_ADDR> > /data/local/tmp/play.log 2>&1 &'"
# 并行轨迹采集（对比用）
& $adb -s emulator-5554 shell "su -c 'nohup /data/local/tmp/a9tas_traj_capture $gpid <TRANSFORM_ADDR> <VELOCITY_ADDR> 60000 /data/local/tmp/traj1.bin > /data/local/tmp/traj1.log 2>&1 &'"
# 3) 一致性：同一录制回放 3 次 → 对比
powershell -File tools\compare_traj.ps1 -Rec rec.bin -Traj traj1.bin
```

## 关键日志
- 录：`RACE_START` → `HWBP_REC_DONE frames=…`
- 放：`RACE_START (replay)` → `PLAY events=… served=…`（每 60 事件）→ `HWBP_PLAY_DONE`
- `SEQ MISMATCH` = 事件序列不连续（录制缺帧/异常）

## 安全与坑
- 全程零代码修改（无 Houdini 崩溃区）；游戏内钩子方案已废弃（3 次实机崩溃验证）。
- 实验后检查 `TracerPid=0`（ptrace detach 干净）。
- 每次游戏重启 ASLR 全变：重取 base/owner；对象地址（final_owner/controller）每次运行时重新解析。
- transform/velocity 偏移相对 final_owner（对象内偏移稳定，地址随对象变）。
- 氮气/Space 写值：安全规则禁止（未授权前不做覆盖）。
- 记录文件用 A9HDPV2（112B 帧）；旧 A9HDPV1（20B）不兼容。
