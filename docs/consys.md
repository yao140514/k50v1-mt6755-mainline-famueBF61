# CONNSYS on MT6755 — power-on, chip ID, and where it stops

Status: **AP-side bring-up done, MCU not executing.** This documents exactly
how far we got and what is provably true, so it does not have to be
rediscovered.

All of this was done from userspace with `devmem` on the running mainline
kernel — no driver, no reflash.

## 1. What works

Powering the CONN domain and reading the chip ID back:

```sh
devmem 0x1000632C 32 0x112     # start from the OFF value (ISO|CLK_DIS|SRAM_PDN)
devmem 0x10006000 32 0x0B160001 # SPM write unlock (PWRON_CONFG_EN)
devmem 0x1000632C 32 0x11E     # PWR_ON | PWR_ON_2ND
sleep 1
devmem 0x10006180              # PWR_STATUS bit1 (CONN) becomes 1
devmem 0x1000632C 32 0x11C     # clear PWR_ISO
devmem 0x1000632C 32 0x01C     # clear SRAM_PDN, wait for ACK bit12 to drop
devmem 0x1000632C 32 0x00C     # clear PWR_CLK_DIS
devmem 0x1000632C 32 0x00D     # set PWR_RST_B   <-- last, block only answers after this
devmem 0x18070008              # -> 0x00000326
```

Verified transitions:

| register | before | after |
|---|---|---|
| `PWR_STATUS` (0x10006180) | `0x2A4FFF4D` | `0x2A4FFF4F` (bit1 = CONN) |
| `CONN_PWR_CON` (0x1000632C) | `0x00000112` | `0x0000000D` |
| `CONSYS_CHIP_ID` (0x18070008) | `0x00000000` | **`0x00000326`** |
| `CONN_HIF` (0x180F0000) | — | `0x00100326` (same chip ID, HIF block alive) |

Two things that are easy to get wrong:

- **The block reads all-zero until `PWR_RST_B` (bit0) is set.** Reading the
  chip ID one step early returns 0 and looks like a dead chip.
- **CONN's SRAM bits are not the CPU core's.** The core uses
  `SRAM_CKISO(5)` / `SRAM_ISOINT_B(6)`; CONN uses `SRAM_PDN(8)` and
  `SRAM_PDN_ACK(12)`. Copying the CPU sequence writes into non-existent fields.

### Register map (from `connectivity-common`, `platform/include/mt6755.h`)

| name | address |
|---|---|
| `CONSYS_CHIP_ID` | `0x18070008` |
| `CONSYS_MCU_CFG_ACR` | `0x18070110` |
| `CONSYS_ROM_RAM_DELSEL` | `0x18070114` |
| `CONSYS_CPUPCR` (MCU PC) | `0x18070160` |
| `CONN_HIF` | `0x180F0000` |
| `CONN_PWR_CON` (SPM) | `0x1000632C` |
| `PWR_STATUS` / `_2ND` | `0x10006180` / `0x10006184` |
| `PWRON_CONFG_EN` | `0x10006000` (write `0x0B160001`) |
| `CONSYS_CPU_SW_RST` (AP_RGU) | `0x10007018`, bit12 set = asserted, key `0x88<<24` |
| `CONSYS_TOPAXI_PROT_EN` / `STA1` | `0x10001220` / `0x10001228` |

## 2. What does not work

The MCU is **not executing**. `CONSYS_CPUPCR` (`0x18070160`) reads
`0x00000C48` and stays there — sampled seconds apart, always identical.

The vendor sequence was replayed exactly, including the parts we had skipped:

```
assert CONNSYS_CPU_SW_RST (0x10007018 |= (1<<12) | (0x88<<24))
SPM unlock + power on (above)          -> chip ID 0x0326 OK
ACR |= (1<<18)  (MBIST at full speed)  -> 0x00340002
deassert CONNSYS_CPU_SW_RST            -> 0x10007018 = 0x00008000
```

Still `0xC48`. Also tried, with no effect:

- `TOPCKGEN+0x84` write `1<<26` — `CONSYS_TOP_CLKCG_BIT`, the only clock gate
  the vendor header names. Register reads 0 before and after (write-only
  clear), PC unchanged.
- `TOPCKGEN+0x1F00` — `CONSYS_AP2CONN_OSC_EN`. Reads `0x01C03224`, bit10
  clear. **Not written**: it looks like a mux/divider register and we have no
  documentation for the other bits.
- `TOPAXI_PROT_STA1` reads `0x8A9F9DA0`, but `PROT_EN` is 0 and the chip ID
  reads fine, so protection is not blocking the AP view.

`0xC48` is most likely a parked loop in the boot ROM (WFI), i.e. the MCU is
alive and waiting for the host to start the download. That cannot be
distinguished from a stalled core without speaking the download protocol.

## 3. Why the download needs the driver, not register pokes

`WIFI_RAM_CODE_6755.bin` is **not** a flat image. Its first word is
`0x574B544D` (`MTKW`), which is `MTK_WIFI_SIGNATURE` — the "divided download"
container parsed by `common/wlan_lib.c`:

```
struct FIRMWARE_DIVIDED_DOWNLOAD_T {
    u32 u4Signature;      // 0x574B544D
    u32 u4CRC;
    u32 u4NumOfEntries;   // this file: 4
    ...
    struct { u32 u4DestAddr, u4Length, u4Offset; } arSection[];
};
```

Each section is pushed into CONNSYS memory through the **AHB HIF / PDMA
ring**, which lives in `connectivity-wlan-core-gen2` (`os/linux/hif/ahb/`,
`mt6755/ahb_pdma.c`). There is no register pair that copies a block — it is a
descriptor ring, and it is only reachable once the MCU has been told to expect
a download.

Notably, neither WMT nor the WLAN driver contains a plain `request_firmware()`
for this file in the SOC path — the image is handed down through the divided
download path above.

## 4. What is in hand

| item | where |
|---|---|
| WMT core + `mt6755.c` platform ops | `connectivity-common`, branch `Pie-9-release-POBS` |
| AHB HIF / PDMA | `connectivity-wlan-core-gen2`, same branch |
| `/dev/wmtWifi` cdev | `connectivity-wlan-adaptor`, same branch |
| `WIFI_RAM_CODE_6755.bin` | `F:\BF61\dl\fw_WIFI_RAM_CODE_6755` (414800 B) |
| `WMT_SOC.cfg` | `F:\BF61\dl\fw_WMT_SOC.cfg` (119 B) |

Porting cost measured, not guessed: 54 `.c` files, 46 private headers, of
which only ~12 are outside these three repos (`mt_clkmgr.h`, `mt_reg_base.h`,
`mtk_btif_exp.h`, `emi_mpu.h`, `conn_md_exp.h`, ...). The tree is 4.9-era and
targets 6.12.

## 5. Honest assessment

Reaching `chip ID` proves the AP-side path, the SPM domain, and the HIF block
are all reachable from mainline. That is the whole of what register-level work
can give us.

Getting further means porting the driver — a weeks-scale job, and the one
detail that decides feasibility is whether the AHB PDMA layer can be made to
run. A cheap way to test that in isolation would be to port only
`os/linux/hif/ahb/` plus enough of `wlan_lib.c` to issue one download, which is
still a large slice.
