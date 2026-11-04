/*
 * martida-bootd - boot splash for the Samsung Galaxy Book4 Edge (X1P42100).
 *
 * Replaces plymouth, which failed here in three measured ways:
 *   - its udev hotplug path silently dropped msm's card1 add event, so no
 *     splash was ever drawn;
 *   - it opens /dev/tty1, which ends fbcon's deferred takeover and destroys
 *     the Samsung firmware logo at 1.23s;
 *   - ~1s of daemon init sat between msm binding and the first pixel.
 *
 * This does none of that. No udev, no tty, no VT, no D-Bus, no theme
 * interpreter. It polls /dev/dri directly, and it opens no terminal, so the
 * firmware logo survives until msm resets the display block on its own.
 *
 * The one trick that matters is frecon's (ChromeOS): compose the whole frame
 * into the dumb buffer BEFORE drmModeSetCrtc, so the very first scanout is
 * already correct and there is no black frame at the handover.
 *
 * Handoff follows the same rule ChromeOS and Android both land on: nobody
 * blanks. On SIGUSR1 we drmDropMaster but keep the fd open and the buffer
 * alive, so the CRTC keeps scanning out our last frame while SDDM and
 * Hyprland come up. We only exit once the compositor's wayland socket exists
 * -- closing the last DRM fd earlier would trigger drm_lastclose() ->
 * drm_fbdev_client_restore(), which commits the blank fbcon framebuffer.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* ---- tunables ------------------------------------------------------------ */

/*
 * Fallbacks only. The real values come from matugen at runtime -- see
 * theme_load(). The desktop's background is colors.surface.dark, which changes
 * with every wallpaper, so hardcoding it here would leave the splash drifting
 * out of sync with the session the moment the theme changed.
 */
#define BG_R 0x14
#define BG_G 0x13
#define BG_B 0x15

/* soft lavender, for the progress line */
#define ACCENT_R 0xB8
#define ACCENT_G 0xA6
#define ACCENT_B 0xE8

/*
 * Written by matugen's [templates.martida_boot] on every theme change. Both runtime files
 * live under /var/lib: this runs as root before any session exists, so a path naming a user
 * is a path that only works on one machine. install.sh links them at the installing user's
 * state; --theme/--ground or MARTIDA_THEME/MARTIDA_GROUND override them.
 */
#define THEME_FILE "/var/lib/martida-boot/theme.conf"

static uint8_t bg_r = BG_R, bg_g = BG_G, bg_b = BG_B;
static uint8_t ac_r = ACCENT_R, ac_g = ACCENT_G, ac_b = ACCENT_B;

#define CARD_WAIT_MS     20000   /* give up looking for a real DRM card */
#define SESSION_WAIT_MS  30000   /* hard cap before we exit regardless */
#define AUTO_RELEASE_MS  10000   /* drop master even if SIGUSR1 never comes */
#define HANDOVER_GRACE_MS 2500   /* after the wayland socket appears */
#define FRAME_MS         33      /* ~30fps */
#define MSM_LOAD_MS      10000   /* modprobe msm, with the panel dark */
#define SHUTDOWN_HOLD_MS 30000   /* cap the shutdown picture: a stall must become visible */
/*
 * The lock screen's ground (wallpaper blurred and dimmed, notch pill baked in), rendered by
 * mkassets.sh --ground on every wallpaper change. With it the splash is the lock screen's
 * still: boot -> swaybg -> lock surface is one picture. Without it, the flat theme colour.
 */
#define GROUND_FILE "/var/lib/martida-boot/ground.bgra"

static const char *theme_file = THEME_FILE;
static const char *ground_file = GROUND_FILE;


/* ---- small helpers ------------------------------------------------------- */

static int verbose;

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * Failure paths are always logged, never gated on -v. This daemon's failure
 * mode is "black screen"; a silent exit(0) is the worst possible diagnostic.
 */
static void errmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * key=#RRGGBB, one per line. Anything unparseable leaves the fallback in place:
 * a malformed theme file must never cost us the splash.
 */
