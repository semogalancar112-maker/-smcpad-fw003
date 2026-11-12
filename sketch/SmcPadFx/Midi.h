/*
 * Midi.h - Helper MIDI untuk SmcPadFx: SysEx text upload, MIDI clock (0xF8),
 * start/stop, dan tap tempo.
 */
#ifndef SMCPADFX_MIDI_H
#define SMCPADFX_MIDI_H

#include <Arduino.h>
#include <MIDIUSB.h>

// SysEx untuk upload teks running text ke SMC-PAD:
//   F0 00 12 03 01 <teks...> F7   (vendor id 00 12 03 = open-smc-pad, cmd 0x01 = teks marquee)
#define SYSEX_VENDOR0 0x00
#define SYSEX_VENDOR1 0x12
#define SYSEX_VENDOR2 0x03
#define SYSEX_CMD_TEXT 0x01

#define MAX_TEXT 128

struct MidiState {
    uint8_t sysex[MAX_TEXT];
    uint8_t sysex_len;
    bool in_sysex;
    uint32_t last_clock_ms;
    uint16_t ppqn;                /* ticks per quarternote (6 = MIDI clock) */
    uint16_t tick;                /* 0..5 */
    uint32_t beat_ms;             /* ms per beat saat ini */
    uint32_t last_tap_ms[4];
    uint8_t tap_idx;
    bool transport_running;
};

extern MidiState midi;

void midi_init(void);
void midi_poll(void);             /* panggil di loop() */
uint32_t midi_bpm(void);          /* BPM saat ini (0 = tidak ada tempo) */
bool midi_transport_running(void);

/* Tap tempo: panggil saat tombol TAP ditekan (mis. SHIFT+PLAY). */
void midi_tap(void);

#endif // SMCPADFX_MIDI_H
