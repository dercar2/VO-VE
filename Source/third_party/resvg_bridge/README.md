# VO-VE resvg bridge

This is the deliberately narrow Rust-to-C bridge used by the lazy SVG/SVGZ preview worker.

- Renderer: `resvg` `0.48.1`, pinned exactly by `Cargo.toml` and `Cargo.lock`.
- Toolchain: Rust `1.98.0`, pinned by `rust-toolchain.toml`.
- Enabled features: text, system fonts and embedded raster images. SVGZ is expanded by the bridge
  itself so both compressed and expanded input have explicit byte limits.
- Input: caller-owned SVG/SVGZ bytes only.
- External image paths: disabled by `usvg::Tree::from_data_nested`; embedded data images remain
  supported.
- Output: bounded straight-alpha RGBA8888 in sRGB.

The bridge intentionally exports one function. VO-VE does not expose the general resvg API or ship
its command-line tools.
