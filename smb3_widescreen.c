/*
 * Super Mario Bros. 3 (NES) — Native Widescreen (16:9 / 21:9 / 32:9) Extension
 *
 * Implements true full-screen horizontal field-of-view expansion for SMB3:
 *  - Renders the decompressed 15-page horizontal level tilemap ($6000-$794F in
 *    Cartridge WRAM) seamlessly across the ENTIRE widescreen playfield width
 *    (0..width-1), eliminating both the NES PPU left-8-pixel clip seam ($2001)
 *    and the 256px horizontal nametable scroll/attribute clash border.
 *  - Extends SMB3's enemy spawner ($B838/$B842), despawner ($D40F/$D41A),
 *    spawn activation gate ($D3E5), and OAM sprite draw visibility gates
 *    ($D5C6, $B948, $A74F, $A760, $B41F, $B44E) to match the active widescreen
 *    viewport so multi-part enemies (16x32 Koopa Troopas, Piranha Plants with
 *    stems, Venus Fire Traps, etc.) are fully animated and drawn by SMB3's
 *    own 6502 OAM routines across the entire widescreen canvas.
 *  - Renders all 64 OAM sprites in unified widescreen coordinates with full
 *    support for 8x16 banking ($0719-$071E), horizontal/vertical flipping,
 *    and PPU background-priority occlusion (`BEHIND_BG` bit 0x20 for Piranha
 *    Plants inside pipes, sprouting power-ups, and white blocks).
 *  - Integrates with recomp-ui (F1 / Escape menu) for aspect ratio selection
 *    (4:3, 16:9, Fit to Window) and camera boundary anchoring.
 */

#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_video.h"
#include "hw_internal.h"
#include "recomp_runtime_ui.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    SMB3_WS_MAX_W = 1024,
    SMB3_PLAYFIELD_H = 192,
    SMB3_SCREEN_H = 240,
};

/* View modes matching recomp-ui's RecompRuntimeUiViewMode */
static int s_view_mode = RECOMP_RUNTIME_UI_VIEW_FIXED_16_9; /* Default: 16:9 Widescreen (426x240) */
static int s_camera_anchor = 1;                             /* Default: Anchor viewport to level bounds */

/* Effective widescreen viewport bounds in world pixels (updated each frame) */
static int s_active_level = 0;
static int s_view_left = 0;
static int s_view_right = 256;
static int s_level_width = 256;
static int s_spawn_cam_x = -1;
static uint8_t s_last_tileset = 0;

/*
 * Widescreen spawn edge trackers.
 * SMB3's spawner ($B836) scans ONE 16px column per frame:
 *   Scroll_LastDir ($5B) = 0 -> column at cam + $110 (right edge + 16)
 *   Scroll_LastDir ($5B) = 1 -> column at cam - $20  (left edge - 32)
 * Each widescreen edge has its own fake camera that only moves with that edge,
 * so a direction change never sweeps the spawn column through the visible
 * interior (which made killed/despawned enemies pop back in at the 4:3 border).
 */
static int s_spawn_r = -1;          /* fake cam for the right edge (view_right - 256) */
static int s_spawn_l = -1;          /* fake cam for the left edge (view_left) */
static int s_init_r = 0;            /* initial widescreen population sweep pending */
static int s_init_l = 0;
static uint8_t s_spawn_dir = 0;     /* edge processed this frame (fed to $5B read at $B836) */
static unsigned s_frame_parity = 0;

/*
 * Widescreen Sprite Buffer:
 * On the NES PPU, OAM is strictly limited to 64 sprites total, and SMB3 rotates
 * OAM slot allocation ($055D decrementing each frame at L_C969) when multiple
 * objects and multi-part enemies (16x32 Koopas, 6-slot Piranha Plants, Hammer Bros)
 * compete for slots, causing traditional NES sprite flickering.
 *
 * To completely eliminate this flicker in widescreen mode:
 * - Each of the 8 enemy slots has its own dedicated sprite buffer (up to 8 sprites).
 * - As each enemy runs its 6502 draw routine, we snapshot its newly written OAM sprites
 *   directly into `s_curr_enemy_sprites[e]`.
 * - The compositor renders both the native unowned OAM sprites (Mario, items, debris, HUD)
 *   and all active enemy sprites without any 64-sprite or 8-sprites-per-line limit!
 */
typedef struct {
    uint8_t y;
    uint8_t tile;
    uint8_t attr;
    uint8_t x;
} WsSprite;

