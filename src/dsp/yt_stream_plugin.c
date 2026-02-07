#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plugin_api_v1.h"

#define RING_SAMPLES (MOVE_SAMPLE_RATE * 2 * 2) /* 2s stereo */
#define RESTART_RETRY_BLOCKS 64                /* ~186ms at 128f blocks */

static const host_api_v1_t *g_host = NULL;

typedef struct {
    char module_dir[512];
    char stream_url[1024];
    char error_msg[256];

    FILE *pipe;
    int pipe_fd;
    bool stream_eof;
    int restart_countdown;

    int16_t ring[RING_SAMPLES];
    size_t read_pos;
    size_t write_pos;
    size_t count;

    float gain;
} yt_instance_t;

static void yt_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[384];
        snprintf(buf, sizeof(buf), "[yt] %s", msg);
        g_host->log(buf);
    }
}

static void set_error(yt_instance_t *inst, const char *msg) {
    if (!inst) return;
    snprintf(inst->error_msg, sizeof(inst->error_msg), "%s", msg ? msg : "unknown error");
    yt_log(inst->error_msg);
}

static void clear_error(yt_instance_t *inst) {
    if (!inst) return;
    inst->error_msg[0] = '\0';
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    char needle[128];
    const char *p;
    const char *q;
    size_t n;

    if (!json || !key || !out || out_len == 0) return -1;
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;

    q = strchr(p, '"');
    if (!q) return -1;

    n = (size_t)(q - p);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static void ring_push(yt_instance_t *inst, const int16_t *samples, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (inst->count == RING_SAMPLES) {
            inst->read_pos = (inst->read_pos + 1) % RING_SAMPLES;
            inst->count--;
        }
        inst->ring[inst->write_pos] = samples[i];
        inst->write_pos = (inst->write_pos + 1) % RING_SAMPLES;
        inst->count++;
    }
}

static size_t ring_pop(yt_instance_t *inst, int16_t *out, size_t n) {
    size_t i = 0;
    while (i < n && inst->count > 0) {
        out[i] = inst->ring[inst->read_pos];
        inst->read_pos = (inst->read_pos + 1) % RING_SAMPLES;
        inst->count--;
        i++;
    }
    return i;
}

static void stop_stream(yt_instance_t *inst) {
    if (!inst || !inst->pipe) return;
    pclose(inst->pipe); /* reap child process if it exited */
    inst->pipe = NULL;
    inst->pipe_fd = -1;
}