static void theme_load(void)
{
	char line[128];
	FILE *f = fopen(theme_file, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned r, g, b;

		if (sscanf(line, "background=#%2x%2x%2x", &r, &g, &b) == 3) {
			bg_r = r; bg_g = g; bg_b = b;
		} else if (sscanf(line, "accent=#%2x%2x%2x", &r, &g, &b) == 3) {
			ac_r = r; ac_g = g; ac_b = b;
		}
	}
	fclose(f);
	logmsg("theme: bg #%02x%02x%02x accent #%02x%02x%02x",
	       bg_r, bg_g, bg_b, ac_r, ac_g, ac_b);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ---- assets -------------------------------------------------------------- */

/*
 * Raw pre-converted BGRA, 8-byte header (u32 w, u32 h little-endian).
 * Produced by mkassets.sh so no PNG decoder is on the boot path.
 */
struct asset {
	uint32_t w, h;
	const uint8_t *px;      /* BGRA, w*h*4 */
	void *map;
	size_t maplen;
};

static int asset_load(struct asset *a, const char *path)
{
	struct stat st;
	int fd;
	uint32_t hdr[2];

	memset(a, 0, sizeof(*a));

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("asset %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
	    (size_t)st.st_size < sizeof(hdr)) {
		close(fd);
		return -1;
	}
	a->map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (a->map == MAP_FAILED) {
		a->map = NULL;
		return -1;
	}
	a->maplen = st.st_size;

	memcpy(hdr, a->map, sizeof(hdr));
	a->w = hdr[0];
	a->h = hdr[1];

	/*
	 * Bound the dimensions BEFORE multiplying: w and h come straight off
	 * disk, and w = h = 0x80000000 would make w*h*4 wrap to 0 and sail
	 * through the size check.
	 */
	if (!a->w || !a->h || a->w > 16384 || a->h > 16384 ||
	    (uint64_t)a->w * a->h * 4 + sizeof(hdr) > (uint64_t)st.st_size) {
		logmsg("asset %s: bad header %ux%u for %zu bytes",
		       path, a->w, a->h, (size_t)st.st_size);
		munmap(a->map, a->maplen);
		a->map = NULL;
		return -1;
	}
	a->px = (const uint8_t *)a->map + sizeof(hdr);
	logmsg("asset %s: %ux%u", path, a->w, a->h);
	return 0;
}

static volatile sig_atomic_t want_release;
static volatile sig_atomic_t want_quit;

/* ---- framebuffer --------------------------------------------------------- */

struct fb {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;
};

struct canvas {
	uint8_t *px;            /* XRGB8888 */
	uint32_t w, h, pitch;
};

static void canvas_fill(struct canvas *c, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
	uint32_t x, y;

	for (y = 0; y < c->h; y++) {
		uint32_t *row = (uint32_t *)(c->px + (uint64_t)y * c->pitch);
		for (x = 0; x < c->w; x++)
			row[x] = v;
	}
}

/* ---- the scene ----------------------------------------------------------- */

struct scene {
	struct asset ground;	/* the lock ground with the mark baked in (mkassets.sh --ground) */
};

/*
 * One static picture: the ground the compositor's first frames, swaybg and the boot lock all
 * show, mark included, so nothing moves from the splash until the lock dissolves the mark
 * into its clock. No animation here: whatever we drew would be cut at the handover anyway.
 *
 * A ground made for another panel is still the right picture: centre it, crop what does not
 * fit, fill the margin with the theme colour. Only a MISSING ground gives a flat screen.
 */
static void scene_draw(struct canvas *c, struct scene *s)
{
	uint32_t y, cw, ch, sx, sy, dx, dy;

	if (!s->ground.px) {
		canvas_fill(c, bg_r, bg_g, bg_b);
		return;
	}
	if (s->ground.w == c->w && s->ground.h == c->h) {
		/* opaque ground: straight row copies, no blend */
		for (y = 0; y < c->h; y++)
			memcpy(c->px + (uint64_t)y * c->pitch,
			       s->ground.px + (uint64_t)y * s->ground.w * 4,
			       (size_t)c->w * 4);
		return;
	}
	errmsg("martida-bootd: ground is %ux%u for a %ux%u frame, centred crop",
	       s->ground.w, s->ground.h, c->w, c->h);
	cw = s->ground.w < c->w ? s->ground.w : c->w;
	ch = s->ground.h < c->h ? s->ground.h : c->h;
	sx = (s->ground.w - cw) / 2;
	sy = (s->ground.h - ch) / 2;
	dx = (c->w - cw) / 2;
	dy = (c->h - ch) / 2;
	canvas_fill(c, bg_r, bg_g, bg_b);
	for (y = 0; y < ch; y++)
		memcpy(c->px + (uint64_t)(dy + y) * c->pitch + (uint64_t)dx * 4,
		       s->ground.px + ((uint64_t)(sy + y) * s->ground.w + sx) * 4,
		       (size_t)cw * 4);
}

/* ---- DRM ----------------------------------------------------------------- */

struct display {
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	drmModeModeInfo mode;
	struct fb fb;
	/* atomic path (see present()) */
	int atomic_ok;
	uint32_t plane_id;
	uint32_t mode_blob, ctm_blob;
	uint32_t p_conn_crtc, p_conn_link;
	uint32_t p_crtc_active, p_crtc_mode, p_crtc_vrr, p_crtc_ctm;
	uint32_t p_pl_fb, p_pl_crtc, p_pl_sx, p_pl_sy, p_pl_sw, p_pl_sh,
		 p_pl_cx, p_pl_cy, p_pl_cw, p_pl_ch;
};

/*
 * Why atomic, and why exactly these properties: the compositor's first commit is an
 * atomic modeset that sets connector CRTC_ID + link-status, CRTC ACTIVE + MODE_ID (+
 * VRR_ENABLED), and the primary plane. If the state it finds is bit-for-bit what it
 * asks for, the kernel treats that commit as a page flip and the panel never
 * power-cycles at the handover. The one that bit: msm_atomic_check() forces a full
 * modeset whenever a commit adds or removes the CRTC's CTM blob, and the compositor's
 * first commit always carries an identity CTM. So we set the same state, the same way,
 * identity CTM included.
 */
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name,
			uint64_t *cur)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0, i;
	if (!props)
		return 0;
	for (i = 0; i < props->count_props && !id; i++) {
		drmModePropertyPtr pr = drmModeGetProperty(fd, props->props[i]);
		if (!pr)
			continue;
		if (!strcmp(pr->name, name)) {
			id = pr->prop_id;
			if (cur)
				*cur = props->prop_values[i];
		}
		drmModeFreeProperty(pr);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static int atomic_setup(struct display *dp)
{
	drmModeResPtr res;
	drmModePlaneResPtr pres;
	int crtc_index = -1, i;
	uint32_t j;

	dp->atomic_ok = 0;
	dp->plane_id = 0;
	if (dp->mode_blob)
		drmModeDestroyPropertyBlob(dp->fd, dp->mode_blob);
	if (dp->ctm_blob)
		drmModeDestroyPropertyBlob(dp->fd, dp->ctm_blob);
	dp->mode_blob = dp->ctm_blob = 0;
	if (drmSetClientCap(dp->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(dp->fd, DRM_CLIENT_CAP_ATOMIC, 1))
		return -1;
	res = drmModeGetResources(dp->fd);
	if (!res)
		return -1;
	for (i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == dp->crtc_id)
			crtc_index = i;
	drmModeFreeResources(res);
	if (crtc_index < 0)
		return -1;

	pres = drmModeGetPlaneResources(dp->fd);
	if (!pres)
		return -1;
	for (j = 0; j < pres->count_planes && !dp->plane_id; j++) {
		drmModePlanePtr pl = drmModeGetPlane(dp->fd, pres->planes[j]);
		uint64_t type = 0;
		if (!pl)
			continue;
		if ((pl->possible_crtcs & (1u << crtc_index)) &&
		    prop_id(dp->fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) &&
		    type == DRM_PLANE_TYPE_PRIMARY)
			dp->plane_id = pl->plane_id;
		drmModeFreePlane(pl);
	}
	drmModeFreePlaneResources(pres);
	if (!dp->plane_id)
		return -1;

	dp->p_conn_crtc = prop_id(dp->fd, dp->conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	dp->p_conn_link = prop_id(dp->fd, dp->conn_id, DRM_MODE_OBJECT_CONNECTOR, "link-status", NULL);
	dp->p_crtc_active = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	dp->p_crtc_mode = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	dp->p_crtc_vrr = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", NULL);
	dp->p_crtc_ctm = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "CTM", NULL);
	dp->p_pl_fb = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
	dp->p_pl_crtc = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
	dp->p_pl_sx = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
	dp->p_pl_sy = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
	dp->p_pl_sw = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
	dp->p_pl_sh = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
	dp->p_pl_cx = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
	dp->p_pl_cy = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
	dp->p_pl_cw = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
	dp->p_pl_ch = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
	if (!dp->p_conn_crtc || !dp->p_crtc_active || !dp->p_crtc_mode || !dp->p_pl_fb ||
	    !dp->p_pl_crtc || !dp->p_pl_sx || !dp->p_pl_sy || !dp->p_pl_sw || !dp->p_pl_sh ||
	    !dp->p_pl_cx || !dp->p_pl_cy || !dp->p_pl_cw || !dp->p_pl_ch)
		return -1;
	if (drmModeCreatePropertyBlob(dp->fd, &dp->mode, sizeof(dp->mode), &dp->mode_blob))
		return -1;
	if (dp->p_crtc_ctm) {
		struct drm_color_ctm ctm = { .matrix = { 1ULL << 32, 0, 0,
							 0, 1ULL << 32, 0,
							 0, 0, 1ULL << 32 } };
		if (drmModeCreatePropertyBlob(dp->fd, &ctm, sizeof(ctm), &dp->ctm_blob))
			dp->ctm_blob = 0;
	}
	dp->atomic_ok = 1;
	return 0;
}

/* Put fb_id on the panel: an atomic modeset (first frame) or an atomic flip; legacy
 * SetCrtc if the atomic path is unavailable or refused. Returns 0 on success. */
static int present(struct display *dp, uint32_t fb_id, int modeset)
{
	if (dp->atomic_ok) {
		drmModeAtomicReqPtr req = drmModeAtomicAlloc();
		uint32_t flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
		int rc;
		if (!req)
			goto legacy;
		if (modeset) {
			drmModeAtomicAddProperty(req, dp->conn_id, dp->p_conn_crtc, dp->crtc_id);
			if (dp->p_conn_link)
				drmModeAtomicAddProperty(req, dp->conn_id, dp->p_conn_link,
							 DRM_MODE_LINK_STATUS_GOOD);
			drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_active, 1);
			drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_mode, dp->mode_blob);
			if (dp->p_crtc_vrr)
				drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_vrr, 0);
			if (dp->ctm_blob)
				drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_ctm, dp->ctm_blob);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_crtc, dp->crtc_id);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sx, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sy, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sw,
						 (uint64_t)dp->mode.hdisplay << 16);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sh,
						 (uint64_t)dp->mode.vdisplay << 16);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_cx, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_cy, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_cw, dp->mode.hdisplay);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_ch, dp->mode.vdisplay);
		}
		drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_fb, fb_id);
		rc = drmModeAtomicCommit(dp->fd, req, flags, NULL);
		drmModeAtomicFree(req);
		if (rc == 0)
			return 0;
		errmsg("martida-bootd: atomic %s failed (%s), legacy path",
		       modeset ? "modeset" : "flip", strerror(errno));
		dp->atomic_ok = 0;
	}
