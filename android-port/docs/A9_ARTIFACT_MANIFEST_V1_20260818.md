# A9 工件 manifest v1(2026-08-18)

> **重要**:manifest 批量检查**不是目录扫描器,也不是 companion 自动发现器**。
> 只有用户在 manifest 中显式写出的路径才会被读取;相对路径以 manifest 所在目录为基准。
> 所有检查都是只读的,不会修改、转换或重写任何输入文件。

## 1. 用途

`tools/inspect_a9_artifact_manifest_v1.py` 让用户显式列出多组「主文件 / companion」或
FC-1 报告,依次调用 `tools/inspect_a9_artifact_v1.py` 的 `inspect_bytes()`(进程内,
不启动子进程)汇总全部结果。适合一次校验一批录像/报告,或在 CI 中固定清单。

## 2. Schema

顶层 JSON 对象只允许两个键:`schema` 与 `entries`。

```json
{
  "schema": "A9_ARTIFACT_MANIFEST_V1",
  "entries": [
    {
      "id": "source-capture",
      "path": "capture.a9usr4",
      "companion": "capture.a9utk1"
    },
    {
      "id": "fc1-report",
      "path": "fc1.bin",
      "format": "FC1",
      "pid": 1234,
      "base": "0x70000000"
    }
  ]
}
```

约束(任何一条不满足都 fail closed,整体退出码 3):

| 字段 | 规则 |
|---|---|
| `schema` | 必须严格等于 `A9_ARTIFACT_MANIFEST_V1` |
| `entries` | 数组,数量 1–256 |
| `id` | 必填、唯一,长度 1–64,仅 `[A-Za-z0-9_.-]` |
| `path` | 必填且为非空、非纯空白字符串;含 NUL 字符 fail closed;含 `*`、`?`、`[`、`]` 等 glob 形状 fail closed |
| `companion` | 可选;字段**出现**时必须为非空、非纯空白字符串(空字符串 `""` 是 schema 错误);含 NUL 或 glob 形状 fail closed;与 `format=FC1` 互斥 |
| `format`/`pid`/`base` | 仅 FC-1 条目允许;`pid` 必须为严格整数(拒绝 boolean)且 `1 <= pid <= 0xFFFFFFFF`;`base` 必须为严格整数(拒绝 boolean)或 `"0x..."` 字符串,解析后 `1 <= base <= 0xFFFFFFFFFFFFFFFF` |
| 未知键 | 顶层或条目内出现任何未知键都 fail closed |
| 类型错误/空路径/重复 id | fail closed |

路径规则:

- 相对路径以 manifest 文件所在目录为基准解析;
- 绝对路径仅在用户明确写入 manifest 时使用;
- 禁止 glob、目录递归、相邻文件扫描和文件名猜测。

## 3. 执行语义

1. 读取并校验 manifest 本身(schema/类型/约束);
2. 逐项处理:先读取主文件并完成主文件识别与结构校验;
3. 只有「合格且结构有效」的 A9USR1–A9USR4 主文件,且条目显式提供了 `companion` 时,
   才读取 companion 并调用对应权威 verifier 做交叉绑定;
4. 非 A9USR 主文件带 `companion` → 该项 `USAGE_ERROR`(companion 不被读取);
5. 主文件**已识别为 A9USR1–A9USR4** 但自身失败(版本/结构)→ 保留主文件结果,
   `companion_status=NOT_EVALUATED`(companion 不被读取);
   主文件**不是** A9USR1–A9USR4(含未知 magic、KNOWN_UNSUPPORTED、其它支持格式)时带
   `companion` → `USAGE_ERROR`,不是 `NOT_EVALUATED`;
6. 一项失败不停止后续条目,最终汇总全部结果;
7. 每项记录主文件 SHA-256,有 companion 时记录 companion SHA-256;
   哈希仅用于身份记录,不代表文件可信;文本模式显示完整 64 位哈希,未读取时显示 `na`。

## 4. 输出

文本模式(默认)汇总行:

```text
A9_MANIFEST_SUMMARY manifest_sha256=<64hex> entries=N valid=N invalid=N unsupported=N io_error=N usage_error=N read_only=1 device_access=0 auto_discovery=0
```

每个条目一行(`entry id=... path=... format=... status=... validation_scope=... companion_status=... capture_validated=... main_sha256=... companion_sha256=...`),
错误详情以 `error:` 前缀行输出。

`--json` 输出稳定 JSON:`schema`、`manifest_sha256`、`entries`(含每项完整检查报告与哈希)、
`summary`(total/valid/invalid/unsupported/io_error/usage_error/exit_code)、
`read_only: true`、`device_access: 0`、`auto_discovery: false`。

## 5. 退出码

按最高严重度汇总:

- 全部有效 → 0;
- 任一项 INVALID(结构/交叉绑定失败)→ 1;
- 任一项 UNSUPPORTED(未知 magic/版本)→ 2;
- 任一项 IO_ERROR 或 USAGE_ERROR(含 manifest schema 错误)→ 3。

## 6. 只读保证

工具不提供输出文件参数,不写任何报告;输入文件只执行 `read_bytes()`。
测试断言 manifest、主文件、companion 在运行前后 SHA-256 完全一致。
所有输出只到 stdout/stderr。
