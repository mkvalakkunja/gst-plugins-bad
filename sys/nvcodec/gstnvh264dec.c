/* GStreamer
 * Copyright (C) 2017 Ericsson AB. All rights reserved.
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
 *
 * Copyright 2015 The Chromium Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_NVCODEC_GST_GL
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#endif

#include "gstnvh264dec.h"
#include "gstcudautils.h"
#include "gstnvdecoder.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_nv_h264_dec_debug);
#define GST_CAT_DEFAULT gst_nv_h264_dec_debug

#ifdef HAVE_NVCODEC_GST_GL
#define SUPPORTED_GL_APIS (GST_GL_API_OPENGL | GST_GL_API_OPENGL3 | GST_GL_API_GLES2)
#endif

struct _GstNvH264Dec
{
  GstH264Decoder parent;

  GstVideoCodecState *output_state;

  const GstH264SPS *last_sps;
  const GstH264PPS *last_pps;

  GstCudaContext *context;
  CUstream cuda_stream;
  GstNvDecoder *decoder;
  CUVIDPICPARAMS params;

  /* slice buffer which will be passed to CUVIDPICPARAMS::pBitstreamData */
  guint8 *bitstream_buffer;
  /* allocated memory size of bitstream_buffer */
  gsize bitstream_buffer_alloc_size;
  /* current offset of bitstream_buffer (per frame) */
  gsize bitstream_buffer_offset;

  guint *slice_offsets;
  guint slice_offsets_alloc_len;
  guint num_slices;

  guint width, height;
  guint coded_width, coded_height;
  guint bitdepth;
  guint chroma_format_idc;
  GstVideoFormat out_format;

#ifdef HAVE_NVCODEC_GST_GL
  GstGLDisplay *gl_display;
  GstGLContext *gl_context;
  GstGLContext *other_gl_context;
#endif

  GstNvDecoderOutputType output_type;
};

struct _GstNvH264DecClass
{
  GstH264DecoderClass parent_class;
  guint cuda_device_id;
};

#define gst_nv_h264_dec_parent_class parent_class
G_DEFINE_TYPE (GstNvH264Dec, gst_nv_h264_dec, GST_TYPE_H264_DECODER);

static void gst_nv_h264_decoder_finalize (GObject * object);
static void gst_nv_h264_dec_set_context (GstElement * element,
    GstContext * context);
static gboolean gst_nv_h264_dec_open (GstVideoDecoder * decoder);
static gboolean gst_nv_h264_dec_close (GstVideoDecoder * decoder);
static gboolean gst_nv_h264_dec_negotiate (GstVideoDecoder * decoder);
static gboolean gst_nv_h264_dec_decide_allocation (GstVideoDecoder *
    decoder, GstQuery * query);
static gboolean gst_nv_h264_dec_src_query (GstVideoDecoder * decoder,
    GstQuery * query);

/* GstH264Decoder */
static gboolean gst_nv_h264_dec_new_sequence (GstH264Decoder * decoder,
    const GstH264SPS * sps, gint max_dpb_size);
static gboolean gst_nv_h264_dec_new_picture (GstH264Decoder * decoder,
    GstH264Picture * picture);
static GstFlowReturn gst_nv_h264_dec_output_picture (GstH264Decoder *
    decoder, GstH264Picture * picture);
static gboolean gst_nv_h264_dec_start_picture (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice, GstH264Dpb * dpb);
static gboolean gst_nv_h264_dec_decode_slice (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice);
static gboolean gst_nv_h264_dec_end_picture (GstH264Decoder * decoder,
    GstH264Picture * picture);

static void
gst_nv_h264_dec_class_init (GstNvH264DecClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstH264DecoderClass *h264decoder_class = GST_H264_DECODER_CLASS (klass);

  object_class->finalize = gst_nv_h264_decoder_finalize;

  element_class->set_context = GST_DEBUG_FUNCPTR (gst_nv_h264_dec_set_context);

  decoder_class->open = GST_DEBUG_FUNCPTR (gst_nv_h264_dec_open);
  decoder_class->close = GST_DEBUG_FUNCPTR (gst_nv_h264_dec_close);
  decoder_class->negotiate = GST_DEBUG_FUNCPTR (gst_nv_h264_dec_negotiate);
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_decide_allocation);
  decoder_class->src_query = GST_DEBUG_FUNCPTR (gst_nv_h264_dec_src_query);

  h264decoder_class->new_sequence =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_new_sequence);
  h264decoder_class->new_picture =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_new_picture);
  h264decoder_class->output_picture =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_output_picture);
  h264decoder_class->start_picture =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_start_picture);
  h264decoder_class->decode_slice =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_decode_slice);
  h264decoder_class->end_picture =
      GST_DEBUG_FUNCPTR (gst_nv_h264_dec_end_picture);

  GST_DEBUG_CATEGORY_INIT (gst_nv_h264_dec_debug,
      "nvh264dec", 0, "Nvidia H.264 Decoder");
}

