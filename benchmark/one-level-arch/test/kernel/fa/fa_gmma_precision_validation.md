# FA GMMA 数值精度验证报告

> 验证日期：2026-09-23
>
> 算子：`fa_2d_unroll_gmma`
>
> 功能模型：`SuperScalarModel-asl/bin/gfrun`

## 1. 结论

以下 FP32 长 KV 序列配置通过 host golden 数值校验：

| Cube / Vector 类型 | Shape | Tile | KV blocks | 判定 | max_abs | MSE | mismatch |
| --- | --- | --- | ---: | --- | ---: | ---: | ---: |
| FP32 / FP32 | Sq=128, Skv=1024, QD=VD=128 | Tm=128, Tk=128 | 8 | PASS | 1.8160790e-08 | 6.6770790e-18 | 0/16384 |

判定阈值为 `atol=1e-5, rtol=1e-5`。功能模型正常到达 benchmark 终点并报告
`R2 = 0`；`res.bin` 全部为有限值且不是全零输出。

该配置的 `Skv/Tk = 8`，因此不仅覆盖单次 QK/PV 计算，还覆盖 online-softmax
跨 KV block 的 running max、旧输出重缩放、running sum 和 `TMATMUL_ACC` 累加。

## 2. 校验方法

验证脚本为 [`src/gfrun_fa.py`](src/gfrun_fa.py)，流程与 MatMul 的
host-golden 校验一致：

1. 使用 `res_check=on` 编译，令 PE0 从 `CHK_DIR` 读取输入并在四 PE 完成后写出
   `res.bin`；
2. 使用固定 seed 123 生成非平凡随机 FP32 输入，分布为 `N(0, 0.1)` 并裁剪到
   `[-1, 1]`；
3. 输入采用 RowMajor：Q `[Sq,QD]`、K `[Skv,QD]`、V `[Skv,VD]`；
4. 使用 PyTorch FP32 计算：

   ```python
   golden = torch.softmax((q @ k.T) / sqrt(QD), dim=-1) @ v
   ```

5. 从 `SuperScalarModel-asl` 根目录执行四 PE `gfrun`；
6. 检查退出码、`Reach the End of Benchmark`、`R2 = 0`，再使用
   `numpy.allclose` 对比 `res.bin` 和 `golden.bin`，同时报告 MSE、最大绝对误差和
   mismatch 数量。

`R2 = 0` 只作为功能模型正常结束的判据；最终数值 PASS 由独立的 host golden
比较决定。

## 3. 复现

必须使用主 `linx-toolchain-build` checkout：

```bash
cd /Users/blacktraker/Programming/gitproj/DV4/SuperNPUBench

export COMPILER_DIR=/Users/blacktraker/Programming/gitproj/DV4/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin

make -C benchmark/one-level-arch/test/kernel/fa -B diss \
  TESTCASE=fa_2d_unroll_gmma \
  FA_MODE=FP32_VECFP32 \
  Sq=128 Skv=1024 QD=128 VD=128 \
  Tm=128 Tk=128 X_dim=1 Y_dim=1 \
  res_check=on \
  OBJ_ROOT=/tmp/fa_gmma_numeric_fp32_sq128_skv1024 \
  COMPILER_DIR="$COMPILER_DIR"

python3 benchmark/one-level-arch/test/kernel/fa/src/gfrun_fa.py \
  --elf /tmp/fa_gmma_numeric_fp32_sq128_skv1024/kernel/fa/elf/\
kernel_fa_fa_2d_unroll_gmma_Sq128_Skv1024_Tm128_Tk128_X1_Y1_CubeFP32_VectorFP32.elf
```

脚本默认使用：

```text
/Users/blacktraker/Programming/gitproj/DV4/SuperScalarModel-asl/bin/gfrun
```

也可以通过 `--gfrun-root` 指定另一个 ASL checkout。

## 4. 本次环境与证据

本次原始验证使用：

```text
linx-toolchain-build: e6a31efb4cfb
SuperScalarModel-asl: 3ddb05f9b0cb
ELF SHA256: 93c0ccf7ecfcb3a0d03e107a77771d347122cc14b9136188ff3b8b1d5faed348
```

校验产物写到：

```text
benchmark/one-level-arch/compare/
  kernel_fa_fa_2d_unroll_gmma_Sq128_Skv1024_Tm128_Tk128_X1_Y1_CubeFP32_VectorFP32/
```

其中包括 `srcq.bin`、`srck.bin`、`srcv.bin`、`golden.bin`、`res.bin`、
`gfrun.log` 和 `golden_compare.log`。这些是运行产物，不提交到仓库。

## 5. 当前边界

当前脚本有意只接受 ELF 名称中明确标记为 `CubeFP32_VectorFP32` 的
`fa_2d_unroll_gmma`。BF16、FP8、MXFP8 和 MXFP4 需要先按 DUT 的实际 payload、
舍入及 scale 编码生成输入和 golden，不能直接复用 FP32 输入路径。
