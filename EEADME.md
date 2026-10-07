# solution 算子仿真结果汇总报告（gfrun / gfsim）— mega_moe / group_token_old / group_token_vec / moe_combine / moe_dispatch

- **日期**: 2026-09-30
- **对象**: `output/solution/` 下 **5 类算子共 14 个 ELF**（mega_moe × 3 + group_token_old × 2 + group_token_vec × 3 + moe_combine × 3 + moe_dispatch × 3）
  - mega_moe 部分原单独成文 `mega_moe_sim_results.md`（保留作独立存档），本篇为全量汇总
- **仿真器**: SuperScalarModel `bin/gfrun`（功能模型）与 `bin/gfsim`（时序模型）
  - 模型版本: git `9905a05f1a61e7af48b82401cd87dfd16ee3f734`（`9905a05f Merge pull request #885 from LinxISA/fix/subview_vec`）
  - 二进制构建时间: gfrun 2026-09-30 09:58 / gfsim 2026-09-30 10:00
  - 所有命令均在 SuperScalarModel 仓库根目录执行（配置相对路径依赖 CWD）
- **原始日志**: 全部保存在本目录 `sim_logs/{mega_moe,group_token_old,group_token_vec,moe_combine,moe_dispatch}/` 下

## 0. 判据说明

- **gfrun**: `Suaccelss to Reach the End of Benchmark! R2 = 0` 为 PASS；非 0 为各驱动分层诊断码（各源码 `main` 返回值）。
- **gfsim**:
  - mega_moe 系驱动写 `0x10009000` test-finisher（`0x5555` = PASS），gfsim 侧可自动判读；
  - 其余 4 类算子驱动**均不写 finisher**，gfsim 侧无 finisher 判读通道；
    完成判据为 **正常退出（exit_code=0，经 exit 系统调用 lockstep 收尾）+ 完整产出 `SuperScalar NPU Stats`**，
    数值正确性以同 ELF 的 gfrun R2=0 为准（见 §4 建议）。
- 全部 `_mt` 系 ELF 的 4-PE barrier 均为 **sPhaseDone/mtBarrier 软件自旋**（kernel 中无 B.IOS 集体指令），
  gfsim PE 自动检测按 B.IOS binder 扫描 → 全部误判 single-PE，4-PE 需显式 `--conf mt` 或 `--conf fourpe`。

## 1. ELF 一览与运行约定（14 ELF）

| 算子 | ELF | 测试 shape | 执行模型 | gfrun 线程 |
|---|---|---|---|---|
| mega_moe | `solution_mega_moe_mega_moe_sim_BS16_H32_HD64.elf` | MegaMoe A8W8：BS=16, H=32, HD=64（16 伪核串行覆盖） | 单 PE | 1 |
| mega_moe | `solution_mega_moe_mega_moe_sim_mt_BS16_H32_HD64.elf` | 同上（4 PE × 4 伪核，sPhaseDone 自旋 barrier） | 4-PE SPMD | 4 |
| mega_moe | `solution_mega_moe_mega_moe_sim_mt_dyn.elf` | cfgA {bs=16, h=32, hd=64} + cfgB {bs=18, h=32, hd=128} 动态 | 4-PE 动态 shape | 4 |
| group_token_old | `solution_group_token_old_group_token_old.elf` | BS=512, TopK=16, EPR=4, SPN=2（纯标量实现） | 单 PE | 1 |
| group_token_old | `solution_group_token_old_group_token_old_mt.elf` | 同上（3-phase 软件流水） | 4-PE SPMD | 4 |
| group_token_vec | `solution_group_token_vec_group_token_vec.elf` | BS=512, TopK=16, EPR=4, SPN=2（tile/vec 实现） | 单 PE | 1 |
| group_token_vec | `solution_group_token_vec_group_token_vec_mt.elf` | 同上 | 4-PE SPMD | 4 |
| group_token_vec | `solution_group_token_vec_group_token_vec_mt_dyn.elf` | cfgA 同上 + cfgB {bs=517, topK=16, epr=8, epp=128} 尾块/专家域动态 | 4-PE 动态 shape | 4 |
| moe_combine | `solution_moe_combine_moe_combine_v2.elf` | BS=8, H=128, K=4, Expanded=32, TILE_W=128 | 单 PE V2 | 1 |
| moe_combine | `solution_moe_combine_moe_combine_mt.elf` | 同上 | 4-PE SPMD | 4 |
| moe_combine | `solution_moe_combine_moe_combine_mt_dyn.elf` | cfgA 同上 + cfgB {BS=7, H=128, K=3, Expanded=21 非整除} | 4-PE 动态 shape | 4 |
| moe_dispatch | `solution_moe_dispatch_moe_dispatch_v2.elf` | BS=8, H=128, K=4, ExpertNum=4, TILE_W=128 | 单 PE V2 | 1 |
| moe_dispatch | `solution_moe_dispatch_moe_dispatch_mt.elf` | 同上 | 4-PE SPMD | 4 |
| moe_dispatch | `solution_moe_dispatch_moe_dispatch_mt_dyn.elf` | cfgA 同上 + cfgB {BS=7, H=128, K=3, ExpertNum=5 非整除} | 4-PE 动态 shape | 4 |

