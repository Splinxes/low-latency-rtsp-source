// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>
#include <util/platform.h>

#include "ui-status.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTING_URL "url"
#define SETTING_PRESET "preset"
#define SETTING_LATENCY_MS "latency_ms"
#define SETTING_MAX_BUFFERS "max_buffers"
#define SETTING_RECONNECT_MS "reconnect_ms"
#define SETTING_TCP_TIMESTAMP "tcp_timestamp"
#define SETTING_PREFER_HW_DECODE "prefer_hw_decode"
#define SETTING_ENABLE_AUDIO "enable_audio"
#define SETTING_AUDIO_MODE "audio_mode"
#define SETTING_AUDIO_DELAY_MS "audio_delay_ms"
#define SETTING_SHOW_ADVANCED "show_advanced"
#define SETTING_NO_SIGNAL_MODE "no_signal_mode"

#define PROP_INFO "plugin_info"
#define PROP_STATUS "status_info"
#define PROP_REFRESH_STATUS "refresh_status"
#define PROP_RESET_STATS "reset_stats"
#define PROP_RECONNECT_NOW "reconnect_now"
#define PROP_COPY_DIAGNOSTICS "copy_diagnostics"
#define PROP_UPDATE_PLUGIN "update_plugin"
#define PROP_VERSION "version_info"

#define LLRTSP_VERSION "0.5.1"

#define PRESET_LOW_LATENCY 0
#define PRESET_BALANCED 1
#define PRESET_STABLE 2
#define PRESET_CUSTOM 3

#define AUDIO_MODE_LOW_LATENCY 0
#define AUDIO_MODE_SYNCHRONIZED 1

#define NO_SIGNAL_TRANSPARENT 0
#define NO_SIGNAL_BLACK 1
#define NO_SIGNAL_RECONNECTING 2
#define NO_SIGNAL_NO_SIGNAL 3

#define AUDIO_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_BYTES_PER_SAMPLE 2

#define CODEC_NAME_MAX 48
#define DECODER_LABEL_MAX 80
#define DECODER_ELEMENT_MAX 64
#define DIAGNOSTIC_TEXT_MAX 160
#define VIDEO_STALL_TIMEOUT_MIN_NS 3000000000ULL
#define VIDEO_STALL_TIMEOUT_MAX_NS 30000000000ULL
#define VIDEO_START_TIMEOUT_NS 10000000000ULL
#define RECONNECT_BACKOFF_MAX_MS 10000

enum llrtsp_connection_state {
    LLRTSP_STATE_STOPPED = 0,
    LLRTSP_STATE_WAITING,
    LLRTSP_STATE_CONNECTING,
    LLRTSP_STATE_CONNECTED,
    LLRTSP_STATE_RECONNECTING,
    LLRTSP_STATE_ERROR,
};

enum llrtsp_diagnostic_severity {
    LLRTSP_DIAG_NONE = 0,
    LLRTSP_DIAG_INFO,
    LLRTSP_DIAG_WARNING,
    LLRTSP_DIAG_ERROR,
};

struct llrtsp_source {
    obs_source_t *source;
    GThread *worker;
    GMutex settings_mutex;
    GMutex stats_mutex;

    gchar *url;
    gint preset;
    gint latency_ms;
    gint max_buffers;
    gint reconnect_ms;
    gboolean tcp_timestamp;
    gboolean prefer_hw_decode;
    gboolean enable_audio;
    gint audio_mode;
    gint audio_delay_ms;
    gboolean show_advanced;
    gint no_signal_mode;

    /* Once a Protect secure URL is detected, keep the guidance visible
     * throughout partial edits until the conversion is fully complete. */
    gboolean unifi_hint_latched;

    gint stop_requested;
    gint restart_requested;

    enum llrtsp_connection_state state;
    guint64 frames_delivered;
    guint64 dropped_total;
    guint64 dropped_current;
    guint64 reconnect_count;
    guint width;
    guint height;
    gint fps_n;
    gint fps_d;
    guint last_good_width;
    guint last_good_height;
    guint64 stats_started_ns;

    gchar video_codec[CODEC_NAME_MAX];
    gchar audio_codec[CODEC_NAME_MAX];
    gchar decoder_label[DECODER_LABEL_MAX];
    gchar decoder_element[DECODER_ELEMENT_MAX];
    gboolean decoder_seen;
    gboolean decoder_is_hardware;
    gboolean software_fallback_active;
    guint64 audio_blocks_delivered;

    gchar diagnostic[DIAGNOSTIC_TEXT_MAX];
    enum llrtsp_diagnostic_severity diagnostic_severity;
    gboolean diagnostic_clear_on_connect;
    gboolean audio_track_seen;
    guint64 connection_started_ns;
    guint64 pipeline_started_ns;
    guint64 last_video_frame_ns;
    guint64 reconnect_due_ns;
    guint reconnect_backoff_attempt;

    double status_refresh_elapsed_s;

    gboolean have_video_pts_reference;
    GstClockTime last_video_pts;
    guint64 last_video_local_ns;
};

struct llrtsp_runtime_options {
    gchar *url;
    gint latency_ms;
    gint max_buffers;
    gint reconnect_ms;
    gboolean tcp_timestamp;
    gboolean prefer_hw_decode;
    gboolean enable_audio;
    gint audio_mode;
    gint audio_delay_ms;
};

struct llrtsp_pipeline_context {
    struct llrtsp_source *ctx;
    GstElement *pipeline;
    GstElement *video_decode;
    GstElement *audio_decode;
    gboolean audio_enabled;
    gboolean prefer_hw_decode;
    gboolean force_software_decode;
    gint video_claimed;
    gint audio_claimed;
};

static const char *llrtsp_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("SourceName");
}

