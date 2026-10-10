# solution 算子仿真结果报告（v2 优化版）

- **日期**: 2026-10-09
- **对象**: `benchmark/one-level-arch/output/solution/` 下 5 个算子共 14 个 ELF（**基于 v1 仿真结果优化后的代码**）
- **仿真器**: SuperScalarModel 仓库 (`/mnt/workspace/projects/1009_SuperScalarModel/SuperScalarModel`)
  - `bin/gfrun` — 功能仿真模型（emulator/SoftCore）
  - `bin/gfsim` — 周期级时序仿真模型（TimingSim）
- **基线**: `/mnt/workspace/projects/1009_SuperNPUBench_v1/.../output/solution/simulation_results.md`（v1 未优化版）
- **结论摘要**: 14 个 ELF 全部通过 gfrun 功能仿真（`R2 = 0`）；14 个 ELF 全部完成 gfsim 时序仿真（退出码 0，报告完整）。相对 v1：**moe_combine_mt 3.37×、moe_combine_mt_dyn 2.98×、moe_dispatch_mt 2.06×、moe_dispatch_mt_dyn 1.86×、mega_moe_sim_mt 1.24× 加速**；其余 9 个未改动 ELF 与 v1 结果逐位一致（确定性复现）。

## 0. 本版优化内容（依据 v1 仿真瓶颈分析）

### 0.1 优化依据

v1 报告的两个关键发现：

1. **4-PE 共享前端饱和**：`--conf fourpe` 下前端为单条全局 4-wide 译码管线（服务 4 个 SMT 线程），v1 全部 mt 变体的聚合 IPC 为 3.53–3.70，逼近 4-wide 上限——**动态指令总数是 4-PE 墙钟的第一瓶颈**（减少指令数即近似等比减少周期）。
2. **driver 输入生成 4× 冗余**：v1 各 mt driver 的输入初始化在 4 个 PE 上冗余全量执行（证据：moe_combine_mt 退休块中 16,385 个 FP 块 = 4096 元素填充 × 4 PE，约占该 ELF 88% 指令）；mega_moe_sim_mt driver 还含一段被后续循环逐元素完全覆盖的死代码权重生成循环。

### 0.2 优化清单

| # | 优化 | 文件 | 内容 |
| --- | --- | --- | --- |
| 1 | 输入生成按 PE 写域分片 | `moe_combine_mt.cpp` / `moe_combine_mt_dyn.cpp` | expand_x/expand_idx 按 pack 行分片、expert_scales 按 reduce token 分片；写域不相交（无同 cacheline 并发写），kernel 内部屏障前的读点全部自覆盖 |
| 2 | 同上 | `moe_dispatch_mt.cpp` | x 按 token 行分片（BS%4==0 与 slot 段对齐）、expertIds/scales 按 slot 段分片 |
| 3 | 同上 + kernel 前发布屏障 | `moe_dispatch_mt_dyn.cpp` + `moe_dispatch_mt_dyn.hpp` | genInputs 连续 ceil 分片；cfgB 的 slot 段边界 token 被相邻 PE 读取，故新增 kernel 前输入发布屏障（相位 8c+1），kernel 相位整体 +1 重编号至 8c+2..8c+5，driver 汇合移至 8c+6（镜像 mega_moe_sim_mt_dyn 的 mtBarrierDyn(8c+1) 已证模式） |
| 4 | 同上 + 移植 dyn 模式 | `mega_moe_sim_mt.cpp` + `mega_moe_sim_mt.hpp` | 输入生成移植 dyn 的连续 ceil 分片（LCG 链推进保值一致）；kernel 前新增 `mtBarrier(1)` 发布屏障（tile 量化解码读全量权重），kernel 末端汇合 1→2 重编号；**删除死代码权重循环**（第一遍随机权重被第二遍确定性细化逐元素覆盖，纯冗余） |
| 5 | 验证汇合屏障（修复） | `moe_dispatch_mt.cpp` | 原实现 worker 在 kernel 后直接 return；分片加速后 PE0 独占验证与 worker 退出 ecall 的交错窗口触发 gfsim BFU nuke 恢复断言（实测 cycle 4935 `Can't find occupied local pipe by global fbid`）。补齐 dispatch_mt_dyn 同款验证汇合屏障（相位 5）：PE0 验证期间 worker 在屏障自旋，验证后全 PE 一起放行 |

