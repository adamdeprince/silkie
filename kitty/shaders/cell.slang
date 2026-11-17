#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

// https://github.com/shader-slang/slang/issues/11874
// warnings-disable: 41012

import utils;
import hsluv;
import alpha_blend;
import linear2srgb;
import background;
import gamut;

extern static const bool TEXT_NEW_GAMMA;
extern static const bool TEXT_CONTRAST_IDENTITY;
extern static const uint RENDER_MODE;

static const bool ONLY_FOREGROUND = RENDER_MODE == 2;
static const bool ONLY_BACKGROUND = RENDER_MODE == 1;

// The background color computation, the cell render data, color table and gamma
// LUT as well as most color utility functions live in the background module so
// that they can be shared with the padding shader.

uniform Sampler2D<uint4> sprite_decorations_map;

static const uint SPRITE_INDEX_MASK = 0x7fffffff;
static const uint SPRITE_COLORED_MASK = 0x80000000;
static const uint SPRITE_COLORED_SHIFT = 31u;
// Set in underline_exclusion_pos when the glyph, decorations, and cursor sprite
// are all 0. The block cursor is sprite 0 and its color is already in the
// background. Packed into an existing flat varying to not use another slot.
static const uint SKIP_TEXT_FLAG = 0x80000000u;

struct VertexOutput {
    float4 position : SV_Position;

    float3 background;
    float4 effective_background_premul;
    float effective_text_alpha;
    float3 sprite_pos;
    float3 underline_pos;
    float3 cursor_pos;
    float3 strike_pos;
    nointerpolation uint underline_exclusion_pos; // has SKIP_TEXT_FLAG set for blank cells
    float3 cell_foreground;
    float4 cursor_color_premult;
    float3 decoration_fg;
    float colored_sprite;
    nointerpolation uint4 sprite_indices;
    nointerpolation float spatial_antialias;
};

// Foreground utility functions {{{
uint3
to_sprite_coords(uint idx) {
    uint sprites_per_page = crd.sprites_xnum * crd.sprites_ynum;
    uint z = idx / sprites_per_page;
    uint num_on_last_page = idx - sprites_per_page * z;
    uint y = num_on_last_page / crd.sprites_xnum;
    uint x = num_on_last_page - crd.sprites_xnum * y;
    return uint3(x, y, z);
}

float3
to_sprite_pos(uint2 pos, uint idx) {
    uint3 c = to_sprite_coords(idx);
    float2 s_xpos = float2(float(c.x), float(c.x) + 1.0f) * (1.0f / float(crd.sprites_xnum));
    float2 s_ypos = float2(float(c.y), float(c.y) + 1.0f) * (1.0f / float(crd.sprites_ynum));
    uint texture_height_px = (crd.cell_height + 1u) * crd.sprites_ynum;
    float row_height = 1.0f / float(texture_height_px);
    s_ypos[1] -= row_height; // skip the decorations_exclude row
    return float3(s_xpos[pos.x], s_ypos[pos.y], float(c.z));
}

uint
to_underline_exclusion_pos(uint2 sprite_idx) {
    uint3 c = to_sprite_coords(sprite_idx[0]);
    uint cell_top_px = c.y * (crd.cell_height + 1u);
    return cell_top_px + crd.cell_height;
}

uint
read_sprite_decorations_idx(uint2 sprite_idx) {
    int idx = int(sprite_idx[0] & SPRITE_INDEX_MASK);
    int width, height;
    sprite_decorations_map.GetDimensions(width, height);
    int2 sz = int2(width, height);
    int y = idx / sz[0];
    int x = idx - y * sz[0];
    return sprite_decorations_map[int2(x, y)].r;
}

