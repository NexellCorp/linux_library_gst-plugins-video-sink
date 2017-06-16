/* GStreamer
 * Copyright (C) 2016 Biela.Jo <doriya@nexell.co.kr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_NXVIDEOSINK_H_
#define _GST_NXVIDEOSINK_H_

#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>

#include <stdint.h>

G_BEGIN_DECLS

#define GST_TYPE_NXVIDEOSINK   (gst_nxvideosink_get_type())
#define GST_NXVIDEOSINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NXVIDEOSINK,GstNxvideosink))
#define GST_NXVIDEOSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NXVIDEOSINK,GstNxvideosinkClass))
#define GST_IS_NXVIDEOSINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NXVIDEOSINK))
#define GST_IS_NXVIDEOSINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NXVIDEOSINK))

typedef struct _GstNxvideosink GstNxvideosink;
typedef struct _GstNxvideosinkClass GstNxvideosinkClass;

#define MAX_INPUT_BUFFER		32
#define MAX_ALLOC_BUFFER		8
#define MAX_PLANE_NUM			4

typedef struct
{
	int32_t		width;			//	Video Image's Width
	int32_t		height;			//	Video Image's Height
	int32_t		align;			//	Start address align
	int32_t		planes;			//	Number of valid planes
	int32_t		pixel_byte;		//	Pixel Bytes
	uint32_t	format;			//	Pixel Format

	int			drm_fd;					//	Drm Device Handle
	int			dma_fd[MAX_PLANE_NUM];	//	DMA memory Handle
	int			gem_fd[MAX_PLANE_NUM];	//	GEM Handle
	int32_t		size[MAX_PLANE_NUM];	//	Each plane's size.
	int32_t		stride[MAX_PLANE_NUM];	//	Each plane's stride.
	void*		buffer[MAX_PLANE_NUM];	//	virtual address.
} NX_VID_MEMORY;

struct _GstNxvideosink
{
	GstVideoSink base_nxvideosink;

	gint	width;
	gint	height;
	gint	src_x, src_y, src_w, src_h;
	gint	dst_x, dst_y, dst_w, dst_h;

	int		drm_fd;
	gint	drm_format;
	guint	plane_id;
	guint	crtc_id;
	guint	buffer_id[MAX_INPUT_BUFFER];
	gint	index;
	gboolean init;

	NX_VID_MEMORY *video_memory[MAX_INPUT_BUFFER];
	NX_VID_MEMORY *extra_video_buf;
	guint	extra_video_buf_id;
	GstBuffer *prv_buf;
};

struct _GstNxvideosinkClass
{
	GstVideoSinkClass base_nxvideosink_class;
};

GType gst_nxvideosink_get_type (void);

G_END_DECLS

#endif
