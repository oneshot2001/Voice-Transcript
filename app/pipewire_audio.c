/*
 * pipewire_audio.c - PipeWire audio implementation for ACAP
 * Simplified version based on SSW Audio API
 * Copyright (C) Axis Communications AB
 */

#include "pipewire_audio.h"
#include <syslog.h>
#include <string.h>
#include <errno.h>

#ifndef APP_PACKAGE
#define APP_PACKAGE "base"
#endif

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

#define SAMPLE_RATE_MIN     8000
#define DEFAULT_INPUT_NODE  "AudioDevice0Input0"
#define DEFAULT_OUTPUT_NODE "AudioDevice0Output0"

#define PW_AUDIO_ERR_QUARK (pw_audio_err_quark())

G_LOCK_DEFINE(audio);

typedef enum {
    PW_AUDIO_GENERIC_ERROR = 0,
    PW_AUDIO_SETUP_ERROR,
    PW_AUDIO_PARAMETER_ERROR,
    PW_AUDIO_STATE_ERROR,
    PW_AUDIO_STREAM_ERROR
} PWAudioErrorCode;

typedef struct _PipeWireSource {
    GSource base;
    struct pw_loop *loop;
    gpointer userdata;
} PipeWireSource;

struct stream_data {
    struct spa_list link;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    uint32_t node_id;
    char node_name[64];
    struct spa_audio_info info;
    PWAudio *pa;
};

struct _PWAudio {
    gboolean node_detected;
    PWAudioStreamType type;
    const gchar *wanted_node_name;
    enum spa_audio_format format;
    guint32 samplerate;
    PWAudioChannel channels;
    struct pw_loop *pwloop;
    GSource *gsource;  /* GLib source wrapping PipeWire loop */
    struct pw_core *core;
    struct pw_context *context;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct spa_list streams;
    PWAudioOnError on_error;
    gpointer on_error_userdata;
    PWAudioOnBuffer on_buffer;
    gpointer on_buffer_userdata;
    gboolean debug;
};

static GMainContext *lib_g_main_context = NULL;

static GQuark
pw_audio_err_quark(void)
{
    return g_quark_from_string("PWAudioError");
}

/* Forward declarations */
static void on_process(void *data);
static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param);
static void on_state_changed(void *data,
                             enum pw_stream_state old,
                             enum pw_stream_state state,
                             const char *error);
static gboolean pwloop_source_dispatch(GSource *source,
                                       GSourceFunc callback,
                                       gpointer userdata);

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
    .state_changed = on_state_changed
};

static GSourceFuncs source_funcs = {
    .dispatch = pwloop_source_dispatch,
};

static gboolean
pwloop_source_dispatch(GSource *source,
                       G_GNUC_UNUSED GSourceFunc callback,
                       G_GNUC_UNUSED gpointer userdata)
{
    PipeWireSource *s = (PipeWireSource *) source;
    PWAudio *pa = (PWAudio *) s->userdata;
    int result;

    g_assert(pa != NULL);

    result = pw_loop_iterate(s->loop, 0);

    if (result < 0) {
        if (pa->on_error) {
            GError *err = g_error_new(PW_AUDIO_ERR_QUARK,
                                     PW_AUDIO_STATE_ERROR,
                                     "pipewire_loop_iterate failed: %s",
                                     spa_strerror(result));
            G_LOCK(audio);
            pa->on_error(err, pa->on_error_userdata);
            G_UNLOCK(audio);
            g_error_free(err);
        }
    }

    return TRUE;
}

static void
on_process(void *data)
{
    struct stream_data *stream_data = data;
    PWAudio *pa = stream_data->pa;
    struct pw_buffer *b = NULL;

    g_assert(g_main_context_is_owner(lib_g_main_context));

    b = pw_stream_dequeue_buffer(stream_data->stream);

    if (b) {
        struct spa_buffer *spa_buf = b->buffer;

        if (pa->on_buffer) {
            G_LOCK(audio);
            if (pa->type == PW_AUDIO_CAPTURE_STREAM) {
                pa->on_buffer(spa_buf, (guint64) b->time, pa->on_buffer_userdata);
            } else { /* PW_AUDIO_PLAYBACK_STREAM */
                pa->on_buffer(spa_buf, (guint64) b->requested, pa->on_buffer_userdata);
            }
            G_UNLOCK(audio);
        }

        pw_stream_queue_buffer(stream_data->stream, b);
        return;
    }

    /* Error: out of buffers */
    if (pa->on_error) {
        GError *err = g_error_new(PW_AUDIO_ERR_QUARK,
                                 PW_AUDIO_STREAM_ERROR,
                                 "Out of buffers from %s: %s",
                                 stream_data->node_name,
                                 strerror(errno));
        G_LOCK(audio);
        pa->on_error(err, pa->on_error_userdata);
        G_UNLOCK(audio);
        g_error_free(err);
    }
}

