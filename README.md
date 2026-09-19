# VO-VE

**English** | [Русский](README.ru.md)

**View Only - View Everything**

**VO-VE** is an ultra-lightweight, fast viewer with a clean, straightforward interface. The concept rests on two principles: view only and view everything (View Only - View Everything). No graphics editing, tags or other junk features, now or ever.

VO-VE is built for designers, print shops, publishers, prepress specialists and anyone who works with large collections of graphics files on local drives, file servers and
NAS devices. It helps you quickly find the artwork you need, recognise it by its thumbnail and examine it in a large viewing pane without opening a heavy graphics editor.

- Website: [vo-ve.ru](https://vo-ve.ru) (not ready yet)
- Repository: [github.com/dercar2/VO-VE](https://github.com/dercar2/VO-VE)
- Report a problem: [GitHub Issues](https://github.com/dercar2/VO-VE/issues)
- Version: **0.2.17**
- Build: **release Alfa**
- Currently available for: **Windows x64 and Linux amd64**

**[Download VO-VE](https://github.com/dercar2/VO-VE/releases/latest)**

## Why VO-VE Exists

An ordinary file manager shows names and standard low-resolution icons. A graphics editor can open a document, but it is too heavy for quickly browsing hundreds of pieces of artwork. Cataloguing tools require imports, a database, tags and a separately organised archive. The closest in spirit are XnView, FastStone, IrfanView or the more elite ACDSee, Adobe Bridge and others. But for a simple viewer, they all have too much functionality and support a limited range of graphics formats (cdr being the biggest problem).

VO-VE sits somewhere in between: it works with existing folders like a file manager, shows the contents of graphic artwork right in the directory, supports formats popular in printing and design, does not annoy you with a bulky interface packed with a million tools and needs almost no setup. Besides, there is no need to import files, rebuild your archive or transfer documents.

## Main Features

1. Viewing most formats that matter in real work, including that shitty CDR we all hate,
   Adobe documents (including InDesign), Affinity and open graphics formats.
2. Fast browsing of local folders, SMB shares, servers and NAS devices without blocking the interface during slow operations.
3. Copying, moving, single and batch renaming of files and folders, deletion with explicit conflict handling and recovery of interrupted operations. Ctrl+C/Ctrl+V
   work with selected items through the system clipboard.
4. Instant local filtering by name in the current directory as you type.
5. Global filename search through the incomparable Everything on Windows or plocate on Linux.
6. By default: opening a file in its native application.
7. A permanent large viewing pane, including for multipage documents; zooming and panning a loaded static preview without opening an editor.
8. Colour-coded formats without placing a label over the artwork itself.
9. Folder mosaics made from previews of the documents inside.
10. A persistent, size-limited cache: browse again without reprocessing heavy files.
11. Colour management using embedded ICC profiles.
12. Page counts and page navigation for multipage documents.
13. Four themes and an original vintage interface inspired by the aesthetics of circuit boards.
14. Viewing files from an entire subfolder tree together: "Eye" mode, with a clearly visible border around the thumbnail area (a feature borrowed from Total Commander - Ctrl+B).
15. Natural filename sorting (`2, 17, 187`) and restoring your position and selection when returning to a folder.
16. Modification date and time below the selected filename; a recent time is highlighted with a coloured background.

## Supported Formats

| Formats | Viewing Method and Limits |
| --- | --- |
| JPG/JPEG/JFIF, PNG, BMP, GIF, ICO, TIFF | Raster decoding |
| WebP | Decode-only; no encoder or auxiliary tools |
| HEIF/HEIC, AVIF | Primary image, orientation, transparency and colour profile |
| JPEG XL | First composited SDR image and ICC profile |
| PDF | Thumbnails, large preview, page count and page navigation |
| PDF-compatible AI | Viewing through the MuPDF PDF pipeline |
| PS, EPS, classic AI | Rendering through a separately installed Ghostscript |
| CDR | Best embedded preview without launching CorelDRAW or full vector rendering |
| PSD, PSB | Flattened RGB/CMYK/Lab/Gray image; no layer recompositing |
| RAW: CR2, CR3, DNG, NEF, NRW, ORF, PEF, RAF, RW2, SRW, ARW | Embedded JPEG through PIEX; no sensor demosaicing |
| SVG, SVGZ | Isolated rendering through resvg |
| INDD, INDT | First preview saved inside the document; no InDesign layout rendering |
| IDML | First saved JPEG from XMP; no rendering of layouts, fonts or external references |
| afphoto, afdesign, afpub, af, aftemplate | PNG preview saved inside the Affinity document, if present |
| KRA, ORA | Saved composite image; no layer recompositing |
| XCF 0-19 | Compact embedded KImageFormats decoder; NONE/RLE/zlib, no GIMP installation |
| PLT, HPGL | A limited, verifiable set of commands through hp2xx and the SVG worker |
| DXF | Saved `THUMBNAILIMAGE` in ASCII DXF; no full CAD rendering |

A file extension is not considered proof of its format: the handler checks the signature and
container structure. If a document feature is unsupported, VO-VE should honestly refuse to display it rather than show a plausible but incorrect image.

Support for XCF versions 20 and above is deferred until effects, vector layers and linked layers can be safely recognised (an enthusiast is welcome to take this on). CPT and AKVIS are not currently included in the declared support.

## Working With Files

VO-VE does not alter the graphic contents of documents, but it supports the necessary operations on the files and folders themselves:

- creating folders, including with F7;
- single and batch renaming;
- copying by drag-and-drop;
- moving by drag-and-drop while holding `Shift`;
- choosing what to do when names match: create a copy, overwrite or cancel the entire operation;
- deleting to the VO-VE Trash pinned in Favourites, restoring to the original location or another directory on the same volume, and explicit permanent deletion;
- copying the name and full path;
- opening a document in the system's default application or a chosen external application.

File operations run separately from the interface. A slow network, a dropped connection or
a damaged document should not freeze the main window or turn an uncertain result
into a success message.

## What Deliberately Stays Out

- drawing, retouching and colour correction;
- changing pages, layers or document contents;
- exporting, converting and resaving graphics;
- writing EXIF, IPTC and XMP;
- tags, ratings, collections and media library management;
- printing (printing is being considered for future implementation), slideshows and a video player;
- launching native graphics editors to generate previews;
- user-supplied executable plugins.

VO-VE does not replace Photoshop, CorelDRAW, InDesign, Affinity, GIMP or CAD. Its job is to show files quickly and help you work with your existing directory structure.

## How It Works

The main code is written in **C++20**, and the interface uses **Qt 6 Widgets**. Small individual adapters are written in C, and the SVG rendering bridge is written in Rust.

The catalogue, cache, search, file operations and document handlers are separate. Heavy and untrusted files are decoded outside the UI process in separate worker processes with limits on time, memory, input file size and output size. A single handler failing should not close the entire application.

Previews are converted to canonical tagged sRGB and saved in a persistent cache. The cache key accounts for the file, profile and processing parameters. When the allocated space is exceeded, old unused
results are removed; original documents are never touched.

## Components

The distribution uses only the necessary parts of third-party projects:

- **Qt 6 Widgets** - interface;
- **MuPDF** - PDF and PDF-compatible AI;
- **Little CMS** - ICC profile conversion;
- **SQLite and QOI** - persistent cache index and data;
- **resvg** - SVG and SVGZ;
- **libheif, libde265 and dav1d** - HEIC and AVIF;
- **libjxl** - JPEG XL;
- **libwebp** - WebP;
- **PIEX** - embedded RAW previews;
- **pugixml** - limited parsing of InDesign metadata;
- **KImageFormats XCF reader** - embedded XCF viewing;
- **miniz** - compressed entries in CDR, KRA, ORA and XCF;
- **GNU hp2xx** - limited PLT/HPGL pipeline;
- **utf8proc** - correct handling of Unicode filenames.

Exact versions, linking boundaries and licences are listed in `THIRD_PARTY_NOTES.md`.

Installed separately:

- **Ghostscript** - required for PS, EPS and classic AI;
- **LINE Seed JP** - the chosen typeface for the interface;
- **Everything** on Windows - used for instant global search, with boundless gratitude and respect;
- **plocate** on Linux - global index-based search;
- **OpenSSL** from the Linux repository - update signature verification.

No native graphics editor is required or launched to generate previews.

## Installation

Ready-to-use builds are in the separate
[Releases](https://github.com/dercar2/VO-VE/releases) section, not mixed in with the source files.

Current version: **[VO-VE 0.2.17](https://github.com/dercar2/VO-VE/releases/tag/v0.2.17)**.

- [Windows x64 installer](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/VO-VE-0.2.17-windows-x64-setup.exe)
- [Windows x64 Portable](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/VO-VE-0.2.17-windows-x64.zip)
- [Linux amd64: DEB package](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/vo-ve_0.2.17_amd64.deb)
- [Source code 0.2.17](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/VO-VE-0.2.17-sources.zip)
- [Licences and notices](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/VO-VE-0.2.13-notices.zip)
- [Dependency sources](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/VO-VE-0.2.13-dependency-sources.zip) (for building, not needed for a normal installation)
- [SHA-256 checksums](https://github.com/dercar2/VO-VE/releases/download/v0.2.17/SHA256SUMS.txt)

Windows uses a single `.exe` installer. The current Linux distribution is an amd64 `.deb` package. Additional components are not disguised as part of VO-VE and are installed only
when explicitly selected by the user.

## Building From Source

The complete source code for the published build is available as `VO-VE-0.2.17-sources.zip` on the release page. This archive contains production source code, CMake files, artwork used by the application, packaging definitions and selected parts of third-party libraries.
GitHub's automatic `Source code (zip)` / `Source code (tar.gz)` links contain a snapshot of the public repository; until the code is moved into Git, use the specified release attachment to build the application.
Compiled objects, tool caches, private test corpora and ready-built release binaries are not included in the Git history.

Basic headless build:

```sh
cmake --preset source-release
cmake --build --preset source-release
```

Building the full interface and all handlers requires Qt and the pinned dependencies. The exact build toolchain is described in `BUILDING.md` inside the archive; dependency versions and checksums are in
`THIRD_PARTY_NOTES.md` and `docs/dependencies`.
The exact sources of external dependencies are available as a separate attachment,
`VO-VE-0.2.13-dependency-sources.zip`, on the release page.
The dependencies have not changed: their source archive and the licence archive are preserved byte-for-byte under the baseline version number 0.2.13. For the application itself, use the 0.2.17 sources.

## Project Status

VO-VE is at the **release Alfa** stage. The core functionality is implemented and used in real work on Windows and Linux. Field testing, visual refinement and
expansion of the real-document test corpus continue.

The project deliberately stays focused. A new feature is accepted only when it helps with viewing or working with files and does not undermine the core requirements: compactness, speed, stability and predictable behaviour on local and network resources.

## Authors

- Idea and design: **dercar** - [dercar@ya.com](mailto:dercar@ya.com)
- Programming: **Rubilaks**
- Contributors: no outside contributors yet

Inspired by the fine ideas and shortcomings of XnView, Geeqie, Total Commander and the incomparable Everything.

## Licence

VO-VE is distributed under **GPL-3.0-or-later**. You may use, study, copy, modify and distribute the program, including for peanuts, as long as you comply with the GPL. Derivative versions must also preserve the freedom to access the source code under the GPL.

Third-party components retain their own licences and notices.
In particular, the PDF worker includes MuPDF under AGPL-3.0-or-later; the terms for this part are not replaced by the project's GPL alone. Full texts and notices are available in `VO-VE-0.2.13-notices.zip` and must accompany the binaries when redistributed.
