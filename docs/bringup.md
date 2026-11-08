# Bring-up 记录：踩过的坑

按时间顺序，只记录**现象 → 原因 → 解法**，同类问题合并。

## 1. 串口抓不到日志

| 现象 | 原因 | 解法 |
|---|---|---|
| CH340 一个字节都没有 | 波特率是 **921600**，不是 115200 | 改 921600 8N1 |
| 只有开机头 1.5 秒有输出 | 厂商 cmdline 带 `printk.disable_uart=1` | 只抓 preloader/LK 阶段，或自己传 cmdline |

## 2. boot.img 格式

原厂 boot.img 不是标准 AOSP 头，MTK 在 1644 字节处插了私有扩展：

```
1644  u32  扩展头大小 = 1660
1648  u32  DTBO 段大小
1652  u32  DTB 加载地址 = 0x44000000
```

| 现象 | 原因 | 解法 |
|---|---|---|
| `incomplete bootimage` | SHA1 是 20 字节，赋给了 32 字节切片 | 写 `hdr[576:596]` |
| LK panic `bootargs.c:96` | DTB 缺 `bootargs` 属性 | 补 `bootargs` |
| `FDT_ERR_BADSTRUCTURE` | DTB 缺 `__symbols__`，overlay fixup 破坏结构 | `dtc -@` |
| `Couldn't find 'chosen' symbol` | `chosen` 没有 label | `chosen: chosen { ... }` |
| `Not enough space for adding property` | DTB 只有 4 KB | `dtc -p 0x10000` |
| LK `ASSERT at mt_boot.c:447` | 头的 `tags_addr` ≠ LK 实际分配的 DTB 地址 | 必须写 `0x44000000` |

## 3. LK

从 `svoboda18/lk` 编译（含 `project/k50v1_64_bsp.mk`），改了 19 个文件，见
`lk-changes.patch`。主要几个：

| 现象 | 原因 | 解法 |
|---|---|---|
| DCT 脚本报语法错 | 脚本是 Python2 | 移植 print/except/路径 |
| 编译缺 logo 资源 | 没有对应分辨率的开机图 | `BOOT_LOGO := hd720` |
| 显示初始化崩溃（空指针） | 面板驱动与实际屏不符（配的是 FHD，实际 480×800） | `MACH_FPGA_NO_DISPLAY=y` + 关掉 `primary_display_trigger` |
| 13 秒后自动重启 | MTK WDT 没有内核驱动 | LK 里 `mtk_wdt_disable()` |
| 进了 recovery 菜单 | boot mode 判断误判 | 禁用 recovery 分支与菜单入口 |

## 4. 内核启动

| 现象 | 原因 | 解法 |
|---|---|---|
| `Kernel is 32Bit` | cmdline 缺位数声明 | `bootopt=64S3,32N2,64N2` |
| `decompress kernel image fail` | 64 位分支强制 gunzip | 用 `Image.gz` |
| `ramdisk_real_sz != 0` 断言 | LK 不接受空 ramdisk | 带一个非空 initramfs |
| `data abort` | DTB 缺 `reserved-memory` | 补节点 |

## 5. eMMC / 文件系统

| 现象 | 原因 | 解法 |
|---|---|---|
| `mmc: error -ENODEV: Cannot find pinctrl` | `mtk-sd` 硬性依赖 pinctrl | 打补丁改成可选 |
| `no support for card's volts` | 缺 vmmc | DTS 加 3.3 V `regulator-fixed` |
| deferred probe pending | `regulator-fixed` 拒绝可变电压 | vqmmc 写 `3300000-3300000` |

## 6. USB（mtu3 / SSUSB）

这是耗时最久的一块，最终结论很反直觉。

| 现象 | 原因 | 解法 |
|---|---|---|
| probe 超时 `clks of sts1 are not stable` | `IP_DEV_CAP` 读到 `0xFFFFFFFF`，被误判成 U3 capable，多等一个永远不来的 `U3_MAC_RST_B_STS` | DTS 加 `mediatek,u3p-dis-msk = <0x1>` |
| 修好上面后 `IP version 0x0(U2 IP)` 正常，但 D+ 上拉始终不上线 | mainline `phy-mtk-tphy` 在 U2 端口 **清 SESSEND**，厂商是**置位**；此外还少了 ISO_EN/BC1.1/OTG_VBUSCMP/SQTH 等设置 | 按 `mtk-phy-asic.c` 的 `phy_init_soc()` 逐条重放 |
| CDC-ACM 能枚举，RNDIS 装不上（Windows 代码 10） | RNDIS 需要 Microsoft OS 描述符，且 Windows 驱动始终没发过一条 RNDIS 报文 | 放弃 RNDIS，改 **CDC-NCM**，Windows 10 1903+ 原生支持 |

PHY 上电序列（`0x11290800` 起）：

```
ACR6 (0x18): ISO_EN=0, BC1.1=0, OTG_VBUSCMP=1, SQTH=2
ACR5 (0x14): bit11 = 1
ACR4 (0x20): DP_100K_MODE=1, 其余复用位清 0
DTM0 (0x68): FORCE_UART_EN=0, FORCE_SUSPENDM=0
DTM1 (0x6c): RG_UART_EN=0
延时 800 µs
DTM1 (0x6c): FORCE_VBUSVALID | FORCE_SESSEND | FORCE_AVALID = 1
```

## 7. 网络

| 现象 | 原因 | 解法 |
|---|---|---|
| 设备 `192.168.42.1`，PC `192.168.137.1`，互相 ping 不通 | init 无条件退回静态地址 | 先等 UDC 状态变 `configured`，再配置网络；两侧地址都挂 |
| `udhcpc: lease obtained` 但 `ifconfig` 没有地址 | busybox 的 udhcpc 需要 `default.script` 才会真正配置网卡 | 自己写一个并 `-s` 指定 |
| ICS 的 DHCP 完全不应答 | Windows 把这块网卡归为 **Public**，UDP 67 入站被防火墙拦 | `Set-NetConnectionProfile -NetworkCategory Private` |
| ICS NAT 仍不转发 | Windows 的 `ipnat` 半残，重启服务无效 | 用 PC 侧代理 `tools/proxy.py` |
| `busybox wget` 访问 https 报 `error getting response` | 它不发 `CONNECT`，而是把 `GET https://...` 直接交给代理 | 用 `curl -x`；或让代理端代建 TLS |

## 8. initramfs 自身

| 现象 | 原因 | 解法 |
|---|---|---|
| `/bin/alpine`、`/sbin/reboot` 都不存在 | `wf()` 用 `open(path, O_WRONLY)`，**没有 O_CREAT**，写新文件静默失败 | 普通文件改用 `wfile()`（O_CREAT\|O_TRUNC） |
| 自建的 `/sbin/reboot` 被覆盖 | `busybox --install -s /bin` 在脚本之后又跑了一次 | 只保留一次，且在安装脚本之前 |
| `reboot` 命令无效 | busybox 的 reboot 在非 PID 1 时只给 init 发信号，而 init 已经 exec 成 sh | 改走 `/proc/sysrq-trigger` |
| `sh: can't access tty; job control turned off` | 控制台没有成为控制终端 | `ioctl(0, TIOCSCTTY, 1)` |
| `apt` 报 `Release file ... is not valid yet` | 无 RTC，时钟停在 1970（或硬编码的旧日期） | 用编译时间 `BUILD_EPOCH` 播种 |
| 解压 rootfs 解到内存盘 | `cd /mnt/data` 写错成执行 `/mnt/data`，后续命令仍在 `/` | `distro` 脚本里做路径检查 |
