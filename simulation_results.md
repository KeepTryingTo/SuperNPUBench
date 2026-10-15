# solution 算子仿真结果报告（v8 终版；v9 证伪记录见 §10）

- **日期**: 2026-10-15（v8 三轮 + v9 证伪一轮）；v7 一轮 2026-10-14 二轮；v6 一轮同日
- **对象**: `benchmark/one-level-arch/output/solution/` 下 5 个算子共 14 个 ELF（**基于 v7 驱动段瘦身版代码的再优化**；v6 轮为驱动验证段 LUT 化/双循环合并/wave 分派，v7 轮为流水图/并行度图驱动的驱动段瘦身，**v8 轮为验证段 golden 4-PE 并行分割**，v9 轮 cfgA 并行分割证伪回退，详见 §9/§10）
- **仿真器**: SuperScalarModel 仓库 (`/mnt/workspace/projects/1009_SuperScalarModel/SuperScalarModel`)
  - `bin/gfrun` — 功能仿真模型（emulator/SoftCore）
  - `bin/gfsim` — 周期级时序仿真模型（TimingSim）
- **基线**: v7（2026-10-14 二轮，本文件 §8）；v6（2026-10-14 一轮，本文件 §0-§7：sim 287,642 / mt 418,784 / mt_dyn 1,912,274）；v5 A5 对齐版（2026-10-13）；v4（2026-10-12）
- **结论摘要**: 14 个 ELF 全部通过 gfrun 功能仿真（`R2 = 0`）。v8 轮 3 个 mega_moe ELF 全部通过 gfsim（finisher `val=0x5555 pass`）并获得优化（vs v7）: **mega_moe_sim_mt 2.36×（382,376 → 162,234）、mega_moe_sim_mt_dyn 1.51×（1,633,876 → 1,079,112）**，sim 不变（214,848，ELF 逐位一致）；vs v5 A5 对齐版 mt / mt_dyn 分别 **3.51× / 2.25×**。其余 11 个未改动 ELF 继承 v4 结果。v8 全部优化落在**验证段并行化**（cfgB/mt 验证 golden 按 decode 行分片 + GEMM token 分片 4-PE 分割，单写者 + 发布扫描 + 屏障），kernel 侧 A5 对齐结构与 cfgA 轮 legacy 保护形态零改动。

## 0. v6 一轮优化内容（依据 v5 流水图/并行度图瓶颈分析；v7 二轮见 §8）

### 0.1 优化依据

v5 `mega_moe_pipeline_analysis.md` §5 的优化空间结论（按是否依赖模型侧修复筛选）：

1. **【驱动段，本轮实施】golden 解码段 LUT 化**（报告预期 −0.2~0.3M）：E4M3 仅 256 个可表示值 + 组 scale 常量 → 查表，数值逐位一致，不依赖模型修复。v4 §6.3-2 曾指出 "golden 解码的 `ref_fp8_e4m3` 逐元素分支链可再压（~12K 元素 × ~15 op）"。
2. **【kernel 段，本轮部分实施】pad 槽位开销削减**（报告 §5-#6）：cfgB bs=18 的 PE 行片 ∩ expert 段边界产生 10 个 pad 槽（×2 幂等双遍），每个 pad 槽走完整 SwiGLU/GEMV/Combine VEC/CUBE 链；按段长自适应分组可消除。需权衡时序敏感性（见 §0.3 回退记录）。
3. 驱动段其余两项（cfgA golden 分块化 −0.7M、worker 自旋减负）与 kernel 段 TMOV 消除/A 侧重载提升仍被 M7/M1 模型封锁，维持 v5 结论。

### 0.2 优化清单（成功保留项）

| # | 优化 | 文件 | 内容 | 效果（gfsim Total Cycles） |
| --- | --- | --- | --- | --- |
| 1 | golden E4M3 解码 LUT 化 | `mega_moe_sim.cpp` / `mega_moe_sim_mt.cpp` / `mega_moe_sim_mt_dyn.cpp` | bss 尾部新增 `g_e4m3Lut[256]`（double），`compute_golden` 入口一次性构建（构建即原 `ref_fp8_e4m3` 逐值计算 → `lut[b] ≡ ref_fp8_e4m3((double)b)` 逐位一致；compute_golden 仅 PE0/单线程调用 → 单写者无竞争）；预解码内循环由 "位提取 + exp2 位级构造"（~20 op/元素）退化为 1 load + 1 fmul，乘法操作数不变 → golden 数值逐位一致。置于 bss 尾部不动既有符号布局（含 mt 版 g_timingPad 衬垫域） | 三 ELF 验证段主体压缩（见 §5） |
| 2 | verify 误差双循环合并 | 同上三文件 | abs/rel 两个误差统计循环合并为单遍（err/denom 每元素只算一次，省一遍 y/yRef 全量重扫）；rel 分子即 err（原式同表达式），max 比较次序不变 → 两统计量结果逐位一致 | 省 ~1K 元素/轮 的重复扫描 |
| 3 | kernel wave 组自适应分派 | `mega_moe_sim.hpp`（`mx_expert_wave_group` 模板化 + 新增 `mx_expert_wave_run`）+ `mega_moe_sim.hpp`/`mega_moe_sim_mt.hpp` 两入口调用点 | `mx_expert_wave_group` 改 `template <uint32_t G>`（G∈[1,4]，if constexpr 逐槽展开——每实例内全部 tile op 仍无条件执行，无跨分支 tile PHI → 不触发 TCOPY 保留字缺陷）；`mx_expert_wave_run` 按剩余行数分派：满组（rem≥4）走 G=4（与原固定组逐 op 等价，B8 权重复用粒度不变），尾组 rem<4 走 G=rem——组槽位与有效行恰好重合，越段 pad 槽全量消除。数值不变：有效行 op 序列与固定组版完全一致，被消除的仅是 pad 槽无效计算（其 combine 写 dummy 尾行本就永不读取） | sim/mt 两入口（cfgA 无 pad，为代码等价性回归）；mt_dyn 见 §0.3 |

**未改动**：全部 kernel tile 计算链与算子语义（A5 对齐结构不变）；11 个非 mega_moe ELF 的全部代码（结果继承 v4）；cfgA 轮 legacy GEMM 形态（模型稳定性承载位，v4 §0.2-3 结论维持）。

### 0.3 试验与回退记录（防止复走弯路）

