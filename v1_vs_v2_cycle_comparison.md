# v1 / v2 版本 gfsim Cycle 数对比报告

- **日期**: 2026-10-09
- **数据来源**:
  - v1（优化前基线）: `/mnt/workspace/projects/1009_SuperNPUBench_v1/.../output/solution/simulation_results.md` §4.1
  - v2（优化后）: `/mnt/workspace/projects/1009_SuperNPUBench_v2/.../output/solution/simulation_results.md` §4.1
- **仿真口径**: 两版完全一致——gfsim 从 SuperScalarModel 仓库根目录运行；4-PE 变体用 `--conf fourpe --pto-v02 true -s tlsu.fake_l2_enable=false`，单 PE 变体用默认配置；Total Cycles 均为 gfsim 日志全程计数。
- **一句话结论**: v2 对 5 个 4-PE ELF 完成"driver 输入分片 + 死代码删除 + 屏障补齐"优化，**moe_combine_mt 3.37×、moe_combine_mt_dyn 2.98×、moe_dispatch_mt 2.06×、moe_dispatch_mt_dyn 1.86×、mega_moe_sim_mt 1.24× 加速**；其余 9 个未改动 ELF 与 v1 逐位一致（确定性对照）。

## 1. 全量 Cycle 对比表（14 ELF）

| 算子 | ELF 变体 | PE 模式 | v1 Cycles | v2 Cycles | Δ Cycles | 变化幅度 | 加速比 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| group_token_old | group_token_old | 单 PE | 1,117,803 | 1,117,803 | 0 | 0% | 1.00× |
| group_token_old | group_token_old_mt | 4 PE | 651,493 | 651,493 | 0 | 0% | 1.00× |
| group_token_vec | group_token_vec | 单 PE | 478,066 | 478,066 | 0 | 0% | 1.00× |
| group_token_vec | group_token_vec_mt | 4 PE | 701,118 | 701,118 | 0 | 0% | 1.00× |
| group_token_vec | group_token_vec_mt_dyn | 4 PE | 1,282,647 | 1,282,647 | 0 | 0% | 1.00× |
| mega_moe | mega_moe_sim | 单 PE | 1,058,546 | 1,058,546 | 0 | 0% | 1.00× |
| **mega_moe** | **mega_moe_sim_mt** | **4 PE** | **1,252,892** | **1,010,167** | **−242,725** | **−19.4%** | **1.24×** |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | 3,827,616 | 3,827,616 | 0 | 0% | 1.00× |
| moe_combine | moe_combine_v2 | 单 PE | 5,188 | 5,188 | 0 | 0% | 1.00× |
| **moe_combine** | **moe_combine_mt** | **4 PE** | **57,341** | **17,016** | **−40,325** | **−70.3%** | **3.37×** |
| **moe_combine** | **moe_combine_mt_dyn** | **4 PE** | **88,727** | **29,796** | **−58,931** | **−66.4%** | **2.98×** |
| moe_dispatch | moe_dispatch_v2 | 单 PE | 9,749 | 9,749 | 0 | 0% | 1.00× |
| **moe_dispatch** | **moe_dispatch_mt** | **4 PE** | **18,131** | **8,820** | **−9,311** | **−51.4%** | **2.06×** |
| **moe_dispatch** | **moe_dispatch_mt_dyn** | **4 PE** | **36,798** | **19,795** | **−17,003** | **−46.2%** | **1.86×** |

**表头说明**：

- **v1 / v2 Cycles**：gfsim `Total Cycles`（墙钟周期，越少越好）。
- **Δ Cycles**：v2 − v1（负值 = 优化）。
- **变化幅度**：Δ ÷ v1。
- **加速比**：v1 ÷ v2（>1 表示 v2 更快）。

**汇总统计**：

| 统计项 | 数值 |
| --- | ---: |
| 14 个 ELF v1 合计 | 10,586,115 cycles |
| 14 个 ELF v2 合计 | 10,217,820 cycles |
| 合计节省 | 368,295 cycles（−3.5%） |
| 优化 ELF 数 / 总数 | 5 / 14（均为 4-PE mt / mt_dyn 变体） |
| 最大单项加速 | moe_combine_mt 3.37× |
| 未改动 ELF 复现 | 9 / 9 与 v1 逐位一致（确定性验证） |

## 2. 优化 ELF 明细：Cycle 与指令数同步下降

优化依据：4-PE 共享前端饱和（v1 全部 mt 变体聚合 IPC 3.5–3.7 ≈ 4-wide 译码上限）⇒ **减指令即减周期**。下表可见周期降幅与指令降幅高度同步：

