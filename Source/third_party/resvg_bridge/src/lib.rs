// SPDX-License-Identifier: GPL-3.0-or-later

use std::borrow::Cow;
use std::io::Read;
use std::slice;

const MAXIMUM_SOURCE_BYTES: usize = 64 * 1024 * 1024;
const MAXIMUM_EXPANDED_SOURCE_BYTES: usize = 64 * 1024 * 1024;
const MAXIMUM_EDGE: u32 = 4096;

#[repr(i32)]
#[derive(Clone, Copy)]
enum Status {
    Ok = 0,
    InvalidArgument = 1,
    SourceLimit = 2,
    NotUtf8 = 3,
    MalformedGzip = 4,
    ElementLimit = 5,
    InvalidSize = 6,
    ParsingFailed = 7,
    OutputTooSmall = 8,
    InternalError = 9,
}

#[repr(C)]
pub struct RenderResult {
    status: i32,
    width: u32,
    height: u32,
    bytes_written: usize,
}

impl RenderResult {
    const fn failure(status: Status) -> Self {
        Self {
            status: status as i32,
            width: 0,
            height: 0,
            bytes_written: 0,
        }
    }
}

fn parse_error_status(error: resvg::usvg::Error) -> Status {
    match error {
        resvg::usvg::Error::NotAnUtf8Str => Status::NotUtf8,
        resvg::usvg::Error::MalformedGZip => Status::MalformedGzip,
        resvg::usvg::Error::ElementsLimitReached => Status::ElementLimit,
        resvg::usvg::Error::InvalidSize => Status::InvalidSize,
        resvg::usvg::Error::ParsingFailed(_) => Status::ParsingFailed,
        resvg::usvg::Error::SvgzFeatureNotEnabled => Status::InternalError,
    }
}

fn unpremultiply_rgba(pixels: &mut [u8]) {
    for pixel in pixels.as_chunks_mut::<4>().0 {
        let alpha = u32::from(pixel[3]);
        if alpha == 0 {
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 0;
        } else if alpha != 255 {
            for channel in &mut pixel[..3] {
                let straight = (u32::from(*channel) * 255 + alpha / 2) / alpha;
                *channel = straight.min(255) as u8;
            }
        }
    }
}

fn svg_bytes(source: &[u8], maximum_expanded_bytes: usize) -> Result<Cow<'_, [u8]>, Status> {
    if !source.starts_with(&[0x1f, 0x8b]) {
        return Ok(Cow::Borrowed(source));
    }
    let decoder = flate2::read::GzDecoder::new(source);
    let mut limited = decoder.take(maximum_expanded_bytes.saturating_add(1) as u64);
    let mut expanded = Vec::new();
    if limited.read_to_end(&mut expanded).is_err() {
        return Err(Status::MalformedGzip);
    }
    if expanded.len() > maximum_expanded_bytes {
        return Err(Status::SourceLimit);
    }
    Ok(Cow::Owned(expanded))
}

fn render(source: &[u8], canonical_edge: u32, output: &mut [u8]) -> RenderResult {
    if source.is_empty() || canonical_edge == 0 || canonical_edge > MAXIMUM_EDGE {
        return RenderResult::failure(Status::InvalidArgument);
    }
    if source.len() > MAXIMUM_SOURCE_BYTES {
        return RenderResult::failure(Status::SourceLimit);
    }

    let source = match svg_bytes(source, MAXIMUM_EXPANDED_SOURCE_BYTES) {
        Ok(source) => source,
        Err(status) => return RenderResult::failure(status),
    };

    let mut options = resvg::usvg::Options::default();
    options.fontdb_mut().load_system_fonts();
    let tree = match resvg::usvg::Tree::from_data_nested(source.as_ref(), &options) {
        Ok(tree) => tree,
        Err(error) => return RenderResult::failure(parse_error_status(error)),
    };
    let source_size = tree.size();
    let source_width = source_size.width();
    let source_height = source_size.height();
    if !source_width.is_finite()
        || !source_height.is_finite()
        || source_width <= 0.0
        || source_height <= 0.0
    {
        return RenderResult::failure(Status::InvalidSize);
    }

    let scale = canonical_edge as f32 / source_width.max(source_height);
    let width = (source_width * scale)
        .ceil()
        .clamp(1.0, canonical_edge as f32) as u32;
    let height = (source_height * scale)
        .ceil()
        .clamp(1.0, canonical_edge as f32) as u32;
    let required = match usize::try_from(width)
        .ok()
        .and_then(|value| value.checked_mul(height as usize))
        .and_then(|value| value.checked_mul(4))
    {
        Some(required) => required,
        None => return RenderResult::failure(Status::InvalidSize),
    };
    if output.len() < required {
        return RenderResult {
            status: Status::OutputTooSmall as i32,
            width,
            height,
            bytes_written: required,
        };
    }

    let Some(mut pixmap) =
        resvg::tiny_skia::PixmapMut::from_bytes(&mut output[..required], width, height)
    else {
        return RenderResult::failure(Status::InternalError);
    };
    let transform = resvg::tiny_skia::Transform::from_scale(scale, scale);
    resvg::render(&tree, transform, &mut pixmap);
    unpremultiply_rgba(&mut output[..required]);

    RenderResult {
        status: Status::Ok as i32,
        width,
        height,
        bytes_written: required,
    }
}

