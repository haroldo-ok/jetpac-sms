/* Rendering: hardware sprites for every moving object, background tile
 * composition (text and laser beams drawn over the nebula), and a small
 * VRAM write scheduler so all uploads happen safely.
 */
#include <string.h>
#include "SMSlib.h"
#include "game.h"

/* --------------------------------------------------------------- VRAM slots */
#define N_GAME_SLOTS   (448 - HUD_TILE)             /* composites usable in game */
#define N_MENU_SLOTS   (N_GAME_SLOTS + N_SPR_TILES) /* sprite area reusable in menus */
#define MAX_SLOTS      254
#define SLOT_NONE      0xFF
#define GLYPH_NONE     0x3F
#define CF_PLATFORM    0x40
#define CF_FLASH       0x80

static u8 cell_info[768];      /* glyph id (bits 0-5) | platform | flash */
static u8 cell_slot[768];      /* composite slot or SLOT_NONE */
static u16 slot_cell[MAX_SLOTS];
static u8 n_slots;             /* slots available in the current screen mode */
static u8 slot_scan;

u8 beam[4][32];
#define MAX_DIRTY 250
static u16 dirty_list[MAX_DIRTY];
static u8 n_dirty;
static u8 cell_dirty[96];      /* bitmap, one bit per cell */

/* pending VRAM writes (flushed in vblank) */
#define MAX_PTILES 20
#define MAX_PMAP   64
#define MAX_PROW   32
static u8 ptile_data[MAX_PTILES][32];
static const u8 *ptile_src[MAX_PTILES]; /* composed buffer or ROM tile */
static u16 ptile_idx[MAX_PTILES];
static u8 n_ptile;
static u16 prow_addr[MAX_PROW];         /* 4-byte tile-row patches (beams) */
static u8 prow_data[MAX_PROW][4];
static u8 n_prow;
static u16 pmap_addr[MAX_PMAP];
static u16 pmap_val[MAX_PMAP];
static u8 n_pmap;
static u8 display_off_mode;    /* writes go straight to VRAM */

static u8 alien_order;         /* rotates sprite order to spread dropouts */
static u8 label_flash[2];
static u8 last_flash_phase;
u8 render_dirty_hud;

static const u8 item_h[6] = {11, 8, 11, 13, 9, 12};
static const u8 bg_of_attr[8] = {0, 0, 0, BG_PINK, 0, BG_CYAN, 0, BG_WHITE};

/* --------------------------------------------------------------- VRAM helpers */
/* paced tile upload: safe at any time (32 cycles between data port writes) */
static void vram_tile(u16 tile, const u8 *data) __naked
{
    (void)tile; (void)data;
    __asm
        ; hl = tile, de = data
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        set  6, h
        ld   c, #0xBF
        di
        out  (c), l
        out  (c), h
        ei
        ex   de, hl
        ld   c, #0xBE
        ld   b, #32
    1$:
        outi
        nop
        jr   nz, 1$
        ret
    __endasm;
}

/* 32-byte block copy (one tile) */
static void copy32(u8 *dst, const u8 *src) __naked
{
    (void)dst; (void)src;
    __asm
        ; hl = dst, de = src
        ex   de, hl
        ld   bc, #32
        ldir
        ret
    __endasm;
}

/* blend a 40-byte glyph (32 bytes planes + 8 mask rows) over the tile at dst */
static void blend_glyph(const u8 *gl, u8 *dst) __naked
{
    (void)gl; (void)dst;
    __asm
        ; hl = glyph planes (mask rows follow at +32), de = dst
        push ix
        push hl
        pop  ix
        ld   bc, #32
        add  hl, bc            ; hl = mask
        ld   b, #8
    1$:
        ld   a, (hl)
        cpl
        ld   c, a              ; c = ~mask: background pixels to keep
        inc  hl
        ld   a, (de)
        and  a, c
        or   a, 0 (ix)
        ld   (de), a
        inc  de
        ld   a, (de)
        and  a, c
        or   a, 1 (ix)
        ld   (de), a
        inc  de
        ld   a, (de)
        and  a, c
        or   a, 2 (ix)
        ld   (de), a
        inc  de
        ld   a, (de)
        and  a, c
        or   a, 3 (ix)
        ld   (de), a
        inc  de
        inc  ix
        inc  ix
        inc  ix
        inc  ix
        djnz 1$
        pop  ix
        ret
    __endasm;
}

static u16 slot_tile(u8 s)
{
    if (s < N_GAME_SLOTS) return HUD_TILE + s;
    return s - N_GAME_SLOTS;
}

/* one name-table entry; the two data writes are spaced so that this is
 * safe while the display is active (hl = VRAM address, de = value) */
static void vram_word(u16 addr, u16 val) __naked
{
    (void)addr; (void)val;
    __asm
        ld   c, #0xBF
        di
        out  (c), l
        out  (c), h
        ei
        ld   c, #0xBE
        out  (c), e
        nop                         ; padding: 32 cycles between the data writes
        nop
        nop
        nop
        nop
        out  (c), d
        ret
    __endasm;
}

static void map_write(u16 cell, u16 val)
{
    u16 addr = 0x7800 | (cell << 1);
    if (display_off_mode || n_pmap >= MAX_PMAP) {
        vram_word(addr, val);
        return;
    }
    pmap_addr[n_pmap] = addr; pmap_val[n_pmap] = val; n_pmap++;
}

/* --------------------------------------------------------------- composition */
static u8 cbuf[32];

static const u8 *plat_tile_of_cell(u16 cell)
{
    u8 i, col = cell & 31, row = cell >> 5;
    for (i = 0; i < N_PLAT_CELLS; i++)
        if (plat_cells[i][0] == col && plat_cells[i][1] == row)
            return plat_tiles_bin + plat_cells[i][2] * 32;
    return neb_tiles_bin;
}

static u16 base_tile_of_cell(u16 cell)
{
    u8 i, col = cell & 31, row = cell >> 5;
    if (cell_info[cell] & CF_PLATFORM) {
        for (i = 0; i < N_PLAT_CELLS; i++)
            if (plat_cells[i][0] == col && plat_cells[i][1] == row)
                return PLAT_TILE + plat_cells[i][2];
    }
    return NEB_TILE + neb_map_bin[cell];
}

/* Builds the cell's composite tile in dst (base nebula/platform tile, then
 * the text glyph, then any laser beam rows crossing the cell).
 * Returns 1 if the result differs from the plain base tile.
 * Hand-written: this runs for every cell a laser beam touches, every frame.
 *   in : hl = cell (0..767), de = dst (32 bytes)     out: a = has
 */
