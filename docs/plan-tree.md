# qp 开发计划树

> 配套文件：`00-charter.md`（法）· `standards/function-contract.md`（工艺）·
> `standards/test-taxonomy.md`（验证）· `standards/enforcement.md`（门禁）· `plan-status.json`（进度）
>
> **本文件是"图"：从顶层架构逐级下推到模块。** 下推规则见 §7。

---

## 1. 四层总览

```
┌─ L3  core/authoring/  ── 视图服务：文档、命令总线、布局、端口 UI
├─ L2  core/runtime/    ── 运行身份、记录、数据集、导出
├─ L1  core/graph/      ── IR、图结构、求值、缓存、调度、内核契约
└─ L0  core/            ── 契约与机制：量纲、ABI、端口、插件、诊断（地基）
     ─────────────────────────────────────────────────────────────
        views/       ⊂ L3 消费者：Qt 界面（节点编辑器 / 表格 / 仪器面板）
        plugins/     ⊄ 任意层：内容（模型、仪器、内核实现、视图项、实验）
        tests/ bench/     贯穿全部层
```

**核心断言：`views/` 与 `plugins/` 可以整个删掉，`core/` 仍能编译并通过全部测试。**
这是 CI 必须验证的一条（见 `enforcement.md` §6）。

---

## 2. L0 —— 地基（`core/`）

L0 是本项目唯一不可插件化的部分，也是接口面必须最小的部分。

### 2.1 `core/units/` —— 量纲与量

| 项 | 内容 |
|---|---|
| 职责 | 编译期量纲推导；`unit` 字符串从类型自动生成 |
| 关键类型 | `Dim`, `Quantity<D>`, `Unit`, `Dimensionless` |
| 冻结 | `Dim` 的**底层表示**冻结（7 个指数）；API 可扩 |
| 依赖 | 无（最底层） |
| 为什么在地基 | 跨插件语义一致性的前提；所有 YAML/脚本都依赖它（冻结代价极高） |
| 不做 | 单位换算表、英制、量纲分析 UI —— 插件 |

### 2.2 `core/abi/` —— 跨边界内存布局

| 项 | 内容 |
|---|---|
| 职责 | 定义跨 Qt / 跨进程语言 / 未来 Web 共用的二进制布局与版本协商 |
| 关键类型 | `FieldBuffer`, `LatticeDesc`, `RuntimePlan`, `ParticleSoA`, `abi_version` |
| 冻结 | **全部冻结**。改布局必须升 `abi_version` + 改 `static_assert` |
| 依赖 | `units`（仅用于量纲标注） |
| 为什么在地基 | 唯一共用物；改了所有消费者全废 |
| 不做 | 序列化格式、共享内存、socket —— 全是传输适配器插件 |

### 2.3 `core/ports/` —— 端口类型注册表

| 项 | 内容 |
|---|---|
| 职责 | 类型的注册、校验、兼容性判定；量纲/单位校验的挂载点 |
| 关键类型 | `PortTypeId`, `PortTypeDesc`, `PortTypeRegistry`, `Value`, `TypeCheck` |
| 冻结 | 注册 API 冻结；**已注册类型的集合可扩**（加类型不算破坏兼容） |
| 依赖 | `units`, `diag` |
| 为什么在地基 | 新类型必须能被声明，否则新域无法接入（"加一个域零核心改动"的前提） |
| 不做 | UI 控件 —— 见 `authoring/portui/` |

### 2.4 `core/plugin/` —— 插件契约与加载

| 项 | 内容 |
|---|---|
| 职责 | 清单解析、兼容性判定、加载、失败降级、能力声明 |
| 关键类型 | `Manifest`, `Capability`, `PluginHost`, `CompatPolicy`, `PluginHandle` |
| 冻结 | 清单 schema + 兼容策略冻结 |
| 依赖 | `ports`, `diag`, `reflect` |
| 为什么在地基 | 加载契约；版本静默错配是插件架构最常见的失败模式 |
| 不做 | 具体插件；网络分发；GUI 插件市场 |

