# `core/units` 模块规范

- **层**：L0（地基）
- **状态**：`contract` → `implemented`（P1 纵切）
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
| 物理常量表（G、g、k_B…） | 见 §6 待决项 |
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

### 2.3 `Unit` —— 运行期单位

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

---

## 4. 单位字符串约定

### 4.1 生成规则

1. 分子按**单位符号字母升序**：`A < K < cd < kg < m < mol < s`
2. 分母同理，符号取绝对值，用 `/` 引入
3. 分母含**多个因子**时加括号：`kg/(A*s^2)`
4. 指数 1 省略 `^1`；无量纲输出 `"1"`

### 4.2 这不是"标准"，是**本项目的约定**

现行规范（BIPM SI 手册、[NIST SP 811](https://www.nist.gov/pml/special-publication-811)）
对"导出单位用基本单位表达时的书写顺序"**没有强制规定**，只要求同一文本内自洽。
因此本项目的做法是**选定一种确定性顺序并写进 ABI 版本**，而不是声称遵循某条并不存在的强制条款。

不影响物理正确性：量纲信息在 `Dim` 类型里，与字符串无关。

若将来需要严格照抄某个标准符号（如 NIST 的 `kg·m²/(A·s³)`），
应**新增符号表并升 `kUnitsAbiVersion`**，而不是偷偷改顺序。

### 4.3 黄金值（`tests/golden/units/test_unit_symbol.cpp`）

| 量 | 字符串 |
|---|---|
| 力 | `kg*m/s^2` |
| 能量 | `kg*m^2/s^2` |
| 压强 | `kg/(m*s^2)` |
| 频率 | `1/s` |
| 电压 | `kg*m^2/(A*s^3)` |
| 磁感应强度 | `kg/(A*s^2)` |
| 无量纲 | `1` |

---

## 5. 双编译器要求

**在 GCC 上通过 ≠ 可移植。**（P1 的实测结论，见 ADR-0004）

本模块必须在以下编译器上同时构建通过：

| 编译器 | 实测版本 | 结果 |
|---|---|---|
| GCC (MinGW-w64) | 15.2.0 | 通过 |
| MSVC | 19.51（VS 18 Insiders） | 通过 |

---

## 6. 待决项（未解决，明确记录）

| # | 问题 | 现状 | 影响 |
|---|---|---|---|
| U1 | **平面角的量纲** | L0 视为无量纲（rad = 1，与 SI 一致），故 `dims::angular_velocity == dims::frequency`（同量纲不同义） | 振动学节点若同时用 Hz 与 rad/s，类型系统无法区分；需在端口层用**语义标签**而非量纲来区分 |
| U2 | **同量纲不同义的别名** | `Energy`/`Work`/`Torque` 可直接相加（已测，见 `units.quantity.no_cross_dim_add`） | 力矩加能量的错误无法被类型系统捕获；这是量纲系统的**已知极限**，不假装解决 |
| U3 | **指数越界的行为** | `detail::add_exp` 返回哨兵值 `kMinExp-1`，仅在调用点 `constexpr` 检查时失败 | 运行期组合（如 `pow<N>` 的 N 来自模板参数链）可能静默溢出；P3 接入 `abi/` 时需补一条编译期断言路径 |
| U4 | **物理常量归属** | 未定义任何常量（无 `constants.hpp`） | 有 `constants.hpp` 的需求（g、G、k_B）；归属待定：`units` 会给地基增接口面，独立 `core/constants` 更符合"地基最小" |
| U5 | `sqrt_unchecked` 的奇偶检查 | 不做检查，靠调用点断言 | 量纲指数为奇数时结果无意义；P3 需要时改为 `sqrt<D>` 带编译期断言 |
| U6 | **`Quantity` 的数值类型** | 固定 `double` | float32 在场数据（19MB/张）里更省带宽；是否需要 `Quantity<D, T>` 泛型化待 P2 `abi` 定案 |

---

## 7. 验证矩阵（当前状态）

| 类别 | 内容 | 位置 | 状态 |
|---|---|---|---|
| 1 单元 | `Dim` 代数、`Quantity` 算术、字面量换算 | `tests/unit/units/` | 已写 |
| 2 静态契约 | 尺寸/对齐/平凡可复制/`kUnitsAbiVersion` | `tests/unit/units/test_dim.cpp` | 已写 |
| 3 性质 | 交换律、结合律、单位元、幂复合 | 同上（`[property]` tag） | 已写（固定样本集）|
| 4 黄金回归 | 单位字符串逐字对照 | `tests/golden/units/` | 已写 |
| 5 确定性 | — | — | 不适用（本模块无随机性）|
| 6 性能 | — | — | 不适用（全 constexpr）|
| 7 模糊 | — | — | 不适用 |
| 8 契约合规套件 | — | — | 不适用（本模块不定义插件接口）|
| — 门禁自检 | 四类违规必被抓到 | `tests/meta/` | 已通过 |
| — 编译矩阵 | GCC 15.2 + MSVC 19.51 | CI | 双绿 |

**规格缺口（必须在 P2 前补上）**：

1. 性质测试目前用**固定样本集**而非 RapidCheck。
   `test-taxonomy.md` §4 要求随机生成；接入 RapidCheck 后**用例 id 不变**
   （契约承诺的是性质，不是实现方式）。
2. **Catch2 尚未接入**：CMake 图与门禁已通过 CTest 验证，
   但 `qp_test_*` 目标未在真实 Catch2 上构建过。装好 Catch2 v3 后首次构建即为 P1 的收尾动作。

---

## 8. 达成 P1 的判据

| 判据 | 证据 |
|---|---|
| 类型契约冻结 | 本文件 §2 |
| 头文件契约完整 | `scripts/check_contracts.py` 通过（31 个契约，0 违规，0 孤儿）|
| 门禁可执行 | `ctest -L gate` 双绿；`tests/meta/` 证明四类违规必被抓到 |
| 跨编译器 | GCC 15.2 与 MSVC 19.51 双绿 |
| 单位字符串唯一来源 | 黄金测试逐字对照通过 |
| 规范本身可用 | 本文件 + `functions in headers` 的组合证明"①类型→②签名→③函数契约→④实现→⑤测试"可执行 |

**P1 的结论**：这套规范可执行。P2 起按同一模板复制到 `abi` / `ports`。