**未改动**：全部 kernel tile 计算链与算子语义；group_token_old/vec 系列（old_mt 已是 PE0 独占生成 + 发布屏障；vec_mt 的 LCG 链式依赖 + 跨步读取使分片不划算）；mega_moe_sim_mt_dyn（v1 已实现同款分片优化）；全部单 PE 变体。

## 1. 仿真命令与环境

gfsim 从 SuperScalarModel 仓库根目录运行（配置与报告路径依赖当前工作目录）。

| 仿真类型 | 命令 | 适用对象 |
| --- | --- | --- |
| 功能仿真 | `bin/gfrun -f <elf>` | 全部 14 个 ELF（默认 `softcore.multiThreadNum=4`，单线程变体仅 PE0 做功、其余线程空转） |
| 时序仿真（4 PE） | `bin/gfsim -f <elf> --conf fourpe --pto-v02 true -s tlsu.fake_l2_enable=false` | 8 个 4-PE SPMD 变体（`_mt` / `_mt_dyn`） |
| 时序仿真（单 PE） | `bin/gfsim -f <elf> --pto-v02 true -s tlsu.fake_l2_enable=false` | 6 个单 PE 变体（基础版 / `_v2` / `mega_moe_sim`），gfsim 对其自动保持单 PE 配置 |

**表头与命令参数说明**：

- **仿真类型**：`gfrun` 为功能仿真（只验证执行结果正确性，不产生周期数据）；`gfsim` 为时序仿真（周期级模型，输出 Total Cycles 与 PMU 统计）。
- **`-f <elf>`**：指定待仿真的 ELF 文件路径（必选参数）。
- **`--conf fourpe`**：加载 4-PE 数据通路配置束（threadCount=4、cube_core_num=4、vec_core_num=4、PE Cluster 分区容量等，见 `configs/fourpe.conf`）；显式 `--conf` 会跳过 ELF 4-PE 自动检测。
- **`--pto-v02 true`**：启用 PTO-ISA v0.2 指令解码（与代码默认值一致，显式传入以固定行为）。
- **`-s tlsu.fake_l2_enable=false`**：关闭 TLSU 的 fake L2 后端（使用真实 L2 建模通路；与默认值一致）。
- **适用对象**：单 PE 变体（基础版 / `_v2` / `mega_moe_sim`）按默认单 PE 配置仿真；4-PE SPMD 变体（`_mt` / `_mt_dyn`）须用 `--conf fourpe`。强制对单 PE ELF 使用 `--conf fourpe` 会触发 syscall lockstep watchdog 断言，禁止。

## 2. 仿真对象一览

| 算子 | ELF 变体 | PE 模式 | 测试形状 |
| --- | --- | --- | --- |
| group_token_old | `..._group_token_old.elf` | 单 PE | BS=512, topK=16, expertPerRank=4, expertNum=128（标量实现） |
| group_token_old | `..._group_token_old_mt.elf` | 4-PE SPMD | 同上（跨 PE 分片 + 汇合屏障） |
| group_token_vec | `..._group_token_vec.elf` | 单 PE | 同上（全 tile 指令链实现） |
| group_token_vec | `..._group_token_vec_mt.elf` | 4-PE SPMD | 同上 |
| group_token_vec | `..._group_token_vec_mt_dyn.elf` | 4-PE SPMD | 运行时 tiling，2 组 shape 配置 |
| mega_moe | `..._mega_moe_sim_BS16_H32_HD64.elf` | 单 PE | BS=16, h=32, hiddenDim=64（A8W8 MX FP16） |
| mega_moe | `..._mega_moe_sim_mt_BS16_H32_HD64.elf` | 4-PE SPMD | 同上（16 伪核 = 4 PE × 4 伪核） |
| mega_moe | `..._mega_moe_sim_mt_dyn.elf` | 4-PE SPMD | 运行时 tiling，2 组 shape 配置 |
| moe_combine | `..._moe_combine_v2.elf` | 单 PE | BS=8, H=128, K=4, NumExpanded=32, bf16 |
| moe_combine | `..._moe_combine_mt.elf` | 4-PE SPMD | 同上 |
| moe_combine | `..._moe_combine_mt_dyn.elf` | 4-PE SPMD | cfgA {8,128,4,32} + cfgB {7,128,3,21}（覆盖非整除分片路径） |
| moe_dispatch | `..._moe_dispatch_v2.elf` | 单 PE | BS=8, H=128, K=4, MoeExpertNum=4, bf16 |
| moe_dispatch | `..._moe_dispatch_mt.elf` | 4-PE SPMD | 同上 |
| moe_dispatch | `..._moe_dispatch_mt_dyn.elf` | 4-PE SPMD | cfgA {8,128,4,4} + cfgB {7,128,3,5}（覆盖非整除分片路径） |

