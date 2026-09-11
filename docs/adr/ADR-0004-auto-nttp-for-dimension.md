# ADR-0004：量纲类型作为非类型模板参数时使用 `auto`

- **状态**：已采纳
- **日期**：P1（`core/units` 纵切）
- **影响面**：`core/units/` 全部运算符；后续所有需要把 `Dim` 放进模板参数的代码

## 背景

`Quantity<Dim D>` 用非类型模板参数（NTTP）承载量纲，这样量纲是**类型的一部分**，
写错量纲即编译失败。这是本项目量纲系统的核心机制。

在此基础上，二元运算需要把两个量纲参数推导出来：

```cpp
template <Dim L, Dim R>
constexpr Quantity<L + R> operator*(Quantity<L> a, Quantity<R> b) noexcept;
```

**这段代码在 GCC 上编译通过，在 MSVC 19.44 上编译失败。**

## 症状

MSVC 报 `error C2678: 二进制"*": 没有找到接受 "const qp::units::Mass" 类型的左操作数的运算符`，
并列出全部候选重载；其中 `operator*(Quantity<D>, Quantity<R>)` 的推导结果显示为

```
Quantity<='函数',>
```

即 MSVC **无法从实参推导"类类型的非类型模板参数"**，推导失败后整个重载被丢弃。

最小复现（`Dim` 只含两个 `signed char`）：

```cpp
template <Dim D> struct Q { double v; };
template <Dim L, Dim R> constexpr Q<L * R> mul_a(Q<L> a, Q<R> b);   // MSVC: C2672 未找到匹配的重载
template <auto L, auto R> constexpr Q<L * R> mul_b(Q<L> a, Q<R> b); // MSVC: OK
```

用一个"类模板偏特化做返回类型"的写法（`template <Dim L, Dim R> struct MulResult;`）
在 MSVC 上也能通过，说明问题**只在 NTTP 推导**，不在类类型做模板实参本身。

## 决策

**所有把量纲作为非类型模板参数的地方，一律写 `auto`，不写 `Dim`。**

```cpp
template <auto D>
constexpr Quantity<D> operator+(Quantity<D> a, Quantity<D> b) noexcept;

template <auto L, auto R>
constexpr Quantity<L * R> operator*(Quantity<L> a, Quantity<R> b) noexcept;

template <int P, auto D>
constexpr Quantity<dim_pow<P>(D)> pow(Quantity<D> a) noexcept;
```

`dim_pow` 的返回类型改用 `auto` 推导（`if constexpr` 两个分支返回 `Dim` 即可）。

## 理由

1. **零语义差异**：`auto` 参数仍然被 `Quantity<D>` 的类模板约束为 `Dim`，
   实参仍然是编译期常量。两者的类型检查强度完全相同。
2. **可移植**：GCC 15 与 MSVC 19.44 均通过（已实测）。
3. **不损失可读性**：`auto` 在 NTTP 位置已是 C++20 惯用法。
4. 备选方案（用偏特化 trait 计算返回类型）能在 MSVC 上通过，但把每个运算符
   从 2 行膨胀到 6 行，且引入一个纯为绕开编译器缺陷而存在的辅助模板——更差。

## 代价与约束

- **不能显式指定模板实参**：`operator* <dims::length, dims::time>(a, b)` 不再合法。
  实际上无人这样写；若将来需要，提供具名函数（如 `mul<L, R>()`）而不是运算符。
- 本决策被 `tests/unit/units/test_quantity.cpp` 的编译本身守护：
  若有人改回 `template <Dim L, Dim R>`，MSVC CI 会立即失败。

## 教训（写给后续的模块）

> **"在 GCC 上通过"不等于"可移植"。**

`units` 是第一个纵切模块，也是第一次暴露这个问题。后续 P2 的 `core/abi/`
涉及 ABI 布局断言，必须同样在 **GCC + MSVC**（以及可选的 Clang）三方上验证，
而不是只在一个编译器上通过就宣布冻结。这条已写入 `standards/enforcement.md` 的 CI 矩阵。
