#!/usr/bin/env bash
set -euo pipefail
umask 022

if (($# < 1 || $# > 2)); then
    echo "usage: $0 BUILD_DIRECTORY [OUTPUT_DIRECTORY]" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
build=$(realpath "$1")
output_argument=${2:-out/packages/linux}
mkdir -p "$project_root/out/packages"
if [[ $output_argument = /* ]]; then
    output=$(realpath -m "$output_argument")
else
    output=$(realpath -m "$project_root/$output_argument")
fi
allowed_root=$(realpath "$project_root/out/packages")
case "$output/" in
    "$allowed_root"/*/) ;;
    *) echo "output directory must stay below $allowed_root" >&2; exit 2 ;;
esac

cache=$build/CMakeCache.txt
manifest=$project_root/packaging/linux-runtime-manifest.tsv
for required in "$cache" "$manifest"; do
    [[ -f $required ]] || { echo "required package input is missing: $required" >&2; exit 2; }
done
for required_cache in \
        'CMAKE_BUILD_TYPE:STRING=Release' \
        'VOVE_BUILD_TESTS:BOOL=OFF' \
        'VOVE_BUILD_QT_APP:BOOL=ON' \
        'VOVE_BUILD_MUPDF_DOCUMENT_WORKER:BOOL=ON' \
        'VOVE_BUILD_RESVG_SVG_WORKER:BOOL=ON' \
        'VOVE_EXPECT_STRICT_SANDBOX:BOOL=ON' \
        'VOVE_RELEASE_BUILD:BOOL=ON'; do
    grep -Fqx "$required_cache" "$cache" || {
        echo "build is not a complete Linux release: $required_cache" >&2
        exit 2
    }
done

version=$(sed -n 's/^project(VO_VE VERSION \([^ ]*\).*/\1/p' "$project_root/CMakeLists.txt")
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "invalid project version" >&2; exit 2; }
architecture=$(dpkg --print-architecture)
[[ $architecture == amd64 ]] || { echo "unsupported package architecture: $architecture" >&2; exit 2; }

work=$output/.stage
stage=$work/root
rm -rf -- "$work"
mkdir -p "$stage"

while IFS='|' read -r kind destination source strip_mode owner reason; do
    [[ -z $kind || $kind == \#* ]] && continue
    [[ -n $destination && -n $source && -n $owner && -n $reason ]] || {
        echo "incomplete runtime manifest entry: $destination" >&2
        exit 2
    }
    target=$stage/$destination
    mkdir -p "$(dirname -- "$target")"
    if [[ $kind == symlink ]]; then
        ln -s -- "$source" "$target"
        continue
    fi
    [[ $kind == file ]] || { echo "unsupported manifest kind: $kind" >&2; exit 2; }
    source=${source//\{build\}/$build}
    source=${source//\{project\}/$project_root}
    [[ -f $source ]] || { echo "runtime source is missing: $source" >&2; exit 2; }
    mode=0644
    [[ $destination == usr/lib/vo-ve/vove-* ]] && mode=0755
    install -D -m "$mode" -- "$source" "$target"
    case "$strip_mode" in
        all) strip --strip-all "$target" ;;
        none) ;;
        *) echo "unsupported strip mode: $strip_mode" >&2; exit 2 ;;
    esac
done < "$manifest"

if LC_ALL=C grep -aFq 'VOVE_TEST_TRANSFER_' \
        "$stage/usr/lib/vo-ve/vove-fileop-helper"; then
    echo "packaged file-operation helper contains fault-injection hooks" >&2
    exit 2
fi

mkdir -p "$work/debian" "$stage/DEBIAN"
cat > "$work/debian/control" <<'EOF'
Source: vo-ve
Section: graphics
Priority: optional
Maintainer: VO-VE Project <noreply@example.invalid>
Standards-Version: 4.7.0

Package: vo-ve
Architecture: any
Description: fast document and image viewer
 VO-VE lists local and network folders and previews supported files without graphics editing.
EOF

mapfile -t executables < <(find "$stage/usr/lib/vo-ve" -maxdepth 1 -type f -name 'vove-*' -print | sort)
mapfile -t expected_executables < <(
    awk -F'|' -v root="$stage" \
        '$1 == "file" && $2 ~ /^usr\/lib\/vo-ve\/vove-/ { print root "/" $2 }' \
        "$manifest" | sort
)
if ! diff -u \
        <(printf '%s\n' "${expected_executables[@]}") \
        <(printf '%s\n' "${executables[@]}"); then
    echo "release package runtime executables differ from the manifest" >&2
    exit 2
fi
shlib_output=$(cd "$work" && LC_ALL=C dpkg-shlibdeps -O -l"$stage/usr/lib/vo-ve" "${executables[@]}")
shlib_depends=${shlib_output#shlibs:Depends=}
[[ -n $shlib_depends && $shlib_depends != "$shlib_output" ]] || {
    echo "dpkg-shlibdeps did not produce package dependencies" >&2
    exit 2
}
installed_kib=$(du -sk "$stage" | awk '{print $1}')
cat > "$stage/DEBIAN/control" <<EOF
Package: vo-ve
Version: $version
Architecture: $architecture
Maintainer: VO-VE Project <noreply@example.invalid>
Section: graphics
Priority: optional
Installed-Size: $installed_kib
Depends: $shlib_depends, bubblewrap, desktop-file-utils, fonts-dejavu-core, hicolor-icon-theme, qt6-qpa-plugins, qt6-image-formats-plugins, xdg-utils
Recommends: plocate
Suggests: ghostscript
Description: fast document and image viewer
 VO-VE lists local and network folders and previews supported files without graphics editing.
EOF

(cd "$stage" && find usr -type f -print0 | sort -z | xargs -0 md5sum) > "$stage/DEBIAN/md5sums"
install -m 0755 "$project_root/packaging/linux/preinst" "$stage/DEBIAN/preinst"
install -m 0755 "$project_root/packaging/linux/postinst" "$stage/DEBIAN/postinst"
install -m 0755 "$project_root/packaging/linux/postrm" "$stage/DEBIAN/postrm"
chmod 0644 "$stage/DEBIAN/control" "$stage/DEBIAN/md5sums"
find "$stage" -type d -exec chmod 0755 {} +
find "$stage" -print0 | xargs -0 touch --no-dereference --date=@1704067200
export SOURCE_DATE_EPOCH=1704067200

mkdir -p "$output"
package=$output/vo-ve_${version}_${architecture}.deb
rm -f -- "$package" "$package.sha256"
dpkg-deb --root-owner-group --build "$stage" "$package"
package_name=$(basename -- "$package")
package_hash=$(sha256sum "$package" | awk '{print $1}')
printf '%s  %s\n' "$package_hash" "$package_name" > "$package.sha256"

printf 'PACKAGE=%s\n' "$package"
printf 'PACKAGE_BYTES=%s\n' "$(stat -c %s "$package")"
printf 'PACKAGE_SHA256=%s\n' "$package_hash"
