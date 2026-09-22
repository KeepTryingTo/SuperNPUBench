# topk_tiled — tiled top-k radix-select

`topk_tiled.hpp` (shared core) + `topk_tail_fp32.hpp` / `topk_tail_fp16.hpp`
(boundary refinement, selected by `kFp32Refine`).

Pipeline:

1. **Stage 1** — one 256-bin histogram over each element's **FP16 sortable-key
   high byte** (`key16 >> 8`), suffix-cumsum, threshold `thr`. Elements with
   `bin > thr` are written to `out`; `bin == thr` become the candidate list.
2. **Tail** — refine the candidate bucket into an exact top-k:
   - `kFp32Refine == true`  → four FP32 key-byte rounds (exact FP32 top-k).
   - `kFp32Refine == false` → one FP16 key low-byte round (exact top-k of the
     FP16-rounded values), reusing `round_write`'s final pass; ties in the full
     16-bit FP16 key are broken in candidate order.

Outer shape is runtime (`TopkTilingData {batch, cols, topk}`) validated against
the compile-time maxima `kBatchMax=4`, `kColsMax=131072`, `kTopKMax=1024`,
`kCandCap=65536`.

## The 131072 -> 1024 fix

`Scratch.hist[256]` was immediately followed by `num[0]`. Two things need
`hist[256]` to be a real, per-round-cleared word:

- `round_write` allocates the FP32 top-byte (`0xFF`) slot through
  `H[bin+1]`, i.e. `H[256]`;
- `suffix_cumsum` reads `hist[256:384]` as its zero boundary.

Because `hist[256]` aliased `num[0]` — the same counter the round uses to append
its `== thr` candidates (`nr == 0` for round 1) — `0xFF` slots and the append
stream shared one word, producing duplicate slots and skipped (`-1`) slots.

Why only 131072: it is the first size whose `[127.5, 128)` mass makes FP32
`byte1 == 0xFF` a GT lane in round 1 (65536 and below pass). Fixed by inserting
a per-round-cleared `hist_pad[256]` between `hist` and `num`.

## Why the GM atom add is still hand-written

`mgather_add_s32_m32` keeps an explicit `BSTART.TLSU MGATHER.ADD` block with
literal `1`s for LB0/LB2 instead of the TileOP `MGATHER_ADD` wrapper, even
though LinxISA/Linx-TileOP-API#185 (PR #209) now emits `B.DATR CUBE_M32`:

- the LinxV5 backend folds any value-1 dimension on a TLSU head
  (`LinxV5ExpandPseudoInsts.cpp`: `bundleHeadMayOmitLB0` /
  `omitDefaultInlineAsmDims`), so the wrapper's placeholder LB0 disappears;
- gfrun's gm-atom-red legality still requires an explicit LB0
  (`AccumulateBlockInfo.cpp`, `bdimMask & 1`), so the folded bundle is rejected
  with `illegal MGATHER_ADD operand or descriptor contract`.

That contradicts linx-isa#202 (an omitted LB0 has effective value one; legality
must not require presence), tracked by LinxISA/SuperScalarModel#816. The
literal `1`s survive the fold, so this block is retained until #816 lands; it
can then be replaced by the one-line `MGATHER_ADD` wrapper call.

## Build / run

The outer shape is runtime (`TopkTilingData`), so different batch/cols/topk
need **no recompile** as long as they stay within the compile-time maxima
(`kBatchMax` / `kColsMax` / `kTopKMax`; exceeding them means editing those
constants and `./compile.all` again).

```sh
export COMPILER_DIR=/path/to/linx_blockisa_llvm_musl/bin
cd test/kernel/multi_thread/topk_tiled && ./compile.all

# shipped case: 4 x 8192, topk 512, varied per-row ranges
python3 src/run_topk_tiled_check.py --gfrun <gfrun>

# any runtime shape within the maxima, e.g. 131072 -> 1024
python3 src/run_topk_tiled_check.py --gfrun <gfrun> \
    --rows 1 --cols 131072 --topk 1024 --mode fp32

# fp16 build: same flags, --mode fp16
python3 src/run_topk_tiled_check.py --gfrun <gfrun> \
    --rows 1 --cols 131072 --topk 1024 --mode fp16
```

`--rows/--cols/--topk` override the shipped shape and run each row over the
full `[0, cols)` range. `--mode` must match the compiled `kFp32Refine`: fp16
mode compares the output's FP16 sortable-key multiset against the golden top-K
keys (arbitrary tie-break), fp32 mode compares the exact index set.

## Validation status

gfrun `-s softcore.multiThreadNum=4`, LinxV5 toolchain (TileOP API #209).

| mode | shape | seeds | result |
| --- | --- | --- | --- |
| fp32 | `4x8192x512` | 12345 | PASS |
| fp32 | `1x131072x1024` | 0..20 (21 seeds) | 21/21 PASS |
| fp32 | `2x131072x1024` | 3 | PASS |
| fp16 | `4x8192x512` | 12345 | PASS |
| fp16 | `1x131072x1024` | 0..20 (21 seeds) | 21/21 PASS |
| fp16 | `2x131072x1024` | 3 | PASS |

Also: `test/kernel/multi_thread/gm_atom_dup` PASS (same-address `MGATHER.ADD`
serialization, which the slot allocator relies on); `histogram_cumsum_m32`
PASS (4 rows).