| # | 试验 | 结果 | 结论/回退原因 |
| --- | --- | --- | --- |
| T0 | mt_dyn 同步启用 wave 自适应分派（与 sim/mt 相同代码） | gfsim `--conf fourpe` **Deadlock @ cycle 79,300**（cfgA kernel 末端冲刷段）：thread0 的 STD 块 allocated 9999 cyc 不完成，TPC 参考队列满 256，thread2/3 同块已完成在末端屏障自旋 | **M7 类 tile-store 排序门停摆窗口再触发**：cfgA 无 pad 槽、tile op 序列与固定组版相同——纯代码布局变化重掷时序骰子落入模型缺陷窗口（A5 报告 §4.4/§4.5 同族）。日志归档 `sim_logs/v6_opt/exp0_mtdyn_wavedispatch_deadlock.gfsim.log` |
| T-A（=T0 回退） | mt_dyn 回退固定 G=4 分组（保留 driver 优化；正式注释版重编译 md5 与实验版一致 `047c10f3`） | **1,912,274，val=0x5555 PASS**（nuke=1 自愈） | 肇事者为 kernel 侧分派形态（分辨实验：driver 优化在固定分组下完整通过）；mt_dyn 维持固定分组，cfgB pad 削减收益（~3K cycle，占 Total 0.15%）不值得冒险。sim/mt 不涉该窗口（实测通过），保留自适应分派 |
| T-E1（2026-10-14 二轮） | cfgA 轮验证改分块 GEMM + k 循环内交织 L2 读流量衬垫（每 k-iter 4 次 96KB 冷区 128B 步长 cached load，~20K 次 / 密度 ≈ legacy 的 ~53 次/Kcyc；golden 数值逐位不变）—— 目标为替代 v4 以来的 legacy 访存形态承载位（cfgA verify ≈ 930K → 预期 ~400K） | gfsim **val=0x0001 fail 且更慢（Total 1,960,992 > 1,912,274）**；gfrun R2=0（衬垫代码功能正确） | **双重证伪**：① 衬垫读经 volatile sink 块以 L2 延迟（~19 cyc）串行化，成本吃掉分块收益（cfgA 窗口仍 ~800K）；② 冷区流量不具备保护性——legacy 的保护流量读的是 GEMM 自身的 w1Dbl/w2Dbl 权重行且与 fmadd 依赖链同构，非单纯密度可替代（v4 六探针 + 本试验共 7 种替代均失败）。**结论：保护因子的成本即保护本身，cfgA legacy 形态（~930K）为 M7 修复前的模型税**。完整回退后重编译 md5 = `047c10f3`（与 v6 逐字节一致，仿真结果直接继承）。日志 `sim_logs/v6_opt/expE1_mtdyn_l2pad.gfsim.log` |

## 1. 仿真命令与环境

gfsim 从 SuperScalarModel 仓库根目录运行（配置与报告路径依赖当前工作目录）。

| 仿真类型 | 命令 | 适用对象 |
| --- | --- | --- |
| 功能仿真 | `bin/gfrun -f <elf>`（4-PE 变体加 `-s softcore.multiThreadNum=4`） | 全部 14 个 ELF |
| 时序仿真（4 PE） | `bin/gfsim -f <elf> --conf fourpe --pto-v02 true -s tlsu.fake_l2_enable=false` | 8 个 4-PE SPMD 变体（`_mt` / `_mt_dyn`） |
| 时序仿真（单 PE） | `bin/gfsim -f <elf> --pto-v02 true -s tlsu.fake_l2_enable=false` | 6 个单 PE 变体（基础版 / `_v2` / `mega_moe_sim`），gfsim 对其自动保持单 PE 配置 |

表头与命令参数说明与 v4 报告一致（`--conf fourpe` 加载 4-PE 数据通路束；`--pto-v02` 固定 PTO-ISA v0.2 解码；`-s tlsu.fake_l2_enable=false` 关闭 TLSU fake L2 后端；单 PE ELF 强制 `--conf fourpe` 会触发 syscall lockstep watchdog 断言，禁止）。

## 2. 仿真对象一览

与 v4 报告一致（14 个 ELF / 5 算子；mega_moe 三变体形状：cfgA BS=16/h=32/hiddenDim=64/epr=2/topK=1，mt_dyn 另含 cfgB BS=18/h=32/hiddenDim=128）。

## 3. gfrun 功能仿真结果

> **注**: §1–§7 为 v6 一轮记录（mega 家族数值已被 §8.4/§8.5 的 v7 终版取代；未改动 11 ELF 仍然有效）。

全部 14 个 ELF 退出码 0，输出 `Suaccelss to Reach the End of Benchmark! R2 = 0`（功能自校验通过）。本轮 3 个 mega ELF 用最终构建重测：

| 算子 | ELF 变体 | 退出码 | 功能结果 | Total Block 数 | Total Inst 数 | v4 Block / Inst |
| --- | --- | --- | --- | ---: | ---: | --- |
| mega_moe | mega_moe_sim | 0 | PASS (R2=0) | 107,128 | 2,229,780 | 155,952 / 2,659,028 |
| mega_moe | mega_moe_sim_mt | 0 | PASS (R2=0) | 71,826 | 709,938 | 119,590 / 992,870 |
| mega_moe | mega_moe_sim_mt_dyn | 0 | PASS (R2=0) | 433,965 | 3,490,148 | 580,177 / 4,356,052 |

其余 11 个 ELF 与 v4 逐位一致（代码未动），结果继承 v4 §3。注：mega 家族 Inst 降幅（−16~−29%）来自 LUT 化 + 双循环合并；golden 数值逐位不变（`lut[b]` 即原函数逐值计算，gfrun R2=0 三端一致佐证）。

## 4. gfsim 时序仿真结果

全部 14 个 ELF 退出码 0（mega 家族 finisher `val=0x5555 pass`），完整输出 SuperScalar Report（`Report Stop` 正常收尾）。

### 4.1 总周期与 Unified Top-Down（slot 视角）