legacy:
	return drmModeSetCrtc(dp->fd, dp->crtc_id, fb_id, 0, 0, &dp->conn_id, 1, &dp->mode);
}

/*
 * Reject simpledrm: it is the firmware framebuffer holding the Samsung logo.
 *
 * CRITICAL: we must not open-then-close it. drm_release() -> drm_lastclose()
 * -> drm_client_dev_restore() -> drm_fbdev_client_restore() commits the BLANK
 * fbdev shadow buffer, which on simpledrm blits black over the firmware
 * framebuffer. Polling every 10ms would do that ~450 times before msm binds,
 * destroying the logo at ~1s -- earlier than plymouth did, and by the very
 * daemon that exists to prevent it.
 *
 * So: identify simpledrm from sysfs WITHOUT opening it, and if we ever do open
 * a card we reject for another reason, hold its fd open for our lifetime so
 * its open_count never reaches zero.
 */
static int held_fds[8];
static int held_n;

static void hold_fd(int fd)
{
	if (held_n < (int)(sizeof(held_fds) / sizeof(held_fds[0])))
		held_fds[held_n++] = fd;
	else
		close(fd);   /* should never happen; better than leaking */
}

/*
 * True if /sys/class/drm/<name> resolves through a simple-framebuffer platform
 * device. This is the same test plymouth uses (syspath_is_simpledrm), and it
 * needs no open(). Note the platform driver is called "simple-framebuffer",
 * not "simpledrm" -- comparing against the DRM driver name here would silently
 * never match.
 */
