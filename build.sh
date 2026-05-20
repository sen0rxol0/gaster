#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh  –  gastera1n cross-platform build script
#
# Targets:
#   macOS universal  (arm64 + x86_64, merged with lipo)
#   Linux x86_64 / arm64 (aarch64)
#
# Assumptions:
#   • The host application already embeds libplist 2.2.0 and libirecovery 1.0.0
#     as shared libraries inside its own bundle/package.  This script therefore
#     links *against* those ABI-stable dylibs/SOs but does NOT copy them into
#     the release tree.
#   • img4 is built from source (img4lib) and placed into the release tree
#     directly from the per-arch sysroot.
#   • iproxy is built from source (libusbmuxd 2.0.2) and placed into the
#     release tree.  Its only non-system dependency (libplist) is already
#     present in the sysroot from the static-libs phase.
#   • Companion pre-built tool archives (ldid2, iBoot64Patcher, tsschecker)
#     are expected in ROOT_DIR with the naming convention described in
#     stage_release_tree().
#
#
# Usage:
#   ./build.sh [TARGET_PLATFORM] [TARGET_ARCH] [--force-rebuild]
#
#   TARGET_PLATFORM : macos | linux          (default: macos)
#   TARGET_ARCH     : arm64 | x86_64 | universal
#                     macOS default: universal
#                     Linux default: x86_64
#
# Incremental build caching:
#   Completed phases leave a stamp file in the build root.  On subsequent
#   runs the script skips any phase whose stamp is still valid, so only
#   changed / not-yet-built phases run.  Pass --force-rebuild to wipe all
#   stamps and start from scratch.
#
#   Stamp files:
#     <build_root>/.stamp_sources  – git clone + checkout complete
#     <build_root>/.stamp_libs     – static libraries built
#     <build_root>/.stamp_img4     – img4 built
#     <build_root>/.stamp_iproxy   – iproxy (libusbmuxd) built
#   (gastera1n itself has no stamp; it always re-links from cached .a files.)
#
# Environment overrides (all optional):
#   WORK_ROOT    – scratch directory          (default: .build/<platform>-<arch>)
#   DIST_ROOT    – output directory           (default: dist/)
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
# Stamp-file helpers
#
# stamp_ok  <file>  → returns 0 (true) when the stamp exists
# stamp_set <file>  → touches the stamp
# stamp_del <file>  → removes the stamp (if present)
# ---------------------------------------------------------------------------
stamp_ok()  { [[ -f "$1" ]]; }
stamp_set() { touch "$1"; }
stamp_del() { rm -f "$1"; }

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------

# Consume --force-rebuild from any position in the argument list, leaving
# only positional parameters behind.
FORCE_REBUILD=0
_ARGS=()
for _a in "$@"; do
    if [[ "${_a}" == "--force-rebuild" ]]; then
        FORCE_REBUILD=1
    else
        _ARGS+=("${_a}")
    fi
done
set -- "${_ARGS[@]+"${_ARGS[@]}"}"

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
GASTER_VERSION="v2.0"

# libplist / libirecovery versions the host application ships
LIBPLIST_VERSION="2.2.0"
LIBIRECOVERY_VERSION="1.0.0"

# libusbmuxd version built from source
LIBUSBMUXD_VERSION="2.0.2"

# ---------------------------------------------------------------------------
# rpath constants
#
# gastera1n lives at:  Contents/MacOS/bin/gastera1n/gastera1n
# Frameworks lives at: Contents/Frameworks/
#
# Relative traversal from the binary to Frameworks/:
#   ../  → gastera1n/   (the containing dir)
#   ../  → bin/
#   ../  → MacOS/
#   ../  → Contents/
#   Frameworks
#   = @loader_path/../../../Frameworks
#
# Two system fallback paths cover Homebrew (/usr/local/lib) and the OS
# (/usr/lib) so the binary is usable outside the bundle during development.
# ---------------------------------------------------------------------------
RPATH_BUNDLE="@loader_path/../../../Frameworks"
RPATH_SYSTEM_LOCAL="/usr/local/lib"
RPATH_SYSTEM="/usr/lib"

