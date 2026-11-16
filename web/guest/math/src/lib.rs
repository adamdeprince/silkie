// SPDX-License-Identifier: GPL-3.0-only
//! Math for the emulated Linux guest. Many i386 musl math routines
//! (logarithms, powers, exponentials, inverse trigonometry, rounding, fmod,
//! hypot and scaling) use x87 instructions, which are costly under CheerpX.
//! Use the full-precision Rust implementations compiled for SSE2 instead.
//! Only the browser guest links these --wrap symbols, and each game wraps
//! only the functions listed for it in build-guest.py; native games keep
//! their platform's normal math library.

macro_rules! wrap {
    ($($wrapper:ident => $function:ident($($arg:ident: $ty:ty),+) -> $ret:ty;)+) => {
        $(
            #[unsafe(no_mangle)]
            pub extern "C" fn $wrapper($($arg: $ty),+) -> $ret { libm::$function($($arg),+) }
        )+
    };
}

wrap! {
    __wrap_exp => exp(x: f64) -> f64;
    __wrap_exp2 => exp2(x: f64) -> f64;
    __wrap_log => log(x: f64) -> f64;
    __wrap_log2 => log2(x: f64) -> f64;
    __wrap_log10 => log10(x: f64) -> f64;
    __wrap_pow => pow(x: f64, y: f64) -> f64;
    __wrap_expf => expf(x: f32) -> f32;
    __wrap_logf => logf(x: f32) -> f32;
    __wrap_powf => powf(x: f32, y: f32) -> f32;
    __wrap_asin => asin(x: f64) -> f64;
    __wrap_acos => acos(x: f64) -> f64;
    __wrap_atan => atan(x: f64) -> f64;
    __wrap_atan2 => atan2(y: f64, x: f64) -> f64;
    __wrap_acosf => acosf(x: f32) -> f32;
    __wrap_atan2f => atan2f(y: f32, x: f32) -> f32;
    __wrap_floor => floor(x: f64) -> f64;
    __wrap_ceil => ceil(x: f64) -> f64;
    __wrap_trunc => trunc(x: f64) -> f64;
    __wrap_floorf => floorf(x: f32) -> f32;
    __wrap_ceilf => ceilf(x: f32) -> f32;
    __wrap_truncf => truncf(x: f32) -> f32;
    __wrap_fmod => fmod(x: f64, y: f64) -> f64;
    __wrap_fmodf => fmodf(x: f32, y: f32) -> f32;
    __wrap_hypot => hypot(x: f64, y: f64) -> f64;
    __wrap_hypotf => hypotf(x: f32, y: f32) -> f32;
    __wrap_ldexp => ldexp(x: f64, n: i32) -> f64;
    __wrap_scalbn => scalbn(x: f64, n: i32) -> f64;
}
