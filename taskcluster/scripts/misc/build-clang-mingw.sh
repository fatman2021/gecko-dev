#!/bin/bash
set -x -e -v

# This script is for building a mingw-clang toolchain for use on Linux.

if [[ $# -eq 0 ]]; then
    echo "Provide either x86 or x64 to specify a toolchain."
    exit 1;
elif [ "$1" == "x86" ]; then
  machine="i686"
  compiler_rt_machine="i386"
  crt_flags="--enable-lib32 --disable-lib64"
  WRAPPER_FLAGS=""
elif [ "$1" == "x64" ]; then
  machine="x86_64"
  compiler_rt_machine="x86_64"
  crt_flags="--disable-lib32 --enable-lib64"
  WRAPPER_FLAGS=""
else
  echo "Provide either x86 or x64 to specify a toolchain."
  exit 1;
fi

TOOLCHAIN_DIR=$MOZ_FETCHES_DIR/llvm-project
INSTALL_DIR=$MOZ_FETCHES_DIR/clang
CROSS_PREFIX_DIR=$INSTALL_DIR/$machine-w64-mingw32

make_flags="-j$(nproc)"

if [ -d "$MOZ_FETCHES_DIR/binutils/bin" ]; then
  export PATH="$MOZ_FETCHES_DIR/binutils/bin:$PATH"
fi

# This is default value of _WIN32_WINNT. Gecko configure script explicitly sets this,
# so this is not used to build Gecko itself. We default to 0x601, which is Windows 7.
default_win32_winnt=0x601

cd $GECKO_PATH

patch_file1="$(pwd)/taskcluster/scripts/misc/mingw-winrt.patch"
patch_file2="$(pwd)/taskcluster/scripts/misc/mingw-dwrite_3.patch"
patch_file3="$(pwd)/taskcluster/scripts/misc/mingw-unknown.patch"
patch_file4="$(pwd)/taskcluster/scripts/misc/mingw-enum.patch"
patch_file5="$(pwd)/taskcluster/scripts/misc/mingw-widl.patch"
patch_file6="$(pwd)/taskcluster/scripts/misc/mingw-dispatchqueue.patch"
patch_file7="$(pwd)/taskcluster/scripts/misc/mingw-numerics.patch"
patch_file8="$(pwd)/taskcluster/scripts/misc/mingw-mft-type.patch"
patch_file9="$(pwd)/taskcluster/scripts/misc/mingw-directmanip.patch"
patch_file10="$(pwd)/taskcluster/scripts/misc/mingw-ts_sd.patch"
patch_file11="$(pwd)/taskcluster/scripts/misc/mingw-winerror.patch"

prepare() {
  pushd $MOZ_FETCHES_DIR/mingw-w64
  patch -p1 <$patch_file1
  patch -p1 <$patch_file2
  patch -p1 <$patch_file3
  patch -p1 <$patch_file4
  patch -p1 <$patch_file5
  patch -p1 <$patch_file6
  patch -p1 <$patch_file7
  patch -p1 <$patch_file8
  patch -p1 <$patch_file9
  patch -p1 <$patch_file10
  patch -p1 <$patch_file11
  popd
}

install_wrappers() {
  pushd $INSTALL_DIR/bin

  compiler_flags="--sysroot \$DIR/../$machine-w64-mingw32 -rtlib=compiler-rt -stdlib=libc++ -fuse-ld=lld $WRAPPER_FLAGS -fuse-cxa-atexit -Qunused-arguments"

  cat <<EOF >$machine-w64-mingw32-clang
#!/bin/sh
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
\$DIR/clang -target $machine-w64-mingw32 $compiler_flags "\$@"
EOF
  chmod +x $machine-w64-mingw32-clang

  cat <<EOF >$machine-w64-mingw32-clang++
#!/bin/sh
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
\$DIR/clang -target $machine-w64-mingw32 --driver-mode=g++ $compiler_flags "\$@"
EOF
  chmod +x $machine-w64-mingw32-clang++

  CC="$machine-w64-mingw32-clang"
  CXX="$machine-w64-mingw32-clang++"

  popd
}

build_mingw() {
  mkdir mingw-w64-headers
  pushd mingw-w64-headers
  $MOZ_FETCHES_DIR/mingw-w64/mingw-w64-headers/configure \
    --host=$machine-w64-mingw32 \
    --enable-sdk=all \
    --enable-idl \
    --with-default-msvcrt=ucrt \
    --with-default-win32-winnt=$default_win32_winnt \
    --prefix=$CROSS_PREFIX_DIR
  make $make_flags install
  popd

  mkdir mingw-w64-crt
  pushd mingw-w64-crt
  $MOZ_FETCHES_DIR/mingw-w64/mingw-w64-crt/configure \
    --host=$machine-w64-mingw32 \
    $crt_flags \
    --with-default-msvcrt=ucrt \
    CC="$CC" \
    AR=llvm-ar \
    RANLIB=llvm-ranlib \
    DLLTOOL=llvm-dlltool \
    --prefix=$CROSS_PREFIX_DIR
  make $make_flags
  make $make_flags install
  popd

  mkdir widl
  pushd widl
  $MOZ_FETCHES_DIR/mingw-w64/mingw-w64-tools/widl/configure --target=$machine-w64-mingw32 --prefix=$INSTALL_DIR
  make $make_flags
  make $make_flags install
  popd
}

build_compiler_rt() {
  CLANG_VERSION=$(basename $(dirname $(dirname $(dirname $($CC --print-libgcc-file-name -rtlib=compiler-rt)))))
  mkdir compiler-rt
  pushd compiler-rt
  cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=$CC \
      -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_AR=$INSTALL_DIR/bin/llvm-ar \
      -DCMAKE_RANLIB=$INSTALL_DIR/bin/llvm-ranlib \
      -DCMAKE_C_COMPILER_WORKS=1 \
      -DCMAKE_C_COMPILER_TARGET=$compiler_rt_machine-windows-gnu \
      -DCOMPILER_RT_DEFAULT_TARGET_ONLY=TRUE \
      $TOOLCHAIN_DIR/compiler-rt/lib/builtins
  make $make_flags
  mkdir -p $INSTALL_DIR/lib/clang/$CLANG_VERSION/lib/windows
  cp lib/windows/libclang_rt.builtins-$compiler_rt_machine.a $INSTALL_DIR/lib/clang/$CLANG_VERSION/lib/windows/
  popd
}

build_runtimes() {
  # Below, we specify -g -gcodeview to build static libraries with debug information.
  # Because we're not distributing these builds, this is fine. If one were to distribute
  # the builds, perhaps one would want to make those flags conditional or investigation
  # other options.
  DEBUG_FLAGS="-g -gcodeview"

  # First configure libcxx
  mkdir runtimes
  pushd runtimes
  cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$CROSS_PREFIX_DIR \
      -DCMAKE_C_COMPILER=$CC \
      -DCMAKE_CXX_COMPILER=$CXX \
      -DCMAKE_CROSSCOMPILING=TRUE \
      -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER_WORKS=TRUE \
      -DCMAKE_CXX_COMPILER_WORKS=TRUE \
      -DLLVM_COMPILER_CHECKED=TRUE \
      -DCMAKE_AR=$INSTALL_DIR/bin/llvm-ar \
      -DCMAKE_RANLIB=$INSTALL_DIR/bin/llvm-ranlib \
      -DCMAKE_CXX_FLAGS="${DEBUG_FLAGS} -D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS" \
      -DLIBCXX_USE_COMPILER_RT=ON \
      -DLIBCXX_INSTALL_HEADERS=ON \
      -DLIBCXX_ENABLE_EXCEPTIONS=ON \
      -DLIBCXX_ENABLE_THREADS=ON \
      -DLIBCXX_HAS_WIN32_THREAD_API=ON \
      -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
      -DLIBCXX_ENABLE_SHARED=OFF \
      -DLIBCXX_SUPPORTS_STD_EQ_CXX11_FLAG=TRUE \
      -DLIBCXX_HAVE_CXX_ATOMICS_WITHOUT_LIB=TRUE \
      -DLIBCXX_ENABLE_EXPERIMENTAL_LIBRARY=OFF \
      -DLIBCXX_ENABLE_FILESYSTEM=OFF \
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=TRUE \
      -DLIBCXX_CXX_ABI=libcxxabi \
      -DLIBCXXABI_USE_LLVM_UNWINDER=TRUE \
      -DLIBCXXABI_ENABLE_STATIC_UNWINDER=TRUE \
      -DLLVM_NO_OLD_LIBSTDCXX=TRUE \
      -DLIBUNWIND_USE_COMPILER_RT=TRUE \
      -DLIBUNWIND_ENABLE_THREADS=TRUE \
      -DLIBUNWIND_ENABLE_SHARED=FALSE \
      -DLIBUNWIND_ENABLE_CROSS_UNWINDING=FALSE \
      -DLIBUNWIND_CXX_FLAGS="${DEBUG_FLAGS} -Wno-dll-attribute-on-redeclaration -nostdinc++ -DPSAPI_VERSION=2" \
      -DLIBUNWIND_C_FLAGS="-Wno-dll-attribute-on-redeclaration" \
      -DLIBCXXABI_USE_COMPILER_RT=ON \
      -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
      -DLIBCXXABI_ENABLE_THREADS=ON \
      -DLIBCXXABI_TARGET_TRIPLE=$machine-w64-mingw32 \
      -DLIBCXXABI_ENABLE_SHARED=OFF \
      -DLIBCXXABI_CXX_FLAGS="${DEBUG_FLAGS} -D_LIBCPP_HAS_THREAD_API_WIN32" \
      -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx;libunwind" \
      $TOOLCHAIN_DIR/runtimes

  make $make_flags VERBOSE=1
  make $make_flags install

  popd
}

build_libssp() {
  pushd $MOZ_FETCHES_DIR/gcc-source/

  # Massage the environment for the build-libssp.sh script
  mkdir -p ./$machine-w64-mingw32/lib
  cp $MOZ_FETCHES_DIR/llvm-mingw/libssp-Makefile .
  sed -i 's/set -e/set -x -e -v/' $MOZ_FETCHES_DIR/llvm-mingw/build-libssp.sh
  sed -i 's/(CROSS)gcc/(CROSS)clang/' libssp-Makefile
  sed -i 's/\$(CROSS)ar/llvm-ar/' libssp-Makefile
  OLDPATH=$PATH
  PATH=$INSTALL_DIR/bin:$PATH

  # Run the script
  TOOLCHAIN_ARCHS=$machine $MOZ_FETCHES_DIR/llvm-mingw/build-libssp.sh .

  # Grab the artifacts, cleanup
  cp $MOZ_FETCHES_DIR/gcc-source/$machine-w64-mingw32/lib/{libssp.a,libssp_nonshared.a} $INSTALL_DIR/$machine-w64-mingw32/lib/
  unset TOOLCHAIN_ARCHS
  PATH=$OLDPATH
  popd
}

build_utils() {
  pushd $INSTALL_DIR/bin/
  for prog in ar nm objcopy ranlib readobj strip; do
    ln -s llvm-$prog $machine-w64-mingw32-$prog
  done
  ./clang $MOZ_FETCHES_DIR/llvm-mingw/wrappers/windres-wrapper.c -O2 -Wl,-s -o $machine-w64-mingw32-windres
  popd
}

export PATH=$INSTALL_DIR/bin:$PATH

prepare

mkdir $TOOLCHAIN_DIR/build
pushd $TOOLCHAIN_DIR/build

install_wrappers
build_mingw
build_compiler_rt
build_runtimes
build_libssp
build_utils

popd

# Put a tarball in the artifacts dir
mkdir -p $UPLOAD_DIR

pushd $(dirname $INSTALL_DIR)
tar caf clangmingw.tar.zst clang
mv clangmingw.tar.zst $UPLOAD_DIR
popd