static int s_building_enemy = -1;
static uint8_t s_oam_y_snap[64];

static int8_t s_curr_oam_owner[64];
static int s_curr_enemy_wx[8];
static int s_curr_cam_x = 0;
static int s_curr_valid = 0;

static WsSprite s_curr_enemy_sprites[8][8];
static int s_curr_enemy_sprite_count[8];

static int8_t s_latched_oam_owner[64];
static int s_latched_enemy_wx[8];
static int s_latched_cam_x = 0;

static WsSprite s_latched_enemy_sprites[8][8];
static int s_latched_enemy_sprite_count[8];
static uint8_t s_latched_enemy_state[8];

/* Per-pixel background opacity mask (1 = opaque BG pixel, 0 = backdrop ci == 0)
 * used to honor OAM `BEHIND_BG` priority (attr bit 0x20) across the full screen. */
static uint8_t s_bg_opaque[SMB3_WS_MAX_W * SMB3_PLAYFIELD_H];

static void smb3_apply_view_mode(void)
{
    switch (s_view_mode) {
    case RECOMP_RUNTIME_UI_VIEW_NATIVE:
        cyc_video_set_mode(NES_VIDEO_STOCK);
        break;
    case RECOMP_RUNTIME_UI_VIEW_FIXED_16_9:
        cyc_video_set_mode(NES_VIDEO_16_9);
        break;
    case RECOMP_RUNTIME_UI_VIEW_ADAPTIVE:
        cyc_video_set_mode(NES_VIDEO_FIT);
        break;
    default:
        cyc_video_set_mode(NES_VIDEO_16_9);
        break;
    }
}

static int smb3_get_view_mode(void *ctx)
{
    (void)ctx;
    return s_view_mode;
}

static bool smb3_set_view_mode(void *ctx, int mode)
{
    (void)ctx;
    if (mode < RECOMP_RUNTIME_UI_VIEW_NATIVE || mode > RECOMP_RUNTIME_UI_VIEW_ADAPTIVE) {
        return false;
    }
    s_view_mode = mode;
    smb3_apply_view_mode();
    return true;
}

/*
 * Check whether SMB3 is currently rendering an active horizontal scrolling
 * course playfield (as opposed to title screen, world map, vertical tower/pipe
 * stage, spade/N-spade bonus game, or screen fade transition).
 */
static int smb3_is_active_horizontal_level(const uint8_t *cpu_ram,
                                           const uint8_t *cart_ram,
                                           size_t cart_len)
{
    if (!cpu_ram || !cart_ram || cart_len < 0x2000) return 0;
    if (!hw_cart.prg || hw_cart.prg_len < 0x40000) return 0;
    if (!hw_cart.chr || hw_cart.chr_len < 0x20000) return 0;

    /* Level_Tileset ($070A): 1..14 are standard playable courses.
     * 0 = World Map, 15+ = Bonus/Spade/Title scenes. */
    uint8_t tileset = cpu_ram[0x070A];
    if (tileset < 1 || tileset > 14) return 0;

    /* Level_7Vertical ($03EF): non-zero in vertical 1-page-wide stages. */
    if (cpu_ram[0x03EF] != 0) return 0;

    /* Verify PPU background rendering is active on playfield scanlines. */
    if ((cyc_render_line_mask(32) & 0x08) == 0) return 0;
    if ((cyc_render_line_mask(120) & 0x08) == 0) return 0;

    /* Verify background palettes are not all black ($0F) during level entry fade. */
    int non_black = 0;
    for (int i = 1; i < 16; ++i) {
        if ((cyc_render_palette(i) & 0x3F) != 0x0F) {
            non_black = 1;
            break;
        }
    }
    if (!non_black) return 0;

    /* Verify live BG CHR bank registers ($0719/$071A) are initialized. */
    if (cpu_ram[0x0719] == 0 && cpu_ram[0x071A] == 0) return 0;

    return 1;
}

/*
 * Compute the unwrapped 16-bit camera X coordinate and widescreen viewport
 * bounds [view_left, view_left + width).
 */
