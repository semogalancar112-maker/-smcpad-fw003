/*
 * SmcPadFx.ino - Custom LED effect / running text untuk SMC-PAD (open firmware).
 *
 * Efek: static, rainbow, chase, scanner, pulse, VU, sparkle, marquee (running text).
 * MIDI: clock 0xF8 (auto-tempo), start/stop, note on/off (VU), CC, SysEx upload teks.
 * EEPROM: settings (efek, warna, kecepatan, kecerahan, teks).
 *
 * Panggil SMCPad.update() di loop(); SMCPad.padColor() + SMCPad.show() untuk LED.
 */
#include <Arduino.h>
#include <EEPROM.h>
#include <SMCPad.h>
#include <MIDIUSB.h>

#include "Font5x7.h"
#include "Midi.h"

// --------------------------------------------------------------------------- konstanta
#define NUM_PADS 16
#define PAD_W 4
#define PAD_H 4

// EEPROM alamat
#define EEPROM_MAGIC_ADDR   0
#define EEPROM_MAGIC        0xA5
#define EEPROM_EFFECT_ADDR  1
#define EEPROM_COLOR_ADDR   2
#define EEPROM_SPEED_ADDR   6
#define EEPROM_BRIGHT_ADDR  10
#define EEPROM_TEXT_ADDR    14
#define EEPROM_TEXT_LEN     112

// Efek
enum Effect {
    FX_OFF = 0,
    FX_STATIC,
    FX_RAINBOW,
    FX_CHASE,
    FX_SCANNER,
    FX_PULSE,
    FX_VU,
    FX_SPARKLE,
    FX_MARQUEE,
    FX_COUNT
};

// --------------------------------------------------------------------------- state global
static Effect fx = FX_RAINBOW;
static uint32_t fx_color = 0x00FF00;   // format RRGGBB (padColor pakai r<<16|g<<8|b)
static uint16_t fx_speed = 100;        // ms per frame (kecil = cepat)
static uint8_t fx_bright = 255;
static char marquee_text[MAX_TEXT + 1] = "SMCPAD FX ";
static uint8_t marquee_len = 10;
static int16_t scroll_x = 0;           // posisi scroll marquee

static uint32_t last_frame_ms = 0;
static uint16_t frame = 0;
static uint8_t vu_level[NUM_PADS] = {0};

MidiState midi;                        /* definisi (dilihat via extern di Midi.h) */

// --------------------------------------------------------------------------- util warna
static uint8_t sin8_approx(uint8_t x)
{
    /* Aproksimasi sinus 0..255 dari segitiga. */
    uint8_t tri = (x < 128) ? (uint8_t)(x * 2) : (uint8_t)((255 - x) * 2);
    return tri;
}

static uint32_t hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    uint8_t region = h / 60;
    uint8_t remainder = h % 60;
    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 256);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * remainder / 60) / 256);
    uint8_t t = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * (255 - remainder) / 60) / 256);
    uint8_t r = 0, g = 0, b = 0;
    switch (region) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t dim_color(uint32_t c, uint8_t d)
{
    uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * d / 255);
    uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * d / 255);
    uint8_t b = (uint8_t)((c & 0xFF) * d / 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// --------------------------------------------------------------------------- EEPROM
static void eeprom_load(void)
{
    if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC) {
        return;
    }
    uint8_t e = EEPROM.read(EEPROM_EFFECT_ADDR);
    if (e < FX_COUNT) fx = (Effect)e;

    uint8_t r = EEPROM.read(EEPROM_COLOR_ADDR);
    uint8_t g = EEPROM.read(EEPROM_COLOR_ADDR + 1);
    uint8_t b = EEPROM.read(EEPROM_COLOR_ADDR + 2);
    fx_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    fx_speed = EEPROM.read(EEPROM_SPEED_ADDR) | (uint16_t)(EEPROM.read(EEPROM_SPEED_ADDR + 1) << 8);
    if (fx_speed == 0) fx_speed = 100;

    fx_bright = EEPROM.read(EEPROM_BRIGHT_ADDR);
    if (fx_bright == 0) fx_bright = 255;

    uint8_t len = EEPROM.read(EEPROM_TEXT_ADDR);
    if (len > MAX_TEXT) len = MAX_TEXT;
    marquee_len = len;
    for (uint8_t i = 0; i < len; i++) {
        marquee_text[i] = (char)EEPROM.read(EEPROM_TEXT_ADDR + 1 + i);
    }
    marquee_text[len] = '\0';
    if (marquee_len == 0) {
        const char def[] = "SMCPAD FX ";
        marquee_len = (uint8_t)(sizeof(def) - 1);
        memcpy(marquee_text, def, marquee_len);
        marquee_text[marquee_len] = '\0';
    }
}

