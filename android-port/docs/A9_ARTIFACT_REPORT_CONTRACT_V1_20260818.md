# A9 artifact report contract v1(2026-08-18)

> 本文档定义 `inspect_a9_artifact_v1.py` 的 JSON report 与
> `inspect_a9_artifact_manifest_v1.py` 逐项报告的**字段类型与跨字段不变量**。
> 契约验证器 `tools/validate_a9_artifact_report_contract_v1.py` 只检查 report JSON
> 的结构与关系,不重新解析任何二进制文件。

## 1. 核心必需字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `status` | str | `VALID` / `INVALID` / `UNSUPPORTED` / `IO_ERROR` / `USAGE_ERROR` |
| `format` | str 或 null | 主文件格式名 |
| `version` | int 或 null | 主文件版本 |
| `size` | int 或 null | 主文件字节数 |
| `frames` | int 或 null | 帧数(不适用时为 null) |
| `first_tick` / `last_tick` | int 或 null | 首/末 tick |
| `checks` | str 列表 | 已执行的检查名;非 VALID 时为空 |
| `validation_scope` | str | `STRUCTURE_ONLY` / `CAPTURE_CROSS_BOUND` |
| `companion_status` | str | `NOT_APPLICABLE` / `NOT_PROVIDED` / `NOT_EVALUATED` / `IO_ERROR` / `INVALID` / `VALID` |
| `companion_format` | str 或 null | companion 格式(当前固定 `A9UTK1`) |
| `capture_validated` | bool | 是否完成完整交叉绑定验证 |
| `read_only` | bool | 必须为 `true` |
| `device_access` | int | 必须为 `0` |
| `error` | str 或 null | 非 VALID 时为非空错误说明;VALID 时为 null |

manifest 工具可以在核心字段之外增加外层字段(`id`、`path`、`companion_path`、
`main_sha256`、`companion_sha256` 等),但核心字段的类型与关系必须仍然有效。

## 2. 跨字段不变量

- `capture_validated=true` 必须同时满足:
  - `status=VALID`;
  - `format` 属于 A9USR1–A9USR4;
  - `validation_scope=CAPTURE_CROSS_BOUND`;
  - `companion_status=VALID`;
  - `companion_format=A9UTK1`;
  - `checks` 包含 `companion_cross_bind`;
- `validation_scope=STRUCTURE_ONLY` 时 `capture_validated=false`,且 `checks` 不含
  `companion_cross_bind`;
- `status=VALID` 时 `error=null`;非 VALID 时 `error` 必须为非空字符串;
- 非 VALID 时 `checks` 必须为空;
- `version`/`size`/`frames`/`first_tick`/`last_tick` 必须是严格 int 或 null
  (JSON boolean 不是 int);
- 可选的 `main_sha256`/`companion_sha256` 只能是 null 或 64 位 hex(大小写均可)。

## 3. 不承诺的字段

本契约不发明、也不要求任何“实机成功”“清理成功”“游戏世界状态回滚”字段。
离线结构/交叉绑定通过不代表实机回放成功。

## 4. CLI

```text
python android-port/tools/validate_a9_artifact_report_contract_v1.py report.json
python android-port/tools/validate_a9_artifact_report_contract_v1.py report.json --json
```

- 契约有效:退出 0;
- JSON 可读但契约无效:退出 1;
- 文件不可读 / JSON 无法解析 / CLI 用法错误:退出 3;
- 只读,不写报告;输出包含 `read_only=1 device_access=0`。

文本模式:

```text
A9_REPORT_CONTRACT_VALID read_only=1 device_access=0
A9_REPORT_CONTRACT_INVALID error=<首个违规> read_only=1 device_access=0
```

`--json` 输出:

```json
{
  "valid": true,
  "errors": [],
  "read_only": true,
  "device_access": 0
}
```