static void smb3_compute_viewport(const uint8_t *cpu_ram, int width,
                                  int *out_cam_x, int *out_view_left,
                                  int *out_level_width)
{
    int num_pages = (int)cpu_ram[0x0372] + 1;
    if (num_pages < 1) num_pages = 1;
    if (num_pages > 15) num_pages = 15;
    int level_width = num_pages * 256;

    int raw_cam = ((int)cpu_ram[0x0012] << 8) | (int)cpu_ram[0x00FD];
    int ppu_x = cyc_render_line_scroll_x(32) & 0xFF;
    int delta = ((ppu_x - (raw_cam & 0xFF) + 128) & 0xFF) - 128;
    int cam_x = raw_cam + delta;
    if (cam_x < 0) cam_x = 0;
    if (cam_x > level_width - 256) cam_x = level_width - 256;

    int view_left = cam_x - (width - 256) / 2;
    if (s_camera_anchor) {
        if (level_width <= width) {
            view_left = (level_width - width) / 2;
        } else {
            if (view_left < 0) view_left = 0;
            if (view_left + width > level_width) view_left = level_width - width;
        }
    }

    if (out_cam_x) *out_cam_x = cam_x;
    if (out_view_left) *out_view_left = view_left;
    if (out_level_width) *out_level_width = level_width;
}

static int smb3_compositor(uint32_t *out, int width, int height,
                           int native_x0, const uint32_t *native, void *user);

/*
 * RAM read hook for SMB3's enemy spawner, despawner, widescreen visibility,
 * and OAM slot ownership tracking.
 */
static uint8_t smb3_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t raw_val)
{
    if (!s_active_level) return raw_val;

    /*
     * 1. Track per-enemy OAM slot writes during the object update/draw loop
     *    (L_C975..L_C9D3, X = 7..0):
     *    - At L_CA46 (LDA $0661,X): start of enemy `e`'s update/draw routine.
     *    - At L_C9C8 (LDA $0661,X): right after JSR $CA46 and JSR $DAF2 return.
     */
    if (addr >= 0x0661 && addr < 0x0669) {
        const uint8_t *cpu_ram = cyc_cpu_ram();
        if (cpu_ram) {
            int e = (int)(addr - 0x0661);
            if (pc == 0xCA46) {
                if (e == 7) {
                    memset(s_curr_oam_owner, -1, sizeof(s_curr_oam_owner));
                    memset(s_curr_enemy_sprite_count, 0, sizeof(s_curr_enemy_sprite_count));
                    s_curr_cam_x = ((int)cpu_ram[0x0012] << 8) | (int)cpu_ram[0x00FD];
                    s_curr_valid = 1;
                }
                s_building_enemy = e;
                for (int s = 0; s < 64; ++s) {
                    s_oam_y_snap[s] = cpu_ram[0x0200 + s * 4];
                }
            } else if (pc == 0xC9C8 && s_building_enemy == e) {
                s_curr_enemy_wx[e] = ((int)cpu_ram[0x0076 + e] << 8) | (int)cpu_ram[0x0091 + e];
                int spr_count = 0;
                /* Enemies are only allocated OAM slots 16..63 ($0240-$02FF).
                 * Slots 0..15 ($0200-$023F) are strictly reserved for Mario and
                 * player objects (fireballs, tail swipes, sprouting items) and
                 * must never be claimed by enemies when 6502 code wraps modulo 256. */
                for (int s = 16; s < 64; ++s) {
                    uint8_t new_y = cpu_ram[0x0200 + s * 4];
                    if (new_y != s_oam_y_snap[s] && new_y < 0xEFu) {
                        s_curr_oam_owner[s] = (int8_t)e;
                        if (spr_count < 8) {
                            s_curr_enemy_sprites[e][spr_count].y = new_y;
                            s_curr_enemy_sprites[e][spr_count].tile = cpu_ram[0x0200 + s * 4 + 1];
                            s_curr_enemy_sprites[e][spr_count].attr = cpu_ram[0x0200 + s * 4 + 2];
                            s_curr_enemy_sprites[e][spr_count].x = cpu_ram[0x0200 + s * 4 + 3];
                            spr_count++;
                        }
                    }
                }
                s_curr_enemy_sprite_count[e] = spr_count;
                s_building_enemy = -1;
            }
        }
    }

    /*
     * 2. Unlock widescreen enemy activation ($D3E5) and OAM sprite drawing
     *    ($D5C6, $B948, $A74F, $A760, $B41F, $B44E) for enemies inside the
     *    widescreen viewport, while keeping real $0651,X flags for Mario
     *    8-bit bounding-box collision ($DD54) so off-4:3 enemies cannot wrap
     *    around modulo 256 and hit Mario.
     */
    if (addr >= 0x0651 && addr < 0x0659) {
        if (pc == 0xD3E5 || pc == 0xD5C6 || pc == 0xB948 ||
            pc == 0xA74F || pc == 0xA760 || pc == 0xB41F || pc == 0xB44E) {
            const uint8_t *cpu_ram = cyc_cpu_ram();
            if (cpu_ram) {
                int e = (int)(addr - 0x0651);
                if (cpu_ram[0x0661 + e] != 0) {
                    int wx = ((int)cpu_ram[0x0076 + e] << 8) | (int)cpu_ram[0x0091 + e];
                    if (wx + 48 >= s_view_left && wx - 48 < s_view_right) {
                        return 0x00;
                    }
                }
            }
        }
    }

    /*
     * 3. Extend SMB3's enemy spawner ($B838/$B842) and despawner ($D40F/$D41A)
     *    camera horizon to match the widescreen viewport.
     */
    if (pc == 0xB836 && addr == 0x005B) {
        if (s_spawn_cam_x < 0) return raw_val;
        return s_spawn_dir;
    } else if (pc == 0xB838 || pc == 0xB842) {
        int fake_cam = s_spawn_cam_x;
        if (fake_cam < 0) return raw_val;
        if (addr == 0x00FD) return (uint8_t)(fake_cam & 0xFF);
        if (addr == 0x0012) return (uint8_t)((fake_cam >> 8) & 0xFF);
    } else if (pc == 0xD40F || pc == 0xD41A) {
        const uint8_t *cpu_ram = cyc_cpu_ram();
        if (!cpu_ram) return raw_val;
        uint8_t side = cpu_ram[0x0001];
        int fake_cam = (side == 0) ? (s_view_right - 256) : s_view_left;
        if (fake_cam < 0) fake_cam = 0;
        if (fake_cam > s_level_width - 256) fake_cam = s_level_width - 256;
        if (addr == 0x00FD) return (uint8_t)(fake_cam & 0xFF);
        if (addr == 0x0012) return (uint8_t)((fake_cam >> 8) & 0xFF);
    }

    return raw_val;
}

