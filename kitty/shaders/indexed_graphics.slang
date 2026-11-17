#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

// Renders images whose pixels are indices into the terminal color palette, so
// that they change automatically when the terminal colors change. This is a
// separate program from graphics.slang so that rendering of normal images has
// no overhead because of it.
//
// The same program draws Silkie's palette images, whose pixels are indices into
// a palette of their own, see docs/silkie/palette-image-protocol.rst.

import blit_common;
import background;
import gamut;

// False for the variant used when the image is drawn at its natural size,
// which needs only a single pixel read per fragment.
extern static const bool is_scaled = true;
// True for the variants that draw palette images, with a palette of their own
// rather than the colors of the terminal.
extern static const bool own_palette = false;

struct VSOutput {
    float2 texcoord : TEXCOORD;
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput
vertex_main(uint vertex_id: SV_VertexID, uniform float4 src_rect, uniform float4 dest_rect, ) {
    BlitOutput ans = get_coords_for_blit(vertex_id, src_rect, dest_rect);
    return { ans.texcoord, float4(ans.position[0], ans.position[1], 0.0, 1.0) };
}

// The red channel is the palette index and, when has_alpha is non-zero, the
// green channel is the straight alpha. Palette images have the index alone.
uniform Sampler2D<uint4> image;
// The palette of a palette image: a row of slots, straight alpha, linear as
// the texture is sRGB, in the gamut of the image.
uniform Sampler2D palette;

struct Lookup {
    uint has_alpha, image_gamut, output_gamut;
};

float4
premult_color_at(int2 pos, Lookup l) {
    uint4 px = image[pos];
    if (own_palette) {
        // a palette has every slot an index of its width can name, so the index is always in it
        const float4 c = palette[int2(int(px.r), 0)];
        return float4(convert_from_gamut(c.rgb, l.image_gamut, l.output_gamut) * c.a, c.a);
    }
    float alpha = l.has_alpha != 0 ? float(px.g) / 255.0 : 1.0;
    return float4(indexed_image_color(px.r) * alpha, alpha);
}

[shader("fragment")]
float4
fragment_main(float2 texcoord: TEXCOORD, uniform uint has_alpha, uniform float extra_alpha, uniform uint image_gamut, uniform uint output_gamut)
    : SV_Target {
    const Lookup l = { has_alpha, image_gamut, output_gamut };
    // Integer textures cannot be filtered and anyway the palette lookup must
    // happen before interpolation, so do bilinear filtering here.
    int width, height;
    image.GetDimensions(width, height);
    const int2 limit = int2(width - 1, height - 1);
    // When the image is drawn at its natural size every fragment is at the
    // center of a pixel, so just read that pixel. is_scaled is a compile time
    // constant so this is not a branch in the shader.
    if (!is_scaled) return premult_color_at(clamp(int2(texcoord * float2(width, height)), int2(0, 0), limit), l) * extra_alpha;
    const float2 pos = texcoord * float2(width, height) - 0.5;
    const float2 base = floor(pos);
    const float2 frac = pos - base;
    const int2 p0 = clamp(int2(base), int2(0, 0), limit);
    const int2 p1 = clamp(int2(base) + int2(1, 1), int2(0, 0), limit);
    const float4 top = lerp(premult_color_at(p0, l), premult_color_at(int2(p1.x, p0.y), l), frac.x);
    const float4 bottom = lerp(premult_color_at(int2(p0.x, p1.y), l), premult_color_at(p1, l), frac.x);
    return lerp(top, bottom, frac.y) * extra_alpha;
}
