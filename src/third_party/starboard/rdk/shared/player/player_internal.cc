//
// Copyright 2020 Comcast Cable Communications Management, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0
#include "third_party/starboard/rdk/shared/player/player_internal.h"

#include <inttypes.h>
#include <stdint.h>

#include <glib.h>
#include <gst/app/gstappsrc.h>
#include <gst/audio/streamvolume.h>
#include <gst/base/gstbytewriter.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <map>
#include <string>

#include "starboard/common/mutex.h"
#include "starboard/thread.h"
#include "starboard/time.h"
#include "starboard/memory.h"
#include "third_party/starboard/rdk/shared/drm/drm_system_ocdm.h"
#include "third_party/starboard/rdk/shared/media/gst_media_utils.h"

namespace third_party {
namespace starboard {
namespace rdk {
namespace shared {
namespace player {

static constexpr int kMaxNumberOfSamplesPerWrite = 1;

// static
int Player::MaxNumberOfSamplesPerWrite() {
  return kMaxNumberOfSamplesPerWrite;
}

using third_party::starboard::rdk::shared::drm::DrmSystemOcdm;
using third_party::starboard::rdk::shared::media::CodecToGstCaps;

// **************************** GST/GLIB Helpers **************************** //

namespace {

GST_DEBUG_CATEGORY(cobalt_gst_player_debug);
#define GST_CAT_DEFAULT cobalt_gst_player_debug

#if !defined(GST_HAS_HDR_SUPPORT)
#if GST_CHECK_VERSION(1, 18, 0) || (defined(__has_include) &&  __has_include("gstreamer-1.0/gst/video/video-hdr.h"))
#define GST_HAS_HDR_SUPPORT 1
#endif
#endif

static GSourceFuncs SourceFunctions = {
    // prepare
    nullptr,
    // check
    nullptr,
    // dispatch
    [](GSource* source, GSourceFunc callback, gpointer userData) -> gboolean {
      if (g_source_get_ready_time(source) == -1)
        return G_SOURCE_CONTINUE;
      g_source_set_ready_time(source, -1);
      return callback(userData);
    },
    // finalize
    nullptr,
    // closure_callback
    nullptr,
    // closure_marshall
    nullptr,
};

unsigned getGstPlayFlag(const char* nick) {
  static GFlagsClass* flagsClass = static_cast<GFlagsClass*>(
      g_type_class_ref(g_type_from_name("GstPlayFlags")));
  SB_DCHECK(flagsClass);

  GFlagsValue* flag = g_flags_get_value_by_nick(flagsClass, nick);
  if (!flag)
    return 0;

  return flag->value;
}

G_BEGIN_DECLS

#define GST_COBALT_TYPE_SRC (gst_cobalt_src_get_type())
#define GST_COBALT_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_COBALT_TYPE_SRC, GstCobaltSrc))
#define GST_COBALT_SRC_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_COBALT_TYPE_SRC, GstCobaltSrcClass))
#define GST_IS_COLABT_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_COBALT_TYPE_SRC))
#define GST_IS_COBALT_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_COBALT_TYPE_SRC))

typedef struct _GstCobaltSrc GstCobaltSrc;
typedef struct _GstCobaltSrcClass GstCobaltSrcClass;
typedef struct _GstCobaltSrcPrivate GstCobaltSrcPrivate;

struct _GstCobaltSrc {
  GstBin parent;
  GstCobaltSrcPrivate* priv;
};

struct _GstCobaltSrcClass {
  GstBinClass parentClass;
};

GType gst_cobalt_src_get_type(void);

G_END_DECLS

struct _GstCobaltSrcPrivate {
  gchar* uri;
  guint pad_number;
  gboolean async_start;
  gboolean async_done;
};

enum { PROP_0, PROP_LOCATION };

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE("src_%u",
                            GST_PAD_SRC,
                            GST_PAD_SOMETIMES,
                            GST_STATIC_CAPS_ANY);

static void gst_cobalt_src_uri_handler_init(gpointer gIface,
                                            gpointer ifaceData);
#define gst_cobalt_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstCobaltSrc,
                        gst_cobalt_src,
                        GST_TYPE_BIN,
                        G_ADD_PRIVATE(GstCobaltSrc)
                        G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER,
                                              gst_cobalt_src_uri_handler_init));

static void gst_cobalt_src_init(GstCobaltSrc* src) {
  GstCobaltSrcPrivate* priv = (GstCobaltSrcPrivate*)gst_cobalt_src_get_instance_private(src);
  src->priv = priv;
  src->priv->pad_number = 0;
  src->priv->async_start = FALSE;
  src->priv->async_done = FALSE;
  g_object_set(GST_BIN(src), "message-forward", TRUE, NULL);
}

static void gst_cobalt_src_dispose(GObject* object) {
  GST_CALL_PARENT(G_OBJECT_CLASS, dispose, (object));
}

static void gst_cobalt_src_finalize(GObject* object) {
  GstCobaltSrc* src = GST_COBALT_SRC(object);
  GstCobaltSrcPrivate* priv = src->priv;

  g_free(priv->uri);
  priv->~GstCobaltSrcPrivate();

  GST_CALL_PARENT(G_OBJECT_CLASS, finalize, (object));
}

static void gst_cobalt_src_set_property(GObject* object,
                                        guint propID,
                                        const GValue* value,
                                        GParamSpec* pspec) {
  GstCobaltSrc* src = GST_COBALT_SRC(object);

  switch (propID) {
    case PROP_LOCATION:
      gst_uri_handler_set_uri(reinterpret_cast<GstURIHandler*>(src),
                              g_value_get_string(value), 0);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propID, pspec);
      break;
  }
}

static void gst_cobalt_src_get_property(GObject* object,
                                        guint propID,
                                        GValue* value,
                                        GParamSpec* pspec) {
  GstCobaltSrc* src = GST_COBALT_SRC(object);
  GstCobaltSrcPrivate* priv = src->priv;

  GST_OBJECT_LOCK(src);
  switch (propID) {
    case PROP_LOCATION:
      g_value_set_string(value, priv->uri);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propID, pspec);
      break;
  }
  GST_OBJECT_UNLOCK(src);
}

// uri handler interface
static GstURIType gst_cobalt_src_uri_get_type(GType) {
  return GST_URI_SRC;
}

const gchar* const* gst_cobalt_src_get_protocols(GType) {
  static const char* protocols[] = {"cobalt", 0};
  return protocols;
}

static gchar* gst_cobalt_src_get_uri(GstURIHandler* handler) {
  GstCobaltSrc* src = GST_COBALT_SRC(handler);
  gchar* ret;

  GST_OBJECT_LOCK(src);
  ret = g_strdup(src->priv->uri);
  GST_OBJECT_UNLOCK(src);
  return ret;
}

static gboolean gst_cobalt_src_set_uri(GstURIHandler* handler,
                                       const gchar* uri,
                                       GError** error) {
  GstCobaltSrc* src = GST_COBALT_SRC(handler);
  GstCobaltSrcPrivate* priv = src->priv;

  if (GST_STATE(src) >= GST_STATE_PAUSED) {
    GST_ERROR_OBJECT(src, "URI can only be set in states < PAUSED");
    return FALSE;
  }

  GST_OBJECT_LOCK(src);

  g_free(priv->uri);
  priv->uri = 0;

  if (!uri) {
    GST_OBJECT_UNLOCK(src);
    return TRUE;
  }

  priv->uri = g_strdup(uri);
  GST_OBJECT_UNLOCK(src);
  return TRUE;
}

static void gst_cobalt_src_uri_handler_init(gpointer gIface, gpointer) {
  GstURIHandlerInterface* iface = (GstURIHandlerInterface*)gIface;

  iface->get_type = gst_cobalt_src_uri_get_type;
  iface->get_protocols = gst_cobalt_src_get_protocols;
  iface->get_uri = gst_cobalt_src_get_uri;
  iface->set_uri = gst_cobalt_src_set_uri;
}

static gboolean gst_cobalt_src_query_with_parent(GstPad* pad,
                                                 GstObject* parent,
                                                 GstQuery* query) {
  GstCobaltSrc* src = GST_COBALT_SRC(GST_ELEMENT(parent));
  gboolean result = FALSE;

  switch (GST_QUERY_TYPE(query)) {
    default: {
      GstPad* target = gst_ghost_pad_get_target(GST_GHOST_PAD_CAST(pad));
      // Forward the query to the proxy target pad.
      if (target)
        result = gst_pad_query(target, query);
      gst_object_unref(target);
      break;
    }
  }

  return result;
}

void gst_cobalt_src_handle_message(GstBin* bin, GstMessage* message) {
  GstCobaltSrc* src = GST_COBALT_SRC(GST_ELEMENT(bin));

  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
      gboolean emit_eos = TRUE;
      GstPad* pad = gst_element_get_static_pad(
          GST_ELEMENT(GST_MESSAGE_SRC(message)), "src");

      GST_DEBUG_OBJECT(src, "EOS received from %s",
                       GST_MESSAGE_SRC_NAME(message));
      g_object_set_data(G_OBJECT(pad), "is-eos", GINT_TO_POINTER(1));
      gst_object_unref(pad);
      for (guint i = 0; i < src->priv->pad_number; i++) {
        gchar* name = g_strdup_printf("src_%u", i);
        GstPad* src_pad = gst_element_get_static_pad(GST_ELEMENT(src), name);
        GstPad* target = gst_ghost_pad_get_target(GST_GHOST_PAD_CAST(src_pad));
        gint is_eos =
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(target), "is-eos"));
        gst_object_unref(target);
        gst_object_unref(src_pad);
        g_free(name);

        if (!is_eos) {
          emit_eos = FALSE;
          break;
        }
      }

      gst_message_unref(message);

      if (emit_eos) {
        GST_DEBUG_OBJECT(src,
                         "All appsrc elements are EOS, emitting event now.");
        gst_element_send_event(GST_ELEMENT(bin), gst_event_new_eos());
      }
      break;
    }
    default:
      GST_BIN_CLASS(parent_class)->handle_message(bin, message);
      break;
  }
}

