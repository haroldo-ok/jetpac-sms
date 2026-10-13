/* Jetpac game logic - a faithful C transcription of the original Z80 game
 * engine (object slots, per-pass updates, collision rules, scoring), with
 * the original's timing reproduced through a virtual T-state budget.
 */
#include "game.h"

Obj jet;
Laser lasers[4];
u8 sfx_type, sfx_dur;
Obj slots[N_SLOTS];
Obj rocket2[3];

u8 hiscore[3];
u8 score[2][3];
u8 options;
u8 level, lives, level2, lives2;
u8 cur_player;
u8 begin_delay;
u8 game_timer;
u8 rnd;
u8 alien_count;
u8 sub_x, sub_y;
u8 frame_count;
u8 game_state, gameover_timer, gameover_player, flash_phase;
u8 in_left, in_right, in_up, in_down, in_fire;
s16 budget;
u8 cur_slot;

static u16 seed = 0x2545;
static u8 newdir;           /* $5DC9 */
static u16 fire_cost;       /* extra virtual time spent on one-shot SFX this tick */
u8 velmod = 4;              /* $5DCA */

static const u8 platforms[16] = {4,0x80,0x60,0x1B, 6,0x78,0xB8,0x88, 4,0x30,0x48,0x23, 4,0xD0,0x30,0x23};
static const u8 default_player[8]  = {1,0x80,0xB7,0x47,0,0,0,0x24};
static const u8 default_rocket[24] = {9,0xA8,0xB7,2,1,0,0,0x1C,
                                      4,0x30,0x47,0x47,0,0,0x10,0x18,
                                      4,0x80,0x5F,0x47,1,0,0x08,0x18};
static const u8 default_fuel[8]    = {4,0,0x20,0x43,1,0,0x18,0x18};
static const u8 default_collect[8] = {0x0E,0,0x20,0,0,0,0,0x18};
static const u8 default_alien[8]   = {3,0,0,0x42,0x80,4,0,0x1C};
static const u8 level_alien_type[8] = {3,0x11,6,7,0x0F,5,3,0x0F};
static const u8 drop_columns[16] = {8,0x20,0x28,0x30,0x38,0x40,0x58,0x60,0x78,0x80,0x88,0xC0,0xE0,8,0x58,0x60};
static const u8 laser_colours[4] = {0x47,0x43,0x43,0x45};
static const u8 eraser_mask[3] = {0x03,0x1C,0xE0};

/* virtual cost (units of 64 T-states) of one update, by object type */
static const u8 slot_cost_tab[18] = {
    83,   /* 0 empty: FrameRateLimiter delay           */
    5, 5, /* 1,2 (never in a pass slot)                 */
    183,  /* 3 meteor                                   */
    108,  /* 4 module / fuel item                       */
    215,  /* 5 crossed ship                             */
    250,  /* 6 sphere                                   */
    170,  /* 7 jet fighter                              */
    115,  /* 8 explosion                                */
    178,  /* 9 rocket                                   */
    190, 190, /* 10,11 takeoff / landing (plus SFX)     */
    5, 5,     /* 12,13                                  */
    220,  /* 14 collectible                             */
    215,  /* 15 UFO                                     */
    5,    /* 16                                         */
    230   /* 17 squidgy                                 */
};

/* ------------------------------------------------------------------ helpers */
static void copy8(Obj *d, const u8 *s)
{
    u8 *p = (u8 *)d; u8 i;
    for (i = 0; i < 8; i++) p[i] = s[i];
}

/* 16-bit xorshift (x ^= x<<7; x ^= x>>9; x ^= x<<8), period 65535 */
static void rnd_next(void) __naked
{
    __asm
        ld   hl, (_seed)
        ld   a, h
        rra
        ld   a, l
        rra
        xor  a, h
        ld   h, a
        ld   a, l
        rra
        ld   a, h
        rra
        xor  a, l
        ld   l, a
        xor  a, h
        ld   h, a
        ld   (_seed), hl
        ld   (_rnd), a
        ret
    __endasm;
}

static u8 bcd_add(u8 a, u8 b, u8 *carry)
{
    u8 lo = (a & 0x0F) + (b & 0x0F) + *carry;
    u8 hi = (a >> 4) + (b >> 4);
    *carry = 0;
    if (lo > 9) { lo -= 10; hi++; }
    if (hi > 9) { hi -= 10; *carry = 1; }
    return (hi << 4) | lo;
}

static void add_score(u16 bcd)
{
    u8 *s = score[cur_player];
    u8 c = 0;
    s[2] = bcd_add(s[2], (u8)bcd, &c);
    s[1] = bcd_add(s[1], (u8)(bcd >> 8), &c);
    s[0] = bcd_add(s[0], 0, &c);
    hud_score(cur_player);
}

