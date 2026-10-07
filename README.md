# solution 算子 gfrun / gfsim 仿真结果报告（全 5 族 14 ELF）

- **日期**: 2026-10-06
- **对象**: `benchmark/one-level-arch/output/solution/` 全部 14 个 ELF（5 个算子族：
  group_token_old ×2 / group_token_vec ×3 / mega_moe ×3 / moe_combine ×3 /
  moe_dispatch ×3）。其中 group_token_old_mt、group_token_vec_mt/mt_dyn、mega_moe 三件
  与 **moe_dispatch_mt_dyn** 均为 2026-10-01/10-04/10-06 修复后的构建（moe_dispatch_mt_dyn
  为 2026-10-06 验证汇合屏障修复版，见 `kernels/solution/moe_dispatch/
  moe_dispatch_mt_dyn_gfsim_fix_report.md`）。
- **仿真工具**: `/mnt/workspace/projects/0931_SuperScalarModel/SuperScalarModel/bin/`
  （gfrun / gfsim，模型仓库 HEAD `9905a05f`，2026-09-30 10:00 构建；均在模型仓库根目录
  运行，gfsim 以便解析 `configs/fourpe.conf`）
- **gfsim 4-PE 参数（本轮指定）**: `--conf fourpe --pto-v02 true -s tlsu.fake_l2_enable=false`
  —— `--pto-v02`（PTO ISA v0.2 tile 解码）与 `tlsu.fake_l2_enable=false`（真实 L2 路径）
  均为模型当前默认值（SimSys.h / configs/tlsu.toml）的显式化；本轮全部周期数与本仓库
  各修复报告 / 历史基线**逐 cycle 一致**（确定性复现，见 §6）
- **总体结论**:
  - **gfrun（功能模型）: 14/14 全部 PASS（R2 = 0）**
  - **gfsim（时序模型）: 14/14 全部 PASS** —— mega_moe 三件 `linx_test_finisher
    val=0x5555 pass`（数值判定）；其余 11 件完成性判定（完整跑完 + 完整 stats +
    `sb_lockstep_and_timeout=0` + 无断言）。**moe_dispatch_mt_dyn 修复后首次全绿**：
  此前五族 13 ELF + 1 FAIL 的最后一例已消除（36,798 cycle，exit_parks=4）

---

## 1. 运行矩阵与判定标准

| 运行 | 命令模板 | 适用 |
|---|---|---|
| gfrun 单 PE | `gfrun -f <elf>` | v2 / sim 类单线程 ELF |
| gfrun 4-PE SPMD | `gfrun -t 1 -s softcore.multiThreadNum=4 -f <elf>` | `_mt` / `_mt_dyn` ELF |
| gfsim 单 PE | `gfsim --test-finisher true -f <elf>`（默认配置，ELF 自动检测 single-PE） | 单线程 ELF |
| gfsim 4-PE SPMD | `gfsim --conf fourpe --pto-v02 true -s tlsu.fake_l2_enable=false --test-finisher true -f <elf>` | `_mt` / `_mt_dyn` ELF |

判定标准：
- gfrun：`R2 = 0` = PASS（非 0 为分层诊断码）
- gfsim：
  - 写 test-finisher 的 ELF（mega_moe 三件）：`val=0x5555 pass` = PASS（数值自检 =
    tokOk && maxAbsErr < 1e-2 && maxRelErr < 1e-2）
  - 其余 11 件：**完成性判定**（rc=0 + 完整 stats + 无 Deadlock/lockstep 断言 +
    `sb_lockstep_and_timeout=0`）；数据正确性锚点为同 ELF 的 gfrun R2=0（gfsim 数据面
    差异见 §7 已知边界）

## 2. 结果汇总表（14 ELF）

