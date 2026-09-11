# qp — general-purpose physics experiment simulation platform

一个本地化的、面向**大学基础物理实验（测量导向）**的通用仿真平台。架构原则是
**万物皆插件**：地基只提供契约、机制与调度，内容（模型、仪器、内核、视图项、实验）
全部是可增删的插件。

## 它和 PhET / Comsol 不是同一件事

真正的竞争对象是 **PhET + Excel**，而不是 Comsol。差异化在于那个闭环：

```
测量  ->  记录  ->  不确定度  ->  报告
```

现成的仿真软件演示现象，Excel 处理数据，中间那一步——把「我测到了什么、误差多大、
结论能不能站得住」变成平台机制而不是学生的自觉——是空白的。

## 四条平台承诺

它们是存在理由，不是特性列表（`docs/00-charter.md` §6）：

1. **加一个仪器不需要改模型代码。**
2. **加一个实验 = 写一份 YAML，不需要编译。**
3. **改仪器精度，必须真的改变最终不确定度。**
4. **同 seed 逐位复现。**

第 3 条和第 4 条决定了地基里为什么必须有 `runtime/run`（运行身份）和
`runtime/store`（测量值 + 不确定度的形状）——没有它们，这两条承诺无处安放。

## 架构

```
L0  core/units  core/diag  core/abi  core/ports  core/plugin
L1  core/graph/{ir,structure,mutate,validate,eval,domain,kernels,field}
L2  core/runtime/{run,store,trace,io}
L3  core/authoring/{document,capability,commands,layout,portui}
--------------------------------------------------------------
    views/      Qt 界面          （消费者，可整体删除）
    plugins/    内容             （可整体删除）
```

**核心断言**：`views/` 与 `plugins/` 可以整个删掉，`core/` 仍能编译并通过全部测试。

这条不是口号，是被机械验证的：`core/` 不包含任何 Qt 头文件（层级门禁 L2 强制），
而测试会在**完全没装 Qt** 的配置下构建并跑完 core 的全部测试。

设计文档入口：`docs/00-charter.md`（章程）→ `docs/plan-tree.md`（计划树，到模块级）
→ `standards/`（契约模板、测试分类、门禁规则）。

## 构建

依赖由脚本显式拉取，**配置阶段默认不联网**：

```powershell
pwsh scripts/fetch_deps.ps1              # Catch2（测试必需）
pwsh scripts/build.ps1                   # GCC + MSVC 双编译器构建与测试
pwsh scripts/build.ps1 -Only gcc         # 只跑一个编译器
```

要求：CMake ≥ 3.24、C++20、GCC 或 MSVC。

### 构建 GUI（可选）

GUI 是消费者，需要 Qt 6.5+，且**只能用 MSVC x64**——Qt 6 没有 32 位包，
所以本项目的 MinGW i686 工具链构建不了视图层。

```powershell
# Qt 装到 C:\Qt 或 external/Qt 均可，CMake 两处都会找
cmake -S . -B build-views -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_CXX_COMPILER=cl -DQP_BUILD_VIEWS=ON
cmake --build build-views
```

框架与许可的取舍见 `docs/adr/ADR-0006-gui-framework-and-qt-licensing.md`
（要点：用 QWidgets 而非 QML；**不用 Qt Charts**，它是 GPL-3.0 而非 LGPL，
会把整个分发物拉进 GPL）。

## 许可

**Apache License 2.0** —— 见 `LICENSE`。

选它的理由是它和架构自洽：`core/` 被设计成可复用的地基，那就应该让它尽可能容易
被复用（含专利授权、无 copyleft 摩擦），而不是给它加一道需要法律判断的门槛。

使用 Qt（LGPL-3.0）的动态链接，第三方组件与各自的义务登记在
`THIRD_PARTY_NOTICES.md`，随二进制分发的归属声明在 `NOTICE`。

**分发二进制前请先读 `THIRD_PARTY_NOTICES.md` §1.1**：它列出 LGPL-3.0 对本项目的
具体要求，以及打包时必须一并附带什么。

`scripts/check_qt_modules.py` 会检查链接进来的 Qt 模块集合，任何非 LGPL 兼容的
模块（最典型的是 Qt Charts）都会让门禁失败。它读的是**生成出来的链接行**而不是
源码，因为源码扫描看不见 CMake 组件声明和传递依赖这两种真实出错方式。

## 状态

`docs/plan-status.json` 是模块状态的唯一权威记录（`planned` → `contract` →
`specified` → `implemented` → `verified`），门禁与编译器矩阵结果也在那里。

## 贡献

每个函数都要有契约注释（`@ownership` / `@thread` / `@pre` / `@post` / `@errors` /
`@tests`），且 `@tests` 里的 id 必须与 `tests/` 下的 `TEST_CASE` **逐字对应**——
`scripts/check_contracts.py` 会核对，对不上就构建失败。模板见
`standards/function-contract.md`。

代码方言（注释语言、禁 emoji、日志 JSON 格式）见 `standards/code-dialect.md`。
