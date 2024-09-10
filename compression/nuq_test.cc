// Copyright 2023 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// SFP uses ConcatEven/Odd which are not supported; skip SVE for faster tests.
#ifndef HWY_DISABLED_TARGETS
#define HWY_DISABLED_TARGETS (HWY_SCALAR | HWY_SVE)
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>  // std::shuffle
#include <random>

#include "compression/distortion.h"
#include "compression/shared.h"
#include "util/test_util.h"
#include "hwy/aligned_allocator.h"
#include "hwy/base.h"
#include "hwy/tests/hwy_gtest.h"
#include "hwy/tests/test_util.h"
#include "hwy/timer.h"

// clang-format off
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "compression/nuq_test.cc"  // NOLINT
// clang-format on
#include "hwy/foreach_target.h"  // IWYU pragma: keep
#include "hwy/highway.h"
// After highway.h
#include "compression/nuq-inl.h"
#include "hwy/tests/test_util-inl.h"

HWY_BEFORE_NAMESPACE();
namespace gcpp {
namespace HWY_NAMESPACE {

static constexpr size_t kTimingReps = hn::AdjustedReps(3);
static constexpr size_t kClusters = NuqStream::kClusters;
static constexpr size_t kGroupSize = NuqStream::kGroupSize;

// All-equal inputs: only one cluster
struct TestFlat {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    // Run this simple test only once to save time/debug output.
    if (!(HWY_ONCE && hn::Lanes(df) == hn::Lanes(hn::ScalableTag<float>()))) {
      return;
    }

    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);
    for (size_t i = 0; i < kGroupSize; ++i) {
      in[i] = 0.5f;
    }
    NuqStream::ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    const size_t unused_clusters = NuqClustering::ClusterExactL2(
        df, in.get(), kGroupSize, buf, centers, indices);
    HWY_ASSERT(unused_clusters == kClusters - 1);

    for (size_t i = 0; i < unused_clusters; ++i) {
      HWY_ASSERT(centers[i] == 0.0f);
    }
    HWY_ASSERT(centers[unused_clusters] == 0.5f);
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] == unused_clusters);
    }
  }
};

void TestAllFlat() { hn::ForGEVectors<64, TestFlat>()(float()); }

// Generate shuffled plateaus, one per cluster
struct TestPlateaus {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    // Run this simple test only once to save time/debug output.
    if (!(HWY_ONCE && hn::Lanes(df) == hn::Lanes(hn::ScalableTag<float>()))) {
      return;
    }

    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);

    for (size_t i = 0; i < kGroupSize; ++i) {
      const size_t idx_cluster = i / (kGroupSize / kClusters);
      HWY_ASSERT(idx_cluster < kClusters);
      in[i] = (1.0f * idx_cluster / kClusters) - 0.5f;
      HWY_ASSERT(-0.5f <= in[i] && in[i] < 0.5f);
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(in.get(), in.get() + kGroupSize, rng);

    NuqStream::ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    const size_t unused_clusters = NuqClustering::ClusterExactL2(
        df, in.get(), kGroupSize, buf, centers, indices);
    HWY_ASSERT(unused_clusters == 0);

    DistortionStats stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] < kClusters);
      stats.Notify(in[i], centers[indices[i]]);
    }
    // Zero error.
    HWY_ASSERT_EQ(kGroupSize, stats.NumExact());
    HWY_ASSERT_EQ(0, stats.NumSignFlip());
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT_EQ(0.0, stats.SumL1());
    HWY_ASSERT_EQ(0.0f, stats.GeomeanValueDivL1());
    HWY_ASSERT_EQ(0.0f, stats.WeightedAverageL1());
    // Input was symmetric and zero-mean.
    HWY_ASSERT(gcpp::IsInside(-0.05, 0.05, stats.Original().Mean()));
    HWY_ASSERT(gcpp::IsNear(0.0, stats.Original().Skewness()));
  }
};

void TestAllPlateaus() { hn::ForGEVectors<64, TestPlateaus>()(float()); }

