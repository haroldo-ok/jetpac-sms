/* Jetpac (ZX Spectrum, Ultimate 1983) - Sega Master System port.
 * Shared declarations.  The game logic mirrors the original Z80 code
 * (object layout, timing model, RNG, collision rules) so it plays the same.
 */
#ifndef GAME_H
#define GAME_H

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned int u16;
typedef signed int s16;

#include "gen_assets.h"

/* ---------------------------------------------------------------- objects */
typedef struct {
    u8 t;     /* byte 0: type / direction bits (bit6 left, bit7 down)         */
    u8 x;     /* byte 1: X (left edge)                                          */
    u8 y;     /* byte 2: Y (bottom row of the sprite)                           */
    u8 c;     /* byte 3: colour attribute                                       */
    u8 s;     /* byte 4: state / moving bits (bit6 left, bit7 down)             */
    u8 v1;    /* byte 5: X velocity / speed / sub position                      */
    u8 v2;    /* byte 6: Y velocity / sprite offset / anim direction            */
    u8 h;     /* byte 7: height used for collisions                             */
} Obj;

/* object types (byte 0 & 0x3F) - indices of the original main jump table */
#define T_NONE      0
#define T_FLY       1
#define T_WALK      2
#define T_METEOR    3
#define T_ITEM      4
#define T_CROSS     5
#define T_SPHERE    6
#define T_JET       7
#define T_EXPLODE   8
#define T_ROCKET    9
#define T_TAKEOFF   10
#define T_LANDING   11
#define T_SFX_ENEMY 12
#define T_SFX_PLAYER 13
#define T_COLLECT   14
#define T_UFO       15
#define T_LASER     16
#define T_SQUIDGY   17

typedef struct {
    u8 t;       /* 0x10 while in use */
    u8 y;
    u8 px[4];   /* pulse X positions: [0] head; bit0 = moving left, bit2 = active */
    u8 len;
    u8 col;     /* 0x47 white, 0x43 magenta, 0x45 cyan */
} Laser;

/* object slots (same order as the original RAM layout) */
#define SLOT_ROCKET  0
#define SLOT_ITEMMOD 1
#define SLOT_ITEMCOL 2
#define SLOT_PUFF    3
#define SLOT_ALIEN0  4
#define SLOT_JEXPL   10
#define N_SLOTS      11

extern Obj jet;                 /* $5D00 */
extern Laser lasers[4];         /* $5D08 */
extern u8 sfx_type, sfx_dur;    /* $5D28 */
extern Obj slots[N_SLOTS];      /* $5D30 .. $5D87 */
extern Obj rocket2[3];          /* $5D98 inactive player's rocket + module items */
#define rocket   (slots[SLOT_ROCKET])
#define item_mod (slots[SLOT_ITEMMOD])
#define item_col (slots[SLOT_ITEMCOL])
#define puff     (slots[SLOT_PUFF])
#define jexpl    (slots[SLOT_JEXPL])

/* ---------------------------------------------------------------- game vars */
extern u8 hiscore[3];           /* BCD */
extern u8 score[2][3];          /* BCD, player 1 / player 2 */
extern u8 options;              /* bit0: 2 player game, bit1: two control pads */
extern u8 level, lives;         /* current player */
extern u8 level2, lives2;       /* inactive player */
extern u8 cur_player;           /* 0 or 1 */
extern u8 begin_delay;
extern u8 game_timer;
extern u8 rnd;
extern u8 alien_count;
extern u8 sub_x, sub_y;         /* Jetman sub-pixel accumulators ($5DC7/$5DC8) */
extern u8 frame_count;          /* 50Hz logic ticks (SYSVAR FRAMES stand-in) */

/* game flow */
#define GS_MENU      0
#define GS_PLAY      1
#define GS_GAMEOVER  2
extern u8 game_state;
extern u8 gameover_timer;
extern u8 gameover_player;
extern u8 flash_phase;

/* input (per tick) */
extern u8 in_left, in_right, in_up, in_down, in_fire;

/* timing model: virtual T-state budget (units of 64 T) */
#define FRAME_UNITS 1092
extern s16 budget;
extern u8 cur_slot;

/* ---------------------------------------------------------------- game.c */
void game_new(void);
void rnd_stir(void);                /* advance the RNG (menu idle) */
void game_tick(void);
void level_init(void);
void player_init(void);

/* ---------------------------------------------------------------- render.c */
void render_init(void);
void render_level_screen(void);     /* ResetScreen: nebula, platforms, HUD */
void render_menu_screen(void);
void render_gameover_screen(void);
void render_frame(void);            /* build sprites + composites for this frame */
void render_vblank(void);           /* flush pending VRAM writes (call right after vblank) */
void hud_score(u8 which);           /* 0 = P1, 1 = P2, 2 = HI */
void hud_lives(void);
void hud_flash_label(u8 player, u8 on);
void beams_clear(void);
u8 screen_byte_nonzero(u8 x, u8 y);
/* assembly entry only: a = laser, l = column, h = byte, ix = &lasers[li] */
void beam_set_asm(void);
extern u8 beam[4][32];
extern u8 render_dirty_hud;

/* ---------------------------------------------------------------- sound.c */
void snd_init(void);
void snd_set_pal(u8 pal);
void snd_play(const u8 *seq);
void snd_thrust(u8 y);
void snd_stop(void);
void snd_frame(void);               /* called once per displayed frame (vblank) */
extern const u8 sfx_build[], sfx_pickup_fuel[], sfx_pickup_item[], sfx_laser[];
extern const u8 sfx_explode_enemy[], sfx_explode_player[];
extern const u8 sfx_thrust_tab[32*3];

/* ---------------------------------------------------------------- data.c (bank 2) */
extern const u8 spr_core_bin[], spr_alien_bin[], spr_rocket_bin[];
extern const u8 neb_tiles_bin[], neb_map_bin[], plat_tiles_bin[];
extern const u8 font_bin[], pal_bg_bin[];

#endif