void gst_cobalt_src_setup_and_add_app_src(GstElement* element,
                                          GstElement* appsrc,
                                          const char* caps,
                                          GstAppSrcCallbacks* callbacks,
                                          gpointer user_data) {
  if (caps) {
    GstCaps* gst_caps = gst_caps_from_string(caps);
    gst_app_src_set_caps(GST_APP_SRC(appsrc), gst_caps);
    gst_caps_unref(gst_caps);
  }

  g_object_set(appsrc, "block", FALSE, "format", GST_FORMAT_TIME, "stream-type",
               GST_APP_STREAM_TYPE_SEEKABLE, nullptr);
  gst_app_src_set_callbacks(GST_APP_SRC(appsrc), callbacks, user_data, nullptr);
  gst_app_src_set_max_bytes(GST_APP_SRC(appsrc), 16 * 1024 * 1024);

  GstCobaltSrc* src = GST_COBALT_SRC(element);
  gchar* name = g_strdup_printf("src_%u", src->priv->pad_number);
  src->priv->pad_number++;
  gst_bin_add(GST_BIN(element), appsrc);
  GstPad* target = gst_element_get_static_pad(appsrc, "src");
  GstPad* pad = gst_ghost_pad_new(name, target);
  gst_pad_set_query_function(pad, gst_cobalt_src_query_with_parent);
  gst_pad_set_active(pad, TRUE);

  gst_element_add_pad(element, pad);
  GST_OBJECT_FLAG_SET(pad, GST_PAD_FLAG_NEED_PARENT);

  gst_element_sync_state_with_parent(appsrc);

  g_free(name);
  gst_object_unref(target);
}

static void gst_cobalt_src_do_async_start(GstCobaltSrc* src) {
  GstCobaltSrcPrivate* priv = src->priv;
  if (priv->async_done)
    return;
  priv->async_start = TRUE;
  GST_BIN_CLASS(parent_class)
      ->handle_message(GST_BIN(src),
                       gst_message_new_async_start(GST_OBJECT(src)));
}

static void gst_cobalt_src_do_async_done(GstCobaltSrc* src) {
  GstCobaltSrcPrivate* priv = src->priv;
  if (priv->async_start) {
    GST_BIN_CLASS(parent_class)
        ->handle_message(
            GST_BIN(src),
            gst_message_new_async_done(GST_OBJECT(src), GST_CLOCK_TIME_NONE));
    priv->async_start = FALSE;
    priv->async_done = TRUE;
  }
}

void gst_cobalt_src_all_app_srcs_added(GstElement* element) {
  GstCobaltSrc* src = GST_COBALT_SRC(element);

  GST_DEBUG_OBJECT(src,
                   "===> All sources registered, completing state-change "
                   "(TID:%d)",
                   SbThreadGetId());
  gst_element_no_more_pads(element);
  gst_cobalt_src_do_async_done(src);
}

static GstStateChangeReturn gst_cobalt_src_change_state(
    GstElement* element,
    GstStateChange transition) {
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstCobaltSrc* src = GST_COBALT_SRC(element);
  GstCobaltSrcPrivate* priv = src->priv;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_cobalt_src_do_async_start(src);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
  if (G_UNLIKELY(ret == GST_STATE_CHANGE_FAILURE)) {
    gst_cobalt_src_do_async_done(src);
    return ret;
  }

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED: {
      if (!priv->async_done)
        ret = GST_STATE_CHANGE_ASYNC;
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY: {
      gst_cobalt_src_do_async_done(src);
      break;
    }
    default:
      break;
  }

  return ret;
}