struct TestRamp {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    // Run this simple test only once to save time/debug output.
    if (!(HWY_ONCE && hn::Lanes(df) == hn::Lanes(hn::ScalableTag<float>()))) {
      return;
    }

    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);

    for (size_t i = 0; i < kGroupSize; ++i) {
      in[i] = (1.0f * i / kGroupSize) - 0.45f;  // slightly asymmetric
      HWY_ASSERT(-0.45f <= in[i] && in[i] < 0.55f);
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(in.get(), in.get() + kGroupSize, rng);

    NuqStream::ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    const size_t unused_clusters = NuqClustering::ClusterExactL2(
        df, in.get(), kGroupSize, buf, centers, indices);
    HWY_ASSERT(unused_clusters == 0);

    DistortionStats stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] < kClusters);
      stats.Notify(in[i], centers[indices[i]]);
    }

    // Low error.
    HWY_ASSERT_EQ(0, stats.NumExact());
    HWY_ASSERT(stats.NumSignFlip() < 10);
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT_EQ(kGroupSize / kClusters / 4.0, stats.SumL1());
    HWY_ASSERT(gcpp::IsInside(17.0, 18.0, stats.GeomeanValueDivL1()));
    HWY_ASSERT(gcpp::IsInside(0.005, 0.010, stats.WeightedAverageL1()));
    HWY_ASSERT(stats.L1().Max() <= 0.04f);
    // Input was symmetric about 0.05.
    HWY_ASSERT(gcpp::IsNear(0.05, stats.Original().Mean(), 0.01));
    HWY_ASSERT(gcpp::IsNear(0.0, stats.Original().Skewness(), 1E-4));
    static_assert(kGroupSize == 256, "Update expected");
  }
};

void TestAllRamp() { hn::ForGEVectors<64, TestRamp>()(float()); }

struct TestNormal {
  template <typename T, class DF>
  HWY_INLINE void operator()(T /*unused*/, DF df) {
    auto in = hwy::AllocateAligned<float>(kGroupSize);
    HWY_ASSERT(in);

    hwy::RandomState rng;
    hwy::Stats in_stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      const double r = RandomGaussian(rng);
      in_stats.Notify(r);
      in[i] = hwy::ConvertScalarTo<T>(r);
    }
    VerifyGaussian(in_stats);

    NuqStream::ClusterBuf buf;
    float centers[kClusters];
    uint16_t indices[kGroupSize];
    double elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < kTimingReps; ++rep) {
      const double t0 = hwy::platform::Now();
      const size_t unused_clusters = NuqClustering::ClusterExactL2(
          df, in.get(), kGroupSize, buf, centers, indices);
      HWY_ASSERT(unused_clusters == 0);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
    }
    fprintf(stderr, "Vec %zu Enc %.2f MB/s\n", Lanes(df) * 4,
            kGroupSize * sizeof(float) * 1E-6 / elapsed);

    DistortionStats stats;
    for (size_t i = 0; i < kGroupSize; ++i) {
      HWY_ASSERT(indices[i] < kClusters);
      stats.Notify(in[i], centers[indices[i]]);
    }

    // Moderate error.
    HWY_ASSERT_EQ(0, stats.NumExact());
    HWY_ASSERT(stats.NumSignFlip() < kGroupSize / kClusters);
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT(gcpp::IsInside(5.0, 6.0, stats.SumL1()));
    HWY_ASSERT(gcpp::IsInside(12.7, 12.8, stats.GeomeanValueDivL1()));
    HWY_ASSERT(gcpp::IsInside(0.036, 0.037, stats.WeightedAverageL1()));
    HWY_ASSERT(stats.L1().Max() <= 0.10f);
    static_assert(kGroupSize == 256, "Update expected");
  }
};

void TestAllNormal() { hn::ForGEVectors<64, TestNormal>()(float()); }

