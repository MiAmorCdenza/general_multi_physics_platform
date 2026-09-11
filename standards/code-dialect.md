# 代码方言（注释、日志与输出语言）

> 核心原则：**注释与调试输出是英文，doc 是人话。**
>
> 理由不是"崇洋"，而是可检验性：一条中文日志在 GBK 控制台下读回来是乱码，
> 正则匹配失败，门禁会**误报通过**。这个坑本项目已经踩过一次（见 §5）。

---

## 1. 三条规则

| 规则 | 约束对象 | 判据 | 例外 |
|---|---|---|---|
| **D1** | 全仓库所有文本文件 | 不得出现 emoji / 图形符号（象形字符） | `views/` 下的 UI 资源与本地化字符串 |
| **D2** | 源码注释（`.hpp/.h/.cpp/.cc/.txt/.cmake/.py/.ps1/.bat`） | 注释正文必须是 ASCII 英文 | 字符串字面量内的本地化文本 |
| **D3** | 源码注释 | 不得用箭头 / 制表符画装饰（用 `->`、`+--`） | 文档（`.md`）里的图 |

D1 按 Unicode 区段判定，不按"看起来像不像"：

```
U+2300–U+23FF  杂项技术符号     U+2460–U+24FF  带圈字母数字
U+2700–U+27BF  装饰符号         U+2B00–U+2BFF  杂项符号与箭头
U+FE0F         变体选择符-16（emoji 呈现）
U+1F000–U+1FAFF 表情 / 象形 / 交通 / 补充符号
```

区段判定而不是"白名单字符"，是因为前者可写成一个正则并在门禁里被执行；
后者要求每个新字符都来改一次规则，实际结果是规则被绕过。

### 1.1 为什么箭头不属于 D1

D1 删的是**象形**字符：`[OK]` `[X]` `⚠` 与表情符号不携带技术信息，只在渲染上
制造差异。箭头（U+2190–U+21FF）与制表符（U+2500–U+257F）不是象形——它们表达
结构（数据流向、框住的警告），只是用了非 ASCII 画法。

所以拆成两条：

- 文档（`.md`）里保留箭头与制表符：那里的图是给人读的；
- **源码注释里禁止**（D3）：注释会被终端、CI 日志、编辑器、`git diff` 反复搬运，
  非 ASCII 画法在这些场合都可能变成 `?` 或乱码，而 ASCII 画法永远不会。

```text
禁止：  in ──→ out        // ── 是 U+2500，→ 是 U+2192
允许：  in --> out        // 全是 ASCII
```

同理，几何图形（U+25A0–U+25FF）不在 D1 里：`▲` `▼` 是设计注释表达单调性的
正常写法。但**圆形数字**（U+2460–U+24FF，即 `(1)(2)(3)`）在 D1 里——它只是装饰性
编号，直接用 `(1)` `(2)` 即可。


## 2. 为什么 D2 是"注释必须英文"而不是"注释最好英文"

注释不是给人看的装饰，它是**契约的一部分**：`standards/function-contract.md`
里的 `@ownership` / `@thread` / `@pre` / `@post` / `@tests` 全部写在注释里，
`scripts/check_contracts.py` 逐字段解析它们。

混用语言会立刻产生两个可观测的问题：

1. **宽度不可预测。** 一个中文字符在不同编辑器占 1 或 2 列，同一段注释在两个
   人屏幕上换行位置不同，行内对齐（本项目的 `@tests` 多行缩进对齐）会崩。
2. **搜索不可靠。** 想找"所有标了 `@frozen 是` 的接口"，得同时搜 `是` 与 `yes`。

因此定成机械规则：**注释正文 ASCII 英文，标识符与字段名不变。**

## 3. 文档（`.md`）不受 D2 与 D3 约束

`docs/` 与 `standards/` 的说明性散文按中文书写——那是给人读的规格，不是编译器
解析的对象。门禁据此把 `.md/.json/.yml/.yaml` 从 D2 与 D3 中排除
（`DOC_SUFFIXES`），但它们**仍然受 D1 约束**：流程图上贴 emoji 一样会被抓。

## 4. 日志：统一 JSON Lines

### 4.1 格式选择

一条日志要同时服务三种消费者：终端前的人、按级别分组的脚本、以及"拿到日志就要
能复现"的缺陷报告。自由文本只服务第一种。

**JSON Lines**（一行一个完整 JSON 对象）是唯一同时满足"可流式追加"和"可解析"的
形状。用 JSON 数组不行：文件被截断时整个数组都不可解析，而 JSON Lines 只损失
最后一行。

```text
{"ts":"2026-09-11T22:13:45.123Z","level":"warning","code":"dimension_mismatch",
 "domain":"typing","source":"units","consequence":"recoverable","msg":"expected m/s, got m/s^2"}
```

### 4.2 字段契约（冻结）

| 字段 | 类型 | 含义 | 可否改名 |
|---|---|---|---|
| `ts` | string | ISO 8601 UTC，毫秒，定宽 | 否 |
| `level` | string | `info` / `warning` / `error` | 否 |
| `code` | string | 稳定标识，可 grep、可告警 | 否 |
| `domain` | string | 错误来自哪个子系统 | 否 |
| `source` | string | 哪个插件 / 模块发出的 | 否 |
| `consequence` | string | 对本次运行的影响程度 | 否 |
| `msg` | string | 人类可读细节 | 措辞可变 |

**判定一律看 `code`，永远不要对 `msg` 做告警或断言。** `msg` 是给人读的，
随时可能被改写；`code` 取值来自 `qp::diag::ErrorCode`，数值与短名都冻结
（`diag.error_code_values_are_stable`）。

### 4.3 时间戳

形状固定为 `YYYY-MM-DDTHH:MM:SS.mmmZ`，24 字符，三点刻意设计：