| # | ELF | 模式 | gfrun | gfsim 判定 | gfsim Cycles | gfsim 墙钟 |
|---|---|---|---|---|---|---|
| 1 | group_token_old | 单 PE | **PASS（R2=0）** | **PASS**（完成+stats） | 1,117,803 | 356s |
| 2 | group_token_old_mt | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | 651,493 | 378s |
| 3 | group_token_vec | 单 PE | **PASS（R2=0）** | **PASS**（完成+stats） | 478,066 | 86s |
| 4 | group_token_vec_mt | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | 701,118 | 387s |
| 5 | group_token_vec_mt_dyn | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | 1,282,647 | 734s |
| 6 | mega_moe_sim (BS16_H32_HD64) | 单 PE | **PASS（R2=0）** | **PASS `val=0x5555`** | 1,058,278 | 251s |
| 7 | mega_moe_sim_mt (BS16_H32_HD64) | fourpe | **PASS（R2=0）** | **PASS `val=0x5555`** | 1,259,952 | 766s |
| 8 | mega_moe_sim_mt_dyn | fourpe | **PASS（R2=0）** | **PASS `val=0x5555`** | 3,842,845 | 2423s |
| 9 | moe_combine_v2 | 单 PE | **PASS（R2=0）** | **PASS**（完成+stats） | 5,188 | 1s |
| 10 | moe_combine_mt | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | 57,341 | 29s |
| 11 | moe_combine_mt_dyn | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | 88,727 | 45s |
| 12 | moe_dispatch_v2 | 单 PE | **PASS（R2=0）** | **PASS**（完成+stats） | 9,749 | 2s |
| 13 | moe_dispatch_mt | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | 18,131 | 10s |
| 14 | **moe_dispatch_mt_dyn** | fourpe | **PASS（R2=0）** | **PASS**（完成+stats，parks=4） | **36,798** | 20s |

moe_dispatch_mt_dyn 修复前基线：确定性 exit lockstep 断言 @ cycle 45,448（`ready=3/4`，
无 stats）—— 2026-10-06 修复（cfgB 验证汇合屏障 mtBarrier(13)）后 PASS，为本轮
solution 全集首次 14/14 通过。

## 3. 分族明细

### 3.1 group_token_old（2 件）

| 项 | 单 PE | _mt (fourpe) |
|---|---|---|
| gfrun Total Inst | 3,037,996 | 1,165,960 |
| gfsim Total Cycles | 1,117,803 | 651,493（4-PE 加速 1.72×） |
| exit_parks / and_timeout | 0 / 0 | 4 / 0 |
| nuke / waw 记账 | 254 / 0（恢复无损） | 5 / 0（恢复无损） |
| 修复报告 | — | `kernels/solution/group_token_old/group_token_old_mt_gfsim_fix_report.md` |

### 3.2 group_token_vec（3 件）

| 项 | 单 PE | _mt (fourpe) | _mt_dyn (fourpe) |
|---|---|---|---|
| gfrun Total Inst | 2,878,818 | 10,620,226 | 14,910,165 |
| gfsim Total Cycles | 478,066 | 701,118 | 1,282,647 |
| exit_parks / and_timeout | 0 / 0 | 4 / 0 | 4 / 0 |
| nuke / waw 记账 | 0 / 41（恢复无损） | 1 / 127（恢复无损） | 1 / 259（恢复无损） |
| 修复报告 | — | `kernels/solution/group_token_vec/group_token_vec_mt_gfsim_fix_report.md` | — |

### 3.3 mega_moe（3 件，全部带 test-finisher 数值判定）

| 项 | sim（单 PE） | sim_mt (fourpe) | sim_mt_dyn (fourpe) |
|---|---|---|---|
| gfrun Total Inst | 7,643,628 | 3,059,006 | 7,024,599 |
| gfsim Total Cycles | 1,058,278 | 1,259,952 | 3,842,845 |
| gfsim finisher | `val=0x5555 pass` | `val=0x5555 pass` | `val=0x5555 pass` |
| nuke / waw 记账 | 0 / 0 | 0 / 52 | 1（末端汇合段，恢复无损）/ 137 |
| 数据取证（--dump-memory，前轮同构建） | — | tokOut (8,8)、y 无污染、maxAbsErr 4.49e-113 | tokOut (9,9)、y 无污染、maxAbsErr 1.04e-112 |
| 修复报告 | `mega_moe_sim_gfsim_fix_report.md` | `mega_moe_sim_mt_gfsim_fix_report.md` | `mega_moe_sim_mt_dyn_gfsim_fix_report.md`（均在 `kernels/solution/mega_moe/`） |

