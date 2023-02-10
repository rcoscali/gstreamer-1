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

/**
 * SECTION:element-discret11audio
 * @title: discret11audio
 *
 * This element parses incoming data as raw audio samples and timestamps it.
 * It also handles seek queries in said raw audio data, and ensures that output
 * buffers contain an integer number of samples, even if the input buffers don't.
 * For example, with sample format S16LE and 2 channels, an input buffer of 411
 * bytes contains 102.75 samples. discret11audio will then output 102 samples
 * (= 408 bytes) and keep the remaining 3 bytes. These will then be prepended to
 * the next input data.
 *
 * Currently, this parser supports raw data in a-law, mu-law, or linear PCM format.
 *
 * To facilitate operation with the unalignedaudioparse element, discret11audio
 * supports the "audio/x-unaligned-raw" media type. This is treated identically to
 * "audio/x-raw", except that it is used by source elements which do not guarantee
 * that the buffers they push out are timestamped and contain an integer amount of
 * samples (see the 411 bytes example above). By using a different media type, it
 * is guaranteed that unalignedaudioparse is autoplugged, making sure that the
 * autoplugged chain does not push unparsed content downstream. The source caps'
 * media type with linear PCM data is always "audio/x-raw", even if the sink caps
 * use "audio/x-unaligned-raw".
 *
 * The channel-positions property can be used to set explicit position information
 * for each channel. If the array that is passed to this property does not match
 * the number of channels indicated by num-channels, then said number of channels
 * is updated to the array length. If channel-positions is NULL, then the default
 * GStreamer positioning is used. This property is also useful for swapping left
 * and right in a stereo signal for example.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 souphttpsrc http://my-dlna-server/track.l16 \
 *     discret11audio ! audioconvert ! audioresample ! autoaudiosink
 * ]|
 *  Receive L16 data from a DLNA server, parse and timestamp it with
 * discret11audio, and play it. use-sink-caps is set to true since souphttpsrc
 * will set its source pad's caps to audio/x-unaligned-raw for the L16 stream.
 * |[
 * gst-launch-1.0 filesrc location=audio.raw ! discret11audio use-sink-caps=false \
 *         format=pcm pcm-format=s16le sample-rate=48000 num-channels=2 \
 *         audioconvert ! audioresample ! autoaudiosink
 * ]|
 *  Read raw data from a local file and parse it as PCM data with 48000 Hz sample
 * rate, signed 16 bit integer samples, and 2 channels. use-sink-caps is set to
 * false to ensure the property information is used and the parser does not expect
 * audio/x-raw or audio/x-unaligned-raw caps.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* FIXME: GValueArray is deprecated, but there is currently no viabla alternative
 * See https://bugzilla.gnome.org/show_bug.cgi?id=667228 */
#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include <string.h>
#include "gstdiscret11parseelements.h"
#include "gstdiscret11audioparse.h"
#include "unalignedaudio.h"

GST_DEBUG_CATEGORY_STATIC (discret11_audio_parse_debug);
#define GST_CAT_DEFAULT discret11_audio_parse_debug

enum
{
  PROP_0,
  PROP_FORMAT,
  PROP_PCM_FORMAT,
  PROP_SAMPLE_RATE,
  PROP_NUM_CHANNELS,
  PROP_INTERLEAVED,
  PROP_CHANNEL_POSITIONS
};

#define DEFAULT_FORMAT         GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM
#define DEFAULT_PCM_FORMAT     GST_AUDIO_FORMAT_S16
#define DEFAULT_SAMPLE_RATE    44100
#define DEFAULT_NUM_CHANNELS   2
#define DEFAULT_INTERLEAVED    TRUE

#define GST_DISCRET11_AUDIO_PARSE_CAPS \
  GST_AUDIO_CAPS_MAKE(GST_AUDIO_FORMATS_ALL) \
  ", layout = (string) { interleaved, non-interleaved }; " \
  "audio/x-alaw, rate = (int) [ 1, MAX ], channels = (int) [ 1, MAX ]; " \
  "audio/x-mulaw, rate = (int) [ 1, MAX ], channels = (int) [ 1, MAX ]; "

static GstStaticPadTemplate static_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_UNALIGNED_DISCRET11_AUDIO_CAPS "; "
        GST_DISCRET11_AUDIO_PARSE_CAPS)
    );

static GstStaticPadTemplate static_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_DISCRET11_AUDIO_PARSE_CAPS)
    );

#define gst_discret11_audio_parse_parent_class parent_class
G_DEFINE_TYPE (GstDiscret11AudioParse, gst_discret11_audio_parse,
    GST_TYPE_DISCRET11_AUDIO_PARSE);
