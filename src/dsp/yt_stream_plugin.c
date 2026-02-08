#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugin_api_v1.h"

#define RING_SECONDS 60
#define RING_SAMPLES (MOVE_SAMPLE_RATE * 2 * RING_SECONDS) /* stereo ring */
#define RESTART_RETRY_BLOCKS 64                 /* ~186ms at 128f blocks */
#define DEBOUNCE_PLAY_PAUSE_MS 220ULL
#define DEBOUNCE_SEEK_MS 140ULL
#define DEBOUNCE_STOP_MS 220ULL
#define DEBOUNCE_RESTART_MS 220ULL

#define SEARCH_MAX_RESULTS 20
#define SEARCH_QUERY_MAX 256
#define SEARCH_ID_MAX 32
#define SEARCH_TEXT_MAX 192
#define SEARCH_URL_MAX 160
#define STREAM_URL_MAX 4096
#define HTTP_HEADER_MAX 384
#define DAEMON_LINE_MAX 4096
#define DAEMON_START_TIMEOUT_MS 12000
#define DAEMON_SEARCH_TIMEOUT_MS 12000
#define DAEMON_RESOLVE_TIMEOUT_MS 12000

static const host_api_v1_t *g_host = NULL;

static void* search_thread_main(void *arg);
static void* resolve_thread_main(void *arg);
static void* warmup_thread_main(void *arg);

typedef struct {
    char id[SEARCH_ID_MAX];
    char title[SEARCH_TEXT_MAX];
    char channel[SEARCH_TEXT_MAX];
    char duration[24];
    char url[SEARCH_URL_MAX];
} search_result_t;

typedef struct {
    char module_dir[512];
    char stream_url[STREAM_URL_MAX];
    char error_msg[256];

    FILE *pipe;
    int pipe_fd;
    bool stream_eof;
    int restart_countdown;
    bool active_stream_resolved;
    bool resolved_fallback_attempted;

    int16_t ring[RING_SAMPLES];
    size_t write_pos;
    uint64_t write_abs;
    uint64_t play_abs;
    uint64_t dropped_samples;
    uint64_t dropped_log_next;
    uint8_t pending_bytes[4];
    uint8_t pending_len;
    size_t prime_needed_samples;
    bool paused;
    size_t played_samples;
    size_t seek_discard_samples;
    int play_pause_step;
    int rewind_15_step;
    int forward_15_step;
    int stop_step;
    int restart_step;
    uint64_t last_play_pause_ms;
    uint64_t last_rewind_ms;
    uint64_t last_forward_ms;
    uint64_t last_stop_ms;
    uint64_t last_restart_ms;
    bool warmup_started;
    pthread_t warmup_thread;
    bool warmup_thread_valid;

    pthread_mutex_t daemon_mutex;
    FILE *daemon_in;
    FILE *daemon_out;
    pid_t daemon_pid;
    bool daemon_ready;

    pthread_mutex_t resolve_mutex;
    pthread_t resolve_thread;
    bool resolve_thread_valid;
    bool resolve_thread_running;
    bool resolve_ready;
    bool resolve_failed;
    char resolved_media_url[STREAM_URL_MAX];
    char resolved_user_agent[HTTP_HEADER_MAX];
    char resolved_referer[HTTP_HEADER_MAX];
    char resolve_error[256];

    float gain;

    pthread_mutex_t search_mutex;
    pthread_t search_thread;
    bool search_thread_valid;
    bool search_thread_running;
    char search_query[SEARCH_QUERY_MAX];
    char queued_search_query[SEARCH_QUERY_MAX];
    bool queued_search_pending;
    char search_status[24];
    char search_error[256];
    uint64_t search_elapsed_ms;
    int search_count;
    search_result_t search_results[SEARCH_MAX_RESULTS];
} yt_instance_t;

