/**
 * Copyright (C) 2011-2016 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   webmdmuxfltprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia - WebM demuxer processor
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>

#include <OMX_TizoniaExt.h>

#include <tizplatform.h>

#include <tizkernel.h>
#include <tizscheduler.h>

#include "webmdmux.h"
#include "webmdmuxfltprc.h"
#include "webmdmuxfltprc_decls.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.webm_demuxer.prc"
#endif

/* Forward declarations */
static OMX_ERRORTYPE webmdmuxflt_prc_deallocate_resources (void *);
static OMX_ERRORTYPE release_buffer (webmdmuxflt_prc_t *);
static OMX_ERRORTYPE prepare_for_port_auto_detection (
    webmdmuxflt_prc_t *ap_prc);
static OMX_ERRORTYPE store_data (webmdmuxflt_prc_t *ap_prc);

#define on_nestegg_error_ret_omx_oom(expr)                            \
  do                                                                  \
    {                                                                 \
      int nestegg_error = 0;                                          \
      if (0 != (nestegg_error = (expr)))                              \
        {                                                             \
          TIZ_ERROR (handleOf (p_prc),                                \
                     "[OMX_ErrorInsufficientResources] : while using" \
                     "libnestegg");                                   \
          return OMX_ErrorInsufficientResources;                      \
        }                                                             \
    }                                                                 \
  while (0)

/** User supplied read callback.

    @param buffer   Buffer to read data into.
    @param length   Length of supplied buffer in bytes.
    @param userdata The #userdata supplied by the user.

    @retval  1 Read succeeded.
    @retval  0 End of stream.
    @retval -1 Error.
    */
static int ne_io_read (void *ap_buffer, size_t a_length, void *a_userdata)
{
  webmdmuxflt_prc_t *p_prc = a_userdata;
  int retval = -1;

  assert (a_userdata);
  assert (ap_buffer);

  (void)store_data (p_prc);

  if (ap_buffer && a_length > 0)
    {
      TIZ_TRACE (handleOf (p_prc),
                 "demuxer_inited_ [%s] store bytes [%d] offset [%d]",
                 (p_prc->demuxer_inited_ ? "YES" : "NO"),
                 tiz_buffer_available (p_prc->p_store_), p_prc->store_offset_);

      if (tiz_buffer_available (p_prc->p_store_) > 0)
        {
          int bytes_read = MIN (a_length, tiz_buffer_available (p_prc->p_store_)
                                              - p_prc->store_offset_);
          memcpy (ap_buffer,
                  tiz_buffer_get (p_prc->p_store_) + p_prc->store_offset_,
                  bytes_read);
          if (p_prc->demuxer_inited_)
            {
              tiz_buffer_advance (p_prc->p_store_, bytes_read);
            }
          else
            {
              p_prc->store_offset_ += bytes_read;
            }
          retval = 1;
        }
      else
        {
          TIZ_TRACE (handleOf (p_prc), "Run out of compressed data");
        }
    }

  return retval;
}

/** User supplied seek callback.

    @param offset   Offset within the stream to seek to.
    @param whence   Seek direction.  One of #NESTEGG_SEEK_SET,
                    #NESTEGG_SEEK_CUR, or #NESTEGG_SEEK_END.
    @param userdata The #userdata supplied by the user.

    @retval  0 Seek succeeded.
    @retval -1 Error.
    */
static int ne_io_seek (int64_t offset, int whence, void *userdata)
{
  return 0;
}

/** User supplied tell callback.

    @param userdata The #userdata supplied by the user.
    @returns Current position within the stream.
    @retval -1 Error.
*/
static int64_t ne_io_tell (void *userdata)
{
  return 0;
}

/** nestegg logging callback function. */
static void ne_log_cback (nestegg *ctx, unsigned int severity, char const *fmt,
                          ...)
{
  va_list ap;
  char const *sev = NULL;

#if !defined(DEBUG)
  if (severity < NESTEGG_LOG_WARNING)
    return;
#endif

  switch (severity)
    {
      case NESTEGG_LOG_DEBUG:
        sev = "debug:   ";
        break;
      case NESTEGG_LOG_WARNING:
        sev = "warning: ";
        break;
      case NESTEGG_LOG_CRITICAL:
        sev = "critical:";
        break;
      default:
        sev = "unknown: ";
    }

  fprintf (stderr, "%p %s ", (void *)ctx, sev);

  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);

  fprintf (stderr, "\n");
}