GST_ELEMENT_REGISTER_DEFINE (discret11audioparse, "discret11audioparse",
    GST_RANK_NONE, GST_TYPE_DISCRET11_AUDIO_PARSE);

static void gst_discret11_audio_parse_set_property (GObject * object,
    guint prop_id, GValue const *value, GParamSpec * pspec);
static void gst_discret11_audio_parse_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_discret11_audio_parse_stop (GstBaseParse * parse);

static gboolean gst_discret11_audio_parse_set_current_config (GstDiscret11Parse
    * discret11_parse, GstDiscret11ParseConfig config);
static GstDiscret11ParseConfig
gst_discret11_audio_parse_get_current_config (GstDiscret11Parse *
    discret11_parse);
static gboolean
gst_discret11_audio_parse_set_config_from_caps (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config, GstCaps * caps);
static gboolean
gst_discret11_audio_parse_get_caps_from_config (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config, GstCaps ** caps);
static gsize gst_discret11_audio_parse_get_config_frame_size (GstDiscret11Parse
    * discret11_parse, GstDiscret11ParseConfig config);
static gboolean gst_discret11_audio_parse_is_config_ready (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config);
static gboolean gst_discret11_audio_parse_process (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config, GstBuffer * in_data,
    gsize total_num_in_bytes, gsize num_valid_in_bytes,
    GstBuffer ** processed_data);
static gboolean
gst_discret11_audio_parse_is_unit_format_supported (GstDiscret11Parse *
    discret11_parse, GstFormat format);
static void gst_discret11_audio_parse_get_units_per_second (GstDiscret11Parse *
    discret11_parse, GstFormat format, GstDiscret11ParseConfig config,
    gsize * units_per_sec_n, gsize * units_per_sec_d);
static gint gst_discret11_audio_parse_get_alignment (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config);

static gboolean
gst_discret11_audio_parse_is_using_sink_caps (GstDiscret11AudioParse *
    discret11_audio_parse);
static GstDiscret11AudioParseConfig
    * gst_discret11_audio_parse_get_config_ptr (GstDiscret11AudioParse *
    discret11_audio_parse, GstDiscret11ParseConfig config);

static void gst_discret11_audio_parse_init_config (GstDiscret11AudioParseConfig
    * config);
static gboolean
gst_discret11_audio_parse_set_config_channels (GstDiscret11AudioParseConfig *
    config, guint num_channels, guint64 channel_mask, gboolean set_positions);
static gboolean
    gst_discret11_audio_parse_update_channel_reordering_flag
    (GstDiscret11AudioParseConfig * config);
static void
gst_discret11_audio_parse_update_config_bpf (GstDiscret11AudioParseConfig *
    config);
static gboolean gst_discret11_audio_parse_caps_to_config (GstDiscret11AudioParse
    * discret11_audio_parse, GstCaps * caps,
    GstDiscret11AudioParseConfig * config);
static gboolean gst_discret11_audio_parse_config_to_caps (GstDiscret11AudioParse
    * discret11_audio_parse, GstCaps ** caps,
    GstDiscret11AudioParseConfig * config);

