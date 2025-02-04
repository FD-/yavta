/*
 * yavta --  Yet Another V4L2 Test Application
 *
 * Copyright (C) 2005-2010 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 */

#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <linux/videodev2.h>

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "bcm_host.h"
#include "user-vcsm.h"

#ifndef V4L2_BUF_FLAG_ERROR
#define V4L2_BUF_FLAG_ERROR	0x0040
#endif

#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))

int debug = 1;
#define print(...) do { if (debug) printf(__VA_ARGS__); }  while (0)

enum buffer_fill_mode
{
	BUFFER_FILL_NONE = 0,
	BUFFER_FILL_FRAME = 1 << 0,
	BUFFER_FILL_PADDING = 1 << 1,
};

struct buffer
{
	unsigned int idx;
	unsigned int padding[VIDEO_MAX_PLANES];
	unsigned int size[VIDEO_MAX_PLANES];
	void *mem[VIDEO_MAX_PLANES];
	MMAL_BUFFER_HEADER_T *mmal;
	int dma_fd;
	unsigned int vcsm_handle;
};

struct device
{
	int fd;
	int opened;

	enum v4l2_buf_type type;
	enum v4l2_memory memtype;
	unsigned int nbufs;
	struct buffer *buffers;

	MMAL_COMPONENT_T *isp;
	MMAL_COMPONENT_T *render;
	MMAL_COMPONENT_T *encoder;
	MMAL_POOL_T *isp_output_pool;
	MMAL_POOL_T *render_pool;
	MMAL_POOL_T *encode_pool;

	MMAL_BOOL_T can_zero_copy;

	/* V4L2 to MMAL interface */
	MMAL_QUEUE_T *isp_queue;
	MMAL_POOL_T *mmal_pool;
	/* Encoded data */
	MMAL_POOL_T *output_pool;



	unsigned int width;
	unsigned int height;
	unsigned int fps;
	unsigned int frame_time_usec;
	uint32_t buffer_output_flags;
	uint32_t timestamp_type;
	struct timeval starttime;
	int64_t lastpts;

	unsigned char num_planes;
	struct v4l2_plane_pix_format plane_fmt[VIDEO_MAX_PLANES];

	void *pattern[VIDEO_MAX_PLANES];
	unsigned int patternsize[VIDEO_MAX_PLANES];

	bool write_data_prefix;


	VCOS_THREAD_T save_thread;
	MMAL_QUEUE_T *save_queue;
	int thread_quit;
	FILE *h264_fd;
	FILE *pts_fd;
};

static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        exit(EXIT_FAILURE);
}

static bool video_is_mplane(struct device *dev)
{
	return dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	       dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static bool video_is_capture(struct device *dev)
{
	return dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	       dev->type == V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

static bool video_is_output(struct device *dev)
{
	return dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	       dev->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

static struct {
	enum v4l2_buf_type type;
	bool supported;
	const char *name;
	const char *string;
} buf_types[] = {
	{ V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1, "Video capture mplanes", "capture-mplane", },
	{ V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1, "Video output", "output-mplane", },
	{ V4L2_BUF_TYPE_VIDEO_CAPTURE, 1, "Video capture", "capture", },
	{ V4L2_BUF_TYPE_VIDEO_OUTPUT, 1, "Video output mplanes", "output", },
	{ V4L2_BUF_TYPE_VIDEO_OVERLAY, 0, "Video overlay", "overlay" },
};

static int v4l2_buf_type_from_string(const char *str)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(buf_types); i++) {
		if (!buf_types[i].supported)
			continue;

		if (strcmp(buf_types[i].string, str))
			continue;

		return buf_types[i].type;
	}

	return -1;
}

static const char *v4l2_buf_type_name(enum v4l2_buf_type type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(buf_types); ++i) {
		if (buf_types[i].type == type)
			return buf_types[i].name;
	}

	if (type & V4L2_BUF_TYPE_PRIVATE)
		return "Private";
	else
		return "Unknown";
}

#define MMAL_ENCODING_UNUSED 0

//MIPI packed monochrome images
#ifndef MMAL_ENCODING_GREY
#define MMAL_ENCODING_GREY    MMAL_FOURCC('G', 'R', 'E', 'Y') //8bpp Greyscale
#endif
#ifndef MMAL_ENCODING_Y10P
#define MMAL_ENCODING_Y10P    MMAL_FOURCC('Y', '1', '0', 'P') //10bpp  Greyscale, MIPI RAW10 packed
#endif
#ifndef MMAL_ENCODING_Y12P
#define MMAL_ENCODING_Y12P    MMAL_FOURCC('Y', '1', '2', 'P') //12bpp  Greyscale, MIPI RAW12 packed
#endif
#ifndef MMAL_ENCODING_Y14P
#define MMAL_ENCODING_Y14P    MMAL_FOURCC('Y', '1', '4', 'P') //14bpp  Greyscale, MIPI RAW14 packed
#endif
#ifndef MMAL_ENCODING_Y16
#define MMAL_ENCODING_Y16     MMAL_FOURCC('Y', '1', '6', ' ') //16bpp  Greyscale
#endif


/* Grey bit-packed formats */
#ifndef V4L2_PIX_FMT_Y10P
#define V4L2_PIX_FMT_Y10P    v4l2_fourcc('Y', '1', '0', 'P') /* 10  Greyscale, MIPI RAW10 packed */
#endif
#ifndef V4L2_PIX_FMT_Y12P
#define V4L2_PIX_FMT_Y12P    v4l2_fourcc('Y', '1', '2', 'P') /* 12  Greyscale, MIPI RAW12 packed */
#endif
#ifndef V4L2_PIX_FMT_Y14P
#define V4L2_PIX_FMT_Y14P    v4l2_fourcc('Y', '1', '4', 'P') /* 14  Greyscale, MIPI RAW12 packed */
#endif

#ifndef V4L2_PIX_FMT_SBGGR12P
#define V4L2_PIX_FMT_SBGGR12P v4l2_fourcc('p', 'B', 'C', 'C')
#endif
#ifndef V4L2_PIX_FMT_SGBRG12P
#define V4L2_PIX_FMT_SGBRG12P v4l2_fourcc('p', 'G', 'C', 'C')
#endif
#ifndef V4L2_PIX_FMT_SGRBG12P
#define V4L2_PIX_FMT_SGRBG12P v4l2_fourcc('p', 'g', 'C', 'C')
#endif
#ifndef V4L2_PIX_FMT_SRGGB12P
#define V4L2_PIX_FMT_SRGGB12P v4l2_fourcc('p', 'R', 'C', 'C')
#endif

static struct v4l2_format_info {
	const char *name;
	unsigned int fourcc;
	unsigned char n_planes;
	MMAL_FOURCC_T mmal_encoding;
} pixel_formats[] = {
	{ "RGB332", V4L2_PIX_FMT_RGB332, 1, 	MMAL_ENCODING_UNUSED },
	{ "RGB444", V4L2_PIX_FMT_RGB444, 1,	MMAL_ENCODING_UNUSED },
	{ "ARGB444", V4L2_PIX_FMT_ARGB444, 1,	MMAL_ENCODING_UNUSED },
	{ "XRGB444", V4L2_PIX_FMT_XRGB444, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB555", V4L2_PIX_FMT_RGB555, 1,	MMAL_ENCODING_UNUSED },
	{ "ARGB555", V4L2_PIX_FMT_ARGB555, 1,	MMAL_ENCODING_UNUSED },
	{ "XRGB555", V4L2_PIX_FMT_XRGB555, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB565", V4L2_PIX_FMT_RGB565, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB555X", V4L2_PIX_FMT_RGB555X, 1,	MMAL_ENCODING_UNUSED },
	{ "RGB565X", V4L2_PIX_FMT_RGB565X, 1,	MMAL_ENCODING_RGB16 },
	{ "BGR666", V4L2_PIX_FMT_BGR666, 1,	MMAL_ENCODING_UNUSED },
	{ "BGR24", V4L2_PIX_FMT_BGR24, 1,	MMAL_ENCODING_RGB24 },
	{ "RGB24", V4L2_PIX_FMT_RGB24, 1,	MMAL_ENCODING_BGR24 },
	{ "BGR32", V4L2_PIX_FMT_BGR32, 1,	MMAL_ENCODING_BGR32 },
	{ "ABGR32", V4L2_PIX_FMT_ABGR32, 1,	MMAL_ENCODING_BGRA },
	{ "XBGR32", V4L2_PIX_FMT_XBGR32, 1,	MMAL_ENCODING_BGR32 },
	{ "RGB32", V4L2_PIX_FMT_RGB32, 1,	MMAL_ENCODING_RGB32 },
	{ "ARGB32", V4L2_PIX_FMT_ARGB32, 1,	MMAL_ENCODING_ARGB },
	{ "XRGB32", V4L2_PIX_FMT_XRGB32, 1,	MMAL_ENCODING_UNUSED },
	{ "HSV24", V4L2_PIX_FMT_HSV24, 1,	MMAL_ENCODING_UNUSED },
	{ "HSV32", V4L2_PIX_FMT_HSV32, 1,	MMAL_ENCODING_UNUSED },
	{ "Y8", V4L2_PIX_FMT_GREY, 1,		MMAL_ENCODING_GREY },
	{ "Y10", V4L2_PIX_FMT_Y10, 1,		MMAL_ENCODING_UNUSED },
	{ "Y12", V4L2_PIX_FMT_Y12, 1,		MMAL_ENCODING_UNUSED },
	{ "Y16", V4L2_PIX_FMT_Y16, 1,		MMAL_ENCODING_UNUSED },
	{ "UYVY", V4L2_PIX_FMT_UYVY, 1,		MMAL_ENCODING_UYVY },
	{ "VYUY", V4L2_PIX_FMT_VYUY, 1,		MMAL_ENCODING_VYUY },
	{ "YUYV", V4L2_PIX_FMT_YUYV, 1,		MMAL_ENCODING_YUYV },
	{ "YVYU", V4L2_PIX_FMT_YVYU, 1,		MMAL_ENCODING_YVYU },
	{ "NV12", V4L2_PIX_FMT_NV12, 1,		MMAL_ENCODING_NV12 },
	{ "NV12M", V4L2_PIX_FMT_NV12M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV21", V4L2_PIX_FMT_NV21, 1,		MMAL_ENCODING_NV21 },
	{ "NV21M", V4L2_PIX_FMT_NV21M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV16", V4L2_PIX_FMT_NV16, 1,		MMAL_ENCODING_UNUSED },
	{ "NV16M", V4L2_PIX_FMT_NV16M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV61", V4L2_PIX_FMT_NV61, 1,		MMAL_ENCODING_UNUSED },
	{ "NV61M", V4L2_PIX_FMT_NV61M, 2,	MMAL_ENCODING_UNUSED },
	{ "NV24", V4L2_PIX_FMT_NV24, 1,		MMAL_ENCODING_UNUSED },
	{ "NV42", V4L2_PIX_FMT_NV42, 1,		MMAL_ENCODING_UNUSED },
	{ "YUV420M", V4L2_PIX_FMT_YUV420M, 3,	MMAL_ENCODING_UNUSED },
	{ "YUV422M", V4L2_PIX_FMT_YUV422M, 3,	MMAL_ENCODING_UNUSED },
	{ "YUV444M", V4L2_PIX_FMT_YUV444M, 3,	MMAL_ENCODING_UNUSED },
	{ "YVU420M", V4L2_PIX_FMT_YVU420M, 3,	MMAL_ENCODING_UNUSED },
	{ "YVU422M", V4L2_PIX_FMT_YVU422M, 3,	MMAL_ENCODING_UNUSED },
	{ "YVU444M", V4L2_PIX_FMT_YVU444M, 3,	MMAL_ENCODING_UNUSED },
	{ "SBGGR8", V4L2_PIX_FMT_SBGGR8, 1,	MMAL_ENCODING_BAYER_SBGGR8 },
	{ "SGBRG8", V4L2_PIX_FMT_SGBRG8, 1,	MMAL_ENCODING_BAYER_SGBRG8 },
	{ "SGRBG8", V4L2_PIX_FMT_SGRBG8, 1,	MMAL_ENCODING_BAYER_SGRBG8 },
	{ "SRGGB8", V4L2_PIX_FMT_SRGGB8, 1,	MMAL_ENCODING_BAYER_SRGGB8 },
	{ "SBGGR10_DPCM8", V4L2_PIX_FMT_SBGGR10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SGBRG10_DPCM8", V4L2_PIX_FMT_SGBRG10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SGRBG10_DPCM8", V4L2_PIX_FMT_SGRBG10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SRGGB10_DPCM8", V4L2_PIX_FMT_SRGGB10DPCM8, 1,	MMAL_ENCODING_UNUSED },
	{ "SBGGR10", V4L2_PIX_FMT_SBGGR10, 1,	MMAL_ENCODING_UNUSED },
	{ "SGBRG10", V4L2_PIX_FMT_SGBRG10, 1,	MMAL_ENCODING_UNUSED },
	{ "SGRBG10", V4L2_PIX_FMT_SGRBG10, 1,	MMAL_ENCODING_UNUSED },
	{ "SRGGB10", V4L2_PIX_FMT_SRGGB10, 1,	MMAL_ENCODING_UNUSED },
	{ "SBGGR10P", V4L2_PIX_FMT_SBGGR10P, 1,	MMAL_ENCODING_BAYER_SBGGR10P },
	{ "SGBRG10P", V4L2_PIX_FMT_SGBRG10P, 1,	MMAL_ENCODING_BAYER_SGBRG10P },
	{ "SGRBG10P", V4L2_PIX_FMT_SGRBG10P, 1,	MMAL_ENCODING_BAYER_SGRBG10P },
	{ "SRGGB10P", V4L2_PIX_FMT_SRGGB10P, 1,	MMAL_ENCODING_BAYER_SRGGB10P },
	{ "SBGGR12", V4L2_PIX_FMT_SBGGR12, 1,	MMAL_ENCODING_UNUSED },
	{ "SGBRG12", V4L2_PIX_FMT_SGBRG12, 1,	MMAL_ENCODING_UNUSED },
	{ "SGRBG12", V4L2_PIX_FMT_SGRBG12, 1,	MMAL_ENCODING_UNUSED },
	{ "SRGGB12", V4L2_PIX_FMT_SRGGB12, 1,	MMAL_ENCODING_UNUSED },
	{ "SBGGR12P", V4L2_PIX_FMT_SBGGR12P, 1,	MMAL_ENCODING_BAYER_SBGGR12P },
	{ "SGBRG12P", V4L2_PIX_FMT_SGBRG12P, 1,	MMAL_ENCODING_BAYER_SGBRG12P },
	{ "SGRBG12P", V4L2_PIX_FMT_SGRBG12P, 1,	MMAL_ENCODING_BAYER_SGRBG12P },
	{ "SRGGB12P", V4L2_PIX_FMT_SRGGB12P, 1,	MMAL_ENCODING_BAYER_SRGGB12P },
	{ "DV", V4L2_PIX_FMT_DV, 1,		MMAL_ENCODING_UNUSED },
	{ "MJPEG", V4L2_PIX_FMT_MJPEG, 1,	MMAL_ENCODING_UNUSED },
	{ "MPEG", V4L2_PIX_FMT_MPEG, 1,		MMAL_ENCODING_UNUSED },
	{ "Y10P", V4L2_PIX_FMT_Y10P, 1,		MMAL_ENCODING_Y10P },
	{ "Y12P", V4L2_PIX_FMT_Y12P, 1,		MMAL_ENCODING_Y12P },
	{ "Y14P", V4L2_PIX_FMT_Y14P, 1,		MMAL_ENCODING_Y14P },
};

static void list_formats(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); i++)
		print("%s (\"%c%c%c%c\", %u planes)\n",
		       pixel_formats[i].name,
		       pixel_formats[i].fourcc & 0xff,
		       (pixel_formats[i].fourcc >> 8) & 0xff,
		       (pixel_formats[i].fourcc >> 16) & 0xff,
		       (pixel_formats[i].fourcc >> 24) & 0xff,
		       pixel_formats[i].n_planes);
}

static const struct v4l2_format_info *v4l2_format_by_fourcc(unsigned int fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (pixel_formats[i].fourcc == fourcc)
			return &pixel_formats[i];
	}

	return NULL;
}

static const struct v4l2_format_info *v4l2_format_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pixel_formats); ++i) {
		if (strcasecmp(pixel_formats[i].name, name) == 0)
			return &pixel_formats[i];
	}

	return NULL;
}