static void
on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct stream_data *stream_data = data;
    PWAudio *pa = stream_data->pa;
    int res;

    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }

    res = spa_format_parse(param,
                          &stream_data->info.media_type,
                          &stream_data->info.media_subtype);

    if (res < 0) {
        if (pa->on_error) {
            GError *err = g_error_new(PW_AUDIO_ERR_QUARK,
                                     PW_AUDIO_PARAMETER_ERROR,
                                     "Failed to parse format from %s: %s",
                                     stream_data->node_name,
                                     strerror(-res));
            G_LOCK(audio);
            pa->on_error(err, pa->on_error_userdata);
            G_UNLOCK(audio);
            g_error_free(err);
        }
        return;
    }

    if (stream_data->info.media_type != SPA_MEDIA_TYPE_audio ||
        stream_data->info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        if (pa->on_error) {
            GError *err = g_error_new(PW_AUDIO_ERR_QUARK,
                                     PW_AUDIO_PARAMETER_ERROR,
                                     "Format from %s is not raw audio",
                                     stream_data->node_name);
            G_LOCK(audio);
            pa->on_error(err, pa->on_error_userdata);
            G_UNLOCK(audio);
            g_error_free(err);
        }
        return;
    }

    spa_format_audio_raw_parse(param, &stream_data->info.info.raw);

    if (pa->debug) {
        LOG("Audio stream from node %s, %d channel(s), rate %d\n",
            stream_data->node_name,
            stream_data->info.info.raw.channels,
            stream_data->info.info.raw.rate);
    }
}

static void
on_state_changed(void *data,
                 enum pw_stream_state old,
                 enum pw_stream_state state,
                 const char *error)
{
    struct stream_data *stream_data = data;
    PWAudio *pa = stream_data->pa;

    if (pa->debug) {
        LOG("Stream state changed %s -> %s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state));
    }

    if (state == PW_STREAM_STATE_ERROR) {
        if (pa->on_error) {
            GError *err = g_error_new(PW_AUDIO_ERR_QUARK,
                                     PW_AUDIO_STATE_ERROR,
                                     "Stream from %s got error: %s",
                                     stream_data->node_name,
                                     error);
            G_LOCK(audio);
            pa->on_error(err, pa->on_error_userdata);
            G_UNLOCK(audio);
            g_error_free(err);
        }
    }
}

static void
pw_registry_event_global(void *userdata,
                         uint32_t id,
                         G_GNUC_UNUSED uint32_t permissions,
                         const char *type,
                         G_GNUC_UNUSED uint32_t version,
                         const struct spa_dict *props)
{
    PWAudio *pa = (PWAudio *) userdata;
    const char *errmsg = NULL;

    LOG_TRACE("pw_registry_event_global: type=%s, id=%u\n", type, id);

    // Log ALL nodes, not just when searching
    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

        // ALWAYS log all nodes to see what's available
        LOG("pw_registry_event_global: Found Node: media_class=%s, name=%s (looking for %s, detected=%d)\n",
            media_class ? media_class : "NULL",
            name ? name : "NULL",
            pa->wanted_node_name,
            pa->node_detected);

        // Only try to connect if we haven't found our node yet
        if (pa->node_detected) {
            return;  // Already connected to a node
        }

        if (g_strcmp0(name, pa->wanted_node_name) != 0) {
            return;  // Name doesn't match what we're looking for
        }

        uint8_t buf[1024];
        struct pw_properties *stream_props;
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod *params[1];
        struct stream_data *stream_data;
        int res;
        enum pw_direction direction;

        pa->node_detected = TRUE;

        LOG("*** FOUND AUDIO NODE! media_class=%s, name=%s, id=%u\n",
            media_class ? media_class : "NULL", name, id);

        if (pa->debug) {
            LOG("Detected %s node %s with id %u\n", media_class, name, id);
        }

        /* Set stream direction based on type */
        if (pa->type == PW_AUDIO_CAPTURE_STREAM) {
            direction = PW_DIRECTION_INPUT;
            stream_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                            PW_KEY_MEDIA_CATEGORY, "Capture",
                                            PW_KEY_TARGET_OBJECT, name,
                                            NULL);

            /* Monitor output node for capture */
            if (media_class != NULL && strcmp(media_class, "Audio/Sink") == 0) {
                pw_properties_set(stream_props, PW_KEY_STREAM_CAPTURE_SINK, "true");
            }
        } else {
            direction = PW_DIRECTION_OUTPUT;
            stream_props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                            PW_KEY_MEDIA_CATEGORY, "Playback",
                                            PW_KEY_TARGET_OBJECT, name,
                                            NULL);
        }

        if (stream_props == NULL) {
            errmsg = "Could not create stream properties";
            goto error;
        }

        /* Create stream data */
        stream_data = calloc(1, sizeof(struct stream_data));
        if (!stream_data) {
            pw_properties_free(stream_props);
            errmsg = "Could not allocate stream data";
            goto error;
        }

        stream_data->pa = pa;
        stream_data->node_id = id;
        strncpy(stream_data->node_name, name, sizeof(stream_data->node_name) - 1);
        stream_data->node_name[sizeof(stream_data->node_name) - 1] = '\0';

        stream_data->stream = pw_stream_new(pa->core,
                                           pa->type == PW_AUDIO_CAPTURE_STREAM ?
                                           "Audio capture" : "Audio playback",
                                           stream_props);

        if (stream_data->stream == NULL) {
            free(stream_data);
            errmsg = "Could not create stream";
            goto error;
        }

        pw_stream_add_listener(stream_data->stream,
                              &stream_data->stream_listener,
                              &stream_events,
                              stream_data);

        /* Build format parameters */
        params[0] = spa_format_audio_raw_build(&builder,
                                              SPA_PARAM_EnumFormat,
                                              &SPA_AUDIO_INFO_RAW_INIT(
                                                  .format = pa->format,
                                                  .rate = pa->samplerate,
                                                  .channels = pa->channels));

        /* Connect stream */
        res = pw_stream_connect(stream_data->stream,
                               direction,
                               PW_ID_ANY,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               params,
                               1);
        if (res < 0) {
            pw_stream_destroy(stream_data->stream);
            free(stream_data);
            errmsg = "Could not connect stream";
            goto error;
        }

        spa_list_append(&pa->streams, &stream_data->link);
    }

    return;

