#!/bin/sh
#
# smp_on.sh -- 给 MT6755 的从核上电，然后热插拔上线
#
# 背景：ATF 的 PSCI v0.2 是真的（CPU_ON 返回 SUCCESS），但内核只会
# 打印 "CPU1: failed in unknown state : 0x0" —— 这表示从核连一行代码
# 都没执行（secondary_data.status 仍是 0）。原因是 SPM 里这颗核的电源
# 域没开，而且复位也没释放。
#
# 本脚本逐位复现 vendor 的 spm_mtcmos_ctrl_cpuN(STA_POWER_ON) 序列
# (vendor/spm.c:364-405)。旧版漏掉了最关键的最后两步：
#     PWR_CLK_DIS = 0     -- 放开时钟门控
#     PWR_RST_B   = 1     -- 释放复位
# PWR_RST_B 保持 0 时 CPU 永远停在复位态，PSCI 返回成功也起不来。
#
# 用法（在 initramfs 的 shell 里，需要 busybox devmem）：
#   sh smp_on.sh dump      # 只打印 SPM 状态，不写寄存器
#   sh smp_on.sh 1         # 只上 CPU1（最安全，先验证）
#   sh smp_on.sh 1 2 3     # 依次上 MP0 的三个从核
#   sh smp_on.sh all       # 1..7
#
# 寄存器偏移来自 vendor 的 spm6755.h，位定义来自同名头文件的 *_LSB 宏。
# 风险：SPM 是整机电源控制，写错可能掉电或挂死；这里只动需要的位。

SPM=0x10006000
POWERON_CONFIG_EN=$SPM			# +0x000  SPM 写使能（要写魔数解锁）
PWR_STATUS=0x10006180			# +0x180
PWR_STATUS_2ND=0x10006184		# +0x184
CPU_PWR_STATUS=0x10006188		# +0x188 每个 CPU 单独的状态
CPU_PWR_STATUS_2ND=0x1000618C		# +0x18C
# INFRA_AO：总线保护（待首次运行确认，见 dump 的提示）
INFRA_TOPAXI_PROTECTEN=0x10001220
INFRA_TOPAXI_PROTECTSTA1=0x100012A8

# cluster top
MP0_CPUTOP_CON=0x10006204		# +0x204
MP0_CPUTOP_L2=0x10006240		# +0x240
MP1_CPUTOP_CON=0x10006218		# +0x218
MP1_CPUTOP_L2=0x10006258		# +0x258
# per-core PWR_CON
MP0_CPU0_CON=0x10006208			# +0x208
MP0_CPU1_CON=0x1000620C			# +0x20C
MP0_CPU2_CON=0x10006210			# +0x210
MP0_CPU3_CON=0x10006214			# +0x214
MP1_CPU0_CON=0x1000621C			# +0x21C
MP1_CPU1_CON=0x10006220			# +0x220
MP1_CPU2_CON=0x10006224			# +0x224
MP1_CPU3_CON=0x10006228			# +0x228
# per-core L1 PDN
MP0_CPU0_L1=0x10006248			# +0x248
MP0_CPU1_L1=0x1000624C			# +0x24C
MP0_CPU2_L1=0x10006250			# +0x250
MP0_CPU3_L1=0x10006254			# +0x254
MP1_CPU0_L1=0x10006260			# +0x260
MP1_CPU1_L1=0x10006264			# +0x264
MP1_CPU2_L1=0x10006268			# +0x268
MP1_CPU3_L1=0x1000626C			# +0x26C

# PWR_CON 位（spm6755.h 的 MP0_CPUx_*_LSB，各核位序相同）
PWR_RST_B=0x1		# bit0  0=复位保持
PWR_ISO=0x2		# bit1  1=隔离
PWR_ON=0x4		# bit2
PWR_ON_2ND=0x8		# bit3
PWR_CLK_DIS=0x10	# bit4  1=时钟关闭
SRAM_CKISO=0x20		# bit5
SRAM_ISOINT_B=0x40	# bit6