uint2
get_decorations_indices(uint2 sprite_idx, uint in_url /* [0, 1] */, uint text_attrs) {
    uint decorations_idx = read_sprite_decorations_idx(sprite_idx);
    uint has_decorations = uint(zero_or_one(float(decorations_idx)));
    uint strike_style = ((text_attrs >> STRIKE_SHIFT) & BIT_MASK); // 0 or 1
    uint strike_idx = decorations_idx * strike_style;
    uint underline_style = ((text_attrs >> DECORATION_SHIFT) & DECORATION_MASK);
    underline_style = in_url * crd.url_style + (1u - in_url) * underline_style; // [0, 5]
    uint has_underline = uint(step(0.5f, float(underline_style)));              // [0, 1]
    return has_decorations * uint2(strike_idx, has_underline * (decorations_idx + underline_style));
}
// }}}

// Override foreground colors {{{
// How the foreground is adjusted for contrast with the background, chosen per
// window by crd.fg_override_algo: see minimum_contrast in shaders.c. A uniform
// rather than a specialization constant so that every window can have its own,
// all windows share the same programs and the branch is the same for every
// vertex of a draw.
static const uint FG_OVERRIDE_NONE = 0u;
// text_fg_override_threshold with %: black or white below a luminance difference
static const uint FG_OVERRIDE_LUMINANCE = 1u;
// text_fg_override_threshold with ratio: a minimum WCAG contrast ratio
static const uint FG_OVERRIDE_RATIO = 2u;
// minimum_contrast: a minimum difference in lightness
static const uint FG_OVERRIDE_LIGHTNESS = 3u;

// Colors are converted to CIE XYZ from the colorspace the window draws in. A
// window drawn in extended sRGB has its colors in the sRGB primaries and an
// unmanaged one is taken to be sRGB. A color made here is clipped to the gamut
// of the window.
float3
working_to_xyz(float3 c) {
    uint g = crd.output_gamut;
    if ((g & GAMUT_EXTENDED) != 0u || g == GAMUT_UNMANAGED) g = GAMUT_SRGB;
    return gamut_to_cie_xyz(c, g);
}

float3
xyz_to_working(float3 xyz) {
    uint g = crd.output_gamut;
    if (g == GAMUT_UNMANAGED) return clamp(cie_xyz_to_gamut(xyz, GAMUT_SRGB), 0.0, 1.0);
    if ((g & GAMUT_EXTENDED) == 0u) return clamp(cie_xyz_to_gamut(xyz, g), 0.0, 1.0);
    float3 c = clamp(cie_xyz_to_gamut(xyz, g & 3u), 0.0, 1.0);
    return cie_xyz_to_gamut(gamut_to_cie_xyz(c, g & 3u), GAMUT_SRGB);
}

// A color of the same HSLuv hue and saturation as the one whose XYZ is given, with this lightness
float3
with_lightness(float3 xyz, float lightness) {
    float3 hsl = xyzToHsluv(xyz);
    return xyz_to_working(hsluvToXyz(float3(hsl.x, hsl.y, lightness)));
}

float3
fg_override_luminance(float colored_sprite, float under_luminance, float over_luminance, float3 over) {
    // If the difference in luminance is too small,
    // force the foreground color to be black or white.
    float diff_luminance = abs(under_luminance - over_luminance);
    float override_level = (1.f - colored_sprite) * step(diff_luminance, crd.fg_override_threshold);
    return lerp(over, float3(step(under_luminance, 0.5f)), override_level);
}

float3
fg_override_contrast(float under_luminance, float3 over_xyz, float3 under, float3 over) {
    // The lightest or darkest color of the same hue and saturation that has the
    // minimum contrast ratio, whichever has the higher ratio.
    const float min_contrast_ratio = crd.fg_override_threshold;
    float3 diff = abs(under - over);
    if (contrast_ratio(under_luminance, over_xyz.y) >= min_contrast_ratio || diff.x + diff.y + diff.z <= 0.001) return over;
    float target_lum_a = clamp((under_luminance + 0.05) * min_contrast_ratio - 0.05, 0.0, 1.0);
    float target_lum_b = clamp((under_luminance + 0.05) / min_contrast_ratio - 0.05, 0.0, 1.0);
    float3 result_a = with_lightness(over_xyz, lightness_from_luminance(target_lum_a));
    float3 result_b = with_lightness(over_xyz, lightness_from_luminance(target_lum_b));
    float result_a_ratio = contrast_ratio(under_luminance, working_to_xyz(result_a).y);
    float result_b_ratio = contrast_ratio(under_luminance, working_to_xyz(result_b).y);
    return lerp(result_a, result_b, step(result_a_ratio, result_b_ratio));
}