static int start_stream(yt_instance_t *inst) {
    char cmd[4096];

    stop_stream(inst);

    snprintf(cmd, sizeof(cmd),
        "/bin/sh -lc '"
        "U=\"$(\"%s/bin/yt-dlp\" --no-playlist "
        "--extractor-args \"youtube:player_skip=js\" "
        "-f \"ba[protocol^=http][protocol!*=dash]/ba\" -g \"%s\")\" && "
        "exec \"%s/bin/ffmpeg\" -hide_banner -loglevel error "
        "-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 2 "
        "-i \"$U\" -vn -sn -dn -f s16le -ac 2 -ar 44100 pipe:1'",
        inst->module_dir, inst->stream_url, inst->module_dir);

    inst->pipe = popen(cmd, "r");
    if (!inst->pipe) {
        set_error(inst, "failed to launch yt-dlp/ffmpeg pipeline");
        return -1;
    }

    inst->pipe_fd = fileno(inst->pipe);
    if (inst->pipe_fd < 0) {
        set_error(inst, "failed to get pipeline fd");
        pclose(inst->pipe);
        inst->pipe = NULL;
        return -1;
    }

    if (fcntl(inst->pipe_fd, F_SETFL, fcntl(inst->pipe_fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        set_error(inst, "failed to set non-blocking mode");
        pclose(inst->pipe);
        inst->pipe = NULL;
        return -1;
    }

    clear_error(inst);
    inst->stream_eof = false;
    inst->restart_countdown = 0;
    yt_log("stream pipeline started");
    return 0;
}

static void pump_pipe(yt_instance_t *inst) {
    uint8_t buf[4096];
    int16_t samples[2048];

    while (inst->pipe && !inst->stream_eof) {
        ssize_t n = read(inst->pipe_fd, buf, sizeof(buf));
        if (n > 0) {
            size_t sample_count = (size_t)n / sizeof(int16_t);
            memcpy(samples, buf, sample_count * sizeof(int16_t));
            ring_push(inst, samples, sample_count);
            if (sample_count < (sizeof(buf) / sizeof(int16_t))) {
                break;
            }
            continue;
        }
        if (n == 0) {
            inst->stream_eof = true;
            set_error(inst, "stream ended");
            stop_stream(inst);
            inst->restart_countdown = RESTART_RETRY_BLOCKS;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        inst->stream_eof = true;
        set_error(inst, "stream read error");
        stop_stream(inst);
        inst->restart_countdown = RESTART_RETRY_BLOCKS;
        break;
    }
}

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    yt_instance_t *inst;

    inst = calloc(1, sizeof(*inst));
    if (!inst) return NULL;

    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir ? module_dir : ".");
    snprintf(inst->stream_url, sizeof(inst->stream_url), "https://www.youtube.com/watch?v=xvFZjo5PgG0");
    inst->gain = 1.0f;

    if (json_defaults) {
        char url_buf[1024];
        if (json_get_string(json_defaults, "test_url", url_buf, sizeof(url_buf)) == 0) {
            snprintf(inst->stream_url, sizeof(inst->stream_url), "%s", url_buf);
        }
    }

    if (start_stream(inst) != 0) {
        inst->restart_countdown = RESTART_RETRY_BLOCKS;
    }

    return inst;
}

static void v2_destroy_instance(void *instance) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    if (!inst) return;

    stop_stream(inst);
    free(inst);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance;
    (void)msg;
    (void)len;
    (void)source;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "gain") == 0) {
        float g = (float)atof(val);
        if (g < 0.0f) g = 0.0f;
        if (g > 2.0f) g = 2.0f;
        inst->gain = g;
        return;
    }
    if (strcmp(key, "stream_url") == 0 && val[0] != '\0') {
        snprintf(inst->stream_url, sizeof(inst->stream_url), "%s", val);
        inst->stream_eof = false;
        if (start_stream(inst) != 0) {
            inst->restart_countdown = RESTART_RETRY_BLOCKS;
        }
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    if (!key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "gain") == 0) {
        return snprintf(buf, (size_t)buf_len, "%.2f", inst ? inst->gain : 1.0f);
    }
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, (size_t)buf_len, "YT Stream");
    }
    if (strcmp(key, "stream_url") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", inst ? inst->stream_url : "");
    }
    if (strcmp(key, "stream_status") == 0) {
        if (!inst) return snprintf(buf, (size_t)buf_len, "stopped");
        if (!inst->pipe && inst->restart_countdown > 0) {
            return snprintf(buf, (size_t)buf_len, "restarting");
        }
        if (!inst->pipe) return snprintf(buf, (size_t)buf_len, "stopped");
        if (inst->stream_eof) return snprintf(buf, (size_t)buf_len, "eof");
        return snprintf(buf, (size_t)buf_len, "streaming");
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hier = "{\"modes\":null,\"levels\":{\"root\":{\"children\":null,\"knobs\":[\"gain\"],\"params\":[\"gain\"]}}}";
        return snprintf(buf, (size_t)buf_len, "%s", hier);
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    if (!inst || !inst->error_msg[0]) return 0;
    return snprintf(buf, (size_t)buf_len, "%s", inst->error_msg);
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    size_t needed;
    size_t got;
    size_t i;

    if (!out_interleaved_lr || frames <= 0) return;

    needed = (size_t)frames * 2;
    memset(out_interleaved_lr, 0, needed * sizeof(int16_t));

    if (!inst) return;

    if (!inst->pipe) {
        if (inst->restart_countdown > 0) {
            inst->restart_countdown--;
        } else if (start_stream(inst) != 0) {
            inst->restart_countdown = RESTART_RETRY_BLOCKS;
        }
    }

    pump_pipe(inst);
    got = ring_pop(inst, out_interleaved_lr, needed);

    if (inst->gain != 1.0f) {
        for (i = 0; i < got; i++) {
            float s = out_interleaved_lr[i] * inst->gain;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            out_interleaved_lr[i] = (int16_t)s;
        }
    }
}

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    yt_log("yt stream plugin v2 initialized");
    return &g_plugin_api_v2;
}