static void eeprom_save(void)
{
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
    EEPROM.write(EEPROM_EFFECT_ADDR, (uint8_t)fx);
    EEPROM.write(EEPROM_COLOR_ADDR, (uint8_t)((fx_color >> 16) & 0xFF));
    EEPROM.write(EEPROM_COLOR_ADDR + 1, (uint8_t)((fx_color >> 8) & 0xFF));
    EEPROM.write(EEPROM_COLOR_ADDR + 2, (uint8_t)(fx_color & 0xFF));
    EEPROM.write(EEPROM_SPEED_ADDR, fx_speed & 0xFF);
    EEPROM.write(EEPROM_SPEED_ADDR + 1, fx_speed >> 8);
    EEPROM.write(EEPROM_BRIGHT_ADDR, fx_bright);
    EEPROM.write(EEPROM_TEXT_ADDR, marquee_len);
    for (uint8_t i = 0; i < marquee_len; i++) {
        EEPROM.write(EEPROM_TEXT_ADDR + 1 + i, marquee_text[i]);
    }
    EEPROM.commit();
}

// --------------------------------------------------------------------------- efek
static void fx_off(void)
{
    SMCPad.clearPads();
}

static void fx_static(void)
{
    for (int p = 0; p < NUM_PADS; p++) {
        SMCPad.padColor(p, fx_color);
    }
}

static void fx_rainbow(void)
{
    for (int p = 0; p < NUM_PADS; p++) {
        uint16_t h = (uint16_t)(((uint16_t)frame + (uint16_t)p * 40) % 384) * 60 / 16;
        SMCPad.padColor(p, hsv_to_rgb(h, 255, fx_bright));
    }
}

static void fx_chase(void)
{
    int pos = (frame / (fx_speed / 20 + 1)) % NUM_PADS;
    for (int p = 0; p < NUM_PADS; p++) {
        SMCPad.padColor(p, (p == pos) ? fx_color : 0);
    }
}

static void fx_scanner(void)
{
    int pos = (frame / (fx_speed / 20 + 1)) % (NUM_PADS * 2 - 2);
    if (pos >= NUM_PADS) pos = NUM_PADS * 2 - 2 - pos;
    for (int p = 0; p < NUM_PADS; p++) {
        SMCPad.padColor(p, (p == pos) ? fx_color : 0);
    }
}

static void fx_pulse(void)
{
    uint8_t bri = (uint8_t)(((uint16_t)sin8_approx((uint8_t)(frame * 2)) + 128) * fx_bright / 256);
    for (int p = 0; p < NUM_PADS; p++) {
        SMCPad.padColor(p, dim_color(fx_color, bri));
    }
}

static void fx_vu(void)
{
    for (int p = 0; p < NUM_PADS; p++) {
        if (vu_level[p] > 0) vu_level[p]--;
        SMCPad.padColor(p, dim_color(fx_color, vu_level[p] * fx_bright / 255));
    }
}

static void fx_sparkle(void)
{
    static int8_t last_sparkle = -1;
    if (last_sparkle >= 0) SMCPad.padColor(last_sparkle, 0);
    if ((frame % 4) == 0) {
        int p = random(NUM_PADS);
        SMCPad.padColor(p, hsv_to_rgb(random(384), 255, fx_bright));
        last_sparkle = p;
    } else {
        last_sparkle = -1;
    }
}