static void
gst_discret11_audio_parse_class_init (GstDiscret11AudioParseClass * klass)
{
  GObjectClass *object_class;
  GstElementClass *element_class;
  GstBaseParseClass *baseparse_class;
  GstDiscret11ParseClass *discret11parse_class;

  GST_DEBUG_CATEGORY_INIT (discret11_audio_parse_debug, "discret11audio", 0,
      "discret11audio element");

  object_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  baseparse_class = GST_BASE_PARSE_CLASS (klass);
  discret11parse_class = GST_DISCRET11_PARSE_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&static_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&static_src_template));

  object_class->set_property =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_set_property);
  object_class->get_property =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_get_property);

  baseparse_class->stop = GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_stop);

  discret11parse_class->set_current_config =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_set_current_config);
  discret11parse_class->get_current_config =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_get_current_config);
  discret11parse_class->set_config_from_caps =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_set_config_from_caps);
  discret11parse_class->get_caps_from_config =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_get_caps_from_config);
  discret11parse_class->get_config_frame_size =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_get_config_frame_size);
  discret11parse_class->is_config_ready =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_is_config_ready);
  discret11parse_class->process =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_process);
  discret11parse_class->is_unit_format_supported =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_is_unit_format_supported);
  discret11parse_class->get_units_per_second =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_get_units_per_second);
  discret11parse_class->get_alignment =
      GST_DEBUG_FUNCPTR (gst_discret11_audio_parse_get_alignment);

  g_object_class_install_property (object_class,
      PROP_FORMAT,
      g_param_spec_enum ("format",
          "Format",
          "Format of the raw audio stream",
          GST_TYPE_DISCRET11_AUDIO_PARSE_FORMAT,
          GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );
  g_object_class_install_property (object_class,
      PROP_PCM_FORMAT,
      g_param_spec_enum ("pcm-format",
          "PCM format",
          "Format of audio samples in PCM stream (ignored if format property is not set to pcm)",
          GST_TYPE_AUDIO_FORMAT,
          GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );
  g_object_class_install_property (object_class,
      PROP_SAMPLE_RATE,
      g_param_spec_int ("sample-rate",
          "Sample rate",
          "Rate of audio samples in raw stream",
          1, INT_MAX,
          DEFAULT_SAMPLE_RATE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );
  g_object_class_install_property (object_class,
      PROP_NUM_CHANNELS,
      g_param_spec_int ("num-channels",
          "Number of channels",
          "Number of channels in raw stream",
          1, INT_MAX,
          DEFAULT_NUM_CHANNELS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );
  g_object_class_install_property (object_class,
      PROP_INTERLEAVED,
      g_param_spec_boolean ("interleaved",
          "Interleaved layout",
          "True if audio has interleaved layout",
          DEFAULT_INTERLEAVED, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );
  g_object_class_install_property (object_class,
      PROP_CHANNEL_POSITIONS,
      g_param_spec_value_array ("channel-positions",
          "Channel positions",
          "Channel positions used on the output",
          g_param_spec_enum ("channel-position",
              "Channel position",
              "Channel position of the n-th input",
              GST_TYPE_AUDIO_CHANNEL_POSITION,
              GST_AUDIO_CHANNEL_POSITION_NONE,
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS),
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );

  gst_element_class_set_static_metadata (element_class,
      "discret11audio",
      "Codec/Parser/Audio",
      "Converts unformatted data streams into timestamped raw audio frames",
      "Rémi COHEN SCALI <remi@cohenscali.net>");

  gst_type_mark_as_plugin_api (GST_TYPE_DISCRET11_AUDIO_PARSE_FORMAT, 0);
}

static void
gst_discret11_audio_parse_init (GstDiscret11AudioParse * discret11_audio_parse)
{
  /* Setup configs and select which one shall be the current one from the start. */
  gst_discret11_audio_parse_init_config (&
      (discret11_audio_parse->properties_config));
  gst_discret11_audio_parse_init_config (&
      (discret11_audio_parse->sink_caps_config));
  /* As required by GstDiscret11Parse, ensure that the current configuration
   * is initially set to be the properties config */
  discret11_audio_parse->current_config =
      &(discret11_audio_parse->properties_config);

  /* Properties config must be valid from the start, so set its ready value
   * to TRUE, and make sure its bpf value is valid. */
  discret11_audio_parse->properties_config.ready = TRUE;
  gst_discret11_audio_parse_update_config_bpf (&
      (discret11_audio_parse->properties_config));
}

static void
gst_discret11_audio_parse_set_property (GObject * object, guint prop_id,
    GValue const *value, GParamSpec * pspec)
{
  GstBaseParse *base_parse = GST_BASE_PARSE (object);
  GstDiscret11Parse *discret11_parse = GST_DISCRET11_PARSE (object);
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (object);

  /* All properties are handled similarly:
   * - if the new value is the same as the current value, nothing is done
   * - the parser lock is held while the new value is set
   * - if the properties config is the current config, the source caps are
   *   invalidated to ensure that the code in handle_frame pushes a new CAPS
   *   event out
   * - properties that affect the bpf value call the function to update
   *   the bpf and also call gst_base_parse_set_min_frame_size() to ensure
   *   that the minimum frame size can hold 1 frame (= one sample for each
   *   channel)
   */

  switch (prop_id) {
    case PROP_FORMAT:
    {
      GstDiscret11AudioParseFormat new_format = g_value_get_enum (value);

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      if (new_format != discret11_audio_parse->properties_config.format) {
        discret11_audio_parse->properties_config.format = new_format;
        gst_discret11_audio_parse_update_config_bpf (&
            (discret11_audio_parse->properties_config));

        if (!gst_discret11_audio_parse_is_using_sink_caps
            (discret11_audio_parse)) {
          gst_discret11_parse_invalidate_src_caps (discret11_parse);
          gst_base_parse_set_min_frame_size (base_parse,
              discret11_audio_parse->properties_config.bpf);
        }
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;
    }

    case PROP_PCM_FORMAT:
    {
      GstAudioFormat new_pcm_format = g_value_get_enum (value);

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      if (new_pcm_format != discret11_audio_parse->properties_config.pcm_format) {
        discret11_audio_parse->properties_config.pcm_format = new_pcm_format;
        gst_discret11_audio_parse_update_config_bpf (&
            (discret11_audio_parse->properties_config));

        if (!gst_discret11_audio_parse_is_using_sink_caps
            (discret11_audio_parse)) {
          gst_discret11_parse_invalidate_src_caps (discret11_parse);
          gst_base_parse_set_min_frame_size (base_parse,
              discret11_audio_parse->properties_config.bpf);
        }
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;
    }

    case PROP_SAMPLE_RATE:
    {
      guint new_sample_rate = g_value_get_int (value);

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      if (new_sample_rate !=
          discret11_audio_parse->properties_config.sample_rate) {
        discret11_audio_parse->properties_config.sample_rate = new_sample_rate;

        if (!gst_discret11_audio_parse_is_using_sink_caps
            (discret11_audio_parse))
          gst_discret11_parse_invalidate_src_caps (discret11_parse);
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;
    }

    case PROP_NUM_CHANNELS:
    {
      guint new_num_channels = g_value_get_int (value);

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      if (new_num_channels !=
          discret11_audio_parse->properties_config.num_channels) {
        gst_discret11_audio_parse_set_config_channels (&
            (discret11_audio_parse->properties_config), new_num_channels, 0,
            TRUE);

        discret11_audio_parse->properties_config.num_channels =
            new_num_channels;
        gst_discret11_audio_parse_update_config_bpf (&
            (discret11_audio_parse->properties_config));

        if (!gst_discret11_audio_parse_is_using_sink_caps
            (discret11_audio_parse)) {
          gst_discret11_parse_invalidate_src_caps (discret11_parse);
          gst_base_parse_set_min_frame_size (base_parse,
              discret11_audio_parse->properties_config.bpf);
        }
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;
    }

    case PROP_INTERLEAVED:
    {
      gboolean new_interleaved = g_value_get_boolean (value);

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      if (new_interleaved !=
          discret11_audio_parse->properties_config.interleaved) {
        discret11_audio_parse->properties_config.interleaved = new_interleaved;

        if (!gst_discret11_audio_parse_is_using_sink_caps
            (discret11_audio_parse))
          gst_discret11_parse_invalidate_src_caps (discret11_parse);
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;
    }

    case PROP_CHANNEL_POSITIONS:
    {
      GValueArray *valarray = g_value_get_boxed (value);
      GstDiscret11AudioParseConfig *config =
          &(discret11_audio_parse->properties_config);

      /* Sanity check - reject empty arrays */
      if ((valarray != NULL) && (valarray->n_values == 0)) {
        GST_ELEMENT_ERROR (discret11_audio_parse, LIBRARY, SETTINGS,
            ("channel position property holds an empty array"), (NULL));
        break;
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      if ((valarray == NULL) && (config->num_channels > 0)) {
        /* NULL value given, and number of channels is nonzero.
         * Use the default GStreamer positioning. Call
         * set_config_channels with the set_positions parameter
         * set to TRUE to ensure the position values are filled. */
        gst_discret11_audio_parse_set_config_channels (&
            (discret11_audio_parse->properties_config), config->num_channels, 0,
            TRUE);
      } else if (valarray != NULL) {
        /* Non-NULL value given. Make sure the channel_positions
         * array in the properties config has enough room, and that
         * the num_channels value equals the array length. Then copy
         * the values from the valarray to channel_positions, and
         * produce a copy of that array in case its channel positions
         * are not in a valid GStreamer order (to be able to apply
         * channel reordering later).
         */

        guint i;

        if (valarray->n_values != config->num_channels) {
          /* Call with set_positions == FALSE to ensure that
           * the array is properly allocated but not filled
           * (it is filled below) */
          gst_discret11_audio_parse_set_config_channels (config,
              valarray->n_values, 0, FALSE);
        }

        for (i = 0; i < config->num_channels; ++i) {
          GValue *val = g_value_array_get_nth (valarray, i);
          config->channel_positions[i] = g_value_get_enum (val);
        }

        gst_discret11_audio_parse_update_channel_reordering_flag (config);
      }

      gst_discret11_audio_parse_update_config_bpf (&
          (discret11_audio_parse->properties_config));

      if (!gst_discret11_audio_parse_is_using_sink_caps (discret11_audio_parse)) {
        gst_discret11_parse_invalidate_src_caps (discret11_parse);
        gst_base_parse_set_min_frame_size (base_parse,
            discret11_audio_parse->properties_config.bpf);
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;
    }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_discret11_audio_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (object);

  switch (prop_id) {
    case PROP_FORMAT:
      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);
      g_value_set_enum (value, discret11_audio_parse->properties_config.format);
      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;

    case PROP_PCM_FORMAT:
      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);
      g_value_set_enum (value,
          discret11_audio_parse->properties_config.pcm_format);
      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;

    case PROP_SAMPLE_RATE:
      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);
      g_value_set_int (value,
          discret11_audio_parse->properties_config.sample_rate);
      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;

    case PROP_NUM_CHANNELS:
      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);
      g_value_set_int (value,
          discret11_audio_parse->properties_config.num_channels);
      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;

    case PROP_INTERLEAVED:
      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);
      g_value_set_boolean (value,
          discret11_audio_parse->properties_config.interleaved);
      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);
      break;

    case PROP_CHANNEL_POSITIONS:
    {
      GstDiscret11AudioParseConfig *config;
      GValueArray *valarray;

      GST_DISCRET11_PARSE_CONFIG_MUTEX_LOCK (object);

      valarray = NULL;
      config = &(discret11_audio_parse->properties_config);

      /* Copy channel positions into the valuearray */
      if (config->num_channels > 0) {
        guint i;
        GValue val = G_VALUE_INIT;
        g_assert (config->channel_positions);

        g_value_init (&val, GST_TYPE_AUDIO_CHANNEL_POSITION);
        valarray = g_value_array_new (config->num_channels);

        for (i = 0; i < config->num_channels; ++i) {
          g_value_set_enum (&val, config->channel_positions[i]);
          g_value_array_insert (valarray, i, &val);
        }

        g_value_unset (&val);
      }

      GST_DISCRET11_PARSE_CONFIG_MUTEX_UNLOCK (object);

      /* Pass on ownership to the value array,
       * since we don't need it anymore */
      g_value_take_boxed (value, valarray);

      break;
    }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_discret11_audio_parse_stop (GstBaseParse * parse)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (parse);

  /* Sink caps config is not ready until caps come in.
   * We are stopping processing, the element is being reset,
   * so the config has to be un-readied.
   * (Since the properties config is not depending on caps,
   * its ready status is always TRUE.) */
  discret11_audio_parse->sink_caps_config.ready = FALSE;

  return GST_BASE_PARSE_CLASS (parent_class)->stop (parse);
}

static gboolean
gst_discret11_audio_parse_set_current_config (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);

  switch (config) {
    case GST_DISCRET11_PARSE_CONFIG_PROPERTIES:
      discret11_audio_parse->current_config =
          &(discret11_audio_parse->properties_config);
      break;

    case GST_DISCRET11_PARSE_CONFIG_SINKCAPS:
      discret11_audio_parse->current_config =
          &(discret11_audio_parse->sink_caps_config);
      break;

    default:
      g_assert_not_reached ();
  }

  return TRUE;
}

static GstDiscret11ParseConfig
gst_discret11_audio_parse_get_current_config (GstDiscret11Parse *
    discret11_parse)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  return gst_discret11_audio_parse_is_using_sink_caps (discret11_audio_parse) ?
      GST_DISCRET11_PARSE_CONFIG_SINKCAPS :
      GST_DISCRET11_PARSE_CONFIG_PROPERTIES;
}

static gboolean
gst_discret11_audio_parse_set_config_from_caps (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config, GstCaps * caps)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  return gst_discret11_audio_parse_caps_to_config (discret11_audio_parse, caps,
      gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse, config));
}

static gboolean
gst_discret11_audio_parse_get_caps_from_config (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config, GstCaps ** caps)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  return gst_discret11_audio_parse_config_to_caps (discret11_audio_parse, caps,
      gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse, config));
}

static gsize
gst_discret11_audio_parse_get_config_frame_size (GstDiscret11Parse *
    discret11_parse, GstDiscret11ParseConfig config)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  return gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse,
      config)->bpf;
}

