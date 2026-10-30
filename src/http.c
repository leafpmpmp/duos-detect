// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * One thread serves every client from a poll() loop. The pipeline hands
 * over results and, when a viewer is due one, a preview frame; this thread
 * encodes the JPEG. A client that falls behind skips frames (and is closed
 * when it falls far behind), so a slow network never stalls detection.
 */
#define _GNU_SOURCE	/* accept4 */
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "af.h"
#include "http.h"
#include "lens.h"
#include "preview.h"

#define MAX_CLIENTS	16
#define REQ_MAX		2048
#define OUT_MAX		(4 << 20)	/* close a client this far behind */
#define EVENTS_SKIP	(64 << 10)	/* skip results while this much waits */
#define MAX_BOX		300
#define QUALITY		75

enum kind { NEW, ONESHOT, STREAM, SNAPSHOT, EVENTS };

struct client {
	int fd;
	enum kind kind;
	char req[REQ_MAX];
	size_t req_len;
	char *out;
	size_t len, off, cap;
	int close_when_sent;
};

struct frame {
	int8_t *rgb;
	size_t cap;
	int w, h, n;
	struct det_box box[MAX_BOX];
};

struct http_server {
	int lfd, efd;
	pthread_t th;
	volatile int stop;
	double interval;	/* ms between preview frames */

	/* Shared with the pipeline, under lock */
	pthread_mutex_t lock;
	char *json;		/* latest result */
	unsigned long json_seq;
	struct frame in;
	int frame_ready, want;
	double last_frame;

	/* This thread only */
	struct client cl[MAX_CLIENTS];
	unsigned long sent_seq;
	struct frame work;
};

static const char page[] =
	"<!doctype html><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'>"
	"<title>duos-detect</title><style>"
	"body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif}"
	"main{max-width:900px;margin:auto;padding:12px}"
	"img{width:100%;background:#222}pre{white-space:pre-wrap}a{color:#8cf}"
	"</style><main><img src=/stream.mjpg alt=preview><pre id=s>waiting</pre>"
	"<p><label>focus <input id=f type=range min=0 max=1023 style='width:55%'>"
	" <span id=fv></span></label> <button id=a>auto</button>"
	"<p><a href=/api/detections>/api/detections</a> "
	"<a href=/api/events>/api/events</a> <a href=/snapshot.jpg>/snapshot.jpg</a>"
	"</main><script>"
	"const f=document.getElementById('f'),fv=document.getElementById('fv');"
	"fetch('/api/focus').then(r=>r.json()).then(j=>{if(j.focus===undefined)"
	"{f.disabled=true;fv.textContent='no lens';return}"
	"f.min=j.min;f.max=j.max;f.value=fv.textContent=j.focus});"
	"f.oninput=()=>{fv.textContent=f.value;fetch('/api/focus?value='+f.value)};"
	"const poll=()=>fetch('/api/focus').then(r=>r.json()).then(j=>{"
	"f.value=fv.textContent=j.focus;if(j.auto)setTimeout(poll,200)});"
	"document.getElementById('a').onclick=()=>"
	"fetch('/api/focus?auto=1').then(()=>setTimeout(poll,200));"
	"const s=document.getElementById('s'),e=new EventSource('/api/events');"
	"let n=0,t=performance.now();"
	"e.onmessage=m=>{const d=JSON.parse(m.data),now=performance.now();n++;"
	"if(now-t>=1000){s.textContent=(n*1000/(now-t)).toFixed(1)+' results/s, '"
	"+d.boxes.length+' people, latency '+d.latency_ms+' ms';n=0;t=now}};"
	"</script>";

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void wake(struct http_server *s)
{
	uint64_t one = 1;

	if (write(s->efd, &one, sizeof(one)) < 0 && errno != EAGAIN)
		perror("http wake");
}

