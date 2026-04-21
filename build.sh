#!/usr/bin/env bash
set -euo pipefail

log() { printf '%s\n' "==> $*"; }
die() { printf '%s\n' "Error: $*" >&2; exit 1; }

TARGET_PLATFORM="${1:-macos}"     # macos | linux
TARGET_ARCH="${2:-universal}"     # arm64 | x86_64 | universal (macOS only)

ROOT_DIR="$(pwd)"
PREFIX="/usr/local"
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
MAKE_BIN="$(command -v gmake 2>/dev/null || command -v make 2>/dev/null || true)"
[ -n "${MAKE_BIN}" ] || die "make/gmake not found"

WORK_ROOT="${WORK_ROOT:-${ROOT_DIR}/.build/${TARGET_PLATFORM}-${TARGET_ARCH}}"
DIST_ROOT="${DIST_ROOT:-${ROOT_DIR}/dist}"
SRC_ROOT="${SRC_ROOT:-${WORK_ROOT}/src}"
SYSROOT="${SYSROOT:-${WORK_ROOT}/sysroot}"
RELEASE_ROOT="${RELEASE_ROOT:-${DIST_ROOT}/gastera1n-${TARGET_PLATFORM}-${TARGET_ARCH}_v1.0}"

XCODE_APP="${XCODE_APP:-/Applications/Xcode.app}"
UNIVERSAL_BASE_ARCH="${UNIVERSAL_BASE_ARCH:-x86_64}"   # used only for the final merged package

mkdir -p "${WORK_ROOT}" "${DIST_ROOT}"

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

is_macho() {
    file -b "$1" 2>/dev/null | grep -q 'Mach-O'
}

copy_tree() {
    # copy_tree SRC DST
    local src="$1"
    local dst="$2"
    rm -rf "$dst"
    mkdir -p "$dst"

    if command -v rsync >/dev/null 2>&1; then
        rsync -a "${src}/" "${dst}/"
    else
        # cp -R is portable across macOS/Linux.
        cp -R "${src}/." "${dst}/"
    fi
}

clone_sources() {
    rm -rf "${SRC_ROOT}"
    mkdir -p "${SRC_ROOT}"
    cd "${SRC_ROOT}"

    log "Download dependencies (source code)"
    git clone https://github.com/madler/zlib.git
    git -C zlib checkout "51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf"
    
    git clone https://github.com/tihmstar/libgeneral.git
    git -C libgeneral checkout "2c3cce029bfb440859cb4affc37c03ada39a0604"
    
    git clone https://github.com/tihmstar/libfragmentzip.git
    git -C libfragmentzip checkout "92f184e631c7156113850afdb9c68a2d892e35b6"

    # libplist and libirecovery are built as dylibs on macOS so the host app can
    # vendor the canonical dylibs in its Frameworks bundle.
    git clone https://github.com/libimobiledevice/libplist.git
    git -C libplist checkout "c5a30e9267068436a75b5d00fcbf95cb9c1f4dcd"

    git clone https://github.com/libimobiledevice/libirecovery.git
    git -C libirecovery checkout "1b9d9c3cdd3ef2f38198a21c356352f13641482d"

    git clone https://github.com/xerub/img4lib.git
    git -C img4lib submodule update --init --recursive
}

