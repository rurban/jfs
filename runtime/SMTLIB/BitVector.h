//===----------------------------------------------------------------------===//
//
//                        JFS - The JIT Fuzzing Solver
//
// Copyright 2017 Daniel Liew
//
// This file is distributed under the MIT license.
// See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
#ifndef JFS_RUNTIME_SMTLIB_BITVECTOR_H
#define JFS_RUNTIME_SMTLIB_BITVECTOR_H
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

template <typename T> class BufferRef {
private:
  T *data;
  size_t size;

public:
  BufferRef(T *data, size_t size) : data(data), size(size) {}
  T *get() const { return data; }
  operator T *() const { return get(); }
  size_t getSize() const { return size; }
};

// Arbitary precision bitvector of width N
// that mimics the semantics of SMT-LIBv2
template <uint64_t N, typename = void> class BitVector {};

// Use template magic to specialize BitVector for widths
// <= 64 bits. This implementation uses native machine operations
// for speed.
template <uint64_t N>
class BitVector<N, typename std::enable_if<(N <= 64)>::type> {
private:
  typedef uint64_t dataTy;
  dataTy data;
  constexpr dataTy mask() const {
    if (N >= 64)
      return UINT64_MAX;

    // FIXME: gcc fails this assert
    // static_assert(N < 64, "Invalid N value");
    // FIXME: gcc incorrect warns about overshift here
    return (UINT64_C(1) << N) - 1;
  }
  dataTy doMod(dataTy value) const {
    if (N >= 64)
      return value;
    else
      return value % (1 << N);
  }
  constexpr dataTy mostSignificantBitMask() const {
    return (UINT64_C(1) << (N - 1));
  }

public:
  BitVector(uint64_t value) {
    static_assert(N > 0 && N <= 64, "Invalid value for N");
    data = doMod(value);
    assert(data == value);
  }

  BitVector() : BitVector(0) {
    static_assert(N > 0 && N <= 64, "Invalid value for N");
  }
  BitVector(BufferRef<uint8_t> bufferRef) {
    data = 0;
    memcpy(&data, bufferRef, bufferRef.getSize());
  }
  BitVector(const BitVector<N> &other) : data(other.data) {
    static_assert(N > 0 && N <= 64, "Invalid value for N");
  }
  BufferRef<uint8_t> getBuffer() const {
    return BufferRef<uint8_t>(
        reinterpret_cast<uint8_t *>(const_cast<dataTy *>(&data)),
        sizeof(dataTy));
  }
  // Operators producing values of width != N
  // TODO

  // Concat [this][rhs]
  // this is conceptually in MSB.
  // rhs is in conceptually in LSB.
  //
  // Implementation for where result is a native BitVector
  template <uint64_t M,
            typename std::enable_if<((N + M) <= 64)>::type * = nullptr>
  BitVector<N + M> concat(const BitVector<M> &rhs) const {
    // Concatentation produces native BitVector.
    static_assert((N + M) <= 64, "Too many bits");
    assert((rhs.doMod(rhs.data) == rhs.data) && "too many bits");
    assert((doMod(data) == data) && "too many bits");
    uint64_t newValue = rhs.data;
    newValue |= (data << M);
    return BitVector<N + M>(newValue);
  }

  // Implementation for where result is not a native BitVector
  template <uint64_t M,
            typename std::enable_if<((N + M) > 64)>::type * = nullptr>
  BitVector<N + M> concat(const BitVector<M> &rhs) const {
    // Concat produces bitvector that we can't represent natively.
    constexpr size_t bufferSize = (N + M + 7) / 8;
    uint8_t rawData[bufferSize];

    // Copy across rhs
    memcpy(rawData, rhs.getBuffer().get(), rhs.getBuffer().getSize());

    const size_t lhsByteStart = M / 8;
    const size_t shiftOffset = M % 8;
    const size_t lookBackShiftOffset = 8 - shiftOffset;
    if (shiftOffset == 0) {
      // On byte boundary
      for (unsigned int index = lhsByteStart; index < bufferSize; ++index) {
        if ((index * 8) < (N + M)) {
          const uint8_t *lhsByte =
              reinterpret_cast<const uint8_t *>(&data) + (index - lhsByteStart);
          // We are writing a byte from lhs
          rawData[index] = *lhsByte;
          continue;
        }
        // Zero the data
        rawData[index] = 0;
      }
    } else {
      // Not on byte boundary. More complicated
      for (unsigned int index = lhsByteStart; index < bufferSize; ++index) {
        if ((index * 8) < (N + M)) {
          // We are writing at least 1 bit for lhs
          const uint8_t *lhsByte =
              reinterpret_cast<const uint8_t *>(&data) + (index - lhsByteStart);
          if (index == lhsByteStart) {
            // First byte has to be done specially because we writing
            // to a byte that contains bits from rhs.
            rawData[index] |= ((*lhsByte) << shiftOffset);
            continue;
          }
          // Not doing the first byte. This means we need to also grab the bits
          // from the previous iteration that we shifted out.
          const uint8_t *lhsBytePrevIter = lhsByte - 1;
          rawData[index] |= ((*lhsByte) << shiftOffset) |
                            ((*lhsBytePrevIter) >> lookBackShiftOffset);
          continue;
        }
        // Not writing any bits from lhs so just zero the data
        rawData[index] = 0;
      }
    }
    BufferRef<uint8_t> rawDataRef(reinterpret_cast<uint8_t *>(rawData),
                                  bufferSize);
    return BitVector<N + M>(rawDataRef);
  }