static void set_explode_sfx(u8 player)
{
    sfx_type = player ? T_SFX_PLAYER : T_SFX_ENEMY;
    sfx_dur = 4;
    snd_play(player ? sfx_explode_player : sfx_explode_enemy);
}

/* $685D AnimationStateReset: object becomes an explosion */
static void anim_reset(Obj *o)
{
    u8 a = (o->t & 0xC0) | 3;
    o->v1 = 0;
    o->v2 = a; o->t = T_EXPLODE; o->s = 0;
}

/* $683E ExplosionAfterKill: Jetman dies */
static void explosion_after_kill(void)
{
    jexpl.x = jet.x; jexpl.y = jet.y;
    jexpl.v1 = 1; jexpl.v2 = jet.t; jexpl.t = T_EXPLODE; jexpl.s = 0;
    jet.t = 0;
}

/* $6456 */
static void alien_collision_anim_sfx(Obj *o)
{
    anim_reset(o);
    set_explode_sfx(1);
    explosion_after_kill();
}

/* $6BF1 */
static void alien_kill_sfx1(Obj *o)
{
    anim_reset(o);
    set_explode_sfx(0);
}

/* $6D5C */
static void alien_kill_sfx2(Obj *o, u16 pts)
{
    add_score(pts);
    anim_reset(o);
    set_explode_sfx(1);
}

/* $75E8 PlatformCollisionCheckAll - returns the E flags of the original */
/* $6D9E PlatformCollision: flags for object vs the four platforms
 *   bit 2 collision, bit 7 landed on top, bit 4 hit from below, bit 6 platform
 *   is to the left, bit 3 near the edge.   hl = object -> a
 */
static u8 plat_collide(Obj *o) __naked
{
    (void)o;
    __asm
        push ix
        push hl
        pop  ix
        ld   hl, #_platforms
        ld   b, #4
    1$:
        ld   e, #0                  ; e = flags
        inc  hl
        ld   a, (hl)                ; platform x
        sub  a, 1 (ix)
        jp   p, 2$
        neg
        ld   e, #0x40
    2$:
        ld   c, a                   ; c = horizontal distance
        ld   a, 0 (ix)
        and  a, #0x3F
        cp   a, #T_METEOR
        jr   nz, 3$
        ld   a, c
        sub  a, #9
        jp   m, 3$
        ld   c, a
    3$:
        inc  hl
        inc  hl                     ; platform width
        ld   a, c
        cp   a, (hl)
        jr   nc, 9$
        add  a, #0x12
        cp   a, (hl)
        jr   c, 4$
        set  3, e
    4$:
        dec  hl                     ; platform y
        ld   a, 2 (ix)
        sub  a, (hl)
        add  a, #2                  ; d = o->y - py + 2
        inc  hl
        jp   m, 9$
        cp   a, #2
        jr   nc, 5$
        ld   a, e
        or   a, #0x84
        jr   8$
    5$:
        cp   a, 7 (ix)
        jr   nc, 6$
        ld   a, e
        or   a, #0x04
        jr   8$
    6$:
        sub  a, #2
        cp   a, 7 (ix)
        jr   nc, 9$
        ld   a, e
        or   a, #0x14
        jr   8$
    9$:
        inc  hl
        djnz 1$
        ld   a, e
    8$:
        pop  ix
        ret
    __endasm;
}

/* $6DE9 AlienCollision: 1 if Jetman touches the object */
static u8 alien_collide(Obj *o)
{
    u8 a, lim;
    a = jet.t & 0x3F;
    if (a != T_FLY && a != T_WALK) return 0;
    a = jet.x - o->x;
    if (a & 0x80) a = (u8)(0 - a);
    if (a >= 12) return 0;
    a = jet.y - o->y;
    if (a & 0x80) { a = (u8)(0 - a); a += 14; lim = o->h; }
    else lim = 21;
    return a < lim;
}