static void drop(struct client *c)
{
	close(c->fd);
	free(c->out);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static size_t pending(const struct client *c)
{
	return c->len - c->off;
}

static int append(struct client *c, const void *p, size_t n)
{
	if (c->off && c->off == c->len)
		c->off = c->len = 0;
	if (pending(c) + n > OUT_MAX)
		return -1;
	if (c->len + n > c->cap) {
		size_t cap = c->cap ? c->cap : 4096;
		char *q;

		while (cap < c->len + n)
			cap *= 2;
		q = realloc(c->out, cap);
		if (!q)
			return -1;
		c->out = q;
		c->cap = cap;
	}
	memcpy(c->out + c->len, p, n);
	c->len += n;
	return 0;
}

static int appendf(struct client *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static int appendf(struct client *c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	return append(c, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void flush(struct client *c)
{
	while (pending(c)) {
		ssize_t n = send(c->fd, c->out + c->off, pending(c),
				 MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno != EAGAIN && errno != EINTR)
				drop(c);
			return;
		}
		c->off += n;
	}
	if (c->close_when_sent)
		drop(c);
}

static void reply(struct client *c, const char *status, const char *type,
		  const void *body, size_t n)
{
	c->kind = ONESHOT;
	c->close_when_sent = 1;
	if (appendf(c, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		    "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
		    "Connection: close\r\n\r\n", status, type, n) ||
	    append(c, body, n))
		drop(c);
}

/* Parse "GET /path HTTP/1.x" and set the client up */
static void route(struct http_server *s, struct client *c)
{
	char path[256] = "", *query;

	if (sscanf(c->req, "GET %255s", path) != 1) {
		reply(c, "405 Method Not Allowed", "text/plain", "GET only\n", 9);
		return;
	}
	query = strchr(path, '?');
	if (query)
		*query++ = 0;
	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		reply(c, "200 OK", "text/html", page, sizeof(page) - 1);
	} else if (!strcmp(path, "/api/detections")) {
		pthread_mutex_lock(&s->lock);
		if (s->json)
			reply(c, "200 OK", "application/json", s->json, strlen(s->json));
		else
			reply(c, "200 OK", "application/json", "{}\n", 3);
		pthread_mutex_unlock(&s->lock);
	} else if (!strcmp(path, "/api/focus")) {
		char body[96];
		int v, min, max, n;
		const char *q = query ? strstr(query, "value=") : NULL;

		if (q && !af_busy())
			lens_set(atoi(q + 6));
		if (query && strstr(query, "auto"))
			af_start();
		if (lens_get(&v, &min, &max)) {
			reply(c, "404 Not Found", "application/json", "{}\n", 3);
		} else {
			n = snprintf(body, sizeof(body),
				     "{\"focus\":%d,\"min\":%d,\"max\":%d,\"auto\":%s}\n",
				     v, min, max, af_busy() ? "true" : "false");
			reply(c, "200 OK", "application/json", body, n);
		}
	} else if (!strcmp(path, "/api/events")) {
		c->kind = EVENTS;
		if (appendf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
			    "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
			    "Connection: keep-alive\r\n\r\n"))
			drop(c);
	} else if (!strcmp(path, "/stream.mjpg")) {
		c->kind = STREAM;
		if (appendf(c, "HTTP/1.1 200 OK\r\n"
			    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
			    "Cache-Control: no-cache\r\nConnection: close\r\n\r\n"))
			drop(c);
	} else if (!strcmp(path, "/snapshot.jpg")) {
		c->kind = SNAPSHOT;	/* answered with the next frame */
	} else {
		reply(c, "404 Not Found", "text/plain", "not found\n", 10);
	}
}

static void read_request(struct http_server *s, struct client *c)
{
	ssize_t n = recv(c->fd, c->req + c->req_len,
			 sizeof(c->req) - 1 - c->req_len, MSG_DONTWAIT);

	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR))
			drop(c);
		return;
	}
	c->req_len += n;
	c->req[c->req_len] = 0;
	if (strstr(c->req, "\r\n\r\n"))
		route(s, c);
	else if (c->req_len == sizeof(c->req) - 1)
		drop(c);
}

static void new_result(struct http_server *s)
{
	char *json = NULL;
	size_t len;

	pthread_mutex_lock(&s->lock);
	if (s->json && s->json_seq != s->sent_seq) {
		json = strdup(s->json);
		s->sent_seq = s->json_seq;
	}
	pthread_mutex_unlock(&s->lock);
	if (!json)
		return;
	len = strlen(json);
	if (len && json[len - 1] == '\n')
		len--;		/* the event ends with an empty line instead */
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &s->cl[i];

		if (c->fd < 0 || c->kind != EVENTS || pending(c) > EVENTS_SKIP)
			continue;
		if (appendf(c, "data: ") || append(c, json, len) ||
		    appendf(c, "\n\n"))
			drop(c);
	}
	free(json);
}

static void new_frame(struct http_server *s)
{
	struct frame t;
	unsigned char *jpg;
	unsigned long n;
	int ready;

	pthread_mutex_lock(&s->lock);
	ready = s->frame_ready;
	if (ready) {
		t = s->in;		/* swap buffers: the pipeline keeps one */
		s->in.rgb = s->work.rgb;
		s->in.cap = s->work.cap;
		s->work = t;
		s->frame_ready = 0;
	}
	pthread_mutex_unlock(&s->lock);
	if (!ready)
		return;
	jpg = preview_jpeg(s->work.rgb, s->work.w, s->work.h, s->work.box,
			   s->work.n, QUALITY, PREVIEW_AUTO, &n);
	if (!jpg)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &s->cl[i];

		if (c->fd < 0)
			continue;
		if (c->kind == STREAM && !pending(c)) {
			if (appendf(c, "--frame\r\nContent-Type: image/jpeg\r\n"
				    "Content-Length: %lu\r\n\r\n", n) ||
			    append(c, jpg, n) || appendf(c, "\r\n"))
				drop(c);
		} else if (c->kind == SNAPSHOT) {
			reply(c, "200 OK", "image/jpeg", jpg, n);
		}
	}
	free(jpg);
}