static void
gst_nv_h264_dec_init (GstNvH264Dec * self)
{
}

static void
gst_nv_h264_decoder_finalize (GObject * object)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (object);

  g_free (self->bitstream_buffer);
  g_free (self->slice_offsets);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_nv_h264_dec_set_context (GstElement * element, GstContext * context)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (element);
  GstNvH264DecClass *klass = GST_NV_H264_DEC_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "set context %s",
      gst_context_get_context_type (context));

  if (gst_cuda_handle_set_context (element, context, klass->cuda_device_id,
          &self->context)) {
    goto done;
  }
#ifdef HAVE_NVCODEC_GST_GL
  gst_gl_handle_set_context (element, context, &self->gl_display,
      &self->other_gl_context);
#endif

done:
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_nv_h264_dec_open (GstVideoDecoder * decoder)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  CUresult cuda_ret;

  if (!gst_cuda_ensure_element_context (GST_ELEMENT_CAST (decoder),
          0, &self->context)) {
    GST_ERROR_OBJECT (self, "failed to create CUDA context");
    return FALSE;
  }

  if (gst_cuda_context_push (self->context)) {
    cuda_ret = CuStreamCreate (&self->cuda_stream, CU_STREAM_DEFAULT);
    if (!gst_cuda_result (cuda_ret)) {
      GST_WARNING_OBJECT (self,
          "Could not create CUDA stream, will use default stream");
      self->cuda_stream = NULL;
    }
    gst_cuda_context_pop (NULL);
  }
#if HAVE_NVCODEC_GST_GL
  gst_gl_ensure_element_data (GST_ELEMENT (self),
      &self->gl_display, &self->other_gl_context);
  if (self->gl_display)
    gst_gl_display_filter_gl_api (GST_GL_DISPLAY (self->gl_display),
        SUPPORTED_GL_APIS);
#endif

  return TRUE;
}

static gboolean
gst_nv_h264_dec_close (GstVideoDecoder * decoder)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);

  g_clear_pointer (&self->output_state, gst_video_codec_state_unref);
  gst_clear_object (&self->decoder);

  if (self->context && self->cuda_stream) {
    if (gst_cuda_context_push (self->context)) {
      gst_cuda_result (CuStreamDestroy (self->cuda_stream));
      gst_cuda_context_pop (NULL);
    }
  }
#ifdef HAVE_NVCODEC_GST_GL
  gst_clear_object (&self->gl_context);
  gst_clear_object (&self->other_gl_context);
  gst_clear_object (&self->gl_display);
#endif

  gst_clear_object (&self->context);
  self->cuda_stream = NULL;

  return TRUE;
}

#ifdef HAVE_NVCODEC_GST_GL
static void
gst_nv_h264_dec_check_cuda_device_from_context (GstGLContext * context,
    gboolean * ret)
{
  guint device_count = 0;
  CUdevice device_list[1] = { 0, };
  CUresult cuda_ret;

  *ret = FALSE;

  cuda_ret = CuGLGetDevices (&device_count,
      device_list, 1, CU_GL_DEVICE_LIST_ALL);

  if (!gst_cuda_result (cuda_ret) || device_count == 0)
    return;

  *ret = TRUE;

  return;
}