static const char *v4l2_format_name(unsigned int fourcc)
{
	const struct v4l2_format_info *info;
	static char name[5];
	unsigned int i;

	info = v4l2_format_by_fourcc(fourcc);
	if (info)
		return info->name;

	for (i = 0; i < 4; ++i) {
		name[i] = fourcc & 0xff;
		fourcc >>= 8;
	}

	name[4] = '\0';
	return name;
}

static const struct {
	const char *name;
	enum v4l2_field field;
} fields[] = {
	{ "any", V4L2_FIELD_ANY },
	{ "none", V4L2_FIELD_NONE },
	{ "top", V4L2_FIELD_TOP },
	{ "bottom", V4L2_FIELD_BOTTOM },
	{ "interlaced", V4L2_FIELD_INTERLACED },
	{ "seq-tb", V4L2_FIELD_SEQ_TB },
	{ "seq-bt", V4L2_FIELD_SEQ_BT },
	{ "alternate", V4L2_FIELD_ALTERNATE },
	{ "interlaced-tb", V4L2_FIELD_INTERLACED_TB },
	{ "interlaced-bt", V4L2_FIELD_INTERLACED_BT },
};

static enum v4l2_field v4l2_field_from_string(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fields); ++i) {
		if (strcasecmp(fields[i].name, name) == 0)
			return fields[i].field;
	}

	return -1;
}

static const char *v4l2_field_name(enum v4l2_field field)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fields); ++i) {
		if (fields[i].field == field)
			return fields[i].name;
	}

	return "unknown";
}

static void video_set_buf_type(struct device *dev, enum v4l2_buf_type type)
{
	dev->type = type;
}

static bool video_has_valid_buf_type(struct device *dev)
{
	return (int)dev->type != -1;
}

static void video_init(struct device *dev)
{
	memset(dev, 0, sizeof *dev);
	dev->fd = -1;
	dev->memtype = V4L2_MEMORY_MMAP;
	dev->buffers = NULL;
	dev->type = (enum v4l2_buf_type)-1;
}

static bool video_has_fd(struct device *dev)
{
	return dev->fd != -1;
}

static int video_set_fd(struct device *dev, int fd)
{
	if (video_has_fd(dev)) {
		print("Can't set fd (already open).\n");
		return -1;
	}

	dev->fd = fd;

	return 0;
}

static int video_open(struct device *dev, const char *devname)
{
	if (video_has_fd(dev)) {
		print("Can't open device (already open).\n");
		return -1;
	}

	dev->fd = open(devname, O_RDWR);
	if (dev->fd < 0) {
		print("Error opening device %s: %s (%d).\n", devname,
		       strerror(errno), errno);
		return dev->fd;
	}

	print("Device %s opened.\n", devname);

	dev->opened = 1;

	return 0;
}

static int video_querycap(struct device *dev, unsigned int *capabilities)
{
	struct v4l2_capability cap;
	unsigned int caps;
	int ret;

	memset(&cap, 0, sizeof cap);
	ret = ioctl(dev->fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0)
		return 0;

	caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
	     ? cap.device_caps : cap.capabilities;

	print("Device `%s' on `%s' (driver '%s') is a video %s (%s mplanes) device.\n",
		cap.card, cap.bus_info, cap.driver,
		caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_CAPTURE) ? "capture" : "output",
		caps & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE) ? "with" : "without");

	*capabilities = caps;

	return 0;
}

static int cap_get_buf_type(unsigned int capabilities)
{
	if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
		return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	} else if (capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) {
		return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	} else if (capabilities & V4L2_CAP_VIDEO_CAPTURE) {
		return  V4L2_BUF_TYPE_VIDEO_CAPTURE;
	} else if (capabilities & V4L2_CAP_VIDEO_OUTPUT) {
		return V4L2_BUF_TYPE_VIDEO_OUTPUT;
	} else {
		print("Device supports neither capture nor output.\n");
		return -EINVAL;
	}

	return 0;
}

static void video_close(struct device *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_planes; i++)
		free(dev->pattern[i]);

	free(dev->buffers);
	if (dev->opened)
		close(dev->fd);
}

static void video_log_status(struct device *dev)
{
	ioctl(dev->fd, VIDIOC_LOG_STATUS);
}

static int query_control(struct device *dev, unsigned int id,
			 struct v4l2_queryctrl *query)
{
	int ret;

	memset(query, 0, sizeof(*query));
	query->id = id;

	ret = ioctl(dev->fd, VIDIOC_QUERYCTRL, query);
	if (ret < 0 && errno != EINVAL)
		print("unable to query control 0x%8.8x: %s (%d).\n",
		       id, strerror(errno), errno);

	return ret;
}

static int get_control(struct device *dev, const struct v4l2_queryctrl *query,
		       struct v4l2_ext_control *ctrl)
{
	struct v4l2_ext_controls ctrls;
	int ret;

	memset(&ctrls, 0, sizeof(ctrls));
	memset(ctrl, 0, sizeof(*ctrl));

	ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(query->id);
	ctrls.count = 1;
	ctrls.controls = ctrl;

	ctrl->id = query->id;

	if (query->type == V4L2_CTRL_TYPE_STRING) {
		ctrl->string = malloc(query->maximum + 1);
		if (ctrl->string == NULL)
			return -ENOMEM;

		ctrl->size = query->maximum + 1;
	}

	ret = ioctl(dev->fd, VIDIOC_G_EXT_CTRLS, &ctrls);
	if (ret != -1)
		return 0;

	if (query->type != V4L2_CTRL_TYPE_INTEGER64 &&
	    query->type != V4L2_CTRL_TYPE_STRING &&
	    (errno == EINVAL || errno == ENOTTY)) {
		struct v4l2_control old;

		old.id = query->id;
		ret = ioctl(dev->fd, VIDIOC_G_CTRL, &old);
		if (ret != -1) {
			ctrl->value = old.value;
			return 0;
		}
	}

	print("unable to get control 0x%8.8x: %s (%d).\n",
		query->id, strerror(errno), errno);
	return -1;
}

static void set_control(struct device *dev, unsigned int id,
		        int64_t val)
{
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control ctrl;
	struct v4l2_queryctrl query;
	int64_t old_val = val;
	int is_64;
	int ret;

	ret = query_control(dev, id, &query);
	if (ret < 0)
		return;

	is_64 = query.type == V4L2_CTRL_TYPE_INTEGER64;

	memset(&ctrls, 0, sizeof(ctrls));
	memset(&ctrl, 0, sizeof(ctrl));

	ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(id);
	ctrls.count = 1;
	ctrls.controls = &ctrl;

	ctrl.id = id;
	if (is_64)
		ctrl.value64 = val;
	else
		ctrl.value = val;

	ret = ioctl(dev->fd, VIDIOC_S_EXT_CTRLS, &ctrls);
	if (ret != -1) {
		if (is_64)
			val = ctrl.value64;
		else
			val = ctrl.value;
	} else if (!is_64 && query.type != V4L2_CTRL_TYPE_STRING &&
		   (errno == EINVAL || errno == ENOTTY)) {
		struct v4l2_control old;

		old.id = id;
		old.value = val;
		ret = ioctl(dev->fd, VIDIOC_S_CTRL, &old);
		if (ret != -1)
			val = old.value;
	}
	if (ret == -1) {
		print("unable to set control 0x%8.8x: %s (%d).\n",
			id, strerror(errno), errno);
		return;
	}

	print("Control 0x%08x set to %" PRId64 ", is %" PRId64 "\n",
	       id, old_val, val);
}

static int video_get_format(struct device *dev)
{
	struct v4l2_format fmt;
	unsigned int i;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = dev->type;

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
		print("Unable to get format: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	if (video_is_mplane(dev)) {
		dev->width = fmt.fmt.pix_mp.width;
		dev->height = fmt.fmt.pix_mp.height;
		dev->num_planes = fmt.fmt.pix_mp.num_planes;

		print("Video format: %s (%08x) %ux%u field %s, %u planes: \n",
			v4l2_format_name(fmt.fmt.pix_mp.pixelformat), fmt.fmt.pix_mp.pixelformat,
			fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
			v4l2_field_name(fmt.fmt.pix_mp.field),
			fmt.fmt.pix_mp.num_planes);

		for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
			dev->plane_fmt[i].bytesperline =
					fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
			dev->plane_fmt[i].sizeimage =
					fmt.fmt.pix_mp.plane_fmt[i].bytesperline ?
						fmt.fmt.pix_mp.plane_fmt[i].sizeimage : 0;

			print(" * Stride %u, buffer size %u\n",
				fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
				fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
		}
	} else {
		dev->width = fmt.fmt.pix.width;
		dev->height = fmt.fmt.pix.height;
		dev->num_planes = 1;

		dev->plane_fmt[0].bytesperline = fmt.fmt.pix.bytesperline;
		dev->plane_fmt[0].sizeimage = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.sizeimage : 0;

		print("Video format: %s (%08x) %ux%u (stride %u) field %s buffer size %u\n",
			v4l2_format_name(fmt.fmt.pix.pixelformat), fmt.fmt.pix.pixelformat,
			fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
			v4l2_field_name(fmt.fmt.pix_mp.field),
			fmt.fmt.pix.sizeimage);
	}

	return 0;
}

