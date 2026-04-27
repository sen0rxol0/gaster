#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh  –  gastera1n cross-platform build script
#
# Targets:
#   macOS universal  (arm64 + x86_64, merged with lipo)
#   Linux x86_64 / arm64 (aarch64)
#
# Assumptions:
#   • The host application already embeds libplist 2.0.2 and libirecovery 1.0.0
#     as shared libraries inside its own bundle/package.  This script therefore
#     links *against* those ABI-stable dylibs/SOs but does NOT copy them into
#     the release tree.
#   • img4 is built from source (img4lib) and placed into the release tree
#     directly from the per-arch sysroot.
#   • Companion pre-built tool archives (ldid2, iBoot64Patcher, tsschecker)
#     are expected in ROOT_DIR with the naming convention described in
#     stage_release_tree().
#
# Usage:
#   ./build.sh [TARGET_PLATFORM] [TARGET_ARCH]
#
#   TARGET_PLATFORM : macos | linux          (default: macos)
#   TARGET_ARCH     : arm64 | x86_64 | universal
#                     macOS default: universal
#                     Linux default: x86_64
#
# Environment overrides (all optional):
#   WORK_ROOT    – scratch directory          (default: .build/<platform>-<arch>)
#   DIST_ROOT    – output directory           (default: dist/)
#   XCODE_APP    – path to Xcode.app          (default: /Applications/Xcode.app)
#   INSTALL_DEPS – set to 1 to auto-brew-install build tools on macOS
# ---------------------------------------------------------------------------
set -euo pipefail

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mWARN:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

is_macho() { file -b "$1" 2>/dev/null | grep -q 'Mach-O'; }
is_elf()   { file -b "$1" 2>/dev/null | grep -q 'ELF'; }

copy_tree() {
    local src="$1" dst="$2"
    rm -rf "$dst"
    mkdir -p "$dst"
    if command -v rsync >/dev/null 2>&1; then
        rsync -a "${src}/" "${dst}/"
    else
        cp -R "${src}/." "${dst}/"
    fi
}

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------
TARGET_PLATFORM="${1:-macos}"
TARGET_ARCH="${2:-}"

# Default arch per platform
if [[ -z "${TARGET_ARCH}" ]]; then
    case "${TARGET_PLATFORM}" in
        macos) TARGET_ARCH="universal" ;;
        linux) TARGET_ARCH="x86_64"   ;;
        *)     die "Unknown platform '${TARGET_PLATFORM}'. Use: macos | linux" ;;
    esac
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="/usr/local"

# Prefer gmake, fall back to make
MAKE_BIN="$(command -v gmake 2>/dev/null || command -v make 2>/dev/null || true)"
[[ -n "${MAKE_BIN}" ]] || die "make / gmake not found"

# CPU count – nproc (Linux) first, then sysctl (macOS), then safe default
NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

WORK_ROOT="${WORK_ROOT:-${ROOT_DIR}/.build/${TARGET_PLATFORM}-${TARGET_ARCH}}"
DIST_ROOT="${DIST_ROOT:-${ROOT_DIR}/dist}"

# Version string embedded in release directory names
GASTER_VERSION="v1.0"

# libplist / libirecovery versions the host application ships
LIBPLIST_VERSION="2.2.0"
LIBIRECOVERY_VERSION="1.0.0"

