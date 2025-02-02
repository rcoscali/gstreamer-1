/* GStreamer
 * Copyright (C) <2023> Rémi COHEN SCALI <remi at cohenscali dot net>
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

#ifndef __GST_DISCRET11_AUDIO_PARSE_H__
#define __GST_DISCRET11_AUDIO_PARSE_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include "gstdiscret11parse.h"

G_BEGIN_DECLS

#define GST_TYPE_DISCRET11_AUDIO_PARSE (gst_discret11_audio_parse_get_type())
#define GST_DISCRET11_AUDIO_PARSE_CAST(obj) ((GstDiscret11AudioParse *)(obj))
G_DECLARE_FINAL_TYPE (GstDiscret11AudioParse, gst_discret11_audio_parse,
    GST, DISCRET11_AUDIO_PARSE, GstDiscret11Parse)

typedef enum _GstDiscret11AudioParseFormat GstDiscret11AudioParseFormat;

typedef struct _GstDiscret11AudioParseConfig GstDiscret11AudioParseConfig;

enum _GstDiscret11AudioParseFormat
{
  GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM,
  GST_DISCRET11_AUDIO_PARSE_FORMAT_MULAW,
  GST_DISCRET11_AUDIO_PARSE_FORMAT_ALAW
};

/* Contains information about the sample rate, format, and channel count to use. */
struct _GstDiscret11AudioParseConfig
{
  /* If TRUE, then this configuration is ready to use */
  gboolean ready;
  /* Format of the configuration. Can be PCM, a-law, mu-law. */
  GstDiscret11AudioParseFormat format;
  /* If format is set to PCM, this specifies the exact PCM format in use.
   * Meaningless if format is set to anything other than PCM. */
  GstAudioFormat pcm_format;
  /* Bytes per frame. Calculated as: bpf = bytes_per_sample * num_channels
   * Must be nonzero. This is the size of one frame, the value returned
   * by the GstDiscret11BaseParseClass get_config_frame_size() vfunc. */
  guint bpf;
  /* Sample rate in Hz - must be nonzero */
  guint sample_rate;
  /* Number of channels - must be nonzero */
  guint num_channels;
  /* TRUE if the data is interleaved, FALSE otherwise */
  gboolean interleaved;

  /* Array of channel positions, one position per channel; its first
   * num_channels values are valid. They are computed out of the number
   * of channels if no positions are explicitly given. */
  GstAudioChannelPosition channel_positions[64];

  /* If the channel_positions are in a valid GStreamer channel order, then
   * this is not used, and needs_channel_reordering is FALSE. Otherwise,
   * this contains the same positions as in channel_positions, but in the
   * order GStreamer expects. needs_channel_reordering will be TRUE in that
   * case. This is used for reordering samples in outgoing buffers if
   * necessary. */
  GstAudioChannelPosition reordered_channel_positions[64];

  /* TRUE if channel reordering is necessary, FALSE otherwise. See above
   * for details. */
  gboolean needs_channel_reordering;
};

struct _GstDiscret11AudioParse
{
  GstDiscret11Parse parent;

  /*< private > */

  /* Configuration controlled by the object properties. Its ready value
   * is set to TRUE from the start, so it can be used right away.
   */
  GstDiscret11AudioParseConfig properties_config;
  /* Configuration controlled by the sink caps. Its ready value is
   * initially set to FALSE until valid sink caps come in. It is set to
   * FALSE again when the stream-start event is observed.
   */
  GstDiscret11AudioParseConfig sink_caps_config;
  /* Currently active configuration. Points either to properties_config
   * or to sink_caps_config. This is never NULL. */
  GstDiscret11AudioParseConfig *current_config;
};

#define GST_TYPE_DISCRET11_AUDIO_PARSE_FORMAT (gst_discret11_audio_parse_format_get_type())
GType gst_discret11_audio_parse_format_get_type (void);

G_END_DECLS

#endif
