// Copyright 2024 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Include guard for headers.
#ifndef THIRD_PARTY_GEMMA_CPP_COMPRESSION_COMPRESS_INL_H_
#define THIRD_PARTY_GEMMA_CPP_COMPRESSION_COMPRESS_INL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <cmath>  // lroundf, only if COMPRESS_STATS

#include "compression/blob_store.h"
#include "compression/compress.h"
#include "compression/distortion.h"
#include "hwy/aligned_allocator.h"
#include "hwy/base.h"
#include "hwy/contrib/thread_pool/thread_pool.h"
#include "hwy/timer.h"

#endif  // THIRD_PARTY_GEMMA_CPP_COMPRESSION_COMPRESS_INL_H_

// Include guard for (potentially) SIMD code.
#if defined(THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE) == defined(HWY_TARGET_TOGGLE)
#ifdef THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE
#undef THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE
#else
#define THIRD_PARTY_GEMMA_CPP_COMPRESS_TOGGLE
#endif

#include "hwy/highway.h"
// After highway.h
#include "compression/nuq-inl.h"
#include "compression/sfp-inl.h"
#include "hwy/profiler.h"  // also uses SIMD

HWY_BEFORE_NAMESPACE();
namespace gcpp {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Enables generic code independent of compression type.
template <typename T>  // primary, must specialize
struct CompressTraits {};

// Used by backprop/, where weights are currently f32; also MatMul for f32
// weights or activations, if native `ReorderWidenMulAccumulate` is available.
template <>
struct CompressTraits<float> {
  using Packed = float;

  template <class DF, HWY_IF_F32_D(DF), class VF = hn::Vec<DF>>
  static HWY_INLINE void Compress(DF /*df*/, const float* HWY_RESTRICT raw,
                                  size_t num, CompressPerThread& /*tls*/,
                                  const PackedSpan<Packed>& packed,
                                  const size_t packed_ofs) {
    hwy::CopyBytes(raw, packed.ptr + packed_ofs, num * sizeof(raw[0]));
  }

  template <class DF, HWY_IF_F32_D(DF), class VF = hn::Vec<DF>>
  static void Store2(DF df, VF raw0, VF raw1, const PackedSpan<Packed>& packed,
                     const size_t packed_ofs) {
    const size_t NF = hn::Lanes(df);
    hn::StoreU(raw0, df, packed.ptr + packed_ofs);
    hn::StoreU(raw1, df, packed.ptr + packed_ofs + NF);
  }

  template <class DBF16, HWY_IF_BF16_D(DBF16), class VBF16 = hn::Vec<DBF16>>
  static HWY_INLINE void Load2(DBF16 dbf16,
                               const PackedSpan<const Packed>& packed,
                               const size_t packed_ofs, VBF16& raw0,
                               VBF16& raw1) {
    const hn::Repartition<float, decltype(dbf16)> df;
    using VF = hn::Vec<decltype(df)>;
    const size_t NF = hn::Lanes(df);
    const VF f0 = hn::LoadU(df, packed.ptr + packed_ofs + 0 * NF);
    const VF f1 = hn::LoadU(df, packed.ptr + packed_ofs + 1 * NF);
    const VF f2 = hn::LoadU(df, packed.ptr + packed_ofs + 2 * NF);
    const VF f3 = hn::LoadU(df, packed.ptr + packed_ofs + 3 * NF);
    raw0 = hn::OrderedDemote2To(dbf16, f0, f1);
    raw1 = hn::OrderedDemote2To(dbf16, f2, f3);
  }

  template <class DF, HWY_IF_F32_D(DF), class VF = hn::Vec<DF>>
  static HWY_INLINE void Load2(DF df, const PackedSpan<const Packed>& packed,
                               const size_t packed_ofs, VF& raw0, VF& raw1) {
    const size_t N = hn::Lanes(df);
    raw0 = hn::LoadU(df, packed.ptr + packed_ofs);
    raw1 = hn::LoadU(df, packed.ptr + packed_ofs + N);
  }