# ---------------------------------------------------------------------------
# Source pinning
# ---------------------------------------------------------------------------
clone_sources() {
    local src_root="$1"
    rm -rf "${src_root}"
    mkdir -p "${src_root}"
    cd "${src_root}"

    log "Cloning sources"

    git clone --quiet https://github.com/madler/zlib.git
    git -C zlib checkout --quiet "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf"

    git clone --quiet https://github.com/tihmstar/libgeneral.git
    git -C libgeneral checkout --quiet "2c3cce029bfb440859cb4affc37c03ada39a0604"

    git clone --quiet https://github.com/tihmstar/libfragmentzip.git
    git -C libfragmentzip checkout --quiet "92f184e631c7156113850afdb9c68a2d892e35b6"

    # libplist – pinned to the 2.0.2-era commit; built only for headers + .pc.
    # The actual shared library at runtime comes from the host application.
    git clone --quiet https://github.com/libimobiledevice/libplist.git
    git -C libplist checkout --quiet "c5a30e9267068436a75b5d00fcbf95cb9c1f4dcd"

    # libirecovery – same rationale as libplist above.
    git clone --quiet https://github.com/libimobiledevice/libirecovery.git
    git -C libirecovery checkout --quiet "1b9d9c3cdd3ef2f38198a21c356352f13641482d"

    git clone --quiet https://github.com/xerub/img4lib.git
    git -C img4lib submodule update --init --recursive --quiet
}

# ---------------------------------------------------------------------------
# macOS compiler environment
# ---------------------------------------------------------------------------
setup_macos_env() {
    local arch="$1"
    local sdk minos host_triple

    sdk="$(xcrun -sdk macosx --show-sdk-path)"

    case "${arch}" in
        arm64)
            minos="11.0"
            host_triple="arm-apple-darwin"
            ;;
        x86_64)
            minos="10.13"
            host_triple="x86_64-apple-darwin"
            ;;
        *)
            die "Unsupported macOS architecture: ${arch}"
            ;;
    esac

    export CC="$(xcrun --find clang)"
    export CXX="$(xcrun --find clang++)"
    export CPP="${CC} -E"
    export AR="$(xcrun --find ar)"
    export RANLIB="$(xcrun --find ranlib)"
    export STRIP="$(xcrun --find strip)"
    export LD="$(xcrun --find ld)"

    local common_cflags="-g -Os -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk}"

    export CFLAGS="${common_cflags} -isystem ${_SYSROOT}${PREFIX}/include"
    export CPPFLAGS="${common_cflags} -isystem ${_SYSROOT}${PREFIX}/include -Wno-error=implicit-function-declaration"
    export CXXFLAGS="${common_cflags} -stdlib=libc++ -isystem ${_SYSROOT}${PREFIX}/include"
    export LDFLAGS="-g -Wl,-dead_strip -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk} \
-L${_SYSROOT}${PREFIX}/lib \
-Wl,-rpath,@executable_path/../Frameworks \
-Wl,-rpath,@loader_path/../Frameworks"

    # Build-host flags (for any host-native tools produced during the build)
    local host_arch
    host_arch="$(uname -m)"
    export CFLAGS_FOR_BUILD="-arch ${host_arch} -isysroot ${sdk} -Os"
    export CXXFLAGS_FOR_BUILD="-stdlib=libc++ -arch ${host_arch} -isysroot ${sdk} -Os"
    export CPPFLAGS_FOR_BUILD="-arch ${host_arch} -isysroot ${sdk} -Wno-error=implicit-function-declaration -Os"
    export LDFLAGS_FOR_BUILD="-Wl,-dead_strip"

    export PKG_CONFIG_PATH="${_SYSROOT}${PREFIX}/lib/pkgconfig:${_SYSROOT}${PREFIX}/share/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    export HOST_TRIPLE="${host_triple}"
}

# ---------------------------------------------------------------------------
# Linux compiler environment
# ---------------------------------------------------------------------------
setup_linux_env() {
    local arch="$1"

    case "${arch}" in
        x86_64)
            export CC="${CC:-gcc}"
            export CXX="${CXX:-g++}"
            export HOST_TRIPLE="x86_64-linux-gnu"
            ;;
        arm64|aarch64)
            export CC="${CC:-aarch64-linux-gnu-gcc}"
            export CXX="${CXX:-aarch64-linux-gnu-g++}"
            export HOST_TRIPLE="aarch64-linux-gnu"
            ;;
        *)
            die "Unsupported Linux architecture: ${arch}"
            ;;
    esac

    export CPP="${CC} -E"
    export AR="${AR:-ar}"
    export RANLIB="${RANLIB:-ranlib}"
    export STRIP="${STRIP:-strip}"
    export LD="${LD:-ld}"

    export CFLAGS="-g -Os -fPIC -I${_SYSROOT}${PREFIX}/include"
    export CPPFLAGS="-g -Os -fPIC -I${_SYSROOT}${PREFIX}/include -Wno-error=implicit-function-declaration"
    export CXXFLAGS="-g -Os -fPIC -I${_SYSROOT}${PREFIX}/include"
    export LDFLAGS="-g -Wl,--gc-sections -L${_SYSROOT}${PREFIX}/lib"

    export PKG_CONFIG_PATH="${_SYSROOT}${PREFIX}/lib/pkgconfig:${_SYSROOT}${PREFIX}/share/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
}

