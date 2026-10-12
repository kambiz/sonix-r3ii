// r3ii_probe: a one-shot hardware survey for the pre-2025 HiBy R3 II.
//
// It runs from the stock hiby_player.sh before the stock player starts, so
// the device stays usable whatever this program finds. For about a minute it:
//
//   - writes what the kernel exposes (framebuffer, input devices, sound
//     cards and mixer controls, partitions, memory) to sysinfo.txt;
//   - paints colour bands on the framebuffer and a bar along the bottom
//     that shrinks as the time runs out;
//   - logs every input event to events.txt, draws a white square wherever
//     the screen is touched and flashes the border on every key press;
//   - plays a quiet 1 kHz tone through aplay and logs the result.
//
// Everything goes to /mnt/sd_0/r3ii_probe/, or to /data/r3ii_probe/ when no
// card is mounted. It is built static against musl, so it needs nothing from
// the device's glibc 2.16.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RUN_SECONDS 60
#define HARD_LIMIT_SECONDS 90
#define SD_WAIT_SECONDS 15
#define MAX_INPUTS 16
#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static char out_dir[128];
static FILE *log_file;
static struct timespec t0;

static double elapsed(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

static void logf_(const char *fmt, ...)
{
	if (!log_file)
		return;
	va_list ap;
	fprintf(log_file, "[%7.3f] ", elapsed());
	va_start(ap, fmt);
	vfprintf(log_file, fmt, ap);
	va_end(ap);
	fputc('\n', log_file);
	fflush(log_file);
}

// ---------------------------------------------------------------------------
// output directory
// ---------------------------------------------------------------------------

static int sd_mounted(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512];
	int found = 0;
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (strstr(line, " /mnt/sd_0 ")) {
			found = 1;
			break;
		}
	fclose(f);
	return found;
}

static void pick_out_dir(void)
{
	for (int i = 0; i < SD_WAIT_SECONDS * 2 && !sd_mounted(); i++)
		usleep(500 * 1000);
	if (sd_mounted())
		snprintf(out_dir, sizeof out_dir, "/mnt/sd_0/r3ii_probe");
	else
		snprintf(out_dir, sizeof out_dir, "/data/r3ii_probe");
	mkdir(out_dir, 0755);
}

static FILE *open_out(const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s/%s", out_dir, name);
	return fopen(path, "w");
}

// ---------------------------------------------------------------------------
// system survey
// ---------------------------------------------------------------------------

// Each command's output goes into sysinfo.txt under a header. The commands
// are busybox applets and alsa-utils from the stock image.
static const char *const survey[] = {
	"uname -a",
	"cat /proc/cmdline",
	"cat /proc/cpuinfo",
	"cat /proc/meminfo",
	"free",
	"cat /proc/mtd",
	"cat /proc/partitions",
	"cat /proc/mounts",
	"df",
	"ls -l /dev /dev/input /dev/snd",
	"cat /proc/bus/input/devices",
	"for f in /sys/class/graphics/fb0/*; do [ -f $f ] && echo \"$f: $(cat $f 2>&1)\"; done",
	"cat /proc/asound/cards /proc/asound/devices /proc/asound/pcm",
	"aplay -l",
	"aplay -L",
	"cat /etc/asound.conf",
	"for c in /proc/asound/card*; do n=${c##*card}; echo \"== card $n\"; amixer -c $n contents; done",
	"lsmod",
	"ls /proc/jz /proc/jz/*",
	"cat /proc/jz/efuse/efuse_chip_id",
	"for d in /sys/class/power_supply/*; do echo \"== $d\"; cat $d/uevent; done",
	"ls -l /sys/class/backlight/*/ && cat /sys/class/backlight/*/brightness /sys/class/backlight/*/max_brightness",
	"ls /sys/class/leds",
	"ps",
	"dmesg",
	NULL,
};

static void run_survey(void)
{
	FILE *out = open_out("sysinfo.txt");
	if (!out) {
		logf_("cannot write sysinfo.txt: %s", strerror(errno));
		return;
	}
	for (int i = 0; survey[i]; i++) {
		char cmd[1024], buf[4096];
		fprintf(out, "\n######## %s\n", survey[i]);
		fflush(out);
		snprintf(cmd, sizeof cmd, "( %s ) 2>&1", survey[i]);
		FILE *p = popen(cmd, "r");
		if (!p) {
			fprintf(out, "popen failed: %s\n", strerror(errno));
			continue;
		}
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, p)) > 0)
			fwrite(buf, 1, n, out);
		fprintf(out, "[exit %d]\n", pclose(p));
	}
	fclose(out);
	logf_("survey written");
}