static void stats_set_state(struct llrtsp_source *ctx,
                            enum llrtsp_connection_state state)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->state = state;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_set_diagnostic(struct llrtsp_source *ctx,
                                 enum llrtsp_diagnostic_severity severity,
                                 gboolean clear_on_connect,
                                 const char *text)
{
    g_mutex_lock(&ctx->stats_mutex);
    g_strlcpy(ctx->diagnostic, text ? text : "", sizeof(ctx->diagnostic));
    ctx->diagnostic_severity = text && *text ? severity : LLRTSP_DIAG_NONE;
    ctx->diagnostic_clear_on_connect = clear_on_connect;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_note_pipeline_started(struct llrtsp_source *ctx,
                                        guint64 now_ns)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->pipeline_started_ns = now_ns;
    ctx->last_video_frame_ns = 0;
    ctx->reconnect_due_ns = 0;
    ctx->reconnect_backoff_attempt = 0;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_schedule_reconnect(struct llrtsp_source *ctx,
                                     guint attempt, gint delay_ms)
{
    const guint64 now = os_gettime_ns();
    const guint64 delay_ns = (guint64)MAX(delay_ms, 0) * 1000000ULL;

    g_mutex_lock(&ctx->stats_mutex);
    ctx->state = LLRTSP_STATE_RECONNECTING;
    ctx->reconnect_due_ns = now + delay_ns;
    ctx->reconnect_backoff_attempt = attempt;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_clear_reconnect_schedule(struct llrtsp_source *ctx)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->reconnect_due_ns = 0;
    ctx->reconnect_backoff_attempt = 0;
    g_mutex_unlock(&ctx->stats_mutex);
}

static guint64 video_stall_timeout_ns(struct llrtsp_source *ctx)
{
    gint fps_n = 0;
    gint fps_d = 0;

    g_mutex_lock(&ctx->stats_mutex);
    fps_n = ctx->fps_n;
    fps_d = ctx->fps_d;
    g_mutex_unlock(&ctx->stats_mutex);

    guint64 timeout_ns = VIDEO_STALL_TIMEOUT_MIN_NS;
    if (fps_n > 0 && fps_d > 0) {
        const double frame_interval_ns =
            1000000000.0 * (double)fps_d / (double)fps_n;
        const double adaptive_ns = frame_interval_ns * 5.0;
        if (adaptive_ns > (double)timeout_ns)
            timeout_ns = (guint64)adaptive_ns;
    }

    return CLAMP(timeout_ns, VIDEO_STALL_TIMEOUT_MIN_NS,
                 VIDEO_STALL_TIMEOUT_MAX_NS);
}

static gboolean watchdog_should_reconnect(struct llrtsp_source *ctx,
                                          gboolean *stalled)
{
    guint64 pipeline_started_ns = 0;
    guint64 last_video_frame_ns = 0;
    enum llrtsp_connection_state state = LLRTSP_STATE_STOPPED;

    g_mutex_lock(&ctx->stats_mutex);
    pipeline_started_ns = ctx->pipeline_started_ns;
    last_video_frame_ns = ctx->last_video_frame_ns;
    state = ctx->state;
    g_mutex_unlock(&ctx->stats_mutex);

    const guint64 now = os_gettime_ns();

    if (state == LLRTSP_STATE_CONNECTED && last_video_frame_ns > 0) {
        const guint64 timeout_ns = video_stall_timeout_ns(ctx);
        if (now > last_video_frame_ns &&
            now - last_video_frame_ns >= timeout_ns) {
            if (stalled)
                *stalled = TRUE;
            return TRUE;
        }
    } else if (state == LLRTSP_STATE_CONNECTING &&
               pipeline_started_ns > 0 &&
               now > pipeline_started_ns &&
               now - pipeline_started_ns >= VIDEO_START_TIMEOUT_NS) {
        if (stalled)
            *stalled = FALSE;
        return TRUE;
    }

    return FALSE;
}

static gint reconnect_backoff_delay_ms(gint base_ms, guint attempt)
{
    const gint base = MAX(base_ms, 100);
    const gint cap = RECONNECT_BACKOFF_MAX_MS;

    if (attempt == 0)
        return MIN(base, cap);
    if (attempt == 1)
        return MIN(base * 2, cap);
    if (attempt == 2)
        return MIN(base * 5, cap);
    return cap;
}

static void stats_clear_transient_diagnostic_locked(struct llrtsp_source *ctx)
{
    if (!ctx->diagnostic_clear_on_connect)
        return;

    ctx->diagnostic[0] = '\0';
    ctx->diagnostic_severity = LLRTSP_DIAG_NONE;
    ctx->diagnostic_clear_on_connect = FALSE;
}

static void stats_note_audio_track(struct llrtsp_source *ctx)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->audio_track_seen = TRUE;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_clear_codec_diagnostic(struct llrtsp_source *ctx,
                                         gboolean audio)
{
    const char *prefix = audio ? "Unsupported audio codec:"
                               : "Unsupported video codec:";

    g_mutex_lock(&ctx->stats_mutex);
    if (g_str_has_prefix(ctx->diagnostic, prefix)) {
        ctx->diagnostic[0] = '\0';
        ctx->diagnostic_severity = LLRTSP_DIAG_NONE;
        ctx->diagnostic_clear_on_connect = FALSE;
    }
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_set_codec(struct llrtsp_source *ctx, gboolean audio,
                            const char *codec)
{
    g_mutex_lock(&ctx->stats_mutex);
    g_strlcpy(audio ? ctx->audio_codec : ctx->video_codec,
              codec ? codec : "", CODEC_NAME_MAX);
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_note_video_sample(struct llrtsp_source *ctx,
                                    GstAppSink *sink, guint width,
                                    guint height, gint fps_n, gint fps_d,
                                    GstClockTime pts, guint64 local_ns)
{
    guint64 dropped = 0;
    g_object_get(G_OBJECT(sink), "dropped", &dropped, NULL);

    g_mutex_lock(&ctx->stats_mutex);
    if (ctx->state != LLRTSP_STATE_CONNECTED) {
        ctx->state = LLRTSP_STATE_CONNECTED;
        ctx->connection_started_ns = local_ns;
        stats_clear_transient_diagnostic_locked(ctx);
    }
    ctx->last_video_frame_ns = local_ns;
    ctx->reconnect_due_ns = 0;
    ctx->reconnect_backoff_attempt = 0;
    if (ctx->stats_started_ns == 0)
        ctx->stats_started_ns = local_ns;
    ctx->frames_delivered++;
    ctx->dropped_current = dropped;
    ctx->width = width;
    ctx->height = height;
    ctx->fps_n = fps_n;
    ctx->fps_d = fps_d;
    ctx->last_good_width = width;
    ctx->last_good_height = height;
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        ctx->have_video_pts_reference = TRUE;
        ctx->last_video_pts = pts;
        ctx->last_video_local_ns = local_ns;
    }
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_note_audio_sample(struct llrtsp_source *ctx)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->audio_blocks_delivered++;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_update_dropped(struct llrtsp_source *ctx, guint64 dropped)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->dropped_current = dropped;
    g_mutex_unlock(&ctx->stats_mutex);
}

/* Simple 5x7 glyphs used for portable no-signal slates.  Keeping this tiny
 * avoids adding a font/UI dependency to the video path. */
static const uint8_t *slate_glyph(char c)
{
    static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 0x0c, 0x0c};
    static const uint8_t A[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t C[7] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
    static const uint8_t E[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    static const uint8_t G[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f};
    static const uint8_t I[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
    static const uint8_t L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t O[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t R[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const uint8_t S[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t T[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};

    switch (c) {
    case 'A': return A;
    case 'C': return C;
    case 'E': return E;
    case 'G': return G;
    case 'I': return I;
    case 'L': return L;
    case 'N': return N;
    case 'O': return O;
    case 'R': return R;
    case 'S': return S;
    case 'T': return T;
    case '.': return dot;
    case ' ':
    default: return space;
    }
}

static void draw_slate_text(uint8_t *pixels, guint width, guint height,
                            guint32 stride, const char *text)
{
    if (!pixels || !text || !*text || width == 0 || height == 0)
        return;

    const size_t len = strlen(text);
    if (len == 0)
        return;

    const guint glyph_units = (guint)(len * 6U - 1U);
    guint scale_w = glyph_units ? width / (glyph_units + 4U) : 1U;
    guint scale_h = height / 14U;
    guint scale = MIN(scale_w, scale_h);
    if (scale < 1U)
        scale = 1U;
    if (scale > 20U)
        scale = 20U;

    const guint total_w = glyph_units * scale;
    const guint total_h = 7U * scale;
    const guint start_x = width > total_w ? (width - total_w) / 2U : 0U;
    const guint start_y = height > total_h ? (height - total_h) / 2U : 0U;

    for (size_t i = 0; i < len; i++) {
        const uint8_t *glyph = slate_glyph(text[i]);
        const guint gx = start_x + (guint)i * 6U * scale;

        for (guint row = 0; row < 7U; row++) {
            for (guint col = 0; col < 5U; col++) {
                if ((glyph[row] & (uint8_t)(1U << (4U - col))) == 0)
                    continue;

                for (guint sy = 0; sy < scale; sy++) {
                    const guint y = start_y + row * scale + sy;
                    if (y >= height)
                        continue;

                    for (guint sx = 0; sx < scale; sx++) {
                        const guint x = gx + col * scale + sx;
                        if (x >= width)
                            continue;

                        uint8_t *px = pixels + (gsize)y * stride + (gsize)x * 4U;
                        px[0] = 235;
                        px[1] = 235;
                        px[2] = 235;
                        px[3] = 255;
                    }
                }
            }
        }
    }
}

/* Replace a stale live frame with the user's configured signal-loss display. */
static void clear_video_output(struct llrtsp_source *ctx)
{
    if (!ctx || !ctx->source)
        return;

    gint mode = NO_SIGNAL_TRANSPARENT;
    g_mutex_lock(&ctx->settings_mutex);
    mode = CLAMP(ctx->no_signal_mode, NO_SIGNAL_TRANSPARENT,
                 NO_SIGNAL_NO_SIGNAL);
    g_mutex_unlock(&ctx->settings_mutex);

    if (mode == NO_SIGNAL_TRANSPARENT) {
        obs_source_output_video(ctx->source, NULL);
        return;
    }

    guint width = 1280;
    guint height = 720;
    g_mutex_lock(&ctx->stats_mutex);
    if (ctx->last_good_width > 0 && ctx->last_good_height > 0) {
        width = ctx->last_good_width;
        height = ctx->last_good_height;
    }
    g_mutex_unlock(&ctx->stats_mutex);

    if (width == 0 || height == 0 || width > 8192U || height > 8192U) {
        obs_source_output_video(ctx->source, NULL);
        return;
    }

    const gsize stride_size = (gsize)width * 4U;
    if (stride_size > G_MAXUINT32 ||
        (gsize)height > G_MAXSIZE / stride_size) {
        obs_source_output_video(ctx->source, NULL);
        return;
    }

    const gsize bytes = stride_size * (gsize)height;
    uint8_t *pixels = g_malloc0(bytes);
    if (!pixels) {
        obs_source_output_video(ctx->source, NULL);
        return;
    }

    /* BGRA black with an opaque alpha channel. */
    for (gsize i = 3; i < bytes; i += 4)
        pixels[i] = 255;

    if (mode == NO_SIGNAL_RECONNECTING)
        draw_slate_text(pixels, width, height, (guint32)stride_size,
                        "RECONNECTING...");
    else if (mode == NO_SIGNAL_NO_SIGNAL)
        draw_slate_text(pixels, width, height, (guint32)stride_size,
                        "NO SIGNAL");

    struct obs_source_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.format = VIDEO_FORMAT_BGRA;
    frame.width = width;
    frame.height = height;
    frame.data[0] = pixels;
    frame.linesize[0] = (uint32_t)stride_size;
    frame.timestamp = os_gettime_ns();
    frame.full_range = true;

    obs_source_output_video(ctx->source, &frame);
    g_free(pixels);
}

static void stats_finalize_pipeline(struct llrtsp_source *ctx)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->dropped_total += ctx->dropped_current;
    ctx->dropped_current = 0;
    ctx->have_video_pts_reference = FALSE;
    ctx->last_video_pts = GST_CLOCK_TIME_NONE;
    ctx->last_video_local_ns = 0;
    ctx->width = 0;
    ctx->height = 0;
    ctx->fps_n = 0;
    ctx->fps_d = 0;
    ctx->video_codec[0] = '\0';
    ctx->audio_codec[0] = '\0';
    ctx->decoder_label[0] = '\0';
    ctx->decoder_element[0] = '\0';
    ctx->decoder_seen = FALSE;
    ctx->decoder_is_hardware = FALSE;
    ctx->audio_track_seen = FALSE;
    ctx->connection_started_ns = 0;
    ctx->pipeline_started_ns = 0;
    ctx->last_video_frame_ns = 0;
    ctx->reconnect_due_ns = 0;
    ctx->reconnect_backoff_attempt = 0;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_note_reconnect(struct llrtsp_source *ctx)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->reconnect_count++;
    ctx->state = LLRTSP_STATE_RECONNECTING;
    ctx->reconnect_due_ns = 0;
    ctx->have_video_pts_reference = FALSE;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_reset(struct llrtsp_source *ctx)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->frames_delivered = 0;
    ctx->audio_blocks_delivered = 0;
    ctx->dropped_total = 0;
    ctx->dropped_current = 0;
    ctx->reconnect_count = 0;
    if (ctx->state == LLRTSP_STATE_CONNECTED) {
        const guint64 now = os_gettime_ns();
        ctx->stats_started_ns = now;
    } else {
        ctx->stats_started_ns = 0;
    }
    g_mutex_unlock(&ctx->stats_mutex);
}

static const char *state_label(enum llrtsp_connection_state state)
{
    switch (state) {
    case LLRTSP_STATE_WAITING:
        return "Waiting for URL";
    case LLRTSP_STATE_CONNECTING:
        return "Connecting...";
    case LLRTSP_STATE_CONNECTED:
        return "Connected";
    case LLRTSP_STATE_RECONNECTING:
        return "Reconnecting...";
    case LLRTSP_STATE_ERROR:
        return "Error";
    case LLRTSP_STATE_STOPPED:
    default:
        return "Stopped";
    }
}

static void status_snapshot(struct llrtsp_source *ctx, char *buffer,
                            size_t buffer_size,
                            enum obs_text_info_type *info_type)
{
    enum llrtsp_connection_state state = LLRTSP_STATE_STOPPED;
    guint64 frames = 0;
    guint64 audio_blocks = 0;
    guint64 dropped = 0;
    guint64 reconnects = 0;
    guint width = 0;
    guint height = 0;
    gint fps_n = 0;
    gint fps_d = 0;
    guint64 stats_started_ns = 0;
    gchar video_codec[CODEC_NAME_MAX] = {0};
    gchar audio_codec[CODEC_NAME_MAX] = {0};
    gchar decoder_label[DECODER_LABEL_MAX] = {0};
    gchar decoder_element[DECODER_ELEMENT_MAX] = {0};
    gboolean decoder_seen = FALSE;
    gboolean software_fallback_active = FALSE;
    gchar diagnostic[DIAGNOSTIC_TEXT_MAX] = {0};
    enum llrtsp_diagnostic_severity diagnostic_severity = LLRTSP_DIAG_NONE;
    gboolean audio_track_seen = FALSE;
    guint64 connection_started_ns = 0;
    guint64 reconnect_due_ns = 0;
    guint reconnect_backoff_attempt = 0;
    gboolean audio_enabled = FALSE;
    gboolean show_advanced = FALSE;
    gboolean prefer_hw_decode = FALSE;

    if (ctx) {
        g_mutex_lock(&ctx->stats_mutex);
        state = ctx->state;
        frames = ctx->frames_delivered;
        audio_blocks = ctx->audio_blocks_delivered;
        dropped = ctx->dropped_total + ctx->dropped_current;
        reconnects = ctx->reconnect_count;
        width = ctx->width;
        height = ctx->height;
        fps_n = ctx->fps_n;
        fps_d = ctx->fps_d;
        stats_started_ns = ctx->stats_started_ns;
        g_strlcpy(video_codec, ctx->video_codec, sizeof(video_codec));
        g_strlcpy(audio_codec, ctx->audio_codec, sizeof(audio_codec));
        g_strlcpy(decoder_label, ctx->decoder_label, sizeof(decoder_label));
        g_strlcpy(decoder_element, ctx->decoder_element, sizeof(decoder_element));
        decoder_seen = ctx->decoder_seen;
        software_fallback_active = ctx->software_fallback_active;
        g_strlcpy(diagnostic, ctx->diagnostic, sizeof(diagnostic));
        diagnostic_severity = ctx->diagnostic_severity;
        audio_track_seen = ctx->audio_track_seen;
        connection_started_ns = ctx->connection_started_ns;
        reconnect_due_ns = ctx->reconnect_due_ns;
        reconnect_backoff_attempt = ctx->reconnect_backoff_attempt;
        g_mutex_unlock(&ctx->stats_mutex);

        g_mutex_lock(&ctx->settings_mutex);
        audio_enabled = ctx->enable_audio;
        show_advanced = ctx->show_advanced;
        prefer_hw_decode = ctx->prefer_hw_decode;
        g_mutex_unlock(&ctx->settings_mutex);
    }

    if (info_type) {
        if (state == LLRTSP_STATE_ERROR ||
            diagnostic_severity == LLRTSP_DIAG_ERROR)
            *info_type = OBS_TEXT_INFO_ERROR;
        else if (state == LLRTSP_STATE_CONNECTING ||
                 state == LLRTSP_STATE_RECONNECTING ||
                 diagnostic_severity == LLRTSP_DIAG_WARNING)
            *info_type = OBS_TEXT_INFO_WARNING;
        else
            *info_type = OBS_TEXT_INFO_NORMAL;
    }

    guint64 uptime_s = 0;
    if (stats_started_ns > 0) {
        const guint64 now = os_gettime_ns();
        if (now > stats_started_ns)
            uptime_s = (now - stats_started_ns) / 1000000000ULL;
    }

    const double fps = (fps_d > 0) ? ((double)fps_n / (double)fps_d) : 0.0;
    const char *vcodec = video_codec[0] ? video_codec : "Detecting";
    const char *acodec = audio_codec[0] ? audio_codec : "Waiting";

    char state_text[96];
    g_strlcpy(state_text, state_label(state), sizeof(state_text));
    if (state == LLRTSP_STATE_RECONNECTING && reconnect_due_ns > 0) {
        const guint64 now = os_gettime_ns();
        if (reconnect_due_ns > now) {
            const guint64 remaining_ns = reconnect_due_ns - now;
            const guint64 remaining_s =
                (remaining_ns + 999999999ULL) / 1000000000ULL;
            g_snprintf(state_text, (gulong)sizeof(state_text),
                       "Reconnecting in %" G_GUINT64_FORMAT "s...",
                       remaining_s);
        }
    }

    if (audio_enabled && !audio_codec[0] &&
        state == LLRTSP_STATE_CONNECTED && connection_started_ns > 0) {
        const guint64 now = os_gettime_ns();
        if (now > connection_started_ns &&
            (now - connection_started_ns) >= 3000000000ULL &&
            !audio_track_seen)
            acodec = "No track";
    }

    if (width && height && fps > 0.0) {
        g_snprintf(buffer, (gulong)buffer_size,
                   "%s  |  %s  |  %ux%u  |  %.2f FPS",
                   state_text, vcodec, width, height, fps);
    } else if (width && height) {
        g_snprintf(buffer, (gulong)buffer_size,
                   "%s  |  %s  |  %ux%u",
                   state_text, vcodec, width, height);
    } else {
        g_snprintf(buffer, (gulong)buffer_size,
                   "%s  |  Video: %s",
                   state_text, vcodec);
    }

    if (state == LLRTSP_STATE_CONNECTED && stats_started_ns > 0) {
        gchar extra[96];
        const guint64 hours = uptime_s / 3600ULL;
        const guint64 minutes = (uptime_s % 3600ULL) / 60ULL;
        const guint64 seconds = uptime_s % 60ULL;

        if (hours > 0) {
            g_snprintf(extra, (gulong)sizeof(extra),
                       "  |  Uptime: %" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT
                       ":%02" G_GUINT64_FORMAT,
                       hours, minutes, seconds);
        } else {
            g_snprintf(extra, (gulong)sizeof(extra),
                       "  |  Uptime: %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
                       minutes, seconds);
        }
        g_strlcat(buffer, extra, buffer_size);
    }

    gchar decoder_text[DECODER_LABEL_MAX + DECODER_ELEMENT_MAX + 48];
    if (decoder_seen) {
        if (show_advanced && decoder_element[0]) {
            g_snprintf(decoder_text, (gulong)sizeof(decoder_text),
                       "%s (%s)%s",
                       decoder_label, decoder_element,
                       software_fallback_active ? " [fallback]" : "");
        } else {
            g_snprintf(decoder_text, (gulong)sizeof(decoder_text),
                       "%s%s",
                       decoder_label,
                       software_fallback_active ? " [fallback]" : "");
        }
    } else if (software_fallback_active) {
        g_strlcpy(decoder_text, "Software fallback (detecting)",
                  sizeof(decoder_text));
    } else if (prefer_hw_decode) {
        g_strlcpy(decoder_text, "Detecting (hardware preferred)",
                  sizeof(decoder_text));
    } else {
        g_strlcpy(decoder_text, "Detecting (software forced)",
                  sizeof(decoder_text));
    }

    gchar line2[448];
    if (audio_enabled) {
        g_snprintf(line2, (gulong)sizeof(line2),
                   "\nDecoder: %s  |  Video: %" G_GUINT64_FORMAT
                   " frames  |  Dropped: %" G_GUINT64_FORMAT
                   "  |  Reconnects: %" G_GUINT64_FORMAT
                   "  |  Audio: %s  |  Audio Blocks: %" G_GUINT64_FORMAT,
                   decoder_text, frames, dropped, reconnects, acodec,
                   audio_blocks);
    } else {
        g_snprintf(line2, (gulong)sizeof(line2),
                   "\nDecoder: %s  |  Video: %" G_GUINT64_FORMAT
                   " frames  |  Dropped: %" G_GUINT64_FORMAT
                   "  |  Reconnects: %" G_GUINT64_FORMAT "  |  Audio: Off",
                   decoder_text, frames, dropped, reconnects);
    }
    g_strlcat(buffer, line2, buffer_size);

    if (diagnostic[0]) {
        gchar line3[DIAGNOSTIC_TEXT_MAX + 24];
        g_snprintf(line3, (gulong)sizeof(line3),
                   "\nStatus: %s", diagnostic);
        g_strlcat(buffer, line3, buffer_size);
    }

    if (show_advanced && state == LLRTSP_STATE_RECONNECTING &&
        reconnect_backoff_attempt > 0) {
        gchar line4[64];
        g_snprintf(line4, (gulong)sizeof(line4),
                   "\nReconnect attempt: %u", reconnect_backoff_attempt);
        g_strlcat(buffer, line4, buffer_size);
    }
}

static void update_status_property(obs_properties_t *props,
                                   struct llrtsp_source *ctx)
{
    obs_property_t *status = obs_properties_get(props, PROP_STATUS);
    if (!status)
        return;

    char text[768];
    enum obs_text_info_type type = OBS_TEXT_INFO_NORMAL;
    status_snapshot(ctx, text, sizeof(text), &type);
    obs_property_set_description(status, text);
    obs_property_text_set_info_type(status, type);
}

static gboolean is_custom_preset(gint preset)
{
    return preset == PRESET_CUSTOM;
}

static void update_property_visibility(obs_properties_t *props, gint preset,
                                       gboolean show_advanced,
                                       gboolean enable_audio)
{
    const gboolean custom = is_custom_preset(preset);
    obs_property_t *latency = obs_properties_get(props, SETTING_LATENCY_MS);
    obs_property_t *buffers = obs_properties_get(props, SETTING_MAX_BUFFERS);
    obs_property_t *reconnect = obs_properties_get(props, SETTING_RECONNECT_MS);
    obs_property_t *tcp = obs_properties_get(props, SETTING_TCP_TIMESTAMP);
    obs_property_t *audio_mode = obs_properties_get(props, SETTING_AUDIO_MODE);
    obs_property_t *audio_delay = obs_properties_get(props, SETTING_AUDIO_DELAY_MS);
    obs_property_t *version = obs_properties_get(props, PROP_VERSION);
    obs_property_t *copy_diagnostics =
        obs_properties_get(props, PROP_COPY_DIAGNOSTICS);
    obs_property_t *refresh_status =
        obs_properties_get(props, PROP_REFRESH_STATUS);
    obs_property_t *reset_stats =
        obs_properties_get(props, PROP_RESET_STATS);

    if (latency)
        obs_property_set_visible(latency, custom);
    if (buffers)
        obs_property_set_visible(buffers, custom);
    if (reconnect)
        obs_property_set_visible(reconnect, show_advanced);
    if (tcp)
        obs_property_set_visible(tcp, show_advanced);
    if (audio_mode)
        obs_property_set_visible(audio_mode, enable_audio);
    if (audio_delay)
        obs_property_set_visible(audio_delay, enable_audio);
    if (version)
        obs_property_set_visible(version, show_advanced);
    if (copy_diagnostics)
        obs_property_set_visible(copy_diagnostics, show_advanced);
    if (refresh_status)
        obs_property_set_visible(refresh_status, show_advanced);
    if (reset_stats)
        obs_property_set_visible(reset_stats, show_advanced);
}

static gboolean looks_like_unifi_protect_secure_url(const char *url)
{
    if (!url || !*url)
        return FALSE;

    const gboolean secure_scheme =
        g_ascii_strncasecmp(url, "rtsps://", 8) == 0;
    const gboolean secure_port =
        g_strrstr(url, ":7441/") != NULL ||
        g_strrstr(url, ":7441?") != NULL;
    const gboolean srtp_query =
        g_strrstr(url, "?enableSrtp") != NULL ||
        g_strrstr(url, "&enableSrtp") != NULL;

    return secure_scheme && (secure_port || srtp_query);
}

static gboolean unifi_protect_conversion_complete(const char *url)
{
    if (!url || !*url)
        return FALSE;

    const gboolean normal_scheme =
        g_ascii_strncasecmp(url, "rtsp://", 7) == 0;
    const gboolean normal_port =
        g_strrstr(url, ":7447/") != NULL ||
        g_strrstr(url, ":7447?") != NULL;
    const gboolean srtp_query =
        g_strrstr(url, "?enableSrtp") != NULL ||
        g_strrstr(url, "&enableSrtp") != NULL;

    return normal_scheme && normal_port && !srtp_query;
}

static bool properties_modified(void *priv, obs_properties_t *props,
                                obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(priv);
    UNUSED_PARAMETER(property);

    const gint preset = (gint)obs_data_get_int(settings, SETTING_PRESET);
    const gboolean advanced = obs_data_get_bool(settings, SETTING_SHOW_ADVANCED);
    const gboolean audio = obs_data_get_bool(settings, SETTING_ENABLE_AUDIO);
    update_property_visibility(props, preset, advanced, audio);
    return true;
}


static const char *diagnostics_preset_label(gint preset)
{
    switch (preset) {
    case PRESET_BALANCED:
        return "Balanced";
    case PRESET_STABLE:
        return "Stable";
    case PRESET_CUSTOM:
        return "Custom";
    case PRESET_LOW_LATENCY:
    default:
        return "Low Latency";
    }
}

static const char *diagnostics_no_signal_label(gint mode)
{
    switch (mode) {
    case NO_SIGNAL_BLACK:
        return "Black";
    case NO_SIGNAL_RECONNECTING:
        return "Reconnecting...";
    case NO_SIGNAL_NO_SIGNAL:
        return "No Signal";
    case NO_SIGNAL_TRANSPARENT:
    default:
        return "Transparent";
    }
}

static const char *diagnostics_audio_mode_label(gint mode)
{
    return mode == AUDIO_MODE_SYNCHRONIZED ? "Synchronized" : "Low Latency";
}

static void build_diagnostics_text(struct llrtsp_source *ctx,
                                   char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        return;

    buffer[0] = '\0';

    enum llrtsp_connection_state state = LLRTSP_STATE_STOPPED;
    guint64 frames = 0;
    guint64 audio_blocks = 0;
    guint64 dropped = 0;
    guint64 reconnects = 0;
    guint width = 0;
    guint height = 0;
    gint fps_n = 0;
    gint fps_d = 0;
    guint64 stats_started_ns = 0;
    gchar video_codec[CODEC_NAME_MAX] = {0};
    gchar audio_codec[CODEC_NAME_MAX] = {0};
    gchar decoder_label[DECODER_LABEL_MAX] = {0};
    gchar decoder_element[DECODER_ELEMENT_MAX] = {0};
    gchar diagnostic[DIAGNOSTIC_TEXT_MAX] = {0};
    guint reconnect_attempt = 0;

    gint preset = PRESET_LOW_LATENCY;
    gint latency_ms = 0;
    gint max_buffers = 1;
    gint reconnect_ms = 1000;
    gboolean tcp_timestamp = TRUE;
    gboolean prefer_hw_decode = TRUE;
    gboolean enable_audio = FALSE;
    gint audio_mode = AUDIO_MODE_LOW_LATENCY;
    gint audio_delay_ms = 0;
    gint no_signal_mode = NO_SIGNAL_TRANSPARENT;

    if (ctx) {
        g_mutex_lock(&ctx->stats_mutex);
        state = ctx->state;
        frames = ctx->frames_delivered;
        audio_blocks = ctx->audio_blocks_delivered;
        dropped = ctx->dropped_total + ctx->dropped_current;
        reconnects = ctx->reconnect_count;
        width = ctx->width;
        height = ctx->height;
        fps_n = ctx->fps_n;
        fps_d = ctx->fps_d;
        stats_started_ns = ctx->stats_started_ns;
        reconnect_attempt = ctx->reconnect_backoff_attempt;
        g_strlcpy(video_codec, ctx->video_codec, sizeof(video_codec));
        g_strlcpy(audio_codec, ctx->audio_codec, sizeof(audio_codec));
        g_strlcpy(decoder_label, ctx->decoder_label, sizeof(decoder_label));
        g_strlcpy(decoder_element, ctx->decoder_element,
                  sizeof(decoder_element));
        g_strlcpy(diagnostic, ctx->diagnostic, sizeof(diagnostic));
        g_mutex_unlock(&ctx->stats_mutex);

        g_mutex_lock(&ctx->settings_mutex);
        preset = ctx->preset;
        latency_ms = ctx->latency_ms;
        max_buffers = ctx->max_buffers;
        reconnect_ms = ctx->reconnect_ms;
        tcp_timestamp = ctx->tcp_timestamp;
        prefer_hw_decode = ctx->prefer_hw_decode;
        enable_audio = ctx->enable_audio;
        audio_mode = ctx->audio_mode;
        audio_delay_ms = ctx->audio_delay_ms;
        no_signal_mode = ctx->no_signal_mode;
        g_mutex_unlock(&ctx->settings_mutex);
    }

    gint effective_latency_ms = 0;
    gint effective_buffers = 1;
    switch (preset) {
    case PRESET_BALANCED:
        effective_latency_ms = 80;
        effective_buffers = 2;
        break;
    case PRESET_STABLE:
        effective_latency_ms = 250;
        effective_buffers = 4;
        break;
    case PRESET_CUSTOM:
        effective_latency_ms = MAX(latency_ms, 0);
        effective_buffers = MAX(max_buffers, 1);
        break;
    case PRESET_LOW_LATENCY:
    default:
        effective_latency_ms = 0;
        effective_buffers = 1;
        break;
    }

    guint64 uptime_s = 0;
    if (stats_started_ns > 0) {
        const guint64 now = os_gettime_ns();
        if (now > stats_started_ns)
            uptime_s = (now - stats_started_ns) / 1000000000ULL;
    }

    gchar uptime[48];
    const guint64 hours = uptime_s / 3600ULL;
    const guint64 minutes = (uptime_s % 3600ULL) / 60ULL;
    const guint64 seconds = uptime_s % 60ULL;
    if (hours > 0) {
        g_snprintf(uptime, (gulong)sizeof(uptime),
                   "%" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT
                   ":%02" G_GUINT64_FORMAT,
                   hours, minutes, seconds);
    } else {
        g_snprintf(uptime, (gulong)sizeof(uptime),
                   "%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
                   minutes, seconds);
    }

    guint gst_major = 0, gst_minor = 0, gst_micro = 0, gst_nano = 0;
    gst_version(&gst_major, &gst_minor, &gst_micro, &gst_nano);

    const double fps =
        (fps_d > 0) ? ((double)fps_n / (double)fps_d) : 0.0;
    const char *obs_version = obs_get_version_string();
    const char *vcodec = video_codec[0] ? video_codec : "Not detected";
    const char *acodec = enable_audio
                             ? (audio_codec[0] ? audio_codec : "Not detected")
                             : "Off";
    const char *decoder =
        decoder_label[0] ? decoder_label : "Not detected";
    const char *decoder_element_text =
        decoder_element[0] ? decoder_element : "Not detected";

    g_snprintf(buffer, (gulong)buffer_size,
               "Low Latency RTSP v%s\n"
               "OBS: %s\n"
               "GStreamer: %u.%u.%u\n"
               "State: %s\n"
               "Video: %s | %ux%u | %.2f FPS\n"
               "Decoder: %s (%s)\n"
               "Audio: %s%s%s\n"
               "Preset: %s | Jitter: %d ms | Frame Queue: %d\n"
               "Hardware Decode Preferred: %s\n"
               "TCP Receive Timestamping: %s\n"
               "No Signal Display: %s\n"
               "Reconnect Base Delay: %d ms | Backoff Max: %d ms\n"
               "Video Stall Watchdog: adaptive, minimum 3000 ms\n"
               "Uptime: %s | Video Frames: %" G_GUINT64_FORMAT
               " | Audio Blocks: %" G_GUINT64_FORMAT "\n"
               "Dropped: %" G_GUINT64_FORMAT
               " | Reconnects: %" G_GUINT64_FORMAT
               " | Reconnect Attempt: %u",
               LLRTSP_VERSION,
               obs_version ? obs_version : "Unknown",
               gst_major, gst_minor, gst_micro,
               state_label(state),
               vcodec, width, height, fps,
               decoder, decoder_element_text,
               acodec,
               enable_audio ? " | Mode: " : "",
               enable_audio ? diagnostics_audio_mode_label(audio_mode) : "",
               diagnostics_preset_label(preset),
               effective_latency_ms, effective_buffers,
               prefer_hw_decode ? "Yes" : "No",
               tcp_timestamp ? "On" : "Off",
               diagnostics_no_signal_label(no_signal_mode),
               reconnect_ms, RECONNECT_BACKOFF_MAX_MS,
               uptime, frames, audio_blocks, dropped, reconnects,
               reconnect_attempt);

    if (enable_audio) {
        gchar audio_detail[64];
        g_snprintf(audio_detail, (gulong)sizeof(audio_detail),
                   "\nAudio Delay: %d ms", audio_delay_ms);
        g_strlcat(buffer, audio_detail, buffer_size);
    }

    if (diagnostic[0]) {
        gchar line[DIAGNOSTIC_TEXT_MAX + 32];
        g_snprintf(line, (gulong)sizeof(line),
                   "\nLast Status: %s", diagnostic);
        g_strlcat(buffer, line, buffer_size);
    }

    g_strlcat(buffer,
              "\nRTSP URL / credentials: intentionally omitted",
              buffer_size);
}

static bool copy_diagnostics_clicked(obs_properties_t *props,
                                     obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);

    struct llrtsp_source *ctx = data;
    char diagnostics[2048];
    build_diagnostics_text(ctx, diagnostics, sizeof(diagnostics));
    llrtsp_ui_copy_text(diagnostics);
    return false;
}

static bool update_plugin_clicked(obs_properties_t *props,
                                   obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);
    UNUSED_PARAMETER(data);

    llrtsp_ui_open_url(
        "https://github.com/Splinxes/obs-low-latency-rtsp-source/releases");
    return false;
}

static bool refresh_status_clicked(obs_properties_t *props,
                                   obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(property);
    update_status_property(props, data);
    return true;
}

static bool reset_stats_clicked(obs_properties_t *props,
                                obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(property);
    struct llrtsp_source *ctx = data;
    if (ctx)
        stats_reset(ctx);
    update_status_property(props, ctx);
    return true;
}

static bool reconnect_now_clicked(obs_properties_t *props,
                                  obs_property_t *property, void *data)
{
    UNUSED_PARAMETER(property);
    struct llrtsp_source *ctx = data;

    if (ctx) {
        clear_video_output(ctx);
        stats_note_reconnect(ctx);
        stats_set_diagnostic(ctx, LLRTSP_DIAG_INFO, TRUE,
                             "Manual reconnect requested");
        g_atomic_int_set(&ctx->restart_requested, 1);
    }

    update_status_property(props, ctx);
    return true;
}

static void get_runtime_options(struct llrtsp_source *ctx,
                                struct llrtsp_runtime_options *options)
{
    memset(options, 0, sizeof(*options));

    g_mutex_lock(&ctx->settings_mutex);
    options->url = g_strdup(ctx->url ? ctx->url : "");
    options->reconnect_ms = ctx->reconnect_ms;
    options->tcp_timestamp = ctx->tcp_timestamp;
    options->prefer_hw_decode = ctx->prefer_hw_decode;
    options->enable_audio = ctx->enable_audio;
    options->audio_mode = ctx->audio_mode;
    options->audio_delay_ms = ctx->audio_delay_ms;

    switch (ctx->preset) {
    case PRESET_BALANCED:
        options->latency_ms = 80;
        options->max_buffers = 2;
        break;
    case PRESET_STABLE:
        options->latency_ms = 250;
        options->max_buffers = 4;
        break;
    case PRESET_CUSTOM:
        options->latency_ms = MAX(ctx->latency_ms, 0);
        options->max_buffers = MAX(ctx->max_buffers, 1);
        break;
    case PRESET_LOW_LATENCY:
    default:
        options->latency_ms = 0;
        options->max_buffers = 1;
        break;
    }
    g_mutex_unlock(&ctx->settings_mutex);
}

static void link_dynamic_pad(GstElement *src, GstPad *new_pad,
                             gpointer user_data)
{
    UNUSED_PARAMETER(src);

    GstElement *target = GST_ELEMENT(user_data);
    GstPad *sink_pad = gst_element_get_static_pad(target, "sink");
    if (!sink_pad)
        return;

    if (!gst_pad_is_linked(sink_pad)) {
        GstPadLinkReturn result = gst_pad_link(new_pad, sink_pad);
        if (result != GST_PAD_LINK_OK && result != GST_PAD_LINK_NOFORMAT)
            blog(LOG_DEBUG,
                 "[low-latency-rtsp] Dynamic pad link returned %d",
                 (int)result);
    }

    gst_object_unref(sink_pad);
}

static guint64 apply_audio_delay(guint64 timestamp, gint delay_ms)
{
    const gint64 delay_ns = (gint64)delay_ms * 1000000LL;
    if (delay_ns >= 0)
        return timestamp + (guint64)delay_ns;

    const guint64 amount = (guint64)(-delay_ns);
    return timestamp > amount ? timestamp - amount : 0;
}

static guint64 audio_timestamp_for_sample(struct llrtsp_source *ctx,
                                          GstBuffer *buffer)
{
    gint audio_mode = AUDIO_MODE_LOW_LATENCY;
    gint audio_delay_ms = 0;

    g_mutex_lock(&ctx->settings_mutex);
    audio_mode = ctx->audio_mode;
    audio_delay_ms = ctx->audio_delay_ms;
    g_mutex_unlock(&ctx->settings_mutex);

    guint64 timestamp = os_gettime_ns();

    if (audio_mode == AUDIO_MODE_SYNCHRONIZED &&
        GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
        gboolean have_ref = FALSE;
        GstClockTime video_pts = GST_CLOCK_TIME_NONE;
        guint64 video_local_ns = 0;

        g_mutex_lock(&ctx->stats_mutex);
        have_ref = ctx->have_video_pts_reference;
        video_pts = ctx->last_video_pts;
        video_local_ns = ctx->last_video_local_ns;
        g_mutex_unlock(&ctx->stats_mutex);

        if (have_ref && GST_CLOCK_TIME_IS_VALID(video_pts)) {
            const GstClockTime audio_pts = GST_BUFFER_PTS(buffer);
            gint64 delta = 0;
            if (audio_pts >= video_pts)
                delta = (gint64)(audio_pts - video_pts);
            else
                delta = -(gint64)(video_pts - audio_pts);

            gint64 mapped = (gint64)video_local_ns + delta;
            const gint64 now = (gint64)os_gettime_ns();

            /* Ignore pathological RTP/PTS jumps rather than feeding OBS a
             * timestamp that is seconds away from the current video. */
            if (mapped > 0 && llabs(mapped - now) <= 2000000000LL)
                timestamp = (guint64)mapped;
        }
    }

    return apply_audio_delay(timestamp, audio_delay_ms);
}

static GstFlowReturn on_new_video_sample(GstAppSink *sink, gpointer user_data)
{
    struct llrtsp_source *ctx = user_data;

    if (g_atomic_int_get(&ctx->stop_requested))
        return GST_FLOW_FLUSHING;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_EOS;

    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;

    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoFrame video_frame;
    memset(&video_frame, 0, sizeof(video_frame));

    if (gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
        struct obs_source_frame frame;
        memset(&frame, 0, sizeof(frame));

        frame.format = VIDEO_FORMAT_BGRA;
        frame.width = GST_VIDEO_INFO_WIDTH(&info);
        frame.height = GST_VIDEO_INFO_HEIGHT(&info);
        frame.data[0] = (uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0);
        frame.linesize[0] = (uint32_t)GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);

        const guint64 local_ns = os_gettime_ns();
        frame.timestamp = local_ns;
        frame.full_range = true;

        obs_source_output_video(ctx->source, &frame);
        stats_note_video_sample(ctx, sink, frame.width, frame.height,
                                GST_VIDEO_INFO_FPS_N(&info),
                                GST_VIDEO_INFO_FPS_D(&info),
                                GST_BUFFER_PTS(buffer), local_ns);
        gst_video_frame_unmap(&video_frame);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn on_new_audio_sample(GstAppSink *sink, gpointer user_data)
{
    struct llrtsp_source *ctx = user_data;

    if (g_atomic_int_get(&ctx->stop_requested))
        return GST_FLOW_FLUSHING;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_EOS;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    memset(&map, 0, sizeof(map));
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        const gsize frame_bytes = AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE;
        const gsize frames = frame_bytes ? map.size / frame_bytes : 0;

        if (frames > 0 && frames <= UINT32_MAX) {
            struct obs_source_audio audio;
            memset(&audio, 0, sizeof(audio));
            audio.data[0] = map.data;
            audio.frames = (uint32_t)frames;
            audio.speakers = SPEAKERS_STEREO;
            audio.format = AUDIO_FORMAT_16BIT;
            audio.samples_per_sec = AUDIO_RATE;
            audio.timestamp = audio_timestamp_for_sample(ctx, buffer);
            obs_source_output_audio(ctx->source, &audio);
            stats_note_audio_sample(ctx);
        }

        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstElement *make_element(const char *factory, const char *name)
{
    GstElement *element = gst_element_factory_make(factory, name);
    if (!element)
        blog(LOG_ERROR, "[low-latency-rtsp] Missing GStreamer element: %s", factory);
    return element;
}

static GstCaps *get_pad_caps(GstPad *pad)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, NULL);
    return caps;
}

static gboolean add_rtp_branch(struct llrtsp_pipeline_context *pctx,
                               GstPad *rtp_pad, const char *depay_factory,
                               const char *parser_factory,
                               GstElement *decode_target,
                               const char *depay_name,
                               const char *parser_name)
{
    GstElement *depay = make_element(depay_factory, depay_name);
    GstElement *parser = parser_factory ? make_element(parser_factory, parser_name) : NULL;

    if (!depay || (parser_factory && !parser)) {
        if (depay)
            gst_object_unref(depay);
        if (parser)
            gst_object_unref(parser);
        return FALSE;
    }

    gst_bin_add(GST_BIN(pctx->pipeline), depay);
    if (parser)
        gst_bin_add(GST_BIN(pctx->pipeline), parser);

    gboolean linked = parser
        ? gst_element_link_many(depay, parser, decode_target, NULL)
        : gst_element_link(depay, decode_target);

    if (!linked) {
        blog(LOG_WARNING, "[low-latency-rtsp] Could not link detected RTP codec branch");
        if (parser)
            gst_bin_remove(GST_BIN(pctx->pipeline), parser);
        gst_bin_remove(GST_BIN(pctx->pipeline), depay);
        return FALSE;
    }

    GstPad *sink_pad = gst_element_get_static_pad(depay, "sink");
    if (!sink_pad) {
        if (parser)
            gst_bin_remove(GST_BIN(pctx->pipeline), parser);
        gst_bin_remove(GST_BIN(pctx->pipeline), depay);
        return FALSE;
    }

    const GstPadLinkReturn pad_result = gst_pad_link(rtp_pad, sink_pad);
    gst_object_unref(sink_pad);
    if (pad_result != GST_PAD_LINK_OK) {
        blog(LOG_WARNING, "[low-latency-rtsp] Could not attach RTP pad to detected codec branch (%d)",
             (int)pad_result);
        if (parser)
            gst_bin_remove(GST_BIN(pctx->pipeline), parser);
        gst_bin_remove(GST_BIN(pctx->pipeline), depay);
        return FALSE;
    }

    if (!gst_element_sync_state_with_parent(depay))
        blog(LOG_WARNING, "[low-latency-rtsp] Could not sync depayloader state");
    if (parser && !gst_element_sync_state_with_parent(parser))
        blog(LOG_WARNING, "[low-latency-rtsp] Could not sync parser state");

    return TRUE;
}

static gboolean text_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return FALSE;

    gchar *lower_haystack = g_ascii_strdown(haystack, -1);
    gchar *lower_needle = g_ascii_strdown(needle, -1);
    const gboolean found =
        strstr(lower_haystack, lower_needle) != NULL;
    g_free(lower_haystack);
    g_free(lower_needle);
    return found;
}


static gboolean decoder_factory_is_hardware(const char *factory_name,
                                            const char *klass)
{
    if (klass && text_contains_ci(klass, "hardware"))
        return TRUE;

    if (!factory_name)
        return FALSE;

    return g_str_has_prefix(factory_name, "nvh") ||
           g_str_has_prefix(factory_name, "nv") ||
           g_str_has_prefix(factory_name, "d3d11") ||
           g_str_has_prefix(factory_name, "d3d12") ||
           g_str_has_prefix(factory_name, "qsv") ||
           g_str_has_prefix(factory_name, "vaapi") ||
           g_str_has_prefix(factory_name, "va") ||
           g_str_has_prefix(factory_name, "v4l2") ||
           g_str_has_prefix(factory_name, "amf");
}

static void decoder_friendly_label(const char *factory_name,
                                   gboolean hardware,
                                   char *out, size_t out_size)
{
    if (!factory_name || !*factory_name) {
        g_strlcpy(out, hardware ? "Hardware" : "Software", out_size);
        return;
    }

    if (g_str_has_prefix(factory_name, "nvh") ||
        g_str_has_prefix(factory_name, "nv")) {
        g_strlcpy(out, "NVIDIA NVDEC", out_size);
    } else if (g_str_has_prefix(factory_name, "d3d11")) {
        g_strlcpy(out, "Direct3D 11 Hardware", out_size);
    } else if (g_str_has_prefix(factory_name, "d3d12")) {
        g_strlcpy(out, "Direct3D 12 Hardware", out_size);
    } else if (g_str_has_prefix(factory_name, "qsv")) {
        g_strlcpy(out, "Intel Quick Sync", out_size);
    } else if (g_str_has_prefix(factory_name, "vaapi") ||
               g_str_has_prefix(factory_name, "va")) {
        g_strlcpy(out, "VA-API Hardware", out_size);
    } else if (g_str_has_prefix(factory_name, "v4l2")) {
        g_strlcpy(out, "V4L2 Hardware", out_size);
    } else if (g_str_has_prefix(factory_name, "amf")) {
        g_strlcpy(out, "AMD Hardware", out_size);
    } else if (g_str_has_prefix(factory_name, "avdec_")) {
        g_strlcpy(out, "Software (FFmpeg)", out_size);
    } else if (g_str_has_prefix(factory_name, "openh264")) {
        g_strlcpy(out, "Software (OpenH264)", out_size);
    } else {
        g_strlcpy(out, hardware ? "Hardware" : "Software", out_size);
    }
}

static void stats_set_decoder(struct llrtsp_source *ctx,
                              const char *label,
                              const char *element,
                              gboolean hardware,
                              gboolean software_fallback)
{
    g_mutex_lock(&ctx->stats_mutex);
    g_strlcpy(ctx->decoder_label, label ? label : "",
              sizeof(ctx->decoder_label));
    g_strlcpy(ctx->decoder_element, element ? element : "",
              sizeof(ctx->decoder_element));
    ctx->decoder_seen = label && *label;
    ctx->decoder_is_hardware = hardware;
    ctx->software_fallback_active = software_fallback;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_prepare_decoder(struct llrtsp_source *ctx,
                                  gboolean software_fallback)
{
    g_mutex_lock(&ctx->stats_mutex);
    ctx->decoder_label[0] = '\0';
    ctx->decoder_element[0] = '\0';
    ctx->decoder_seen = FALSE;
    ctx->decoder_is_hardware = FALSE;
    ctx->software_fallback_active = software_fallback;
    g_mutex_unlock(&ctx->stats_mutex);
}

static void stats_get_decoder_state(struct llrtsp_source *ctx,
                                    gboolean *seen,
                                    gboolean *hardware)
{
    g_mutex_lock(&ctx->stats_mutex);
    if (seen)
        *seen = ctx->decoder_seen;
    if (hardware)
        *hardware = ctx->decoder_is_hardware;
    g_mutex_unlock(&ctx->stats_mutex);
}

static gboolean stream_has_connected_video(struct llrtsp_source *ctx)
{
    gboolean connected = FALSE;

    g_mutex_lock(&ctx->stats_mutex);
    connected = ctx->state == LLRTSP_STATE_CONNECTED &&
                ctx->connection_started_ns != 0;
    g_mutex_unlock(&ctx->stats_mutex);

    return connected;
}

static gboolean decoder_error_is_fallback_candidate(const GError *error,
                                                    const char *debug)
{
    if (error && error->domain == GST_STREAM_ERROR) {
        switch (error->code) {
        case GST_STREAM_ERROR_CODEC_NOT_FOUND:
        case GST_STREAM_ERROR_DECODE:
        case GST_STREAM_ERROR_WRONG_TYPE:
        case GST_STREAM_ERROR_FORMAT:
            return TRUE;
        default:
            break;
        }
    }

    const char *message = error ? error->message : NULL;
    return text_contains_ci(message, "decoder") ||
           text_contains_ci(debug, "decoder") ||
           text_contains_ci(message, "decode") ||
           text_contains_ci(debug, "decode") ||
           text_contains_ci(message, "not-negotiated") ||
           text_contains_ci(debug, "not-negotiated") ||
           text_contains_ci(message, "negotiat") ||
           text_contains_ci(debug, "negotiat");
}

static void on_video_decoder_element_added(GstBin *bin,
                                           GstElement *element,
                                           gpointer user_data)
{
    UNUSED_PARAMETER(bin);
    struct llrtsp_pipeline_context *pctx = user_data;

    if (!pctx || !element)
        return;

    if (GST_IS_BIN(element)) {
        g_signal_connect(element, "element-added",
                         G_CALLBACK(on_video_decoder_element_added), pctx);
    }

    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory)
        return;

    const char *klass =
        gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    if (!klass || !text_contains_ci(klass, "decoder") ||
        !text_contains_ci(klass, "video"))
        return;

    const char *factory_name =
        gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (!factory_name || !*factory_name)
        return;

    const gboolean hardware =
        decoder_factory_is_hardware(factory_name, klass);
    char label[DECODER_LABEL_MAX];
    decoder_friendly_label(factory_name, hardware, label, sizeof(label));

    stats_set_decoder(pctx->ctx, label, factory_name, hardware,
                      pctx->force_software_decode);

    blog(LOG_INFO,
         "[low-latency-rtsp] Selected video decoder: %s (%s, %s)",
         label, factory_name, hardware ? "hardware" : "software");
}

static void classify_gstreamer_error(const GError *error, const char *debug,
                                     char *out, size_t out_size)
{
    const char *message = error ? error->message : NULL;

    if (error && error->domain == GST_RESOURCE_ERROR &&
        error->code == GST_RESOURCE_ERROR_NOT_AUTHORIZED) {
        g_strlcpy(out, "Authentication failed", out_size);
        return;
    }

    if (error && error->domain == GST_RESOURCE_ERROR &&
        error->code == GST_RESOURCE_ERROR_NOT_FOUND) {
        g_strlcpy(out, "RTSP stream not found", out_size);
        return;
    }

    if (text_contains_ci(message, "401") ||
        text_contains_ci(debug, "401") ||
        text_contains_ci(message, "unauthorized") ||
        text_contains_ci(debug, "unauthorized") ||
        text_contains_ci(message, "not authorized") ||
        text_contains_ci(debug, "not authorized")) {
        g_strlcpy(out, "Authentication failed", out_size);
        return;
    }

    if (text_contains_ci(message, "404") ||
        text_contains_ci(debug, "404") ||
        text_contains_ci(message, "not found") ||
        text_contains_ci(debug, "not found")) {
        g_strlcpy(out, "RTSP stream not found", out_size);
        return;
    }

    if (text_contains_ci(message, "timed out") ||
        text_contains_ci(debug, "timed out") ||
        text_contains_ci(message, "timeout") ||
        text_contains_ci(debug, "timeout")) {
        g_strlcpy(out, "Camera unreachable", out_size);
        return;
    }

    if (text_contains_ci(message, "connection refused") ||
        text_contains_ci(debug, "connection refused") ||
        text_contains_ci(message, "refused") ||
        text_contains_ci(debug, "refused")) {
        g_strlcpy(out, "Camera unreachable", out_size);
        return;
    }

    if (text_contains_ci(message, "network is unreachable") ||
        text_contains_ci(debug, "network is unreachable") ||
        text_contains_ci(message, "host is unreachable") ||
        text_contains_ci(debug, "host is unreachable") ||
        text_contains_ci(message, "no route to host") ||
        text_contains_ci(debug, "no route to host") ||
        text_contains_ci(message, "name or service not known") ||
        text_contains_ci(debug, "name or service not known") ||
        text_contains_ci(message, "could not resolve") ||
        text_contains_ci(debug, "could not resolve")) {
        g_strlcpy(out, "Camera unreachable", out_size);
        return;
    }

    if (error && error->domain == GST_STREAM_ERROR) {
        if (error->code == GST_STREAM_ERROR_CODEC_NOT_FOUND ||
            error->code == GST_STREAM_ERROR_TYPE_NOT_FOUND ||
            error->code == GST_STREAM_ERROR_WRONG_TYPE) {
            g_strlcpy(out, "Unsupported media codec", out_size);
            return;
        }

        if (error->code == GST_STREAM_ERROR_DECODE) {
            g_strlcpy(out, "Media decode error", out_size);
            return;
        }

        if (error->code == GST_STREAM_ERROR_FORMAT) {
            g_strlcpy(out, "Unsupported stream format", out_size);
            return;
        }
    }

    if (error && error->domain == GST_RESOURCE_ERROR) {
        g_strlcpy(out, "Camera unreachable", out_size);
        return;
    }

    g_strlcpy(out, "RTSP stream error", out_size);
}

static void on_rtsp_pad_added(GstElement *src, GstPad *new_pad,
                              gpointer user_data)
{
    UNUSED_PARAMETER(src);
    struct llrtsp_pipeline_context *pctx = user_data;

    GstCaps *caps = get_pad_caps(new_pad);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps)
            gst_caps_unref(caps);
        return;
    }

    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const char *media = gst_structure_get_string(s, "media");
    const char *encoding = gst_structure_get_string(s, "encoding-name");

    if (!media || !encoding) {
        gst_caps_unref(caps);
        return;
    }

    if (g_ascii_strcasecmp(media, "video") == 0) {
        if (!g_atomic_int_compare_and_exchange(&pctx->video_claimed, 0, 1)) {
            gst_caps_unref(caps);
            return;
        }

        gboolean ok = FALSE;
        const char *label = NULL;

        if (g_ascii_strcasecmp(encoding, "H264") == 0) {
            label = "H.264";
            ok = add_rtp_branch(pctx, new_pad, "rtph264depay", "h264parse",
                                pctx->video_decode, "video-depay-h264",
                                "video-parse-h264");
        } else if (g_ascii_strcasecmp(encoding, "H265") == 0 ||
                   g_ascii_strcasecmp(encoding, "HEVC") == 0) {
            label = "H.265 / HEVC";
            ok = add_rtp_branch(pctx, new_pad, "rtph265depay", "h265parse",
                                pctx->video_decode, "video-depay-h265",
                                "video-parse-h265");
        } else {
            gchar reason[DIAGNOSTIC_TEXT_MAX];
            g_snprintf(reason, (gulong)sizeof(reason),
                       "Unsupported video codec: %.64s", encoding);
            stats_set_state(pctx->ctx, LLRTSP_STATE_ERROR);
            stats_set_diagnostic(pctx->ctx, LLRTSP_DIAG_ERROR, FALSE,
                                 reason);
            blog(LOG_WARNING,
                 "[low-latency-rtsp] Unsupported RTSP video codec: %s",
                 encoding);
        }

        if (ok) {
            stats_clear_codec_diagnostic(pctx->ctx, FALSE);
            stats_set_codec(pctx->ctx, FALSE, label);
            blog(LOG_INFO, "[low-latency-rtsp] Auto-detected video codec: %s", label);
        } else {
            g_atomic_int_set(&pctx->video_claimed, 0);
        }
    } else if (g_ascii_strcasecmp(media, "audio") == 0 && pctx->audio_enabled) {
        stats_note_audio_track(pctx->ctx);

        if (!g_atomic_int_compare_and_exchange(&pctx->audio_claimed, 0, 1)) {
            gst_caps_unref(caps);
            return;
        }

        const char *factory = NULL;
        const char *label = NULL;

        if (g_ascii_strcasecmp(encoding, "MPEG4-GENERIC") == 0) {
            factory = "rtpmp4gdepay";
            label = "AAC";
        } else if (g_ascii_strcasecmp(encoding, "MP4A-LATM") == 0) {
            factory = "rtpmp4adepay";
            label = "AAC-LATM";
        } else if (g_ascii_strcasecmp(encoding, "OPUS") == 0) {
            factory = "rtpopusdepay";
            label = "Opus";
        } else if (g_ascii_strcasecmp(encoding, "PCMU") == 0) {
            factory = "rtppcmudepay";
            label = "G.711 mu-law";
        } else if (g_ascii_strcasecmp(encoding, "PCMA") == 0) {
            factory = "rtppcmadepay";
            label = "G.711 A-law";
        }

        gboolean ok = FALSE;
        if (factory) {
            ok = add_rtp_branch(pctx, new_pad, factory, NULL,
                                pctx->audio_decode, "audio-depay", NULL);
        } else {
            gchar reason[DIAGNOSTIC_TEXT_MAX];
            gchar codec_label[CODEC_NAME_MAX];
            g_snprintf(reason, (gulong)sizeof(reason),
                       "Unsupported audio codec: %.64s", encoding);
            g_snprintf(codec_label, (gulong)sizeof(codec_label),
                       "Unsupported (%.28s)", encoding);
            stats_set_codec(pctx->ctx, TRUE, codec_label);
            stats_set_diagnostic(pctx->ctx, LLRTSP_DIAG_WARNING, FALSE,
                                 reason);
            blog(LOG_WARNING,
                 "[low-latency-rtsp] Unsupported RTSP audio codec: %s",
                 encoding);
        }

        if (ok) {
            stats_clear_codec_diagnostic(pctx->ctx, TRUE);
            stats_set_codec(pctx->ctx, TRUE, label);
            blog(LOG_INFO, "[low-latency-rtsp] Auto-detected audio codec: %s", label);
        } else {
            g_atomic_int_set(&pctx->audio_claimed, 0);
        }
    }

    gst_caps_unref(caps);
}

static GstElement *build_pipeline(struct llrtsp_source *ctx,
                                  GstElement **video_sink_out,
                                  gboolean force_software_decode)
{
    struct llrtsp_runtime_options options;
    get_runtime_options(ctx, &options);

    gchar *url = options.url;
    if (!url || !*url) {
        g_free(url);
        return NULL;
    }

    stats_prepare_decoder(ctx, force_software_decode);

    GstElement *pipeline = gst_pipeline_new("llrtsp-pipeline");
    GstElement *src = make_element("rtspsrc", "rtsp-source");
    GstElement *video_decode = make_element("decodebin", "video-decode");
    GstElement *video_convert = make_element("videoconvert", "video-convert");
    GstElement *video_capsfilter = make_element("capsfilter", "bgra-caps");
    GstElement *video_sink = make_element("appsink", "obs-video-appsink");

    GstElement *audio_decode = NULL;
    GstElement *audio_convert = NULL;
    GstElement *audio_resample = NULL;
    GstElement *audio_capsfilter = NULL;
    GstElement *audio_sink = NULL;

    if (options.enable_audio) {
        audio_decode = make_element("decodebin", "audio-decode");
        audio_convert = make_element("audioconvert", "audio-convert");
        audio_resample = make_element("audioresample", "audio-resample");
        audio_capsfilter = make_element("capsfilter", "audio-caps");
        audio_sink = make_element("appsink", "obs-audio-appsink");
    }

    if (!pipeline || !src || !video_decode || !video_convert ||
        !video_capsfilter || !video_sink ||
        (options.enable_audio && (!audio_decode || !audio_convert ||
                                  !audio_resample || !audio_capsfilter ||
                                  !audio_sink))) {
        if (pipeline)
            gst_object_unref(pipeline);
        else {
            if (src) gst_object_unref(src);
            if (video_decode) gst_object_unref(video_decode);
            if (video_convert) gst_object_unref(video_convert);
            if (video_capsfilter) gst_object_unref(video_capsfilter);
            if (video_sink) gst_object_unref(video_sink);
            if (audio_decode) gst_object_unref(audio_decode);
            if (audio_convert) gst_object_unref(audio_convert);
            if (audio_resample) gst_object_unref(audio_resample);
            if (audio_capsfilter) gst_object_unref(audio_capsfilter);
            if (audio_sink) gst_object_unref(audio_sink);
        }
        g_free(url);
        return NULL;
    }

    GstCaps *video_caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGRA", NULL);

    g_object_set(src, "location", url,
                 "protocols", GST_RTSP_LOWER_TRANS_TCP,
                 "latency", (guint)MAX(options.latency_ms, 0),
                 "drop-on-latency", TRUE,
                 "tcp-timestamp", options.tcp_timestamp,
                 NULL);

    g_object_set(video_decode, "force-sw-decoders",
                 force_software_decode || !options.prefer_hw_decode, NULL);
    g_object_set(video_capsfilter, "caps", video_caps, NULL);
    gst_caps_unref(video_caps);

    g_object_set(video_sink,
                 "sync", FALSE,
                 "max-buffers", (guint)MAX(options.max_buffers, 1),
                 "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM,
                 "wait-on-eos", FALSE,
                 "enable-last-sample", FALSE,
                 NULL);

    GstAppSinkCallbacks video_callbacks;
    memset(&video_callbacks, 0, sizeof(video_callbacks));
    video_callbacks.new_sample = on_new_video_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(video_sink),
                               &video_callbacks, ctx, NULL);

    gst_bin_add_many(GST_BIN(pipeline), src, video_decode, video_convert,
                     video_capsfilter, video_sink, NULL);

    if (!gst_element_link_many(video_convert, video_capsfilter,
                               video_sink, NULL)) {
        blog(LOG_ERROR, "[low-latency-rtsp] Failed to link video output chain");
        gst_object_unref(pipeline);
        g_free(url);
        return NULL;
    }

    g_signal_connect(video_decode, "pad-added",
                     G_CALLBACK(link_dynamic_pad), video_convert);

    if (options.enable_audio) {
        GstCaps *audio_caps = gst_caps_new_simple(
            "audio/x-raw",
            "format", G_TYPE_STRING, "S16LE",
            "rate", G_TYPE_INT, AUDIO_RATE,
            "channels", G_TYPE_INT, AUDIO_CHANNELS,
            "layout", G_TYPE_STRING, "interleaved",
            NULL);

        g_object_set(audio_capsfilter, "caps", audio_caps, NULL);
        gst_caps_unref(audio_caps);

        const guint audio_buffers =
            options.audio_mode == AUDIO_MODE_SYNCHRONIZED ? 32U : 4U;
        g_object_set(audio_sink,
                     "sync", FALSE,
                     "max-buffers", audio_buffers,
                     "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM,
                     "wait-on-eos", FALSE,
                     "enable-last-sample", FALSE,
                     NULL);

        GstAppSinkCallbacks audio_callbacks;
        memset(&audio_callbacks, 0, sizeof(audio_callbacks));
        audio_callbacks.new_sample = on_new_audio_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(audio_sink),
                                   &audio_callbacks, ctx, NULL);

        gst_bin_add_many(GST_BIN(pipeline), audio_decode, audio_convert,
                         audio_resample, audio_capsfilter, audio_sink, NULL);

        if (!gst_element_link_many(audio_convert, audio_resample,
                                   audio_capsfilter, audio_sink, NULL)) {
            blog(LOG_ERROR, "[low-latency-rtsp] Failed to link audio output chain");
            gst_object_unref(pipeline);
            g_free(url);
            return NULL;
        }

        g_signal_connect(audio_decode, "pad-added",
                         G_CALLBACK(link_dynamic_pad), audio_convert);
    }

    struct llrtsp_pipeline_context *pctx = g_new0(struct llrtsp_pipeline_context, 1);
    pctx->ctx = ctx;
    pctx->pipeline = pipeline;
    pctx->video_decode = video_decode;
    pctx->audio_decode = audio_decode;
    pctx->audio_enabled = options.enable_audio;
    pctx->prefer_hw_decode = options.prefer_hw_decode;
    pctx->force_software_decode = force_software_decode;
    g_object_set_data_full(G_OBJECT(pipeline), "llrtsp-pipeline-context",
                           pctx, g_free);

    g_signal_connect(video_decode, "element-added",
                     G_CALLBACK(on_video_decoder_element_added), pctx);
    g_signal_connect(src, "pad-added", G_CALLBACK(on_rtsp_pad_added), pctx);

    if (video_sink_out)
        *video_sink_out = video_sink;

    blog(LOG_INFO,
         "[low-latency-rtsp] Pipeline configured: latency=%dms buffers=%d hw-preferred=%s force-software=%s audio=%s",
         options.latency_ms, options.max_buffers,
         options.prefer_hw_decode ? "yes" : "no",
         force_software_decode ? "yes" : "no",
         options.enable_audio ? "on" : "off");

    g_free(url);
    return pipeline;
}