**表头与形状参数说明**：

- **算子**：solution 目录下的算子名（与子目录名一致）。
- **ELF 变体**：`_mt` 为 4-PE SPMD 多线程版；`_mt_dyn` 为 4-PE + 运行时动态 shape 版（单次运行内顺序执行 2 组 tiling 配置）；`_v2` / 基础版为单线程实现。
- **PE 模式**：`单 PE` = gfsim 默认配置（threadCount=1）；`4-PE SPMD` = 4 SMT 线程执行同一程序映像，按 `get_thread_idx()` 分片并行，阶段间汇合屏障同步。
- **测试形状**：`BS` token 数；`topK`/`K` 每 token 路由 expert 数；`H` slot 数据宽度；`hiddenDim` FFN 中间维度；`NumExpanded` = BS×K 展开行数；`MoeExpertNum` expert 总数；cfgA/cfgB 为 dyn 版两组运行时配置。

## 3. gfrun 功能仿真结果

全部 14 个 ELF 退出码 0，输出 `Suaccelss to Reach the End of Benchmark! R2 = 0`（功能自校验通过）。

| 算子 | ELF 变体 | 退出码 | 功能结果 | Total Block 数 | Total Inst 数 |
| --- | --- | --- | --- | ---: | ---: |
| group_token_old | group_token_old | 0 | PASS (R2=0) | 470,101 | 3,037,996 |
| group_token_old | group_token_old_mt | 0 | PASS (R2=0) | 179,997 | 1,165,960 |
| group_token_vec | group_token_vec | 0 | PASS (R2=0) | 468,734 | 2,878,818 |
| group_token_vec | group_token_vec_mt | 0 | PASS (R2=0) | 2,062,136 | 10,620,226 |
| group_token_vec | group_token_vec_mt_dyn | 0 | PASS (R2=0) | 2,590,312 | 14,910,165 |
| mega_moe | mega_moe_sim_BS16_H32_HD64 | 0 | PASS (R2=0) | 670,036 | 7,643,628 |
| mega_moe | mega_moe_sim_mt_BS16_H32_HD64 | 0 | PASS (R2=0) | 315,534 | 2,065,891 |
| mega_moe | mega_moe_sim_mt_dyn | 0 | PASS (R2=0) | 1,024,865 | 7,024,599 |
| moe_combine | moe_combine_v2 | 0 | PASS (R2=0) | 4,932 | 25,368 |
| moe_combine | moe_combine_mt | 0 | PASS (R2=0) | 15,787 | 137,776 |
| moe_combine | moe_combine_mt_dyn | 0 | PASS (R2=0) | 92,746 | 528,666 |
| moe_dispatch | moe_dispatch_v2 | 0 | PASS (R2=0) | 43,184 | 251,564 |
| moe_dispatch | moe_dispatch_mt | 0 | PASS (R2=0) | 11,603 | 68,115 |
| moe_dispatch | moe_dispatch_mt_dyn | 0 | PASS (R2=0) | 64,861 | 353,209 |