# L1_PDN / L2_PDN 位
SRAM_PDN=0x1		# bit0
SRAM_PDN_ACK=0x100	# bit8

# PWR_STATUS 位图（MT6755）：
#   bit8  MP0_CPUTOP      bit9..12  MP0_CPU0..3
#   bit13 MP1_CPUTOP      bit14..17 MP1_CPU0..3
# 注意 MP1 段从 bit14 起，不是 bit15 —— 旧版整体偏了一位，
# 上 cpu4..7 时会去等一个永远不会置位的状态。
MP0_CPUTOP_STA=0x100
MP1_CPUTOP_STA=0x2000

# (0xb16 << 16) | 1  -- SPM_PROJECT_CODE << 16 | REGWR_EN
SPM_UNLOCK=0xB1600001

rd() { devmem "$1" 2>/dev/null; }
wr() { devmem "$1" 32 "$2" >/dev/null 2>&1; }

# read-modify-write
set_bits() { v=$(rd "$1"); [ -n "$v" ] && wr "$1" $(( v | $2 )); }
clr_bits() { v=$(rd "$1"); [ -n "$v" ] && wr "$1" $(( v & ~$2 )); }

# wait_mask ADDR MASK WANT TIMEOUT -- 返回 0 表示等到
wait_mask() {
	_a=$1; _m=$2; _w=$3; _t=${4:-30}; _i=0
	while [ $_i -lt $_t ]; do
		_v=$(rd "$_a")
		# busybox 的 test 不认 0x 前缀，比较必须留在 $(( )) 里做
		[ $(( (_v & _m) == _w )) -ne 0 ] && return 0
		sleep 1
		_i=$(( _i + 1 ))
	done
	return 1
}

# 同时等 PWR_STATUS 和 PWR_STATUS_2ND 的掩码都等于 WANT
wait_both() {
	_m=$1; _w=$2; _t=${3:-30}; _i=0
	while [ $_i -lt $_t ]; do
		_a=$(rd $PWR_STATUS); _b=$(rd $PWR_STATUS_2ND)
		[ $(( (_a & _m) == _w )) -ne 0 ] && 		[ $(( (_b & _m) == _w )) -ne 0 ] && return 0
		sleep 1
		_i=$(( _i + 1 ))
	done
	return 1
}

# cpu_slot N -> CON / L1 / TOP / TOPL2 / TOPSTA / STA
cpu_slot() {
	case "$1" in
	1) CON=$MP0_CPU1_CON; L1=$MP0_CPU1_L1; TOP=$MP0_CPUTOP_CON; TOPL2=$MP0_CPUTOP_L2; TOPSTA=$MP0_CPUTOP_STA; STA=0x400 ;;
	2) CON=$MP0_CPU2_CON; L1=$MP0_CPU2_L1; TOP=$MP0_CPUTOP_CON; TOPL2=$MP0_CPUTOP_L2; TOPSTA=$MP0_CPUTOP_STA; STA=0x800 ;;
	3) CON=$MP0_CPU3_CON; L1=$MP0_CPU3_L1; TOP=$MP0_CPUTOP_CON; TOPL2=$MP0_CPUTOP_L2; TOPSTA=$MP0_CPUTOP_STA; STA=0x1000 ;;
	4) CON=$MP1_CPU0_CON; L1=$MP1_CPU0_L1; TOP=$MP1_CPUTOP_CON; TOPL2=$MP1_CPUTOP_L2; TOPSTA=$MP1_CPUTOP_STA; STA=0x4000 ;;
	5) CON=$MP1_CPU1_CON; L1=$MP1_CPU1_L1; TOP=$MP1_CPUTOP_CON; TOPL2=$MP1_CPUTOP_L2; TOPSTA=$MP1_CPUTOP_STA; STA=0x8000 ;;
	6) CON=$MP1_CPU2_CON; L1=$MP1_CPU2_L1; TOP=$MP1_CPUTOP_CON; TOPL2=$MP1_CPUTOP_L2; TOPSTA=$MP1_CPUTOP_STA; STA=0x10000 ;;
	7) CON=$MP1_CPU3_CON; L1=$MP1_CPU3_L1; TOP=$MP1_CPUTOP_CON; TOPL2=$MP1_CPUTOP_L2; TOPSTA=$MP1_CPUTOP_STA; STA=0x20000 ;;
	*) return 1 ;;
	esac
	return 0
}

