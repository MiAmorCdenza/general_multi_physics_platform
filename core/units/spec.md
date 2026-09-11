# `core/units` 模块规范

- **层**：L0（地基）
- **状态**：`implemented` → `verified`（P1 纵切完成）
- **依赖**：无（最底层，见 `plan-tree.md` §8 铁律 4）
- **ABI 版本**：`kUnitsAbiVersion = 1`
- **函数契约**：见 `include/qp/units/*.hpp` 的文档块（唯一真相源，由 `scripts/check_contracts.py` 强制）

---

## 1. 职责与边界

### 做什么

| 职责 | 说明 |
|---|---|
| 编译期量纲表示 | 七个 SI 基本量纲的整数指数，作为**类型的一部分** |
| 量纲代数 | 乘除幂的指数运算，编译期完成 |
| 带量纲数值 | `Quantity<D>`：同量纲可运算，跨量纲编译失败 |
| 单位字符串生成 | 由编译期类型**自动推导**，禁止手写字面量 |
| 运行期单位 | `Unit`：量纲 + 换算因子，供端口标注与 YAML 使用 |

### 不做什么（明确划出边界）

| 不做 | 归谁 |
|---|---|
| 单位换算表（英制、市制、历史单位） | 插件 |
| 量纲分析的业务规则（如"这个公式对不对"） | `core/graph/validate` |
| 物理常量表（G、g、k_B…） | 见 §6 待决项 U4 |
| 数值格式化与显示精度 | 视图层 |
| 端口的 UI 控件 | `core/authoring/portui` |

**判据**：本模块只回答"这个量是什么量纲"，不回答"这个量该显示成什么样"。

---

## 2. 类型契约（已冻结）

### 2.1 `Dim` —— 量纲向量

```cpp
struct Dim final {
    DimExp L = 0, M = 0, T = 0, I = 0, Th = 0, N = 0, J = 0;
};
using DimExp = std::int8_t;
```

| 属性 | 值 | 含义 |
|---|---|---|
| 尺寸 | 7 字节 | `static_assert(sizeof(Dim) == 7)` |
| 对齐 | 1 | 无填充，可 memcpy |
| 平凡可复制 | 是 | 可跨 ABI 边界 |
| 成员顺序 | **L, M, T, I, Th, N, J** | 与 SI 基本单位 m kg s A K mol cd 一一对应 |

**冻结理由**：`Dim` 会出现在跨进程 / 跨语言的 `FieldBuffer` 元数据里。
改成员顺序或尺寸必须升 `kUnitsAbiVersion`。

**指数范围**：`[kMinExp, kMaxExp] = [-100, 100]`。越界由 `detail::add_exp` 捕获，
结果为 `kMinExp - 1`，由调用点断言。选 int8 而非 int16 是为了让 `Dim` 保持 7 字节。

### 2.2 `Quantity<D>` —— 带量纲标量

```cpp
template <Dim D>
class Quantity final {
public:
    using value_type = double;
    static constexpr Dim dim = D;
    explicit constexpr Quantity(double v) noexcept;
    [[nodiscard]] constexpr double value() const noexcept;
    constexpr void set(double v) noexcept;
    [[nodiscard]] bool is_finite() const noexcept;
};
```

两条**不可协商**的设计决定：

1. **没有到标量的隐式转换**。拿裸数值必须显式 `.value()`——
   这一步是"我在放弃量纲保护"的确认动作。
2. **构造函数是 explicit**，`Length x = 3.0;` 必须编译失败。

数值语义：`value()` 恒为"以 SI 基本单位表示的值"。

**数值类型固定 `double`**（U6 已决策，见 ADR-0005）：
float32 只活在 `FieldBuffer`/`abi` 层，在端口边界做唯一一次拓宽。

### 2.3 无量纲判据（P1 修正）

`Dim::is_dimensionless()` 定义为**七个指数全为 0**。