/* $6E1B LaserBeamFire: 1 if a laser pulse hits the object */
static u8 laser_hit(Obj *o)
{
    u8 i, a, lim;
    for (i = 0; i < 4; i++) {
        Laser *L = &lasers[i];
        if (!L->t) continue;
        a = L->px[1];
        if (!(a & 4)) continue;
        a = (a & 0xF8) - o->x;
        if (a & 0x80) { a = (u8)(0 - a); lim = 8; } else lim = 32;
        if (a >= lim) continue;
        a = o->y - L->y;
        if (a & 0x80) continue;
        a += 12;
        if (a >= o->h) continue;
        L->px[0] &= 0xF8;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ level / player flow */
static void rocket_reset(void)
{
    copy8(&rocket, default_rocket);
    copy8(&item_mod, default_rocket + 8);
    copy8(&item_col, default_rocket + 16);
}

void player_init(void)
{
    copy8(&jet, default_player);
    begin_delay = (options & 1) ? 0xFF : 0x80;
    lives--;
    hud_lives();
}

void level_init(void)
{
    render_level_screen();
    hud_lives();
}

static void level_new(void)
{
    if ((level & 3) == 0) {
        rocket_reset();
        lives++;
        player_init();
    }
    level_init();
}

static void players_swap(void)
{
    u8 t, i;
    t = level; level = level2; level2 = t;
    t = lives; lives = lives2; lives2 = t;
    for (i = 0; i < 3; i++) { Obj tmp = slots[i]; slots[i] = rocket2[i]; rocket2[i] = tmp; }
}

static void update_hiscore(void)
{
    u8 *best = score[0];
    u8 i;
    for (i = 0; i < 3; i++) {
        if (score[1][i] > score[0][i]) { best = score[1]; break; }
        if (score[1][i] < score[0][i]) break;
    }
    for (i = 0; i < 3; i++) {
        if (best[i] > hiscore[i]) { hiscore[0] = best[0]; hiscore[1] = best[1]; hiscore[2] = best[2]; return; }
        if (best[i] < hiscore[i]) return;
    }
}

static void game_over(u8 player)
{
    game_state = GS_GAMEOVER;
    gameover_player = player;
    gameover_timer = 100;           /* ~2 seconds like the original delay loop */
    render_gameover_screen();
}

/* $60B7 PlayerTurnEnds */
static void player_turn_ends(void)
{
    u8 i;
    for (i = SLOT_PUFF; i < N_SLOTS; i++) slots[i].t = 0;
    item_mod.s &= ~2;
    item_col.s &= ~2;
    if (!(options & 1) || lives2 == 0) {
        if (lives == 0) { update_hiscore(); game_over(cur_player); return; }
        level_init();
        player_init();
        return;
    }
    if (lives == 0) {
        /* show GAME OVER for this player, then the other one carries on */
        players_swap();
        cur_player ^= 1;
        game_state = GS_GAMEOVER;
        gameover_player = cur_player ^ 1;
        gameover_timer = 100;
        render_gameover_screen();
        return;
    }
    players_swap();
    cur_player ^= 1;
    level_init();
    player_init();
}

/* called by main when the game-over message has been shown */
void gameover_done(void)
{
    if (game_state != GS_GAMEOVER) return;
    /* two player game: the other player still has lives -> continue */
    if ((options & 1) && lives != 0) {
        game_state = GS_PLAY;
        level_init();
        player_init();
        return;
    }
    game_state = GS_MENU;
    render_menu_screen();
}

/* Advance the random generator while the menu is shown: the original seeds
 * itself from hardware state, so no two games start with the same aliens. */
void rnd_stir(void)
{
    rnd_next();
    game_timer++;
}

/* $62FE NewGame */
void game_new(void)
{
    u8 i, j;
    for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) score[i][j] = 0;
    jet.t = 0;
    for (i = 0; i < 4; i++) lasers[i].t = 0;
    sfx_type = 0;
    for (i = 0; i < N_SLOTS; i++) slots[i].t = 0;
    beams_clear();
    /* ResetPlayerData */
    level = 0; lives = 4; rocket_reset(); players_swap();
    level = 0; lives = 4; rocket_reset(); players_swap();
    /* both players get 4 lives; player 1 immediately gains the new-rocket
     * bonus life in level_new, player 2 does not (as on the Spectrum) */
    lives2 = (options & 1) ? 4 : 0;
    cur_player = 0;
    alien_count = 0; budget = 0; cur_slot = 0;      /* the game timer keeps running between games, as on the Spectrum */
    game_state = GS_PLAY;
    hud_score(0); hud_score(1); hud_score(2);
    level_new();
}

/* ------------------------------------------------------------------ items */
static u8 item_drop_column(void)
{
    return drop_columns[rnd & 0x0F];
}

/* $65F9 */
static void item_new_fuel_pod(void)
{
    u8 a = jet.t & 0x3F;
    if (a == 0 || a >= 3) return;
    if (item_mod.t) return;
    if (rocket.v1 >= 6) return;
    if ((u8)(~game_timer & 0x0F)) return;
    copy8(&item_mod, default_fuel);
    item_mod.x = item_drop_column();
}

/* $65A8 */
static void item_new_collectible(void)
{
    u8 a = jet.t & 0x3F;
    if (a == 0 || a >= 3) return;
    if (item_col.t) return;
    if (game_timer & 0x7F) return;
    copy8(&item_col, default_collect);
    item_col.x = item_drop_column();
    a = (u8)(seed >> 3) & 0x0E;
    if (a & 8) a &= 8;
    item_col.v2 = a | 0x20;
}

/* $64E8 ItemPickup: rocket modules and fuel pods */
static void item_pickup(Obj *o)
{
    u8 a;
    if (o->s & 4) {
        /* $6565 carried item released above the rocket: falls onto it */
        if (o->v2 == 0x18) {
            if (o->y < 0xB0) { o->y += 2; return; }
            rocket.v1++;
        } else {
            a = (o->v2 << 1) + o->y;
            if (a < 0xB7) { o->y += 2; return; }
            item_mod.s |= 1;
            rocket.s++;
        }
        o->t = 0;
        snd_play(sfx_build); fire_cost += 1040;
        return;
    }
    if (o->s & 2) {
        /* $6541 carried by Jetman */
        o->x = jet.x; o->y = jet.y;
        a = rocket.x - o->x;
        if (a & 0x80) a = (u8)(0 - a);
        if (a >= 6) return;
        o->s |= 4;
        o->x = rocket.x;
        return;
    }
    if (!(o->s & 1)) return;            /* resting, not yet active */
    if (alien_collide(o)) {
        /* $6523 CollectRocketItem */
        o->s |= 2;
        add_score(0x0100);
        snd_play(sfx_pickup_fuel); fire_cost += 1300;
        o->x = jet.x; o->y = jet.y;
        return;
    }
    if (plat_collide(o) & 4) return;
    o->y += 2;
}

/* $6461 ItemCheckCollect: collectible treasure */
static void item_check_collect(Obj *o)
{
    if (!(plat_collide(o) & 4)) o->y += 2;
    if (alien_collide(o)) {
        o->t = 0;
        add_score(0x0250);
        snd_play(sfx_pickup_item); fire_cost += 1250;
        return;
    }
    /* $648B ItemDropNew: colour handling (visibility of flashing items) */
    switch (o->v2 & 0x0F) {
    case 0: o->c = 0x46; break;
    case 2: case 4:
        o->c = ((game_timer & 0x1F) >= 0x18) ? 0x45 : 0x00;
        break;
    case 6: o->c = 0x44; break;
    default: { u8 a = (game_timer >> 2) & 7; if (!a) a = 1; o->c = a | 0x40; } break;
    }
}

/* ------------------------------------------------------------------ rocket */
static void rocket_update(Obj *o)
{
    if (!alien_collide(o)) return;
    if (o->v1 < 6) return;
    o->t++;                      /* take off */
    jet.t = 0;
    lives++;
    hud_lives();
}

static void modules_reset(void)
{
    u8 i;
    for (i = SLOT_ITEMMOD; i < N_SLOTS; i++) slots[i].t = 0;
}

static void rocket_takeoff(Obj *o)
{
    o->y--;
    snd_thrust(o->y); fire_cost += 520;
    if (o->y >= 0x28) return;
    level++;
    modules_reset();
    o->t++;                      /* landing */
    o->v1 = 0;
    level_new();
}

static void rocket_landing(Obj *o)
{
    o->y++;
    snd_thrust(o->y); fire_cost += 520;
    if (o->y < 0xB7) return;
    o->t = T_ROCKET;
    player_init();
}

/* ------------------------------------------------------------------ explosions */
static void anim_explosion(Obj *o)
{
    u8 c = o->s, b = o->v1;
    alien_count++;
    if ((game_timer & b) == 0) o->s++;
    if (c >= 6) {
        o->t = 0;
        if ((o->v2 & 0x3F) < 3) player_turn_ends();
    }
}

/* ------------------------------------------------------------------ aliens */
static void meteor_update(Obj *o)
{
    alien_count++;
    if (o->s & 0x40) o->x -= o->v1; else o->x += o->v1;
    o->y += o->v2;
    if (plat_collide(o) & 4) { alien_kill_sfx1(o); return; }
    if (laser_hit(o)) { add_score(0x0025); alien_kill_sfx1(o); return; }
    if (alien_collide(o)) alien_collision_anim_sfx(o);
}

static void jet_set_speed(Obj *o, u8 c)
{
    o->s |= 2;
    o->v1 = ((game_timer + c) & 0x7F) | 0x20;
    o->c = 0x47;
}

static void jet_fighter_update(Obj *o)
{
    s8 l = 0, h;
    alien_count++;
    if (o->s & 2) {
        o->v1--;
        if (o->v1 == 0) goto killed;
        l = (o->t & 0x40) ? -4 : 4;
        h = (jet.y < o->y) ? -2 : 2;
    } else {
        if ((rnd & 0x1F) == 0) jet_set_speed(o, 0);
        { u8 a = jet.y - 12; if (a == o->y) jet_set_speed(o, a); }
        h = (game_timer & 0x40) ? 2 : -2;
    }
    o->t = (o->t & 0xC0) | 3;
    o->x += l;
    o->y += h;
    if (o->y < 0x28) goto killed;
    if (laser_hit(o)) goto killed;
    if (plat_collide(o) & 4) goto killed;
    if (alien_collide(o)) { alien_collision_anim_sfx(o); return; }
    o->t = (o->t & 0xC0) | 7;
    return;
killed:
    add_score(0x0055);
    snd_thrust(o->y); fire_cost += 520;
    anim_reset(o);
}

static void squidgy_update(Obj *o)
{
    u8 e, a;
    alien_count++;
    if (laser_hit(o)) { add_score(0x0080); alien_kill_sfx1(o); return; }
    if (alien_collide(o)) { alien_collision_anim_sfx(o); return; }
    newdir = 0;
    for (;;) {
        e = plat_collide(o);
        if (e & 4) {
            if (e & 0x80) o->s &= ~0x80;
            else if (e & 0x10) o->s |= 0x80;
            else o->s = (o->s & 0xBF) | (e & 0x40);
        }
        a = o->x;
        if (o->s & 0x40) a -= 2; else a += 2;
        o->x = a;
        a = o->y;
        if (o->s & 0x80) a += 2;
        else { a -= 2; if (a < 0x24) o->s |= 0x80; }
        o->y = a;
        if (newdir) return;
        newdir = 1;
    }
}

static void sphere_update(Obj *o)
{
    u8 e, a;
    alien_count++;
    newdir = 0;
    if (laser_hit(o)) { alien_kill_sfx2(o, 0x0040); return; }
    if (alien_collide(o)) { alien_collision_anim_sfx(o); return; }
    for (;;) {
        e = plat_collide(o);
        if (e & 4) {
            if (e & 0x80) o->s &= ~0x80;
            else if (e & 0x10) o->s |= 0x80;
            else o->s = (o->s & 0xBF) | (e & 0x40);
        }
        if (!(o->s & 1)) {
            e = rnd;
            if ((e & 0x0F) == 0) {
                o->s |= 1;
                o->v2 = (game_timer & 0x1F) + 0x10;
                o->s = (o->s & 0x7F) | (e & 0x80);
                continue;               /* $6C59: repeat the collision check */
            }
        }
        if (o->s & 1) {
            a = o->y;
            if (o->s & 0x80) a += 2;
            else { a -= 2; if (a < 0x28) o->s |= 0x80; }
            o->y = a;
            o->v2--;
            if (o->v2 == 0) o->s &= ~1;
        }
        a = o->x;
        if (o->s & 0x40) a -= 2; else a += 2;
        o->x = a;
        if (newdir) return;
        newdir = 1;
    }
}

/* 16-bit position/speed helper for the UFO: returns speed<<5 */
static u16 ufo_step(u8 c)
{
    return (u16)(c & 0x0F) << 5;
}

static void ufo_update(Obj *o)
{
    u8 e, c, a;
    u16 hl;
    alien_count++;
    if (laser_hit(o)) { add_score(0x0050); alien_kill_sfx1(o); return; }
    if (alien_collide(o)) { alien_collision_anim_sfx(o); return; }
    newdir = 0;
    for (;;) {
        e = plat_collide(o);
        if (e & 4) {
            if (e & 0x80) o->s &= ~0x80;
            else if (e & 0x10) o->s |= 0x80;
            else o->s = (o->s & 0xBF) | ((e & 0x40) ^ 0x40);
        }
        /* horizontal */
        c = o->v1 & 0x0F;
        hl = ((u16)o->x << 8) | (o->v1 & 0xF0);
        a = jet.x - o->x;
        if (!(a & 0x80)) {                      /* Jetman to the right */
            if (!(o->s & 0x40)) {               /* moving right: accelerate */
                if (c < 0x0F) c++;
                hl += ufo_step(c);
            } else {
                c--;
                if (c) hl -= ufo_step(c);
                else o->s &= ~0x40;
            }
        } else {                                /* Jetman to the left */
            if (o->s & 0x40) {
                if (c < 0x0F) c++;
                hl -= ufo_step(c);
            } else {
                c--;
                if (c) hl += ufo_step(c);
                else o->s |= 0x40;
            }
        }
        o->x = hl >> 8;
        o->v1 = (hl & 0xF0) | c;
        /* vertical */
        c = o->v2 & 0x0F;
        hl = ((u16)o->y << 8) | (o->v2 & 0xF0);
        a = jet.y - o->y;
        if (!(a & 0x80)) {                      /* Jetman below (or level) */
            if (o->s & 0x80) {                  /* moving down */
                if (c < 0x0F) c++;
                hl += ufo_step(c);
            } else {
                c--;
                if (c) hl -= ufo_step(c);
                else o->s |= 0x80;
            }
        } else {                                /* Jetman above */
            if (!(o->s & 0x80)) {
                if (c < 0x0F) c++;
                hl -= ufo_step(c);
            } else {
                c--;
                if (c) hl += ufo_step(c);
                else o->s &= ~0x80;
            }
        }
        a = hl >> 8;
        if (a < 0x28) o->s |= 0x80;
        o->y = a;
        o->v2 = (hl & 0xF0) | c;
        if (newdir) return;
        newdir = 1;
    }
}

static void cross_update(Obj *o)
{
    u8 e, a;
    u16 hl, de;
    alien_count++;
    if (laser_hit(o)) { alien_kill_sfx2(o, 0x0060); return; }
    if (alien_collide(o)) { alien_collision_anim_sfx(o); return; }
    newdir = 0;
    for (;;) {
        e = plat_collide(o);
        if (e & 4) {
            if (e & 0x80) { o->s &= ~0x80; o->v2 = rnd + 8; }
            else if (e & 0x10) o->s |= 0x80;
            else o->s = (o->s & 0xBF) | ((e & 0x40) ^ 0x40);
        }
        a = o->x;
        if (o->s & 0x40) a -= 2; else a += 2;
        o->x = a;
        hl = (u16)o->v2 << 1;
        de = ((u16)o->y << 8) | o->v1;
        if (o->s & 0x80) hl = de + hl; else hl = de - hl;
        o->v1 = (u8)hl;
        a = hl >> 8;
        o->y = a;
        if (a < 0x28) o->s |= 0x80;
        if (o->s & 0x80) {
            o->v2++;
            if (o->v2 == 0) o->v2 = 0xFF;
        } else {
            o->v2--;
            if (o->v2 == 0) o->s |= 0x80;
        }
        if (newdir) return;
        newdir = 1;
    }
}

/* ------------------------------------------------------------------ lasers */
static void laser_init(u8 i)
{
    Laser *L = &lasers[i];
    u8 c;
    L->t = 0x10;
    c = (jet.x & 0xF8) | 5;
    if (jet.t & 0x40) c -= 8;
    else {
        if (jet.x & 7) c += 8;
        c += 0x10;
        c &= 0xFE;
    }
    L->y = jet.y - 13;
    L->px[0] = c;
    L->px[1] = L->px[2] = L->px[3] = c & 0xFB;
    L->len = (rnd & 0x38) | 0x84;
    L->col = laser_colours[rnd & 3];
    snd_play(sfx_laser); fire_cost += 610;
}

static void laser_new_if_free(void)
{
    u8 i;
    if (frame_count & 3) return;
    for (i = 0; i < 4; i++) if (!lasers[i].t) { laser_init(i); return; }
}

static u8 la_li;

/* Laser beam animation (skool $6fc5): the head advances one cell per frame
 * until its length runs out or it hits something; three erasers follow,
 * each clearing part of the byte (masks $03/$1C/$E0), each starting after
 * a random delay; the laser is freed when all three reach the head cell. */
static void laser_animate(u8 li) __naked
{
    (void)li;
    __asm
        push ix
        ld   (_la_li), a
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl
        ld   de, #_lasers
        add  hl, de
        push hl
        pop  ix                     ; ix = laser
        ; ---- head
        ld   a, 2 (ix)
        bit  2, a
        jr   z, 40$
        ld   c, a
        add  a, #8
        bit  0, c
        jr   z, 41$
        sub  a, #16                 ; moving left
    41$:
        ld   e, a                   ; e = new head x
        ld   a, 1 (ix)
        cp   a, #0x80
        jr   nc, 42$                ; below the screen bottom: no collision
        push de
        ld   l, a
        ld   a, e
        call _screen_byte_nonzero   ; a = x, l = y -> a
        pop  de
        or   a, a
        jr   z, 42$
        res  2, 2 (ix)              ; blocked: head stops here
        jr   40$
    42$:
        ld   2 (ix), e
        ld   a, e
        rrca
        rrca
        rrca
        and  a, #0x1F
        ld   l, a                   ; column
        ld   h, #0xFF               ; full byte
        ld   a, (_la_li)
        call _beam_set_asm
        ld   a, 6 (ix)
        sub  a, #8
        ld   6 (ix), a
        and  a, #0xF8
        jr   nz, 40$
        res  2, 2 (ix)              ; length used up
    40$:
        ; ---- erasers
        ld   b, #3
        ld   c, #0                  ; caught count
        push ix
        pop  hl
        inc  hl
        inc  hl
        inc  hl                     ; hl = &px[1]
        ld   de, #_eraser_mask
    43$:
        ld   a, (hl)
        xor  a, 2 (ix)
        and  a, #0xF8
        jr   nz, 44$
        inc  c                      ; reached the head cell
        jr   47$
    44$:
        ld   a, (hl)
        bit  2, a
        jr   z, 46$                 ; not started yet
        add  a, #8
        bit  0, a
        jr   z, 45$
        sub  a, #16
    45$:
        ld   (hl), a
        rrca
        rrca
        rrca
        and  a, #0x1F
        push hl
        push de
        push bc
        ld   c, a                   ; c = column
        ld   a, (de)
        cpl
        ld   b, a                   ; b = ~mask
        ld   a, (_la_li)
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        ld   de, #_beam
        add  hl, de
        ld   a, l
        add  a, c
        ld   l, a
        jr   nc, 48$
        inc  h
    48$:
        ld   a, (hl)
        and  a, b
        ld   h, a                   ; new byte
        ld   l, c                   ; column
        ld   a, (_la_li)
        call _beam_set_asm
        pop  bc
        pop  de
        pop  hl
        jr   47$
    46$:
        dec  6 (ix)                 ; delay countdown in the low bits of len
        ld   a, 6 (ix)
        and  a, #7
        jr   nz, 49$
        ld   a, (_rnd)
        and  a, #3
        or   a, #4
        or   a, 6 (ix)
        ld   6 (ix), a
        set  2, (hl)                ; start this eraser
        jr   49$
    47$:
        inc  hl
        inc  de
        djnz 43$
        ld   a, c
        cp   a, #3
        jr   nz, 49$
        ld   0 (ix), #0             ; all erasers caught up: laser done
    49$:
        pop  ix
        ret
    __endasm;
}

/* ------------------------------------------------------------------ Jetman */
static void jet_redraw(void)
{
    if (in_fire) laser_new_if_free();
}

static void fly_inc_x(void)
{
    u8 a = (8 - velmod) + jet.v1;
    jet.v1 = (a >= 0x40) ? 0x40 : a;
}

static void fly_flip_x(void)
{
    u8 a = (velmod - 8) + jet.v1;
    if (!(a & 0x80)) { jet.v1 = a; return; }
    jet.v1 = 0;
    jet.s ^= 0x40;
}

static void vel_inc_y(void)
{
    u8 a = (8 - velmod) + jet.v2;
    jet.v2 = (a >= 0x3F) ? 0x3F : a;
}

static void dir_flip_y(void)
{
    u8 a = (velmod - 8) + jet.v2;
    if (!(a & 0x80)) { jet.v2 = a; return; }
    jet.v2 = 0;
    jet.s ^= 0x80;
}

static void jet_fly(void)
{
    u16 pos; u8 a, e;
    if (in_right) { jet.t &= ~0x40; if (jet.s & 0x40) fly_flip_x(); else fly_inc_x(); }
    else if (in_left) { jet.t |= 0x40; if (jet.s & 0x40) fly_inc_x(); else fly_flip_x(); }
    else if (game_timer & 1) {
        a = (velmod - 8) + jet.v1;
        jet.v1 = (a & 0x80) ? 0 : a;
    }
    pos = ((u16)jet.x << 8) | sub_x;
    if (jet.s & 0x40) pos -= (u16)jet.v1 << 3; else pos += (u16)jet.v1 << 3;
    sub_x = (u8)pos; jet.x = pos >> 8;

    if (in_down) jet.v2 = 0;
    else if (!in_up) { jet.t |= 0x80; if (jet.s & 0x80) vel_inc_y(); else dir_flip_y(); }
    else { jet.t &= ~0x80; if (jet.s & 0x80) dir_flip_y(); else vel_inc_y(); }
    pos = ((u16)jet.y << 8) | sub_y;
    if (jet.s & 0x80) pos += (u16)jet.v2 << 3; else pos -= (u16)jet.v2 << 3;
    sub_y = (u8)pos; jet.y = pos >> 8;
    if (jet.y >= 0xC0) jet.s &= ~0x80;
    else if (jet.y < 0x2A) { jet.s |= 0x80; a = jet.v2 >> 1; if (a) jet.v2 = a; }

    e = plat_collide(&jet);
    if (e & 4) {
        if (e & 0x80) { jet.s &= ~0x80; jet.t = (jet.t & 0xC0) | T_WALK; jet.v1 = 0; jet.v2 = 0; }
        else if (e & 0x10) jet.s |= 0x80;
        else jet.s = (jet.s & 0xBF) | ((e ^ 0x40) & 0x40);
    }
    jet_redraw();
}

static void leave_platform(void)
{
    jet.t = (jet.t & 0xC0) | T_FLY;
    if (puff.t == 0) {
        puff.t = 3; puff.x = jet.x; puff.y = jet.y;
        anim_reset(&puff);
    }
    jet.y -= 2;
    jet_redraw();
}

static void jet_walk(void)
{
    u8 e;
    if (in_right) { jet.x++; jet.t &= ~0x40; jet.s &= ~0x40; jet.v1 = 0x20; }
    else if (in_left) { jet.x--; jet.t |= 0x40; jet.s |= 0x40; jet.v1 = 0x20; }
    else jet.v1 = 0;
    if (in_up) { leave_platform(); return; }
    e = plat_collide(&jet);
    if (!(e & 4)) { leave_platform(); return; }
    if (!(e & 8) || jet.v1) { jet_redraw(); return; }
    if (jet.t & 0x40) jet.x--;
    else { jet.x++; jet.v1 = 0x20; }
    jet_redraw();
}

/* $737D PlayerTurnStarts (+ fly) : returns virtual cost */
static u16 jetman_update(void)
{
    u8 t = jet.t & 0x3F;
    if (t == T_FLY) {
        if (begin_delay) {
            begin_delay--;
            if (begin_delay) { hud_flash_label(cur_player, 1); return 10; }
            snd_play(sfx_pickup_item); fire_cost += 1250;
            hud_flash_label(cur_player, 0);
        }
        jet_fly();
        return 330;
    }
    if (t == T_WALK) { jet_walk(); return 260; }
    return 5;
}

/* ------------------------------------------------------------------ passes */
static void spawn_alien(void)
{
    u8 i, a;
    Obj *o;
    if (begin_delay) return;
    a = jet.t & 0x3F;
    if (a != T_FLY && a != T_WALK) return;
    for (i = 0; i < 6; i++) {
        o = &slots[SLOT_ALIEN0 + i];
        if (o->t) continue;
        copy8(o, default_alien);
        a = game_timer & 0x40;
        o->s = a; o->t = a;
        o->y = (game_timer & 0x7F) + 0x28;
        a = (((0x50 + i * 8) >> 3) & 3) + 2;
        o->c = a;
        o->v2 = a & 1;
        o->t |= level_alien_type[level & 7];
        return;
    }
}

/* end of an object pass: $6986.. in NewActor */
static void end_of_pass(void)
{
    game_timer++;
    rnd_next();
    rnd += game_timer;
    if (alien_count < 3) spawn_alien();
    else if ((rnd & 0x1F) == 0 && alien_count < 6) spawn_alien();
    item_new_fuel_pod();
    item_new_collectible();
}

static void update_slot(u8 i)
{
    Obj *o = &slots[i];
    switch (o->t & 0x3F) {
    case T_METEOR:  meteor_update(o); break;
    case T_ITEM:    item_pickup(o); break;
    case T_CROSS:   cross_update(o); break;
    case T_SPHERE:  sphere_update(o); break;
    case T_JET:     jet_fighter_update(o); break;
    case T_EXPLODE: anim_explosion(o); break;
    case T_ROCKET:  rocket_update(o); break;
    case T_TAKEOFF: rocket_takeoff(o); break;
    case T_LANDING: rocket_landing(o); break;
    case T_COLLECT: item_check_collect(o); break;
    case T_UFO:     ufo_update(o); break;
    case T_SQUIDGY: squidgy_update(o); break;
    default: break;
    }
}

/* one 50Hz logic tick */
void game_tick(void)
{
    u16 fcost;
    u8 i;
    frame_count++;
    fire_cost = 0;
    /* frame sub-pass: Jetman, lasers, explosion SFX */
    fcost = 25;
    rnd_next();
    fcost += jetman_update();
    for (i = 0; i < 4; i++) {
        rnd_next();
        if (lasers[i].t) { laser_animate(i); fcost += 19; } else fcost += 5;
    }
    rnd_next();
    if (sfx_type) {
        if (--sfx_dur == 0) sfx_type = 0;
        fcost += 165;
    } else fcost += 8;
    game_timer++;
    if (game_state != GS_PLAY) return;

    /* object passes, paced by the virtual time budget */
    budget += (s16)FRAME_UNITS - (s16)fcost - (s16)fire_cost;
    fire_cost = 0;
    if (budget > 4000) budget = 4000;
    for (;;) {
        u8 cost;
        if (cur_slot == 0) alien_count = 0;
        cost = slot_cost_tab[slots[cur_slot].t & 0x3F];
        if (budget < (s16)cost) break;
        budget -= cost;
        rnd_next();
        update_slot(cur_slot);
        budget -= (s16)fire_cost; fire_cost = 0;
        if (game_state != GS_PLAY) { budget = 0; break; }
        if (++cur_slot == N_SLOTS) { cur_slot = 0; end_of_pass(); budget -= 8; }
    }
}
