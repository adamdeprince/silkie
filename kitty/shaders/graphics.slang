#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

import blit_common;
import alpha_blend;
import utils;
import gamut;
import linear2srgb;

extern static const bool is_alpha_mask = false;
extern static const bool texture_is_not_premultiplied = true;


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

uniform Sampler2D image;

// How the values sampled from the texture of an image are encoded, the
// ImageDecode of image-transfer.h, see docs/silkie/precision-image-protocol.rst.
// The texture of an eight bit sRGB image is an sRGB texture, which the GPU
// decodes, as is the half float one of an HDR image.
static const uint IMAGE_DECODE_NONE = 0, IMAGE_DECODE_SRGB = 1, IMAGE_DECODE_PQ = 2, IMAGE_DECODE_HLG = 3;
// SDR white, 1.0 here, is the reference white of ITU-R BT.2408. An HLG image is
// shown as on a display with the nominal peak of 1000 nits, which takes the
// reference white of HLG to that same white.
static const float SDR_WHITE_NITS = 203.f, HLG_PEAK_NITS = 1000.f;

// SMPTE ST 2084, in multiples of SDR white
float3
pq_eotf(float3 v) {
    const float m1 = 2610.f / 16384.f, m2 = 2523.f / 4096.f * 128.f, c1 = 3424.f / 4096.f, c2 = 2413.f / 4096.f * 32.f, c3 = 2392.f / 4096.f * 32.f;
    float3 p = pow(v, float3(1.f / m2));
    return 10000.f / SDR_WHITE_NITS * pow(max(p - c1, float3(0.f)) / (c2 - c3 * p), float3(1.f / m1));
}

// The inverse OETF of HLG and the OOTF of ITU-R BT.2100, in multiples of SDR white
float3
hlg_eotf(float3 v, uint gamut) {
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    float3 e = lerp(v * v / 3.f, (exp((v - c) / a) + b) / 12.f, step(float3(0.5f), v));
    float y = luminance_in_gamut(e, gamut);
    return y > 0.f ? e * (HLG_PEAK_NITS / SDR_WHITE_NITS * pow(y, 0.2f)) : float3(0.f);
}

float3
decode_image(float3 v, uint decode, uint gamut) {
    if (decode == IMAGE_DECODE_SRGB) return srgb2linear(v);
    if (decode == IMAGE_DECODE_PQ) return pq_eotf(v);
    if (decode == IMAGE_DECODE_HLG) return hlg_eotf(v, gamut);
    return v;
}

// The GPU filters the values in a texture before the shader decodes them, which
// is right only when they are linear. An sRGB texture is decoded by the GPU before
// it filters, so eight bit sRGB images, and linear and half float ones, take the
// hardware path. The others (PQ, HLG and high precision sRGB) are filtered here,
// in linear light, when they are scaled: each texel is decoded, then interpolated.
// An image drawn at its natural size has every fragment at the center of a texel,
// where filtering changes nothing, so it takes the hardware path as well.

// The texel at pos decoded to linear light, alpha multiplied in. Outside the image
// it is transparent, as the border color of the texture is.
float4
decoded_texel(int2 pos, int2 size, uint decode, uint gamut, uint is_gray) {
    if (any(pos < int2(0)) || any(pos >= size)) return float4(0.f);
    float4 c = image[pos];
    // Sixteen bit gray is in the red channel alone
    if (is_gray != 0u) c.rgb = c.rrr;
    c.rgb = decode_image(c.rgb, decode, gamut);
    return texture_is_not_premultiplied ? float4(c.rgb * c.a, c.a) : c;
}

// Bilinear interpolation of the decoded texels around pos, in texels
float4
bilinear_decoded(float2 pos, int2 size, uint decode, uint gamut, uint is_gray) {
    const float2 p = pos - 0.5f, base = floor(p), f = p - base;
    const int2 i = int2(base);
    const float4 top = lerp(decoded_texel(i, size, decode, gamut, is_gray), decoded_texel(i + int2(1, 0), size, decode, gamut, is_gray), f.x);
    const float4 bottom = lerp(decoded_texel(i + int2(0, 1), size, decode, gamut, is_gray), decoded_texel(i + int2(1, 1), size, decode, gamut, is_gray), f.x);
    return lerp(top, bottom, f.y);
}