# 复现 spm_mtcmos_ctrl_cpusysN(STA_POWER_ON)：L2 也在这个函数里放出来
power_on_cluster() {
	[ $(( $(rd $PWR_STATUS) & $TOPSTA )) -ne 0 ] && return 0
	printf '  cluster up... '
	set_bits "$TOP" $(( PWR_ON | PWR_ON_2ND ))
	if ! wait_both "$TOPSTA" "$TOPSTA" 20; then
		echo "FAILED (PWR_STATUS=$(rd $PWR_STATUS) 2ND=$(rd $PWR_STATUS_2ND))"
		return 1
	fi
	clr_bits "$TOP" $PWR_ISO
	clr_bits "$TOPL2" $SRAM_PDN
	if ! wait_mask "$TOPL2" "$SRAM_PDN_ACK" 0 20; then
		echo "L2 ACK TIMEOUT (L2_PDN=$(rd $TOPL2))"
		return 1
	fi
	set_bits "$TOP" $SRAM_ISOINT_B
	clr_bits "$TOP" $SRAM_CKISO
	clr_bits "$TOP" $PWR_CLK_DIS
	set_bits "$TOP" $PWR_RST_B
	echo "ok"
	return 0
}

# 按 vendor 的 STA_POWER_DOWN 顺序把一个核断开。
# 只在该核不在线时使用：那时它肯定没在跑 Linux。
power_down_cpu() {
	set_bits "$CON" $PWR_ISO
	set_bits "$CON" $SRAM_CKISO
	clr_bits "$CON" $SRAM_ISOINT_B
	set_bits "$L1" $SRAM_PDN
	clr_bits "$CON" $PWR_RST_B
	set_bits "$CON" $PWR_CLK_DIS
	clr_bits "$CON" $PWR_ON
	clr_bits "$CON" $PWR_ON_2ND
	sleep 1
	return 0
}