- **只用 UTC。** 本地时间日志无法跨机器关联，且夏令时切换处顺序会倒退。
- **定宽零填充。** 因此文本排序等于时间排序——日志文件可以直接当纯文本排序
  （`diag.log.iso8601_is_lexically_sortable` 就是这个性质）。
- **毫秒是上限。** 当前 32 位工具链的 `time_t` 只有秒级分辨率，再给三位小数
  就是伪造精度。

### 4.4 级别怎么定

级别是**呈现**分类（决定进哪个流、显示多大声），不是控制信号——宿主该降级还是
中止看 `Consequence`。级别由错误域导出，避免每个调用点自己发明一套：

| 错误域 | 级别 | 道理 |
|---|---|---|
| `input` / `typing` | `info` | 用户改一下参数就好，这不是事故 |
| `graph` / `plugin` / `runtime` | `warning` | 本次运行退化或中止 |
| `internal` | `error` | 进程状态已不可信 |

### 4.5 转义

`msg` 与 `source` 经 `qp::diag::json_escape` 处理，规则（RFC 8259）：

- `"` 与 `\` 必须转义，否则整个文件不可解析；
- 有短形式的控制字符用短形式（`\n \r \t \b \f`），其余用 `\u00XX`；
- **非法 UTF-8 字节替换为 `?`**。放行非法字节会让整个日志文件无法被任何 JSON
  解析器接受——损失一个字符和损失整份日志，选前者。

## 5. D3 的由来（真实故障）

死亡测试用 CTest 的正则断言"进程确实死于契约违反，而不是别的崩溃"。原始消息是
中文。在 MSVC 上，`cl` 以 UTF-8 写出，而控制台按 GBK 读回，正则匹配失败——
**门禁把一次真实通过报成了失败**。

修法不是"换个匹配串"，而是让消息本身 ASCII：

```cpp
#define QP_PRECONDITION(cond)                                              \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fputs("[qp] contract violation (@pre does not hold)\n",   \
                       stderr);                                            \
            std::abort();                                                  \
        }                                                                  \
    } while (false)
```

推论：**任何会被机器读的字符串都必须是 ASCII。** 需要给人看的中文放在 GUI 层，
那里有 `QString` 与本地化链路，且不参与门禁匹配。

同一条推论也是 D3 的来历：注释不参与门禁匹配，但会出现在终端、CI 日志、
`git log -p`、代码评审页面里——每一步都可能被按 GBK 解码。ASCII 画法在所有这些
环节都不会坏，非 ASCII 画法只是"在我的编辑器里看起来还行"。

## 6. 门禁

`scripts/check_dialect.py` 实现 D1–D3，注册为 CTest 的 `gate.dialect`。

```
python scripts/check_dialect.py                    # 全仓库
python scripts/check_dialect.py --quiet            # 只报违规
python scripts/check_dialect.py --by-file          # 按文件汇总（看进度用）
python scripts/check_dialect.py --by-file --rules D1,D3
```

排除目录：`.git` `build` `build-msvc` `external` `_deps` `__pycache__` `.vs`
`.venv` `venv` `site-packages` `node_modules`。

配套脚本：

| 脚本 | 用途 |
|---|---|
| `scripts/plan_translation.py` | 把 D2 待翻译清单切成均衡批次并落 JSON |
| `scripts/show_translation_plan.py` | 打印批次清单 |
| `scripts/show_emoji_chars.py` | 列出命中的 D1 字符及频次 |
| `scripts/show_nonascii_stats.py` | 按 Unicode 区段统计非 ASCII 字符，用于规划批量替换 |

按 `enforcement.md` §8，门禁自身也要有测试：`tests/meta/check_dialect_gate.py`
用最小夹具同时验证**能抓违规**（负例）与**不误报**（正例）。
只测负例的门禁挡不住"把一切都判违规"这种实现。

### 6.1 分段启用

门禁的基线清到 0 之前，`gate.contracts` 那套"红了就停"的做法会把整个构建卡死。
处理方式是分开注册：

- `meta.dialect_checker`：门禁自身的自检，**任何时刻都必须绿**；
- `gate.dialect`：全仓库扫描。基线清零后已作为强制门禁打开。

一个永远红的门禁会被忽略；而"门禁不存在"和"门禁红着"是两种不同的失败，
所以先让自检常驻，再让全量扫描上岗。

### 6.2 例外必须写在行内

门禁自己也要被测试，而测试门禁的文件必须**含有**门禁要拒绝的字符。这种自指
无法用"把夹具挪到扫描不到的地方"解决——那会让夹具既难读又容易写错。因此提供
一个行内标记：

```python
f"// {CJK}注释\n",  # qp-dialect-allow: D2 fixture
```

标记必须落在**出错的那一行**，不能写在上方另起一行：行作用域让例外无法被顺手
扩大。全仓库目前只有两处，都在 `tests/meta/` 下。

### 6.3 扫描范围

门禁跳过这些目录，理由都是"内容不属于交付物"：

| 跳过 | 理由 |
|---|---|
| `.git` `build` `build-msvc` `build-*` | 构建产物 |
| `external` `_deps` | 第三方依赖，由 `scripts/fetch_deps.ps1` 拉取 |
| `.venv` `venv` `site-packages` `node_modules` | 供应商代码（本检出可能位于含这些兄弟目录的共享目录下） |
| `tmp_*` `.tmp` | 临时脚手架。`tmp_` 是刻意约定，不是"忘了删" |
| `__pycache__` `.vs` | 工具缓存 |

排除目录本身也有测试（`tests/meta/check_dialect_gate.py` 的 `collect()` 用例）。
否则"排除了不该排除的目录"会让门禁静默失效——那比门禁红着更难发现。