# ---------------------------------------------------------------------------
# Optional macOS dependency installation
# ---------------------------------------------------------------------------
install_macos_build_deps() {
    [[ "${INSTALL_DEPS:-0}" == "1" ]] || return 0
    require_cmd brew
    log "Installing build dependencies via Homebrew"
    brew install make autoconf automake pkg-config gnu-sed libzip libtool jq
}

# ---------------------------------------------------------------------------
# Individual library / tool builds
# ---------------------------------------------------------------------------

build_zlib() {
    log "Building zlib"
    cd "${_SRC_ROOT}/zlib"
    ./configure --prefix="${PREFIX}" --static
    "${MAKE_BIN}" -j"${NCPU}"
    "${MAKE_BIN}" -j"${NCPU}" install DESTDIR="${_SYSROOT}"
}

# Generic autotools helper: run autogen.sh (with autoreconf -fi fallback),
# configure, make, make install DESTDIR=...
build_autotools() {
    local dir="$1"; shift          # remaining args forwarded to configure
    cd "${dir}"

    if [[ -x ./autogen.sh ]]; then
        ./autogen.sh "$@" \
            || { warn "autogen.sh failed in $(basename "${dir}"), retrying with autoreconf -fi"
                 autoreconf -fi
                 ./configure "$@"; }
    else
        autoreconf -fi
        ./configure "$@"
    fi

    "${MAKE_BIN}" -j"${NCPU}"
    "${MAKE_BIN}" -j"${NCPU}" install DESTDIR="${_SYSROOT}"
}

build_static_libs() {
    local platform="$1"
    local common_args=(--prefix="${PREFIX}" --host="${HOST_TRIPLE}")

    # Build order matters: libgeneral <- libplist <- libfragmentzip (needs both)
    #                                 <- libirecovery (needs libplist)

    build_zlib

    log "Building libgeneral (static)"
    build_autotools "${_SRC_ROOT}/libgeneral" \
        "${common_args[@]}" \
        --disable-shared --enable-static

    # -----------------------------------------------------------------------
    # libplist:
    #   Required by libfragmentzip and libirecovery.  Built only to produce
    #   headers + .pc files for compile/link resolution; the actual shared
    #   library at runtime is the one embedded in the host application.
    #   The static archive is removed after install to prevent accidental
    #   static linking against it.
    # -----------------------------------------------------------------------
    log "Building libplist ${LIBPLIST_VERSION} (headers/pkgconfig only)"
    build_autotools "${_SRC_ROOT}/libplist" \
        "${common_args[@]}" \
        --disable-shared --enable-static --without-cython
    rm -f "${_SYSROOT}${PREFIX}/lib/libplist"*.a \
          "${_SYSROOT}${PREFIX}/lib/libplist"*.la

    # libfragmentzip depends on libgeneral and libplist – both must be built first.
    log "Building libfragmentzip (static)"
    build_autotools "${_SRC_ROOT}/libfragmentzip" \
        "${common_args[@]}" \
        --disable-shared --enable-static

    # -----------------------------------------------------------------------
    # libirecovery:
    #   Depends on libplist headers.  Same headers/pkgconfig-only treatment.
    # -----------------------------------------------------------------------
    log "Building libirecovery ${LIBIRECOVERY_VERSION} (headers/pkgconfig only)"
    build_autotools "${_SRC_ROOT}/libirecovery" \
        "${common_args[@]}" \
        --disable-shared --enable-static
    rm -f "${_SYSROOT}${PREFIX}/lib/libirecovery"*.a \
          "${_SYSROOT}${PREFIX}/lib/libirecovery"*.la

    # Inject pkg-config stubs so gastera1n's Makefile resolves the correct
    # -l flags for the host-embedded shared libraries.
    _inject_host_pc_stubs "${platform}"
}

