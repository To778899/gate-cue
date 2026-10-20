/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Gate Cue: on the GATE CUE pad page (HOT CUE's second page), a cue plays only while its pad
 * is held, also on a deck that is playing. Release the pad and the deck goes back to the cue
 * and waits there, paused.
 *
 * Why: measured on the XDJ-AZ emulator (firmware 2.00 deck program), GATE CUE gates only a
 * paused deck: held, the cue plays; let go, it stops. On a playing deck the same pad jumps to
 * the cue and the deck plays on after the release. So when a set cue is pressed on a playing
 * deck, this mod first sends one PLAY press (the deck pauses where it is), holds the pad back
 * for a few frames, then lets the pad through, and the paused deck's own gate does the rest.
 * The HOT CUE page is left alone: there a paused deck starts playing from the cue and keeps
 * playing, so a pause would only add a gap. Nothing else is changed: no audio, no
 * hook, no EP147 code. Only services from the SDK: frame filters, a Mod Menu row, deck state.
 *
 * Unknown means do nothing: an empty cue pad (it would store a new cue), two pads at once, a
 * pad mode this mod has not seen chosen since start, a page that is not the secondary one, the
 * 4-deck view (a side may drive deck 3 or 4), a deck whose play state is unknown. Each passes
 * through untouched.
 *
 * Frame layouts (all measured by others, none guessed here):
 *   ERP input (AZ_BUS_ERP_IN), side d = 0/1, +8 per side: byte 10 bit 0x02 PLAY, byte 13 the
 *     pad-mode buttons (HOT CUE 0x01, BEAT LOOP 0x02, SLIP LOOP 0x04, BEAT JUMP 0x08, momentary),
 *     byte 14 pads 1-8 (bit k = pad k+1). Source: the XDJ-AZ emulator's panel map
 *     (tools/az-drive.py) and the AZ-MODS core (bus_interpose.c, erp_side).
 *   ERP LED frame (AZ_BUS_ERP_LED): pad k of side d at 0x1e + 4k + 0x38d, mode button j at
 *     0x0e + 4j + 0x38d, {R, G, B, -}. On a secondary pad page all four mode buttons are lime
 *     (G the largest, B 0, 0 < R < G). Source: SDK docs/reference-frames.md, and the AZ-MODS
 *     core's lime test (bus_interpose.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "azmod-mod.h"

#define MOD_ID "to778899-gate-cue"

#define ERP_MIN_LEN 23 /* the highest byte touched is side 2's pads, 14 + 8 */
#define OFF_PLAY(d) (10 + 8 * (d))
#define BIT_PLAY 0x02u
#define OFF_MODE(d) (13 + 8 * (d))
#define MODE_HOTCUE 0x01u
#define OFF_PADS(d) (14 + 8 * (d))
#define LED_PAD(d, k) (0x1e + 4 * (k) + 0x38 * (d))
#define LED_MODE(d, j) (0x0e + 4 * (j) + 0x38 * (d))
#define LED_MIN_LEN 0x78

/* Frames of the injected gesture: PLAY held, then PLAY up with the pad still held back, then
 * (for a tap released during those) the pad shown for a few frames. */
#define PRESS_FRAMES 2
#define GAP_FRAMES 1
#define TAP_FRAMES 3

static const az_core_v1 *C;
static int h_on = -1;      /* our Gate Cue row */
static int erp_ok, led_ok; /* filters registered */

/* ---- shared between threads: one word each, atomics ----------------------------------- */
static int deck_playing[2];        /* tick -> ERP thread: 1 playing, 0 not, -1 unknown */
static int two_deck;               /* tick -> ERP thread: 1 = 2-deck view (side d drives deck d) */
static unsigned cue_lit[2];        /* LED thread -> ERP thread and page: bit k = pad k lit */
static int page2[2];               /* LED thread -> ERP thread and page: secondary pad page */
static unsigned n_gated, n_passed; /* ERP thread -> page */
static int side_mode[2];           /* ERP thread -> page: 0 unknown, 1 HOT CUE, 2 another mode */
static int dirty = 1;

/* ---- ERP thread only -------------------------------------------------------------------- */
enum { IDLE, PRESS, GAP, HOLD, TAP };
static struct side {
    unsigned prev_pads, prev_mode;
    int phase, pad, frames;
} S[2];

static void bump(unsigned *n)
{
    __atomic_add_fetch(n, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&dirty, 1, __ATOMIC_RELAXED);
}

/* One side of an ERP input frame. Edits the frame in place; the core recomputes the CRC. */
static void erp_side(unsigned char *f, int d, int on, int two)
{
    struct side *s = &S[d];
    const unsigned mode = f[OFF_MODE(d)], mrise = mode & ~s->prev_mode;
    s->prev_mode = mode;
    if (mrise) {
        int m = (mrise & MODE_HOTCUE) ? 1 : 2;
        if (m != __atomic_load_n(&side_mode[d], __ATOMIC_RELAXED)) {
            __atomic_store_n(&side_mode[d], m, __ATOMIC_RELAXED);
            __atomic_store_n(&dirty, 1, __ATOMIC_RELAXED);
        }
    }
    const unsigned pads = f[OFF_PADS(d)], rise = pads & ~s->prev_pads;
    s->prev_pads = pads;

    if (s->phase == IDLE) {
        if (!rise)
            return;
        if (!on || !two || __atomic_load_n(&side_mode[d], __ATOMIC_RELAXED) != 1 ||
            !__atomic_load_n(&page2[d], __ATOMIC_RELAXED) || (rise & (rise - 1u)))
            return; /* off, not GATE CUE, unknown or two pads at once: native */
        int k = 0;
        while (!(rise & (1u << k)))
            k++;
        if (__atomic_load_n(&deck_playing[d], __ATOMIC_RELAXED) != 1 ||
            !(__atomic_load_n(&cue_lit[d], __ATOMIC_RELAXED) & (1u << k))) {
            bump(&n_passed); /* paused already (native gate), or no cue on that pad */
            return;
        }
        s->phase = PRESS;
        s->pad = k;
        s->frames = 0;
        bump(&n_gated);
    }
    const unsigned bit = 1u << s->pad;
    switch (s->phase) {
    case PRESS: /* PLAY down: the deck pauses where it is; the pad waits */
        f[OFF_PLAY(d)] |= BIT_PLAY;
        f[OFF_PADS(d)] &= (unsigned char)~bit;
        if (++s->frames >= PRESS_FRAMES) {
            s->phase = GAP;
            s->frames = 0;
        }
        break;
    case GAP: /* PLAY up, so the press ends; the pad still waits */
        f[OFF_PLAY(d)] &= (unsigned char)~BIT_PLAY;
        f[OFF_PADS(d)] &= (unsigned char)~bit;
        if (++s->frames >= GAP_FRAMES) {
            s->phase = (pads & bit) ? HOLD : TAP;
            s->frames = 0;
        }
        break;
    case HOLD: /* the pad passes as it is; its release is the deck's own return to the cue */
        if (!(pads & bit))
            s->phase = IDLE;
        break;
    case TAP: /* released during the gesture: show the press now, briefly, then let go */
        f[OFF_PADS(d)] |= (unsigned char)bit;
        if (++s->frames >= TAP_FRAMES)
            s->phase = IDLE;
        break;
    }
}

static int erp_filter(int bus, unsigned char *frame, int len, void *user)
{
    (void)bus;
    (void)user;
    if (!frame || len < ERP_MIN_LEN)
        return AZ_FRAME_OK;
    const int on = h_on >= 0 && C->row_value(h_on) == 1;
    const int two = __atomic_load_n(&two_deck, __ATOMIC_RELAXED);
    for (int d = 0; d < 2; d++)
        erp_side(frame, d, on, two);
    return AZ_FRAME_OK;
}

/* Read only: which hot cue pads the deck program lights. */
static int led_filter(int bus, unsigned char *frame, int len, void *user)
{
    (void)bus;
    (void)user;
    if (!frame || len < LED_MIN_LEN)
        return AZ_FRAME_OK;
    for (int d = 0; d < 2; d++) {
        unsigned lit = 0;
        for (int k = 0; k < 8; k++) {
            const unsigned char *c = frame + LED_PAD(d, k);
            if (c[0] | c[1] | c[2])
                lit |= 1u << k;
        }
        if (__atomic_exchange_n(&cue_lit[d], lit, __ATOMIC_RELAXED) != lit)
            __atomic_store_n(&dirty, 1, __ATOMIC_RELAXED);
        int lime = 1; /* all four mode buttons lime: the secondary page */
        for (int j = 0; j < 4; j++) {
            const unsigned char *m = frame + LED_MODE(d, j);
            if (!(m[1] >= m[0] && m[2] == 0 && m[0] > 0 && m[0] < m[1]))
                lime = 0;
        }
        if (__atomic_exchange_n(&page2[d], lime, __ATOMIC_RELAXED) != lime)
            __atomic_store_n(&dirty, 1, __ATOMIC_RELAXED);
    }
    return AZ_FRAME_OK;
}

/* ---- UI thread ---------------------------------------------------------------------------- */

/* The Mod Menu row's icon, drawn once in init: a play triangle up to a bar ("plays until you
 * let go"), as a coverage mask with 4x4 supersampling. The core copies it. */
static unsigned char row_icon[AZ_ROW_ICON_N * AZ_ROW_ICON_N];
static int icon_ink(float x, float y)
{
    const float s = AZ_ROW_ICON_N / 96.0f; /* designed on a 96 grid */
    x /= s;
    y /= s;
    if (x >= 66.f && x <= 78.f && y >= 22.f && y <= 74.f)
        return 1; /* the gate bar */
    if (x < 18.f || x > 58.f)
        return 0;
    const float half = 26.f * (1.f - (x - 18.f) / 40.f); /* the triangle, apex at x 58 */
    return y >= 48.f - half && y <= 48.f + half;
}
static void make_row_icon(void)
{
    for (int y = 0; y < AZ_ROW_ICON_N; y++)
        for (int x = 0; x < AZ_ROW_ICON_N; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    hits += icon_ink(x + (sx + 0.5f) / 4.f, y + (sy + 0.5f) / 4.f);
            row_icon[y * AZ_ROW_ICON_N + x] = (unsigned char)(hits * 255 / 16);
        }
}

static void tick(void)
{
    int two = AZ_HAS(C, deck_count) && C->deck_count() == 2;
    if (__atomic_exchange_n(&two_deck, two, __ATOMIC_RELAXED) != two)
        __atomic_store_n(&dirty, 1, __ATOMIC_RELAXED);
    for (int d = 0; d < 2; d++) {
        int p = -1;
        if (AZ_HAS(C, deck_info)) {
            az_deck_info di;
            memset(&di, 0, sizeof di);
            di.size = sizeof di;
            if (C->deck_info(d, &di) == 0)
                p = di.playing;
        }
        if (__atomic_exchange_n(&deck_playing[d], p, __ATOMIC_RELAXED) != p)
            __atomic_store_n(&dirty, 1, __ATOMIC_RELAXED);
    }
    if (AZ_HAS(C, request_redraw) && __atomic_exchange_n(&dirty, 0, __ATOMIC_RELAXED))
        C->request_redraw();
}

/* One line, cut to the width (every string here is ASCII, so a byte cut is a character cut). */
static void put(az_draw *d, int x, int *y, int row, int px, int w, int bottom, const char *s,
                uint32_t c)
{
    if (*y + px * 3 / 2 > bottom)
        return;
    char fit[160];
    snprintf(fit, sizeof fit, "%s", s);
    size_t n = strlen(fit);
    while (n > 0 && AZ_HAS(C, text_width) && C->text_width(fit, px) > w)
        fit[--n] = '\0';
    if (n)
        C->draw_text(d, x, *y, fit, px, c);
    *y += row;
}

static void on_draw(az_draw *d, az_rect area, void *user)
{
    (void)user;
    C->draw_header(d, "Gate Cue");
    const int row = C->row_height(d), pad = row / 4, bottom = area.y + area.h;
    int px = row / 2;
    if (px < 8)
        px = 8;
    if (px > 96)
        px = 96;
    int y = area.y + row;
    const int x = area.x + pad, w = area.w - 2 * pad;
    const uint32_t text = C->color(AZ_COL_TEXT), dim = C->color(AZ_COL_TEXT_DIM);
    char line[160];
    const int on = h_on >= 0 && C->row_value(h_on) == 1;
    put(d, x, &y, row, px, w, bottom,
        !(erp_ok && led_ok) ? "Frame filters unavailable on this core: nothing is gated"
        : on                ? "On: on GATE CUE, a set cue plays while its pad is held"
                            : "Off (Mod Menu row Gate Cue): hot cues are native",
        on && erp_ok && led_ok ? text : dim);
    static const char *const MODE[] = {"pad mode unknown (press HOT CUE)", "HOT CUE", "other mode"};
    static const char *const PAGE[] = {"", " / GATE CUE"};
    for (int s = 0; s < 2; s++) {
        int m = __atomic_load_n(&side_mode[s], __ATOMIC_RELAXED);
        int p = __atomic_load_n(&deck_playing[s], __ATOMIC_RELAXED);
        unsigned lit = __atomic_load_n(&cue_lit[s], __ATOMIC_RELAXED);
        char cues[9];
        for (int k = 0; k < 8; k++)
            cues[k] = (lit >> k) & 1u ? (char)('A' + k) : '-';
        cues[8] = '\0';
        int pg = __atomic_load_n(&page2[s], __ATOMIC_RELAXED) && m == 1;
        snprintf(line, sizeof line, "Deck %d: %s%s | %s | cues %s", s + 1,
                 MODE[m >= 0 && m <= 2 ? m : 0], PAGE[pg],
                 p == 1 ? "playing" : p == 0 ? "paused" : "?", cues);
        put(d, x, &y, row, px, w, bottom, line, text);
    }
    snprintf(line, sizeof line, "Gated presses %u | passed natively %u",
             __atomic_load_n(&n_gated, __ATOMIC_RELAXED), __atomic_load_n(&n_passed, __ATOMIC_RELAXED));
    put(d, x, &y, row, px, w, bottom, line, text);
    put(d, x, &y, row, px, w, bottom,
        __atomic_load_n(&two_deck, __ATOMIC_RELAXED) ? "2-deck view" : "4-deck view: Gate Cue waits",
        dim);
}

static const az_screen_v1 SCREEN = {
    .size = sizeof(az_screen_v1),
    .on_draw = on_draw,
    .on_input = NULL,
    .on_enter = NULL,
    .user = NULL,
};

static int init(const az_core_v1 *core)
{
    if (!core || core->abi != AZ_MOD_ABI)
        return 1;
    C = core;
    if (C->register_screen(MOD_ID, &SCREEN) != 0)
        return 4;
    if (AZ_HAS(C, register_row) && AZ_HAS(C, row_value)) {
        make_row_icon();
        const az_row_v1 r = {
            .size = sizeof r,
            .key = "on",
            .label = "Gate Cue",
            .kind = AZ_ROW_TOGGLE,
            .flags = AZ_ROW_DEFAULT_ON,
            .icon = row_icon,
            .help = "GATE CUE also gates a playing deck: a cue plays while its pad is held; "
                    "let go and the deck goes back to the cue, paused.",
        };
        h_on = C->register_row(MOD_ID, &r);
    }
    if (AZ_HAS(C, frame_filter)) {
        erp_ok = C->frame_filter(MOD_ID, 1u << AZ_BUS_ERP_IN, erp_filter, NULL) == 0;
        led_ok = C->frame_filter(MOD_ID, 1u << AZ_BUS_ERP_LED, led_filter, NULL) == 0;
    }
    C->log(MOD_ID, "loaded; row=%d erp=%d led=%d", h_on, erp_ok, led_ok);
    return 0;
}

static const char *const FW_TAGS[] = { "any", NULL };
static const az_mod_v1 MOD = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(az_mod_v1),
    .id = MOD_ID,
    .fw_tags = FW_TAGS,
    .init = init,
    .tick = tick,
    .on_frame = NULL,
};

const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