static u8 compose_cell(u16 cell, u8 *dst) __naked
{
    (void)cell; (void)dst;
    __asm
        push ix
        push hl                     ; [sp] = cell (kept for the beam pass)
        ; ---- info = cell_info[cell]
        push de
        ld   bc, #_cell_info
        add  hl, bc
        ld   c, (hl)                ; c = info
        pop  de                     ; de = dst
        pop  hl                     ; hl = cell
        push hl
        ; ---- base tile pointer -> hl
        bit  6, c                   ; CF_PLATFORM ?
        jr   z, 1$
        push bc
        push de
        call _plat_tile_of_cell     ; hl = cell -> de = tile data
        ex   de, hl
        pop  de
        pop  bc
        jr   2$
    1$:
        push bc
        ld   bc, #_neb_map_bin
        add  hl, bc
        ld   l, (hl)
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl                 ; * 32
        ld   bc, #_neb_tiles_bin
        add  hl, bc
        pop  bc
    2$:
        ; ---- copy the base tile to dst
        push de
        push bc
        ld   bc, #32
        ldir
        pop  bc                     ; c = info
        pop  de                     ; de = dst
        ld   b, #0                  ; b = has
        ld   a, c
        and  a, #0x3F
        cp   a, #0x3F               ; GLYPH_NONE ?
        jr   z, 5$
        inc  b                      ; has = 1
        ; ---- gl = font_bin + g * 40
        push de
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl                 ; g*8
        ld   e, l
        ld   d, h
        add  hl, hl
        add  hl, hl                 ; g*32
        add  hl, de                 ; g*40
        ld   de, #_font_bin
        add  hl, de
        pop  de
        bit  7, c                   ; CF_FLASH ?
        jr   z, 3$
        ld   a, (_flash_phase)
        or   a, a
        jr   z, 3$
        ; ---- inverted glyph: white paper, black ink, from the mask rows at gl+32
        push bc
        push de
        ld   bc, #32
        add  hl, bc
        ld   b, #8
    4$:
        ld   a, (hl)
        cpl
        inc  hl
        ld   (de), a
        inc  de
        ld   (de), a
        inc  de
        ld   (de), a
        inc  de
        xor  a, a
        ld   (de), a
        inc  de
        djnz 4$
        pop  de
        pop  bc
        jr   5$
    3$:
        push bc
        push de
        call _blend_glyph           ; hl = gl, de = dst
        pop  de
        pop  bc
    5$:
        ; ---- laser beams.  c = (row << 3) | has, b = 4 lasers, hl = &beam[0][col], ix = lasers
        pop  hl                     ; hl = cell
        ld   a, l
        and  a, #0x1F               ; col
        push bc
        ld   c, a
        ld   b, #0
        ld   a, l
        rlca
        rlca
        rlca
        and  a, #0x07
        ld   l, a
        ld   a, h
        rlca
        rlca
        rlca
        and  a, #0xF8
        or   a, l                   ; a = row
        ld   hl, #_beam
        add  hl, bc                 ; hl = &beam[0][col]
        pop  bc
        add  a, a
        add  a, a
        add  a, a                   ; row << 3
        or   a, b                   ; | has (b was 0/1)
        ld   c, a
        ld   b, #4
        ld   ix, #_lasers
    6$:
        ld   a, (hl)
        or   a, a
        jr   z, 7$                  ; no beam byte in this column
        push hl
        push bc
        push de
        ld   b, a                   ; b = beam byte
        ld   a, 1 (ix)              ; laser y
        and  a, #0xF8
        xor  a, c
        and  a, #0xF8
        jr   nz, 8$                 ; beam is on another row
        ; colour index of the beam (bg palette) -> c
        ld   a, 7 (ix)
        and  a, #0x07
        ld   l, a
        ld   h, #0
        push de
        ld   de, #_bg_of_attr
        add  hl, de
        ld   c, (hl)
        pop  de
        ; de = dst + (y & 7) * 4
        ld   a, 1 (ix)
        and  a, #0x07
        add  a, a
        add  a, a
        add  a, e
        ld   e, a
        jr   nc, 9$
        inc  d
    9$:
        ld   a, b
        cpl
        ld   h, a                   ; h = ~beam
        ld   l, b                   ; l = beam
        ld   a, (de)
        and  a, h
        bit  0, c
        jr   z, 10$
        or   a, l
    10$:
        ld   (de), a
        inc  de
        ld   a, (de)
        and  a, h
        bit  1, c
        jr   z, 11$
        or   a, l
    11$:
        ld   (de), a
        inc  de
        ld   a, (de)
        and  a, h
        bit  2, c
        jr   z, 12$
        or   a, l
    12$:
        ld   (de), a
        inc  de
        ld   a, (de)
        and  a, h
        bit  3, c
        jr   z, 13$
        or   a, l
    13$:
        ld   (de), a
        pop  de
        pop  bc
        set  0, c                   ; has = 1
        pop  hl
        jr   7$
    8$:
        pop  de
        pop  bc
        pop  hl
    7$:
        ; next laser: beam row += 32, laser += 8
        ld   a, l
        add  a, #32
        ld   l, a
        jr   nc, 14$
        inc  h
    14$:
        push de
        ld   de, #8
        add  ix, de
        pop  de
        djnz 6$
        ld   a, c
        and  a, #0x01               ; return has
        pop  ix
        ret
    __endasm;
}

static u8 slot_alloc(u16 cell)
{
    u8 i, s = slot_scan;
    for (i = 0; i < n_slots; i++) {
        if (++s >= n_slots) s = 0;
        if (slot_cell[s] == 0xFFFF) {
            slot_cell[s] = cell; cell_slot[cell] = s; slot_scan = s;
            return s;
        }
    }
    return SLOT_NONE;
}

static const u8 bit_of[8] = {1, 2, 4, 8, 16, 32, 64, 128};

static void mark_dirty(u16 cell)
{
    u8 *bp = &cell_dirty[cell >> 3];
    u8 m = bit_of[(u8)cell & 7];
    if (*bp & m) return;
    if (n_dirty >= MAX_DIRTY) return;
    *bp |= m;
    dirty_list[n_dirty++] = cell;
}