### 3.4 moe_combine（3 件）

| 项 | v2（单 PE） | _mt (fourpe) | _mt_dyn (fourpe) |
|---|---|---|---|
| gfrun Total Inst | 25,368 | 298,808 | 767,147 |
| gfsim Total Cycles | 5,188 | 57,341 | 88,727 |
| exit_parks / and_timeout | 0 / 0 | 4 / 0 | 4 / 0 |
| nuke / waw 记账 | 0 / 0 | 0 / 0 | 0 / 0 |

### 3.5 moe_dispatch（3 件）

| 项 | v2（单 PE） | _mt (fourpe) | _mt_dyn (fourpe) |
|---|---|---|---|
| gfrun Total Inst | 251,564 | 105,158 | 423,846 |
| gfsim Total Cycles | 9,749 | 18,131 | **36,798（修复后）** |
| exit_parks / and_timeout | 0 / 0 | 4 / 0 | **4 / 0**（修复前：断言截断无 stats） |
| nuke / waw 记账 | 0 / 0 | 1 / 0（恢复无损） | 10 / 0（恢复无损） |
| 修复报告 | — | — | `kernels/solution/moe_dispatch/moe_dispatch_mt_dyn_gfsim_fix_report.md`（2026-10-06，cfgB 验证汇合屏障） |

## 4. 运行健康度汇总（gfsim，14 用例全 PASS）

- `sb_lockstep_and_timeout`：**全部 = 0**；Deadlock/断言：**全部 = 0**
- `sb_lockstep_exit_parks`：单 PE 0；4-PE 全部 4（mega_moe_mt=1 / mt_dyn=0 为
  finisher-stop 模式：判定写后仿真停止，属正常形态）
- nuke_pair_diag / scb_waw_violation 记账：全部恢复无损（完成性/数据取证佐证；
  记账条数见 §3 分族表，同类先例 vec_mt_dyn "scb_waw_tile=254 收尾干净"）

## 5. gfsim 性能参考（wall-cycle 口径）

- 4-PE 加速（单 PE 同族 / fourpe）：group_token_old 1.72×
- Tileop Total Cycles（tile 流水异步汇总）：gt_old_mt 2.61M / gt_vec_mt 2.80M /
  gt_vec_mt_dyn 5.13M / mm_sim_mt 5.04M / mm_sim_mt_dyn 15.37M / mc_mt_dyn 355K /
  md_mt_dyn 147K
- 墙钟预算：mega_moe_sim_mt_dyn ≈ 40min（建议 ≥45min）；gt_vec_mt_dyn ≈ 12min；
  gt_old / gt_old_mt / gt_vec_mt / mm_sim_mt ≈ 4-13min；其余 ≤ 1.5min

## 6. 周期数确定性对照（本轮 vs 既有验证值）

| ELF | 本轮（显式指定 4-PE 参数） | 既有验证值 | 一致性 |
|---|---|---|---|
| group_token_old / _mt | 1,117,803 / 651,493 | 同值（gt_old_mt 修复报告 §7） | ✓ |
| group_token_vec / _mt / _mt_dyn | 478,066 / 701,118 / 1,282,647 | 同值（历史基线 / vec_mt 修复报告 §5） | ✓ |
| mega_moe sim / _mt / _mt_dyn | 1,058,278 / 1,259,952 / 3,842,845 | 同值（各自修复报告） | ✓ |
| moe_combine v2 / _mt / _mt_dyn | 5,188 / 57,341 / 88,727 | 同值（历史基线） | ✓ |
| moe_dispatch v2 / _mt | 9,749 / 18,131 | 同值（历史基线） | ✓ |
| moe_dispatch_mt_dyn | **36,798（PASS）** | 36,798（mt_dyn 修复报告 §5，4 次确定性一致） | ✓（修复前基线 FAIL @45,448 已消除） |