setup_macos_env() {
    local arch="$1"
    local sdk minos host_triple

    export DEVELOPER_DIR="${XCODE_APP}/Contents/Developer"
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
            die "unsupported macOS architecture: ${arch}"
            ;;
    esac

    export CC="$(xcrun --find clang)"
    export CXX="$(xcrun --find clang++)"
    export CPP="${CC} -E"
    export AR="$(xcrun --find ar)"
    export RANLIB="$(xcrun --find ranlib)"
    export STRIP="$(xcrun --find strip)"
    export LD="$(xcrun --find ld)"

    export CFLAGS="-g -Os -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk} -isystem ${SYSROOT}${PREFIX}/include"
    export CPPFLAGS="-g -Os -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk} -isystem ${SYSROOT}${PREFIX}/include -Wno-error-implicit-function-declaration"
    export CXXFLAGS="-stdlib=libc++ -g -Os -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk} -isystem ${SYSROOT}${PREFIX}/include"
    export LDFLAGS="-g -Wl,-dead_strip -arch ${arch} -mmacosx-version-min=${minos} -isysroot ${sdk} -L${SYSROOT}${PREFIX}/lib -Wl,-rpath,@executable_path/../Frameworks"

    export CFLAGS_FOR_BUILD="-arch $(uname -m) -isysroot ${sdk} -Os"
    export CXXFLAGS_FOR_BUILD="-stdlib=libc++ -arch $(uname -m) -isysroot ${sdk} -Os"
    export CPPFLAGS_FOR_BUILD="-arch $(uname -m) -isysroot ${sdk} -Wno-error-implicit-function-declaration -Os"
    export LDFLAGS_FOR_BUILD="-Wl,-dead_strip"

    export PKG_CONFIG_PATH="${SYSROOT}${PREFIX}/lib/pkgconfig:${SYSROOT}${PREFIX}/share/pkgconfig:${PKG_CONFIG_PATH:-}"
    export HOST_TRIPLE="${host_triple}"
    export BUILD_ARGS_COMMON=(--prefix="${PREFIX}" --host="${HOST_TRIPLE}")
}

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
            die "unsupported Linux architecture: ${arch}"
            ;;
    esac

    export CPP="${CC} -E"
    export AR="${AR:-ar}"
    export RANLIB="${RANLIB:-ranlib}"
    export STRIP="${STRIP:-strip}"
    export LD="${LD:-ld}"

    export CFLAGS="-g -Os -fPIC -I${SYSROOT}${PREFIX}/include"
    export CPPFLAGS="-g -Os -fPIC -I${SYSROOT}${PREFIX}/include -Wno-error-implicit-function-declaration"
    export CXXFLAGS="-g -Os -fPIC -I${SYSROOT}${PREFIX}/include"
    export LDFLAGS="-g -Wl,--gc-sections -L${SYSROOT}${PREFIX}/lib"

    export PKG_CONFIG_PATH="${SYSROOT}${PREFIX}/lib/pkgconfig:${SYSROOT}${PREFIX}/share/pkgconfig:${PKG_CONFIG_PATH:-}"
    export BUILD_ARGS_COMMON=(--prefix="${PREFIX}" --host="${HOST_TRIPLE}")
}

install_macos_dependencies() {
    if [[ "${INSTALL_DEPS:-0}" != "1" ]]; then
        return 0
    fi
    need_cmd brew
    log "Install dependencies (packages)"
    brew install make autoconf automake pkg-config gnu-sed libzip libtool jq
}

build_zlib() {
    log "Build zlib"
    cd "${SRC_ROOT}/zlib"
    ./configure --prefix="${PREFIX}" --static
    "${MAKE_BIN}" -j"${NCPU}"
    "${MAKE_BIN}" -j"${NCPU}" install DESTDIR="${SYSROOT}"
}

build_autotools_project() {
    # build_autotools_project DIR [extra configure args...]
    local dir="$1"
    shift
    cd "${dir}"
    ./autogen.sh "$@"
    "${MAKE_BIN}" -j"${NCPU}"
    "${MAKE_BIN}" -j"${NCPU}" install DESTDIR="${SYSROOT}"
}

build_libs() {
    build_zlib

    log "Build libgeneral"
    build_autotools_project "${SRC_ROOT}/libgeneral" "${BUILD_ARGS_COMMON[@]}" --disable-shared --enable-static

    log "Build libfragmentzip"
    build_autotools_project "${SRC_ROOT}/libfragmentzip" "${BUILD_ARGS_COMMON[@]}" --disable-shared --enable-static

    if [[ "${TARGET_PLATFORM}" == "macos" ]]; then
        log "Build libplist (dylib)"
        build_autotools_project "${SRC_ROOT}/libplist" "${BUILD_ARGS_COMMON[@]}" --enable-shared --disable-static --without-cython

        log "Build libirecovery (dylib)"
        build_autotools_project "${SRC_ROOT}/libirecovery" "${BUILD_ARGS_COMMON[@]}" --enable-shared --disable-static --without-cython
    else
        log "Build libplist (static)"
        build_autotools_project "${SRC_ROOT}/libplist" "${BUILD_ARGS_COMMON[@]}" --disable-shared --enable-static --without-cython

        log "Build libirecovery (static)"
        build_autotools_project "${SRC_ROOT}/libirecovery" "${BUILD_ARGS_COMMON[@]}" --disable-shared --enable-static --without-cython
    fi
}