static int sysfs_is_simpledrm(const char *name)
{
	char link[PATH_MAX];
	char target[PATH_MAX];
	ssize_t n;

	snprintf(link, sizeof(link), "/sys/class/drm/%s", name);
	n = readlink(link, target, sizeof(target) - 1);
	if (n < 0)
		return 0;
	target[n] = '\0';
	return strstr(target, "simple-framebuffer") != NULL;
}

static int is_real_card(int fd)
{
	drmVersionPtr v = drmGetVersion(fd);
	int real;

	if (!v)
		return 0;
	real = strcmp(v->name, "simpledrm") != 0;
	logmsg("drm driver: %s (%s)", v->name, real ? "accept" : "skip");
	drmFreeVersion(v);
	return real;
}

static int open_real_card(uint64_t deadline)
{
	for (;;) {
		DIR *d = opendir("/dev/dri");
		struct dirent *e;

		if (d) {
			while ((e = readdir(d))) {
				char path[NAME_MAX + 16];
				int fd;

				if (strncmp(e->d_name, "card", 4) != 0)
					continue;
				/* never even open the firmware framebuffer */
				if (sysfs_is_simpledrm(e->d_name))
					continue;

				snprintf(path, sizeof(path), "/dev/dri/%s",
					 e->d_name);
				fd = open(path, O_RDWR | O_CLOEXEC);
				if (fd < 0)
					continue;
				if (is_real_card(fd)) {
					closedir(d);
					logmsg("using %s", path);
					return fd;
				}
				/* rejected: keep it open, do not trip lastclose */
				hold_fd(fd);
			}
			closedir(d);
		}
		if (want_quit)
			return -1;
		if (now_ms() >= deadline)
			return -1;
		sleep_ms(10);
	}
}

static int connector_is_internal(drmModeConnectorPtr conn)
{
	return conn->connector_type == DRM_MODE_CONNECTOR_eDP ||
	       conn->connector_type == DRM_MODE_CONNECTOR_DSI ||
	       conn->connector_type == DRM_MODE_CONNECTOR_LVDS;
}

/* Returns 0 and fills dp->conn_id / mode / crtc_id if this connector is usable. */
static int try_connector(struct display *dp, drmModeResPtr res,
			 drmModeConnectorPtr conn)
{
	int j, ok = -1;

	if (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes)
		return -1;

	/* preferred mode, else the first (they are listed best-first) */
	dp->mode = conn->modes[0];
	for (j = 0; j < conn->count_modes; j++) {
		if (conn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
			dp->mode = conn->modes[j];
			break;
		}
	}
	dp->conn_id = conn->connector_id;

	for (j = 0; j < conn->count_encoders && ok < 0; j++) {
		drmModeEncoderPtr enc =
			drmModeGetEncoder(dp->fd, conn->encoders[j]);
		int k;

		if (!enc)
			continue;
		for (k = 0; k < res->count_crtcs; k++) {
			if (!(enc->possible_crtcs & (1u << k)))
				continue;
			dp->crtc_id = res->crtcs[k];
			ok = 0;
			break;
		}
		drmModeFreeEncoder(enc);
	}
	return ok;
}

static int pick_output(struct display *dp)
{
	drmModeResPtr res = drmModeGetResources(dp->fd);
	int i, ok = -1;

	if (!res)
		return -1;

	/*
	 * Connector selection is on the critical path to first light, and
	 * drmModeGetConnector() FORCES a detect -- on eDP that is an EDID read
	 * over AUX, on a disconnected DP or HDMI port it is a detect that can
	 * block for tens of ms each. res->connectors[] here is ordered DP-1,
	 * DP-2, eDP-1, HDMI-A-1, Writeback-1, so probing in array order pays
	 * for two dead DisplayPorts before ever reaching the laptop panel.
	 *
	 * So: read the CACHED state first (no probe), and when a probe is
	 * unavoidable, decide whether we even want this connector -- from its
	 * cached type -- BEFORE paying for it.
	 *
	 *   pass 0  cached, internal only          (free)
	 *   pass 1  probe, but only internal       (one EDID read)
	 *   pass 2  cached, anything connected     (free)
	 *   pass 3  probe anything                 (last resort)
	 */
	for (i = 0; i < res->count_connectors * 4 && ok < 0; i++) {
		int pass = i / res->count_connectors;
		int idx = i % res->count_connectors;
		drmModeConnectorPtr cached, conn;

		cached = drmModeGetConnectorCurrent(dp->fd, res->connectors[idx]);
		if (!cached)
			continue;

		if (pass < 2 && !connector_is_internal(cached)) {
			drmModeFreeConnector(cached);
			continue;
		}

		if (pass == 0 || pass == 2) {
			ok = try_connector(dp, res, cached);
			drmModeFreeConnector(cached);
			continue;
		}

		/* worth a real probe */
		drmModeFreeConnector(cached);
		conn = drmModeGetConnector(dp->fd, res->connectors[idx]);
		if (!conn)
			continue;
		ok = try_connector(dp, res, conn);
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);
	if (ok == 0)
		logmsg("connector %u crtc %u mode %ux%u@%u",
		       dp->conn_id, dp->crtc_id,
		       dp->mode.hdisplay, dp->mode.vdisplay, dp->mode.vrefresh);
	return ok;
}

static int fb_create(int fd, struct fb *f, uint32_t w, uint32_t h)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;

	memset(&creq, 0, sizeof(creq));
	creq.width = w;
	creq.height = h;
	creq.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;

	f->handle = creq.handle;
	f->pitch = creq.pitch;
	f->size = creq.size;

	if (drmModeAddFB(fd, w, h, 24, 32, f->pitch, f->handle, &f->fb_id) < 0)
		return -1;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = f->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		return -1;

	f->map = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fd, mreq.offset);
	if (f->map == MAP_FAILED) {
		f->map = NULL;
		return -1;
	}
	return 0;
}

