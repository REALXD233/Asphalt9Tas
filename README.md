# Asphalt9Tas

Asphalt9Tas 是面向 Android 版《狂野飙车 9》的研究型 TAS 工具。它在游戏自己的 Tick 边界记录和回放输入、固定时间步与必要的车辆物理状态，并提供完整圈回放、指定 Tick 检查、分段续录、录像管理和悬浮窗刷圈流程。

本项目独立开发，与 Driver555a9 或游戏开发商、发行商不存在隶属或授权关系。实现思路参考了 AluTasV2 的事件驱动回放语义，但 Android 注入、生命周期管理、Build Profile 与 APK 工作流均为独立实现。

## 当前能力

### 2026-09-10 源码更新

- 加入 120/144 Hz 录制时间步选项及 24000 Tick 容量；录像回放沿用录像自身时间步。
- 改进 Android Root 扫描、共享录像导入与运行状态诊断。
- 补充碰撞网格、材质区域、小地图节点的只读分析工具；这些工具仍属于研究性功能，不能把显示节点直接等同于已验证的检查点/重置触发区。
- 最新修订保留首 Tick 故障的回调来源和底层错误码。Android 7 ARM64 上报告的 `error=13` 尚待实机复现确认，不能视为已修复。

核心入口：`android-port/src/payload_g4_multi_hook_runtime_v1.cpp`（Hook 与执行）、`g4_g3_adapter_v1.h` / `g3_tick_coordinator_v1.h`（Tick 协调）、`g4_input_action_core_v1.h`（输入与状态）、`barrel_prng_v1.h`（确定性随机数）。APK 操作流程见 `android-port/A9TasAndroid/app/src/main/java/dev/a9tas/android/SessionOrchestrator.java`。

本仓库不附带游戏二进制、设备内存转储、私人诊断包、签名或卡密签发私钥。部分历史构建/分析脚本依赖本地工具链及未公开的测试制品，并非所有脚本都能在干净克隆后直接运行。

- ARM64 原生 Android 7+ 与 x86_64/ARM NativeBridge 运行路径
- 自动扫描游戏进程、Build Profile 选择与未知 ARM64 核心签名定位
- 逐 Tick 录制与确定性回放
- 转向、刹车/漂移、氮气、车辆最终物理状态、滚筒与空中 Yaw
- 完整圈、目标 Tick 暂停检查、前缀加载及分段续录
- 同进程 Retry/连续刷圈、悬浮窗快捷控制
- 录像重命名、删除、导入、导出及诊断包导出
- 练习模式限制与限时授权机制

## 已验证范围

- 雷电 9：x86_64 主机与 ARM 游戏运行路径
- ARM64 Android 7 rooted 环境
- 九游/阿里与华为渠道构建；未知 ARM64 构建支持自动生成 Profile，但仍应先在非重要进程中验证

“已验证”不等于适配全部设备。Root 实现、SELinux、NativeBridge、游戏核心版本和渠道 SDK 都可能改变注入条件。

## 构建

主构建入口：

```powershell
./android-port/build-g10-android-product-v1.ps1
```

构建需要 Windows、PowerShell、Python、Android SDK 35、JDK 17 和 Android NDK r27d。默认脚本从 `android-port/.toolchains` 查找便携工具链；该目录不进入版本控制。

APK 内使用的 native runtime 属于构建产物，不进入 Git 历史；配置模板保存在 `android-port/config/runtime-manifest-template.json`，构建时生成清单并仅打包清单引用的文件。

原生构建还需要外部的参考游戏 ELF、雷电 libc 和历史实测基线文件，克隆仓库不会自动获得这些输入。准备方式和离线测试见 [源码构建说明](android-port/SOURCE_BUILD.md)。此前说明未列出这些依赖，现已更正。

Release 签名必须使用源码目录之外的 keystore，并通过环境变量提供密码：

```powershell
$env:A9TAS_RELEASE_STORE_PASSWORD = '<store password>'
$env:A9TAS_RELEASE_KEY_PASSWORD = '<key password>'
./android-port/build-g10-android-product-v1.ps1 `
  -SigningMode Release `
  -ReleaseKeystorePath 'D:\secure\asphalt9tas-release.jks' `
  -ReleaseKeyAlias 'asphalt9tas'
```

不要提交 keystore、私钥、设备诊断包或游戏二进制文件。

## 使用边界

本工具仅用于离线练习、可重复性研究和兼容性测试。不得用于多人、排行榜、活动或任何影响其他玩家与游戏平衡的场景。使用者自行承担 Root、注入、账号和数据损坏风险。

## 版本

首个仓库版本对应已通过雷电实测的 `0.8.0-profile-autogen` 构建，并包含 2026-09-04 的精确 replay-to-record 原子交接修复。
