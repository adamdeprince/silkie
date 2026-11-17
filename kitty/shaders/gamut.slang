#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

module gamut;

// The gamuts a color or an image can be tagged with, see
// docs/wide-gamut-protocol.rst, and the last one, which only a window has:
// its colorspace is whatever the display is, so nothing is converted for it.
public static const uint GAMUT_SRGB = 0, GAMUT_DISPLAY_P3 = 1, GAMUT_REC2020 = 2, GAMUT_UNMANAGED = 3;
// Added to the gamut of a window in which untagged colors are sRGB whatever
// the gamut of the window is, so that they look as they do in an sRGB window.
// Such a window is drawn in linear sRGB that extends below zero and above one
// to reach the colors outside sRGB, and everything is converted to the gamut
// of the window at the very end, by output_from_extended().
public static const uint GAMUT_EXTENDED = 4;

// Linear RGB of each gamut to CIE XYZ and back, all with the D65 white point
static const float3x3 gamut_to_xyz[3] = {
    float3x3(0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041),
    float3x3(0.4865709, 0.2656677, 0.1982173, 0.2289746, 0.6917385, 0.0792869, 0.0000000, 0.0451134, 1.0439444),
    float3x3(0.6369580, 0.1446169, 0.1688810, 0.2627002, 0.6779981, 0.0593017, 0.0000000, 0.0280727, 1.0609851),
};
static const float3x3 xyz_to_gamut[3] = {
    float3x3(3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252),
    float3x3(2.4934969, -0.9313836, -0.4027108, -0.8294890, 1.7626641, 0.0236247, 0.0358458, -0.0761724, 0.9568845),
    float3x3(1.7166512, -0.3556708, -0.2533663, -0.6666844, 1.6164812, 0.0157685, 0.0176399, -0.0427706, 0.9421031),
};

// A linear color tagged with a gamut, as the window draws it. An untagged
// color is the colorspace of the window, as it always was, so it and a color
// already in the gamut of the window are passed through untouched. Anything
// else is converted to the gamut of the window and clipped to it. In a window
// drawn in extended sRGB a tagged color is brought into the gamut of the
// window in the same way and then written in the sRGB primaries, unclipped.
public float3
convert_from_gamut(float3 c, uint gamut, uint output_gamut) {
    return convert_from_gamut_upto(c, gamut, output_gamut, 1.f);
}

// The same for a color that may be brighter than SDR white, which is clipped
// to the gamut of the window at ceiling instead of one
public float3
convert_from_gamut_upto(float3 c, uint gamut, uint output_gamut, float ceiling) {
    if (gamut == GAMUT_SRGB || gamut == output_gamut || output_gamut == GAMUT_UNMANAGED) return c;
    if ((output_gamut & GAMUT_EXTENDED) == 0u) return clamp(mul(xyz_to_gamut[output_gamut], mul(gamut_to_xyz[gamut], c)), 0.f, ceiling);
    uint window_gamut = output_gamut & 3u;
    if (gamut != window_gamut) c = clamp(mul(xyz_to_gamut[window_gamut], mul(gamut_to_xyz[gamut], c)), 0.f, ceiling);
    return mul(xyz_to_gamut[GAMUT_SRGB], mul(gamut_to_xyz[window_gamut], c));
}

// The luminance, Y, of a linear color in one of the gamuts colors and images are tagged with
public float
luminance_in_gamut(float3 c, uint gamut) {
    return dot(gamut_to_xyz[min(gamut, GAMUT_REC2020)][1], c);
}

// A linear color in one of the gamuts colors are tagged with to CIE XYZ, and back
public float3
gamut_to_cie_xyz(float3 c, uint gamut) {
    return mul(gamut_to_xyz[min(gamut, GAMUT_REC2020)], c);
}

public float3
cie_xyz_to_gamut(float3 xyz, uint gamut) {
    return mul(xyz_to_gamut[min(gamut, GAMUT_REC2020)], xyz);
}

// The last step for a window drawn in extended sRGB: a linear color in the
// sRGB primaries becomes one in the gamut of the window, clipped to it. Any
// other window is left as it is.
public float3
output_from_extended(float3 c, uint output_gamut) {
    if ((output_gamut & GAMUT_EXTENDED) == 0u) return c;
    return clamp(mul(xyz_to_gamut[output_gamut & 3u], mul(gamut_to_xyz[GAMUT_SRGB], c)), 0.f, 1.f);
}

// The same step for an HDR window, whose values stay linear and reach above
// one, where there is no upper bound to clip to: only what lies outside the
// gamut of the window, the values below zero, is clipped. The change of
// primaries is linear, so this works on colors multiplied by alpha as well.
public float3
linear_output_from_extended(float3 c, uint output_gamut) {
    if ((output_gamut & GAMUT_EXTENDED) == 0u) return c;
    return max(mul(xyz_to_gamut[output_gamut & 3u], mul(gamut_to_xyz[GAMUT_SRGB], c)), 0.f);
}
