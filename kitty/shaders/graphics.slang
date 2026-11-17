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
    uniform uint image_is_gray)
    : SV_Target {
    float4 color = image.Sample(texcoord);
    if (is_alpha_mask) {
        color = float4(amask_fg, color.r);
        color = vec4_premul(color);
        color = alpha_blend_premul(color, amask_bg_premult);
    } else {
        // Sixteen bit gray is in the red channel alone
        if (image_is_gray != 0u) color.rgb = color.rrr;
        if (image_decode != IMAGE_DECODE_NONE) color.rgb = decode_image(color.rgb, image_decode, image_gamut);
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
