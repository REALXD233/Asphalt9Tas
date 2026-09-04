# A9 二进制格式注册表(2026-08-18)

> **重要声明:格式结构有效 ≠ TAS 回放成功,更 ≠ LIVE PASS。**
> 本注册表与 `tools/inspect_a9_artifact_v1.py` 只做**只读结构验证**。一个文件通过
> 结构检查只证明它符合该格式的 ABI/语义约束,绝不证明任何 replay 在游戏中成功,
> 也绝不构成任何实机（live）通过结论。所有 live 结论必须来自经过授权的实机证据。

配套工具：

- 检查器：`android-port/tools/inspect_a9_artifact_v1.py`（只读，绝不写入/转换/修复输入）
- 检查器测试：`android-port/tools/test_inspect_a9_artifact_v1.py`（全部使用合成 fixture）
- 旧注册表生成器（只读参考，不做结构判定）：`android-port/tools/build_binary_format_registry_v1.py`

## 支持状态说明

| 状态 | 含义 |
|---|---|
| `SUPPORTED` | 有权威严格 decoder，且本轮用合成 fixture 验证通过；检查器对这类文件执行完整结构检查 |
| `REQUIRES_EXPLICIT_FORMAT` | 有权威 validator，但**不允许**按 magic/长度自动识别，必须显式 `--format FC1 --pid <pid> --base <base>` |
| `KNOWN_UNSUPPORTED` | magic 已知，但本轮没有接入带合成 fixture 验证的严格 decoder；命中即 fail closed（退出码 2） |

## 支持的格式

| 格式 | Magic（8 字节） | Version | Header / Frame（字节） | 权威 decoder/validator | 状态 | companion / 参数 | tick 与有限浮点检查来源 |
|---|---|---|---|---|---|---|---|
| A9NPS1 | `A9NPS1\0\0` | 1 | 64 / 96 | `tools/native_physics_recording_v1.py`（MAGIC@26，decode_recording@113，validate_runtime_safe@174） | `SUPPORTED` | 无 | tick 连续 + monotonic + 有限浮点：`validate_runtime_safe`（native_physics_recording_v1.py:174） |
| A9UTK1 | `A9UTK1\0\0` | 1 | 96 / 144 | `tools/unified_tick_recording_v1.py`（MAGIC@16，decode_recording@178） | `SUPPORTED` | 无 | tick 连续 + monotonic + 有限/有界控制与物理：`decode_recording`（unified_tick_recording_v1.py:178） |
| A9USR1 | `A9USR1\0\0` | 1 | 240 / 212 | `tools/synchronized_tick_recording_v1.py`（MAGIC@27，decode_sync_report@105） | `SUPPORTED` | 可选显式 A9UTK1（`--companion`） | tick==index 连续 + monotonic + steering/物理有限：`decode_sync_report`（synchronized_tick_recording_v1.py:105） |
| A9USR2 | `A9USR2\0\0` | 2 | 240 / 212 | `tools/synchronized_brake_recording_v1.py`（MAGIC@32，decode_sync_brake_report@49） | `SUPPORTED` | 可选显式 A9UTK1（`--companion`） | 复用 A9USR1 结构 + C98/C9C brake pair 一致；已登记 flags profile `0x7f`（base）与 `0x27f`（bit 9 input-cycle anchor），其他组合 fail-closed |
| A9USR3 | `A9USR3\0\0` | 3 | 240 / 212 | `tools/synchronized_action_window_recording_v1.py`（MAGIC@21，decode_action_window_report@26） | `SUPPORTED` | 可选显式 A9UTK1（`--companion`） | 复用 A9USR2 结构验证：`decode_action_window_report`（synchronized_action_window_recording_v1.py:26） |
| A9USR4 | `A9USR4\0\0` | 4 | 240 / 212 | `tools/synchronized_action_until_release_recording_v1.py`（MAGIC@21，decode_action_until_release_report@53） | `SUPPORTED` | 可选显式 A9UTK1（`--companion`） | 复用 A9USR3 结构 + bounded maximum 检查：`decode_action_until_release_report`（synchronized_action_until_release_recording_v1.py:53） |
| A9UER1 | `A9UER1\0\0` | 1 | 296 / 1764 | `tools/parse_unified_executor_report_v1.py`（MAGIC@13，decode_report@69） | `SUPPORTED` | 无 | tick 连续 + 事件序 + 有限物理：`decode_report`（parse_unified_executor_report_v1.py:69） |
| A9UER2 | `A9UER2\0\0` | 2 | 296 / 1796 | `tools/parse_unified_executor_report_v2.py`（MAGIC@13，decode_report@70） | `SUPPORTED` | 无 | 同上：`decode_report`（parse_unified_executor_report_v2.py:70） |
| A9UER3 | `A9UER3\0\0` | 3 | 296 / 1804 | `tools/parse_unified_executor_report_v3.py`（MAGIC@19，decode_report@28） | `SUPPORTED` | 无 | 降级复用 A9UER2 + deferred clear 事件序：`decode_report`（parse_unified_executor_report_v3.py:28） |
| A9UER4 | `A9UER4\0\0` | 4 | 296 / 1804 | `tools/parse_unified_executor_report_v4.py`（MAGIC@18，decode_report@27） | `SUPPORTED` | 无 | 降级复用 A9UER3 + 独立 commit tid：`decode_report`（parse_unified_executor_report_v4.py:27） |
| A9UER5 | `A9UER5\0\0` | 5 | 296 / 1828 | `tools/parse_unified_executor_report_v5.py`（MAGIC@18，decode_report@27） | `SUPPORTED` | 无 | 降级复用 A9UER4 + steering write audit：`decode_report`（parse_unified_executor_report_v5.py:27） |
| A9UER6 | `A9UER6\0\0` | 6 | 296 / 1828 | `tools/parse_unified_executor_report_v6.py`（MAGIC@15，decode_report@21） | `SUPPORTED` | 无 | A9UER5 布局 + brake pair audit：`decode_report`（parse_unified_executor_report_v6.py:21） |
| A9UER7 | `A9UER7\0\0` | 7 | 320 / 1940 | `tools/parse_unified_nitro_observe_report_v7.py`（MAGIC@14，decode_report@35） | `SUPPORTED` | 无 | 单帧约束 + RPC counters + nitro response 协议：`decode_report`（parse_unified_nitro_observe_report_v7.py:35） |
| FC1 | `A9FC1R1\0`（仅作 validator 内部校验，**不用于自动识别**） | 1 | 336（整文件） | `tools/validate_fc1_report_v1.py`（REPORT_SIZE@11，validate@28） | `REQUIRES_EXPLICIT_FORMAT` | 必须 `--format FC1 --pid <pid> --base <base>` | 无 tick/浮点概念；校验 size/flags/phase/identity/threads/边界/counters/evidence（validate@28） |