static void fx_marquee(void)
{
    int16_t text_width = marquee_len * (FONT_W + FONT_SPACING) - FONT_SPACING;
    int16_t display_w = PAD_W * 5;  // 4 pad * 5 kolom = 20 piksel

    for (int p = 0; p < NUM_PADS; p++) SMCPad.padColor(p, 0);

    for (int16_t cx = -FONT_W; cx < display_w; cx += (FONT_W + FONT_SPACING)) {
        int16_t char_x = cx + scroll_x;
        int char_idx = char_x / (FONT_W + FONT_SPACING);
        if (char_idx < 0 || char_idx >= marquee_len) continue;
        int16_t col_offset = char_x % (FONT_W + FONT_SPACING);
        if (col_offset < 0) col_offset += (FONT_W + FONT_SPACING);

        uint8_t ch = marquee_text[char_idx];
        if (ch < 32 || ch > 126) ch = '?';
        uint8_t ci = ch - 32;

        for (int col = 0; col < FONT_W; col++) {
            int16_t px = cx + col;
            if (px < 0 || px >= display_w) continue;
            uint8_t col_data = font5x7[ci][col];
            for (int row = 0; row < FONT_H; row++) {
                if (col_data & (1 << row)) {
                    int16_t pad_x = px / 5;
                    int16_t pad_y = row / 5;
                    if (pad_x >= 0 && pad_x < PAD_W && pad_y >= 0 && pad_y < PAD_H / 2) {
                        int pad = (1 - pad_y) * PAD_W + pad_x;  // 0 = kiri-bawah
                        SMCPad.padColor(pad, fx_color);
                    }
                }
            }
        }
    }

    scroll_x -= 1;
    if (scroll_x < -text_width) scroll_x = display_w;
}

// --------------------------------------------------------------------------- MIDI
void midi_init(void)
{
    midi.sysex_len = 0;
    midi.in_sysex = false;
    midi.last_clock_ms = 0;
    midi.ppqn = 6;
    midi.tick = 0;
    midi.beat_ms = 500;
    midi.tap_idx = 0;
    midi.transport_running = false;
    for (int i = 0; i < 4; i++) midi.last_tap_ms[i] = 0;
}

void midi_tap(void)
{
    uint32_t now = millis();
    midi.last_tap_ms[midi.tap_idx] = now;
    midi.tap_idx = (uint8_t)((midi.tap_idx + 1) % 4);
    uint32_t total = 0;
    uint8_t n = 0;
    for (int i = 0; i < 4; i++) {
        if (midi.last_tap_ms[i] != 0) {
            uint32_t diff = now - midi.last_tap_ms[i];
            if (diff > 0 && diff < 5000) { total += diff; n++; }
        }
    }
    if (n >= 2) {
        midi.beat_ms = total / n;
    }
}

uint32_t midi_bpm(void)
{
    if (midi.beat_ms == 0) return 0;
    return 60000UL / midi.beat_ms;
}

bool midi_transport_running(void)
{
    return midi.transport_running;
}