static void sleep_interruptible(struct llrtsp_source *ctx, gint milliseconds)
{
    gint remaining = MAX(milliseconds, 0);
    while (remaining > 0 && !g_atomic_int_get(&ctx->stop_requested) &&
           !g_atomic_int_get(&ctx->restart_requested)) {
        const gint slice = MIN(remaining, 100);
        g_usleep((gulong)slice * 1000);
        remaining -= slice;
    }
}

static gboolean has_configured_url(struct llrtsp_source *ctx)
{
    gboolean has_url = FALSE;
    g_mutex_lock(&ctx->settings_mutex);
    has_url = ctx->url && *ctx->url;
    g_mutex_unlock(&ctx->settings_mutex);
    return has_url;
}

static gboolean prefer_hardware_decode_enabled(struct llrtsp_source *ctx)
{
    gboolean enabled = FALSE;
    g_mutex_lock(&ctx->settings_mutex);
    enabled = ctx->prefer_hw_decode;
    g_mutex_unlock(&ctx->settings_mutex);
    return enabled;
}

static gpointer pipeline_worker(gpointer data)
{
    struct llrtsp_source *ctx = data;
    gboolean force_software_fallback = FALSE;
    guint reconnect_attempt = 0;

    while (!g_atomic_int_get(&ctx->stop_requested)) {
        g_atomic_int_set(&ctx->restart_requested, 0);

        if (!has_configured_url(ctx)) {
            reconnect_attempt = 0;
            force_software_fallback = FALSE;
            stats_prepare_decoder(ctx, FALSE);
            clear_video_output(ctx);
            stats_clear_reconnect_schedule(ctx);
            stats_set_state(ctx, LLRTSP_STATE_WAITING);
            stats_set_diagnostic(ctx, LLRTSP_DIAG_NONE, FALSE, "");
            sleep_interruptible(ctx, 500);
            continue;
        }

        stats_clear_reconnect_schedule(ctx);
        stats_set_state(ctx, LLRTSP_STATE_CONNECTING);

        GstElement *video_sink = NULL;
        GstElement *pipeline = build_pipeline(ctx, &video_sink,
                                              force_software_fallback);
        if (!pipeline) {
            clear_video_output(ctx);
            stats_set_diagnostic(ctx, LLRTSP_DIAG_ERROR, TRUE,
                                 "Could not create RTSP pipeline");
            stats_note_reconnect(ctx);

            gint base_ms = 1000;
            g_mutex_lock(&ctx->settings_mutex);
            base_ms = ctx->reconnect_ms;
            g_mutex_unlock(&ctx->settings_mutex);

            const gint delay_ms =
                reconnect_backoff_delay_ms(base_ms, reconnect_attempt);
            reconnect_attempt++;
            stats_schedule_reconnect(ctx, reconnect_attempt, delay_ms);
            sleep_interruptible(ctx, delay_ms);
            stats_clear_reconnect_schedule(ctx);

            if (g_atomic_int_get(&ctx->restart_requested)) {
                reconnect_attempt = 0;
                force_software_fallback = FALSE;
            }
            continue;
        }

        GstBus *bus = gst_element_get_bus(pipeline);
        GstStateChangeReturn state_result =
            gst_element_set_state(pipeline, GST_STATE_PLAYING);

        gboolean reconnect = FALSE;
        gboolean immediate_retry = FALSE;
        if (state_result == GST_STATE_CHANGE_FAILURE) {
            blog(LOG_WARNING,
                 "[low-latency-rtsp] Pipeline failed to enter PLAYING; reconnecting");
            clear_video_output(ctx);

            gboolean decoder_seen = FALSE;
            gboolean decoder_hardware = FALSE;
            stats_get_decoder_state(ctx, &decoder_seen, &decoder_hardware);

            if (!force_software_fallback &&
                prefer_hardware_decode_enabled(ctx) &&
                decoder_seen && decoder_hardware) {
                force_software_fallback = TRUE;
                immediate_retry = TRUE;
                stats_set_diagnostic(
                    ctx, LLRTSP_DIAG_WARNING, FALSE,
                    "Hardware decoder failed - software fallback active");
            } else {
                stats_set_diagnostic(ctx, LLRTSP_DIAG_ERROR, TRUE,
                                     "Failed to start RTSP pipeline");
            }

            stats_note_reconnect(ctx);
            reconnect = TRUE;
        } else {
            stats_note_pipeline_started(ctx, os_gettime_ns());
        }

        while (!reconnect && !g_atomic_int_get(&ctx->stop_requested) &&
               !g_atomic_int_get(&ctx->restart_requested)) {
            GstMessage *message = gst_bus_timed_pop_filtered(
                bus, 250 * GST_MSECOND,
                GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

            if (!message) {
                if (stream_has_connected_video(ctx))
                    reconnect_attempt = 0;

                gboolean stalled = FALSE;
                if (watchdog_should_reconnect(ctx, &stalled)) {
                    if (stalled) {
                        blog(LOG_WARNING,
                             "[low-latency-rtsp] Video watchdog detected a stalled stream; reconnecting");
                        stats_set_diagnostic(ctx, LLRTSP_DIAG_WARNING, TRUE,
                                             "Video stalled");
                    } else {
                        blog(LOG_WARNING,
                             "[low-latency-rtsp] Video watchdog received no initial frames; reconnecting");
                        stats_set_diagnostic(ctx, LLRTSP_DIAG_WARNING, TRUE,
                                             "No video frames");
                    }

                    clear_video_output(ctx);
                    stats_note_reconnect(ctx);
                    reconnect = TRUE;
                }
                continue;
            }

            switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError *error = NULL;
                gchar *debug = NULL;
                gst_message_parse_error(message, &error, &debug);

                blog(LOG_WARNING,
                     "[low-latency-rtsp] Stream error (domain=%s code=%d); reconnecting",
                     error ? g_quark_to_string(error->domain) : "unknown",
                     error ? error->code : -1);

                gchar diagnostic[DIAGNOSTIC_TEXT_MAX];
                classify_gstreamer_error(error, debug, diagnostic,
                                         sizeof(diagnostic));
                clear_video_output(ctx);

                const gboolean decoder_fallback =
                    !force_software_fallback &&
                    prefer_hardware_decode_enabled(ctx) &&
                    !stream_has_connected_video(ctx) &&
                    decoder_error_is_fallback_candidate(error, debug);

                if (decoder_fallback) {
                    force_software_fallback = TRUE;
                    immediate_retry = TRUE;
                    stats_set_diagnostic(
                        ctx, LLRTSP_DIAG_WARNING, FALSE,
                        "Hardware decoder failed - software fallback active");
                    blog(LOG_WARNING,
                         "[low-latency-rtsp] Hardware decoder startup failed; retrying with software decoding");
                } else {
                    stats_set_diagnostic(ctx, LLRTSP_DIAG_ERROR, TRUE,
                                         diagnostic);
                }

                if (error)
                    g_error_free(error);
                g_free(debug);
                stats_note_reconnect(ctx);
                reconnect = TRUE;
                break;
            }
            case GST_MESSAGE_EOS:
                blog(LOG_WARNING, "[low-latency-rtsp] Stream ended; reconnecting");
                clear_video_output(ctx);
                stats_set_diagnostic(ctx, LLRTSP_DIAG_WARNING, TRUE,
                                     "RTSP stream ended");
                stats_note_reconnect(ctx);
                reconnect = TRUE;
                break;
            default:
                break;
            }

            gst_message_unref(message);
        }

        if (video_sink) {
            guint64 dropped = 0;
            g_object_get(G_OBJECT(video_sink), "dropped", &dropped, NULL);
            stats_update_dropped(ctx, dropped);
        }

        gst_element_set_state(pipeline, GST_STATE_NULL);
        clear_video_output(ctx);
        stats_finalize_pipeline(ctx);
        gst_object_unref(bus);
        gst_object_unref(pipeline);

        if (g_atomic_int_get(&ctx->stop_requested))
            break;

        if (g_atomic_int_get(&ctx->restart_requested)) {
            force_software_fallback = FALSE;
            reconnect_attempt = 0;
            stats_clear_reconnect_schedule(ctx);
            stats_set_state(ctx, LLRTSP_STATE_CONNECTING);
            continue;
        }

        if (reconnect && !immediate_retry) {
            gint base_ms = 1000;
            g_mutex_lock(&ctx->settings_mutex);
            base_ms = ctx->reconnect_ms;
            g_mutex_unlock(&ctx->settings_mutex);

            const gint delay_ms =
                reconnect_backoff_delay_ms(base_ms, reconnect_attempt);
            reconnect_attempt++;
            stats_schedule_reconnect(ctx, reconnect_attempt, delay_ms);
            sleep_interruptible(ctx, delay_ms);
            stats_clear_reconnect_schedule(ctx);

            if (g_atomic_int_get(&ctx->restart_requested)) {
                force_software_fallback = FALSE;
                reconnect_attempt = 0;
            }
        } else if (immediate_retry) {
            stats_clear_reconnect_schedule(ctx);
        }
    }

    stats_clear_reconnect_schedule(ctx);
    stats_set_state(ctx, LLRTSP_STATE_STOPPED);
    obs_source_output_video(ctx->source, NULL);
    return NULL;
}

