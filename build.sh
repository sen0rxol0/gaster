#!/bin/sh
# FIX: set -e aborts on any command failure so dependency build errors are
# not silently swallowed.  set -u treats unset variables as errors.
set -eu

matrix_os=$1
matrix_arch=$2
matrix_minos=$3
matrix_gnu_triple=$4

echo "Install dependencies (packages)"
brew install make autoconf automake pkg-config gnu-sed
brew install libzip libtool

echo "Download dependencies (source code)"
git clone https://github.com/madler/zlib.git
git clone https://github.com/tihmstar/libgeneral.git
git clone https://github.com/tihmstar/libfragmentzip.git
# libplist and libirecovery are NOT statically linked — the host app owns the
# canonical dylibs in its Frameworks bundle.  We build them as dylibs only so
# the linker can resolve symbols and record the dependency load commands.
git clone https://github.com/libimobiledevice/libplist.git
git -C libplist checkout c5a30e9267068436a75b5d00fcbf95cb9c1f4dcd
git clone https://github.com/libimobiledevice/libirecovery.git
git -C libirecovery checkout 1b9d9c3cdd3ef2f38198a21c356352f13641482d

echo "Select correct Xcode"
sudo xcode-select -s /Applications/Xcode_15.2.app

echo "Setup environment"
SDK=$(xcrun -sdk "${matrix_os}" --show-sdk-path)
NCPU=$(sysctl -n hw.ncpu)
DESTDIR=$(pwd)/sysroot
PREFIX=/usr/local
LOCAL_INCLUDE="${DESTDIR}/usr/local/include"
LOCAL_LIB="${DESTDIR}/usr/local/lib"

# FIX: export PKG_CONFIG_PATH so child configure scripts can find it
export PKG_CONFIG_PATH="${DESTDIR}/usr/local/lib/pkgconfig"

CONFIGURE_ARGS="--prefix=${PREFIX} --disable-shared --enable-static \
    --build=x86_64-apple-darwin --host=${matrix_gnu_triple}"

# FIX: xcrun --find returns a path; -E is a flag for CPP, not part of the
# path. The original `CPP=$(xcrun --find cc) -E` set CPP to just the path
# and silently dropped -E.  Use a wrapper that appends -E at call time.
export CC=$(xcrun --find cc)
export CXX=$(xcrun --find c++)
export CPP="${CC} -E"

# FIX: variable assignments with spaces must be exported with proper quoting.
# The original bare assignments (CFLAGS=-g -Os ...) caused the shell to
# interpret -Os and later words as separate commands.
export CFLAGS="-g -Os -arch ${matrix_arch} -m${matrix_os}-version-min=${matrix_minos} -isysroot ${SDK} -isystem ${LOCAL_INCLUDE}"
export CPPFLAGS="-g -Os -arch ${matrix_arch} -m${matrix_os}-version-min=${matrix_minos} -isysroot ${SDK} -isystem ${LOCAL_INCLUDE} -Wno-error-implicit-function-declaration"
export CXXFLAGS="-stdlib=libc++ -g -Os -isysroot ${SDK} -arch ${matrix_arch} -m${matrix_os}-version-min=${matrix_minos} -isystem ${LOCAL_INCLUDE}"
export LDFLAGS="-g -Wl,-dead_strip -arch ${matrix_arch} -isysroot ${SDK} -m${matrix_os}-version-min=${matrix_minos} -L${LOCAL_LIB}"
export CFLAGS_FOR_BUILD="-arch $(uname -m) -isysroot $(xcrun -sdk macosx --show-sdk-path) -Os"
export CXXFLAGS_FOR_BUILD="-stdlib=libc++ -arch $(uname -m) -isysroot $(xcrun -sdk macosx --show-sdk-path) -Os"
export CPPFLAGS_FOR_BUILD="-arch $(uname -m) -isysroot $(xcrun -sdk macosx --show-sdk-path) -Wno-error-implicit-function-declaration -Os"
export LDFLAGS_FOR_BUILD="-Wl,-dead_strip"

mkdir -p "${DESTDIR}/usr/local/lib"

# FIX: brace expansion {,64} is a bashism and is not valid in #!/bin/sh.
# Use an explicit ln for the lib64 symlink.
ln -sf "${DESTDIR}/usr/local/lib" "${DESTDIR}/usr/local/lib64"

echo "Build zlib"
cd zlib
# FIX: zlib ships its own minimal configure that does not accept the standard
# autoconf flags (--disable-shared, --build, --host).  Pass only --prefix and
# --static, then let the cross-compiler env vars do the rest.
./configure --prefix="${PREFIX}" --static
gmake -j"${NCPU}"
gmake -j"${NCPU}" install DESTDIR="${DESTDIR}"
cd ..

