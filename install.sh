#!/bin/sh
# Install martida-bootd and retire the plymouth wiring.
#
# Plymouth the PACKAGE is left installed, but plymouth-start is masked -- two
# daemons must never fight over DRM master. So the fallback is not "boot with
# plymouth", it is "boot book4-mic24.conf", which has no `splash` on its command
# line; martida-bootd.service carries ConditionKernelCommandLine=splash, so that
# entry provably runs no splash daemon at all.
set -eu

cd "$(dirname "$0")"

# Everything user-specific is resolved once, here: the daemon runs as root before any session
# exists, so nothing it reads may have a user name compiled into it.
user=${SUDO_USER:-$(id -un)}
home=$(getent passwd "$user" | cut -d: -f6)
[ -n "$home" ] || { echo "cannot resolve the home directory of $user" >&2; exit 1; }

echo "== build"
make

echo "== assets"
# The ground is made from the user's wallpaper and palette, so it is rendered as the user.
# Failing that costs the picture, not the boot, so say so loudly and carry on.
sudo -H -u "$user" ./mkassets.sh || \
	echo "== assets FAILED: the splash stays a flat colour until mkassets.sh runs" >&2

echo "== binary + units"
install -Dm755 martida-bootd /usr/local/bin/martida-bootd
install -Dm755 martida-ground.so /usr/local/lib/hyprland/martida-ground.so
# The daemon reads /var/lib/martida-boot: these two links hold the user-specific half, so the
# binary and the units stay the same on every machine.
install -d -m755 /var/lib/martida-boot
ln -sfn "$home/.local/state/martida-boot/ground.bgra" /var/lib/martida-boot/ground.bgra
ln -sfn "$home/.local/state/quickshell/user/generated/martida-boot.conf" \
	/var/lib/martida-boot/theme.conf
# Hyprland's own first frames paint its wall0.png (misc:force_default_wallpaper=0), so the
# ground the splash holds stays up until the lock paints. /usr/share is searched before
# /usr/local/share, so the packaged file itself becomes the link; NoExtract keeps it across
# hyprland upgrades (the original is kept beside it).
rm -f /usr/local/share/hypr/wall0.png
[ -L /usr/share/hypr/wall0.png ] || mv /usr/share/hypr/wall0.png /usr/share/hypr/wall0.png.orig
ln -sfn "$home/.local/state/martida-boot/lock-ground-mark.png" /usr/share/hypr/wall0.png
grep -q '^NoExtract *= *usr/share/hypr/wall0.png' /etc/pacman.conf || \
	sed -i '/^\[options\]/a NoExtract   = usr/share/hypr/wall0.png' /etc/pacman.conf
rm -f /usr/local/bin/martida-msm-gate
install -Dm644 martida-msm-gate.conf /etc/modprobe.d/martida-msm-gate.conf
install -Dm644 martida-msm-load.service /etc/systemd/system/martida-msm-load.service
systemctl enable martida-msm-load.service >/dev/null 2>&1 || true
install -Dm644 martida-shutdown.service /etc/systemd/system/martida-shutdown.service
systemctl enable martida-shutdown.service >/dev/null 2>&1 || true
# plymouth's shutdown screens still carried the old theme: ours replaces them.
systemctl mask plymouth-poweroff.service plymouth-reboot.service plymouth-halt.service plymouth-kexec.service >/dev/null 2>&1 || true
install -Dm644 martida-bootd.service /etc/systemd/system/martida-bootd.service
install -Dm644 martida-bootd-release.service /etc/systemd/system/martida-bootd-release.service

mkdir -p /etc/systemd/system/sddm.service.d

# Pull the release in THROUGH sddm rather than reordering anything else --
# the same shape as the plymouth-deactivate wiring it replaces.
cat > /etc/systemd/system/sddm.service.d/martida-release.conf <<'EOF'
[Unit]
Wants=martida-bootd-release.service
After=martida-bootd-release.service
EOF

# Stock sddm.service is StartLimitBurst=2 / StartLimitIntervalSec=30 with
# Restart=always and RestartSec=100ms. If anything ever holds DRM master when
# the greeter starts, sddm burns both attempts in 200ms and lands in `failed`
# PERMANENTLY -- black screen, no display manager, and no getty either because
# sddm has Conflicts=getty@tty1.service. This turns that whole failure class
# from fatal into cosmetic, and is worth having regardless of this daemon.
cat > /etc/systemd/system/sddm.service.d/restart-tolerance.conf <<'EOF'
[Unit]
StartLimitIntervalSec=0

[Service]
RestartSec=2s
EOF

echo "== retire the plymouth wiring"
# the ExecStartPre gate that caused the 1.9s gap and the slower boot
rm -rf /etc/systemd/system/plymouth-start.service.d
rm -f  /usr/local/bin/book4-wait-for-drm
# the plymouth handoff units this daemon replaces
systemctl disable --now plymouth-handoff.service 2>/dev/null || true
rm -f /etc/systemd/system/plymouth-deactivate.service \
      /etc/systemd/system/plymouth-handoff.service \
      /etc/systemd/system/sddm.service.d/plymouth-deactivate.conf \
      /etc/systemd/system/graphical.target.wants/plymouth-handoff.service
# Two splash daemons must never contend for DRM master. quit and quit-wait go with them:
# sddm.service is ordered After=plymouth-quit-wait.service, and left active that unit hangs
# every boot entry -- they were masked by hand here and never by this script.
systemctl mask plymouth-start.service plymouth-read-write.service \
	plymouth-quit.service plymouth-quit-wait.service 2>/dev/null || true

systemctl daemon-reload
systemctl enable martida-bootd.service martida-bootd-release.service

echo
echo "== state"
systemctl is-enabled martida-bootd.service martida-bootd-release.service \
        plymouth-start.service plymouth-quit.service plymouth-quit-wait.service 2>&1 || true
echo
echo "== sddm restart tolerance:"
systemctl show sddm.service -p StartLimitBurst -p StartLimitIntervalUSec -p RestartUSec
echo
echo "== release is actually reachable from sddm:"
systemctl show sddm.service -p Wants | tr ' ' '\n' | grep martida || echo "MISSING - release would never fire"
echo
systemd-analyze verify martida-bootd.service martida-bootd-release.service \
        sddm.service sysinit.target basic.target graphical.target 2>&1 | head -20
echo "(verify silent above = no ordering cycles)"
