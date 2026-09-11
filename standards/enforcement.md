# 边界门禁（CI 强制检查）

> 核心原则：**边界靠工具强制，不靠文档。**
> 别指望"写清楚了就不会越界"。写清楚了仍然会越界——所以要写检查器。

---

## 1. 提交阶段（每次 commit，< 2 min）

| 检查 | 工具 | 失败即拒绝 |
|---|---|---|
| 格式 | clang-format --dry-run -Werror | 格式不一致 |
| 静态分析 | clang-tidy（含自定义规则，见 §3） | 命中任一规则 |
| 单元测试 | Catch2 | 任一失败 |
| 性质测试 | RapidCheck（缩减到 100 次迭代，快速模式） | 任一反例 |
| 静态布局断言 | `tests/abi/` 编译 | 布局改变 |
| 层级依赖 | `scripts/check_layers.py`（§4） | 越界 include |
| **契约完备性** | `scripts/check_contracts.py`（§5） | 函数缺 `@tests` |
| **编译器矩阵** | GCC + MSVC 双编译（§2.1） | 任一编译器失败 |

### 2.1 编译器矩阵（不可省）

> **"在 GCC 上通过"不等于"可移植"。**

P1 阶段实测：`template <Dim L, Dim R>` 形式的运算符在 GCC 15 通过、在 MSVC 19.44
失败（MSVC 不能推导类类型的非类型模板参数，详见 `docs/adr/ADR-0004`）。

因此 **core 的每一次提交都必须在 GCC 与 MSVC 上同时构建通过**。
只在一个编译器上验证过的代码不算完成——这是"接口冻结"这个动作的最低门槛。

CI 矩阵：

| 编译器 | 标准 | 说明 |
|---|---|---|
| GCC（MinGW-w64 或 Linux） | C++20 | 主力开发与快速反馈 |
| MSVC 19.4x | /std:c++20 | Windows 交付路径，必须绿 |
| Clang（可选） | C++20 | 交叉验证，尽早发现方言差异 |

## 2. PR / 合并阶段

| 检查 | 说明 |
|---|---|
| 全量性质测试 | RapidCheck 1000 次迭代 |
| 黄金回归 | 逐位比对，`max|Δ| == 0.0` |
| 确定性双跑 | 同 seed 逐位相同 + 异 seed 必须不同 |
| 契约合规套件 | `tests/contract/` 全部通过 |
| 覆盖率证明义务 | 按 `test-taxonomy.md` §10 逐条核对，**不看覆盖率数字** |
| **core 独立性** | 删除 `views/` 与 `plugins/` 后 `core` 仍能构建并通过全部测试 |

## 3. 自定义 clang-tidy 规则

内核里有五件事永远不该出现。写成规则，而不是写进 review checklist。

| 规则 | 抓什么 | 例外 |
|---|---|---|
| `qp-no-raw-new` | `core/` 内裸 `new` / `delete` | `abi/` 的分配器实现 |
| `qp-no-mutable-global` | 非 const 全局 / 函数内 `static` 可变 | 必须显式 `// qp-allow-global: <理由>` |
| `qp-no-throw-hotpath` | 热路径函数抛异常 | `Result<T>` 返回值 |
| `qp-no-id-key` | 用 `id(`/指针值当缓存键或持久标识 | 显式标注 |
| `qp-no-qt-in-core` | `core/` 内出现任何 `Q*` 类型 | 无 |

**`qp-no-mutable-global` 这条特别重要**：它是"归属不清"最常见的物理形态——
一个全局变量等于一个没有主人的状态。旧工程 `registry.py` 里那个从不清空的模块级
`_REGISTERED` 就是这个模式，它导致"删掉的插件节点类型仍然存在"。

---

## 4. 层级依赖检查（`scripts/check_layers.py`）

### 机制

1. 解析 CMake，构建 target 依赖图。
2. 扫描 `core/` 下所有 `#include`，映射回头文件所属模块。
3. 按 `plan-tree.md` §8 的允许方向判定。

### 允许方向（白名单）