| 算子 | ELF 变体 | PE 模式 | Total Cycles | Retiring | Bad Spec | Frontend | CMD | Backend |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| group_token_old | group_token_old | 单 PE | 1,117,803 | 35.97% | 8.41% | 0.01% | 0.00% | 55.62% |
| group_token_old | group_token_old_mt | 4 PE | 523,819 | 30.10% | 7.01% | 0.01% | 0.01% | 62.87% |
| group_token_vec | group_token_vec | 单 PE | 478,066 | 16.34% | 3.37% | 0.01% | 0.36% | 79.91% |
| group_token_vec | group_token_vec_mt | 4 PE | 701,118 | 45.44% | 11.40% | 0.01% | 0.11% | 43.04% |
| group_token_vec | group_token_vec_mt_dyn | 4 PE | 1,282,647 | 45.48% | 2.92% | 0.01% | 0.17% | 51.42% |
| mega_moe | mega_moe_sim | 单 PE | **287,642** | 24.43% | 0.02% | 0.02% | 0.40% | 75.14% |
| mega_moe | mega_moe_sim_mt | 4 PE | **418,784** | 45.98% | 12.29% | 0.01% | 0.11% | 41.60% |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | **1,912,274** | 45.87% | 15.02% | 0.00% | 0.14% | 38.97% |
| moe_combine | moe_combine_v2 | 单 PE | 5,188 | 18.00% | 35.08% | 0.89% | 0.73% | 45.30% |
| moe_combine | moe_combine_mt | 4 PE | 17,016 | 45.93% | 0.12% | 0.28% | 0.24% | 53.44% |
| moe_combine | moe_combine_mt_dyn | 4 PE | 29,765 | 46.21% | 0.14% | 0.16% | 0.82% | 52.67% |
| moe_dispatch | moe_dispatch_v2 | 单 PE | 9,749 | 26.14% | 4.46% | 0.52% | 0.27% | 68.61% |
| moe_dispatch | moe_dispatch_mt | 4 PE | 8,820 | 39.24% | 9.28% | 0.54% | 0.94% | 50.00% |
| moe_dispatch | moe_dispatch_mt_dyn | 4 PE | 17,486 | 38.07% | 14.94% | 0.27% | 0.85% | 45.87% |

**表头说明**：同 v4 报告（Unified Top-Down 为 gfsim 一级性能分解，slot 口径，五桶之和恒为 100%；**加粗**行为本轮优化且数值变化的 ELF）。未加粗行数值与 v4 逐位一致（ELF 未改动 + 仿真确定性）。

### 4.2 退休指令与吞吐统计（gfsim 口径）

| 算子 | ELF 变体 | Retired Inst Num | IPC（Inst/Cycle） | v4 Retired Inst |
| --- | --- | ---: | ---: | ---: |
| group_token_old | group_token_old | 3,216,446 | 2.88 | 3,216,446 |
| group_token_old | group_token_old_mt | 1,261,244 | 2.41 | 1,261,244 |
| group_token_vec | group_token_vec | 625,090 | 1.31 | 625,090 |
| group_token_vec | group_token_vec_mt | 2,548,692 | 3.64 | 2,548,692 |
| group_token_vec | group_token_vec_mt_dyn | 4,666,391 | 3.64 | 4,666,391 |
| mega_moe | mega_moe_sim | **562,065** | 1.95 | 681,149 |
| mega_moe | mega_moe_sim_mt | **1,540,501** | 3.68 | 2,000,172 |
| mega_moe | mega_moe_sim_mt_dyn | **7,017,124** | 3.67 | 8,461,334 |
| moe_combine | moe_combine_v2 | 7,471 | 1.44 | 7,471 |
| moe_combine | moe_combine_mt | 62,520 | 3.67 | 62,520 |
| moe_combine | moe_combine_mt_dyn | 110,030 | 3.70 | 110,030 |
| moe_dispatch | moe_dispatch_v2 | 20,384 | 2.09 | 20,384 |
| moe_dispatch | moe_dispatch_mt | 27,685 | 3.14 | 27,685 |
| moe_dispatch | moe_dispatch_mt_dyn | 53,251 | 3.05 | 53,251 |

注：mega 家族 Retired Inst −17~−23%（LUT 化消除解码分支链 + 双循环合并省一遍扫描），周期降幅（v5→v6 −21~−59%）大于指令降幅——LUT 连续读访存（8B stride）较位操作链对 LSU/前端的占用更轻。

### 4.3 引擎占用（gfsim PMU，PE 求和资源周期）

| 算子 | ELF 变体 | PE 模式 | Total Cycles | Cube Busy | Vector Busy | TLSU Busy | TLSU 活跃占墙钟 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| mega_moe | mega_moe_sim | 单 PE | 287,642 | 1,866 | 2,490 | 280,753 | 97.5% |
| mega_moe | mega_moe_sim_mt | 4 PE | 418,784 | 5,318 | 6,791 | 417,218 | 99.6% |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | 1,912,274 | 21,615 | 17,133 | 1,900,967 | 99.4% |

（其余 11 个 ELF 与 v4 §4.3 逐位一致，此处省略；TLSU 仍为 mega 家族绑定资源，kernel 段瓶颈结论不变。）

## 5. 版本对比

### 5.1 v5 → v6（本轮）+ v4 对照

| 算子 | ELF 变体 | PE 模式 | v5 Cycles (A5 对齐) | v6 Cycles | 加速比 | v4 Cycles | v4→v6 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| mega_moe | mega_moe_sim | 单 PE | 696,680 | **287,642** | **2.42×** | 358,296 | **1.25×** |
| mega_moe | mega_moe_sim_mt | 4 PE | 568,852 | **418,784** | **1.36×** | 544,228 | **1.30×** |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | 2,430,310 | **1,912,274** | **1.27×** | 2,312,312 | **1.21×** |

其余 11 个 ELF：v5 = v4 = v6（未改动）。

### 5.2 分算子说明

- **mega_moe_sim（2.42×）**: LUT 化 + 双循环合成的全量收益（单 PE 无自旋/无双轮复用，验证段即周期主体）。v5 的 A5 对齐代价（dispatch 通路 +4.3%）被完全吸收：v6 已低于 v4 最优值 21%。
- **mega_moe_sim_mt（1.36×）**: 同款 driver 优化。mt 版 kernel 的 wave 自适应分派为等价回归（cfgA slots=16 全满组），零风险保留。
- **mega_moe_sim_mt_dyn（1.27×）**: driver 优化收益 518,036 cycle（−21.3%）；kernel 侧分派优化因 M7 类停摆窗口回退（§0.3-T0），cfgB pad 削减（~3K）放弃。cfgA legacy 形态与轮间刷洗衬垫维持（v4 §0.2-3 的模型稳定性承载位）。
- **A5 对齐代价的回收**：v4→v5 的 +5.1%（A1 dispatch 通路流量）在 v6 中被驱动段优化反超（mt_dyn v4→v6 = 1.21×），kernel 段 A5 对齐结构完整保留（dispatch 通路/expert 波/同步点可比性不变）。

## 6. 总体观察与结论

### 6.1 优化有效性