static void llrtsp_video_tick(void *data, float seconds)
{
    struct llrtsp_source *ctx = data;
    if (!ctx || g_atomic_int_get(&ctx->stop_requested))
        return;

    ctx->status_refresh_elapsed_s += (double)seconds;
    if (ctx->status_refresh_elapsed_s < 1.0)
        return;

    ctx->status_refresh_elapsed_s = 0.0;

    /*
     * Update only the status label in the open Properties dialog. Rebuilding
     * the full OBS properties tree every second destroys and recreates the
     * URL Show/Hide control and combo boxes, which closes open dropdowns and
     * resets transient UI state. The Qt bridge queues a label-only update on
     * OBS's UI thread and leaves every interactive control untouched.
     */
    char text[768];
    enum obs_text_info_type type = OBS_TEXT_INFO_NORMAL;
    status_snapshot(ctx, text, sizeof(text), &type);
    const char *source_name = obs_source_get_name(ctx->source);
    llrtsp_ui_update_status(source_name, text, (int)type);

    /* Keep the UniFi guidance synchronized even when the Properties dialog
     * opens after the URL was already saved. This updates only the existing
     * top information label and never rebuilds the OBS properties tree. */
    gboolean show_unifi_hint = FALSE;
    g_mutex_lock(&ctx->settings_mutex);
    show_unifi_hint = ctx->unifi_hint_latched;
    g_mutex_unlock(&ctx->settings_mutex);
    llrtsp_ui_update_unifi_hint(source_name, obs_module_text("Info"),
                                obs_module_text("UniFiRTSPHint"),
                                show_unifi_hint);
}

