CC = gcc
AZ_SDK_INCLUDE ?= ../sdk/include
OUT ?= out
all:
	mkdir -p $(OUT)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-misleading-indentation -fPIC -fvisibility=hidden -fno-omit-frame-pointer -fexceptions -fasynchronous-unwind-tables -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu -I$(AZ_SDK_INCLUDE) panel-test.c native-integrated.c -o $(OUT)/mrdjjag-touchfx-test.so -ldl -lpthread