static void gst_cobalt_src_class_init(GstCobaltSrcClass* klass) {
  GObjectClass* oklass = G_OBJECT_CLASS(klass);
  GstElementClass* eklass = GST_ELEMENT_CLASS(klass);
  GstBinClass* bklass = GST_BIN_CLASS(klass);

  oklass->dispose = gst_cobalt_src_dispose;
  oklass->finalize = gst_cobalt_src_finalize;
  oklass->set_property = gst_cobalt_src_set_property;
  oklass->get_property = gst_cobalt_src_get_property;

  gst_element_class_add_pad_template(
      eklass, gst_static_pad_template_get(&src_template));
  gst_element_class_set_metadata(eklass, "Cobalt source element", "Source",
                                 "Handles data incoming from the Cobalt player",
                                 "Pawel Stanek <p.stanek@metrological.com>");
  g_object_class_install_property(
      oklass, PROP_LOCATION,
      g_param_spec_string(
          "location", "location", "Location to read from", 0,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  bklass->handle_message = GST_DEBUG_FUNCPTR(gst_cobalt_src_handle_message);
  eklass->change_state = GST_DEBUG_FUNCPTR(gst_cobalt_src_change_state);
}

#if defined(GST_HAS_HDR_SUPPORT) && GST_HAS_HDR_SUPPORT
static GstVideoColorRange RangeIdToGstVideoColorRange(SbMediaRangeId value) {
  switch (value) {
    case kSbMediaRangeIdLimited:
      return GST_VIDEO_COLOR_RANGE_16_235;
    case kSbMediaRangeIdFull:
      return GST_VIDEO_COLOR_RANGE_0_255;
    default:
    case kSbMediaRangeIdUnspecified:
      return GST_VIDEO_COLOR_RANGE_UNKNOWN;
  }
}

static GstVideoColorMatrix MatrixIdToGstVideoColorMatrix(SbMediaMatrixId value) {
  switch (value) {
    case kSbMediaMatrixIdRgb:
      return GST_VIDEO_COLOR_MATRIX_RGB;
    case kSbMediaMatrixIdBt709:
      return GST_VIDEO_COLOR_MATRIX_BT709;
    case kSbMediaMatrixIdFcc:
      return GST_VIDEO_COLOR_MATRIX_FCC;
    case kSbMediaMatrixIdBt470Bg:
    case kSbMediaMatrixIdSmpte170M:
      return GST_VIDEO_COLOR_MATRIX_BT601;
    case kSbMediaMatrixIdSmpte240M:
      return GST_VIDEO_COLOR_MATRIX_SMPTE240M;
    case kSbMediaMatrixIdBt2020NonconstantLuminance:
      return GST_VIDEO_COLOR_MATRIX_BT2020;
    case kSbMediaMatrixIdUnspecified:
    default:
      return GST_VIDEO_COLOR_MATRIX_UNKNOWN;
  }
}

static GstVideoTransferFunction TransferIdToGstVideoTransferFunction(SbMediaTransferId value) {
  switch (value) {
    case kSbMediaTransferIdBt709:
    case kSbMediaTransferIdSmpte170M:
      return GST_VIDEO_TRANSFER_BT709;
    case kSbMediaTransferIdGamma22:
      return GST_VIDEO_TRANSFER_GAMMA22;
    case kSbMediaTransferIdGamma28:
      return GST_VIDEO_TRANSFER_GAMMA28;
    case kSbMediaTransferIdSmpte240M:
      return GST_VIDEO_TRANSFER_SMPTE240M;
    case kSbMediaTransferIdLinear:
      return GST_VIDEO_TRANSFER_GAMMA10;
    case kSbMediaTransferIdLog:
      return GST_VIDEO_TRANSFER_LOG100;
    case kSbMediaTransferIdLogSqrt:
      return GST_VIDEO_TRANSFER_LOG316;
    case kSbMediaTransferIdIec6196621:
      return GST_VIDEO_TRANSFER_SRGB;
    case kSbMediaTransferId10BitBt2020:
      return GST_VIDEO_TRANSFER_BT2020_10;
    case kSbMediaTransferId12BitBt2020:
      return GST_VIDEO_TRANSFER_BT2020_12;
    case kSbMediaTransferIdSmpteSt2084:
      return GST_VIDEO_TRANSFER_SMPTE_ST_2084;
    case kSbMediaTransferIdAribStdB67:
      return GST_VIDEO_TRANSFER_ARIB_STD_B67;
    case kSbMediaTransferIdUnspecified:
    default:
      return GST_VIDEO_TRANSFER_UNKNOWN;
  }
}

static GstVideoColorPrimaries PrimaryIdToGstVideoColorPrimaries(SbMediaPrimaryId value) {
  switch (value) {
    case kSbMediaPrimaryIdBt709:
      return GST_VIDEO_COLOR_PRIMARIES_BT709;
    case kSbMediaPrimaryIdBt470M:
      return GST_VIDEO_COLOR_PRIMARIES_BT470M;
    case kSbMediaPrimaryIdBt470Bg:
      return GST_VIDEO_COLOR_PRIMARIES_BT470BG;
    case kSbMediaPrimaryIdSmpte170M:
      return GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
    case kSbMediaPrimaryIdSmpte240M:
      return GST_VIDEO_COLOR_PRIMARIES_SMPTE240M;
    case kSbMediaPrimaryIdFilm:
      return GST_VIDEO_COLOR_PRIMARIES_FILM;
    case kSbMediaPrimaryIdBt2020:
      return GST_VIDEO_COLOR_PRIMARIES_BT2020;
    case kSbMediaPrimaryIdUnspecified:
    default:
      return GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;
  }
}

static void AddColorMetadataToGstCaps(GstCaps* caps, const SbMediaColorMetadata& color_metadata) {
  GstVideoColorimetry colorimetry;
  colorimetry.range = RangeIdToGstVideoColorRange(color_metadata.range);
  colorimetry.matrix = MatrixIdToGstVideoColorMatrix(color_metadata.matrix);
  colorimetry.transfer = TransferIdToGstVideoTransferFunction(color_metadata.transfer);
  colorimetry.primaries = PrimaryIdToGstVideoColorPrimaries(color_metadata.primaries);

  if (colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN ||
      colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN ||
      colorimetry.transfer != GST_VIDEO_TRANSFER_UNKNOWN ||
      colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) {
    gchar *tmp =
      gst_video_colorimetry_to_string (&colorimetry);
    gst_caps_set_simple (caps, "colorimetry", G_TYPE_STRING, tmp, NULL);
    GST_DEBUG ("Setting \"colorimetry\" to %s", tmp);
    g_free (tmp);
  }

  GstVideoMasteringDisplayMetadata mastering_display_metadata;
  gst_video_mastering_display_metadata_init (&mastering_display_metadata);
  mastering_display_metadata.Rx = color_metadata.mastering_metadata.primary_r_chromaticity_x;
  mastering_display_metadata.Ry = color_metadata.mastering_metadata.primary_r_chromaticity_y;
  mastering_display_metadata.Gx = color_metadata.mastering_metadata.primary_g_chromaticity_x;
  mastering_display_metadata.Gy = color_metadata.mastering_metadata.primary_g_chromaticity_y;
  mastering_display_metadata.Bx = color_metadata.mastering_metadata.primary_b_chromaticity_x;
  mastering_display_metadata.By = color_metadata.mastering_metadata.primary_b_chromaticity_y;
  mastering_display_metadata.Wx = color_metadata.mastering_metadata.white_point_chromaticity_x;
  mastering_display_metadata.Wy = color_metadata.mastering_metadata.white_point_chromaticity_y;
  mastering_display_metadata.max_luma = color_metadata.mastering_metadata.luminance_max;
  mastering_display_metadata.min_luma = color_metadata.mastering_metadata.luminance_min;

  if (gst_video_mastering_display_metadata_has_primaries(&mastering_display_metadata) &&
      gst_video_mastering_display_metadata_has_luminance(&mastering_display_metadata) ) {
    gchar *tmp =
      gst_video_mastering_display_metadata_to_caps_string
      (&mastering_display_metadata);
    gst_caps_set_simple (caps, "mastering-display-metadata", G_TYPE_STRING, tmp, NULL);
    GST_DEBUG ("Setting \"mastering-display-metadata\" to %s", tmp);
    g_free (tmp);
  }

  if (color_metadata.max_cll && color_metadata.max_fall) {
    GstVideoContentLightLevel content_light_level;
    content_light_level.maxCLL = color_metadata.max_cll;
    content_light_level.maxFALL = color_metadata.max_fall;
    gchar *tmp = gst_video_content_light_level_to_caps_string(&content_light_level);
    gst_caps_set_simple (caps, "content-light-level", G_TYPE_STRING, tmp, NULL);
    GST_DEBUG ("setting \"content-light-level\" to %s", tmp);
    g_free (tmp);
  }
}
#else
static void AddColorMetadataToGstCaps(GstCaps*, const SbMediaColorMetadata&) {}
#endif

static int CompareColorMetadata(const SbMediaColorMetadata& lhs, const SbMediaColorMetadata& rhs) {
  return SbMemoryCompare(&lhs, &rhs, sizeof(SbMediaColorMetadata));
}

static void AddVideoInfoToGstCaps(const SbMediaVideoSampleInfo& info, GstCaps* caps) {
  AddColorMetadataToGstCaps(caps, info.color_metadata);
  gst_caps_set_simple (caps,
    "width", G_TYPE_INT, info.frame_width,
    "height", G_TYPE_INT, info.frame_height,
    NULL);
}

}  // namespace

// ********************************* Player ******************************** //
namespace {

const int kMaxIvSize = 16;

enum class MediaType {
  kNone = 0,
  kAudio = 1,
  kVideo = 2,
  kBoth = kAudio | kVideo
};

constexpr char kClearSamplesKey[] = "fake-key-magic";

struct Task {
  virtual ~Task() {}
  virtual void Do() = 0;
  virtual void PrintInfo() = 0;
};

static const char* PlayerStateToStr(SbPlayerState state) {
#define CASE(x) case x: return #x
    switch(state) {
        CASE(kSbPlayerStateInitialized);
        CASE(kSbPlayerStatePrerolling);
        CASE(kSbPlayerStatePresenting);
        CASE(kSbPlayerStateEndOfStream);
        CASE(kSbPlayerStateDestroyed);
    }
#undef CASE
    return "unknown";
}

static const char* DecoderStateToStr(SbPlayerDecoderState state) {
#define CASE(x) case x: return #x
    switch(state) {
        CASE(kSbPlayerDecoderStateNeedsData);
    }
#undef CASE
    return "unknown";
}

class PlayerStatusTask : public Task {
 public:
  PlayerStatusTask(SbPlayerStatusFunc func,
                   SbPlayer player,
                   int ticket,
                   void* ctx,
                   SbPlayerState state) {
    this->func_ = func;
    this->player_ = player;
    this->ticket_ = ticket;
    this->ctx_ = ctx;
    this->state_ = state;
  }

  ~PlayerStatusTask() override {}

  void Do() override { func_(player_, ctx_, state_, ticket_); }

  void PrintInfo() override {
    GST_TRACE("PlayerStatusTask state:%d (%s), ticket:%d", state_, PlayerStateToStr(state_), ticket_);
  }

 private:
  SbPlayerStatusFunc func_;
  SbPlayer player_;
  int ticket_;
  void* ctx_;
  SbPlayerState state_;
};

class PlayerDestroyedTask : public PlayerStatusTask {
 public:
  PlayerDestroyedTask(SbPlayerStatusFunc func,
                      SbPlayer player,
                      int ticket,
                      void* ctx,
                      GMainLoop* loop)
      : PlayerStatusTask(func, player, ticket, ctx, kSbPlayerStateDestroyed) {
    this->loop_ = loop;
  }

  ~PlayerDestroyedTask() override {}

  void Do() override {
    PlayerStatusTask::Do();
    g_main_loop_quit(loop_);
  }

  void PrintInfo() override {
    GST_TRACE("PlayerDestroyedTask: START");
    PlayerStatusTask::PrintInfo();
    GST_TRACE("PlayerDestroyedTask: END");
  }

 private:
  GMainLoop* loop_;
};

class DecoderStatusTask : public Task {
 public:
  DecoderStatusTask(SbPlayerDecoderStatusFunc func,
                    SbPlayer player,
                    int ticket,
                    void* ctx,
                    SbPlayerDecoderState state,
                    MediaType media) {
    this->func_ = func;
    this->player_ = player;
    this->ticket_ = ticket;
    this->ctx_ = ctx;
    this->state_ = state;
    this->media_ = media;
  }

  ~DecoderStatusTask() override {}

  void Do() override {
    if ((static_cast<int>(media_) & static_cast<int>(MediaType::kAudio)) != 0)
      func_(player_, ctx_, kSbMediaTypeAudio, state_, ticket_);
    if ((static_cast<int>(media_) & static_cast<int>(MediaType::kVideo)) != 0)
      func_(player_, ctx_, kSbMediaTypeVideo, state_, ticket_);
  }

  void PrintInfo() override {
    GST_TRACE("DecoderStatusTask state:%d (%s), ticket:%d, media:%d", state_,
              DecoderStateToStr(state_), ticket_, static_cast<int>(media_));
  }

 private:
  SbPlayerDecoderStatusFunc func_;
  SbPlayer player_;
  int ticket_;
  void* ctx_;
  SbPlayerDecoderState state_;
  MediaType media_;
};

class PlayerErrorTask : public Task {
 public:
  PlayerErrorTask(SbPlayerErrorFunc func,
                  SbPlayer player,
                  void* ctx,
                  SbPlayerError error,
                  const char* msg) {
    this->func_ = func;
    this->player_ = player;
    this->ctx_ = ctx;
    this->error_ = error;
    this->msg_ = msg;
  }

  ~PlayerErrorTask() override {}

  void Do() override { func_(player_, ctx_, error_, msg_.c_str()); }

  void PrintInfo() override { GST_TRACE("PlayerErrorTask"); }

 private:
  SbPlayerErrorFunc func_;
  SbPlayer player_;
  SbPlayerError error_;
  void* ctx_;
  std::string msg_;
};

class PlayerImpl : public Player, public DrmSystemOcdm::Observer {
 public:
  PlayerImpl(SbPlayer player,
             SbWindow window,
             SbMediaVideoCodec video_codec,
             SbMediaAudioCodec audio_codec,
             SbDrmSystem drm_system,
             const SbMediaAudioSampleInfo& audio_sample_info,
             const char* max_video_capabilities,
             SbPlayerDeallocateSampleFunc sample_deallocate_func,
             SbPlayerDecoderStatusFunc decoder_status_func,
             SbPlayerStatusFunc player_status_func,
             SbPlayerErrorFunc player_error_func,
             void* context,
             SbPlayerOutputMode output_mode,
             SbDecodeTargetGraphicsContextProvider* provider);
  ~PlayerImpl() override;

  // Player
  void MarkEOS(SbMediaType stream_type) override;
  void WriteSample(SbMediaType sample_type,
                   const SbPlayerSampleInfo* sample_infos,
                   int number_of_sample_infos) override;
  void SetVolume(double volume) override;
  void Seek(SbTime seek_to_timestamp, int ticket) override;
  bool SetRate(double rate) override;
  void GetInfo(SbPlayerInfo2* info) override;
  void SetBounds(int zindex, int x, int y, int w, int h) override;

  // DrmSystemOcdm::Observer
  void OnKeyReady(const uint8_t* key, size_t key_len) override;

 private:
  enum class State {
    kNull,
    kInitial,
    kInitialPreroll,
    kPrerollAfterSeek,
    kPresenting,
  };

  enum MediaTimestampIndex {
    kAudioIndex,
    kVideoIndex,
    kMediaNumber,
  };

  struct DispatchData {
    DispatchData& operator=(DispatchData&) = delete;
    DispatchData(const DispatchData&) = delete;

    DispatchData(Task* task, GSource* src) : task_(task), src_(src) {
      SB_DCHECK(task_ && src_);
    }

    ~DispatchData() {
      delete task_;
      g_source_unref(src_);
    }

    Task* task() const { return task_; }

   private:
    Task* task_{nullptr};
    GSource* src_{nullptr};
  };

  class PendingSample {
   public:
    PendingSample() = delete;
    PendingSample& operator=(const PendingSample&) = delete;
    PendingSample(const PendingSample&) = delete;

    PendingSample& operator=(PendingSample&& other) {
      type_ = other.type_;
      buffer_ = other.buffer_;
      other.buffer_ = nullptr;
      buffer_copy_ = other.buffer_copy_;
      other.buffer_copy_ = nullptr;
      iv_ = other.iv_;
      other.iv_ = nullptr;
      subsamples_ = other.subsamples_;
      other.subsamples_ = nullptr;
      subsamples_count_ = other.subsamples_count_;
      other.subsamples_count_ = 0;
      key_ = other.key_;
      other.key_ = nullptr;
      return *this;
    }

    PendingSample(PendingSample&& other) { operator=(std::move(other)); }

    PendingSample(SbMediaType type,
                  GstBuffer* buffer,
                  GstBuffer* iv,
                  GstBuffer* subsamples,
                  int32_t subsamples_count,
                  GstBuffer* key)
        : type_(type),
          buffer_(buffer),
          iv_(iv),
          subsamples_(subsamples),
          subsamples_count_(subsamples_count),
          key_(key) {
      SB_DCHECK(gst_buffer_is_writable(buffer));
      buffer_copy_ = gst_buffer_copy_deep(buffer);
    }

    ~PendingSample() {
      if (key_)
        gst_buffer_unref(key_);
      if (subsamples_)
        gst_buffer_unref(subsamples_);
      if (iv_)
        gst_buffer_unref(iv_);
      if (buffer_)
        gst_buffer_unref(buffer_);
      if (buffer_copy_)
        gst_buffer_unref(buffer_copy_);
    }

    void Written() { buffer_copy_ = gst_buffer_copy_deep(buffer_); }

    SbMediaType Type() const { return type_; }
    GstBuffer* Buffer() const { return buffer_copy_; }
    GstBuffer* Iv() const { return iv_; }
    GstBuffer* Subsamples() const { return subsamples_; }
    int32_t SubsamplesCount() const { return subsamples_count_; }
    GstBuffer* Key() const { return key_; }

   private:
    SbMediaType type_;
    GstBuffer* buffer_;
    GstBuffer* buffer_copy_;
    GstBuffer* iv_;
    GstBuffer* subsamples_;
    int32_t subsamples_count_;
    GstBuffer* key_;
  };

  struct PendingBounds {
    PendingBounds() : x{0}, y{0}, w{0}, h{0} {}
    PendingBounds(int ix, int iy, int iw, int ih)
        : x{ix}, y{iy}, w{iw}, h{ih} {}
    bool IsEmpty() { return w == 0 && h == 0; }
    int x;
    int y;
    int w;
    int h;
  };

  using PendingSamples = std::vector<PendingSample>;
  using SamplesPendingKey = std::map<std::string, PendingSamples>;

  static gboolean BusMessageCallback(GstBus* bus,
                                     GstMessage* message,
                                     gpointer user_data);
  static void* ThreadEntryPoint(void* context);
  static gboolean WorkerTask(gpointer user_data);
  static gboolean FinishSourceSetup(gpointer user_data);
  static void AppSrcNeedData(GstAppSrc* src, guint length, gpointer user_data);
  static void AppSrcEnoughData(GstAppSrc* src, gpointer user_data);
  static gboolean AppSrcSeekData(GstAppSrc* src,
                                 guint64 offset,
                                 gpointer user_data);
  static void SetupSource(GstElement* pipeline,
                          GstElement* source,
                          PlayerImpl* self);
  bool ChangePipelineState(GstState state) const;
  void DispatchOnWorkerThread(Task* task) const;
  gint64 GetPosition() const;
  bool WriteSample(SbMediaType sample_type,
                   GstBuffer* buffer,
                   const std::string& session_id,
                   GstBuffer* subsample = nullptr,
                   int32_t subsamples_count = 0,
                   GstBuffer* iv = nullptr,
                   GstBuffer* key = nullptr);
  MediaType GetBothMediaTypeTakingCodecsIntoAccount() const;
  void RecordTimestamp(SbMediaType type, SbTime timestamp);
  SbTime MinTimestamp(MediaType* origin) const;

  void DecoderNeedsData(::starboard::ScopedLock&, MediaType media) const {
    int need_data = static_cast<int>(media);
    if (media != MediaType::kNone && (decoder_state_data_ & need_data) == need_data) {
      GST_LOG("Already sent 'kSbPlayerDecoderStateNeedsData', ignoring new request");
      return;
    }
    if (media != MediaType::kNone && (eos_data_ & need_data) == need_data) {
      GST_LOG("Stream(%d) already ended, ignoring needs data request", need_data);
      return;
    }
    decoder_state_data_ |= need_data;
    DispatchOnWorkerThread(new DecoderStatusTask(
      decoder_status_func_, player_, ticket_, context_,
      kSbPlayerDecoderStateNeedsData, media));
  }

  SbPlayer player_;
  SbWindow window_;
  SbMediaVideoCodec video_codec_;
  SbMediaAudioCodec audio_codec_;
  DrmSystemOcdm* drm_system_;
  const SbMediaAudioSampleInfo audio_sample_info_;
  const char* max_video_capabilities_;
  SbPlayerDeallocateSampleFunc sample_deallocate_func_;
  SbPlayerDecoderStatusFunc decoder_status_func_;
  SbPlayerStatusFunc player_status_func_;
  SbPlayerErrorFunc player_error_func_;
  void* context_{nullptr};
  SbPlayerOutputMode output_mode_;
  SbDecodeTargetGraphicsContextProvider* provider_{nullptr};
  GMainLoop* main_loop_{nullptr};
  GMainContext* main_loop_context_{nullptr};
  GstElement* source_{nullptr};
  GstElement* video_appsrc_{nullptr};
  GstElement* audio_appsrc_{nullptr};
  GstElement* pipeline_{nullptr};
  int source_setup_id_{-1};
  int bus_watch_id_{-1};
  SbThread playback_thread_;
  ::starboard::Mutex mutex_;
  ::starboard::Mutex source_setup_mutex_;
  double rate_{1.0};
  int ticket_{SB_PLAYER_INITIAL_TICKET};
  mutable SbTime seek_position_{kSbTimeMax};
  SbTime max_sample_timestamps_[kMediaNumber]{0};
  SbTime min_sample_timestamp_{kSbTimeMax};
  MediaType min_sample_timestamp_origin_{MediaType::kNone};
  bool is_seek_pending_{false};
  double pending_rate_{.0};
  bool is_rate_being_changed_{false};
  int has_enough_data_{static_cast<int>(MediaType::kBoth)};
  mutable int decoder_state_data_{static_cast<int>(MediaType::kNone)};
  int eos_data_{static_cast<int>(MediaType::kNone)};
  int total_video_frames_{0};
  int dropped_video_frames_{0};
  int frame_width_{0};
  int frame_height_{0};
  State state_{State::kNull};
  SamplesPendingKey pending_samples_;
  mutable gint64 cached_position_ns_{kSbTimeMax};
  mutable SbTime position_update_time_us_{0};
  PendingBounds pending_bounds_;
  SbMediaColorMetadata color_metadata_{};
};

PlayerImpl::PlayerImpl(SbPlayer player,
                       SbWindow window,
                       SbMediaVideoCodec video_codec,
                       SbMediaAudioCodec audio_codec,
                       SbDrmSystem drm_system,
                       const SbMediaAudioSampleInfo& audio_sample_info,
                       const char* max_video_capabilities,
                       SbPlayerDeallocateSampleFunc sample_deallocate_func,
                       SbPlayerDecoderStatusFunc decoder_status_func,
                       SbPlayerStatusFunc player_status_func,
                       SbPlayerErrorFunc player_error_func,
                       void* context,
                       SbPlayerOutputMode output_mode,
                       SbDecodeTargetGraphicsContextProvider* provider)
    : player_(player),
      window_(window),
      video_codec_(video_codec),
      audio_codec_(audio_codec),
      drm_system_(reinterpret_cast<DrmSystemOcdm*>(drm_system)),
      audio_sample_info_(audio_sample_info),
      max_video_capabilities_(max_video_capabilities),
      sample_deallocate_func_(sample_deallocate_func),
      decoder_status_func_(decoder_status_func),
      player_status_func_(player_status_func),
      player_error_func_(player_error_func),
      context_(context) {
  main_loop_context_ = g_main_context_new ();
  g_main_context_push_thread_default(main_loop_context_);
  main_loop_ = g_main_loop_new(main_loop_context_, FALSE);

  if (drm_system_)
    drm_system_->AddObserver(this);

  GST_DEBUG_CATEGORY_INIT(cobalt_gst_player_debug, "gstplayer", 0,
                          "Cobalt player");

  GST_INFO("Creating player with max capabilities: %s",
           max_video_capabilities_);

  GstElementFactory* src_factory = gst_element_factory_find("cobaltsrc");
  if (!src_factory) {
    gst_element_register(0, "cobaltsrc", GST_RANK_PRIMARY + 100,
                         GST_COBALT_TYPE_SRC);
  } else {
    gst_object_unref(src_factory);
  }

  pipeline_ = gst_element_factory_make("playbin", "media_pipeline");

  unsigned flagAudio = getGstPlayFlag("audio");
  unsigned flagVideo = getGstPlayFlag("video");
  unsigned flagNativeVideo = getGstPlayFlag("native-video");
  unsigned flagNativeAudio = 0;
#if SB_HAS(NATIVE_AUDIO)
  flagNativeAudio = getGstPlayFlag("native-audio");
#endif
  g_object_set(pipeline_, "flags",
               flagAudio | flagVideo | flagNativeVideo | flagNativeAudio,
               nullptr);
  g_signal_connect(pipeline_, "source-setup",
                   G_CALLBACK(&PlayerImpl::SetupSource), this);
  g_object_set(pipeline_, "uri", "cobalt://", nullptr);

  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  bus_watch_id_ = gst_bus_add_watch(bus, &PlayerImpl::BusMessageCallback, this);
  gst_object_unref(bus);

  video_appsrc_ = gst_element_factory_make("appsrc", "vidsrc");
  audio_appsrc_ = gst_element_factory_make("appsrc", "audsrc");

  GstElement* playsink = (gst_bin_get_by_name(GST_BIN(pipeline_), "playsink"));
  if (playsink) {
    g_object_set(G_OBJECT(playsink), "send-event-mode", 0, nullptr);
    g_object_unref(playsink);
  } else {
    GST_WARNING("No playsink ?!?!?");
  }
  ChangePipelineState(GST_STATE_READY);
  g_main_context_pop_thread_default(main_loop_context_);

  playback_thread_ =
      SbThreadCreate(0, kSbThreadPriorityRealTime, kSbThreadNoAffinity, true,
                     "playback_thread", &PlayerImpl::ThreadEntryPoint, this);
  SB_DCHECK(SbThreadIsValid(playback_thread_));
  if (SbThreadIsValid(playback_thread_)) {
    while(!g_main_loop_is_running(main_loop_))
      g_usleep(1);
  }
}

PlayerImpl::~PlayerImpl() {
  GST_DEBUG_OBJECT(pipeline_, "Destroying player");
  {
    ::starboard::ScopedLock lock(source_setup_mutex_);
    if (source_setup_id_ > -1) {
      GSource* src = g_main_context_find_source_by_id(main_loop_context_, source_setup_id_);
      g_source_destroy(src);
    }
  }
  if (bus_watch_id_ > -1) {
    GSource* src = g_main_context_find_source_by_id(main_loop_context_, bus_watch_id_);
    g_source_destroy(src);
  }
  ChangePipelineState(GST_STATE_NULL);
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
  gst_object_unref(bus);
  DispatchOnWorkerThread(new PlayerDestroyedTask(
      player_status_func_, player_, ticket_, context_, main_loop_));
  SbThreadJoin(playback_thread_, nullptr);
  g_main_loop_unref(main_loop_);
  g_main_context_unref(main_loop_context_);
  g_object_unref(pipeline_);
  if (drm_system_)
    drm_system_->RemoveObserver(this);
  GST_DEBUG("BYE BYE player");
}

// static
gboolean PlayerImpl::BusMessageCallback(GstBus* bus,
                                        GstMessage* message,
                                        gpointer user_data) {
  SB_UNREFERENCED_PARAMETER(bus);

  PlayerImpl* self = static_cast<PlayerImpl*>(user_data);
  GST_TRACE("%d", SbThreadGetId());

  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline_)) {
        GST_INFO("EOS");
        self->DispatchOnWorkerThread(new PlayerStatusTask(
            self->player_status_func_, self->player_, self->ticket_,
            self->context_, kSbPlayerStateEndOfStream));
      }
      break;

    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &err, &debug);

      bool is_eos = (self->eos_data_ == (int)self->GetBothMediaTypeTakingCodecsIntoAccount());
      if (err->domain == GST_STREAM_ERROR && is_eos) {
        GST_WARNING("Got stream error. But all streams are ended, so reporting EOS. Error code %d: %s (%s).",
          err->code, err->message, debug);
        self->DispatchOnWorkerThread(new PlayerStatusTask(
          self->player_status_func_, self->player_, self->ticket_,
          self->context_, kSbPlayerStateEndOfStream));
      } else {
        GST_ERROR("Error %d: %s (%s)", err->code, err->message, debug);
        self->DispatchOnWorkerThread(new PlayerErrorTask(
          self->player_error_func_, self->player_, self->context_,
          kSbPlayerErrorDecode, err->message));
      }
      g_free(debug);
      g_error_free(err);
      break;
    }

    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline_)) {
        GstState old_state, new_state, pending;
        gst_message_parse_state_changed(message, &old_state, &new_state,
                                        &pending);
        GST_INFO_OBJECT(GST_MESSAGE_SRC(message),
                        "===> State changed (old: %s, new: %s, pending: %s)",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state),
                        gst_element_state_get_name(pending));
        std::string file_name = "cobalt_";
        file_name += (GST_OBJECT_NAME(self->pipeline_));
        file_name += "_";
        file_name += gst_element_state_get_name(old_state);
        file_name += "_";
        file_name += gst_element_state_get_name(new_state);
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(self->pipeline_),
                                          GST_DEBUG_GRAPH_SHOW_ALL,
                                          file_name.c_str());

        if (GST_STATE(self->pipeline_) >= GST_STATE_PAUSED) {
          int ticket = 0;
          bool is_seek_pending = false;
          bool is_rate_pending = false;
          double rate = 0.;
          SbTime pending_seek_pos = kSbTimeMax;

          {
            ::starboard::ScopedLock lock(self->mutex_);
            ticket = self->ticket_;
            is_seek_pending = self->is_seek_pending_;
            is_rate_pending = self->pending_rate_ != .0;
            pending_seek_pos = self->seek_position_;
            SB_DCHECK(!is_seek_pending || self->seek_position_ != kSbTimeMax);
            rate = self->pending_rate_;
            if (is_seek_pending && is_rate_pending) {
              is_rate_pending = false;
              self->rate_ = rate;
              self->pending_rate_ = .0;
            }
          }

          if (self->video_codec_ != kSbMediaVideoCodecNone && !self->pending_bounds_.IsEmpty()) {
            PendingBounds bounds = self->pending_bounds_;
            self->pending_bounds_ = {};
            self->SetBounds(0, bounds.x, bounds.y, bounds.w, bounds.h);
          }

          if (is_rate_pending) {
            GST_INFO("Sending pending SetRate(rate=%lf)", rate);
            self->SetRate(rate);
          } else if (is_seek_pending) {
            GST_INFO("Sending pending Seek(position=%" PRId64 ", ticket=%d)", pending_seek_pos, ticket);
            self->Seek(pending_seek_pos, ticket);
          }
        }
      }
    } break;

    case GST_MESSAGE_ASYNC_DONE: {
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline_)) {
        GST_INFO("===> ASYNC-DONE %s %d",
                 gst_element_state_get_name(GST_STATE(self->pipeline_)),
                 static_cast<int>(self->state_));
        {
          ::starboard::ScopedLock lock(self->mutex_);
          self->is_rate_being_changed_ = false;
        }
        if (self->state_ == State::kPrerollAfterSeek ||
            self->state_ == State::kInitialPreroll) {
          bool is_seek_pending = false;
          bool is_rate_pending = false;
          {
            ::starboard::ScopedLock lock(self->mutex_);
            is_seek_pending = self->is_seek_pending_;
            is_rate_pending = self->pending_rate_ != 0.;
          }
          if (!is_seek_pending && !is_rate_pending) {
            int prev_has_data = static_cast<int>(MediaType::kNone);
            {
              ::starboard::ScopedLock lock(self->mutex_);
              prev_has_data = static_cast<int>(self->has_enough_data_);
              self->has_enough_data_ = static_cast<int>(MediaType::kBoth);
            }
            GST_INFO("===> Writing pending samples");
            self->OnKeyReady(reinterpret_cast<const uint8_t*>(kClearSamplesKey),
                             strlen(kClearSamplesKey));
            if (self->drm_system_) {
              auto ready_keys = self->drm_system_->GetReadyKeys();
              for (auto& key : ready_keys) {
                self->OnKeyReady(reinterpret_cast<const uint8_t*>(key.c_str()),
                                 key.size());
              }
            }
            {
              ::starboard::ScopedLock lock(self->mutex_);
              if (self->has_enough_data_ == static_cast<int>(MediaType::kBoth))
                self->has_enough_data_ = prev_has_data;
            }
          }
          GST_INFO("===> Asuming preroll done");
          {
            ::starboard::ScopedLock lock(self->mutex_);
            // The below code is good but on BRCM the decoder reports old
            // position for some time which makes some YTLB 2020 test failing.
            // self->seek_position_ = kSbTimeMax;
            self->DispatchOnWorkerThread(new PlayerStatusTask(
                self->player_status_func_, self->player_, self->ticket_,
                self->context_, kSbPlayerStatePresenting));
            self->state_ = State::kPresenting;
          }
        }
      }
    } break;

    case GST_MESSAGE_CLOCK_LOST:
      self->ChangePipelineState(GST_STATE_PAUSED);
      self->ChangePipelineState(GST_STATE_PLAYING);
      break;

    case GST_MESSAGE_LATENCY:
      gst_bin_recalculate_latency(GST_BIN(self->pipeline_));
      break;

    case GST_MESSAGE_QOS: {
      const gchar *klass;
      klass = gst_element_class_get_metadata (
          GST_ELEMENT_GET_CLASS (GST_MESSAGE_SRC(message)),
          GST_ELEMENT_METADATA_KLASS);
      if (g_strrstr(klass, "Video")) {
        GstFormat format;
        guint64 dropped = 0;
        gst_message_parse_qos_stats(message, &format, nullptr, &dropped);
        if (format == GST_FORMAT_BUFFERS) {
          ::starboard::ScopedLock lock(self->mutex_);
          self->dropped_video_frames_ = static_cast<int>(dropped);
        }
      }
    } break;

    default:
      GST_LOG("Got GST message %s from %s", GST_MESSAGE_TYPE_NAME(message),
              GST_MESSAGE_SRC_NAME(message));
      break;
  }

  return TRUE;
}