static gboolean
gst_nv_h264_dec_ensure_gl_context (GstNvH264Dec * self)
{
  gboolean ret;

  if (!self->gl_display) {
    GST_DEBUG_OBJECT (self, "No available OpenGL display");
    return FALSE;
  }

  if (!gst_gl_query_local_gl_context (GST_ELEMENT (self), GST_PAD_SRC,
          &self->gl_context)) {
    GST_INFO_OBJECT (self, "failed to query local OpenGL context");
    if (self->gl_context)
      gst_object_unref (self->gl_context);
    self->gl_context =
        gst_gl_display_get_gl_context_for_thread (self->gl_display, NULL);
    if (!self->gl_context
        || !gst_gl_display_add_context (self->gl_display, self->gl_context)) {
      if (self->gl_context)
        gst_object_unref (self->gl_context);
      if (!gst_gl_display_create_context (self->gl_display,
              self->other_gl_context, &self->gl_context, NULL)) {
        GST_ERROR_OBJECT (self, "failed to create OpenGL context");
        return FALSE;
      }
      if (!gst_gl_display_add_context (self->gl_display, self->gl_context)) {
        GST_ERROR_OBJECT (self,
            "failed to add the OpenGL context to the display");
        return FALSE;
      }
    }
  }

  if (!gst_gl_context_check_gl_version (self->gl_context,
          SUPPORTED_GL_APIS, 3, 0)) {
    GST_WARNING_OBJECT (self, "OpenGL context could not support PBO download");
    return FALSE;
  }

  gst_gl_context_thread_add (self->gl_context,
      (GstGLContextThreadFunc) gst_nv_h264_dec_check_cuda_device_from_context,
      &ret);

  if (!ret) {
    GST_WARNING_OBJECT (self, "Current OpenGL context is not CUDA-compatible");
    return FALSE;
  }

  return TRUE;
}

#endif

static gboolean
gst_nv_h264_dec_negotiate (GstVideoDecoder * decoder)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  GstH264Decoder *h264dec = GST_H264_DECODER (decoder);
  GstVideoCodecState *state;

  GST_DEBUG_OBJECT (self, "negotiate");

  state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      self->out_format, self->width, self->height, h264dec->input_state);

  state->caps = gst_video_info_to_caps (&state->info);

  self->output_type = GST_NV_DECOCER_OUTPUT_TYPE_SYSTEM;