static void yt_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[384];
        snprintf(buf, sizeof(buf), "[yt] %s", msg);
        g_host->log(buf);
    }
}

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static void trim_line_end(char *line) {
    size_t len;
    if (!line) return;
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

static int split_tab_fields(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;
    if (!line || !fields || max_fields <= 0) return 0;
    fields[count++] = p;
    while (*p && count < max_fields) {
        if (*p == '\t') {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

static void stop_daemon_locked(yt_instance_t *inst) {
    int status;
    pid_t rc;

    if (!inst) return;

    if (inst->daemon_in) {
        fprintf(inst->daemon_in, "QUIT\n");
        fflush(inst->daemon_in);
        fclose(inst->daemon_in);
        inst->daemon_in = NULL;
    }

    if (inst->daemon_out) {
        fclose(inst->daemon_out);
        inst->daemon_out = NULL;
    }

    if (inst->daemon_pid > 0) {
        rc = waitpid(inst->daemon_pid, &status, WNOHANG);
        if (rc == 0) {
            (void)kill(inst->daemon_pid, SIGTERM);
            usleep(200000);
            rc = waitpid(inst->daemon_pid, &status, WNOHANG);
            if (rc == 0) {
                (void)kill(inst->daemon_pid, SIGKILL);
                (void)waitpid(inst->daemon_pid, &status, 0);
            }
        }
        inst->daemon_pid = -1;
    }

    inst->daemon_ready = false;
}

static int read_daemon_line_locked(yt_instance_t *inst, char *line, size_t line_len, int timeout_ms) {
    struct pollfd pfd;
    int rc;

    if (!inst || !inst->daemon_out || !line || line_len < 2) return -1;
    pfd.fd = fileno(inst->daemon_out);
    pfd.events = POLLIN;
    pfd.revents = 0;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return -1;
    if (!(pfd.revents & POLLIN)) return -1;

    if (!fgets(line, (int)line_len, inst->daemon_out)) {
        return -1;
    }
    trim_line_end(line);
    return 0;
}

static int start_daemon_locked(yt_instance_t *inst, char *err, size_t err_len) {
    int parent_to_child[2];
    int child_to_parent[2];
    pid_t pid;
    char daemon_path[1024];
    char ytdlp_path[1024];
    char line[DAEMON_LINE_MAX];

    if (!inst) return -1;
    if (inst->daemon_ready && inst->daemon_in && inst->daemon_out && inst->daemon_pid > 0) {
        return 0;
    }

    stop_daemon_locked(inst);

    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon pipe failed");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);
        if (err && err_len > 0) snprintf(err, err_len, "daemon fork failed");
        return -1;
    }

    if (pid == 0) {
        snprintf(daemon_path, sizeof(daemon_path), "%s/bin/yt_dlp_daemon.py", inst->module_dir);
        snprintf(ytdlp_path, sizeof(ytdlp_path), "%s/bin/yt-dlp", inst->module_dir);

        dup2(parent_to_child[0], STDIN_FILENO);
        dup2(child_to_parent[1], STDOUT_FILENO);
        close(parent_to_child[0]);
        close(parent_to_child[1]);
        close(child_to_parent[0]);
        close(child_to_parent[1]);
        execlp("python3", "python3", daemon_path, ytdlp_path, (char *)NULL);
        _exit(127);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    inst->daemon_in = fdopen(parent_to_child[1], "w");
    inst->daemon_out = fdopen(child_to_parent[0], "r");
    inst->daemon_pid = pid;
    if (!inst->daemon_in || !inst->daemon_out) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon fdopen failed");
        stop_daemon_locked(inst);
        return -1;
    }

    setvbuf(inst->daemon_in, NULL, _IOLBF, 0);
    setvbuf(inst->daemon_out, NULL, _IOLBF, 0);

    if (read_daemon_line_locked(inst, line, sizeof(line), DAEMON_START_TIMEOUT_MS) != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon startup timeout");
        stop_daemon_locked(inst);
        return -1;
    }

    if (strcmp(line, "READY") != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon startup failed: %s", line);
        stop_daemon_locked(inst);
        return -1;
    }

    inst->daemon_ready = true;
    return 0;
}

static int ensure_daemon_started(yt_instance_t *inst, char *err, size_t err_len) {
    int rc;
    if (!inst) return -1;
    pthread_mutex_lock(&inst->daemon_mutex);
    rc = start_daemon_locked(inst, err, err_len);
    pthread_mutex_unlock(&inst->daemon_mutex);
    return rc;
}

static void* warmup_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char err[256];
    if (!inst) return NULL;
    err[0] = '\0';
    if (ensure_daemon_started(inst, err, sizeof(err)) == 0) {
        yt_log("yt-dlp daemon warmed");
    } else {
        char msg[320];
        snprintf(msg, sizeof(msg), "yt-dlp daemon warmup failed: %s", err[0] ? err : "unknown");
        yt_log(msg);
    }
    return NULL;
}

static void start_warmup_if_needed(yt_instance_t *inst) {
    if (!inst || inst->warmup_started) return;
    inst->warmup_started = true;

    if (pthread_create(&inst->warmup_thread, NULL, warmup_thread_main, inst) == 0) {
        inst->warmup_thread_valid = true;
        yt_log("started yt-dlp daemon warmup thread");
    } else {
        inst->warmup_thread_valid = false;
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

static void set_search_status(yt_instance_t *inst, const char *status, const char *err) {
    if (!inst) return;
    snprintf(inst->search_status, sizeof(inst->search_status), "%s", status ? status : "idle");
    snprintf(inst->search_error, sizeof(inst->search_error), "%s", err ? err : "");
}

static void sanitize_query(const char *in, char *out, size_t out_len) {
    size_t i;
    size_t j = 0;
    bool prev_space = true;

    if (!in || !out || out_len == 0) return;

    for (i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        bool keep = isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == ',' ||
                    c == '!' || c == '?' || c == '+' || c == '/';
        if (!keep) c = ' ';

        if (c == ' ') {
            if (prev_space) continue;
            prev_space = true;
            out[j++] = ' ';
        } else {
            prev_space = false;
            out[j++] = (char)c;
        }
    }

    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';

    if (out[0] == '\0') {
        snprintf(out, out_len, "music");
    }
}

static void sanitize_display_text(char *s) {
    size_t i;
    size_t j = 0;
    bool prev_space = true;

    if (!s) return;

    for (i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        char out_c = (c >= 32 && c <= 126) ? (char)c : ' ';

        if (out_c == ' ') {
            if (prev_space) continue;
            prev_space = true;
            s[j++] = out_c;
        } else {
            prev_space = false;
            s[j++] = out_c;
        }
    }

    while (j > 0 && s[j - 1] == ' ') j--;
    s[j] = '\0';
}

static bool is_allowed_stream_url_char(unsigned char c) {
    if (isalnum(c)) return true;
    return c == ':' || c == '/' || c == '?' || c == '&' || c == '=' || c == '%' ||
           c == '.' || c == '_' || c == '-' || c == '+' || c == '#' || c == '~' ||
           c == ',';
}

static bool host_is_supported(const char *host) {
    size_t len;
    if (!host || host[0] == '\0') return false;
    if (strcmp(host, "youtube.com") == 0 ||
        strcmp(host, "www.youtube.com") == 0 ||
        strcmp(host, "m.youtube.com") == 0 ||
        strcmp(host, "music.youtube.com") == 0 ||
        strcmp(host, "youtu.be") == 0 ||
        strcmp(host, "www.youtu.be") == 0) {
        return true;
    }
    len = strlen(host);
    if (len > 12 && strcmp(host + len - 12, ".youtube.com") == 0) {
        return true;
    }
    return false;
}

static bool sanitize_stream_url(const char *in, char *out, size_t out_len) {
    const char *p;
    const char *host_start;
    const char *host_end;
    size_t host_len;
    char host[128];
    size_t i;
    size_t j = 0;

    if (!in || !out || out_len == 0) return false;
    if (!(strncmp(in, "https://", 8) == 0 || strncmp(in, "http://", 7) == 0)) {
        return false;
    }

    p = strstr(in, "://");
    if (!p) return false;
    host_start = p + 3;
    host_end = host_start;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#') {
        host_end++;
    }
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(host)) return false;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    for (i = 0; host[i] != '\0'; i++) {
        if (host[i] == ':') {
            host[i] = '\0'; /* strip optional :port */
            break;
        }
    }

    if (!host_is_supported(host)) return false;

    for (i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (!is_allowed_stream_url_char(c)) return false;
        if (j + 1 >= out_len) return false;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return j > 0;
}

static bool sanitize_any_http_url(const char *in, char *out, size_t out_len) {
    size_t i;
    size_t j = 0;
    if (!in || !out || out_len == 0) return false;
    if (!(strncmp(in, "https://", 8) == 0 || strncmp(in, "http://", 7) == 0)) {
        return false;
    }
    for (i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (!is_allowed_stream_url_char(c)) return false;
        if (j + 1 >= out_len) return false;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return j > 0;
}

static void sanitize_header_text(const char *in, char *out, size_t out_len) {
    size_t i;
    size_t j = 0;
    if (!out || out_len == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 32 || c > 126) continue;
        if (c == '"' || c == '\'' || c == '\\' || c == '`') continue;
        out[j++] = (char)c;
    }
    out[j] = '\0';
}

static uint64_t ring_oldest_abs(const yt_instance_t *inst) {
    if (!inst) return 0;
    if (inst->write_abs > (uint64_t)RING_SAMPLES) {
        return inst->write_abs - (uint64_t)RING_SAMPLES;
    }
    return 0;
}

static size_t ring_available(const yt_instance_t *inst) {
    uint64_t avail;
    if (!inst) return 0;
    if (inst->write_abs <= inst->play_abs) return 0;
    avail = inst->write_abs - inst->play_abs;
    if (avail > (uint64_t)RING_SAMPLES) avail = (uint64_t)RING_SAMPLES;
    return (size_t)avail;
}

static void ring_push(yt_instance_t *inst, const int16_t *samples, size_t n) {
    size_t i;
    uint64_t oldest;
    for (i = 0; i < n; i++) {
        inst->ring[inst->write_pos] = samples[i];
        inst->write_pos = (inst->write_pos + 1) % RING_SAMPLES;
        inst->write_abs++;
    }

    oldest = ring_oldest_abs(inst);
    if (inst->play_abs < oldest) {
        inst->dropped_samples += (oldest - inst->play_abs);
        inst->play_abs = oldest;
        inst->played_samples = (size_t)inst->play_abs;
    }
}

static size_t ring_pop(yt_instance_t *inst, int16_t *out, size_t n) {
    size_t got;
    size_t i;
    uint64_t abs_pos;

    if (!inst || !out || n == 0) return 0;

    got = ring_available(inst);
    if (got > n) got = n;
    abs_pos = inst->play_abs;

    for (i = 0; i < got; i++) {
        out[i] = inst->ring[(size_t)(abs_pos % (uint64_t)RING_SAMPLES)];
        abs_pos++;
    }

    inst->play_abs = abs_pos;
    inst->played_samples = (size_t)inst->play_abs;
    return got;
}

static void stop_stream(yt_instance_t *inst) {
    if (!inst || !inst->pipe) return;
    pclose(inst->pipe); /* reap child process if it exited */
    inst->pipe = NULL;
    inst->pipe_fd = -1;
}

static void clear_ring(yt_instance_t *inst) {
    if (!inst) return;
    inst->write_pos = 0;
    inst->write_abs = 0;
    inst->play_abs = 0;
    inst->dropped_samples = 0;
    inst->dropped_log_next = (uint64_t)MOVE_SAMPLE_RATE * 2ULL;
    inst->pending_len = 0;
    memset(inst->pending_bytes, 0, sizeof(inst->pending_bytes));
    inst->prime_needed_samples = 0;
    inst->played_samples = 0;
}

static void restart_stream_from_beginning(yt_instance_t *inst, size_t discard_samples) {
    if (!inst) return;
    stop_stream(inst);
    clear_ring(inst);
    clear_error(inst);
    inst->stream_eof = false;
    inst->restart_countdown = 0;
    inst->paused = false;
    inst->played_samples = 0;
    inst->seek_discard_samples = discard_samples;
    inst->active_stream_resolved = false;
    inst->resolved_fallback_attempted = false;
}

static void seek_relative_seconds(yt_instance_t *inst, long delta_sec) {
    int64_t current_samples;
    int64_t target_samples;
    int64_t delta_samples;
    int64_t oldest_samples;
    int64_t newest_samples;

    if (!inst || inst->stream_url[0] == '\0') return;

    current_samples = (int64_t)inst->play_abs;
    delta_samples = (int64_t)delta_sec * (int64_t)MOVE_SAMPLE_RATE * 2LL;
    target_samples = current_samples + delta_samples;

    oldest_samples = (int64_t)ring_oldest_abs(inst);
    newest_samples = (int64_t)inst->write_abs;
    if (target_samples < oldest_samples) target_samples = oldest_samples;
    if (target_samples > newest_samples) target_samples = newest_samples;

    inst->play_abs = (uint64_t)target_samples;
    inst->played_samples = (size_t)inst->play_abs;
}

static void stop_everything(yt_instance_t *inst) {
    if (!inst) return;
    inst->stream_url[0] = '\0';
    inst->stream_eof = false;
    inst->restart_countdown = 0;
    inst->paused = false;
    inst->played_samples = 0;
    inst->seek_discard_samples = 0;
    inst->active_stream_resolved = false;
    inst->resolved_fallback_attempted = false;
    stop_stream(inst);
    clear_ring(inst);
    clear_error(inst);
    pthread_mutex_lock(&inst->resolve_mutex);
    inst->resolve_ready = false;
    inst->resolve_failed = false;
    inst->resolved_media_url[0] = '\0';
    inst->resolved_user_agent[0] = '\0';
    inst->resolved_referer[0] = '\0';
    inst->resolve_error[0] = '\0';
    pthread_mutex_unlock(&inst->resolve_mutex);
}

static int start_stream_legacy(yt_instance_t *inst) {
    char cmd[8192];

    stop_stream(inst);

    snprintf(cmd, sizeof(cmd),
        "/bin/sh -lc '"
        "exec \"%s/bin/yt-dlp\" --no-playlist "
        "--extractor-args \"youtube:player_skip=js\" "
        "-f \"bestaudio[ext=m4a]/bestaudio\" -o - \"%s\" 2>/dev/null | "
        "\"%s/bin/ffmpeg\" -hide_banner -loglevel error "
        "-i pipe:0 -vn -sn -dn "
        "-af \"aresample=%d:async=1:min_hard_comp=0.100:first_pts=0\" "
        "-f s16le -ac 2 -ar %d pipe:1'",
        inst->module_dir, inst->stream_url, inst->module_dir, MOVE_SAMPLE_RATE, MOVE_SAMPLE_RATE);

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
    inst->prime_needed_samples = (size_t)MOVE_SAMPLE_RATE; /* ~0.5s stereo */
    inst->active_stream_resolved = false;
    yt_log("stream pipeline started (legacy)");
    return 0;
}

static int start_stream_resolved(yt_instance_t *inst, const char *media_url) {
    char cmd[8192];
    char clean_url[STREAM_URL_MAX];

    if (!inst || !media_url || media_url[0] == '\0') {
        set_error(inst, "resolved media url missing");
        return -1;
    }

    if (!sanitize_any_http_url(media_url, clean_url, sizeof(clean_url))) {
        set_error(inst, "resolved media url invalid");
        return -1;
    }

    stop_stream(inst);

    snprintf(cmd,
             sizeof(cmd),
             "/bin/sh -lc '"
             "exec \"%s/bin/ffmpeg\" -hide_banner -loglevel error "
             "-i \"%s\" -vn -sn -dn "
             "-af \"aresample=%d:async=1:min_hard_comp=0.100:first_pts=0\" "
             "-f s16le -ac 2 -ar %d pipe:1'",
             inst->module_dir,
             clean_url,
             MOVE_SAMPLE_RATE,
             MOVE_SAMPLE_RATE);

    inst->pipe = popen(cmd, "r");
    if (!inst->pipe) {
        set_error(inst, "failed to launch ffmpeg pipeline");
        return -1;
    }

    inst->pipe_fd = fileno(inst->pipe);
    if (inst->pipe_fd < 0) {
        set_error(inst, "failed to get ffmpeg fd");
        pclose(inst->pipe);
        inst->pipe = NULL;
        return -1;
    }

    if (fcntl(inst->pipe_fd, F_SETFL, fcntl(inst->pipe_fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        set_error(inst, "failed to set ffmpeg non-blocking mode");
        pclose(inst->pipe);
        inst->pipe = NULL;
        return -1;
    }

    clear_error(inst);
    inst->stream_eof = false;
    inst->restart_countdown = 0;
    inst->prime_needed_samples = (size_t)MOVE_SAMPLE_RATE; /* ~0.5s stereo */
    inst->active_stream_resolved = true;
    yt_log("stream pipeline started (resolved)");
    return 0;
}

static int parse_search_line(const char *line_in, search_result_t *out) {
    char line[4096];
    char *saveptr = NULL;
    char *id;
    char *title;
    char *channel;
    char *duration;
    char *src;
    char *dst;

    if (!line_in || !out) return -1;
    snprintf(line, sizeof(line), "%s", line_in);

    line[strcspn(line, "\r\n")] = '\0';

    /* yt-dlp --print emits literal "\t" sequences, not real tab bytes. */
    src = line;
    dst = line;
    while (*src) {
        if (src[0] == '\\' && src[1] == 't') {
            *dst++ = '\t';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    id = strtok_r(line, "\t", &saveptr);
    title = strtok_r(NULL, "\t", &saveptr);
    channel = strtok_r(NULL, "\t", &saveptr);
    duration = strtok_r(NULL, "\t", &saveptr);

    if (!id || !title) return -1;

    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->title, sizeof(out->title), "%s", title);
    snprintf(out->channel, sizeof(out->channel), "%s", channel ? channel : "");
    snprintf(out->duration, sizeof(out->duration), "%s", duration ? duration : "");
    sanitize_display_text(out->title);
    sanitize_display_text(out->channel);
    sanitize_display_text(out->duration);
    snprintf(out->url, sizeof(out->url), "https://www.youtube.com/watch?v=%s", out->id);
    return 0;
}

static int run_search_command_legacy(const yt_instance_t *inst,
                                     const char *query,
                                     search_result_t *results,
                                     int *out_count,
                                     char *err,
                                     size_t err_len) {
    char clean_query[SEARCH_QUERY_MAX];
    char cmd[8192];
    FILE *fp;
    char line[4096];
    int count = 0;
    int rc;

    if (!inst || !query || !results || !out_count) {
        if (err && err_len > 0) snprintf(err, err_len, "invalid search args");
        return -1;
    }

    sanitize_query(query, clean_query, sizeof(clean_query));

    snprintf(cmd, sizeof(cmd),
        "/bin/sh -lc \"\\\"%s/bin/yt-dlp\\\" --flat-playlist --no-warnings --no-playlist "
        "--extractor-args 'youtube:player_skip=js' "
        "--print '%%(id)s\\t%%(title)s\\t%%(channel)s\\t%%(duration_string)s' "
        "\\\"ytsearch%d:%s\\\" 2>/dev/null\"",
        inst->module_dir, SEARCH_MAX_RESULTS, clean_query);

    fp = popen(cmd, "r");
    if (!fp) {
        if (err && err_len > 0) snprintf(err, err_len, "failed to start yt-dlp search");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) && count < SEARCH_MAX_RESULTS) {
        if (parse_search_line(line, &results[count]) == 0) {
            count++;
        }
    }

    rc = pclose(fp);

    *out_count = count;

    if (count == 0 && rc != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "yt-dlp search failed");
        return -1;
    }

    if (count == 0) {
        if (err && err_len > 0) snprintf(err, err_len, "no results");
        return 0;
    }

    if (err && err_len > 0) err[0] = '\0';
    return 0;
}

static int run_search_command_daemon(yt_instance_t *inst,
                                     const char *query,
                                     search_result_t *results,
                                     int *out_count,
                                     char *err,
                                     size_t err_len) {
    char clean_query[SEARCH_QUERY_MAX];
    char req[SEARCH_QUERY_MAX + 64];
    char line[DAEMON_LINE_MAX];
    char *fields[6];
    int field_count;
    int count;
    int attempt;
    int timed_out;
    bool retryable_timeout = false;

    if (!inst || !query || !results || !out_count) {
        if (err && err_len > 0) snprintf(err, err_len, "invalid search args");
        return -1;
    }

    sanitize_query(query, clean_query, sizeof(clean_query));

    pthread_mutex_lock(&inst->daemon_mutex);
    for (attempt = 0; attempt < 2; attempt++) {
        count = 0;
        timed_out = 0;

        if (start_daemon_locked(inst, err, err_len) != 0) {
            pthread_mutex_unlock(&inst->daemon_mutex);
            return -1;
        }

        snprintf(req, sizeof(req), "SEARCH\t%d\t%s\n", SEARCH_MAX_RESULTS, clean_query);
        if (fputs(req, inst->daemon_in) == EOF || fflush(inst->daemon_in) != 0) {
            if (err && err_len > 0) snprintf(err, err_len, "daemon write failed");
            stop_daemon_locked(inst);
            pthread_mutex_unlock(&inst->daemon_mutex);
            return -1;
        }

        while (1) {
            if (read_daemon_line_locked(inst, line, sizeof(line), DAEMON_SEARCH_TIMEOUT_MS) != 0) {
                timed_out = 1;
                retryable_timeout = true;
                stop_daemon_locked(inst);
                break;
            }

            field_count = split_tab_fields(line, fields, 6);
            if (field_count <= 0 || !fields[0]) continue;

            if (strcmp(fields[0], "SEARCH_BEGIN") == 0) {
                continue;
            }
            if (strcmp(fields[0], "SEARCH_ITEM") == 0) {
                if (count < SEARCH_MAX_RESULTS && field_count >= 3) {
                    snprintf(results[count].id, sizeof(results[count].id), "%s", fields[1]);
                    snprintf(results[count].title, sizeof(results[count].title), "%s", fields[2]);
                    snprintf(results[count].channel, sizeof(results[count].channel), "%s", field_count >= 4 ? fields[3] : "");
                    snprintf(results[count].duration, sizeof(results[count].duration), "%s", field_count >= 5 ? fields[4] : "");
                    sanitize_display_text(results[count].title);
                    sanitize_display_text(results[count].channel);
                    sanitize_display_text(results[count].duration);
                    snprintf(results[count].url,
                             sizeof(results[count].url),
                             "https://www.youtube.com/watch?v=%s",
                             results[count].id);
                    count++;
                }
                continue;
            }
            if (strcmp(fields[0], "SEARCH_END") == 0) {
                *out_count = count;
                if (count == 0) {
                    if (err && err_len > 0) snprintf(err, err_len, "no results");
                } else if (err && err_len > 0) {
                    err[0] = '\0';
                }
                pthread_mutex_unlock(&inst->daemon_mutex);
                return 0;
            }
            if (strcmp(fields[0], "ERROR") == 0) {
                if (err && err_len > 0) snprintf(err, err_len, "%s", field_count >= 2 ? fields[1] : "daemon search failed");
                pthread_mutex_unlock(&inst->daemon_mutex);
                return -1;
            }
        }

        if (!timed_out) break;
        if (attempt == 0) {
            yt_log("search timeout; restarting daemon and retrying once");
        }
    }

    if (retryable_timeout) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon search timeout");
        *out_count = 0;
        pthread_mutex_unlock(&inst->daemon_mutex);
        return -1;
    }

    pthread_mutex_unlock(&inst->daemon_mutex);
    if (err && err_len > 0) snprintf(err, err_len, "daemon search failed");
    return -1;
}

static int run_search_command(yt_instance_t *inst,
                              const char *query,
                              search_result_t *results,
                              int *out_count,
                              char *err,
                              size_t err_len) {
    return run_search_command_daemon(inst, query, results, out_count, err, err_len);
}

/* Caller must hold search_mutex. */
static int spawn_search_thread_locked(yt_instance_t *inst, const char *query) {
    if (!inst || !query || query[0] == '\0') return -1;

    snprintf(inst->search_query, sizeof(inst->search_query), "%s", query);
    inst->search_count = 0;
    inst->search_elapsed_ms = 0;
    set_search_status(inst, "searching", "");
    inst->search_thread_running = true;

    if (pthread_create(&inst->search_thread, NULL, search_thread_main, inst) != 0) {
        inst->search_thread_running = false;
        set_search_status(inst, "error", "failed to start search thread");
        return -1;
    }

    inst->search_thread_valid = true;
    return 0;
}

static void* search_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char query[SEARCH_QUERY_MAX];
    char next_query[SEARCH_QUERY_MAX];
    search_result_t local_results[SEARCH_MAX_RESULTS];
    int local_count = 0;
    char local_err[256] = {0};
    char log_msg[320];
    int rc;
    int start_next;
    uint64_t start_ms;
    uint64_t elapsed_ms;

    if (!inst) return NULL;

    pthread_mutex_lock(&inst->search_mutex);
    snprintf(query, sizeof(query), "%s", inst->search_query);
    pthread_mutex_unlock(&inst->search_mutex);

    yt_log("search started");
    start_ms = now_ms();
    rc = run_search_command(inst, query, local_results, &local_count, local_err, sizeof(local_err));
    elapsed_ms = now_ms() - start_ms;

    pthread_mutex_lock(&inst->search_mutex);

    inst->search_elapsed_ms = elapsed_ms;
    inst->search_count = local_count;

    if (local_count > 0) {
        memcpy(inst->search_results, local_results, (size_t)local_count * sizeof(search_result_t));
    }

    if (rc == 0 && local_count > 0) {
        set_search_status(inst, "done", "");
    } else if (rc == 0) {
        set_search_status(inst, "no_results", local_err[0] ? local_err : "no results");
    } else {
        set_search_status(inst, "error", local_err[0] ? local_err : "search error");
    }
    snprintf(log_msg,
             sizeof(log_msg),
             "search finished status=%s rc=%d count=%d elapsed_ms=%llu err=%s",
             inst->search_status,
             rc,
             local_count,
             (unsigned long long)elapsed_ms,
             local_err[0] ? local_err : "-");
    yt_log(log_msg);

    start_next = 0;
    next_query[0] = '\0';
    if (inst->queued_search_pending && inst->queued_search_query[0] != '\0') {
        snprintf(next_query, sizeof(next_query), "%s", inst->queued_search_query);
        inst->queued_search_pending = false;
        inst->queued_search_query[0] = '\0';
        start_next = 1;
    } else {
        inst->search_thread_running = false;
    }
    pthread_mutex_unlock(&inst->search_mutex);

    if (start_next) {
        pthread_mutex_lock(&inst->search_mutex);
        if (spawn_search_thread_locked(inst, next_query) == 0) {
            snprintf(log_msg,
                     sizeof(log_msg),
                     "starting queued search query=%s",
                     next_query);
            yt_log(log_msg);
        }
        pthread_mutex_unlock(&inst->search_mutex);
    }

    return NULL;
}

static int start_search_async(yt_instance_t *inst, const char *query) {
    if (!inst || !query || query[0] == '\0') return -1;

    if (inst->search_thread_valid && !inst->search_thread_running) {
        pthread_join(inst->search_thread, NULL);
        inst->search_thread_valid = false;
    }

    if (inst->search_thread_running) {
        snprintf(inst->queued_search_query, sizeof(inst->queued_search_query), "%s", query);
        inst->queued_search_pending = true;
        set_search_status(inst, "queued", "search queued");
        return 1;
    }

    return spawn_search_thread_locked(inst, query);
}

static int resolve_stream_url_daemon(yt_instance_t *inst,
                                     const char *source_url,
                                     char *media_url,
                                     size_t media_url_len,
                                     char *user_agent,
                                     size_t user_agent_len,
                                     char *referer,
                                     size_t referer_len,
                                     char *err,
                                     size_t err_len) {
    char req[STREAM_URL_MAX + 16];
    char line[DAEMON_LINE_MAX];
    char *fields[5];
    int field_count;

    if (!inst || !source_url || !media_url || media_url_len == 0) return -1;

    pthread_mutex_lock(&inst->daemon_mutex);
    if (start_daemon_locked(inst, err, err_len) != 0) {
        pthread_mutex_unlock(&inst->daemon_mutex);
        return -1;
    }

    snprintf(req, sizeof(req), "RESOLVE\t%s\n", source_url);
    if (fputs(req, inst->daemon_in) == EOF || fflush(inst->daemon_in) != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon write failed");
        stop_daemon_locked(inst);
        pthread_mutex_unlock(&inst->daemon_mutex);
        return -1;
    }

    if (read_daemon_line_locked(inst, line, sizeof(line), DAEMON_RESOLVE_TIMEOUT_MS) != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "daemon resolve timeout");
        stop_daemon_locked(inst);
        pthread_mutex_unlock(&inst->daemon_mutex);
        return -1;
    }

    field_count = split_tab_fields(line, fields, 5);
    if (field_count >= 2 && strcmp(fields[0], "RESOLVE_OK") == 0) {
        if (!sanitize_any_http_url(fields[1], media_url, media_url_len)) {
            if (err && err_len > 0) snprintf(err, err_len, "daemon resolve url invalid");
            pthread_mutex_unlock(&inst->daemon_mutex);
            return -1;
        }
        sanitize_header_text(field_count >= 3 ? fields[2] : "", user_agent, user_agent_len);
        sanitize_header_text(field_count >= 4 ? fields[3] : "", referer, referer_len);
        if (err && err_len > 0) err[0] = '\0';
        pthread_mutex_unlock(&inst->daemon_mutex);
        return 0;
    }

    if (field_count >= 2 && strcmp(fields[0], "ERROR") == 0) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", fields[1]);
    } else if (err && err_len > 0) {
        snprintf(err, err_len, "daemon resolve failed");
    }
    pthread_mutex_unlock(&inst->daemon_mutex);
    return -1;
}

