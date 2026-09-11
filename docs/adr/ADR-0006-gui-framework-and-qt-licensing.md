# ADR-0006：GUI 框架与 Qt 许可

- 状态：已接受（2026-09，取代 `docs/00-charter.md` §5 中 GUI 那一格）
- 决策者：项目维护者
- 相关：`docs/00-charter.md` §5、§6；`standards/enforcement.md` §4；`views/CMakeLists.txt`

---

## 1. 背景

章程 §5 写的是「Qt 6.5+（QML + Qt Quick 为主，QWidget 用 `QQuickWidget` 嵌入）」。
这句是在只有抽象需求时写下的。开始实现视图层时，有三件事需要重新确认：

1. 这个平台**实际**需要哪几种界面；
2. 这些界面里哪些是 Qt 擅长的、哪些不是；
3. 因为要**分发二进制**，Qt 的许可到底允许什么。

---

## 2. 这个平台实际需要的界面

来自 `docs/plan-tree.md` §6.1，判据是「四个创作视图投影同一份 IR」：

| 视图 | 需要什么 |
|---|---|
| `qt/nodegraph/` | 画布：拖动节点、连边、缩放、命中测试 |
| `qt/blocks/` | 同一张图的积木式投影 |
| `qt/formula/` | **带语法高亮的多行文本编辑器** |
| `qt/property/` | 数据驱动的属性面板（`portui` 的渲染端） |
| `qt/instrument/` | 大字号、高对比、色盲安全的读数面板 |
| `qt/modelconf/` | 能量漂移、∇·B 漂移等诊断数值 |

关键观察：**画布和文本编辑器都必须好用。** 只考虑画布的话，即时模式 GUI（Dear ImGui 一类）会更省事；
但要自建一个像样的、带高亮的代码编辑器，是数周量级的工作，而 `QPlainTextEdit` + `QSyntaxHighlighter`
是现成的。

---

## 3. 决策

**维持 Qt 6.5+。** 本 ADR 不改这一点。

**但把「QML + Qt Quick 为主」改为「QWidgets 为主」。** 理由：

1. QML 的强项是触摸、动画、声明式布局。实验台软件不需要这三样。
2. QML 会把调试分成两个世界（C++ 栈 + QML 运行时），而本项目的规范是
   「每个函数都有契约、`@tests` 与用例逐字对应」——一个不能单步进去的运行时
   对这套规范是净负担。
3. 节点画布用 `QGraphicsScene` 完全够用：它本来就是为「大量可交互图元 + 命中测试」
   设计的。

**不引入 Qt Charts。** 见 §5 的许可分析。

---

## 4. 被否决的替代品，以及各自的否决理由

| 框架 | 优点 | 否决理由 |
|---|---|---|
| **Dear ImGui** | 集成最轻（几个源文件，无安装器、无账号）；即时模式天生适合**仪器读数面板**；节点画布有成熟的第三方实现 | 文本编辑是它最弱的一环，而 `qt/formula/` 需要它；没有原生控件，实验软件的观感会像调试工具 |
| **wxWidgets** | 真原生控件；许可最宽松（wxWindows 许可，无 LGPL 义务） | 节点画布、科学绘图、文本高亮都要自己接第三方；四个创作视图几乎没有现成的 |
| **GTK** | 成熟 | Windows 上不是一等公民；本项目主交付路径是 Windows 64 位 |
| **Web 前端** | 生态最丰富 | 章程 §5 已否决（本地效率优先），且 wasm 无法加载 `.pyd`/DLL |

---

## 5. 许可：因为要分发二进制，这一节是硬约束

Qt 6 开源版按 **LGPLv3**（部分模块例外）提供。分发二进制时，LGPLv3 要求：

| 义务 | 本项目的做法 |
|---|---|
| **动态链接** Qt | 已满足：链接 `Qt6Widgets.dll` / `Qt6Guid.dll` / `Qt6Cored.dll`，不静态链接 |
| 随分发提供 LGPLv3 全文 | **待办**，见 §6 |
| 声明使用了 Qt 及其版本 | **待办**，见 §6 |
| 允许用户替换 Qt 库（可重链接） | 已满足：动态链接即可替换 DLL |
| 若修改了 Qt 本身，需公开修改 | 未修改 Qt 源码 |