static void process_cell(u16 cell)
{
    u8 s = cell_slot[cell];
    u8 *dst;
    cell_dirty[cell >> 3] &= ~bit_of[(u8)cell & 7];
    /* compose straight into the upload queue when it is in use */
    dst = (display_off_mode || n_ptile >= MAX_PTILES) ? cbuf : ptile_data[n_ptile];
    if (compose_cell(cell, dst)) {
        if (s == SLOT_NONE) {
            s = slot_alloc(cell);
            if (s == SLOT_NONE) return;
            map_write(cell, slot_tile(s));
        }
        if (dst == cbuf) {
            vram_tile(slot_tile(s), cbuf);
        } else {
            ptile_idx[n_ptile] = slot_tile(s);
            ptile_src[n_ptile] = dst;
            n_ptile++;
        }
    } else if (s != SLOT_NONE) {
        slot_cell[s] = 0xFFFF;
        cell_slot[cell] = SLOT_NONE;
        map_write(cell, base_tile_of_cell(cell));
    }
}

static void process_dirty(u8 max_tiles)
{
    u8 i = 0, done = 0;
    while (i < n_dirty) {
        if (!display_off_mode && n_ptile >= max_tiles) break;
        process_cell(dirty_list[i]);
        i++;
    }
    if (i) {
        /* remove processed entries */
        u8 j;
        for (j = 0; i < n_dirty; i++, j++) dirty_list[j] = dirty_list[i];
        n_dirty = j;
    }
    (void)done;
}

/* --------------------------------------------------------------- beams */
void beams_clear(void)
{
    u8 i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 32; j++) beam[i][j] = 0;
}

/* Laser beams change a few bytes per frame; instead of recomposing whole
 * tiles we patch the 4 bytes (one pixel row) of the cell's composite tile.
 * A cell without text gets its slot filled with the plain base tile straight
 * from ROM the first time a beam enters it, then only row patches follow. */
static void queue_full_tile(u16 tile, const u8 *src)
{
    if (display_off_mode || n_ptile >= MAX_PTILES) { vram_tile(tile, src); return; }
    ptile_idx[n_ptile] = tile;
    ptile_src[n_ptile] = src;
    n_ptile++;
}

/* plane masks per attribute colour: plane p is 0xFF when bit p of the
 * palette index is set */
static const u8 plane_mask[8][4] = {
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {(BG_PINK & 1) ? 0xFF : 0, (BG_PINK & 2) ? 0xFF : 0, (BG_PINK & 4) ? 0xFF : 0, (BG_PINK & 8) ? 0xFF : 0},
    {0, 0, 0, 0},
    {(BG_CYAN & 1) ? 0xFF : 0, (BG_CYAN & 2) ? 0xFF : 0, (BG_CYAN & 4) ? 0xFF : 0, (BG_CYAN & 8) ? 0xFF : 0},
    {0, 0, 0, 0},
    {(BG_WHITE & 1) ? 0xFF : 0, (BG_WHITE & 2) ? 0xFF : 0, (BG_WHITE & 4) ? 0xFF : 0, (BG_WHITE & 8) ? 0xFF : 0},
};
static u8 row_col, row_y;

/* queue the patch for pixel row (row_y & 7) of the composite 'tile':
 * base pixels (base = ROM tile), then every beam byte on that row.
 *   hl = base tile, de = tile index
 */
static void queue_row(const u8 *base, u16 tile) __naked
{
    (void)base; (void)tile;
    __asm
        push ix
        ld   a, (_row_y)
        and  a, #7
        add  a, a
        add  a, a
        ld   c, a                   ; c = (y & 7) * 4
        ld   b, #0
        add  hl, bc                 ; hl = base row
        ex   de, hl                 ; de = base row, hl = tile
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, bc                 ; hl = VRAM address of the row
        ld   a, (_n_prow)
        ld   c, a
        push hl
        ld   hl, #_prow_addr
        add  hl, bc
        add  hl, bc
        pop  bc                     ; bc = address
        ld   (hl), c
        inc  hl
        ld   (hl), b
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        ld   bc, #_prow_data
        add  hl, bc                 ; hl = patch bytes
        inc  a
        ld   (_n_prow), a
        ex   de, hl                 ; hl = base row, de = patch
        push de
        ldi
        ldi
        ldi
        ldi
        pop  hl                     ; hl = patch
        ; ---- apply every beam on this row
        ld   ix, #_lasers
        ld   bc, #_beam
        ld   a, (_row_col)
        add  a, c
        ld   c, a
        jr   nc, 1$
        inc  b
    1$:
        ld   a, (_row_y)
        ld   d, a                   ; d = y
        ld   e, #4                  ; e = lasers left
    2$:
        ld   a, (bc)
        or   a, a
        jr   z, 4$                  ; no beam byte here
        ld   a, 1 (ix)
        cp   a, d
        jr   nz, 4$                 ; other row
        push bc
        push de
        push hl
        ld   a, 7 (ix)
        and  a, #7
        add  a, a
        add  a, a
        add  a, #<_plane_mask
        ld   e, a
        ld   a, #>_plane_mask
        adc  a, #0
        ld   d, a                   ; de = plane masks of this colour (hl = patch)
        ld   a, (bc)
        ld   b, a                   ; b = beam byte
        cpl
        ld   c, a                   ; c = ~beam
        ld   a, (hl)
        and  a, c
        ld   (hl), a
        ld   a, (de)
        and  a, b
        or   a, (hl)
        ld   (hl), a
        inc  hl
        inc  de
        ld   a, (hl)
        and  a, c
        ld   (hl), a
        ld   a, (de)
        and  a, b
        or   a, (hl)
        ld   (hl), a
        inc  hl
        inc  de
        ld   a, (hl)
        and  a, c
        ld   (hl), a
        ld   a, (de)
        and  a, b
        or   a, (hl)
        ld   (hl), a
        inc  hl
        inc  de
        ld   a, (hl)
        and  a, c
        ld   (hl), a
        ld   a, (de)
        and  a, b
        or   a, (hl)
        ld   (hl), a
        pop  hl
        pop  de
        pop  bc
    4$:
        push de
        ld   de, #8
        add  ix, de
        pop  de
        ld   a, c
        add  a, #32
        ld   c, a
        jr   nc, 5$
        inc  b
    5$:
        dec  e
        jr   nz, 2$
        pop  ix
        ret
    __endasm;
}

/* does any laser still have a beam byte in cell (yrow = y & 0xF8, col)? */
static u8 cell_has_beam(u8 col, u8 yrow)
{
    const u8 *bm = beam[0] + col;
    const Laser *L = lasers;
    u8 i;
    for (i = 0; i < 4; i++, bm += 32, L++)
        if (*bm && (L->y & 0xF8) == yrow) return 1;
    return 0;
}

