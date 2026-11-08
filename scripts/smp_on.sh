#!/bin/sh
#
# smp_on.sh -- 在运行时给 MT6755 的从核上电，然后热插拔上线
#
# 背景：ATF 的 PSCI v0.2 是真的（CPU_ON 返回 SUCCESS），但 SPM 里
# MP0_CPU1_PWR_STA (bit10) 是 0 —— 电源域根本没开，核拿不到电，
# 所以内核只会看到 "failed in unknown state : 0x0"。
# 厂商的做法是在 OS 侧调 spm_mtcmos_ctrl_cpu1()，这里用 devmem 复现。
#
# 用法（在 initramfs 的 shell 里，必须有 busybox devmem）：
#   sh smp_on.sh 1          # 只上 CPU1，最安全，先验证
#   sh smp_on.sh 1 2 3      # 依次上 MP0 的三个从核
#   sh smp_on.sh all        # 1..7
#
# 寄存器与位定义来自 vendor mt_spm_reg_mt6755.h：
#   PWR_CON: bit0 RST_B  bit1 ISO  bit2 PWR_ON  bit3 PWR_ON_2ND
#            bit4 CLK_DIS  bit5 SRAM_CKISO  bit6 SRAM_ISOINT_B
#   L1_PDN : bit0 SRAM_PDN   bit8 SRAM_PDN_ACK
#
# 风险：SPM 是整机电源控制，写错可能掉电或挂死；只写我们需要的位。

SPM_BASE=0x10006000
POWERON_CONFIG_EN=0x10006000
PWR_STATUS=0x10006180
PWR_STATUS_2ND=0x10006184

# cluster top
MP0_CPUTOP_CON=0x10006204
MP1_CPUTOP_CON=0x10006218
# per-core PWR_CON
MP0_CPU0_CON=0x10006208
MP0_CPU1_CON=0x1000620C
MP0_CPU2_CON=0x10006210
MP0_CPU3_CON=0x10006214
MP1_CPU0_CON=0x1000621C
MP1_CPU1_CON=0x10006220
MP1_CPU2_CON=0x10006224
MP1_CPU3_CON=0x10006228
# per-core L1 PDN
MP0_CPU0_L1=0x10006248
MP0_CPU1_L1=0x1000624C
MP0_CPU2_L1=0x10006250
MP0_CPU3_L1=0x10006254
MP1_CPU0_L1=0x10006260
MP1_CPU1_L1=0x10006264
MP1_CPU2_L1=0x10006268
MP1_CPU3_L1=0x1000626C

PWR_ON=0x4          # bit2
PWR_ON_2ND=0x8      # bit3
PWR_ISO=0x2         # bit1
SRAM_CKISO=0x20     # bit5
SRAM_ISOINT_B=0x40  # bit6

rd() { devmem "$1"; }
wr() { devmem "$1" 32 "$2" >/dev/null 2>&1; }

# 读-改-写：set_bits ADDR MASK / clr_bits ADDR MASK
set_bits() { v=$(rd "$1"); wr "$1" $(( v | $2 )); }
clr_bits() { v=$(rd "$1"); wr "$1" $(( v & ~$2 )); }

# wait_mask ADDR MASK WANT TIMEOUT_TICKS  -- 返回 0 表示等到
wait_mask() {
	_a=$1; _m=$2; _w=$3; _t=${4:-50}; _i=0
	while [ $_i -lt $_t ]; do
		_v=$(rd "$_a")
		if [ $(( _v & _m )) -eq "$_w" ]; then
			return 0
		fi
		sleep 1
		_i=$(( _i + 1 ))
	done
	return 1
}

