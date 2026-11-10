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
 *   /api/focus      lens position; ?value=N moves it, ?auto runs autofocus,
 *                   &x=X&y=Y around that point of the preview (fractions)
 *   /api/exposure   exposure time, gain and white balance
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
/*
 * Preview and sensor frame sizes, and the frame point of a preview pixel:
 * frame = preview * scale + off. Needed for autofocus at a point.
 */
void http_set_geometry(struct http_server *s, int prev_w, int prev_h, int frame_w,
		       int frame_h, float scale_x, float off_x, float scale_y, float off_y);

#endif