static u8 row_v;
static u16 row_cell;
static const u8 *row_base;

/* beam_set: store one beam byte and schedule the matching VRAM update.
 * Assembly-only entry point (not the C calling convention!):
 *   a = laser index, l = column, h = new beam byte, ix = &lasers[li]
 * Preserves ix. Text cells fall back to a full composite (mark_dirty);
 * plain cells get a slot holding the base tile plus 4-byte row patches.
 */
void beam_set_asm(void) __naked
{
    __asm
        push hl
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        ld   de, #_beam
        add  hl, de                 ; hl = beam[li]
        pop  de                     ; e = col, d = v
        ld   a, e
        ld   (_row_col), a
        add  a, l
        ld   l, a
        jr   nc, 20$
        inc  h
    20$:
        ld   a, (hl)
        cp   a, d
        ret  z                      ; unchanged
        ld   (hl), d
        ld   a, d
        ld   (_row_v), a
        ld   a, 1 (ix)
        ld   (_row_y), a
        and  a, #0xF8
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        ld   a, l
        or   a, e
        ld   l, a                   ; hl = cell = row*32 + col
        ld   (_row_cell), hl
        ld   de, #_cell_info
        add  hl, de
        ld   c, (hl)                ; c = info
        ld   a, c
        and  a, #0x3F
        cp   a, #0x3F
        jp   nz, 29$                ; text cell -> full composite
        ld   a, (_display_off_mode)
        or   a, a
        jp   nz, 29$
        ld   a, (_n_prow)
        cp   a, #32                 ; MAX_PROW
        jp   nc, 29$
        ; ---- base tile pointer -> row_base
        ld   hl, (_row_cell)
        bit  6, c                   ; CF_PLATFORM ?
        jr   z, 21$
        call _plat_tile_of_cell     ; hl = cell -> de = tile data
        jr   22$
    21$:
        ld   de, #_neb_map_bin
        add  hl, de
        ld   l, (hl)
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        ld   de, #_neb_tiles_bin
        add  hl, de
        ex   de, hl
    22$:
        ld   (_row_base), de
        ; ---- slot
        ld   hl, (_row_cell)
        ld   de, #_cell_slot
        add  hl, de
        ld   a, (hl)
        cp   a, #0xFF               ; SLOT_NONE ?
        jr   nz, 24$
        ld   a, (_row_v)
        or   a, a
        ret  z                      ; nothing drawn, nothing to clear
        ld   hl, (_row_cell)
        call _slot_alloc            ; hl = cell -> a = slot
        cp   a, #0xFF
        ret  z                      ; out of slots
        call _slot_tile             ; a = slot -> de = tile
        push de
        ld   hl, (_row_base)
        ex   de, hl
        call _queue_full_tile       ; hl = tile, de = base
        pop  de
        push de
        ld   hl, (_row_cell)
        call _map_write             ; hl = cell, de = tile
        pop  de
        jr   28$
    24$:
        ld   b, a                   ; b = slot
        push bc
        call _slot_tile             ; de = tile
        pop  bc
        ld   a, (_row_v)
        or   a, a
        jr   nz, 28$
        push bc
        push de
        ld   a, (_row_y)
        and  a, #0xF8
        ld   l, a
        ld   a, (_row_col)
        call _cell_has_beam         ; a = col, l = y row -> a
        pop  de
        pop  bc
        or   a, a
        jr   nz, 28$
        ; ---- last beam byte left the cell: release the slot
        push de                     ; tile
        ld   l, b
        ld   h, #0
        add  hl, hl
        ld   bc, #_slot_cell
        add  hl, bc
        ld   (hl), #0xFF
        inc  hl
        ld   (hl), #0xFF
        ld   hl, (_row_cell)
        ld   bc, #_cell_slot
        add  hl, bc
        ld   (hl), #0xFF
        ld   hl, (_row_cell)
        call _base_tile_of_cell     ; hl = cell -> de = tile
        ld   hl, (_row_cell)
        call _map_write
        pop  hl                     ; tile
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl                 ; hl = tile * 32
        ex   de, hl
        ld   a, (_n_prow)
        or   a, a
        ret  z
        ld   b, a
        ld   hl, #_prow_addr
    25$:                            ; cancel queued patches of this tile
        ld   a, (hl)
        and  a, #0xE0
        cp   a, e
        jr   nz, 26$
        inc  hl
        ld   a, (hl)
        dec  hl
        cp   a, d
        jr   nz, 26$
        ld   (hl), #0xFF
        inc  hl
        ld   (hl), #0xFF
        dec  hl
    26$:
        inc  hl
        inc  hl
        djnz 25$
        ret
    28$:                            ; de = tile: patch this pixel row
        ld   hl, (_row_base)
        jp   _queue_row
    29$:
        ld   hl, (_row_cell)
        jp   _mark_dirty
    __endasm;
}

/* flush the queued row patches (vblank) */
static void flush_rows(void) __naked
{
    __asm
        ld   a, (_n_prow)
        or   a, a
        ret  z
        ld   hl, #_prow_addr
        ld   de, #_prow_data
    1$:
        ex   af, af'
        inc  hl
        ld   a, (hl)                ; address high byte
        dec  hl
        cp   a, #0xFF
        jr   z, 2$                  ; cancelled entry
        set  6, a                   ; VRAM write command
        ld   b, a
        ld   c, #0xBF
        ld   a, (hl)
        di
        out  (c), a
        out  (c), b
        ei
        ld   c, #0xBE
        ld   a, (de)
        out  (c), a
        inc  de
        nop
        nop
        ld   a, (de)
        out  (c), a
        inc  de
        nop
        nop
        ld   a, (de)
        out  (c), a
        inc  de
        nop
        nop
        ld   a, (de)
        out  (c), a
        inc  de
        nop
        inc  hl
        inc  hl
        ex   af, af'
        dec  a
        jr   nz, 1$
        ld   (_n_prow), a
        ret
    2$:
        inc  hl
        inc  hl
        inc  de
        inc  de
        inc  de
        inc  de
        ex   af, af'
        dec  a
        jr   nz, 1$
        ld   (_n_prow), a
        ret
    __endasm;
}


/* Is the screen byte at (x, y) non-empty? (what the original reads from the
 * display file to stop a laser pulse): another beam, a platform, or the box
 * of any drawn object. Called for every active laser head each frame.
 *   a = x, l = y  ->  a = 1 if blocked
 */