## 2. gfrun 功能仿真结果 —— 14/14 PASS

### 2.1 汇总

| ELF | gfrun 线程 | 退出码 | Total Blocks | Total Insts | R2 | 结论 |
|---|---|---|---|---|---|---|
| mega_moe_sim | 1 | 0 | 167,517 | 1,910,783 | 0 | **PASS** |
| mega_moe_sim_mt | 4 | 0 | 200,311 | 2,832,670 | 0 | **PASS** |
| mega_moe_sim_mt_dyn | 4 | 0 | 1,009,958 | 10,665,713 | 0 | **PASS** |
| group_token_old | 1 | 0 | 470,080 | 3,037,909 | 0 | **PASS** |
| group_token_old_mt | 4 | 0 | 512,367 | 3,387,752 | 0 | **PASS** |
| group_token_vec | 1 | 0 | 468,713 | 2,878,731 | 0 | **PASS** |
| group_token_vec_mt | 4 | 0 | 683,159 | 4,187,164 | 0 | **PASS** |
| group_token_vec_mt_dyn | 4 | 0 | 2,590,312 | 14,910,165 | 0 | **PASS** |
| moe_combine_v2 | 1 | 0 | 1,233 | 6,342 | 0 | **PASS** |
| moe_combine_mt | 4 | 0 | 28,199 | 298,808 | 0 | **PASS** |
| moe_combine_mt_dyn | 4 | 0 | 113,010 | 767,147 | 0 | **PASS** |
| moe_dispatch_v2 | 1 | 0 | 10,796 | 62,891 | 0 | **PASS** |
| moe_dispatch_mt | 4 | 0 | 14,810 | 105,158 | 0 | **PASS** |
| moe_dispatch_mt_dyn | 4 | 0 | 53,273 | 336,582 | 0 | **PASS** |

### 2.2 分 PE 明细（Blocks / Insts）

**mega_moe_sim（单 PE）**：T0 = 167,517 / 1,910,783

**mega_moe_sim_mt（4 PE）**
```
PE0(leader, 数据生成+kernel分片+golden验证) = 166,114 / 1,904,305
PE1 = PE2 = PE3 = 11,399 / 309,455   (worker: 冗余数据生成+kernel分片后直接返回)
```

**mega_moe_sim_mt_dyn（4 PE，双 shape）**
```
PE0(leader, 双 cfg 数据生成+kernel+逐组 golden 验证) = 513,138 / 7,787,163
PE1 = 165,606 / 959,145 ; PE2 = 165,606 / 959,465 ; PE3 = 165,608 / 959,940
```

**group_token_old（单 PE，纯标量）**：T0 = 470,080 / 3,037,909

**group_token_old_mt（4 PE）**
```
PE0(leader, 数据生成+3-phase kernel+标量参考验证) = 473,643 / 2,976,611
PE1 = PE2 = PE3 = 12,908 / 137,047   (worker: kernel 分片后早退)
```

**group_token_vec（单 PE，tile/vec）**：T0 = 468,713 / 2,878,731

**group_token_vec_mt（4 PE）**
```
PE0(leader) = 515,507 / 3,170,722 ; PE1=PE2=PE3 = 55,884 / 338,814
```

**group_token_vec_mt_dyn（4 PE，双 shape）**
```
PE0(leader, 双 cfg 数据生成+kernel+逐组 golden 验证) = 943,291 / 6,204,743
PE1 = 549,007 / 2,901,914 ; PE2 = 549,006 / 2,901,764 ; PE3 = 549,008 / 2,901,744
```

**moe_combine_v2（单 PE）**：T0 = 1,233 / 6,342

**moe_combine_mt（4 PE）**
```
PE0(leader) = 14,801 / 132,725 ; PE1=PE2=PE3 = 4,466 / 55,361
```

**moe_combine_mt_dyn（4 PE，双 shape）**
```
PE0 = 37,724 / 286,980 ; PE1 = PE2 = 25,096 / 160,073 ; PE3 = 25,094 / 160,021
```

**moe_dispatch_v2（单 PE）**：T0 = 10,796 / 62,891

**moe_dispatch_mt（4 PE）**
```
PE0(leader) = 10,196 / 59,174 ; PE1 = 1,537/15,291 ; PE2 = 1,537/15,323 ; PE3 = 1,540/15,370
```

**moe_dispatch_mt_dyn（4 PE，双 shape）**
```
PE0 = 17,678 / 116,124 ; PE1 = 11,865/73,426 ; PE2 = 11,865/73,486 ; PE3 = 11,865/73,546
```

