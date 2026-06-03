/*
 * SPDX-FileCopyrightText: 2025 xlongfeng
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * GREE YAPOF20 / YAPOF3 IR transmitter using ESP-IDF RMT.
 *
 * Each call to gree_ir_send() transmits TWO consecutive frames.
 * Both frames share the same wire format (70 RMT symbols, LSB-first):
 *
 *   [HEADER]   9000 µs mark + 4500 µs space
 *   [BLOCK 1]  32 bits  (bytes 0–3)
 *   [FOOTER]    3 bits  = 0b010
 *   [GAP]       620 µs mark + 19980 µs space
 *   [BLOCK 2]  32 bits  (bytes 4–7)
 *   [EOF]       620 µs mark + 0 µs  (RMT idles after this mark)
 *
 * Checksum (bits[7:4] of byte[7]):
 *   sum = 10 + low_nibble(b0..b3) + high_nibble(b4..b6),  result & 0x0F
 */

#include "gree_ir.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "gree_ir";

/* Timing in µs (RMT resolution = 1 µs) */
#define IR_RESOLUTION_HZ 1000000U
#define GREE_HDR_MARK_US 9000
#define GREE_HDR_SPACE_US 4500
#define GREE_BIT_MARK_US 620
#define GREE_ONE_SPACE_US 1600
#define GREE_ZERO_SPACE_US 540
#define GREE_MSG_SPACE_US 19980

#define GREE_BLOCK_FOOTER 0x02 /* 0b010 – 3 inter-block separator bits */
#define GREE_RMT_SYMBOLS 70    /* symbols per frame */

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_copy_enc = NULL;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static uint8_t gree_checksum(const uint8_t b[8])
{
    uint8_t sum = 10;
    for (int i = 0; i < 4; i++)
        sum += (b[i] & 0x0F); /* low nibbles  b0–b3 */
    for (int i = 4; i < 7; i++)
        sum += (b[i] >> 4); /* high nibbles b4–b6 */
    return sum & 0x0F;
}

/**
 * Build the 8-byte frame 1 (byte[3] upper nibble = 0x5).
 */
static void build_frame1(const gree_ac_state_t *s, uint8_t b[8])
{
    memset(b, 0, 8);

    uint8_t fan = s->fan;
    if (s->turbo)
        fan = GREE_FAN_LVL5;
    if (s->quiet)
        fan = GREE_FAN_LVL1;

    uint8_t basic_fan = (fan > 3) ? 3 : fan; /* BasicFan is 2-bit, cap at Lvl3 */

    uint8_t temp = s->temperature;
    if (temp < 16)
        temp = 16;
    if (temp > 30)
        temp = 30;

    /* Byte 0: Mode[2:0] | Power[3] | BasicFan[5:4] | SwingAuto[6] | Sleep[7] */
    b[0] = (s->mode & 0x07) | (s->power ? (1 << 3) : 0) | ((basic_fan & 0x03) << 4) | (s->swing_auto ? (1 << 6) : 0) |
           (s->sleep ? (1 << 7) : 0);

    /* Byte 1: Temp[3:0] (offset 16) */
    b[1] = (temp - 16) & 0x0F;

    /* Byte 2: Turbo[4] | Light[5] | XFan[7] */
    b[2] = (s->turbo ? (1 << 4) : 0) | (s->light ? (1 << 5) : 0) | (s->xfan ? (1 << 7) : 0);

    /* Byte 3: fixed frame-1 identifier 0x5 in upper nibble */
    b[3] = 0x50;

    /* Byte 4: SwingV[3:0] | SwingH[6:4] */
    b[4] = (s->swing_v & 0x0F) | ((s->swing_h & 0x07) << 4);

    /* Byte 5: IFeel[2] | WiFi[6] */
    b[5] = (s->ifeel ? (1 << 2) : 0) | (s->wifi ? (1 << 6) : 0);

    /* Byte 6: reserved */
    b[6] = 0x00;

    /* Byte 7: Econo[2] | Checksum[7:4] */
    b[7] = (s->econo ? (1 << 2) : 0);
    b[7] |= (gree_checksum(b) << 4);
}

/**
 * Build the 8-byte frame 2 (byte[3] upper nibble = 0x7).
 * Bytes 0–2 repeat frame 1.
 */
