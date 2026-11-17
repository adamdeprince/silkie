#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

import blit_common;
import alpha_blend;
import utils;
import linear2srgb;
import gamut;
import dither;

struct VSOutput {
    float2 texcoord : TEXCOORD;
    float4 position : SV_Position;
};


[shader("vertex")]
VSOutput
vertex_main(uint vertex_id: SV_VertexID, uniform float4 src_rect, uniform float4 dest_rect,
    uniform float4 spatial_wall, uniform float3x3 spatial_rotation, ) {
    BlitOutput ans = get_coords_for_blit(vertex_id, src_rect, dest_rect);
    if (spatial_wall.x != 0.f) {
        // Keep the existing top-left content in view when the wall grows.
        float pan = spatial_wall.y - 1.f;
        float3 ray = float3((ans.position.x * spatial_wall.y + pan) * spatial_wall.z,
            (ans.position.y * spatial_wall.y - pan) * spatial_wall.z / spatial_wall.w, -1.f);
        // Slang's row-major lowering emits a GLSL matrix times column vector.
        ray = mul(ray, spatial_rotation);
        return {ans.texcoord, float4(ray.x / spatial_wall.z, ray.y * spatial_wall.w / spatial_wall.z, 0.f, -ray.z)};
    }
    return { ans.texcoord, float4(ans.position[0], ans.position[1], 0.0, 1.0) };
}

uniform Sampler2D image;

[shader("fragment")]
float4
fragment_main(float2 texcoord: TEXCOORD, float4 frag_coord: SV_Position, uniform float encode_srgb, uniform uint output_gamut,
    uniform float spatial_anaglyph, uniform float3 dither_levels)
    : SV_Target {
    // The one place the layers of a window become what its surface holds. They are linear sRGB, which in a window
    // in a wide gamut reaches beyond zero and one, see gamut.slang. An HDR window has a linear float backbuffer: its
    // values are brought to the primaries of the window and left linear. Any other window is sRGB encoded, after
    // its values are brought to its primaries and clipped to them. For a window in sRGB neither step changes anything
    float4 color_premul = image.Sample(texcoord);
    if (spatial_anaglyph != 0.f) {
        float luminance = dot(color_premul.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        color_premul.rgb = spatial_anaglyph == 1.f ? float3(luminance, 0.f, 0.f) : float3(0.f, 0.f, luminance);
    }
    // dither_levels are the steps to dither to, zero for no dithering, see output_dither_levels()
    if (encode_srgb == 0.0f) {
        float4 linear = float4(linear_output_from_extended(color_premul.rgb, output_gamut), color_premul.a);
        if (dither_levels.x > 0.f) return dither_linear_premultiplied(linear, frag_coord.xy, dither_levels);
        return linear;
    }
    float3 encoded = linear2srgb(output_from_extended(color_premul.rgb / color_premul.a, output_gamut));
    if (dither_levels.x > 0.f && color_premul.a > 0.f) return dither_premultiplied(encoded, color_premul.a, frag_coord.xy, dither_levels);
    return vec4_premul(encoded, color_premul.a);
}