## 3. gfsim 时序仿真结果

### 3.1 汇总（与 §2.1 gfrun 表同口径：一 ELF 一行）—— **8 PASS / 6 FAIL**

| ELF | gfsim 线程 | 退出码 | Total Cycles | Total Insts | R2 | 结论 |
|---|---|---|---|---|---|---|
| mega_moe_sim | 1 | 70 | 1,058,314 | 2,012,901 | 0x0001 | **FAIL** |
| mega_moe_sim_mt | 4 | 134 | — | — | — | **FAIL** |
| mega_moe_sim_mt_dyn | 4 | 134 | — | — | — | **FAIL** |
| group_token_old | 1 | 0 | 1,117,803 | 3,216,446 | — | **PASS** |
| group_token_old_mt | 4 | 134 | — | — | — | **FAIL** |
| group_token_vec | 1 | 0 | 478,066 | 625,090 | — | **PASS** |
| group_token_vec_mt | 4 | 134 | — | — | — | **FAIL** |
| group_token_vec_mt_dyn | 4 | 0 | 1,275,337 | 4,638,761 | — | **PASS** |
| moe_combine_v2 | 1 | 0 | 5,188 | 7,471 | — | **PASS** |
| moe_combine_mt | 4 | 0 | 57,341 | 223,530 | — | **PASS** |
| moe_combine_mt_dyn | 4 | 0 | 88,727 | 347,701 | — | **PASS** |
| moe_dispatch_v2 | 1 | 0 | 9,749 | 20,384 | — | **PASS** |
| moe_dispatch_mt | 4 | 0 | 18,131 | 63,947 | — | **PASS** |
| moe_dispatch_mt_dyn | 4 | 134 | — | — | — | **FAIL** |

注（表项口径）：
- 运行配置：单 PE ELF 为默认配置（auto single-PE），`_mt` ELF 为推荐配置 `--conf fourpe`（默认配置必 livelock、`--conf mt` 必停顿，见 §3.4/§3.5）。
- Total Cycles / Total Insts 取自 gfsim `SuperScalar NPU Stats`（后者为 Retired Instruction Number，4-PE 运行为 4 SThreads 合计，与 gfrun Total Insts 口径不同）；FAIL 且被截断者无 stats，故为 —。
- R2：gfsim 侧 guest 返回值不可直接观测 —— mega_moe_sim 经 finisher 通道得 0x0001（`0x5555`=PASS，数值自检失败，见 §3.3），其余 ELF 无 finisher 通道为 —（以 gfrun R2=0 为准，见 §4 建议 3）。
- FAIL 原因：mega_moe_sim 为数值自检失败；其余 5 个 `_mt` ELF 均为模型侧 exit-lockstep 断言截断（workload 已执行、无 stats，见 §3.6），非算子错误。
- 全部分配置（默认 / `--conf mt` / `--conf fourpe`，共 32 次）运行明细见 §3.1.1。

#### 3.1.1 分配置运行明细（32 次运行：8 PASS / 24 FAIL；9 次产出完整 stats，其中 mega_moe_sim 数值自检 FAIL）