// ---------------------------------------------------------------------------
// framebuffer
// ---------------------------------------------------------------------------

static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_len;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

static uint32_t pack_rgb(int r, int g, int b)
{
	const struct fb_bitfield *R = &vinfo.red, *G = &vinfo.green, *B = &vinfo.blue;
	return ((uint32_t)(r >> (8 - R->length)) << R->offset) |
		   ((uint32_t)(g >> (8 - G->length)) << G->offset) |
		   ((uint32_t)(b >> (8 - B->length)) << B->offset) |
		   (vinfo.transp.length ? (((1u << vinfo.transp.length) - 1) << vinfo.transp.offset) : 0);
}

static void put_px(int x, int y, uint32_t c)
{
	if (x < 0 || y < 0 || x >= (int)vinfo.xres || y >= (int)vinfo.yres)
		return;
	uint8_t *p = fb_mem + (size_t)(y + vinfo.yoffset) * finfo.line_length +
				 (size_t)(x + vinfo.xoffset) * (vinfo.bits_per_pixel / 8);
	switch (vinfo.bits_per_pixel) {
	case 32:
		*(uint32_t *)p = c;
		break;
	case 24:
		p[0] = c;
		p[1] = c >> 8;
		p[2] = c >> 16;
		break;
	case 16:
		*(uint16_t *)p = c;
		break;
	}
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			put_px(i, j, c);
}

// The panel is a smart LCD: some drivers only send the frame to it on a pan.
static void fb_flush(void)
{
	if (fb_fd >= 0)
		ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
}

static void fb_open(void)
{
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		logf_("open /dev/fb0: %s", strerror(errno));
		return;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)) {
		logf_("fb ioctl: %s", strerror(errno));
		close(fb_fd);
		fb_fd = -1;
		return;
	}
	logf_("fb id=%.16s res=%ux%u virt=%ux%u off=%u,%u bpp=%u line=%u smem=%u",
		  finfo.id, vinfo.xres, vinfo.yres, vinfo.xres_virtual, vinfo.yres_virtual,
		  vinfo.xoffset, vinfo.yoffset, vinfo.bits_per_pixel, finfo.line_length, finfo.smem_len);
	logf_("fb rgba r=%u/%u g=%u/%u b=%u/%u a=%u/%u size=%umm x %umm rotate=%u",
		  vinfo.red.offset, vinfo.red.length, vinfo.green.offset, vinfo.green.length,
		  vinfo.blue.offset, vinfo.blue.length, vinfo.transp.offset, vinfo.transp.length,
		  vinfo.width, vinfo.height, vinfo.rotate);
	if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 24 && vinfo.bits_per_pixel != 32) {
		logf_("fb: %u bpp is not drawn", vinfo.bits_per_pixel);
		return;
	}
	fb_len = finfo.smem_len;
	fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		logf_("fb mmap: %s", strerror(errno));
		fb_mem = NULL;
	}
}

static void draw_bands(void)
{
	static const int colors[][3] = {
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255},
	};
	int n = sizeof colors / sizeof colors[0];
	int h = vinfo.yres - 24;
	for (int i = 0; i < n; i++)
		fill_rect(0, h * i / n, vinfo.xres, h * (i + 1) / n - h * i / n,
				  pack_rgb(colors[i][0], colors[i][1], colors[i][2]));
	// A one-pixel white frame shows whether the visible area matches xres x yres.
	uint32_t w = pack_rgb(255, 255, 255);
	fill_rect(0, 0, vinfo.xres, 1, w);
	fill_rect(0, vinfo.yres - 1, vinfo.xres, 1, w);
	fill_rect(0, 0, 1, vinfo.yres, w);
	fill_rect(vinfo.xres - 1, 0, 1, vinfo.yres, w);
	// Top-left marker, so a rotated or mirrored panel shows up in a photo.
	fill_rect(4, 4, 24, 24, pack_rgb(0, 0, 0));
	fill_rect(8, 8, 8, 8, w);
}