// static
void* PlayerImpl::ThreadEntryPoint(void* context) {
  SB_DCHECK(context);
  GST_TRACE("%d", SbThreadGetId());

  PlayerImpl* self = reinterpret_cast<PlayerImpl*>(context);
  self->state_ = State::kInitial;

  g_main_context_push_thread_default(self->main_loop_context_);

  self->DispatchOnWorkerThread(new PlayerStatusTask(
      self->player_status_func_, self->player_, self->ticket_, self->context_,
      kSbPlayerStateInitialized));
  g_main_loop_run(self->main_loop_);

  return nullptr;
}

void PlayerImpl::DispatchOnWorkerThread(Task* task) const {
  GSource* src = g_source_new(&SourceFunctions, sizeof(GSource));
  g_source_set_ready_time(src, 0);
  DispatchData* data = new DispatchData(task, src);
  g_source_set_callback(src,
                        [](gpointer userData) -> gboolean {
                          DispatchData* data =
                              static_cast<DispatchData*>(userData);
                          GST_TRACE("%d", SbThreadGetId());
                          data->task()->PrintInfo();
                          data->task()->Do();
                          delete data;
                          return G_SOURCE_REMOVE;
                        },
                        data, nullptr);
  g_source_attach(src, main_loop_context_);
}

// static
gboolean PlayerImpl::FinishSourceSetup(gpointer user_data) {
  PlayerImpl* self = static_cast<PlayerImpl*>(user_data);
  ::starboard::ScopedLock lock(self->source_setup_mutex_);
  SB_DCHECK(self->source_);
  GstElement* source = self->source_;
  GstAppSrcCallbacks callbacks = {&PlayerImpl::AppSrcNeedData,
                                  &PlayerImpl::AppSrcEnoughData,
                                  &PlayerImpl::AppSrcSeekData, nullptr};
  auto caps = CodecToGstCaps(self->audio_codec_, &self->audio_sample_info_);
  if (self->audio_codec_ != kSbMediaAudioCodecNone) {
    gst_cobalt_src_setup_and_add_app_src(
        source, self->audio_appsrc_, !caps.empty() ? caps[0].c_str() : nullptr,
        &callbacks, self);
  }
  if (self->video_codec_ != kSbMediaVideoCodecNone) {
    gst_cobalt_src_setup_and_add_app_src(
        source, self->video_appsrc_, nullptr,
        &callbacks, self);
  }
  gst_cobalt_src_all_app_srcs_added(self->source_);
  self->source_setup_id_ = -1;

  return FALSE;
}

