# 函数契约模板

> 本项目要求：**每一个函数都必须有契约注释、唯一归属、以及对应的测试条目。**
> 没有 `@tests` 条目的函数，CI 拒绝合并。

---

## 1. 为什么是契约而不是注释

普通注释回答"这段代码在干什么"。契约回答四个工程问题：

1. **谁负责它的生命周期？**（`@ownership`）
2. **它能不能被并发调用？**（`@thread`）
3. **失败怎么表达？**（`@errors`）
4. **改它要不要升版本号？**（`@frozen`）

这四问的答案是**函数边界**。边界不清就是技术债的唯一来源。

---

## 2. 模板

```cpp
/**
 * @brief 一句话：做什么。（不写"如何做"——如何做由实现和测试保证）
 *
 * @ownership   pure | owns | observes | borrows
 * @thread      any | main | eval
 * @pre         前置条件。违反 = 编程错误 → 断言，不返回错误码
 * @post        后置条件。调用返回后必然成真的事实
 * @invariant   跨调用保持的不变量
 * @errors      失败表达方式：Result<T> | throws | noexcept | assert-only
 * @complexity  复杂度。数值内核必填（含常数因子来源）
 * @nondet      若依赖随机/时间/地址，必须写明种子来源；否则写 none
 * @frozen      是否属于冻结契约 / ABI；若是，改它要升版本号
 * @tests       对应测试用例 id，必须非空，可多个
 */
```

### 字段释义

| 字段 | 含义 | 违反的后果 |
|---|---|---|
| `pure` | 无副作用、无状态、同输入同输出 | —— |
| `owns` | 拥有传入/传出对象的生命周期 | 泄漏或双重释放 |
| `observes` | 只读观察，不延长生命周期 | 悬垂引用 |
| `borrows` | 借用，期间保证存活，调用方负责 | 悬垂引用 |
| `any` | 无共享可变状态，可任意线程调用 | —— |
| `main` | 仅主线程（UI、文档编辑、Qt 对象） | 数据竞争 |
| `eval` | 仅图求值线程 | 数据竞争 |
| `ui` | 仅 UI 线程（需要与 `main` 区分时） | 数据竞争 |
| `publish` | 仅数据发布线程（seqlock 写侧） | 数据竞争、快照撕裂 |
| `@nondet` | **本项目特有**——守住章程 R2（同 seed 复现） | 复现性失效 |
| `@frozen` | **本项目特有**——守住变更流程第 7 条 | 静默不兼容 |

---

## 3. 完整示例

### 例 1：纯函数（量纲代数）

```cpp
/**
 * @brief 两个量相乘：量纲相加，数值相乘。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果的量纲为 lhs.dim + rhs.dim；数值为精确乘积
 * @invariant   满足交换律与结合律（见 property test）
 * @errors      noexcept；无失败模式
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否——但 Dim 的底层表示是冻结的，见 abi/
 * @tests       units.multiply.basic, units.multiply.commutative,
 *              units.multiply.associative, units.multiply.dim_exact
 */
template<Dim L, Dim R>
[[nodiscard]] constexpr Quantity<L + R>
operator*(Quantity<L> lhs, Quantity<R> rhs) noexcept;
```

注意 `@tests` 里同时有 basic 和 property：**纯函数也要有性质测试**，
否则只能证明你想到的那几个例子对。

---

### 例 2：有副作用、有归属问题（图变异）

```cpp
/**
 * @brief 在图中建立一条连接。成功后图的版本号递增。
 *
 * @ownership   borrows（graph 必须存活至调用返回；不保留任何引用）
 * @thread      main
 * @pre         graph 未处于求值中（evaluating() == false）
 * @post        成功时：边存在，graph.version() == 调用前 + 1，
 *              缓存中所有下游条目失效
 * @post        失败时：图与版本号均不变（强异常保证）
 * @invariant   成功后图必然无环
 * @errors      Result<void>；失败码见 GraphError
 * @complexity  O(V+E)（环检测）；连接本身 O(1)
 * @nondet      none
 * @frozen      否
 * @tests       graph.connect.ok, graph.connect.cycle_rejected,
 *              graph.connect.strong_guarantee, graph.connect.version_bump,
 *              graph.connect.cache_invalidation
 */
Result<void> Graph::connect(PortRef from, PortRef to);
```