1. **功能正确性**：14 个 ELF gfrun 全部 `R2 = 0`；mega golden LUT 化数值逐位一致（`lut[b]` 即原 `ref_fp8_e4m3` 的逐值预计算，乘法操作数不变）；误差双循环合并的两统计量结果逐位一致（rel 分子同表达式）。SIM_FAKE=1 构建编译通过（mega_moe_sim）。
2. **"E4M3 256 值查表" 是验证段的下一个单点杠杆**：v4 的备忘录化把解码从 GMM 内循环提到预解码段（~18K 元素 × 2 轮），本轮 LUT 把预解码内循环本身从 ~20 op/元素 压到 2 op/元素——三 ELF 驱动段合计 −1.1M cycle。
3. **mt_dyn 的时序敏感性再实证**：cfgA 段 tile op 序列完全相同的两个构建（固定分组 vs 自适应分派），后者在 kernel 末端落入 M7 类停摆（cycle 79.3K）——代码布局即"时序骰子"，与 A5 报告 §4.5（kMaxOutputSize 立即数编码差异重掷时序）同族。分辨实验（回退 kernel 侧、保留 driver 侧 → 完整通过）给出了干净的因果隔离。

### 6.2 模型缺陷新发现/再触发（供模型侧修复参考）

| # | 缺陷 | 触发方式 | 现象 |
| --- | --- | --- | --- |
| M7（再触发） | tile-store 排序门停摆（TPC 参考队列满 256） | mt_dyn kernel 内代码布局变化（wave 自适应分派的分派分支 + G=1/2/3 模板实例引入，cfgA tile op 序列不变） | `Deadlock detected` @ cycle 79,300：thread0 末端冲刷 STD 块 allocated 9999 cyc 不完成，thread2/3 同块 completed 后在屏障自旋；日志 `sim_logs/v6_opt/exp0_mtdyn_wavedispatch_deadlock.gfsim.log` |

### 6.3 后续优化方向（按预期收益排序）

1. **mega_moe_sim_mt_dyn cfgA 轮验证分块化**（预期 1.91M → ~1.4M）：前置条件仍为模型侧修复 M7；**替代性流量衬垫路线已证伪**（§0.3 T-E1：冷区 L2 读流量既昂贵（volatile sink 块的 L2 延迟串行化）又不具备保护性（legacy 流量与 GEMM 权重行/fmadd 依赖链同构）），唯一路径是模型侧修复 tile-store→GM 缺口后移除 legacy 形态承载位与刷洗衬垫。
2. **mt_dyn kernel 侧 pad 槽削减重启**（~3K）：M7 修复后可将 `mx_expert_wave_run` 分派重新接入 mt_dyn（代码已就位于 `mega_moe_sim.hpp`，sim/mt 入口已验证）。
3. **worker 自旋减负**（与 M5/M7 修复捆绑）：mt/mt_dyn 的 verify 窗口 TLSU 占用仍被自旋轮询占据（v5 §4）。
4. **kernel 段 TMOV 消除 + A 侧 v 重载提升**（M1 封锁，合计约 −6K）：维持 v5 结论。
5. **verify GEMM kNb 16→32 加宽**（预期 −3~5%）：寄存器压力风险 + mt_dyn 时序重掷风险，收益微小，暂缓。

## 7. 附：原始日志

- 本轮最终结果日志: `sim_logs/v6_opt/`（`mega_moe_sim.{gfrun,gfsim}.log` / `mega_moe_sim_mt.{gfrun,gfsim}.log` / `mega_moe_sim_mt_dyn.{gfrun,gfsim}.log`，全部 R2=0 / val=0x5555）。
- 本轮试验取证日志: `sim_logs/v6_opt/exp0_mtdyn_wavedispatch_deadlock.gfsim.log`（T0 停摆取证）、`sim_logs/v6_opt/expA_mtdyn_kernelfix.gfsim.log`（T-A 分辨实验，= mt_dyn 终版结果）、`sim_logs/v6_opt/expE1_mtdyn_l2pad.gfsim.log`（二轮 T-E1 流量衬垫证伪取证，val=0x0001）。
- **流水图/并行度图（v6 终版 ELF 再生成，2026-10-14）**: `kernels/solution/` 下 `mega_moe_pipe.out`（2.81GB，`-p 1` 全程，PASS）+ `mega_moe_swim{,_kA,_kB,_verify}.json`（三窗口门控，PASS）——阶段分解/kernel 统计/泳道画像见同目录 `mega_moe_pipeline_analysis.md`（v6 版）与 `mega_moe_pipe_swan_summary.md`；阶段边界：kernelA [59,741, 70,300] / kernelB [1,107,810, 1,128,606] / Total 1,912,273。
- 未改动 11 ELF 的 v4/v3 日志: 本目录 `sim_logs/` 根层（v4 报告 §7 继承）。
- 基线日志: v5（A5 对齐版）见 `mega_moe_pipeline_analysis.md` §6 数据源；v4/v3/v2 见各自仓库 `sim_logs/`。

## 8. v7 二轮优化（2026-10-14，流水图/并行度图驱动的驱动段瘦身）

### 8.1 优化依据（v6 终版 pipe 窗口普查 + 反汇编归因）

对 v6 终版 ELF 的 `mega_moe_pipe.out` 做逐周期窗口普查（`/tmp/opencode/pipeswan/pipe_window_census.py`，窗口 pre/legacyA/legacyB/genB/postB1-3），结合反汇编逐指令归因，得到 v6 剩余周期的真实落点（全部在**驱动段**，kernel 段 31,355 cycle 仅占 1.64%）：

1. **golden 解码内循环 ~14 cyc/元素**（postB1 实测 4,096 块 / 56.8K cycle）：每元素 10 条指令，其中 `addtpc+addi` 每元素重取 `&g_e4m3Lut` 基址 2 条、循环控制（subi/setc/addi×2）4 条；两轮解码共 18,432 元素（cfgA 6,144 + cfgB 12,288）。
2. **legacy GMM1（cfgA 轮模型稳定性承载位）内循环 ~17.8 cyc/k 迭代**：每迭代 10 条指令中有 5 条服务于 x 侧（`lw` x[k] + `fcvt.fs2fd` + 3 条地址算术）——**(double)g_mmX[t*h+k] 是元素纯函数，每 token 重复计算 64×32 次**；保护性的 w1e 跨行步进访问（每 fmadd 一次 L2 往返）不受 x 侧指令影响。
3. **genInputs 权重填充 ~9.3 cyc/元素**（pre 实测 4,096 块 / 38.2K cycle）：w1/w2 确定性细化的三个分量 `(i*7)%5`、`(i*3+1)&7`、`(i/7)&1`（w2 同族）编译为 mulhi 常数除法链，每元素 ~30 条指令；两轮共 18,432 元素 ≈ 160K cycle（pre 59.7K + genB ~100K）。**三分量周期为 5/8/14 与 5/8/26 → 字节模式周期仅 280/520**。
4. **sim 版 genInputs 仍保留 v1 遗留的随机小值第一遍循环**（4,096 元素，被确定性细化循环逐元素覆盖，纯死代码；mt 版 2026-10-09 已删，sim 漏删）。
5. 4 PE 的标量侧共享 1 个 SPE（4 SMT）：worker 自旋轮询（~0.56 块/cycle）与 PE0 验证块竞争发射口 —— **驱动段每省 1 条指令约得 2× 周期收益**（自旋不可动：v3 opt5 实证降频暴露 kernel 末端 TSTORE 落盘竞态，M5/M7 封锁项维持）。