u8 screen_byte_nonzero(u8 x, u8 y) __naked
{
    (void)x; (void)y;
    __asm
        push ix
        ld   h, a                   ; h = x, l = y
        ; ---- other beams on this row
        rrca
        rrca
        rrca
        and  a, #0x1F
        ld   c, a                   ; c = column
        ld   b, #0
        ld   ix, #_lasers
        push hl
        ld   hl, #_beam
        add  hl, bc
        ex   de, hl                 ; de = &beam[0][col]
        pop  hl
        ld   b, #4
    1$:
        ld   a, (de)
        or   a, a
        jr   z, 2$
        ld   a, 1 (ix)
        cp   a, l
        jp   z, 90$
    2$:
        ld   a, e
        add  a, #32
        ld   e, a
        jr   nc, 3$
        inc  d
    3$:
        push de
        ld   de, #8
        add  ix, de
        pop  de
        djnz 1$
        ; ---- platform cell
        ld   a, l
        and  a, #0xF8
        ld   e, a
        ld   d, #0
        ex   de, hl                 ; hl = y & 0xF8, de = x:y
        add  hl, hl
        add  hl, hl
        ld   a, l
        or   a, c
        ld   l, a                   ; hl = cell
        ld   bc, #_cell_info
        add  hl, bc
        bit  6, (hl)                ; CF_PLATFORM
        ex   de, hl
        jp   nz, 90$
        ; ---- objects: box test per slot
        ld   ix, #_slots
        ld   b, #11
    10$:
        push bc
        ld   a, 0 (ix)
        and  a, #0x3F
        jp   z, 19$
        ld   c, 2 (ix)              ; c = bottom row
        ld   d, 1 (ix)              ; d = x
        ld   e, #16                 ; e = width
        cp   a, #T_ITEM
        jr   nz, 11$
        ld   a, d
        and  a, #0xF8
        ld   d, a
        ld   a, c
        sub  a, #15
        jp   15$
    11$:
        cp   a, #T_COLLECT
        jr   nz, 12$
        ld   a, 3 (ix)
        or   a, a
        jp   z, 19$
        ld   a, d
        and  a, #0xF8
        ld   d, a
        ld   a, c
        sub  a, #12
        jp   15$
    12$:
        cp   a, #T_EXPLODE
        jr   nz, 13$
        ld   a, 4 (ix)
        cp   a, #3
        jp   nc, 19$
        ld   a, d
        and  a, #0xF8
        ld   d, a
        ld   e, #24
        ld   a, c
        sub  a, #15
        jp   15$
    13$:
        cp   a, #T_ROCKET
        jr   c, 14$
        cp   a, #T_LANDING + 1
        jr   nc, 14$
        ld   a, 4 (ix)              ; modules on the pad
        or   a, a
        jp   z, 19$
        cp   a, #3
        jr   c, 16$
        ld   a, #3
    16$:
        add  a, a
        add  a, a
        add  a, a
        add  a, a
        dec  a                      ; height - 1
        ld   e, a
        ld   a, d
        and  a, #0xF8
        ld   d, a
        ld   a, c
        sub  a, e
        ld   e, #16
        jr   15$
    14$:
        cp   a, #T_METEOR
        jr   z, 17$
        cp   a, #T_CROSS
        jr   z, 17$
        cp   a, #T_SPHERE
        jr   z, 17$
        cp   a, #T_JET
        jr   z, 17$
        cp   a, #T_UFO
        jr   z, 17$
        cp   a, #T_SQUIDGY
        jr   nz, 19$
    17$:
        push hl
        ld   a, (_level)
        and  a, #7
        add  a, a
        bit  2, d
        jr   z, 18$
        inc  a
    18$:
        ld   l, a
        ld   h, #0
        push de
        ld   de, #_alien_heights
        add  hl, de
        ld   a, (hl)
        pop  de
        pop  hl
        ld   e, a                   ; e = sprite height
        ld   a, d
        and  a, #0xFC
        bit  6, 0 (ix)
        jr   z, 20$
        add  a, #4
    20$:
        ld   d, a
        ld   a, c
        sub  a, e
        inc  a                      ; top row
        ld   e, #16
    15$:
        ; a = top, c = bottom, d = left, e = width, h = x, l = y
        ld   b, a
        ld   a, l
        cp   a, b
        jr   c, 19$                 ; above the box
        ld   a, c
        cp   a, l
        jr   c, 19$                 ; below the box
        ld   a, h
        sub  a, d
        cp   a, e
        jr   c, 21$                 ; x inside [left, left+width)
        ld   a, d
        sub  a, h
        cp   a, #8
        jr   c, 21$                 ; the 8-pixel byte overlaps the left edge
    19$:
        pop  bc
        ld   de, #8
        add  ix, de
        dec  b
        jp   nz, 10$
        pop  ix
        xor  a, a
        ret
    21$:
        pop  bc
    90$:
        pop  ix
        ld   a, #1
        ret
    __endasm;
}

/* --------------------------------------------------------------- text */
static void text_put(u8 col, u8 row, u8 glyph, u8 flags)
{
    u16 cell = ((u16)row << 5) | col;
    u8 v = (glyph == 0xFF ? GLYPH_NONE : glyph) | flags | (cell_info[cell] & CF_PLATFORM);
    if (cell_info[cell] == v) return;
    cell_info[cell] = v;
    mark_dirty(cell);
}

static void text_str(u8 col, u8 row, const char *s, u8 flags)
{
    while (*s) {
        u8 c = (u8)*s++;
        text_put(col++, row, glyph_of_ascii[c - 32], flags);
    }
}

static void text_digits(u8 col, u8 row, const u8 *bcd)
{
    u8 i;
    for (i = 0; i < 3; i++) {
        text_put(col++, row, bcd[i] >> 4, 0);
        text_put(col++, row, bcd[i] & 15, 0);
    }
}

void hud_score(u8 which)
{
    if (which == 0) text_digits(1, 1, score[0]);
    else if (which == 1) text_digits(13, 1, score[1]);
    else text_digits(25, 1, hiscore);
}

static void hud_lives_at(u8 col, u8 n)
{
    /* the original prints the character '0'+n, so 10 lives show as ':' etc. */
    if (n) { text_put(col, 0, (n < 80) ? glyph_of_ascii[16 + n] : 0xFF, 0); text_put(col + 1, 0, LIFE_GLYPH, 0); }
    else   { text_put(col, 0, 0xFF, 0); text_put(col + 1, 0, 0xFF, 0); }
}

void hud_lives(void)
{
    u8 l1 = cur_player ? lives2 : lives;
    u8 l2 = cur_player ? lives : lives2;
    hud_lives_at(8, l1);
    hud_lives_at(22, l2);
}

