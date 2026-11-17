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

// A color within exact of a step is rounded to it, so a color the surface can hold exactly, solid text and backgrounds
// among them, comes out exactly as it does without dithering. Further from a step the distance is stretched so that it
// grows from zero at exact to half a step halfway between steps, and the result is dithered, rounding up on as many of
// the pixels as the stretched fraction says. The average is within exact of the color and has no jump anywhere along
// a gradient. Returns the color in steps, before rounding, and the noise to add to it before rounding.
static float3
stretched_steps(float3 straight, float3 levels, float2 frag_coord, out float3 noise) {
    float3 v = straight * levels, nearest = round(v), d = v - nearest;
    float3 far = max(abs(d) - exact, float3(0.f));
    noise = bayer_threshold(frag_coord) * step(float3(1e-6f), far);
    return nearest + sign(d) * far * (0.5f / (0.5f - exact));
}

// The color to write, premultiplied with alpha, for straight, a color in the encoding of the surface, not premultiplied,
// quantized to levels steps per channel above zero. In a transparent window a color that is exactly a step is
// premultiplied with an alpha that makes it fall between steps; it is rounded, not dithered, as without dithering.
// Alpha itself is not dithered.
public float4
dither_premultiplied(float3 straight, float alpha, float2 frag_coord, float3 levels) {
    float3 noise;
    float3 stretched = stretched_steps(straight, levels, frag_coord, noise);
    float3 stepped = floor(stretched * alpha + noise + 0.5f);
    return float4(clamp(stepped, float3(0.f), levels) / levels, alpha);
}

static float3
dither_encode(float3 x) {
    return lerp(12.92f * x, 1.055f * pow(x, float3(1.0f / 2.4f)) - 0.055f, step(float3(0.0031308f), x));
}

static float3
dither_decode(float3 x) {
    return lerp(x / 12.92f, pow((x + 0.055f) / 1.055f, float3(2.4f)), step(float3(0.04045f), x));
}

// The same for a surface that holds linear values, floating point ones, which the system encodes and rounds to the
// display: the color is dithered to levels steps of its sRGB encoding and written linear again. Colors brighter than
// SDR white are HDR and are left alone, as are those below zero, of a wide gamut.
public float4
dither_linear_premultiplied(float4 color_premul, float2 frag_coord, float3 levels) {
    if (color_premul.a <= 0.f) return color_premul;
    float3 straight = color_premul.rgb / color_premul.a;
    if (any(straight > float3(1.f)) || any(straight < float3(0.f))) return color_premul;
    float3 noise;
    float3 stretched = stretched_steps(dither_encode(straight), levels, frag_coord, noise);
    float3 stepped = clamp(floor(stretched + noise + 0.5f), float3(0.f), levels) / levels;
    return float4(dither_decode(stepped) * color_premul.a, color_premul.a);
}