static int resolve_stream_url_legacy(const yt_instance_t *inst,
                                     const char *source_url,
                                     char *media_url,
                                     size_t media_url_len,
                                     char *err,
                                     size_t err_len) {
    char cmd[8192];
    FILE *fp;
    char line[DAEMON_LINE_MAX];
    int rc;

    if (!inst || !source_url || !media_url || media_url_len == 0) return -1;

    snprintf(cmd,
             sizeof(cmd),
             "/bin/sh -lc \"\\\"%s/bin/yt-dlp\\\" --no-playlist "
             "--extractor-args 'youtube:player_skip=js' "
             "-f 'bestaudio[ext=m4a]/bestaudio' -g "
             "\\\"%s\\\" 2>/dev/null\"",
             inst->module_dir,
             source_url);

    fp = popen(cmd, "r");
    if (!fp) {
        if (err && err_len > 0) snprintf(err, err_len, "failed to start legacy resolve");
        return -1;
    }

    line[0] = '\0';
    if (!fgets(line, sizeof(line), fp)) {
        rc = pclose(fp);
        if (err && err_len > 0) snprintf(err, err_len, "legacy resolve empty (rc=%d)", rc);
        return -1;
    }
    rc = pclose(fp);
    trim_line_end(line);
    if (rc != 0) {
        if (err && err_len > 0) snprintf(err, err_len, "legacy resolve failed");
        return -1;
    }
    if (!sanitize_any_http_url(line, media_url, media_url_len)) {
        if (err && err_len > 0) snprintf(err, err_len, "legacy resolve url invalid");
        return -1;
    }
    if (err && err_len > 0) err[0] = '\0';
    return 0;
}