echo "Build libgeneral"
cd libgeneral
./autogen.sh ${CONFIGURE_ARGS}
gmake -j"${NCPU}"
gmake -j"${NCPU}" install DESTDIR="${DESTDIR}"
cd ..

echo "Build libfragmentzip"
cd libfragmentzip
./autogen.sh ${CONFIGURE_ARGS}
gmake -j"${NCPU}"
gmake -j"${NCPU}" install DESTDIR="${DESTDIR}"
cd ..

# Build libplist and libirecovery as dylibs only (no static archive).
# The host application already ships the canonical dylibs in its Frameworks
# bundle; we just need them present at link time and staged for the rpath.
DYLIB_CONFIGURE_ARGS="--prefix=${PREFIX} --enable-shared --disable-static \
    --build=x86_64-apple-darwin --host=${matrix_gnu_triple} \
    --without-cython"

echo "Build libplist (dylib)"
cd libplist
./autogen.sh ${DYLIB_CONFIGURE_ARGS}
gmake -j"${NCPU}"
gmake -j"${NCPU}" install DESTDIR="${DESTDIR}"
cd ..

echo "Build libirecovery (dylib)"
cd libirecovery
./autogen.sh ${DYLIB_CONFIGURE_ARGS}
gmake -j"${NCPU}"
gmake -j"${NCPU}" install DESTDIR="${DESTDIR}"
cd ..

ls -la

echo "Build gastera1n"
gastera1n="gastera1n-${matrix_os}-${matrix_arch}"
mkdir -p libs_root
cp -a "${DESTDIR}/${PREFIX}/include" libs_root/
cp -a "${DESTDIR}/${PREFIX}/lib"     libs_root/

gmake -j"${NCPU}"
mv gastera1n "${gastera1n}"

# Rewrite dylib load commands BEFORE stripping — strip can invalidate the
# symbol table that install_name_tool needs to locate LC_LOAD_DYLIB entries.
echo "Fix install names in ${gastera1n}"
for lib in libplist-2.0 libirecovery-1.0; do
    dylib_path=$(find "${LOCAL_LIB}" -name "${lib}*.dylib" ! -name '*-static*' | head -1)
    if [ -z "${dylib_path}" ]; then
        echo "WARNING: could not find dylib for ${lib}" >&2
        continue
    fi
    dylib_file=$(basename "${dylib_path}")
    install_name_tool \
        -change "${dylib_path}" \
        "@rpath/${dylib_file}" \
        "${gastera1n}"
    install_name_tool \
        -id "@rpath/${dylib_file}" \
        "${dylib_path}"
done

# Strip AFTER install_name_tool so load commands are not corrupted.
strip "${gastera1n}"

# Select the architecture-appropriate tool archives
if [ "${matrix_arch}" = "arm64" ]; then
    mv iBoot64Patcher_macOS_arm64.gz           iBoot64Patcher.gz
    mv ldid_v2.1.5-procursus7_macosx_arm64.gz ldid2.gz
else
    mv iBoot64Patcher_macOS_x86_64.gz           iBoot64Patcher.gz
    mv ldid_v2.1.5-procursus7_macosx_x86_64.gz ldid2.gz
fi

# FIX: -LOOOOOO is not a valid curl flag combination.  -L follows redirects,
# -O saves to a local file with the remote name.  Use -LO.
curl -LO "https://github.com/xerub/img4lib/releases/download/1.0/img4lib-2020-10-27.tar.gz"
tar -xzf img4lib-2020-10-27.tar.gz
mv img4lib-2020-10-27/apple/img4 .
rm -rf img4lib-2020-10-27 img4lib-2020-10-27.tar.gz
gzip -9 img4

release_dir="${gastera1n}_v1.0"
mkdir -p "${release_dir}"
cd "${release_dir}"
cp -v ../LICENSE \
      ../restored_external.gz \
      ../ssh64* \
      "../bootim@750x1334.im4p" \
      .
cp -v ../img4.gz \
      ../ldid2.gz \
      ../iBoot64Patcher.gz \
      ../tsschecker_macOS_v440.gz \
      .
cp -v "../${gastera1n}" ./gastera1n

# Stage the dylibs that the host app must place in Contents/Frameworks/.
# They are NOT bundled inside the gastera1n tool itself — this directory is
# informational, showing the host app which dylibs it needs to vendor.
mkdir -p Frameworks
for lib in libplist-2.0 libirecovery-1.0; do
    dylib_path=$(find "${LOCAL_LIB}" -name "${lib}*.dylib" ! -name '*-static*' | head -1)
    if [ -n "${dylib_path}" ]; then
        cp -v "${dylib_path}" Frameworks/
    else
        echo "WARNING: dylib for ${lib} not found, skipping" >&2
    fi
done
cd ..

tar -zcf "${release_dir}.tgz" "${release_dir}"
echo "Done: ${release_dir}.tgz"