  template <class DBF, HWY_IF_BF16_D(DBF)>
  static HWY_INLINE void DecompressAndZeroPad(
      DBF dbf, const PackedSpan<const Packed>& packed, const size_t packed_ofs,
      BF16* HWY_RESTRICT raw, size_t num) {
    const hn::Repartition<float, decltype(dbf)> df;
    using VF = hn::Vec<decltype(df)>;
    using VBF = hn::Vec<decltype(dbf)>;
    const size_t NF = hn::Lanes(df);

    size_t i = 0;
    if (num >= 2 * NF) {
      for (; i <= num - 2 * NF; i += 2 * NF) {
        const VF f0 = hn::LoadU(df, packed.ptr + packed_ofs + i);
        const VF f1 = hn::LoadU(df, packed.ptr + packed_ofs + i + NF);
        hn::StoreU(hn::OrderedDemote2To(dbf, f0, f1), dbf, raw + i);
      }
    }
    const size_t remaining = num - i;
    HWY_DASSERT(remaining < 2 * NF);
    if (HWY_UNLIKELY(remaining != 0)) {
      const size_t remaining2 = remaining - HWY_MIN(remaining, NF);
      const VF f0 = hn::LoadN(df, packed.ptr + packed_ofs + i, remaining);
      const VF f1 = hn::LoadN(df, packed.ptr + packed_ofs + i + NF, remaining2);
      hn::StoreU(hn::OrderedDemote2To(dbf, f0, f1), dbf, raw + i);
    }
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void DecompressAndZeroPad(
      DF df, const PackedSpan<const Packed>& packed, const size_t packed_ofs,
      float* HWY_RESTRICT raw, size_t num) {
    using VF = hn::Vec<decltype(df)>;
    const size_t NF = hn::Lanes(df);

    size_t i = 0;
    if (num >= NF) {
      for (; i <= num - NF; i += NF) {
        const VF vf = hn::LoadU(df, packed.ptr + packed_ofs + i);
        hn::StoreU(vf, df, raw + i);
      }
    }
    const size_t remaining = num - i;
    HWY_DASSERT(remaining < NF);
    if (HWY_UNLIKELY(remaining != 0)) {
      const VF vf = hn::LoadN(df, packed.ptr + packed_ofs + i, remaining);
      hn::StoreU(vf, df, raw + i);  // adds zero padding
    }
  }
};

template <>
struct CompressTraits<BF16> {
  using Packed = BF16;

  // Note: it is fine for the lower 16 mantissa bits of `raw` to be nonzero
  // because we round rather than truncate.
  template <class DF, HWY_IF_F32_D(DF), class VF = hn::Vec<DF>>
  static HWY_INLINE void Compress(DF df, const float* HWY_RESTRICT raw,
                                  size_t num, CompressPerThread& tls,
                                  const PackedSpan<Packed>& packed,
                                  const size_t packed_ofs) {
    const hn::RebindToUnsigned<decltype(df)> du;
    const hn::Repartition<BF16, decltype(df)> dbf;
    const size_t NF = hn::Lanes(df);

    size_t i = 0;
    if (num >= 2 * NF) {
      for (; i <= num - 2 * NF; i += 2 * NF) {
        const VF raw0 = hn::LoadU(df, raw + i);
        const VF raw1 = hn::LoadU(df, raw + i + NF);

        hn::StoreU(hn::OrderedDemote2To(dbf, raw0, raw1), dbf,
                   packed.ptr + packed_ofs + i);

        if (COMPRESS_STATS) {
          DistortionStats stats;
          for (size_t j = 0; j < 2 * NF; ++j) {
            stats.Notify(raw[i + j],
                         hwy::F32FromBF16(packed.ptr[packed_ofs + i + j]));
          }
          tls.stats.Notify(stats);
        }
      }
    }

    const size_t remaining = num - i;
    HWY_DASSERT(remaining < 2 * NF);
    if (remaining != 0) {
      const VF raw0 = hn::LoadN(df, raw + i, remaining);
      const size_t remaining1 = remaining - HWY_MIN(remaining, NF);
      const VF raw1 = hn::LoadN(df, raw + i + NF, remaining1);

      hn::StoreN(hn::OrderedDemote2To(dbf, raw0, raw1), dbf,
                 packed.ptr + packed_ofs + i, remaining);

      if (COMPRESS_STATS) {
        DistortionStats stats;
        for (size_t j = 0; j < remaining; ++j) {
          stats.Notify(raw[i + j],
                       hwy::F32FromBF16(packed.ptr[packed_ofs + i + j]));
        }
        tls.stats.Notify(stats);
      }
    }
  }

