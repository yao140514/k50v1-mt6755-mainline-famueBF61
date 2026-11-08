#!/usr/bin/env python3
"""Make the mt65xx timeout path print the controller state at dev_err level.

dev_dbg is a no-op in our build, so a timed-out transfer tells us nothing.
With the registers printed we can finally tell apart:
  * INTR_STAT has TRANSAC_COMP  -> the transfer finished, the IRQ never came
  * INTR_STAT == 0, START still set -> the state machine never ran
This is a temporary bring-up diagnostic; revert before upstreaming.
"""
import sys

p = 'drivers/i2c/busses/i2c-mt65xx.c'
s = open(p).read()

old = """	if (ret == 0) {
		dev_dbg(i2c->dev, "addr: %x, transfer timeout\\n", msgs->addr);
		i2c_dump_register(i2c);
		mtk_i2c_init_hw(i2c);
		return -ETIMEDOUT;
	}
"""

new = """	if (ret == 0) {
		dev_err(i2c->dev,
			"BRINGUP timeout addr=0x%x INTR_STAT=0x%x START=0x%x DEBUGSTAT=0x%x IO_CONFIG=0x%x TIMING=0x%x CONTROL=0x%x\\n",
			msgs->addr,
			mtk_i2c_readw(i2c, OFFSET_INTR_STAT),
			mtk_i2c_readw(i2c, OFFSET_START),
			mtk_i2c_readw(i2c, OFFSET_DEBUGSTAT),
			mtk_i2c_readw(i2c, OFFSET_IO_CONFIG),
			mtk_i2c_readw(i2c, OFFSET_TIMING),
			mtk_i2c_readw(i2c, OFFSET_CONTROL));
		i2c_dump_register(i2c);
		mtk_i2c_init_hw(i2c);
		return -ETIMEDOUT;
	}
"""

if old not in s:
    sys.exit('timeout block not found')
s = s.replace(old, new, 1)
open(p, 'w').write(s)
print('timeout diagnostics added')