### 2.5 `core/diag/` —— 诊断与错误

| 项 | 内容 |
|---|---|
| 职责 | 结构化诊断、错误传播、日志、用户可见归因 |
| 关键类型 | `Diagnostic`, `Severity`, `Result<T>`, `ErrorCode`, `Sink` |
| 冻结 | `Result<T>` 与错误码分类 |
| 依赖 | 无 |
| 为什么在地基 | 插件出错必须能被用户看见并归因，否则平台不可用 |
| 不做 | 日志文件轮转策略、上传、遥测 |

### 2.6 `core/reflect/` —— 类型枚举与名字

| 项 | 内容 |
|---|---|
| 职责 | 类型 → 字符串（含单位、端口类型名）的唯一来源 |
| 关键类型 | `type_name<T>()`, `unit_symbol<D>()`, `enum_names<E>()` |
| 依赖 | `units` |
| 为什么在地基 | C++ / YAML / 脚本必须共用**同一份**名字来源，否则三处会漂移 |

---

## 3. L1 —— 图与执行（`core/graph/`）

### 3.1 `core/graph/ir/` —— 中间表示

| 项 | 内容 |
|---|---|
| 职责 | 节点/端口/边的**契约**，不含求值逻辑 |
| 关键类型 | `NodeId`, `PortId`, `PortRef`, `NodeDesc`, `PortDesc`, `Edge`, `NodeKind` |
| 关键决策 | `Node` 只允许**三个可选钩子**（`compute` / `validate` / `on_param`），是上限不是起点 |
| 关键决策 | `Param` 与 `Port` **统一**——端口带 `connectable` 标志，不设第二套机制 |
| 冻结 | 是（这是插件 API 本身） |
| 依赖 | `ports`, `units`, `diag` |
| 不做 | 求值、缓存、布局坐标 |

### 3.2 `core/graph/structure/` —— 图结构

| 项 | 内容 |
|---|---|
| 职责 | 增删节点、连接、声明输出、版本号、无环校验 |
| 关键类型 | `Graph`, `GraphError`, `Version` |
| 强异常保证 | 任何变异失败后图与版本号均不变 |
| 依赖 | `ir` |
| 不做 | **布局坐标**（`auto_layout` 属于视图）· 求值 · 缓存 |

### 3.3 `core/graph/mutate/` —— 命令总线

| 项 | 内容 |
|---|---|
| 职责 | **全部**图变异的唯一入口；校验 → 应用 → 失效 → 入撤销栈 |
| 关键类型 | `Command`, `CommandBus`, `UndoStack`, `TxResult` |
| 关键决策 | 撤销栈**在地基**——多视图共享一个编辑会话，只能有一个撤销栈 |
| 依赖 | `structure` |
| 不做 | 具体 UI 手势 |

### 3.4 `core/graph/validate/` —— 加载期校验

| 项 | 内容 |
|---|---|
| 职责 | 打开实验时校验：无环、类型兼容、**单位匹配**、版本兼容 |
| 关键决策 | 校验发生在**加载期**，不是求值期。打开即报错，而不是算了十分钟曲线荒谬 |
| 依赖 | `structure`, `ports` |

### 3.5 `core/graph/eval/` —— 求值调度

| 项 | 内容 |
|---|---|
| 职责 | 拉取式求值、拓扑调度、节点级内容寻址缓存、失效传播 |
| 关键类型 | `Evaluator`, `EvalContext`, `CacheKey`, `CacheStore` |
| 关键决策 | 缓存键基于 **`Lattice::uid`（单调递增）**，绝不使用 `id()`（内存地址） |
| 依赖 | `structure`, `ports` |
| 不做 | 数值内核本身；具体域的语义 |

### 3.6 `core/graph/domain/` —— 域绑定