void hud_flash_label(u8 player, u8 on)
{
    if (label_flash[player] == on) return;
    label_flash[player] = on;
    if (player == 0) text_str(3, 0, "1UP", on ? CF_FLASH : 0);
    else text_str(27, 0, "2UP", on ? CF_FLASH : 0);
}

static void hud_draw(void)
{
    label_flash[0] = label_flash[1] = 0xFF;
    hud_flash_label(0, 0);
    hud_flash_label(1, 0);
    text_str(15, 0, "HI", 0);
    hud_score(0); hud_score(1); hud_score(2);
    hud_lives();
}

/* --------------------------------------------------------------- screens */
static void screen_reset(u8 in_game)
{
    u16 c;
    u8 i;
    SMS_displayOff();
    display_off_mode = 1;
    n_ptile = 0; n_pmap = 0; n_prow = 0; n_dirty = 0;
    memset(cell_dirty, 0, sizeof(cell_dirty));
    memset(cell_info, GLYPH_NONE, sizeof(cell_info));
    memset(cell_slot, SLOT_NONE, sizeof(cell_slot));
    memset(slot_cell, 0xFF, sizeof(slot_cell));
    if (in_game)
        for (i = 0; i < N_PLAT_CELLS; i++)
            cell_info[((u16)plat_cells[i][1] << 5) | plat_cells[i][0]] |= CF_PLATFORM;
    { u16 ns = in_game ? N_GAME_SLOTS : N_MENU_SLOTS;
      if (ns > MAX_SLOTS) ns = MAX_SLOTS;
      n_slots = (u8)ns; }
    slot_scan = 0;
    beams_clear();
    /* full tilemap: nebula, then the platform cells on top */
    {
        const u8 *nm = neb_map_bin;
        SMS_setAddr(0x7800);
        for (c = 0; c < 768; c++) SMS_setTile(NEB_TILE + *nm++);
    }
    if (in_game)
        for (i = 0; i < N_PLAT_CELLS; i++)
            SMS_setTileatXY(plat_cells[i][0], plat_cells[i][1], PLAT_TILE + plat_cells[i][2]);
    if (in_game) {
        /* the menu / game-over screens borrow the sprite tile area for text,
         * so the whole sprite set is reloaded for every game screen */
        SMS_loadTiles(spr_core_bin, 0, (u16)N_CORE_TILES * 32);          /* bank 2 */
        SMS_mapROMBank(3);                                               /* alien + rocket sets */
        SMS_loadTiles(spr_alien_bin + (u16)(level & 7) * 512, ALIEN_TILE, 512);
        SMS_loadTiles(spr_rocket_bin + (u16)((level >> 2) & 3) * 1152, ROCKET_TILE, 1152);
        SMS_mapROMBank(2);
        SMS_setSpritePaletteColor(13, alien_dyn_pal[level & 7][0]);
        SMS_setSpritePaletteColor(14, alien_dyn_pal[level & 7][1]);
        SMS_setSpritePaletteColor(15, rocket_dyn_pal[(level >> 2) & 3]);
    }
    SMS_initSprites();
    SMS_copySpritestoSAT();
    hud_draw();
}

static void screen_finish(void)
{
    process_dirty(255);
    display_off_mode = 0;
    SMS_displayOn();
}

void render_level_screen(void)
{
    screen_reset(1);
    screen_finish();
}

static const char *menu_lines[6] = {
    "JETPAC GAME SELECTION", "1   1 PLAYER GAME", "2   2 PLAYER GAME",
    "3   ONE CONTROL PAD", "4   TWO CONTROL PADS", "5   START GAME" };
static const u8 menu_rows[6] = {4, 7, 9, 11, 13, 19};
u8 menu_cursor;

void menu_redraw(void)
{
    u8 i;
    for (i = 0; i < 6; i++) {
        u8 fl = 0;
        if (i == 1 && !(options & 1)) fl = CF_FLASH;
        if (i == 2 && (options & 1)) fl = CF_FLASH;
        if (i == 3 && !(options & 2)) fl = CF_FLASH;
        if (i == 4 && (options & 2)) fl = CF_FLASH;
        text_str(6, menu_rows[i], menu_lines[i], fl);
    }
    for (i = 1; i < 6; i++)
        text_put(4, menu_rows[i], (i == menu_cursor + 1) ? GLYPH_CURSOR : 0xFF, 0);
    text_put(0, 23, GLYPH_COPYRIGHT, 0);
    text_str(1, 23, "1983 A.C.G. ALL RIGHTS RESERVED", 0);
}

void render_menu_screen(void)
{
    screen_reset(0);
    menu_redraw();
    screen_finish();
}

void render_gameover_screen(void)
{
    screen_reset(0);
    text_str(7, 14, "GAME OVER PLAYER", 0);
    text_put(24, 14, gameover_player + 1, 0);
    screen_finish();
}

/* --------------------------------------------------------------- init */
void render_init(void)
{
    SMS_displayOff();
    SMS_useFirstHalfTilesforSprites(1);
    SMS_setSpriteMode(SPRITEMODE_TALL);
    SMS_loadBGPalette(pal_bg_bin);
    SMS_loadSpritePalette(spr_pal_core);
    SMS_setBackdropColor(0);
    SMS_loadTiles(spr_core_bin, 0, (u16)N_CORE_TILES * 32);
    SMS_loadTiles(neb_tiles_bin, NEB_TILE, (u16)K_NEBULA * 32);
    SMS_loadTiles(plat_tiles_bin, PLAT_TILE, (u16)N_PLAT_TILES * 32);
    memset(cell_info, GLYPH_NONE, sizeof(cell_info));
    memset(cell_slot, SLOT_NONE, sizeof(cell_slot));
    memset(slot_cell, 0xFF, sizeof(slot_cell));
    memset(cell_dirty, 0, sizeof(cell_dirty));
    n_dirty = 0; n_ptile = 0; n_pmap = 0; n_prow = 0;
    label_flash[0] = label_flash[1] = 0;
    last_flash_phase = 0;
    alien_order = 0;
    display_off_mode = 0;
}

/* --------------------------------------------------------------- sprites */
/* Hardware sprites (8x16) for every moving object, written straight into
 * SMSlib's sprite table buffers. All in assembly: this runs for ~30 sprites
 * every frame.
 *   spr_add_asm: h = x, l = y, a = tile      (clobbers a, bc, de, hl)
 */