早期实现写成 `sum() == 0`，这是**物理错误**：速度 `L/T` 的指数和为 0，
会被误判为无量纲。现拆成两个谓词：

| 谓词 | 含义 | 用途 |
|---|---|---|
| `is_dimensionless()` | 全部指数为 0 | 量纲判据 |
| `has_zero_exponent_sum()` | 指数和为零 | 诊断信息（"指数相互抵消了"） |

单向蕴含：`is_dimensionless() ⟹ has_zero_exponent_sum()`，反之不成立
（力 `kg*m/s^2` 的指数和也是 0）。已由 `units.dim.zero_exponent_sum` 断言。

### 2.4 `Unit` —— 运行期单位

```cpp
struct Unit final {
    Dim dim{};
    double factor = 1.0;
    std::string symbol{};
};
```

用于端口标注、YAML 的 `unit:` 字段、仪器读数展示。
`symbol` 必须由 `unit_symbol(dim)` 生成，**不允许手写**（否则三处会漂移）。

---

## 3. 运算符接口

| 形式 | 结果 | 量纲规则 |
|---|---|---|
| `q1 + q2`、`q1 - q2` | `Quantity<D>` | 仅同量纲（跨量纲编译失败） |
| `-q` | `Quantity<D>` | 不变 |
| `q1 * q2` | `Quantity<L * R>` | 指数相加 |
| `q1 / q2` | `Quantity<L / R>` | 指数相减 |
| `q * s`、`s * q`、`q / s` | `Quantity<D>` | 标量不改变量纲 |
| `pow<P>(q)` | `Quantity<dim_pow<P>(D)>` | 指数乘 P |
| `q1 == q2` 等比较 | `bool` | 仅同量纲 |

### 3.1 模板参数一律写 `auto`（ADR-0004）

```cpp
template <auto L, auto R>
constexpr Quantity<L * R> operator*(Quantity<L> a, Quantity<R> b) noexcept;
```

**不要写成 `template <Dim L, Dim R>`**。MSVC 无法从实参推导类类型的非类型模板参数，
会直接丢弃整个重载（GCC 可以，所以这个缺陷只会在 Windows 上暴露）。
详见 `docs/adr/ADR-0004`。

### 3.2 `pow` 用平方求幂，不用 `std::pow`

- `std::pow` 在 C++20 不是 constexpr（GCC 15 可，MSVC 不可），会破坏编译期可用性。
- `detail::int_pow` 用重复乘法：结果与"逐个相乘"一致到 ULP 级，且求值顺序固定。
- **但不要要求与连乘逐位相等**：平方求幂与连乘是不同的求值顺序，
  浮点乘法不满足结合律，差几 ULP 是正确行为。见 `standards/test-taxonomy.md` §5.1。

---

## 4. 单位字符串约定

### 4.1 生成规则

1. 分子按**单位符号字母升序**：`A < K < cd < kg < m < mol < s`
2. 分母同理，符号取绝对值，用 `/` 引入
3. 分母含**多个因子**时加括号：`kg/(m*s^2)`
4. 指数 1 省略 `^1`；无量纲输出 `"1"`

### 4.2 这不是"标准"，是**本项目的约定**