error:
    if (pa->on_error) {
        GError *err = g_error_new(PW_AUDIO_ERR_QUARK,
                                 PW_AUDIO_SETUP_ERROR,
                                 "%s",
                                 errmsg);
        G_LOCK(audio);
        pa->on_error(err, pa->on_error_userdata);
        G_UNLOCK(audio);
        g_error_free(err);
    }
}

static void
pw_registry_event_global_remove(void *data, uint32_t id)
{
    PWAudio *pa = data;
    struct stream_data *stream_data;

    LOG_WARN("Removed pipewire object with id %u\n", id);

    spa_list_for_each(stream_data, &pa->streams, link) {
        if (stream_data->node_id == id) {
            if (pa->debug) {
                LOG("Destroy stream from %s\n", stream_data->node_name);
            }
            spa_hook_remove(&stream_data->stream_listener);
            pw_stream_destroy(stream_data->stream);
            spa_list_remove(&stream_data->link);
            free(stream_data);
            break;
        }
    }
}

static const struct pw_registry_events pw_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = pw_registry_event_global,
    .global_remove = pw_registry_event_global_remove,
};

static PWAudio *
audio_stream_start(PWAudioStreamType type,
                  enum spa_audio_format format,
                  guint32 samplerate,
                  PWAudioChannel channels,
                  PWAudioOnError on_error,
                  gpointer userdata)
{
    PipeWireSource *source = NULL;
    PWAudio *pa = g_malloc0(sizeof(*pa));

    LOG_TRACE("audio_stream_start: Starting %s stream\n",
              type == PW_AUDIO_CAPTURE_STREAM ? "CAPTURE" : "PLAYBACK");

    /* Use global-default main context
     * IMPORTANT: lib_g_main_context must be set from the main thread
     * BEFORE this is called from any other thread (e.g., FastCGI thread) */
    G_LOCK(audio);
    if (lib_g_main_context == NULL) {
        lib_g_main_context = g_main_context_default();
        LOG_TRACE("audio_stream_start: Using default main context\n");
    }
    g_main_context_ref(lib_g_main_context);
    G_UNLOCK(audio);

    /* Initialize PipeWire (can be called multiple times) */
    LOG_TRACE("audio_stream_start: Calling pw_init()\n");
    pw_init(NULL, NULL);

    pa->type = type;
    pa->format = format;
    pa->samplerate = samplerate;
    pa->channels = channels;
    pa->on_error = on_error;
    pa->on_error_userdata = userdata;

    if (type == PW_AUDIO_CAPTURE_STREAM) {
        pa->wanted_node_name = DEFAULT_INPUT_NODE;
    } else {
        pa->wanted_node_name = DEFAULT_OUTPUT_NODE;
    }

    LOG_TRACE("audio_stream_start: Looking for node: %s\n", pa->wanted_node_name);

    LOG_TRACE("audio_stream_start: Creating PipeWire loop\n");
    pa->pwloop = pw_loop_new(NULL);
    if (pa->pwloop == NULL) {
        LOG_WARN("audio_stream_start: FAILED to create pw_loop\n");
        g_free(pa);
        G_LOCK(audio);
        g_main_context_unref(lib_g_main_context);
        G_UNLOCK(audio);
        return NULL;
    }
    LOG_TRACE("audio_stream_start: PipeWire loop created successfully\n");

    /* Wrap PipeWire loop in GMainLoop */
    LOG_TRACE("audio_stream_start: Creating GSource wrapper\n");
    source = (PipeWireSource *) g_source_new(&source_funcs, sizeof(PipeWireSource));
    source->loop = pa->pwloop;
    source->userdata = pa;

    g_source_add_unix_fd(&source->base,
                        pw_loop_get_fd(pa->pwloop),
                        G_IO_IN | G_IO_ERR);
    g_source_attach(&source->base, lib_g_main_context);

    /* Store GSource pointer so we can destroy it later in pw_audio_stop() */
    pa->gsource = &source->base;

    g_source_unref(&source->base);  /* Unref but keep attached to context */
    LOG_TRACE("audio_stream_start: GSource attached to main context\n");

    LOG_TRACE("audio_stream_start: Creating PipeWire context\n");
    pa->context = pw_context_new(pa->pwloop, NULL, 0);
    if (pa->context == NULL) {
        LOG_WARN("audio_stream_start: FAILED to create pw_context\n");
        g_source_destroy(pa->gsource);
        pw_loop_destroy(pa->pwloop);
        g_free(pa);
        G_LOCK(audio);
        g_main_context_unref(lib_g_main_context);
        G_UNLOCK(audio);
        return NULL;
    }
    LOG_TRACE("audio_stream_start: PipeWire context created\n");

    LOG_TRACE("audio_stream_start: Connecting to PipeWire core\n");
    pa->core = pw_context_connect(pa->context, NULL, 0);
    if (pa->core == NULL) {
        LOG_WARN("audio_stream_start: FAILED to connect to pw_core\n");
        g_source_destroy(pa->gsource);
        pw_context_destroy(pa->context);
        pw_loop_destroy(pa->pwloop);
        g_free(pa);
        G_LOCK(audio);
        g_main_context_unref(lib_g_main_context);
        G_UNLOCK(audio);
        return NULL;
    }
    LOG_TRACE("audio_stream_start: Connected to PipeWire core\n");

    LOG_TRACE("audio_stream_start: Getting registry\n");
    pa->registry = pw_core_get_registry(pa->core, PW_VERSION_REGISTRY, 0);
    if (pa->registry == NULL) {
        LOG_WARN("audio_stream_start: FAILED to get registry\n");
        g_source_destroy(pa->gsource);
        pw_core_disconnect(pa->core);
        pw_context_destroy(pa->context);
        pw_loop_destroy(pa->pwloop);
        g_free(pa);
        G_LOCK(audio);
        g_main_context_unref(lib_g_main_context);
        G_UNLOCK(audio);
        return NULL;
    }
    LOG_TRACE("audio_stream_start: Registry obtained\n");

    spa_zero(pa->registry_listener);

    LOG_TRACE("audio_stream_start: Adding registry listener\n");
    pw_registry_add_listener(pa->registry,
                            &pa->registry_listener,
                            &pw_registry_events,
                            pa);

    spa_list_init(&pa->streams);

    /* NOTE: We do NOT call pw_loop_enter() here because the PipeWire loop
     * is already integrated with GLib main loop via GSource (see above).
     * The GLib main loop will automatically dispatch PipeWire events via
     * pwloop_source_dispatch(). Calling pw_loop_enter() would block the
     * main loop and prevent other events (signals, idle callbacks) from
     * being processed. */

    LOG_TRACE("audio_stream_start: Success! Returning handle\n");
    return pa;
}

