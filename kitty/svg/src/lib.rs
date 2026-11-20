// Copyright (C) 2026 Adam DePrince
// Distributed under the terms of the GPL3 license.

use resvg::{tiny_skia, usvg};
use std::alloc::{Layout, alloc};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::slice;
use std::sync::{Arc, OnceLock};

#[repr(C)]
pub enum SvgStatus {
    Ok,
    Invalid,
    Overflow,
    NoMemory,
}

static SYSTEM_FONTS: OnceLock<Arc<usvg::fontdb::Database>> = OnceLock::new();

fn parse(data: &[u8], width: u32, height: u32) -> Result<(usvg::Tree, u32, u32), SvgStatus> {
    let text = std::str::from_utf8(data).map_err(|_| SvgStatus::Invalid)?;
    // Use the XML parser's default policy, which rejects DTDs. Only in-document
    // references and embedded raster images can supply graphic content.
    let xml = roxmltree::Document::parse(text).map_err(|_| SvgStatus::Invalid)?;
    let raster = usvg::ImageHrefResolver::default_data_resolver();
    let mut options = usvg::Options {
        image_href_resolver: usvg::ImageHrefResolver {
            resolve_data: Box::new(move |mime, data, opts| match mime {
                "image/png" | "image/jpeg" | "image/jpg" | "image/gif" | "image/webp" => {
                    raster(mime, data, opts)
                }
                _ => None,
            }),
            resolve_string: Box::new(|_, _| None),
        },
        ..Default::default()
    };
    if xml.descendants().any(|node| node.has_tag_name("text")) {
        options.fontdb = Arc::clone(SYSTEM_FONTS.get_or_init(|| {
            let mut fonts = usvg::fontdb::Database::new();
            fonts.load_system_fonts();
            Arc::new(fonts)
        }));
    }
    if width != 0 && height != 0 {
        options.default_size = usvg::Size::from_wh(width as f32, height as f32).unwrap();
    }
    let tree = usvg::Tree::from_xmltree(&xml, &options).map_err(|_| SvgStatus::Invalid)?;
    if width != 0 && height != 0 {
        return Ok((tree, width, height));
    }
    let size = tree.size();
    let (w, h) = (
        f64::from(size.width()).ceil(),
        f64::from(size.height()).ceil(),
    );
    if w > f64::from(u32::MAX) || h > f64::from(u32::MAX) {
        return Err(SvgStatus::Overflow);
    }
    Ok((tree, w as u32, h as u32))
}

/// Parse an owned SVG tree. The input bytes are borrowed during parsing.
///
/// # Safety
/// `data` must point to `len` readable bytes. The dimensions must be initialized
/// and writable, and `out` must be writable. These regions must be disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn silkie_svg_parse(
    data: *const u8,
    len: usize,
    width: *mut u32,
    height: *mut u32,
    out: *mut *mut usvg::Tree,
) -> SvgStatus {
    if len == 0 || len > isize::MAX as usize {
        return SvgStatus::Invalid;
    }
    let result =
        catch_unwind(|| unsafe { parse(slice::from_raw_parts(data, len), *width, *height) });
    match result {
        Ok(Ok((tree, w, h))) => unsafe {
            let storage = alloc(Layout::new::<usvg::Tree>()).cast::<usvg::Tree>();
            if storage.is_null() {
                return SvgStatus::NoMemory;
            }
            storage.write(tree);
            *width = w;
            *height = h;
            *out = storage;
            SvgStatus::Ok
        },
        Ok(Err(status)) => status,
        Err(_) => SvgStatus::Invalid,
    }
}

/// # Safety
/// `tree` must be null or a live pointer returned by `silkie_svg_parse`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn silkie_svg_free(tree: *mut usvg::Tree) {
    if !tree.is_null() {
        unsafe { drop(Box::from_raw(tree)) };
    }
}

/// Render a tile into premultiplied sRGB RGBA. Its origin is relative to the
/// whole image at the requested display size.
///
/// # Safety
/// `tree` must be a live pointer returned by `silkie_svg_parse`. `pixels` must
/// point to a disjoint region of `len` initialized, writable bytes, all zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn silkie_svg_render(
    tree: *const usvg::Tree,
    image_width: f64,
    image_height: f64,
    x: f64,
    y: f64,
    width: u32,
    height: u32,
    pixels: *mut u8,
    len: usize,
) -> bool {
    let tree = unsafe { &*tree };
    let sx = (image_width / f64::from(tree.size().width())) as f32;
    let sy = (image_height / f64::from(tree.size().height())) as f32;
    let (tx, ty) = (-x as f32, -y as f32);
    if ![sx, sy, tx, ty].iter().all(|n| n.is_finite()) || sx == 0.0 || sy == 0.0 {
        return false;
    }

    // resvg builds an IntRect five times the tile size for offscreen layers.
    // IntRect dimensions must fit in i32, including that expansion.
    let max = i32::MAX as u32 / 5;
    if width > max || height > max || len > isize::MAX as usize {
        return false;
    }
    let pixels = unsafe { slice::from_raw_parts_mut(pixels, len) };
    let Some(mut pixmap) = tiny_skia::PixmapMut::from_bytes(pixels, width, height) else {
        return false;
    };
    let transform = tiny_skia::Transform::from_row(sx, 0.0, 0.0, sy, tx, ty);
    catch_unwind(AssertUnwindSafe(|| {
        resvg::render(tree, transform, &mut pixmap)
    }))
    .is_ok()
}
