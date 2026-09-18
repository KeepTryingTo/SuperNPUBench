#pragma once

// Reuse the baseline packed conversion helper without changing the baseline.
#include "fa_2d_unroll_gmma.hpp"

// Two Q blocks reuse each loaded K and V tile.
// Per-Q softmax/PV recurrence still visits j=0..Kb-1 in its original order.
// Q: [kGroupM, qD], K: [kTk, qD], V: [kTk, vD] (logical shared shapes).
// Per-PE score/P: [kPeTm, kTk]; per-Q output state: [kPeTm, vD].
template <typename matrix_dtype, typename vector_dtype, int PackedFactor,
          int Sq, int Skv, int qD, int vD, int kTm, int kTk,
          int scaleD = qD>
void flash_attention_gmma_opt_impl(
    vector_dtype *out_ptr, matrix_dtype *q_ptr, matrix_dtype *k_ptr,
    matrix_dtype *v_ptr) {
    const uint32_t tid = get_thread_idx();
    constexpr int kPeNum = 4;
    constexpr int kStoredQD = qD / PackedFactor;
    constexpr int kStoredSkv = Skv / PackedFactor;
    constexpr int kStoredTk = kTk / PackedFactor;
    constexpr int kGroupM = kTm <= 128 ? kTm : 128;
    constexpr int kPeTm = kGroupM <= 64 ? 16 : 32;
    // Change 1: physical Rows must be 128 or 64; ValidRow = actual kGroupM.
    constexpr int kTileRows = (kGroupM <= 64) ? 64 : 128;
    // Change 2: QK uses the full head dimension (no K chunking).
    constexpr int kPVStoredChunk = kStoredTk;
    static_assert(kTm % kGroupM == 0 && Sq % kGroupM == 0,
                  "Tm and Sq must be divisible by cooperative group_M");
    static_assert(PackedFactor == 1 || PackedFactor == 2,
                  "PackedFactor must be 1 (FP8+) or 2 (packed FP4x2)");
    static_assert(qD % PackedFactor == 0 && kTk % PackedFactor == 0,
                  "logical matrix dimensions must be divisible by PackedFactor");
    static_assert(kGroupM >= 1 && kGroupM <= 128,
                  "cooperative group_M must be in the range 1..128");

    // GM tensors are RowMajor in their natural [rows, cols] orientation.
    // K is [Skv, qD] with the head dimension contiguous.
    using gmQ = global_tensor<matrix_dtype, RowMajor<Sq, kStoredQD>>;
    using gmK = global_tensor<matrix_dtype, RowMajor<Skv, kStoredQD>>;
    using gmV = global_tensor<matrix_dtype, RowMajor<kStoredSkv, vD>>;
    using gmO = global_tensor<vector_dtype, RowMajor<Sq, vD>>;

    // Change 1: SharedMatrixLeft uses kTileRows (128/64) for physical Rows,
    // kGroupM for ValidRow.
    using tileQMatrix =
        SharedMatrixLeft<matrix_dtype, kTileRows, kStoredQD,
                         kGroupM, kStoredQD>;
    // K tile is physically [N=kTk, K=qD/PackedFactor]. Shared B without
    // transpose_b interprets this as the right operand of QK^T.
    using tileKMatrix =
        SharedMatrixRight<matrix_dtype, kTk, kStoredQD>;
    // V tile [kPVStoredChunk, vD] - natural [K, N] right-operand shape.
    using tileVMatrix =
        SharedMatrixRight<matrix_dtype, kPVStoredChunk, vD>;
    using tileQ = SharedTile<tileQMatrix>;
    using tileK = SharedTile<tileKMatrix>;
    using tileV = SharedTile<tileVMatrix>;

    // Change 8: tileW is a CubeTileM32/M16 (Location::Left) instead of
    // CubeAccumulatorM32/M16 (Location::Acc). The QK TMATMUL writes directly
    // to a Left tile, and tW is used as the PV TMATMUL left operand without
    // any TCVT (for FP32). For packed types a Left->Left TCVT remains.
    using tileWM16 = CubeTileM16<vector_dtype, kPeTm, kTk>;
    using tileWM32 = CubeTileM32<vector_dtype, kPeTm, kTk>;
    using tileW = std::conditional_t<(kPeTm <= 16), tileWM16, tileWM32>;

    using tilePVM16 = CubeAccumulatorM16<float, kPeTm, vD>;
    using tilePVM32 = CubeAccumulatorM32<float, kPeTm, vD>;
    using tilePVCube =
        std::conditional_t<(kPeTm <= 16), tilePVM16, tilePVM32>;
    using tileO = tilePVCube;

    // P shard: local CUBE Left tile for non-FP32 type conversion.
    // For FP32 (PackedFactor==1, matrix_dtype==float), tW is already a Left
    // float tile and is used directly as the PV TMATMUL left operand.
    using tilePShardM16 = CubeTileM16<matrix_dtype, kPeTm, kStoredTk>;
    using tilePShardM32 = CubeTileM32<matrix_dtype, kPeTm, kStoredTk>;
    using tilePShard =
        std::conditional_t<(kPeTm <= 16), tilePShardM16, tilePShardM32>;

    using tileOCastM16 = CubeAccumulatorM16<vector_dtype, kPeTm, vD>;
    using tileOCastM32 = CubeAccumulatorM32<vector_dtype, kPeTm, vD>;
    using tileOCast =
        std::conditional_t<(kPeTm <= 16), tileOCastM16, tileOCastM32>;

    // PTO #311: TROWSUM/TROWMAX require the dst to have the same physical
    // storage as the src (destinationShape = [cellRows, source.col]).
    // tileReduce mirrors tileW's [kPeTm, kTk] shape with ValidCol=1.
    using tileReduceM16 = VecTileM16<vector_dtype, kPeTm, kTk, kPeTm, 1>;
    using tileReduceM32 = VecTileM32<vector_dtype, kPeTm, kTk, kPeTm, 1>;
    using tileReduce =
        std::conditional_t<(kPeTm <= 16), tileReduceM16, tileReduceM32>;

    // TROWEXPAND* require a single-column (Cols=1) broadcast source.  tileMax
    // describes the first CELL exposed by TREDUCEPREFIXVIEW and also stores
    // the persistent online-softmax state.
    using tileMaxM16 = VecTileM16<vector_dtype, kPeTm, 1, kPeTm, 1>;
    using tileMaxM32 = VecTileM32<vector_dtype, kPeTm, 1, kPeTm, 1>;
    using tileMax =
        std::conditional_t<(kPeTm <= 16), tileMaxM16, tileMaxM32>;
    using tileSum = tileMax;
    using tileScale = tileMax;

    using itQ = global_iterator<gmQ, tileQMatrix>;
    using itK = global_iterator<gmK, tileKMatrix>;
    using itV = global_iterator<gmV, tileVMatrix>;
    using itO = global_iterator<gmO, tileOCast>;

    itQ gIterQ(q_ptr);
    itK gIterK(k_ptr);
    itV gIterV(v_ptr);
    itO gIterO(out_ptr);

    // Two Q tiles remain live while a K/V pair is reused by both Q blocks.
    // This bounds explicitly retained shared tiles, not compiler temporaries.
    constexpr int kSharedTRegBytes = 256 * 1024;
    constexpr int kQSlots = Sq / kGroupM >= 2 ? 2 : 1;
    constexpr int kSharedBytes =
        kQSlots * tileQMatrix::LogicalTileBytes +
        tileKMatrix::LogicalTileBytes + tileVMatrix::LogicalTileBytes;
    static_assert(kSharedBytes <= kSharedTRegBytes,
                  "Retained Q pair and K/V exceed the 256 KiB SharedTReg pool");
    static_assert(Skv > 0 && Skv % kTk == 0,
                  "This variant requires complete K/V tiles");

    const float scale = 1.0f / sqrt((float)scaleD);
    constexpr int Qb = Sq / kGroupM;
    constexpr int Kb = (Skv + kTk - 1) / kTk;

    // Loop-invariant fixpipe options - hoisted outside both loops.
    constexpr auto qkOptions = fixp::keep_acc();
    constexpr auto pvOptions = fixp::keep_acc().transpose_b();

    // Pair Q blocks so the persistent local state stays bounded for large Sq.
#pragma clang loop unroll(full)
    for (int qBase = 0; qBase < Qb; qBase += kQSlots) {
        tileQ qTiles[kQSlots];
        // Slot indexes Q blocks within this PE, not other PEs' private tiles.
        tileMax maxima[kQSlots];
        tileSum sums[kQSlots];
        tileO outputs[kQSlots];
#pragma clang loop unroll(full)
        for (int slot = 0; slot < kQSlots; ++slot) {
            if (qBase + slot < Qb) {
                TEXPANDS(maxima[slot], -1e30f);
                TEXPANDS(sums[slot], 0.0f);
            }
        }

#pragma clang loop unroll(full)
        for (int j = 0; j < Kb; ++j) {
            // Load B first. Both Q blocks consume exactly this K tile.
            tileK tK;
            auto gK = gIterK(j, 0);
            TLOAD<tileKMatrix, 1>(tK, gK);

            // Q is loaded only on the first K iteration, then retained.
#pragma clang loop unroll(full)
            for (int slot = 0; slot < kQSlots; ++slot) {
                if (j == 0 && qBase + slot < Qb) {
                    auto gQ = gIterQ(qBase + slot, 0);
                    TLOAD<tileQMatrix, 1>(qTiles[slot], gQ);
                }
            }

            // Load V once before either Q path; both PV operations reuse it.
            tileV tV;
            auto gV = gIterV(j, 0);
            TLOAD<tileVMatrix, 1>(tV, gV);

            // Submit both QK products before either PV. Each slot retains
            // its own local score tile for the following softmax/PV phase.
            tileW scores[kQSlots];
#pragma clang loop unroll(full)
            for (int slot = 0; slot < kQSlots; ++slot) {
                if (qBase + slot < Qb) {
                    TMATMUL(scores[slot], qTiles[slot], tK, qkOptions);
                }
            }

#pragma clang loop unroll(full)
            for (int slot = 0; slot < kQSlots; ++slot) {
                if (qBase + slot < Qb) {
                    auto &tMax = maxima[slot];
                    auto &tSum = sums[slot];
                    auto &tO = outputs[slot];
                    auto &tW = scores[slot];

                    // Scale
                    TMULS(tW, tW, scale);

                    // --- Softmax ---
                    tileReduce tLocalMaxR;
                    TROWMAX(tLocalMaxR, tW);
                    auto tLocalMax = TREDUCEPREFIXVIEW<tileMax>(tLocalMaxR);

                    tileMax tNewMax;
                    tileScale tScale;
                    // tMax starts at the softmax sentinel, so TMAX can consume the
                    // reduction prefix directly for both the first and later blocks.
                    TMAX(tNewMax, tMax, tLocalMax);
                    if (j != 0) {
                        // Change 7: TROWEXPANDEXPDIF replaces TSUB+TEXP (j!=0 only)
                        TROWEXPANDEXPDIF(tScale, tMax, tNewMax);
                        // Rescale old output before PV accumulation
                        TROWEXPANDMUL(tO, tO, tScale);
                    }

                    // Exponentiate current scores (common to both branches)
                    TROWEXPANDEXPDIF(tW, tW, tNewMax);

                    tileReduce tLocalSumR;
                    TROWSUM(tLocalSumR, tW);
                    auto tLocalSum = TREDUCEPREFIXVIEW<tileSum>(tLocalSumR);

                    tileSum tNewSum;
                    if (j == 0) {
                        TADD(tNewSum, tSum, tLocalSum);
                    } else {
                        // There is no TFMA reduction-prefix overload yet.  Preserve
                        // the zero-copy localSum path with separate MUL and ADD; this
                        // uses separate rather than fused rounding.
                        tileSum tScaledSum;
                        TMUL(tScaledSum, tSum, tScale);
                        TADD(tNewSum, tScaledSum, tLocalSum);
                    }

                    // --- PV matmul ---
                    // Change 8: tW is already a Left tile - use directly as PV left
                    // operand (FP32). For packed types, TCVT to a local-Left shard.
                    if constexpr (PackedFactor == 1 &&
                                  std::is_same_v<matrix_dtype, float>) {
                        // FP32: tW is already Left, float - no TCVT
                        if (j == 0) {
                            TMATMUL(tO, tW, tV, pvOptions, kGroupM);
                        } else {
                            TMATMUL_ACC(tO, tO, tW, tV, pvOptions, kGroupM);
                        }
                    } else {
                        // Non-FP32 or packed: TCVT to local-Left CUBE shard
                        tilePShard tPShard;
                        if constexpr (PackedFactor == 2) {
                            fa_tcvt_packed_x2(tPShard, tW);
                        } else {
                            TCVT(tPShard, tW);
                        }
                        if (j == 0) {
                            TMATMUL(tO, tPShard, tV, pvOptions, kGroupM);
                        } else {
                            TMATMUL_ACC(tO, tO, tPShard, tV, pvOptions, kGroupM);
                        }
                    }

                    tMax = tNewMax;
                    tSum = tNewSum;
                }
            }
        }

        // Normalize only after all K/V tiles; output mapping is unchanged.
#pragma clang loop unroll(full)
        for (int slot = 0; slot < kQSlots; ++slot) {
            if (qBase + slot < Qb) {
                const int i = qBase + slot;
                auto &tO = outputs[slot];
                auto &tSum = sums[slot];
                TROWEXPANDDIV(tO, tO, tSum);
                auto dstO = gIterO(i * kPeNum + tid, 0);
                if constexpr (std::is_same_v<vector_dtype, float>) {
                    TSTORE_CUBE(dstO, tO);
                } else {
                    tileOCast tOCast;
                    TCVT(tOCast, tO);
                    TSTORE_CUBE(dstO, tOCast);
                }
            }
        }
    }
}