  template <uint64_t HIGH, uint64_t LOW>
  BitVector<(HIGH - LOW) + 1> extract() const {
    static_assert(HIGH >= LOW, "Invalid HIGH and/or LOW value");
    // TODO
    return BitVector<(HIGH - LOW) + 1>(0);
  }

  template <uint64_t BITS> BitVector<N + BITS> zeroExtend() const {
    // TODO
    return BitVector<N + BITS>(0);
  }

  template <uint64_t BITS> BitVector<N + BITS> signExtend() const {
    // TODO
    return BitVector<N + BITS>(0);
  }

  // Arithmetic operators
  BitVector<N> bvneg() const {
    // [[(bvneg s)]] := nat2bv[m](2^m - bv2nat([[s]]))
    if (data == 0) {
      return BitVector<N>(0);
    }

    // In two's complement, flipping bits and adding one negates
    // the number.
    return BitVector<N>(((~data) & mask()) + 1);
  }
  BitVector<N> bvadd(const BitVector<N> &other) const {
    // [[(bvadd s t)]] := nat2bv[m](bv2nat([[s]]) + bv2nat([[t]]))
    return BitVector<N>(doMod(data + other.data));
  }
  BitVector<N> bvsub(const BitVector<N> &other) const {
    // (bvsub s t) abbreviates (bvadd s (bvneg t))

    // TODO: Verify the implementation is semantically equivalent
    // to SMT-LIBv2
    return BitVector<N>(doMod(data - other.data));
  }
  BitVector<N> bvmul(const BitVector<N> &other) const {
    // [[(bvmul s t)]] := nat2bv[m](bv2nat([[s]]) * bv2nat([[t]]))
    return BitVector<N>(doMod(data * other.data));
  }
  BitVector<N> bvudiv(const BitVector<N> &divisor) const {
    //   [[(bvudiv s t)]] := if bv2nat([[t]]) = 0
    //                       then λx:[0, m). 1
    //                       else nat2bv[m](bv2nat([[s]]) div bv2nat([[t]]))
    if (divisor == 0) {
      return BitVector<N>(mask());
    }
    return data / divisor.data;
  }
  BitVector<N> bvurem(const BitVector<N> &divisor) const {
    //  [[(bvurem s t)]] := if bv2nat([[t]]) = 0
    //                      then [[s]]
    //                      else nat2bv[m](bv2nat([[s]]) rem bv2nat([[t]]))
    if (divisor == 0) {
      return BitVector<N>(*this);
    }
    return data % divisor.data;
  }
  BitVector<N> bvshl(const BitVector<N> &shift) const {
    //  [[(bvshl s t)]] := nat2bv[m](bv2nat([[s]]) * 2^(bv2nat([[t]])))
    if (shift.data >= N) {
      // Overshift to zero
      return BitVector<N>(0);
    }
    return BitVector<N>((data << shift.data) & mask());
  }
  BitVector<N> bvlshr(const BitVector<N> &shift) const {
    // [[(bvlshr s t)]] := nat2bv[m](bv2nat([[s]]) div 2^(bv2nat([[t]])))
    if (shift.data >= N) {
      // Overshift to zero
      return BitVector<N>(0);
    }
    return BitVector<N>((data >> shift.data) & mask());
  }
  // TODO

  // Bitwise operators
  BitVector<N> bvand(const BitVector<N> &other) const {
    return BitVector<N>((data & other.data) & mask());
  }
  BitVector<N> bvor(const BitVector<N> &other) const {
    return BitVector<N>((data | other.data) & mask());
  }
  BitVector<N> bvnot() const { return BitVector<N>((~data) & mask()); }
  // TODO

  // Comparison operators
  bool operator==(const BitVector &rhs) const { return data == rhs.data; }
  bool operator!=(const BitVector &rhs) const { return !(*this == rhs); }

  bool bvule(const BitVector &rhs) const { return data <= rhs.data; }
  bool bvult(const BitVector &rhs) const { return data < rhs.data; }
  bool bvugt(const BitVector &rhs) const { return data > rhs.data; }
  bool bvuge(const BitVector &rhs) const { return data >= rhs.data; }
  // TODO

  // This template is friends with all other instantiations
  // FIXME: It would be better if we were only friends where
  // N <= 64.
  template <uint64_t W, typename T> friend class BitVector;
};

template <uint64_t N>
class BitVector<N, typename std::enable_if<(N > 64)>::type> {
private:
  uint8_t *data;
  constexpr size_t numBytesRequired(size_t bits) const {
    return (bits + 7) / 8;
  }

public:
  // FIXME: We make this more efficient by lazily allocating memory.
  // Initialize from array
  BitVector(uint8_t *bytesToCopy, size_t numBytes) : data(nullptr) {
    data = reinterpret_cast<uint8_t *>(malloc(numBytesRequired(N)));
    assert(data);
    assert(bytesToCopy);
    assert(numBytes <= numBytesRequired(N));
    memcpy(data, bytesToCopy, numBytesRequired(N));
  }
  BitVector(BufferRef<uint8_t> bufferRef)
      : BitVector(bufferRef.get(), bufferRef.getSize()) {}
  // Initialize to zero
  BitVector() {
    data = reinterpret_cast<uint8_t *>(malloc(numBytesRequired(N)));
    assert(data);
    memset(data, 0, numBytesRequired(N));
  }
  BitVector(uint64_t value) : BitVector() {
    memcpy(data, &value, sizeof(uint64_t));
  }
  ~BitVector() {
    if (data)
      free(data);
  }
  BufferRef<uint8_t> getBuffer() const {
    return BufferRef<uint8_t>(data, numBytesRequired(N));
  }
  // TODO:
};
#endif