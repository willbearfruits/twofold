/*
 * usb_midi.c: USB-MIDI 1.0 (one cable, one jack each way). Called from the ctrl task only (core 0).
 * Incoming Control Change on the configured channel (P_MIDI_CH, 0 = omni) for a mapped CC number sets the
 * parameter with SRC_MIDI, so the outgoing path (ctrl task) does not echo it back.
 */
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
