// SPDX-License-Identifier: AGPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/media.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#include "camera.h"

#define MAX_HOPS	8

struct topo {
	struct media_v2_topology t;
	struct media_v2_entity *ent;
	struct media_v2_interface *intf;
	struct media_v2_pad *pad;
	struct media_v2_link *link;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do
		ret = ioctl(fd, req, arg);
	while (ret < 0 && errno == EINTR);
	return ret;
}

/* Report a failed step from errno. EBUSY: another program streams. */
static void fail(const char *step, const char *name)
{
	int err = errno;

	fprintf(stderr, "camera: %s on %s: %s\n", step, name, strerror(err));
	if (err == EBUSY)
		fprintf(stderr, "camera: another program uses the camera "
			"(the service? systemctl stop duos-detect)\n");
}

static void topo_free(struct topo *tp)
{
	free(tp->ent);
	free(tp->intf);
	free(tp->pad);
	free(tp->link);
}

static int topo_read(int fd, struct topo *tp)
{
	memset(tp, 0, sizeof(*tp));
	if (xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &tp->t))
		return -1;
	tp->ent = calloc(tp->t.num_entities, sizeof(*tp->ent));
	tp->intf = calloc(tp->t.num_interfaces, sizeof(*tp->intf));
	tp->pad = calloc(tp->t.num_pads, sizeof(*tp->pad));
	tp->link = calloc(tp->t.num_links, sizeof(*tp->link));
	if (!tp->ent || !tp->intf || !tp->pad || !tp->link)
		return -1;
	tp->t.ptr_entities = (uintptr_t)tp->ent;
	tp->t.ptr_interfaces = (uintptr_t)tp->intf;
	tp->t.ptr_pads = (uintptr_t)tp->pad;
	tp->t.ptr_links = (uintptr_t)tp->link;
	return xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &tp->t);
}

static const struct media_v2_pad *pad_by_id(const struct topo *tp, uint32_t id)
{
	for (uint32_t i = 0; i < tp->t.num_pads; i++)
		if (tp->pad[i].id == id)
			return &tp->pad[i];
	return NULL;
}

static const struct media_v2_entity *ent_by_id(const struct topo *tp, uint32_t id)
{
	for (uint32_t i = 0; i < tp->t.num_entities; i++)
		if (tp->ent[i].id == id)
			return &tp->ent[i];
	return NULL;
}

/* /dev path of the entity's device node (subdev or video), from sysfs */
static int devnode(const struct topo *tp, uint32_t ent, char *path, size_t n)
{
	for (uint32_t i = 0; i < tp->t.num_links; i++) {
		const struct media_v2_link *l = &tp->link[i];
		char sys[96], line[128];
		FILE *f;

		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_INTERFACE_LINK ||
		    l->sink_id != ent)
			continue;
		for (uint32_t j = 0; j < tp->t.num_interfaces; j++) {
			if (tp->intf[j].id != l->source_id)
				continue;
			snprintf(sys, sizeof(sys), "/sys/dev/char/%u:%u/uevent",
				 tp->intf[j].devnode.major, tp->intf[j].devnode.minor);
			f = fopen(sys, "r");
			if (!f)
				return -1;
			while (fgets(line, sizeof(line), f))
				if (!strncmp(line, "DEVNAME=", 8)) {
					line[strcspn(line, "\n")] = 0;
					snprintf(path, n, "/dev/%.48s", line + 8);
					fclose(f);
					return 0;
				}
			fclose(f);
		}
	}
	return -1;
}

/* The enabled data link leaving the entity: sink pad on the next one */
static const struct media_v2_pad *next_sink(const struct topo *tp, uint32_t ent)
{
	for (uint32_t i = 0; i < tp->t.num_links; i++) {
		const struct media_v2_link *l = &tp->link[i];
		const struct media_v2_pad *src;

		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK ||
		    !(l->flags & MEDIA_LNK_FL_ENABLED))
			continue;
		src = pad_by_id(tp, l->source_id);
		if (src && src->entity_id == ent)
			return pad_by_id(tp, l->sink_id);
	}
	return NULL;
}