# cpu_slot N -> 设置 CON / L1 / cluster top / 状态位
cpu_slot() {
	case "$1" in
	1) CON=$MP0_CPU1_CON; L1=$MP0_CPU1_L1; TOP=$MP0_CPUTOP_CON; TOPS=0x100; STA=0x400 ;;
	2) CON=$MP0_CPU2_CON; L1=$MP0_CPU2_L1; TOP=$MP0_CPUTOP_CON; TOPS=0x100; STA=0x800 ;;
	3) CON=$MP0_CPU3_CON; L1=$MP0_CPU3_L1; TOP=$MP0_CPUTOP_CON; TOPS=0x100; STA=0x1000 ;;
	4) CON=$MP1_CPU0_CON; L1=$MP1_CPU0_L1; TOP=$MP1_CPUTOP_CON; TOPS=0x8000; STA=0x10000 ;;
	5) CON=$MP1_CPU1_CON; L1=$MP1_CPU1_L1; TOP=$MP1_CPUTOP_CON; TOPS=0x8000; STA=0x20000 ;;
	6) CON=$MP1_CPU2_CON; L1=$MP1_CPU2_L1; TOP=$MP1_CPUTOP_CON; TOPS=0x8000; STA=0x40000 ;;
	7) CON=$MP1_CPU3_CON; L1=$MP1_CPU3_L1; TOP=$MP1_CPUTOP_CON; TOPS=0x8000; STA=0x80000 ;;
	*) return 1 ;;
	esac
	return 0
}

power_on_cpu() {
	cpu=$1
	cpu_slot "$cpu" || { echo "bad cpu $cpu"; return 1; }

	printf 'cpu%d: ' "$cpu"

	# 1) cluster 没上电就先上（CPUTOP 的 STA 位与 CPU0 不同，用掩码判断）
	if [ $(( $(rd $PWR_STATUS) & TOPS )) -eq 0 ]; then
		printf 'cluster up... '
		set_bits "$TOP" $(( PWR_ON | PWR_ON_2ND ))
		if ! wait_mask "$PWR_STATUS" "$TOPS" "$TOPS" 30; then
			echo "CLUSTER FAILED (PWR_STATUS=$(rd $PWR_STATUS))"
			return 1
		fi
	fi

	# 2) 目标核上电
	printf 'core on... '
	set_bits "$CON" $(( PWR_ON | PWR_ON_2ND ))
	if ! wait_mask "$PWR_STATUS" "$STA" "$STA" 30; then
		echo "POWER FAILED (PWR_STATUS=$(rd $PWR_STATUS))"
		return 1
	fi

	# 3) 解隔离
	clr_bits "$CON" $PWR_ISO

	# 4) SRAM 退出掉电，等 ACK
	clr_bits "$L1" 0x1
	if ! wait_mask "$L1" 0x100 0 30; then
		echo "SRAM ACK TIMEOUT (L1_PDN=$(rd $L1))"
		return 1
	fi

	# 5) 释放 SRAM 隔离、关 SRAM 时钟隔离
	set_bits "$CON" $SRAM_ISOINT_B
	clr_bits "$CON" $SRAM_CKISO

	echo "ok (PWR_CON=$(rd $CON))"

	# 6) 交给 PSCI 把它拉起来
	echo 1 > "/sys/devices/system/cpu/cpu${cpu}/online" 2>/dev/null
	if [ $? -ne 0 ]; then
		echo "  cpu${cpu}: PSCI online FAILED"
		return 1
	fi
	echo "  cpu${cpu}: online"
	return 0
}

main() {
	if ! devmem $PWR_STATUS >/dev/null 2>&1; then
		echo "devmem 不可用（要在 initramfs 的 shell 里跑）"
		exit 1
	fi

	echo "SPM_PWR_STATUS = $(rd $PWR_STATUS)   (预期 bit8/9/14 为 1，bit10 为 0)"
	echo "unlock SPM register control"
	wr $POWERON_CONFIG_EN $(( 0xb16 << 16 | 1 ))

	if [ $# -eq 0 ]; then
		set -- 1
	fi
	if [ "$1" = "all" ]; then
		set -- 1 2 3 4 5 6 7
	fi

	for c in "$@"; do
		power_on_cpu "$c" || echo "cpu$c: 失败，继续下一个"
	done

	echo
	echo "PWR_STATUS = $(rd $PWR_STATUS)"
	echo "online     = $(cat /sys/devices/system/cpu/online)"
	echo "present    = $(cat /sys/devices/system/cpu/present)"
}

main "$@"