# Write minimal .pc stubs for libplist and libirecovery.
# On macOS the @rpath entries in LDFLAGS already point to the host Frameworks/
# bundle; on Linux the host is expected to set LD_LIBRARY_PATH / RUNPATH.
_inject_host_pc_stubs() {
    local platform="$1"
    local pc_dir="${_SYSROOT}${PREFIX}/lib/pkgconfig"
    mkdir -p "${pc_dir}"

    cat > "${pc_dir}/libplist-2.0.pc" <<EOF
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libplist
Description: Library for working with Apple Binary and XML Property Lists (host-embedded)
Version: ${LIBPLIST_VERSION}
Libs: -lplist-2.0
Cflags: -I\${includedir}
EOF

    cat > "${pc_dir}/libirecovery-1.0.pc" <<EOF
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libirecovery
Description: Library for talking to Apple devices in DFU/Recovery mode (host-embedded)
Version: ${LIBIRECOVERY_VERSION}
Libs: -lirecovery-1.0
Cflags: -I\${includedir}
EOF
}

build_img4() {
    log "Building img4lib / img4 tool"
    cd "${_SRC_ROOT}/img4lib"
    
    if [[ "${TARGET_PLATFORM}" == "macos" ]]; then
        local CFLAGS="-isysroot $(xcrun --sdk macosx --show-sdk-path) -mmacosx-version-min=10.13 -fPIC"
        "${MAKE_BIN}" -C lzfse CFLAGS="${CFLAGS}" -j"${NCPU}"
        "${MAKE_BIN}" -j"${NCPU}" COMMONCRYPTO=1
    else
        "${MAKE_BIN}" -j"${NCPU}"
    fi

    install -d "${_SYSROOT}${PREFIX}/bin"
    install -m 755 img4 "${_SYSROOT}${PREFIX}/bin/img4"
}

build_gastera1n() {
    log "Building gastera1n"
    cd "${ROOT_DIR}"

    # Expose the per-arch sysroot as a libs_root/ convenience tree that the
    # gastera1n Makefile reads from.
    rm -rf "${ROOT_DIR}/libs_root"
    mkdir -p "${ROOT_DIR}/libs_root"
    cp -a "${_SYSROOT}${PREFIX}/include" "${ROOT_DIR}/libs_root/"
    cp -a "${_SYSROOT}${PREFIX}/lib"     "${ROOT_DIR}/libs_root/"

    "${MAKE_BIN}" -j"${NCPU}"

    [[ -x "${ROOT_DIR}/gastera1n" ]] || die "gastera1n binary not produced by make"
}

# ---------------------------------------------------------------------------
# macOS rpath / strip fixup
#   gastera1n links against libplist and libirecovery from the host bundle.
#   If the build accidentally picked up absolute Homebrew paths, rewrite them
#   to @rpath so the host's Frameworks/ copy is used at runtime.
# ---------------------------------------------------------------------------
fix_macos_rpath() {
    local bin="$1"
    log "Fixing rpath / stripping: $(basename "${bin}")"

    local lib
    while IFS= read -r lib; do
        case "${lib}" in
            */libplist*|*/libirecovery*)
                local base
                base="$(basename "${lib}")"
                install_name_tool -change "${lib}" "@rpath/${base}" "${bin}" 2>/dev/null || true
                ;;
        esac
    done < <(otool -L "${bin}" 2>/dev/null | awk 'NR>1{print $1}')

    # -u -r: strip unreferenced symbols, keep external relocs (safe for dyld)
    "${STRIP:-strip}" -u -r "${bin}" 2>/dev/null \
        || "${STRIP:-strip}" "${bin}" \
        || true
}

