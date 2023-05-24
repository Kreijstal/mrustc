#!/bin/bash
set -e
export RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0
if [ "$OSTYPE" = "cygwin" ]; then
    export CXXFLAGS_EXTRA="-D_GNU_SOURCE=1"
fi
make -j $(($(nproc) - 1))
make -f minicargo.mk RUSTCSRC
make -f minicargo.mk LIBS
make -f minicargo.mk test
make -f minicargo.mk output-1.19.0/stdtest/rustc_data_structures-test_out.txt
make -f minicargo.mk output-1.19.0/rustc
make -f minicargo.mk output-1.19.0/cargo
./output-1.19.0/cargo --version