static void smb3_power_on(void *ctx)
{
    (void)ctx;
    memset(s_curr_oam_owner, -1, sizeof(s_curr_oam_owner));
    memset(s_curr_enemy_sprite_count, 0, sizeof(s_curr_enemy_sprite_count));
    memset(s_latched_oam_owner, -1, sizeof(s_latched_oam_owner));
    memset(s_latched_enemy_sprite_count, 0, sizeof(s_latched_enemy_sprite_count));
    memset(s_latched_enemy_state, 0, sizeof(s_latched_enemy_state));
    cyc_mod_set_ram_read_hook(smb3_ram_read_hook);
    cyc_render_set_compositor(smb3_compositor, NULL);
    smb3_apply_view_mode();
}

static void smb3_frame_begin(void *ctx)
{
    (void)ctx;
    int width = cyc_video_width();
    const uint8_t *cpu_ram = cyc_cpu_ram();
    size_t cart_len = 0;
    const uint8_t *cart_ram = cyc_cart_ram(&cart_len);

    /*
     * Latch the shadow OAM ownership metadata and per-enemy sprite buffers that
     * were built during the previous frame's CPU logic, right before this frame's
     * NMI copies $0200-$02FF into `ppu.oam` via $4014 DMA.
     */
    if (s_curr_valid) {
        memcpy(s_latched_oam_owner, s_curr_oam_owner, sizeof(s_latched_oam_owner));
        memcpy(s_latched_enemy_wx, s_curr_enemy_wx, sizeof(s_latched_enemy_wx));
        memcpy(s_latched_enemy_sprites, s_curr_enemy_sprites, sizeof(s_latched_enemy_sprites));
        memcpy(s_latched_enemy_sprite_count, s_curr_enemy_sprite_count, sizeof(s_latched_enemy_sprite_count));
        s_latched_cam_x = s_curr_cam_x;
    } else if (cpu_ram) {
        memset(s_latched_oam_owner, -1, sizeof(s_latched_oam_owner));
        memset(s_latched_enemy_sprite_count, 0, sizeof(s_latched_enemy_sprite_count));
        s_latched_cam_x = ((int)cpu_ram[0x0012] << 8) | (int)cpu_ram[0x00FD];
    }
    if (cpu_ram) {
        for (int e = 0; e < 8; ++e) {
            s_latched_enemy_state[e] = cpu_ram[0x0661 + e];
        }
    }
    s_building_enemy = -1;

    if (width <= 256 || !smb3_is_active_horizontal_level(cpu_ram, cart_ram, cart_len)) {
        s_active_level = 0;
        s_spawn_cam_x = -1;
        s_spawn_r = -1;
        s_spawn_l = -1;
        s_last_tileset = 0;
        s_curr_valid = 0;
        return;
    }

    int cam_x = 0, view_left = 0, level_width = 256;
    smb3_compute_viewport(cpu_ram, width, &cam_x, &view_left, &level_width);

    s_active_level = 1;
    s_view_left = view_left;
    s_view_right = view_left + width;
    s_level_width = level_width;

    uint8_t tileset = cpu_ram[0x070A];

    int tgt_r = s_view_right - 256;
    if (tgt_r < 0) tgt_r = 0;
    if (tgt_r > level_width - 256) tgt_r = level_width - 256;
    int tgt_l = s_view_left;
    if (tgt_l < 0) tgt_l = 0;
    if (tgt_l > level_width - 256) tgt_l = level_width - 256;

    /* Level entry / teleport: start both edges at the 4:3 camera and let them
     * sweep outward once, populating the extra widescreen strips. */
    if (s_spawn_r < 0 || tileset != s_last_tileset ||
        abs(s_spawn_r - tgt_r) > width || abs(s_spawn_l - tgt_l) > width) {
        s_spawn_r = cam_x;
        s_spawn_l = cam_x;
        s_init_r = 1;
        s_init_l = 1;
        s_last_tileset = tileset;
    }

    /*
     * Pick which edge SMB3's spawner scans this frame: normally the leading edge
     * (Scroll_LastDir, $5B), exactly like the original game. The trailing edge is
     * only scanned on alternate frames while its one-time initial sweep is pending.
     */
    s_frame_parity++;
    int lead = (cpu_ram[0x005B] == 0) ? 0 : 1;
    int edge = lead;
    if (lead == 0 && s_init_l && (s_frame_parity & 1)) edge = 1;
    if (lead == 1 && s_init_r && (s_frame_parity & 1)) edge = 0;

    int *trk = (edge == 0) ? &s_spawn_r : &s_spawn_l;
    int tgt = (edge == 0) ? tgt_r : tgt_l;
    /* Step at most one 16px column per frame so no spawn column is skipped. */
    if (*trk < tgt) {
        *trk += 16;
        if (*trk > tgt) *trk = tgt;
    } else if (*trk > tgt) {
        *trk -= 16;
        if (*trk < tgt) *trk = tgt;
    }
    if (*trk == tgt) {
        if (edge == 0) s_init_r = 0; else s_init_l = 0;
    }

    /* The unscanned edge just follows its widescreen edge (no sweep through
     * the visible area), unless its initial population sweep is still pending. */
    if (edge == 0 && !s_init_l) s_spawn_l = tgt_l;
    if (edge == 1 && !s_init_r) s_spawn_r = tgt_r;

    s_spawn_dir = (uint8_t)edge;
    s_spawn_cam_x = *trk;
}