### 8.2 优化清单（成功保留项）

| # | 优化 | 落点变体 | 内容 | 数值不变性论证 | 效果（gfsim Total Cycles） |
| --- | --- | --- | --- | --- | --- |
| 1 | golden 解码内循环瘦身 | 三驱动 | LUT 基址提升为局部指针（消每元素 addtpc+addi 重取基址）+ n 循环展开摊薄循环控制（sim/mt 4 路，mt_dyn 2 路——展开度受 §8.3-E3/E4 时序约束） | 同表同乘数，读写地址序列与元素值逐位不变 | sim −5.5K / mt −26K（4 路 vs 2 路增量）；mt_dyn 随 E2/E5 合并度量 |
| 2 | x 行每 token 预转换（xrow） | 三驱动 | `(double)g_mmX[t*h+k]` 每 token 转换一次到局部 double 数组：legacy GMM1 内循环 10→~7 指令/迭代（消 lw+fcvt+地址算术），分块 GMM1 每 (n0 块,k) 的重复 lw+fcvt 消除 | 转换为元素纯函数，预转换值与逐次转换逐位一致；**legacy 轮的 w1e/w2e 跨行步进访问序列（模型稳定性承载位）与 fmadd 依赖链完全不变**，仅消除 L1 命中的 x 重复装载 | mt_dyn −211K（E2+E5+E1 合并实测，exp4） |
| 3 | genInputs 填充模式周期化 + 模式表 | 三驱动 | w1/w2 字节模式周期 280/520，首次经增量递推（`(i*7)%5` 步进 +7≡+2 等）构建模式表，填充退化为表游走（每元素 lbu+sb+指针步进）；表构建按 PE 独立触发（`sPatReadyPE[tid]`，各 PE 冗余同值写幂等——共享 ready 标志会让后到 PE 跳过自身构建而读到半建表） | 递推初值 = i=0 的公式值，步进与闭式公式在模运算意义下等价 → 表中字节与闭式公式在全部 i 上逐位一致，**权重数据逐字节不变**（gfrun R2=0 三端佐证） | sim −59.7K（274,593→214,848）；mt −8.4K；mt_dyn −67K（1,701,272→1,633,876） |
| 4 | sim 版死代码删除 | sim | 删除 genInputs 的 v1 遗留随机小值第一遍循环（被细化循环逐元素完全覆盖；mt 版 2026-10-09 同款清理） | 被覆盖写，值不变 | 与 #3 合并度量（sim pre 60K→~15K） |

**未改动**：全部 kernel 头文件（`mega_moe_sim.hpp` / `mega_moe_sim_mt.hpp` / `mega_moe_sim_mt_dyn.hpp`——A5 对齐结构、tile op 序列、屏障协议、幂等双遍零改动）；cfgA 轮 legacy GEMM 形态与轮间刷洗衬垫（模型稳定性承载位维持）；worker 自旋（M5/M7 封锁项）；`ref_exp2i` 分支形态（§8.3-E2 证伪无分支版）；11 个非 mega_moe ELF（继承 v4）。

### 8.3 试验与回退记录（防止复走弯路）

| # | 试验 | 结果 | 结论/回退原因 |
| --- | --- | --- | --- |
| E1 | 分块 GEMM kNb 16→32（sim） | gfsim **val=0x0001 fail 且更慢（414,732 vs 基线 287,642）**；gfrun R2=0 | 32 个 fp 累加器超出寄存器文件，后端把 acc[] 溢出到栈（GEMM 内循环退化为 `ldi.u` 栈读 + fmadd），溢出流量叠加 TLSU tile-store→GM 缺口模型缺陷 → gfsim 数值污染。回退 kNb=16 |
| E2 | `ref_exp2i` 四路分支 → csel 无分支选择链（sim） | gfsim **287,735 vs 280,065（+7.7K）** | 本测试数据 SwiGLU 输入 \|z\|~1e-37 → k 恒 0，分支版每调用仅 1~2 次块转移 + 4 op；无分支版恒 ~10 op —— **块式 SPE 的块转移比指令便宜**。证伪回退（数值本身逐位一致，纯性能负收益） |
| E3 | 解码 4 路展开（mt_dyn，无模式版） | gfsim fourpe **Deadlock @ cycle 1,355,436**（thread0 验证段 TPC 0x13096 BSTART 块 allocated 不完成，workers 在汇合自旋） | M7 类代码布局时序骰子（T0 同族）；sim/mt 同代码通过。mt_dyn 回退 2 路（exp4 实测 PASS 1,701,272），sim/mt 保留 4 路 |
| E4 | 解码 4 路展开 + 模式表（mt_dyn 终版代码再试） | gfsim fourpe **Deadlock @ cycle 778,736**（同为 thread0 块停摆） | 4 路展开在 mt_dyn 两次独立构建均落入停摆窗口，**二次证伪**；终版 mt_dyn 维持 2 路（1,633,876 PASS） |

### 8.4 v7 仿真结果（最终构建）

全部 3 个 mega_moe ELF：gfrun 退出码 0（`R2 = 0`）、gfsim 退出码 0（finisher `val=0x5555 pass`）。日志归档 `sim_logs/v7_opt/`。

**gfrun 功能仿真**：

| 算子 | ELF 变体 | 退出码 | 功能结果 | Total Block 数 | Total Inst 数 | v6 Block / Inst |
| --- | --- | --- | --- | ---: | ---: | --- |
| mega_moe | mega_moe_sim | 0 | PASS (R2=0) | 75,600 | 1,389,236 | 107,128 / 2,229,780 |
| mega_moe | mega_moe_sim_mt | 0 | PASS (R2=0) | 56,686 | 580,543 | 71,826 / 709,938 |
| mega_moe | mega_moe_sim_mt_dyn | 0 | PASS (R2=0) | 404,945 | 2,931,450 | 433,965 / 3,490,148 |

**gfsim 时序仿真（Unified Top-Down 与退休统计）**：