static OMX_ERRORTYPE release_buffer (webmdmuxflt_prc_t *ap_prc)
{
  assert (ap_prc);

  if (ap_prc->p_outhdr_)
    {
      TIZ_NOTICE (handleOf (ap_prc), "releasing HEADER [%p] nFilledLen [%d]",
                  ap_prc->p_outhdr_, ap_prc->p_outhdr_->nFilledLen);
      tiz_check_omx_err (tiz_krn_release_buffer (
          tiz_get_krn (handleOf (ap_prc)),
          ARATELIA_WEBM_DEMUXER_FILTER_PORT_0_INDEX, ap_prc->p_outhdr_));
      ap_prc->p_outhdr_ = NULL;
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE prepare_for_port_auto_detection (webmdmuxflt_prc_t *ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  /* Prepare audio port */
  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
  tiz_check_omx_err (
      tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                            OMX_IndexParamPortDefinition, &port_def));
  ap_prc->audio_coding_type_ = port_def.format.audio.eEncoding;
  ap_prc->audio_auto_detect_on_
      = (OMX_AUDIO_CodingAutoDetect == ap_prc->audio_coding_type_) ? true
                                                                   : false;

  /* Prepare video port */
  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX);
  tiz_check_omx_err (
      tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                            OMX_IndexParamPortDefinition, &port_def));
  ap_prc->video_coding_type_ = port_def.format.video.eCompressionFormat;
  ap_prc->video_auto_detect_on_
      = (OMX_VIDEO_CodingAutoDetect == ap_prc->video_coding_type_) ? true
                                                                   : false;

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE release_input_header (webmdmuxflt_prc_t *ap_prc)
{
  OMX_BUFFERHEADERTYPE *p_in = tiz_filter_prc_get_header (
      ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_0_INDEX);

  assert (ap_prc);

  if (p_in)
    {
      if ((p_in->nFlags & OMX_BUFFERFLAG_EOS) > 0)
        {
          /* Let's propagate EOS flag to output */
          TIZ_TRACE (handleOf (ap_prc), "Propagating EOS flag to output");
          OMX_BUFFERHEADERTYPE *p_out = tiz_filter_prc_get_header (
              ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
          if (p_out)
            {
              p_out->nFlags |= OMX_BUFFERFLAG_EOS;
            }
          tiz_filter_prc_update_eos_flag (ap_prc, true);
          p_in->nFlags &= ~(1 << OMX_BUFFERFLAG_EOS);
        }
      tiz_filter_prc_release_header (ap_prc,
                                     ARATELIA_WEBM_DEMUXER_FILTER_PORT_0_INDEX);
    }
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE store_data (webmdmuxflt_prc_t *ap_prc)
{
  bool rc = OMX_ErrorNone;

  if ((tiz_buffer_available (ap_prc->p_store_) - ap_prc->store_offset_)
      < ARATELIA_WEBM_DEMUXER_WEBM_PORT_MIN_BUF_SIZE)
    {
      OMX_BUFFERHEADERTYPE *p_in = tiz_filter_prc_get_header (
          ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_0_INDEX);

      assert (ap_prc);

      if (p_in)
        {
          TIZ_TRACE (handleOf (ap_prc), "store available [%d]",
                     tiz_buffer_available (ap_prc->p_store_));
          if (tiz_buffer_push (ap_prc->p_store_, p_in->pBuffer + p_in->nOffset,
                               p_in->nFilledLen)
              < p_in->nFilledLen)
            {
              TIZ_ERROR (handleOf (ap_prc),
                         "[OMX_ErrorInsufficientResources] : Unable to store "
                         "all the data.");
              rc = OMX_ErrorInsufficientResources;
            }
          release_input_header (ap_prc);
        }
    }
  return rc;
}

static OMX_ERRORTYPE extract_data (webmdmuxflt_prc_t *ap_prc,
                                   const unsigned int a_track,
                                   OMX_U32 a_pid)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *p_hdr = tiz_filter_prc_get_header (
      ap_prc, a_pid);

  if (p_hdr)
    {
      unsigned int chunks = 0;
      unsigned char *p_data = NULL;
      size_t data_size = 0;

      assert (ap_prc);
      assert (ap_prc->p_ne_pkt_);

      nestegg_packet_count (ap_prc->p_ne_pkt_, &chunks);

      /* Extract as many chunks of data as possible. */
      assert (ap_prc->ne_chunk_ <= chunks);
      while (ap_prc->ne_chunk_ < chunks
             && (nestegg_packet_data (ap_prc->p_ne_pkt_, ap_prc->ne_chunk_,
                                      &p_data, &data_size)
                 > 0)
             && TIZ_OMX_BUF_AVAIL (p_hdr) > data_size)
        {
          memcpy (TIZ_OMX_BUF_PTR (p_hdr), p_data, data_size);
          p_hdr->nFilledLen += data_size;
          ++ap_prc->ne_chunk_;
        }

      /* Release the OMX buffer */
      (void)tiz_filter_prc_release_header (ap_prc, a_pid);

      /* Release the ne packet if all chunks already processed */
      if (ap_prc->ne_chunk_ >= chunks)
        {
          /* All chunks extracted, release the packet now. */
          nestegg_free_packet (ap_prc->p_ne_pkt_);
          ap_prc->p_ne_pkt_ = NULL;
        }
    }
  return rc;
}

void propagate_eos_if_required (webmdmuxflt_prc_t *ap_prc)
{
  assert (ap_prc);

  /* If EOS, propagate the flag to the next component */
  if (tiz_filter_prc_is_eos (ap_prc))
    {
      /* .. but let's wait until buffers are available on both out ports */
      OMX_BUFFERHEADERTYPE *p_out_1 = tiz_filter_prc_get_header (
          ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
      OMX_BUFFERHEADERTYPE *p_out_2 = tiz_filter_prc_get_header (
          ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX);
      if (p_out_1 && p_out_2)
        {
          p_out_1->nFlags |= OMX_BUFFERFLAG_EOS;
          tiz_filter_prc_release_header (
              ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
          p_out_2->nFlags |= OMX_BUFFERFLAG_EOS;
          tiz_filter_prc_release_header (
              ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX);
          tiz_filter_prc_update_eos_flag (ap_prc, false);
        }
    }
}

static OMX_ERRORTYPE am_i_able_to_demux (webmdmuxflt_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *p_out_1 = tiz_filter_prc_get_header (
      ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
  OMX_BUFFERHEADERTYPE *p_out_2 = tiz_filter_prc_get_header (
      ap_prc, ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX);
  int compressed_data_avail = tiz_buffer_available (ap_prc->p_store_);

  assert (ap_prc);

  if (!compressed_data_avail || (!p_out_1 && !p_out_2))
    {
      TIZ_TRACE (handleOf (ap_prc),
                 "store bytes [%d] OUT HEADER 1 [%p] OUT HEADER 2 [%p]",
                 compressed_data_avail, p_out_1, p_out_2);
      rc = OMX_ErrorNotReady;
    }
  return rc;
}

static OMX_ERRORTYPE demux_stream (webmdmuxflt_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  tiz_check_omx_err (store_data (ap_prc));
  tiz_check_omx_err (am_i_able_to_demux (ap_prc));

  {
    int nestegg_rc = 0;

    assert (ap_prc);
    assert (ap_prc->p_ne_ctx_);

    if (ap_prc->p_ne_pkt_
        || (nestegg_rc
            = nestegg_read_packet (ap_prc->p_ne_ctx_, &ap_prc->p_ne_pkt_))
               > 0)
      {
        unsigned int track = 0;
        nestegg_packet_track (ap_prc->p_ne_pkt_, &track);
        if (track == ap_prc->ne_audio_track_)
          {
            extract_data (ap_prc, track,
                          ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
          }
        else if (track == ap_prc->ne_video_track_)
          {
            extract_data (ap_prc, track,
                          ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX);
          }
      }
  }

  return rc;
}

static void reset_stream_parameters (webmdmuxflt_prc_t *ap_prc)
{
  assert (ap_prc);

  ap_prc->store_offset_ = 0;
  ap_prc->demuxer_inited_ = false;
  ap_prc->audio_auto_detect_on_ = false;
  ap_prc->audio_coding_type_ = OMX_AUDIO_CodingUnused;
  ap_prc->video_coding_type_ = OMX_VIDEO_CodingUnused;
  ap_prc->bitrate_ = ARATELIA_WEBM_DEMUXER_DEFAULT_BIT_RATE_KBITS;

  tiz_buffer_clear (ap_prc->p_store_);
  tiz_filter_prc_update_eos_flag (ap_prc, false);
}

static void reset_nestegg_object (webmdmuxflt_prc_t *ap_prc)
{
  assert (ap_prc);
  assert (!ap_prc->p_ne_ctx_);
  ap_prc->p_ne_ctx_ = NULL;
  ap_prc->ne_io_.read = ne_io_read;
  ap_prc->ne_io_.seek = ne_io_seek;
  ap_prc->ne_io_.tell = ne_io_tell;
  ap_prc->ne_io_.userdata = ap_prc;
  tiz_mem_set (&(ap_prc->ne_audio_params_), 0,
               sizeof (ap_prc->ne_audio_params_));
  tiz_mem_set (&(ap_prc->ne_video_params_), 0,
               sizeof (ap_prc->ne_video_params_));
  ap_prc->ne_audio_track_ = NESTEGG_TRACK_UNKNOWN;
  ap_prc->ne_video_track_ = NESTEGG_TRACK_UNKNOWN;
  ap_prc->p_ne_pkt_ = NULL;
  ap_prc->ne_chunk_ = 0;
}

static inline void deallocate_temp_data_store (
    /*@special@ */ webmdmuxflt_prc_t *ap_prc)
/*@releases ap_prc->p_store_@ */
/*@ensures isnull ap_prc->p_store_@ */
{
  assert (ap_prc);
  tiz_buffer_destroy (ap_prc->p_store_);
  ap_prc->p_store_ = NULL;
}

static OMX_ERRORTYPE allocate_temp_data_store (webmdmuxflt_prc_t *ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  assert (ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_WEBM_DEMUXER_FILTER_PORT_0_INDEX);
  tiz_check_omx_err (
      tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                            OMX_IndexParamPortDefinition, &port_def));

  assert (ap_prc->p_store_ == NULL);
  return tiz_buffer_init (&(ap_prc->p_store_), port_def.nBufferSize);
}

static inline void deallocate_nestegg_object (
    /*@special@ */ webmdmuxflt_prc_t *ap_prc)
/*@releases ap_prc->p_ne_ctx_@ */
/*@ensures isnull ap_prc->p_ne_ctx_@ */
{
  assert (ap_prc);
  if (ap_prc->p_ne_ctx_)
    {
      nestegg_destroy (ap_prc->p_ne_ctx_);
      ap_prc->p_ne_ctx_ = NULL;
    }
}

static OMX_ERRORTYPE set_audio_coding_on_port (webmdmuxflt_prc_t *ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  TIZ_DEBUG (handleOf (ap_prc), " audio: %.2fhz %u bit %u channels",
             ap_prc->ne_audio_params_.rate, ap_prc->ne_audio_params_.depth,
             ap_prc->ne_audio_params_.channels);

  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX);
  tiz_check_omx_err (
      tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                            OMX_IndexParamPortDefinition, &port_def));

  /* Set the new value */
  port_def.format.audio.eEncoding = ap_prc->audio_coding_type_;

  tiz_check_omx_err (tiz_krn_SetParameter_internal (
      tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
      OMX_IndexParamPortDefinition, &port_def));

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE set_video_coding_on_port (webmdmuxflt_prc_t *ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  TIZ_DEBUG (handleOf (ap_prc), " video: %ux%u (d: %ux%u %ux%ux%ux%u)",
             ap_prc->ne_video_params_.width, ap_prc->ne_video_params_.height,
             ap_prc->ne_video_params_.display_width,
             ap_prc->ne_video_params_.display_height,
             ap_prc->ne_video_params_.crop_top,
             ap_prc->ne_video_params_.crop_left,
             ap_prc->ne_video_params_.crop_bottom,
             ap_prc->ne_video_params_.crop_right);

  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX);
  tiz_check_omx_err (
      tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                            OMX_IndexParamPortDefinition, &port_def));

  /* Set the new value */
  port_def.format.video.eCompressionFormat = ap_prc->video_coding_type_;

  tiz_check_omx_err (tiz_krn_SetParameter_internal (
      tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
      OMX_IndexParamPortDefinition, &port_def));

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE obtain_track_info (webmdmuxflt_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  unsigned int tracks = 0;
  uint64_t duration = 0;
  int nestegg_rc = 0;
  int type = 0;
  unsigned int i = 0, j = 0;
  unsigned int data_items = 0;
  unsigned char *p_codec_data = NULL;
  size_t length = 0;

  assert (ap_prc);

  (void)nestegg_track_count (ap_prc->p_ne_ctx_, &tracks);
  nestegg_rc = nestegg_duration (ap_prc->p_ne_ctx_, &duration);

  if (nestegg_rc == 0)
    {
      TIZ_DEBUG (handleOf (ap_prc), "media has %u tracks and duration %fs",
                 tracks, duration / 1e9);
    }
  else
    {
      TIZ_DEBUG (handleOf (ap_prc),
                 "media has %u tracks and unknown duration\n", tracks);
    }

  for (i = 0; i < tracks; ++i)
    {
      type = nestegg_track_type (ap_prc->p_ne_ctx_, i);
      TIZ_DEBUG (handleOf (ap_prc), "track %u: type: %d codec: %d", i, type,
                 nestegg_track_codec_id (ap_prc->p_ne_ctx_, i));
      nestegg_track_codec_data_count (ap_prc->p_ne_ctx_, i, &data_items);
      for (j = 0; j < data_items; ++j)
        {
          nestegg_track_codec_data (ap_prc->p_ne_ctx_, i, j, &p_codec_data,
                                    &length);
          TIZ_DEBUG (handleOf (ap_prc), " (%p, %u)", p_codec_data,
                     (unsigned int)length);
        }
      if (NESTEGG_TRACK_VIDEO == type
          && NESTEGG_TRACK_UNKNOWN == ap_prc->ne_video_track_)
        {
          nestegg_track_video_params (ap_prc->p_ne_ctx_, i,
                                      &ap_prc->ne_video_params_);
          ap_prc->video_coding_type_
              = (nestegg_track_codec_id (ap_prc->p_ne_ctx_, i)
                         == NESTEGG_CODEC_VP8
                     ? OMX_VIDEO_CodingVP8
                     : OMX_VIDEO_CodingVP9);
          ap_prc->ne_video_track_ = i;
          set_video_coding_on_port (ap_prc);
        }
      else if (NESTEGG_TRACK_AUDIO == type
               && NESTEGG_TRACK_UNKNOWN == ap_prc->ne_audio_track_)
        {
          nestegg_track_audio_params (ap_prc->p_ne_ctx_, i,
                                      &ap_prc->ne_audio_params_);
          ap_prc->audio_coding_type_
              = (nestegg_track_codec_id (ap_prc->p_ne_ctx_, i)
                         == NESTEGG_CODEC_OPUS
                     ? OMX_AUDIO_CodingOPUS
                     : OMX_AUDIO_CodingVORBIS);
          ap_prc->ne_audio_track_ = i;
          set_audio_coding_on_port (ap_prc);
        }
    }
  return rc;
}

static void send_audio_port_auto_detect_events (webmdmuxflt_prc_t *ap_prc)
{
  assert (ap_prc);
  if (ap_prc->audio_coding_type_ != OMX_AUDIO_CodingUnused
      || ap_prc->audio_coding_type_ != OMX_AUDIO_CodingAutoDetect)
    {
      tiz_srv_issue_event ((OMX_PTR)ap_prc, OMX_EventPortFormatDetected, 0, 0,
                           NULL);
      tiz_srv_issue_event (
          (OMX_PTR)ap_prc, OMX_EventPortSettingsChanged,
          ARATELIA_WEBM_DEMUXER_FILTER_PORT_1_INDEX, /* port 1 */
          OMX_IndexParamPortDefinition,              /* the index of the
                                                        struct that has
                                                        been modififed */
          NULL);
    }
  else
    {
      /* Oops... could not detect the stream format */
      tiz_srv_issue_err_event ((OMX_PTR)ap_prc, OMX_ErrorFormatNotDetected);
    }
}

static void send_video_port_auto_detect_events (webmdmuxflt_prc_t *ap_prc)
{
  assert (ap_prc);
  if (ap_prc->video_coding_type_ != OMX_VIDEO_CodingUnused
      || ap_prc->video_coding_type_ != OMX_VIDEO_CodingAutoDetect)
    {
      tiz_srv_issue_event ((OMX_PTR)ap_prc, OMX_EventPortFormatDetected, 0, 0,
                           NULL);
      tiz_srv_issue_event (
          (OMX_PTR)ap_prc, OMX_EventPortSettingsChanged,
          ARATELIA_WEBM_DEMUXER_FILTER_PORT_2_INDEX, /* port 1 */
          OMX_IndexParamPortDefinition,              /* the index of the
                                                        struct that has
                                                        been modififed */
          NULL);
    }
  else
    {
      /* Oops... could not detect the stream format */
      tiz_srv_issue_err_event ((OMX_PTR)ap_prc, OMX_ErrorFormatNotDetected);
    }
}

static void send_port_auto_detect_events (webmdmuxflt_prc_t *ap_prc)
{
  send_audio_port_auto_detect_events (ap_prc);
  send_video_port_auto_detect_events (ap_prc);
}

static OMX_ERRORTYPE allocate_nestegg_object (webmdmuxflt_prc_t *ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  if (OMX_ErrorNone == (rc = store_data (ap_prc)))
    {
      int nestegg_rc = 0;
      assert (!ap_prc->p_ne_ctx_);

      nestegg_rc
          = nestegg_init (&ap_prc->p_ne_ctx_, ap_prc->ne_io_, ne_log_cback, -1);

      ap_prc->store_offset_ = 0;
      if (0 != nestegg_rc)
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "Unable to open the nestegg demuxer handle (error = %d).",
                     nestegg_rc);
          deallocate_nestegg_object (ap_prc);
        }
      else
        {
          TIZ_TRACE (handleOf (ap_prc),
                     "decoder_inited = TRUE - store_offset [%d]",
                     ap_prc->store_offset_);
          if (OMX_ErrorNone == obtain_track_info (ap_prc))
            {
              send_port_auto_detect_events (ap_prc);
            }
          ap_prc->demuxer_inited_ = true;
          tiz_buffer_advance (ap_prc->p_store_, ap_prc->store_offset_);
        }
    }

  return rc;
}