static int format_bpp(__u32 pixfmt)
{
	switch(pixfmt)
	{
		case V4L2_PIX_FMT_BGR24:
		case V4L2_PIX_FMT_RGB24:
			return 4;
		case V4L2_PIX_FMT_YUYV:
		case V4L2_PIX_FMT_YVYU:
		case V4L2_PIX_FMT_UYVY:
		case V4L2_PIX_FMT_VYUY:
			return 2;
		case V4L2_PIX_FMT_SRGGB8:
		case V4L2_PIX_FMT_SBGGR8:
		case V4L2_PIX_FMT_SGRBG8:
		case V4L2_PIX_FMT_SGBRG8:
			return 1;
		default:
			return 1;
	}
}

static int video_set_format(struct device *dev, unsigned int w, unsigned int h,
			    unsigned int format, unsigned int stride,
			    unsigned int buffer_size, enum v4l2_field field,
			    unsigned int flags)
{
	struct v4l2_format fmt;
	unsigned int i;
	int ret;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = dev->type;

	if (video_is_mplane(dev)) {
		const struct v4l2_format_info *info = v4l2_format_by_fourcc(format);

		fmt.fmt.pix_mp.width = w;
		fmt.fmt.pix_mp.height = h;
		fmt.fmt.pix_mp.pixelformat = format;
		fmt.fmt.pix_mp.field = field;
		fmt.fmt.pix_mp.num_planes = info->n_planes;
		fmt.fmt.pix_mp.flags = flags;

		for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
			fmt.fmt.pix_mp.plane_fmt[i].bytesperline = stride;
			fmt.fmt.pix_mp.plane_fmt[i].sizeimage = buffer_size;
		}
	} else {
		fmt.fmt.pix.width = w;
		fmt.fmt.pix.height = h;
		fmt.fmt.pix.pixelformat = format;
		fmt.fmt.pix.field = field;
		print("stride is %d\n",stride);
		if (!stride)
			stride = ((w+31) &~31)*format_bpp(format);
		print("stride is now %d\n",stride);
		fmt.fmt.pix.bytesperline = stride;
		fmt.fmt.pix.sizeimage = buffer_size;
		fmt.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
		fmt.fmt.pix.flags = flags;
	}

	ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		print("Unable to set format: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	if (video_is_mplane(dev)) {
		print("Video format set: %s (%08x) %ux%u field %s, %u planes: \n",
			v4l2_format_name(fmt.fmt.pix_mp.pixelformat), fmt.fmt.pix_mp.pixelformat,
			fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
			v4l2_field_name(fmt.fmt.pix_mp.field),
			fmt.fmt.pix_mp.num_planes);

		for (i = 0; i < fmt.fmt.pix_mp.num_planes; i++) {
			print(" * Stride %u, buffer size %u\n",
				fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
				fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
		}
	} else {
		print("Video format set: %s (%08x) %ux%u (stride %u) field %s buffer size %u\n",
			v4l2_format_name(fmt.fmt.pix.pixelformat), fmt.fmt.pix.pixelformat,
			fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
			v4l2_field_name(fmt.fmt.pix.field),
			fmt.fmt.pix.sizeimage);
	}

	return 0;
}

