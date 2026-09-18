#include "psx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_init.h"
#include "dev/cdrom/queue.h"
#include "dev/mcd.h"
#include "input/sda.h"

#define PSX_STATE_VERSION 1u
#define PSX_STATE_MAGIC "ARMSXST"

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t psx_size;
    uint32_t cpu_size;
    uint32_t ram_size;
    uint32_t dma_size;
    uint32_t mc1_size;
    uint32_t mc2_size;
    uint32_t mc3_size;
    uint32_t ic_size;
    uint32_t scratchpad_size;
    uint32_t gpu_size;
    uint32_t spu_size;
    uint32_t bus_size;
    uint32_t timer_size;
    uint32_t cdrom_size;
    uint32_t pad_size;
    uint32_t mdec_size;
    uint32_t ram_bytes;
    uint32_t mdec_input_bytes;
    uint32_t mdec_output_bytes;
} psx_state_header_t;

static int write_exact(FILE* file, const void* data, size_t size) {
    return file && data && fwrite(data, 1, size, file) == size;
}

static int read_exact(FILE* file, void* data, size_t size) {
    return file && data && fread(data, 1, size, file) == size;
}

static int write_queue(FILE* file, const queue_t* queue) {
    if (!queue || queue->size > UINT32_MAX)
        return 0;

    const uint32_t size = (uint32_t)queue->size;
    const uint32_t read_index = (uint32_t)queue->read_index;
    const uint32_t write_index = (uint32_t)queue->write_index;
    return write_exact(file, &size, sizeof(size)) &&
        write_exact(file, &read_index, sizeof(read_index)) &&
        write_exact(file, &write_index, sizeof(write_index)) &&
        write_exact(file, queue->buf, queue->size);
}

static int read_queue(FILE* file, queue_t* queue) {
    uint32_t size = 0;
    uint32_t read_index = 0;
    uint32_t write_index = 0;
    if (!queue || !read_exact(file, &size, sizeof(size)) ||
        !read_exact(file, &read_index, sizeof(read_index)) ||
        !read_exact(file, &write_index, sizeof(write_index)))
        return 0;
    if (size != queue->size || read_index > write_index || write_index > size)
        return 0;
    if (!read_exact(file, queue->buf, queue->size))
        return 0;
    queue->read_index = read_index;
    queue->write_index = write_index;
    return 1;
}

static int write_mcd(FILE* file, const psx_mcd_t* mcd) {
    const uint8_t present = mcd ? 1 : 0;
    if (!write_exact(file, &present, sizeof(present)))
        return 0;
    if (!mcd)
        return 1;
    return write_exact(file, mcd, sizeof(*mcd)) &&
        write_exact(file, mcd->buf, MCD_MEMORY_SIZE);
}

static int read_mcd(FILE* file, psx_mcd_t* mcd) {
    uint8_t present = 0;
    if (!read_exact(file, &present, sizeof(present)))
        return 0;
    if (!present)
        return mcd == NULL;
    if (!mcd || !mcd->buf)
        return 0;

    psx_mcd_t saved;
    if (!read_exact(file, &saved, sizeof(saved)))
        return 0;
    const char* path = mcd->path;
    uint8_t* buf = mcd->buf;
    *mcd = saved;
    mcd->path = path;
    mcd->buf = buf;
    return read_exact(file, mcd->buf, MCD_MEMORY_SIZE);
}

static psx_state_header_t make_header(const psx_t* psx) {
    psx_state_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, PSX_STATE_MAGIC, 7);
    header.version = PSX_STATE_VERSION;
    header.psx_size = sizeof(psx_t);
    header.cpu_size = sizeof(psx_cpu_t);
    header.ram_size = sizeof(psx_ram_t);
    header.dma_size = sizeof(psx_dma_t);
    header.mc1_size = sizeof(psx_mc1_t);
    header.mc2_size = sizeof(psx_mc2_t);
    header.mc3_size = sizeof(psx_mc3_t);
    header.ic_size = sizeof(psx_ic_t);
    header.scratchpad_size = sizeof(psx_scratchpad_t);
    header.gpu_size = sizeof(psx_gpu_t);
    header.spu_size = sizeof(psx_spu_t);
    header.bus_size = sizeof(psx_bus_t);
    header.timer_size = sizeof(psx_timer_t);
    header.cdrom_size = sizeof(psx_cdrom_t);
    header.pad_size = sizeof(psx_pad_t);
    header.mdec_size = sizeof(psx_mdec_t);
    header.ram_bytes = psx && psx->ram ? (uint32_t)psx->ram->size : 0;
    if (psx && psx->mdec && psx->mdec->input && psx->mdec->input_size)
        header.mdec_input_bytes = (uint32_t)psx->mdec->input_size;
    if (psx && psx->mdec && psx->mdec->output && psx->mdec->output_words_remaining) {
        const size_t bytes = (size_t)psx->mdec->output_words_remaining * sizeof(uint32_t);
        header.mdec_output_bytes = bytes <= UINT32_MAX ? (uint32_t)bytes : 0;
    }
    return header;
}