static gboolean
gst_discret11_audio_parse_is_config_ready (GstDiscret11Parse * discret11_parse,
    GstDiscret11ParseConfig config)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  return gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse,
      config)->ready;
}

static guint
round_up_pow2 (guint n)
{
  n = n - 1;
  n = n | (n >> 1);
  n = n | (n >> 2);
  n = n | (n >> 4);
  n = n | (n >> 8);
  n = n | (n >> 16);
  return n + 1;
}

static gint
gst_discret11_audio_parse_get_alignment (GstDiscret11Parse * discret11_parse,
    GstDiscret11ParseConfig config)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  GstDiscret11AudioParseConfig *config_ptr =
      gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse, config);
  gint width;

  if (config_ptr->format != GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM)
    return 1;

  width =
      GST_AUDIO_FORMAT_INFO_WIDTH (gst_audio_format_get_info
      (config_ptr->pcm_format)) / 8;
  width = GST_ROUND_UP_8 (width);
  width = round_up_pow2 (width);

  return width;
}

static gboolean
gst_discret11_audio_parse_process (GstDiscret11Parse * discret11_parse,
    GstDiscret11ParseConfig config,
    GstBuffer * in_data, gsize total_num_in_bytes,
    gsize num_valid_in_bytes, GstBuffer ** processed_data)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  GstDiscret11AudioParseConfig *config_ptr =
      gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse, config);

  if ((config_ptr->format == GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM)
      && config_ptr->needs_channel_reordering) {
    /* Need to reorder samples, since they are in an invalid
     * channel order. */

    GstBuffer *outbuf;

    GST_LOG_OBJECT (discret11_audio_parse,
        "using %" G_GSIZE_FORMAT " bytes out of the %" G_GSIZE_FORMAT
        " bytes from the input buffer with reordering", num_valid_in_bytes,
        total_num_in_bytes);

    outbuf =
        gst_buffer_copy_region (in_data,
        GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS |
        GST_BUFFER_COPY_META | GST_BUFFER_COPY_MEMORY, 0, num_valid_in_bytes);

    gst_audio_buffer_reorder_channels (outbuf,
        config_ptr->pcm_format,
        config_ptr->num_channels,
        config_ptr->channel_positions, config_ptr->reordered_channel_positions);

    *processed_data = outbuf;
  } else {
    /* Nothing needs to be done with the sample data.
     * Instruct the baseparse class to just take out_size bytes
     * from the input buffer */

    GST_LOG_OBJECT (discret11_audio_parse,
        "using %" G_GSIZE_FORMAT " bytes out of the %" G_GSIZE_FORMAT
        " bytes from the input buffer without reordering", num_valid_in_bytes,
        total_num_in_bytes);

    *processed_data = NULL;
  }

  return TRUE;
}

