#include "basic_op/fa/fa_gmma_dynamic.hpp"

#include <cstdint>
#include <cmath>
#include "benchmark.h"
#include "fileop.h"

#ifndef Tsq
#define Tsq 128
#endif
#ifndef Tskv
#define Tskv 128
#endif
#ifndef FA_MAX_SQ
#define FA_MAX_SQ 256
#endif
#ifndef FA_MAX_SKV
#define FA_MAX_SKV 256
#endif
#ifndef Tm
#define Tm 128
#endif
#ifndef Tk
#define Tk 64
#endif
#ifndef FA_QD
#define FA_QD 128
#endif
#ifndef FA_VD
#define FA_VD 128
#endif

static_assert(Tsq <= FA_MAX_SQ && Tskv <= FA_MAX_SKV);
constexpr int kGroupM = Tm <= 128 ? Tm : 128;
static_assert(Tsq % kGroupM == 0 && FA_MAX_SQ % kGroupM == 0);
static_assert(Tskv % Tk == 0 && FA_MAX_SKV % Tk == 0);

// Two different shapes are executed by the same ELF. The buffers for both
// cases are distinct, so all PEs can advance without a per-case barrier.
alignas(4096) static float q[2][FA_MAX_SQ * FA_QD];
alignas(4096) static float k[2][FA_MAX_SKV * FA_QD];
alignas(4096) static float v[2][FA_MAX_SKV * FA_VD];
alignas(4096) static float out[2][FA_MAX_SQ * FA_VD];
static FaGmmaTilingData shapes[2];
static volatile uint32_t inputs_ready = 0;
static volatile uint32_t shape_invalid = 0;
static volatile uint32_t done[4] = {};
volatile int fa_gmma_dynamic_errors = 0;

int main() {
    const uint32_t tid = get_thread_idx();
    if (tid == 0) {
        shapes[0] = {Tsq, Tskv};
        shapes[1] = {FA_MAX_SQ, FA_MAX_SKV};
#ifdef RES_CHECK
        // shape.bin: two consecutive {int64_t sq, int64_t skv} records.
        // Replacing this file changes the shapes without rebuilding the ELF.
        readBinaryFile(CHK_DIR "/shape.bin", reinterpret_cast<uint8_t *>(shapes),
                       sizeof(shapes));
#endif
        // Check that each runtime shape fits the preallocated buffers and
        // consists of complete cooperative Q/KV tiles.
        for (int c = 0; c < 2; ++c) {
            if (shapes[c].sq <= 0 || shapes[c].skv <= 0 ||
                shapes[c].sq > FA_MAX_SQ || shapes[c].skv > FA_MAX_SKV ||
                shapes[c].sq % kGroupM != 0 || shapes[c].skv % Tk != 0)
                shape_invalid = 1;
        }
        // Fill deterministic inputs for the numerical check below: only Q's
        // first head element is 1; K's first element varies by row; every V
        // column has the same row-dependent value. The resulting output has
        // a simple weighted-average reference for both runtime shapes.
        if (shape_invalid == 0) for (int c = 0; c < 2; ++c) {
            for (int row = 0; row < shapes[c].sq; ++row)
                q[c][row * FA_QD] = 1.0f;
            for (int row = 0; row < shapes[c].skv; ++row) {
                k[c][row * FA_QD] = static_cast<float>(row % 5);
                for (int col = 0; col < FA_VD; ++col) {
                    v[c][row * FA_VD + col] = static_cast<float>(row % 7);
                }
            }
        }
        inputs_ready = 1;
    } else {
        while (inputs_ready == 0) {}
    }

    if (shape_invalid != 0) return 1;

    BENCHSTART;
    for (int c = 0; c < 2; ++c) {
        if (!fa_gmma_dynamic<FA_QD, FA_VD, Tm, Tk>(
                out[c], q[c], k[c], v[c], &shapes[c])) {
            fa_gmma_dynamic_errors = fa_gmma_dynamic_errors + 1;
        }
    }
    BENCHEND;

    done[tid] = 1;
    if (tid == 0) {
        for (int pe = 0; pe < 4; ++pe) {
            while (done[pe] == 0) {}
        }
        for (int c = 0; c < 2; ++c) {
            float numerator = 0.0f;
            float denominator = 0.0f;
            const float scale = 1.0f / sqrtf(static_cast<float>(FA_QD));
            for (int row = 0; row < shapes[c].skv; ++row) {
                const float weight = expf(static_cast<float>(row % 5) * scale);
                numerator += weight * static_cast<float>(row % 7);
                denominator += weight;
            }
            const float expected = numerator / denominator;
            for (int row = 0; row < shapes[c].sq; ++row) {
                for (int col = 0; col < FA_VD; ++col) {
                    const float diff = out[c][row * FA_VD + col] - expected;
                    if (diff < -0.02f || diff > 0.02f)
                        fa_gmma_dynamic_errors = fa_gmma_dynamic_errors + 1;
                }
            }
        }
    }
    return fa_gmma_dynamic_errors != 0;
}