**表头说明**：

- **退出码**：gfrun 进程返回码。`0` = 正常完成；非 0 = 非法指令（1）/ dump 失败（2）等错误。
- **功能结果**：驱动自校验结论。`PASS (R2=0)` 表示计算输出与 golden 参考一致（`R2` 为驱动校验寄存器）。
- **Total Block 数**：gfrun 统计的动态执行块数（全线程合计，含块描述指令序列）。
- **Total Inst 数**：gfrun 统计的动态执行指令总数（全线程合计）。

注：gfrun 默认 4 线程软核运行；单 PE 变体仅线程 0 执行主体，`_mt` 变体 4 线程按 PE 分片并行。优化后 mt 变体的 Block/Inst 数显著下降（冗余输入生成消除），功能结果不变。

## 4. gfsim 时序仿真结果

全部 14 个 ELF 退出码 0，完整输出 SuperScalar Report（`Report Stop` 正常收尾）。

### 4.1 总周期与 Unified Top-Down（slot 视角）

| 算子 | ELF 变体 | PE 模式 | Total Cycles | Retiring | Bad Spec | Frontend | CMD | Backend | 仿真墙钟 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| group_token_old | group_token_old | 单 PE | 1,117,803 | 35.97% | 8.41% | 0.01% | 0.00% | 55.62% | 367 s |
| group_token_old | group_token_old_mt | 4 PE | 651,493 | 44.95% | 18.14% | 0.01% | 0.00% | 36.90% | 383 s |
| group_token_vec | group_token_vec | 单 PE | 478,066 | 16.34% | 3.37% | 0.01% | 0.36% | 79.91% | 100 s |
| group_token_vec | group_token_vec_mt | 4 PE | 701,118 | 45.44% | 11.40% | 0.01% | 0.11% | 43.04% | 382 s |
| group_token_vec | group_token_vec_mt_dyn | 4 PE | 1,282,647 | 45.48% | 2.92% | 0.01% | 0.17% | 51.42% | 716 s |
| mega_moe | mega_moe_sim | 单 PE | 1,058,546 | 23.77% | 0.00% | 0.01% | 0.08% | 76.15% | 247 s |
| mega_moe | mega_moe_sim_mt | 4 PE | **1,010,167** | 45.56% | 19.80% | 0.00% | 0.05% | 34.58% | 625 s |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | 3,827,616 | 45.28% | 19.10% | 0.00% | 0.04% | 35.57% | 2,402 s |
| moe_combine | moe_combine_v2 | 单 PE | 5,188 | 18.00% | 35.08% | 0.89% | 0.73% | 45.30% | 1 s |
| moe_combine | moe_combine_mt | 4 PE | **17,016** | 45.93% | 0.12% | 0.28% | 0.24% | 53.44% | 9 s |
| moe_combine | moe_combine_mt_dyn | 4 PE | **29,796** | 46.53% | 0.15% | 0.16% | 0.72% | 52.43% | 16 s |
| moe_dispatch | moe_dispatch_v2 | 单 PE | 9,749 | 26.14% | 4.46% | 0.52% | 0.27% | 68.61% | 2 s |
| moe_dispatch | moe_dispatch_mt | 4 PE | **8,820** | 39.24% | 9.28% | 0.54% | 0.94% | 50.00% | 5 s |
| moe_dispatch | moe_dispatch_mt_dyn | 4 PE | **19,795** | 40.88% | 14.99% | 0.24% | 0.80% | 43.08% | 12 s |

**表头说明**（Unified Top-Down 为 gfsim 的一级性能分解，slot 口径，五桶之和恒为 100%）：