  template <class DF, HWY_IF_F32_D(DF), class VF = hn::Vec<DF>>
  static void Store2(DF df, VF raw0, VF raw1, const PackedSpan<Packed>& packed,
                     const size_t packed_ofs) {
    const hn::Repartition<BF16, decltype(df)> dbf;
    hn::StoreU(hn::OrderedDemote2To(dbf, raw0, raw1), dbf,
               packed.ptr + packed_ofs);
  }

  template <class DBF16, HWY_IF_BF16_D(DBF16)>
  static HWY_INLINE void Load2(DBF16 dbf16,
                               const PackedSpan<const Packed>& packed,
                               const size_t packed_ofs, hn::Vec<DBF16>& raw0,
                               hn::Vec<DBF16>& raw1) {
    const size_t N16 = hn::Lanes(dbf16);
    raw0 = hn::LoadU(dbf16, packed.ptr + packed_ofs);
    raw1 = hn::LoadU(dbf16, packed.ptr + packed_ofs + N16);
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Load2(DF df, const PackedSpan<const Packed>& packed,
                               const size_t packed_ofs, hn::Vec<DF>& raw0,
                               hn::Vec<DF>& raw1) {
    const hn::Repartition<BF16, decltype(df)> dbf;
    using VBF = hn::Vec<decltype(dbf)>;
    const VBF packed0 = hn::LoadU(dbf, packed.ptr + packed_ofs);
    raw0 = hn::PromoteLowerTo(df, packed0);
    raw1 = hn::PromoteUpperTo(df, packed0);
  }

  template <class DBF, HWY_IF_BF16_D(DBF)>
  static HWY_INLINE void DecompressAndZeroPad(
      DBF dbf, const PackedSpan<const Packed>& packed, const size_t packed_ofs,
      BF16* HWY_RESTRICT raw, size_t num) {
    using VBF = hn::Vec<decltype(dbf)>;
    const size_t N16 = hn::Lanes(dbf);

    size_t i = 0;
    if (num >= N16) {
      for (i = 0; i <= num - N16; i += N16) {
        const VBF packed0 = hn::LoadU(dbf, packed.ptr + packed_ofs + i);
        hn::StoreU(packed0, dbf, raw + i);
      }
    }

    const size_t remaining = num - i;
    HWY_DASSERT(remaining < N16);
    if (HWY_UNLIKELY(remaining != 0)) {
      const VBF packed0 =
          hn::LoadN(dbf, packed.ptr + packed_ofs + i, remaining);
      hn::StoreU(packed0, dbf, raw + i);
    }
  }

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void DecompressAndZeroPad(
      DF df, const PackedSpan<const Packed>& packed, const size_t packed_ofs,
      float* HWY_RESTRICT raw, size_t num) {
    const hn::Repartition<BF16, decltype(df)> dbf;
    using VF = hn::Vec<decltype(df)>;
    using VBF = hn::Vec<decltype(dbf)>;
    const size_t NF = hn::Lanes(df);

    size_t i = 0;
    if (num >= 2 * NF) {
      for (i = 0; i <= num - 2 * NF; i += 2 * NF) {
        VF raw0, raw1;
        Load2(df, packed, packed_ofs + i, raw0, raw1);
        hn::StoreU(raw0, df, raw + i);
        hn::StoreU(raw1, df, raw + i + NF);
      }
    }

    const size_t remaining = num - i;
    HWY_DASSERT(remaining < 2 * NF);
    if (HWY_UNLIKELY(remaining != 0)) {
      const VBF packed0 =
          hn::LoadN(dbf, packed.ptr + packed_ofs + i, remaining);
      const VF raw0 = hn::PromoteLowerTo(df, packed0);
      const VF raw1 = hn::PromoteUpperTo(df, packed0);
      // If at most one vector, the first store adds zero padding. Check before
      // storing the second, because callers only pad to one vector.
      hn::StoreU(raw0, df, raw + i);
      if (remaining >= NF) hn::StoreU(raw1, df, raw + i + NF);
    }
  }
};

// Switching floating point: 8-bit, 2..3 mantissa bits.
template <>
struct CompressTraits<SfpStream> {
  using Packed = SfpStream;

