#!/usr/bin/env sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 SOURCE_ARCHIVE WORKSPACE_DIRECTORY" >&2
    exit 2
fi

source_archive=$1
workspace=$2
expected_archive=21c7f064903154f1c3a7458bee81f130fc36f9b5147ea13328f9980e02d2dea2
expected_library=79cf8a84b85e716ab44da6807e93684dfe31e4280ce9b0eff2bd8e0e2f3d6de6
expected_third=fd18a333099a6eebc05d0844ec7757404297a0cca47b77548e8eac67f1e1269f
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
verify_script=$script_dir/../cmake/VerifyMuPdfContour.cmake

for required in "$source_archive" "$verify_script"; do
    if [ ! -e "$required" ]; then
        echo "required MuPDF contour input is missing: $required" >&2
        exit 2
    fi
done
if [ -e "$workspace" ] && [ ! -d "$workspace" ]; then
    echo "workspace is not a directory: $workspace" >&2
    exit 2
fi
if [ -d "$workspace" ] && [ -n "$(find "$workspace" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    echo "workspace must be new or empty: $workspace" >&2
    exit 2
fi
mkdir -p "$workspace/source" "$workspace/build"

actual_archive=$(sha256sum "$source_archive" | awk '{print $1}')
if [ "$actual_archive" != "$expected_archive" ]; then
    echo "MuPDF source archive hash mismatch: $actual_archive" >&2
    exit 2
fi
tar -xzf "$source_archive" -C "$workspace/source"
source_root=$workspace/source/mupdf-1.28.0-source
version_header=$source_root/include/mupdf/fitz/version.h
if [ ! -f "$version_header" ] ||
        ! grep -Eq '^#define[[:space:]]+FZ_VERSION[[:space:]]+"1\.28\.0"' "$version_header"; then
    echo "extracted MuPDF source version is not 1.28.0" >&2
    exit 2
fi
if [ "$(make --version | sed -n '1p')" != "GNU Make 4.4.1" ]; then
    echo "the pinned Linux contour requires GNU Make 4.4.1" >&2
    exit 2
fi
if ! cc --version | sed -n '1p' | grep -Eq '14\.2\.0'; then
    echo "the pinned Linux contour requires GCC 14.2.0" >&2
    exit 2
fi
if ! ar --version | sed -n '1p' | grep -Eq '2\.44'; then
    echo "the pinned Linux contour requires GNU binutils 2.44" >&2
    exit 2
fi

build_directory=$workspace/build
make -C "$source_root" -j 4 \
    build=small shared=no USE_SYSTEM_LIBS=no mujs=no html=no xps=no svg=no \
    extract=no brotli=no tesseract=no barcode=no archive=no tofu=yes tofu_cjk=yes \
    tofu_cjk_ext=yes tofu_cjk_lang=yes HAVE_GLUT=no HAVE_X11=no HAVE_CURL=no \
    HAVE_LIBCRYPTO=no threading=no \
    'XCFLAGS=-DFZ_ENABLE_HYPHEN=0 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0' \
    CC=cc CXX=c++ AR=ar RANLIB=ranlib LD=ld OUT="$build_directory" libs

library=$build_directory/libmupdf.a
third_library=$build_directory/libmupdf-third.a
actual_library=$(sha256sum "$library" | awk '{print $1}')
actual_third=$(sha256sum "$third_library" | awk '{print $1}')
if [ "$actual_library" != "$expected_library" ]; then
    echo "MuPDF library hash mismatch: $actual_library" >&2
    exit 2
fi
if [ "$actual_third" != "$expected_third" ]; then
    echo "MuPDF third-party library hash mismatch: $actual_third" >&2
    exit 2
fi

cmake \
    -DVOVE_MUPDF_SOURCE_ROOT="$source_root" \
    -DVOVE_MUPDF_SOURCE_ARCHIVE="$source_archive" \
    -DVOVE_MUPDF_LIBRARY="$library" \
    -DVOVE_MUPDF_THIRD_LIBRARY="$third_library" \
    -P "$verify_script"

printf 'MUPDF_WORKSPACE=%s\n' "$workspace"
printf 'MUPDF_SOURCE_ROOT=%s\n' "$source_root"
printf 'MUPDF_SOURCE_ARCHIVE=%s\n' "$source_archive"
printf 'MUPDF_LIBRARY=%s\n' "$library"
printf 'MUPDF_THIRD_LIBRARY=%s\n' "$third_library"