| ELF | 配置 | 退出码 | Total Cycles | 结论 | 说明 |
|---|---|---|---|---|---|
| mega_moe_sim | 默认（single-PE） | 70 | **1,058,314** | **FAIL**（数值自检） | 跑完并产出完整 stats，但 finisher val=0x0001（见 §3.3） |
| mega_moe_sim_mt | 默认 | 124 | — | **FAIL**（配置错误） | Livelock：mtBarrier 自旋（误判 single-PE） |
| mega_moe_sim_mt | `--conf mt` | 134 | — | **FAIL**（模型侧） | 时序模型停顿 → ResVerify 死锁断言 @ cycle 317,561 |
| mega_moe_sim_mt | `--conf fourpe` | 134 | — | **FAIL**（模型侧） | 末端 exit lockstep 断言 @ cycle 323,593（见 §3.6） |
| mega_moe_sim_mt_dyn | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（自旋于 BPC 0x117b0） |
| mega_moe_sim_mt_dyn | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 72,420 |
| mega_moe_sim_mt_dyn | `--conf fourpe` | 134 | — | **FAIL**（模型侧） | 全量 workload 执行完毕（≥3.6M 周期）后被末端 exit lockstep 断言截断 @ cycle 3,638,308，无 stats（见 §3.6） |
| group_token_old | 默认（single-PE） | 0 | **1,117,803** | **PASS** | 完成，stats 完整 |
| group_token_old_mt | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| group_token_old_mt | `--conf mt` | 134 | — | **FAIL**（模型侧） | 末端 exit lockstep 断言 @ cycle 244,623 |
| group_token_old_mt | `--conf fourpe` | 134 | — | **FAIL**（模型侧） | 末端 exit lockstep 断言 @ cycle 242,720 |
| group_token_vec | 默认（single-PE） | 0 | **478,066** | **PASS** | 完成，stats 完整 |
| group_token_vec_mt | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| group_token_vec_mt | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 125,673 |
| group_token_vec_mt | `--conf fourpe` | 134 | — | **FAIL**（模型侧） | 末端 exit lockstep 断言 @ cycle 457,690 |
| group_token_vec_mt_dyn | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| group_token_vec_mt_dyn | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 351,813 |
| group_token_vec_mt_dyn | `--conf fourpe` | 0 | **1,275,337** | **PASS** | 完成（需长超时，见 §3.5 注） |
| moe_combine_v2 | 默认（single-PE） | 0 | **5,188** | **PASS** | 完成，stats 完整 |
| moe_combine_mt | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| moe_combine_mt | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 64,811 |
| moe_combine_mt | `--conf fourpe` | 0 | **57,341** | **PASS** | 完成，stats 完整 |
| moe_combine_mt_dyn | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| moe_combine_mt_dyn | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 61,018 |
| moe_combine_mt_dyn | `--conf fourpe` | 0 | **88,727** | **PASS** | 完成，stats 完整 |
| moe_dispatch_v2 | 默认（single-PE） | 0 | **9,749** | **PASS** | 完成，stats 完整 |
| moe_dispatch_mt | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| moe_dispatch_mt | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 24,509 |
| moe_dispatch_mt | `--conf fourpe` | 0 | **18,131** | **PASS** | 完成，stats 完整 |
| moe_dispatch_mt_dyn | 默认 | 124 | — | **FAIL**（配置错误） | Livelock（同上） |
| moe_dispatch_mt_dyn | `--conf mt` | 134 | — | **FAIL**（模型侧） | ResVerify 死锁断言 @ cycle 23,767 |
| moe_dispatch_mt_dyn | `--conf fourpe` | 134 | — | **FAIL**（模型侧） | 末端 exit lockstep 断言 @ cycle 45,448 |

结论判据（与 §0 对应）：
- **PASS**：exit_code=0，经 exit 系统调用 lockstep 正常收尾，完整产出 `SuperScalar NPU Stats`。
- **FAIL**（数值自检）：跑完且有 stats，但 finisher 数值比对不通过（mega_moe_sim，val=0x0001 vs PASS 值 0x5555）。
- **FAIL**（配置错误）：默认配置对软件 barrier 型 `_mt` ELF 误判 single-PE → livelock 超时，属使用方式问题（换 `--conf fourpe` 后 4/9 通过），非算子/模型缺陷。
- **FAIL**（模型侧）：gfsim 模型侧限制（`--conf mt` ResVerify 死锁 8 例 / 末端 exit lockstep watchdog 断言 6 例），workload 本身已执行，非算子错误（§3.5/§3.6）。
- 注：8 个 PASS 运行中除 mega_moe 外的驱动无 finisher 写出，gfsim 侧数值正确性不可判读，以同 ELF 的 gfrun R2=0 为准（§4 建议 3）。

### 3.2 完成运行的时序 stats（9 个）

| ELF | 配置 | Total Cycles | Retired Inst | IPC | BPC | Top-Down（Retire/BadSpec/Backend） | Cube TileCyc | Vector TileCyc | TLSU TileCyc | LSU/TLSU Util | invariants 异常 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| mega_moe_sim | single-PE | 1,058,314 | 2,012,901（含 Cube TileOp 48） | 1.902 | 0.158 | 23.77 / 0.00 / 76.14 % | 3,888 | 2,077 | 5,179 | 75.21% | 干净，但 **finisher 数值自检 FAIL** |
| group_token_old | single-PE | 1,117,803 | 3,216,446 | 2.877 | 0.421 | 35.97 / 8.41 / 55.62 % | 0 | 0 | 0 | 97.50% | nuke=254 |
| group_token_vec | single-PE | 478,066 | 625,090 | 1.308 | 0.173 | 16.34 / 3.37 / 79.91 % | 0 | 134,571 | 113,901 | 95.52% | scb_waw_tile=41 |
| moe_combine_v2 | single-PE | 5,188 | 7,471 | 1.440 | 0.238 | 18.00 / 35.08 / 45.30 % | 0 | 1,860 | 3,323 | 94.97% | 干净 |
| moe_dispatch_v2 | single-PE | 9,749 | 20,384 | 2.091 | 0.223 | 26.14 / 4.46 / 68.61 % | 0 | 1,457 | 3,835 | 97.78% | 干净 |
| moe_combine_mt | fourpe | 57,341 | 223,530 | 3.898 | 0.315 | 48.73 / 0.04 / 51.06 % | 0 | 1,206 | 10,985 | 99.33% | 干净 |
| moe_combine_mt_dyn | fourpe | 88,727 | 347,701 | 3.919 | 0.345 | 48.98 / 0.04 / 50.84 % | 0 | 2,344 | 17,756 | 99.53% | 干净 |
| moe_dispatch_mt | fourpe | 18,131 | 63,947 | 3.527 | 0.347 | 44.09 / 0.17 / 55.02 % | 0 | 1,995 | 16,188 | 97.53% | nuke=1 |
| group_token_vec_mt_dyn | fourpe | 1,275,337 | 4,638,761 | 3.637 | 0.464 | 45.47 / 2.80 / 51.54 % | 0 | 74,102 | 290,865 | 86.52% | scb_waw_tile=254 |

