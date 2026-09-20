#ifndef PAD_H
#define PAD_H

#include "input.h"

#include <string.h>
#include <stdlib.h>

psx_input_t* psx_input_create(void) {
    return (psx_input_t*)malloc(sizeof(psx_input_t));
}

void psx_input_init(psx_input_t* input) {
    memset(input, 0, sizeof(psx_input_t));
}

void psx_input_set_write_func(psx_input_t* input, psx_input_write_t write_func) {
    input->write_func = write_func;
}

void psx_input_set_read_func(psx_input_t* input, psx_input_read_t read_func) {
    input->read_func = read_func;
}

void psx_input_set_on_button_press_func(psx_input_t* input, psx_input_on_button_press_t on_button_press_func) {
    input->on_button_press_func = on_button_press_func;
}

void psx_input_set_on_button_release_func(psx_input_t* input, psx_input_on_button_release_t on_button_release_func) {
    input->on_button_release_func = on_button_release_func;
}

void psx_input_set_on_analog_change_func(psx_input_t* input, psx_input_on_analog_change_t on_analog_change_func) {
    input->on_analog_change_func = on_analog_change_func;
}

void psx_input_set_query_fifo_func(psx_input_t* input, psx_input_query_fifo_t query_fifo_func) {
    input->query_fifo_func = query_fifo_func;
}

void psx_input_set_query_ack_func(psx_input_t* input, psx_input_query_ack_t query_ack_func) {
    input->query_ack_func = query_ack_func;
}

void psx_input_set_reset_transfer_func(psx_input_t* input, psx_input_reset_transfer_t reset_transfer_func) {
    input->reset_transfer_func = reset_transfer_func;
}

void psx_input_destroy(psx_input_t* input) {
    free(input->udata);
    free(input);
}

#endif