static void draw_progress(double left_fraction)
{
	int y = vinfo.yres - 24, w = (int)(vinfo.xres * left_fraction);
	fill_rect(0, y, w, 23, pack_rgb(255, 255, 255));
	fill_rect(w, y, vinfo.xres - w, 23, pack_rgb(40, 40, 40));
}

static void draw_border(uint32_t c)
{
	int t = 8;
	fill_rect(0, 0, vinfo.xres, t, c);
	fill_rect(0, vinfo.yres - 24 - t, vinfo.xres, t, c);
	fill_rect(0, 0, t, vinfo.yres - 24, c);
	fill_rect(vinfo.xres - t, 0, t, vinfo.yres - 24, c);
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

struct input {
	int fd;
	char path[32];
	char name[64];
	int has_abs_mt, has_abs_xy;
	struct input_absinfo ax, ay;
	int x, y;
};

static struct input inputs[MAX_INPUTS];
static int n_inputs;

static int test_bit(int bit, const unsigned long *bits)
{
	return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1;
}

static void input_open_all(FILE *ev)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	if (!d) {
		logf_("opendir /dev/input: %s", strerror(errno));
		return;
	}
	while ((e = readdir(d)) && n_inputs < MAX_INPUTS) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		struct input *in = &inputs[n_inputs];
		snprintf(in->path, sizeof in->path, "/dev/input/%s", e->d_name);
		in->fd = open(in->path, O_RDONLY | O_NONBLOCK);
		if (in->fd < 0) {
			logf_("open %s: %s", in->path, strerror(errno));
			continue;
		}
		ioctl(in->fd, EVIOCGNAME(sizeof in->name), in->name);
		unsigned long evbits[NLONGS(EV_MAX + 1)] = {0}, absbits[NLONGS(ABS_MAX + 1)] = {0},
					  keybits[NLONGS(KEY_MAX + 1)] = {0};
		ioctl(in->fd, EVIOCGBIT(0, sizeof evbits), evbits);
		ioctl(in->fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
		ioctl(in->fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
		if (test_bit(EV_ABS, evbits)) {
			if (test_bit(ABS_MT_POSITION_X, absbits)) {
				in->has_abs_mt = 1;
				ioctl(in->fd, EVIOCGABS(ABS_MT_POSITION_X), &in->ax);
				ioctl(in->fd, EVIOCGABS(ABS_MT_POSITION_Y), &in->ay);
			} else if (test_bit(ABS_X, absbits)) {
				in->has_abs_xy = 1;
				ioctl(in->fd, EVIOCGABS(ABS_X), &in->ax);
				ioctl(in->fd, EVIOCGABS(ABS_Y), &in->ay);
			}
		}
		fprintf(ev, "# %s \"%s\" evbits=%lx abs=%s x=[%d..%d] y=[%d..%d] keys:", in->path, in->name,
				evbits[0], in->has_abs_mt ? "mt" : in->has_abs_xy ? "xy" : "none", in->ax.minimum,
				in->ax.maximum, in->ay.minimum, in->ay.maximum);
		for (int k = 0; k <= KEY_MAX; k++)
			if (test_bit(k, keybits))
				fprintf(ev, " %d", k);
		fputc('\n', ev);
		n_inputs++;
	}
	closedir(d);
	fflush(ev);
	logf_("%d input devices", n_inputs);
}

static int scale(int v, const struct input_absinfo *a, int out)
{
	int span = a->maximum - a->minimum;
	if (span <= 0)
		return v;
	return (v - a->minimum) * (out - 1) / span;
}

static void input_loop(FILE *ev)
{
	struct pollfd pfd[MAX_INPUTS];
	for (int i = 0; i < n_inputs; i++) {
		pfd[i].fd = inputs[i].fd;
		pfd[i].events = POLLIN;
	}
	double last_draw = -1;
	while (elapsed() < RUN_SECONDS) {
		int r = poll(pfd, n_inputs, 100);
		int dirty = 0;
		if (r > 0)
			for (int i = 0; i < n_inputs; i++) {
				if (!(pfd[i].revents & POLLIN))
					continue;
				struct input_event e;
				struct input *in = &inputs[i];
				while (read(in->fd, &e, sizeof e) == sizeof e) {
					fprintf(ev, "%7.3f %s type=%u code=%u value=%d\n", elapsed(), in->path, e.type, e.code,
							e.value);
					if (!fb_mem)
						continue;
					if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_X || e.code == ABS_X))
						in->x = e.value;
					else if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_Y || e.code == ABS_Y))
						in->y = e.value;
					else if (e.type == EV_SYN && (in->has_abs_mt || in->has_abs_xy)) {
						int sx = scale(in->x, &in->ax, vinfo.xres), sy = scale(in->y, &in->ay, vinfo.yres);
						fill_rect(sx - 4, sy - 4, 9, 9, pack_rgb(255, 255, 255));
						dirty = 1;
					} else if (e.type == EV_KEY && e.value == 1) {
						static const int k[][3] = {{0, 0, 0}, {255, 255, 255}, {255, 128, 0}};
						static int n;
						n = (n + 1) % 3;
						draw_border(pack_rgb(k[n][0], k[n][1], k[n][2]));
						dirty = 1;
					}
				}
			}
		fflush(ev);
		if (fb_mem && (dirty || elapsed() - last_draw > 0.5)) {
			double left = 1.0 - elapsed() / RUN_SECONDS;
			draw_progress(left < 0 ? 0 : left);
			fb_flush();
			last_draw = elapsed();
		}
	}
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------