static int resolve_stream_url(yt_instance_t *inst,
                              const char *source_url,
                              char *media_url,
                              size_t media_url_len,
                              char *user_agent,
                              size_t user_agent_len,
                              char *referer,
                              size_t referer_len,
                              char *err,
                              size_t err_len) {
    return resolve_stream_url_daemon(inst,
                                     source_url,
                                     media_url,
                                     media_url_len,
                                     user_agent,
                                     user_agent_len,
                                     referer,
                                     referer_len,
                                     err,
                                     err_len);
}

static void* resolve_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char source_url[STREAM_URL_MAX];
    char media_url[STREAM_URL_MAX];
    char user_agent[HTTP_HEADER_MAX];
    char referer[HTTP_HEADER_MAX];
    char err[256];
    int rc;
    char log_msg[320];

    if (!inst) return NULL;

    pthread_mutex_lock(&inst->resolve_mutex);
    snprintf(source_url, sizeof(source_url), "%s", inst->stream_url);
    pthread_mutex_unlock(&inst->resolve_mutex);

    media_url[0] = '\0';
    user_agent[0] = '\0';
    referer[0] = '\0';
    err[0] = '\0';
    rc = resolve_stream_url(inst,
                            source_url,
                            media_url,
                            sizeof(media_url),
                            user_agent,
                            sizeof(user_agent),
                            referer,
                            sizeof(referer),
                            err,
                            sizeof(err));

    pthread_mutex_lock(&inst->resolve_mutex);
    if (strcmp(inst->stream_url, source_url) == 0 && source_url[0] != '\0') {
        if (rc == 0) {
            inst->resolve_ready = true;
            inst->resolve_failed = false;
            snprintf(inst->resolved_media_url, sizeof(inst->resolved_media_url), "%s", media_url);
            snprintf(inst->resolved_user_agent, sizeof(inst->resolved_user_agent), "%s", user_agent);
            snprintf(inst->resolved_referer, sizeof(inst->resolved_referer), "%s", referer);
            inst->resolve_error[0] = '\0';
        } else {
            inst->resolve_ready = false;
            inst->resolve_failed = true;
            snprintf(inst->resolve_error, sizeof(inst->resolve_error), "%s", err[0] ? err : "resolve failed");
        }
    }
    inst->resolve_thread_running = false;
    pthread_mutex_unlock(&inst->resolve_mutex);

    if (rc == 0) {
        yt_log("resolve finished");
    } else {
        snprintf(log_msg, sizeof(log_msg), "resolve failed: %s", err[0] ? err : "unknown");
        yt_log(log_msg);
    }

    return NULL;
}