static void spr_add_asm(void) __naked
{
    __asm
        ld   c, a                   ; c = tile
        ld   a, (_SpriteNextFree)
        cp   a, #64
        ret  nc                     ; table full
        ld   e, a
        ld   a, l
        cp   a, #0xD1
        ret  z                      ; y-1 would be the SAT terminator
        dec  a
        ld   b, h                   ; b = x
        ld   d, #0
        ld   hl, #_SpriteTableY
        add  hl, de
        ld   (hl), a
        ld   hl, #_SpriteTableXN
        add  hl, de
        add  hl, de
        ld   (hl), b
        inc  hl
        ld   (hl), c
        ld   a, e
        inc  a
        ld   (_SpriteNextFree), a
        ret
    __endasm;
}

/* h = x, l = bottom y, c = module (0..2), e = variant (0..2) */
static void draw_module_asm(void) __naked
{
    __asm
        ld   a, c
        add  a, a
        add  a, c                   ; m * 3
        add  a, e
        add  a, a
        add  a, a
        add  a, #ROCKET_TILE
        ld   c, a                   ; c = first tile
        ld   a, l
        sub  a, #15
        ld   l, a
        push hl
        push bc
        ld   a, c
        call _spr_add_asm
        pop  bc
        pop  hl
        ld   a, h
        add  a, #8
        ld   h, a
        ld   a, c
        add  a, #2
        jp   _spr_add_asm
    __endasm;
}

/* a = item image index, ix = object */
static void draw_item_sprite_asm(void) __naked
{
    __asm
        ld   c, a
        ld   b, #0
        ld   hl, #_item_h
        add  hl, bc
        ld   a, 2 (ix)
        sub  a, (hl)
        inc  a
        ld   e, a                   ; y = bottom - height + 1
        ld   hl, #_item_tiles
        add  hl, bc
        add  hl, bc
        ld   a, 1 (ix)
        and  a, #0xF8
        ld   d, a                   ; x
        ld   c, (hl)
        inc  hl
        ld   b, (hl)
        push bc
        push de
        ex   de, hl
        ld   a, c
        call _spr_add_asm
        pop  hl
        pop  bc
        ld   a, h
        add  a, #8
        ld   h, a
        ld   a, b
        jp   _spr_add_asm
    __endasm;
}

/* ix = alien object */
static void draw_alien_asm(void) __naked
{
    __asm
        ld   a, 1 (ix)
        and  a, #4
        ld   c, #0                  ; c = animation frame
        jr   z, 1$
        inc  c
    1$:
        ld   a, 0 (ix)
        and  a, #0x40
        ld   b, a                   ; b = facing left
        jr   z, 2$
        ld   a, c
        xor  a, #1
        ld   c, a                   ; frame ^= 1 when facing left
    2$:
        ld   a, c
        bit  6, b
        jr   z, 3$
        add  a, #2                  ; left-facing images follow the right ones
    3$:
        add  a, a
        add  a, a
        add  a, #ALIEN_TILE
        ld   e, a                   ; e = first tile
        ld   a, (_level)
        and  a, #7
        add  a, a
        add  a, c
        ld   l, a
        ld   h, #0
        push de
        ld   de, #_alien_heights
        add  hl, de
        ld   a, (hl)                ; sprite height
        pop  de
        ld   l, a
        ld   a, 2 (ix)
        sub  a, l
        inc  a
        ld   l, a                   ; y = bottom - h + 1
        ld   a, 1 (ix)
        and  a, #0xFC
        bit  6, b
        jr   z, 4$
        add  a, #4
    4$:
        ld   h, a                   ; x
        push hl
        push de
        ld   a, e
        call _spr_add_asm
        pop  de
        pop  hl
        ld   a, h
        add  a, #8
        ld   h, a
        ld   a, e
        add  a, #2
        jp   _spr_add_asm
    __endasm;
}

/* ix = explosion object */
static void draw_explosion_asm(void) __naked
{
    __asm
        ld   a, 4 (ix)              ; animation step
        cp   a, #3
        ret  nc
        ld   c, a
        add  a, a
        add  a, c
        ld   e, a
        ld   d, #0
        ld   hl, #_expl_tiles
        add  hl, de
        ex   de, hl                 ; de = tiles of this step
        ld   a, 1 (ix)
        and  a, #0xF8
        ld   h, a
        ld   a, 2 (ix)
        sub  a, #15
        ld   l, a
        ld   b, #3
    1$:
        push bc
        push de
        push hl
        ld   a, (de)
        call _spr_add_asm
        pop  hl
        pop  de
        pop  bc
        inc  de
        ld   a, h
        add  a, #8
        ld   h, a
        djnz 1$
        ret
    __endasm;
}

/* ix = item / falling module object */
static void draw_item_mod_asm(void) __naked
{
    __asm
        ld   a, 6 (ix)
        cp   a, #0x18
        jr   nz, 1$
        xor  a, a                   ; fuel pod image
        jp   _draw_item_sprite_asm
    1$:
        ld   c, #1
        cp   a, #0x10
        jr   nz, 2$
        ld   c, #2
    2$:
        ld   a, 1 (ix)
        and  a, #0xF8
        ld   h, a
        ld   l, 2 (ix)
        ld   e, #0
        jp   _draw_module_asm
    __endasm;
}