void midi_poll(void)
{
    while (MidiUSB.available()) {
        midiEventPacket_t ev = MidiUSB.read();
        uint8_t b1 = ev.byte1;
        uint8_t b2 = ev.byte2;
        uint8_t b3 = ev.byte3;

        if (b1 == 0xF8) {                     // MIDI Clock
            uint32_t now = millis();
            if (midi.last_clock_ms != 0) {
                uint32_t dt = now - midi.last_clock_ms;
                if (dt > 10 && dt < 1000) midi.beat_ms = dt * 6;
            }
            midi.last_clock_ms = now;
            midi.tick = (uint8_t)((midi.tick + 1) % 6);
        } else if (b1 == 0xFA) {              // Start
            midi.transport_running = true;
            midi.tick = 0;
            midi.last_clock_ms = millis();
        } else if (b1 == 0xFC) {              // Stop
            midi.transport_running = false;
        } else if (b1 == 0xFB) {              // Continue
            midi.transport_running = true;
        } else if (b1 == 0xF0) {              // SysEx begin
            midi.in_sysex = true;
            midi.sysex_len = 0;
        } else if (b1 == 0xF7) {              // SysEx end
            if (midi.in_sysex && midi.sysex_len >= 4 &&
                midi.sysex[0] == SYSEX_VENDOR0 &&
                midi.sysex[1] == SYSEX_VENDOR1 &&
                midi.sysex[2] == SYSEX_VENDOR2 &&
                midi.sysex[3] == SYSEX_CMD_TEXT) {
                uint8_t len = midi.sysex_len - 4;
                if (len > MAX_TEXT) len = MAX_TEXT;
                memcpy(marquee_text, midi.sysex + 4, len);
                marquee_len = len;
                marquee_text[len] = '\0';
                fx = FX_MARQUEE;
                eeprom_save();
            }
            midi.in_sysex = false;
            midi.sysex_len = 0;
        } else if (midi.in_sysex) {
            if (midi.sysex_len < MAX_TEXT) midi.sysex[midi.sysex_len++] = b1;
        } else {
            uint8_t status = b1 & 0xF0;
            if (status == 0x90 && b3 > 0) {   // Note On
                int pad = b2 % NUM_PADS;
                vu_level[pad] = (uint8_t)(b3 * 2);
                if (fx == FX_VU) SMCPad.padColor(pad, fx_color);
            } else if (status == 0x80 || (status == 0x90 && b3 == 0)) { // Note Off
                int pad = b2 % NUM_PADS;
                vu_level[pad] = 0;
            } else if (status == 0xB0) {      // CC
                if (b2 == 1) {                // Modulation -> brightness
                    fx_bright = b3;
                    eeprom_save();
                } else if (b2 == 7) {         // Volume -> speed
                    fx_speed = (uint16_t)map(b3, 0, 127, 300, 20);
                    if (fx_speed < 20) fx_speed = 20;
                }
            }
        }
    }
    MidiUSB.flush();
}

// --------------------------------------------------------------------------- callbacks hardware
static void on_pad_press(int pad, int velocity)
{
    if (SMCPad.button(SMCPad.SHIFT)) {
        fx = (Effect)((fx + 1) % FX_COUNT);
        eeprom_save();
    }
}

static void on_button_press(int index)
{
    switch ((SMCPad::Button)index) {
        case SMCPad::PLAY:
            midi_tap();
            break;
        default:
            break;
    }
}

static void on_knob(int knob, int delta)
{
    if (knob == 0) {
        fx_speed += (uint16_t)(delta * 5);
        if (fx_speed < 20) fx_speed = 20;
        if (fx_speed > 500) fx_speed = 500;
        eeprom_save();
    } else if (knob == 1) {
        fx_bright = (uint8_t)((int)fx_bright + delta * 10);
        if (fx_bright < 10) fx_bright = 10;
        if (fx_bright > 255) fx_bright = 255;
        eeprom_save();
    }
}

// --------------------------------------------------------------------------- setup / loop
void setup()
{
    SMCPad.begin();
    SMCPad.onPadPress(on_pad_press);
    SMCPad.onButtonPress(on_button_press);
    SMCPad.onKnob(on_knob);
    midi_init();
    eeprom_load();

    SMCPad.buttonLed(SMCPad.PLAY, true);
    randomSeed(12345);
}

void loop()
{
    SMCPad.update();
    midi_poll();

    uint32_t now = millis();
    if (now - last_frame_ms < fx_speed) return;
    last_frame_ms = now;

    frame++;

    switch (fx) {
        case FX_OFF:      fx_off(); break;
        case FX_STATIC:   fx_static(); break;
        case FX_RAINBOW:  fx_rainbow(); break;
        case FX_CHASE:    fx_chase(); break;
        case FX_SCANNER:  fx_scanner(); break;
        case FX_PULSE:    fx_pulse(); break;
        case FX_VU:       fx_vu(); break;
        case FX_SPARKLE:  fx_sparkle(); break;
        case FX_MARQUEE:  fx_marquee(); break;
        default:          fx_off(); break;
    }

    SMCPad.show();
}