// Can encode and decode sub-regions.
struct TestOffset {
  template <typename T, class D>
  HWY_INLINE void operator()(T /*unused*/, D d) {
    const hn::Repartition<float, D> df;
    const size_t total = 10 * kGroupSize;   // already padded
    const size_t kMidLen = 2 * kGroupSize;  // length of middle piece

    auto in = hwy::AllocateAligned<float>(total);  // Enc() requires f32
    auto dec1 = hwy::AllocateAligned<T>(total);
    auto dec2 = hwy::AllocateAligned<T>(kMidLen);
    auto nuq = hwy::AllocateAligned<NuqStream>(NuqStream::PackedEnd(total));
    HWY_ASSERT(in && dec1 && dec2 && nuq);
    const auto nuq_span = MakeSpan(nuq.get(), total);

    hwy::RandomState rng;
    for (size_t i = 0; i < total; ++i) {
      in[i] = static_cast<float>(RandomGaussian(rng));
    }

    // Encode + decode everything
    NuqStream::ClusterBuf buf;
    (void)NuqCodec::Enc(df, in.get(), total, buf, nuq_span, 0);
    NuqCodec::DecompressAndZeroPad(d, MakeConst(nuq_span), 0, dec1.get(),
                                   total);

    // Overwrite middle with first inputs
    const size_t offset = 5 * kGroupSize;
    (void)NuqCodec::Enc(df, in.get(), kMidLen, buf, nuq_span, offset);

    // Decoded middle now matches previously decoded first
    NuqCodec::DecompressAndZeroPad(d, MakeConst(nuq_span), offset, dec2.get(),
                                   kMidLen);
    for (size_t i = 0; i < kMidLen; ++i) {
      HWY_ASSERT(dec1[i] == dec2[i]);
    }
  }
};

void TestOffsetBF16() { hn::ForGEVectors<128, TestOffset>()(BF16()); }
void TestOffsetF32() { hn::ForGEVectors<128, TestOffset>()(float()); }

struct TestNibble {
  template <typename T, class D>
  HWY_INLINE void operator()(T /*unused*/, D d) {
    const hn::Repartition<uint8_t, D> d8;
    const hn::Half<decltype(d8)> d8h;
    using V = hn::Vec<decltype(d)>;
    using V8 = hn::Vec<decltype(d8)>;
    using V8H = hn::Vec<decltype(d8h)>;
    const V mask = hn::Set(d, 15);

    {
      const V v0 = hn::And(hn::Iota(d, 0), mask);
      const V v1 = hn::Set(d, 1);
      const V v2 = hn::OddEven(v1, hn::Zero(d));
      const V v3 = hn::Reverse(d, v0);
      const V8 nibbles = NibbleCodec::OrderedPackU16(d, v0, v1, v2, v3);
      const V8H nibbles0 = hn::LowerHalf(d8h, nibbles);
      const V8H nibbles1 = hn::UpperHalf(d8h, nibbles);
      const V out0 = NibbleCodec::OrderedUnpackU16<0>(d, nibbles0);
      const V out1 = NibbleCodec::OrderedUnpackU16<1>(d, nibbles0);
      const V out2 = NibbleCodec::OrderedUnpackU16<0>(d, nibbles1);
      const V out3 = NibbleCodec::OrderedUnpackU16<1>(d, nibbles1);
      HWY_ASSERT_VEC_EQ(d, v0, out0);
      HWY_ASSERT_VEC_EQ(d, v1, out1);
      HWY_ASSERT_VEC_EQ(d, v2, out2);
      HWY_ASSERT_VEC_EQ(d, v3, out3);
    }
    // Same, but with different values in each lane.
    {
      const V v0 = hn::And(hn::Iota(d, 0), mask);
      const V v1 = hn::And(hn::Iota(d, 1), mask);
      const V v2 = hn::And(hn::Iota(d, 2), mask);
      const V v3 = hn::And(hn::Iota(d, 3), mask);
      const V8 nibbles = NibbleCodec::OrderedPackU16(d, v0, v1, v2, v3);
      const V8H nibbles0 = hn::LowerHalf(d8h, nibbles);
      const V8H nibbles1 = hn::UpperHalf(d8h, nibbles);
      const V out0 = NibbleCodec::OrderedUnpackU16<0>(d, nibbles0);
      const V out1 = NibbleCodec::OrderedUnpackU16<1>(d, nibbles0);
      const V out2 = NibbleCodec::OrderedUnpackU16<0>(d, nibbles1);
      const V out3 = NibbleCodec::OrderedUnpackU16<1>(d, nibbles1);
      HWY_ASSERT_VEC_EQ(d, v0, out0);
      HWY_ASSERT_VEC_EQ(d, v1, out1);
      HWY_ASSERT_VEC_EQ(d, v2, out2);
      HWY_ASSERT_VEC_EQ(d, v3, out3);
    }
  }
};