/* Public API */

void
pw_audio_init(GMainContext *main_context)
{
    G_LOCK(audio);
    if (lib_g_main_context == NULL) {
        if (main_context) {
            lib_g_main_context = g_main_context_ref(main_context);
            LOG_TRACE("pw_audio_init: Set main context %p from caller\n", (void*)lib_g_main_context);
        } else {
            lib_g_main_context = g_main_context_default();
            LOG_TRACE("pw_audio_init: Using default main context\n");
        }
    }
    G_UNLOCK(audio);
}

PWAudio *
pw_audio_capture_start(enum spa_audio_format format,
                      guint32 samplerate,
                      PWAudioChannel channels,
                      PWAudioOnError on_error,
                      gpointer userdata)
{
    if (samplerate < SAMPLE_RATE_MIN) {
        LOG_WARN("Sample rate too low: %u\n", samplerate);
        return NULL;
    }

    return audio_stream_start(PW_AUDIO_CAPTURE_STREAM,
                             format,
                             samplerate,
                             channels,
                             on_error,
                             userdata);
}

PWAudio *
pw_audio_playback_start(enum spa_audio_format format,
                       guint32 samplerate,
                       PWAudioChannel channels,
                       PWAudioOnError on_error,
                       gpointer userdata)
{
    return audio_stream_start(PW_AUDIO_PLAYBACK_STREAM,
                             format,
                             samplerate,  /* Use specified sample rate, or 0 for device native */
                             channels,
                             on_error,
                             userdata);
}