// static
void PlayerImpl::AppSrcNeedData(GstAppSrc* src,
                                guint length,
                                gpointer user_data) {
  PlayerImpl* self = static_cast<PlayerImpl*>(user_data);

  GST_LOG_OBJECT(src, "===> Gimme more data");

  ::starboard::ScopedLock lock(self->mutex_);
  int need_data = static_cast<int>(MediaType::kNone);
  SB_DCHECK(src == GST_APP_SRC(self->video_appsrc_) ||
         src == GST_APP_SRC(self->audio_appsrc_));
  if (src == GST_APP_SRC(self->video_appsrc_)) {
    self->has_enough_data_ &= ~static_cast<int>(MediaType::kVideo);
    need_data |= static_cast<int>(MediaType::kVideo);
  } else if (src == GST_APP_SRC(self->audio_appsrc_)) {
    self->has_enough_data_ &= ~static_cast<int>(MediaType::kAudio);
    need_data |= static_cast<int>(MediaType::kAudio);
  }

  if (self->state_ == State::kPrerollAfterSeek) {
    if (self->has_enough_data_ != static_cast<int>(MediaType::kNone)) {
      GST_LOG_OBJECT(src, "Seeking. Waiting for other appsrcs.");
      return;
    }

    need_data =
        static_cast<int>(self->GetBothMediaTypeTakingCodecsIntoAccount());
  }

  GST_LOG_OBJECT(src, "===> Really. Gimme more data need:%d", need_data);
  self->DecoderNeedsData(lock, static_cast<MediaType>(need_data));
}