- **Total Cycles**：仿真总周期数（墙钟 cycle 数），是衡量执行时间的最终口径，加速比以此计算。**加粗**行为本版优化且数值变化的 ELF。
- **Retiring**：正常退休（有效完成）的 slot 占比，越高说明流水线产出效率越好。
- **Bad Spec**（Bad Speculation）：错误推测浪费的 slot 占比（分支误预测 + 机器清空/nuke 恢复）。
- **Frontend**（Frontend Bound）：前端（取指/译码/rename/BROB）供给不足的 slot 占比。
- **CMD**（CMD Bound）：TileOp 命令队列供给或背压阻塞的 slot 占比。
- **Backend**（Backend Bound）：后端执行资源等待的 slot 占比。
- **仿真墙钟**：宿主机运行本次 gfsim 的实际时间（非模型内周期）。
- 注意：slot 视角允许执行单元活动重叠，不能直接当作墙钟关键路径占比。

### 4.2 退休块与吞吐统计（gfsim 口径）

| 算子 | ELF 变体 | Retired Inst Num | Retired Block Num | IPC（Inst/Cycle） | BPC（Block/Cycle） |
| --- | --- | ---: | ---: | ---: | ---: |
| group_token_old | group_token_old | 3,216,446 | 470,081 | 2.88 | 0.42 |
| group_token_old | group_token_old_mt | 2,342,648 | 407,897 | 3.60 | 0.63 |
| group_token_vec | group_token_vec | 625,090 | 82,809 | 1.31 | 0.17 |
| group_token_vec | group_token_vec_mt | 2,548,692 | 391,410 | 3.64 | 0.56 |
| group_token_vec | group_token_vec_mt_dyn | 4,666,391 | 597,361 | 3.64 | 0.47 |
| mega_moe | mega_moe_sim | 2,012,997 | 167,507 | 1.90 | 0.16 |
| mega_moe | mega_moe_sim_mt | 3,682,186 | 634,200 | 3.65 | 0.63 |
| mega_moe | mega_moe_sim_mt_dyn | 13,866,342 | 2,378,366 | 3.62 | 0.62 |
| moe_combine | moe_combine_v2 | 7,471 | 1,237 | 1.44 | 0.24 |
| moe_combine | moe_combine_mt | 62,520 | 5,644 | 3.67 | 0.33 |
| moe_combine | moe_combine_mt_dyn | 110,920 | 10,666 | 3.72 | 0.36 |
| moe_dispatch | moe_dispatch_v2 | 20,384 | 2,175 | 2.09 | 0.22 |
| moe_dispatch | moe_dispatch_mt | 27,685 | 3,248 | 3.14 | 0.37 |
| moe_dispatch | moe_dispatch_mt_dyn | 64,745 | 9,014 | 3.27 | 0.46 |

**表头说明**：

- **Retired Inst Num**：核心退休的指令总数（`Retired Instruction Number`）。
- **Retired Block Num**：核心退休的块实例总数（BlockISA 层面"动态块数"）。
- **IPC（Inst/Cycle）**：= Retired Inst Num ÷ Total Cycles，与 gfsim 日志 `IPC` 字段一致（4-PE 核聚合口径，折算单 PE ≈ IPC÷4）。
- **BPC（Block/Cycle）**：= Retired Block Num ÷ Total Cycles（块级吞吐；提交带宽上限 `bctrl_bandwidth = 4`）。

注（IPC 指标解读——越大越好，但它是效率指标而非速度指标）：IPC 反映每周期退休指令数（流水线供给效率），但性能最终以 Total Cycles 为准——本版数据即为例证：moe_combine_mt 的 IPC（3.67）与 v1（3.90）相近，但指令数从 223,530 降到 62,520，周期数随指令数近似等比下降（3.37× 加速）。4-PE 下共享前端饱和（IPC ≈ 3.1–3.7 ≈ 4-wide 上限），**减指令即减周期**是本版优化的核心逻辑。

### 4.3 引擎占用（gfsim PMU，PE 求和资源周期）