build_img4() {
    log "Build img4lib / img4"

    cd "${SRC_ROOT}/img4lib"
    "${MAKE_BIN}" -C lzfse -j"${NCPU}"

    if [[ "${TARGET_PLATFORM}" == "macos" ]]; then
        "${MAKE_BIN}" -j"${NCPU}" COMMONCRYPTO=1
    else
        "${MAKE_BIN}" -j"${NCPU}"
    fi

    install -d "${SYSROOT}${PREFIX}/bin"
    install -m 755 ./img4 "${SYSROOT}${PREFIX}/bin/img4"
}

build_gastera1n() {
    log "Build gastera1n"

    mkdir -p "${ROOT_DIR}/libs_root"
    rm -rf "${ROOT_DIR}/libs_root/include" "${ROOT_DIR}/libs_root/lib"
    cp -a "${SYSROOT}${PREFIX}/include" "${ROOT_DIR}/libs_root/"
    cp -a "${SYSROOT}${PREFIX}/lib" "${ROOT_DIR}/libs_root/"

    "${MAKE_BIN}" -j"${NCPU}"

    if [[ ! -x "${ROOT_DIR}/gastera1n" ]]; then
        die "gastera1n binary was not produced"
    fi
}

fix_macos_install_names() {
    [[ "${TARGET_PLATFORM}" == "macos" ]] || return 0

    local bin="$1"
    local lib dylib_path dylib_file

    log "Fix install names in ${bin}"
    for lib in libplist-2.0 libirecovery-1.0; do
        dylib_path="$(find "${SYSROOT}${PREFIX}/lib" -name "${lib}*.dylib" ! -name '*-static*' | head -n 1 || true)"
        if [[ -z "${dylib_path}" ]]; then
            echo "WARNING: could not find dylib for ${lib}" >&2
            continue
        fi
        dylib_file="$(basename "${dylib_path}")"

        install_name_tool \
            -change "${dylib_path}" \
            "@rpath/${dylib_file}" \
            "${bin}"

        install_name_tool \
            -id "@rpath/${dylib_file}" \
            "${dylib_path}"
    done

    strip "${bin}" || true
}

select_arch_tool_archives() {
    local arch="$1"

    if [[ "${arch}" == "arm64" ]]; then
        mv -f "${ROOT_DIR}/iBoot64Patcher_macOS_arm64.gz" "${ROOT_DIR}/iBoot64Patcher.gz"
        mv -f "${ROOT_DIR}/ldid_v2.1.5-procursus7_macosx_arm64.gz" "${ROOT_DIR}/ldid2.gz"
    else
        mv -f "${ROOT_DIR}/iBoot64Patcher_macOS_x86_64.gz" "${ROOT_DIR}/iBoot64Patcher.gz"
        mv -f "${ROOT_DIR}/ldid_v2.1.5-procursus7_macosx_x86_64.gz" "${ROOT_DIR}/ldid2.gz"
    fi
}

stage_release_tree() {
    local release_dir="$1"
    local gaster_bin="$2"
    local img4_bin="$3"

    rm -rf "${release_dir}"
    mkdir -p "${release_dir}"

    cp -v "${ROOT_DIR}/LICENSE" \
          "${ROOT_DIR}/restored_external.gz" \
          "${ROOT_DIR}"/ssh64* \
          "${ROOT_DIR}/bootim@750x1334.im4p" \
          "${release_dir}/"

    cp -v "${ROOT_DIR}/img4.gz" \
          "${ROOT_DIR}/ldid2.gz" \
          "${ROOT_DIR}/iBoot64Patcher.gz" \
          "${ROOT_DIR}/tsschecker_macOS_v440.gz" \
          "${release_dir}/"

    cp -v "${gaster_bin}" "${release_dir}/gastera1n"
    cp -v "${img4_bin}" "${release_dir}/img4"

    mkdir -p "${release_dir}/Frameworks"
    while IFS= read -r -d '' dylib; do
        cp -v "${dylib}" "${release_dir}/Frameworks/"
    done < <(find "${SYSROOT}${PREFIX}/lib" -maxdepth 1 -name '*.dylib' ! -name '*-static*' -print0 2>/dev/null || true)

    cp -v "${ROOT_DIR}/README"* "${release_dir}/" 2>/dev/null || true
}