power_on_cpu() {
	cpu=$1
	cpu_slot "$cpu" || { echo "bad cpu $cpu"; return 1; }

	printf 'cpu%d: ' "$cpu"

	# 已经在线的核不用再走一遍：那 5 秒等待只对上电中才有意义。
	if [ "$(cat /sys/devices/system/cpu/cpu${cpu}/online 2>/dev/null)" = "1" ]; then
		echo "already online"
		return 0
	fi

	if [ $(( $(rd $PWR_STATUS) & STA )) -ne 0 ] && \
	   [ $(( $(rd $PWR_STATUS_2ND) & STA )) -ne 0 ]; then
		# 有电、但没上线：这种核大多是 ATF 开机时就把电源打开了，
		# 复位也放了，但它停在 Linux 之外。直接交给 PSCI 不会有任何动作，
		# 必须先断电再按正常顺序上电。
		echo "有电但未上线，先断电重来"
		power_down_cpu || return 1
		printf 'cpu%d: ' "$cpu"
	fi

	power_on_cluster || return 1

	printf '  core on... '
	set_bits "$CON" $(( PWR_ON | PWR_ON_2ND ))
	if ! wait_both "$STA" "$STA" 20; then
		echo "FAILED (PWR_STATUS=$(rd $PWR_STATUS) 2ND=$(rd $PWR_STATUS_2ND))"
		return 1
	fi

	clr_bits "$CON" $PWR_ISO

	clr_bits "$L1" $SRAM_PDN
	if ! wait_mask "$L1" "$SRAM_PDN_ACK" 0 20; then
		echo "SRAM ACK TIMEOUT (L1_PDN=$(rd $L1))"
		return 1
	fi

	set_bits "$CON" $SRAM_ISOINT_B
	clr_bits "$CON" $SRAM_CKISO
	clr_bits "$CON" $PWR_CLK_DIS
	echo "done (PWR_CON=$(rd $CON), 复位仍保持)"

	#
	# 顺序很重要，而且要在一个时间窗内完成。
	#
	# ATF 的 CPU_ON 本该做三件事：存 entrypoint、跑它自己的 SPM 上电、
	# 解复位。实测它只做了第一件 —— 调用前后 MP0_CPU1_PWR_CON 都是
	# 0x4C（RST_B 始终为 0），所以核永远停在复位里，内核看到的就是
	# "failed in unknown state : 0x0"。最后一位只能我们自己补。
	#
	# 但这个写会阻塞约 5 秒：内核在 __cpu_up() 里
	# wait_for_completion_timeout(&cpu_running, 5000ms)，超时后立刻把
	# secondary_data.task 清成 NULL —— 从核靠它算栈顶，
	# 一旦在窗口外解复位，核跳进 secondary_entry 时 sp=0，
	# 必然取指失败。所以放后台执行，
	# 在窗口内补上最后一位。
	#
	echo 1 > "/sys/devices/system/cpu/cpu${cpu}/online" 2>/dev/null &
	sleep 1

	if [ $(( $(rd $CON) & PWR_RST_B )) -eq 0 ]; then
		echo "  ATF 没有解复位，本地补 PWR_RST_B"
		set_bits "$CON" $PWR_RST_B
	fi

	sleep 5

	echo "  PWR_CON=$(rd $CON)  online=$(cat /sys/devices/system/cpu/online 2>/dev/null)"
	if [ $(( $(rd $CON) & PWR_RST_B )) -ne 0 ]; then
		return 0
	fi
	return 1
}

dump() {
	echo "POWERON_CONFIG_EN  = $(rd $POWERON_CONFIG_EN)"
	echo "PWR_STATUS         = $(rd $PWR_STATUS)"
	echo "PWR_STATUS_2ND     = $(rd $PWR_STATUS_2ND)"
	echo "MP0_CPUTOP_PWR_CON = $(rd $MP0_CPUTOP_CON)"
	echo "MP0_CPUTOP_L2_PDN  = $(rd $MP0_CPUTOP_L2)"
	echo "MP0_CPU0_PWR_CON   = $(rd $MP0_CPU0_CON)"
	echo "MP0_CPU1_PWR_CON   = $(rd $MP0_CPU1_CON)"
	echo "MP0_CPU1_L1_PDN    = $(rd $MP0_CPU1_L1)"
	echo "MP1_CPUTOP_PWR_CON = $(rd $MP1_CPUTOP_CON)"
	echo "CPU_PWR_STATUS     = $(rd $CPU_PWR_STATUS)"
	echo "CPU_PWR_STATUS_2ND = $(rd $CPU_PWR_STATUS_2ND)"
	#
	# MT6755 的 INFRA_AO 里，总线保护（TOPAXI protect）会把 CPU 的取指
	# 通路挡掉：核有电、出了复位，但一条指令都取不到，表现就是 PSCI
	# 返回成功而 secondary_data.status 一直是 0。vendor 的 cpusys 上电
	# 序列最后一步就是 spm_topaxi_protect(mask, 0)。
	# 下面两个地址是按 MT6755 INFRA_AO 布局写的，先读出来看，不要盲写。
	echo "TOPAXI_PROTECTEN   = $(rd $INFRA_TOPAXI_PROTECTEN)   [$INFRA_TOPAXI_PROTECTEN]"
	echo "TOPAXI_PROTECTSTA1 = $(rd $INFRA_TOPAXI_PROTECTSTA1)  [$INFRA_TOPAXI_PROTECTSTA1]"
	echo "  (若读到 0xffffffff 说明地址可能不对，用 peek 试别的)"
	echo "online             = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
	echo "present            = $(cat /sys/devices/system/cpu/present 2>/dev/null)"
}