| 算子 | ELF 变体 | PE 模式 | Total Cycles | Cube Busy | Vector Busy | TLSU Busy | TLSU 活跃占墙钟 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| group_token_old | group_token_old | 单 PE | 1,117,803 | 0 | 0 | 1,089,349 | 97.5% |
| group_token_old | group_token_old_mt | 4 PE | 651,493 | 0 | 0 | 642,902 | 98.7% |
| group_token_vec | group_token_vec | 单 PE | 478,066 | 0 | 412,256 | 456,620 | 95.5% |
| group_token_vec | group_token_vec_mt | 4 PE | 701,118 | 0 | 1,360,204 | 689,932 | 98.4% |
| group_token_vec | group_token_vec_mt_dyn | 4 PE | 1,282,647 | 0 | 262,708 | 1,057,632 | 82.5% |
| mega_moe | mega_moe_sim | 单 PE | 1,058,546 | 4,128 | 2,454 | 795,655 | 75.2% |
| mega_moe | mega_moe_sim_mt | 4 PE | 1,010,167 | 7,347 | 5,768 | 1,003,289 | 99.2% |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | 3,827,616 | 22,654 | 13,400 | 3,792,424 | 99.1% |
| moe_combine | moe_combine_v2 | 单 PE | 5,188 | 0 | 6,554 | 4,924 | 94.9% |
| moe_combine | moe_combine_mt | 4 PE | 17,016 | 0 | 6,503 | 16,707 | 98.2% |
| moe_combine | moe_combine_mt_dyn | 4 PE | 29,796 | 0 | 10,858 | 29,361 | 98.6% |
| moe_dispatch | moe_dispatch_v2 | 单 PE | 9,749 | 0 | 6,795 | 9,522 | 97.7% |
| moe_dispatch | moe_dispatch_mt | 4 PE | 8,820 | 0 | 8,107 | 8,568 | 97.1% |
| moe_dispatch | moe_dispatch_mt_dyn | 4 PE | 19,795 | 0 | 20,850 | 19,001 | 96.0% |

**表头说明**：

- **Cube/Vector/TLSU Busy**（PE-summed Resource-Cycles）：各引擎实例忙碌周期按 PE 求和（4-PE 模式 cube/vec 有 4 实例，数值可超 Total Cycles）；TLSU 为共享单实例。
- **TLSU 活跃占墙钟**：= TLSU Active Wall Cycles (union) ÷ Total Cycles，"至少一个 TLSU 通道活跃"的墙钟占比。

## 5. v1 → v2 优化效果对比

### 5.1 全量对比表

| 算子 | ELF 变体 | PE 模式 | v1 Cycles | v2 Cycles | 加速比 | v1 Inst | v2 Inst | 指令降幅 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| group_token_old | group_token_old | 单 PE | 1,117,803 | 1,117,803 | 1.00× | 3,216,446 | 3,216,446 | 0% |
| group_token_old | group_token_old_mt | 4 PE | 651,493 | 651,493 | 1.00× | 2,342,648 | 2,342,648 | 0% |
| group_token_vec | group_token_vec | 单 PE | 478,066 | 478,066 | 1.00× | 625,090 | 625,090 | 0% |
| group_token_vec | group_token_vec_mt | 4 PE | 701,118 | 701,118 | 1.00× | 2,548,692 | 2,548,692 | 0% |
| group_token_vec | group_token_vec_mt_dyn | 4 PE | 1,282,647 | 1,282,647 | 1.00× | 4,666,391 | 4,666,391 | 0% |
| mega_moe | mega_moe_sim | 单 PE | 1,058,546 | 1,058,546 | 1.00× | 2,012,997 | 2,012,997 | 0% |
| mega_moe | mega_moe_sim_mt | 4 PE | 1,252,892 | **1,010,167** | **1.24×** | 4,640,915 | 3,682,186 | −20.7% |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | 3,827,616 | 3,827,616 | 1.00× | 13,866,342 | 13,866,342 | 0% |
| moe_combine | moe_combine_v2 | 单 PE | 5,188 | 5,188 | 1.00× | 7,471 | 7,471 | 0% |
| moe_combine | moe_combine_mt | 4 PE | 57,341 | **17,016** | **3.37×** | 223,530 | 62,520 | −72.0% |
| moe_combine | moe_combine_mt_dyn | 4 PE | 88,727 | **29,796** | **2.98×** | 347,701 | 110,920 | −68.1% |
| moe_dispatch | moe_dispatch_v2 | 单 PE | 9,749 | 9,749 | 1.00× | 20,384 | 20,384 | 0% |
| moe_dispatch | moe_dispatch_mt | 4 PE | 18,131 | **8,820** | **2.06×** | 63,947 | 27,685 | −56.7% |
| moe_dispatch | moe_dispatch_mt_dyn | 4 PE | 36,798 | **19,795** | **1.86×** | 131,982 | 64,745 | −51.0% |