float3
fg_override_lightness(float under_luminance, float3 over_xyz, float3 over) {
    // minimum_contrast: the lightness, CIE L* from 0 for black to 100 for white,
    // of the foreground must differ from that of the background by at least the
    // percentage. Otherwise the foreground keeps its hue and saturation and moves
    // to the lightness that differs by that much, on its own side of the
    // background if there is room there, else on the side with more room. At 100%
    // that is black or white.
    float wanted = crd.fg_override_threshold * 100.f;
    float lb = lightness_from_luminance(under_luminance), lf = lightness_from_luminance(over_xyz.y);
    if (abs(lf - lb) >= wanted) return over;
    float room_above = 100.f - lb, room_below = lb;
    // Text as light as its background goes to the side with more room
    bool above = lf > lb || (lf == lb && room_above >= room_below);
    float own_room = above ? room_above : room_below, other_room = above ? room_below : room_above;
    if (own_room < wanted && other_room > own_room) above = !above;
    float target = above ? min(lb + wanted, 100.f) : max(lb - wanted, 0.f);
    return with_lightness(over_xyz, target);
}

// over and under are the colors as drawn, with their HDR gains. Contrast is
// measured and changed on the SDR colors and the gain of the foreground is then
// applied to the result, so an HDR brightness keeps its meaning and the
// adjustment does not change as the headroom of the display does.
float3
override_foreground_color(float3 over, float over_gain, float3 under, float under_gain, float colored_sprite) {
    float3 o = over / max(over_gain, 1e-6), u = under / max(under_gain, 1e-6);
    float under_luminance = working_to_xyz(u).y;
    float3 over_xyz = working_to_xyz(o);
    float3 ans;
    switch (crd.fg_override_algo) {
        case FG_OVERRIDE_LUMINANCE: ans = fg_override_luminance(colored_sprite, under_luminance, over_xyz.y, o); break;
        case FG_OVERRIDE_RATIO: ans = fg_override_contrast(under_luminance, over_xyz, u, o); break;
        case FG_OVERRIDE_LIGHTNESS: ans = fg_override_lightness(under_luminance, over_xyz, o); break;
        default: return over;
    }
    return ans * over_gain;
}
// }}}