static void llrtsp_update(void *data, obs_data_t *settings)
{
    struct llrtsp_source *ctx = data;
    if (!ctx)
        return;

    const char *url = obs_data_get_string(settings, SETTING_URL);
    const gint preset = (gint)obs_data_get_int(settings, SETTING_PRESET);
    const gint latency_ms = (gint)obs_data_get_int(settings, SETTING_LATENCY_MS);
    const gint max_buffers = (gint)obs_data_get_int(settings, SETTING_MAX_BUFFERS);
    const gint reconnect_ms = (gint)obs_data_get_int(settings, SETTING_RECONNECT_MS);
    const gboolean tcp_timestamp = obs_data_get_bool(settings, SETTING_TCP_TIMESTAMP);
    const gboolean prefer_hw_decode = obs_data_get_bool(settings, SETTING_PREFER_HW_DECODE);
    const gboolean enable_audio = obs_data_get_bool(settings, SETTING_ENABLE_AUDIO);
    const gint audio_mode = (gint)obs_data_get_int(settings, SETTING_AUDIO_MODE);
    const gint audio_delay_ms = (gint)obs_data_get_int(settings, SETTING_AUDIO_DELAY_MS);
    const gboolean show_advanced = obs_data_get_bool(settings, SETTING_SHOW_ADVANCED);
    const gint no_signal_mode = (gint)obs_data_get_int(settings, SETTING_NO_SIGNAL_MODE);

    g_mutex_lock(&ctx->settings_mutex);
    const gboolean url_changed = g_strcmp0(ctx->url, url ? url : "") != 0;

    if (url_changed) {
        if (!url || !*url) {
            ctx->unifi_hint_latched = FALSE;
        } else if (unifi_protect_conversion_complete(url)) {
            ctx->unifi_hint_latched = FALSE;
        } else if (looks_like_unifi_protect_secure_url(url)) {
            ctx->unifi_hint_latched = TRUE;
        }
        /*
         * Otherwise keep the existing latched state. This is what preserves
         * the guidance while the user temporarily has an incomplete scheme,
         * port, or query string during editing.
         */
    }

    const gboolean show_unifi_hint = ctx->unifi_hint_latched;
    g_free(ctx->url);
    ctx->url = g_strdup(url ? url : "");
    ctx->preset = CLAMP(preset, PRESET_LOW_LATENCY, PRESET_CUSTOM);
    ctx->latency_ms = MAX(latency_ms, 0);
    ctx->max_buffers = CLAMP(max_buffers, 1, 16);
    ctx->reconnect_ms = CLAMP(reconnect_ms, 100, RECONNECT_BACKOFF_MAX_MS);
    ctx->tcp_timestamp = tcp_timestamp;
    ctx->prefer_hw_decode = prefer_hw_decode;
    ctx->enable_audio = enable_audio;
    ctx->audio_mode = CLAMP(audio_mode, AUDIO_MODE_LOW_LATENCY,
                            AUDIO_MODE_SYNCHRONIZED);
    ctx->audio_delay_ms = CLAMP(audio_delay_ms, -500, 2000);
    const gboolean no_signal_changed =
        ctx->no_signal_mode != CLAMP(no_signal_mode, NO_SIGNAL_TRANSPARENT,
                                     NO_SIGNAL_NO_SIGNAL);
    ctx->show_advanced = show_advanced;
    ctx->no_signal_mode = CLAMP(no_signal_mode, NO_SIGNAL_TRANSPARENT,
                                NO_SIGNAL_NO_SIGNAL);

    g_mutex_unlock(&ctx->settings_mutex);

    if (url_changed || no_signal_changed)
        clear_video_output(ctx);

    /*
     * OBS password text properties do not reliably fire a per-field modified
     * callback when text is pasted. The source update path does receive the
     * committed setting change, so detect the Protect RTSPS pattern here.
     * Trigger only when the URL actually changes to avoid repeated popups from
     * unrelated setting updates.
     */
    if (url_changed)
        llrtsp_ui_update_unifi_hint(
            obs_source_get_name(ctx->source),
            obs_module_text("Info"),
            obs_module_text("UniFiRTSPHint"),
            show_unifi_hint);

    obs_source_set_audio_active(ctx->source, enable_audio);
    g_atomic_int_set(&ctx->restart_requested, 1);
}