/*
 * Fetch a single pixel (0..3) from an 8x8 BG tile using SMB3's live playfield
 * MMC3 2KB CHR banks ($0719 for $0000-$07FF, $071A for $0800-$0FFF).
 */
static inline uint8_t smb3_sample_bg_tile_pixel(const uint8_t *cpu_ram,
                                                uint8_t tile, int tx, int ty)
{
    uint32_t bank_1k = (tile < 128)
        ? (uint32_t)(cpu_ram[0x0719] & 0xFEu)
        : (uint32_t)(cpu_ram[0x071A] & 0xFEu);
    uint32_t local_tile = (uint32_t)(tile & 0x7Fu);
    uint32_t chr_off = (bank_1k * 0x400u) + (local_tile * 16u) + (uint32_t)(ty & 7);
    chr_off %= (uint32_t)hw_cart.chr_len;

    uint8_t lo = hw_cart.chr[chr_off];
    uint8_t hi = hw_cart.chr[chr_off + 8u];
    int shift = 7 - (tx & 7);
    return (uint8_t)(((lo >> shift) & 1u) | (((hi >> shift) & 1u) << 1));
}

/*
 * Draw a single 8x16 sprite into the widescreen canvas at (sx, sy), honoring
 * `BEHIND_BG` priority (attr bit 0x20 and Piranha Plant tiles) against `s_bg_opaque`.
 */
