#!/bin/sh

matrix_os=$1
matrix_arch=$2
matrix_minos=$3
matrix_gnu_triple=$4

MBEDTLS_VERSION=3.5.2
READLINE_VERSION=8.2
LIBIMOBILEDEVICE_COMMIT=ed0d66d0341562731bb19928dfe48155509fa7a7
LIBIRECOVERY_COMMIT=7ce02c347b7c26e59498e6af31c9da51018d0fa1
LIBIMOBILEDEVICE_GLUE_COMMIT=362f7848ac89b74d9dd113b38b51ecb601f76094
LIBPLIST_COMMIT=2117b8fdb6b4096455bd2041a63e59a028120136
LIBUSBMUXD_COMMIT=a7f0543fb1ecb20ac7121c0fd77297200e0e43fc

echo "Install dependencies (packages)"
brew install make autoconf automake pkg-config gnu-sed
brew install libzip libtool
# curl -LO https://github.com/ProcursusTeam/ldid/releases/download/v2.1.5-procursus7/ldid_v2.1.5_macosx_x86_64
# sudo install -m755 ldid_v2.1.5_macosx_x86_64 /usr/local/bin/ldid_v2.1.5

echo "Download dependencies (embedded binaries)"
SDK=$(xcrun -sdk ${matrix_os} --show-sdk-path)

echo "Download dependencies (source code)"
git clone --depth=1 https://github.com/libimobiledevice/libplist
git clone --depth=1 https://github.com/libimobiledevice/libirecovery
git clone --depth=1 https://github.com/libimobiledevice/libimobiledevice-glue
git clone --depth=1 https://github.com/libimobiledevice/libimobiledevice
git clone --depth=1 https://github.com/libimobiledevice/libusbmuxd

cd libplist && git fetch origin $LIBPLIST_COMMIT && git reset --hard $LIBPLIST_COMMIT && cd ..
cd libirecovery && git fetch origin $LIBIRECOVERY_COMMIT && git reset --hard $LIBIRECOVERY_COMMIT && cd ..
cd libimobiledevice-glue && git fetch origin $LIBIMOBILEDEVICE_GLUE_COMMIT && git reset --hard $LIBIMOBILEDEVICE_GLUE_COMMIT && cd ..
cd libimobiledevice && git fetch origin $LIBIMOBILEDEVICE_COMMIT && git reset --hard $LIBIMOBILEDEVICE_COMMIT && cd ..
cd libusbmuxd && git fetch origin $LIBUSBMUXD_COMMIT && git reset --hard $LIBUSBMUXD_COMMIT && cd ..

# git clone https://github.com/curl/curl.git
git clone https://github.com/madler/zlib.git
git clone https://github.com/tihmstar/libgeneral.git
git clone https://github.com/tihmstar/libfragmentzip.git

curl -LOOOOOO \
  https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v$MBEDTLS_VERSION.tar.gz \
  https://mirror-hk.koddos.net/gnu/readline/readline-$READLINE_VERSION.tar.gz

tar -xf v$MBEDTLS_VERSION.tar.gz
tar -xf readline-$READLINE_VERSION.tar.gz

echo "Select correct Xcode"
sudo xcode-select -s /Applications/Xcode_15.2.app

echo "Setup environment"
NCPU=$(sysctl -n hw.ncpu)
DESTDIR=$(pwd)/sysroot
PREFIX=/usr/local
LOCAL_INCLUDE=$DESTDIR/usr/local/include
LOCAL_LIB=$DESTDIR/usr/local/lib
PKG_CONFIG_PATH=$DESTDIR/usr/local/lib/pkgconfig
CONFIGURE_ARGS=--prefix=$PREFIX --disable-shared --enable-static --build=x86_64-apple-darwin --host=$matrix_gnu_triple
CC=$(xcrun --find cc)
CXX=$(xcrun --find c++)
CPP=$(xcrun --find cc) -E
CFLAGS=-g -Os -arch $matrix_arch -m$matrix_os-version-min=$matrix_minos -isysroot ${SDK} -isystem $LOCAL_INCLUDE
CPPFLAGS=-g -Os -arch $matrix_arch -m$matrix_os-version-min=$matrix_minos -isysroot ${SDK} -isystem $LOCAL_INCLUDE -Wno-error-implicit-function-declaration -Os
CXXFLAGS=-stdlib=libc++ -g -Os -isysroot ${SDK} -arch $matrix_arch -m$matrix_os-version-min=$matrix_minos -isystem $LOCAL_INCLUDE -Os
LDFLAGS=-g -Wl,-dead_strip -arch $matrix_arch -isysroot ${SDK} -m$matrix_os-version-min=$matrix_minos -L$LOCAL_LIB
CFLAGS_FOR_BUILD=-arch $(uname -m) -isysroot $(xcrun -sdk macosx --show-sdk-path) -Os
CXXFLAGS_FOR_BUILD=-stdlib=libc++ -arch $(uname -m) -isysroot $(xcrun -sdk macosx --show-sdk-path) -Os
CPPFLAGS_FOR_BUILD=-arch $(uname -m) -isysroot $(xcrun -sdk macosx --show-sdk-path) -Wno-error-implicit-function-declaration -Os
LDFLAGS_FOR_BUILD=-Wl,-dead_strip