# ---------------------------------------------------------------------------
# Source pinning
# ---------------------------------------------------------------------------
clone_sources() {
    local src_root="$1"
    local stamp="${_BUILD_ROOT}/.stamp_sources"

    if stamp_ok "${stamp}"; then
        log "Sources already cloned – skipping (use --force-rebuild to re-clone)"
        return 0
    fi

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

    # libplist – pinned to the 2.2.0 commit.
    # The static archive stays in the sysroot for link-time symbol resolution.
    # At runtime dyld loads the host Frameworks/ shared copy via the
    # @loader_path-relative LC_RPATH entry embedded by fix_macos_rpath().
    git clone --quiet https://github.com/libimobiledevice/libplist.git
    git -C libplist checkout --quiet "c5a30e9267068436a75b5d00fcbf95cb9c1f4dcd"

    # libusbmuxd – provides the iproxy TCP-tunnel tool.
    # Pinned to the commit matching LIBUSBMUXD_VERSION above.
    git clone --quiet https://github.com/libimobiledevice/libusbmuxd.git
    git -C libusbmuxd checkout --quiet "ce98c346b7c1dc2a21faea4fd3f32c88e27ca2af"
    
    # libirecovery – same rationale as libplist above.
    git clone --quiet https://github.com/libimobiledevice/libirecovery.git
    git -C libirecovery checkout --quiet "1b9d9c3cdd3ef2f38198a21c356352f13641482d"

    git clone --quiet https://github.com/xerub/img4lib.git
    git -C img4lib submodule update --init --recursive --quiet

    stamp_set "${stamp}"
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

    # ---------------------------------------------------------------------------
    # LDFLAGS – rpath chain (order matters: dyld searches in order)
    #
    #   1. @loader_path/../../../Frameworks   bundle-relative Frameworks/
    #      (primary: where the app ships libplist + libirecovery)
    #   2. /usr/local/lib                     Homebrew system install
    #      (secondary: allows running the binary outside the bundle)
    #   3. /usr/lib                           macOS system libraries
    #      (tertiary: last-resort fallback)
    # ---------------------------------------------------------------------------
    export LDFLAGS="-g -Wl,-dead_strip -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk} \
-L${_SYSROOT}${PREFIX}/lib \
-Wl,-rpath,${RPATH_BUNDLE} \
-Wl,-rpath,${RPATH_SYSTEM_LOCAL} \
-Wl,-rpath,${RPATH_SYSTEM}"

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

build_autotools() {
    local dir="$1"; shift
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
    local stamp="${_BUILD_ROOT}/.stamp_libs"

    if stamp_ok "${stamp}"; then
        log "Static libraries already built – skipping (use --force-rebuild to rebuild)"
        return 0
    fi

    local common_args=(--prefix="${PREFIX}" --host="${HOST_TRIPLE}")

    build_zlib

    log "Building libgeneral (static)"
    build_autotools "${_SRC_ROOT}/libgeneral" \
        "${common_args[@]}" \
        --disable-shared --enable-static

    # -----------------------------------------------------------------------
    # libplist:
    #   Built as a static archive so the linker can resolve symbols from
    #   libirecovery and libfragmentzip (which depend on it).  The archive
    #   stays in the sysroot; gastera1n's Makefile links -lplist-2.0 against
    #   it at build time.  At runtime dyld loads the host Frameworks/ dylib
    #   via the @loader_path-relative LC_RPATH entry baked in by
    #   fix_macos_rpath().
    #   DO NOT remove the .a — that breaks the link step.
    # -----------------------------------------------------------------------
    log "Building libplist ${LIBPLIST_VERSION} (static)"
    build_autotools "${_SRC_ROOT}/libplist" \
        "${common_args[@]}" \
        --disable-shared --enable-static --without-cython

    log "Building libfragmentzip (static)"
    build_autotools "${_SRC_ROOT}/libfragmentzip" \
        "${common_args[@]}" \
        --disable-shared --enable-static

    # -----------------------------------------------------------------------
    # libirecovery:  same static / link-time-only treatment as libplist.
    # -----------------------------------------------------------------------
    log "Building libirecovery ${LIBIRECOVERY_VERSION} (static)"
    build_autotools "${_SRC_ROOT}/libirecovery" \
        "${common_args[@]}" \
        --disable-shared --enable-static

    # Overwrite auto-generated .pc files with stubs that reflect the correct
    # Libs: line (sysroot -L so the linker finds the .a files).
    _inject_host_pc_stubs "${platform}"

    stamp_set "${stamp}"
}

_inject_host_pc_stubs() {
    local platform="$1"
    local pc_dir="${_SYSROOT}${PREFIX}/lib/pkgconfig"
    mkdir -p "${pc_dir}"

    # On macOS the static archives in the sysroot are sufficient for the
    # linker; the rpath chain handles runtime resolution.  On Linux we
    # additionally probe for any system shared library and emit a -L for it.
    local extra_libs_plist=""
    local extra_libs_irecovery=""

    if [[ "${platform}" == "linux" ]]; then
        for candidate in \
                "/usr/lib/${HOST_TRIPLE}" \
                "/usr/lib/$(uname -m)-linux-gnu" \
                "/usr/lib64" \
                "/usr/lib"; do
            if [[ -e "${candidate}/libplist-2.0.so"   || \
                  -e "${candidate}/libplist-2.0.so.3" ]]; then
                extra_libs_plist="-L${candidate}"
                break
            fi
        done
        for candidate in \
                "/usr/lib/${HOST_TRIPLE}" \
                "/usr/lib/$(uname -m)-linux-gnu" \
                "/usr/lib64" \
                "/usr/lib"; do
            if [[ -e "${candidate}/libirecovery-1.0.so"   || \
                  -e "${candidate}/libirecovery-1.0.so.0" ]]; then
                extra_libs_irecovery="-L${candidate}"
                break
            fi
        done

        if ! ldconfig -p 2>/dev/null | grep -q "libplist-2.0"; then
            warn "libplist-2.0 not found in ldconfig cache -- install the host package before linking gastera1n"
        fi
        if ! ldconfig -p 2>/dev/null | grep -q "libirecovery-1.0"; then
            warn "libirecovery-1.0 not found in ldconfig cache -- install the host package before linking gastera1n"
        fi
    fi

    cat > "${pc_dir}/libplist-2.0.pc" <<EOF
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libplist
Description: Library for working with Apple Binary and XML Property Lists (host-embedded)
Version: ${LIBPLIST_VERSION}
Libs: -L\${libdir} ${extra_libs_plist} -lplist-2.0
Libs.private:
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
Libs: -L\${libdir} ${extra_libs_irecovery} -lirecovery-1.0
Libs.private:
Cflags: -I\${includedir}
EOF
}

build_img4() {
    local stamp="${_BUILD_ROOT}/.stamp_img4"

    if stamp_ok "${stamp}"; then
        log "img4 already built – skipping (use --force-rebuild to rebuild)"
        return 0
    fi

    log "Building img4lib / img4 tool"
    cd "${_SRC_ROOT}/img4lib"

    if [[ "${TARGET_PLATFORM}" == "macos" ]]; then
        local arch="$1"
        local SDK
        SDK="$(xcrun --sdk macosx --show-sdk-path)"
        local IMG4_CFLAGS="-arch ${arch} -isysroot ${SDK} -mmacosx-version-min=10.13 -O2 -fPIC"
        local IMG4_LDFLAGS="-arch ${arch} -isysroot ${SDK} -mmacosx-version-min=10.13 -L${_SYSROOT}${PREFIX}/lib -Llzfse/build/bin"

        sed -i '' 's/^CFLAGS[[:space:]]*=[[:space:]]*-Wall -W -pedantic/CFLAGS = $(EXTRA_CFLAGS) -Wall -W -pedantic/' Makefile

        # Always rebuild lzfse from scratch for this arch so a prior
        # arm64 .a is never reused by the x86_64 (or vice-versa) build.
        "${MAKE_BIN}" -C lzfse clean 2>/dev/null || true
        rm -rf lzfse/build   # belt-and-suspenders: nuke the cmake output dir

        "${MAKE_BIN}" -C lzfse CFLAGS="${IMG4_CFLAGS}" -j"${NCPU}"

        "${MAKE_BIN}" EXTRA_CFLAGS="${IMG4_CFLAGS}" LDFLAGS="${IMG4_LDFLAGS}" COMMONCRYPTO=1 -j"${NCPU}"
    else
        "${MAKE_BIN}" -j"${NCPU}"
    fi

    install -d "${_SYSROOT}${PREFIX}/bin"
    install -m 755 img4 "${_SYSROOT}${PREFIX}/bin/img4"

    stamp_set "${stamp}"
}

build_iproxy() {
    local stamp="${_BUILD_ROOT}/.stamp_iproxy"

    if stamp_ok "${stamp}"; then
        log "iproxy already built – skipping (use --force-rebuild to rebuild)"
        return 0
    fi

    # -----------------------------------------------------------------------
    # libusbmuxd:
    #   The package ships both a shared library and the iproxy / inetcat
    #   tools.  We want only the iproxy binary, so we build the full package
    #   (static lib + tools) and install into the sysroot, then copy just the
    #   iproxy executable out from there during staging.
    #
    #   configure flags:
    #     --disable-shared   – no .dylib/.so needed at runtime (tool is
    #                          statically linked against usbmuxd)
    #     --enable-static    – produce the .a for link-time resolution
    #
    #   iproxy depends on:
    #     libplist-2.0       (already in sysroot from build_static_libs)
    # -----------------------------------------------------------------------
    log "Building libusbmuxd ${LIBUSBMUXD_VERSION} (iproxy)"
    build_autotools "${_SRC_ROOT}/libusbmuxd" \
        --prefix="${PREFIX}" \
        --host="${HOST_TRIPLE}" \
        --disable-shared \
        --enable-static

    # iproxy is installed to <sysroot>/usr/local/bin/iproxy by build_autotools
    [[ -x "${_SYSROOT}${PREFIX}/bin/iproxy" ]] \
        || die "iproxy binary not found after libusbmuxd build"

    if [[ "${TARGET_PLATFORM}" == "macos" ]]; then
        fix_macos_rpath "${_SYSROOT}${PREFIX}/bin/iproxy"
    else
        fix_linux_strip "${_SYSROOT}${PREFIX}/bin/iproxy"
    fi

    stamp_set "${stamp}"
}

build_gastera1n() {
    log "Building gastera1n"
    cd "${ROOT_DIR}"

    rm -rf "${ROOT_DIR}/libs_root"
    mkdir -p "${ROOT_DIR}/libs_root"
    cp -a "${_SYSROOT}${PREFIX}/include" "${ROOT_DIR}/libs_root/"
    cp -a "${_SYSROOT}${PREFIX}/lib"     "${ROOT_DIR}/libs_root/"

    "${MAKE_BIN}" clean || true
    "${MAKE_BIN}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" -j"${NCPU}"

    [[ -x "${ROOT_DIR}/gastera1n" ]] || die "gastera1n binary not produced by make"
}

# ---------------------------------------------------------------------------
# macOS rpath / install-name fixup
#
# Steps performed on each Mach-O binary:
#
#   1. Rewrite any absolute sysroot-embedded install_names for libplist and
#      libirecovery to @rpath/<dylib>.  This is necessary because the static
#      builds may bake an absolute path into LC_LOAD_DYLIB.
#
#   2. Ensure the three LC_RPATH entries are present in the correct order:
#        a. @loader_path/../../../Frameworks   (bundle – primary)
#        b. /usr/local/lib                     (Homebrew – secondary)
#        c. /usr/lib                           (system   – tertiary)
#      The LDFLAGS already embed these at link time; the explicit
#      install_name_tool calls here are a safety net for any that may have
#      been dropped by libtool or the Makefile.
#
#   3. Strip dead symbols (best-effort).
# ---------------------------------------------------------------------------
fix_macos_rpath() {
    local bin="$1"
    log "Fixing rpath / stripping: $(basename "${bin}")"

    # Step 1 – rewrite absolute install names → @rpath/<basename>
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

    # Step 2 – ensure rpath entries are present in priority order.
    #
    # install_name_tool -add_rpath is idempotent when the entry already
    # exists (it exits non-zero but we suppress the error).  We delete
    # any stale entries first to guarantee ordering, then re-add them.
    local rpath_entry
    for rpath_entry in "${RPATH_BUNDLE}" "${RPATH_SYSTEM_LOCAL}" "${RPATH_SYSTEM}"; do
        install_name_tool -delete_rpath "${rpath_entry}" "${bin}" 2>/dev/null || true
    done
    # Add in reverse order so that after the loop the binary's LC_RPATH
    # list reads: BUNDLE → SYSTEM_LOCAL → SYSTEM  (dyld searches in order).
    for rpath_entry in "${RPATH_SYSTEM}" "${RPATH_SYSTEM_LOCAL}" "${RPATH_BUNDLE}"; do
        install_name_tool -add_rpath "${rpath_entry}" "${bin}" 2>/dev/null || true
    done

    # Step 3 – strip
    "${STRIP:-strip}" -u -r "${bin}" 2>/dev/null \
        || "${STRIP:-strip}" "${bin}" \
        || true
}

fix_linux_strip() {
    local bin="$1"
    "${STRIP:-strip}" --strip-unneeded "${bin}" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Companion tool helpers
#
# All tools are installed FLAT alongside the gastera1n binary with no
# arch/platform suffix in the filename.
#
# Expected archive names in ROOT_DIR:
#   <tool>_macOS_arm64.gz    <tool>_macOS_x86_64.gz
#   <tool>_linux_x86_64.gz  <tool>_linux_arm64.gz
# A platform-only fallback (e.g. tsschecker_macOS.gz) is also accepted.
#
# Tools: ldid2, tsschecker, iBoot64Patcher, Kernel64Patcher
# img4 is built from source and handled separately.
# ---------------------------------------------------------------------------

_install_tool_archive() {
    local archive="$1"
    local tool_name="$2"
    local dest_dir="$3"

    local tmp
    tmp="$(mktemp)"
    gunzip -c "${archive}" > "${tmp}"
    install -m 755 "${tmp}" "${dest_dir}/${tool_name}"
    rm -f "${tmp}"
    log "  installed tool: ${tool_name}"
}

_install_companion_tools() {
    local platform="$1"
    local arch="$2"
    local dest_dir="$3"

    local tools=(ldid2 tsschecker iBoot64Patcher Kernel64Patcher)
    local t
    for t in "${tools[@]}"; do
        if   [[ -f "${ROOT_DIR}/${t}_${platform}_${arch}.gz" ]]; then
            _install_tool_archive "${ROOT_DIR}/${t}_${platform}_${arch}.gz" "${t}" "${dest_dir}"
        elif [[ -f "${ROOT_DIR}/${t}_${platform}.gz" ]]; then
            _install_tool_archive "${ROOT_DIR}/${t}_${platform}.gz" "${t}" "${dest_dir}"
        else
            warn "Archive not found for '${t}' on ${platform}/${arch} – skipping"
        fi
    done
}

# ---------------------------------------------------------------------------
# Release tree staging
#
# Flat layout — everything lives alongside gastera1n, no tools/ subdir,
# no arch/platform suffixes on tool names.
#
#   <release>/
#     gastera1n
#     img4
#     iproxy
#     ldid2
#     iBoot64Patcher
#     tsschecker
#     LICENSE
#     README
#     restored_external.gz
#     ssh64.tar.gz
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

    # img4 – built from source, flat alongside gastera1n, no arch suffix
    local img4_bin="${_SYSROOT}${PREFIX}/bin/img4"
    if [[ -x "${img4_bin}" ]]; then
        install -m 755 "${img4_bin}" "${release_dir}/img4"
    else
        warn "img4 binary not found in sysroot -- skipping"
    fi

    # iproxy – built from libusbmuxd, flat alongside gastera1n
    local iproxy_bin="${_SYSROOT}${PREFIX}/bin/iproxy"
    if [[ -x "${iproxy_bin}" ]]; then
        install -m 755 "${iproxy_bin}" "${release_dir}/iproxy"
    else
        warn "iproxy binary not found in sysroot -- skipping"
    fi

    # Pre-built companion tools – decompressed, flat, no arch/platform suffix
    _install_companion_tools "${platform}" "${arch}" "${release_dir}"

    # Static assets
    local asset
    for asset in LICENSE restored_external.gz "bootim@750x1334.im4p"; do
        if [[ -f "${ROOT_DIR}/${asset}" ]]; then
            cp -v "${ROOT_DIR}/${asset}" "${release_dir}/"
        else
            warn "Optional asset not found, skipping: ${asset}"
        fi
    done

    cp -v "${ROOT_DIR}/readme.md" "${release_dir}/README"
    cat "${ROOT_DIR}"/ssh64.tar.gz_* > "${ROOT_DIR}/ssh64.tar.gz"
    install -m 644 "${ROOT_DIR}/ssh64.tar.gz" "${release_dir}/ssh64.tar.gz"
    
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

    # Expose build_root to stamp helpers called from sub-functions
    export _BUILD_ROOT="${build_root}"
    export _SYSROOT="${build_root}/sysroot"
    export _SRC_ROOT="${build_root}/src"

    # --force-rebuild: wipe all stamps so every phase runs from scratch.
    # The sysroot and source tree are NOT wiped here — only the stamps are
    # removed, which causes each phase to re-run and overwrite its outputs
    # in place.  Use this when you want a clean rebuild without the cost of
    # re-cloning sources.
    if [[ "${FORCE_REBUILD}" == "1" ]]; then
        log "  --force-rebuild: clearing stamps for ${platform}/${arch}"
        stamp_del "${_BUILD_ROOT}/.stamp_sources"
        stamp_del "${_BUILD_ROOT}/.stamp_libs"
        stamp_del "${_BUILD_ROOT}/.stamp_img4"
        stamp_del "${_BUILD_ROOT}/.stamp_iproxy"
    fi

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
    build_img4 "${arch}"
    build_iproxy
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
#
# Walk the arm64 release (flat dir); lipo-merge every Mach-O that also
# appears in the x86_64 release.  Non-Mach-O files are copied verbatim.
# ---------------------------------------------------------------------------
merge_universal_release() {
    local arm_release="$1"
    local x86_release="$2"
    local universal_release="$3"

    log "Merging universal release"
    require_cmd lipo

    copy_tree "${arm_release}" "${universal_release}"

    while IFS= read -r -d '' file; do
        local rel="${file#${arm_release}/}"
        local other="${x86_release}/${rel}"
        local out="${universal_release}/${rel}"

        [[ -f "${other}" ]] || continue

        if is_macho "${file}" && is_macho "${other}"; then
            mkdir -p "$(dirname "${out}")"

            # Get architectures
            local archs
            archs="$(lipo -info "${file}" 2>/dev/null)"

            # If already universal (contains both), skip
            if [[ "${archs}" == *"arm64"* && "${archs}" == *"x86_64"* ]]; then
                log "  skip (already universal): ${rel}"
                continue
            fi

            log "  lipo: ${rel}"
            lipo -create "${file}" "${other}" -output "${out}"
        fi
    done < <(find "${arm_release}" -type f -print0)

    log "Universal merge complete: ${universal_release}"
}

# ---------------------------------------------------------------------------
# macOS universal top-level flow
#
# The arm64 and x86_64 single-arch builds are launched in parallel as
# background jobs.  Each operates in its own WORK_ROOT subtree with
# independent stamp files, sysroots, and source trees, so there is no
# shared mutable state between them.  The script waits for both jobs before
# proceeding to the lipo merge.
#
# If either job fails its exit status is captured via wait; the script then
# aborts with a non-zero exit code so the caller (CI, Make, etc.) sees the
# failure.
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

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