注：
- 除 mega_moe_sim 外，其余 8 个完成运行 `exit_code=0`；4-PE 运行 `sb_lockstep_exit_parks = 4`（4 PE 均经 exit 系统调用正常收尾），全部 `stq_conservation=OK`。
- Cube TileOp 仅 mega_moe_sim 出现（48 个，occupancy 0.35%，Equivalent MAC 49,152 → 0.000153 TFLOPS @1.65GHz）；其余算子无 CUBE 路径。
- `--conf fourpe` 下 Retired Inst 为 4 SThreads 合计的标量退役数（STID0-3 BROB occupancy 拆分可见），Vector/TLSU tile cycles 为 4 PE 汇总；与 gfrun "Total Inst" 统计口径不同，不宜直接对减。

**判读**：
- **group_token_old vs group_token_vec（同 shape BS=512/TopK=16/EPR=4/SPN=2 的单 PE 对照）**：
  旧版纯标量实现 1,117,803 cycles（0 tileop，LSU 利用率 97.5% 为瓶颈，BadSpec 8.4% 主要来自 254 次 nuke 恢复）；
  tile/vec 版 478,066 cycles（Vector 134,571 + TLSU 113,901 tile cycles），**周期数降至旧版 42.8%（约 2.34× 加速）**，
  但 Retiring 仅 16.3%、Backend 79.9% —— 标量驱动/验证代码仍主导 wall time，kernel tile 路径占比小，与 mega_moe 单 PE 结论同型。
- **4-PE 运行 IPC 3.5~3.9**（4 SThreads 并行 retire），moe 系 4-PE 版周期数受 leader 端数据生成+golden 验证主导；
  moe_combine_mt（57K cycles）vs 单 PE v2（5.2K cycles）不可直接对比 —— mt 版驱动负载远大于 v2 精简版（gfrun 298,808 vs 6,342 insts）。
- **group_token_vec_mt_dyn（fourpe）**：双 shape 全流程（含 cfgA/cfgB 两轮数据生成+kernel+验证）1,275,337 cycles，
  期间 peId=0 出现 254 次 `scb_waw_violation`（与 invariants `scb_waw_tile=254` 一致），最终仍干净收尾 —— 为可恢复事件，建议模型侧复核（见 §3.7）。

### 3.3 mega_moe_sim（单 PE）—— 完整时序 stats 与数值自检 FAIL 判读

命令：`bin/gfsim -f <elf>`（自动 single-PE），仿真 237s，正常跑完并输出完整 `SuperScalar NPU Stats`：

| 指标 | 数值 |
|---|---|
| **Total Cycles** | **1,058,314** |
| Retired Instruction Number | 2,012,901（Scalar 2,012,901 / Vec 0） |
| Retired Cube TileOp Number | 48 |
| Blocks Per Cycle | 0.158285 |
| **IPC** | **1.901988** |
| Average BROB occupancy | 93.47 |
| MPKB / MPKI | 1.40 / 0.12 |
| Top-Down (L1) | Retiring 23.77% / Bad Spec 0.00% / Frontend 0.01% / CMD 0.08% / **Backend 76.14%** |

TileOp 明细：

| 模块 | Total Cycles | TileOp 数 | 效率 |
|---|---|---|---|
| CUBE | 3,888 | 48 | TileOpRate 0.00004536/cyc，Occupancy 0.35%，Equivalent MAC 49,152（0.05 MAC/cyc，1.65GHz 下 0.000153 TFLOPS） |
| VECTOR | 2,077 | 172 | Utilization 0.00 |
| TLSU | 5,179 | 404 | **Utilization 75.21%**，IssueRate 0.21 events/cyc |
| 合计 wall | 795,541（Run Tileop Wall Cycles） | 624 | TLSU Busy 795,515 resource-cycles |

**判读**：
- 本驱动是"纯标量黄金参考主导"型负载（golden 侧 double 逐元素计算 ~2M 标量指令，IPC 1.9、Backend Bound 76%），tile 加速器路径占比极小（Cube 仅 48 个 TileOp / 0.35% occupancy），TLSU 因 kernel 内 cell 搬运成为唯一高占用率单元（75.21%）。
- 程序末端：`linx_test_finisher write addr=0x10009000 val=0x0001 fail`，退出码 70。即 **gfsim 时序模型下 kernel 输出与 golden 比对未通过**（driver 判据 `tokOk && maxAbsErr<1e-2 && maxRelErr<1e-2` 不成立），而同一 ELF 在 gfrun 功能模型下 R2=0 PASS。全程无 scb_waw/nuke 违例诊断（stq_conservation OK、scb_waw=0、nuke=0），执行干净，属 **时序模型与功能模型的数值分歧**，建议模型侧排查（候选方向：tile load/store 数据通路在时序模型中的数值行为、FP8/E8M0 解码路径）。