static int ctrl_get(int fd, uint32_t id, int *v)
{
	struct v4l2_control c = { .id = id };

	if (xioctl(fd, VIDIOC_G_CTRL, &c))
		return -1;
	*v = c.value;
	return 0;
}

static int ctrl_set(int fd, uint32_t id, int v)
{
	struct v4l2_control c = { .id = id, .value = v };

	return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int64_t pixel_rate(int fd)
{
	struct v4l2_ext_control c = { .id = V4L2_CID_PIXEL_RATE };
	struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL,
					.count = 1, .controls = &c };

	return xioctl(fd, VIDIOC_G_EXT_CTRLS, &cs) ? -1 : c.value64;
}

/* Capture format of the VI for a Bayer bus code: packed RAW12 */
static uint32_t pixfmt(uint32_t code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR8_1X8: case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SBGGR12_1X12:
		return V4L2_PIX_FMT_SBGGR12P;
	case MEDIA_BUS_FMT_SGBRG8_1X8: case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
		return V4L2_PIX_FMT_SGBRG12P;
	case MEDIA_BUS_FMT_SGRBG8_1X8: case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
		return V4L2_PIX_FMT_SGRBG12P;
	case MEDIA_BUS_FMT_SRGGB8_1X8: case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		return V4L2_PIX_FMT_SRGGB12P;
	}
	return 0;
}

/*
 * The sensor mode for a detector input: its 4x4-binned height must cover
 * the input; then the width closest to the input (cropped or padded), then
 * the smaller mode.
 */
static int pick_mode(int fd, uint32_t code, int in_w, int in_h, int *w, int *h)
{
	long best = -1;

	for (uint32_t i = 0;; i++) {
		struct v4l2_subdev_frame_size_enum fse = {
			.index = i, .code = code, .which = V4L2_SUBDEV_FORMAT_ACTIVE,
		};
		long cost;

		if (xioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &fse))
			break;
		if ((int)fse.max_height / 4 < in_h)
			continue;
		cost = labs((long)fse.max_width / 4 - in_w) * 100000000L +
		       (long)fse.max_width * fse.max_height;
		if (best < 0 || cost < best) {
			best = cost;
			*w = fse.max_width;
			*h = fse.max_height;
		}
	}
	return best < 0 ? -1 : 0;
}

static int set_fmt(int fd, uint32_t pad, uint32_t code, int w, int h,
		   struct v4l2_mbus_framefmt *out)
{
	struct v4l2_subdev_format f = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE, .pad = pad,
		.format = { .code = code, .width = w, .height = h,
			    .field = V4L2_FIELD_NONE },
	};

	if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &f))
		return -1;
	if (out)
		*out = f.format;
	return 0;
}