| 项 | 内容 |
|---|---|
| 职责 | 三个域的语义与调度形态：`field`（烘焙，纯函数）/ `particle`（实时，编译为原生计划）/ `render`（声明，不求值） |
| 关键决策 | `particle` 是 DAG 唯一的例外（有状态时间步进）；**仪器/测量域不需要例外** |
| 依赖 | `eval` |
| 不做 | Boris / 蛙跳 本身 —— 见 `kernels/` |

### 3.7 `core/graph/kernels/` —— 原生算子契约

| 项 | 内容 |
|---|---|
| 职责 | POD 算子契约 + 注册表 + 批量推进接口 |
| 关键类型 | `IBatchAdvancer`, `PlanOp`, `KernelRegistry` |
| 关键决策 | **契约是地基，Boris / 蛙跳 / RK4 / Verlet 是插件**（旧工程已做对这一点） |
| 依赖 | `abi`, `domain` |
| 性能 | 2 万粒子 × 5 步/帧 × 60fps，每帧零 Python 调用 |

### 3.8 `core/graph/field/` —— 场语义契约

| 项 | 内容 |
|---|---|
| 职责 | 场的**最小语义**：这是场吗、格子是什么、哪些分量有效 |
| 不做 | 偶极、磁尾、T04、包络 —— 全是插件 |
| 依赖 | `abi` |

---

## 4. L2 —— 运行与数据（`core/runtime/`）

> **这一层是上一轮分析中发现"缺失"的部分。** 旧工程的抽象里只有"图 + 版本号 + 槽位"，
> 没有"一次实验运行"这个概念。没有它，整条可追溯链无处安放。

### 4.1 `core/runtime/run/` —— 运行身份

| 项 | 内容 |
|---|---|
| 职责 | 定义"一次运行"：seed + 参数集 + 图版本 + 插件版本快照 |
| 关键类型 | `RunId`, `RunSpec`, `RunRecord`, `Provenance` |
| 为什么在地基 | 章程 R2（同 seed 复现）与 C1（确定性重放）全挂在这里；否则每个插件各自发明一套 |
| 关键决策 | seed 属于**运行**，不属于插件私有状态 |
| 依赖 | `diag` |

### 4.2 `core/runtime/store/` —— 数据与拟合

| 项 | 内容 |
|---|---|
| 职责 | **结构**：测量值 = 值 + 不确定度 + 时间戳 + 有效标志；拟合结果 = 系数 + 协方差 + 残差 + R² |
| 关键类型 | `Dataset`, `Measurement`, `FitResult`, `Uncertainty` |
| 关键决策 | 契约在地基，**算法是插件**（回归 / 传播 / 卡方都是内容） |
| 依赖 | `units`, `ports` |
| 不做 | 任何具体的拟合或不确定度传播算法 —— 插件 |

### 4.3 `core/runtime/trace/` —— 记录

| 项 | 内容 |
|---|---|
| 职责 | 时间轴、快照、读数序列 |
| 关键决策 | 时间轴是**平台机制**，不是视图的私有状态（C3） |
| 依赖 | `run`, `store` |

### 4.4 `core/runtime/io/` —— 导出契约

| 项 | 内容 |
|---|---|
| 职责 | 导出接口与格式注册 |
| 关键决策 | 格式全是插件；内核只提供 `FieldBuffer` ABI 与数据集读取 |
| 关键决策 | 预检 `check_export`：**写之前**回答「这个格式留不留得住不确定度」 |
| 依赖 | `store`, `trace`, `abi` |

### 4.5 `core/runtime/file/` —— 整文件读写

| 项 | 内容 |
|---|---|
| 职责 | 读/写整个文件；`absent` 与 `unreadable` 分开；UTF-8 路径到平台打开调用的**唯一**转换处 |
| 关键决策 | 它不在任何一个格式契约里：迹导出器与文档存取契约都要「把字节搬到路径上」，谁托管都会让另一个依赖一个主题不是它的模块 |
| 关键决策 | 失败不得留下半个缓冲区：调用者拿到半份文件时无法与「文件本身就短」区分，而后者看起来就是数据 |
| 依赖 | 无（标准库） |

