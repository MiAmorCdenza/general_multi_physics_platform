/**
 * @file field_buffer.hpp
 * @brief 跨边界传递大块场数据的描述符（不是数据本身）。
 *
 * ## 为什么是"描述符"而不是"容器"
 *
 * 一张场 ≈ 19MB。任何把它塞进容器的做法都意味着拷贝：
 * `QByteArray`、`std::vector`、Python `bytes` 都会复制一次。
 * 在 60fps 下那是 1.1GB/s 的额外流量。
 *
 * 因此 `FieldBuffer` 只描述"数据在哪、多大、什么形状"，
 * **不拥有也不复制**数据。所有权由发布方保证（见 §data 的生命周期）。
 *
 * ## 为什么用 seqlock 而不是互斥量
 *
 * 读者（渲染线程、分析线程）远多于写者（烘焙线程）。
 * 互斥量会让每个读者阻塞写者；seqlock 让读者**无锁**且**不阻塞写者**：
 *   - 读者读两遍 seq，若读前读后相同且为偶数，则拿到一致数据；
 *   - 不一致就重试（最坏情况是重试，不会撕裂数据）。
 *
 * ## 与 `qp::units` 的关系：**故意零依赖**
 *
 * 本结构只使用 C 子集（定长整数 + 原子）。外部语言绑定（Python / MATLAB）
 * 读的是一份逐字节对应的布局说明，不是 C++ 模板库。
 * 量纲用 `FieldDim` 自带表示，与 `qp::units::Dim` 的一致性由测试断言守住。
 *
 * @frozen 是——本结构是 ABI 契约，布局变更必须升 kFieldBufferLayout 与 kAbiMajor。
 */
#pragma once

#include <qp/abi/abi_version.hpp>
#include <qp/abi/lattice.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qp::abi {

/// @brief 结构体魔数。用于识别"这块内存到底是不是 FieldBuffer"。
/// 值本身没有含义，只是足够不可能与随机内存相同。
inline constexpr std::uint32_t kFieldBufferMagic = 0x51'50'46'42U;   // 'QPFB'

/// @brief 数据有效标志的位定义。
enum class BufferFlags : std::uint32_t {
    none = 0,
    /// 数据已完整写入，可读。
    valid = 1U << 0,
    /// 该 slot 已被淘汰（内存可能仍被引用，但内容无意义）。
    tombstone = 1U << 1,
    /// 数据来自磁盘缓存而非本次计算。诊断用。
    from_cache = 1U << 2,
};