static void fb_destroy(int fd, struct fb *f)
{
	struct drm_mode_destroy_dumb dreq;

	if (f->map)
		munmap(f->map, f->size);
	if (f->fb_id)
		drmModeRmFB(fd, f->fb_id);
	if (f->handle) {
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = f->handle;
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	memset(f, 0, sizeof(*f));
}

/* ---- mode cache ---------------------------------------------------------- */

/*
 * The 171ms in `pick` is one drmModeGetConnector() on eDP-1: the first EDID
 * read over the DP AUX channel plus mode-list construction. It is not the dead
 * DisplayPorts (skipping those changed nothing), and it is not something a
 * better search order can avoid -- it is the cost of asking the panel who it
 * is, and the panel is the same panel on every boot.
 *
 * So ask once. After a successful modeset we write the connector id, CRTC id
 * and the exact modeline to disk; on later boots we go straight to that and
 * never probe. If anything about it no longer holds -- different hardware, a
 * stale id, a modeset that fails -- we fall back to the full probing path, so
 * the cache can only ever make us faster, never wrong.
 */
#define MODE_CACHE "/var/lib/martida-boot/mode.bin"
#define MODE_CACHE_MAGIC 0x4d425431u   /* "MBT1" */

struct mode_cache {
	uint32_t magic;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t conn_type;
	drmModeModeInfo mode;
};

static int mode_cache_load(struct display *dp)
{
	struct mode_cache mc;
	int fd, ok = -1;

	fd = open(MODE_CACHE, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (read(fd, &mc, sizeof(mc)) == (ssize_t)sizeof(mc) &&
	    mc.magic == MODE_CACHE_MAGIC &&
	    mc.mode.hdisplay >= 1 && mc.mode.hdisplay <= 16384 &&
	    mc.mode.vdisplay >= 1 && mc.mode.vdisplay <= 16384) {
		/*
		 * No hardware check here, deliberately: the modeset is the validator.
		 *
		 * We open the card the instant its node appears, earlier than anything
		 * else on the system, so drmModeGetConnectorCurrent() can still return
		 * NULL while the driver finishes registering connectors -- and treating
		 * that as "cache invalid" made every boot pay the full probe anyway.
		 * MEASURED: with that call still being made the phase cost 191ms against
		 * 189ms for the EDID probe it was supposed to replace. So the ~190ms is
		 * not the probe at all, it is the first DRM ioctl on a device that has
		 * only just registered, and any call pays it.
		 *
		 * A cache that no longer holds is therefore caught where it shows:
		 * present() refuses the mode, and the caller unlinks the cache, reprobes,
		 * rebuilds the framebuffer at the probed size and presents again.
		 */
		dp->conn_id = mc.conn_id;
		dp->crtc_id = mc.crtc_id;
		dp->mode = mc.mode;
		ok = 0;
	}
	close(fd);
	return ok;
}

static void mode_cache_save(struct display *dp)
{
	struct mode_cache mc;
	drmModeConnectorPtr conn;
	char tmp[] = MODE_CACHE ".XXXXXX";
	int fd;

	memset(&mc, 0, sizeof(mc));
	mc.magic = MODE_CACHE_MAGIC;
	mc.conn_id = dp->conn_id;
	mc.crtc_id = dp->crtc_id;
	mc.mode = dp->mode;

	/* recorded for inspection only -- nothing reads it back, so a connector that is
	 * not readable yet must not cost us the cache */
	conn = drmModeGetConnectorCurrent(dp->fd, dp->conn_id);
	if (conn) {
		mc.conn_type = conn->connector_type;
		drmModeFreeConnector(conn);
	}

	if (mkdir("/var/lib/martida-boot", 0755) < 0 && errno != EEXIST)
		return;
	fd = mkstemp(tmp);
	if (fd < 0)
		return;
	if (write(fd, &mc, sizeof(mc)) == (ssize_t)sizeof(mc)) {
		fchmod(fd, 0644);
		close(fd);
		if (rename(tmp, MODE_CACHE) == 0)
			return;
	} else {
		close(fd);
	}
	unlink(tmp);
}

/* ---- session detection --------------------------------------------------- */

static int wayland_socket_exists(void)
{
	DIR *ud = opendir("/run/user");
	struct dirent *ue;
	int found = 0;

	if (!ud)
		return 0;
	while (!found && (ue = readdir(ud))) {
		char dir[NAME_MAX + 16];
		DIR *sd;
		struct dirent *se;

		if (ue->d_name[0] < '0' || ue->d_name[0] > '9')
			continue;
		snprintf(dir, sizeof(dir), "/run/user/%s", ue->d_name);
		sd = opendir(dir);
		if (!sd)
			continue;
		while ((se = readdir(sd))) {
			size_t n = strlen(se->d_name);
			if (strncmp(se->d_name, "wayland-", 8) != 0)
				continue;
			if (n > 5 && strcmp(se->d_name + n - 5, ".lock") == 0)
				continue;
			found = 1;
			break;
		}
		closedir(sd);
	}
	closedir(ud);
	return found;
}

/* ---- backlight + driver load -------------------------------------------- */
/*
 * The msm driver resets the panel pipeline when it loads, and the upstream driver does not
 * inherit the firmware's configuration (drm/msm/dpu: "Issue MDSS reset during
 * initialization"). Lit, that reset shows as a torn bright line after the Samsung logo. So
 * this daemon owns the handover: msm is blacklisted from autoload; we switch the backlight
 * off, load msm ourselves, compose and modeset, and switch the backlight on once our first
 * frame is scanning out. Recovery entries without `splash` load msm from
 * martida-msm-load.service instead; martida-bootd-release.service restores the backlight if
 * we died in between.
 */
/*
 * The device is the driver's name, not ours ("backlight" here, "acpi_video0" or
 * "amdgpu_bl0" elsewhere), so glob the class directory rather than compile in a name
 * that exists on one machine.
 */
static char backlight_node[PATH_MAX];
static int saved_brightness = -1;

static const char *backlight_path(void)
{
	DIR *d;
	struct dirent *e;

	if (backlight_node[0])
		return backlight_node;
	d = opendir("/sys/class/backlight");
	if (!d)
		return NULL;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(backlight_node, sizeof(backlight_node),
			 "/sys/class/backlight/%s/brightness", e->d_name);
		break;
	}
	closedir(d);
	return backlight_node[0] ? backlight_node : NULL;
}

static int read_int(const char *path)
{
	char buf[32];
	FILE *f = fopen(path, "r");
	int v = -1;
	if (!f)
		return -1;
	if (fgets(buf, sizeof(buf), f))
		v = atoi(buf);
	fclose(f);
	return v;
}

static int write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");
	int ok;

	if (!f)
		return -1;
	ok = fprintf(f, "%d", v) > 0;
	if (fflush(f) || ferror(f))
		ok = 0;
	if (fclose(f))
		ok = 0;
	return ok ? 0 : -1;
}