void TestAllNibble() {
  const hn::ForGEVectors<128, TestNibble> test;
  test(uint16_t());
}

// Checks the distortion from an encode and decode round trip. Unlike
// `TestShortLengthsT` in compress_test, this covers large `num` and
// prints the enc/dec throughput.
struct TestEncDec {
  template <typename T, class D>
  HWY_INLINE void operator()(T /*unused*/, D d) {
    const hn::Repartition<float, D> df;
    const size_t num = 4 * kGroupSize;
    auto in = hwy::AllocateAligned<float>(num);  // Enc() requires f32
    auto out = hwy::AllocateAligned<T>(num);     // already padded
    auto nuq = hwy::AllocateAligned<NuqStream>(NuqStream::PackedEnd(num));
    HWY_ASSERT(in && out && nuq);
    const auto nuq_span = MakeSpan(nuq.get(), num);

    hwy::RandomState rng;
    hwy::Stats in_stats;
    for (size_t i = 0; i < num; ++i) {
      in[i] = static_cast<float>(RandomGaussian(rng));
      in_stats.Notify(in[i]);
    }
    VerifyGaussian(in_stats);

    NuqStream::ClusterBuf buf;
    double elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < kTimingReps; ++rep) {
      const double t0 = hwy::platform::Now();
      const size_t unused_clusters =
          NuqCodec::Enc(df, in.get(), num, buf, nuq_span, 0);
      HWY_ASSERT(unused_clusters == 0);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
    }
    fprintf(stderr, "Vec %zu Enc %.2f MB/s\n", Lanes(d) * sizeof(T),
            num * sizeof(float) * 1E-6 / elapsed);

    elapsed = hwy::HighestValue<double>();
    for (size_t rep = 0; rep < kTimingReps; ++rep) {
      const double t0 = hwy::platform::Now();
      NuqCodec::DecompressAndZeroPad(d, MakeConst(nuq_span), 0, out.get(), num);
      const double t1 = hwy::platform::Now();
      elapsed = HWY_MIN(elapsed, t1 - t0);
    }
    fprintf(stderr, "Vec %zu Dec %.2f MB/s\n", Lanes(d) * sizeof(T),
            num * sizeof(T) * 1E-6 / elapsed);

    DistortionStats stats;
    for (size_t i = 0; i < num; ++i) {
      stats.Notify(in[i], hwy::ConvertScalarTo<float>(out[i]));
    }

    // Moderate error.
    HWY_ASSERT_EQ(0, stats.NumExact());
    HWY_ASSERT(stats.NumSignFlip() < num / kClusters);
    HWY_ASSERT_EQ(0, stats.NumRoundedToZero());
    HWY_ASSERT(gcpp::IsInside(23.0, 24.0, stats.SumL1()));
    HWY_ASSERT(gcpp::IsInside(13.0, 13.3, stats.GeomeanValueDivL1()));
    HWY_ASSERT(gcpp::IsInside(0.034, 0.035, stats.WeightedAverageL1()));
    HWY_ASSERT(stats.L1().Max() <= 0.11f);
    static_assert(kGroupSize == 256, "Update expected");
  }
};

void TestEncDecBF16() { hn::ForGEVectors<128, TestEncDec>()(BF16()); }
void TestEncDecF32() { hn::ForGEVectors<128, TestEncDec>()(float()); }

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace gcpp
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace gcpp {
HWY_BEFORE_TEST(NuqTest);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllFlat);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllPlateaus);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllRamp);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllNormal);
HWY_EXPORT_AND_TEST_P(NuqTest, TestOffsetBF16);
HWY_EXPORT_AND_TEST_P(NuqTest, TestOffsetF32);
HWY_EXPORT_AND_TEST_P(NuqTest, TestAllNibble);
HWY_EXPORT_AND_TEST_P(NuqTest, TestEncDecBF16);
HWY_EXPORT_AND_TEST_P(NuqTest, TestEncDecF32);
HWY_AFTER_TEST();
}  // namespace gcpp
#endif  // HWY_ONCE