#ifdef HAVE_NVCODEC_GST_GL
  {
    GstCaps *caps;
    caps = gst_pad_get_allowed_caps (GST_VIDEO_DECODER_SRC_PAD (self));
    GST_DEBUG_OBJECT (self, "Allowed caps %" GST_PTR_FORMAT, caps);

    if (!caps || gst_caps_is_any (caps)) {
      GST_DEBUG_OBJECT (self,
          "cannot determine output format, use system memory");
    } else if (self->gl_display) {
      GstCapsFeatures *features;
      guint size = gst_caps_get_size (caps);
      guint i;

      for (i = 0; i < size; i++) {
        features = gst_caps_get_features (caps, i);
        if (features && gst_caps_features_contains (features,
                GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
          GST_DEBUG_OBJECT (self, "found GL memory feature, use gl");
          self->output_type = GST_NV_DECOCER_OUTPUT_TYPE_GL;
          break;
        }
      }
    }
    gst_clear_caps (&caps);
  }

  if (self->output_type == GST_NV_DECOCER_OUTPUT_TYPE_GL &&
      !gst_nv_h264_dec_ensure_gl_context (self)) {
    GST_WARNING_OBJECT (self,
        "OpenGL context is not CUDA-compatible, fallback to system memory");
    self->output_type = GST_NV_DECOCER_OUTPUT_TYPE_SYSTEM;
  }

  if (self->output_type == GST_NV_DECOCER_OUTPUT_TYPE_GL) {
    gst_caps_set_features (state->caps, 0,
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
    gst_caps_set_simple (state->caps, "texture-target", G_TYPE_STRING,
        "2D", NULL);
  } else {
    GST_DEBUG_OBJECT (self, "use system memory");
  }
#endif

  if (self->output_state)
    gst_video_codec_state_unref (self->output_state);

  self->output_state = state;

  /* TODO: add support D3D11 memory */

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static gboolean
gst_nv_h264_dec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
#ifdef HAVE_NVCODEC_GST_GL
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  GstCaps *outcaps;
  GstBufferPool *pool = NULL;
  guint n, size, min, max;
  GstVideoInfo vinfo = { 0, };
  GstStructure *config;

  GST_DEBUG_OBJECT (self, "decide allocation");

  if (self->output_type == GST_NV_DECOCER_OUTPUT_TYPE_SYSTEM)
    return GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation
        (decoder, query);

  gst_query_parse_allocation (query, &outcaps, NULL);
  n = gst_query_get_n_allocation_pools (query);
  if (n > 0)
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

  if (pool && !GST_IS_GL_BUFFER_POOL (pool)) {
    gst_object_unref (pool);
    pool = NULL;
  }

  if (!pool) {
    pool = gst_gl_buffer_pool_new (self->gl_context);

    if (outcaps)
      gst_video_info_from_caps (&vinfo, outcaps);
    size = (guint) vinfo.size;
    min = max = 0;
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_set_config (pool, config);
  if (n > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);
  gst_object_unref (pool);
#endif

  return GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation
      (decoder, query);
}

static gboolean
gst_nv_h264_dec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      if (gst_cuda_handle_context_query (GST_ELEMENT (decoder),
              query, self->context)) {
        return TRUE;
      }
#ifdef HAVE_NVCODEC_GST_GL
      if (gst_gl_handle_context_query (GST_ELEMENT (decoder), query,
              self->gl_display, self->gl_context, self->other_gl_context)) {
        if (self->gl_display)
          gst_gl_display_filter_gl_api (GST_GL_DISPLAY (self->gl_display),
              SUPPORTED_GL_APIS);
        return TRUE;
      }
#endif
      break;
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->src_query (decoder, query);
}

static gboolean
gst_nv_h264_dec_new_sequence (GstH264Decoder * decoder, const GstH264SPS * sps,
    gint max_dpb_size)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  gint crop_width, crop_height;
  gboolean modified = FALSE;

  GST_LOG_OBJECT (self, "new sequence");

  if (sps->frame_cropping_flag) {
    crop_width = sps->crop_rect_width;
    crop_height = sps->crop_rect_height;
  } else {
    crop_width = sps->width;
    crop_height = sps->height;
  }

  if (self->width != crop_width || self->height != crop_height ||
      self->coded_width != sps->width || self->coded_height != sps->height) {
    GST_INFO_OBJECT (self, "resolution changed %dx%d (%dx%d)",
        crop_width, crop_height, sps->width, sps->height);
    self->width = crop_width;
    self->height = crop_height;
    self->coded_width = sps->width;
    self->coded_height = sps->height;
    modified = TRUE;
  }

  if (self->bitdepth != sps->bit_depth_luma_minus8 + 8) {
    GST_INFO_OBJECT (self, "bitdepth changed");
    self->bitdepth = sps->bit_depth_luma_minus8 + 8;
    modified = TRUE;
  }

  if (self->chroma_format_idc != sps->chroma_format_idc) {
    GST_INFO_OBJECT (self, "chroma format changed");
    self->chroma_format_idc = sps->chroma_format_idc;
    modified = TRUE;
  }

  if (modified || !self->decoder) {
    GstVideoInfo info;

    self->out_format = GST_VIDEO_FORMAT_UNKNOWN;

    if (self->bitdepth == 8) {
      if (self->chroma_format_idc == 1)
        self->out_format = GST_VIDEO_FORMAT_NV12;
      else {
        GST_FIXME_OBJECT (self, "Could not support 8bits non-4:2:0 format");
      }
    } else if (self->bitdepth == 10) {
      if (self->chroma_format_idc == 1)
        self->out_format = GST_VIDEO_FORMAT_P010_10LE;
      else {
        GST_FIXME_OBJECT (self, "Could not support 10bits non-4:2:0 format");
      }
    }

    if (self->out_format == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_ERROR_OBJECT (self, "Could not support bitdepth/chroma format");
      return FALSE;
    }

    gst_clear_object (&self->decoder);

    gst_video_info_set_format (&info,
        self->out_format, self->width, self->height);

    /* FIXME: add support cudaVideoCodec_H264_SVC and cudaVideoCodec_H264_MVC */
    self->decoder = gst_nv_decoder_new (self->context, cudaVideoCodec_H264,
        &info,
        /* Additional 2 buffers for margin */
        max_dpb_size + 2);

    if (!self->decoder) {
      GST_ERROR_OBJECT (self, "Failed to create decoder");
      return FALSE;
    }

    if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to negotiate with downstream");
      return FALSE;
    }

    self->last_sps = NULL;
    self->last_pps = NULL;
    memset (&self->params, 0, sizeof (CUVIDPICPARAMS));
  }

  return TRUE;
}

static gboolean
gst_nv_h264_dec_new_picture (GstH264Decoder * decoder, GstH264Picture * picture)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  GstNvDecoderFrame *frame;

  frame = gst_nv_decoder_new_frame (self->decoder);
  if (!frame) {
    GST_ERROR_OBJECT (self, "No available decoder frame");
    return FALSE;
  }

  GST_LOG_OBJECT (self, "New decoder frame %p (index %d)", frame, frame->index);

  gst_h264_picture_set_user_data (picture,
      frame, (GDestroyNotify) gst_nv_decoder_frame_free);

  return TRUE;
}

