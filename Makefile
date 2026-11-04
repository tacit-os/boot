CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += $(shell pkg-config --cflags libdrm)
LDLIBS  += $(shell pkg-config --libs libdrm)

PREFIX  ?= /usr/local
UNITDIR ?= /etc/systemd/system
HYPRVER := $(shell pkg-config --modversion hyprland 2>/dev/null)

all: martida-bootd martida-ground.so

martida-bootd: martida-bootd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Rebuild after every hyprland upgrade: the API version and the client hash are baked in at
# compile time and the compositor refuses a stale plugin (logged, not fatal; the first frame
# is just flat again). The source does not change when Hyprland does, so the installed
# version is the prerequisite that has to be watched -- without it `make` had nothing to
# rebuild from and install.sh shipped the stale plugin.
martida-ground.so: martida-ground.cpp .hyprver
	g++ -shared -fPIC -std=c++26 -O2 $(shell pkg-config --cflags hyprland pixman-1 libdrm) -o $@ $<

.hyprver: FORCE
	@[ "$$(cat $@ 2>/dev/null)" = "$(HYPRVER)" ] || echo "$(HYPRVER)" > $@

# Deliberately NOT a real install target. Installing the units without the
# sddm drop-ins leaves a daemon that takes DRM master and is never told to let
# go -- which is the one failure that can leave this machine with no display
# manager. install.sh is the only supported path.
install:
	@echo "use ./install.sh (needs root); it also writes the sddm drop-ins" >&2
	@false

clean:
	rm -f martida-bootd martida-ground.so .hyprver

.PHONY: all install clean FORCE
FORCE:
