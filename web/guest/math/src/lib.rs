// SPDX-License-Identifier: GPL-3.0-only
//! Math for the emulated Linux guest. The i386 musl logarithm and power
//! routines use x87 instructions, which are costly under CheerpX. Use the
//! full-precision Rust implementations compiled for SSE2 instead.
//! Only the browser guest links these --wrap symbols; native games keep
//! their platform's normal math library.

#[unsafe(no_mangle)]
pub extern "C" fn __wrap_exp(x: f64) -> f64 { libm::exp(x) }
#[unsafe(no_mangle)]
pub extern "C" fn __wrap_log(x: f64) -> f64 { libm::log(x) }
#[unsafe(no_mangle)]
pub extern "C" fn __wrap_pow(x: f64, y: f64) -> f64 { libm::pow(x, y) }
#[unsafe(no_mangle)]
pub extern "C" fn __wrap_expf(x: f32) -> f32 { libm::expf(x) }
#[unsafe(no_mangle)]
pub extern "C" fn __wrap_logf(x: f32) -> f32 { libm::logf(x) }
#[unsafe(no_mangle)]
pub extern "C" fn __wrap_powf(x: f32, y: f32) -> f32 { libm::powf(x, y) }