static GstFlowReturn
gst_nv_h264_dec_output_picture (GstH264Decoder * decoder,
    GstH264Picture * picture)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  GstVideoCodecFrame *frame = NULL;
  GstBuffer *output_buffer = NULL;
  GstNvDecoderFrame *decoder_frame;
  gboolean ret = FALSE;

  GST_LOG_OBJECT (self,
      "Outputting picture %p (poc %d)", picture, picture->pic_order_cnt);

  decoder_frame =
      (GstNvDecoderFrame *) gst_h264_picture_get_user_data (picture);
  if (!decoder_frame) {
    GST_ERROR_OBJECT (self, "No decoder frame in picture %p", picture);
    return GST_FLOW_ERROR;
  }

  frame = gst_video_decoder_get_frame (GST_VIDEO_DECODER (self),
      picture->system_frame_number);
  if (!frame) {
    GST_ERROR_OBJECT (self, "Failed to retrieve codec frame");
    return GST_FLOW_ERROR;
  }

  output_buffer =
      gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));
  frame->output_buffer = output_buffer;

#ifdef HAVE_NVCODEC_GST_GL
  if (self->output_type == GST_NV_DECOCER_OUTPUT_TYPE_GL) {
    ret = gst_nv_decoder_finish_frame (self->decoder,
        GST_NV_DECOCER_OUTPUT_TYPE_GL, GST_OBJECT (self->gl_context),
        decoder_frame, output_buffer);

    /* FIXME: This is the case where OpenGL context of downstream glbufferpool
     * belongs to non-nvidia (or different device).
     * There should be enhancement to ensure nvdec has compatible OpenGL context
     */
    if (!ret) {
      GST_WARNING_OBJECT (self,
          "Couldn't copy frame to GL memory, fallback to system memory");
      self->output_type = GST_NV_DECOCER_OUTPUT_TYPE_SYSTEM;
    }
  }

  if (!ret)
#endif
  {
    if (!gst_nv_decoder_finish_frame (self->decoder,
            GST_NV_DECOCER_OUTPUT_TYPE_SYSTEM, NULL, decoder_frame,
            output_buffer)) {
      GST_ERROR_OBJECT (self, "Failed to finish frame");
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
      return GST_FLOW_ERROR;
    }
  }

  return gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
}

static GstNvDecoderFrame *
gst_nv_h264_dec_get_decoder_frame_from_picture (GstNvH264Dec * self,
    GstH264Picture * picture)
{
  GstNvDecoderFrame *frame;

  frame = (GstNvDecoderFrame *) gst_h264_picture_get_user_data (picture);

  if (!frame)
    GST_DEBUG_OBJECT (self, "current picture does not have decoder frame");

  return frame;
}

static void
gst_nv_h264_dec_picture_params_from_sps (GstNvH264Dec * self,
    const GstH264SPS * sps, gboolean field_pic, CUVIDH264PICPARAMS * params)
{
  params->residual_colour_transform_flag = sps->separate_colour_plane_flag;
  params->MbaffFrameFlag = sps->mb_adaptive_frame_field_flag && field_pic;

#define COPY_FIELD(f) \
  (params)->f = (sps)->f

  COPY_FIELD (log2_max_frame_num_minus4);
  COPY_FIELD (pic_order_cnt_type);
  COPY_FIELD (log2_max_pic_order_cnt_lsb_minus4);
  COPY_FIELD (delta_pic_order_always_zero_flag);
  COPY_FIELD (frame_mbs_only_flag);
  COPY_FIELD (direct_8x8_inference_flag);
  COPY_FIELD (num_ref_frames);
  COPY_FIELD (bit_depth_luma_minus8);
  COPY_FIELD (bit_depth_chroma_minus8);
  COPY_FIELD (qpprime_y_zero_transform_bypass_flag);

#undef COPY_FIELD
}