static int video_set_framerate(struct device *dev, struct v4l2_fract *time_per_frame)
{
	struct v4l2_streamparm parm;
	int ret;

	memset(&parm, 0, sizeof parm);
	parm.type = dev->type;

	ret = ioctl(dev->fd, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		print("Unable to get frame rate: %s (%d).\n",
			strerror(errno), errno);
		return ret;
	}

	print("Current frame rate: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);

	print("Setting frame rate to: %u/%u\n",
		time_per_frame->numerator,
		time_per_frame->denominator);

	parm.parm.capture.timeperframe.numerator = time_per_frame->numerator;
	parm.parm.capture.timeperframe.denominator = time_per_frame->denominator;

	ret = ioctl(dev->fd, VIDIOC_S_PARM, &parm);
	if (ret < 0) {
		print("Unable to set frame rate: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	ret = ioctl(dev->fd, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		print("Unable to get frame rate: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	print("Frame rate set: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);
	return 0;
}

static int buffer_export(int v4l2fd, enum v4l2_buf_type bt, int index, int *dmafd, unsigned int *vcsm_hdl)
{
	struct v4l2_exportbuffer expbuf;
	unsigned int vcsm_handle;

	memset(&expbuf, 0, sizeof(expbuf));
	expbuf.type = bt;
	expbuf.index = index;
	if (ioctl(v4l2fd, VIDIOC_EXPBUF, &expbuf))
	{
		print("Failed to EXPBUF\n");
		return -1;
	}
	*dmafd = expbuf.fd;

	print("Importing DMABUF %d into VCSM...\n", expbuf.fd);
	vcsm_handle = vcsm_import_dmabuf(expbuf.fd, "V4L2 buf");
	if (vcsm_handle)
		print("...done. vcsm_handle %u\n", vcsm_handle);
	else
		print("...done. Failed\n");
	*vcsm_hdl = vcsm_handle;
	return vcsm_handle ? 0 : -1;
}

static int video_buffer_mmap(struct device *dev, struct buffer *buffer,
			     struct v4l2_buffer *v4l2buf)
{
	unsigned int length;
	unsigned int offset;
	unsigned int i;

	for (i = 0; i < dev->num_planes; i++) {
		if (video_is_mplane(dev)) {
			length = v4l2buf->m.planes[i].length;
			offset = v4l2buf->m.planes[i].m.mem_offset;
		} else {
			length = v4l2buf->length;
			offset = v4l2buf->m.offset;
		}

		buffer->mem[i] = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED,
				      dev->fd, offset);
		if (buffer->mem[i] == MAP_FAILED) {
			print("Unable to map buffer %u/%u: %s (%d)\n",
			       buffer->idx, i, strerror(errno), errno);
			return -1;
		}

		buffer->size[i] = length;
		buffer->padding[i] = 0;

		print("Buffer %u/%u mapped at address %p.\n",
		       buffer->idx, i, buffer->mem[i]);
	}

	return 0;
}

static int video_buffer_munmap(struct device *dev, struct buffer *buffer)
{
	unsigned int i;
	int ret;

	for (i = 0; i < dev->num_planes; i++) {
		ret = munmap(buffer->mem[i], buffer->size[i]);
		if (ret < 0) {
			print("Unable to unmap buffer %u/%u: %s (%d)\n",
			       buffer->idx, i, strerror(errno), errno);
		}

		buffer->mem[i] = NULL;
	}

	return 0;
}

static int video_buffer_alloc_userptr(struct device *dev, struct buffer *buffer,
				      struct v4l2_buffer *v4l2buf,
				      unsigned int offset, unsigned int padding)
{
	int page_size = getpagesize();
	unsigned int length;
	unsigned int i;
	int ret;

	for (i = 0; i < dev->num_planes; i++) {
		if (video_is_mplane(dev))
			length = v4l2buf->m.planes[i].length;
		else
			length = v4l2buf->length;

		ret = posix_memalign(&buffer->mem[i], page_size,
				     length + offset + padding);
		if (ret < 0) {
			print("Unable to allocate buffer %u/%u (%d)\n",
			       buffer->idx, i, ret);
			return -ENOMEM;
		}

		buffer->mem[i] += offset;
		buffer->size[i] = length;
		buffer->padding[i] = padding;

		print("Buffer %u/%u allocated at address %p.\n",
		       buffer->idx, i, buffer->mem[i]);
	}

	return 0;
}

static void video_buffer_free_userptr(struct device *dev, struct buffer *buffer)
{
	unsigned int i;

	for (i = 0; i < dev->num_planes; i++) {
		free(buffer->mem[i]);
		buffer->mem[i] = NULL;
	}
}

static void video_buffer_fill_userptr(struct device *dev, struct buffer *buffer,
				      struct v4l2_buffer *v4l2buf)
{
	unsigned int i;

	if (!video_is_mplane(dev)) {
		v4l2buf->m.userptr = (unsigned long)buffer->mem[0];
		return;
	}

	for (i = 0; i < dev->num_planes; i++)
		v4l2buf->m.planes[i].m.userptr = (unsigned long)buffer->mem[i];
}

static void get_ts_flags(uint32_t flags, const char **ts_type, const char **ts_source)
{
	switch (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) {
	case V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN:
		*ts_type = "unk";
		break;
	case V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC:
		*ts_type = "mono";
		break;
	case V4L2_BUF_FLAG_TIMESTAMP_COPY:
		*ts_type = "copy";
		break;
	default:
		*ts_type = "inv";
	}
	switch (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) {
	case V4L2_BUF_FLAG_TSTAMP_SRC_EOF:
		*ts_source = "EoF";
		break;
	case V4L2_BUF_FLAG_TSTAMP_SRC_SOE:
		*ts_source = "SoE";
		break;
	default:
		*ts_source = "inv";
	}
}

static int video_alloc_buffers(struct device *dev, int nbufs,
	unsigned int offset, unsigned int padding)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer buf;
	struct buffer *buffers;
	unsigned int i;
	int ret;

	memset(&rb, 0, sizeof rb);
	rb.count = nbufs;
	rb.type = dev->type;
	rb.memory = dev->memtype;

	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		print("Unable to request buffers: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	print("%u buffers requested, V4L2 returned %u bufs.\n", nbufs, rb.count);

	buffers = malloc(rb.count * sizeof buffers[0]);
	if (buffers == NULL)
		return -ENOMEM;

	/* Map the buffers. */
	for (i = 0; i < rb.count; ++i) {
		const char *ts_type, *ts_source;

		memset(&buf, 0, sizeof buf);
		memset(planes, 0, sizeof planes);

		buf.index = i;
		buf.type = dev->type;
		buf.memory = dev->memtype;
		buf.length = VIDEO_MAX_PLANES;
		buf.m.planes = planes;

		ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &buf);
		if (ret < 0) {
			print("Unable to query buffer %u: %s (%d).\n", i,
				strerror(errno), errno);
			return ret;
		}
		get_ts_flags(buf.flags, &ts_type, &ts_source);
		print("length: %u offset: %u timestamp type/source: %s/%s\n",
		       buf.length, buf.m.offset, ts_type, ts_source);

		buffers[i].idx = i;

		switch (dev->memtype) {
		case V4L2_MEMORY_MMAP:
			ret = video_buffer_mmap(dev, &buffers[i], &buf);
			break;

		case V4L2_MEMORY_USERPTR:
			ret = video_buffer_alloc_userptr(dev, &buffers[i], &buf, offset, padding);
			break;

		default:
			break;
		}

		if (ret < 0)
			return ret;

		if (!buffer_export(dev->fd, dev->type, i, &buffers[i].dma_fd, &buffers[i].vcsm_handle))
		{
			dev->can_zero_copy = MMAL_TRUE;
			print("Exported buffer %d to dmabuf %d, vcsm handle %u\n", i, buffers[i].dma_fd, buffers[i].vcsm_handle);
		}
		else
		{
			if (dev->can_zero_copy)
			{
				print("Some buffer exported whilst others not. HELP!\n");
				dev->can_zero_copy = MMAL_FALSE;
			}
		}
		if (dev->mmal_pool) {
			MMAL_BUFFER_HEADER_T *mmal_buf;
			mmal_buf = mmal_queue_get(dev->mmal_pool->queue);
			if (!mmal_buf) {
				print("Failed to get a buffer from the pool. Queue length %d\n", mmal_queue_length(dev->mmal_pool->queue));
				return -1;
			}
			mmal_buf->user_data = &buffers[i];

			if (dev->can_zero_copy)
				mmal_buf->data = (uint8_t*)vcsm_vc_hdl_from_hdl(buffers[i].vcsm_handle);
			else
				mmal_buf->data = buffers[i].mem[0];
			mmal_buf->alloc_size = buf.length;
			buffers[i].mmal = mmal_buf;
			print("Linking V4L2 buffer index %d ptr %p to MMAL header %p. mmal->data 0x%X\n",
				i, &buffers[i], mmal_buf, (uint32_t)mmal_buf->data);
			/* Put buffer back in the pool */
			mmal_buffer_header_release(mmal_buf);
		}
	}

	dev->timestamp_type = buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
	dev->buffers = buffers;
	dev->nbufs = rb.count;
	return 0;
}

static int video_free_buffers(struct device *dev)
{
	struct v4l2_requestbuffers rb;
	unsigned int i;
	int ret;

	if (dev->nbufs == 0)
		return 0;

	for (i = 0; i < dev->nbufs; ++i) {
		switch (dev->memtype) {
		case V4L2_MEMORY_MMAP:
			if (dev->buffers[i].vcsm_handle)
			{
				print("Releasing vcsm handle %u\n", dev->buffers[i].vcsm_handle);
				vcsm_free(dev->buffers[i].vcsm_handle);
			}
			if (dev->buffers[i].dma_fd)
			{
				print("Closing dma_buf %d\n", dev->buffers[i].dma_fd);
				close(dev->buffers[i].dma_fd);
			}
			ret = video_buffer_munmap(dev, &dev->buffers[i]);
			if (ret < 0)
				return ret;
			break;
		case V4L2_MEMORY_USERPTR:
			video_buffer_free_userptr(dev, &dev->buffers[i]);
			break;
		default:
			break;
		}
	}

	memset(&rb, 0, sizeof rb);
	rb.count = 0;
	rb.type = dev->type;
	rb.memory = dev->memtype;

	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
	if (ret < 0) {
		print("Unable to release buffers: %s (%d).\n",
			strerror(errno), errno);
		return ret;
	}

	print("%u buffers released.\n", dev->nbufs);

	free(dev->buffers);
	dev->nbufs = 0;
	dev->buffers = NULL;

	return 0;
}

static int video_queue_buffer(struct device *dev, int index, enum buffer_fill_mode fill)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int ret;
	unsigned int i;

	memset(&buf, 0, sizeof buf);
	memset(&planes, 0, sizeof planes);

	buf.index = index;
	buf.type = dev->type;
	buf.memory = dev->memtype;

	if (video_is_output(dev)) {
		buf.flags = dev->buffer_output_flags;
		if (dev->timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_COPY) {
			struct timespec ts;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			buf.timestamp.tv_sec = ts.tv_sec;
			buf.timestamp.tv_usec = ts.tv_nsec / 1000;
		}
	}

	if (video_is_mplane(dev)) {
		buf.m.planes = planes;
		buf.length = dev->num_planes;
	}

	if (dev->memtype == V4L2_MEMORY_USERPTR) {
		if (video_is_mplane(dev)) {
			for (i = 0; i < dev->num_planes; i++) {
				buf.m.planes[i].m.userptr = (unsigned long)
					dev->buffers[index].mem[i];
				buf.m.planes[i].length =
					dev->buffers[index].size[i];
			}
		} else {
			buf.m.userptr = (unsigned long)dev->buffers[index].mem[0];
			buf.length = dev->buffers[index].size[0];
		}
	}

	for (i = 0; i < dev->num_planes; i++) {
		if (video_is_output(dev)) {
			if (video_is_mplane(dev))
				buf.m.planes[i].bytesused = dev->patternsize[i];
			else
				buf.bytesused = dev->patternsize[i];

			memcpy(dev->buffers[buf.index].mem[i], dev->pattern[i],
			       dev->patternsize[i]);
		} else {
			if (fill & BUFFER_FILL_FRAME)
				memset(dev->buffers[buf.index].mem[i], 0x55,
				       dev->buffers[index].size[i]);
			if (fill & BUFFER_FILL_PADDING)
				memset(dev->buffers[buf.index].mem[i] +
					dev->buffers[index].size[i],
				       0x55, dev->buffers[index].padding[i]);
		}
	}

	ret = ioctl(dev->fd, VIDIOC_QBUF, &buf);
	if (ret < 0)
		print("Unable to queue buffer: %s (%d).\n",
			strerror(errno), errno);

	return ret;
}

static int video_enable(struct device *dev, int enable)
{
	int type = dev->type;
	int ret;

	ret = ioctl(dev->fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
	if (ret < 0) {
		print("Unable to %s streaming: %s (%d).\n",
			enable ? "start" : "stop", strerror(errno), errno);
		return ret;
	}

	return 0;
}

static void video_query_menu(struct device *dev, struct v4l2_queryctrl *query,
			     unsigned int value)
{
	struct v4l2_querymenu menu;
	int ret;

	for (menu.index = query->minimum;
	     menu.index <= (unsigned)query->maximum; menu.index++) {
		menu.id = query->id;
		ret = ioctl(dev->fd, VIDIOC_QUERYMENU, &menu);
		if (ret < 0)
			continue;

		if (query->type == V4L2_CTRL_TYPE_MENU)
			print("  %u: %.32s%s\n", menu.index, menu.name,
			       menu.index == value ? " (*)" : "");
		else
			print("  %u: %lld%s\n", menu.index, menu.value,
			       menu.index == value ? " (*)" : "");
	};
}

static int video_print_control(struct device *dev, unsigned int id, bool full)
{
	struct v4l2_ext_control ctrl;
	struct v4l2_queryctrl query;
	char sval[24];
	char *current = sval;
	int ret;

	ret = query_control(dev, id, &query);
	if (ret < 0)
		return ret;

	if (query.flags & V4L2_CTRL_FLAG_DISABLED)
		return query.id;

	if (query.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
		print("--- %s (class 0x%08x) ---\n", query.name, query.id);
		return query.id;
	}

	ret = get_control(dev, &query, &ctrl);
	if (ret < 0)
		strcpy(sval, "n/a");
	else if (query.type == V4L2_CTRL_TYPE_INTEGER64)
		sprintf(sval, "%lld", ctrl.value64);
	else if (query.type == V4L2_CTRL_TYPE_STRING)
		current = ctrl.string;
	else
		sprintf(sval, "%d", ctrl.value);

	if (full)
		print("control 0x%08x `%s' min %d max %d step %d default %d current %s.\n",
			query.id, query.name, query.minimum, query.maximum,
			query.step, query.default_value, current);
	else
		print("control 0x%08x current %s.\n", query.id, current);

	if (query.type == V4L2_CTRL_TYPE_STRING)
		free(ctrl.string);

	if (!full)
		return query.id;

	if (query.type == V4L2_CTRL_TYPE_MENU ||
	    query.type == V4L2_CTRL_TYPE_INTEGER_MENU)
		video_query_menu(dev, &query, ctrl.value);

	return query.id;
}

static void video_list_controls(struct device *dev)
{
	unsigned int nctrls = 0;
	unsigned int id;
	int ret;

#ifndef V4L2_CTRL_FLAG_NEXT_CTRL
	unsigned int i;

	for (i = V4L2_CID_BASE; i <= V4L2_CID_LASTP1; ++i) {
		id = i;
#else
	id = 0;
	while (1) {
		id |= V4L2_CTRL_FLAG_NEXT_CTRL;
#endif

		ret = video_print_control(dev, id, true);
		if (ret < 0)
			break;

		id = ret;
		nctrls++;
	}

	if (nctrls)
		print("%u control%s found.\n", nctrls, nctrls > 1 ? "s" : "");
	else
		print("No control found.\n");
}

static void video_enum_frame_intervals(struct device *dev, __u32 pixelformat,
	unsigned int width, unsigned int height)
{
	struct v4l2_frmivalenum ival;
	unsigned int i;
	int ret;

	for (i = 0; ; ++i) {
		memset(&ival, 0, sizeof ival);
		ival.index = i;
		ival.pixel_format = pixelformat;
		ival.width = width;
		ival.height = height;
		ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival);
		if (ret < 0)
			break;

		if (i != ival.index)
			print("Warning: driver returned wrong ival index "
				"%u.\n", ival.index);
		if (pixelformat != ival.pixel_format)
			print("Warning: driver returned wrong ival pixel "
				"format %08x.\n", ival.pixel_format);
		if (width != ival.width)
			print("Warning: driver returned wrong ival width "
				"%u.\n", ival.width);
		if (height != ival.height)
			print("Warning: driver returned wrong ival height "
				"%u.\n", ival.height);

		if (i != 0)
			print(", ");

		switch (ival.type) {
		case V4L2_FRMIVAL_TYPE_DISCRETE:
			print("%u/%u",
				ival.discrete.numerator,
				ival.discrete.denominator);
			break;

		case V4L2_FRMIVAL_TYPE_CONTINUOUS:
			print("%u/%u - %u/%u",
				ival.stepwise.min.numerator,
				ival.stepwise.min.denominator,
				ival.stepwise.max.numerator,
				ival.stepwise.max.denominator);
			return;

		case V4L2_FRMIVAL_TYPE_STEPWISE:
			print("%u/%u - %u/%u (by %u/%u)",
				ival.stepwise.min.numerator,
				ival.stepwise.min.denominator,
				ival.stepwise.max.numerator,
				ival.stepwise.max.denominator,
				ival.stepwise.step.numerator,
				ival.stepwise.step.denominator);
			return;

		default:
			break;
		}
	}
}

static void video_enum_frame_sizes(struct device *dev, __u32 pixelformat)
{
	struct v4l2_frmsizeenum frame;
	unsigned int i;
	int ret;

	for (i = 0; ; ++i) {
		memset(&frame, 0, sizeof frame);
		frame.index = i;
		frame.pixel_format = pixelformat;
		ret = ioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &frame);
		if (ret < 0)
			break;

		if (i != frame.index)
			print("Warning: driver returned wrong frame index "
				"%u.\n", frame.index);
		if (pixelformat != frame.pixel_format)
			print("Warning: driver returned wrong frame pixel "
				"format %08x.\n", frame.pixel_format);

		switch (frame.type) {
		case V4L2_FRMSIZE_TYPE_DISCRETE:
			print("\tFrame size: %ux%u (", frame.discrete.width,
				frame.discrete.height);
			video_enum_frame_intervals(dev, frame.pixel_format,
				frame.discrete.width, frame.discrete.height);
			print(")\n");
			break;

		case V4L2_FRMSIZE_TYPE_CONTINUOUS:
			print("\tFrame size: %ux%u - %ux%u (",
				frame.stepwise.min_width,
				frame.stepwise.min_height,
				frame.stepwise.max_width,
				frame.stepwise.max_height);
			video_enum_frame_intervals(dev, frame.pixel_format,
				frame.stepwise.max_width,
				frame.stepwise.max_height);
			print(")\n");
			break;

		case V4L2_FRMSIZE_TYPE_STEPWISE:
			print("\tFrame size: %ux%u - %ux%u (by %ux%u) (\n",
				frame.stepwise.min_width,
				frame.stepwise.min_height,
				frame.stepwise.max_width,
				frame.stepwise.max_height,
				frame.stepwise.step_width,
				frame.stepwise.step_height);
			video_enum_frame_intervals(dev, frame.pixel_format,
				frame.stepwise.max_width,
				frame.stepwise.max_height);
			print(")\n");
			break;

		default:
			break;
		}
	}
}

static void video_enum_formats(struct device *dev, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc fmt;
	unsigned int i;
	int ret;

	for (i = 0; ; ++i) {
		memset(&fmt, 0, sizeof fmt);
		fmt.index = i;
		fmt.type = type;
		ret = ioctl(dev->fd, VIDIOC_ENUM_FMT, &fmt);
		if (ret < 0)
			break;

		if (i != fmt.index)
			print("Warning: driver returned wrong format index "
				"%u.\n", fmt.index);
		if (type != fmt.type)
			print("Warning: driver returned wrong format type "
				"%u.\n", fmt.type);

		print("\tFormat %u: %s (%08x)\n", i,
			v4l2_format_name(fmt.pixelformat), fmt.pixelformat);
		print("\tType: %s (%u)\n", v4l2_buf_type_name(fmt.type),
			fmt.type);
		print("\tName: %.32s\n", fmt.description);
		video_enum_frame_sizes(dev, fmt.pixelformat);
		print("\n");
	}
}

static void video_enum_inputs(struct device *dev)
{
	struct v4l2_input input;
	unsigned int i;
	int ret;

	for (i = 0; ; ++i) {
		memset(&input, 0, sizeof input);
		input.index = i;
		ret = ioctl(dev->fd, VIDIOC_ENUMINPUT, &input);
		if (ret < 0)
			break;

		if (i != input.index)
			print("Warning: driver returned wrong input index "
				"%u.\n", input.index);

		print("\tInput %u: %s.\n", i, input.name);
	}

	print("\n");
}

static int video_get_input(struct device *dev)
{
	__u32 input;
	int ret;

	ret = ioctl(dev->fd, VIDIOC_G_INPUT, &input);
	if (ret < 0) {
		print("Unable to get current input: %s (%d).\n",
			strerror(errno), errno);
		return ret;
	}

	return input;
}

static int video_set_input(struct device *dev, unsigned int input)
{
	__u32 _input = input;
	int ret;

	ret = ioctl(dev->fd, VIDIOC_S_INPUT, &_input);
	if (ret < 0)
		print("Unable to select input %u: %s (%d).\n", input,
			strerror(errno), errno);

	return ret;
}

static int video_set_quality(struct device *dev, unsigned int quality)
{
	struct v4l2_jpegcompression jpeg;
	int ret;

	if (quality == (unsigned int)-1)
		return 0;

	memset(&jpeg, 0, sizeof jpeg);
	jpeg.quality = quality;

	ret = ioctl(dev->fd, VIDIOC_S_JPEGCOMP, &jpeg);
	if (ret < 0) {
		print("Unable to set quality to %u: %s (%d).\n", quality,
			strerror(errno), errno);
		return ret;
	}

	ret = ioctl(dev->fd, VIDIOC_G_JPEGCOMP, &jpeg);
	if (ret >= 0)
		print("Quality set to %u\n", jpeg.quality);

	return 0;
}

static int video_load_test_pattern(struct device *dev, const char *filename)
{
	unsigned int plane;
	unsigned int size;
	int fd = -1;
	int ret;

	if (filename != NULL) {
		fd = open(filename, O_RDONLY);
		if (fd == -1) {
			print("Unable to open test pattern file '%s': %s (%d).\n",
				filename, strerror(errno), errno);
			return -errno;
		}
	}

	/* Load or generate the test pattern */
	for (plane = 0; plane < dev->num_planes; plane++) {
		size = dev->buffers[0].size[plane];
		dev->pattern[plane] = malloc(size);
		if (dev->pattern[plane] == NULL) {
			ret = -ENOMEM;
			goto done;
		}

		if (filename != NULL) {
			ret = read(fd, dev->pattern[plane], size);
			if (ret != (int)size && dev->plane_fmt[plane].bytesperline != 0) {
				print("Test pattern file size %u doesn't match image size %u\n",
					ret, size);
				ret = -EINVAL;
				goto done;
			}
		} else {
			uint8_t *data = dev->pattern[plane];
			unsigned int i;

			if (dev->plane_fmt[plane].bytesperline == 0) {
				print("Compressed format detected for plane %u and no test pattern filename given.\n"
					"The test pattern can't be generated automatically.\n", plane);
				ret = -EINVAL;
				goto done;
			}

			for (i = 0; i < dev->plane_fmt[plane].sizeimage; ++i)
				*data++ = i;
		}

		dev->patternsize[plane] = size;
	}

	ret = 0;

done:
	if (fd != -1)
		close(fd);

	return ret;
}

static int video_prepare_capture(struct device *dev, int nbufs, unsigned int offset,
				 const char *filename, enum buffer_fill_mode fill)
{
	unsigned int padding;
	int ret;

	/* Allocate and map buffers. */
	padding = (fill & BUFFER_FILL_PADDING) ? 4096 : 0;
	if ((ret = video_alloc_buffers(dev, nbufs, offset, padding)) < 0)
		return ret;

	if (video_is_output(dev)) {
		ret = video_load_test_pattern(dev, filename);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int video_queue_all_buffers(struct device *dev, enum buffer_fill_mode fill)
{
	unsigned int i;
	int ret;

	/* Queue the buffers. */
	for (i = 0; i < dev->nbufs; ++i) {
		ret = video_queue_buffer(dev, i, fill);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void video_verify_buffer(struct device *dev, struct v4l2_buffer *buf)
{
	struct buffer *buffer = &dev->buffers[buf->index];
	unsigned int plane;
	unsigned int i;

	for (plane = 0; plane < dev->num_planes; ++plane) {
		const uint8_t *data = buffer->mem[plane] + buffer->size[plane];
		unsigned int errors = 0;
		unsigned int dirty = 0;

		if (buffer->padding[plane] == 0)
			continue;

		for (i = 0; i < buffer->padding[plane]; ++i) {
			if (data[i] != 0x55) {
				errors++;
				dirty = i + 1;
			}
		}

		if (errors) {
			print("Warning: %u bytes overwritten among %u first padding bytes for plane %u\n",
			       errors, dirty, plane);

			dirty = (dirty + 15) & ~15;
			dirty = dirty > 32 ? 32 : dirty;

			for (i = 0; i < dirty; ++i) {
				print("%02x ", data[i]);
				if (i % 16 == 15)
					print("\n");
			}
		}
	}
}

static void isp_ip_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct device *dev = (struct device *)port->userdata;
	unsigned int i;
//	print("Buffer %p (->data %p) returned\n", buffer, buffer->data);
	for (i = 0; i < dev->nbufs; i++) {
		if (dev->buffers[i].mmal == buffer) {
//			print("Matches V4L2 buffer index %d / %d\n", i, dev->buffers[i].idx);
			video_queue_buffer(dev, dev->buffers[i].idx, BUFFER_FILL_NONE);
			mmal_buffer_header_release(buffer);
			buffer = NULL;
			break;
		}
	}
	if (buffer) {
		print("Failed to find matching V4L2 buffer for mmal buffer %p\n", buffer);
		mmal_buffer_header_release(buffer);
	}
}

static void * save_thread(void *arg)
{
	struct device *dev = (struct device *)arg;
	MMAL_BUFFER_HEADER_T *buffer;
	MMAL_STATUS_T status;
	unsigned int bytes_written;

	while (!dev->thread_quit)
	{
		//Being lazy and using a timed wait instead of setting up a
		//mechanism for skipping this when destroying the thread
		buffer = mmal_queue_timedwait(dev->save_queue, 500);
		if (!buffer)
			continue;

		//print("Buffer %p saving, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);
		if (dev->h264_fd)
		{
			bytes_written = fwrite(buffer->data, 1, buffer->length, dev->h264_fd);
			fflush(dev->h264_fd);

			if (bytes_written != buffer->length)
			{
				print("Failed to write buffer data (%d from %d)- aborting", bytes_written, buffer->length);
			}
		}
		else
		{
			print("No file to save to\n");
		}
		if (dev->pts_fd &&
		    !(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) &&
		    buffer->pts != MMAL_TIME_UNKNOWN)
			fprintf(dev->pts_fd, "%lld.%03lld\n", buffer->pts/1000, buffer->pts%1000);

		buffer->length = 0;
		status = mmal_port_send_buffer(dev->encoder->output[0], buffer);
		if(status != MMAL_SUCCESS)
		{
			print("mmal_port_send_buffer failed on buffer %p, status %d", buffer, status);
		}
	}
	return NULL;
}

static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct device *dev = (struct device *)port->userdata;

	//print("Buffer %p returned, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);
	//vcos_log_error("File handle: %p", port->userdata);

	if (port->is_enabled)
		mmal_queue_put(dev->save_queue, buffer);
	else
		mmal_buffer_header_release(buffer);
}

static void buffers_to_isp(struct device *dev)
{
	MMAL_BUFFER_HEADER_T *buffer;

	while ((buffer = mmal_queue_get(dev->isp_output_pool->queue)) != NULL)
	{
		mmal_port_send_buffer(dev->isp->output[0], buffer);
	}

}
static void isp_output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	//print("Buffer %p from isp, filled %d, timestamp %llu, flags %04X\n", buffer, buffer->length, buffer->pts, buffer->flags);
	//vcos_log_error("File handle: %p", port->userdata);
	struct device *dev = (struct device*)port->userdata;

	if (dev->render)
	{
		MMAL_BUFFER_HEADER_T *out = mmal_queue_get(dev->render_pool->queue);
		if (out)
		{
			mmal_buffer_header_replicate(out, buffer);
			mmal_port_send_buffer(dev->render->input[0], out);
		}
	}
	if (dev->encoder)
	{
		MMAL_BUFFER_HEADER_T *out = mmal_queue_get(dev->encode_pool->queue);
		if (out)
		{
			mmal_buffer_header_replicate(out, buffer);
			mmal_port_send_buffer(dev->encoder->input[0], out);
		}
	}
	mmal_buffer_header_release(buffer);

	buffers_to_isp(dev);
}

static void render_encoder_input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	//print("Buffer %p returned from %s, filled %d, timestamp %llu, flags %04X\n", buffer, port->name, buffer->length, buffer->pts, buffer->flags);
	//vcos_log_error("File handle: %p", port->userdata);
	struct device *dev = (struct device*)port->userdata;

	mmal_buffer_header_release(buffer);

	buffers_to_isp(dev);
}

#define LOG_DEBUG print

static void dump_port_format(MMAL_ES_FORMAT_T *format)
{
   const char *name_type;

   if (!format)
      return;

   switch(format->type)
   {
   case MMAL_ES_TYPE_AUDIO: name_type = "audio"; break;
   case MMAL_ES_TYPE_VIDEO: name_type = "video"; break;
   case MMAL_ES_TYPE_SUBPICTURE: name_type = "subpicture"; break;
   default: name_type = "unknown"; break;
   }

   LOG_DEBUG("type: %s, fourcc: %4.4s", name_type, (char *)&format->encoding);
   LOG_DEBUG(" bitrate: %i, framed: %i", format->bitrate,
            !!(format->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
   LOG_DEBUG(" extra data: %i, %p", format->extradata_size, format->extradata);
   switch(format->type)
   {
   case MMAL_ES_TYPE_AUDIO:
      LOG_DEBUG(" samplerate: %i, channels: %i, bps: %i, block align: %i\n",
               format->es->audio.sample_rate, format->es->audio.channels,
               format->es->audio.bits_per_sample, format->es->audio.block_align);
      break;

   case MMAL_ES_TYPE_VIDEO:
      LOG_DEBUG(" width: %i, height: %i, (%i,%i,%i,%i)",
               format->es->video.width, format->es->video.height,
               format->es->video.crop.x, format->es->video.crop.y,
               format->es->video.crop.width, format->es->video.crop.height);
      LOG_DEBUG(" pixel aspect ratio: %i/%i, frame rate: %i/%i\n",
               format->es->video.par.num, format->es->video.par.den,
               format->es->video.frame_rate.num, format->es->video.frame_rate.den);
      break;

   case MMAL_ES_TYPE_SUBPICTURE:
      break;

   default: break;
   }
}

void mmal_log_dump_port(MMAL_PORT_T *port)
{
   if (!port)
      return;

   LOG_DEBUG("%s(%p)", port->name, port);

   dump_port_format(port->format);

   LOG_DEBUG("buffers num: %i(opt %i, min %i), size: %i(opt %i, min: %i), align: %i\n",
            port->buffer_num, port->buffer_num_recommended, port->buffer_num_min,
            port->buffer_size, port->buffer_size_recommended, port->buffer_size_min,
            port->buffer_alignment_min);
}

int video_set_dv_timings(struct device *dev);

static void handle_event(struct device *dev)
{
        struct v4l2_event ev;

        while (!ioctl(dev->fd, VIDIOC_DQEVENT, &ev)) {
            switch (ev.type) {
            case V4L2_EVENT_SOURCE_CHANGE:
                fprintf(stderr, "Source changed\n");

		video_set_dv_timings(dev);
//                stop_capture(V4L2_BUF_TYPE_VIDEO_CAPTURE);
//                unmap_buffers(buffers, n_buffers);

                fprintf(stderr, "Unmapped all buffers\n");
//                free_buffers_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE);

//                init_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, &buffers, &n_buffers);

//                start_capturing_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE, buffers, n_buffers);
                break;
            case V4L2_EVENT_EOS:
                fprintf(stderr, "EOS\n");
                break;
            }
        }
}

static int setup_mmal(struct device *dev, int nbufs, int do_encode, const char *filename)
{
	MMAL_STATUS_T status;
	VCOS_STATUS_T vcos_status;
	MMAL_PORT_T *port;
	const struct v4l2_format_info *info;
	struct v4l2_format fmt;
	int ret;
	MMAL_PORT_T *isp_output, *encoder_input = NULL, *encoder_output = NULL;

	//FIXME: Clean up after errors

	status = mmal_component_create("vc.ril.isp", &dev->isp);
	if(status != MMAL_SUCCESS)
	{
		print("Failed to create isp\n");
		return -1;
	}

	if (do_encode)
	{
		status = mmal_component_create("vc.ril.video_encode", &dev->encoder);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to create encoder");
			return -1;
		}
		encoder_input = dev->encoder->input[0];
		encoder_output = dev->encoder->output[0];
	}

	if (1)
	{
		status = mmal_component_create("vc.ril.video_render", &dev->render);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to create render\n");
			return -1;
		}
	}

	port = dev->isp->input[0];

	memset(&fmt, 0, sizeof fmt);
	fmt.type = dev->type;

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0) {
		print("Unable to get format: %s (%d).\n", strerror(errno),
			errno);
		return ret;
	}

	info = v4l2_format_by_fourcc(fmt.fmt.pix.pixelformat);
	if (!info || info->mmal_encoding == MMAL_ENCODING_UNUSED)
	{
		print("Unsupported encoding\n");
		return -1;
	}
	port->format->encoding = info->mmal_encoding;
	port->format->es->video.crop.width = fmt.fmt.pix.width;
	port->format->es->video.crop.height = fmt.fmt.pix.height;
	port->format->es->video.width = (port->format->es->video.crop.width+31) & ~31;
	//mmal_encoding_stride_to_width(port->format->encoding, fmt.fmt.pix.bytesperline);
	//The ISP doesn't care about padding on the height, but other components may.
	port->format->es->video.height = fmt.fmt.pix.height;
	//Ignore for now, but will be wanted for video encode.
	//port->format->es->video.frame_rate.num = 10000;
	//port->format->es->video.frame_rate.den = frame_interval ? frame_interval : 10000;
	port->buffer_num = nbufs;
	if (dev->fps) {
		dev->frame_time_usec = 1000000/dev->fps;
	}

	status = mmal_port_format_commit(port);
	if (status != MMAL_SUCCESS)
	{
		print("Commit failed\n");
		return -1;
	}
	mmal_log_dump_port(port);

	unsigned int mmal_stride = mmal_encoding_width_to_stride(info->mmal_encoding, port->format->es->video.width);
	if (mmal_stride != fmt.fmt.pix.bytesperline) {
		if (video_set_format(dev, fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat, mmal_stride,
				     fmt.fmt.pix.sizeimage, fmt.fmt.pix.field, fmt.fmt.pix.flags) < 0) 
			print("Failed to adjust stride\n");
		else
			// Retrieve settings again so local state is correct
			video_get_format(dev);
	}

	dev->mmal_pool = mmal_pool_create(nbufs, 0);
	if (!dev->mmal_pool) {
		print("Failed to create pool\n");
		return -1;
	}
	print("Created pool of length %d, size %d\n", nbufs, 0);

	port->userdata = (struct MMAL_PORT_USERDATA_T *)dev;

	mmal_format_copy(dev->isp->output[0]->format, port->format);
	port = dev->isp->output[0];
	port->format->encoding = MMAL_ENCODING_I420;
	while(port->format->es->video.crop.width > 1920)
	{
		port->format->es->video.crop.width >>= 1;
		port->format->es->video.crop.height >>= 1;
	}
	port->buffer_num = 3;

	status = mmal_port_format_commit(port);
	if (status != MMAL_SUCCESS)
	{
		print("ISP o/p commit failed\n");
		return -1;
	}

	isp_output = dev->isp->output[0];

	if (dev->render)
	{
		status = mmal_format_full_copy(dev->render->input[0]->format, isp_output->format);
		dev->render->input[0]->buffer_num = 3;
		if (status == MMAL_SUCCESS)
			status = mmal_port_format_commit(dev->render->input[0]);
	}

	//  Encoder setup
	if (dev->encoder)
	{
		status = mmal_format_full_copy(encoder_input->format, isp_output->format);
		encoder_input->buffer_num = 3;
		if (status == MMAL_SUCCESS)
			status = mmal_port_format_commit(encoder_input);

		// Only supporting H264 at the moment
		encoder_output->format->encoding = MMAL_ENCODING_H264;

		encoder_output->format->bitrate = 5000000;
		encoder_output->buffer_size = 256<<10;//encoder_output->buffer_size_recommended;

		if (encoder_output->buffer_size < encoder_output->buffer_size_min)
			encoder_output->buffer_size = encoder_output->buffer_size_min;

		encoder_output->buffer_num = 8; //encoder_output->buffer_num_recommended;

		if (encoder_output->buffer_num < encoder_output->buffer_num_min)
			encoder_output->buffer_num = encoder_output->buffer_num_min;

		// We need to set the frame rate on output to 0, to ensure it gets
		// updated correctly from the input framerate when port connected
		encoder_output->format->es->video.frame_rate.num = 0;
		encoder_output->format->es->video.frame_rate.den = 1;

		// Commit the port changes to the output port
		status = mmal_port_format_commit(encoder_output);

		if (status != MMAL_SUCCESS)
		{
			print("Unable to set format on encoder output port\n");
		}

		{
			MMAL_PARAMETER_VIDEO_PROFILE_T  param;
			param.hdr.id = MMAL_PARAMETER_PROFILE;
			param.hdr.size = sizeof(param);

			param.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;//state->profile;
			param.profile[0].level = MMAL_VIDEO_LEVEL_H264_4;

			status = mmal_port_parameter_set(encoder_output, &param.hdr);
			if (status != MMAL_SUCCESS)
			{
				print("Unable to set H264 profile\n");
			}
		}

		if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, 1) != MMAL_SUCCESS)
		{
			print("Unable to set immutable input flag\n");
			// Continue rather than abort..
		}

		//set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
		if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, 1) != MMAL_SUCCESS)
		{
			print("failed to set INLINE HEADER FLAG parameters\n");
			// Continue rather than abort..
		}

		//set INLINE VECTORS flag to request motion vector estimates
		if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, 0) != MMAL_SUCCESS)
		{
			print("failed to set INLINE VECTORS parameters\n");
			// Continue rather than abort..
		}

		if (status != MMAL_SUCCESS)
		{
			print("Unable to set format on video encoder input port\n");
		}

		print("Enable encoder....\n");
		status = mmal_component_enable(dev->encoder);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to enable\n");
			return -1;
		}
		status = mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
		status += mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to set zero copy\n");
			return -1;
		}
		encoder_input->userdata = (struct MMAL_PORT_USERDATA_T *)dev;
	}

	status = mmal_port_parameter_set_boolean(isp_output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);

	isp_output->userdata = (struct MMAL_PORT_USERDATA_T *)dev;

	if (dev->render)
	{
		status += mmal_port_parameter_set_boolean(dev->render->input[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
		if (status != MMAL_SUCCESS)
		{
			return -1;
		}

		dev->render->input[0]->userdata = (struct MMAL_PORT_USERDATA_T *)dev;

		status = mmal_port_enable(dev->render->input[0], render_encoder_input_callback);
		if (status != MMAL_SUCCESS)
			return -1;

		print("Create pool of %d buffers of size %d for render\n", dev->render->input[0]->buffer_num, 0);
		dev->render_pool = mmal_port_pool_create(dev->render->input[0], dev->render->input[0]->buffer_num, dev->render->input[0]->buffer_size);
		if(!dev->render_pool)
		{
			print("Failed to create render pool\n");
			return -1;
		}
	}

	if (dev->encoder)
	{
		status = mmal_port_enable(encoder_input, render_encoder_input_callback);
		if (status != MMAL_SUCCESS)
			return -1;

		print("Create pool of %d buffers of size %d for encode ip\n", encoder_input->buffer_num, 0);
		dev->encode_pool = mmal_port_pool_create(encoder_input, isp_output->buffer_num, isp_output->buffer_size);
		if(!dev->encode_pool)
		{
			print("Failed to create encode ip pool\n");
			return -1;
		}
	}

	status = mmal_port_enable(isp_output, isp_output_callback);
	if (status != MMAL_SUCCESS)
		return -1;

	print("Create pool of %d buffers of size %d for encode/render\n", isp_output->buffer_num, isp_output->buffer_size);
	dev->isp_output_pool = mmal_port_pool_create(isp_output, isp_output->buffer_num, isp_output->buffer_size);
	if(!dev->isp_output_pool)
	{
		print("Failed to create pool\n");
		return -1;
	}

	buffers_to_isp(dev);

	// open h264 file and put the file handle in userdata for the encoder output port
	if (dev->encoder)
	{
		if (filename[0] == '-' && filename[1] == '\0')
		{
			dev->h264_fd = stdout;
			debug = 0;
		}
		else {
			printf("Writing data to %s\n", filename);
			dev->h264_fd = fopen(filename, "wb");
		}

		dev->pts_fd = (void*)fopen("file.pts", "wb");
		if (dev->pts_fd) /* save header for mkvmerge */
			fprintf(dev->pts_fd, "# timecode format v2\n");

		encoder_output->userdata = (void*)dev;

		//Create encoder output buffers

		print("Create pool of %d buffers of size %d\n", encoder_output->buffer_num, encoder_output->buffer_size);
		dev->output_pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);
		if(!dev->output_pool)
		{
			print("Failed to create pool\n");
			return -1;
		}

		dev->save_queue = mmal_queue_create();
		if(!dev->save_queue)
		{
			print("Failed to create queue\n");
			return -1;
		}

		vcos_status = vcos_thread_create(&dev->save_thread, "save-thread",
					NULL, save_thread, dev);
		if(vcos_status != VCOS_SUCCESS)
		{
			print("Failed to create save thread\n");
			return -1;
		}

		status = mmal_port_enable(encoder_output, encoder_buffer_callback);
		if(status != MMAL_SUCCESS)
		{
			print("Failed to enable port\n");
			return -1;
		}

		unsigned int i;
		for(i=0; i<encoder_output->buffer_num; i++)
		{
			MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(dev->output_pool->queue);

			if (!buffer)
			{
				print("Where'd my buffer go?!\n");
				return -1;
			}
			status = mmal_port_send_buffer(encoder_output, buffer);
			if(status != MMAL_SUCCESS)
			{
				print("mmal_port_send_buffer failed on buffer %p, status %d\n", buffer, status);
				return -1;
			}
			print("Sent buffer %p\n", buffer);
		}
	}

	return 0;
}

