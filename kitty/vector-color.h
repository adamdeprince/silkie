/* Linear extended-sRGB helpers for vector paints and raster samples. */
static float vector_decode_srgb(float x) {
    return x <= .04045f ? x / 12.92f : powf((x + .055f) / 1.055f, 2.4f);
}
static float vector_encode_srgb(float x) {
    return x <= .0031308f ? x * 12.92f : 1.055f * powf(x, 1.f/2.4f) - .055f;
}
static void vector_to_srgb(float *c, unsigned gamut) {
    if (!gamut || gamut > 2) return;
    // D65 primaries, matching shaders/gamut.slang. No gamut clipping here.
    static const double to_xyz[2][9] = {
        {.4865709,.2656677,.1982173,.2289746,.6917385,.0792869,0,.0451134,1.0439444},
        {.6369580,.1446169,.1688810,.2627002,.6779981,.0593017,0,.0280727,1.0609851}
    };
    static const double from_xyz[9] = {3.2404542,-1.5371385,-.4985314,-.9692660,1.8760108,.0415560,.0556434,-.2040259,1.0572252};
    double xyz[3] = {0}, rgb[3] = {0};
    for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 3; j++) xyz[i] += to_xyz[gamut-1][3*i+j] * c[j];
    for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 3; j++) rgb[i] += from_xyz[3*i+j] * xyz[j];
    for (unsigned i = 0; i < 3; i++) c[i] = (float)rgb[i];
}
static float vector_half_to_float(uint16_t h) {
    unsigned exp = (h >> 10) & 31, mant = h & 1023;
    float v = exp == 0 ? ldexpf((float)mant, -24) : exp == 31 ? (mant ? NAN : INFINITY) : ldexpf((float)(1024|mant), (int)exp-25);
    return h & 0x8000 ? -v : v;
}
