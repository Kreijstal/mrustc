#!/bin/bash
set -e
export RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0
if [ "$OSTYPE" = "cygwin" ]; then
    export CXXFLAGS_EXTRA="-D_GNU_SOURCE=1"
fi
make -j $(($(nproc) - 1))
make RUSTCSRC
make -f minicargo.mk LIBS
make test
make local_tests
## Build just rustc-driver BEFORE building llvm
#RUSTC_INSTALL_BINDIR=bin make -f minicargo.mk output-1.54.0/rustc-build/librustc_driver.rlib
RUSTC_INSTALL_BINDIR=bin make -f minicargo.mk output-1.54.0/rustc
./output-1.54.0/rustc --version

LIBGIT2_SYS_USE_PKG_CONFIG=1 make -f minicargo.mk output-1.54.0/cargo
./output-1.54.0/cargo --version