**两个 @post 是重点**：一个描述成功，一个描述失败。
"失败时什么都不变"（强异常保证）是必须写下来的——否则调用方无法推理。
`@pre` 里的"求值中禁止变异"就是线程边界的具体化。

---

### 例 3：数值内核（性能红线 + 确定性）

```cpp
/**
 * @brief 推进一批粒子一个时间步（蛙跳法）。
 *
 * @ownership   borrows（in/out 缓冲区由调用方拥有）
 * @thread      eval
 * @pre         out.size() == in.size()；dt > 0；in 中所有分量有限
 * @post        所有 out 分量有限（非有限值被钳制，见 C2）
 * @post        能量漂移上界见测试；不保证能量守恒，保证不发散
 * @invariant   in 不被修改
 * @errors      noexcept；通过 out 中的 valid 标志报告单个粒子失效
 * @complexity  O(N)，N = 粒子数；无分配、无虚调用
 * @nondet      none（本内核无随机项）
 * @frozen      否——但输入输出缓冲区布局来自 abi/，那是冻结的
 * @tests       kernels.leapfrog.golden_19point,
 *              kernels.leapfrog.energy_drift_bound,
 *              kernels.leapfrog.second_order_convergence,
 *              kernels.leapfrog.nonfinite_clamped,
 *              kernels.leapfrog.perf_budget_20k_x5
 */
void advance_leapfrog(std::span<const ParticleSoA> in,
                      std::span<ParticleSoA> out,
                      const FieldBuffer& field,
                      double dt) noexcept;
```

`@tests` 里一共五条，覆盖了单元、黄金回归、性质、鲁棒性、性能——
**这就是"每个函数都要过单独测试"在一个数值内核上的具体含义**。

---

### 例 4：反面示例（不要这样写）

```cpp
// [X] 没有契约
void process(Graph& g, Data& d, bool flag);

// 这行代码有四个未回答的问题：
//   - g 和 d 谁拥有？谁能在调用期间改动它们？
//   - flag 是什么？为真为假各是什么语义？
//   - 失败怎么办？
//   - 能并发调用吗？
// 这四项任一不清楚，三个月后必然出现"改一个坏三个"。
```

---

## 4. 契约与测试的绑定

| 契约字段 | 强制对应的测试 |
|---|---|
| `@pre` | 至少一个**违反前置条件的断言测试**（在 debug 下必须触发） |
| `@post`（成功分支） | 至少一个正向测试 |
| `@post`（失败分支） | 至少一个**失败后状态不变**的测试（强异常保证） |
| `@invariant` | 至少一个 property test |
| `@errors` | 每个错误码至少被触发一次 |
| `@nondet` 非 none | 至少一个**同 seed 两次结果逐位相同**的测试 |
| `@frozen` 为是 | `static_assert` 布局断言 |
| `@complexity`（数值内核） | 一条 Google Benchmark 基线 |
| `@thread` 非 any | 至少一个 TSan 下的并发测试 |

---

## 5. 层级规则（编译期强制）

- 函数契约注释中的 `@ownership` / `@thread` 与实际用法不符 → code review 拒绝。
- `core/` 不得包含 `views/` 或 `plugins/` 的头文件 → CMake target 依赖 + CI 检查（见 `enforcement.md`）。
- 内核热路径函数禁止 `@errors throws` → clang-tidy 自定义检查。
- 任何 `static` / 全局可变状态 → 必须在契约中说明其生命周期与线程规则，否则拒绝。

---

## 6. 落地顺序（不可颠倒）

```
(1) 类型契约  →  (2) 接口签名  →  (3) 函数契约  →  (4) 实现  →  (5) 测试
   （冻结）      （冻结）       （逐模块推进）
```

> **函数契约不能独立于类型契约设计。**

把 `graph.connect()` 的签名先写死、而 `PortId` 还没定型，那份规范 100% 会返工，
而且会带着后面几十个函数一起返工。

**同一模块内，类型先冻结；不同模块间，下一层的类型定型前不写上一层的函数规范。**