说明：

- A9USR1–A9USR4 的完整「报告 + A9UTK1 录制」交叉绑定验证**可选**：用户显式传入
  `--companion <path>` 时，检查器在主文件结构校验通过后调用对应的权威 verifier
  （`verify_synchronized_capture` / `verify_synchronized_brake_capture` /
  `verify_action_window_capture` / `verify_action_until_release_capture`），
  输出 `validation_scope=CAPTURE_CROSS_BOUND`、`capture_validated=true`；
- 不带 companion 时，检查器只做报告结构验证，输出 `validation_scope=STRUCTURE_ONLY`、
  `companion_status=NOT_PROVIDED`、`capture_validated=false`，不会声称「capture 完整有效」；
- companion 文件**绝不会自动发现**：不扫描相邻目录、不按文件名猜测，只有显式 `--companion`
  路径才会被读取；companion 仅适用于 A9USR1–A9USR4 主文件，其它主文件带 `--companion`
  一律作为 CLI 使用错误（退出码 3）；
- A9UER3–A9UER7 的 decoder 通过「降级复用低版本 decoder」实现严格验证，检查器直接调用
  对应版本的 `decode_report`，与权威实现完全一致。
- A9UER1 的 header/frame 布局与 A9UER2 相同尺寸族，唯一差异为每帧事件数（5 vs 6）。

## KNOWN_UNSUPPORTED（已知 magic，本轮不承诺支持）

以下 magic 在注册表中可见或证据中可观测，但本轮**没有**接入带合成 fixture 验证的严格
decoder。命中时检查器 fail closed：`status=UNSUPPORTED`，退出码 2。
Version 列来自 `FormatSpec.version`；无版本信息的条目填 `na`。