/*
 * webmdmuxfltprc
 */

static void *webmdmuxflt_prc_ctor (void *ap_prc, va_list *app)
{
  webmdmuxflt_prc_t *p_prc
      = super_ctor (typeOf (ap_prc, "webmdmuxfltprc"), ap_prc, app);
  assert (p_prc);

  p_prc->p_outhdr_ = NULL;
  p_prc->p_store_ = NULL;

  reset_stream_parameters (p_prc);
  reset_nestegg_object (p_prc);

  return p_prc;
}

static void *webmdmuxflt_prc_dtor (void *ap_obj)
{
  (void)webmdmuxflt_prc_deallocate_resources (ap_obj);
  return super_dtor (typeOf (ap_obj, "webmdmuxfltprc"), ap_obj);
}

/*
 * from tizsrv class
 */

static OMX_ERRORTYPE webmdmuxflt_prc_allocate_resources (void *ap_prc,
                                                         OMX_U32 a_pid)
{
  webmdmuxflt_prc_t *p_prc = ap_prc;
  assert (p_prc);
  return allocate_temp_data_store (p_prc);
}

static OMX_ERRORTYPE webmdmuxflt_prc_deallocate_resources (void *ap_prc)
{
  webmdmuxflt_prc_t *p_prc = ap_prc;
  assert (p_prc);
  deallocate_temp_data_store (p_prc);
  deallocate_nestegg_object (p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE webmdmuxflt_prc_prepare_to_transfer (void *ap_prc,
                                                          OMX_U32 a_pid)
{
  webmdmuxflt_prc_t *p_prc = ap_prc;
  assert (ap_prc);
  return prepare_for_port_auto_detection (p_prc);
}

static OMX_ERRORTYPE webmdmuxflt_prc_transfer_and_process (void *ap_prc,
                                                           OMX_U32 a_pid)
{
  webmdmuxflt_prc_t *p_prc = ap_prc;
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (p_prc);
  if (p_prc->audio_auto_detect_on_)
    {
    }
  return rc;
}

static OMX_ERRORTYPE webmdmuxflt_prc_stop_and_return (void *ap_prc)
{
  webmdmuxflt_prc_t *p_prc = ap_prc;
  assert (p_prc);
  return release_buffer (p_prc);
}

/*
 * from tizprc class
 */

static OMX_ERRORTYPE webmdmuxflt_prc_buffers_ready (const void *ap_prc)
{
  webmdmuxflt_prc_t *p_prc = (webmdmuxflt_prc_t *)ap_prc;
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (ap_prc);

  if (!p_prc->demuxer_inited_)
    {
      rc = allocate_nestegg_object (p_prc);
    }

  if (p_prc->demuxer_inited_ && OMX_ErrorNone == rc)
    {
      while (OMX_ErrorNone == rc)
        {
          rc = demux_stream (p_prc);
        }
      if (OMX_ErrorNotReady == rc)
        {
          rc = OMX_ErrorNone;
        }
    }

  propagate_eos_if_required (p_prc);

  return rc;
}

static OMX_ERRORTYPE webmdmuxflt_prc_pause (const void *ap_obj)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE webmdmuxflt_prc_resume (const void *ap_obj)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE webmdmuxflt_prc_port_flush (const void *ap_prc,
                                                 OMX_U32 a_pid)
{
  webmdmuxflt_prc_t *p_prc = (webmdmuxflt_prc_t *)ap_prc;
  reset_stream_parameters (p_prc);
  return tiz_filter_prc_release_header (p_prc, a_pid);
}

static OMX_ERRORTYPE webmdmuxflt_prc_port_disable (const void *ap_prc,
                                                   OMX_U32 a_pid)
{
  webmdmuxflt_prc_t *p_prc = (webmdmuxflt_prc_t *)ap_prc;
  OMX_ERRORTYPE rc = tiz_filter_prc_release_header (p_prc, a_pid);
  reset_stream_parameters (p_prc);
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, true);
  return rc;
}

static OMX_ERRORTYPE webmdmuxflt_prc_port_enable (const void *ap_prc,
                                                  OMX_U32 a_pid)
{
  webmdmuxflt_prc_t *p_prc = (webmdmuxflt_prc_t *)ap_prc;
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, false);
  return OMX_ErrorNone;
}