static void *llrtsp_create(obs_data_t *settings, obs_source_t *source)
{
    struct llrtsp_source *ctx = bzalloc(sizeof(*ctx));
    ctx->source = source;
    g_mutex_init(&ctx->settings_mutex);
    g_mutex_init(&ctx->stats_mutex);

    /*
     * This source is explicitly designed for minimum live latency. OBS async
     * sources are buffered by default, which can retain older frames in the
     * libobs async queue. Keep only the newest frame so creating/configuring a
     * source at runtime behaves like a clean OBS startup.
     */
    obs_source_set_async_unbuffered(source, true);

    const char *url = obs_data_get_string(settings, SETTING_URL);
    ctx->url = g_strdup(url ? url : "");
    ctx->unifi_hint_latched =
        looks_like_unifi_protect_secure_url(ctx->url) &&
        !unifi_protect_conversion_complete(ctx->url);
    ctx->preset = (gint)obs_data_get_int(settings, SETTING_PRESET);
    ctx->latency_ms = (gint)obs_data_get_int(settings, SETTING_LATENCY_MS);
    ctx->max_buffers = (gint)obs_data_get_int(settings, SETTING_MAX_BUFFERS);
    ctx->reconnect_ms = CLAMP((gint)obs_data_get_int(settings, SETTING_RECONNECT_MS),
                              100, RECONNECT_BACKOFF_MAX_MS);
    ctx->tcp_timestamp = obs_data_get_bool(settings, SETTING_TCP_TIMESTAMP);
    ctx->prefer_hw_decode = obs_data_get_bool(settings, SETTING_PREFER_HW_DECODE);
    ctx->enable_audio = obs_data_get_bool(settings, SETTING_ENABLE_AUDIO);
    ctx->audio_mode = (gint)obs_data_get_int(settings, SETTING_AUDIO_MODE);
    ctx->audio_delay_ms = (gint)obs_data_get_int(settings, SETTING_AUDIO_DELAY_MS);
    ctx->show_advanced = obs_data_get_bool(settings, SETTING_SHOW_ADVANCED);
    ctx->no_signal_mode = (gint)obs_data_get_int(settings, SETTING_NO_SIGNAL_MODE);
    ctx->no_signal_mode = CLAMP(ctx->no_signal_mode, NO_SIGNAL_TRANSPARENT,
                                NO_SIGNAL_NO_SIGNAL);
    ctx->state = (ctx->url && *ctx->url) ? LLRTSP_STATE_CONNECTING
                                         : LLRTSP_STATE_WAITING;

    obs_source_set_audio_active(source, ctx->enable_audio);

    ctx->worker = g_thread_new("llrtsp-worker", pipeline_worker, ctx);
    if (!ctx->worker) {
        blog(LOG_ERROR, "[low-latency-rtsp] Failed to create pipeline worker thread");
        g_free(ctx->url);
        g_mutex_clear(&ctx->stats_mutex);
        g_mutex_clear(&ctx->settings_mutex);
        bfree(ctx);
        return NULL;
    }

    return ctx;
}

