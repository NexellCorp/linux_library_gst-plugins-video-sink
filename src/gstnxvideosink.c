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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstnxvideosink
 *
 * The nxvideosink element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! nxvideosink ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <nexell/nexell_drm.h>
#include <mm_types.h>

#include <gstmmvideobuffermeta.h>
#include "gstnxvideosink.h"

struct crtc {
	drmModeCrtc *crtc;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	drmModeModeInfo *mode;
};

struct plane {
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

struct resources {
	drmModeRes *res;
	drmModePlaneRes *plane_res;

	struct crtc *crtcs;
	struct plane *planes;
};


GST_DEBUG_CATEGORY_STATIC (gst_nxvideosink_debug_category);
#define GST_CAT_DEFAULT gst_nxvideosink_debug_category

/* prototypes */

static void gst_nxvideosink_set_property (GObject * object,
		guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_nxvideosink_get_property (GObject * object,
		guint property_id, GValue * value, GParamSpec * pspec);
static void gst_nxvideosink_finalize (GObject * object);

static gboolean gst_nxvideosink_set_caps( GstBaseSink *base_sink, GstCaps *caps );
static gboolean gst_nxvideosink_event( GstPad *pad, GstObject *parent, GstEvent *event );

static GstFlowReturn gst_nxvideosink_show_frame( GstVideoSink *video_sink, GstBuffer *buf );

#define MAX_DISPLAY_WIDTH	2048
#define MAX_DISPLAY_HEIGHT	2048

enum
{
	PROP_0,

	PROP_SRC_X,		// Source Crop
	PROP_SRC_Y,
	PROP_SRC_W,
	PROP_SRC_H,

	PROP_DST_X,		// Destination Position
	PROP_DST_Y,
	PROP_DST_W,
	PROP_DST_H,

	PROP_PLANE_ID,
	PROP_CRTC_ID,
#if TEST_CONNECT
	PROP_CRTC_INDEX,
#endif
};

/* pad templates */

/* FIXME: add/remove formats you can handle */
static GstStaticPadTemplate gst_nxvideosink_sink_template =
	GST_STATIC_PAD_TEMPLATE( "sink",
		GST_PAD_SINK,
		GST_PAD_ALWAYS,
		GST_STATIC_CAPS(
			"video/x-raw, "
			"format = (string) { I420, YUY2 }, "
			"width = (int) [ 1, 2048 ], "
			"height = (int) [ 1, 2048 ]; "
		)
	);

static GstStaticPadTemplate gst_nxvideosink_sink_template_nxp3220 =
	GST_STATIC_PAD_TEMPLATE( "sink",
		GST_PAD_SINK,
		GST_PAD_ALWAYS,
		GST_STATIC_CAPS(
			"video/x-raw, "
			"format = (string) { I420, YUY2, NV12 }, "
			"width = (int) [ 1, 2048 ], "
			"height = (int) [ 1, 2048 ]; "
		)
	);

///////////////////////////////////////////////////////////////////////////////
static gint IsCpuNXP322X()
{
    FILE *pFileCpuInfo = NULL;
    char *pCpuInfoBuf = NULL;
    int32_t cpuInfoLen = 0;
    int32_t findStream = 0;
    char *pFindStream = NULL;
    
    pFileCpuInfo = fopen( "/proc/cpuinfo", "rb" );
    if(pFileCpuInfo == NULL)
    {
        GST_ERROR("Error File Open!\n");
        return findStream;
    }
    pCpuInfoBuf = (char *)malloc(4096);
    if(pCpuInfoBuf == NULL)
    {
        GST_ERROR("Error malloc!\n");
        if(pFileCpuInfo)
            fclose(pFileCpuInfo);
        return findStream;
    }

    cpuInfoLen = fread(pCpuInfoBuf, sizeof(char), 4096, pFileCpuInfo);
    pFindStream = strstr(pCpuInfoBuf, "nxp322x");
    if(pFindStream)
    {
        findStream = 1;  //nxp322x
    }

    if(pFileCpuInfo)
        fclose(pFileCpuInfo);

    if(pCpuInfoBuf)
        free(pCpuInfoBuf);

    return findStream;
}	


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstNxvideosink, gst_nxvideosink, GST_TYPE_VIDEO_SINK,
	GST_DEBUG_CATEGORY_INIT (gst_nxvideosink_debug_category, "nxvideosink", 0,
	"debug category for nxvideosink element"));

static void
gst_nxvideosink_class_init (GstNxvideosinkClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( klass );
	GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS( klass );
	GstVideoSinkClass *video_sink_class = GST_VIDEO_SINK_CLASS( klass );

	/* Setting up pads and setting metadata should be moved to
		 base_class_init if you intend to subclass this class. */

	if( IsCpuNXP322X() )
	{
		gst_element_class_add_pad_template( GST_ELEMENT_CLASS(klass),
				gst_static_pad_template_get(&gst_nxvideosink_sink_template_nxp3220) );

	}
	else
	{
		gst_element_class_add_pad_template( GST_ELEMENT_CLASS(klass),
				gst_static_pad_template_get(&gst_nxvideosink_sink_template) );

	}

	gst_element_class_set_static_metadata( GST_ELEMENT_CLASS(klass),
		"S5PXX18/NXP4330/NXP322X H/W Video Renderer",
		"Renderer/Video",
		"Nexell H/W Video Renderer for S5PXX18/NXP4330/NXP322X",
		"Hyun Chul Jun <hcjun@nexell.co.kr>"
	);

	gobject_class->set_property = gst_nxvideosink_set_property;
	gobject_class->get_property = gst_nxvideosink_get_property;
	gobject_class->finalize = gst_nxvideosink_finalize;

	base_sink_class->set_caps = GST_DEBUG_FUNCPTR( gst_nxvideosink_set_caps );

	video_sink_class->show_frame = GST_DEBUG_FUNCPTR( gst_nxvideosink_show_frame );

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_SRC_X,
		g_param_spec_int( "src-x", "src-x",
			"Source Crop X",
			0, MAX_DISPLAY_WIDTH, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_SRC_Y,
		g_param_spec_int( "src-y", "src-y",
			"Source Crop y",
			0, MAX_DISPLAY_HEIGHT, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_SRC_W,
		g_param_spec_int( "src-w", "src-w",
			"Source Crop Width",
			0, MAX_DISPLAY_WIDTH, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_SRC_H,
		g_param_spec_int( "src-h", "src-h",
			"Source Crop Height",
			0, MAX_DISPLAY_HEIGHT, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_DST_X,
		g_param_spec_int( "dst-x", "dst-x",
			"Destination Position X",
			-MAX_DISPLAY_WIDTH, MAX_DISPLAY_WIDTH, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_DST_Y,
		g_param_spec_int( "dst-y", "dst-y",
			"Destination Position y",
			-MAX_DISPLAY_HEIGHT, MAX_DISPLAY_HEIGHT, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_DST_W,
		g_param_spec_int( "dst-w", "dst-w",
			"Destination Position Width",
			-MAX_DISPLAY_WIDTH, MAX_DISPLAY_WIDTH, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_DST_H,
		g_param_spec_int( "dst-h", "dst-h",
			"Destination Position Height",
			-MAX_DISPLAY_HEIGHT, MAX_DISPLAY_HEIGHT, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_PLANE_ID,
		g_param_spec_uint( "plane-id","plane-id",
			"Drm PlaneId",
			0, 100, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_CRTC_ID,
		g_param_spec_uint( "crtc-id","crtc-id",
			"Drm CrtcId",
			0, 100, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);

#if TEST_CONNECT
	g_object_class_install_property( G_OBJECT_CLASS (klass), PROP_CRTC_INDEX,
		g_param_spec_uint( "crtc-index","crtc-index",
			"Drm CrtcInDex",
			0, 100, 0,
			(GParamFlags) (G_PARAM_READWRITE)
		)
	);
#endif
}

#if TEST_CONNECT
static uint32_t find_crtc_for_encoder(const drmModeRes *pResources, const drmModeEncoder *pEncoder)
{
	int i = 0;

	for (i = 0; i < pResources->count_crtcs; i++)
	{
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = pResources->crtcs[i];
		if (pEncoder->possible_crtcs & crtc_mask)
		{
			return crtc_id;
		}
	}

	/* no match found */
	return -1;
}

static uint32_t find_crtc_for_connector(int drmFd, const drmModeRes *pResources, const drmModeConnector *pConnector)
{
	int i = 0;

	for (i = 0; i < pConnector->count_encoders; i++)
	{
		const uint32_t encoder_id = pConnector->encoders[i];
		drmModeEncoder *pEncoder = drmModeGetEncoder(drmFd, encoder_id);

		if (pEncoder)
		{
			const uint32_t crtc_id = find_crtc_for_encoder(pResources, pEncoder);

			drmModeFreeEncoder(pEncoder);
			if (crtc_id != 0)
			{
				return crtc_id;
			}
		}
	}

	/* no match found */
	return -1;
}

// drmMode: 1 => usrConnectorID
//          0 => autoConnectorID
static drmModeConnector* init_drm_set_mode(GstNxvideosink *nxvideosink, int drm_fd, drmModeRes *pResources, int drmMode, int usrConnectorID)
{
	int i, area;
	drmModeConnector *pConnector = NULL;

	/* find a connected connector: */
	for (i = 0; i < pResources->count_connectors; i++)
	{
		pConnector = drmModeGetConnector(drm_fd, pResources->connectors[i]);
		if (drmMode)
		{
			if ((pConnector->connector_id == usrConnectorID) && (pConnector->connection == DRM_MODE_CONNECTED))
			{
				/* it's connected, let's use this! */
				break;
			}
		}
		else
		{
			if (pConnector->connection == DRM_MODE_CONNECTED)
			{
				/* it's connected, let's use this! */
				if(pResources->count_connectors == (i)) //lcd
//				if(pResources->count_connectors == (i+1)) //hdmi
					break;
			}
		}
		drmModeFreeConnector(pConnector);
		pConnector = NULL;
	}

	if (!pConnector)
	{
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		GST_ERROR("no connected connector!, count_connectors(%d), usrConnectorID(%d), drmMode(%d)\n",
			pResources->count_connectors, usrConnectorID, drmMode);
		return NULL;
	}
	else
	{
		GST_DEBUG_OBJECT(nxvideosink,"connected connector!, count_connectors(%d), connector_id(%d), usrConnectorID(%d), drmMode(%d)\n",
						pResources->count_connectors, pConnector->connector_id, usrConnectorID, drmMode);
	}

	/* find prefered mode or the highest resolution mode: */
	for (i = 0, area = 0; i < pConnector->count_modes; i++)
	{
		drmModeModeInfo *pCurrent_mode = &pConnector->modes[i];

		if (pCurrent_mode->type & DRM_MODE_TYPE_PREFERRED)
		{
			nxvideosink->pCurrent_mode = (void *)pCurrent_mode;
		}

		int current_area = pCurrent_mode->hdisplay * pCurrent_mode->vdisplay;

		if (current_area > area) {
			nxvideosink->pCurrent_mode = (void *)pCurrent_mode;
			area = current_area;
		}
	}
	return pConnector;
}

static int init_drm_find_encoder(int drmFd, drmModeRes *pResources, drmModeConnector *pConnector,
								int *pCrtcID, int *pConnectorID)
{
	drmModeEncoder *pEncoder = NULL;
	int i;

	/* find encoder: */
	for (i = 0; i < pResources->count_encoders; i++)
	{
		pEncoder = drmModeGetEncoder(drmFd, pResources->encoders[i]);

		if (pEncoder->encoder_id == pConnector->encoder_id)
		{
			break;
		}
		drmModeFreeEncoder(pEncoder);
		pEncoder = NULL;
	}

	if (pEncoder)
	{
		*pCrtcID = pEncoder->crtc_id;
	}
	else
	{
		uint32_t crtc_id = find_crtc_for_connector(drmFd, pResources, pConnector);
		if (crtc_id == 0)
		{
			GST_ERROR("no crtc found!\n");
			return -1;
		}

		*pCrtcID = crtc_id;
	}

	*pConnectorID = pConnector->connector_id;

	return  0;
}

static int init_drm(GstNxvideosink *nxvideosink, int drmFd, drmModeRes *pDrmResources, int drmMode, int usrConnectorID, int *pCrtcID, int *pConnectorID)
{
	drmModeConnector *pConnector = NULL;
	int ret = 0;

	pConnector = init_drm_set_mode(nxvideosink, drmFd, pDrmResources, drmMode, usrConnectorID);
	if(pConnector == NULL)
	{
		GST_ERROR("Error: init_drm_set_mode() !!!\n");
		return -1;
	}

	ret = init_drm_find_encoder(drmFd, pDrmResources, pConnector, pCrtcID, pConnectorID);
	if(ret != 0)
	{
		GST_ERROR("Error: init_drm_find_encoder() !!!\n");
		return -1;
	}

	return 0;
}
#endif

static int32_t find_video_plane( int fd, int crtcIdx, uint32_t *connId, uint32_t *crtcId, uint32_t *planeId )
{
	uint32_t possible_crtcs = 0;
	drmModeRes *res;
	drmModePlaneRes *pr;
	drmModePlane *plane;
	uint32_t i, j;
	int32_t found = 0;

	res = drmModeGetResources(fd);

	if( crtcIdx >= res->count_crtcs )
	{
		goto ErrorExit;
	}

	*crtcId = res->crtcs[ crtcIdx ];
	*connId = res->connectors[ crtcIdx ];

	possible_crtcs = 1<<crtcIdx;

	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	pr = drmModeGetPlaneResources( fd );

	for( i=0 ; i<pr->count_planes ; ++i )
	{
		plane = drmModeGetPlane( fd, pr->planes[i] );
		if( plane->possible_crtcs & possible_crtcs )
		{
			for( j=0 ; j<plane->count_formats ; j++ )
			{
				if( plane->formats[j]==DRM_FORMAT_YUV420 ||
					plane->formats[j]==DRM_FORMAT_YVU420 ||
					plane->formats[j]==DRM_FORMAT_UYVY ||
					plane->formats[j]==DRM_FORMAT_VYUY ||
					plane->formats[j]==DRM_FORMAT_YVYU ||
					plane->formats[j]==DRM_FORMAT_YUYV )
				{
					found = 1;
					*planeId = plane->plane_id;
				}
			}
		}
	}
	drmModeFreeResources(res);
	return found?0:-1;
ErrorExit:
	drmModeFreeResources(res);
	return -1;
}

static int drm_ioctl( int fd, unsigned long request, void *arg )
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));

	return ret;
}

static int drm_command_write_read( int fd, uint32_t command_index, void *data, uint32_t size )
{
	uint32_t request;

	request = DRM_IOC( DRM_IOC_READ|DRM_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + command_index, size );

	if (drm_ioctl(fd, request, data))
		return -errno;

	return 0;
}

static int alloc_gem(int fd, uint64_t size, int flags)
{
	struct nx_drm_gem_create arg = { 0, };
	int ret;

	arg.size = size;
	arg.flags = flags;

	ret = drm_command_write_read( fd, DRM_NX_GEM_CREATE, &arg, sizeof(arg) );
	if (ret)
		return ret;

	return arg.handle;
}

static void free_gem(int fd, int gem)
{
	struct drm_gem_close arg = {0, };

	arg.handle = gem;
	drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &arg);
}

static int gem_to_dmafd(int fd, int gem_fd)
{
	int ret;
	struct drm_prime_handle arg = {0, };

	arg.handle = gem_fd;
	ret = drm_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg);
	if (0 != ret)
		return -1;

	return arg.fd;
}

static int import_gem_from_flink( int fd, unsigned int flink_name )
{
	struct drm_gem_open arg = { 0, };
	/* struct nx_drm_gem_info info = { 0, }; */

	arg.name = flink_name;
	if (drm_ioctl(fd, DRM_IOCTL_GEM_OPEN, &arg)) {
		return -EINVAL;
	}

	return arg.handle;
}

static NX_VID_MEMORY*
allocate_buffer( int drm_fd, int32_t width, int32_t height, int32_t format )
{
	int gem_fd[MAX_PLANE_NUM] = {0, };
	int dma_fd[MAX_PLANE_NUM] = {0, };
	int32_t i=0, pixelByte=0, planes=0;
	int32_t luStride, cStride;
	int32_t luVStride, cVStride;
	int32_t stride[MAX_PLANE_NUM];
	int32_t size[MAX_PLANE_NUM];
	void*	buffer[MAX_PLANE_NUM];
	NX_VID_MEMORY *video_memory;

	luStride  = GST_ROUND_UP_32( width );
	luVStride = GST_ROUND_UP_16( height );

	switch( format )
	{
	case DRM_FORMAT_YUV420:
		cStride   = GST_ROUND_UP_16( luStride / 2 );
		cVStride  = GST_ROUND_UP_16( height / 2 );
		planes    = 3;
		pixelByte = 1;
		break;

	case DRM_FORMAT_YUYV:
		cStride   = GST_ROUND_UP_16( luStride / 2 );
		cVStride  = GST_ROUND_UP_16( height / 2 );
		planes    = 1;
		pixelByte = 2;
		break;

	default:
		break;
	}

	switch( planes )
	{
	case 3:
		size[2]   = cStride*cVStride;
		stride[2] = cStride;

		gem_fd[2] = alloc_gem(drm_fd, size[2], 0);
		if (gem_fd[2] < 0) goto ErrorExit;

		dma_fd[2] = gem_to_dmafd(drm_fd, gem_fd[2]);
		if (dma_fd[2] < 0) goto ErrorExit;

		buffer[2] = mmap( 0, size[2], PROT_READ|PROT_WRITE, MAP_SHARED, dma_fd[2], 0 );
		if (buffer[0] == MAP_FAILED) goto ErrorExit;

	case 2:
		size[1]   = cStride*cVStride;
		stride[1] = cStride;

		gem_fd[1] = alloc_gem(drm_fd, size[1], 0);
		if (gem_fd[1] < 0) goto ErrorExit;

		dma_fd[1] = gem_to_dmafd(drm_fd, gem_fd[1]);
		if (dma_fd[1] < 0) goto ErrorExit;

		buffer[1] = mmap( 0, size[1], PROT_READ|PROT_WRITE, MAP_SHARED, dma_fd[1], 0 );
		if (buffer[0] == MAP_FAILED) goto ErrorExit;

	case 1:
		size[0]   = luStride*luVStride*pixelByte;
		stride[0] = luStride*pixelByte;

		gem_fd[0] = alloc_gem(drm_fd, size[0], 0);
		if (gem_fd[0] < 0) goto ErrorExit;

		dma_fd[0] = gem_to_dmafd(drm_fd, gem_fd[0]);
		if (dma_fd[0] < 0) goto ErrorExit;

		buffer[0] = mmap( 0, size[0], PROT_READ|PROT_WRITE, MAP_SHARED, dma_fd[0], 0 );
		if (buffer[0] == MAP_FAILED) goto ErrorExit;

	default:
		break;
	}

	video_memory = (NX_VID_MEMORY *)calloc(1, sizeof(NX_VID_MEMORY));
	video_memory->width      = width;
	video_memory->height     = height;
	video_memory->planes     = planes;
	video_memory->pixel_byte = pixelByte;
	video_memory->format     = format;
	video_memory->drm_fd     = drm_fd;

	for( i=0 ; i<planes ; i++ )
	{
		video_memory->dma_fd[i] = dma_fd[i];
		video_memory->gem_fd[i] = gem_fd[i];
		video_memory->size[i]   = size[i];
		video_memory->stride[i] = stride[i];
		video_memory->buffer[i] = buffer[i];
	}
	return video_memory;

ErrorExit:
	for( i=0 ; i<planes ; i++ )
	{
		if( buffer[i] )
		{
			munmap( buffer[i], size[i] );
		}

		if( gem_fd[i] > 0 )
		{
			free_gem( drm_fd, gem_fd[i] );
		}
		if( dma_fd[i] > 0 )
		{
			close( dma_fd[i] );
		}
	}

	return NULL;
}

static void
free_buffer( NX_VID_MEMORY *video_memory )
{
	int32_t i;
	if( video_memory )
	{
		for( i = 0; i < video_memory->planes; i++ )
		{
			if( video_memory->buffer[i] )
			{
				munmap( video_memory->buffer[i], video_memory->size[i] );
			}

			free_gem( video_memory->drm_fd, video_memory->gem_fd[i] );
			close( video_memory->dma_fd[i] );
		}
		free( video_memory );
	}
}

static void
copy_to_videomemory( GstBuffer *buffer, NX_VID_MEMORY *video_memory )
{
	guchar *pSrc, *pDst;
	gint i, j;

	GstMapInfo info;
	GstVideoMeta *video_meta = gst_buffer_get_video_meta( buffer );

	gst_buffer_map( buffer, &info, GST_MAP_READ );

	if( NULL != video_meta )
	{
		pSrc = (guchar*)info.data;
		pDst = (guchar*)video_memory->buffer[0];

		for( i = 0; i < video_meta->height; i++ )
		{
			memcpy( pDst, pSrc, video_meta->stride[0] );

			pSrc += video_meta->stride[0];
			pDst += video_memory->stride[0];
		}

		for( j = 1; j < video_meta->n_planes; j++ )
		{
			pSrc = (guchar*)info.data + video_meta->offset[j];
			pDst = (guchar*)video_memory->buffer[j];

			for( i = 0;  i < video_meta->height / 2; i ++ )
			{
				memcpy( pDst, pSrc, video_meta->stride[j] );

				pSrc += video_meta->stride[j];
				pDst += video_memory->stride[j];
			}
		}
	}
	else
	{
		pSrc = (guchar*)info.data;
		pDst = (guchar*)video_memory->buffer[0];

		for( i = 0; i < video_memory->height; i++ )
		{
			memcpy( pDst, pSrc, video_memory->width * video_memory->pixel_byte );

			pSrc += video_memory->width * video_memory->pixel_byte;
			pDst += video_memory->stride[0];
		}

		for( j = 1; j < video_memory->planes; j++ )
		{
			pDst = (guchar*)video_memory->buffer[j];

			for( i = 0; i < video_memory->height / 2; i++ )
			{
				memcpy( pDst, pSrc, video_memory->width / 2 );

				pSrc += video_memory->width / 2;
				pDst += video_memory->stride[j];
			}
		}
	}

	gst_buffer_unmap( buffer, &info );
}

static void
gst_nxvideosink_init( GstNxvideosink *nxvideosink )
{
	gint i = 0;
	uint32_t connId = 0;

	struct resources *res = NULL;

	nxvideosink->width      = 0;
	nxvideosink->height     = 0;

	nxvideosink->src_x      = 0;
	nxvideosink->src_y      = 0;
	nxvideosink->src_w      = 0;
	nxvideosink->src_h      = 0;

	nxvideosink->dst_x      = 0;
	nxvideosink->dst_y      = 0;
	nxvideosink->dst_w      = 0;
	nxvideosink->dst_h      = 0;

	nxvideosink->drm_fd     = -1;
	nxvideosink->drm_format = -1;
	nxvideosink->plane_id   = -1;
	nxvideosink->crtc_id    = -1;
	nxvideosink->crtc_index = 0;  //display number
	nxvideosink->index      = 0;
	nxvideosink->init       = FALSE;
	nxvideosink->prv_buf    = NULL;

	for( i = 0 ; i < MAX_INPUT_BUFFER; i++ )
	{
		nxvideosink->buffer_id[i] = 0;
		nxvideosink->video_memory[i] = NULL;
	}

	//
	// FIX ME!! ( DO NOT MOVE THIS CODE )
	//   It is seems to be bugs.
	//   If this drmOpen() is called late than another drmOpen(), drmModeSetPlane() is failed.
	//
	nxvideosink->drm_fd = drmOpen( "nexell", NULL );
	if( 0 > nxvideosink->drm_fd )
	{
		GST_ERROR("Fail, drmOpen().\n");
	}

	drmSetMaster( nxvideosink->drm_fd );

	gst_pad_set_event_function( GST_VIDEO_SINK_PAD(nxvideosink), gst_nxvideosink_event );

#if TEST_CONNECT
	{
		int CrtcID; int ConnectorID;
		drmModeRes *pDrmRes;
		pDrmRes = drmModeGetResources(nxvideosink->drm_fd);
		init_drm(nxvideosink, nxvideosink->drm_fd, pDrmRes, 0, 0, &CrtcID, &ConnectorID);
	}
#endif

	/* get default crtc, plane ids
	 * default ids are going be the first detected ids
	 */
	if( 0 == find_video_plane(nxvideosink->drm_fd, nxvideosink->crtc_index, &connId, &nxvideosink->crtc_id, &nxvideosink->plane_id) )
	{
		GST_DEBUG_OBJECT(nxvideosink, "VIDEO : connId = %d, crtcId = %d, planeId = %d\n", connId, nxvideosink->crtc_id, nxvideosink->plane_id);
	}
	else
	{
		GST_DEBUG_OBJECT(nxvideosink, "cannot found video format for %dth crtc\n", nxvideosink->crtc_index );
	}
}

void
gst_nxvideosink_set_property (GObject * object, guint property_id,
		const GValue * value, GParamSpec * pspec)
{
	GstNxvideosink *nxvideosink = GST_NXVIDEOSINK (object);

	GST_DEBUG_OBJECT( nxvideosink, "set_property" );

	switch (property_id) {
		case PROP_SRC_X:
			nxvideosink->src_x = g_value_get_int( value );
			break;

		case PROP_SRC_Y:
			nxvideosink->src_y = g_value_get_int( value );
			break;

		case PROP_SRC_W:
			nxvideosink->src_w = g_value_get_int( value );
			break;

		case PROP_SRC_H:
			nxvideosink->src_h = g_value_get_int( value );
			break;

		case PROP_DST_X:
			nxvideosink->dst_x = g_value_get_int( value );
			break;

		case PROP_DST_Y:
			nxvideosink->dst_y = g_value_get_int( value );
			break;

		case PROP_DST_W:
			nxvideosink->dst_w = g_value_get_int( value );
			break;

		case PROP_DST_H:
			nxvideosink->dst_h = g_value_get_int( value );
			break;

		case PROP_PLANE_ID:
//			nxvideosink->plane_id = g_value_get_uint( value );
			break;

		case PROP_CRTC_ID:
//			nxvideosink->crtc_id = g_value_get_uint( value );
			break;

#if TEST_CONNECT
		case PROP_CRTC_INDEX:
			nxvideosink->crtc_index = g_value_get_uint( value );
			break;
#endif
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID( object, property_id, pspec );
			break;
	}
}

void
gst_nxvideosink_get_property (GObject * object, guint property_id,
		GValue * value, GParamSpec * pspec)
{
	GstNxvideosink *nxvideosink = GST_NXVIDEOSINK (object);

	GST_DEBUG_OBJECT( nxvideosink, "get_property" );

	switch (property_id) {
		case PROP_SRC_X:
			g_value_set_int( value, nxvideosink->src_x );
			break;

		case PROP_SRC_Y:
			g_value_set_int( value, nxvideosink->src_y );
			break;

		case PROP_SRC_W:
			g_value_set_int( value, nxvideosink->src_w );
			break;

		case PROP_SRC_H:
			g_value_set_int( value, nxvideosink->src_h );
			break;

		case PROP_DST_X:
			g_value_set_int( value, nxvideosink->dst_x );
			break;

		case PROP_DST_Y:
			g_value_set_int( value, nxvideosink->dst_y );
			break;

		case PROP_DST_W:
			g_value_set_int( value, nxvideosink->dst_w );
			break;

		case PROP_DST_H:
			g_value_set_int( value, nxvideosink->dst_h );
			break;

		case PROP_PLANE_ID:
			g_value_set_uint( value, nxvideosink->plane_id );
			break;

		case PROP_CRTC_ID:
			g_value_set_uint( value, nxvideosink->crtc_id );
			break;
#if TEST_CONNECT
		case PROP_CRTC_INDEX:
			g_value_set_uint( value, nxvideosink->crtc_index );
			break;
#endif

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID( object, property_id, pspec );
			break;
	}
}

void
gst_nxvideosink_finalize (GObject * object)
{
	GstNxvideosink *nxvideosink = GST_NXVIDEOSINK (object);
	gint i;

	GST_DEBUG_OBJECT( nxvideosink, "finalize" );

	/* clean up object here */
	for( i = 0; i < MAX_INPUT_BUFFER; i++ )
	{
		if( 0 < nxvideosink->buffer_id[i] )
		{
			drmModeRmFB( nxvideosink->drm_fd, nxvideosink->buffer_id[i] );
			nxvideosink->buffer_id[i] = 0;
		}
	}

	for( i = 0; i < MAX_ALLOC_BUFFER; i++ )
	{
		if( NULL != nxvideosink->video_memory[i] )
		{
			free_buffer( nxvideosink->video_memory[i] );
		}
	}

	if( 0 <= nxvideosink->drm_fd )
	{
		drmClose( nxvideosink->drm_fd );
		nxvideosink->drm_fd = -1;
	}

	if( nxvideosink->prv_buf )
	{
		gst_buffer_unref( nxvideosink->prv_buf );
		nxvideosink->prv_buf = NULL;
	}

	G_OBJECT_CLASS (gst_nxvideosink_parent_class)->finalize (object);
}

static gboolean
gst_nxvideosink_set_caps( GstBaseSink *base_sink, GstCaps *caps )
{
	GstNxvideosink *nxvideosink = GST_NXVIDEOSINK( base_sink );
	GstStructure *structure;
	gint i;

	const gchar *mime_type;
	const gchar *format;

	GST_DEBUG_OBJECT( nxvideosink, "set_caps" );

	structure = gst_caps_get_structure( caps, 0 );

	mime_type = gst_structure_get_name( structure );
	if( g_strcmp0( mime_type, "video/x-raw" ) )
	{
		GST_ERROR("Fail, Not Support Mime Type.\n");
		return FALSE;
	}

	format = gst_structure_get_string( structure, "format" );
	if( !g_strcmp0( format, "I420" ) )
	{
		nxvideosink->drm_format = DRM_FORMAT_YUV420;
	}
	else if( !g_strcmp0( format, "YUY2") )
	{
		nxvideosink->drm_format = DRM_FORMAT_YUYV;
	}
	if( IsCpuNXP322X() )
	{
		if( !g_strcmp0( format, "NV12") )
		{
			nxvideosink->drm_format = DRM_FORMAT_NV12;
		}
		else
		{
			GST_ERROR("Fail, Not Support Format.\n");
			return FALSE;
		}
	}
	else
	{
		GST_ERROR("Fail, Not Support Format.\n");
		return FALSE;
	}

	gst_structure_get_int( structure, "width", &nxvideosink->width );
	gst_structure_get_int( structure, "height", &nxvideosink->height );

	if( nxvideosink->width < 1 || nxvideosink->height < 1 )
	{
		GST_ERROR("Fail, Not Support Resolution.( %d x %d )\n", nxvideosink->width, nxvideosink->height );
		return FALSE;
	}

	nxvideosink->src_w  = nxvideosink->src_w ? nxvideosink->src_w : nxvideosink->width;
	nxvideosink->src_h  = nxvideosink->src_h ? nxvideosink->src_h : nxvideosink->height;

	nxvideosink->dst_w  = nxvideosink->dst_w ? nxvideosink->dst_w : nxvideosink->width;
	nxvideosink->dst_h  = nxvideosink->dst_h ? nxvideosink->dst_h : nxvideosink->height;

	GST_DEBUG_OBJECT(nxvideosink, "mime( %s ), format( %s ), width( %d ), height( %d ), src( %d, %d, %d, %d ), dst( %d, %d, %d, %d )\n",
		mime_type, format, nxvideosink->width, nxvideosink->height,
		nxvideosink->src_x, nxvideosink->src_y, nxvideosink->src_w, nxvideosink->src_h,
		nxvideosink->dst_x, nxvideosink->dst_y, nxvideosink->dst_w, nxvideosink->dst_h
	);

	//
	//
	//
	// nxvideosink->drm_fd = drmOpen( "nexell", NULL );
	// if( 0 > nxvideosink->drm_fd )
	// {
	// 	GST_ERROR("Fail, drmOpen().\n");
	// 	return FALSE;
	// }

	// drmSetMaster( nxvideosink->drm_fd );

	if( 0 > drmSetClientCap(nxvideosink->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) )
	{
		drmClose( nxvideosink->drm_fd );
		nxvideosink->drm_fd = -1;

		GST_ERROR("Fail, drmSetClientCap().\n");
		return FALSE;
	}

	for( i = 0; i < MAX_INPUT_BUFFER; i++ )
	{
		nxvideosink->buffer_id[i] = 0;
	}

	return TRUE;
}

static gboolean
gst_nxvideosink_event( GstPad *pad, GstObject *parent, GstEvent *event )
{
	GstBaseSink *base_sink = GST_BASE_SINK_CAST( parent );
	GstBaseSinkClass *base_class = GST_BASE_SINK_GET_CLASS( base_sink );
	GstNxvideosink *nxvideosink = GST_NXVIDEOSINK( base_sink );

	gboolean result = TRUE;

	GST_DEBUG_OBJECT( nxvideosink, "received event %p %" GST_PTR_FORMAT, event, event );

	switch( GST_EVENT_TYPE( event ) )
	{
		case GST_EVENT_FLUSH_STOP:
			// special case for this serialized event because we don't want to grab
			// the PREROLL lock or check if we were flushing
			if( nxvideosink->prv_buf )
			{
				gst_buffer_unref( nxvideosink->prv_buf );
				nxvideosink->prv_buf = NULL;
			}

			if( base_class->event )
			{
				result = base_class->event( base_sink, event );
			}
			break;

		default:
			if( GST_EVENT_IS_SERIALIZED(event) )
			{
				GST_BASE_SINK_PREROLL_LOCK( base_sink );

				if( G_UNLIKELY(base_sink->flushing) )
				{
					GST_DEBUG_OBJECT( base_sink, "Fail, Drop Message. ( Flushing )" );
					GST_BASE_SINK_PREROLL_UNLOCK( base_sink );
					gst_event_unref( event );

					result = FALSE;
					break;
				}

				if( G_UNLIKELY(base_sink->eos) )
				{
					GST_DEBUG_OBJECT( base_sink, "Fail, Drop Message. ( EOS )" );
					GST_BASE_SINK_PREROLL_UNLOCK( base_sink );
					gst_event_unref( event );

					result = FALSE;
					break;
				}

				if( base_class->event )
				{
					result = base_class->event( base_sink, event );
				}

				GST_BASE_SINK_PREROLL_UNLOCK( base_sink );
			}
			else
			{
				if( base_class->event )
				{
					result = base_class->event( base_sink, event );
				}
			}
			break;
	}

	return result;
}

static GstFlowReturn
gst_nxvideosink_show_frame( GstVideoSink * sink, GstBuffer * buf )
{
	GstNxvideosink *nxvideosink = GST_NXVIDEOSINK( sink );

	GstMMVideoBufferMeta *meta = NULL;
	GstFlowReturn ret = GST_FLOW_OK;
	GstMapInfo info;

	gint err;

	GST_DEBUG_OBJECT( nxvideosink, "show_frame" );

	if( nxvideosink->prv_buf == buf )
	{
		return GST_FLOW_OK;
	}

	gst_buffer_ref( buf );

	meta = gst_buffer_get_mmvideobuffer_meta( buf );
	if( NULL != meta && meta->memory_index >= 0 )
	{
		GstMemory *meta_block = NULL;
		MMVideoBuffer *mm_buf = NULL;

		meta_block = gst_buffer_peek_memory( buf, meta->memory_index );
		if( !meta_block )
		{
			GST_ERROR("Fail, gst_buffer_peek_memory().\n");
		}

		memset(&info, 0, sizeof(GstMapInfo));
		gst_memory_map(meta_block, &info, GST_MAP_READ);

		mm_buf = (MMVideoBuffer*)info.data;
		if( !mm_buf )
		{
			GST_ERROR("Fail, get MMVideoBuffer.\n");
		}
		else
		{
			GST_DEBUG_OBJECT( nxvideosink, "type: 0x%x, width: %d, height: %d, plane_num: %d, handle_num: %d, index: %d\n",
					mm_buf->type, mm_buf->width[0], mm_buf->height[0], mm_buf->plane_num, mm_buf->handle_num, mm_buf->buffer_index );

			if( 0 > mm_buf->buffer_index )
			{
				gst_memory_unmap( meta_block, &info );
				gst_buffer_unref( buf );
				return GST_FLOW_OK;
			}

			if( nxvideosink->buffer_id[mm_buf->buffer_index] == 0 )
			{
				gint i = 0;
				guint handles[4] = { 0, };
				guint pitches[4] = { 0, };
				guint offsets[4] = { 0, };
				guint offset = 0;

				for( i = 0; i < mm_buf->plane_num; i++ )
				{
					handles[i] = (mm_buf->handle_num == 1) ?
						import_gem_from_flink( nxvideosink->drm_fd, mm_buf->handle.gem[0] ) :
						import_gem_from_flink( nxvideosink->drm_fd, mm_buf->handle.gem[i] );

					pitches[i] = mm_buf->stride_width[i];
					offsets[i] = offset;

					offset += ( (mm_buf->handle_num == 1) ? (mm_buf->stride_width[i] * mm_buf->stride_height[i]) : 0 );
				}

				err = drmModeAddFB2( nxvideosink->drm_fd, mm_buf->width[0], mm_buf->height[0],
					nxvideosink->drm_format, handles, pitches, offsets, &nxvideosink->buffer_id[mm_buf->buffer_index], 0 );

				if( 0 > err )
				{
					GST_ERROR("Fail, drmModeAddFB2() ( %s ).\n", strerror(err) );
					gst_memory_unmap( meta_block, &info );
					gst_buffer_unref( buf );
					return GST_FLOW_ERROR;
				}

#if TEST_CONNECT
			int32_t connectorID = 43;
//			int32_t connectorID = 41;
			err = drmModeSetCrtc(nxvideosink->drm_fd, nxvideosink->crtc_id, nxvideosink->buffer_id[mm_buf->buffer_index], 0, 0,
					&connectorID, 1, (drmModeModeInfo *)nxvideosink->pCurrent_mode);
#endif
			}

			err = drmModeSetPlane( nxvideosink->drm_fd, nxvideosink->plane_id, nxvideosink->crtc_id, nxvideosink->buffer_id[mm_buf->buffer_index], 0,
					nxvideosink->dst_x, nxvideosink->dst_y, nxvideosink->dst_w, nxvideosink->dst_h,
					nxvideosink->src_x << 16, nxvideosink->src_y << 16, nxvideosink->src_w << 16, nxvideosink->src_h << 16 );

			if( 0 > err )
			{
				GST_ERROR("Fail, drmModeSetPlane() ( %s ).\n", strerror(err) );
				gst_memory_unmap( meta_block, &info );
				gst_buffer_unref( buf );
				return GST_FLOW_ERROR;
			}

			gst_memory_unmap( meta_block, &info );
		}
	}
	else
	{
		if( FALSE == nxvideosink->init )
		{
			gint i = 0;
			for( i = 0; i < MAX_ALLOC_BUFFER; i++ )
			{
				nxvideosink->video_memory[i] = allocate_buffer( nxvideosink->drm_fd, nxvideosink->width, nxvideosink->height, nxvideosink->drm_format );
				if( NULL == nxvideosink->video_memory[i] )
				{
					GST_ERROR("Fail, Allocate Buffer.\n");
					return FALSE;
				}
			}

			nxvideosink->init = TRUE;
		}

		copy_to_videomemory( buf, nxvideosink->video_memory[nxvideosink->index] );

		if( nxvideosink->buffer_id[nxvideosink->index] == 0 )
		{
			gint i = 0;
			guint handles[4] = { 0, };
			guint pitches[4] = { 0, };
			guint offsets[4] = { 0, };

			for( i = 0; i < nxvideosink->video_memory[nxvideosink->index]->planes; i++ )
			{
				handles[i] = nxvideosink->video_memory[nxvideosink->index]->gem_fd[i];
				pitches[i] = nxvideosink->video_memory[nxvideosink->index]->stride[i];
				offsets[i] = 0;
			}

			err = drmModeAddFB2( nxvideosink->drm_fd, nxvideosink->width, nxvideosink->height,
				nxvideosink->drm_format, handles, pitches, offsets, &nxvideosink->buffer_id[nxvideosink->index], 0 );

			if( 0 > err )
			{
				GST_ERROR("Fail, drmModeAddFB2() ( %d ).\n", err );
				gst_buffer_unref( buf );
				return GST_FLOW_ERROR;
			}
		}

		err = drmModeSetPlane( nxvideosink->drm_fd, nxvideosink->plane_id, nxvideosink->crtc_id, nxvideosink->buffer_id[nxvideosink->index], 0,
				nxvideosink->dst_x, nxvideosink->dst_y, nxvideosink->dst_w, nxvideosink->dst_h,
				nxvideosink->src_x << 16, nxvideosink->src_y << 16, nxvideosink->src_w << 16, nxvideosink->src_h << 16 );

		if( 0 > err )
		{
			GST_ERROR("Fail, drmModeSetPlane() ( %d ).\n", err );
			gst_buffer_unref( buf );
			return GST_FLOW_ERROR;
		}

		nxvideosink->index = (nxvideosink->index + 1) % MAX_ALLOC_BUFFER;
	}

	if( nxvideosink->prv_buf )
	{
		gst_buffer_unref( nxvideosink->prv_buf );
	}

	nxvideosink->prv_buf = buf;

	return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{

	/* FIXME Remember to set the rank if it's an element that is meant
		 to be autoplugged by decodebin. */
	return gst_element_register( plugin, "nxvideosink", GST_RANK_NONE,
			GST_TYPE_NXVIDEOSINK );
}

/* FIXME: these are normally defined by the GStreamer build system.
	 If you are creating an element to be included in gst-plugins-*,
	 remove these, as they're always defined.  Otherwise, edit as
	 appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.1.0"
#endif
#ifndef PACKAGE
#define PACKAGE "S5PXX18/NXP4330/NXP322X GStreamer PlugIn"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "S5PXX18/NXP4330/NXP322X GStreamer PlugIn"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://www.nexell.co.kr"
#endif

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	nxvideosink,
	"Nexell H/W Video Renderer for S5PXX18/NXP4330/NXP322X",
	plugin_init, VERSION,
	"LGPL",
	PACKAGE_NAME,
	GST_PACKAGE_ORIGIN
)