static void
gst_nv_h264_dec_picture_params_from_pps (GstNvH264Dec * self,
    const GstH264PPS * pps, CUVIDH264PICPARAMS * params)
{
  params->second_chroma_qp_index_offset =
      (gint8) pps->second_chroma_qp_index_offset;

#define COPY_FIELD(f) \
  (params)->f = (pps)->f

  COPY_FIELD (entropy_coding_mode_flag);
  COPY_FIELD (pic_order_present_flag);
  COPY_FIELD (num_ref_idx_l0_active_minus1);
  COPY_FIELD (num_ref_idx_l1_active_minus1);
  COPY_FIELD (pic_init_qp_minus26);
  COPY_FIELD (weighted_pred_flag);
  COPY_FIELD (weighted_bipred_idc);
  COPY_FIELD (pic_init_qp_minus26);
  COPY_FIELD (deblocking_filter_control_present_flag);
  COPY_FIELD (redundant_pic_cnt_present_flag);
  COPY_FIELD (transform_8x8_mode_flag);
  COPY_FIELD (constrained_intra_pred_flag);
  COPY_FIELD (chroma_qp_index_offset);
#undef COPY_FIELD

  /* h264parser will copy scaling list from sps to pps regardless of
   * pic_scaling_matrix_present_flag */
  g_assert (sizeof (params->WeightScale4x4) == sizeof (pps->scaling_lists_4x4));
  g_assert (sizeof (params->WeightScale8x8[0]) ==
      sizeof (pps->scaling_lists_8x8[0]));
  memcpy (params->WeightScale4x4, pps->scaling_lists_4x4,
      sizeof (params->WeightScale4x4));
  memcpy (params->WeightScale8x8[0], pps->scaling_lists_8x8[0],
      sizeof (params->WeightScale8x8[0]));
  memcpy (params->WeightScale8x8[1], pps->scaling_lists_8x8[3],
      sizeof (params->WeightScale8x8[0]));
}

static void
gst_nv_h264_dec_reset_bitstream_params (GstNvH264Dec * self)
{
  self->bitstream_buffer_offset = 0;
  self->num_slices = 0;

  self->params.nBitstreamDataLen = 0;
  self->params.pBitstreamData = NULL;
  self->params.nNumSlices = 0;
  self->params.pSliceDataOffsets = NULL;
}

static gboolean
gst_nv_h264_dec_start_picture (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice, GstH264Dpb * dpb)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  CUVIDPICPARAMS *params = &self->params;
  CUVIDH264PICPARAMS *h264_params = &params->CodecSpecific.h264;
  const GstH264SliceHdr *slice_header = &slice->header;
  const GstH264SPS *sps;
  const GstH264PPS *pps;
  GstNvDecoderFrame *frame;
  GArray *dpb_array;
  gint i;

  g_return_val_if_fail (slice_header->pps != NULL, FALSE);
  g_return_val_if_fail (slice_header->pps->sequence != NULL, FALSE);

  frame = gst_nv_h264_dec_get_decoder_frame_from_picture (self, picture);

  if (!frame) {
    GST_ERROR_OBJECT (self,
        "Couldn't get decoder frame frame picture %p", picture);
    return FALSE;
  }

  gst_nv_h264_dec_reset_bitstream_params (self);

  sps = slice_header->pps->sequence;
  pps = slice_header->pps;

  /* FIXME: update sps/pps related params only when it's required */
  params->PicWidthInMbs = sps->pic_width_in_mbs_minus1 + 1;
  params->FrameHeightInMbs = sps->pic_height_in_map_units_minus1 + 1;
  params->CurrPicIdx = frame->index;
  /* TODO: verifiy interlaced */
  params->field_pic_flag = picture->field != GST_H264_PICTURE_FIELD_FRAME;
  params->bottom_field_flag =
      picture->field == GST_H264_PICTURE_FIELD_BOTTOM_FIELD;
  /* TODO: set second_field here */
  params->second_field = 0;

  /* nBitstreamDataLen, pBitstreamData, nNumSlices and pSliceDataOffsets
   * will be set later */

  params->ref_pic_flag = picture->ref;
  /* will be updated later, if any slices belong to this frame is not
   * intra slice */
  params->intra_pic_flag = 1;

  h264_params->frame_num = picture->frame_num;
  h264_params->ref_pic_flag = picture->ref;
  /* FIXME: should be updated depending on field type? */
  h264_params->CurrFieldOrderCnt[0] = picture->top_field_order_cnt;
  h264_params->CurrFieldOrderCnt[1] = picture->bottom_field_order_cnt;

  if (!self->last_sps || self->last_sps != sps) {
    GST_DEBUG_OBJECT (self, "Update params from SPS and PPS");
    gst_nv_h264_dec_picture_params_from_sps (self,
        sps, slice_header->field_pic_flag, h264_params);
    gst_nv_h264_dec_picture_params_from_pps (self, pps, h264_params);
    self->last_sps = sps;
    self->last_pps = pps;
  } else if (!self->last_pps || self->last_pps != pps) {
    GST_DEBUG_OBJECT (self, "Update params from PPS");
    gst_nv_h264_dec_picture_params_from_pps (self, pps, h264_params);
    self->last_pps = pps;
  } else {
    GST_TRACE_OBJECT (self, "SPS and PPS were not updated");
  }

  memset (&h264_params->dpb, 0, sizeof (h264_params->dpb));
  for (i = 0; i < G_N_ELEMENTS (h264_params->dpb); i++)
    h264_params->dpb[i].PicIdx = -1;

  dpb_array = gst_h264_dpb_get_pictures_all (dpb);
  for (i = 0; i < dpb_array->len && i < G_N_ELEMENTS (h264_params->dpb); i++) {
    GstH264Picture *other = g_array_index (dpb_array, GstH264Picture *, i);
    GstNvDecoderFrame *other_frame;
    gint picture_index = -1;
    CUVIDH264DPBENTRY *dpb = &h264_params->dpb[i];

    if (!other->ref)
      continue;

    other_frame = gst_nv_h264_dec_get_decoder_frame_from_picture (self, other);

    if (other_frame)
      picture_index = other_frame->index;

    dpb->PicIdx = picture_index;
    if (other->long_term) {
      dpb->FrameIdx = other->long_term_frame_idx;
      dpb->is_long_term = 1;
    } else {
      dpb->FrameIdx = other->frame_num;
      dpb->is_long_term = 0;
    }

    dpb->not_existing = other->nonexisting;
    if (dpb->not_existing && dpb->PicIdx != -1) {
      GST_WARNING_OBJECT (self,
          "Non-existing frame has valid picture index %d", dpb->PicIdx);
      dpb->PicIdx = -1;
    }

    /* FIXME: 1=top_field, 2=bottom_field, 3=both_fields */
    dpb->used_for_reference = 3;

    dpb->FieldOrderCnt[0] = other->top_field_order_cnt;
    dpb->FieldOrderCnt[1] = other->bottom_field_order_cnt;
  }

  g_array_unref (dpb_array);

  return TRUE;
}

