# ABI 布局说明书（语言中立）

> **本文件是给外部语言绑定看的。** Python / MATLAB / 未来 Web 的适配层
> 不应编译 C++ 头文件，而应读这份逐字节说明，并用 `tests/abi/` 里的断言
> 作为"说明书没漂移"的证据。

- ABI 主版本：`1`
- `FieldBuffer` 布局版本：`1`
- `LatticeDesc` 布局版本：`1`
- 字节序：**仅小端**（大端平台会编译失败，见 `byte_order.hpp`）

---

## 1. `FieldDim` —— 7 字节量纲

| 偏移 | 类型 | 字段 | 含义 |
|---|---|---|---|
| 0 | int8 | `L` | 长度（米） |
| 1 | int8 | `M` | 质量（千克） |
| 2 | int8 | `T` | 时间（秒） |
| 3 | int8 | `I` | 电流（安培） |
| 4 | int8 | `Th` | 温度（开尔文） |
| 5 | int8 | `N` | 物质的量（摩尔） |
| 6 | int8 | `J` | 发光强度（坎德拉） |

`sizeof == 7`，`alignof == 1`。全零即无量纲。

**单位字符串不进 ABI。** 需要显示时由 `unit_symbol` 现算，
保证"同一量纲 → 同一字符串"。

---

## 2. `LatticeDesc` —— 32 字节（alignof 4）

| 偏移 | 类型 | 字段 | 取值 |
|---|---|---|---|
| 0 | FieldDim | `dimension` | 见 §1 |
| 7 | uint8 | `component` | 0=scalar, 1=vector |
| 8 | uint8 | `element` | 0=f32, 1=f64 |
| 9 | uint8 | `kind` | 0=point, 1=line, 2=plane, 3=volume |
| 10 | uint8 | `padding` | **必须为 0** |
| 12 | uint32 | `count[0]` | 第 0 维采样点数 |
| 16 | uint32 | `count[1]` | 第 1 维 |
| 20 | uint32 | `count[2]` | 第 2 维 |
| 24 | uint32 | `spacing_bytes` | 每点字节数 = 分量数 × 元素大小 |
| 28 | uint32 | `reserved` | **必须为 0** |

派生量：

```
point_count     = kind==point ? 1 : kind==line ? c0 : kind==plane ? c0*c1 : c0*c1*c2
component_count = component==vector ? 3 : 1
element_size    = element==f64 ? 8 : 4
data_bytes      = point_count * component_count * element_size
```

---

## 3. `FieldBuffer` —— 描述符（不拥有数据）

### 3.1 实测布局（GCC 15.2 i686 与 MSVC 19.51 x64 均为 `sizeof == 80`，`alignof == 8`）

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | LatticeDesc | `lattice` |
| 32 | uint32 | `magic`（必须 = `0x51504642`，即 'QPFB'）|
| 36 | uint16 | `layout`（必须 = 1）|
| 38 | uint16 | `abi_major`（必须 = 1）|
| 40 | uint32 | `writer_seq`（原子）|
| 44 | uint32 | `flags`（原子）|
| 48 | pointer | `data` |
| 56 | uint64 | `data_bytes` |
| 64 | uint64 | `capacity_bytes` |
| 72 | uint64[8/sizeof(uint64_t)] | `reserved`（总字节数恒为 8）|

> **32 位下 `data` 之后有 4 字节填充**：`uint64` 字段仍需 8 字节对齐，
> 因此 `data_bytes` 在 56 而不是 52。这让 32 位与 64 位得到**相同**的偏移，
> 外部绑定只需一份表。填充是刻意的，不是浪费。
>
> `reserved` 的元素个数随平台不同（64 位 1 个、32 位 2 个），
> 但**总字节数恒为 8**。它的作用是保证结构体尾部对齐到 8 字节。

### 3.2 `flags` 位定义

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | `valid` | 数据完整可读 |
| 1 | `tombstone` | 已淘汰，内容无意义 |
| 2 | `from_cache` | 来自磁盘缓存而非本次计算 |

### 3.4 数据生命周期
**发布方保持存活**模型：

- 发布方保证：只要还有读者持有描述符副本，`data` 就有效。
- 读者**不得**释放、重分配或写入 `data`。
- 淘汰 slot 时先置 `tombstone`，等所有读者释放副本后再回收。

引用计数在**外层**（拥有者），不在 `FieldBuffer` 里——
因为 `shared_ptr` 是 C++ 运行时概念，无法写进语言中立的布局说明。

### 3.5 seqlock 协议

```
写者:  seq = ++writer_seq          (奇数 = 写入中)
       ... 写 lattice / data ...
       flags = 新值
       writer_seq = seq + 1        (偶数 = 稳定)

读者:  seq = writer_seq            (acquire)
       若 seq 为奇数 → 重试
       ... 读 data ...
       若 writer_seq != seq → 重试
       否则本次读到的是一致快照
```

读者**无锁**且**不阻塞写者**。最坏情况是重试，不会撕裂数据。

---

## 4. 版本兼容判定

```
若 layout != host_layout         → layout_mismatch   （拒绝）
若 plugin.major > host.major     → host_too_old      （拒绝）
若 plugin.major < host.major     → plugin_too_old    （拒绝）
若 plugin.minor > host.minor     → plugin_too_new    （拒绝）
否则                              → compatible
```

**刻意不做**语义化版本的"向后兼容"推断：在 ABI 层面，
"我以为它兼容"与"它确实兼容"的差别就是数据损坏。

---

## 5. 布局断言的权威位置

本文档中的所有偏移与尺寸都由 `tests/abi/test_abi_layout.cpp` 的
`static_assert` 守护。改布局会**先**让断言失败，强迫作者面对兼容性问题。

若本文档与断言不一致，**以断言为准**，并立即修正本文档。