（`--pto-v02 true` 与 `-s tlsu.fake_l2_enable=false` 为默认值显式化，逐 cycle 可复现；
日志 `Override configurations: tlsu.fake_l2_enable=false` 确认参数生效。）

## 7. 已知边界与注意事项

1. **`--conf mt` 不可用于 tile 密集 SPMD**：4 SMT 线程共享单 VEC/TLSU 引擎下 tile 流水
   确定性停摆（ResVerify），4-PE 时序仿真一律用 `--conf fourpe`。
2. **gfsim TLSU tile-store→GM 数据通路缺口**：tile TSTORE 执行后架构内存不可见（探针
   实证，`mega_moe_sim_gfsim_fix_report.md` §5）。mega_moe 家族 tokOut 导出已因此改
   标量路径并通过数值判定；其余无 finisher ELF 的 gfsim 判定为完成性判定，数据正确性
   以 gfrun R2=0 为锚点。
3. **mega_moe y 校验为弱校验**：测试数据 E8M0 scale=0x00 使 golden ~1e-113、f32 正确值
   下溢为 0（可捕获 2.0f 类污染；模型缺口修复后建议 scale 改 0x7F 恢复强校验）。
4. **模型 nuke 恢复竞态**：nuke flush 恢复存在丢 tile 链状态 / 丢标量生产者两副面孔，
   成败由 ±10 cycle 相位决定（`mega_moe_sim_mt_gfsim_fix_report.md` §3.3-4 取证）。
   已修复算子以幂等双遍 + 预量化 + 全宽块 + 相位衬垫规避；本轮全部 14 用例的 nuke
   均恢复无损。
5. **退出 lockstep 早退缺陷族（已全部修复）**：worker 提前 return + PE0 验证迟到 →
   退出 AND 组搁浅（group_token_old_mt / vec_mt / mega_moe ×2 / moe_dispatch_mt_dyn
   五例，修复模板 = 验证汇合屏障；静态版 moe_dispatch_mt / moe_combine 家族的同款
   结构实测可过，如验证逻辑加长需按同法加固）。

## 8. 复现命令

```bash
MODEL=/mnt/workspace/projects/0931_SuperScalarModel/SuperScalarModel
BASE=/mnt/workspace/projects/0931_SuperNPUBench/SuperNPUBench/benchmark/one-level-arch/output/solution
FOURPE="--conf fourpe --pto-v02 true -s tlsu.fake_l2_enable=false --test-finisher true"
cd $MODEL
# 单 PE ELF:
./bin/gfrun -f $BASE/<fam>/elf/<sp.elf>                                # 期望 R2 = 0
./bin/gfsim --test-finisher true -f $BASE/<fam>/elf/<sp.elf>           # 完成性判定
# 4-PE SPMD ELF (_mt / _mt_dyn):
./bin/gfrun -t 1 -s softcore.multiThreadNum=4 -f $BASE/<fam>/elf/<mt.elf>
./bin/gfsim $FOURPE -f $BASE/<fam>/elf/<mt.elf>
# 数据取证 (判定数组为 .bss 静态符号, 以 llvm-nm 实测地址为准):
./bin/gfsim $FOURPE --dump-memory <符号地址>:<字节数>:<输出文件> --dump-force -f <elf>
```

本轮全部原始日志归档于 `output/solution/sim_logs/`（14 ELF × `__gfrun.log` /
`__gfsim.log` + `summary_raw.txt` 运行清单；gfrun 全量指令 trace 已裁剪为摘要尾部）。