static void backlight_off(void)
{
	uint64_t deadline = now_ms() + 1500;
	const char *node;
	int v = -1;

	/* the backlight device usually exists by now; give it a moment if not */
	for (;;) {
		node = backlight_path();
		if (node && (v = read_int(node)) >= 0)
			break;
		if (now_ms() >= deadline)
			break;
		sleep_ms(10);
	}
	if (v <= 0)
		return;
	if (write_int(node, 0) < 0) {
		errmsg("martida-bootd: cannot blank %s (%s), the panel reset will show",
		       node, strerror(errno));
		return;
	}
	saved_brightness = v;
	errmsg("martida-bootd: backlight off (was %d)", v);
}

/*
 * We are not the only writer: systemd-backlight@.service is pulled in by udev, is ordered
 * against nothing of ours, and writes the saved brightness back unconditionally -- measured
 * ~330ms after we blank, i.e. in the middle of msm's panel reset, which is the one artefact
 * this daemon exists to hide. So re-blank whenever we find the panel lit while we are
 * supposed to be holding it dark.
 */
static void backlight_reassert(void)
{
	const char *node = backlight_path();

	if (saved_brightness <= 0 || !node)
		return;
	if (read_int(node) > 0)
		write_int(node, 0);
}

static void backlight_restore(void)
{
	const char *node = backlight_path();
	int i, rc = -1;

	if (saved_brightness <= 0 || !node)
		return;
	/* the splash has no fade of its own any more: the panel ramps up instead */
	for (i = 1; i <= 8; i++) {
		rc = write_int(node, saved_brightness * i / 8);
		if (i < 8)
			sleep_ms(25);
	}
	if (rc < 0) {
		/*
		 * A dark panel must never read as success. Keep saved_brightness so a
		 * later attempt still has a value to write.
		 */
		errmsg("martida-bootd: BACKLIGHT RESTORE FAILED (%s): %s",
		       node, strerror(errno));
		return;
	}
	errmsg("martida-bootd: backlight restored to %d", saved_brightness);
	saved_brightness = -1;
}

/*
 * Bounded, and it holds the blank while it runs. modprobe can block indefinitely (firmware
 * that never arrives, a probe that deadlocks) and this runs with the backlight at 0, so an
 * unbounded wait is an indefinitely dark panel with no console on it. Every other wait in
 * this daemon has a deadline; this one had none.
 */
static int load_msm(void)
{
	uint64_t deadline = now_ms() + MSM_LOAD_MS;
	int status = 0;
	pid_t pid = fork();

	if (pid < 0) {
		errmsg("martida-bootd: fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execl("/sbin/modprobe", "modprobe", "msm", (char *)NULL);
		_exit(127);
	}
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);

		if (r == pid)
			break;
		if (r < 0) {
			errmsg("martida-bootd: waitpid: %s", strerror(errno));
			return -1;
		}
		if (now_ms() >= deadline) {
			errmsg("martida-bootd: modprobe msm still running after %dms, "
			       "giving up", MSM_LOAD_MS);
			return -1;
		}
		backlight_reassert();
		sleep_ms(20);
	}
	errmsg("martida-bootd: modprobe msm -> %d", status);
	return status;
}


/* ---- main ---------------------------------------------------------------- */

static void on_usr1(int sig) { (void)sig; want_release = 1; }
static void on_term(int sig) { (void)sig; want_quit = 1; }