/*
 * Check if a sprite is behind background:
 * - Standard NES sprite attribute bit 5 (0x20): used by sprouting mushrooms,
 *   fire flowers, stars, 1-ups, vines, and Mario ducked behind white scenery blocks.
 * - Piranha Plants & Venus Fire Traps (tiles 0x77, 0xE3, 0xF0..0xF7):
 *   Multi-part piranha plant sprites entering or leaving pipes are physically
 *   inside the pipe cylinder and must be drawn behind opaque pipe background tiles.
 */
static inline int smb3_sprite_is_behind_bg(uint8_t tile, uint8_t attr)
{
    int is_piranha = (tile == 0x77u || tile == 0xE3u || (tile >= 0xF0u && tile <= 0xF7u));
    return ((attr & 0x20u) != 0) || is_piranha;
}

/*
 * Draw a single 8x16 sprite into the widescreen canvas at (sx, sy).
 * pass: 0 = behind-BG sprites only, 1 = foreground sprites only.
 */
static void smb3_draw_sprite_8x16(uint32_t *out, int width,
                                  const uint8_t *cpu_ram,
                                  int sx, int sy,
                                  uint8_t tile, uint8_t attr,
                                  int pass)
{
    int behind_bg = smb3_sprite_is_behind_bg(tile, attr);
    if (pass == 0 && !behind_bg) return;
    if (pass == 1 && behind_bg) return;

    int flip_h = (attr & 0x40u) != 0;
    int flip_v = (attr & 0x80u) != 0;
    int pal_idx = attr & 0x03;

    uint8_t top_tile = (uint8_t)(tile & 0xFEu);
    uint8_t bot_tile = (uint8_t)(top_tile | 1u);

    for (int py = 0; py < 16; ++py) {
        int dy = sy + py;
        if (dy < 0 || dy >= SMB3_PLAYFIELD_H) continue;

        int src_y = flip_v ? (15 - py) : py;
        uint8_t sub_tile = (src_y < 8) ? top_tile : bot_tile;
        int row_in_tile = src_y & 7;

        /* SMB3 uses $2000 bit 5 (8x16 sprites). Odd tiles use $1000-$1FFF
         * (1KB banks in $071B..$071E); even tiles use $0000-$0FFF ($0719..$071A). */
        uint32_t bank_1k;
        uint32_t tile_in_bank;
        if ((tile & 1u) != 0) {
            int slot_1k = (sub_tile >> 6) & 3;
            bank_1k = (uint32_t)cpu_ram[0x071B + slot_1k];
            tile_in_bank = (uint32_t)(sub_tile & 0x3Fu);
        } else {
            bank_1k = (sub_tile < 128)
                ? (uint32_t)(cpu_ram[0x0719] & 0xFEu)
                : (uint32_t)(cpu_ram[0x071A] & 0xFEu);
            tile_in_bank = (uint32_t)(sub_tile & 0x7Fu);
        }

        uint32_t chr_off = (bank_1k * 0x400u) + (tile_in_bank * 16u) + (uint32_t)row_in_tile;
        chr_off %= (uint32_t)hw_cart.chr_len;
        uint8_t lo = hw_cart.chr[chr_off];
        uint8_t hi = hw_cart.chr[chr_off + 8u];

        for (int px = 0; px < 8; ++px) {
            int dx = sx + px;
            if (dx < 0 || dx >= width) continue;

            int src_x = flip_h ? px : (7 - px);
            uint8_t ci = (uint8_t)(((lo >> src_x) & 1u) | (((hi >> src_x) & 1u) << 1));
            if (ci == 0) continue;

            out[dy * width + dx] = cyc_render_color(0x10 + pal_idx * 4 + ci);
        }
    }
}

/*
 * Main widescreen frame compositor called at the end of every NES frame.
 */