static void put_le(FILE *f, uint32_t v, int bytes)
{
	for (int i = 0; i < bytes; i++)
		fputc((v >> (8 * i)) & 0xff, f);
}

// Two seconds of 1 kHz at -30 dBFS, 44.1 kHz 16-bit stereo, with 50 ms fades.
static int write_tone(const char *path)
{
	const int rate = 44100, secs = 2, frames = rate * secs, fade = rate / 20;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	uint32_t data = frames * 4;
	fwrite("RIFF", 1, 4, f);
	put_le(f, 36 + data, 4);
	fwrite("WAVEfmt ", 1, 8, f);
	put_le(f, 16, 4);
	put_le(f, 1, 2);
	put_le(f, 2, 2);
	put_le(f, rate, 4);
	put_le(f, rate * 4, 4);
	put_le(f, 4, 2);
	put_le(f, 16, 2);
	fwrite("data", 1, 4, f);
	put_le(f, data, 4);
	const double amp = 32767 * 0.0316; // -30 dBFS
	for (int i = 0; i < frames; i++) {
		double g = 1.0;
		if (i < fade)
			g = (double)i / fade;
		else if (i > frames - fade)
			g = (double)(frames - i) / fade;
		int16_t s = (int16_t)lrint(amp * g * sin(2 * M_PI * 1000.0 * i / rate));
		put_le(f, (uint16_t)s, 2);
		put_le(f, (uint16_t)s, 2);
	}
	fclose(f);
	return 0;
}

static void play_tone(void)
{
	const char *wav = "/tmp/r3ii_probe_tone.wav";
	if (write_tone(wav)) {
		logf_("cannot write %s", wav);
		return;
	}
	char cmd[512];
	snprintf(cmd, sizeof cmd, "aplay -v %s > %s/aplay.txt 2>&1", wav, out_dir);
	int st = system(cmd);
	logf_("aplay exit %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	unlink(wav);
}

// ---------------------------------------------------------------------------

int main(void)
{
	clock_gettime(CLOCK_MONOTONIC, &t0);
	alarm(HARD_LIMIT_SECONDS);
	signal(SIGPIPE, SIG_IGN);

	pick_out_dir();
	log_file = open_out("probe.log");
	logf_("r3ii_probe started, writing to %s", out_dir);

	fb_open();
	if (fb_mem) {
		draw_bands();
		draw_progress(1.0);
		fb_flush();
	}

	FILE *ev = open_out("events.txt");
	if (!ev)
		ev = fopen("/dev/null", "w");
	input_open_all(ev);

	// The survey and the tone run in a child so the screen keeps answering
	// touches while they take their time.
	pid_t child = fork();
	if (child == 0) {
		alarm(HARD_LIMIT_SECONDS);
		run_survey();
		play_tone();
		_exit(0);
	}

	input_loop(ev);

	if (child > 0) {
		int st;
		if (waitpid(child, &st, WNOHANG) == 0)
			logf_("survey still running at the end, left behind");
	}
	fclose(ev);
	logf_("done");
	if (fb_mem) {
		memset(fb_mem, 0, fb_len);
		fb_flush();
	}
	fclose(log_file);
	sync();
	return 0;
}