static int start_resolve_async(yt_instance_t *inst) {
    if (!inst) return -1;

    pthread_mutex_lock(&inst->resolve_mutex);
    if (inst->stream_url[0] == '\0') {
        pthread_mutex_unlock(&inst->resolve_mutex);
        return -1;
    }

    if (inst->resolve_thread_valid && !inst->resolve_thread_running) {
        pthread_join(inst->resolve_thread, NULL);
        inst->resolve_thread_valid = false;
    }

    if (inst->resolve_thread_running) {
        pthread_mutex_unlock(&inst->resolve_mutex);
        return 1;
    }

    inst->resolve_ready = false;
    inst->resolve_failed = false;
    inst->resolved_media_url[0] = '\0';
    inst->resolved_user_agent[0] = '\0';
    inst->resolved_referer[0] = '\0';
    inst->resolve_error[0] = '\0';
    inst->resolve_thread_running = true;

    if (pthread_create(&inst->resolve_thread, NULL, resolve_thread_main, inst) != 0) {
        inst->resolve_thread_running = false;
        inst->resolve_failed = true;
        snprintf(inst->resolve_error, sizeof(inst->resolve_error), "failed to start resolve thread");
        pthread_mutex_unlock(&inst->resolve_mutex);
        return -1;
    }

    inst->resolve_thread_valid = true;
    pthread_mutex_unlock(&inst->resolve_mutex);
    return 0;
}