static gboolean
gst_nv_h264_dec_decode_slice (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  gsize new_size;

  GST_LOG_OBJECT (self, "Decode slice, nalu size %u", slice->nalu.size);

  if (self->slice_offsets_alloc_len < self->num_slices + 1) {
    self->slice_offsets = (guint *) g_realloc_n (self->slice_offsets,
        self->num_slices + 1, sizeof (guint));
  }
  self->slice_offsets[self->num_slices] = self->bitstream_buffer_offset;
  GST_LOG_OBJECT (self, "Slice offset %u for slice %d",
      self->slice_offsets[self->num_slices], self->num_slices);

  self->num_slices++;

  new_size = self->bitstream_buffer_offset + slice->nalu.size + 3;
  if (self->bitstream_buffer_alloc_size < new_size) {
    self->bitstream_buffer =
        (guint8 *) g_realloc (self->bitstream_buffer, new_size);
  }

  self->bitstream_buffer[self->bitstream_buffer_offset] = 0;
  self->bitstream_buffer[self->bitstream_buffer_offset + 1] = 0;
  self->bitstream_buffer[self->bitstream_buffer_offset + 2] = 1;

  memcpy (self->bitstream_buffer + self->bitstream_buffer_offset + 3,
      slice->nalu.data + slice->nalu.offset, slice->nalu.size);
  self->bitstream_buffer_offset = new_size;

  if (!GST_H264_IS_I_SLICE (&slice->header) &&
      !GST_H264_IS_SI_SLICE (&slice->header))
    self->params.intra_pic_flag = 0;

  return TRUE;
}

static gboolean
gst_nv_h264_dec_end_picture (GstH264Decoder * decoder, GstH264Picture * picture)
{
  GstNvH264Dec *self = GST_NV_H264_DEC (decoder);
  gboolean ret;
  CUVIDPICPARAMS *params = &self->params;

  params->nBitstreamDataLen = self->bitstream_buffer_offset;
  params->pBitstreamData = self->bitstream_buffer;
  params->nNumSlices = self->num_slices;
  params->pSliceDataOffsets = self->slice_offsets;

  GST_LOG_OBJECT (self, "End picture, bitstream len: %" G_GSIZE_FORMAT
      ", num slices %d", self->bitstream_buffer_offset, self->num_slices);

  ret = gst_nv_decoder_decode_picture (self->decoder, &self->params);

  if (!ret)
    GST_ERROR_OBJECT (self, "Failed to decode picture");

  return ret;
}