[[nodiscard]] constexpr std::uint32_t operator|(BufferFlags a, BufferFlags b) noexcept {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
[[nodiscard]] constexpr bool has_flag(std::uint32_t flags, BufferFlags f) noexcept {
    return (flags & static_cast<std::uint32_t>(f)) != 0;
}

/**
 * @brief 场数据的跨边界描述符。
 *
 * 布局（小端；`P` = 指针大小）：
 * ```
 * 偏移        长度  字段
 *   0          32   lattice
 *  32           4   magic
 *  36           2   layout
 *  38           2   abi_major
 *  40           4   writer_seq   （原子）
 *  44           4   flags        （原子）
 *  48           P   data
 * 48+P        8-Δ   data_bytes   （Δ = 8 字节对齐所需的填充）
 * 56+P        8-Δ   capacity_bytes
 * 64+P        …     reserved[8]  （撑到 8 字节整体对齐）
 * ```
 * 64 位（P=8）：`sizeof == 80`，`alignof == 8`
 * 32 位（P=4）：`sizeof == 88`，`alignof == 8`
 *
 * **不写死这些数字**——它们由 `tests/abi/` 的 offsetof 断言守护。
 * 注意 32 位下 `uint64` 字段仍需 8 字节对齐，因此 `data` 之后有填充。
 *
 * ### 数据生命周期（`data` 指向的内存谁负责）
 *
 * 模型是**发布方保持存活**：
 *   - 发布方（烘焙线程 / 缓存）保证只要还有读者持有本描述符的副本，
 *     `data` 就有效；
 *   - 读者**不得**释放、重分配或写入 `data`；
 *   - 淘汰一个 slot 时先置 `tombstone` 标志，等所有读者释放副本后再回收。
 *
 * 这个模型故意不用 `shared_ptr`：`FieldBuffer` 要能逐字节复制到
 * 一份语言中立的布局说明里，而 `shared_ptr` 是 C++ 运行时概念。
 * 引用计数的责任在**外层**（`core/abi` 之外的拥有者），不在本结构里。
 *
 * @ownership   observes（不拥有 data 指向的内存）
 * @thread      写者单线程；读者任意多线程（seqlock 保证）
 * @pre         写者发布前必须填好 lattice/magic/layout/abi_major/data/data_bytes
 * @post        none
 * @invariant   flags 含 valid 时，data 指向至少 data_bytes 字节的有效内存
 * @errors      noexcept（本结构不抛；分配由外层负责）
 * @frozen      是
 * @tests       abi.field_buffer.size_and_alignment, abi.field_buffer.field_offsets,
 *              abi.field_buffer.magic_constant, abi.field_buffer.flags_are_bitwise,
 *              abi.field_buffer.trivially_copyable
 */
struct FieldBuffer final {
    LatticeDesc lattice{};                    ///< 数据形状（32 字节）

    std::uint32_t magic = kFieldBufferMagic;  ///< 识别用魔数
    std::uint16_t layout = kFieldBufferLayout;///< 布局版本
    std::uint16_t abi_major = kAbiMajor;      ///< 主版本

    std::atomic<std::uint32_t> writer_seq{0}; ///< seqlock 序号：奇数 = 写入中
    std::atomic<std::uint32_t> flags{0};      ///< BufferFlags 位组合

    const void* data = nullptr;               ///< 指向实际数据（float32 数组等）
    std::uint64_t data_bytes = 0;             ///< 有效字节数
    std::uint64_t capacity_bytes = 0;         ///< data 实际可容纳的字节数

    std::uint64_t reserved[8 / sizeof(std::uint64_t)] = {};

    // ── 特化成员 ────────────────────────────────────────────────────────────
    //
    // `std::atomic` 不可复制，因此必须显式提供拷贝语义，否则
    // `FieldBuffer` 会变成只可移动——而"描述符按值传递"是本结构的设计前提。
    //
    // 拷贝的是**当前观察值**，语义上是"再拿一个句柄指向同一份数据"。
    // seqlock 的活跃状态始终在**原先那个对象**里；副本上的 seq 快照
    // 唯一用途是交给 read_end 校验。契约里已写明本结构是 observes（不拥有数据）。

    FieldBuffer() noexcept = default;

    FieldBuffer(const FieldBuffer& other) noexcept
        : lattice(other.lattice),
          magic(other.magic),
          layout(other.layout),
          abi_major(other.abi_major),
          writer_seq(other.writer_seq.load(std::memory_order_acquire)),
          flags(other.flags.load(std::memory_order_acquire)),
          data(other.data),
          data_bytes(other.data_bytes),
          capacity_bytes(other.capacity_bytes) {}

    FieldBuffer& operator=(const FieldBuffer& other) noexcept {
        if (this != &other) {
            lattice = other.lattice;
            magic = other.magic;
            layout = other.layout;
            abi_major = other.abi_major;
            writer_seq.store(other.writer_seq.load(std::memory_order_acquire),
                             std::memory_order_release);
            flags.store(other.flags.load(std::memory_order_acquire), std::memory_order_release);
            data = other.data;
            data_bytes = other.data_bytes;
            capacity_bytes = other.capacity_bytes;
        }
        return *this;
    }

    FieldBuffer(FieldBuffer&& other) noexcept : FieldBuffer(static_cast<const FieldBuffer&>(other)) {}

    FieldBuffer& operator=(FieldBuffer&& other) noexcept {
        return operator=(static_cast<const FieldBuffer&>(other));
    }

    ~FieldBuffer() = default;
};

/// @brief 结构体是否为 8 字节对齐（指针 + 原子字段的自然对齐）。
inline constexpr std::size_t kFieldBufferAlign = alignof(FieldBuffer);

/**
 * @brief 检查描述符自身是否自洽（不含数据内容校验）。
 *
 * 用途：宿主在采用任何外部（插件、子进程、脚本）传入的 `FieldBuffer` 前
 * **必须**先调用它。这是抵御"插件给了个野指针"的第一道闸。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        逐项检查 magic / layout / abi_major / 指针非空 / 容量不越界
 * @invariant   返回 true 的描述符不会导致越界读取（在 data 确实有效的前提下）
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.field_buffer.validate_ok, abi.field_buffer.validate_rejects_bad_magic,
 *              abi.field_buffer.validate_rejects_layout_mismatch,
 *              abi.field_buffer.validate_rejects_oversized_data
 */
[[nodiscard]] inline bool validate(const FieldBuffer& b) noexcept {
    if (b.magic != kFieldBufferMagic) return false;
    if (b.layout != kFieldBufferLayout) return false;
    if (b.abi_major != kAbiMajor) return false;
    if (b.data == nullptr) return false;
    // data_bytes 不得超过 lattice 声明的需求，也不得超过容量
    if (b.data_bytes > b.capacity_bytes) return false;
    const std::uint64_t needed = data_bytes(b.lattice);
    if (needed != 0 && b.data_bytes < needed) return false;
    return true;
}

/// @brief 数据是否已就绪可读。
[[nodiscard]] inline bool is_readable(const FieldBuffer& b) noexcept {
    return has_flag(b.flags.load(std::memory_order_acquire), BufferFlags::valid);
}

/**
 * @brief 发布者：进入写入状态（seqlock 的写侧协议，第一步）。
 *
 * 协议：
 * ```
 *   seq = begin_write(buf);          // 变奇数，读者开始重试
 *   ... 写入 lattice / data 内容 ...
 *   end_write(buf, seq, flags);      // 变偶数，读者可读到一致数据
 * ```
 *
 * @ownership   borrows
 * @thread      publish
 * @pre         buf 未被其他线程写入中
 * @post        writer_seq 变为奇数
 * @invariant   同一线程的 begin/end 必须成对
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
[[nodiscard]] inline std::uint32_t begin_write(FieldBuffer& buf) noexcept {
    return buf.writer_seq.fetch_add(1, std::memory_order_acq_rel) + 1U;
}

/**
 * @brief 发布者：结束写入并设置标志。
 *
 * @ownership   borrows
 * @thread      publish
 * @pre         seq 来自配对的 begin_write
 * @post        writer_seq 变为偶数且大于 seq
 * @invariant   写入开始后必定结束
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
inline void end_write(FieldBuffer& buf, std::uint32_t seq, std::uint32_t flag_bits) noexcept {
    buf.flags.store(flag_bits, std::memory_order_release);
    buf.writer_seq.store(seq + 1U, std::memory_order_release);
}

/**
 * @brief 读者：读取一个一致快照的序号。
 *
 * 用法：
 * ```
 *   retry:
 *     seq = read_begin(buf);
 *     if (seq & 1) goto retry;        // 正在写
 *     ... 读取 data ...
 *     if (!read_end(buf, seq)) goto retry;
 * ```
 *
 * @ownership   observes
 * @thread      any
 * @pre         none
 * @post        返回当前序号（可能为奇数，表示正在写入）
 * @invariant   返回值单调不减
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
[[nodiscard]] inline std::uint32_t read_begin(const FieldBuffer& buf) noexcept {
    return buf.writer_seq.load(std::memory_order_acquire);
}

/**
 * @brief 读者：确认快照仍然一致。
 *
 * @ownership   observes
 * @thread      any
 * @pre         seq 来自配对的 read_begin
 * @post        返回 true 表示 seq 为偶数且未被写者改动过
 * @invariant   返回 true 时，期间读到的数据是一致快照
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
[[nodiscard]] inline bool read_end(const FieldBuffer& buf, std::uint32_t seq) noexcept {
    if ((seq & 1U) != 0U) return false;
    return buf.writer_seq.load(std::memory_order_acquire) == seq;
}

/// @brief 数据首地址，按 `T` 解释。**不做任何类型检查**——调用方负责。
template <class T>
[[nodiscard]] inline const T* data_as(const FieldBuffer& b) noexcept {
    return static_cast<const T*>(b.data);
}

}  // namespace qp::abi