mkdir sysroot
mkdir -p $(pwd)/sysroot/usr/local/lib
ln -sf $(pwd)/sysroot/usr/local/lib{,64}

echo "Build libplist"
cd libplist
autoreconf -fiv
./configure ${CONFIGURE_ARGS} --without-cython
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..

echo "Build libimobiledevice-glue"
cd libimobiledevice-glue
autoreconf -fiv
./configure ${CONFIGURE_ARGS}
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..

echo "Build libirecovery"
sudo cp -a ${DESTDIR}${PREFIX}/* /usr/local
cd libirecovery
autoreconf -fiv

# if [ "$matrix_os" != "macosx" ]; then
#   gsed -i '/case kIOUSBTransactionTimeout/d' src/libirecovery.c
# fi

./configure ${CONFIGURE_ARGS}
echo -e 'all:\ninstall:' > tools/Makefile
make -j$NCPU LIBS="-lncurses"
make -j$NCPU install
install -m644 src/.libs/libirecovery-1.0.a ${DESTDIR}${PREFIX}/lib
cd ..

echo "Build libusbmuxd"
cd libusbmuxd
autoreconf -fiv
./configure ${CONFIGURE_ARGS}
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..

echo "Build Mbed TLS"
cd mbedtls-$MBEDTLS_VERSION
curl -LOOOOOO https://raw.githubusercontent.com/palera1n/palera1n/refs/heads/main/patches/mbedtls/0001-Allow-empty-x509-cert-issuer.patch
cat 0001-Allow-empty-x509-cert-issuer.patch | patch -sN -d . -p1
mkdir build
cd build
SDKROOT="$SDK" cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CROSSCOMPILING=true -DCMAKE_SYSTEM_NAME=Darwin \
-DCMAKE_C_COMPILER="$CC" -DCMAKE_C_FLAGS="$CFLAGS" \
-DCMAKE_FIND_ROOT_PATH="$DESTDIR" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
-DMBEDTLS_PYTHON_EXECUTABLE="/usr/local/bin/python3" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_INSTALL_SYSCONFDIR="/etc"
gmake -j$NCPU SDKROOT="$SDK"
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..
cd ..

echo "Build readline"
cd readline-$READLINE_VERSION
CC=clang CXX=clang++ ./configure ${CONFIGURE_ARGS} ac_cv_type_sig_atomic_t=no
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..

echo "Build libimobiledevice"
cd libimobiledevice
autoreconf -fiv
./configure ${CONFIGURE_ARGS} --with-mbedtls --enable-debug --disable-wireless-pairing
echo -e 'all:\ninstall:' > tools/Makefile
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..

# echo "Build curl"
# cd curl
# autoreconf -fiv
# ./configure ${CONFIGURE_ARGS} --with-openssl --without-libpsl
# gmake -j$NCPU
# gmake -j$NCPU install DESTDIR=${DESTDIR}
# cd ..

echo "Build libfragmentzip"
cd zlib
./configure ${CONFIGURE_ARGS}
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..
cd libgeneral
./autogen.sh ${CONFIGURE_ARGS}
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..
cd libfragmentzip
./autogen.sh ${CONFIGURE_ARGS}
gmake -j$NCPU
gmake -j$NCPU install DESTDIR=${DESTDIR}
cd ..

ls -la

echo "Build gastera1n"

gastera1n="gastera1n-$matrix_os-$matrix_arch"
mkdir libs_root
cp -a sysroot/$PREFIX/{include,lib} libs_root
# find libs_root -name '*.dylib' -delete
# find libs_root -name '*.la' -delete
gmake -j$NCPU
mv gastera1n $gastera1n
# dsymutil $gastera1n
strip $gastera1n

# if [ "$matrix_os" == "macosx" ]; then
  # ldid -S $gastera1n
# else
  # ldid -Ssrc/usb.xml $gastera1n
# fi

if [ "$matrix_arch" == "arm64" ]; then
    mv iBoot64Patcher_macOS_arm64.gz iBoot64Patcher.gz
    mv ldid_v2.1.5-procursus7_macosx_arm64.gz ldid2.gz
else
    mv iBoot64Patcher_macOS_x86_64.gz iBoot64Patcher.gz
    mv ldid_v2.1.5-procursus7_macosx_x86_64.gz ldid2.gz
fi

mkdir ${gastera1n}_v1.0
cd ${gastera1n}_v1.0
# cp -a ../libs_root .
cp ../LICENSE ../restored_external.gz ../ssh64* ../bootim@750x1334.im4p .
cp ../iBoot64Patcher.gz ../tsschecker_macOS_v440.gz ../ldid2.gz .
cp ../$gastera1n .
cd ..
tar -zcf ${gastera1n}_v1.0.tgz ${gastera1n}_v1.0