### 4.6 `core/runtime/instrument/` —— 仪器一等公民（C6）

| 项 | 内容 |
|---|---|
| 职责 | 测量设备的契约、注册表，以及「分度 → 标准不确定度」的框架模型 |
| 关键类型 | `IInstrument`, `InstrumentDesc`, `MeasureContext`, `MeasureRefusal`, `InstrumentRegistry` |
| 关键决策 | **先有契约、后有设备**：C6 的原文是 `IInstrument` 与 `ISimModel` 同时立项、不许后补。`ISimModel` 那一半在本仓库已以 `kernels::IBatchAdvancer`／`execution::IStateOperator` 的形式存在，所以这里不造同名接口去满足一个名词 |
| 关键决策 | **R1 由类型承载**：`measure()` 收裸 `double`（真值）、吐 `UncertainValue`（值+不确定度+量纲）——没有不确定度的测量不是测量，而能返回裸数字的仪器可以把别人给它的真值当成自己测到的 |
| 关键决策 | **承诺 3 是一个函数**：`resolution_uncertainty(w) = w / sqrt(12)`（均匀分布的 B 类评定）。插件可覆盖，但起点是有定义的答案 |
| 依赖 | `units`, `diag`, `store`, `plugin`（宿主取读数走 C4 的故障屏障） |

---

## 5. L3 —— 视图服务（`core/authoring/`）

> 注意：**L3 是"视图服务"，不是"视图本身"。** 具体 UI 在 `views/`（消费者）。
> 这一层存在的唯一理由：让多个视图能共存于同一个编辑会话。

### 5.1 `core/authoring/document/` —— 文档与会话

| 项 | 内容 |
|---|---|
| 职责 | 图文档的打开/保存/另存；**按视图 id 分槽的布局元数据** |
| 关键类型 | `Document`, `DocumentSession`, `ViewLayouts` |
| 关键决策 | 节点坐标是**布局算法的输出**，存进 `view_layouts["<view_id>"]`，内核全程不知道 "x" 是什么 |
| 关键决策 | 没装节点编辑器的用户，文档里就没有 `graph` 那一槽 |
| 依赖 | `graph/ir` |

### 5.2 `core/authoring/capability/` —— 能力协商

| 项 | 内容 |
|---|---|
| 职责 | 插件声明能力，宿主查询与协商 |
| 关键类型 | `ICapability`, `ICapabilityHost`, `CapabilityQuery` |
| 依赖 | `plugin` |

### 5.3 `core/authoring/commands/` —— 命令总线暴露

| 项 | 内容 |
|---|---|
| 职责 | 把 `graph/mutate` 的命令暴露给视图；统一撤销栈；变更广播 |
| 关键决策 | 所有视图**必须**走这条总线；任何视图不得自持一份"我的图" |
| 依赖 | `graph/mutate` |

### 5.4 `core/authoring/layout/` —— 布局契约

| 项 | 内容 |
|---|---|
| 职责 | 布局算法接口，输入图结构输出坐标 |
| 关键决策 | 算法**是插件**；这里只有接口与注册 |
| 依赖 | `document`, `graph/ir` |

### 5.5 `core/authoring/portui/` —— 端口类型驱动的属性面板

| 项 | 内容 |
|---|---|
| 职责 | 每种端口类型声明自己的 UI 描述；通用面板自动渲染任何已注册类型；插件可覆盖 |
| 关键决策 | **新端口类型自动获得可编辑 UI**——加一种类型不需要动任何面板代码 |
| 依赖 | `ports`, `capability` |

### 5.6 `core/authoring/persist/` —— 存取契约

