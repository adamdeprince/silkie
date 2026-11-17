#language slang 2026
// Copyright (C) 2026 Adam DePrince
// Distributed under terms of the GPLv3 license.

// Ordered dithering of what is written to a surface with few steps per channel, see the output_dither option
module dither;

// The threshold of an 8x8 Bayer matrix for the pixel at frag_coord, in (-0.5, 0.5). Its 64 values are spread evenly
// over every 8x8 block, so a value a fraction f of a step above one step rounds up on f of the pixels of the block,
// and the pattern is too fine to be seen at one step of 8 bits.
public float
bayer_threshold(float2 frag_coord) {
    uint x = uint(frag_coord.x) & 7u, y = uint(frag_coord.y) & 7u;
    uint v = x ^ y;
    uint index = ((v & 1u) << 5) | ((x & 1u) << 4) | ((v & 2u) << 2) | ((x & 2u) << 1) | ((v & 4u) >> 1) | ((x & 4u) >> 2);
    return (float(index) + 0.5f) / 64.f - 0.5f;
}

// Values that lie this close to a step, as a fraction of a step, are taken to be exact. They are colors the surface
// holds, off only by the rounding on their way to it: up to 0.025 of a step of 8 bits in the 16 bit textures a window
// is drawn into, near black, and up to 0.18 in the sRGB decoding of 8 bit image textures, as measured on an Apple GPU.
static const float exact = 0.25f;

// The color to write, premultiplied with alpha, for straight, a color in the encoding of the surface, not premultiplied,
// quantized to levels steps per channel above zero. A color within exact of a step is rounded to it, so a color the
// surface can hold exactly, solid text and backgrounds among them, comes out exactly as it does without dithering, in a
// transparent window too, where it is premultiplied with an alpha that makes it fall between steps. Further from a step
// the distance is stretched so that it grows from zero at exact to half a step halfway between steps, and the result is
// dithered, rounding up on as many of the pixels as the stretched fraction says. The average is within exact of the
// color and has no jump anywhere along a gradient. Alpha itself is not dithered.
public float4
dither_premultiplied(float3 straight, float alpha, float2 frag_coord, float3 levels) {
    float3 v = straight * levels, nearest = round(v), d = v - nearest;
    float3 far = max(abs(d) - exact, float3(0.f));
    float3 stretched = nearest + sign(d) * far * (0.5f / (0.5f - exact));
    float3 noise = bayer_threshold(frag_coord) * step(float3(1e-6f), far);
    float3 stepped = floor(stretched * alpha + noise + 0.5f);
    return float4(clamp(stepped, float3(0.f), levels) / levels, alpha);
}