#[unsafe(no_mangle)]
/// Renders caller-owned SVG/SVGZ bytes into caller-owned RGBA storage.
///
/// # Safety
///
/// `source` must reference `source_bytes` readable bytes and `output` must reference
/// `output_capacity` writable bytes for the duration of this call. The regions must not overlap.
pub unsafe extern "C" fn vove_resvg_render(
    source: *const u8,
    source_bytes: usize,
    canonical_edge: u32,
    output: *mut u8,
    output_capacity: usize,
) -> RenderResult {
    if source.is_null() || output.is_null() {
        return RenderResult::failure(Status::InvalidArgument);
    }
    let source = unsafe { slice::from_raw_parts(source, source_bytes) };
    let output = unsafe { slice::from_raw_parts_mut(output, output_capacity) };
    render(source, canonical_edge, output)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    const SVG: &[u8] = br##"<svg xmlns="http://www.w3.org/2000/svg" width="80" height="40">
      <rect width="80" height="40" fill="#336699" fill-opacity="0.5"/>
      <rect x="20" y="10" width="40" height="20" fill="#ff8040" fill-opacity="0.5"/>
    </svg>"##;

    #[test]
    fn renders_to_requested_canonical_edge() {
        let mut output = vec![0_u8; 64 * 64 * 4];
        let result = render(SVG, 64, &mut output);
        assert_eq!(result.status, Status::Ok as i32);
        assert_eq!((result.width, result.height), (64, 32));
        assert_eq!(result.bytes_written, 64 * 32 * 4);
        assert!(
            output[..result.bytes_written]
                .as_chunks::<4>()
                .0
                .iter()
                .any(|pixel| pixel[3] != 0)
        );
    }

    #[test]
    fn reports_required_output_capacity() {
        let mut output = [0_u8; 16];
        let result = render(SVG, 64, &mut output);
        assert_eq!(result.status, Status::OutputTooSmall as i32);
        assert_eq!(result.bytes_written, 64 * 32 * 4);
    }

    #[test]
    fn rejects_non_svg_input() {
        let mut output = vec![0_u8; 64 * 64 * 4];
        let result = render(b"not svg", 64, &mut output);
        assert_eq!(result.status, Status::ParsingFailed as i32);
    }

    #[test]
    fn keeps_straight_alpha_channels() {
        let mut output = vec![0_u8; 64 * 64 * 4];
        let result = render(SVG, 64, &mut output);
        assert_eq!(result.status, Status::Ok as i32);
        let translucent = output[..result.bytes_written]
            .as_chunks::<4>()
            .0
            .iter()
            .find(|pixel| pixel[3] > 0 && pixel[3] < 255)
            .expect("fixture should contain a translucent edge");
        assert!(
            translucent[..3]
                .iter()
                .any(|channel| *channel > translucent[3])
        );
    }

    #[test]
    fn rejects_svgz_expanded_beyond_the_limit() {
        let mut encoded = Vec::new();
        {
            let mut encoder =
                flate2::write::GzEncoder::new(&mut encoded, flate2::Compression::best());
            encoder.write_all(&vec![b'a'; 1025]).unwrap();
            encoder.finish().unwrap();
        }
        assert!(matches!(
            svg_bytes(&encoded, 1024),
            Err(Status::SourceLimit)
        ));
    }

    #[test]
    fn nested_parse_does_not_load_external_images() {
        const RED_PNG: &[u8] = &[
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
            0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
            0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x08,
            0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99,
            0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
        ];
        let path =
            std::env::temp_dir().join(format!("vove-resvg-external-{}.png", std::process::id()));
        std::fs::write(&path, RED_PNG).unwrap();
        let uri_path = path.to_string_lossy().replace('\\', "/");
        let file_uri = if cfg!(windows) {
            format!("file:///{uri_path}")
        } else {
            format!("file://{uri_path}")
        };
        let svg = format!(
            r#"<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
            <image href="{file_uri}" width="16" height="32"/>
            <image href="https://example.invalid/vove.png" x="16" width="16" height="32"/>
            </svg>"#
        );
        let mut output = vec![0_u8; 32 * 32 * 4];
        let result = render(svg.as_bytes(), 32, &mut output);
        let _ = std::fs::remove_file(path);
        assert_eq!(result.status, Status::Ok as i32);
        assert!(
            output[..result.bytes_written]
                .as_chunks::<4>()
                .0
                .iter()
                .all(|pixel| pixel[3] == 0)
        );
    }
}