```python
ALLOWED = {
  "units":   set(),
  "diag":    {"units"},
  "ports":   {"units", "diag"},
  "reflect": {"units"},
  "abi":     {"units"},
  "ir":      {"ports", "units", "diag"},
  "structure": {"ir"},
  "mutate":  {"structure"},
  "validate": {"structure", "ports"},
  "eval":    {"structure", "ports"},
  "domain":  {"eval"},
  "kernels": {"abi", "domain"},
  "field":   {"abi"},
  "runtime": {"diag", "units", "ports", "abi"},
  "authoring": {"ir", "mutate", "ports", "plugin"},
}
# core 内任何模块 → views / plugins ：直接拒绝
```

### 附带检查

- `core/` 下出现 `#include <Q...>` → 拒绝
- `core/` 下出现 `#include <Eigen/...>` 且模块不是 `eval` / `kernels` → 拒绝
- `core/` 下出现 `#include "../views/..."` → 拒绝

**白名单是显式的。** 新增跨模块依赖必须改这个文件——改文件这个动作本身就是
"我知道我在扩大耦合"的确认。这是故意的摩擦。

---

## 5. 契约完备性检查（`scripts/check_contracts.py`）

扫描所有 `include/qp/**/*.hpp` 中的函数声明，验证：

| 检查 | 规则 |
|---|---|
| 有契约块 | 每个非平凡函数声明前必须有 `@brief` + 五个必填字段 |
| 有 `@tests` | 且至少一个条目非空 |
| **测试 id 存在** | `@tests` 里的每个 id 必须在 `tests/` 下真实存在为 `TEST_CASE` |
| `@errors` 与签名一致 | 若 `@errors` 写 `throws`，函数不得是 `noexcept`（反之亦然） |
| `@thread` 与锁一致 | 标注 `any` 的函数体内不得出现仅主线程资源 |
| `@frozen` 与 ABI 一致 | `abi/` 下所有结构体成员函数/字段必须标 `@frozen` |

**"测试 id 必须真实存在"是关键一条。** 否则 `@tests` 会退化成写几个看起来
合理的字符串——那比不写更糟，因为它制造了"已测试"的假象。

---

## 6. core 独立性验证（最重要的门禁）

```bash
# CI 中最重要的一条
cmake -B build-nocorecons -DQP_BUILD_VIEWS=OFF -DQP_BUILD_PLUGINS=OFF
cmake --build build-nocorecons
ctest --test-dir build-nocorecons --output-on-failure
```

> **`core/` 必须能脱离 Qt、脱离所有插件独立构建并通过全部测试。**

这条门禁是"地基 vs 插件"边界唯一的机械证明。如果它失败了，
说明有东西住进了地基——而它本该是插件。

---

## 7. ABI 守护

| 检查 | 机制 |
|---|---|
| 布局不变 | `static_assert` 尺寸/对齐/偏移 |
| 平凡可复制 | `static_assert(std::is_trivially_copyable_v<...>)` |
| 版本一致 | `static_assert(kAbiVersion == 期望值)` |
| 改布局必须升版本 | 上面三行断言同时失败 → 强迫作者思考兼容性 |
| 跨编译器一致 | CI 上 GCC/Clang/MSVC 三方构建 + 字节级序列化比对 |
| 旧版本拒绝 | 不兼容的 `abi_version` 必须**拒绝加载**，而不是尝试兼容 |

**断言就是流程的执法者。** 没有断言，兼容性规范只是一段没人读的文字。

---

## 8. 门禁自身的测试

> 门禁脚本也是代码，也必须被测。

`tests/meta/` 下放置**故意的违规样本**，验证每条规则都能抓到它：

| 样本 | 期望 |
|---|---|
| 一个缺 `@tests` 的函数 | `check_contracts.py` 报错 |
| 一个引用不存在测试 id 的函数 | 报错 |
| 一个 `core/` 里 include `<QObject>` 的文件 | `check_layers.py` 报错 |
| 一个违反允许方向的 include | 报错 |
| 一个改了 `FieldBuffer` 布局的补丁 | `static_assert` 编译失败 |

**未经验证的门禁等于没有门禁**——它给你虚假的安全感。