[shader("vertex")]
VertexOutput
vertex_main(
    [[vk::location(0)]] uint3 colors,
    [[vk::location(1)]] uint2 sprite_idx,
    [[vk::location(2)]] uint is_selected,
    [[vk::location(3)]] int depth,
    [[vk::location(4)]] float phase,
    [[vk::location(5)]] float depth_pixels,
    uint vertex_id: SV_VertexID,
    uint instance_id: SV_InstanceID,
    uniform uint draw_bg_bitfield, uniform float4 spatial_depth, uniform float2 spatial_phase, uniform float spatial_multiplier,
    uniform float2 spatial_pixel_size, ) {
    VertexOutput vo;

    BackgroundOutput bo = compute_background(colors, sprite_idx, is_selected, instance_id, vertex_id);
    vo.position = bo.position;
    float angle = clamp(float(depth) * 0.001f * spatial_multiplier, -spatial_depth.w, spatial_depth.z);
    float disparity = tan(angle * 0.008726646259971648f) / spatial_depth.y;
    if (depth_pixels != 0.f) disparity = clamp(depth_pixels / 1920.f * spatial_multiplier,
        -tan(spatial_depth.w * 0.008726646259971648f) / spatial_depth.y,
        tan(spatial_depth.z * 0.008726646259971648f) / spatial_depth.y);
    vo.position.x += spatial_depth.x * disparity;
    vo.position.y += spatial_phase.x * clamp(float(phase), -spatial_phase.y, spatial_phase.y);
    float3 background_rgb = bo.background_rgb;
    float3 foreground = bo.foreground;
    uint fg_as_uint = bo.fg_as_uint;
    uint text_attrs = bo.text_attrs;
    CellData cell_data = bo.cell_data;
    vo.spatial_antialias = spatial_pixel_size.x > 0.f ? 1.f : 0.f;
    // the HDR gains of foreground and vo.decoration_fg, see override_foreground_color()
    float fg_gain = bo.fg_gain, decoration_gain = 1.f;

    // Foreground {{{
    if (!ONLY_BACKGROUND) { // background does not depend on foreground
        // The character sprite being rendered
        vo.sprite_pos = to_sprite_pos(cell_data.pos, sprite_idx[0] & SPRITE_INDEX_MASK);
        vo.colored_sprite = float((sprite_idx[0] & SPRITE_COLORED_MASK) >> SPRITE_COLORED_SHIFT);

        float has_dim = float((text_attrs >> DIM_SHIFT) & BIT_MASK), has_blink = float((text_attrs >> BLINK_SHIFT) & BIT_MASK);
        // A reverse video block cursor already uses the dimmed foreground as its text color, so fade out
        // the DIM alpha in step with the cursor opacity to avoid dimming twice without un-dimming a fading cursor
        float undim = cell_data.has_block_cursor * cell_data.reverse_video_cursor * crd.cursor_opacity;
        vo.effective_text_alpha =
            crd.inactive_text_alpha * if_one_then(has_dim, lerp(crd.dim_opacity, 1.0, undim), 1.0) * if_one_then(has_blink, crd.blink_opacity, 1.0);
        float in_url = float((is_selected >> 1) & BIT_MASK);
        // an unset decoration color is the foreground, in its gamut
        float decoration_is_fg = float(one_if_equal_zero_otherwise(colors[2] & BYTE_MASK, 0u));
        float3 decoration_fg = if_one_then(decoration_is_fg, foreground, to_color(colors[2], fg_as_uint));
        decoration_gain = if_one_then(in_url, 1.f, if_one_then(decoration_is_fg, fg_gain, 1.f));
        if (is_deep_color(colors[2])) {
            DeepColorValue d = load_deep_color(colors[2]);
            decoration_fg = convert_gamut(d.rgb, d.gamut);
        }
        vo.decoration_fg = if_one_then(in_url, crd.url_color_lin.rgb, decoration_fg);
        // Selection
        float3 selection_color = if_one_then(crd.use_cell_bg_for_selection_fg, bo.bg, crd.highlight_fg_lin.rgb);
        selection_color = if_one_then(crd.use_cell_fg_for_selection_fg, foreground, selection_color);
        foreground = if_one_then(float(is_selected & BIT_MASK), selection_color, foreground);
        vo.decoration_fg = if_one_then(float(is_selected & BIT_MASK), selection_color, vo.decoration_fg);
        // Find
        foreground = if_one_then(bo.in_find_match, bo.find_match_fg, foreground);
        vo.decoration_fg = if_one_then(bo.in_find_match, bo.find_match_fg, vo.decoration_fg);
        float selection_gain = if_one_then(crd.use_cell_fg_for_selection_fg, fg_gain, if_one_then(crd.use_cell_bg_for_selection_fg, bo.bg_gain, 1.f));
        fg_gain = if_one_then(float(is_selected & BIT_MASK), selection_gain, fg_gain);
        decoration_gain = if_one_then(float(is_selected & BIT_MASK), selection_gain, decoration_gain);
        // Underline and strike through (rendered via sprites)
        uint2 decs = get_decorations_indices(sprite_idx, uint(in_url), text_attrs);
        vo.strike_pos = to_sprite_pos(cell_data.pos, decs[0]);
        vo.underline_pos = to_sprite_pos(cell_data.pos, decs[1]);

        // Cursor
        vo.cursor_color_premult = float4(cell_data.cursor.bg * crd.cursor_opacity, crd.cursor_opacity);
        float3 final_cursor_text_color = lerp(foreground, cell_data.cursor.fg, crd.cursor_opacity);
        foreground = if_one_then(cell_data.has_block_cursor, final_cursor_text_color, foreground);
        vo.decoration_fg = if_one_then(cell_data.has_block_cursor, final_cursor_text_color, vo.decoration_fg);
        fg_gain = if_one_then(cell_data.has_block_cursor, lerp(fg_gain, 1.f, crd.cursor_opacity), fg_gain);
        decoration_gain = if_one_then(cell_data.has_block_cursor, lerp(decoration_gain, 1.f, crd.cursor_opacity), decoration_gain);
        uint cursor_sprite = cell_data.cursor_fg_sprite_idx * uint(cell_data.has_cursor);
        vo.cursor_pos = to_sprite_pos(cell_data.pos, cursor_sprite);
        vo.sprite_indices = uint4(sprite_idx[0] & SPRITE_INDEX_MASK, decs[1], cursor_sprite, decs[0]);
        uint skip_text = uint((sprite_idx[0] & SPRITE_INDEX_MASK) == 0u && decs[0] == 0u && decs[1] == 0u && cursor_sprite == 0u);
        vo.underline_exclusion_pos = to_underline_exclusion_pos(sprite_idx) | (skip_text * SKIP_TEXT_FLAG);
        if (ONLY_FOREGROUND && vo.spatial_antialias != 0.f) {
            // Leave a one-pixel transparent apron for fractional glyph coverage.
            // Samples outside this atlas tile are zero, never a neighboring glyph.
            float2 sign = float2(cell_data.pos) * 2.f - 1.f;
            vo.position.xy += float2(sign.x, -sign.y) * spatial_pixel_size;
            float2 apron = sign / float2(crd.cell_width * crd.sprites_xnum, (crd.cell_height + 1u) * crd.sprites_ynum);
            vo.sprite_pos.xy += apron;
            vo.underline_pos.xy += apron;
            vo.cursor_pos.xy += apron;
            vo.strike_pos.xy += apron;
        }
    }
    // }}}

    vo.background = background_rgb;

    // Block elements and the like, which draw pictures from foreground and
    // background, are left alone by minimum_contrast, see render_line() in fonts.c
    uint contrast_exempt = uint(crd.fg_override_algo == FG_OVERRIDE_LIGHTNESS) & ((text_attrs >> AREA_GRAPHIC_SHIFT) & BIT_MASK);
    if (!ONLY_BACKGROUND && crd.fg_override_algo != FG_OVERRIDE_NONE && contrast_exempt == 0u) {
        vo.decoration_fg = override_foreground_color(vo.decoration_fg, decoration_gain, background_rgb, bo.background_gain, vo.colored_sprite);
        foreground = override_foreground_color(foreground, fg_gain, background_rgb, bo.background_gain, vo.colored_sprite);
    }

    if (!ONLY_FOREGROUND) {
        float4 bgpremul = vec4_premul(background_rgb, bo.bg_alpha);
        // draw_bg_bitfield has bit 0 set to draw default bg cells and bit 1 set to draw non-default bg cells
        float cell_has_non_default_bg = 1.f - bo.cell_has_default_bg;
        uint draw_bg_mask = uint(2.f * cell_has_non_default_bg + bo.cell_has_default_bg); // 1 if has default bg else 2
        float draw_bg = step(0.5, float(draw_bg_bitfield & draw_bg_mask));
        // A translucent cell has nothing beneath it unless something is drawn there: in the
        // default bg pass it is drawn as the default bg, in the non-default pass with its
        // alpha over whatever that pass covered, and in a single pass blended over the default bg.
        float translucent = 1.f - step(1.f, bo.cell_alpha);
        float4 default_premul = vec4_premul(crd.default_bg_lin.rgb, calc_background_opacity(crd.bg_colors0));
        float4 translucent_premul = lerp(bgpremul, default_premul, float(one_if_equal_zero_otherwise(draw_bg_bitfield, 1u)));
        translucent_premul = lerp(translucent_premul, bgpremul + default_premul * (1.f - bgpremul.a), float(one_if_equal_zero_otherwise(draw_bg_bitfield, 3u)));
        bgpremul = lerp(bgpremul, translucent_premul, translucent);
        draw_bg = max(draw_bg, translucent);
        bgpremul *= draw_bg;
        vo.effective_background_premul = bgpremul;
    }

    if (!ONLY_BACKGROUND) vo.cell_foreground = foreground;

    return vo;
}

