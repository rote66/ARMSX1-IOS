#include "fastboot.h"

#include <stdlib.h>
#include <string.h>

#include "bus_init.h"
#include "exe.h"
#include "log.h"
#include "dev/cdrom/cdrom.h"
#include "dev/cdrom/disc.h"
#include "dev/ram.h"

#define FB_SECTOR_BYTES 2352
#define FB_USER_DATA_OFFSET 24
#define FB_USER_DATA_BYTES 2048
#define FB_LEAD_IN 150
#define FB_ISO_PVD_SECTOR 16

#define FB_DR_LENGTH 0
#define FB_DR_EXTENT_LE 2
#define FB_DR_SIZE_LE 10
#define FB_DR_NAME_LEN 32
#define FB_DR_NAME 33
#define FB_PVD_ROOT_DR 156

static int g_fastboot_enabled = 0;
static int g_fastboot_armed = 0;
static int g_fastboot_succeeded = 0;

void psx_fastboot_set_enabled(int enabled) {
    g_fastboot_enabled = enabled ? 1 : 0;
    g_fastboot_armed = 0;
    g_fastboot_succeeded = 0;
}

int psx_fastboot_enabled(void) {
    return g_fastboot_enabled;
}

int psx_fastboot_pending(void) {
    return g_fastboot_armed;
}

int psx_fastboot_succeeded(void) {
    return g_fastboot_succeeded;
}

void psx_fastboot_rearm(void) {
    g_fastboot_armed = 0;
    g_fastboot_succeeded = 0;
}

