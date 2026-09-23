# FA GMMA 数值精度验证报告

> 验证日期：2026-09-23
>
> 算子：`fa_2d_unroll_gmma`
>
> 功能模型：`SuperScalarModel-asl/bin/gfrun`

## 1. 结论

`Sq=128, Skv=1024, QD=VD=128, Tm=Tk=128` 下，FP32、BF16、FP16、FP8
以及 unity-scale MXFP8 均通过 host golden 数值校验：

| Cube / Vector 类型 | KV blocks | atol / rtol | 判定 | max_abs | MSE | mismatch |
| --- | ---: | ---: | --- | ---: | ---: | ---: |
| FP32 / FP32 | 8 | 1e-5 / 1e-5 | PASS | 1.8160790e-08 | 6.6770790e-18 | 0/16384 |
| BF16 / FP32 | 8 | 1e-4 / 1e-4 | PASS | 1.3742130e-05 | 1.3180262e-11 | 0/16384 |
| FP16 / FP32 | 8 | 1e-5 / 1e-5 | PASS | 2.0416919e-06 | 2.0961903e-13 | 0/16384 |
| FP8 E4M3 / FP32 | 8 | 1e-3 / 1e-3 | PASS | 3.0529127e-04 | 5.8589954e-09 | 0/16384 |
| MXFP8 E4M3 + unity E8M0 / FP32 | 8 | 1e-3 / 1e-3 | PASS | 3.0529127e-04 | 5.8589954e-09 | 0/16384 |

五个用例均正常到达 benchmark 终点、报告 `R2 = 0`，输出全部为有限值且不是
全零。`Skv/Tk = 8`，因此测试覆盖 online-softmax 跨 KV block 的 running max、
旧输出重缩放、running sum 和 `TMATMUL_ACC` 累加。

## 2. 校验脚本

验证脚本为 [`src/gfrun_fa.py`](src/gfrun_fa.py)，支持从 ELF 文件名解析：

```text
CubeFP32_VectorFP32
CubeBF16_VectorFP32
CubeFP16_VectorFP32
CubeFP8_VectorFP32
CubeMXFP8_VectorFP32
```

脚本执行以下步骤：

1. 使用固定 seed 123 生成非平凡随机 FP32 源数据，分布为 `N(0, 0.1)`，并裁剪到
   `[-1, 1]`；
2. 编码成 DUT 实际输入格式；
3. 从实际 payload 解码回 FP32，避免把量化前 FP32 当作低精度 DUT 输入；
4. 使用 PyTorch FP32 计算：

   ```python
   golden = torch.softmax((q @ k.T) / sqrt(QD), dim=-1) @ v
   ```

5. 从 `SuperScalarModel-asl` 根目录运行四 PE `gfrun`；
6. 检查退出码、`Reach the End of Benchmark` 和 `R2 = 0`；
7. 使用 `numpy.allclose` 比较 `res.bin` 与 `golden.bin`，并报告 MSE、最大绝对误差、
   mismatch 数量、有限值和全零检查。

`R2 = 0` 只代表功能模型正常结束，最终 PASS 由独立 host golden 比较决定。

## 3. 各类型输入语义

| 类型 | 写入 DUT 的 payload | Golden 使用的值 |
| --- | --- | --- |
| FP32 | IEEE FP32 | 原始 FP32 payload |
| BF16 | PyTorch FP32→BF16 后的原始 U16 bits | BF16 payload 解码为 FP32 |
| FP16 | NumPy FP16 bytes | FP16 payload 解码为 FP32 |
| FP8 | PyTorch E4M3FN 原始 U8 bits | E4M3 payload 解码为 FP32 |
| MXFP8 | E4M3 U8 payload + E8M0 `0x7f` unity scale | E4M3 payload 解码为 FP32 |

Q、K、V 的逻辑 RowMajor shape 分别为 `[Sq,QD]`、`[Skv,QD]`、
`[Skv,VD]`。输出和 host golden 均为 `[Sq,VD]` FP32。

低精度 kernel 会在 PV 边界把 softmax probability 转成相应 Cube 输入类型；host
golden 计算标准 FP32 attention，因此各类型使用独立容差评估最终输出误差。

