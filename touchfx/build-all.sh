#!/bin/sh
# Run from repository root inside the documented ARM64 GCC7 container.
set -eu
: "${AZ_SDK_INCLUDE:?Set AZ_SDK_INCLUDE to your checked-out SDK include directory}"
make -C core/src CC=gcc azmod
mkdir -p audio/out
# Preserve the build's source path: stb_vorbis embeds it in assert strings.
mkdir -p work
if [ ! -e work/stems-eight-078 ]; then ln -s ../audio work/stems-eight-078; fi
r=work/stems-eight-078
inc=$r/src/azstem
gcc -include "$r/compat-math.h" -O2 -fPIC -fvisibility=hidden -w -c "$inc/stb_vorbis.c" -o "$r/out/stb_vorbis.o"
gcc -include "$r/compat-math.h" -O2 -fPIC -fno-omit-frame-pointer -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-but-set-variable -Wno-misleading-indentation -DAZMOD_RELEASE -shared -Wl,--no-undefined "$inc/azstem.c" "$r/out/stb_vorbis.o" -lm -ldl -lpthread -o "$r/out/azstem.so"
make -C panel AZ_SDK_INCLUDE="$AZ_SDK_INCLUDE"
sha256sum core/src/out/azmod.so audio/out/azstem.so panel/out/mrdjjag-touchfx-test.so