  // Callers are responsible for scaling `raw` such that its magnitudes do not
  // exceed `SfpStream::kMax`. See CompressedArray::scale().
  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Compress(DF df, const float* HWY_RESTRICT raw,
                                  size_t num, CompressPerThread& tls,
                                  const PackedSpan<Packed>& packed,
                                  const size_t packed_ofs) {
    SfpCodec::Enc(df, raw, num, packed.ptr + packed_ofs);

    if (COMPRESS_STATS) {
      const hn::Repartition<BF16, DF> dbf;
      auto distorted =
          hwy::AllocateAligned<BF16>(hwy::RoundUpTo(num, hn::Lanes(dbf)));
      SfpCodec::DecompressAndZeroPad(dbf, MakeConst(packed), packed_ofs,
                                     distorted.get(), num);
      DistortionStats stats;
      for (size_t i = 0; i < num; ++i) {
        stats.Notify(raw[i], hwy::F32FromBF16(distorted[i]));
      }
      tls.stats.Notify(stats);
    }
  }

  template <class D>  // Caller checks this is f32 or bf16
  static HWY_INLINE void Load2(D d, const PackedSpan<const Packed>& packed,
                               const size_t packed_ofs, hn::Vec<D>& raw0,
                               hn::Vec<D>& raw1) {
    const hn::Twice<hn::Rebind<uint8_t, D>> d8;
    using V8 = hn::Vec<decltype(d8)>;
    const V8 v8 = hn::LoadU(d8, &packed.ptr->byte + packed_ofs);
    SfpCodec::Dec2(d, v8, raw0, raw1);
  }

  // Store2 is not yet implemented.

  template <class D, typename Raw>
  static HWY_INLINE void DecompressAndZeroPad(
      D d, const PackedSpan<const Packed>& packed, const size_t packed_ofs,
      Raw* HWY_RESTRICT raw, const size_t num) {
    SfpCodec::DecompressAndZeroPad(d, packed, packed_ofs, raw, num);
  }
};

// Nonuniform quantization, 4.5 bits per element, two separate streams.
template <>
struct CompressTraits<NuqStream> {
  using Packed = NuqStream;

  template <class DF, HWY_IF_F32_D(DF)>
  static HWY_INLINE void Compress(DF df, const float* HWY_RESTRICT raw,
                                  size_t num, CompressPerThread& tls,
                                  const PackedSpan<Packed>& packed,
                                  const size_t packed_ofs) {
    NuqCodec::Enc(df, raw, num, tls.buf, packed, packed_ofs);

    if (COMPRESS_STATS) {
      for (size_t i = 0; i < num; ++i) {
        tls.stats.NotifyIn(static_cast<int>(lroundf(raw[i] * 100.0f + 500.0f)));
      }

      const hn::Repartition<BF16, DF> dbf;
      const size_t N16 = hn::Lanes(dbf);
      auto distorted = hwy::AllocateAligned<BF16>(hwy::RoundUpTo(num, N16));
      NuqCodec::DecompressAndZeroPad(dbf, MakeConst(packed), packed_ofs,
                                     distorted.get(), num);
      DistortionStats stats;
      for (size_t i = 0; i < num; ++i) {
        stats.Notify(raw[i], hwy::F32FromBF16(distorted[i]));
      }
      tls.stats.Notify(stats);
    }
  }

  template <class D>  // Caller checks this is f32 or bf16
  static HWY_INLINE void Load2(D d, const PackedSpan<const Packed>& packed,
                               const size_t packed_ofs, hn::Vec<D>& raw0,
                               hn::Vec<D>& raw1) {
    const hn::Twice<hn::Rebind<uint8_t, D>> d8;
    using V8 = hn::Vec<decltype(d8)>;
    NuqCodec::Dec2(d, packed, packed_ofs, raw0, raw1);
  }

  // Store2 is not yet implemented.

