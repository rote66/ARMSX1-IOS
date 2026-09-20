/*
    This file is part of the PSXE Emulator Project

    Sony PlayStation Standard Digital/Analog Controller Emulator
*/

#include "sda.h"

#include <stdlib.h>
#include <string.h>

#define SDA_READY_LAST_MASK         0x000000ff
#define SDA_READY_CONFIG_START      0x00000100
#define SDA_READY_AUX_SHIFT         16
#define SDA_READY_AUX_MASK          0x00ff0000

static int sda_config_mode(const psxi_sda_t* sda) {
    return sda->model == SDA_MODEL_CONFIG;
}

static uint8_t sda_mode_model(const psxi_sda_t* sda) {
    return sda->sa_mode == SA_MODE_ANALOG ? SDA_MODEL_ANALOG_PAD : SDA_MODEL_DIGITAL;
}

static uint8_t sda_response_byte(const psxi_sda_t* sda, int state) {
    const uint8_t command = sda->tx_data;
    const int config_started = (sda->tx_data_ready & SDA_READY_CONFIG_START) != 0;
    const uint8_t aux = (uint8_t)((sda->tx_data_ready & SDA_READY_AUX_MASK) >> SDA_READY_AUX_SHIFT);

    if (state == SDA_STATE_TX_IDL) {
        if (command == 0x43 && config_started)
            return SDA_MODEL_CONFIG;
        if (command >= 0x44 && command <= 0x4d)
            return SDA_MODEL_CONFIG;
        return sda->model;
    }

    if (state == SDA_STATE_TX_IDH)
        return 0x5a;

    switch (command) {
        case 0x42:
            switch (state) {
                case SDA_STATE_TX_SWL: return sda->sw & 0xff;
                case SDA_STATE_TX_SWH: return sda->sw >> 8;
                case SDA_STATE_TX_ADC0: return sda->adc0;
                case SDA_STATE_TX_ADC1: return sda->adc1;
                case SDA_STATE_TX_ADC2: return sda->adc2;
                case SDA_STATE_TX_ADC3: return sda->adc3;
                default: return 0x00;
            }

        case 0x43:
            if (config_started)
                return 0x00;
            switch (state) {
                case SDA_STATE_TX_SWL: return sda->sw & 0xff;
                case SDA_STATE_TX_SWH: return sda->sw >> 8;
                case SDA_STATE_TX_ADC0: return sda->adc0;
                case SDA_STATE_TX_ADC1: return sda->adc1;
                case SDA_STATE_TX_ADC2: return sda->adc2;
                case SDA_STATE_TX_ADC3: return sda->adc3;
                default: return 0x00;
            }

        case 0x44:
            return 0x00;

        case 0x40:
            /* DualShock pressure initialization.  The game may issue this
             * during controller capability probing even when pressure data
             * is not enabled in the poll packet. */
            switch (state) {
                case SDA_STATE_TX_SWL: return 0x00;
                case SDA_STATE_TX_SWH: return 0x00;
                case SDA_STATE_TX_ADC0: return 0x02;
                case SDA_STATE_TX_ADC1: return 0x00;
                case SDA_STATE_TX_ADC2: return 0x00;
                case SDA_STATE_TX_ADC3: return 0x5a;
                default: return 0x00;
            }

        case 0x45:
            switch (state) {
                case SDA_STATE_TX_SWL: return 0x01;
                case SDA_STATE_TX_SWH: return 0x02;
                case SDA_STATE_TX_ADC0: return sda->sa_mode == SA_MODE_ANALOG ? 0x01 : 0x00;
                case SDA_STATE_TX_ADC1: return 0x02;
                case SDA_STATE_TX_ADC2: return 0x01;
                case SDA_STATE_TX_ADC3: return 0x00;
                default: return 0x00;
            }

        case 0x46:
            if (state == SDA_STATE_TX_ADC0) return 0x01;
            if (state == SDA_STATE_TX_ADC1) return aux == 0x01 ? 0x01 : 0x02;
            if (state == SDA_STATE_TX_ADC2) return aux == 0x01 ? 0x01 : 0x00;
            if (state == SDA_STATE_TX_ADC3) return aux == 0x01 ? 0x14 : 0x0a;
            return 0x00;

        case 0x47:
            switch (state) {
                case SDA_STATE_TX_ADC0: return 0x02;
                case SDA_STATE_TX_ADC1: return 0x00;
                case SDA_STATE_TX_ADC2: return 0x01;
                case SDA_STATE_TX_ADC3: return 0x00;
                default: return 0x00;
            }

        case 0x4c:
            if (state == SDA_STATE_TX_ADC1)
                return aux == 0x01 ? 0x07 : 0x04;
            return 0x00;

        case 0x4d:
            if (state >= SDA_STATE_TX_SWL && state <= SDA_STATE_TX_ADC2)
                return 0xff;
            return 0x00;

        case 0x4f:
            /* Set response format.  ARMSX currently exposes the standard
             * digital/analog packet only, so acknowledge the configuration
             * command without enabling pressure bytes. */
            return 0x00;

        default:
            return 0xff;
    }
}

psxi_sda_t* psxi_sda_create(void) {
    return (psxi_sda_t*)malloc(sizeof(psxi_sda_t));
}