static int header_matches(const psx_state_header_t* header, const psx_t* psx) {
    return header && psx &&
        memcmp(header->magic, PSX_STATE_MAGIC, 7) == 0 &&
        header->version == PSX_STATE_VERSION &&
        header->psx_size == sizeof(psx_t) &&
        header->cpu_size == sizeof(psx_cpu_t) &&
        header->ram_size == sizeof(psx_ram_t) &&
        header->dma_size == sizeof(psx_dma_t) &&
        header->mc1_size == sizeof(psx_mc1_t) &&
        header->mc2_size == sizeof(psx_mc2_t) &&
        header->mc3_size == sizeof(psx_mc3_t) &&
        header->ic_size == sizeof(psx_ic_t) &&
        header->scratchpad_size == sizeof(psx_scratchpad_t) &&
        header->gpu_size == sizeof(psx_gpu_t) &&
        header->spu_size == sizeof(psx_spu_t) &&
        header->bus_size == sizeof(psx_bus_t) &&
        header->timer_size == sizeof(psx_timer_t) &&
        header->cdrom_size == sizeof(psx_cdrom_t) &&
        header->pad_size == sizeof(psx_pad_t) &&
        header->mdec_size == sizeof(psx_mdec_t) &&
        psx->ram && header->ram_bytes == psx->ram->size;
}

int psx_save_state(psx_t* psx, const char* path) {
    if (!psx || !path || !psx->cpu || !psx->ram || !psx->gpu || !psx->spu || !psx->cdrom || !psx->mdec)
        return 0;

    FILE* file = fopen(path, "wb");
    if (!file)
        return 0;

    const psx_state_header_t header = make_header(psx);
    int ok = write_exact(file, &header, sizeof(header));
    if (ok) psx_cpu_save_state(psx->cpu, file);
    ok = ok && !ferror(file);
    ok = ok && write_exact(file, psx->ram, sizeof(*psx->ram));
    ok = ok && write_exact(file, psx->ram->buf, psx->ram->size);
    ok = ok && write_exact(file, psx->dma, sizeof(*psx->dma));
    ok = ok && write_exact(file, psx->mc1, sizeof(*psx->mc1));
    ok = ok && write_exact(file, psx->mc2, sizeof(*psx->mc2));
    ok = ok && write_exact(file, psx->mc3, sizeof(*psx->mc3));
    ok = ok && write_exact(file, psx->ic, sizeof(*psx->ic));
    ok = ok && write_exact(file, psx->scratchpad, sizeof(*psx->scratchpad));
    ok = ok && write_exact(file, psx->scratchpad->buf, PSX_SCRATCHPAD_SIZE);
    ok = ok && write_exact(file, psx->gpu, sizeof(*psx->gpu));
    ok = ok && write_exact(file, psx->gpu->vram, PSX_GPU_VRAM_SIZE);
    ok = ok && write_exact(file, psx->spu, sizeof(*psx->spu));
    ok = ok && write_exact(file, psx->spu->ram, SPU_RAM_SIZE);
    ok = ok && write_exact(file, psx->bus, sizeof(*psx->bus));
    ok = ok && write_exact(file, psx->timer, sizeof(*psx->timer));
    ok = ok && write_exact(file, psx->cdrom, sizeof(*psx->cdrom));
    ok = ok && write_queue(file, psx->cdrom->data);
    ok = ok && write_queue(file, psx->cdrom->response);
    ok = ok && write_queue(file, psx->cdrom->parameters);
    ok = ok && write_exact(file, psx->pad, sizeof(*psx->pad));
    ok = ok && write_mcd(file, psx->pad->mcd_slot[0]);
    ok = ok && write_mcd(file, psx->pad->mcd_slot[1]);

    const uint8_t has_sda = (psx->pad->joy_slot[0] && psx->pad->joy_slot[0]->udata) ? 1 : 0;
    ok = ok && write_exact(file, &has_sda, sizeof(has_sda));
    if (ok && has_sda)
        ok = write_exact(file, psx->pad->joy_slot[0]->udata, sizeof(psxi_sda_t));

    ok = ok && write_exact(file, psx->mdec, sizeof(*psx->mdec));
    if (ok && header.mdec_input_bytes)
        ok = write_exact(file, psx->mdec->input, header.mdec_input_bytes);
    if (ok && header.mdec_output_bytes) {
        const uint8_t* remaining = psx->mdec->output + ((size_t)psx->mdec->output_index * sizeof(uint32_t));
        ok = write_exact(file, remaining, header.mdec_output_bytes);
    }

    if (fflush(file) != 0)
        ok = 0;
    fclose(file);
    if (!ok)
        remove(path);
    return ok;
}