### 3.4 默认配置下 `_mt` ELF 一致 livelock（配置问题，非算子问题）

9 个 `_mt`/`_mt_dyn` ELF（mega_moe 2 + 其余 4 类 7）默认配置全部打印
`[gfsim] ELF auto-detect: single-PE; using default config` → 仅 tid0 运行 → SPMD 软件自旋
barrier 永远等不齐 4 个参与者 → 超时（exit 124）。
机理：**4-PE 自动检测按 B.IOS binder 扫描（见 `configs/fourpe.conf` 头注释），软件 barrier 型 SPMD 不触发**。

### 3.5 `--conf mt`（SMT4）：8/9 早期停顿，非可用配置

| ELF | 现象 |
|---|---|
| mega_moe_sim_mt / mega_moe_sim_mt_dyn | 时序模型停顿 → ResVerify watchdog 死锁断言 @ cycle 317,561 / 72,420 |
| moe_combine_mt / moe_combine_mt_dyn | 同上 @ cycle 64,811（thread 3，verified blocks=4,144）/ 61,018（verified blocks=4,138） |
| moe_dispatch_mt / moe_dispatch_mt_dyn | 同上 @ cycle 24,509（thread 0，verified blocks=1,061）/ 23,767（thread 3，verified blocks=1,033） |
| group_token_vec_mt / group_token_vec_mt_dyn | 同上 @ cycle 125,673（verified blocks=518）/ 351,813（verified blocks=24,373） |
| group_token_old_mt | **唯一推进至末端者**：workload 执行完毕后在 exit 处触发 lockstep 断言 @ cycle 244,623 |

结论：**`--conf mt`（共享 CUBE/VEC 的 SMT4）会早期停顿，4-PE 时序仿真应使用 `--conf fourpe`**（5 个 ELF 的 9 次 mt 配置运行中仅 group_token_old_mt 推进到末端）。

另注：`group_token_vec_mt_dyn` 在 `--conf fourpe` 下为 14.9M-inst 级大负载，420s 超时不足以跑完（中断时已推进至
cycle ~374K 且仍在前进）；放宽至 2400s 后完整跑完 1,275,337 cycles 并干净退出。**该 ELF 的时序仿真建议超时 ≥30min**。

### 3.6 `--conf fourpe`：4/9 完成，5/9 被末端 exit lockstep 断言截断

完成的 4 个：moe_combine_mt、moe_combine_mt_dyn、moe_dispatch_mt、group_token_vec_mt_dyn（均 exit 0 + 完整 stats）。

被截断的 5 个（workload 已执行、PE0 验证阶段被 watchdog 断言，无 stats / 无 finisher）：

```
mega_moe_sim_mt        : bpc=0x11458 cycle= 323,593  ready=3/4 joined=3/4  t0 未达 syscall block
mega_moe_sim_mt_dyn    : bpc=0x11370 cycle=3,638,308  （全量 workload ≥3.6M 周期已执行完，末端截断）
group_token_old_mt     : bpc=0x112b0 cycle= 242,720  ready=3/4 joined=3/4
group_token_vec_mt     : bpc=0x112b0 cycle= 457,690  （崩溃前 peId=0 有 230 次 scb_waw_violation）
moe_dispatch_mt_dyn    : bpc=0x11390 cycle=  45,448  lastProgress=35,449（期间 8 次 nuke_pair_diag）
```

**mega_moe_sim_mt 崩溃详情**：
```
命令: bin/gfsim --conf fourpe --test-finisher true -f solution_mega_moe_mega_moe_sim_mt_BS16_H32_HD64.elf
进度: 4 tid 同步推进，cycle 300k 时各 42,173 blocks（SPMD 相同指令流）
诊断: [C:307901..] scb_waw_violation peId=0 ...（多条）
      [C:313407] nuke_pair_diag ld_stid=0 ...
崩溃: [gfsim] syscall_lockstep_and_timeout: bpc=0x11458 entry=0 cycle=323593
      T_deadlock_effective=9999 ready=3/4 joined=3/4
      t0=-/-(st=0) t1/t2/t3=ready/joined
      ASSERTION FAILED: "syscall_lockstep_and_timeout: a lockstep group's AND
      never completed (a participant did not reach its syscall block)"
      (TimingSim/frontend/bctrl/SyscallBarrier.cpp:1889) → exit 134
```

