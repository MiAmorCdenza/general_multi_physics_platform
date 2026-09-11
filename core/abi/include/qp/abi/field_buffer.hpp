/**
 * @file field_buffer.hpp
 * @brief Descriptor that carries large field data across a boundary (not the data itself).
 *
 * ## Why a descriptor and not a container
 *
 * One field is about 19MB. Any attempt to stuff it into a container means a copy:
 * `QByteArray`, `std::vector`, Python `bytes` all copy it once.
 * At 60fps that is 1.1GB/s of extra traffic.
 *
 * Therefore `FieldBuffer` only describes "where the data is, how big, what shape",
 * and **neither owns nor copies** the data. Ownership is guaranteed by the publisher (see the data lifetime section).
 *
 * ## Why a seqlock and not a mutex
 *
 * Readers (render thread, analysis thread) far outnumber writers (bake thread).
 * A mutex makes every reader block the writer; a seqlock leaves readers **lock-free** and **never blocking the writer**:
 *   - The reader reads seq twice; if both reads match and are even, the data is consistent;
 *   - Otherwise it retries (the worst case is a retry, never torn data).
 *
 * ## Relation to `qp::units`: **deliberately zero dependency**
 *
 * This struct uses only the C subset (fixed-width integers + atomics). Foreign language bindings (Python / MATLAB)
 * read a byte-for-byte layout description, not a C++ template library.
 * Dimensions use `FieldDim`'s own representation; consistency with `qp::units::Dim` is guarded by test assertions.
 *
 * @frozen yes -- this struct is an ABI contract; a layout change must bump kFieldBufferLayout and kAbiMajor.
 */
#pragma once

#include <qp/abi/abi_version.hpp>
#include <qp/abi/lattice.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qp::abi {

/// @brief Struct magic number. Identifies "is this memory really a FieldBuffer".
/// The value itself means nothing; it is just unlikely enough to appear in random memory.
inline constexpr std::uint32_t kFieldBufferMagic = 0x51'50'46'42U;   // 'QPFB'

/// @brief Bit definitions of the data-validity flags.
enum class BufferFlags : std::uint32_t {
    none = 0,
    /// The data has been fully written and can be read.
    valid = 1U << 0,
    /// This slot was evicted (memory may still be referenced, but its contents are meaningless).
    tombstone = 1U << 1,
    /// The data came from the disk cache rather than this computation. For diagnostics.
    from_cache = 1U << 2,
};

[[nodiscard]] constexpr std::uint32_t operator|(BufferFlags a, BufferFlags b) noexcept {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
[[nodiscard]] constexpr bool has_flag(std::uint32_t flags, BufferFlags f) noexcept {
    return (flags & static_cast<std::uint32_t>(f)) != 0;
}

/**
 * @brief Cross-boundary descriptor for field data.
 *
 * Layout (little endian; `P` = pointer size):
 * ```
 * offset   length   field
 *   0          32   lattice
 *  32           4   magic
 *  36           2   layout
 *  38           2   abi_major
 *  40           4   writer_seq   (atomic)
 *  44           4   flags        (atomic)
 *  48           P   data
 * 48+P        8-D   data_bytes   (D = padding needed for 8-byte alignment)
 * 56+P        8-D   capacity_bytes
 * 64+P        ...   reserved[8]  (pads the struct out to 8-byte alignment)
 * ```
 * 64-bit (P=8): `sizeof == 80`, `alignof == 8`
 * 32-bit (P=4): `sizeof == 88`, `alignof == 8`
 *
 * **Do not hard-code these numbers** -- `tests/abi/` guards them with offsetof assertions.
 * Note that on 32-bit the `uint64` fields still need 8-byte alignment, hence the padding after `data`.
 *
 * ### Data lifetime (who is responsible for the memory `data` points at)
 *
 * The model is **the publisher keeps it alive**:
 *   - The publisher (bake thread / cache) guarantees that `data` stays valid as long as
 *     any reader still holds a copy of this descriptor;
 *   - A reader **must not** free, reallocate, or write `data`;
 *   - Evicting a slot sets `tombstone` first; reclaim it once all readers release their copies.
 *
 * This model deliberately avoids `shared_ptr`: `FieldBuffer` must be copyable byte-for-byte into
 * a language-neutral layout description, and `shared_ptr` is a C++ runtime concept.
 * Reference counting is the responsibility of the **outer** layer (the owner outside `core/abi`), not of this struct.
 *
 * @ownership   observes (does not own the memory data points at)
 * @thread      one writer thread; any number of reader threads (guaranteed by the seqlock)
 * @pre         the writer must set lattice/magic/layout/abi_major/data/data_bytes before publishing
 * @post        none
 * @invariant   when flags contains valid, data points at valid memory of at least data_bytes bytes
 * @errors      noexcept (this struct does not throw; allocation is the outer layer's job)
 * @frozen      yes
 * @tests       abi.field_buffer.size_and_alignment, abi.field_buffer.field_offsets,
 *              abi.field_buffer.magic_constant, abi.field_buffer.flags_are_bitwise,
 *              abi.field_buffer.trivially_copyable
 */
struct FieldBuffer final {
    LatticeDesc lattice{};                    ///< Data shape (32 bytes)

    std::uint32_t magic = kFieldBufferMagic;  ///< Magic number for identification
    std::uint16_t layout = kFieldBufferLayout;///< Layout version
    std::uint16_t abi_major = kAbiMajor;      ///< Major version

    std::atomic<std::uint32_t> writer_seq{0}; ///< seqlock sequence: odd = write in progress
    std::atomic<std::uint32_t> flags{0};      ///< Combination of BufferFlags bits

    const void* data = nullptr;               ///< Points at the actual data (float32 array, etc.)
    std::uint64_t data_bytes = 0;             ///< Number of valid bytes
    std::uint64_t capacity_bytes = 0;         ///< Number of bytes data can actually hold

    std::uint64_t reserved[8 / sizeof(std::uint64_t)] = {};

    // -- Special members -----------------------------------------------------
    //
    // `std::atomic` is not copyable, so copy semantics must be spelled out explicitly; otherwise
    // `FieldBuffer` would become move-only -- and "descriptors are passed by value" is a design premise.
    //
    // What is copied is the **currently observed value**, semantically "take one more handle to the same data".
    // The live seqlock state always stays in **the original object**; the seq snapshot on a copy
    // is only ever handed to read_end for validation. The contract already says this struct observes (owns no data).

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

/// @brief Whether the struct is 8-byte aligned (the natural alignment of pointer + atomic fields).
inline constexpr std::size_t kFieldBufferAlign = alignof(FieldBuffer);

/**
 * @brief Check whether the descriptor is self-consistent (does not validate the data contents).
 *
 * Use: the host **must** call this before adopting any `FieldBuffer` that arrives from outside
 * (plugin, subprocess, script). This is the first gate against "a plugin handed us a wild pointer".
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        checks magic / layout / abi_major / non-null pointer / capacity, one by one
 * @invariant   a descriptor that returns true cannot cause an out-of-bounds read (given data is really valid)
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.field_buffer.validate_ok, abi.field_buffer.validate_rejects_bad_magic,
 *              abi.field_buffer.validate_rejects_layout_mismatch,
 *              abi.field_buffer.validate_rejects_oversized_data
 */
[[nodiscard]] inline bool validate(const FieldBuffer& b) noexcept {
    if (b.magic != kFieldBufferMagic) return false;
    if (b.layout != kFieldBufferLayout) return false;
    if (b.abi_major != kAbiMajor) return false;
    if (b.data == nullptr) return false;
    // data_bytes must not exceed what lattice declares as needed, nor the capacity
    if (b.data_bytes > b.capacity_bytes) return false;
    const std::uint64_t needed = data_bytes(b.lattice);
    if (needed != 0 && b.data_bytes < needed) return false;
    return true;
}

/// @brief Whether the data is ready to be read.
[[nodiscard]] inline bool is_readable(const FieldBuffer& b) noexcept {
    return has_flag(b.flags.load(std::memory_order_acquire), BufferFlags::valid);
}

/**
 * @brief Publisher: enter the writing state (step one of the seqlock write-side protocol).
 *
 * Protocol:
 * ```
 *   seq = begin_write(buf);          // turns odd, readers start retrying
 *   ... write lattice / data contents ...
 *   end_write(buf, seq, flags);      // turns even, readers can see consistent data
 * ```
 *
 * @ownership   borrows
 * @thread      publish
 * @pre         buf is not being written by another thread
 * @post        writer_seq becomes odd
 * @invariant   begin/end on the same thread must come in pairs
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
[[nodiscard]] inline std::uint32_t begin_write(FieldBuffer& buf) noexcept {
    return buf.writer_seq.fetch_add(1, std::memory_order_acq_rel) + 1U;
}

/**
 * @brief Publisher: finish the write and set the flags.
 *
 * @ownership   borrows
 * @thread      publish
 * @pre         seq comes from a paired begin_write
 * @post        writer_seq becomes even and greater than seq
 * @invariant   a write that started always finishes
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
inline void end_write(FieldBuffer& buf, std::uint32_t seq, std::uint32_t flag_bits) noexcept {
    buf.flags.store(flag_bits, std::memory_order_release);
    buf.writer_seq.store(seq + 1U, std::memory_order_release);
}

/**
 * @brief Reader: read the sequence number of a consistent snapshot.
 *
 * Usage:
 * ```
 *   retry:
 *     seq = read_begin(buf);
 *     if (seq & 1) goto retry;        // a write is in progress
 *     ... read data ...
 *     if (!read_end(buf, seq)) goto retry;
 * ```
 *
 * @ownership   observes
 * @thread      any
 * @pre         none
 * @post        returns the current sequence (may be odd, meaning a write is in progress)
 * @invariant   the return value is monotonically non-decreasing
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
[[nodiscard]] inline std::uint32_t read_begin(const FieldBuffer& buf) noexcept {
    return buf.writer_seq.load(std::memory_order_acquire);
}

/**
 * @brief Reader: confirm that the snapshot is still consistent.
 *
 * @ownership   observes
 * @thread      any
 * @pre         seq comes from a paired read_begin
 * @post        true means seq is even and was never changed by a writer
 * @invariant   when it returns true, the data read in between is a consistent snapshot
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.field_buffer.seqlock_roundtrip
 */
[[nodiscard]] inline bool read_end(const FieldBuffer& buf, std::uint32_t seq) noexcept {
    if ((seq & 1U) != 0U) return false;
    return buf.writer_seq.load(std::memory_order_acquire) == seq;
}

/// @brief First address of the data, interpreted as `T`. **No type checking at all** -- the caller's job.
template <class T>
[[nodiscard]] inline const T* data_as(const FieldBuffer& b) noexcept {
    return static_cast<const T*>(b.data);
}

}  // namespace qp::abi