// static
void PlayerImpl::AppSrcEnoughData(GstAppSrc* src, gpointer user_data) {
  PlayerImpl* self = static_cast<PlayerImpl*>(user_data);

  ::starboard::ScopedLock lock(self->mutex_);

  if (src == GST_APP_SRC(self->video_appsrc_))
    self->has_enough_data_ |= static_cast<int>(MediaType::kVideo);
  else if (src == GST_APP_SRC(self->audio_appsrc_))
    self->has_enough_data_ |= static_cast<int>(MediaType::kAudio);

  GST_DEBUG_OBJECT(src, "===> Enough is enough (enough:%d)",
                   self->has_enough_data_);
}

// static
gboolean PlayerImpl::AppSrcSeekData(GstAppSrc* src,
                                    guint64 offset,
                                    gpointer user_data) {
  PlayerImpl* self = static_cast<PlayerImpl*>(user_data);
  GST_DEBUG_OBJECT(src, "===> Seek on appsrc %" PRId64, offset);

  {
    ::starboard::ScopedLock lock(self->mutex_);
    if (self->state_ != State::kPrerollAfterSeek) {
      GST_DEBUG_OBJECT(src, "Not seeking");
      return TRUE;
    }
  }

  PlayerImpl::AppSrcEnoughData(src, user_data);
  return TRUE;
}

// static
void PlayerImpl::SetupSource(GstElement* pipeline,
                             GstElement* source,
                             PlayerImpl* self) {
  ::starboard::ScopedLock lock(self->source_setup_mutex_);
  SB_DCHECK(!self->source_);
  self->source_ = source;
  static constexpr int kAsyncSourceFinishTimeMs = 50;
  GSource* src = g_timeout_source_new(kAsyncSourceFinishTimeMs);
  g_source_set_callback(src, &PlayerImpl::FinishSourceSetup, self, nullptr);
  self->source_setup_id_ = g_source_attach(src, self->main_loop_context_);
  g_source_unref(src);
}

void PlayerImpl::MarkEOS(SbMediaType stream_type) {
  GstElement* src = nullptr;
  if (stream_type == kSbMediaTypeVideo) {
    src = video_appsrc_;
  } else {
    src = audio_appsrc_;
  }

  GST_DEBUG_OBJECT(src, "===> %d", SbThreadGetId());
  ::starboard::ScopedLock lock(mutex_);

  // Flushing seek in progress so new data will be needed anyway.
  if (state_ == State::kPrerollAfterSeek) {
    GST_DEBUG_OBJECT(src, "===> Ignoring due to seek");
    return;
  }

  if (stream_type == kSbMediaTypeVideo)
      eos_data_ |= static_cast<int>(MediaType::kVideo);
  else
      eos_data_ |= static_cast<int>(MediaType::kAudio);

  gst_app_src_end_of_stream(GST_APP_SRC(src));
  RecordTimestamp(stream_type, kSbTimeMax);
}

bool PlayerImpl::WriteSample(SbMediaType sample_type,
                             GstBuffer* buffer,
                             const std::string& session_id,
                             GstBuffer* subsample,
                             int32_t subsample_count,
                             GstBuffer* iv,
                             GstBuffer* key) {
  GstElement* src = nullptr;
  if (sample_type == kSbMediaTypeVideo) {
    src = video_appsrc_;
  } else {
    src = audio_appsrc_;
  }

  {
    ::starboard::ScopedLock lock(mutex_);
    if (sample_type == kSbMediaTypeVideo)
      decoder_state_data_ &= ~static_cast<int>(MediaType::kVideo);
    else
      decoder_state_data_ &= ~static_cast<int>(MediaType::kAudio);
  }

  GST_TRACE_OBJECT(src,
                   "SampleType:%d %" GST_TIME_FORMAT " b:%p, s:%p, iv:%p, k:%p",
                   sample_type, GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)),
                   buffer, subsample, iv, key);

  bool decrypted = true;
  if (!session_id.empty()) {
    GST_LOG_OBJECT(src, "Decrypting using %s...", session_id.c_str());
    SB_DCHECK(drm_system_ && subsample && subsample_count && iv && key);
    decrypted = drm_system_->Decrypt(session_id, buffer, subsample,
                                     subsample_count, iv, key);
    if (!decrypted)
      GST_ERROR_OBJECT(src, "Failed decrypting");
  }

  if (decrypted)
    gst_app_src_push_buffer(GST_APP_SRC(src), buffer);

  ::starboard::ScopedLock lock(mutex_);
  if (decrypted && sample_type == kSbMediaTypeVideo)
    ++total_video_frames_;
  // Wait for need-data to trigger instead.
  if (state_ == State::kInitial || state_ == State::kInitialPreroll)
    return decrypted;

  bool has_enough =
      (sample_type == kSbMediaTypeVideo &&
       (has_enough_data_ & static_cast<int>(MediaType::kVideo)) != 0) ||
      (sample_type == kSbMediaTypeAudio &&
       (has_enough_data_ & static_cast<int>(MediaType::kAudio)) != 0);
  if (!has_enough) {
    GST_LOG_OBJECT(src, "Asking for more");
    MediaType media = sample_type == kSbMediaTypeVideo
      ? MediaType::kVideo
      : MediaType::kAudio;
    DecoderNeedsData(lock, media);
  } else {
    GST_LOG_OBJECT(src, "Has enough data");
  }

  return decrypted;
}

void PlayerImpl::WriteSample(SbMediaType sample_type,
                             const SbPlayerSampleInfo* sample_infos,
                             int number_of_sample_infos) {
  static_assert(kMaxNumberOfSamplesPerWrite == 1,
                "Adjust impl. to handle more samples after changing samples"
                "count");
  SB_DCHECK(number_of_sample_infos == kMaxNumberOfSamplesPerWrite);
  GstClockTime timestamp = sample_infos[0].timestamp * kSbTimeNanosecondsPerMicrosecond;
  GstBuffer* buffer =
      gst_buffer_new_allocate(nullptr, sample_infos[0].buffer_size, nullptr);
  gst_buffer_fill(buffer, 0, sample_infos[0].buffer,
                  sample_infos[0].buffer_size);
  GST_BUFFER_TIMESTAMP(buffer) = timestamp;
  sample_deallocate_func_(player_, context_, sample_infos[0].buffer);

  GstBuffer* subsamples = nullptr;
  GstBuffer* iv = nullptr;
  GstBuffer* key = nullptr;
  int32_t subsamples_count = 0u;
  std::string session_id;

  if (sample_infos[0].type == kSbMediaTypeVideo) {
    const auto& info = sample_infos[0].video_sample_info;
    if (frame_width_ != info.frame_width ||
        frame_height_ != info.frame_height ||
        CompareColorMetadata(color_metadata_, info.color_metadata) != 0) {
      frame_width_ = info.frame_width;
      frame_height_ = info.frame_height;
      color_metadata_ = info.color_metadata;
      auto caps = CodecToGstCaps(video_codec_);
      if (!caps.empty()) {
        GstCaps* gst_caps = gst_caps_from_string(caps[0].c_str());
        AddVideoInfoToGstCaps(info, gst_caps);
        gst_app_src_set_caps(GST_APP_SRC(video_appsrc_), gst_caps);
        gst_caps_unref(gst_caps);
      }
    }
  }

  RecordTimestamp(sample_type, timestamp);

  if (MinTimestamp(nullptr) == GST_BUFFER_TIMESTAMP(buffer) &&
      GST_STATE(pipeline_) <= GST_STATE_PAUSED &&
      (GST_STATE_PENDING(pipeline_) == GST_STATE_VOID_PENDING ||
       GST_STATE_PENDING(pipeline_) == GST_STATE_PAUSED) &&
      rate_ > .0) {
    GST_TRACE("Moving to playing for %" GST_TIME_FORMAT,
              GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)));
    ChangePipelineState(GST_STATE_PLAYING);
  }

  std::string key_str;
  bool keep_samples = false;
  {
    ::starboard::ScopedLock lock(mutex_);
    keep_samples = is_seek_pending_ || pending_rate_ != .0;
  }
  if (sample_infos[0].drm_info) {
    GST_LOG("Encounterd encrypted %s sample",
            sample_type == kSbMediaTypeVideo ? "video" : "audio");
    SB_DCHECK(drm_system_);
    key = gst_buffer_new_allocate(
        nullptr, sample_infos[0].drm_info->identifier_size, nullptr);
    gst_buffer_fill(key, 0, sample_infos[0].drm_info->identifier,
                    sample_infos[0].drm_info->identifier_size);
    size_t iv_size = sample_infos[0].drm_info->initialization_vector_size;
    const int8_t kEmptyArray[kMaxIvSize / 2] = {0};
    if (iv_size == kMaxIvSize &&
        memcmp(sample_infos[0].drm_info->initialization_vector + kMaxIvSize / 2,
               kEmptyArray, kMaxIvSize / 2) == 0)
      iv_size /= 2;

    iv = gst_buffer_new_allocate(nullptr, iv_size, nullptr);
    gst_buffer_fill(iv, 0, sample_infos[0].drm_info->initialization_vector,
                    iv_size);
    subsamples_count = sample_infos[0].drm_info->subsample_count;
    auto subsamples_raw_size =
        subsamples_count * (sizeof(guint16) + sizeof(guint32));
    guint8* subsamples_raw =
        static_cast<guint8*>(g_malloc(subsamples_raw_size));
    GstByteWriter writer;
    gst_byte_writer_init_with_data(&writer, subsamples_raw, subsamples_raw_size,
                                   FALSE);
    for (int32_t i = 0; i < subsamples_count; ++i) {
      if (!gst_byte_writer_put_uint16_be(
              &writer,
              sample_infos[0].drm_info->subsample_mapping[i].clear_byte_count))
        GST_ERROR("Failed writing clear subsample info at %d", i);
      if (!gst_byte_writer_put_uint32_be(&writer,
                                         sample_infos[0]
                                             .drm_info->subsample_mapping[i]
                                             .encrypted_byte_count))
        GST_ERROR("Failed writing encrypted subsample info at %d", i);
    }
    subsamples = gst_buffer_new_wrapped(subsamples_raw, subsamples_raw_size);

    session_id = drm_system_->SessionIdByKeyId(
        sample_infos[0].drm_info->identifier,
        sample_infos[0].drm_info->identifier_size);
    if (session_id.empty() || keep_samples) {
      GST_INFO("No session/pending flushing operation. Storing sample");
      GST_INFO("SampleType:%d %" GST_TIME_FORMAT " b:%p, s:%p, iv:%p, k:%p",
               sample_type, GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)), buffer,
               subsamples, iv, key);
      PendingSample sample(sample_type, buffer, iv, subsamples,
                           subsamples_count, key);
      key_str = {
          reinterpret_cast<const char*>(sample_infos[0].drm_info->identifier),
          sample_infos[0].drm_info->identifier_size};
      ::starboard::ScopedLock lock(mutex_);
      pending_samples_[key_str].emplace_back(std::move(sample));
      if (session_id.empty())
        return;
    }
  } else {
    GST_TRACE("Encounterd clear sample");
    if (keep_samples) {
      GST_INFO("Pending flushing operation. Storing sample");
      GST_INFO("SampleType:%d %" GST_TIME_FORMAT " b:%p, s:%p, iv:%p, k:%p",
               sample_type, GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buffer)), buffer,
               subsamples, iv, key);
      ::starboard::ScopedLock lock(mutex_);
      PendingSample sample(sample_type, buffer, nullptr, nullptr, 0, nullptr);
      key_str = {kClearSamplesKey};
      pending_samples_[key_str].emplace_back(std::move(sample));
    }
  }

  if (keep_samples) {
    PendingSamples local_samples;
    {
      ::starboard::ScopedLock lock(mutex_);
      local_samples.swap(pending_samples_[key_str]);
    }

    auto& sample = local_samples.back();
    if (WriteSample(sample.Type(), sample.Buffer(), session_id,
                    sample.Subsamples(), sample.SubsamplesCount(), sample.Iv(),
                    sample.Key())) {
      sample.Written();
    }

    {
      ::starboard::ScopedLock lock(mutex_);
      std::move(local_samples.begin(), local_samples.end(),
                std::back_inserter(pending_samples_[key_str]));
    }
  } else {
    WriteSample(sample_type, buffer, session_id, subsamples, subsamples_count,
                iv, key);
  }

  if (!session_id.empty() && !keep_samples) {
    GST_TRACE("Wrote sample. Cleaning up.");
    gst_buffer_unref(iv);
    gst_buffer_unref(key);
    gst_buffer_unref(subsamples);
  }
}