static int enable_mmal_ip(struct device *dev)
{
	MMAL_STATUS_T status;

	if (mmal_port_parameter_set_boolean(dev->isp->input[0], MMAL_PARAMETER_ZERO_COPY, dev->can_zero_copy) != MMAL_SUCCESS)
	{
		print("Failed to set zero copy\n");
		return -1;
	}
	status = mmal_port_enable(dev->isp->input[0], isp_ip_cb);
	if (status != MMAL_SUCCESS)
	{
		print("ISP input enable failed\n");
		return -1;
	}
	return 0;
}

static void destroy_mmal(struct device *dev)
{
	//FIXME: Clean up everything properly
	dev->thread_quit = 1;
	vcos_thread_join(&dev->save_thread, NULL);
}

static void video_save_image(struct device *dev, struct v4l2_buffer *buf,
			     const char *pattern, unsigned int sequence)
{
	unsigned int size;
	unsigned int i;
	char *filename;
	const char *p;
	bool append;
	int ret = 0;
	int fd;

	size = strlen(pattern);
	filename = malloc(size + 12);
	if (filename == NULL)
		return;

	p = strchr(pattern, '#');
	if (p != NULL) {
		sprintf(filename, "%.*s%06u%s", (int)(p - pattern), pattern,
			sequence, p + 1);
		append = false;
	} else {
		strcpy(filename, pattern);
		append = true;
	}

	fd = open(filename, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC),
		  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	free(filename);
	if (fd == -1)
		return;

	for (i = 0; i < dev->num_planes; i++) {
		void *data = dev->buffers[buf->index].mem[i];
		unsigned int length;

		if (video_is_mplane(dev)) {
			length = buf->m.planes[i].bytesused;

			if (!dev->write_data_prefix) {
				data += buf->m.planes[i].data_offset;
				length -= buf->m.planes[i].data_offset;
			}

		} else {
			length = buf->bytesused;
		}

		ret = write(fd, data, length);
		if (ret < 0) {
			print("write error: %s (%d)\n", strerror(errno), errno);
			break;
		} else if (ret != (int)length)
			print("write error: only %d bytes written instead of %u\n",
			       ret, length);
	}
	close(fd);
}

