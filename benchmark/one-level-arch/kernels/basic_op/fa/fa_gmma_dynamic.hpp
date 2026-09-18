#pragma once

#include <common/pto_tileop.hpp>
#include <cmath>
#include <cstdint>
#include <type_traits>

using namespace pto;

// Sq/Skv are read at runtime; qD/vD and tile capacities are compile-time.
// Q/K/V/O are tightly packed row-major tensors.
struct FaGmmaTilingData {
    int64_t sq;
    int64_t skv;
};

template <int qD, int vD, int kTm, int kTk>
__attribute__((noinline)) bool fa_gmma_dynamic(
    float *out_ptr, const float *q_ptr, const float *k_ptr,
    const float *v_ptr, const FaGmmaTilingData *tiling) {
    constexpr int kPeNum = 4;
    constexpr int kGroupM = kTm <= 128 ? kTm : 128;
    constexpr int kPeTm = kGroupM <= 64 ? 16 : 32;
    constexpr int kTileRows = kGroupM <= 64 ? 64 : 128;
    static_assert(kTm > 0 && kTk > 0 && kTm % kGroupM == 0);
    static_assert(kPeTm * kPeNum == kGroupM,
                  "dynamic FA requires 64- or 128-row cooperative groups");

    const int64_t sq = tiling->sq;
    const int64_t skv = tiling->skv;
    const uint32_t tid = get_thread_idx();
    if (sq <= 0 || skv <= 0 || sq % kGroupM != 0 ||
        skv % kTk != 0 || tid >= kPeNum) return false;

    using gmQ = global_tensor<float, RowMajor<-1, qD>>;
    using gmK = global_tensor<float, RowMajor<-1, qD>>;
    using gmV = global_tensor<float, RowMajor<-1, vD>>;
    using gmO = global_tensor<float, RowMajor<-1, vD>>;
    using qMatrix = SharedMatrixLeft<float, kTileRows, qD, kGroupM, qD>;
    using kMatrix = SharedMatrixRight<float, kTk, qD>;
    using vMatrix = SharedMatrixRight<float, kTk, vD>;
    using qTile = SharedTile<qMatrix>;
    using kTile = SharedTile<kMatrix>;
    using vTile = SharedTile<vMatrix>;
    using scoreTile = std::conditional_t<(kPeTm <= 16),
        CubeTileM16<float, kPeTm, kTk>,
        CubeTileM32<float, kPeTm, kTk>>;
    using outputTile = std::conditional_t<(kPeTm <= 16),
        CubeAccumulatorM16<float, kPeTm, vD>,
        CubeAccumulatorM32<float, kPeTm, vD>>;
    using reduceTile = std::conditional_t<(kPeTm <= 16),
        VecTileM16<float, kPeTm, kTk, kPeTm, 1>,
        VecTileM32<float, kPeTm, kTk, kPeTm, 1>>;
    using stateTile = std::conditional_t<(kPeTm <= 16),
        VecTileM16<float, kPeTm, 1, kPeTm, 1>,
        VecTileM32<float, kPeTm, 1, kPeTm, 1>>;
    static_assert(qMatrix::LogicalTileBytes + kMatrix::LogicalTileBytes
                  <= 256 * 1024);
    static_assert(vMatrix::LogicalTileBytes <= 256 * 1024);

    using qIterator = global_iterator<gmQ, qMatrix>;
    using kIterator = global_iterator<gmK, kMatrix>;
    using vIterator = global_iterator<gmV, vMatrix>;
    using oIterator = global_iterator<gmO, outputTile>;
    qIterator gIterQ(const_cast<float *>(q_ptr));
    kIterator gIterK(const_cast<float *>(k_ptr));
    vIterator gIterV(const_cast<float *>(v_ptr));
    oIterator gIterO(out_ptr);

    const int64_t qb = sq / kGroupM;
    const int64_t kb = skv / kTk;
    const float scale = 1.0f / sqrt(static_cast<float>(qD));
    for (int64_t i = 0; i < qb; ++i) {
        // The row count is dynamic, while the compact Q row stride is qD.
        auto gQ = gIterQ(static_cast<int>(i), 0);

        stateTile max, sum;
        outputTile out;
        TEXPANDS(max, -1e30f);
        TEXPANDS(sum, 0.0f);
        for (int64_t j = 0; j < kb; ++j) {
            qTile q;
            TLOAD<qMatrix, 1>(q, gQ);
            auto gK = gIterK(static_cast<int>(j), 0);
            auto gV = gIterV(static_cast<int>(j), 0);
            kTile k;
            vTile v;
            TLOAD<kMatrix, 1>(k, gK);
            TLOAD<vMatrix, 1>(v, gV);
            scoreTile score;
            TMATMUL(score, q, k, fixp::keep_acc());
            TMULS(score, score, scale);

            reduceTile local_max_r;
            TROWMAX(local_max_r, score);
            auto local_max = TREDUCEPREFIXVIEW<stateTile>(local_max_r);
            stateTile new_max;
            TMAX(new_max, max, local_max);
            stateTile old_scale;
            if (j != 0) {
                TROWEXPANDEXPDIF(old_scale, max, new_max);
                TROWEXPANDMUL(out, out, old_scale);
            }
            TROWEXPANDEXPDIF(score, score, new_max);
            reduceTile local_sum_r;
            TROWSUM(local_sum_r, score);
            auto local_sum = TREDUCEPREFIXVIEW<stateTile>(local_sum_r);
            stateTile new_sum;
            if (j == 0) {
                TADD(new_sum, sum, local_sum);
                TMATMUL(out, score, v,
                        fixp::keep_acc().transpose_b(), kGroupM);
            } else {
                stateTile scaled_sum;
                TMUL(scaled_sum, sum, old_scale);
                TADD(new_sum, scaled_sum, local_sum);
                TMATMUL_ACC(out, out, score, v,
                           fixp::keep_acc().transpose_b(), kGroupM);
            }
            max = new_max;
            sum = new_sum;
        }
        TROWEXPANDDIV(out, out, sum);
        auto gO = gIterO(static_cast<int>(i * kPeNum + tid), 0);
        TSTORE_CUBE(gO, out);
    }
    return true;
}