uniform Sampler2DArray sprites;

float4
spatial_sprite_texel(int2 pixel, uint3 tile) {
    int2 first = int2(tile.xy) * int2(crd.cell_width, crd.cell_height + 1u);
    if (any(pixel < first) || any(pixel >= first + int2(crd.cell_width, crd.cell_height))) return float4(0.f);
    float4 value = sprites[int3(pixel, int(tile.z))];
    return float4(value.rgb * value.a, value.a);
}

float4
sample_spatial_sprite(float3 pos, uint index, float antialias) {
    // SampleLevel() as the caller is not uniform across a 2x2 pixel quad, see fragment_main()
    if (antialias == 0.f) return sprites.SampleLevel(pos, 0);
    float2 size = float2(crd.cell_width * crd.sprites_xnum, (crd.cell_height + 1u) * crd.sprites_ynum);
    float2 pixel = pos.xy * size - 0.5f;
    int2 origin = int2(floor(pixel));
    float2 weight = frac(pixel);
    uint3 tile = to_sprite_coords(index);
    float4 value = lerp(lerp(spatial_sprite_texel(origin, tile), spatial_sprite_texel(origin + int2(1, 0), tile), weight.x),
        lerp(spatial_sprite_texel(origin + int2(0, 1), tile), spatial_sprite_texel(origin + int2(1, 1), tile), weight.x), weight.y);
    return float4(value.a > 0.f ? value.rgb / value.a : float3(0.f), value.a);
}
// Scaling factor for the extra text-alpha adjustment for luminance-difference.
static const float text_gamma_scaling = 0.5;