void
pw_audio_set_on_buffer_cb(PWAudio *stream,
                          PWAudioOnBuffer callback,
                          gpointer userdata)
{
    if (!stream || !callback) {
        return;
    }

    G_LOCK(audio);
    stream->on_buffer = callback;
    stream->on_buffer_userdata = userdata;
    G_UNLOCK(audio);
}

void
pw_audio_stop(PWAudio *pa)
{
    if (!pa) {
        return;
    }

    /* NOTE: We do NOT call pw_loop_leave() because we never called
     * pw_loop_enter(). The loop is managed by GLib main loop. */

    /* CRITICAL: Destroy the GSource FIRST before destroying the PipeWire loop.
     * The GSource is still attached to the main context and polling the loop's
     * file descriptor. If we destroy the loop first, the GSource will try to
     * read from an invalid fd and may block the main loop. */
    if (pa->gsource) {
        LOG_TRACE("pw_audio_stop: Destroying GSource\n");
        g_source_destroy(pa->gsource);
        pa->gsource = NULL;
    }

    pw_proxy_destroy((struct pw_proxy *) pa->registry);

    struct stream_data *stream_data;
    spa_list_consume(stream_data, &pa->streams, link) {
        if (pa->debug) {
            LOG("Destroy stream with target node %s\n", stream_data->node_name);
        }
        spa_hook_remove(&stream_data->stream_listener);
        pw_stream_destroy(stream_data->stream);
        spa_list_remove(&stream_data->link);
        free(stream_data);
    }

    pw_core_disconnect(pa->core);
    pw_context_destroy(pa->context);
    pw_loop_destroy(pa->pwloop);
    pw_deinit();

    g_free(pa);

    G_LOCK(audio);
    if (lib_g_main_context != NULL) {
        g_main_context_unref(lib_g_main_context);
    }
    G_UNLOCK(audio);
}

PWAudioStreamType
pw_audio_stream_get_type(PWAudio *stream)
{
    if (!stream) {
        return PW_AUDIO_NO_STREAM;
    }

    PWAudioStreamType type;
    G_LOCK(audio);
    type = stream->type;
    G_UNLOCK(audio);
    return type;
}

guint32
pw_audio_stream_get_samplerate(PWAudio *stream)
{
    if (!stream) {
        return 0;
    }

    guint32 fs;
    G_LOCK(audio);
    fs = stream->samplerate;
    G_UNLOCK(audio);
    return fs;
}

PWAudioChannel
pw_audio_stream_get_channels(PWAudio *stream)
{
    if (!stream) {
        return PW_AUDIO_NONE;
    }

    PWAudioChannel ch;
    G_LOCK(audio);
    ch = stream->channels;
    G_UNLOCK(audio);
    return ch;
}

void
pw_audio_enable_debug(PWAudio *stream, gboolean enable)
{
    if (!stream) {
        return;
    }

    G_LOCK(audio);
    stream->debug = enable;
    G_UNLOCK(audio);
}