fix_linux_strip() {
    local bin="$1"
    "${STRIP:-strip}" --strip-unneeded "${bin}" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Companion tool archive helpers
#
# img4 is built from source (img4lib) and placed directly into the release
# tree from the sysroot; it is NOT expected as a pre-built archive here.
#
# Expected archive naming in ROOT_DIR for the remaining tools:
#
#   <tool>_macOS_arm64.gz     <tool>_macOS_x86_64.gz
#   <tool>_linux_x86_64.gz   <tool>_linux_arm64.gz
#
# where <tool> ∈ { ldid2, iBoot64Patcher, tsschecker }
#
# A platform-only fallback (e.g. tsschecker_macOS.gz) is accepted when no
# arch-specific archive exists.
#
# The release tree layout:
#
#   tools/
#     macos/
#       arm64/
#       x86_64/
#       universal/      ← populated during the merge step
#     linux/
#       x86_64/
#       arm64/
# ---------------------------------------------------------------------------

_tool_archives_for() {
    local platform="$1"
    local arch="$2"
    # img4 is built from source – excluded from this list deliberately.
    local tools=(ldid2 iBoot64Patcher tsschecker)
    local t
    for t in "${tools[@]}"; do
        if   [[ -f "${ROOT_DIR}/${t}_${platform}_${arch}.gz" ]]; then
            echo "${t}_${platform}_${arch}.gz"
        elif [[ -f "${ROOT_DIR}/${t}_${platform}.gz" ]]; then
            echo "${t}_${platform}.gz"
        else
            warn "Archive not found for '${t}' on ${platform}/${arch} – skipping"
        fi
    done
}

# ---------------------------------------------------------------------------
# Release tree staging
#
# Layout:
#   <release>/
#     gastera1n
#     tools/
#       <platform>/
#         <arch>/        ← compressed tool archives
#     LICENSE
#     README*
#     restored_external.gz
#     ssh64*
#     bootim@750x1334.im4p
# ---------------------------------------------------------------------------
stage_release_tree() {
    local release_dir="$1"
    local gastera1n_bin="$2"
    local platform="$3"
    local arch="$4"

    rm -rf "${release_dir}"
    mkdir -p "${release_dir}"

    install -m 755 "${gastera1n_bin}" "${release_dir}/gastera1n"

    # Companion tools directory
    local tools_dir="${release_dir}/tools/${platform}/${arch}"
    mkdir -p "${tools_dir}"

    # img4 is built from source; install the binary directly from the sysroot.
    local img4_bin="${_SYSROOT}${PREFIX}/bin/img4"
    if [[ -x "${img4_bin}" ]]; then
        install -m 755 "${img4_bin}" "${tools_dir}/img4"
    else
        warn "img4 binary not found in sysroot -- skipping"
    fi

    # Pre-built tool archives (ldid2, iBoot64Patcher, tsschecker)
    local archive
    while IFS= read -r archive; do
        [[ -n "${archive}" ]] || continue
        cp -v "${ROOT_DIR}/${archive}" "${tools_dir}/"
    done < <(_tool_archives_for "${platform}" "${arch}")

    # Static assets – warn but do not abort on missing optional files
    local asset
    for asset in LICENSE restored_external.gz "bootim@750x1334.im4p"; do
        if [[ -f "${ROOT_DIR}/${asset}" ]]; then
            cp -v "${ROOT_DIR}/${asset}" "${release_dir}/"
        else
            warn "Optional asset not found, skipping: ${asset}"
        fi
    done

    # Glob assets (ssh64* README*) – nullglob prevents errors on no match
    shopt -s nullglob
    for asset in "${ROOT_DIR}"/ssh64* "${ROOT_DIR}"/README*; do
        cp -v "${asset}" "${release_dir}/"
    done
    shopt -u nullglob

    log "Release tree staged: ${release_dir}"
}

# ---------------------------------------------------------------------------
# Single-arch build orchestration
# ---------------------------------------------------------------------------
build_single() {
    local platform="$1"
    local arch="$2"
    local build_root="$3"
    local release_dir="$4"

    log "━━━ Build start: platform=${platform} arch=${arch} ━━━"

    # _SYSROOT / _SRC_ROOT are used by all helper functions; exported so that
    # any subshells spawned by build_autotools etc. inherit them.
    export _SYSROOT="${build_root}/sysroot"
    export _SRC_ROOT="${build_root}/src"

    rm -rf "${build_root}"
    mkdir -p "${_SYSROOT}${PREFIX}/lib" \
             "${_SYSROOT}${PREFIX}/bin" \
             "${_SYSROOT}${PREFIX}/include" \
             "${_SRC_ROOT}"

    clone_sources "${_SRC_ROOT}"

    if [[ "${platform}" == "macos" ]]; then
        require_cmd xcrun
        setup_macos_env "${arch}"
    else
        setup_linux_env "${arch}"
    fi

    build_static_libs "${platform}"
    build_img4

    # gastera1n binary lands at ROOT_DIR/gastera1n per the project Makefile
    build_gastera1n

    local artifact_bin="${build_root}/artifacts/gastera1n"
    mkdir -p "${build_root}/artifacts"
    cp "${ROOT_DIR}/gastera1n" "${artifact_bin}"

    if [[ "${platform}" == "macos" ]]; then
        fix_macos_rpath "${artifact_bin}"
    else
        fix_linux_strip "${artifact_bin}"
    fi

    stage_release_tree "${release_dir}" "${artifact_bin}" "${platform}" "${arch}"

    log "━━━ Build complete: ${release_dir} ━━━"
}

# ---------------------------------------------------------------------------
# Universal macOS merge
# ---------------------------------------------------------------------------
merge_universal_release() {
    local arm_release="$1"
    local x86_release="$2"
    local universal_release="$3"

    log "Merging universal release"
    require_cmd lipo

    copy_tree "${arm_release}" "${universal_release}"

    # lipo-merge every Mach-O binary that appears in both trees
    while IFS= read -r -d '' file; do
        local rel="${file#${arm_release}/}"
        local other="${x86_release}/${rel}"
        local out="${universal_release}/${rel}"

        [[ -f "${other}" ]] || continue

        if is_macho "${file}" && is_macho "${other}"; then
            mkdir -p "$(dirname "${out}")"
            log "  lipo: ${rel}"
            lipo -create "${file}" "${other}" -output "${out}"
        fi
    done < <(find "${arm_release}" -type f -print0)

    # Merge companion tool archives into tools/macos/universal/
    local univ_tools="${universal_release}/tools/macos/universal"
    mkdir -p "${univ_tools}"

    local arm_tools="${arm_release}/tools/macos/arm64"
    local x86_tools="${x86_release}/tools/macos/x86_64"

    if [[ -d "${arm_tools}" && -d "${x86_tools}" ]]; then
        local gz
        for gz in "${arm_tools}"/*.gz; do
            [[ -f "${gz}" ]] || continue
            local name
            name="$(basename "${gz}")"

            # Derive the x86_64 counterpart: replace _arm64 with _x86_64 in name
            local x86_gz="${x86_tools}/${name//_arm64/_x86_64}"
            [[ -f "${x86_gz}" ]] || x86_gz="${x86_tools}/${name}"

            if [[ -f "${x86_gz}" ]]; then
                local tmp_arm tmp_x86 tmp_out
                tmp_arm="$(mktemp)"
                tmp_x86="$(mktemp)"
                tmp_out="$(mktemp)"

                gunzip -c "${gz}"     > "${tmp_arm}"
                gunzip -c "${x86_gz}" > "${tmp_x86}"

                # Strip arch/platform suffixes to build a clean universal name
                local tool_base="${name%.gz}"
                tool_base="${tool_base%%_macOS*}"
                tool_base="${tool_base%%_arm64*}"

                if is_macho "${tmp_arm}" && is_macho "${tmp_x86}"; then
                    lipo -create "${tmp_arm}" "${tmp_x86}" -output "${tmp_out}"
                    gzip -9 -c "${tmp_out}" > "${univ_tools}/${tool_base}_macOS_universal.gz"
                    log "  lipo tool: ${tool_base}"
                else
                    # Non-Mach-O (scripts, data) – keep the arm64 variant
                    cp "${gz}" "${univ_tools}/"
                fi

                rm -f "${tmp_arm}" "${tmp_x86}" "${tmp_out}"
            else
                cp "${gz}" "${univ_tools}/"
            fi
        done
    else
        warn "Tool archive directories not found; tools/macos/universal/ will be empty"
    fi

    log "Universal merge complete: ${universal_release}"
}

# ---------------------------------------------------------------------------
# macOS universal top-level flow
# ---------------------------------------------------------------------------
build_macos_universal() {
    local arm_build="${WORK_ROOT}/macos-arm64"
    local x86_build="${WORK_ROOT}/macos-x86_64"
    local arm_release="${DIST_ROOT}/gastera1n-macos-arm64_${GASTER_VERSION}"
    local x86_release="${DIST_ROOT}/gastera1n-macos-x86_64_${GASTER_VERSION}"
    local univ_release="${DIST_ROOT}/gastera1n-macos-universal_${GASTER_VERSION}"

    install_macos_build_deps

    build_single macos arm64  "${arm_build}" "${arm_release}"
    build_single macos x86_64 "${x86_build}" "${x86_release}"
    merge_universal_release "${arm_release}" "${x86_release}" "${univ_release}"

    local tarball="${univ_release}.tgz"
    tar -zcf "${tarball}" -C "${DIST_ROOT}" "$(basename "${univ_release}")"
    log "Done ➜ ${tarball}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    require_cmd git
    require_cmd file
    require_cmd tar
    require_cmd gzip
    require_cmd curl

    mkdir -p "${WORK_ROOT}" "${DIST_ROOT}"

    case "${TARGET_PLATFORM}" in
        macos)
            require_cmd xcrun
            require_cmd install_name_tool
            require_cmd lipo
            require_cmd otool

            case "${TARGET_ARCH}" in
                universal)
                    build_macos_universal
                    ;;
                arm64|x86_64)
                    install_macos_build_deps
                    local release_dir="${DIST_ROOT}/gastera1n-macos-${TARGET_ARCH}_${GASTER_VERSION}"
                    build_single macos "${TARGET_ARCH}" "${WORK_ROOT}" "${release_dir}"
                    local tarball="${release_dir}.tgz"
                    tar -zcf "${tarball}" -C "${DIST_ROOT}" "$(basename "${release_dir}")"
                    log "Done ➜ ${tarball}"
                    ;;
                *)
                    die "macOS TARGET_ARCH must be: arm64 | x86_64 | universal"
                    ;;
            esac
            ;;

        linux)
            case "${TARGET_ARCH}" in
                x86_64|arm64|aarch64)
                    # Normalise aarch64 → arm64 in the release name
                    local norm_arch="${TARGET_ARCH}"
                    [[ "${norm_arch}" == "aarch64" ]] && norm_arch="arm64"
                    local release_dir="${DIST_ROOT}/gastera1n-linux-${norm_arch}_${GASTER_VERSION}"
                    build_single linux "${TARGET_ARCH}" "${WORK_ROOT}" "${release_dir}"
                    local tarball="${release_dir}.tgz"
                    tar -zcf "${tarball}" -C "${DIST_ROOT}" "$(basename "${release_dir}")"
                    log "Done ➜ ${tarball}"
                    ;;
                *)
                    die "Linux TARGET_ARCH must be: x86_64 | arm64 | aarch64"
                    ;;
            esac
            ;;

        *)
            die "TARGET_PLATFORM must be: macos | linux"
            ;;
    esac
}

# Guard against accidental sourcing
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