unsigned int video_buffer_bytes_used(struct device *dev, struct v4l2_buffer *buf)
{
	unsigned int bytesused = 0;
	unsigned int i;

	if (!video_is_mplane(dev))
		return buf->bytesused;

	for (i = 0; i < dev->num_planes; i++)
		bytesused += buf->m.planes[i].bytesused;

	return bytesused;
}

static int video_do_capture(struct device *dev, unsigned int nframes,
	unsigned int skip, unsigned int delay, const char *pattern,
	int do_requeue_last, int do_queue_late, enum buffer_fill_mode fill)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer buf;
	struct timespec start;
	struct timeval last;
	struct timespec ts;
	unsigned int size;
	unsigned int i = 0;
	double bps;
	double fps;
	int ret;
	int dropped_frames = 0;

	/* Start streaming. */
	ret = video_enable(dev, 1);
	if (ret < 0)
		goto done;

	if (do_queue_late)
		video_queue_all_buffers(dev, fill);

	size = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	last.tv_sec = start.tv_sec;
	last.tv_usec = start.tv_nsec / 1000;

        while (i < nframes) {
                fd_set fds[3];
                fd_set *rd_fds = &fds[0]; /* for capture */
                fd_set *ex_fds = &fds[1]; /* for capture */
                fd_set *wr_fds = &fds[2]; /* for output */
                struct timeval tv;
                int r;

                if (rd_fds) {
                    FD_ZERO(rd_fds);
                    FD_SET(dev->fd, rd_fds);
                }

                if (ex_fds) {
                    FD_ZERO(ex_fds);
                    FD_SET(dev->fd, ex_fds);
                }

                if (wr_fds) {
                    FD_ZERO(wr_fds);
                    FD_SET(dev->fd, wr_fds);
                }

                /* Timeout. */
                tv.tv_sec = 10;
                tv.tv_usec = 0;

                r = select(dev->fd + 1, rd_fds, wr_fds, ex_fds, &tv);

                if (-1 == r) {
                        if (EINTR == errno)
                                continue;
                        errno_exit("select");
                }

                if (0 == r) {
                        fprintf(stderr, "select timeout\n");
                        exit(EXIT_FAILURE);
                }

                if (rd_fds && FD_ISSET(dev->fd, rd_fds)) {
			const char *ts_type, *ts_source;
			int queue_buffer = 1;
			/* Dequeue a buffer. */
			memset(&buf, 0, sizeof buf);
			memset(planes, 0, sizeof planes);

			buf.type = dev->type;
			buf.memory = dev->memtype;
			buf.length = VIDEO_MAX_PLANES;
			buf.m.planes = planes;

			ret = ioctl(dev->fd, VIDIOC_DQBUF, &buf);
			if (ret < 0) {
				if (errno != EIO) {
					print("Unable to dequeue buffer: %s (%d).\n",
						strerror(errno), errno);
					goto done;
				}
				buf.type = dev->type;
				buf.memory = dev->memtype;
				if (dev->memtype == V4L2_MEMORY_USERPTR)
					video_buffer_fill_userptr(dev, &dev->buffers[i], &buf);
			}

			if (video_is_capture(dev))
				video_verify_buffer(dev, &buf);
			//print("bytesused in buffer is %d\n", buf.bytesused);
			size += buf.bytesused;

			fps = (buf.timestamp.tv_sec - last.tv_sec) * 1000000
			    + buf.timestamp.tv_usec - last.tv_usec;
			fps = fps ? 1000000.0 / fps : 0.0;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			get_ts_flags(buf.flags, &ts_type, &ts_source);
			print("%u (%u) [%c] %s %u %u B %ld.%06ld %ld.%06ld %.3f fps ts %s/%s\n", i, buf.index,
				(buf.flags & V4L2_BUF_FLAG_ERROR) ? 'E' : '-',
				v4l2_field_name(buf.field),
				buf.sequence, video_buffer_bytes_used(dev, &buf),
				buf.timestamp.tv_sec, buf.timestamp.tv_usec,
				ts.tv_sec, ts.tv_nsec/1000, fps,
				ts_type, ts_source);

			last = buf.timestamp;

			/* Save the image. */
			if (video_is_capture(dev) && pattern && !skip)
				video_save_image(dev, &buf, pattern, i);

			if (dev->mmal_pool) {
				MMAL_BUFFER_HEADER_T *mmal;
				MMAL_STATUS_T status;
				while ((mmal = mmal_queue_get(dev->mmal_pool->queue)) && !mmal->user_data) {
					print("Discarding MMAL buffer %p as not mapped\n", mmal);
				}
				if (!mmal) {
					print("Failed to get MMAL buffer\n");
				} else {
					/* Need to wait for MMAL to be finished with the buffer before returning to V4L2 */
					queue_buffer = 0;
					if (((struct buffer*)mmal->user_data)->idx != buf.index) {
						print("Mismatch in expected buffers. V4L2 gave idx %d, MMAL expecting %d\n",
							buf.index, ((struct buffer*)mmal->user_data)->idx);
					}
					/*if (buf.bytesused != buf.length)
					{
						print("V4L2 buffer came back as shorter than allocated - length %u, bytesused %u\n",
						       buf.length, buf.bytesused);
					}*/
					mmal->length = buf.length;	//Deliberately use length as MMAL wants the padding

					if (!dev->starttime.tv_sec)
						dev->starttime = buf.timestamp;

					struct timeval pts;
					timersub(&buf.timestamp, &dev->starttime, &pts);
					//MMAL PTS is in usecs, so convert from struct timeval
					mmal->pts = (pts.tv_sec * 1000000) + pts.tv_usec;
					if (mmal->pts > (dev->lastpts+dev->frame_time_usec+1000)) {
						print("DROPPED FRAME - %lld and %lld, delta %lld\n", dev->lastpts, mmal->pts, mmal->pts-dev->lastpts);
						dropped_frames++;
					}
					dev->lastpts = mmal->pts;

					mmal->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END;
					//mmal->pts = buf.timestamp;
					status = mmal_port_send_buffer(dev->isp->input[0], mmal);
					if (status != MMAL_SUCCESS)
						print("mmal_port_send_buffer failed %d\n", status);
				}
			}

			if (skip)
				--skip;

			/* Requeue the buffer. */
			if (delay > 0)
				usleep(delay * 1000);

			fflush(stdout);

			i++;

			if (i >= nframes - dev->nbufs && !do_requeue_last)
				continue;
			if (!queue_buffer)
				continue;

			ret = video_queue_buffer(dev, buf.index, fill);
			if (ret < 0) {
				print("Unable to requeue buffer: %s (%d).\n",
					strerror(errno), errno);
				goto done;
			}
                }
                if (wr_fds && FD_ISSET(dev->fd, wr_fds)) {
                    fprintf(stderr, "Writing?!?!?\n");
                }
                if (ex_fds && FD_ISSET(dev->fd, ex_fds)) {
                    fprintf(stderr, "Exception\n");
                    handle_event(dev);
                }
                /* EAGAIN - continue select loop. */
        }


	/* Stop streaming. */
	ret = video_enable(dev, 0);
	if (ret < 0)
		return ret;

	if (nframes == 0) {
		print("No frames captured.\n");
		goto done;
	}

	if (ts.tv_sec == start.tv_sec && ts.tv_nsec == start.tv_nsec) {
		print("Captured %u frames (%u bytes) 0 seconds\n", i, size);
		goto done;
	}

	ts.tv_sec -= start.tv_sec;
	ts.tv_nsec -= start.tv_nsec;
	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += 1000000000;
	}

	bps = size/(ts.tv_nsec/1000.0+1000000.0*ts.tv_sec)*1000000.0;
	fps = i/(ts.tv_nsec/1000.0+1000000.0*ts.tv_sec)*1000000.0;

	print("Captured %u frames in %lu.%06lu seconds (%f fps, %f B/s).\n",
		i, ts.tv_sec, ts.tv_nsec/1000, fps, bps);
	print("Total number of frames dropped %d\n", dropped_frames);