static void *http_thread(void *arg)
{
	struct http_server *s = arg;
	struct sched_param sp = { 0 };
	struct pollfd pfd[MAX_CLIENTS + 2];

	/*
	 * Use only the CPU time the pipeline leaves free, with or without
	 * real-time priority: a JPEG takes up to ~25 ms on the C906.
	 */
	if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp))
		perror("http thread SCHED_IDLE");
	while (!s->stop) {
		int n = 0, want = 0;

		pfd[n++] = (struct pollfd){ .fd = s->lfd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = s->efd, .events = POLLIN };
		for (int i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &s->cl[i];

			pfd[n] = (struct pollfd){ .fd = c->fd };
			if (c->fd >= 0) {
				pfd[n].events = c->kind == NEW ? POLLIN : 0;
				if (pending(c))
					pfd[n].events |= POLLOUT;
				want += c->kind == STREAM || c->kind == SNAPSHOT;
			}
			n++;
		}
		pthread_mutex_lock(&s->lock);
		s->want = want;
		pthread_mutex_unlock(&s->lock);

		if (poll(pfd, n, 500) < 0) {
			if (errno == EINTR)
				continue;
			perror("http poll");
			break;
		}
		if (pfd[0].revents & POLLIN) {
			int fd = accept4(s->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
			int i;

			for (i = 0; fd >= 0 && i < MAX_CLIENTS && s->cl[i].fd >= 0; i++)
				;
			if (fd >= 0 && i < MAX_CLIENTS) {
				memset(&s->cl[i], 0, sizeof(s->cl[i]));
				s->cl[i].fd = fd;
			} else if (fd >= 0) {
				close(fd);	/* full */
			}
		}
		if (pfd[1].revents & POLLIN) {
			uint64_t v;

			if (read(s->efd, &v, sizeof(v)) < 0 && errno != EAGAIN)
				perror("http eventfd");
			new_result(s);
			new_frame(s);
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &s->cl[i];
			short ev = pfd[i + 2].revents;

			if (c->fd < 0 || pfd[i + 2].fd != c->fd || !ev)
				continue;
			if (ev & (POLLERR | POLLHUP | POLLNVAL))
				drop(c);
			else if ((ev & POLLIN) && c->kind == NEW)
				read_request(s, c);
			else if (ev & POLLOUT)
				flush(c);
		}
	}
	return NULL;
}

struct http_server *http_start(int port, int preview_fps)
{
	struct http_server *s = calloc(1, sizeof(*s));
	struct sockaddr_in a = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	int one = 1;

	if (!s)
		return NULL;
	pthread_mutex_init(&s->lock, NULL);
	s->interval = 1000.0 / (preview_fps > 0 ? preview_fps : 15);
	for (int i = 0; i < MAX_CLIENTS; i++)
		s->cl[i].fd = -1;
	s->lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	s->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (s->lfd < 0 || s->efd < 0 ||
	    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
	    bind(s->lfd, (struct sockaddr *)&a, sizeof(a)) || listen(s->lfd, 16) ||
	    pthread_create(&s->th, NULL, http_thread, s)) {
		perror("http server");
		if (s->lfd >= 0)
			close(s->lfd);
		if (s->efd >= 0)
			close(s->efd);
		free(s);
		return NULL;
	}
	return s;
}

void http_stop(struct http_server *s)
{
	if (!s)
		return;
	s->stop = 1;
	wake(s);
	pthread_join(s->th, NULL);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (s->cl[i].fd >= 0)
			drop(&s->cl[i]);
	close(s->lfd);
	close(s->efd);
	free(s->json);
	free(s->in.rgb);
	free(s->work.rgb);
	free(s);
}

void http_publish(struct http_server *s, const char *json)
{
	char *copy = strdup(json);

	if (!copy)
		return;
	pthread_mutex_lock(&s->lock);
	free(s->json);
	s->json = copy;
	s->json_seq++;
	pthread_mutex_unlock(&s->lock);
	wake(s);
}

int http_want_frame(struct http_server *s)
{
	int r;

	pthread_mutex_lock(&s->lock);
	r = s->want && !s->frame_ready && now_ms() - s->last_frame >= s->interval;
	pthread_mutex_unlock(&s->lock);
	return r;
}

void http_put_frame(struct http_server *s, const int8_t *rgb, int w, int h,
		    const struct det_box *box, int n)
{
	size_t size = (size_t)3 * w * h;

	pthread_mutex_lock(&s->lock);
	if (s->in.cap < size) {
		int8_t *p = realloc(s->in.rgb, size);

		if (!p) {
			pthread_mutex_unlock(&s->lock);
			return;
		}
		s->in.rgb = p;
		s->in.cap = size;
	}
	memcpy(s->in.rgb, rgb, size);
	s->in.w = w;
	s->in.h = h;
	s->in.n = n < MAX_BOX ? n : MAX_BOX;
	memcpy(s->in.box, box, s->in.n * sizeof(*box));
	s->frame_ready = 1;
	s->last_frame = now_ms();
	pthread_mutex_unlock(&s->lock);
	wake(s);
}
