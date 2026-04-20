#!/bin/sh

matrix_os=$1
matrix_arch=$2
matrix_minos=$3
matrix_gnu_triple=$4

echo "Install dependencies (packages)"
brew install make autoconf automake pkg-config gnu-sed
brew install libzip libtool

echo "Download dependencies (embedded binaries)"
SDK=$(xcrun -sdk ${matrix_os} --show-sdk-path)

echo "Download dependencies (source code)"

# git clone https://github.com/curl/curl.git
git clone https://github.com/madler/zlib.git
git clone https://github.com/tihmstar/libgeneral.git
git clone https://github.com/tihmstar/libfragmentzip.git

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

# if [ "$matrix_os" != "macosx" ]; then
#   gsed -i '/case kIOUSBTransactionTimeout/d' src/libirecovery.c
# fi

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

curl -LOOOOOO "https://github.com/xerub/img4lib/releases/download/1.0/img4lib-2020-10-27.tar.gz"
tar -xzf img4lib-2020-10-27.tar.gz
mv img4lib-2020-10-27/apple/img4 .
rm -rf img4lib-2020-10-27 img4lib-2020-10-27.tar.gz
gzip -9 -S .gz img4

mkdir ${gastera1n}_v1.0
cd ${gastera1n}_v1.0
# cp -a ../libs_root .
cp -v ../LICENSE ../restored_external.gz ../ssh64* ../bootim@750x1334.im4p .
cp -v ../img4.gz ../ldid2.gz ../iBoot64Patcher.gz ../tsschecker_macOS_v440.gz  .
cp -v ../${gastera1n} ./gastera1n
cd ..
tar -zcf ${gastera1n}_v1.0.tgz ${gastera1n}_v1.0