done:
	return video_free_buffers(dev);
}

int video_set_dv_timings(struct device *dev)
{
	struct v4l2_dv_timings timings;
	v4l2_std_id std;
	int ret;

	memset(&timings, 0, sizeof timings);
	ret = ioctl(dev->fd, VIDIOC_QUERY_DV_TIMINGS, &timings);
	if (ret >= 0) {
		print("QUERY_DV_TIMINGS returned %ux%u pixclk %llu\n", timings.bt.width, timings.bt.height, timings.bt.pixelclock);
		//Can read DV timings, so set them.
		ret = ioctl(dev->fd, VIDIOC_S_DV_TIMINGS, &timings);
		if (ret < 0) {
			print("Failed to set DV timings\n");
			return -1;
		} else {
			double tot_height, tot_width;
			const struct v4l2_bt_timings *bt = &timings.bt;
			
			tot_height = bt->height +
				bt->vfrontporch + bt->vsync + bt->vbackporch +
				bt->il_vfrontporch + bt->il_vsync + bt->il_vbackporch;
			tot_width = bt->width +
				bt->hfrontporch + bt->hsync + bt->hbackporch;
			dev->fps = (unsigned int)((double)bt->pixelclock /
				(tot_width * tot_height));
			print("Framerate is %u\n", dev->fps);
		}
	} else {
		memset(&std, 0, sizeof std);
		ret = ioctl(dev->fd, VIDIOC_QUERYSTD, &std);
		if (ret >= 0) {
			//Can read standard, so set it.
			ret = ioctl(dev->fd, VIDIOC_S_STD, &std);
			if (ret < 0) {
				print("Failed to set standard\n");
				return -1;
			} else {
				// SD video - assume 50Hz / 25fps
				dev->fps = 25;
			}
		}
	}
	return 0;
}