| 算子 | ELF 变体 | PE 模式 | Total Cycles | Retiring | Bad Spec | Frontend | Backend | Retired Inst | IPC |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mega_moe | mega_moe_sim | 单 PE | **214,848** | 24.32% | 0.02% | 0.02% | 75.64% | 351,903 | 1.64 |
| mega_moe | mega_moe_sim_mt | 4 PE | **382,376** | 46.09% | 11.31% | 0.01% | 42.59% | 1,396,890 | 3.65 |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | **1,633,876** | 45.43% | 17.54% | 0.00% | 36.83% | 5,937,790 | 3.63 |

注：Inst 降幅（−16~−38%）大于周期降幅（−9~−25%）→ IPC 略降（sim 1.95→1.64）：本轮删除的是填充/解码类"廉价"指令，剩余主体为 L2 往返绑定的 legacy GEMM 与受自旋竞争的验证块（见 §8.1-5）。

**引擎占用（mt_dyn）**: Cube Busy 21,796 / Vector Busy 17,122 / TLSU Busy 1,606,642（活跃占墙钟 98.3%；v6 为 1,900,967/99.4%）——TLSU 占用随验证段缩短下降 294K，仍为绑定资源（worker 自旋 + golden L2 读）。

### 8.5 版本对比

| 算子 | ELF 变体 | v6 Cycles | **v7 Cycles** | v6→v7 | v5 Cycles | v5→v7 | v4 Cycles | v4→v7 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mega_moe | mega_moe_sim | 287,642 | **214,848** | **1.34×** | 696,680 | **3.24×** | 358,296 | **1.67×** |
| mega_moe | mega_moe_sim_mt | 418,784 | **382,376** | **1.10×** | 568,852 | **1.49×** | 544,228 | **1.42×** |
| mega_moe | mega_moe_sim_mt_dyn | 1,912,274 | **1,633,876** | **1.17×** | 2,430,310 | **1.49×** | 2,312,312 | **1.42×** |

其余 11 个 ELF：v4 = v5 = v6 = v7（未改动）。

**v7 mt_dyn 阶段分解**（2026-10-15 pipe 实测修正，见 `kernels/solution/mega_moe_pipeline_analysis.md` §1；原进度采样估算已按实测校准）：pre = [0, 54,166]（54.2K，原估算 "~15K" 偏乐观——填充段 ~38K→~13K 成立，但模式表构建 + 首次增量递推引入额外段，净 −5.6K）；kernelA = [54,166, 64,663]（10.5K）；interA = [64,663, 882,762]（**818.1K**，v6 1,037.5K → −21.1%）；kernelB = [882,762, 903,835]（21.1K）；postB = [903,835, 1,633,875]（**730.0K**，v6 783.7K → −6.8%）。kernel 两段合计 31.6K（1.93%），形态与 v6 逐 op 一致。

### 8.6 结论与后续方向

1. **本轮收益全部来自驱动段指令削减**（解码/填充/x 预转换），kernel 段与 A5 对齐结构零改动，golden 数值逐位一致（三端 gfrun R2=0 + gfsim val=0x5555）。
2. **mt_dyn 的解码展开度存在硬约束**：4 路展开两次独立落入 M7 类停摆窗口（§8.3-E3/E4），2 路为已证安全上界；sim/mt 无此窗口（4 路保留）。时序骰子效应与 T0/T-E1 记录一致，供模型侧修复 M7 后重试。
3. **剩余周期主体**（mt_dyn ~1.60M）：cfgA legacy GEMM ~650K（**保护性 w 跨行访问 = 每 fmadd 一次 L2 往返**，v6 T-E1 已证不可替代，M7 修复前为模型税）+ cfgB 分块 GEMM ~500K + worker 自旋对 PE0 的发射口挤占（M5/M7 封锁）+ SwiGLU/统计/汇合 ~150K。
4. **后续方向**（更新 v6 §6.3 排序）：① cfgA 轮验证分块化（−~0.5M，前置条件仍为 M7 修复；本轮 §8.1-2 的 xrow 已在不触碰保护形态的前提下吃掉 x 侧冗余）；② worker 自旋减负（M5/M7）；③ mt_dyn 解码 4 路展开重试（M7 修复后）；④ wave 分派接入 mt_dyn（~3K，M7 修复后）。
5. **流水图/并行度图**：已于 2026-10-15 按 v7 终版 ELF 全量重生成（全程 val=0x5555）：`kernels/solution/mega_moe_pipe.out`（2.36GB）+ `mega_moe_swim.json` / `_kA` [50K, 70K] / `_kB` [878K, 908K] / `_verify` [400K, 500K]（三窗口门控防 OOM）。阶段实测与本节校正记录见 `kernels/solution/mega_moe_pipe_swan_summary.md` 与 `mega_moe_pipeline_analysis.md`。

### 8.7 本轮修改文件清单

| 文件 | 修改内容 |
| --- | --- |
| `test/solution/mega_moe/src/mega_moe_sim.cpp` | §8.2-#1（解码 4 路瘦身）/#2（xrow）/#3（模式表）/#4（死代码删除） |
| `test/solution/mega_moe/src/mega_moe_sim_mt.cpp` | §8.2-#1（解码 4 路瘦身）/#2（xrow）/#3（模式表） |
| `test/solution/mega_moe/src/mega_moe_sim_mt_dyn.cpp` | §8.2-#1（解码 **2 路**瘦身，§8.3-E3/E4）/#2（xrow，legacy+分块双路径）/#3（模式表） |
| `test/solution/perf_table_case_filling_guide.md` | 性能表格数据更新至 v7（§2/§5/§6） |
| `kernels/solution/mega_moe_pipeline_analysis.md` | 追加 v7 后记（§7） |

kernel 头文件（`kernels/solution/mega_moe/*.hpp`）与其余 11 个 ELF 源码**零改动**。

---

## 9. v8 一轮（2026-10-15）：验证段 golden 4-PE 并行分割（依据 v7 流水图/并行度图）

### 9.1 优化依据（v7 pipe/swim 实测，`kernels/solution/mega_moe_pipeline_analysis.md`）

1. **postB [903,835, 1,633,875] = 730.0K cycle 占 Total 44.7%**：其中 PE0 独占的 cfgB golden（分块 GEMM ~630K + 解码 86K）期间，TID1-3 在 mtRendezvousDynFin 空转 —— swim verify 窗口实测 TID02 921% / TID03 367% 纯自旋占用；4 PE 标量侧共享 1 个 SPE（4 SMT），自旋轮询（~0.56 块/cyc）与 golden 块竞争发射口，且 PE0 单线程的 fmadd 依赖链无跨线程 MLP。mt 变体同构（v7 gfsim 日志：TID1-3 在 0x123d2 STD COND 自旋，验证段 ~280K of 382K）。
2. **cfgB 轮验证在全部 kernel 之后**（无后续 kernel/combine 行需要保护）——不受 cfgA 轮 legacy 形态的 M7 模型稳定性约束（该约束只绑定 cfgA，见 §8.1/compute_golden 头注）；mt 变体单 cfg 且本就为分块形态，同理无保护约束。**cfgA 轮路径（legacyGemm=true）与全部 kernel/屏障协议零触碰。**