static gboolean
gst_discret11_audio_parse_is_unit_format_supported (G_GNUC_UNUSED
    GstDiscret11Parse * raw_base_parse, GstFormat format)
{
  switch (format) {
    case GST_FORMAT_BYTES:
    case GST_FORMAT_DEFAULT:
      return TRUE;
    default:
      return FALSE;
  }
}

static void
gst_discret11_audio_parse_get_units_per_second (GstDiscret11Parse *
    discret11_parse, GstFormat format, GstDiscret11ParseConfig config,
    gsize * units_per_sec_n, gsize * units_per_sec_d)
{
  GstDiscret11AudioParse *discret11_audio_parse =
      GST_DISCRET11_AUDIO_PARSE (discret11_parse);
  GstDiscret11AudioParseConfig *config_ptr =
      gst_discret11_audio_parse_get_config_ptr (discret11_audio_parse, config);

  switch (format) {
    case GST_FORMAT_BYTES:
      *units_per_sec_n = config_ptr->sample_rate * config_ptr->bpf;
      *units_per_sec_d = 1;
      break;

    case GST_FORMAT_DEFAULT:
      *units_per_sec_n = config_ptr->sample_rate;
      *units_per_sec_d = 1;
      break;

    default:
      g_assert_not_reached ();
  }
}