| 项 | 内容 |
|---|---|
| 职责 | 「文档 ↔ 字节」的契约：`DocumentSnapshot`/`DocumentSource`、`DocumentRefusal`、`IDocumentFormat` |
| 关键类型 | `IDocumentFormat`, `DocumentFormatDesc`, `DocumentSnapshot`, `DocumentSource`, `DocumentRefusal` |
| 关键决策 | **格式是插件，问题形状不是**：JSON／紧凑二进制／教师用 YAML 都是选择，选择属于内容（§2.2）；但「文档怎么变成字节、怎么变回来、失败叫什么名字」必须是地基，否则每个格式各写一套 |
| 关键决策 | 写**借用**、读**拥有**：保存路径不得持有图的第二份副本（第二份副本就是第二个答案，两者必然漂移）；而刚解析出来的图总得住在某处，只有调用者知道住哪 |
| 关键决策 | 拒绝是一个**码**而不是 bool：`malformed` 与 `unsupported_version` 是两种问题、两种修法 |
| 依赖 | `ir`, `structure`, `document` |

> 为什么单独一个模块而不是塞进 `document`：`document` 的依赖表刻意只到 `ir`，它的文件头写明「文档描述一张图，不持有第二张图」。保存要按值拿到图，那就需要 `structure`——为一个它明说不做的职责去放宽它的依赖表，比新增一个模块更贵。

---

## 6. 消费者与内容（不在 `core/`）

### 6.1 `views/` —— Qt 界面（可整体删除）

| 视图 | 说明 |
|---|---|
| `qt/nodegraph/` | 节点编辑器。**它就是 `IView` 插件的一个实现** |
| `qt/blocks/` | 积木化视图（Scratch 风格）。与节点编辑器投影同一份图 |
| `qt/formula/` | 公式 / YAML 文本视图 |
| `qt/property/` | 端口类型驱动的属性面板（`portui` 的实现） |
| `qt/instrument/` | 仪器读数面板（投影优先：高对比、大字号、色盲安全） |
| `qt/modelconf/` | 模型置信度面板（能量漂移、∇·B 漂移，C8） |

**判据**：四个创作视图投影同一 IR，是同一份图的四种皮，不是四种架构。

### 6.2 `plugins/` —— 内容（可整体删除）

| 目录 | 内容 |
|---|---|
| `models/` | 偶极、磁尾、T04、包络等具体物理模型 |
| `kernels/` | Boris、蛙跳、RK4、Verlet 的具体实现 |
| `instruments/` | 具体仪器（含误差模型） |
| `analysis/` | 回归、不确定度传播、卡方、残差分析 |
| `formats/` | 具体格式，一个目录一个：`qpjson/`（文档，JSON 文本，实现 `authoring/persist`）、`csv/`（迹导出，实现 `runtime/io`） |
| `views_items/` | 场线、粒子、拖尾等渲染项（三来源：内置 / 用户热扫描 / 节点内联） |
| `experiments/` | L1 教师层 YAML 实验描述 |
| `examples/` | 示例图库 |

### 6.3 `tests/` `bench/`

见 `standards/test-taxonomy.md`。注意 `tests/contract/` 是**契约合规套件**所在地，
它与插件接口**同时提交**，是新增接口的强制配套物。

---

## 7. 下推规则（本文件如何继续细分）

```
docs/plan-tree.md          ← 本文件（到模块级）
    └─ core/<layer>/<module>/spec.md       ← 模块规范：类型契约 + 接口签名（到类型级）
          └─ core/<layer>/<module>/functions.md  ← 函数契约（到函数级）
                └─ include/qp/.../*.hpp     ← 头文件里的契约注释（与代码同源）
                      └─ tests/...          ← 每个 @tests 条目的实现
```

### 不可颠倒的顺序

```
(1) 类型契约  →  (2) 接口签名  →  (3) 函数契约  →  (4) 实现  →  (5) 测试
   （冻结）      （冻结）       （逐模块推进）
```

> **同一模块内，类型先冻结；不同模块间，下一层的类型定型前不写上一层的函数规范。**

理由：把 `graph.connect()` 的签名先写死、而 `PortId` 还没定型，
那份规范 100% 会返工，并带着下游几十个函数一起返工。

### 进度如何记录

`plan-status.json` 记录每个模块的下推深度：