**mega_moe_sim_mt_dyn 崩溃详情**：
```
命令: bin/gfsim --conf fourpe --test-finisher true -f solution_mega_moe_mega_moe_sim_mt_dyn.elf
进度: 全量 workload 执行完毕 —— 4 tid 到 cycle 3.6M 时各 2,247,867 blocks
      （t1-3 自旋于 BPC 0x128ee barrier/park；t0 执行 golden FP 验证 BPC 0x12b18-0x12c44）
诊断: cycle 3.617M-3.626M 期间 peId=0 出现 scb_waw_violation 风暴
崩溃: [gfsim] syscall_lockstep_and_timeout: bpc=0x11370 entry=0 cycle=3638308
      ready=3/4 joined=3/4 t0=-/-(st=0)  → 同上断言，exit 134
后果: finisher 写出与 SuperScalar NPU Stats 均未产出（崩溃先于 ReportStat）
```

**机理（5 例同源）**：**非 leader PE 在 kernel 末端 barrier 后直接 `return 0` 早退**，PE1-3 率先抵达 `_end`
发起 exit 系统调用并加入 lockstep 组；leader PE0 仍在执行 golden 验证，超过 `T_deadlock_effective=9999`
无进展窗口 → `SyscallBarrier` lockstep watchdog 断言（exit 134）。属 **gfsim 系统调用 lockstep 语义对
SPMD 早退模式的模型侧限制，非算子错误**；能否完整收尾取决于 PE0 验证尾长与诊断事件（本批 4 个完成运行
与 5 个截断运行的差异仅在于 PE0 末端路径长短/停顿）。mega_moe_sim_mt_dyn 的崩溃前 scb_waw_violation
风暴提示 PE0 末端 store 提交顺序异常，与 t0 未达 syscall block 直接相关。

### 3.7 诊断事件观察（均不影响 gfrun 侧正确性，建议模型侧复核）

| 事件 | 出现位置 | 量化 |
|---|---|---|
| `scb_waw_violation`（peId=0，scalar=0，tile 域） | group_token_vec 家族、mega_moe `_mt` 系 | group_token_vec 单 PE 41 次；mt 版 230 次（崩溃前）；mt_dyn fourpe 254 次（invariants `scb_waw_tile=254`，收尾干净）；mega_moe mt/mt_dyn fourpe 末端风暴 |
| `nuke_pair_diag`（ld/st 对 nuke 恢复） | group_token_old、moe_dispatch、mega_moe `_mt` | group_token_old 单 PE 254 次（invariants `nuke=254`）、mt 版 1~2 次；moe_dispatch_mt 1 次、mt_dyn 8 次 |
| finisher 数值自检 FAIL | mega_moe_sim（单 PE） | val=0x0001，gfsim 与 gfrun 数值分歧（§3.3），其余 8 个完成运行无此通道不可判读 |

`scb_waw_violation` **亦出现在单 PE 纯净运行**（group_token_vec 单 PE 41 次、exit 0），说明并非 4-PE
交互特有，建议模型侧优先排查 store-commit WAW 判定与 tile 通路。

## 4. 结论与建议

| 维度 | 状态 |
|---|---|
| gfrun 功能回归 | **14/14 PASS（R2=0）**：单 PE × 5、4-PE SPMD × 5、4-PE 动态 shape × 4 |
| gfsim 时序仿真 | **8 PASS / 24 FAIL**（32 次运行，判据见 §3.1）：9/14 ELF 产出完整周期数据（8 PASS：GT-old 1,117,803 cyc / GT-vec 478,066 / GT-vec_mt_dyn 1,275,337 / moe 系 5.2K~89K；1 FAIL：mega_moe_sim 1,058,314 cyc 数值自检 FAIL）；其余 24 次失败中 9 次为默认配置 livelock（配置错误）、14 次为模型侧阻断（ResVerify 死锁 8 / exit-lockstep 断言 6，涉及 **5 个 `_mt` ELF**：mega_moe_sim_mt、mega_moe_sim_mt_dyn、group_token_old_mt、group_token_vec_mt、moe_dispatch_mt_dyn，workload 已执行，无 stats） |
| 数值判读（gfsim 侧） | mega_moe_sim：gfsim FAIL vs gfrun PASS（模型间数值分歧）；其余 4 类无 finisher，gfsim 侧不可判定，以 gfrun R2=0 为准 |

建议：
1. **模型侧（SuperScalarModel）**：
   - **syscall lockstep watchdog 对 SPMD 早退模式不适配**（§3.6，共 5 例 fourpe 截断 + 1 例 mtconf）：非 leader PE 先达 `_end` 发起 exit、leader 长时间运行 golden 的合法形态触发 `SyscallBarrier.cpp:1889` 断言；`--conf mt` 路径另有早期停顿（ResVerify watchdog，8/9）。修复后 5 个被阻断 ELF 方可产出周期数。
   - **peId=0 `scb_waw_violation`**（含单 PE 纯净运行样本，group_token_vec 41 次 / mt_dyn 254 次 / mega_moe 末端风暴）：排查 store-commit WAW 判定。
   - **单 PE 数值分歧**（mega_moe_sim）：同 ELF gfrun R2=0 vs gfsim finisher val=0x0001，建议用 `--dump-memory` 抓 kernel 输出比对定位（TLSU tile 通路、FP8/E8M0 解码路径优先）。
   - `nuke_pair_diag` 计数（group_token_old 单 PE 254 次等）建议复核 ld/st nuke 恢复路径。
