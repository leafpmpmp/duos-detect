/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef DD_HTTP_H
#define DD_HTTP_H

#include <stdint.h>

#include "detector.h"

/*
 * Small HTTP server in its own thread (SCHED_IDLE: it only uses CPU time
 * that detection leaves free), for watching and
 * for integration:
 *   /               viewer page
 *   /stream.mjpg    MJPEG preview of the detector input with the boxes
 *   /snapshot.jpg   one preview frame
 *   /api/detections the latest result (JSON)
 *   /api/focus      lens position; ?value=N moves it, ?auto runs autofocus
 *   /api/events     every result as Server-Sent Events (JSON)
 * Slow clients skip frames; they never block the pipeline.
 */
struct http_server;

struct http_server *http_start(int port, int preview_fps);
void http_stop(struct http_server *s);
/* A result as one line of JSON */
void http_publish(struct http_server *s, const char *json);
/* True when a viewer is due a preview frame */
int http_want_frame(struct http_server *s);
/* Copies a preview frame: [3][h][w] int8 planes, boxes in its pixels */
void http_put_frame(struct http_server *s, const int8_t *rgb, int w, int h,
		    const struct det_box *box, int n);

#endif
