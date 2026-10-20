/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Host test for gate-cue's frame logic. The SDK harness's stand-in core has no frame_filter,
 * so its soak never runs the filters; this test supplies them and drives synthetic ERP and
 * LED frames (the documented layouts, not captures from a deck).
 *
 *   make unit
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../gate-cue.c"

static int fails;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            fails++;                                                               \
        }                                                                          \
    } while (0)

static az_frame_fn f_erp, f_led;
static int row_on = 1, decks = 2, playing[2] = {1, 1};

static int s_register_screen(const char *id, const az_screen_v1 *s) { (void)id; return !s; }
static int s_register_row(const char *id, const az_row_v1 *r) { (void)id; (void)r; return 3; }
static int s_row_value(int h) { return h == 3 ? row_on : -1; }
static int s_frame_filter(const char *id, unsigned mask, az_frame_fn fn, void *u)
{
    (void)id;
    (void)u;
    if (mask == 1u << AZ_BUS_ERP_IN) f_erp = fn;
    if (mask == 1u << AZ_BUS_ERP_LED) f_led = fn;
    return 0;
}
static void s_log(const char *id, const char *fmt, ...) { (void)id; (void)fmt; }
static int s_deck_count(void) { return decks; }
static int s_deck_info(int d, az_deck_info *o)
{
    if (d < 0 || d > 3) return -1;
    o->playing = d < 2 ? playing[d] : 0;
    return 0;
}
static void s_redraw(void) {}

static unsigned char erp[30], led[128];
static unsigned pads[2], modebtn[2], playbtn[2];

/* one ERP frame as the panel sends it; afterwards erp[] is what the deck program sees */
static void frame(void)
{
    memset(erp, 0, sizeof erp);
    for (int d = 0; d < 2; d++) {
        erp[OFF_PADS(d)] = (unsigned char)pads[d];
        erp[OFF_MODE(d)] = (unsigned char)modebtn[d];
        if (playbtn[d]) erp[OFF_PLAY(d)] |= BIT_PLAY;
    }
    CHECK(f_erp(AZ_BUS_ERP_IN, erp, 28, NULL) == AZ_FRAME_OK);
}
static int lime = 1; /* the secondary (GATE CUE) page */
static void leds(int side, unsigned lit)
{
    for (int j = 0; j < 4; j++) {
        unsigned char *m = led + LED_MODE(side, j);
        m[0] = lime ? 0x07 : 0x30; /* lime 070c00, or an orange (R > G) */
        m[1] = lime ? 0x0c : 0x10;
        m[2] = 0;
    }
    for (int k = 0; k < 8; k++) {
        unsigned char *c = led + LED_PAD(side, k);
        c[0] = c[1] = c[2] = (lit >> k) & 1u ? 0x40 : 0;
    }
    CHECK(f_led(AZ_BUS_ERP_LED, led, 0x78, NULL) == AZ_FRAME_OK);
}
static int saw_play(int d) { return (erp[OFF_PLAY(d)] & BIT_PLAY) != 0; }
static unsigned saw_pads(int d) { return erp[OFF_PADS(d)]; }
static void hotcue_mode(int d) { modebtn[d] = MODE_HOTCUE; frame(); modebtn[d] = 0; frame(); }
static void idle(int n) { pads[0] = pads[1] = 0; for (int i = 0; i < n; i++) frame(); }

int main(void)
{
    static az_core_v1 c;
    memset(&c, 0, sizeof c);
    c.abi = AZ_MOD_ABI;
    c.size = sizeof c;
    c.register_screen = s_register_screen;
    c.register_row = s_register_row;
    c.row_value = s_row_value;
    c.frame_filter = s_frame_filter;
    c.log = s_log;
    c.deck_count = s_deck_count;
    c.deck_info = s_deck_info;
    c.request_redraw = s_redraw;
    CHECK(az_mod_v1_entry()->init(&c) == 0);
    CHECK(f_erp && f_led && erp_ok && led_ok && h_on == 3);
    tick();
    leds(0, 0x01); /* deck 1: cue A set, GATE CUE page */
    leds(1, 0x00);

    /* before any pad mode is chosen: native */
    pads[0] = 0x01; frame();
    CHECK(!saw_play(0) && saw_pads(0) == 0x01);
    idle(2);

    /* GATE CUE, playing, cue A set: PLAY, pad hidden, then the pad */
    hotcue_mode(0);
    pads[0] = 0x01;
    for (int i = 0; i < PRESS_FRAMES; i++) { frame(); CHECK(saw_play(0) && saw_pads(0) == 0); }
    for (int i = 0; i < GAP_FRAMES; i++) { frame(); CHECK(!saw_play(0) && saw_pads(0) == 0); }
    frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x01); /* held: the paused deck's gate */
    frame(); CHECK(saw_pads(0) == 0x01);
    idle(1); CHECK(saw_pads(0) == 0); /* released: the deck's own return to the cue */
    CHECK(__atomic_load_n(&n_gated, __ATOMIC_RELAXED) == 1);

    /* deck 2 is untouched by deck 1's gesture */
    pads[0] = 0x01; pads[1] = 0x04; frame();
    CHECK(saw_play(0) && !saw_play(1) && saw_pads(1) == 0x04);
    idle(10);

    /* an empty cue pad (B): native, it stores a cue */
    pads[0] = 0x02; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x02);
    idle(2);

    /* paused deck: native (the deck already gates) */
    playing[0] = 0; tick();
    pads[0] = 0x01; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x01);
    idle(2);
    playing[0] = 1; tick();

    /* two pads in one frame: native */
    leds(0, 0x03);
    pads[0] = 0x03; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x03);
    idle(2);

    /* 4-deck view: native */
    decks = 4; tick();
    pads[0] = 0x01; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x01);
    idle(2);
    decks = 2; tick();

    /* the row off: native */
    row_on = 0;
    pads[0] = 0x01; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x01);
    idle(2);
    row_on = 1;

    /* another pad mode chosen: native */
    modebtn[0] = 0x02; frame(); modebtn[0] = 0; frame();
    pads[0] = 0x01; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x01);
    idle(2);
    hotcue_mode(0);

    /* the HOT CUE page (mode buttons not lime): native, a pause would only add a gap */
    lime = 0; leds(0, 0x01);
    pads[0] = 0x01; frame(); CHECK(!saw_play(0) && saw_pads(0) == 0x01);
    idle(2);
    lime = 1; leds(0, 0x01);

    /* a tap shorter than the gesture: the pad is still shown, briefly */
    pads[0] = 0x01; frame(); CHECK(saw_play(0));
    pads[0] = 0x00;
    int shown = 0;
    for (int i = 0; i < PRESS_FRAMES + GAP_FRAMES + TAP_FRAMES + 2; i++) {
        frame();
        shown += saw_pads(0) == 0x01;
    }
    CHECK(shown == TAP_FRAMES);
    idle(2);

    if (fails) { fprintf(stderr, "gate_test: %d check(s) FAILED\n", fails); return 1; }
    printf("gate_test: all checks passed\n");
    return 0;
}
