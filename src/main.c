/* Jetpac for the Sega Master System - main loop, menu, input. */
#include "SMSlib.h"
#include "game.h"

extern u8 menu_cursor;
void menu_redraw(void);
void gameover_done(void);

u8 paused;
u8 is_pal;
u8 tick_acc;
u8 tick_count;
u8 debug_frames;
volatile u8 vb_flag;                /* set by the frame interrupt (isr.s) */
volatile u16 pad_raw;               /* joypad ports 0xDC/0xDD, sampled each frame */

u8 late_frames;                     /* frames that missed their vertical blank */

/* Wait for the vertical blank. If the frame interrupt already fired while we
 * were still busy we are late: the VRAM flush then runs into the active
 * display, which is safe because every write in render_vblank is paced. */
static void vsync_wait(void)
{
    if (vb_flag) late_frames++;
    while (!vb_flag) ;
    vb_flag = 0;
}

__sfr __at 0x7E VDPVCounter;

/* count scanlines per frame: 262 on NTSC, 313 on PAL */
u8 detect_pal(void)
{
    u16 lines = 0;
    u8 prev, v;
    SMS_waitForVBlank();
    while (VDPVCounter != 0) ;
    prev = 0;
    for (;;) {
        v = VDPVCounter;
        if (v != prev) {
            prev = v;
            lines++;
            if (v == 0 || lines > 400) break;
        }
    }
    return lines > 290;
}

static void read_play_input(u16 keys)
{
    if (cur_player && (options & 2)) keys >>= 6;     /* player 2 on pad B */
    in_left  = (keys & PORT_A_KEY_LEFT) ? 1 : 0;
    in_right = (keys & PORT_A_KEY_RIGHT) ? 1 : 0;
    in_up    = (keys & (PORT_A_KEY_UP | PORT_A_KEY_2)) ? 1 : 0;
    in_down  = (keys & PORT_A_KEY_DOWN) ? 1 : 0;
    in_fire  = (keys & PORT_A_KEY_1) ? 1 : 0;
}

static u16 menu_prev_keys;

static void menu_tick(u16 keys)
{
    u16 p = keys & ~menu_prev_keys;      /* own edge detection: survives skipped ticks */
    u8 changed = 0;
    menu_prev_keys = keys;
    if (p & (PORT_A_KEY_UP | PORT_B_KEY_UP)) { if (menu_cursor) menu_cursor--; else menu_cursor = 4; changed = 1; }
    if (p & (PORT_A_KEY_DOWN | PORT_B_KEY_DOWN)) { if (menu_cursor < 4) menu_cursor++; else menu_cursor = 0; changed = 1; }
    if (p & (PORT_A_KEY_1 | PORT_A_KEY_2 | PORT_B_KEY_1 | PORT_B_KEY_2)) {
        switch (menu_cursor) {
        case 0: options &= ~1; changed = 1; break;
        case 1: options |= 1; changed = 1; break;
        case 2: options &= ~2; changed = 1; break;
        case 3: options |= 2; changed = 1; break;
        default: game_new(); return;
        }
    }
    if (changed) menu_redraw();
}

void main(void)
{
    u16 keys;
    SMS_mapROMBank(2);
    SMS_init();
    options = 0; hiscore[0] = hiscore[1] = hiscore[2] = 0;
    score[0][0] = score[0][1] = score[0][2] = 0;
    score[1][0] = score[1][1] = score[1][2] = 0;
    menu_cursor = 4;
    paused = 0; tick_acc = 0; tick_count = 0; flash_phase = 0;
    menu_prev_keys = 0xFFFF;             /* ignore a button held at power-on */
    jet.t = 0; sfx_type = 0; game_timer = 0;
    render_init();
    snd_init();
    is_pal = detect_pal();
    /* switch to the lean interrupt handler in isr.s (interrupt mode 2) */
    __asm
        di
        ld   a, #0x7C
        ld   i, a
        im   2
        ei
    __endasm;
    snd_set_pal(is_pal);
    game_state = GS_MENU;
    render_menu_screen();

    for (;;) {
        u8 tick;
        vsync_wait();
        render_vblank();
        snd_frame();
        keys = ~pad_raw;                 /* same layout as SMS_getKeysStatus() */
        if (SMS_queryPauseRequested()) {
            SMS_resetPauseRequest();
            if (game_state == GS_PLAY) { paused ^= 1; if (paused) snd_stop(); }
        }
        /* 50Hz logic on a 60Hz display: skip one tick every six frames */
        tick = 1;
        if (!is_pal) {
            tick_acc += 5;
            if (tick_acc >= 6) tick_acc -= 6;   /* accumulator wrapped: run a tick */
            else tick = 0;                       /* 1 frame in 6 has no logic tick */
        }
        if (tick && !paused) {
            tick_count++;
            flash_phase = (tick_count >> 4) & 1;
            switch (game_state) {
            case GS_MENU:
                rnd_stir();
                menu_tick(keys);
                break;
            case GS_PLAY:
                menu_prev_keys = keys;   /* a button still held when we return to the menu is ignored */
                read_play_input(keys);
                game_tick();
                break;
            default:
                menu_prev_keys = keys;
                if (gameover_timer) gameover_timer--;
                else gameover_done();
                break;
            }
        }
        render_frame();
        debug_frames++;
    }
}

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1, 0, "Arena", "Jetpac SMS", "Port of Ultimate's Jetpac (1983) with Spec256 graphics");
