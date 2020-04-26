#!/bin/bash

PREFIX=$1
GLIBC_DIR=$2

./configure --prefix=${PREFIX} --with-glibc-libdir=${GLIBC_DIR}/lib && \
make -j 4 && \
make install