  template <class D, typename Raw>
  static HWY_INLINE void DecompressAndZeroPad(
      D d, const PackedSpan<const Packed>& packed, const size_t packed_ofs,
      Raw* raw, const size_t num) {
    NuqCodec::DecompressAndZeroPad(d, packed, packed_ofs, raw, num);
  }
};

// Compresses `num` elements of `raw` to `packed` starting at `packed_ofs`,
// which is useful for compressing sub-regions of an array.
template <typename Packed>
HWY_NOINLINE void Compress(const float* HWY_RESTRICT raw, size_t num,
                           CompressWorkingSet& work,
                           const PackedSpan<Packed>& packed,
                           const size_t packed_ofs, hwy::ThreadPool& pool) {
  packed.BoundsCheck(packed_ofs, num);
  work.tls.resize(pool.NumWorkers());
  if (COMPRESS_STATS) {
    for (auto& tls : work.tls) {
      tls.stats.Reset();
    }
  }

  const bool want_bench = num > 1024 * 1024 || COMPRESS_STATS;
  const double t0 = want_bench ? hwy::platform::Now() : 0.0;

  using Traits = CompressTraits<Packed>;
  constexpr size_t kBatch = 8192;
  const size_t num_batches = hwy::DivCeil(num, kBatch);
  pool.Run(0, num_batches,
           [&](const uint32_t idx_batch, size_t thread) HWY_ATTR {
             const hn::ScalableTag<float> df;

             const size_t my_pos = idx_batch * kBatch;
             const size_t my_num =
                 idx_batch == num_batches - 1 ? (num - my_pos) : kBatch;
             Traits::Compress(df, raw + my_pos, my_num, work.tls[thread],
                              packed, packed_ofs + my_pos);
           });

  if (want_bench) {  // Avoids log spam in tests
    const double t1 = hwy::platform::Now();
    const double mb = static_cast<double>(num) * sizeof(raw[0]) * 1E-6;
    const double mbps = mb / (t1 - t0);
    fprintf(stderr, "Compress %.1f MB/s\n", mbps);
  }

  if (COMPRESS_STATS) {
    for (size_t i = 1; i < work.tls.size(); ++i) {
      work.tls[0].stats.Assimilate(work.tls[i].stats);
    }
    work.tls[0].stats.PrintAll();
  }
}

// Adapter that compresses into `CompressedArray`. `raw` must already be scaled
// to fit the value range, if `Packed` is `SfpStream`.
template <typename Packed, size_t kCapacity>
HWY_INLINE void CompressScaled(const float* HWY_RESTRICT raw, size_t num,
                               CompressWorkingSet& work,
                               CompressedArray<Packed, kCapacity>& compressed,
                               hwy::ThreadPool& pool) {
  Compress(raw, num, work, MakeSpan(compressed.data(), kCapacity),
           /*packed_ofs=*/0, pool);
}

// Stores two f32 vectors to f32 or bf16; avoids duplicating RMSNorm and
// RMSNormInplace for the two output types.
template <class DF, typename Packed, HWY_IF_F32_D(DF), class VF = hn::Vec<DF>>
void Compress2(DF df, VF raw0, VF raw1, const PackedSpan<Packed>& packed,
               const size_t packed_ofs) {
  static_assert(hwy::IsSameEither<Packed, float, BF16>());
  packed.BoundsCheck(packed_ofs, 2 * hn::Lanes(df));
  using Traits = CompressTraits<Packed>;
  Traits::Store2(df, raw0, raw1, packed, packed_ofs);
}

// Decompresses from any type of `packed`, to two float or BF16 vectors.
template <class DRaw, typename Packed, class VRaw = hn::Vec<DRaw>>
HWY_INLINE void Decompress2(DRaw d, const PackedSpan<Packed>& packed,
                            const size_t packed_ofs, VRaw& raw0, VRaw& raw1) {
  using TRaw = hn::TFromD<DRaw>;
  static_assert(hwy::IsSameEither<TRaw, float, BF16>());
  packed.BoundsCheck(packed_ofs, 2 * hn::Lanes(d));
  using Traits = CompressTraits<hwy::RemoveCvRef<Packed>>;
  Traits::Load2(d, MakeConst(packed), packed_ofs, raw0, raw1);
}

// Decompresses from any type of `packed`, starting at (any) `packed_ofs`, to
// (any) `num` elements in `raw`, then appends `[0, hn::Lanes(d))` zeroes as
// required to round `num` up to one vector, if it is not already. The caller is
// responsible for scaling `raw` to the original range because `EmbedToken`
// also wants to scale the decompressed elements.
template <class DRaw, typename Packed, typename TRaw = hn::TFromD<DRaw>>
HWY_NOINLINE void DecompressAndZeroPad(DRaw d, const PackedSpan<Packed>& packed,
                                       const size_t packed_ofs, TRaw* raw,
                                       size_t num) {
  static_assert(hwy::IsSameEither<TRaw, float, BF16>());
  using Traits = CompressTraits<hwy::RemoveCvRef<Packed>>;
  packed.BoundsCheck(packed_ofs, num);
  Traits::DecompressAndZeroPad(d, MakeConst(packed), packed_ofs, raw, num);
}

// Decompresses to the type specified by `D` from each of two arrays in groups
// of four vectors, passes them to `kernel.Update4`, zero-pads to a vector
// multiple, then calls `kernel.Update1` for the remaining vectors. Returns
// `kernel.Reduce`.
//
// This is useful for implementing dot products, and similar to
// `hwy/contrib/unroller`, but also supports compressed types with simpler
// remainder handling thanks to `DecompressAndZeroPad`.
//
// `w` can be any packed type, including NUQ, which requires a separate `w_ofs`
// rather than pointer arithmetic. `vec_aligned` can also be any type, but
// typically float or BF16. We omit a `v_ofs` because it is 0 in our use cases.
// `num`, the number of elements to process, need not be a vector multiple.
//
// `kernel` is const& so we can pass an rvalue argument, but can contain
// mutable state, though not vectors (see highway.h). We pass in the four
// loaded vectors plus eight *f32* state vectors, independent of `D`.
template <class D, typename WeightT, typename VecT, class Kernel>
HWY_INLINE float DecompressAndCall(D d, const PackedSpan<const WeightT>& w,
                                   const size_t w_ofs,
                                   const VecT* HWY_RESTRICT vec_aligned,
                                   const size_t num, const Kernel& kernel) {
  PROFILER_FUNC;

  HWY_DASSERT(hn::IsAligned(hn::Repartition<VecT, D>(), vec_aligned));
  const auto v_span = MakeSpan(vec_aligned, num);

  // Decompressed inputs
  using V = hn::Vec<decltype(d)>;
  V w0, w1, w2, w3, v0, v1, v2, v3;

  // State for Kernel
  const hn::Repartition<float, D> df;
  using VF = hn::Vec<decltype(df)>;
  VF sum0 = hn::Zero(df);
  VF sum1 = hn::Zero(df);
  VF sum2 = hn::Zero(df);
  VF sum3 = hn::Zero(df);
  VF comp0 = hn::Zero(df);
  VF comp1 = hn::Zero(df);
  VF comp2 = hn::Zero(df);
  VF comp3 = hn::Zero(df);

  const size_t N = hn::Lanes(d);
  size_t i = 0;
  if (num >= 4 * N) {
    for (; i <= num - 4 * N; i += 4 * N) {
      Decompress2(d, w, w_ofs + i + 0 * N, w0, w1);
      Decompress2(d, w, w_ofs + i + 2 * N, w2, w3);
      Decompress2(d, v_span, i + 0 * N, v0, v1);
      Decompress2(d, v_span, i + 2 * N, v2, v3);

      kernel.Update4(d, w0, w1, w2, w3, v0, v1, v2, v3, sum0, sum1, sum2, sum3,
                     comp0, comp1, comp2, comp3);
    }
  }

  size_t remaining = num - i;
  HWY_DASSERT(remaining < 4 * N);
  if (HWY_UNLIKELY(remaining != 0)) {
    using T = hn::TFromD<D>;
    HWY_ALIGN T padded_w[4 * hn::MaxLanes(d)];
    HWY_ALIGN T padded_v[4 * hn::MaxLanes(d)];
    DecompressAndZeroPad(d, w, w_ofs + i, padded_w, remaining);
    DecompressAndZeroPad(d, v_span, i, padded_v, remaining);

    // 1..4 whole vectors, possibly zero-padded.
    for (size_t padded_pos = 0; padded_pos < remaining; padded_pos += N) {
      const V w0 = hn::Load(d, padded_w + padded_pos);
      const V v0 = hn::Load(d, padded_v + padded_pos);
      kernel.Update1(d, w0, v0, sum0, comp0);
    }
  }

  return kernel.Reduce(df, sum0, sum1, sum2, sum3, comp0, comp1, comp2, comp3);
}

// Same as above, but single input array. Used by RMSNorm.
template <class D, typename VecT, class Kernel>
HWY_INLINE float DecompressAndCall(D d, const VecT* HWY_RESTRICT vec_aligned,
                                   const size_t num, const Kernel& kernel) {
  PROFILER_FUNC;

  HWY_DASSERT(hn::IsAligned(hn::Repartition<VecT, D>(), vec_aligned));
  const auto v_span = MakeSpan(vec_aligned, num);

  // Decompressed inputs
  using V = hn::Vec<decltype(d)>;
  V v0, v1, v2, v3;

  // State for Kernel
  const hn::Repartition<float, D> df;
  using VF = hn::Vec<decltype(df)>;
  VF sum0 = hn::Zero(d);
  VF sum1 = hn::Zero(d);
  VF sum2 = hn::Zero(d);
  VF sum3 = hn::Zero(d);
  VF comp0 = hn::Zero(d);
  VF comp1 = hn::Zero(d);
  VF comp2 = hn::Zero(d);
  VF comp3 = hn::Zero(d);

  const size_t N = hn::Lanes(d);
  size_t i = 0;
  if (num >= 4 * N) {
    for (; i <= num - 4 * N; i += 4 * N) {
      Decompress2(d, v_span, i + 0 * N, v0, v1);
      Decompress2(d, v_span, i + 2 * N, v2, v3);

      kernel.Update4(d, v0, v1, v2, v3, v0, v1, v2, v3, sum0, sum1, sum2, sum3,
                     comp0, comp1, comp2, comp3);
    }
  }

  size_t remaining = num - i;
  HWY_DASSERT(remaining < 4 * N);
  if (HWY_UNLIKELY(remaining != 0)) {
    HWY_ALIGN float padded_v[4 * hn::MaxLanes(d)];
    DecompressAndZeroPad(d, v_span, i, padded_v, remaining);

    // 1..4 whole vectors, possibly zero-padded.
    for (size_t padded_pos = 0; padded_pos < remaining; padded_pos += N) {
      const VF v0 = hn::Load(d, padded_v + padded_pos);
      kernel.Update1(d, v0, v0, sum0, comp0);
    }
  }

  return kernel.Reduce(d, sum0, sum1, sum2, sum3, comp0, comp1, comp2, comp3);
}

// Functor called for each tensor, which compresses and stores them along with
// their scaling factors to BlobStore.
class Compressor {
 public:
  explicit Compressor(hwy::ThreadPool& pool) : pool_(pool) {}