| 格式 | Magic | Version | 潜在 decoder（未接入） | 未支持原因 |
|---|---|---|---|---|
| A9CDT1 | `A9CDT1\0\0` | 1 | `tools/parse_conditional_audit_v1.py` | 本轮未接入/未验证 fixture |
| A9BAV1 | `A9BAV1\0\0` | 1 | `tools/parse_hwbp_barrel_angular_v1.py` | 同上 |
| A9ESA1 | `A9ESA1\0\0` | 1 | `tools/parse_hwbp_executor_stack_affinity_v1.py` | 同上 |
| A9PIP1 | `A9PIP1\0\0` | 1 | `tools/parse_hwbp_pipeline_order_v1.py` | 同上 |
| A9WBP1 | `A9WBP1\0\0` | 1 | `tools/parse_hwbp_worker_boundary_v1.py` | 同上 |
| A9WSS1 | `A9WSS1\0\0` | 1 | `tools/parse_hwbp_worker_stack_scope_v1.py` | 同上 |
| A9ZGB1 | `A9ZGB1\0\0` | 1 | `tools/parse_hwbp_zero_gap_v1.py` | 同上 |
| A9NTA1 | `A9NTA1\0\0` | 1 | `tools/parse_nitro_thread_affinity_report_v1.py` | 同上 |
| A9PEA1 | `A9PEA1\0\0` | 1 | `tools/parse_physics_executor_affinity_v1.py` | 同上 |
| A9SBT1 | `A9SBT1\0\0` | 1 | `tools/parse_same_bytes_audit_v1.py` | 同上 |
| A9PST1 | `A9PST1\0\0` | na | `tools/parse_vehicle_state_trace_v1.py` | 同上 |
| A9NPA1 | `A9NPA1\0\0` | 1 | unknown（anchor 解析器本轮未接线） | 无可靠 decoder + fixture |
| A9SPR1 | `A9SPR1\0\0` | 1 | unknown（legacy probe 格式） | 无可靠 decoder + fixture |
| A9NRS1 | `A9NRS1\0\0` | 1 | 内嵌 nitro 响应（A9UER7 payload 内） | 不是独立文件格式 |

## 拒绝规则（fail closed）

- 未知 8-byte magic → `UNSUPPORTED`（退出码 2），不猜测格式；
- 已知 magic 但 version 不符 → `UNSUPPORTED`（退出码 2），不调用旧版本 decoder；
- 只改扩展名、内容 magic 不变 → 按内容识别，不受扩展名影响；
- 截断、frame count 与长度不符、尾随多余字节 → decoder 抛错 → `INVALID`（退出码 1）；
- tick 逆序/断裂（格式要求连续时）、NaN/Inf 载荷 → `INVALID`（退出码 1）；
- FC-1 未提供 `--pid/--base` → `UNSUPPORTED`（退出码 2）；FC-1 绝不按 336 字节长度自动判定；
- 任意 parser/validator 抛出的校验错误 → `INVALID`（退出码 1）；
- 文件不存在/不可读/CLI 用法错误 → `IO_ERROR`（退出码 3）。

### companion 边界错误语义（P0）

- 主文件先于 companion 校验：先识别主文件 magic/version 并完成主文件结构校验，
  只有合法 A9USR1–A9USR4 主文件通过后才会读取显式 `--companion`；
- A9USR1–4 已识别但 version 不支持或主结构失败，且用户提供了 companion →
  主状态与退出码不变（`UNSUPPORTED`/2 或 `INVALID`/1），
  `companion_status=NOT_EVALUATED`、`companion_format=A9UTK1`、`capture_validated=false`，
  companion 文件不被读取；
- 未知 magic、`KNOWN_UNSUPPORTED` 或任何非 A9USR1–4 主文件带 `--companion` →
  `USAGE_ERROR`（退出码 3），companion 文件不被读取；
- 合法 A9USR1–4 主文件 + companion 路径不可读 → `IO_ERROR`（退出码 3），
  主文件 format/version/frames/ticks 字段保留，`companion_status=IO_ERROR`；
- 合法 A9USR1–4 主文件 + companion 内容非法/交叉绑定不符 → `INVALID`（退出码 1），
  `companion_status=INVALID`；
- `--list-formats` 是互斥模式：与 path/`--format`/`--pid`/`--base`/`--companion`/`--json`
  任一组合均退出 3，不静默忽略参数。

## 只读保证

检查器对输入文件只执行 `read_bytes()` 与解码；不打开写句柄、不创建 sidecar、
不做任何转换/修复/重写。测试断言每个 fixture 检查前后 SHA-256 完全一致。
`--json` 仅输出到 stdout。