// When an image is shrunk a fragment covers more than one texel, and four texels
// alone would alias: the footprint of the fragment is averaged over a grid of
// bilinear taps, a tap per texel up to this many in each direction. The bound is
// the time a fragment may take: a GPU resets itself when a draw runs too long, and
// an image shrunk to a few pixels would otherwise read every texel in each. Past
// it the taps are spread over the footprint, as the hardware path aliases already.
// Mipmaps built in linear light are the complete answer, see image-tiling.
static const int MAX_TAPS_PER_AXIS = 4;

// The color at texcoord, filtered in linear light over footprint, the size of the
// fragment in texture coordinates. Straight alpha, as the hardware path gives.
float4
sample_decoded(float2 texcoord, float2 footprint, uint decode, uint gamut, uint is_gray) {
    int width, height;
    image.GetDimensions(width, height);
    const int2 size = int2(width, height);
    const float2 pos = texcoord * float2(size), texels = footprint * float2(size);
    const int2 n = clamp(int2(ceil(texels)), int2(1), int2(MAX_TAPS_PER_AXIS));
    float4 sum = float4(0.f);
    for (int y = 0; y < n.y; y++) {
        for (int x = 0; x < n.x; x++) sum += bilinear_decoded(pos + ((float2(x, y) + 0.5f) / float2(n) - 0.5f) * texels, size, decode, gamut, is_gray);
    }
    sum /= float(n.x * n.y);
    if (!texture_is_not_premultiplied) return sum;
    return sum.a > 0.f ? float4(sum.rgb / sum.a, sum.a) : float4(0.f);
}

[shader("fragment")]
float4
fragment_main(
    float2 texcoord: TEXCOORD,
    uniform float3 amask_fg,
    uniform float4 amask_bg_premult,
    uniform float extra_alpha,
    uniform float hdr_headroom,
    uniform float tone_peak,
    uniform uint image_gamut,
    uniform uint output_gamut,
    uniform uint image_decode,
    uniform uint image_is_gray,
    uniform uint filter_decoded)
    : SV_Target {
    // the derivatives before any branch, a branch on a uniform is the same for every fragment anyway
    const float2 footprint = fwidth(texcoord);
    const bool decoded = filter_decoded != 0u && !is_alpha_mask;
    float4 color = decoded ? sample_decoded(texcoord, footprint, image_decode, image_gamut, image_is_gray) : image.Sample(texcoord);
    if (is_alpha_mask) {
        color = float4(amask_fg, color.r);
        color = vec4_premul(color);
        color = alpha_blend_premul(color, amask_bg_premult);
    } else {
        if (!decoded) {
            // Sixteen bit gray is in the red channel alone
            if (image_is_gray != 0u) color.rgb = color.rrr;
            if (image_decode != IMAGE_DECODE_NONE) color.rgb = decode_image(color.rgb, image_decode, image_gamut);
        }
        // HDR images, SDR ones are never above 1. With tone mapping (tone_peak > 0) the
        // range from the knee to the image peak is compressed into the headroom, preserving
        // hue, otherwise what is above the headroom is clipped. See docs/hdr-protocol.rst
        float knee = 0.75f * hdr_headroom, m = max(color.r, max(color.g, color.b));
        if (tone_peak > hdr_headroom && m > knee) {
            float s = 1.f / (hdr_headroom - knee) - 1.f / (tone_peak - knee);
            color.rgb *= (knee + (m - knee) / (1.f + s * (m - knee))) / m;
        }
        color.rgb = min(color.rgb, float3(hdr_headroom));
        // An image in a gamut of its own is converted to that of the window,
        // which has to be done on colors that are not multiplied by alpha
        if (image_gamut != GAMUT_SRGB) {
            // The colors of an image with an HDR transfer function stay brighter than SDR white
            float ceiling = image_decode == IMAGE_DECODE_PQ || image_decode == IMAGE_DECODE_HLG ? hdr_headroom : 1.f;
            if (texture_is_not_premultiplied) color.rgb = convert_from_gamut_upto(color.rgb, image_gamut, output_gamut, ceiling);
            else if (color.a > 0.f) color.rgb = convert_from_gamut_upto(color.rgb / color.a, image_gamut, output_gamut, ceiling) * color.a;
        }
        color.a *= extra_alpha;
    }

    if (texture_is_not_premultiplied) color = vec4_premul(color);

    return color;
}
