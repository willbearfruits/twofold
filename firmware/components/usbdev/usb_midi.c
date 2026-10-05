/*
 * usb_midi.c: USB-MIDI 1.0 (one cable, one jack each way). Called from the ctrl task only (core 0).
 * Incoming Control Change on the configured channel (P_MIDI_CH, 0 = omni) for a mapped CC number sets the
 * parameter with SRC_MIDI, so the outgoing path (ctrl task) does not echo it back.
 */
#include "bootdiag.h"
#include "usbdev.h"
#include "tusb.h"
#include "params.h"

bool usb_midi_mounted(void) { return tud_mounted() && tud_midi_mounted(); }

int usb_midi_poll_rx(void)
{
    if (!usb_midi_mounted()) return 0;
    int applied = 0;
    uint8_t pkt[4];
    /* Bounded so a flood cannot starve the ctrl task. 64 packets per 2 ms = 32k CC/s. */
    for (int n = 0; n < 64 && tud_midi_available(); n++) {
        if (!tud_midi_packet_read(pkt)) break;
        uint8_t const cin = pkt[0] & 0x0F;          /* code index number */
        if (cin == 0x04 && pkt[1] == 0xF0 && pkt[2] == 0x7D && pkt[3] == 0x01) {
            /* SysEx F0 7D 01 ... : diagnostics query -> reply F0 7D 02 <ascii crash report> F7 */
            static uint8_t rep[200];
            rep[0] = 0xF0; rep[1] = 0x7D; rep[2] = 0x02;
            size_t k = bootdiag_report((char *)rep + 3, sizeof rep - 5);
            for (size_t i = 0; i < k; i++) rep[3 + i] &= 0x7F;
            rep[3 + k] = 0xF7;
            tud_midi_stream_write(0, rep, (uint32_t)(k + 4));
            continue;
        }
        if (cin != 0x0B) continue;                  /* only Control Change */
        uint8_t const status = pkt[1];
        if ((status & 0xF0) != 0xB0) continue;
        int const want = params_get_int(P_MIDI_CH); /* 0 = omni, 1..16 */
        if (want != 0 && (status & 0x0F) != (uint8_t)(want - 1)) continue;
        uint8_t const cc = pkt[2] & 0x7F, val = pkt[3] & 0x7F;
        param_id_t const id = params_param_of_cc(cc);
        if (id >= P_COUNT) continue;
        params_set_norm(id, (float)val / 127.f, SRC_MIDI);
        applied++;
    }
    return applied;
}

bool usb_midi_send_cc(uint8_t ch, uint8_t cc, uint8_t val)
{
    if (!usb_midi_mounted()) return false;
    uint8_t const pkt[4] = { 0x0B /* cable 0, CIN CC */, (uint8_t)(0xB0 | (ch & 0x0F)), (uint8_t)(cc & 0x7F),
                             (uint8_t)(val & 0x7F) };
    return tud_midi_packet_write(pkt);
}