static gboolean
gst_discret11_audio_parse_is_using_sink_caps (GstDiscret11AudioParse *
    discret11_audio_parse)
{
  return discret11_audio_parse->current_config ==
      &(discret11_audio_parse->sink_caps_config);
}

static GstDiscret11AudioParseConfig *
gst_discret11_audio_parse_get_config_ptr (GstDiscret11AudioParse *
    discret11_audio_parse, GstDiscret11ParseConfig config)
{
  g_assert (discret11_audio_parse->current_config != NULL);

  switch (config) {
    case GST_DISCRET11_PARSE_CONFIG_PROPERTIES:
      return &(discret11_audio_parse->properties_config);

    case GST_DISCRET11_PARSE_CONFIG_SINKCAPS:
      return &(discret11_audio_parse->sink_caps_config);

    default:
      g_assert (discret11_audio_parse->current_config != NULL);
      return discret11_audio_parse->current_config;
  }
}

static void
gst_discret11_audio_parse_init_config (GstDiscret11AudioParseConfig * config)
{
  config->ready = FALSE;
  config->format = DEFAULT_FORMAT;
  config->pcm_format = DEFAULT_PCM_FORMAT;
  config->bpf = 0;
  config->sample_rate = DEFAULT_SAMPLE_RATE;
  config->num_channels = DEFAULT_NUM_CHANNELS;
  config->interleaved = DEFAULT_INTERLEAVED;
  config->needs_channel_reordering = FALSE;

  gst_discret11_audio_parse_set_config_channels (config, config->num_channels,
      0, TRUE);
}

static gboolean
gst_discret11_audio_parse_set_config_channels (GstDiscret11AudioParseConfig *
    config, guint num_channels, guint64 channel_mask, gboolean set_positions)
{
  g_assert (num_channels > 0);

  config->num_channels = num_channels;
  /* Setting this to FALSE, since initially, after setting the channels,
   * the default GStreamer channel ordering is used. */
  config->needs_channel_reordering = FALSE;

  /* Set the channel positions based on the given channel mask if set_positions
   * is set to TRUE. A channel mask of 0 signifies that a fallback mask should be
   * used for the given number of channels. */
  if (set_positions) {
    if (channel_mask == 0)
      channel_mask = gst_audio_channel_get_fallback_mask (config->num_channels);

    return gst_audio_channel_positions_from_mask (config->num_channels,
        channel_mask, config->channel_positions);
  } else {
    return TRUE;
  }
}