| 状态 | 含义 |
|---|---|
| `planned` | 只有模块级描述（本文件） |
| `contract` | 类型契约已冻结（`spec.md` 就位） |
| `specified` | 接口签名 + 函数契约就位（`functions.md` 就位） |
| `implemented` | 实现完成，测试通过 |
| `verified` | 全部测试 + 门禁 + 覆盖率证明义务通过 |

---

## 8. 依赖规则（CI 强制）

允许的依赖方向（箭头 = "可以依赖"）：

```
units ← diag ← ports ← reflect
                 ↑
              abi ┤
                 ↓
        ir → structure → mutate
                 ↓
              eval → domain → kernels
                 ↓
              field
─────────────────────────（core 边界：以下禁止被 core 依赖）
runtime → authoring
            ↑
        persist（document + structure：格式是插件，契约是地基）
─────────────────────────
views, plugins        （消费者，core 不得 include）
```

> `runtime/` 内部另有两条不越过上图的边：`instrument` 依赖 `store`（一条读数**就是** `UncertainValue`，
> 另造一个平行类型正是本项目反复避免的「两份表示」），`eval`/`execution`/`instrument` 依赖 `plugin`
> 以取得 C4 的故障屏障——`plugin` 是 L0，这是向下依赖而非倒置。

### 五条铁律

1. **`core/` 不得包含任何 Qt 头文件。** 错误类型、注册表、求值器全部与 Qt 无关。
   Qt 只在 `views/`、`plugins/`、宿主可执行文件中出现。
2. **`core/` 不得包含 `views/` 或 `plugins/` 的头文件。**
3. **`core/` 不得依赖 Eigen 之外的第三方**（Eigen 仅允许出现在 `eval/` 与数值内核接口后方）。
4. **`units` 与 `diag` 不得依赖 core 内任何其他模块。**
5. **热路径函数禁止抛异常、禁止分配、禁止虚调用**（由 clang-tidy 自定义规则检查）。

---

## 9. 里程碑（每期的定义已完成）

> 深度规定：每一期都必须走完 §7 的 (1)→(5)，不允许"先写实现，规范后补"。
> 但也**不允许**在类型未定型时批量预写下游函数规范——那是规范剧场。

| 期 | 范围 | 完成定义（DoD） |
|---|---|---|
| **P0** | 立法 | 计划树 + 契约模板 + 测试分类 + 门禁 + 章程落地；目录骨架就位；CI 空跑通过 |
| **P1** | `core/units/` **全深度纵切** | 类型合同冻结 → 头文件契约 → 单元/性质/静态测试全绿 → 覆盖率按证明义务核对。**用这一个模块验证整套规范可用** |
| **P2** | `core/abi/` + `core/ports/` 冻结 | 布局 `static_assert` 全绿；端口类型注册 + 单位校验测试；兼容性拒绝策略测试 |
| **P3** | `core/graph/` + `exec/` | 求值性质测试；黄金回归（对齐旧工程 19 诊断点形态）；缓存失效正确性；性能基线 |
| **P4** | `core/plugin/` + `authoring/` | 契约合规套件框架就位；manifest 模糊测试；命令总线 + 撤销测试 |
| **P5** | 首个域插件包（力学：弹簧振子 / 斜面 / 碰撞） | **零核心改动证明**；四条平台承诺的自动化验收全绿 |

### P5 的验收断言（对应章程 §6）

| 承诺 | 自动化证明方式 |
|---|---|
| 加仪器不改模型 | CI 中新增一个仪器插件，diff 必须为空（除插件目录） |
| 加实验 = 写 YAML | 新实验无编译产物，直接加载 |
| 改精度 → 改不确定度 | 同一实验两组精度，最终不确定度必须不同（且方向正确） |
| 同 seed 逐位复现 | 双跑比对，逐位相等 |

> **P1 那条纵切的意义**：先用一个模块把整套规范跑通，证明这份流程本身可执行，
> 再按同一模板复制到其余模块。否则会写出一份没人能照着执行的规范——那比没有规范更糟。