int video_get_fps(struct device *dev)
{
	struct v4l2_streamparm parm;
	int ret;

	memset(&parm, 0, sizeof parm);
	parm.type = dev->type;

	ret = ioctl(dev->fd, VIDIOC_G_PARM, &parm);
	if (ret < 0) {
		print("Unable to get frame rate: %s (%d).\n",
			strerror(errno), errno);
		/* Make a wild guess at the frame rate */
		dev->fps = 15;
		return ret;
	}

	print("Current frame rate: %u/%u\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);

	dev->fps = parm.parm.capture.timeperframe.denominator/
			parm.parm.capture.timeperframe.numerator;

	return 0;
}

#define V4L_BUFFERS_DEFAULT	8
#define V4L_BUFFERS_MAX		32

static void usage(const char *argv0)
{
	print("Usage: %s [options] device\n", argv0);
	print("Supported options:\n");
	print("-B, --buffer-type		Buffer type (\"capture\", \"output\",\n");
	print("                                \"capture-mplane\" or \"output-mplane\")\n");
	print("-c, --capture[=nframes]		Capture frames\n");
	print("-C, --check-overrun		Verify dequeued frames for buffer overrun\n");
	print("-d, --delay			Delay (in ms) before requeuing buffers\n");
	print("-f, --format format		Set the video format\n");
	print("				use -f help to list the supported formats\n");
	print("-E, --encode-to [file]		Set filename to write to. Default of file.h264.\n");
	print("-F, --file[=name]		Read/write frames from/to disk\n");
	print("\tFor video capture devices, the first '#' character in the file name is\n");
	print("\texpanded to the frame sequence number. The default file name is\n");
	print("\t'frame-#.bin'.\n");
	print("-h, --help			Show this help screen\n");
	print("-i, --input input		Select the video input\n");
	print("-I, --fill-frames		Fill frames with check pattern before queuing them\n");
	print("-l, --list-controls		List available controls\n");
	print("-n, --nbufs n			Set the number of video buffers\n");
	print("-p, --pause			Pause before starting the video stream\n");
	print("-q, --quality n			MJPEG quality (0-100)\n");
	print("-r, --get-control ctrl		Get control 'ctrl'\n");
	print("-R, --realtime=[priority]	Enable realtime RR scheduling\n");
	print("-s, --size WxH			Set the frame size\n");
	print("-t, --time-per-frame num/denom	Set the time per frame (eg. 1/25 = 25 fps)\n");
	print("-T, --dv-timings		Query and set the DV timings\n");
	print("-u, --userptr			Use the user pointers streaming method\n");
	print("-w, --set-control 'ctrl value'	Set control 'ctrl' to 'value'\n");
	print("    --buffer-prefix		Write portions of buffer before data_offset\n");
	print("    --buffer-size		Buffer size in bytes\n");
	print("    --enum-formats		Enumerate formats\n");
	print("    --enum-inputs		Enumerate inputs\n");
	print("    --fd                        Use a numeric file descriptor insted of a device\n");
	print("    --field			Interlaced format field order\n");
	print("    --log-status		Log device status\n");
	print("    --no-query			Don't query capabilities on open\n");
	print("    --offset			User pointer buffer offset from page start\n");
	print("    --premultiplied		Color components are premultiplied by alpha value\n");
	print("    --queue-late		Queue buffers after streamon, not before\n");
	print("    --requeue-last		Requeue the last buffers before streamoff\n");
	print("    --timestamp-source		Set timestamp source on output buffers [eof, soe]\n");
	print("    --skip n			Skip the first n frames\n");
	print("    --sleep-forever		Sleep forever after configuring the device\n");
	print("    --stride value		Line stride in bytes\n");
	print("-m  --mmal			Enable MMAL rendering of images\n");
}

#define OPT_ENUM_FORMATS	256
#define OPT_ENUM_INPUTS		257
#define OPT_SKIP_FRAMES		258
#define OPT_NO_QUERY		259
#define OPT_SLEEP_FOREVER	260
#define OPT_USERPTR_OFFSET	261
#define OPT_REQUEUE_LAST	262
#define OPT_STRIDE		263
#define OPT_FD			264
#define OPT_TSTAMP_SRC		265
#define OPT_FIELD		266
#define OPT_LOG_STATUS		267
#define OPT_BUFFER_SIZE		268
#define OPT_PREMULTIPLIED	269
#define OPT_QUEUE_LATE		270
#define OPT_DATA_PREFIX		271

static struct option opts[] = {
	{"buffer-size", 1, 0, OPT_BUFFER_SIZE},
	{"buffer-type", 1, 0, 'B'},
	{"capture", 2, 0, 'c'},
	{"check-overrun", 0, 0, 'C'},
	{"data-prefix", 0, 0, OPT_DATA_PREFIX},
	{"delay", 1, 0, 'd'},
	{"encode-to", 1, 0, 'E'},
	{"enum-formats", 0, 0, OPT_ENUM_FORMATS},
	{"enum-inputs", 0, 0, OPT_ENUM_INPUTS},
	{"fd", 1, 0, OPT_FD},
	{"field", 1, 0, OPT_FIELD},
	{"file", 2, 0, 'F'},
	{"fill-frames", 0, 0, 'I'},
	{"format", 1, 0, 'f'},
	{"help", 0, 0, 'h'},
	{"input", 1, 0, 'i'},
	{"list-controls", 0, 0, 'l'},
	{"log-status", 0, 0, OPT_LOG_STATUS},
	{"mmal", 0, 0, 'm'},
	{"nbufs", 1, 0, 'n'},
	{"no-query", 0, 0, OPT_NO_QUERY},
	{"offset", 1, 0, OPT_USERPTR_OFFSET},
	{"pause", 0, 0, 'p'},
	{"premultiplied", 0, 0, OPT_PREMULTIPLIED},
	{"quality", 1, 0, 'q'},
	{"queue-late", 0, 0, OPT_QUEUE_LATE},
	{"get-control", 1, 0, 'r'},
	{"requeue-last", 0, 0, OPT_REQUEUE_LAST},
	{"realtime", 2, 0, 'R'},
	{"size", 1, 0, 's'},
	{"set-control", 1, 0, 'w'},
	{"skip", 1, 0, OPT_SKIP_FRAMES},
	{"sleep-forever", 0, 0, OPT_SLEEP_FOREVER},
	{"stride", 1, 0, OPT_STRIDE},
	{"time-per-frame", 1, 0, 't'},
	{"timestamp-source", 1, 0, OPT_TSTAMP_SRC},
	{"dv-timings", 0, 0, 'T'},
	{"userptr", 0, 0, 'u'},
	{0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
	struct sched_param sched;
	struct device dev = {0};
	int ret;

	/* Options parsings */
	const struct v4l2_format_info *info;
	/* Use video capture by default if query isn't done. */
	unsigned int capabilities = V4L2_CAP_VIDEO_CAPTURE;
	int do_file = 0, do_capture = 0, do_pause = 0;
	int do_set_time_per_frame = 0;
	int do_enum_formats = 0, do_set_format = 0;
	int do_enum_inputs = 0, do_set_input = 0;
	int do_list_controls = 0, do_get_control = 0, do_set_control = 0;
	int do_sleep_forever = 0, do_requeue_last = 0;
	int do_rt = 0, do_log_status = 0;
	int no_query = 0, do_queue_late = 0;
	int do_mmal_render = 0, do_encode = 0;
	int do_set_dv_timings = 0;
	char *endptr;
	int c;

	/* Controls */
	int ctrl_name = 0;
	int ctrl_value = 0;

	/* Video buffers */
	enum v4l2_memory memtype = V4L2_MEMORY_MMAP;
	unsigned int pixelformat = V4L2_PIX_FMT_YUYV;
	unsigned int fmt_flags = 0;
	unsigned int width = 640;
	unsigned int height = 480;
	unsigned int stride = 0;
	unsigned int buffer_size = 0;
	unsigned int nbufs = V4L_BUFFERS_DEFAULT;
	unsigned int input = 0;
	unsigned int skip = 0;
	unsigned int quality = (unsigned int)-1;
	unsigned int userptr_offset = 0;
	struct v4l2_fract time_per_frame = {1, 25};
	enum v4l2_field field = V4L2_FIELD_ANY;

	/* Capture loop */
	enum buffer_fill_mode fill_mode = BUFFER_FILL_NONE;
	unsigned int delay = 0, nframes = (unsigned int)-1;
	const char *filename = "frame-#.bin";
	const char *encode_filename = "file.h264";

	unsigned int rt_priority = 1;

	video_init(&dev);

	opterr = 0;
	while ((c = getopt_long(argc, argv, "B:c::Cd:E:f:F::hi:Ilmn:pq:r:R::s:t:Tuw:", opts, NULL)) != -1) {

		switch (c) {
		case 'B':
			ret = v4l2_buf_type_from_string(optarg);
			if (ret == -1) {
				print("Bad buffer type \"%s\"\n", optarg);
				return 1;
			}
			video_set_buf_type(&dev, ret);
			break;
		case 'c':
			do_capture = 1;
			if (optarg)
				nframes = atoi(optarg);
			break;
		case 'C':
			fill_mode |= BUFFER_FILL_PADDING;
			break;
		case 'd':
			delay = atoi(optarg);
			break;
		case 'E':
			print("We're encoding to %s\n", optarg);
			do_encode = 1;
			if (optarg)
				encode_filename = optarg;
			break;
		case 'f':
			if (!strcmp("help", optarg)) {
				list_formats();
				return 0;
			}
			do_set_format = 1;
			info = v4l2_format_by_name(optarg);
			if (info == NULL) {
				print("Unsupported video format '%s'\n", optarg);
				return 1;
			}
			pixelformat = info->fourcc;
			break;
		case 'F':
			do_file = 1;
			if (optarg)
				filename = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'i':
			do_set_input = 1;
			input = atoi(optarg);
			break;
		case 'I':
			fill_mode |= BUFFER_FILL_FRAME;
			break;
		case 'l':
			do_list_controls = 1;
			break;
		case 'm':
			do_mmal_render = 1;
			bcm_host_init();
			break;
		case 'n':
			nbufs = atoi(optarg);
			if (nbufs > V4L_BUFFERS_MAX)
				nbufs = V4L_BUFFERS_MAX;
			break;
		case 'p':
			do_pause = 1;
			break;
		case 'q':
			quality = atoi(optarg);
			break;
		case 'r':
			ctrl_name = strtol(optarg, &endptr, 0);
			if (*endptr != 0) {
				print("Invalid control name '%s'\n", optarg);
				return 1;
			}
			do_get_control = 1;
			break;
		case 'R':
			do_rt = 1;
			if (optarg)
				rt_priority = atoi(optarg);
			break;
		case 's':
			do_set_format = 1;
			width = strtol(optarg, &endptr, 10);
			if (*endptr != 'x' || endptr == optarg) {
				print("Invalid size '%s'\n", optarg);
				return 1;
			}
			height = strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				print("Invalid size '%s'\n", optarg);
				return 1;
			}
			break;
		case 't':
			do_set_time_per_frame = 1;
			time_per_frame.numerator = strtol(optarg, &endptr, 10);
			if (*endptr != '/' || endptr == optarg) {
				print("Invalid time per frame '%s'\n", optarg);
				return 1;
			}
			time_per_frame.denominator = strtol(endptr + 1, &endptr, 10);
			if (*endptr != 0) {
				print("Invalid time per frame '%s'\n", optarg);
				return 1;
			}
			break;
		case 'T':
			do_set_dv_timings = 1;
			break;
		case 'u':
			memtype = V4L2_MEMORY_USERPTR;
			break;
		case 'w':
			ctrl_name = strtol(optarg, &endptr, 0);
			if (*endptr != ' ' || endptr == optarg) {
				print("Invalid control name '%s'\n", optarg);
				return 1;
			}
			ctrl_value = strtol(endptr + 1, &endptr, 0);
			if (*endptr != 0) {
				print("Invalid control value '%s'\n", optarg);
				return 1;
			}
			do_set_control = 1;
			break;
		case OPT_BUFFER_SIZE:
			buffer_size = atoi(optarg);
			break;
		case OPT_ENUM_FORMATS:
			do_enum_formats = 1;
			break;
		case OPT_ENUM_INPUTS:
			do_enum_inputs = 1;
			break;
		case OPT_FD:
			ret = atoi(optarg);
			if (ret < 0) {
				print("Bad file descriptor %d\n", ret);
				return 1;
			}
			print("Using file descriptor %d\n", ret);
			video_set_fd(&dev, ret);
			break;
		case OPT_FIELD:
			field = v4l2_field_from_string(optarg);
			if (field == (enum v4l2_field)-1) {
				print("Invalid field order '%s'\n", optarg);
				return 1;
			}
			break;
		case OPT_LOG_STATUS:
			do_log_status = 1;
			break;
		case OPT_NO_QUERY:
			no_query = 1;
			break;
		case OPT_PREMULTIPLIED:
			fmt_flags |= V4L2_PIX_FMT_FLAG_PREMUL_ALPHA;
			break;
		case OPT_QUEUE_LATE:
			do_queue_late = 1;
			break;
		case OPT_REQUEUE_LAST:
			do_requeue_last = 1;
			break;
		case OPT_SKIP_FRAMES:
			skip = atoi(optarg);
			break;
		case OPT_SLEEP_FOREVER:
			do_sleep_forever = 1;
			break;
		case OPT_STRIDE:
			stride = atoi(optarg);
			break;
		case OPT_TSTAMP_SRC:
			if (!strcmp(optarg, "eof")) {
				dev.buffer_output_flags |= V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
			} else if (!strcmp(optarg, "soe")) {
				dev.buffer_output_flags |= V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
			} else {
				print("Invalid timestamp source %s\n", optarg);
				return 1;
			}
			break;
		case OPT_USERPTR_OFFSET:
			userptr_offset = atoi(optarg);
			break;
		case OPT_DATA_PREFIX:
			dev.write_data_prefix = true;
			break;
		default:
			print("Invalid option -%c\n", c);
			print("Run %s -h for help.\n", argv[0]);
			return 1;
		}
	}

	if ((fill_mode & BUFFER_FILL_PADDING) && memtype != V4L2_MEMORY_USERPTR) {
		print("Buffer overrun can only be checked in USERPTR mode.\n");
		return 1;
	}

	if (!do_file)
		filename = NULL;

	if (!video_has_fd(&dev)) {
		if (optind >= argc) {
			usage(argv[0]);
			return 1;
		}
		ret = video_open(&dev, argv[optind]);
		if (ret < 0)
			return 1;
	}

	if (!no_query) {
		ret = video_querycap(&dev, &capabilities);
		if (ret < 0)
			return 1;
	}

	ret = cap_get_buf_type(capabilities);
	if (ret < 0)
		return 1;

	if (!video_has_valid_buf_type(&dev))
		video_set_buf_type(&dev, ret);

	dev.memtype = memtype;

	if (do_log_status)
		video_log_status(&dev);

	if (do_get_control)
		video_print_control(&dev, ctrl_name, false);

	if (do_set_control)
		set_control(&dev, ctrl_name, ctrl_value);

	if (do_list_controls)
		video_list_controls(&dev);

	if (do_enum_formats) {
		print("- Available formats:\n");
		video_enum_formats(&dev, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		video_enum_formats(&dev, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		video_enum_formats(&dev, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		video_enum_formats(&dev, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		video_enum_formats(&dev, V4L2_BUF_TYPE_VIDEO_OVERLAY);
	}

	if (do_enum_inputs) {
		print("- Available inputs:\n");
		video_enum_inputs(&dev);
	}

	if (do_set_input) {
		video_set_input(&dev, input);
		ret = video_get_input(&dev);
		print("Input %d selected\n", ret);
	}

	/* Set the video format. */
	if (do_set_format) {
		if (video_set_format(&dev, width, height, pixelformat, stride,
				     buffer_size, field, fmt_flags) < 0) {
			video_close(&dev);
			return 1;
		}
	}

	if (do_set_dv_timings)
		video_set_dv_timings(&dev);

	if (!no_query || do_capture)
		video_get_format(&dev);

	{
		struct v4l2_event_subscription sub;

		memset(&sub, 0, sizeof(sub));

		sub.type = V4L2_EVENT_SOURCE_CHANGE;
		ioctl(dev.fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
	}

	/* Set the frame rate. */
	if (do_set_time_per_frame) {
		if (video_set_framerate(&dev, &time_per_frame) < 0) {
			video_close(&dev);
			return 1;
		}
	}
	if (!dev.fps)
		video_get_fps(&dev);

	if (do_mmal_render) {
		setup_mmal(&dev, nbufs, do_encode, encode_filename);
	}

	while (do_sleep_forever)
		sleep(1000);

	if (!do_capture) {
		video_close(&dev);
		return 0;
	}

	/* Set the compression quality. */
	if (video_set_quality(&dev, quality) < 0) {
		video_close(&dev);
		return 1;
	}

	if (video_prepare_capture(&dev, nbufs, userptr_offset, filename, fill_mode)) {
		video_close(&dev);
		return 1;
	}

	if (do_mmal_render && enable_mmal_ip(&dev))
	{
		video_close(&dev);
		return 1;
	}

	if (!do_queue_late && video_queue_all_buffers(&dev, fill_mode)) {
		video_close(&dev);
		return 1;
	}

	if (do_pause) {
		print("Press enter to start capture\n");
		getchar();
	}

	if (do_rt) {
		memset(&sched, 0, sizeof sched);
		sched.sched_priority = rt_priority;
		ret = sched_setscheduler(0, SCHED_RR, &sched);
		if (ret < 0)
			print("Failed to select RR scheduler: %s (%d)\n",
				strerror(errno), errno);
	}

	if (video_do_capture(&dev, nframes, skip, delay, filename,
			     do_requeue_last, do_queue_late, fill_mode) < 0) {
		video_close(&dev);
		return 1;
	}

	if (do_mmal_render)
		destroy_mmal(&dev);

	video_close(&dev);
	return 0;
}