static void pump_pipe(yt_instance_t *inst) {
    uint8_t buf[4096];
    uint8_t merged[4100];
    int16_t samples[2048];

    while (inst->pipe && !inst->stream_eof) {
        if (ring_available(inst) + 2048 >= RING_SAMPLES) {
            break; /* Let pipe backpressure pace producer; avoid dropping */
        }

        ssize_t n = read(inst->pipe_fd, buf, sizeof(buf));
        if (n > 0) {
            size_t merged_bytes = inst->pending_len;
            size_t aligned_bytes;
            size_t remainder;
            size_t sample_count;

            if (inst->pending_len > 0) {
                memcpy(merged, inst->pending_bytes, inst->pending_len);
            }

            memcpy(merged + merged_bytes, buf, (size_t)n);
            merged_bytes += (size_t)n;

            aligned_bytes = merged_bytes & ~((size_t)3U);
            remainder = merged_bytes - aligned_bytes;
            if (remainder > 0) {
                memcpy(inst->pending_bytes, merged + aligned_bytes, remainder);
            }
            inst->pending_len = (uint8_t)remainder;

            sample_count = aligned_bytes / sizeof(int16_t);
            if (sample_count > 0) {
                memcpy(samples, merged, sample_count * sizeof(int16_t));
                ring_push(inst, samples, sample_count);
            }
            if ((size_t)n < sizeof(buf)) {
                break;
            }
            continue;
        }
        if (n == 0) {
            if (inst->active_stream_resolved && !inst->resolved_fallback_attempted) {
                pthread_mutex_lock(&inst->resolve_mutex);
                inst->resolve_ready = false;
                inst->resolve_failed = true;
                pthread_mutex_unlock(&inst->resolve_mutex);
                inst->resolved_fallback_attempted = true;
                set_error(inst, "resolved stream ended, falling back");
                stop_stream(inst);
                clear_ring(inst);
                inst->stream_eof = false;
                inst->restart_countdown = 0;
                break;
            }
            inst->stream_eof = true;
            set_error(inst, "stream ended");
            stop_stream(inst);
            inst->restart_countdown = 0;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        }
        if (inst->active_stream_resolved && !inst->resolved_fallback_attempted) {
            pthread_mutex_lock(&inst->resolve_mutex);
            inst->resolve_ready = false;
            inst->resolve_failed = true;
            pthread_mutex_unlock(&inst->resolve_mutex);
            inst->resolved_fallback_attempted = true;
            set_error(inst, "resolved stream read error, falling back");
            stop_stream(inst);
            clear_ring(inst);
            inst->stream_eof = false;
            inst->restart_countdown = 0;
            break;
        }
        inst->stream_eof = true;
        set_error(inst, "stream read error");
        stop_stream(inst);
        inst->restart_countdown = 0;
        break;
    }
}

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    yt_instance_t *inst;

    inst = calloc(1, sizeof(*inst));
    if (!inst) return NULL;

    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir ? module_dir : ".");
    inst->stream_url[0] = '\0';
    inst->gain = 1.0f;
    inst->pipe_fd = -1;
    inst->daemon_pid = -1;

    pthread_mutex_init(&inst->search_mutex, NULL);
    pthread_mutex_init(&inst->daemon_mutex, NULL);
    pthread_mutex_init(&inst->resolve_mutex, NULL);
    snprintf(inst->search_status, sizeof(inst->search_status), "idle");
    (void)json_defaults;
    start_warmup_if_needed(inst);

    return inst;
}