### 9.2 优化内容

| # | 优化 | 落点变体 | 内容 | 数值不变性论证 |
| --- | --- | --- | --- | --- |
| 1 | cfgB 验证 golden 4-PE 并行分割 | mt_dyn | PE0 独占 verify(cfgB) 改为全 PE 分片五段：① w1Dbl/w2Dbl 按行连续 ceil 分片解码（行 = (e,k) 组，行宽 = cacheline 整数倍，单写者）→ ② 96KB cached-load 发布扫描（`v8PublishSweep`，同 PE0 刷洗衬垫模式——跨 PE 无 snoop，普通 store 脏行须容量驱逐写回 L2 后他 PE cached load 才可见，见 mtRendezvousDynFin 头注机理）→ ③ `mtBarrierDyn(8*1+6=14)` → ④ GEMM token 连续分片（kNb=16 分块形态逐字节复制，per-PE scratch 数组）→ 发布扫描 → `mtBarrierDyn(8*1+7=15)` → ⑤ PE0 独占比较尾部 `verify_tail_cfgB`（tokRef + abs/rel 统计 + 诊断码）。相位 14/15 为单调相位方案 c=1 槽位空闲位（已用 9/10-12/13）。g_e4m3Lut 复用 PE0 cfgA 轮构建（其后的 96KB 刷洗衬垫确定性写回 L2） | 分片为行/token 纯函数划分：解码值、GEMM 每元素累加序列（k 升序同款 fmadd 链）、比较次序均与串行版逐字节一致 → golden 与两误差统计量逐位不变（max 结合/交换）；gfrun R2=0 + gfsim val=0x5555 佐证 |
| 2 | 验证 golden 4-PE 并行分割（mt 镜像） | mt | 同款五段：per-PE LUT 冗余构建（mt 无 cfgA 轮刷洗可发布共享 LUT，各 PE 自建本地副本 `sV8LutPE[tid]` 消除跨 PE 可见性）→ decode 行分片（保留 mt 的 4 路展开）→ `v8PublishSweepMt`（`mega_moe::sMtEvictSpan`）→ `mtBarrier(5)` → GEMM token 分片（栈上 scratch 天然 per-PE）→ 扫描 → `mtBarrier(6)` → PE0 独占 `verify_tail_mt`。相位 5/6 空闲（已用 1=驱动发布，2/3/4=kernel 内部）；mtBarrier 为写方定向驱逐版，安静窗口安全 | 同 #1；mt 单 cfg 无 legacy 保护约束 |

**未改动**：sim 变体（单 PE 无可并行面，ELF 逐位一致）；cfgA 轮全部路径（genInputs/kernelA/legacy verify/刷洗衬垫/屏障相位 0-13）；全部 kernel 头文件；其余 11 个算子。原 `verify()`/`compute_golden()` 保留（cfgA 路径原样调用）。

### 9.3 v8 仿真结果（最终构建，md5: mt_dyn `ce63028c` / mt `f71a4fb5` / sim `7635313f` 不变）

全部 3 个 mega_moe ELF：gfrun 退出码 0（`R2 = 0`）、gfsim 退出码 0（finisher `val=0x5555 pass`）。日志归档 `sim_logs/v8_opt/`。

**gfrun 功能仿真**：

| 算子 | ELF 变体 | 功能结果 | Total Block 数 | Total Inst 数 | v7 Block / Inst |
| --- | --- | --- | ---: | ---: | --- |
| mega_moe | mega_moe_sim | PASS (R2=0) | 75,600 | 1,389,236 | 逐位一致（ELF 未动） |
| mega_moe | mega_moe_sim_mt | PASS (R2=0) | 46,002 | 560,607 | 56,686 / 580,543 |
| mega_moe | mega_moe_sim_mt_dyn | PASS (R2=0) | 353,761 | 2,700,063 | 404,945 / 2,931,450 |

**gfsim 时序仿真**：

| 算子 | ELF 变体 | PE 模式 | Total Cycles | Retiring | Bad Spec | Frontend | Backend | Retired Inst | IPC |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mega_moe | mega_moe_sim | 单 PE | 214,848 | 24.32% | 0.02% | 0.02% | 75.64% | 351,903 | 1.64 |
| mega_moe | mega_moe_sim_mt | 4 PE | **162,234** | 46.61% | 0.10% | 0.03% | 52.87% | 604,994 | 3.73 |
| mega_moe | mega_moe_sim_mt_dyn | 4 PE | **1,079,112** | 45.26% | 11.39% | 0.01% | 43.11% | 3,907,263 | 3.62 |

**引擎占用（mt_dyn）**: TLSU Busy Resource-Cycles 1,063,437（v7 1,606,642，**−543K** —— 自旋轮询流量随验证段并行化消失）；mt 变体 TLSU Busy 160,435。mt 的 Bad Spec 11.31% → 0.10%（自旋轮询的分支预测压力消失）。

### 9.4 版本对比与阶段分解

| 算子 | ELF 变体 | v7 Cycles | **v8 Cycles** | v7→v8 | v6 Cycles | v6→v8 | v5 Cycles | v5→v8 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mega_moe | mega_moe_sim | 214,848 | 214,848 | 1.00× | 287,642 | 1.34× | 696,680 | 3.24× |
| mega_moe | mega_moe_sim_mt | 382,376 | **162,234** | **2.36×** | 418,784 | 2.58× | 568,852 | **3.51×** |
| mega_moe | mega_moe_sim_mt_dyn | 1,633,876 | **1,079,112** | **1.51×** | 1,912,274 | 1.77× | 2,430,310 | **2.25×** |

其余 11 个 ELF：v4 = v5 = v6 = v7 = v8（未改动）。

**v8 mt_dyn 阶段分解（pipe B.HINT 实测）**：pre = [0, 54,205]（54.2K，与 v7 持平）；kernelA = [54,205, 64,749]（10.5K）；interA = [64,749, 870,390]（**805.6K**，v7 818.1K → −1.5%，布局微移）；kernelB = [870,390, 891,232]（20.8K）；**postB = [891,232, 1,079,111]（187.9K，v7 730.0K → −74.3%，3.9×）**。kernel 两段合计 31.4K（2.91%）。验证段结构反转：cfgB 验证（187.9K）首次低于 cfgA 验证承载段 interA（805.6K）。