  template <typename Packed, size_t kCapacity>
  void operator()(const char* name, const float* weights,
                  CompressedArray<Packed, kCapacity>& compressed) {
    Insert(name, weights, kCapacity, work_, compressed.CompressedSize(),
           compressed.data(), 0, pool_);
  }

  template <typename Packed>
  void Insert(const char* name, const float* weights, size_t weights_count,
              CompressWorkingSet& work, size_t out_capacity, Packed* packed,
              size_t packed_ofs, hwy::ThreadPool& pool) {
    fprintf(stderr, "Regenerating %s (%zuM), please wait\n", name,
            weights_count / (1000 * 1000));
    Compress(weights, weights_count, work_,
             PackedSpan<Packed>{packed, weights_count}, 0, pool_);
    writer_.Add(CacheKey<Packed>(name), packed, out_capacity);
  }

  void AddScales(const float* scales, size_t len) {
    if (len) {
      writer_.Add(CacheKey<float>("scales"), scales, len * sizeof(scales[0]));
    }
  }

  void WriteAll(hwy::ThreadPool& pool, const Path& blob_filename) {
    const BlobError err = writer_.WriteAll(pool, blob_filename);
    if (err != 0) {
      fprintf(stderr, "Failed to write blobs to %s (error %d)\n",
              blob_filename.path.c_str(), err);
    }
  }

 private:
  CompressWorkingSet work_;
  hwy::ThreadPool& pool_;
  BlobWriter writer_;
};

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace gcpp
HWY_AFTER_NAMESPACE();

#endif  // NOLINT