static void llrtsp_destroy(void *data)
{
    struct llrtsp_source *ctx = data;
    if (!ctx)
        return;

    g_atomic_int_set(&ctx->stop_requested, 1);
    g_atomic_int_set(&ctx->restart_requested, 1);

    if (ctx->worker)
        g_thread_join(ctx->worker);

    g_free(ctx->url);
    g_mutex_clear(&ctx->stats_mutex);
    g_mutex_clear(&ctx->settings_mutex);
    bfree(ctx);
}

static void llrtsp_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, SETTING_URL, "");
    obs_data_set_default_int(settings, SETTING_PRESET, PRESET_LOW_LATENCY);
    obs_data_set_default_int(settings, SETTING_LATENCY_MS, 0);
    obs_data_set_default_int(settings, SETTING_MAX_BUFFERS, 1);
    obs_data_set_default_int(settings, SETTING_RECONNECT_MS, 1000);
    obs_data_set_default_bool(settings, SETTING_TCP_TIMESTAMP, true);
    obs_data_set_default_bool(settings, SETTING_PREFER_HW_DECODE, true);
    obs_data_set_default_bool(settings, SETTING_ENABLE_AUDIO, false);
    obs_data_set_default_int(settings, SETTING_AUDIO_MODE, AUDIO_MODE_LOW_LATENCY);
    obs_data_set_default_int(settings, SETTING_AUDIO_DELAY_MS, 0);
    obs_data_set_default_bool(settings, SETTING_SHOW_ADVANCED, false);
    obs_data_set_default_int(settings, SETTING_NO_SIGNAL_MODE,
                             NO_SIGNAL_TRANSPARENT);
}