float
clamp_to_unit_float(float x) {
    // Clamp value to suitable output range
    return clamp(x, 0.0f, 1.0f);
}

float4
foreground_contrast_new(float4 over, float3 under, float text_contrast, float text_gamma_adjustment) {
    if (TEXT_CONTRAST_IDENTITY) return over;
    float under_luminance = dot(under, Y);
    float over_lumininace = dot(over.rgb, Y);
    // Apply additional gamma-adjustment scaled by the luminance difference, the darker the foreground the more adjustment we apply.
    // A multiplicative contrast is also available to increase saturation.
    over.a =
        clamp_to_unit_float(lerp(over.a, pow(over.a, text_gamma_adjustment), (1 - over_lumininace + under_luminance) * text_gamma_scaling) * text_contrast);
    return over;
}

float4
foreground_contrast_old(float4 over, float3 under) {
    // Simulation of gamma-incorrect blending
    float under_luminance = dot(under, Y);
    float over_lumininace = dot(over.rgb, Y);
    // This is the original gamma-incorrect rendering, it is the solution of the following equation:
    //
    // linear2srgb(over * overA2 + under * (1 - overA2)) = linear2srgb(over) * over.a + linear2srgb(under) * (1 - over.a)
    // ^ gamma correct blending with new alpha             ^ gamma incorrect blending with old alpha
    over.a = clamp_to_unit_float(
        (srgb2linear(linear2srgb(over_lumininace) * over.a + linear2srgb(under_luminance) * (1.0f - over.a)) - under_luminance) /
        (over_lumininace - under_luminance));
    return over;
}

