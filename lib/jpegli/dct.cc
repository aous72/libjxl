// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jpegli/dct.h"

#include <cmath>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jpegli/dct.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jpegli/memory_manager.h"

HWY_BEFORE_NAMESPACE();
namespace jpegli {
namespace HWY_NAMESPACE {
namespace {

// These templates are not found via ADL.
using hwy::HWY_NAMESPACE::Abs;
using hwy::HWY_NAMESPACE::Add;
using hwy::HWY_NAMESPACE::DemoteTo;
using hwy::HWY_NAMESPACE::Ge;
using hwy::HWY_NAMESPACE::IfThenElseZero;
using hwy::HWY_NAMESPACE::Mul;
using hwy::HWY_NAMESPACE::MulAdd;
using hwy::HWY_NAMESPACE::Rebind;
using hwy::HWY_NAMESPACE::Round;
using hwy::HWY_NAMESPACE::Sub;

using D = HWY_FULL(float);
using D8 = HWY_CAPPED(float, 8);
using DI = HWY_FULL(int32_t);
using DI16 = Rebind<int16_t, DI>;
constexpr D d;
constexpr D8 d8;
constexpr DI di;
constexpr DI16 di16;

#if HWY_CAP_GE256
JXL_INLINE void Transpose8x8Block(const float* JXL_RESTRICT from,
                                  float* JXL_RESTRICT to) {
  const HWY_CAPPED(float, 8) d;
  auto i0 = Load(d, from);
  auto i1 = Load(d, from + 1 * 8);
  auto i2 = Load(d, from + 2 * 8);
  auto i3 = Load(d, from + 3 * 8);
  auto i4 = Load(d, from + 4 * 8);
  auto i5 = Load(d, from + 5 * 8);
  auto i6 = Load(d, from + 6 * 8);
  auto i7 = Load(d, from + 7 * 8);

  const auto q0 = InterleaveLower(d, i0, i2);
  const auto q1 = InterleaveLower(d, i1, i3);
  const auto q2 = InterleaveUpper(d, i0, i2);
  const auto q3 = InterleaveUpper(d, i1, i3);
  const auto q4 = InterleaveLower(d, i4, i6);
  const auto q5 = InterleaveLower(d, i5, i7);
  const auto q6 = InterleaveUpper(d, i4, i6);
  const auto q7 = InterleaveUpper(d, i5, i7);

  const auto r0 = InterleaveLower(d, q0, q1);
  const auto r1 = InterleaveUpper(d, q0, q1);
  const auto r2 = InterleaveLower(d, q2, q3);
  const auto r3 = InterleaveUpper(d, q2, q3);
  const auto r4 = InterleaveLower(d, q4, q5);
  const auto r5 = InterleaveUpper(d, q4, q5);
  const auto r6 = InterleaveLower(d, q6, q7);
  const auto r7 = InterleaveUpper(d, q6, q7);

  i0 = ConcatLowerLower(d, r4, r0);
  i1 = ConcatLowerLower(d, r5, r1);
  i2 = ConcatLowerLower(d, r6, r2);
  i3 = ConcatLowerLower(d, r7, r3);
  i4 = ConcatUpperUpper(d, r4, r0);
  i5 = ConcatUpperUpper(d, r5, r1);
  i6 = ConcatUpperUpper(d, r6, r2);
  i7 = ConcatUpperUpper(d, r7, r3);

  Store(i0, d, to);
  Store(i1, d, to + 1 * 8);
  Store(i2, d, to + 2 * 8);
  Store(i3, d, to + 3 * 8);
  Store(i4, d, to + 4 * 8);
  Store(i5, d, to + 5 * 8);
  Store(i6, d, to + 6 * 8);
  Store(i7, d, to + 7 * 8);
}
#elif HWY_TARGET != HWY_SCALAR
JXL_INLINE void Transpose8x8Block(const float* JXL_RESTRICT from,
                                  float* JXL_RESTRICT to) {
  const HWY_CAPPED(float, 4) d;
  for (size_t n = 0; n < 8; n += 4) {
    for (size_t m = 0; m < 8; m += 4) {
      auto p0 = Load(d, from + n * 8 + m);
      auto p1 = Load(d, from + (n + 1) * 8 + m);
      auto p2 = Load(d, from + (n + 2) * 8 + m);
      auto p3 = Load(d, from + (n + 3) * 8 + m);
      const auto q0 = InterleaveLower(d, p0, p2);
      const auto q1 = InterleaveLower(d, p1, p3);
      const auto q2 = InterleaveUpper(d, p0, p2);
      const auto q3 = InterleaveUpper(d, p1, p3);

      const auto r0 = InterleaveLower(d, q0, q1);
      const auto r1 = InterleaveUpper(d, q0, q1);
      const auto r2 = InterleaveLower(d, q2, q3);
      const auto r3 = InterleaveUpper(d, q2, q3);
      Store(r0, d, to + m * 8 + n);
      Store(r1, d, to + (1 + m) * 8 + n);
      Store(r2, d, to + (2 + m) * 8 + n);
      Store(r3, d, to + (3 + m) * 8 + n);
    }
  }
}
#else
JXL_INLINE void Transpose8x8Block(const float* JXL_RESTRICT from,
                                  float* JXL_RESTRICT to) {
  for (size_t n = 0; n < 8; ++n) {
    for (size_t m = 0; m < 8; ++m) {
      to[8 * n + m] = from[8 * m + n];
    }
  }
}
#endif

template <size_t N>
void AddReverse(const float* JXL_RESTRICT ain1, const float* JXL_RESTRICT ain2,
                float* JXL_RESTRICT aout) {
  for (size_t i = 0; i < N; i++) {
    auto in1 = Load(d8, ain1 + i * 8);
    auto in2 = Load(d8, ain2 + (N - i - 1) * 8);
    Store(Add(in1, in2), d8, aout + i * 8);
  }
}

template <size_t N>
void SubReverse(const float* JXL_RESTRICT ain1, const float* JXL_RESTRICT ain2,
                float* JXL_RESTRICT aout) {
  for (size_t i = 0; i < N; i++) {
    auto in1 = Load(d8, ain1 + i * 8);
    auto in2 = Load(d8, ain2 + (N - i - 1) * 8);
    Store(Sub(in1, in2), d8, aout + i * 8);
  }
}

template <size_t N>
void B(float* JXL_RESTRICT coeff) {
  constexpr float kSqrt2 = 1.41421356237f;
  auto sqrt2 = Set(d8, kSqrt2);
  auto in1 = Load(d8, coeff);
  auto in2 = Load(d8, coeff + 8);
  Store(MulAdd(in1, sqrt2, in2), d8, coeff);
  for (size_t i = 1; i + 1 < N; i++) {
    auto in1 = Load(d8, coeff + i * 8);
    auto in2 = Load(d8, coeff + (i + 1) * 8);
    Store(Add(in1, in2), d8, coeff + i * 8);
  }
}

// Ideally optimized away by compiler (except the multiply).
template <size_t N>
void InverseEvenOdd(const float* JXL_RESTRICT ain, float* JXL_RESTRICT aout) {
  for (size_t i = 0; i < N / 2; i++) {
    auto in1 = Load(d8, ain + i * 8);
    Store(in1, d8, aout + 2 * i * 8);
  }
  for (size_t i = N / 2; i < N; i++) {
    auto in1 = Load(d8, ain + i * 8);
    Store(in1, d8, aout + (2 * (i - N / 2) + 1) * 8);
  }
}

// Constants for DCT implementation. Generated by the following snippet:
// for i in range(N // 2):
//    print(1.0 / (2 * math.cos((i + 0.5) * math.pi / N)), end=", ")
template <size_t N>
struct WcMultipliers;

template <>
struct WcMultipliers<4> {
  static constexpr float kMultipliers[] = {
      0.541196100146197,
      1.3065629648763764,
  };
};

template <>
struct WcMultipliers<8> {
  static constexpr float kMultipliers[] = {
      0.5097955791041592,
      0.6013448869350453,
      0.8999762231364156,
      2.5629154477415055,
  };
};

constexpr float WcMultipliers<4>::kMultipliers[];
constexpr float WcMultipliers<8>::kMultipliers[];

// Invoked on full vector.
template <size_t N>
void Multiply(float* JXL_RESTRICT coeff) {
  for (size_t i = 0; i < N / 2; i++) {
    auto in1 = Load(d8, coeff + (N / 2 + i) * 8);
    auto mul = Set(d8, WcMultipliers<N>::kMultipliers[i]);
    Store(Mul(in1, mul), d8, coeff + (N / 2 + i) * 8);
  }
}

void LoadFromBlock(const float* JXL_RESTRICT pixels, size_t pixels_stride,
                   size_t off, float* JXL_RESTRICT coeff) {
  for (size_t i = 0; i < 8; i++) {
    Store(LoadU(d8, pixels + i * pixels_stride + off), d8, coeff + i * 8);
  }
}

void StoreToBlockAndScale(const float* JXL_RESTRICT coeff, float* output,
                          size_t off) {
  auto mul = Set(d8, 1.0f / 8);
  for (size_t i = 0; i < 8; i++) {
    StoreU(Mul(mul, Load(d8, coeff + i * 8)), d8, output + i * 8 + off);
  }
}

template <size_t N>
struct DCT1DImpl;

template <>
struct DCT1DImpl<1> {
  JXL_INLINE void operator()(float* JXL_RESTRICT mem) {}
};

template <>
struct DCT1DImpl<2> {
  JXL_INLINE void operator()(float* JXL_RESTRICT mem) {
    auto in1 = Load(d8, mem);
    auto in2 = Load(d8, mem + 8);
    Store(Add(in1, in2), d8, mem);
    Store(Sub(in1, in2), d8, mem + 8);
  }
};

template <size_t N>
struct DCT1DImpl {
  void operator()(float* JXL_RESTRICT mem) {
    HWY_ALIGN float tmp[N * 8];
    AddReverse<N / 2>(mem, mem + N * 4, tmp);
    DCT1DImpl<N / 2>()(tmp);
    SubReverse<N / 2>(mem, mem + N * 4, tmp + N * 4);
    Multiply<N>(tmp);
    DCT1DImpl<N / 2>()(tmp + N * 4);
    B<N / 2>(tmp + N * 4);
    InverseEvenOdd<N>(tmp, mem);
  }
};

void DCT1D(const float* JXL_RESTRICT pixels, size_t pixels_stride,
           float* JXL_RESTRICT output) {
  HWY_ALIGN float tmp[64];
  for (size_t i = 0; i < 8; i += Lanes(d8)) {
    // TODO(veluca): consider removing the temporary memory here (as is done in
    // IDCT), if it turns out that some compilers don't optimize away the loads
    // and this is performance-critical.
    LoadFromBlock(pixels, pixels_stride, i, tmp);
    DCT1DImpl<8>()(tmp);
    StoreToBlockAndScale(tmp, output, i);
  }
}

void TransformFromPixels(const float* JXL_RESTRICT pixels, size_t pixels_stride,
                         float* JXL_RESTRICT coefficients,
                         float* JXL_RESTRICT scratch_space) {
  DCT1D(pixels, pixels_stride, scratch_space);
  Transpose8x8Block(scratch_space, coefficients);
  DCT1D(coefficients, 8, scratch_space);
  Transpose8x8Block(scratch_space, coefficients);
}

void QuantizeBlock(const float* dct, const float* qmc, const float zero_bias,
                   coeff_t* block) {
  const auto threshold = Set(d, zero_bias);
  for (size_t k = 0; k < kDCTBlockSize; k += Lanes(d)) {
    const auto val = Load(d, dct + k);
    const auto q = Load(d, qmc + k);
    const auto qval = Mul(val, q);
    const auto nzero_mask = Ge(Abs(qval), threshold);
    const auto ival = ConvertTo(di, IfThenElseZero(nzero_mask, Round(qval)));
    Store(DemoteTo(di16, ival), di16, block + k);
  }
}

void QuantizeBlockNoAQ(const float* dct, const float* qmc, coeff_t* block) {
  for (size_t k = 0; k < kDCTBlockSize; k += Lanes(d)) {
    const auto val = Load(d, dct + k);
    const auto q = Load(d, qmc + k);
    const auto ival = ConvertTo(di, Round(Mul(val, q)));
    Store(DemoteTo(di16, ival), di16, block + k);
  }
}

static constexpr float kDCBias = 128.0f;

void ComputeDCTCoefficients(j_compress_ptr cinfo) {
  jpeg_comp_master* m = cinfo->master;
  HWY_ALIGN float dct[kDCTBlockSize];
  HWY_ALIGN float scratch_space[kDCTBlockSize];
  for (int c = 0; c < cinfo->num_components; c++) {
    jpeg_component_info* comp = &cinfo->comp_info[c];
    int by0 = m->next_iMCU_row * comp->v_samp_factor;
    int block_rows_left = comp->height_in_blocks - by0;
    int max_block_rows = std::min(comp->v_samp_factor, block_rows_left);
    JBLOCKARRAY ba = (*cinfo->mem->access_virt_barray)(
        reinterpret_cast<j_common_ptr>(cinfo), m->coeff_buffers[c], by0,
        max_block_rows, true);
    float* qmc = m->quant_mul[c];
    RowBuffer<float>* plane = m->raw_data[c];
    const int h_factor = m->h_factor[c];
    const int v_factor = m->v_factor[c];
    for (int iy = 0; iy < comp->v_samp_factor; iy++) {
      size_t by = by0 + iy;
      if (by >= comp->height_in_blocks) continue;
      JBLOCKROW brow = ba[iy];
      const float* row = plane->Row(8 * by);
      for (size_t bx = 0; bx < comp->width_in_blocks; bx++) {
        JCOEF* block = &brow[bx][0];
        TransformFromPixels(row + 8 * bx, plane->stride(), dct, scratch_space);
        if (m->use_adaptive_quantization) {
          // Create more zeros in areas where jpeg xl would have used a lower
          // quantization multiplier.
          float relq = m->quant_field.Row(by * v_factor)[bx * h_factor];
          float zero_bias = 0.5f + m->zero_bias_mul[c] * relq;
          zero_bias = std::min(1.5f, zero_bias);
          QuantizeBlock(dct, qmc, zero_bias, block);
        } else {
          QuantizeBlockNoAQ(dct, qmc, block);
        }
        // Center DC values around zero.
        block[0] = std::round((dct[0] - kDCBias) * qmc[0]);
      }
    }
  }
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace
}  // namespace HWY_NAMESPACE
}  // namespace jpegli
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jpegli {

HWY_EXPORT(ComputeDCTCoefficients);

void ComputeDCTCoefficients(j_compress_ptr cinfo) {
  HWY_DYNAMIC_DISPATCH(ComputeDCTCoefficients)(cinfo);
}

}  // namespace jpegli
#endif  // HWY_ONCE