## 4. MXFP8 当前边界

当前 `fa_2d_unroll_gmma.cpp` 会在 `USE_MX=1` 时读取 `srcq_scale.bin`、
`srck_scale.bin` 和 `srcv_scale.bin`，但调用
`flash_attention_2d_unroll_shared_impl` 时没有传入 scale 指针。当前实现和反汇编使用
普通 `TMATMUL/TMATMUL_ACC`，没有 `TMATMUL_MX/TMATMUL_MX_ACC`。

因此本报告的 MXFP8 行只能证明：

- MXFP8 名称对应的 E4M3 payload 在 unity E8M0 scale 下数值通过；
- `RES_CHECK` MX scale 文件读取路径可运行。

它**不能**证明非单位 E8M0 group scale 已被 QK/PV 计算正确消费。FP8 与 MXFP8 的
本次误差完全相同也符合当前实现。要验证完整 MXFP8 语义，需要先让 kernel 显式加载
scale tile 并调用 `TMATMUL_MX`，再加入非均匀 scale golden。

## 5. 复现

必须使用主 `linx-toolchain-build` checkout：

```bash
cd /Users/blacktraker/Programming/gitproj/DV4/SuperNPUBench

export COMPILER_DIR=/Users/blacktraker/Programming/gitproj/DV4/linx-toolchain-build/output/linx_blockisa_llvm_musl/bin

make -C benchmark/one-level-arch/test/kernel/fa \
  precision-check \
  COMPILER_DIR="$COMPILER_DIR" \
  OBJ_ROOT=/tmp/fa_gmma_precision
```

`precision-check` 会依次强制重编译并运行五种类型，最终打印：

```text
FA GMMA precision summary: PASS=5 FAIL=0
```

也可以只编译并验证单个配置：

```bash
make -C benchmark/one-level-arch/test/kernel/fa \
  golden-check \
  TESTCASE=fa_2d_unroll_gmma FA_MODE=BF16_VECFP32 \
  Sq=128 Skv=1024 QD=128 VD=128 Tm=128 Tk=128 X_dim=1 Y_dim=1 \
  COMPILER_DIR="$COMPILER_DIR" \
  OBJ_ROOT=/tmp/fa_gmma_bf16_precision
```

或直接校验已经带 `res_check=on` 编译的 ELF：

```bash
python3 benchmark/one-level-arch/test/kernel/fa/src/gfrun_fa.py \
  --elf /tmp/fa_gmma_numeric_BF16_VECFP32_sq128_skv1024/kernel/fa/elf/\
kernel_fa_fa_2d_unroll_gmma_Sq128_Skv1024_Tm128_Tk128_X1_Y1_CubeBF16_VectorFP32.elf
```

脚本默认使用：

```text
/Users/blacktraker/Programming/gitproj/DV4/SuperScalarModel-asl/bin/gfrun
```

可以用 `--gfrun-root` 指定另一个 ASL checkout，用 `--atol/--rtol` 覆盖类型默认
阈值。

## 6. 环境与 ELF 校验和

```text
linx-toolchain-build: e6a31efb4cfb
SuperScalarModel-asl: 3ddb05f9b0cb

FP32:  8819633174e2d0cc98fd27694f4a96b54d8870e2f8a149adb196bf6ff2d635f2
BF16:  76d44cf1df17ce64bfafaf88a6a6f6b76e4e9b3d1c490220ba5509c0c8f77c03
FP16:  8d68ee4f23873f40cd424ad0e9c6ea1f84d6e9102b1746b1689d6c575cb6b92e
FP8:   4de126bd5f7f3642244e2abc893383ca2436b460e85b8c4bff1e1557d4a9dcb4
MXFP8: 9afbdd52e8d68389677349042d9150bd5091d1716cbc0cb813e6e97f5624f597
```

每次运行的输入、golden、结果和日志写入：

```text
benchmark/one-level-arch/compare/<ELF basename>/
```

主要文件为 `srcq.bin`、`srck.bin`、`srcv.bin`、`golden.bin`、`res.bin`、
`gfrun.log` 和 `golden_compare.log`；MXFP8 还包含三个 `*_scale.bin`。这些运行产物
不提交到仓库。