static gboolean
    gst_discret11_audio_parse_update_channel_reordering_flag
    (GstDiscret11AudioParseConfig * config)
{
  g_assert (config->num_channels > 0);

  /* If the channel_positions array contains channel positions which are in an
   * order that conforms to the valid GStreamer order, ensure that channel
   * reordering is disabled.
   * Otherwise, if the order of the positions in the channel_positions array
   * does not conform to the GStreamer order, ensure it is enabled.
   */

  if (gst_audio_check_valid_channel_positions (config->channel_positions,
          config->num_channels, TRUE)) {

    config->needs_channel_reordering = FALSE;

    return TRUE;
  } else {
    config->needs_channel_reordering = TRUE;
    memcpy (config->reordered_channel_positions, config->channel_positions,
        sizeof (GstAudioChannelPosition) * config->num_channels);
    return
        gst_audio_channel_positions_to_valid_order
        (config->reordered_channel_positions, config->num_channels);
  }
}

static void
gst_discret11_audio_parse_update_config_bpf (GstDiscret11AudioParseConfig *
    config)
{
  switch (config->format) {
    case GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM:
    {
      GstAudioFormatInfo const *fmt_info =
          gst_audio_format_get_info (config->pcm_format);
      g_assert (fmt_info != NULL);

      config->bpf =
          GST_AUDIO_FORMAT_INFO_WIDTH (fmt_info) * config->num_channels / 8;

      break;
    }

    case GST_DISCRET11_AUDIO_PARSE_FORMAT_ALAW:
    case GST_DISCRET11_AUDIO_PARSE_FORMAT_MULAW:
      /* A-law and mu-law both use 1 byte per sample */
      config->bpf = 1 * config->num_channels;
      break;

    default:
      g_assert_not_reached ();
  }
}

static gboolean
gst_discret11_audio_parse_caps_to_config (GstDiscret11AudioParse *
    discret11_audio_parse, GstCaps * caps,
    GstDiscret11AudioParseConfig * config)
{
  gboolean ret = FALSE;
  GstStructure *structure;

  /* Caps might get copied, and the copy needs to be unref'd.
   * Also, the caller retains ownership over the original caps.
   * So, to make this mechanism also work with cases where the
   * caps are *not* copied, ref the original caps here first. */
  gst_caps_ref (caps);

  structure = gst_caps_get_structure (caps, 0);

  /* For unaligned raw data, the output caps stay the same,
   * except that audio/x-unaligned-raw becomes audio/x-raw,
   * since the parser aligns the sample data */
  if (gst_structure_has_name (structure, "audio/x-unaligned-raw")) {
    /* Copy the caps to be able to modify them */
    GstCaps *new_caps = gst_caps_copy (caps);
    gst_caps_unref (caps);
    caps = new_caps;

    /* Change the media type to audio/x-raw , otherwise
     * gst_audio_info_from_caps() won't work */
    structure = gst_caps_get_structure (caps, 0);
    gst_structure_set_name (structure, "audio/x-raw");
  }

  if (gst_structure_has_name (structure, "audio/x-raw")) {
    guint num_channels;
    GstAudioInfo info;
    if (!gst_audio_info_from_caps (&info, caps)) {
      GST_ERROR_OBJECT (discret11_audio_parse,
          "failed to parse caps %" GST_PTR_FORMAT, (gpointer) caps);
      goto done;
    }

    num_channels = GST_AUDIO_INFO_CHANNELS (&info);

    config->format = GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM;
    config->pcm_format = GST_AUDIO_INFO_FORMAT (&info);
    config->bpf = GST_AUDIO_INFO_BPF (&info);
    config->sample_rate = GST_AUDIO_INFO_RATE (&info);
    config->interleaved =
        (GST_AUDIO_INFO_LAYOUT (&info) == GST_AUDIO_LAYOUT_INTERLEAVED);

    gst_discret11_audio_parse_set_config_channels (config, num_channels, 0,
        FALSE);
    memcpy (config->channel_positions, &(GST_AUDIO_INFO_POSITION (&info, 0)),
        sizeof (GstAudioChannelPosition) * num_channels);
  } else if (gst_structure_has_name (structure, "audio/x-alaw")
      || gst_structure_has_name (structure, "audio/x-mulaw")) {
    gint i;
    guint64 channel_mask;
    guint num_channels;

    config->format =
        gst_structure_has_name (structure,
        "audio/x-alaw") ? GST_DISCRET11_AUDIO_PARSE_FORMAT_ALAW :
        GST_DISCRET11_AUDIO_PARSE_FORMAT_MULAW;

    if (!gst_structure_get_int (structure, "rate", &i)) {
      GST_ERROR_OBJECT (discret11_audio_parse,
          "missing rate value in caps %" GST_PTR_FORMAT, (gpointer) caps);
      goto done;
    }
    config->sample_rate = i;

    if (!gst_structure_get_int (structure, "channels", &i)) {
      GST_ERROR_OBJECT (discret11_audio_parse,
          "missing channels value in caps %" GST_PTR_FORMAT, (gpointer) caps);
      goto done;
    }
    num_channels = i;

    if (!gst_structure_get (structure, "channel-mask", GST_TYPE_BITMASK,
            &channel_mask, NULL)) {
      channel_mask = gst_audio_channel_get_fallback_mask (num_channels);
      GST_DEBUG_OBJECT (discret11_audio_parse,
          "input caps have no channel mask - using fallback mask %#"
          G_GINT64_MODIFIER "x for %u channels", channel_mask, num_channels);
    }

    if (!gst_discret11_audio_parse_set_config_channels (config, num_channels,
            channel_mask, TRUE)) {
      GST_ERROR_OBJECT (discret11_audio_parse,
          "could not use channel mask %#" G_GINT64_MODIFIER
          "x for channel positions", channel_mask);
      goto done;
    }

    /* A-law and mu-law both use 1 byte per sample */
    config->bpf = 1 * num_channels;
  } else {
    GST_ERROR_OBJECT (discret11_audio_parse,
        "caps %" GST_PTR_FORMAT " have an unsupported media type",
        (gpointer) caps);
    goto done;
  }

  ret = TRUE;

done:
  gst_caps_unref (caps);
  if (ret)
    config->ready = TRUE;
  return ret;
}

