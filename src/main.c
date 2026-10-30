// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * duos-detect: camera -> CPU front end -> TPU detector -> boxes.
 *
 * Two threads share two detector slots. The capture thread runs the front
 * end into a slot that the TPU does not use. If that slot still holds an
 * unprocessed frame, the newer frame replaces it (low latency). The TPU
 * thread runs the detector on the newest ready slot and decodes the boxes.
 */
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <linux/videodev2.h>

#include "capture.h"
#include "detector.h"
#include "frontend.h"
#include "http.h"
#include "af.h"
#include "camera.h"
#include "lens.h"

#define NSLOTS	2
#define MAX_BOX	300

enum slot_state { FREE, WRITING, READY, BUSY };

struct slot {
	enum slot_state st;
	uint32_t seq;
	uint64_t ts_ns;
};

struct stats {
	unsigned long frames, done, dropped, lost;
	double fe, tpu, dec, lat;
};

/* Largest stage times since the last report */
struct peaks {
	double fe, tpu;
};

static struct {
	struct detector *det;
	struct slot slot[NSLOTS];
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct stats st, last;
	double t0, cpu0, t_last, cpu_last, lat_max, lat_max_all;
	struct peaks pk;
	volatile sig_atomic_t stop;
	int print, cls, max_box, rt;
	struct http_server *http;
	int http_port, preview_fps, frame_w, frame_h, focus, focus_auto;
	struct cam_setup cam;	/* sensor mode and frame rate */
	int keep_cam;		/* use the camera setup as it is */
	float conf, iou, scale_x, scale_y, off_x, off_y;
	double interval;
	int lost_warned;
} g = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
	.conf = 0.4f, .iou = 0.45f, .interval = 2, .max_box = 64, .focus = -1,
};

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double cpu_ms(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 +
	       (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

static void on_signal(int sig)
{
	(void)sig;
	g.stop = 1;
}

/*
 * One result as a line of JSON, boxes in frame pixels:
 * {"seq":N,"latency_ms":L,"frame":[W,H],"boxes":[[x1,y1,x2,y2,score],...]}
 */
static const char *to_json(uint32_t seq, double lat, const struct det_box *b, int n)
{
	static char buf[64 + MAX_BOX * 56];
	size_t len;

	len = snprintf(buf, sizeof(buf),
		       "{\"seq\":%u,\"latency_ms\":%.1f,\"frame\":[%d,%d],\"boxes\":[",
		       seq, lat, g.frame_w, g.frame_h);
	for (int i = 0; i < n && len < sizeof(buf) - 64; i++)
		len += snprintf(buf + len, sizeof(buf) - len,
				"%s[%.1f,%.1f,%.1f,%.1f,%.4f]", i ? "," : "",
				b[i].x1 * g.scale_x + g.off_x, b[i].y1 * g.scale_y + g.off_y,
				b[i].x2 * g.scale_x + g.off_x, b[i].y2 * g.scale_y + g.off_y,
				b[i].score);
	snprintf(buf + len, sizeof(buf) - len, "]}\n");
	return buf;
}

/* Print stats for the last interval, or for the whole run. g.lock held. */
static void report(int final)
{
	double t = now_ms(), cpu = cpu_ms();
	struct stats d = g.st, b = final ? (struct stats){ 0 } : g.last;
	double tb = final ? g.t0 : g.t_last, cb = final ? g.cpu0 : g.cpu_last;
	unsigned long n;

	if (!final && (g.interval <= 0 || t - g.t_last < g.interval * 1e3))
		return;
	d.frames -= b.frames;
	d.done -= b.done;
	d.dropped -= b.dropped;
	d.lost -= b.lost;
	d.fe -= b.fe;
	d.tpu -= b.tpu;
	d.dec -= b.dec;
	d.lat -= b.lat;
	n = d.done ? d.done : 1;
	fprintf(stderr,
		"%s%5.1f fps | front end %.2f ms | tpu %.2f ms | decode %.2f ms | "
		"latency %.1f ms (max %.1f) | dropped %lu | lost %lu | cpu %.0f%%%s",
		final ? "total: " : "", d.done * 1e3 / (t - tb),
		d.fe / (d.frames ? d.frames : 1), d.tpu / n, d.dec / n,
		d.lat / n, final ? g.lat_max_all : g.lat_max, d.dropped, d.lost,
		100 * (cpu - cb) / (t - tb), final ? "\n" : "");
	if (!final)
		fprintf(stderr, " | peak front end %.1f tpu %.1f ms\n", g.pk.fe, g.pk.tpu);
	if (!g.lost_warned && d.lost * 10 > d.frames + d.lost) {
		g.lost_warned = 1;
		fprintf(stderr, "camera: the capture driver lost %lu of %lu frames: "
			"set a lower frame rate (-r)\n", d.lost, d.frames + d.lost);
	}
	if (!final) {
		g.last = g.st;
		g.t_last = t;
		g.cpu_last = cpu;
		g.lat_max = 0;
		g.pk = (struct peaks){ 0 };
	}
}

/* Run the detector on one slot and decode; returns the box count */
static int detect(int k, uint32_t seq, uint64_t ts_ns, struct det_box *box)
{
	double t0 = now_ms(), t1, t2, lat;
	int n;

	if (det_run(g.det, k)) {
		fprintf(stderr, "tpu run failed\n");
		g.stop = 1;
		return 0;
	}
	t1 = now_ms();
	n = det_decode(g.det, k, g.cls, g.conf, g.iou, box, g.max_box);
	t2 = now_ms();
	lat = ts_ns ? t2 - ts_ns / 1e6 : 0;
	if (g.print || g.http) {
		const char *json = to_json(seq, lat, box, n);

		if (g.print) {
			fputs(json, stdout);
			fflush(stdout);
		}
		if (g.http)
			http_publish(g.http, json);
	}
	if (g.http && http_want_frame(g.http)) {
		int w, h;

		det_input_size(g.det, &w, &h);
		http_put_frame(g.http, det_input(g.det, k), w, h, box, n);
	}
	pthread_mutex_lock(&g.lock);
	g.st.done++;
	g.st.tpu += t1 - t0;
	if (t1 - t0 > g.pk.tpu)
		g.pk.tpu = t1 - t0;
	g.st.dec += t2 - t1;
	g.st.lat += lat;
	if (lat > g.lat_max)
		g.lat_max = lat;
	if (lat > g.lat_max_all)
		g.lat_max_all = lat;
	report(0);
	pthread_mutex_unlock(&g.lock);
	return n;
}

/* Real-time priority keeps other tasks from delaying a frame */
static void set_rt(int prio)
{
	struct sched_param sp = { .sched_priority = prio };
	int err;

	if (!g.rt)
		return;
	err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	if (err == EPERM)
		fprintf(stderr, "real-time priority refused: run as root in the root "
			"cgroup, or boot with rt_group_sched=0\n");
	else if (err)
		fprintf(stderr, "real-time priority: %s\n", strerror(err));
	if (err)
		g.rt = 0;	/* the other threads stay at normal priority too */
}

/*
 * The TPU driver runs each job in its kernel thread "cvitask_tpu_wor". Put
 * it above our threads, or the front end delays the job start.
 */
static void set_rt_tpu_worker(int prio)
{
	struct sched_param sp = { .sched_priority = prio };
	struct dirent *e;
	DIR *d = opendir("/proc");
	char path[64], comm[32];
	int found = 0;

	while (d && (e = readdir(d))) {
		FILE *f;

		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%.16s/comm", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof(comm), f) &&
		    !strncmp(comm, "cvitask_tpu_wor", 15)) {
			found = 1;
			if (sched_setscheduler(atoi(e->d_name), SCHED_FIFO, &sp))
				perror("TPU worker priority");
		}
		fclose(f);
	}
	if (d)
		closedir(d);
	if (!found)
		fprintf(stderr, "TPU worker thread not found\n");
}

static void *tpu_thread(void *arg)
{
	struct det_box box[MAX_BOX];

	(void)arg;
	set_rt(g.rt);
	for (;;) {
		int k = -1;

		pthread_mutex_lock(&g.lock);
		while (!g.stop) {
			for (int i = 0; i < NSLOTS; i++)
				if (g.slot[i].st == READY &&
				    (k < 0 || g.slot[i].seq > g.slot[k].seq))
					k = i;
			if (k >= 0)
				break;
			pthread_cond_wait(&g.cond, &g.lock);
		}
		if (g.stop) {
			pthread_mutex_unlock(&g.lock);
			return NULL;
		}
		g.slot[k].st = BUSY;
		pthread_mutex_unlock(&g.lock);

		detect(k, g.slot[k].seq, g.slot[k].ts_ns, box);

		pthread_mutex_lock(&g.lock);
		g.slot[k].st = FREE;
		pthread_cond_broadcast(&g.cond);
		pthread_mutex_unlock(&g.lock);
	}
}

/* Take a slot for the front end: a free one, else replace a ready one */
static int take_slot(void)
{
	int k = -1;

	pthread_mutex_lock(&g.lock);
	for (;;) {
		for (int i = 0; i < NSLOTS && k < 0; i++)
			if (g.slot[i].st == FREE)
				k = i;
		for (int i = 0; i < NSLOTS && k < 0; i++)
			if (g.slot[i].st == READY) {
				k = i;
				g.st.dropped++;
			}
		if (k >= 0 || g.stop)
			break;
		pthread_cond_wait(&g.cond, &g.lock);
	}
	if (k >= 0)
		g.slot[k].st = WRITING;
	pthread_mutex_unlock(&g.lock);
	return k;
}

static int run_camera(const char *dev, struct fe_params *fp, long frames,
		      int sequential)
{
	struct capture *cap;
	struct cam_info ci;
	struct frontend *fe;
	pthread_t th;
	uint32_t fourcc;
	struct det_box box[MAX_BOX];
	uint32_t seq_next = 0;
	int bin = 4;

	if (!g.keep_cam) {
		g.cam.in_w = fp->out_w;
		g.cam.in_h = fp->out_h;
		if (camera_setup(&g.cam, &ci))
			return 1;
		fprintf(stderr, "camera: %s %dx%d at %.1f fps%s\n", ci.sensor,
			ci.width, ci.height, ci.fps, ci.flipped ? ", flipped (rotation 180)" : "");
		if (!dev)
			dev = ci.video;
	}
	cap = cap_open(dev ? dev : "/dev/video0", 4);
	if (!cap)
		return 1;
	cap_format(cap, &fp->width, &fp->height, &fp->stride, &fourcc);
	if (fourcc != V4L2_PIX_FMT_SBGGR12P && fourcc != V4L2_PIX_FMT_SGBRG12P &&
	    fourcc != V4L2_PIX_FMT_SGRBG12P && fourcc != V4L2_PIX_FMT_SRGGB12P) {
		fprintf(stderr, "need packed RAW12, got %.4s\n", (char *)&fourcc);
		return 1;
	}
	fp->bayer = fourcc == V4L2_PIX_FMT_SBGGR12P ? "BGGR" :
		    fourcc == V4L2_PIX_FMT_SGBRG12P ? "GBRG" :
		    fourcc == V4L2_PIX_FMT_SGRBG12P ? "GRBG" : "RGGB";
	/*
	 * Centre the detector input in the 4x4-binned frame. A frame narrower
	 * than the input gets grey columns on both sides, like the letterbox
	 * in training.
	 */
	if (fp->width / bin >= fp->out_w) {
		fp->left = (fp->width / bin - fp->out_w) / 2;
	} else {
		fp->pad = (fp->out_w - fp->width / bin + 1) / 2;
		fp->pad_value = 114 / 255.0f;
	}
	fp->top = (fp->height / bin - fp->out_h) / 2;
	fe = fe_create(fp);
	if (!fe) {
		fprintf(stderr, "%dx%d frame cannot give a %dx%d input\n",
			fp->width, fp->height, fp->out_w, fp->out_h);
		return 1;
	}
	g.scale_x = g.scale_y = bin;
	g.off_x = (fp->left - fp->pad) * bin;
	g.off_y = fp->top * bin;
	fprintf(stderr, "%dx%d %s -> %dx%d detector input (%d grey columns), "
		"%s front end, %s\n", fp->width, fp->height, fp->bayer, fp->out_w,
		fp->out_h, 2 * fp->pad, fe_simd(fe),
		sequential ? "sequential" : "pipelined");

	g.frame_w = fp->width;
	g.frame_h = fp->height;
	/* Keep the lens powered for live focusing, or at the -f position */
	if ((g.focus >= 0 || g.focus_auto || g.http_port) && lens_open())
		fprintf(stderr, "no focus lens found\n");
	else if (g.focus >= 0 && lens_set(g.focus))
		perror("focus");
	if (g.http_port) {
		/* Created now, it starts at normal priority like this thread */
		g.http = http_start(g.http_port, g.preview_fps);
		if (!g.http)
			return 1;
		fprintf(stderr, "http://<board>:%d/\n", g.http_port);
	}
	if (g.rt)
		set_rt_tpu_worker(g.rt + 1);
	/* Capture just below the TPU thread: a ready frame starts the TPU first */
	set_rt(g.rt > 1 ? g.rt - 1 : g.rt);
	if (!sequential && pthread_create(&th, NULL, tpu_thread, NULL))
		return 1;
	if (cap_start(cap)) {
		perror("stream on");
		g.stop = 1;
	}
	g.t0 = g.t_last = now_ms();
	g.cpu0 = g.cpu_last = cpu_ms();
	if (g.focus_auto)
		af_start();
	for (long f = 0; !g.stop && (!frames || f < frames); f++) {
		struct cap_frame fr;
		double t0;
		int k = 0;
		int32_t lost;

		if (cap_dequeue(cap, &fr)) {
			if (errno != EINTR)
				perror("dequeue");
			break;
		}
		/* The driver numbers every sensor frame: a gap is a lost frame */
		lost = f ? (int32_t)(fr.sequence - seq_next) : 0;
		seq_next = fr.sequence + 1;
		/* Contrast autofocus measures the raw frame while it runs */
		if (af_busy())
			af_frame(fr.data, fp->width, fp->height, fp->stride);
		if (!sequential && (k = take_slot()) < 0) {
			cap_queue(cap, fr.index);
			break;
		}
		t0 = now_ms();
		fe_run(fe, fr.data, det_input(g.det, k));
		t0 = now_ms() - t0;
		cap_queue(cap, fr.index);

		pthread_mutex_lock(&g.lock);
		g.st.frames++;
		if (lost > 0)
			g.st.lost += lost;
		g.st.fe += t0;
		if (t0 > g.pk.fe)
			g.pk.fe = t0;
		if (!sequential) {
			g.slot[k] = (struct slot){ READY, fr.sequence, fr.ts_ns };
			pthread_cond_broadcast(&g.cond);
		}
		pthread_mutex_unlock(&g.lock);
		if (sequential)
			detect(0, fr.sequence, fr.ts_ns, box);
	}

	pthread_mutex_lock(&g.lock);
	g.stop = 1;
	pthread_cond_broadcast(&g.cond);
	pthread_mutex_unlock(&g.lock);
	if (!sequential)
		pthread_join(th, NULL);
	cap_stop(cap);
	cap_close(cap);
	fe_destroy(fe);
	http_stop(g.http);
	g.http = NULL;
	lens_close();
	return 0;
}

/*
 * Detect on images: planar RGB uint8 [3][h][w] at the detector size. Path
 * "-" reads such frames back to back from stdin, one JSON line per frame.
 */
static int run_image(const char *path)
{
	int w, h;
	float s = det_input_scale(g.det) / 255;
	struct det_box box[MAX_BOX];
	FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	int8_t *in = det_input(g.det, 0);
	uint8_t *px;
	size_t n, got = 0;
	uint32_t seq = 0;

	det_input_size(g.det, &w, &h);
	n = (size_t)3 * w * h;
	px = malloc(n);
	if (!f || !px) {
		perror(path);
		return 1;
	}
	g.print = 1;
	g.scale_x = g.scale_y = 1;
	g.interval = 1e9;	/* no periodic stats */
	while (!g.stop && (got = fread(px, 1, n, f)) == n) {
		for (size_t i = 0; i < n; i++)
			in[i] = (int8_t)(px[i] * s + 0.5f);
		detect(0, seq++, 0, box);
	}
	if (f != stdin)
		fclose(f);
	free(px);
	if (!seq || got % n) {
		fprintf(stderr, "%s: need whole frames of %zu bytes\n", path, n);
		return 1;
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s -m model.cvimodel [options]\n"
		"  -d dev     capture device (default: found in the media graph)\n"
		"  -s WxH     sensor mode (default: the best fit for the model input)\n"
		"  -r fps     frame rate (default: the fastest of the mode)\n"
		"  -K         keep the camera setup as it is (no flips, modes, rate)\n"
		"  -i file    detect on planar RGB images instead of the camera\n"
		"             (\"-\": a stream of them on stdin)\n"
		"  -n frames  stop after this many frames (0: until Ctrl-C)\n"
		"  -c conf    score threshold (0.4)\n"
		"  -I iou     NMS overlap threshold (0.45)\n"
		"  -M boxes   most boxes per frame (64, at most 300)\n"
		"  -k class   class index (0: person)\n"
		"  -g R,G,B   white balance gains (1.942,1,1.623)\n"
		"  -B black   black level, 8-bit scale (16)\n"
		"  -p         print boxes as JSON lines\n"
		"  -L sec     statistics every sec seconds (2; 0: only at the end)\n"
		"  -S         sequential: no CPU/TPU overlap\n"
		"  -f pos     focus lens position (Camera Module 3: 0..1023), or\n"
		"             \"auto\": contrast autofocus at start\n"
		"  -H port    serve a preview and the results over HTTP\n"
		"  -F fps     preview frame rate (15)\n"
		"  -R prio    real-time: TPU thread SCHED_FIFO at prio, capture at\n"
		"             prio-1, the TPU driver's worker at prio+1\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *model = NULL, *dev = NULL, *image = NULL;
	struct fe_params fp = {
		.black = 16, .white = 255, .gamma = 2.2f,
		.gains = { 1.942f, 1.0f, 1.623f },
	};
	long frames = 0;
	int sequential = 0, opt, ret;

	while ((opt = getopt(argc, argv, "m:d:i:n:c:I:M:k:g:B:pSR:H:F:f:s:r:KL:")) != -1) {
		switch (opt) {
		case 'm': model = optarg; break;
		case 'd': dev = optarg; break;
		case 'i': image = optarg; break;
		case 'n': frames = atol(optarg); break;
		case 'c': g.conf = atof(optarg); break;
		case 'I': g.iou = atof(optarg); break;
		case 'M': g.max_box = atoi(optarg); break;
		case 'k': g.cls = atoi(optarg); break;
		case 'g':
			if (sscanf(optarg, "%f,%f,%f", &fp.gains[0], &fp.gains[1],
				   &fp.gains[2]) != 3) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'B': fp.black = atof(optarg); break;
		case 'p': g.print = 1; break;
		case 'S': sequential = 1; break;
		case 'R': g.rt = atoi(optarg); break;
		case 'H': g.http_port = atoi(optarg); break;
		case 'F': g.preview_fps = atoi(optarg); break;
		case 's':
			if (sscanf(optarg, "%dx%d", &g.cam.width, &g.cam.height) != 2) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'r': g.cam.fps = atof(optarg); break;
		case 'K': g.keep_cam = 1; break;
		case 'L': g.interval = atof(optarg); break;
		case 'f':
			if (!strcmp(optarg, "auto"))
				g.focus_auto = 1;
			else
				g.focus = atoi(optarg);
			break;
		default: usage(argv[0]); return 1;
		}
	}
	if (!model || g.max_box < 1 || g.max_box > MAX_BOX) {
		usage(argv[0]);
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	g.det = det_open(model, NSLOTS);
	if (!g.det)
		return 1;
	if (g.cls >= det_classes(g.det)) {
		fprintf(stderr, "model has %d classes\n", det_classes(g.det));
		return 1;
	}
	det_input_size(g.det, &fp.out_w, &fp.out_h);
	fp.out_scale = det_input_scale(g.det);

	ret = image ? run_image(image) : run_camera(dev, &fp, frames, sequential);
	if (!image && g.st.frames) {
		pthread_mutex_lock(&g.lock);
		report(1);
		pthread_mutex_unlock(&g.lock);
	}
	det_close(g.det);
	return ret;
}
