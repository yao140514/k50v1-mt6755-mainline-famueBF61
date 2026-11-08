# SMP 调研记录

目标：让 8 个 Cortex-A53 都跑起来。当前只能 `maxcpus=1`。

## 已确认的事实

```
[0.000000] psci: probing for conduit method from DT.
[0.000000] psci: PSCIv0.2 detected in firmware.
[0.000000] psci: Using standard PSCI v0.2 function IDs
[0.000000] psci: Trusted OS migration not required
```

- ATF 实现了**真正的 PSCI v0.2**（不是空壳）
- `echo 1 > /sys/devices/system/cpu/cpu1/online` 返回 `EIO`
- `dmesg`：`CPU1: failed to come online` / `CPU1: failed in unknown state : 0x0`
- `CPU_ON` 返回码为 0（SUCCESS），说明 ATF **收下了请求但核没有运行**

## 关键发现：CPU1 的电源域是关的

厂商 `mt-smp.c` 在 `mt_smp_prepare_cpus()` 里调用 `spm_mtcmos_cpu_init()`，
由 OS 自己负责打开 CPU 的 MTCMOS 电源域——ATF 不做这件事。

实测 SPM 状态寄存器（只读）：

```
0x10006180 (SPM_PWR_STATUS) = 0x2A40E34D
```

按位拆开：

| 位 | 名称 | 值 |
|---|---|---|
| 8 | `MP0_CPUTOP_PWR_STA` | 1（cluster 供电） |
| 9 | `MP0_CPU0_PWR_STA` | 1（CPU0 供电） |
| 10 | `MP0_CPU1_PWR_STA` | **0（CPU1 断电）** |
| 14 | `MCU_PWR_STA` | 1 |

这与 `CPU_ON` 返回成功但核不启动完全吻合。

## SPM 寄存器表（MT6755）

来自 vendor `mt_spm_reg_mt6755.h`：

```
SPM_BASE            = 0x10006000
POWERON_CONFIG_EN   = +0x000    写 (0xb16 << 16) | 1 解锁
PWR_STATUS          = +0x180
PWR_STATUS_2ND      = +0x184
MP0_CPUTOP_PWR_CON  = +0x204
MP0_CPU1_PWR_CON    = +0x20C
MP0_CPU1_L1_PDN     = +0x24C
```

`PWR_CON` 位定义（**注意与老平台不同，PWR_ON 在 bit2**）：

```
bit0 PWR_RST_B
bit1 PWR_ISO
bit2 PWR_ON
bit3 PWR_ON_2ND
bit4 PWR_CLK_DIS
bit5 SRAM_CKISO
bit6 SRAM_ISOINT_B
```

## 厂商的 CPU1 上电序列

`spm_mtcmos_ctrl_cpu1(STA_POWER_ON)` 的核心步骤：

```c
SPM_POWERON_CONFIG_EN = (0xb16 << 16) | 1;

/* cluster 没上电就先上 */
if (!(PWR_STATUS & MP0_CPUTOP_PWR_STA))
        spm_mtcmos_ctrl_cpusys0(STA_POWER_ON);

MP0_CPU1_PWR_CON |= PWR_ON | PWR_ON_2ND;
while (!(PWR_STATUS & MP0_CPU1_PWR_STA)) ;

MP0_CPU1_PWR_CON &= ~PWR_ISO;
MP0_CPU1_L1_PDN  &= ~0x1;                    /* SRAM 退出掉电 */
while (MP0_CPU1_L1_PDN & MP0_CPU1_SRAM_PDN_ACK) ;
udelay(1);
MP0_CPU1_PWR_CON |= SRAM_ISOINT_B;
MP0_CPU1_PWR_CON &= ~SRAM_CKISO;
```

## 未验证的尝试

1. **`INFRACFG_AO + 0x800` 写 boot address**（厂商 `smp_set_boot_addr()` 的行为）
   ```
   devmem 0x10001800 32 0x40e034a8     # secondary_entry 的物理地址
   devmem 0x10001800                   # 读回 0x00000000 —— 写不进去
   ```
   该寄存器被安全策略挡住或偏移不对，此路不通。

2. **SPM MTCMOS 上电**（尚未执行）
   ```
   devmem 0x10006000 32 0xB1600001
   v=$(devmem 0x1000620C); devmem 0x1000620C 32 $((v | 0xC))
   sleep 1; devmem 0x10006180          # 看 bit10
   v=$(devmem 0x1000620C); devmem 0x1000620C 32 $((v & ~0x2))
   devmem 0x1000624C 32 0x0
   sleep 1
   v=$(devmem 0x1000620C); devmem 0x1000620C 32 $((v | 0x40))
   v=$(devmem 0x1000620C); devmem 0x1000620C 32 $((v & ~0x20))
   echo 1 > /sys/devices/system/cpu/cpu1/online
   ```
   有风险：SPM 是整机电源核心，写错可能掉电。

3. **缓存一致性**：即使核起来了，厂商在 PSCI 路径下还会做
   ```c
   mcusys_smc_write_phy(MP0_AXI_CONFIG, readl(MP0_AXI_CONFIG) & ~ACINACTM);
   REG_WRITE(CCI400_SI4_SNOOP_CONTROL, ... | SNOOP_REQ | DVM_MSG_REQ);
   while (readl(CCI400_STATUS) & CHANGE_PENDING) ;
   ```
   `MP0_AXI_CONFIG` 需要 SMC 进 ATF 才能写（非安全态写不进），
   CCI-400 的基址也还没确认。这一步不做，多核会各跑各的 cache。

## 可能的后续路线

| 路线 | 前提 | 代价 |
|---|---|---|
| SPM 上电 + PSCI | ATF 的 CPU_ON 只差电源 | 小，值得先试 |
| 内核里补 mrc | 需要 `mcusys_smc_write` 的 SMC ID | 中，要去 vendor 的 `mt_secure_api.h` 反推 |
| LK 做 spin-table | LK 能在 ATF 之前放出从核 | 大，且要反复刷 LK |

目前建议按 1 → 2 → 3 的顺序试。
