#!/usr/bin/env python3
"""Move the VLDO28 / CTP-reset block so it runs *after* pmic_init().

The first attempt sat before pmic_init(), so the PMIC wrapper was not up yet
and both register writes were silently dropped (the boot log shows
"[BRINGUP] VLDO28 enabled" three lines above "[pmic_init] LK Start").
"""
import sys

p = 'platform/mt6755/platform.c'
s = open(p).read()

start = s.find("\t/*\n\t * bring-up: power the touch panel.")
if start < 0:
    sys.exit('block start not found')

# the block ends with the CTP reset dprintf line
endmark = 'dprintf(CRITICAL, "[BRINGUP] CTP reset released (GPIO10 high)\\n");\n'
end = s.find(endmark, start)
if end < 0:
    sys.exit('block end not found')
end += len(endmark)

block = s[start:end]
s = s[:start] + s[end:]

anchor = '\tpmic_init();\n'
if anchor not in s:
    sys.exit('pmic_init() anchor not found')
s = s.replace(anchor, anchor + '\n' + block, 1)

open(p, 'w').write(s)
print('moved after pmic_init()')