float4
foreground_contrast(float4 over, float3 under, float text_contrast, float text_gamma_adjustment) {
    if (TEXT_NEW_GAMMA) return foreground_contrast_new(over, under, text_contrast, text_gamma_adjustment);
    return foreground_contrast_old(over, under);
}

float4
load_text_foreground_color(float3 sprite_pos, float colored_sprite, float3 cell_foreground, uint index, float antialias) {
    // For colored sprites use the color from the sprite rather than the text foreground
    // Return non-premultiplied foreground color
    float4 text_fg = sample_spatial_sprite(sprite_pos, index, antialias);
    return float4(lerp(cell_foreground, text_fg.xyz, colored_sprite), text_fg.w);
}

float4
calculate_premul_foreground_from_sprites(
    float3 sprite_pos,
    float3 underline_pos,
    float3 cursor_pos,
    float3 strike_pos,
    uint underline_exclusion_pos,
    float4 text_fg,
    float3 decoration_fg,
    float4 cursor_color_premult,
    float effective_text_alpha, uint4 indices, float antialias, ) {
    // Return premul foreground color from decorations (cursor, underline, strikethrough)
    int width, height, layer;
    sprites.GetDimensions(width, height, layer);
    float3 sz = float3(float(width), float(height), float(layer));

    float underline_alpha = sample_spatial_sprite(underline_pos, indices.y, antialias).w;
    int3 fetch_coord = int3(int(sprite_pos.x * sz.x), int(underline_exclusion_pos), int(sprite_pos.z));
    float underline_exclusion = sprites[fetch_coord].w;

    underline_alpha *= 1.0 - underline_exclusion;
    float strike_alpha = sample_spatial_sprite(strike_pos, indices.w, antialias).w;
    float cursor_alpha = sample_spatial_sprite(cursor_pos, indices.z, antialias).w;

    float combined_alpha = min(text_fg.w + strike_alpha, 1.0);

    float4 ans = alpha_blend(float4(text_fg.rgb, combined_alpha * effective_text_alpha), float4(decoration_fg, underline_alpha * effective_text_alpha));

    return lerp(ans, cursor_color_premult, cursor_alpha * cursor_color_premult.w);
}

float4
adjust_foreground_contrast_with_background(float4 text_fg, float3 bg, float text_contrast, float text_gamma_adjustment) {
    return foreground_contrast(text_fg, bg, text_contrast, text_gamma_adjustment);
}

[shader("fragment")]
float4
fragment_main(VertexOutput vo, uniform float text_contrast, uniform float text_gamma_adjustment, )
    : SV_Target {
    float4 ans_premul = 0;
    if (!ONLY_FOREGROUND) ans_premul = vo.effective_background_premul;

    // The sprite samples below use SampleLevel() as this branch is not uniform
    // across a 2x2 pixel quad at cell boundaries, so implicit derivatives would
    // be undefined. The sprites texture has no mipmaps, so LOD 0 is identical.
    if (!ONLY_BACKGROUND && (vo.underline_exclusion_pos & SKIP_TEXT_FLAG) == 0u) {
        // blend in the foreground color
        float4 text_fg = load_text_foreground_color(vo.sprite_pos, vo.colored_sprite, vo.cell_foreground, vo.sprite_indices.x, vo.spatial_antialias);
        text_fg = adjust_foreground_contrast_with_background(text_fg, vo.background, text_contrast, text_gamma_adjustment);
        float4 text_fg_premul = calculate_premul_foreground_from_sprites(
            vo.sprite_pos,
            vo.underline_pos,
            vo.cursor_pos,
            vo.strike_pos,
            vo.underline_exclusion_pos,
            text_fg,
            vo.decoration_fg,
            vo.cursor_color_premult,
            vo.effective_text_alpha, vo.sprite_indices, vo.spatial_antialias);
        if (ONLY_FOREGROUND) ans_premul = text_fg_premul;
        else ans_premul = alpha_blend_premul(text_fg_premul, ans_premul);
    }
    return ans_premul;
}