static int dry_run(struct scene *sc, const char *out)
{
	struct canvas c;
	FILE *f;
	uint32_t x, y;

	c.w = 1920;
	c.h = 1080;
	c.pitch = c.w * 4;
	c.px = calloc(1, (size_t)c.pitch * c.h);
	if (!c.px)
		return 1;

	scene_draw(&c, sc);

	f = fopen(out, "wb");
	if (!f) {
		free(c.px);
		return 1;
	}
	fprintf(f, "P6\n%u %u\n255\n", c.w, c.h);
	for (y = 0; y < c.h; y++) {
		for (x = 0; x < c.w; x++) {
			uint32_t v = *(uint32_t *)(c.px + (uint64_t)y * c.pitch + x * 4);
			uint8_t rgb[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	free(c.px);
	fprintf(stderr, "wrote %s\n", out);
	return 0;
}

int main(int argc, char **argv)
{
	struct display dp;
	struct scene sc;
	struct canvas c;
	struct canvas pre;
	struct sigaction sa;
	uint64_t t0, t_card, t_phase, deadline;
	uint64_t t_release = 0;
	uint64_t ms_pick = 0, ms_fb = 0, ms_draw = 0;
	int cached_mode = 0;
	const char *ppm = NULL;
	int check_cache = 0;
	int shutdown_mode = 0;	/* --shutdown: the same scene, held until the kernel goes */
	const char *env;
	int i, released = 0;

	memset(&dp, 0, sizeof(dp));
	memset(&sc, 0, sizeof(sc));
	memset(&pre, 0, sizeof(pre));

	if ((env = getenv("MARTIDA_GROUND")))
		ground_file = env;
	if ((env = getenv("MARTIDA_THEME")))
		theme_file = env;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--check-cache"))
			check_cache = 1;
		else if (!strcmp(argv[i], "--shutdown"))
			shutdown_mode = 1;
		else if (!strcmp(argv[i], "--ground") && i + 1 < argc)
			ground_file = argv[++i];
		else if (!strcmp(argv[i], "--theme") && i + 1 < argc)
			theme_file = argv[++i];
		else if (!strncmp(argv[i], "--dry-run", 9))
			ppm = (i + 1 < argc && argv[i + 1][0] != '-') ?
				argv[++i] : "/tmp/martida-boot.ppm";
	}

	theme_load();
	asset_load(&sc.ground, ground_file);
	if (!sc.ground.px)
		errmsg("martida-bootd: no ground at %s, flat colour", ground_file);

	if (ppm)
		return dry_run(&sc, ppm);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/*
	 * Pre-render into heap while we wait for msm. The panel is dark from
	 * the moment msm resets the DPU until our modeset lands, so every
	 * millisecond between those two points is visible black. Composing
	 * 1920x1080 in software costs ~31ms measured; doing it now instead of
	 * after the card appears takes that off the critical path.
	 */
	pre.w = 1920;
	pre.h = 1080;
	pre.pitch = pre.w * 4;
	pre.px = malloc((size_t)pre.pitch * pre.h);
	if (pre.px) {
		scene_draw(&pre, &sc);
	}

	if (!check_cache && !shutdown_mode) {	/* the panel is already up at shutdown; --check-cache runs on a live desktop */
		backlight_off();
		if (load_msm() != 0) {
			backlight_restore();
			return 0;
		}
	}
	dp.fd = open_real_card(now_ms() + CARD_WAIT_MS);
	t_card = now_ms();

	if (check_cache) {
		/* Verify the cache resolves, WITHOUT taking master or
		 * touching the display. Safe to run on a live desktop. */
		uint64_t t = now_ms();
		int hit = dp.fd >= 0 && mode_cache_load(&dp) == 0;
		errmsg("cache %s in %lums: conn %u crtc %u mode %ux%u@%u",
		       hit ? "HIT" : "MISS", (unsigned long)(now_ms() - t),
		       dp.conn_id, dp.crtc_id, dp.mode.hdisplay,
		       dp.mode.vdisplay, dp.mode.vrefresh);
		return hit ? 0 : 1;
	}
	if (dp.fd < 0) {
		errmsg("martida-bootd: no real DRM card appeared in %dms",
		       CARD_WAIT_MS);
		backlight_restore();
		return 0;
	}
	t_phase = now_ms();
	cached_mode = mode_cache_load(&dp) == 0;
	if (!cached_mode && pick_output(&dp) < 0) {
		errmsg("martida-bootd: no connected output");
		backlight_restore();
		return 0;
	}
	ms_pick = now_ms() - t_phase;
	/*
	 * At ~4.5s nothing legitimately holds master yet, but retry briefly
	 * rather than allocating two 8MB buffers only to fail the modeset.
	 */
	for (i = 0; i < (shutdown_mode ? 300 : 20); i++) {
		if (drmSetMaster(dp.fd) == 0)
			break;
		sleep_ms(10);
	}
	if (i == (shutdown_mode ? 300 : 20)) {
		errmsg("martida-bootd: drmSetMaster: %s", strerror(errno));
		backlight_restore();
		return 0;
	}

	/* The one buffer: composed below, then put on the panel by present(). */
	t_phase = now_ms();
	if (fb_create(dp.fd, &dp.fb, dp.mode.hdisplay,
		      dp.mode.vdisplay) < 0) {
		errmsg("martida-bootd: fb_create: %s", strerror(errno));
		if (cached_mode)
			unlink(MODE_CACHE);
		backlight_restore();
		return 0;
	}
	ms_fb = now_ms() - t_phase;

	c.w = dp.mode.hdisplay;
	c.h = dp.mode.vdisplay;
	/*
	 * Compose BEFORE the modeset. This is the whole point: the first
	 * scanout is already the finished frame, so the panel never shows a
	 * blank buffer at the handover.
	 */
	t0 = now_ms();
	c.px = dp.fb.map;
	c.pitch = dp.fb.pitch;
	t_phase = now_ms();
	if (pre.px && c.w == pre.w && c.h == pre.h) {
		uint32_t y;
		for (y = 0; y < c.h; y++)
			memcpy(c.px + (uint64_t)y * c.pitch,
			       pre.px + (uint64_t)y * pre.pitch,
			       (size_t)c.w * 4);
	} else {
		scene_draw(&c, &sc);
	}
	ms_draw = now_ms() - t_phase;
	t_phase = now_ms();

	/* somebody else may have re-lit the panel while msm loaded (see
	 * backlight_reassert): the blank must span the whole reset */
	backlight_reassert();
	if (atomic_setup(&dp) < 0)
		errmsg("martida-bootd: atomic path unavailable, legacy SetCrtc");
	if (present(&dp, dp.fb.fb_id, 1) < 0) {
		if (!cached_mode) {
			errmsg("martida-bootd: modeset: %s",
			       strerror(errno));
			backlight_restore();
		return 0;
		}
		/* stale cache: fall back to the full probe and try once more */
		errmsg("martida-bootd: cached mode rejected (%s), probing",
		       strerror(errno));
		unlink(MODE_CACHE);
		cached_mode = 0;
		if (pick_output(&dp) < 0) {
			errmsg("martida-bootd: no output: %s", strerror(errno));
			backlight_restore();
			return 0;
		}
		/*
		 * The buffer is still the cached mode's size, and a plane whose
		 * source rectangle exceeds its framebuffer is refused exactly as
		 * flatly as the bad mode was -- so rebuild it at the probed size
		 * and recompose before trying again. Skipping this is why a
		 * rejected cache meant no splash at all.
		 */
		fb_destroy(dp.fd, &dp.fb);
		if (fb_create(dp.fd, &dp.fb, dp.mode.hdisplay,
			      dp.mode.vdisplay) < 0) {
			errmsg("martida-bootd: fb_create after probe: %s",
			       strerror(errno));
			backlight_restore();
			return 0;
		}
		c.w = dp.mode.hdisplay;
		c.h = dp.mode.vdisplay;
		c.px = dp.fb.map;
		c.pitch = dp.fb.pitch;
		scene_draw(&c, &sc);
		atomic_setup(&dp);
		if (present(&dp, dp.fb.fb_id, 1) < 0) {
			errmsg("martida-bootd: modeset: %s", strerror(errno));
			backlight_restore();
			return 0;
		}
	}
	if (!cached_mode)
		mode_cache_save(&dp);
	/* one vblank so the first scanout has happened before the panel lights */
	sleep_ms(20);
	backlight_restore();
	{
		uint64_t ls = 99, act = 99;
		prop_id(dp.fd, dp.conn_id, DRM_MODE_OBJECT_CONNECTOR, "link-status", &ls);
		prop_id(dp.fd, dp.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &act);
		uint64_t ctm = 0;
		prop_id(dp.fd, dp.crtc_id, DRM_MODE_OBJECT_CRTC, "CTM", &ctm);
		errmsg("martida-bootd: %s path, plane %u, link-status %lu, ACTIVE %lu, CTM %lu",
		       dp.atomic_ok ? "atomic" : "legacy", dp.plane_id,
		       (unsigned long)ls, (unsigned long)act, (unsigned long)ctm);
	}
	errmsg("martida-bootd: splash up %lums after card appeared "
	       "(%s %lu, fb %lu, draw %lu, modeset %lu)",
	       (unsigned long)(now_ms() - t_card),
	       cached_mode ? "cache" : "probe", (unsigned long)ms_pick, (unsigned long)ms_fb,
	       (unsigned long)ms_draw, (unsigned long)(now_ms() - t_phase));

	deadline = now_ms() + (shutdown_mode ? SHUTDOWN_HOLD_MS : SESSION_WAIT_MS);

	for (;;) {
		uint64_t t = now_ms();

		/*
		 * A quit between the drop and the compositor's first commit must not
		 * exit: drm_file_free() -> drm_fb_release() removes the FB that is
		 * still on the primary plane, which blanks it. Hold the frame for the
		 * same bounded grace instead; TimeoutStopSec caps the wait.
		 */
		if (want_quit && (!released ||
				  now_ms() - t_release >= HANDOVER_GRACE_MS))
			break;

		/*
		 * Defence in depth: if the release unit never fires, drop
		 * master anyway. Holding it means the compositor cannot take
		 * the panel at all -- a black screen caused by the splash.
		 */
		if (shutdown_mode)
			want_release = 0;
		if (!shutdown_mode && !released && t - t0 >= AUTO_RELEASE_MS && !want_release) {
			errmsg("martida-bootd: no release signal after %dms, "
			       "dropping master anyway", AUTO_RELEASE_MS);
			want_release = 1;
		}

		if (want_release && !released) {
			drmDropMaster(dp.fd);
			released = 1;
			/* re-arm: the hold must be measured from the handover,
			 * not from when the splash first went up */
			deadline = now_ms() + SESSION_WAIT_MS;
			t_release = now_ms();
			t = t_release;	/* the fade above took time; do not test a stale t below */
			errmsg("martida-bootd: master dropped at +%lums, holding frame",
			       (unsigned long)(t - t0));
		}

		if (released) {
			/*
			 * Do NOT close the fd. While it stays open the CRTC
			 * keeps scanning out our buffer; closing it would let
			 * drm_lastclose() restore the blank fbcon framebuffer.
			 */
			/*
			 * The socket appears when the compositor BINDS it,
			 * which is before its first DRM commit. We must not
			 * exit before that commit: drm_file_free() calls
			 * drm_fb_release() unconditionally, and removing a FB
			 * that is still on the primary plane blanks it --
			 * keeping open_count above zero does not save us.
			 */
			if (wayland_socket_exists()) {
				sleep_ms(HANDOVER_GRACE_MS);
				break;
			}
			if (t >= deadline)
				break;
			sleep_ms(30);
			continue;
		}

		if (t >= deadline) {
			if (shutdown_mode) {
				/* the stop job is stuck: give the display back so its
				 * own messages and the countdown become visible */
				errmsg("martida-bootd: shutdown still running after "
				       "%dms, releasing the display", SHUTDOWN_HOLD_MS);
				drmDropMaster(dp.fd);
			}
			break;
		}
		sleep_ms(FRAME_MS);
	}

	backlight_restore();
	errmsg("martida-bootd: exiting at +%lums",
	       (unsigned long)(now_ms() - t0));
	return 0;
}