static void draw_rocket_asm(void) __naked
{
    __asm
        ld   ix, #_slots            ; rocket object
        ld   a, 4 (ix)              ; modules built
        cp   a, #3
        jr   c, 1$
        ld   a, #3
    1$:
        ld   b, a                   ; b = modules to draw
        ld   c, 5 (ix)              ; c = fuel level
        ld   d, #0                  ; d = bit7: all modules flash, bits0-1: variant
        cp   a, #3
        jr   nz, 2$
        ld   a, c
        or   a, a
        jr   z, 2$
        cp   a, #6
        jr   c, 3$
        ld   a, (_game_timer)
        and  a, #0x10
        ld   d, #0x80
        jr   nz, 3$
        ld   d, #0x82
        jr   3$
    2$:
        ld   c, #0                  ; no fuel shown unless complete
    3$:
        ld   a, b
        or   a, a
        jr   z, 6$
        ld   e, #0                  ; e = module index
    4$:
        bit  7, d
        jr   z, 5$
        ld   a, d
        and  a, #3
        jr   7$
    5$:
        ld   a, e
        add  a, a
        cp   a, c                   ; 2m < fuel ?
        ld   a, #0
        jr   nc, 7$
        ld   a, e
        add  a, a
        inc  a
        cp   a, c                   ; 2m+1 < fuel ?
        ld   a, #1
        jr   nc, 7$
        ld   a, #2
    7$:
        push bc
        push de
        ld   l, a                   ; variant
        ld   a, e
        add  a, a
        add  a, a
        add  a, a
        add  a, a                   ; m * 16
        ld   h, a
        ld   a, 2 (ix)
        sub  a, h                   ; bottom of this module
        ld   h, 1 (ix)
        ld   c, e
        ld   e, l
        ld   l, a
        call _draw_module_asm
        pop  de
        pop  bc
        inc  e
        ld   a, e
        cp   a, b
        jr   c, 4$
    6$:
        ; exhaust flames while taking off / landing
        ld   a, 0 (ix)
        cp   a, #T_TAKEOFF
        jr   z, 8$
        cp   a, #T_LANDING
        ret  nz
    8$:
        ld   a, 2 (ix)
        cp   a, #0xB8
        ret  nc
        add  a, #7
        ld   l, a
        ld   h, 1 (ix)
        ld   de, #_flame_tiles
        ld   a, (_game_timer)
        and  a, #4
        jr   nz, 9$
        inc  de
        inc  de
    9$:
        push hl
        push de
        ld   a, (de)
        call _spr_add_asm
        pop  de
        pop  hl
        inc  de
        ld   a, h
        add  a, #8
        ld   h, a
        ld   a, (de)
        jp   _spr_add_asm
    __endasm;
}

static void draw_jetman_asm(void) __naked
{
    __asm
        ; image = (x & 6) >> 1 | left << 2 | walking << 3
        ld   a, (_jet + 1)
        and  a, #6
        rrca
        ld   c, a
        ld   a, (_jet)
        bit  6, a
        jr   z, 1$
        set  2, c
    1$:
        and  a, #0x3F
        cp   a, #T_WALK
        jr   nz, 2$
        set  3, c
    2$:
        ld   l, c
        ld   h, #0
        ld   e, l
        ld   d, h
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, hl
        add  hl, de
        add  hl, de
        add  hl, de                 ; image * 19
        ld   de, #_jet_parts
        add  hl, de
        ld   b, (hl)                ; number of parts
        inc  hl
        ld   a, (_jet + 1)
        and  a, #0xFE
        ld   d, a                   ; x of the sprite's left edge
        ld   a, (_jet + 2)
        sub  a, #23
        ld   e, a                   ; y of the top
    3$:
        push bc
        push de
        push hl
        ld   a, (hl)
        add  a, d
        ld   d, a
        inc  hl
        ld   a, (hl)
        add  a, e
        ld   e, a
        inc  hl
        ld   a, (hl)
        ex   de, hl
        call _spr_add_asm
        pop  hl
        pop  de
        pop  bc
        inc  hl
        inc  hl
        inc  hl
        djnz 3$
        ret
    __endasm;
}

/* a = slot index */
static void draw_slot_asm(void) __naked
{
    __asm
        ld   l, a
        ld   h, #0
        add  hl, hl
        add  hl, hl
        add  hl, hl
        ld   de, #_slots
        add  hl, de
        push hl
        pop  ix
        ld   a, 0 (ix)
        and  a, #0x3F
        cp   a, #T_METEOR
        jp   z, _draw_alien_asm
        cp   a, #T_CROSS
        jp   z, _draw_alien_asm
        cp   a, #T_SPHERE
        jp   z, _draw_alien_asm
        cp   a, #T_JET
        jp   z, _draw_alien_asm
        cp   a, #T_UFO
        jp   z, _draw_alien_asm
        cp   a, #T_SQUIDGY
        jp   z, _draw_alien_asm
        cp   a, #T_EXPLODE
        jp   z, _draw_explosion_asm
        cp   a, #T_ITEM
        jp   z, _draw_item_mod_asm
        cp   a, #T_COLLECT
        ret  nz
        ld   a, 3 (ix)              ; visible only once coloured
        or   a, a
        ret  z
        ld   a, 6 (ix)
        sub  a, #0x20
        srl  a
        inc  a
        jp   _draw_item_sprite_asm
    __endasm;
}

static void build_sprites(void) __naked
{
    __asm
        push ix
        xor  a, a
        ld   (_SpriteNextFree), a   ; SMS_initSprites()
        ld   a, (_game_state)
        cp   a, #GS_PLAY
        jr   nz, 9$
        ld   a, (_jet)
        and  a, #0x3F
        cp   a, #T_FLY
        jr   z, 1$
        cp   a, #T_WALK
        jr   nz, 2$
    1$:
        call _draw_jetman_asm
    2$:
        ; aliens / explosions in rotating order (spreads 8-per-line dropouts)
        ld   a, (_alien_order)
        ld   b, #7
    3$:
        push bc
        push af
        add  a, #SLOT_PUFF
        call _draw_slot_asm
        pop  af
        inc  a
        cp   a, #7
        jr   nz, 4$
        xor  a, a
    4$:
        pop  bc
        djnz 3$
        ld   a, (_alien_order)
        inc  a
        cp   a, #7
        jr   nz, 5$
        xor  a, a
    5$:
        ld   (_alien_order), a
        ld   a, #SLOT_JEXPL
        call _draw_slot_asm
        ld   a, (_slots)            ; rocket.t
        or   a, a
        call nz, _draw_rocket_asm
        ld   a, #SLOT_ITEMMOD
        call _draw_slot_asm
        ld   a, #SLOT_ITEMCOL
        call _draw_slot_asm
    9$:
        pop  ix
        ret
    __endasm;
}

/* --------------------------------------------------------------- per frame */
void render_frame(void)
{
    u16 c;
    if (flash_phase != last_flash_phase) {
        last_flash_phase = flash_phase;
        for (c = 0; c < 768; c++)
            if ((cell_info[c] & CF_FLASH) && (cell_info[c] & GLYPH_NONE) != GLYPH_NONE) mark_dirty(c);
    }
    process_dirty(MAX_PTILES);
    build_sprites();
}

/* Flush the queued VRAM work. Every write here is paced for the active
 * display, so a frame that overran its vertical blank still updates safely
 * (at worst a tile changes mid-frame). */
void render_vblank(void)
{
    u8 i;
    SMS_copySpritestoSAT();
    for (i = 0; i < n_pmap; i++) vram_word(pmap_addr[i], pmap_val[i]);
    n_pmap = 0;
    for (i = 0; i < n_ptile; i++) vram_tile(ptile_idx[i], ptile_src[i]);
    n_ptile = 0;
    flush_rows();
}
