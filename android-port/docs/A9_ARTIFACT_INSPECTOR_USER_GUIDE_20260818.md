# A9 工件检查器用户指南(2026-08-18)

这份指南面向非技术用户,说明如何用只读工具检查游戏录像/报告文件。
**所有检查都是只读的,不会修改任何录像或报告。**

> **重要提醒**:离线结构检查或交叉绑定通过,**不等于实机回放成功**,也不等于完整比赛确定性。
> 离线检查只证明文件本身的结构与绑定关系正确;是否真的在游戏中回放成功,需要另外的实机证据。

## 1. 检查单个文件

```text
python android-port/tools/inspect_a9_artifact_v1.py <文件路径>
```

例如:

```text
python android-port/tools/inspect_a9_artifact_v1.py capture.a9utk1
```

成功时输出类似:

```text
A9_ARTIFACT_VALID format=A9UTK1 version=1 size=240 frames=1 first_tick=0 last_tick=0 validation_scope=STRUCTURE_ONLY companion_format=na companion_status=NOT_APPLICABLE capture_validated=false read_only=1 device_access=0
```

输出中的 `validation_scope`、`companion_format`、`companion_status`、`capture_validated`
是所有检查都会携带的稳定字段;对 A9UTK1 等不涉及 companion 的格式,它们分别显示
`STRUCTURE_ONLY`、`na`、`NOT_APPLICABLE`、`false`。

## 2. 检查录像 + 配对文件(companion)

A9USR1–A9USR4 报告可以连同对应的 A9UTK1 录像一起检查,验证两者是否真正相互绑定:

```text
python android-port/tools/inspect_a9_artifact_v1.py capture.a9usr4 --companion capture.a9utk1
```

- 不提供 `--companion` 时,只检查报告本身结构,输出 `validation_scope=STRUCTURE_ONLY`,
  `companion_status=NOT_PROVIDED`,`capture_validated=false`;
- 提供正确的 `--companion` 时,输出 `validation_scope=CAPTURE_CROSS_BOUND`,
  `companion_status=VALID`,`capture_validated=true`;
- **companion 不会自动寻找**:必须显式给出路径,工具不会扫描相邻目录或按文件名猜测;
- `--companion` 只适用于 A9USR1–A9USR4 主文件;其它文件带这个参数会报用法错误。

## 3. 批量检查(manifest)

把多组文件显式写进一个 JSON 清单,一次检查:

```text
python android-port/tools/inspect_a9_artifact_manifest_v1.py manifest.json
```

清单格式见 `docs/A9_ARTIFACT_MANIFEST_V1_20260818.md`。批量检查同样只读,
每一项的结果都会列出,一项失败不会中断其它项。

## 3a. 查看支持格式清单

列出全部已知格式(文本):

```text
python android-port/tools/inspect_a9_artifact_v1.py --list-formats
```

机器可读格式清单(JSON):

```text
python android-port/tools/inspect_a9_artifact_v1.py --list-formats --json
```

JSON 输出使用固定 schema `A9_FORMAT_LIST_V1`,共 28 个格式条目,每项包含名称、支持级别、
magic(转义 ASCII 与 hex 两种无损表示)、版本、权威 decoder、是否需要 pid/base、
检查清单以及 A9USR1–A9USR4 的 companion 信息。`--list-formats` 只能单独使用,
不能与文件路径或其它选项组合。

## 4. 退出码含义

| 退出码 | 含义 | 常见原因 |
|---|---|---|
| 0 | 已支持格式,且所有检查通过 | 文件正常 |
| 1 | 文件损坏或校验失败 | 文件被截断、追加了多余字节、版本字段损坏、companion 与报告不一致 |
| 2 | 无法识别的格式或版本不受支持 | 未知文件类型、旧版本、扩展名被改动 |
| 3 | 文件不存在/不可读,或命令用法错误 | 路径写错、FC-1 缺少参数、companion 用于不支持的格式 |

## 5. 常见错误与处理

- **路径不存在**:检查路径是否写对;工具会提示 `cannot read file`。
- **版本不对(wrong version)**:文件来自其它版本;重新获取正确版本的文件。
- **companion 不匹配**:报告与录像不是同一批产物;确认使用的是同一次录制生成的文件。
- **FC-1 缺 pid/base**:FC-1 报告必须显式给出进程标识与基址:

  ```text
  python android-port/tools/inspect_a9_artifact_v1.py report.bin --format FC1 --pid 1234 --base 0x70000000
  ```

- **不确定原因**:保留原文件和完整输出,把两者一起交给技术人员,不要反复重试。

## 6. 常见问题

- **工具会不会修改我的文件?** 不会。所有工具只读,不提供任何修改、转换或修复功能。
- **工具会帮我自动找 companion 吗?** 不会。必须显式提供路径。
- **检查通过就代表回放成功吗?** 不代表。结构/绑定检查通过只说明文件本身正确。
- **检查通过代表完整比赛确定性吗?** 不代表。一次检查只覆盖被检查文件的范围。
