/*
    ARMSX fast boot support.

    The BIOS still performs its kernel initialization. At the shell entry point, the
    implementation loads the disc executable named by SYSTEM.CNF and transfers control
    to it, skipping only the startup presentation. Any unsupported disc falls back to a
    normal BIOS boot.
*/

#ifndef PSX_FASTBOOT_H
#define PSX_FASTBOOT_H

#include "cpu.h"
#include "dev/bios.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_FASTBOOT_SHELL_PHYS 0x00030000u

void psx_fastboot_set_enabled(int enabled);
int psx_fastboot_enabled(void);
int psx_fastboot_pending(void);
int psx_fastboot_succeeded(void);
int psx_fastboot_patch_bios(psx_bios_t* bios);
void psx_fastboot_rearm(void);
int psx_fastboot_try(psx_cpu_t* cpu);

#ifdef __cplusplus
}
#endif

#endif
