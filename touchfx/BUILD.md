# Build the custom components

Use the ARM64 Ubuntu18.04 GCC7 toolchain described in core/UPSTREAM-BUILDING.md.
The SDK commit is recorded in PROVENANCE.json. These are source snapshots of the
working local build, not an upstream-supported Community Mod installation.

1. Obtain a matching official azmod.so yourself. In core/, run:
   `python3 tools/extract-assets.py /path/to/official/azmod.so`.
   Every array must pass template verification. Firmware arrays and database keys
   are deliberately absent from this repository.
2. In the ARM64 build container run `make -C core/src CC=gcc azmod`.
3. Build audio in the same container:
   `mkdir -p audio/out`
   `gcc -include audio/compat-math.h -O2 -fPIC -fvisibility=hidden -w -c audio/src/azstem/stb_vorbis.c -o audio/out/stb_vorbis.o`
   `gcc -include audio/compat-math.h -O2 -fPIC -fno-omit-frame-pointer -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-but-set-variable -Wno-misleading-indentation -DAZMOD_RELEASE -shared -Wl,--no-undefined audio/src/azstem/azstem.c audio/out/stb_vorbis.o -lm -ldl -lpthread -o audio/out/azstem.so`
4. The panel validates the exact core hash before reading native geometry. For a
   rebuilt core, obtain g_pl_view from its full symbol table and add the verified
   hash/offset pair to BOTH panel/geometry_bridge.h and panel/native/geometry_bridge.h.
   Never guess the offset or remove the hash check.
5. Build with `make -C panel AZ_SDK_INCLUDE=/path/to/az-mods-sdk/include`.
   Validate and package with that SDK's azmod-pack tool.

The firmware tag is ep147-9e9454e357389ece. Other firmware/core builds are not
validated. All three binaries have been reproduced byte-for-byte from the cleaned snapshot
with the tested local toolchain. See VERIFICATION.json. A fresh-USB installation
test and verification with a newly built Docker image remain outstanding.

For exact reproduction use build-all.sh from the repository root inside the
container, with AZ_SDK_INCLUDE set to an absolute SDK include path. Extract
resources first. The audio library embeds the source path in stb_vorbis assert
strings: the script preserves the original relative path through a symlink.
Changing that path changes the binary hash without changing the C source.