2. **使用侧（短期）**：`_mt` ELF 时序仿真用 `--conf fourpe` + 充分超时（`group_token_vec_mt_dyn` 需 ≥30min）；被阻断的 5 个 ELF 性能评估暂以 gfrun 指令数与同家族完成运行外推（如 mega_moe_mt_dyn 已知 ≥3.6M 周期、GT-vec_mt_dyn 完整 1.28M 周期）。
3. **测试侧**：建议为 4 类无 finisher 驱动补 `0x10009000` finisher 写出（参照 mega_moe/quant_sparse_flash_mla），使 gfsim 侧数值正确性可自动判读，避免时序/功能模型分歧不可见。

## 5. 日志索引（`sim_logs/`，按算子分目录）

| 目录 | 文件 | 内容 |
|---|---|---|
| mega_moe/ | `gfrun_sim.log` / `gfrun_sim_mt.log` / `gfrun_sim_mt_dyn.log` | gfrun 三次运行完整输出（均 PASS） |
| | `gfsim_sim.log` | 单 PE 完整运行（stats + finisher val=0x0001 fail） |
| | `gfsim_sim_mt.log` / `gfsim_sim_mt_dyn.log` | 默认配置 livelock 超时（barrier 自旋） |
| | `gfsim_mt_mtconf.log` / `gfsim_mt_dyn_mtconf.log` | `--conf mt` ResVerify 死锁崩溃 |
| | `gfsim_mt_fourpe2.log` / `gfsim_mt_dyn_fourpe2.log` | `--conf fourpe` syscall lockstep 崩溃（mt_dyn 含 3.6M 周期完整执行进度） |
| group_token_old/ | `gfrun_group_token_old.log` / `gfrun_group_token_old_mt.log` | gfrun（均 PASS） |
| | `gfsim_group_token_old.log` | 单 PE 完整运行（1,117,803 cycles） |
| | `gfsim_group_token_old_mt_default.log` | 默认配置 livelock 超时 |
| | `gfsim_group_token_old_mt_mtconf.log` / `gfsim_group_token_old_mt_fourpe.log` | exit lockstep 断言（@244,623 / @242,720） |
| group_token_vec/ | `gfrun_group_token_vec*.log` × 3 | gfrun（均 PASS） |
| | `gfsim_group_token_vec.log` | 单 PE 完整运行（478,066 cycles） |
| | `gfsim_group_token_vec_mt_default.log` / `gfsim_group_token_vec_mt_dyn_default.log` | 默认配置 livelock 超时 |
| | `gfsim_group_token_vec_mt_mtconf.log` / `gfsim_group_token_vec_mt_dyn_mtconf.log` | `--conf mt` ResVerify 死锁 |
| | `gfsim_group_token_vec_mt_fourpe.log` | exit lockstep 断言（@457,690，前置 scb_waw 风暴） |
| | `gfsim_group_token_vec_mt_dyn_fourpe.log` | `--conf fourpe` **完整运行**（1,275,337 cycles，2400s 超时下完成） |
| moe_combine/ | `gfrun_moe_combine_*.log` × 3 | gfrun（均 PASS） |
| | `gfsim_moe_combine_v2.log` | 单 PE 完整运行（5,188 cycles） |
| | `gfsim_moe_combine_mt_fourpe.log` / `gfsim_moe_combine_mt_dyn_fourpe.log` | **完整运行**（57,341 / 88,727 cycles） |
| | `gfsim_moe_combine_mt_default.log` / `gfsim_moe_combine_mt_dyn_default.log` | livelock 超时 |
| | `gfsim_moe_combine_mt_mtconf.log` / `gfsim_moe_combine_mt_dyn_mtconf.log` | ResVerify 死锁 |
| moe_dispatch/ | `gfrun_moe_dispatch_*.log` × 3 | gfrun（均 PASS） |
| | `gfsim_moe_dispatch_v2.log` | 单 PE 完整运行（9,749 cycles） |
| | `gfsim_moe_dispatch_mt_fourpe.log` | **完整运行**（18,131 cycles） |
| | `gfsim_moe_dispatch_mt_dyn_fourpe.log` | exit lockstep 断言（@45,448） |
| | `gfsim_moe_dispatch_mt_default.log` / `gfsim_moe_dispatch_mt_dyn_default.log` | livelock 超时 |
| | `gfsim_moe_dispatch_mt_mtconf.log` / `gfsim_moe_dispatch_mt_dyn_mtconf.log` | ResVerify 死锁 |