### 9.5 结论与后续方向

1. **本轮收益来自验证段并行化而非指令削减**（gfrun Inst mt_dyn 仅 −7.9%，周期 −34.0%）：消除 "PE0 独占 golden + 3 worker 空转竞争发射口" 的结构性浪费 —— 4 SMT 线程各持独立 fmadd 依赖链，共享 SPE 的发射带宽首次被验证段满载利用。mt 变体收益更大（2.36×，验证段占比更高且无 cfgA legacy 段）。
2. **不依赖模型修复的驱动段并行化空间已收割完毕**：cfgB/mt 验证已 4-PE 分片；剩余主体 interA 805.6K（74.6%）为 cfgA legacy GEMM —— M7 模型稳定性承载位（v6 T-E1 + v4 六探针共 7 种替代均证伪，"保护因子的成本即保护本身"），维持至模型侧修复。
3. **后续方向**（更新 §8.6 排序）：① cfgA 轮验证分块化 + 并行分片（M7 修复后：legacy 形态解除 → 分块 + 4-PE 分片双收益，预期 Total → ~400-500K）；② worker 自旋减负（M5/M7；本轮后自旋窗口仅剩 PE0 比较尾部 ~8K cycle + 屏障等待，收益已边际化）；③ wave 分派接入 mt_dyn（~3K，M7 修复后）；④ TMOV 消除（M1 封锁，~6K）。
4. **时序骰子记录**：本轮对 mt_dyn 验证段（E3/E4 两次停摆的高敏区）与 mt 尾段的大规模重构一次通过（双变体 val=0x5555），布局骰子未落入 M7 停摆窗口。

### 9.6 本轮修改文件清单

| 文件 | 修改内容 |
| --- | --- |
| `test/solution/mega_moe/src/mega_moe_sim_mt_dyn.cpp` | §9.2-#1：v8 并行分割块（`compute_golden_decode_slice` / `compute_golden_gemm_slice` / `verify_tail_cfgB` / `v8PublishSweep` / per-PE scratch）+ main 尾段改并行路径（相位 14/15） |
| `test/solution/mega_moe/src/mega_moe_sim_mt.cpp` | §9.2-#2：mt 镜像（`compute_golden_decode_slice_mt` / `compute_golden_gemm_slice_mt` / `verify_tail_mt` / `v8PublishSweepMt` / per-PE LUT）+ main 尾段（相位 5/6） |
| `test/solution/perf_table_case_filling_guide.md` | 性能表格数据更新至 v8（§2/§5） |
| `output/solution/simulation_results.md` | 本节 §9 |
| `kernels/solution/mega_moe_pipe_swan_summary.md` / `mega_moe_pipeline_analysis.md` | v8 图件与阶段数据更新（图件按 v8 ELF 重生成） |

kernel 头文件（`kernels/solution/mega_moe/*.hpp`）、sim 变体源码与其余 11 个算子源码**零改动**。

---

## 10. v9 一轮（2026-10-15）：cfgA 轮验证并行分割 —— 证伪与回退（E5，第 8 次证伪）

### 10.1 试验内容

依据 v8 图件（interA 805.6K = Total 74.6% 为 PE0 独占 cfgA legacy 验证 + TID1-3 自旋），将 v8 的并行分片模式延伸到 cfgA 轮：**legacy 访存形态 per-PE 逐字节保留**（每个 PE 对其 token 切片执行与原 PE0 完全相同的 n-outer/k-inner 跨行步进读循环，w 访问序列 / fmadd 依赖链 / xrow 预转换不变，L2 读流量同型且密度 ~4×），decode 行分片 + 发布扫描 + 屏障 6/7 + legacy GEMM token 分片 + PE0 比较尾部 + 屏障 8 汇合（相位重排解决覆写竞争防护）。gfrun R2=0（golden 逐位正确）。

### 10.2 结果与诊断

- **gfsim val=0x0001 fail**（Total 544,604 —— 性能潜力 −49.6% 已证，但正确性失败）。
- **诊断构建**（verdict 临时编码 0x00A0|failA / 0x00B0|rcB）实测 **val=0x00B9**：**failA = 0**（cfgA 自身验证完整通过——并行 golden 正确、cfgA kernel 输出完好、比较时序无 TSTORE 竞态），失败在 **rcB = 9**（cfgB kernel 输出 maxAbsErr ≥ 0.5 —— 即记录在案的 **cfgB combine 行被 TLSU tile-store→GM 缺口污染** 机制）。
- 完整回退后重编译 md5 = `ce63028c`（与 v8 终版逐字节一致，结果直接继承）。

### 10.3 结论

1. **M7 保护对 cfgA 轮执行形态的敏感度超出发形态本身**：即使 legacy 访存序列 per-PE 逐字节保留，仅改变 (a) 并发度 1→4、(b) 窗口时长 ~3× 缩短、(c) worker 自旋轮询（~0.56 块/cyc 的 TLSU uncached 流量）消失为有效负载 —— 三者任一（或组合）即破坏 cfgB kernel 的稳定性承载。至此 cfgA 轮重构共 **8 次独立证伪**（v4 六探针 + v6 T-E1 分块+衬垫 + v9 并行分片），"保护因子的成本即保护本身" 进一步收敛为 "**保护是原执行轨迹的涌现性质（形态 × 并发 × 时序 × 自旋），不可局部替换**"。
2. **轮序反转亦不可行**（分析排除，未实测）：第二位 kernel 需要第一轮验证为 legacy PE0 形态 —— 若 cfgB 先行，其 legacy 验证（cfgB GEMM 工作量 2×cfgA）≈ 1.5M cycle，劣于现状。
3. **v8（mt_dyn 1,079,112 / mt 162,234 / sim 214,848）确认为不依赖模型侧修复的最终可达优化点**；interA 805.6K（74.6%）为 cfgA legacy 模型税，唯一解锁路径为模型侧修复 M7（TLSU tile-store→GM 缺口 + 轮间 L2 状态交互），修复后 v8 的分片基础设施（发布扫描/屏障/比较尾部）可直接复用于 legacy 形态解除的 cfgA 分块+并行双收益（预期 Total → ~400-500K）。
4. 后续小项（收益 ≤2%，未实施）：genInputs 填充游走循环展开（pre −~15K）、模式表单构建共享（pre −~10K，需屏障+发布）—— 相对时序骰子风险收益比不佳，留待模型修复后一并处理。