static obs_properties_t *llrtsp_properties(void *data)
{
    struct llrtsp_source *ctx = data;
    obs_properties_t *props = obs_properties_create();

    obs_property_t *info = obs_properties_add_text(
        props, PROP_INFO, obs_module_text("Info"), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(info, true);

    /*
     * A brand-new source starts with the URL visible so the user can paste and
     * correct it (especially useful for UniFi Protect's RTSPS conversion).
     * Once a URL has been saved, future Properties sessions use the normal
     * password-style masked field again.
     */
    gboolean has_saved_url = FALSE;
    if (ctx) {
        g_mutex_lock(&ctx->settings_mutex);
        has_saved_url = ctx->url && *ctx->url;
        g_mutex_unlock(&ctx->settings_mutex);
    }

    obs_property_t *url = obs_properties_add_text(
        props, SETTING_URL, obs_module_text("URL"),
        has_saved_url ? OBS_TEXT_PASSWORD : OBS_TEXT_DEFAULT);
    obs_property_set_long_description(url, obs_module_text("URLHelp"));

    obs_property_t *preset = obs_properties_add_list(
        props, SETTING_PRESET, obs_module_text("Preset"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(preset, obs_module_text("PresetLowLatency"),
                              PRESET_LOW_LATENCY);
    obs_property_list_add_int(preset, obs_module_text("PresetBalanced"),
                              PRESET_BALANCED);
    obs_property_list_add_int(preset, obs_module_text("PresetStable"),
                              PRESET_STABLE);
    obs_property_list_add_int(preset, obs_module_text("PresetCustom"),
                              PRESET_CUSTOM);
    obs_property_set_modified_callback2(preset, properties_modified, ctx);

    obs_property_t *no_signal = obs_properties_add_list(
        props, SETTING_NO_SIGNAL_MODE, obs_module_text("NoSignalDisplay"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(no_signal, obs_module_text("NoSignalTransparent"),
                              NO_SIGNAL_TRANSPARENT);
    obs_property_list_add_int(no_signal, obs_module_text("NoSignalBlack"),
                              NO_SIGNAL_BLACK);
    obs_property_list_add_int(no_signal, obs_module_text("NoSignalReconnecting"),
                              NO_SIGNAL_RECONNECTING);
    obs_property_list_add_int(no_signal, obs_module_text("NoSignalText"),
                              NO_SIGNAL_NO_SIGNAL);
    obs_property_set_long_description(no_signal,
                                      obs_module_text("NoSignalDisplayHelp"));

    obs_property_t *hardware = obs_properties_add_bool(
        props, SETTING_PREFER_HW_DECODE,
        obs_module_text("PreferHardwareDecode"));
    obs_property_set_long_description(
        hardware, obs_module_text("PreferHardwareDecodeHelp"));

    obs_property_t *enable_audio = obs_properties_add_bool(
        props, SETTING_ENABLE_AUDIO, obs_module_text("EnableAudio"));
    obs_property_set_long_description(enable_audio,
                                      obs_module_text("EnableAudioHelp"));
    obs_property_set_modified_callback2(enable_audio, properties_modified, ctx);

    obs_property_t *audio_mode = obs_properties_add_list(
        props, SETTING_AUDIO_MODE, obs_module_text("AudioMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(audio_mode, obs_module_text("AudioModeLowLatency"),
                              AUDIO_MODE_LOW_LATENCY);
    obs_property_list_add_int(audio_mode, obs_module_text("AudioModeSynchronized"),
                              AUDIO_MODE_SYNCHRONIZED);
    obs_property_set_long_description(audio_mode,
                                      obs_module_text("AudioModeHelp"));

    obs_property_t *audio_delay = obs_properties_add_int(
        props, SETTING_AUDIO_DELAY_MS, obs_module_text("AudioDelay"),
        -500, 2000, 10);
    obs_property_int_set_suffix(audio_delay, " ms");
    obs_property_set_long_description(audio_delay,
                                      obs_module_text("AudioDelayHelp"));

    obs_property_t *advanced = obs_properties_add_bool(
        props, SETTING_SHOW_ADVANCED, obs_module_text("ShowAdvanced"));
    obs_property_set_long_description(advanced, obs_module_text("ShowAdvancedHelp"));
    obs_property_set_modified_callback2(advanced, properties_modified, ctx);

    obs_property_t *latency = obs_properties_add_int(
        props, SETTING_LATENCY_MS, obs_module_text("Latency"), 0, 1000, 1);
    obs_property_int_set_suffix(latency, " ms");

    obs_property_t *buffers = obs_properties_add_int(
        props, SETTING_MAX_BUFFERS, obs_module_text("MaxBuffers"), 1, 16, 1);
    obs_property_set_long_description(buffers, obs_module_text("MaxBuffersHelp"));

    obs_property_t *reconnect = obs_properties_add_int(
        props, SETTING_RECONNECT_MS, obs_module_text("Reconnect"),
        100, RECONNECT_BACKOFF_MAX_MS, 100);
    obs_property_int_set_suffix(reconnect, " ms");
    obs_property_set_long_description(reconnect,
                                      obs_module_text("ReconnectHelp"));

    obs_property_t *tcp = obs_properties_add_bool(
        props, SETTING_TCP_TIMESTAMP, obs_module_text("TcpTimestamp"));
    obs_property_set_long_description(tcp, obs_module_text("TcpTimestampHelp"));

    obs_property_t *version = obs_properties_add_text(
        props, PROP_VERSION, "Low Latency RTSP v" LLRTSP_VERSION,
        OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(version, true);

    gint current_preset = PRESET_LOW_LATENCY;
    gboolean current_advanced = FALSE;
    gboolean current_audio = FALSE;
    if (ctx) {
        g_mutex_lock(&ctx->settings_mutex);
        current_preset = ctx->preset;
        current_advanced = ctx->show_advanced;
        current_audio = ctx->enable_audio;
        g_mutex_unlock(&ctx->settings_mutex);
    }
    obs_property_t *status = obs_properties_add_text(
        props, PROP_STATUS, "", OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(status, true);
    update_status_property(props, ctx);

    obs_properties_add_button2(props, PROP_RECONNECT_NOW,
                               obs_module_text("ReconnectNow"),
                               reconnect_now_clicked, ctx);
    obs_properties_add_button2(props, PROP_UPDATE_PLUGIN,
                               obs_module_text("UpdatePlugin"),
                               update_plugin_clicked, ctx);
    obs_properties_add_button2(props, PROP_COPY_DIAGNOSTICS,
                               obs_module_text("CopyDiagnostics"),
                               copy_diagnostics_clicked, ctx);
    obs_properties_add_button2(props, PROP_REFRESH_STATUS,
                               obs_module_text("RefreshStatus"),
                               refresh_status_clicked, ctx);
    obs_properties_add_button2(props, PROP_RESET_STATS,
                               obs_module_text("ResetStats"),
                               reset_stats_clicked, ctx);

    /*
     * Run visibility after every property/button has been created so the
     * initial Properties view matches the saved Advanced setting.
     */
    update_property_visibility(props, current_preset, current_advanced,
                               current_audio);

    return props;
}

struct obs_source_info low_latency_rtsp_source_info = {
    .id = "low_latency_rtsp_gstreamer",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
    .get_name = llrtsp_get_name,
    .create = llrtsp_create,
    .destroy = llrtsp_destroy,
    .update = llrtsp_update,
    .get_defaults = llrtsp_defaults,
    .get_properties = llrtsp_properties,
    .video_tick = llrtsp_video_tick,
    .icon_type = OBS_ICON_TYPE_MEDIA,
};