# 名字表，只覆盖 CPU 相关的 8..17 位
bit_name() {
	case "$1" in
	8)  echo "MP0_CPUTOP" ;;
	9)  echo "MP0_CPU0" ;;
	10) echo "MP0_CPU1" ;;
	11) echo "MP0_CPU2" ;;
	12) echo "MP0_CPU3" ;;
	13) echo "MP1_CPUTOP" ;;
	14) echo "MP1_CPU0" ;;
	15) echo "MP1_CPU1" ;;
	16) echo "MP1_CPU2" ;;
	17) echo "MP1_CPU3" ;;
	*)  echo "" ;;
	esac
}

decode_status() {
	_v=$(rd $PWR_STATUS)
	_v2=$(rd $PWR_STATUS_2ND)
	_vc=$(rd $CPU_PWR_STATUS)
	echo "  PWR_STATUS       = $_v"
	echo "  PWR_STATUS_2ND   = $_v2"
	echo "  CPU_PWR_STATUS   = $_vc"
	_i=0
	while [ $_i -lt 20 ]; do
		if [ -n "$_v" ] && [ $(( _v & (1 << _i) )) -ne 0 ]; then
			_n=$(bit_name $_i)
			printf '    bit%-2d = 1   %s\n' "$_i" "$_n"
		fi
		_i=$(( _i + 1 ))
	done
}

# /dev/mem 能不能映射 SPM 这一页？不能的话后面全是白忙，先说清楚。
probe_mmio() {
	_out=$(devmem $SPM 2>&1)
	case "$_out" in
	0x*)
		echo "  SPM 可访问: $SPM = $_out"
		return 0
		;;
	*)
		echo "  SPM 不可访问: $_out"
		echo "  → /dev/mem 映射 0x1000_6000 失败。这个内核是 CONFIG_STRICT_DEVMEM=y，"
		echo "    而且 init.c 里已经记录过 0x1000_xxxx 这一段会被拒。"
		echo "  → 在这种情况下 devmem 这条路走不通，SPM 只能由内核里的驱动去 ioremap。"
		return 1
		;;
	esac
}

main() {
	if ! command -v devmem >/dev/null 2>&1; then
		echo "busybox 里没有 devmem，这个脚本没法跑"
		exit 1
	fi

	# sh smp_on.sh peek <addr> [addr...]  —— 只读，用来查任意寄存器
	if [ "$1" = "peek" ]; then
		shift
		for a in "$@"; do
			printf '%s = %s
' "$a" "$(rd $a)"
		done
		exit 0
	fi

	echo "===== SPM 可访问性 ====="
	probe_mmio || exit 2

	echo
	echo "===== 上电前状态 ====="
	decode_status
	echo "  online  = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
	echo "  present = $(cat /sys/devices/system/cpu/present 2>/dev/null)"

	[ "$1" = "dump" ] && { echo; dump; exit 0; }

	echo
	echo "unlock SPM register control (0x$(printf '%08x' $SPM_UNLOCK))"
	wr $POWERON_CONFIG_EN $SPM_UNLOCK
	_v=$(rd $POWERON_CONFIG_EN)
	if [ "$_v" = "0x00000000" ] || [ "$_v" = "0x0" ]; then
		echo "WARN: 写 POWERON_CONFIG_EN 后读回 0，SPM 可能被 ATF 锁住了"
		echo "      （也可能只是这台机器的 SPM 对该写入不响应）"
	fi

	[ $# -eq 0 ] && set -- 1
	[ "$1" = "all" ] && set -- 1 2 3 4 5 6 7

	echo
	for c in "$@"; do
		power_on_cpu "$c" || echo "cpu$c: 失败，继续下一个"
	done

	echo
	echo "===== 上电后状态 ====="
	decode_status
	echo "  online = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
	echo "  nproc  = $(nproc 2>/dev/null)"
}

main "$@"