static int setup(int mfd, const struct cam_setup *s, struct cam_info *info)
{
	struct topo tp;
	const struct media_v2_entity *sensor = NULL;
	struct v4l2_subdev_format cur = { .which = V4L2_SUBDEV_FORMAT_ACTIVE };
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_format vf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	char path[64];
	int sfd = -1, ret = -1, rot = 0, w = s->width, h = s->height, hb, vb;
	uint32_t ent;
	int64_t rate;

	if (topo_read(mfd, &tp))
		goto out;
	for (uint32_t i = 0; i < tp.t.num_entities && !sensor; i++)
		if (tp.ent[i].function == MEDIA_ENT_F_CAM_SENSOR)
			sensor = &tp.ent[i];
	if (!sensor || devnode(&tp, sensor->id, path, sizeof(path))) {
		fprintf(stderr, "camera: no sensor in the media graph\n");
		goto out;
	}
	snprintf(info->sensor, sizeof(info->sensor), "%s", sensor->name);
	sfd = open(path, O_RDWR | O_CLOEXEC);
	if (sfd < 0) {
		perror(path);
		goto out;
	}

	/* Mounted upside down (the Duo S overlay says rotation 180): flip */
	if (!ctrl_get(sfd, V4L2_CID_CAMERA_SENSOR_ROTATION, &rot)) {
		info->flipped = rot == 180;
		ctrl_set(sfd, V4L2_CID_HFLIP, info->flipped);
		ctrl_set(sfd, V4L2_CID_VFLIP, info->flipped);
	}
	ctrl_set(sfd, V4L2_CID_TEST_PATTERN, 0);	/* live image */

	/* The bus code follows the flips: read it back */
	if (xioctl(sfd, VIDIOC_SUBDEV_G_FMT, &cur)) {
		fail("format", sensor->name);
		goto out;
	}
	if (!w && pick_mode(sfd, cur.format.code, s->in_w, s->in_h, &w, &h)) {
		fprintf(stderr, "camera: no mode covers a %dx%d input\n", s->in_w, s->in_h);
		goto out;
	}
	if (set_fmt(sfd, 0, cur.format.code, w, h, &fmt)) {
		fail("format", sensor->name);
		goto out;
	}

	/* The same format on every sink pad down to the capture node */
	for (ent = sensor->id; ; ) {
		const struct media_v2_pad *sink = next_sink(&tp, ent);
		const struct media_v2_entity *e = sink ? ent_by_id(&tp, sink->entity_id) : NULL;

		if (!e) {
			fprintf(stderr, "camera: no capture node after %s\n",
				ent_by_id(&tp, ent)->name);
			goto out;
		}
		if (devnode(&tp, e->id, path, sizeof(path)))
			goto out;
		if (e->function == MEDIA_ENT_F_IO_V4L) {
			snprintf(info->video, sizeof(info->video), "%s", path);
			break;
		}
		{
			int fd = open(path, O_RDWR | O_CLOEXEC);

			if (fd < 0 || set_fmt(fd, sink->index, fmt.code,
					      fmt.width, fmt.height, NULL)) {
				fail("format", e->name);
				if (fd >= 0)
					close(fd);
				goto out;
			}
			close(fd);
		}
		ent = e->id;
	}
	{
		int vfd = open(info->video, O_RDWR | O_CLOEXEC);

		vf.fmt.pix.width = fmt.width;
		vf.fmt.pix.height = fmt.height;
		vf.fmt.pix.pixelformat = pixfmt(fmt.code);
		vf.fmt.pix.field = V4L2_FIELD_NONE;
		if (!vf.fmt.pix.pixelformat)
			errno = EINVAL;
		if (vfd < 0 || !vf.fmt.pix.pixelformat || xioctl(vfd, VIDIOC_S_FMT, &vf)) {
			fail("format", info->video);
			if (vfd >= 0)
				close(vfd);
			goto out;
		}
		close(vfd);
	}
	info->width = fmt.width;
	info->height = fmt.height;

	/* Frame rate: pixel rate / (line length x (height + vertical blanking)) */
	rate = pixel_rate(sfd);
	if (rate > 0 && !ctrl_get(sfd, V4L2_CID_HBLANK, &hb)) {
		struct v4l2_queryctrl q = { .id = V4L2_CID_VBLANK };
		double line = fmt.width + hb;

		if (!xioctl(sfd, VIDIOC_QUERYCTRL, &q)) {
			vb = s->fps > 0 ? (int)lround(rate / (line * s->fps)) - (int)fmt.height
					: q.minimum;
			vb = vb < q.minimum ? q.minimum : vb > q.maximum ? q.maximum : vb;
			ctrl_set(sfd, V4L2_CID_VBLANK, vb);
		}
		if (!ctrl_get(sfd, V4L2_CID_VBLANK, &vb))
			info->fps = rate / (line * (fmt.height + vb));
	}
	ret = 0;
out:
	if (sfd >= 0)
		close(sfd);
	topo_free(&tp);
	return ret;
}

int camera_setup(const struct cam_setup *s, struct cam_info *info)
{
	memset(info, 0, sizeof(*info));
	for (int i = 0; i < 16; i++) {
		struct media_device_info di;
		char path[32];
		int fd, ret;

		snprintf(path, sizeof(path), "/dev/media%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (xioctl(fd, MEDIA_IOC_DEVICE_INFO, &di) ||
		    strncmp(di.driver, "cv1800-vi", sizeof(di.driver))) {
			close(fd);
			continue;
		}
		ret = setup(fd, s, info);
		close(fd);
		return ret;
	}
	fprintf(stderr, "camera: no cv1800-vi media device\n");
	return -1;
}