static uint32_t fb_read_le32_unaligned(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fb_find_type1_patch_offset(const uint8_t* image, size_t image_size, size_t* out_offset) {
    if (!image || !out_offset || image_size < 32)
        return 0;

    for (size_t offset = 0; offset + 32 <= image_size; offset += 4) {
        const uint8_t* p = image + offset;
        const uint32_t w0 = fb_read_le32_unaligned(p + 0);
        const uint32_t w1 = fb_read_le32_unaligned(p + 4);
        const uint32_t w2 = fb_read_le32_unaligned(p + 8);
        const uint32_t w3 = fb_read_le32_unaligned(p + 12);
        const uint32_t w4 = fb_read_le32_unaligned(p + 16);
        const uint32_t w5 = fb_read_le32_unaligned(p + 20);
        const uint32_t w6 = fb_read_le32_unaligned(p + 24);
        const uint32_t w7 = fb_read_le32_unaligned(p + 28);

        if (w0 != 0x27bdffe0u || w1 != 0xafbf001cu || w2 != 0xafa40020u)
            continue;
        if ((w3 & 0xffff0000u) != 0x3c050000u || (w4 & 0xffff0000u) != 0x3c060000u)
            continue;
        if ((w5 & 0xffff0000u) != 0x34c60000u || (w6 & 0xffff0000u) != 0x34a50000u)
            continue;
        if ((w7 & 0xfc000000u) != 0x0c000000u)
            continue;

        *out_offset = offset;
        return 1;
    }

    return 0;
}

int psx_fastboot_patch_bios(psx_bios_t* bios) {
    static const uint32_t shell_replacement[] = {
        0x3c011f80u, /* lui at, 0x1f80 */
        0x3c0a0300u, /* lui t2, 0x0300 */
        0xac2a1814u, /* sw t2, 0x1814(at) - display on */
        0x03e00008u, /* jr ra */
        0x00000000u, /* nop */
    };
    size_t patch_offset = 0;

    g_fastboot_armed = 0;
    g_fastboot_succeeded = 0;

    if (!g_fastboot_enabled || !bios || !bios->buf || bios->io_size < sizeof(shell_replacement))
        return 0;

    if (!fb_find_type1_patch_offset(bios->buf, bios->io_size, &patch_offset)) {
        log_warn("fastboot: unsupported BIOS layout; using normal BIOS boot");
        return 0;
    }

    memcpy(bios->buf + patch_offset, shell_replacement, sizeof(shell_replacement));
    g_fastboot_succeeded = 1;
    log_info("fastboot: patched BIOS shell bootstrap at offset %08zx", patch_offset);
    return 1;
}

static char fb_tolower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static uint32_t fb_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fb_read_iso_sector(psx_disc_t* disc, uint32_t iso_lba, uint8_t* out) {
    uint8_t sector[FB_SECTOR_BYTES];

    if (!disc || !psx_disc_read(disc, iso_lba + FB_LEAD_IN, sector))
        return 0;

    memcpy(out, sector + FB_USER_DATA_OFFSET, FB_USER_DATA_BYTES);
    return 1;
}

static int fb_name_matches(const uint8_t* record_name, int record_len, const char* wanted) {
    int i = 0;

    for (; i < record_len; ++i) {
        const char c = (char)record_name[i];

        if (c == ';')
            break;
        if (!wanted[i] || fb_tolower_ascii(c) != fb_tolower_ascii(wanted[i]))
            return 0;
    }

    return wanted[i] == '\0';
}

static int fb_find_in_root(psx_disc_t* disc, const char* name, uint32_t* out_extent, uint32_t* out_size) {
    uint8_t buffer[FB_USER_DATA_BYTES];
    uint32_t root_extent, root_size, sector, sectors;

    if (!fb_read_iso_sector(disc, FB_ISO_PVD_SECTOR, buffer) || memcmp(buffer + 1, "CD001", 5) != 0)
        return 0;

    root_extent = fb_read_le32(buffer + FB_PVD_ROOT_DR + FB_DR_EXTENT_LE);
    root_size = fb_read_le32(buffer + FB_PVD_ROOT_DR + FB_DR_SIZE_LE);
    if (!root_size || root_size > (16u * 1024u * 1024u))
        return 0;

    sectors = (root_size + FB_USER_DATA_BYTES - 1) / FB_USER_DATA_BYTES;
    for (sector = 0; sector < sectors; ++sector) {
        uint32_t offset = 0;

        if (!fb_read_iso_sector(disc, root_extent + sector, buffer))
            return 0;

        while (offset < FB_USER_DATA_BYTES) {
            const uint8_t* record = buffer + offset;
            const uint8_t length = record[FB_DR_LENGTH];
            uint8_t name_len;

            if (length == 0 || offset + length > FB_USER_DATA_BYTES)
                break;

            name_len = record[FB_DR_NAME_LEN];
            if (name_len && (uint32_t)(FB_DR_NAME + name_len) <= length &&
                fb_name_matches(record + FB_DR_NAME, name_len, name)) {
                *out_extent = fb_read_le32(record + FB_DR_EXTENT_LE);
                *out_size = fb_read_le32(record + FB_DR_SIZE_LE);
                return *out_size != 0;
            }

            offset += length;
        }
    }

    return 0;
}

static int fb_parse_boot_line(const char* text, size_t length, char* out, size_t out_size) {
    size_t i;

    for (i = 0; i + 4 < length; ++i) {
        const char* p;
        size_t written = 0;

        if (!(text[i] == 'B' || text[i] == 'b'))
            continue;
        if (strncmp(text + i, "BOOT", 4) != 0 && strncmp(text + i, "boot", 4) != 0)
            continue;

        p = text + i + 4;
        while ((size_t)(p - text) < length && (*p == ' ' || *p == '\t'))
            ++p;
        if ((size_t)(p - text) >= length || *p != '=')
            continue;
        ++p;
        while ((size_t)(p - text) < length && (*p == ' ' || *p == '\t'))
            ++p;

        if ((size_t)(p - text) + 6 <= length &&
            (strncmp(p, "cdrom:", 6) == 0 || strncmp(p, "CDROM:", 6) == 0)) {
            p += 6;
        }
        while ((size_t)(p - text) < length && (*p == '\\' || *p == '/'))
            ++p;

        while ((size_t)(p - text) < length && written + 1 < out_size) {
            const char c = *p;
            if (c == ';' || c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '\0')
                break;
            out[written++] = c;
            ++p;
        }

        out[written] = '\0';
        return written > 0;
    }

    return 0;
}

int psx_fastboot_try(psx_cpu_t* cpu) {
    uint8_t sector[FB_USER_DATA_BYTES];
    char boot_name[64];
    char cnf[FB_USER_DATA_BYTES + 1];
    psx_disc_t* disc;
    psx_exe_hdr_t hdr;
    uint32_t extent = 0, size = 0, ram_offset, remaining, iso_sector;
    uint8_t* dst;

    if (!g_fastboot_armed || !cpu || !cpu->bus || !cpu->bus->cdrom || !cpu->bus->ram)
        return 0;

    g_fastboot_armed = 0;
    disc = cpu->bus->cdrom->disc;
    if (!disc)
        return 0;

    if (!fb_find_in_root(disc, "SYSTEM.CNF", &extent, &size)) {
        log_info("fastboot: no SYSTEM.CNF; continuing with the BIOS");
        return 0;
    }
    if (!fb_read_iso_sector(disc, extent, sector))
        return 0;

    memcpy(cnf, sector, FB_USER_DATA_BYTES);
    cnf[FB_USER_DATA_BYTES] = '\0';
    if (!fb_parse_boot_line(cnf, size < FB_USER_DATA_BYTES ? size : FB_USER_DATA_BYTES,
                            boot_name, sizeof(boot_name))) {
        log_info("fastboot: SYSTEM.CNF has no usable BOOT line; continuing with the BIOS");
        return 0;
    }

    if (!fb_find_in_root(disc, boot_name, &extent, &size)) {
        log_info("fastboot: '%s' is missing; continuing with the BIOS", boot_name);
        return 0;
    }
    if (!fb_read_iso_sector(disc, extent, sector))
        return 0;

    memcpy(&hdr, sector, sizeof(hdr));
    if (memcmp(hdr.id, "PS-X EXE", 8) != 0) {
        log_info("fastboot: '%s' is not a PS-X EXE; continuing with the BIOS", boot_name);
        return 0;
    }

    ram_offset = hdr.ramdest & 0x7fffffff;
    if (!hdr.filesz || ram_offset >= PSX_RAM_SIZE || hdr.filesz > (PSX_RAM_SIZE - ram_offset)) {
        log_error("fastboot: '%s' is out of RAM range (dest=%08x size=%u)", boot_name, hdr.ramdest, hdr.filesz);
        return 0;
    }

    dst = cpu->bus->ram->buf + ram_offset;
    remaining = hdr.filesz;
    iso_sector = extent + 1;
    while (remaining) {
        const uint32_t chunk = remaining < FB_USER_DATA_BYTES ? remaining : FB_USER_DATA_BYTES;

        if (!fb_read_iso_sector(disc, iso_sector, sector)) {
            log_error("fastboot: read failed %u bytes into '%s'", hdr.filesz - remaining, boot_name);
            return 0;
        }

        memcpy(dst, sector, chunk);
        dst += chunk;
        remaining -= chunk;
        ++iso_sector;
    }

    psx_cpu_invalidate_range(cpu, ram_offset, hdr.filesz);
    cpu->pc = hdr.ipc;
    cpu->next_pc = cpu->pc + 4;
    cpu->r[28] = hdr.igp;
    if (hdr.ispb) {
        cpu->r[29] = hdr.ispb + hdr.ispoff;
        cpu->r[30] = cpu->r[29];
    }

    log_info("fastboot: booted '%s' (PC=%08x SP=%08x GP=%08x)",
             boot_name, cpu->pc, cpu->r[29], cpu->r[28]);
    g_fastboot_succeeded = 1;
    return 1;
}