void psxi_sda_init(psxi_sda_t* sda, uint16_t model) {
    memset(sda, 0, sizeof(psxi_sda_t));

    sda->tx_data = 0xff;
    sda->tx_data_ready = 1;
    sda->prev_model = model;
    sda->model = model;
    sda->state = SDA_STATE_TX_HIZ;
    sda->sw = 0xffff;
    sda->sa_mode = SA_MODE_DIGITAL;
    sda->adc0 = 0x80;
    sda->adc1 = 0x80;
    sda->adc2 = 0x80;
    sda->adc3 = 0x80;
}

uint32_t psxi_sda_read(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    if (sda->state == SDA_STATE_TX_HIZ) {
        sda->tx_data_ready = 1;
        sda->state = SDA_STATE_TX_IDL;
        return 0xff;
    }

    const int last_state = sda->tx_data_ready & SDA_READY_LAST_MASK;
    const uint8_t response = sda_response_byte(sda, sda->state);

    if (last_state != 0 && sda->state >= last_state) {
        sda->state = SDA_STATE_TX_HIZ;
        sda->tx_data = 0;
        sda->tx_data_ready = 0;
    } else {
        sda->state++;
    }

    return response;
}

void psxi_sda_write(void* udata, uint16_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    if (sda->state == SDA_STATE_TX_IDL) {
        const uint8_t command = (uint8_t)data;
        const int config = sda_config_mode(sda);
        int last_state = (config || sda->sa_mode == SA_MODE_ANALOG) ? SDA_STATE_TX_ADC3 : SDA_STATE_TX_SWH;

        if (command == 0x40 || (command >= 0x44 && command <= 0x4f)) {
            if (!config || (command != 0x40 && command != 0x44 && command != 0x45 &&
                            command != 0x46 && command != 0x47 && command != 0x4c &&
                            command != 0x4d && command != 0x4f)) {
                sda->tx_data_ready = 0;
                sda->state = SDA_STATE_TX_HIZ;
                return;
            }
            last_state = SDA_STATE_TX_ADC3;
        } else if (command != 0x42 && command != 0x43) {
            sda->tx_data_ready = 0;
            sda->state = SDA_STATE_TX_HIZ;
            return;
        }

        sda->tx_data = command;
        sda->tx_data_ready = last_state | (config ? SDA_READY_CONFIG_START : 0);
        return;
    }

    const uint8_t command = sda->tx_data;

    if (sda->state == SDA_STATE_TX_SWL) {
        if (command == 0x43) {
            if (data == 0x01 && !sda_config_mode(sda)) {
                sda->prev_model = sda_mode_model(sda);
                sda->model = SDA_MODEL_CONFIG;
            } else if (data == 0x00 && sda_config_mode(sda)) {
                sda->model = sda_mode_model(sda);
                sda->prev_model = sda->model;
            }
        } else if (command == 0x44 && sda_config_mode(sda)) {
            if (data == 0x00 || data == 0x01) {
                sda->sa_mode = data == 0x01 ? SA_MODE_ANALOG : SA_MODE_DIGITAL;
                sda->prev_model = sda_mode_model(sda);
            }
        } else if (command == 0x46 || command == 0x4c) {
            sda->tx_data_ready =
                (sda->tx_data_ready & ~SDA_READY_AUX_MASK) |
                (((int)data & 0xff) << SDA_READY_AUX_SHIFT);
        }
    }
}

void psxi_sda_on_button_press(void* udata, uint32_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    if (data == PSXI_SW_SDA_ANALOG) {
        sda->sa_mode ^= 1;

        sda->prev_model = sda_mode_model(sda);
        if (!sda_config_mode(sda))
            sda->model = sda->prev_model;

        return;
    }

    sda->sw &= ~data;
}

void psxi_sda_on_button_release(void* udata, uint32_t data) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    sda->sw |= data;
}

// To-do: Implement analog mode
void psxi_sda_on_analog_change(void* udata, uint32_t axis, uint16_t data) {
    // Suppress warning until we implement analog mode
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    switch (axis) {
        case PSXI_AX_SDA_RIGHT_HORZ: sda->adc0 = data; break;
        case PSXI_AX_SDA_RIGHT_VERT: sda->adc1 = data; break;
        case PSXI_AX_SDA_LEFT_HORZ: sda->adc2 = data; break;
        case PSXI_AX_SDA_LEFT_VERT: sda->adc3 = data; break;
    }
}

int psxi_sda_query_fifo(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    return sda->tx_data_ready;
}

int psxi_sda_query_ack(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;
    const int last_state = sda->tx_data_ready & SDA_READY_LAST_MASK;

    /* /ACK is a request for the host to clock another byte.  The controller
     * must not acknowledge the host byte paired with its final response byte.
     * Keep FIFO readiness separate: the final response still has to be read. */
    return sda->tx_data_ready && last_state != 0 && sda->state < last_state;
}

void psxi_sda_reset_transfer(void* udata) {
    psxi_sda_t* sda = (psxi_sda_t*)udata;

    sda->state = SDA_STATE_TX_HIZ;
    sda->tx_data = 0xff;
    sda->tx_data_ready = 1;
}

void psxi_sda_init_input(psxi_sda_t* sda, psx_input_t* input) {
    input->udata = sda;
    input->write_func = psxi_sda_write;
    input->read_func = psxi_sda_read;
    input->on_button_press_func = psxi_sda_on_button_press;
    input->on_button_release_func = psxi_sda_on_button_release;
    input->on_analog_change_func = psxi_sda_on_analog_change;
    input->query_fifo_func = psxi_sda_query_fifo;
    input->query_ack_func = psxi_sda_query_ack;
    input->reset_transfer_func = psxi_sda_reset_transfer;
}

void psxi_sda_destroy(psxi_sda_t* sda) {
    free(sda);
}