typedef struct
{
  GstCaps *sink_caps;
  GstCaps *src_caps;
  guint cuda_device_id;
  gboolean is_default;
} GstNvH264DecClassData;

static void
gst_nv_h264_dec_subclass_init (gpointer klass, GstNvH264DecClassData * cdata)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstNvH264DecClass *nvdec_class = (GstNvH264DecClass *) (klass);
  gchar *long_name;

  if (cdata->is_default) {
    long_name = g_strdup_printf ("NVDEC H.264 Stateless Decoder");
  } else {
    long_name = g_strdup_printf ("NVDEC H.264 Stateless Decoder with device %d",
        cdata->cuda_device_id);
  }

  gst_element_class_set_metadata (element_class, long_name,
      "Codec/Decoder/Video/Hardware",
      "Nvidia H.264 video decoder", "Seungha Yang <seungha@centricular.com>");
  g_free (long_name);

  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          cdata->sink_caps));
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          cdata->src_caps));

  nvdec_class->cuda_device_id = cdata->cuda_device_id;

  gst_caps_unref (cdata->sink_caps);
  gst_caps_unref (cdata->src_caps);
  g_free (cdata);
}

void
gst_nv_h264_dec_register (GstPlugin * plugin, guint device_id, guint rank,
    GstCaps * sink_caps, GstCaps * src_caps, gboolean is_primary)
{
  GTypeQuery type_query;
  GTypeInfo type_info = { 0, };
  GType subtype;
  gchar *type_name;
  gchar *feature_name;
  GstNvH264DecClassData *cdata;
  gboolean is_default = TRUE;
  const GValue *value;
  GstStructure *s;

  cdata = g_new0 (GstNvH264DecClassData, 1);
  cdata->sink_caps = gst_caps_from_string ("video/x-h264, "
      "stream-format= (string) { avc, avc3, byte-stream }, "
      "alignment= (string) au, profile = (string) { high, main }, "
      "framerate = " GST_VIDEO_FPS_RANGE);

  s = gst_caps_get_structure (sink_caps, 0);
  value = gst_structure_get_value (s, "width");
  gst_caps_set_value (cdata->sink_caps, "width", value);

  value = gst_structure_get_value (s, "height");
  gst_caps_set_value (cdata->sink_caps, "height", value);

  GST_MINI_OBJECT_FLAG_SET (cdata->sink_caps,
      GST_MINI_OBJECT_FLAG_MAY_BE_LEAKED);
  cdata->src_caps = gst_caps_ref (src_caps);
  cdata->cuda_device_id = device_id;

  g_type_query (GST_TYPE_NV_H264_DEC, &type_query);
  memset (&type_info, 0, sizeof (type_info));
  type_info.class_size = type_query.class_size;
  type_info.instance_size = type_query.instance_size;
  type_info.class_init = (GClassInitFunc) gst_nv_h264_dec_subclass_init;
  type_info.class_data = cdata;

  if (is_primary) {
    type_name = g_strdup ("GstNvH264StatelessPrimaryDec");
    feature_name = g_strdup ("nvh264dec");
  } else {
    type_name = g_strdup ("GstNvH264StatelessDec");
    feature_name = g_strdup ("nvh264sldec");
  }

  if (g_type_from_name (type_name) != 0) {
    g_free (type_name);
    g_free (feature_name);
    if (is_primary) {
      type_name =
          g_strdup_printf ("GstNvH264StatelessPrimaryDevice%dDec", device_id);
      feature_name = g_strdup_printf ("nvh264device%ddec", device_id);
    } else {
      type_name = g_strdup_printf ("GstNvH264StatelessDevice%dDec", device_id);
      feature_name = g_strdup_printf ("nvh264sldevice%ddec", device_id);
    }

    is_default = FALSE;
  }

  cdata->is_default = is_default;
  subtype = g_type_register_static (GST_TYPE_NV_H264_DEC,
      type_name, &type_info, 0);

  /* make lower rank than default device */
  if (rank > 0 && !is_default)
    rank--;

  if (!gst_element_register (plugin, feature_name, rank, subtype))
    GST_WARNING ("Failed to register plugin '%s'", type_name);

  g_free (type_name);
  g_free (feature_name);
}