int psx_load_state(psx_t* psx, const char* path) {
    if (!psx || !path)
        return 0;

    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;

    psx_state_header_t header;
    int ok = read_exact(file, &header, sizeof(header)) && header_matches(&header, psx);
    if (!ok) {
        fclose(file);
        return 0;
    }

    psx_cpu_load_state(psx->cpu, file);
    ok = !ferror(file);

#define LOAD_PRESERVE_ONE(object, saved_type, pointer_field, saved_pointer) \
    do { \
        saved_type saved; \
        if (!read_exact(file, &saved, sizeof(saved))) { ok = 0; break; } \
        saved_pointer = (object)->pointer_field; \
        *(object) = saved; \
        (object)->pointer_field = saved_pointer; \
    } while (0)

    if (ok) {
        psx_ram_t saved;
        if (!read_exact(file, &saved, sizeof(saved))) ok = 0;
        if (ok) {
            uint8_t* buf = psx->ram->buf;
            psx_mc2_t* mc2 = psx->ram->mc2;
            *psx->ram = saved;
            psx->ram->buf = buf;
            psx->ram->mc2 = mc2;
            ok = read_exact(file, psx->ram->buf, psx->ram->size);
        }
    }
    if (ok) {
        psx_dma_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_bus_t* bus = psx->dma->bus;
            psx_ic_t* ic = psx->dma->ic;
            *psx->dma = saved;
            psx->dma->bus = bus;
            psx->dma->ic = ic;
        }
    }
    if (ok) ok = read_exact(file, psx->mc1, sizeof(*psx->mc1));
    if (ok) ok = read_exact(file, psx->mc2, sizeof(*psx->mc2));
    if (ok) ok = read_exact(file, psx->mc3, sizeof(*psx->mc3));
    if (ok) {
        psx_ic_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_cpu_t* cpu = psx->ic->cpu;
            *psx->ic = saved;
            psx->ic->cpu = cpu;
        }
    }
    if (ok) {
        psx_scratchpad_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            uint8_t* buf = psx->scratchpad->buf;
            *psx->scratchpad = saved;
            psx->scratchpad->buf = buf;
            ok = read_exact(file, psx->scratchpad->buf, PSX_SCRATCHPAD_SIZE);
        }
    }
    if (ok) {
        psx_gpu_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            uint16_t* vram = psx->gpu->vram;
            uint16_t* empty = psx->gpu->empty;
            psx_ic_t* ic = psx->gpu->ic;
            void* udata[4];
            psx_gpu_event_callback_t callbacks[8];
            memcpy(udata, psx->gpu->udata, sizeof(udata));
            memcpy(callbacks, psx->gpu->event_cb_table, sizeof(callbacks));
#ifdef USE_HARDWARE
            psx_gpu_renderer_t renderer = psx->gpu->renderer;
#endif
            *psx->gpu = saved;
            psx->gpu->vram = vram;
            psx->gpu->empty = empty;
            psx->gpu->ic = ic;
            memcpy(psx->gpu->udata, udata, sizeof(udata));
            memcpy(psx->gpu->event_cb_table, callbacks, sizeof(callbacks));
#ifdef USE_HARDWARE
            psx->gpu->renderer = renderer;
#endif
            ok = read_exact(file, psx->gpu->vram, PSX_GPU_VRAM_SIZE);
        }
    }
    if (ok) {
        psx_spu_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_ic_t* ic = psx->spu->ic;
            uint8_t* ram = psx->spu->ram;
            *psx->spu = saved;
            psx->spu->ic = ic;
            psx->spu->ram = ram;
            ok = read_exact(file, psx->spu->ram, SPU_RAM_SIZE);
        }
    }
    if (ok) {
        psx_bus_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_bios_t* bios = psx->bus->bios;
            psx_ram_t* ram = psx->bus->ram;
            psx_dma_t* dma = psx->bus->dma;
            psx_exp1_t* exp1 = psx->bus->exp1;
            psx_exp2_t* exp2 = psx->bus->exp2;
            psx_mc1_t* mc1 = psx->bus->mc1;
            psx_mc2_t* mc2 = psx->bus->mc2;
            psx_mc3_t* mc3 = psx->bus->mc3;
            psx_ic_t* ic = psx->bus->ic;
            psx_scratchpad_t* scratchpad = psx->bus->scratchpad;
            psx_gpu_t* gpu = psx->bus->gpu;
            psx_spu_t* spu = psx->bus->spu;
            psx_timer_t* timer = psx->bus->timer;
            psx_cdrom_t* cdrom = psx->bus->cdrom;
            psx_pad_t* pad = psx->bus->pad;
            psx_mdec_t* mdec = psx->bus->mdec;
            psx_bus_write_observer_t observer = psx->bus->write_observer;
            void* observer_udata = psx->bus->write_observer_udata;
            *psx->bus = saved;
            psx->bus->bios = bios; psx->bus->ram = ram; psx->bus->dma = dma;
            psx->bus->exp1 = exp1; psx->bus->exp2 = exp2; psx->bus->mc1 = mc1;
            psx->bus->mc2 = mc2; psx->bus->mc3 = mc3; psx->bus->ic = ic;
            psx->bus->scratchpad = scratchpad; psx->bus->gpu = gpu; psx->bus->spu = spu;
            psx->bus->timer = timer; psx->bus->cdrom = cdrom; psx->bus->pad = pad;
            psx->bus->mdec = mdec; psx->bus->write_observer = observer;
            psx->bus->write_observer_udata = observer_udata;
        }
    }
    if (ok) {
        psx_timer_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_ic_t* ic = psx->timer->ic;
            psx_gpu_t* gpu = psx->timer->gpu;
            *psx->timer = saved;
            psx->timer->ic = ic;
            psx->timer->gpu = gpu;
        }
    }
    if (ok) {
        psx_cdrom_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_disc_t* disc = psx->cdrom->disc;
            psx_ic_t* ic = psx->cdrom->ic;
            queue_t* data = psx->cdrom->data;
            queue_t* response = psx->cdrom->response;
            queue_t* parameters = psx->cdrom->parameters;
            *psx->cdrom = saved;
            psx->cdrom->disc = disc;
            psx->cdrom->ic = ic;
            psx->cdrom->data = data;
            psx->cdrom->response = response;
            psx->cdrom->parameters = parameters;
            ok = read_queue(file, data) && read_queue(file, response) && read_queue(file, parameters);
        }
    }
    if (ok) {
        psx_pad_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            psx_ic_t* ic = psx->pad->ic;
            psx_input_t* joy0 = psx->pad->joy_slot[0];
            psx_input_t* joy1 = psx->pad->joy_slot[1];
            psx_mcd_t* mcd0 = psx->pad->mcd_slot[0];
            psx_mcd_t* mcd1 = psx->pad->mcd_slot[1];
            *psx->pad = saved;
            psx->pad->ic = ic;
            psx->pad->joy_slot[0] = joy0; psx->pad->joy_slot[1] = joy1;
            psx->pad->mcd_slot[0] = mcd0; psx->pad->mcd_slot[1] = mcd1;
            ok = read_mcd(file, mcd0) && read_mcd(file, mcd1);
        }
    }
    if (ok) {
        uint8_t has_sda = 0;
        ok = read_exact(file, &has_sda, sizeof(has_sda));
        if (ok && has_sda) {
            if (!psx->pad->joy_slot[0] || !psx->pad->joy_slot[0]->udata)
                ok = 0;
            else
                ok = read_exact(file, psx->pad->joy_slot[0]->udata, sizeof(psxi_sda_t));
        }
    }
    if (ok) {
        psx_mdec_t saved;
        ok = read_exact(file, &saved, sizeof(saved));
        if (ok) {
            free(psx->mdec->input);
            free(psx->mdec->output);
            *psx->mdec = saved;
            psx->mdec->input = NULL;
            psx->mdec->output = NULL;
            if (header.mdec_input_bytes) {
                psx->mdec->input = malloc(header.mdec_input_bytes);
                if (!psx->mdec->input || !read_exact(file, psx->mdec->input, header.mdec_input_bytes))
                    ok = 0;
            }
            if (ok && header.mdec_output_bytes) {
                psx->mdec->output = malloc(header.mdec_output_bytes);
                if (!psx->mdec->output || !read_exact(file, psx->mdec->output, header.mdec_output_bytes))
                    ok = 0;
                else
                    psx->mdec->output_index = 0;
            }
        }
    }

#undef LOAD_PRESERVE_ONE
    fclose(file);
    if (ok)
        psx_cpu_invalidate_cache(psx->cpu);
    return ok;
}