void PlayerImpl::SetVolume(double volume) {
  GST_DEBUG_OBJECT(pipeline_, "volume %lf, TID %d", volume, SbThreadGetId());
  gst_stream_volume_set_volume(GST_STREAM_VOLUME(pipeline_),
                               GST_STREAM_VOLUME_FORMAT_LINEAR, volume);
}

void PlayerImpl::Seek(SbTime seek_to_timestamp, int ticket) {
  gint64 current_pos_ns = GetPosition();
  GST_INFO_OBJECT(pipeline_, "===> time %" PRId64 " (target=%" GST_TIME_FORMAT ", curr=%" GST_TIME_FORMAT ") TID: %d state %d",
                   seek_to_timestamp,
                   GST_TIME_ARGS(seek_to_timestamp * kSbTimeNanosecondsPerMicrosecond),
                   GST_TIME_ARGS(current_pos_ns),
                   SbThreadGetId(),
                   static_cast<int>(state_));
  double rate = 1.;
  {
    ::starboard::ScopedLock lock(mutex_);

    if (ticket_ != ticket) {
      pending_samples_.clear();
      max_sample_timestamps_[kVideoIndex] = 0;
      max_sample_timestamps_[kAudioIndex] = 0;
      min_sample_timestamp_ = kSbTimeMax;
    }

    ticket_ = ticket;
    seek_position_ = seek_to_timestamp;
    decoder_state_data_ = 0;
    eos_data_ = 0;

    if (state_ == State::kInitial) {
      SB_DCHECK(seek_position_ == .0);
      // This is the initial seek to 0 which will trigger data pumping.
      state_ = State::kInitialPreroll;
      DispatchOnWorkerThread(new PlayerStatusTask(player_status_func_, player_,
                                                  ticket_, context_,
                                                  kSbPlayerStatePrerolling));
      seek_position_ = kSbTimeMax;
      if (GST_STATE(pipeline_) < GST_STATE_PAUSED &&
          GST_STATE_PENDING(pipeline_) < GST_STATE_PAUSED) {
        mutex_.Release();
        ChangePipelineState(GST_STATE_PAUSED);
        mutex_.Acquire();
      }
      return;
    }

    if (GST_STATE(pipeline_) < GST_STATE_PAUSED) {
      GST_INFO("Delaying seek.");
      if (state_ == State::kInitialPreroll) {
        if ((has_enough_data_ & static_cast<int>(MediaType::kVideo)) == 0) {
          DecoderNeedsData(lock, MediaType::kVideo);
        }

        if ((has_enough_data_ & static_cast<int>(MediaType::kAudio)) == 0) {
          DecoderNeedsData(lock, MediaType::kAudio);
        }
      }
      is_seek_pending_ = true;
      return;
    }

    is_seek_pending_ = false;
    rate = rate_;
    state_ = State::kPrerollAfterSeek;
  }

  GST_DEBUG("Calling seek");
  DispatchOnWorkerThread(new PlayerStatusTask(player_status_func_, player_,
                                              ticket_, context_,
                                              kSbPlayerStatePrerolling));
  if (!gst_element_seek(pipeline_, !rate ? 1.0 : rate, GST_FORMAT_TIME,
                        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                  GST_SEEK_FLAG_ACCURATE),
                        GST_SEEK_TYPE_SET,
                        seek_to_timestamp * kSbTimeNanosecondsPerMicrosecond,
                        GST_SEEK_TYPE_NONE, 0)) {
    GST_ERROR_OBJECT(pipeline_, "Seek failed");
    ::starboard::ScopedLock lock(mutex_);
    DispatchOnWorkerThread(new PlayerStatusTask(player_status_func_, player_,
                                                ticket_, context_,
                                                kSbPlayerStatePresenting));
    state_ = State::kPresenting;
  } else {
    GST_DEBUG("Seek called with success");
  }
}

bool PlayerImpl::SetRate(double rate) {
  GST_DEBUG_OBJECT(pipeline_, "===> rate %lf (rate_ %lf), TID: %d", rate, rate_,
                   SbThreadGetId());
  bool success = true;
  double current_rate = 1.;
  {
    ::starboard::ScopedLock lock(mutex_);
    current_rate = rate_;
    decoder_state_data_ = 0;
    eos_data_ = 0;
  }
  if (rate == .0) {
    ChangePipelineState(GST_STATE_PAUSED);
  } else if (rate == 1. && (current_rate == 1. || current_rate == .0)) {
    ChangePipelineState(GST_STATE_PLAYING);
  } else {
    ChangePipelineState(GST_STATE_PLAYING);
    {
      ::starboard::ScopedLock lock(mutex_);
      if (is_seek_pending_) {
        GST_DEBUG("Rate will be set when doing seek");
        rate_ = rate;
        return true;
      }
    }
    if (GST_STATE(pipeline_) < GST_STATE_PAUSED) {
      GST_DEBUG_OBJECT(pipeline_, "===> Set rate postponed");
      ::starboard::ScopedLock lock(mutex_);
      pending_rate_ = rate;
      return true;
    } else {
      {
        ::starboard::ScopedLock lock(mutex_);
        is_rate_being_changed_ = true;
        pending_rate_ = .0;
      }

      GST_DEBUG("Calling seek (set rate)");
      success =
          gst_element_seek(pipeline_, rate, GST_FORMAT_TIME,
                           static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                     GST_SEEK_FLAG_ACCURATE),
                           GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE,
                           GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
      GST_DEBUG("Seek called (set rate)");
    }
  }

  if (success) {
    ::starboard::ScopedLock lock(mutex_);
    rate_ = rate;
  } else {
    GST_ERROR_OBJECT(pipeline_, "Set rate failed");
  }

  return success;
}

void PlayerImpl::GetInfo(SbPlayerInfo2* out_player_info) {
  gint64 duration = 0;
  if (gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &duration) &&
      GST_CLOCK_TIME_IS_VALID(duration)) {
    out_player_info->duration = duration;
  } else {
    out_player_info->duration = SB_PLAYER_NO_DURATION;
  }

  gint64 position = GetPosition();

  GST_TRACE("Position: %" GST_TIME_FORMAT " (Seek to: %" GST_TIME_FORMAT
            ") Duration: %" GST_TIME_FORMAT,
            GST_TIME_ARGS(position),
            GST_TIME_ARGS(seek_position_ * kSbTimeNanosecondsPerMicrosecond),
            GST_TIME_ARGS(duration));

  out_player_info->current_media_timestamp =
      GST_CLOCK_TIME_IS_VALID(position)
          ? position / kSbTimeNanosecondsPerMicrosecond
          : 0;

  out_player_info->frame_width = frame_width_;
  out_player_info->frame_height = frame_height_;
  out_player_info->is_paused = GST_STATE(pipeline_) != GST_STATE_PLAYING;
  out_player_info->volume = gst_stream_volume_get_volume(
      GST_STREAM_VOLUME(pipeline_), GST_STREAM_VOLUME_FORMAT_LINEAR);
  out_player_info->total_video_frames = total_video_frames_;
  out_player_info->corrupted_video_frames = 0;

  {
    ::starboard::ScopedLock lock(mutex_);
    out_player_info->dropped_video_frames = dropped_video_frames_;
  }

  GST_LOG("Frames dropped: %d, Frames corrupted: %d",
          out_player_info->dropped_video_frames,
          out_player_info->corrupted_video_frames);
  out_player_info->playback_rate = rate_;
}