static void v2_destroy_instance(void *instance) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    pid_t daemon_pid_snapshot;
    if (!inst) return;

    stop_stream(inst);

    daemon_pid_snapshot = inst->daemon_pid;
    if (daemon_pid_snapshot > 0) {
        (void)kill(daemon_pid_snapshot, SIGTERM);
    }

    if (inst->warmup_thread_valid) {
        pthread_join(inst->warmup_thread, NULL);
        inst->warmup_thread_valid = false;
    }

    if (inst->resolve_thread_valid) {
        pthread_join(inst->resolve_thread, NULL);
        inst->resolve_thread_valid = false;
        inst->resolve_thread_running = false;
    }

    if (inst->search_thread_valid) {
        pthread_join(inst->search_thread, NULL);
        inst->search_thread_valid = false;
        inst->search_thread_running = false;
    }

    pthread_mutex_lock(&inst->daemon_mutex);
    stop_daemon_locked(inst);
    pthread_mutex_unlock(&inst->daemon_mutex);

    pthread_mutex_destroy(&inst->resolve_mutex);
    pthread_mutex_destroy(&inst->daemon_mutex);
    pthread_mutex_destroy(&inst->search_mutex);
    free(inst);
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance;
    (void)msg;
    (void)len;
    (void)source;
}

/* Accept new enum trigger values and legacy numeric step counters. */
static bool parse_trigger_value(const char *val, int *legacy_step_state) {
    int step;
    int prev;

    if (!val || !legacy_step_state) return false;

    if (strcmp(val, "trigger") == 0 || strcmp(val, "on") == 0) {
        return true;
    }
    if (strcmp(val, "idle") == 0 || strcmp(val, "off") == 0) {
        return false;
    }

    step = atoi(val);
    prev = *legacy_step_state;
    *legacy_step_state = step;
    return step > prev;
}

static bool allow_trigger(uint64_t *last_ms, uint64_t debounce_ms) {
    uint64_t now;
    if (!last_ms) return true;
    now = now_ms();
    if (*last_ms != 0 && now > *last_ms && (now - *last_ms) < debounce_ms) {
        return false;
    }
    *last_ms = now;
    return true;
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

    if (strcmp(key, "stream_url") == 0) {
        char clean_url[STREAM_URL_MAX];
        if (val[0] == '\0') {
            stop_everything(inst);
            return;
        }

        if (!sanitize_stream_url(val, clean_url, sizeof(clean_url))) {
            set_error(inst, "invalid stream_url");
            return;
        }

        snprintf(inst->stream_url, sizeof(inst->stream_url), "%s", clean_url);
        pthread_mutex_lock(&inst->resolve_mutex);
        inst->resolve_ready = false;
        inst->resolve_failed = false;
        inst->resolved_media_url[0] = '\0';
        inst->resolved_user_agent[0] = '\0';
        inst->resolved_referer[0] = '\0';
        inst->resolve_error[0] = '\0';
        pthread_mutex_unlock(&inst->resolve_mutex);
        restart_stream_from_beginning(inst, 0);
        (void)start_resolve_async(inst);
        return;
    }

    if (strcmp(key, "play_pause_toggle") == 0) {
        if (inst->stream_url[0] != '\0' && !inst->stream_eof) {
            inst->paused = !inst->paused;
        }
        return;
    }

    if (strcmp(key, "play_pause_step") == 0) {
        if (parse_trigger_value(val, &inst->play_pause_step)) {
            if (allow_trigger(&inst->last_play_pause_ms, DEBOUNCE_PLAY_PAUSE_MS) &&
                inst->stream_url[0] != '\0' &&
                !inst->stream_eof) {
                inst->paused = !inst->paused;
            }
        }
        return;
    }

    if (strcmp(key, "stop") == 0) {
        stop_everything(inst);
        return;
    }

    if (strcmp(key, "stop_step") == 0) {
        if (parse_trigger_value(val, &inst->stop_step) &&
            allow_trigger(&inst->last_stop_ms, DEBOUNCE_STOP_MS)) {
            stop_everything(inst);
        }
        return;
    }

    if (strcmp(key, "restart") == 0) {
        if (inst->stream_url[0] != '\0') {
            restart_stream_from_beginning(inst, 0);
            (void)start_resolve_async(inst);
        }
        return;
    }

    if (strcmp(key, "restart_step") == 0) {
        if (parse_trigger_value(val, &inst->restart_step)) {
            if (allow_trigger(&inst->last_restart_ms, DEBOUNCE_RESTART_MS) &&
                inst->stream_url[0] != '\0') {
                restart_stream_from_beginning(inst, 0);
                (void)start_resolve_async(inst);
            }
        }
        return;
    }

    if (strcmp(key, "seek_delta_seconds") == 0) {
        long delta_sec = strtol(val, NULL, 10);
        seek_relative_seconds(inst, delta_sec);
        return;
    }

    if (strcmp(key, "rewind_15_step") == 0) {
        if (parse_trigger_value(val, &inst->rewind_15_step) &&
            allow_trigger(&inst->last_rewind_ms, DEBOUNCE_SEEK_MS)) {
            seek_relative_seconds(inst, -15);
        }
        return;
    }

    if (strcmp(key, "forward_15_step") == 0) {
        if (parse_trigger_value(val, &inst->forward_15_step) &&
            allow_trigger(&inst->last_forward_ms, DEBOUNCE_SEEK_MS)) {
            seek_relative_seconds(inst, 15);
        }
        return;
    }

    if (strcmp(key, "search_query") == 0) {
        pthread_mutex_lock(&inst->search_mutex);
        (void)start_search_async(inst, val);
        pthread_mutex_unlock(&inst->search_mutex);
    }
}

static int get_result_index(const char *key, const char *prefix) {
    size_t len;
    int idx;

    if (!key || !prefix) return -1;
    len = strlen(prefix);
    if (strncmp(key, prefix, len) != 0) return -1;
    idx = atoi(key + len);
    if (idx < 0 || idx >= SEARCH_MAX_RESULTS) return -1;
    return idx;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    if (!key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "gain") == 0) {
        return snprintf(buf, (size_t)buf_len, "%.2f", inst ? inst->gain : 1.0f);
    }
    if (strcmp(key, "play_pause_step") == 0) {
        return snprintf(buf, (size_t)buf_len, "idle");
    }
    if (strcmp(key, "rewind_15_step") == 0) {
        return snprintf(buf, (size_t)buf_len, "idle");
    }
    if (strcmp(key, "forward_15_step") == 0) {
        return snprintf(buf, (size_t)buf_len, "idle");
    }
    if (strcmp(key, "stop_step") == 0) {
        return snprintf(buf, (size_t)buf_len, "idle");
    }
    if (strcmp(key, "restart_step") == 0) {
        return snprintf(buf, (size_t)buf_len, "idle");
    }
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, (size_t)buf_len, "YT Stream");
    }
    if (strcmp(key, "stream_url") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", inst ? inst->stream_url : "");
    }
    if (strcmp(key, "stream_status") == 0) {
        size_t avail;
        if (!inst) return snprintf(buf, (size_t)buf_len, "stopped");
        if (inst->stream_url[0] == '\0') return snprintf(buf, (size_t)buf_len, "stopped");
        if (inst->paused) return snprintf(buf, (size_t)buf_len, "paused");
        if (inst->seek_discard_samples > 0) return snprintf(buf, (size_t)buf_len, "seeking");
        if (!inst->pipe && inst->restart_countdown > 0) return snprintf(buf, (size_t)buf_len, "loading");
        if (!inst->pipe && !inst->stream_eof) return snprintf(buf, (size_t)buf_len, "loading");
        if (inst->stream_eof) return snprintf(buf, (size_t)buf_len, "eof");
        avail = ring_available(inst);
        if (inst->prime_needed_samples > 0 && avail < inst->prime_needed_samples) {
            return snprintf(buf, (size_t)buf_len, "buffering");
        }
        return snprintf(buf, (size_t)buf_len, "streaming");
    }

    if (inst && strcmp(key, "search_status") == 0) {
        int ret;
        pthread_mutex_lock(&inst->search_mutex);
        ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_status);
        pthread_mutex_unlock(&inst->search_mutex);
        return ret;
    }
    if (inst && strcmp(key, "search_query") == 0) {
        int ret;
        pthread_mutex_lock(&inst->search_mutex);
        ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_query);
        pthread_mutex_unlock(&inst->search_mutex);
        return ret;
    }
    if (inst && strcmp(key, "search_error") == 0) {
        int ret;
        pthread_mutex_lock(&inst->search_mutex);
        ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_error);
        pthread_mutex_unlock(&inst->search_mutex);
        return ret;
    }
    if (inst && strcmp(key, "search_count") == 0) {
        int ret;
        pthread_mutex_lock(&inst->search_mutex);
        ret = snprintf(buf, (size_t)buf_len, "%d", inst->search_count);
        pthread_mutex_unlock(&inst->search_mutex);
        return ret;
    }
    if (inst && strcmp(key, "search_elapsed_ms") == 0) {
        int ret;
        pthread_mutex_lock(&inst->search_mutex);
        ret = snprintf(buf, (size_t)buf_len, "%llu", (unsigned long long)inst->search_elapsed_ms);
        pthread_mutex_unlock(&inst->search_mutex);
        return ret;
    }

    if (inst) {
        int idx = get_result_index(key, "search_result_title_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].title);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_channel_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].channel);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_duration_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].duration);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_url_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].url);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) {
                ret = snprintf(buf,
                               (size_t)buf_len,
                               "%s\t%s\t%s\t%s",
                               inst->search_results[idx].title,
                               inst->search_results[idx].channel,
                               inst->search_results[idx].duration,
                               inst->search_results[idx].url);
            }
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }
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
    char log_msg[128];

    if (!out_interleaved_lr || frames <= 0) return;

    needed = (size_t)frames * 2;
    memset(out_interleaved_lr, 0, needed * sizeof(int16_t));

    if (!inst) return;

    if (inst->stream_url[0] == '\0') {
        return;
    }

    if (inst->stream_eof) {
        return;
    }

    if (inst->paused) {
        return;
    }

    if (!inst->pipe) {
        bool resolve_ready = false;
        bool resolve_failed = false;
        bool resolve_running = false;
        char resolved_media_url[STREAM_URL_MAX];

        resolved_media_url[0] = '\0';
        if (inst->restart_countdown > 0) {
            inst->restart_countdown--;
        } else {
            pthread_mutex_lock(&inst->resolve_mutex);
            resolve_ready = inst->resolve_ready;
            resolve_failed = inst->resolve_failed;
            resolve_running = inst->resolve_thread_running;
            if (resolve_ready) {
                snprintf(resolved_media_url, sizeof(resolved_media_url), "%s", inst->resolved_media_url);
            }
            pthread_mutex_unlock(&inst->resolve_mutex);

            if (resolve_ready) {
                if (start_stream_resolved(inst, resolved_media_url) != 0) {
                    pthread_mutex_lock(&inst->resolve_mutex);
                    inst->resolve_ready = false;
                    inst->resolve_failed = true;
                    pthread_mutex_unlock(&inst->resolve_mutex);
                    resolve_failed = true;
                }
            } else if (resolve_failed) {
                /* Resolve failed in background; fail over to legacy stream pipeline now. */
            } else if (!resolve_running) {
                if (start_resolve_async(inst) < 0) {
                    resolve_failed = true;
                } else {
                    return;
                }
            } else {
                return;
            }

            if (!inst->pipe && resolve_failed) {
                if (start_stream_legacy(inst) != 0) {
                    inst->stream_eof = true;
                    inst->restart_countdown = 0;
                } else {
                    pthread_mutex_lock(&inst->resolve_mutex);
                    inst->resolve_failed = false;
                    pthread_mutex_unlock(&inst->resolve_mutex);
                }
            }
        }
    }

    pump_pipe(inst);

    if (inst->prime_needed_samples > 0) {
        if (ring_available(inst) < inst->prime_needed_samples && !inst->stream_eof) {
            return;
        }
        inst->prime_needed_samples = 0;
    }

    got = ring_pop(inst, out_interleaved_lr, needed);

    if (inst->dropped_samples >= inst->dropped_log_next) {
        snprintf(log_msg,
                 sizeof(log_msg),
                 "ring overflow dropped_samples=%llu",
                 (unsigned long long)inst->dropped_samples);
        yt_log(log_msg);
        inst->dropped_log_next += (uint64_t)MOVE_SAMPLE_RATE * 2ULL;
    }

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