build_single() {
    local platform="$1"
    local arch="$2"
    local build_root="$3"
    local release_dir="$4"

    log "Start build: platform=${platform} arch=${arch}"

    rm -rf "${build_root}"
    mkdir -p "${build_root}"
    export WORK_ROOT="${build_root}"
    export SRC_ROOT="${build_root}/src"
    export SYSROOT="${build_root}/sysroot"

    mkdir -p "${SRC_ROOT}" "${SYSROOT}${PREFIX}/lib" "${SYSROOT}${PREFIX}/bin"

    clone_sources

    if [[ "${platform}" == "macos" ]]; then
        setup_macos_env "${arch}"
    else
        setup_linux_env "${arch}"
    fi

    build_libs
    build_img4
    build_gastera1n

    fix_macos_install_names "${ROOT_DIR}/gastera1n"

    select_arch_tool_archives "${arch}"

    # Keep the staged gastera1n/img4 in the per-build sysroot for later merging.
    install -d "${build_root}/artifacts"
    cp -v "${ROOT_DIR}/gastera1n" "${build_root}/artifacts/gastera1n"
    cp -v "${SYSROOT}${PREFIX}/bin/img4" "${build_root}/artifacts/img4"

    stage_release_tree \
        "${release_dir}" \
        "${build_root}/artifacts/gastera1n" \
        "${build_root}/artifacts/img4"

    log "Single build complete: ${release_dir}"
}

merge_universal_release() {
    local arm_release="$1"
    local x86_release="$2"
    local universal_release="$3"
    local base_release other_release
    local base_arch="${UNIVERSAL_BASE_ARCH}"

    case "${base_arch}" in
        arm64)
            base_release="${arm_release}"
            other_release="${x86_release}"
            ;;
        x86_64)
            base_release="${x86_release}"
            other_release="${arm_release}"
            ;;
        *)
            die "UNIVERSAL_BASE_ARCH must be arm64 or x86_64"
            ;;
    esac

    log "Merge universal release"
    rm -rf "${universal_release}"
    copy_tree "${base_release}" "${universal_release}"

    while IFS= read -r -d '' file; do
        rel="${file#${base_release}/}"
        other_file="${other_release}/${rel}"
        out_file="${universal_release}/${rel}"

        [[ -f "${other_file}" ]] || continue
        if is_macho "${file}" && is_macho "${other_file}"; then
            mkdir -p "$(dirname "${out_file}")"
            lipo -create "${file}" "${other_file}" -output "${out_file}"
        fi
    done < <(find "${base_release}" -type f -print0)

    log "Universal merge complete: ${universal_release}"
}

main() {
    need_cmd git
    need_cmd file
    need_cmd tar
    need_cmd gzip
    need_cmd curl

    if [[ "${TARGET_PLATFORM}" == "macos" ]]; then
        need_cmd xcrun
        need_cmd install_name_tool
        need_cmd lipo
        need_cmd strip
        install_macos_dependencies
    fi

    if [[ "${TARGET_PLATFORM}" == "macos" && "${TARGET_ARCH}" == "universal" ]]; then
        local arm_build="${WORK_ROOT}/macos-arm64"
        local x86_build="${WORK_ROOT}/macos-x86_64"
        local arm_release="${DIST_ROOT}/gastera1n-macos-arm64_v1.0"
        local x86_release="${DIST_ROOT}/gastera1n-macos-x86_64_v1.0"
        local universal_release="${DIST_ROOT}/gastera1n-macos-universal_v1.0"

        build_single macos arm64 "${arm_build}" "${arm_release}"
        build_single macos x86_64 "${x86_build}" "${x86_release}"
        merge_universal_release "${arm_release}" "${x86_release}" "${universal_release}"

        tar -zcf "${universal_release}.tgz" -C "${DIST_ROOT}" "$(basename "${universal_release}")"
        log "Done: ${universal_release}.tgz"
        return 0
    fi

    case "${TARGET_PLATFORM}" in
        macos)
            case "${TARGET_ARCH}" in
                arm64|x86_64) ;;
                *) die "macOS TARGET_ARCH must be arm64, x86_64, or universal" ;;
            esac
            build_single macos "${TARGET_ARCH}" "${WORK_ROOT}" "${RELEASE_ROOT}"
            ;;
        linux)
            case "${TARGET_ARCH}" in
                x86_64|arm64|aarch64) ;;
                *) die "Linux TARGET_ARCH must be x86_64, arm64, or aarch64" ;;
            esac
            build_single linux "${TARGET_ARCH}" "${WORK_ROOT}" "${RELEASE_ROOT}"
            ;;
        *)
            die "TARGET_PLATFORM must be macos or linux"
            ;;
    esac

    tar -zcf "${RELEASE_ROOT}.tgz" -C "${DIST_ROOT}" "$(basename "${RELEASE_ROOT}")"
    log "Done: ${RELEASE_ROOT}.tgz"
}

main "$@"