### 5.1 为什么不用 Qt Charts

**Qt Charts 是 GPLv3 或商业许可，不是 LGPLv3。** 引入它会把整个分发物拉进 GPLv3，
而本项目并不打算整体 GPL。绘图组件改用 **QCustomPlot（MIT）** 或 **QWT（LGPL）**。

这条不是风格偏好：它是一个会让「分发二进制」这个目标直接失效的选择。

### 5.2 项目自身的许可还没定

仓库目前**没有 LICENSE 文件**。这是分发前的阻塞项：没有许可的代码默认是
「保留所有权利」，别人拿到二进制也无法合法使用。本项目自己的许可由维护者决定，
本 ADR 只记录它必须先被定下来。§6 列了清单。

---

## 6. 分发前必须补齐的清单

- [ ] 选定并写入 `LICENSE`（项目自身许可）
- [ ] 写入 `THIRD_PARTY_NOTICES.md`：Qt 版本、模块、LGPLv3 指向
- [ ] 随二进制附带 LGPLv3 全文（`LICENSE.LGPLv3`）
- [ ] `scripts/fetch_deps.ps1 -WithQt` 能无人值守装出 Qt（本 ADR 落地时用 `aqtinstall` 手工装到 `C:\Qt`，脚本化待补）
- [ ] 用 `windeployqt` 生成可分发目录，并验证在一台没装 Qt 的机器上能跑
- [ ] 若最终使用了 QCustomPlot/QWT，同样登记进 notices

---

## 7. 工具链后果：GUI 只能是 MSVC x64

Qt 6 的 Windows 桌面包只提供：

```
win64_msvc2022_64          win64_mingw          win64_llvm_mingw
```

**没有 32 位 Qt 6。** 本项目的 GCC 工具链是 **MinGW i686（32 位）**，所以
`QP_BUILD_VIEWS=ON` 无法用 GCC 构建。

这不是架构上的妥协，而是消费者/地基分离的正常结果：

- `core/` 依然**双编译器**验证（GCC 15.2 + MSVC 19.51），且**完全不依赖 Qt**——
  铁律 1 与层级门禁 L2 在守这条线；
- `views/` 只在 MSVC x64 下构建。

换句话说：**「没有Qt时core仍然全绿」是本架构的既有性质，现在它有了第二个用途**——
它让 GUI 成为可选前端这件事可被机械验证，而不是靠承诺。

---

## 8. 落地时踩到的两个 Qt 陷阱（记录以免重踩）

### 8.1 `slots` / `signals` / `emit` 是宏，不是关键字

`views/hello_view.cpp` 同时包含了 Qt 与 core，MSVC 立刻报：

```
core/graph/structure/include/qp/graph/structure/graph.hpp(411): error C2059: 语法错误:"<parameter-list>"
```

因为 core 的 `Graph::slots()` 是合法标识符，而 Qt 把 `slots` 定义成了
`QT_ANNOTATE_FUNCTION(qt_slots)`。**报错指向一个完全正确的 core 头文件**，这是它最坏的地方。

**修法**：对 `qp_views` 定义 `QT_NO_KEYWORDS`，改用 `Q_SLOTS` / `Q_SIGNALS` / `Q_EMIT`。
不采用「调整 include 顺序」——那只是把碰撞留给下一个头文件，而错误信息依然指向受害者。

### 8.2 AUTOMOC 的两个顺序要求

1. `set(CMAKE_AUTOMOC ON)` 必须在**任何 `add_library` / `add_executable` 之前**：
   这个变量在创建目标时被读取，之后设置会让该目标静默地没有 moc。
2. **带 `Q_OBJECT` 的头文件必须列进它所属目标的 sources。** AUTOMOC 只扫描
   *目标自己的* source，靠 include 传递进来的头文件会被 moc 到「碰巧包含它的那个目标」里，
   于是另一个目标链接时报三个未解析符号（`metaObject` / `qt_metacast` / `qt_metacall`）。

两条都表现为**链接错误而非配置错误**，而错误信息指向宏或目标之外的地方。