/*
 * webmdmuxflt_prc_class
 */

static void *webmdmuxflt_prc_class_ctor (void *ap_prc, va_list *app)
{
  /* NOTE: Class methods might be added in the future. None for now. */
  return super_ctor (typeOf (ap_prc, "webmdmuxfltprc_class"), ap_prc, app);
}

/*
 * initialization
 */

void *webmdmuxflt_prc_class_init (void *ap_tos, void *ap_hdl)
{
  void *tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void *webmdmuxfltprc_class = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (classOf (tizfilterprc), "webmdmuxfltprc_class", classOf (tizfilterprc),
       sizeof (webmdmuxflt_prc_class_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, webmdmuxflt_prc_class_ctor,
       /* TIZ_CLASS_COMMENT: stop value*/
       0);
  return webmdmuxfltprc_class;
}

void *webmdmuxflt_prc_init (void *ap_tos, void *ap_hdl)
{
  void *tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void *webmdmuxfltprc_class = tiz_get_type (ap_hdl, "webmdmuxfltprc_class");
  TIZ_LOG_CLASS (webmdmuxfltprc_class);
  void *webmdmuxfltprc = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (webmdmuxfltprc_class, "webmdmuxfltprc", tizfilterprc,
       sizeof (webmdmuxflt_prc_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, webmdmuxflt_prc_ctor,
       /* TIZ_CLASS_COMMENT: class destructor */
       dtor, webmdmuxflt_prc_dtor,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_allocate_resources, webmdmuxflt_prc_allocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_deallocate_resources, webmdmuxflt_prc_deallocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_prepare_to_transfer, webmdmuxflt_prc_prepare_to_transfer,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_transfer_and_process, webmdmuxflt_prc_transfer_and_process,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_stop_and_return, webmdmuxflt_prc_stop_and_return,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_buffers_ready, webmdmuxflt_prc_buffers_ready,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_pause, webmdmuxflt_prc_pause,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_resume, webmdmuxflt_prc_resume,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_flush, webmdmuxflt_prc_port_flush,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_disable, webmdmuxflt_prc_port_disable,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_enable, webmdmuxflt_prc_port_enable,
       /* TIZ_CLASS_COMMENT: stop value */
       0);

  return webmdmuxfltprc;
}