static void build_frame2(const gree_ac_state_t *s, const uint8_t f1[8], uint8_t b[8])
{
    memset(b, 0, 8);

    uint8_t fan = s->fan;
    if (s->turbo)
        fan = GREE_FAN_LVL5;
    if (s->quiet)
        fan = GREE_FAN_LVL1;

    /* Bytes 0–2: identical to frame 1 */
    b[0] = f1[0];
    b[1] = f1[1];
    b[2] = f1[2];

    /* Byte 3: fixed = 0x70 (bits[7:6]=0b01, bits[5:4]=0b11) */
    b[3] = 0x70;

    /* Byte 4: Quiet[7] */
    b[4] = (s->quiet ? (1 << 7) : 0);

    /* Byte 5: reserved */
    b[5] = 0x00;

    /* Byte 6: Fan[6:4] (full 3-bit range 0–5) | bit[7] always set */
    b[6] = ((fan & 0x07) << 4) | (1 << 7);

    /* Byte 7: CoolingSensation[3] | Checksum[7:4] */
    b[7] = (s->cooling_sensation ? (1 << 3) : 0);
    b[7] |= (gree_checksum(b) << 4);
}

/**
 * Encode 8 bytes into 70 RMT symbols.
 * Layout: header(1) + block1(32) + footer(3) + gap(1) + block2(32) + eof(1)
 */
static void gree_encode(const uint8_t b[8], rmt_symbol_word_t syms[GREE_RMT_SYMBOLS])
{
    int n = 0;

#define SYM(d0, l0, d1, l1)                                                                                            \
    syms[n++] = (rmt_symbol_word_t){.duration0 = (d0), .level0 = (l0), .duration1 = (d1), .level1 = (l1)}

    /* Header */
    SYM(GREE_HDR_MARK_US, 1, GREE_HDR_SPACE_US, 0);

    /* Block 1: bytes 0–3, LSB first */
    for (int i = 0; i < 32; i++) {
        bool v = (b[i / 8] >> (i % 8)) & 1;
        SYM(GREE_BIT_MARK_US, 1, v ? GREE_ONE_SPACE_US : GREE_ZERO_SPACE_US, 0);
    }

    /* Footer: 3 bits = 0b010, LSB first (bit0=0, bit1=1, bit2=0) */
    for (int i = 0; i < 3; i++) {
        bool v = (GREE_BLOCK_FOOTER >> i) & 1;
        SYM(GREE_BIT_MARK_US, 1, v ? GREE_ONE_SPACE_US : GREE_ZERO_SPACE_US, 0);
    }

    /* Inter-block gap */
    SYM(GREE_BIT_MARK_US, 1, GREE_MSG_SPACE_US, 0);

    /* Block 2: bytes 4–7, LSB first */
    for (int i = 0; i < 32; i++) {
        bool v = (b[4 + i / 8] >> (i % 8)) & 1;
        SYM(GREE_BIT_MARK_US, 1, v ? GREE_ONE_SPACE_US : GREE_ZERO_SPACE_US, 0);
    }

    /* EOF mark (duration1=0 → RMT goes idle) */
    SYM(GREE_BIT_MARK_US, 1, 0, 0);

#undef SYM
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t gree_ir_init(int gpio_num)
{
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio_num,
        .mem_block_symbols = 48, /* one HW block; driver ping-pongs for >48 symbols */
        .resolution_hz = IR_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_tx_chan), TAG, "create TX channel failed");

    rmt_carrier_config_t carrier = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33f,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx_chan, &carrier), TAG, "apply carrier failed");

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&enc_cfg, &s_copy_enc), TAG, "create copy encoder failed");

    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_chan), TAG, "enable TX channel failed");

    ESP_LOGI(TAG, "IR TX initialized on GPIO%d", gpio_num);
    return ESP_OK;
}

esp_err_t gree_ir_send(const gree_ac_state_t *state)
{
    uint8_t f1[8], f2[8];
    build_frame1(state, f1);
    build_frame2(state, f1, f2);

    ESP_LOGI(TAG, "send: pwr=%d mode=%d temp=%d°C fan=%d swing_v=%d swing_h=%d", state->power, state->mode,
             state->temperature, state->fan, state->swing_v, state->swing_h);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, f1, 8, ESP_LOG_DEBUG);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, f2, 8, ESP_LOG_DEBUG);

    rmt_symbol_word_t syms[GREE_RMT_SYMBOLS];
    rmt_transmit_config_t tx_cfg = {.loop_count = 0};

    /* Frame 1 */
    gree_encode(f1, syms);
    ESP_RETURN_ON_ERROR(rmt_transmit(s_tx_chan, s_copy_enc, syms, sizeof(syms), &tx_cfg), TAG,
                        "transmit frame 1 failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_tx_chan, portMAX_DELAY), TAG, "wait frame 1 failed");

    /* ~40 ms gap between frames */
    vTaskDelay(pdMS_TO_TICKS(40));

    /* Frame 2 */
    gree_encode(f2, syms);
    ESP_RETURN_ON_ERROR(rmt_transmit(s_tx_chan, s_copy_enc, syms, sizeof(syms), &tx_cfg), TAG,
                        "transmit frame 2 failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_tx_chan, portMAX_DELAY), TAG, "wait frame 2 failed");

    return ESP_OK;
}