static int smb3_compositor(uint32_t *out, int width, int height,
                           int native_x0, const uint32_t *native, void *user)
{
    (void)height;
    (void)user;
    if (width <= 256 || width > SMB3_WS_MAX_W) return 0;

    const uint8_t *cpu_ram = cyc_cpu_ram();
    size_t cart_len = 0;
    const uint8_t *cart_ram = cyc_cart_ram(&cart_len);

    if (!smb3_is_active_horizontal_level(cpu_ram, cart_ram, cart_len)) {
        return 0;
    }

    int cam_x = 0, view_left = 0, level_width = 256;
    smb3_compute_viewport(cpu_ram, width, &cam_x, &view_left, &level_width);

    int play_x0 = cam_x - view_left;
    if (play_x0 < 0) play_x0 = 0;
    if (play_x0 + 256 > width) play_x0 = width - 256;
    cyc_render_set_native_origin(play_x0);

    /*
     * Look up the active tileset's metatile quadrant definitions in PRG-ROM.
     * Table at PRG offset 0x3DAA7 (Bank 30 $FAA7) maps Level_Tileset (1..14)
     * to the 8KB MMC3 PRG bank containing the 4x256-byte quadrant tables:
     *   +0x000: Upper-Left  8x8 tile
     *   +0x100: Lower-Left  8x8 tile
     *   +0x200: Upper-Right 8x8 tile
     *   +0x300: Lower-Right 8x8 tile
     */
    uint8_t tileset = cpu_ram[0x070A];
    uint8_t meta_bank = hw_cart.prg[0x3DAA7u + tileset] & 0x1Fu;
    const uint8_t *meta_tbl = hw_cart.prg + ((size_t)meta_bank * 0x2000u) % hw_cart.prg_len;
    const uint8_t *meta_ul = meta_tbl + 0x000u;
    const uint8_t *meta_ll = meta_tbl + 0x100u;
    const uint8_t *meta_ur = meta_tbl + 0x200u;
    const uint8_t *meta_lr = meta_tbl + 0x300u;

    uint32_t backdrop_rgb = cyc_render_color(0);
    uint32_t pal_rgb[16];
    for (int i = 0; i < 16; ++i) {
        pal_rgb[i] = cyc_render_color(i);
    }

    const uint8_t *oam = cyc_render_oam();

    /*
     * Layer 0: Fill the playfield with the backdrop/sky color.
     */
    for (int y = 0; y < SMB3_PLAYFIELD_H; ++y) {
        uint32_t *row_out = &out[y * width];
        for (int x = 0; x < width; ++x) {
            row_out[x] = backdrop_rgb;
        }
    }

    /*
     * Layer 1: Render Behind-BG sprites (pass 0).
     * This includes sprouting mushrooms, fire flowers, stars, 1-ups, vines,
     * and piranha plants entering/leaving pipes. They are painted directly
     * onto the backdrop before any opaque blocks or pipes are rendered.
     */
    if (oam) {
        for (int s = 63; s >= 0; --s) {
            int owner = (int)s_latched_oam_owner[s];
            if (s >= 16 && owner >= 0 && owner < 8) continue;

            uint8_t sy_raw = oam[s * 4 + 0];
            if (sy_raw >= 0xEFu) continue;
            uint8_t tile = oam[s * 4 + 1];
            if (tile == 0xFCu) continue;
            uint8_t attr = oam[s * 4 + 2];
            uint8_t sx_raw = oam[s * 4 + 3];

            int sy = (int)sy_raw + 1;
            int sx = play_x0 + (int)sx_raw;
            smb3_draw_sprite_8x16(out, width, cpu_ram, sx, sy, tile, attr, 0);
        }
    }
    for (int e = 7; e >= 0; --e) {
        if (s_latched_enemy_state[e] < 2) continue;
        int enemy_wx = s_latched_enemy_wx[e];
        int base_rel_x = enemy_wx - s_latched_cam_x;
        int count = s_latched_enemy_sprite_count[e];

        for (int i = 0; i < count; ++i) {
            const WsSprite *spr = &s_latched_enemy_sprites[e][i];
            if (spr->y >= 0xEFu) continue;
            if (spr->tile == 0xFCu) continue;

            int sy = (int)spr->y + 1;
            int diff = (((int)spr->x - (base_rel_x & 0xFF) + 128) & 0xFF) - 128;
            int true_wx = enemy_wx + diff;
            int sx = true_wx - view_left;

            smb3_draw_sprite_8x16(out, width, cpu_ram, sx, sy, spr->tile, spr->attr, 0);
        }
    }

    /*
     * Layer 2: Render opaque background tiles over the backdrop and behind-BG sprites.
     * Question blocks, bumped blocks, ground, and pipes naturally cover the mushroom
     * and piranha plant stem, while sky pixels (ci == 0) leave them visible!
     */
    for (int y = 0; y < SMB3_PLAYFIELD_H; ++y) {
        int wy = cyc_render_line_scroll_y(y);
        if (wy < 0) wy = 0;
        if (wy >= 432) wy = 431;

        int my = wy >> 4;
        int qy = (wy >> 3) & 1;
        int ty = wy & 7;
        uint32_t *row_out = &out[y * width];

        for (int x = 0; x < width; ++x) {
            int wx = view_left + x;
            if (wx < 0 || wx >= level_width) {
                continue;
            }

            int mx = wx >> 4;
            int page = mx >> 4;
            int col = mx & 15;
            uint8_t mt = cart_ram[page * 0x1B0 + my * 16 + col];
            int pal = (mt >> 6) & 3;
            int qx = (wx >> 3) & 1;
            int tx = wx & 7;

            uint8_t tile = qx
                ? (qy ? meta_lr[mt] : meta_ur[mt])
                : (qy ? meta_ll[mt] : meta_ul[mt]);

            uint8_t ci = smb3_sample_bg_tile_pixel(cpu_ram, tile, tx, ty);
            if (ci != 0) {
                row_out[x] = pal_rgb[pal * 4 + ci];
            }
        }
    }

    /*
     * Layer 3: Render Foreground sprites (pass 1).
     * Mario, Goombas, Koopas, active items, and moving debris are drawn on top
     * of all background blocks and terrain.
     */
    if (oam) {
        for (int s = 63; s >= 0; --s) {
            int owner = (int)s_latched_oam_owner[s];
            if (s >= 16 && owner >= 0 && owner < 8) continue;

            uint8_t sy_raw = oam[s * 4 + 0];
            if (sy_raw >= 0xEFu) continue;
            uint8_t tile = oam[s * 4 + 1];
            if (tile == 0xFCu) continue;
            uint8_t attr = oam[s * 4 + 2];
            uint8_t sx_raw = oam[s * 4 + 3];

            int sy = (int)sy_raw + 1;
            int sx = play_x0 + (int)sx_raw;
            smb3_draw_sprite_8x16(out, width, cpu_ram, sx, sy, tile, attr, 1);
        }
    }

    for (int e = 7; e >= 0; --e) {
        if (s_latched_enemy_state[e] < 2) continue;
        int enemy_wx = s_latched_enemy_wx[e];
        int base_rel_x = enemy_wx - s_latched_cam_x;
        int count = s_latched_enemy_sprite_count[e];

        for (int i = 0; i < count; ++i) {
            const WsSprite *spr = &s_latched_enemy_sprites[e][i];
            if (spr->y >= 0xEFu) continue;
            if (spr->tile == 0xFCu) continue;

            int sy = (int)spr->y + 1;
            int diff = (((int)spr->x - (base_rel_x & 0xFF) + 128) & 0xFF) - 128;
            int true_wx = enemy_wx + diff;
            int sx = true_wx - view_left;

            smb3_draw_sprite_8x16(out, width, cpu_ram, sx, sy, spr->tile, spr->attr, 1);
        }
    }

    /*
     * Step 3: Render the status bar / HUD (scanlines 192..239) centered at
     * native_x0, filling the side wings and left-16 border with the HUD
     * border background color.
     */
    for (int y = SMB3_PLAYFIELD_H; y < SMB3_SCREEN_H; ++y) {
        uint32_t hud_fill = native[y * 256 + 255];
        uint32_t *row_out = &out[y * width];
        for (int x = 0; x < width; ++x) {
            row_out[x] = hud_fill;
        }
        if (native_x0 >= 0 && native_x0 + 256 <= width) {
            memcpy(&row_out[native_x0 + 16],
                   &native[y * 256 + 16],
                   (248 - 16) * sizeof(uint32_t));
        }
    }

    return 1;
}

static const CycHostExtras k_smb3_extras = {
    .view_modes = RECOMP_RUNTIME_UI_VIEW_MODE_NATIVE |
                  RECOMP_RUNTIME_UI_VIEW_MODE_FIXED_16_9 |
                  RECOMP_RUNTIME_UI_VIEW_MODE_ADAPTIVE,
    .get_view_mode = smb3_get_view_mode,
    .set_view_mode = smb3_set_view_mode,
    .power_on = smb3_power_on,
    .frame_begin = smb3_frame_begin,
};

const CycHostExtras *cyc_host_extras(void)
{
    return &k_smb3_extras;
}