| ELF | v1 Cycles | v2 Cycles | 周期降幅 | v1 Inst | v2 Inst | 指令降幅 | 优化内容 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| moe_combine_mt | 57,341 | 17,016 | −70.3% | 223,530 | 62,520 | −72.0% | driver 输入按 PE 写域分片（原 4× 冗余占 88% 指令） |
| moe_combine_mt_dyn | 88,727 | 29,796 | −66.4% | 347,701 | 110,920 | −68.1% | 同上（genInputs ceil 分片） |
| moe_dispatch_mt | 18,131 | 8,820 | −51.4% | 63,947 | 27,685 | −56.7% | 输入分片 + 验证汇合屏障（相位 5，修复 BFU nuke 断言） |
| moe_dispatch_mt_dyn | 36,798 | 19,795 | −46.2% | 131,982 | 64,745 | −51.0% | 输入分片 + kernel 前发布屏障（相位 8c+1，kernel 相位 +1 重编号） |
| mega_moe_sim_mt | 1,252,892 | 1,010,167 | −19.4% | 4,640,915 | 3,682,186 | −20.7% | 输入分片（移植 dyn 模式）+ 死代码权重循环删除 + kernel 前发布屏障 |

**表头说明**：Inst 为 gfsim `Retired Instruction Number`（核心退休指令总数）。指令降幅与周期降幅的差值（1–5 pt）来自新增屏障的自旋开销与 TLSU 排队占比变化。

## 3. 4-PE 相对单 PE 的格局变化

v1 结论是"tile 搬运主导的算子 4-PE 无墙钟收益"；v2 优化（消除 driver 冗余）后格局部分反转：

| 算子 | 单 PE（v2，未改动） | 4-PE mt v1 | 4-PE mt v2 | mt/单 PE v1 | mt/单 PE v2 | 结论变化 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| mega_moe | 1,058,546 | 1,252,892 | 1,010,167 | 1.18×（慢） | **0.95×（快）** | 4-PE 首次正收益 |
| moe_dispatch | 9,749 | 18,131 | 8,820 | 1.86×（慢） | **0.90×（快）** | 4-PE 首次正收益 |
| moe_combine | 5,188 | 57,341 | 17,016 | 11.05×（慢） | 3.28×（慢） | 仍慢（workload 过小，屏障 + TLSU 排队固定开销无法摊薄，结构性下限） |

**表头说明**：mt/单 PE = 4-PE mt 版 cycles ÷ 单 PE 版 cycles，<1 表示 4-PE 更快。单 PE 版两版均未改动（数值一致），作为对照基准。

## 4. 确定性与归因说明

1. **9 个未改动 ELF 逐位复现 v1**：group_token_old/vec 全系、mega_moe_sim、mega_moe_sim_mt_dyn（v1 已含同款优化）、全部单 PE 变体的 Cycles 与 v1 完全相同——两版仿真环境/命令一致且模型确定性，**v2 的全部周期差异可归因于代码优化本身**。
2. **优化不改算子语义**：全部 kernel tile 计算链未动；driver 分片写入的值与全量版逐位一致（LCG 链推进 / 绝对下标保值）；gfrun 功能自校验 14/14 `R2 = 0`。
3. **一处时序模型交互修复**：dispatch_mt 分片加速后，PE0 独占验证与 worker 退出 ecall 的交错窗口触发 gfsim BFU nuke 恢复断言（v1 时序下未触发）；以 dispatch_mt_dyn 已证的"验证汇合屏障"模式（相位 5）修复——worker 在屏障自旋而非提前退出，PE0 验证后全 PE 一起放行。该屏障的少量自旋开销已计入 v2 数值。
4. **剩余瓶颈**（v2 数据，供下一轮参考）：group_token_vec_mt 701,118（tile 直方图 4096 条计数链）、moe_combine_mt 17,016（workload 过小）、mega_moe_sim_mt 1,010,167（kernel "幂等双遍" 规避模型 nuke bug，GMM 流水 ×2 冗余）。

## 5. 结论

- v2 优化在 **5 个 4-PE ELF** 上取得 **1.24×–3.37×** 加速，合计节省 368,295 cycles；优化机制（消除 driver 4× 冗余指令）与瓶颈诊断（共享前端饱和）严格对应，周期降幅 ≈ 指令降幅验证了机制有效性。
- **mega_moe 与 moe_dispatch 的 4-PE mt 版首次反超单 PE 版**，4-PE SPMD 的价值从"流水线占用率提升"（Retiring 16–36% → 44–49%）变为实际的墙钟收益。
- moe_combine_mt 因 workload 过小仍慢于单 PE（3.28×），属 4-PE 屏障 + 共享 TLSU 排队的结构性下限；group_token_vec 系列与 mega_moe_sim_mt_dyn 为本轮未优化项，是后续优化空间所在。

---

**附：原始数据出处**

| 版本 | 报告 | 日志 |
| --- | --- | --- |
| v1 | `1009_SuperNPUBench_v1/.../output/solution/simulation_results.md` | 同目录 `sim_logs/` |
| v2 | `1009_SuperNPUBench_v2/.../output/solution/simulation_results.md` | 同目录 `sim_logs/` |
