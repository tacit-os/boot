#!/bin/sh
# The boot ground for martida-bootd, Hyprland's first frames, swaybg and the boot lock.
#
#   ./mkassets.sh [--ground]      -> ~/.local/state/martida-boot/
#       lock-ground.png       wallpaper filled to the panel, blurred, dimmed (the lock's ground)
#       lock-ground-mark.png  the same with the Martida mark where the lock's clock sits
#       ground.bgra           lock-ground-mark.png as raw BGRA (8-byte u32 w,h LE header),
#                             so the daemon needs no image decoder on the boot path
#
# One picture from the splash to the lock: the daemon holds it, the compositor's own first
# frames paint it (/usr/share/hypr/wall0.png links to it), swaybg keeps it, and the boot lock
# starts from it and dissolves the mark into its clock. Re-run on wallpaper change
# (switchwall.sh does).
set -eu

# The mark ships beside this script: install.sh retires the plymouth wiring, so nothing here
# may depend on that package's theme directory still being there.
SRC=${SRC:-$(dirname "$0")}
STATE=${STATE:-$HOME/.local/state/martida-boot}
CONFIG=${CONFIG:-$HOME/.config/illogical-impulse/config.json}
# The panel's own mode, not a fixed 1920x1080: a ground of the wrong size is a cropped
# picture on the splash and a mark in the wrong place on the lock. The first line of a
# connector's mode list is its preferred mode.
mode=
for m in /sys/class/drm/*-eDP-*/modes /sys/class/drm/*-DSI-*/modes; do
        [ -r "$m" ] || continue
        mode=$(head -1 "$m")
        [ -z "$mode" ] || break
done
W=${mode%%x*}; H=${mode##*x}
case $W$H in ''|*[!0-9]*) W=1920; H=1080 ;; esac
# The mark sits where the boot lock's clock does: that column starts at 0.22 * height and the
# 96px clock's line box is ~112 tall, so its centre is 0.22 * H + 56 (294 at 1080p).
MARK_CY=$(( (H * 22 + 50) / 100 + 56 ))

header() { # w h -> 8 bytes LE
        printf '%b' "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x' \
                $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)) \
                $(($2 & 255)) $(($2 >> 8 & 255)) $(($2 >> 16 & 255)) $(($2 >> 24 & 255)))"
}

wall=$(python3 -c "import json;print(json.load(open('$CONFIG'))['background']['wallpaperPath'])")
[ -f "$wall" ] || { echo "no wallpaper at $wall" >&2; exit 1; }
[ -f "$SRC/logo.png" ] || { echo "no mark at $SRC/logo.png" >&2; exit 1; }
colors=$HOME/.local/state/quickshell/user/generated/colors.json
surface=$(python3 -c "import json;print(json.load(open('$colors'))['surface'])" 2>/dev/null || echo '#141314')
mkdir -p "$STATE"
# Each output is renamed into place, never written over the live one: a reader must see
# either the whole old file or the whole new one. The daemon mmaps ground.bgra, and reading
# past a file that shrank under it is SIGBUS.
#
# FastBlur radius 48 in the lock surface ~ gaussian sigma 16; dim 40% toward surface.
# [0]: a multi-frame wallpaper (a gif) otherwise writes one numbered PNG per frame, the mv
# below finds no file of the name it expects, and set -eu aborts before ground.bgra is written.
magick "$wall[0]" -resize ${W}x${H}^ -gravity center -extent ${W}x${H} \
        -blur 0x16 -fill "$surface" -colorize 40 \
        -alpha off "$STATE/.lock-ground.new.png"
mv -f "$STATE/.lock-ground.new.png" "$STATE/lock-ground.png"
lw=$(magick identify -format '%w' "$SRC/logo.png"); lh=$(magick identify -format '%h' "$SRC/logo.png")
magick "$STATE/lock-ground.png" "$SRC/logo.png" -gravity NorthWest \
        -geometry +$(( (W - lw) / 2 ))+$(( MARK_CY - lh / 2 )) -composite -alpha off "$STATE/.lock-ground-mark.new.png"
mv -f "$STATE/.lock-ground-mark.new.png" "$STATE/lock-ground-mark.png"
tmp=$STATE/.ground.bgra.new
header $W $H > "$tmp"
magick "$STATE/lock-ground-mark.png" -depth 8 -alpha on BGRA:- >> "$tmp"
want=$((W * H * 4 + 8)); got=$(wc -c < "$tmp")
[ "$got" -eq "$want" ] || { echo "ground: expected $want bytes, got $got" >&2; rm -f "$tmp"; exit 1; }
chmod 644 "$tmp"; mv -f "$tmp" "$STATE/ground.bgra"
echo "$STATE/{lock-ground,lock-ground-mark}.png + ground.bgra ${W}x${H} from $wall"