**表头说明**：加速比 = v1 Cycles ÷ v2 Cycles（>1 为优化）；指令降幅基于 gfsim Retired Inst Num。9 个未改动 ELF 与 v1 逐位一致，验证了仿真确定性并隔离出优化净效果。

### 5.2 分算子说明

- **moe_combine（3.37× / 2.98×）**：收益最大——v1 中 driver 冗余输入生成占 88% 指令（16,384 FP 块），分片后指令数 −72%，周期近似等比下降。kernel tile 链不变（TLSU 活跃占比仍 ~98%，瓶颈从"冗余指令挤占共享前端"回到"TLSU tile 搬运"本身）。
- **moe_dispatch（2.06× / 1.86×）**：分片消除冗余（−57%/−51%）+ 验证汇合屏障修复 BFU 断言。mt 版 8,820 cycles 已低于单 PE v2 版（9,749）——4-PE 首次在 dispatch 上反超单 PE。
- **mega_moe（1.24×）**：driver 分片 + 死代码权重循环删除，指令 −20.7%；周期 1,252,892 → 1,010,167，**首次低于单 PE 版（1,058,546）**，4-PE 首次实现正收益。kernel 的 GMM 流水（含幂等双遍的模型 bug 规避）未动，剩余瓶颈仍是 TLSU 满载（99.2%）。
- **group_token_vec / group_token_old**：未优化（LCG 链式依赖/已是最优模式），作为确定性对照。

## 6. 总体观察与结论

1. **功能正确性**：14 个 ELF 在 gfrun 下全部 `R2 = 0`——分片输入生成的值与全量版逐位一致（LCG 链推进/绝对下标保值），算子语义零改变。
2. **优化有效性验证**："共享前端饱和 ⇒ 减指令即减周期"的推断被数据证实：五个优化 ELF 的周期降幅与指令降幅高度同步（combine −72% 指令 → −70% 周期；dispatch −57% → −51%；mega −21% → −19%）。
3. **4-PE 收益格局改变**：优化前 4 个 tile 算子的 mt 版全部慢于单 PE 版；优化后 **moe_dispatch_mt（8,820 < 9,749）与 mega_moe_sim_mt（1,010,167 < 1,058,546）反超单 PE**；moe_combine_mt（17,016）仍高于单 PE v2（5,188）——该 workload 过小，4-PE 的屏障 + TLSU 排队固定开销无法摊薄，属结构性下限。
4. **时序模型交互**：分片加速改变了 PE0 验证与 worker 退出的时序交叠，暴露 gfsim BFU nuke 恢复断言（模型 bug）；以已证的"验证汇合屏障"模式修复（dispatch_mt 相位 5）。该类问题与 mega_moe 的"幂等双遍"同类，均为模型 nuke/退出路径缺陷的驱动层规避。
5. **后续优化方向**（按预期收益排序）：
   - group_token_vec_mt（701,118，tile 直方图 4096 条计数链）——重构 bin 循环减少 TSTORE 频次，或按 mega 思路拆开相邻 ld/st 块对降低 nuke；
   - moe_combine_mt kernel 侧——flag-check 链（barrier 已保证数据就绪）与 clear-flag 的 TSTORE 合并；
   - mega_moe kernel 的"幂等双遍执行"（~2× GMM 流水冗余）——若模型 nuke 缺陷修复可整体减半。

## 7. 附：原始日志

原始仿真日志已归档至本目录 `sim_logs/`（`<elf>.gfsim.log` / `<elf>.gfrun.log`，`.gfsim.exit` 记录退出码）。v1 基线日志见 `/mnt/workspace/projects/1009_SuperNPUBench_v1/.../output/solution/sim_logs/`。
