/**
 * @file byte_order.hpp
 * @brief Byte-order declaration. The ABI supports little-endian only.
 *
 * Why **declare this explicitly** instead of "assuming everyone is little-endian":
 *   field data runs to 19MB, so byte swapping is an unacceptable cost;
 *   but a silent assumption becomes silent data corruption on a big-endian port.
 *
 * Therefore: detect at compile time, and fail the build on a non-little-endian target.
 * If big-endian support is ever really needed, the right move is a new explicit
 * conversion layer plus a kAbiMajor bump, not a branch in the hot path.
 *
 * @frozen yes
 */
#pragma once

namespace qp::abi {

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "qp ABI supports little-endian only. Big-endian needs an explicit conversion layer and a kAbiMajor bump."
#endif

#if defined(_M_PPC) || defined(__s390x__) || defined(__sparc__)
#error "qp ABI supports little-endian only. A known big-endian target was detected."
#endif

/// @brief The little-endian flag this ABI assumes. For external language bindings to
inline constexpr bool kLittleEndian = true;

}  // namespace qp::abi