static gboolean
gst_discret11_audio_parse_config_to_caps (GstDiscret11AudioParse *
    discret11_audio_parse, GstCaps ** caps,
    GstDiscret11AudioParseConfig * config)
{
  gboolean ret = TRUE;
  GstAudioChannelPosition *channel_positions;

  g_assert (caps != NULL);

  if (config->bpf == 0) {
    GST_ERROR_OBJECT (discret11_audio_parse,
        "cannot convert config to caps - config not filled with valid values");
    *caps = NULL;
    return FALSE;
  }

  channel_positions =
      config->
      needs_channel_reordering ? &(config->reordered_channel_positions[0]) :
      &(config->channel_positions[0]);

  switch (config->format) {
    case GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM:
    {
      GstAudioInfo info;
      gst_audio_info_init (&info);
      gst_audio_info_set_format (&info,
          config->pcm_format,
          config->sample_rate, config->num_channels, channel_positions);

      *caps = gst_audio_info_to_caps (&info);

      break;
    }

    case GST_DISCRET11_AUDIO_PARSE_FORMAT_ALAW:
    case GST_DISCRET11_AUDIO_PARSE_FORMAT_MULAW:
    {
      guint64 channel_mask;

      if (!gst_audio_channel_positions_to_mask (channel_positions,
              config->num_channels, TRUE, &channel_mask)) {
        GST_ERROR_OBJECT (discret11_audio_parse, "invalid channel positions");
        ret = FALSE;
        break;
      }

      *caps = gst_caps_new_simple (
          (config->format ==
              GST_DISCRET11_AUDIO_PARSE_FORMAT_ALAW) ? "audio/x-alaw" :
          "audio/x-mulaw", "rate", G_TYPE_INT, config->sample_rate, "channels",
          G_TYPE_INT, config->num_channels, "channel-mask", GST_TYPE_BITMASK,
          channel_mask, NULL);

      break;
    }

    default:
      g_assert_not_reached ();
      ret = FALSE;
  }

  if (!ret)
    *caps = NULL;

  return ret;
}

GType
gst_discret11_audio_parse_format_get_type (void)
{
  static GType audio_parse_format_gtype = 0;
  static const GEnumValue types[] = {
    {GST_DISCRET11_AUDIO_PARSE_FORMAT_PCM, "PCM", "pcm"},
    {GST_DISCRET11_AUDIO_PARSE_FORMAT_ALAW, "A-Law", "alaw"},
    {GST_DISCRET11_AUDIO_PARSE_FORMAT_MULAW, "\302\265-Law", "mulaw"},
    {0, NULL, NULL}
  };

  if (!audio_parse_format_gtype)
    audio_parse_format_gtype =
        g_enum_register_static ("GstDiscret11ParseFormat", types);

  return audio_parse_format_gtype;
}