void PlayerImpl::SetBounds(int zindex, int x, int y, int w, int h) {
  GST_TRACE("Set Bounds: %d %d %d %d %d", zindex, x, y, w, h);
  GstElement* vid_sink = nullptr;
  g_object_get(pipeline_, "video-sink", &vid_sink, nullptr);
  if (vid_sink && g_object_class_find_property(G_OBJECT_GET_CLASS(vid_sink),
                                               "rectangle")) {
    gchar* rect = g_strdup_printf("%d,%d,%d,%d", x, y, w, h);
    g_object_set(vid_sink, "rectangle", rect, nullptr);
    free(rect);
  } else {
    pending_bounds_ = PendingBounds{x, y, w, h};
  }
  if (vid_sink)
    gst_object_unref(GST_OBJECT(vid_sink));
}

bool PlayerImpl::ChangePipelineState(GstState state) const {
  GST_DEBUG_OBJECT(pipeline_, "Changing state to %s",
                   gst_element_state_get_name(state));
  return gst_element_set_state(pipeline_, state) != GST_STATE_CHANGE_FAILURE;
}

gint64 PlayerImpl::GetPosition() const {
  auto last_update = position_update_time_us_;
  position_update_time_us_ = SbTimeGetMonotonicNow();
  double rate = 1.;

  gint64 seek_pos_ns = 0;
  {
    ::starboard::ScopedLock lock(mutex_);
    seek_pos_ns = seek_position_ * kSbTimeNanosecondsPerMicrosecond;
    rate = rate_;
  }
  gint64 position = seek_pos_ns;
  GstQuery* query = gst_query_new_position(GST_FORMAT_TIME);
  if (gst_element_query(pipeline_, query)) {
    gst_query_parse_position(query, 0, &position);
  } else {
    position = 0;
  }
  gst_query_unref(query);

  {
    ::starboard::ScopedLock lock(mutex_);
    if (seek_position_ != kSbTimeMax) {
      if (GST_STATE(pipeline_) != GST_STATE_PLAYING)
        return seek_pos_ns;

      if ((rate >= 0. && position <= seek_pos_ns) ||
          (rate < 0. && position >= seek_pos_ns)) {
        return seek_pos_ns;
      }

      cached_position_ns_ = seek_pos_ns;
      seek_position_ = kSbTimeMax;
    }
  }

  {
    ::starboard::ScopedLock lock(mutex_);
    if (is_rate_being_changed_ && audio_codec_ != kSbMediaAudioCodecNone) {
      GST_WARNING("Set rate workaround kicking in.");
      return max_sample_timestamps_[kAudioIndex];
    }
  }

  if (cached_position_ns_ != kSbTimeMax &&
      std::abs(position - cached_position_ns_) > GST_SECOND) {
    GST_WARNING("Unexpected position! More than 1 second jump detected: "
                "%" GST_TIME_FORMAT " --> %" GST_TIME_FORMAT "",
                GST_TIME_ARGS(cached_position_ns_),
                GST_TIME_ARGS(position));
  }

  MediaType origin = MediaType::kNone;
  constexpr SbTime kMarginNs =
      350 * kSbTimeMillisecond * kSbTimeNanosecondsPerMicrosecond;
  SbTime min_ts = MinTimestamp(&origin);
  if (min_ts != kSbTimeMax && min_ts + kMarginNs <= position &&
      GST_STATE(pipeline_) == GST_STATE_PLAYING &&
      GST_STATE_PENDING(pipeline_) != GST_STATE_PAUSED) {
    {
      ::starboard::ScopedLock lock(mutex_);
      DecoderNeedsData(lock, origin);
    }
    GST_WARNING("Force setting to PAUSED. Pos: %" GST_TIME_FORMAT
                " sample:%" GST_TIME_FORMAT,
                GST_TIME_ARGS(position), GST_TIME_ARGS(min_ts + kMarginNs));
    ChangePipelineState(GST_STATE_PAUSED);
  }

  cached_position_ns_ = position;
  return position;
}

void PlayerImpl::OnKeyReady(const uint8_t* key, size_t key_len) {
  std::string key_str(reinterpret_cast<const char*>(key), key_len);
  PendingSamples local_samples;
  bool keep_samples = false;
  int ticket = -1;
  {
    ::starboard::ScopedLock lock(mutex_);
    SamplesPendingKey::iterator iter;
    iter = pending_samples_.find(key_str);
    keep_samples = is_seek_pending_ || pending_rate_ != 0.;
    ticket = ticket_;
    if (iter != pending_samples_.end()) {
      local_samples.swap(iter->second);
    }
  }

  if (!local_samples.empty()) {
    std::string session_id;
    if (drm_system_) {
      session_id = drm_system_->SessionIdByKeyId(key, key_len);
      SB_DCHECK(!session_id.empty());
    }

    std::sort(local_samples.begin(), local_samples.end(),
              [](const PendingSample& lhs, const PendingSample& rhs) -> bool {
                return GST_BUFFER_TIMESTAMP(lhs.Buffer()) <
                       GST_BUFFER_TIMESTAMP(rhs.Buffer());
              });
    GstClockTime prev_timestamps[kMediaNumber] = {-1, -1};
    for (auto& sample : local_samples) {
      GST_INFO("Writing pending: SampleType:%d %" GST_TIME_FORMAT
               " b:%p, s:%p, iv:%p, k:%p",
               sample.Type(),
               GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(sample.Buffer())),
               sample.Buffer(), sample.Subsamples(), sample.Iv(), sample.Key());
      auto &prev_ts = prev_timestamps[sample.Type() == kSbMediaTypeVideo ? kVideoIndex : kAudioIndex];
      if (prev_ts == GST_BUFFER_TIMESTAMP(sample.Buffer())) {
        GST_WARNING("Skipping %" GST_TIME_FORMAT ". Already written.",
                    GST_TIME_ARGS(prev_ts));
        continue;
      }
      prev_ts = GST_BUFFER_TIMESTAMP(sample.Buffer());
      if (WriteSample(sample.Type(), sample.Buffer(), session_id,
                      sample.Subsamples(), sample.SubsamplesCount(),
                      sample.Iv(), sample.Key())) {
        GST_INFO("Pending sample was written.");
        sample.Written();
      }
    }

    if (keep_samples) {
      {
        ::starboard::ScopedLock lock(mutex_);
        if (ticket_ == ticket) {
          std::move(local_samples.begin(), local_samples.end(),
                    std::back_inserter(pending_samples_[key_str]));
        } else {
          keep_samples = false;
        }
      }
      if (keep_samples) {
        GST_INFO("Stored samples again.");
      } else {
        GST_INFO("Seek ticket changed (%d -> %d), dropped local samples.", ticket, ticket_);
      }
    }
  }
}

MediaType PlayerImpl::GetBothMediaTypeTakingCodecsIntoAccount() const {
  SB_DCHECK(audio_codec_ != kSbMediaAudioCodecNone ||
            video_codec_ != kSbMediaVideoCodecNone);
  MediaType both_need_data = MediaType::kBoth;

  if (audio_codec_ == kSbMediaAudioCodecNone)
    both_need_data = MediaType::kVideo;

  if (video_codec_ == kSbMediaVideoCodecNone)
    both_need_data = MediaType::kAudio;

  return both_need_data;
}

void PlayerImpl::RecordTimestamp(SbMediaType type, SbTime timestamp) {
  if (type == kSbMediaTypeVideo) {
    max_sample_timestamps_[kVideoIndex] =
        std::max(max_sample_timestamps_[kVideoIndex], timestamp);
  } else if (type == kSbMediaTypeAudio) {
    max_sample_timestamps_[kAudioIndex] =
        std::max(max_sample_timestamps_[kAudioIndex], timestamp);
  }

  if (audio_codec_ == kSbMediaAudioCodecNone) {
    min_sample_timestamp_origin_ = MediaType::kVideo;
    min_sample_timestamp_ = max_sample_timestamps_[kVideoIndex];
  } else if (video_codec_ == kSbMediaVideoCodecNone) {
    min_sample_timestamp_origin_ = MediaType::kAudio;
    min_sample_timestamp_ = max_sample_timestamps_[kAudioIndex];
  } else {
    min_sample_timestamp_ = std::min(max_sample_timestamps_[kVideoIndex],
                                     max_sample_timestamps_[kAudioIndex]);
    if (min_sample_timestamp_ == max_sample_timestamps_[kVideoIndex])
      min_sample_timestamp_origin_ = MediaType::kVideo;
    else
      min_sample_timestamp_origin_ = MediaType::kAudio;
  }
}

SbTime PlayerImpl::MinTimestamp(MediaType* origin) const {
  if (origin)
    *origin = min_sample_timestamp_origin_;
  return min_sample_timestamp_;
}

}  // namespace
}  // namespace player
}  // namespace shared
}  // namespace rdk
}  // namespace starboard
}  // namespace third_party

using third_party::starboard::rdk::shared::player::PlayerImpl;

SbPlayerPrivate::SbPlayerPrivate(
    SbWindow window,
    SbMediaVideoCodec video_codec,
    SbMediaAudioCodec audio_codec,
    SbDrmSystem drm_system,
    const SbMediaAudioSampleInfo& audio_sample_info,
    const char* max_video_capabilities,
    SbPlayerDeallocateSampleFunc sample_deallocate_func,
    SbPlayerDecoderStatusFunc decoder_status_func,
    SbPlayerStatusFunc player_status_func,
    SbPlayerErrorFunc player_error_func,
    void* context,
    SbPlayerOutputMode output_mode,
    SbDecodeTargetGraphicsContextProvider* provider)
    : player_(new PlayerImpl(this,
                             window,
                             video_codec,
                             audio_codec,
                             drm_system,
                             audio_sample_info,
                             max_video_capabilities,
                             sample_deallocate_func,
                             decoder_status_func,
                             player_status_func,
                             player_error_func,
                             context,
                             output_mode,
                             provider)) {
}
