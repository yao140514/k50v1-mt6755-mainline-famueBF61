# SMP on MT6755 (mainline)

**Status: done. 8 cores at boot.** `smp: Brought up 1 node, 8 CPUs`.

## The finding

ATF's PSCI `CPU_ON` on this board does nothing to the power controller:

- `psci_ops.cpu_on()` returns `SUCCESS`
- `MP0_CPUx_PWR_CON` reads back **unchanged** (`PWR_RST_B` still 0)

The core never leaves reset, never writes `secondary_data.status`, and Linux
reports:

```
CPU1: failed to come online
CPU1: failed in unknown state : 0x0
```

`failed in unknown state : 0x0` is specifically a *successful* PSCI call that
produced no running core -- an ATF error would print
`failed to boot CPU1 (-5)` instead.

## Where the fix lives

`arch/arm64/kernel/psci.c`, in `cpu_psci_cpu_boot()`, right after the PSCI
call returns:

```c
if (!err)
        mt6755_secondary_boot(cpu);
```

This is the only place where the timing works. `__cpu_up()` then waits
`wait_for_completion_timeout(&cpu_running, 5000ms)` and afterwards sets
`secondary_data.task = NULL`. A secondary starting outside that window finds
`task == NULL` in `head.S`:

```
ldr x2, [x0, #CPU_BOOT_TASK]
cbz x2, __secondary_too_slow
```

and parks in a `wfe/wfi` loop -- board alive, CPU never appears. Userspace
always races that window; the kernel hook cannot.

## The sequence (vendor `spm_mtcmos_ctrl_cpuN(STA_POWER_ON)`)

SPM base `0x10006000`. Unlock first: `POWERON_CONFIG_EN(0x000) = 0xB1600001`.

| step | register | value |
|---|---|---|
| 1 | `PWR_CON` | set `PWR_ON`(bit2) + `PWR_ON_2ND`(bit3) |
| 2 | `PWR_STATUS`(0x180) / `_2ND`(0x184) | wait both bits set |
| 3 | `PWR_CON` | clear `PWR_ISO`(bit1) |
| 4 | `L1_PDN` | clear `SRAM_PDN`(bit0), wait `ACK`(bit8) clear |
| 5 | -- | `udelay(1)` |
| 6 | `PWR_CON` | set `SRAM_ISOINT_B`(bit6) |
| 7 | `PWR_CON` | clear `SRAM_CKISO`(bit5) |
| 8 | `PWR_CON` | clear `PWR_CLK_DIS`(bit4) |
| 9 | `PWR_CON` | set `PWR_RST_B`(bit0) -- the one ATF skips |

Offsets:

| cpu | `PWR_CON` | `L1_PDN` | `PWR_STATUS` bit |
|---|---|---|---|
| 0..3 | 0x208 + n*4 | 0x248 + n*4 | 9 + n |
| 4..7 | 0x21C + (n-4)*4 | 0x260 + (n-4)*4 | 14 + (n-4) |

Cluster: `MP0_CPUTOP` 0x204 / L2 0x240 (status bit 8),
`MP1_CPUTOP` 0x218 / L2 0x258 (status bit 13).

## Cores powered but not online

At cold boot `PWR_STATUS` already reports `MP1_CPU0` (bit14) and `MP1_CPU1`
(bit15) as on, yet neither is online -- ATF left them powered with reset
released, parked outside Linux. Handing those to PSCI does nothing, so they
are cycled first: set `PWR_ISO`+`SRAM_CKISO`, clear `SRAM_ISOINT_B`, set
`SRAM_PDN`, clear `PWR_RST_B`, set `PWR_CLK_DIS`, clear `PWR_ON`/`PWR_ON_2ND`,
`mdelay(1)`.

## cmdline

`maxcpus=8`. The old `maxcpus=1` was a workaround for this bug and is gone.

## The userspace script

`scripts/smp_on.sh` is kept as a rescue tool (`dump`, `peek <addr>`, `all`).
It does the same sequence through `/dev/mem` and reproduces the timing window
with `echo 1 > online &` + `sleep 1` + release reset. On a normal boot it is
a no-op.
