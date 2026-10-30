// SPDX-License-Identifier: AGPL-3.0-or-later
#ifdef __riscv
#include <sys/syscall.h>
#include <unistd.h>
#include <asm/hwprobe.h>
#ifndef RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0
#define RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0	11
#endif
#define XTHEADVECTOR_BIT			(1 << 0)
#endif

#include "cpu.h"

int cpu_xtheadvector(void)
{
#ifdef __riscv
	static int have = -1;

	if (have < 0) {
		struct riscv_hwprobe p = {
			.key = RISCV_HWPROBE_KEY_VENDOR_EXT_THEAD_0,
		};

		have = !syscall(__NR_riscv_hwprobe, &p, 1, 0, NULL, 0) &&
		       p.key >= 0 && (p.value & XTHEADVECTOR_BIT);
	}
	return have;
#else
	return 0;
#endif
}