现行规范（BIPM SI 手册、[NIST SP 811](https://www.nist.gov/pml/special-publication-811)）
对"导出单位用基本单位表达时的书写顺序"**没有强制规定**，只要求同一文本内自洽。
因此本项目的做法是**选定一种确定性顺序并写进 ABI 版本**，而不是声称遵循某条并不存在的强制条款。

不影响物理正确性：量纲信息在 `Dim` 类型里，与字符串无关。

若将来需要严格照抄某个标准符号（如 NIST 的 `kg·m²/(A·s³)`），
应**新增符号表并升 `kUnitsAbiVersion`**，而不是偷偷改顺序。

### 4.3 黄金值（`tests/golden/units/test_unit_symbol.cpp`）

| 量 | 字符串 | 分母因子数 |
|---|---|---|
| 力 | `kg*m/s^2` | 1 |
| 能量 | `kg*m^2/s^2` | 1 |
| 压强 | `kg/(m*s^2)` | 2 → 有括号 |
| 频率 | `1/s` | 1（分子为空） |
| 电压 | `kg*m^2/(A*s^3)` | 2 → 有括号 |
| 磁感应强度 | `kg/(A*s^2)` | 2 → 有括号 |
| 无量纲 | `1` | 0 |

**已知实现陷阱**：分母因子数必须按**负指数**统计。
早期版本用"取负之后为正"作判据，等于把分子也数了进去，
于是 `kg/m*s^2`（有歧义）被输出——由黄金测试抓出后修正为
`detail::count_factors(d, /*denominator=*/true)`。

### 4.4 实现细节不构成契约面

`detail::symbol_order_exponents` / `count_factors` / `needs_parentheses`
是**实现细节**，不写函数契约、不单独点名测试。
它们的行为通过公开的 `unit_symbol()` 在黄金测试与性质测试中被完整覆盖。

曾经为这三个内部函数单独写契约与测试，这是**测试实现而非行为**，已改回。
判据：**公开的才能被契约绑定**。

---

## 5. 双编译器要求

**在 GCC 上通过 ≠ 可移植。**（P1 的实测结论，见 ADR-0004）

| 编译器 | 实测版本 | 目标 | 结果 |
|---|---|---|---|
| GCC (MinGW-w64) | 15.2.0 | i686（32 位） | 70/70 通过 |
| MSVC | 19.51（VS 18 Insiders） | x64 | 70/70 通过 |

注意两者的**架构不同**（32 位 vs 64 位），因此浮点求值环境也不同
（`FLT_EVAL_METHOD` 2 vs 0）。这正是 `test_floating_point_env.cpp` 存在的原因。

---

## 6. 待决项

| # | 问题 | 现状 | 影响 |
|---|---|---|---|
| U1 | **平面角的量纲** | L0 视为无量纲（rad = 1，与 SI 一致），故 `dims::angular_velocity == dims::frequency`（同量纲不同义） | 振动学节点若同时用 Hz 与 rad/s，类型系统无法区分；需在端口层用**语义标签**而非量纲来区分 |
| U2 | **同量纲不同义的别名** | `Energy`/`Work`/`Torque` 可直接相加（已测，见 `units.quantity.no_cross_dim_add`） | 力矩加能量的错误无法被类型系统捕获；这是量纲系统的**已知极限**，不假装解决 |
| U3 | **指数越界的行为** | `detail::add_exp` 返回哨兵值 `kMinExp-1`，仅在调用点 `constexpr` 检查时失败 | 运行期组合（如 `pow<N>` 的 N 来自模板参数链）可能静默溢出；P3 接入 `abi/` 时需补一条编译期断言路径 |
| U4 | **物理常量归属** | 未定义任何常量（无 `constants.hpp`） | 有 `constants.hpp` 的需求（g、G、k_B）；归属待定：`units` 会给地基增接口面，独立 `core/constants` 更符合"地基最小" |
| U5 | `sqrt_unchecked` 的奇偶检查 | 不做检查，靠调用点断言 | 量纲指数为奇数时结果无意义；P3 需要时改为 `sqrt<D>` 带编译期断言 |
| ~~U6~~ | ~~`Quantity` 的数值类型~~ | **已决策**：固定 `double`。float32 只活在 `FieldBuffer`/`abi` 层，在端口边界做唯一一次拓宽 | 见 ADR-0005 |
| U7 | **32 位工具链的浮点非确定性** | `FLT_EVAL_METHOD == 2` 使结果依赖求值顺序与优化级别 | "逐位复现"必须以固定工具链 + 优化级别为前提；运行身份须记录这两项。建议 P2 起评估切到 x64 工具链 |

---

## 7. 验证矩阵（当前状态）

| 类别 | 内容 | 位置 | 状态 |
|---|---|---|---|
| 1 单元 | `Dim` 代数、`Quantity` 算术、字面量换算 | `tests/unit/units/` | [OK] 已运行 |
| 2 静态契约 | 尺寸/对齐/平凡可复制/`kUnitsAbiVersion` | `test_dim.cpp` | [OK] |
| 3 性质 | 交换律、结合律、单位元、幂复合、确定性 | 各文件 `[property]` tag | [OK] 固定样本集（RapidCheck 待接入）|
| 4 黄金回归 | 单位字符串逐字对照 + 工具链前提 | `tests/golden/units/`、`test_floating_point_env.cpp` | [OK] |
| 5 确定性 | `pow` 同输入同输出 | `test_quantity.cpp` | [OK] |
| 6 性能 | — | — | 不适用（全 constexpr）|
| 7 模糊 | — | — | 不适用 |
| 8 契约合规套件 | — | — | 不适用（本模块不定义插件接口）|
| 门禁 | 契约完备性 + 门禁自检 | `scripts/check_contracts.py`、`tests/meta/` | [OK] |
| 编译矩阵 | GCC 15.2（i686）+ MSVC 19.51（x64） | 本地 | [OK] 双 70/70 |

**统计**：62 个 `TEST_CASE`、1326 条断言、CTest 注册 70 项（含 8 个聚合项）。

**剩余规格缺口**：

1. 性质测试目前用**固定样本集**而非 RapidCheck。
   接入后**用例 id 不变**（契约承诺的是性质，不是实现方式）。
2. `scripts/check_layers.py`（层级依赖门禁）**尚未实现**。
   core 目前只有 units 一个模块，还没有可越界的依赖；P2 引入第二个模块时必须先补上。

---

## 8. 达成 P1 的判据

| 判据 | 证据 |
|---|---|
| 类型契约冻结 | 本文件 §2 |
| 头文件契约完整 | 契约门禁通过（35 个契约，0 违规，0 孤儿）|
| 门禁可执行且自身受测 | `tests/meta/` 证明 C2–C5 四类违规必被抓到 |
| 跨编译器 | GCC 15.2 与 MSVC 19.51 双 70/70 |
| 单位字符串唯一来源 | 黄金测试逐字对照通过 |
| 规范本身可用 | 本文件 + 头文件内契约证明「(1)类型→(2)签名→(3)函数契约→(4)实现→(5)测试」可执行 |

### P1 期间被测试抓出的真实缺陷（8 个）

| # | 缺陷 | 抓出者 | 后果 |
|---|---|---|---|
| 1 | 非模板 friend 运算符无法推导量纲参数 | 首次编译 | 头文件无法编译 |
| 2 | MSVC 不能推导类类型 NTTP，整个重载被丢弃 | MSVC 构建 | Windows 路径全线崩（ADR-0004） |
| 3 | `pow` 用非 constexpr 的 `std::pow` | 编译期用例 | 无法写编译期常量表达式 |
| 4 | 单位符号分母漏括号（`kg/m*s^2`） | **黄金测试** | 符号有歧义 |
| 5 | 单位符号因子顺序未定义 | 黄金测试 | 与 SI 习惯不符 |
| 6 | `is_dimensionless()` 用 `sum()==0` | 编译期用例 | 物理错误：速度被判为无量纲 |
| 7 | 字面量收窄警告（`long double` → `double`） | `-Wconversion` | 精度隐患，已收成唯一收窄点 |
| 8 | 契约门禁落进函数体，误把语句当声明 | 门禁自身 | 假契约、假豁免；已用花括号深度过滤修复 |

**结论**：这套规范**可执行**，而且它在第一个模块上就抓到了 8 个真实缺陷。
P2 起按同一模板复制到 `abi` / `ports`。
