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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "plugin_api_v1.h"
#include "gain_state.h"

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
#define SEARCH_URL_MAX 512
#define PROVIDER_MAX 24
#define STREAM_URL_MAX 4096
#define HTTP_HEADER_MAX 384
#define DAEMON_LINE_MAX 4096
#define DAEMON_START_TIMEOUT_MS 12000
#define DAEMON_SEARCH_TIMEOUT_MS 12000
#define DAEMON_RESOLVE_TIMEOUT_MS 12000
#define WS_RUNTIME_LOG_PATH "/data/UserData/schwung/cache/webstream-runtime.log"

static const host_api_v1_t *g_host = NULL;

static void* search_thread_main(void *arg);
static void* resolve_thread_main(void *arg);
static void* warmup_thread_main(void *arg);
static void* stream_reap_thread_main(void *arg);
static void* prefetch_thread_main(void *arg);

typedef struct {
    FILE *pipe;
    pid_t pid;
} stream_reap_job_t;

typedef struct {
    char provider[PROVIDER_MAX];
    char id[SEARCH_ID_MAX];
    char title[SEARCH_TEXT_MAX];
    char channel[SEARCH_TEXT_MAX];
    char duration[24];
    char url[SEARCH_URL_MAX];
    char meta_key[8];
    char meta_scale[12];
    char meta_tempo[12];
    char meta_genre[SEARCH_TEXT_MAX];
    char meta_style[SEARCH_TEXT_MAX];
    char meta_country[48];
    char meta_year[8];
} search_result_t;

typedef struct {
    char module_dir[512];
    char stream_provider[PROVIDER_MAX];
    char stream_url[STREAM_URL_MAX];
    char error_msg[256];

    FILE *pipe;
    int pipe_fd;
    pid_t stream_pid;
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
    double resume_offset_sec;  /* seek position for stream reconnect */
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

    int cratedig_result_index;
    bool cratedig_auto_advance;
    int next_track_step;
    uint64_t last_next_track_ms;
    char cratedig_pending_filter[2048];

    pthread_mutex_t prefetch_mutex;
    pthread_t prefetch_thread;
    bool prefetch_thread_valid;
    bool prefetch_thread_running;
    char prefetch_source_url[STREAM_URL_MAX];
    char prefetch_media_url[STREAM_URL_MAX];
    char prefetch_user_agent[HTTP_HEADER_MAX];
    char prefetch_referer[HTTP_HEADER_MAX];
    bool prefetch_ready;

    pthread_mutex_t search_mutex;
    pthread_t search_thread;
    bool search_thread_valid;
    bool search_thread_running;
    char search_provider[PROVIDER_MAX];
    char search_query[SEARCH_QUERY_MAX];
    char queued_search_provider[PROVIDER_MAX];
    char queued_search_query[SEARCH_QUERY_MAX];
    bool queued_search_pending;
    char search_status[24];
    char search_error[256];
    uint64_t search_elapsed_ms;
    int search_count;
    search_result_t search_results[SEARCH_MAX_RESULTS];

    /* WAV download */
    pthread_t download_thread;
    bool download_thread_valid;
    bool download_thread_running;
    bool download_cancel;
    pid_t download_pid;
    char download_status[32];       /* idle, downloading, done, error, cancelled */
    char download_path[512];
    char download_active_path[512]; /* path being written to (for progress) */
    char download_error[256];
    char download_source_url[STREAM_URL_MAX];
    char download_source_provider[PROVIDER_MAX];
    char download_resolved_url[STREAM_URL_MAX];
    char download_user_agent[HTTP_HEADER_MAX];
    char download_referer[HTTP_HEADER_MAX];
    char download_title[SEARCH_TEXT_MAX];
} yt_instance_t;

static void append_ws_log(const char *msg) {
    FILE *fp;
    if (!msg || msg[0] == '\0') return;
    fp = fopen(WS_RUNTIME_LOG_PATH, "a");
    if (!fp) return;
    fprintf(fp, "%s\n", msg);
    fclose(fp);
}

static void yt_log(const char *msg) {
    append_ws_log(msg);
    if (g_host && g_host->log) {
        char buf[384];
        snprintf(buf, sizeof(buf), "[ws] %s", msg);
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
    char line[DAEMON_LINE_MAX];
    if (!inst) return NULL;
    err[0] = '\0';
    if (ensure_daemon_started(inst, err, sizeof(err)) == 0) {
        yt_log("yt-dlp daemon warmed");
        /* Pre-init cratedig session so first search is fast */
        pthread_mutex_lock(&inst->daemon_mutex);
        if (inst->daemon_ready && inst->daemon_in) {
            if (fputs("CRATEDIG_INIT\n", inst->daemon_in) != EOF) {
                fflush(inst->daemon_in);
                if (read_daemon_line_locked(inst, line, sizeof(line), 20000) == 0) {
                    yt_log("cratedig session pre-warmed");
                } else {
                    yt_log("cratedig session pre-warm timeout");
                }
            }
        }
        pthread_mutex_unlock(&inst->daemon_mutex);
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
    append_ws_log(inst->error_msg);
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

static void normalize_provider_value(const char *in, char *out, size_t out_len) {
    char input_copy[PROVIDER_MAX];
    const char *src;
    char tmp[PROVIDER_MAX];
    size_t i;
    size_t j = 0;

    if (!out || out_len == 0) return;

    input_copy[0] = '\0';
    if (in) {
        snprintf(input_copy, sizeof(input_copy), "%s", in);
    }
    src = input_copy;
    out[0] = '\0';

    for (i = 0; src[i] != '\0' && j + 1 < sizeof(tmp); i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '-') {
            tmp[j++] = (char)tolower(c);
        }
    }
    tmp[j] = '\0';

    if (strcmp(tmp, "yt") == 0 || strcmp(tmp, "youtube") == 0) {
        snprintf(out, out_len, "youtube");
        return;
    }
    if (strcmp(tmp, "fs") == 0 || strcmp(tmp, "freesound") == 0) {
        snprintf(out, out_len, "freesound");
        return;
    }
    if (strcmp(tmp, "ia") == 0 || strcmp(tmp, "archive") == 0 ||
        strcmp(tmp, "archiveorg") == 0 || strcmp(tmp, "internetarchive") == 0) {
        snprintf(out, out_len, "archive");
        return;
    }
    if (strcmp(tmp, "sc") == 0 || strcmp(tmp, "soundcloud") == 0) {
        snprintf(out, out_len, "soundcloud");
        return;
    }
    if (strcmp(tmp, "samplette") == 0) {
        snprintf(out, out_len, "samplette");
        return;
    }
    if (strcmp(tmp, "cd") == 0 || strcmp(tmp, "cratedig") == 0) {
        snprintf(out, out_len, "cratedig");
        return;
    }
    if (tmp[0] == '\0') {
        snprintf(out, out_len, "youtube");
        return;
    }
    snprintf(out, out_len, "%s", tmp);
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

static bool sanitize_stream_url(const char *in, char *out, size_t out_len) {
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

static void infer_provider_from_url(const char *url, char *out, size_t out_len) {
    char normalized[PROVIDER_MAX];
    const char *u = url ? url : "";
    if (!out || out_len == 0) return;

    if (strstr(u, "soundcloud.com") || strstr(u, "sndcdn.com")) {
        snprintf(out, out_len, "soundcloud");
        return;
    }
    if (strstr(u, "freesound.org") || strstr(u, "cdn.freesound.org")) {
        snprintf(out, out_len, "freesound");
        return;
    }
    if (strstr(u, "archive.org")) {
        snprintf(out, out_len, "archive");
        return;
    }
    if (strstr(u, "youtube.com") || strstr(u, "youtu.be") || strstr(u, "googlevideo.com")) {
        snprintf(out, out_len, "youtube");
        return;
    }

    normalize_provider_value(out, normalized, sizeof(normalized));
    snprintf(out, out_len, "%s", normalized);
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

    /* Only snap play_abs forward if old data is actually being overwritten.
     * The backpressure check in pump_pipe should prevent this in normal
     * operation, but guard against it anyway. */
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

static bool supports_legacy_fallback(const yt_instance_t *inst) {
    char provider[PROVIDER_MAX];
    if (!inst) return false;
    normalize_provider_value(inst->stream_provider, provider, sizeof(provider));
    return strcmp(provider, "youtube") == 0 || strcmp(provider, "soundcloud") == 0;
}

static bool prefer_legacy_pipeline(const yt_instance_t *inst) {
    char provider[PROVIDER_MAX];
    if (!inst) return false;
    normalize_provider_value(inst->stream_provider, provider, sizeof(provider));
    return strcmp(provider, "soundcloud") == 0;
}

static void terminate_stream_process(pid_t pid) {
    int status;
    pid_t rc;

    if (pid <= 0) return;

    rc = waitpid(pid, &status, WNOHANG);
    if (rc == pid) return;

    (void)kill(-pid, SIGTERM);
    usleep(120000);
    rc = waitpid(pid, &status, WNOHANG);
    if (rc == 0) {
        (void)kill(-pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
    }
}

static void* stream_reap_thread_main(void *arg) {
    stream_reap_job_t *job = (stream_reap_job_t *)arg;
    FILE *pipe = NULL;
    pid_t pid = -1;

    if (job) {
        pipe = job->pipe;
        pid = job->pid;
        free(job);
    }

    if (pipe) {
        fclose(pipe);
    }

    terminate_stream_process(pid);
    return NULL;
}

static void schedule_stream_reap(FILE *pipe, pid_t pid) {
    stream_reap_job_t *job;
    pthread_t thread;

    if (!pipe && pid <= 0) return;

    job = calloc(1, sizeof(*job));
    if (!job) {
        if (pipe) fclose(pipe);
        terminate_stream_process(pid);
        return;
    }

    job->pipe = pipe;
    job->pid = pid;

    if (pthread_create(&thread, NULL, stream_reap_thread_main, job) == 0) {
        pthread_detach(thread);
        return;
    }

    if (pipe) fclose(pipe);
    terminate_stream_process(pid);
    free(job);
}

static void stop_stream(yt_instance_t *inst) {
    FILE *pipe;
    pid_t pid;
    if (!inst || !inst->pipe) return;
    pipe = inst->pipe;
    pid = inst->stream_pid;
    inst->pipe = NULL;
    inst->pipe_fd = -1;
    inst->stream_pid = -1;
    schedule_stream_reap(pipe, pid);
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
    inst->resume_offset_sec = 0.0;
    inst->active_stream_resolved = false;
    inst->resolved_fallback_attempted = false;
}

static void seek_relative_seconds(yt_instance_t *inst, long delta_sec) {
    int64_t current_samples;
    int64_t target_samples;
    int64_t delta_samples;
    int64_t oldest_samples;
    int64_t newest_samples;
    char log_msg[256];

    if (!inst || inst->stream_url[0] == '\0') return;

    current_samples = (int64_t)inst->play_abs;
    delta_samples = (int64_t)delta_sec * (int64_t)MOVE_SAMPLE_RATE * 2LL;
    target_samples = current_samples + delta_samples;

    oldest_samples = (int64_t)ring_oldest_abs(inst);
    newest_samples = (int64_t)inst->write_abs;
    if (target_samples < oldest_samples) target_samples = oldest_samples;
    if (target_samples > newest_samples) target_samples = newest_samples;

    snprintf(log_msg, sizeof(log_msg),
             "seek delta=%lds cur=%lld target=%lld oldest=%lld newest=%lld moved=%lld",
             delta_sec, (long long)current_samples, (long long)target_samples,
             (long long)oldest_samples, (long long)newest_samples,
             (long long)(target_samples - current_samples));
    yt_log(log_msg);

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

static int spawn_stream_command(yt_instance_t *inst, const char *cmd, const char *err_prefix) {
    int pipefd[2];
    pid_t pid;
    FILE *fp;

    if (!inst || !cmd || cmd[0] == '\0') return -1;

    if (pipe(pipefd) != 0) {
        set_error(inst, err_prefix ? err_prefix : "stream pipe failed");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        set_error(inst, err_prefix ? err_prefix : "stream fork failed");
        return -1;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-lc", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    fp = fdopen(pipefd[0], "r");
    if (!fp) {
        close(pipefd[0]);
        terminate_stream_process(pid);
        set_error(inst, err_prefix ? err_prefix : "stream fdopen failed");
        return -1;
    }

    inst->pipe = fp;
    inst->pipe_fd = fileno(fp);
    inst->stream_pid = pid;
    if (inst->pipe_fd < 0) {
        FILE *cleanup_pipe = inst->pipe;
        pid_t cleanup_pid = inst->stream_pid;
        inst->pipe = NULL;
        inst->pipe_fd = -1;
        inst->stream_pid = -1;
        schedule_stream_reap(cleanup_pipe, cleanup_pid);
        set_error(inst, err_prefix ? err_prefix : "stream fileno failed");
        return -1;
    }

    if (fcntl(inst->pipe_fd, F_SETFL, fcntl(inst->pipe_fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        FILE *cleanup_pipe = inst->pipe;
        pid_t cleanup_pid = inst->stream_pid;
        inst->pipe = NULL;
        inst->pipe_fd = -1;
        inst->stream_pid = -1;
        schedule_stream_reap(cleanup_pipe, cleanup_pid);
        set_error(inst, err_prefix ? err_prefix : "stream non-blocking failed");
        return -1;
    }

    return 0;
}

static int start_stream_legacy(yt_instance_t *inst) {
    char cmd[8192];
    char provider[PROVIDER_MAX];
    const char *legacy_fmt = "bestaudio[ext=m4a]/bestaudio";
    const char *extractor_args = "--extractor-args \"youtube:player_skip=js\" ";

    stop_stream(inst);
    normalize_provider_value(inst->stream_provider, provider, sizeof(provider));
    if (strcmp(provider, "soundcloud") == 0) {
        legacy_fmt = "http_mp3_1_0/hls_mp3_1_0/bestaudio";
        extractor_args = "";
    }

    {
        char ss_flag[64];
        ss_flag[0] = '\0';
        if (inst->resume_offset_sec > 1.0) {
            snprintf(ss_flag, sizeof(ss_flag), "-ss %.1f ", inst->resume_offset_sec);
            yt_log("resuming legacy stream with seek offset");
        }
        inst->resume_offset_sec = 0.0;

        snprintf(cmd, sizeof(cmd),
            "exec \"%s/bin/yt-dlp\" --no-playlist "
            "%s"
            "-f \"%s\" -o - \"%s\" 2>/dev/null | "
            "\"%s/bin/ffmpeg\" -hide_banner -loglevel error "
            "%s"
            "-probesize 128k -analyzeduration 0 "
            "-i pipe:0 -vn -sn -dn "
            "-af \"aresample=%d\" "
            "-f s16le -ac 2 -ar %d pipe:1",
            inst->module_dir, extractor_args, legacy_fmt, inst->stream_url,
            inst->module_dir, ss_flag, MOVE_SAMPLE_RATE, MOVE_SAMPLE_RATE);
    }

    if (spawn_stream_command(inst, cmd, "failed to launch yt-dlp/ffmpeg pipeline") != 0) {
        set_error(inst, "failed to launch yt-dlp/ffmpeg pipeline");
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
    char ua_flag[HTTP_HEADER_MAX + 32];
    char ref_flag[HTTP_HEADER_MAX + 32];

    if (!inst || !media_url || media_url[0] == '\0') {
        set_error(inst, "resolved media url missing");
        return -1;
    }

    if (!sanitize_any_http_url(media_url, clean_url, sizeof(clean_url))) {
        set_error(inst, "resolved media url invalid");
        return -1;
    }

    stop_stream(inst);

    ua_flag[0] = '\0';
    ref_flag[0] = '\0';
    pthread_mutex_lock(&inst->resolve_mutex);
    if (inst->resolved_user_agent[0] != '\0') {
        snprintf(ua_flag, sizeof(ua_flag), "-user_agent \"%s\" ", inst->resolved_user_agent);
    }
    if (inst->resolved_referer[0] != '\0') {
        snprintf(ref_flag, sizeof(ref_flag), "-referer \"%s\" ", inst->resolved_referer);
    }
    pthread_mutex_unlock(&inst->resolve_mutex);

    {
        char ss_flag[64];
        ss_flag[0] = '\0';
        if (inst->resume_offset_sec > 1.0) {
            snprintf(ss_flag, sizeof(ss_flag), "-ss %.1f ", inst->resume_offset_sec);
            yt_log("resuming resolved stream with seek offset");
        }
        inst->resume_offset_sec = 0.0;

        snprintf(cmd,
                 sizeof(cmd),
                 "exec \"%s/bin/ffmpeg\" -hide_banner -loglevel warning "
                 "%s%s%s"
                 "-probesize 128k -analyzeduration 0 "
                 "-i \"%s\" -vn -sn -dn "
                 "-af \"aresample=%d\" "
                 "-f s16le -ac 2 -ar %d pipe:1 2>/dev/null",
                 inst->module_dir,
                 ss_flag,
                 ua_flag,
                 ref_flag,
                 clean_url,
                 MOVE_SAMPLE_RATE,
                 MOVE_SAMPLE_RATE);
    }

    if (spawn_stream_command(inst, cmd, "failed to launch ffmpeg pipeline") != 0) {
        set_error(inst, "failed to launch ffmpeg pipeline");
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

    snprintf(out->provider, sizeof(out->provider), "youtube");
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
                                     const char *provider,
                                     const char *query,
                                     search_result_t *results,
                                     int *out_count,
                                     char *err,
                                     size_t err_len) {
    char clean_provider[PROVIDER_MAX];
    char clean_query[SEARCH_QUERY_MAX];
    char req[SEARCH_QUERY_MAX + PROVIDER_MAX + 64];
    char line[DAEMON_LINE_MAX];
    char *fields[14];
    int field_count;
    int count;
    int attempt;
    int timed_out;
    bool retryable_timeout = false;
    bool is_cratedig;

    if (!inst || !provider || !query || !results || !out_count) {
        if (err && err_len > 0) snprintf(err, err_len, "invalid search args");
        return -1;
    }

    normalize_provider_value(provider, clean_provider, sizeof(clean_provider));
    is_cratedig = (strcmp(clean_provider, "samplette") == 0 || strcmp(clean_provider, "cratedig") == 0);
    if (!is_cratedig) {
        sanitize_query(query, clean_query, sizeof(clean_query));
    } else {
        clean_query[0] = '\0';
    }

    pthread_mutex_lock(&inst->daemon_mutex);
    for (attempt = 0; attempt < 2; attempt++) {
        count = 0;
        timed_out = 0;

        if (start_daemon_locked(inst, err, err_len) != 0) {
            pthread_mutex_unlock(&inst->daemon_mutex);
            return -1;
        }

        if (is_cratedig) {
            /* Send pending filter before searching (runs on search thread, OK to block) */
            pthread_mutex_lock(&inst->search_mutex);
            if (inst->cratedig_pending_filter[0] != '\0') {
                char filter_req[2048 + 32];
                char filter_line[DAEMON_LINE_MAX];
                snprintf(filter_req, sizeof(filter_req), "CRATEDIG_FILTER\t%s\n", inst->cratedig_pending_filter);
                inst->cratedig_pending_filter[0] = '\0';
                pthread_mutex_unlock(&inst->search_mutex);
                if (fputs(filter_req, inst->daemon_in) != EOF) {
                    fflush(inst->daemon_in);
                    (void)read_daemon_line_locked(inst, filter_line, sizeof(filter_line), DAEMON_SEARCH_TIMEOUT_MS);
                }
            } else {
                pthread_mutex_unlock(&inst->search_mutex);
            }
            snprintf(req, sizeof(req), "CRATEDIG_SEARCH\t%d\n", SEARCH_MAX_RESULTS);
        } else {
            snprintf(req, sizeof(req), "SEARCH\t%s\t%d\t%s\n", clean_provider, SEARCH_MAX_RESULTS, clean_query);
        }
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

            field_count = split_tab_fields(line, fields, 14);
            if (field_count <= 0 || !fields[0]) continue;

            if (strcmp(fields[0], "SEARCH_BEGIN") == 0) {
                continue;
            }
            if (strcmp(fields[0], "SEARCH_ITEM") == 0) {
                if (count < SEARCH_MAX_RESULTS && field_count >= 3) {
                    char fallback_url[SEARCH_URL_MAX];
                    const char *item_url = "";

                    fallback_url[0] = '\0';
                    snprintf(results[count].id, sizeof(results[count].id), "%s", fields[1]);
                    snprintf(results[count].title, sizeof(results[count].title), "%s", fields[2]);
                    snprintf(results[count].channel, sizeof(results[count].channel), "%s", field_count >= 4 ? fields[3] : "");
                    snprintf(results[count].duration, sizeof(results[count].duration), "%s", field_count >= 5 ? fields[4] : "");
                    snprintf(results[count].provider, sizeof(results[count].provider), "%s",
                             is_cratedig ? "youtube" : clean_provider);
                    sanitize_display_text(results[count].title);
                    sanitize_display_text(results[count].channel);
                    sanitize_display_text(results[count].duration);

                    /* Extended metadata fields (positions 6-12) from cratedig/samplette */
                    snprintf(results[count].meta_key, sizeof(results[count].meta_key), "%s", field_count >= 7 ? fields[6] : "");
                    snprintf(results[count].meta_scale, sizeof(results[count].meta_scale), "%s", field_count >= 8 ? fields[7] : "");
                    snprintf(results[count].meta_tempo, sizeof(results[count].meta_tempo), "%s", field_count >= 9 ? fields[8] : "");
                    snprintf(results[count].meta_genre, sizeof(results[count].meta_genre), "%s", field_count >= 10 ? fields[9] : "");
                    snprintf(results[count].meta_style, sizeof(results[count].meta_style), "%s", field_count >= 11 ? fields[10] : "");
                    snprintf(results[count].meta_country, sizeof(results[count].meta_country), "%s", field_count >= 12 ? fields[11] : "");
                    snprintf(results[count].meta_year, sizeof(results[count].meta_year), "%s", field_count >= 13 ? fields[12] : "");
                    sanitize_display_text(results[count].meta_genre);
                    sanitize_display_text(results[count].meta_style);
                    sanitize_display_text(results[count].meta_country);

                    if (field_count >= 6) {
                        item_url = fields[5];
                    } else if (strcmp(clean_provider, "youtube") == 0 || is_cratedig) {
                        snprintf(fallback_url, sizeof(fallback_url), "https://www.youtube.com/watch?v=%s", results[count].id);
                        item_url = fallback_url;
                    }

                    if (!sanitize_stream_url(item_url, results[count].url, sizeof(results[count].url))) {
                        continue;
                    }
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
                              const char *provider,
                              const char *query,
                              search_result_t *results,
                              int *out_count,
                              char *err,
                              size_t err_len) {
    return run_search_command_daemon(inst, provider, query, results, out_count, err, err_len);
}

/* Caller must hold search_mutex. */
static int spawn_search_thread_locked(yt_instance_t *inst, const char *provider, const char *query) {
    char clean_provider[PROVIDER_MAX];
    if (!inst || !provider || !query || query[0] == '\0') return -1;

    normalize_provider_value(provider, clean_provider, sizeof(clean_provider));
    snprintf(inst->search_provider, sizeof(inst->search_provider), "%s", clean_provider);
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

static void clear_search_locked(yt_instance_t *inst) {
    if (!inst) return;
    inst->search_query[0] = '\0';
    inst->queued_search_provider[0] = '\0';
    inst->queued_search_query[0] = '\0';
    inst->queued_search_pending = false;
    inst->search_count = 0;
    inst->search_elapsed_ms = 0;
    memset(inst->search_results, 0, sizeof(inst->search_results));
    set_search_status(inst, "idle", "");
}

static void* search_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char provider[PROVIDER_MAX];
    char query[SEARCH_QUERY_MAX];
    char next_provider[PROVIDER_MAX];
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
    snprintf(provider, sizeof(provider), "%s", inst->search_provider);
    snprintf(query, sizeof(query), "%s", inst->search_query);
    pthread_mutex_unlock(&inst->search_mutex);

    snprintf(log_msg, sizeof(log_msg), "search started provider=%s", provider);
    yt_log(log_msg);
    start_ms = now_ms();
    rc = run_search_command(inst, provider, query, local_results, &local_count, local_err, sizeof(local_err));
    elapsed_ms = now_ms() - start_ms;

    pthread_mutex_lock(&inst->search_mutex);

    if (strcmp(inst->search_query, query) != 0 || strcmp(inst->search_provider, provider) != 0) {
        start_next = 0;
        next_provider[0] = '\0';
        next_query[0] = '\0';
        if (inst->queued_search_pending &&
            inst->queued_search_provider[0] != '\0' &&
            inst->queued_search_query[0] != '\0') {
            snprintf(next_provider, sizeof(next_provider), "%s", inst->queued_search_provider);
            snprintf(next_query, sizeof(next_query), "%s", inst->queued_search_query);
            inst->queued_search_pending = false;
            inst->queued_search_provider[0] = '\0';
            inst->queued_search_query[0] = '\0';
            start_next = 1;
        } else {
            inst->search_thread_running = false;
        }
        pthread_mutex_unlock(&inst->search_mutex);

        if (start_next) {
            pthread_mutex_lock(&inst->search_mutex);
            if (spawn_search_thread_locked(inst, next_provider, next_query) == 0) {
                snprintf(log_msg,
                         sizeof(log_msg),
                         "starting queued search provider=%s query=%s",
                         next_provider,
                         next_query);
                yt_log(log_msg);
            }
            pthread_mutex_unlock(&inst->search_mutex);
        }
        return NULL;
    }

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
             "search finished provider=%s status=%s rc=%d count=%d elapsed_ms=%llu err=%s",
             provider,
             inst->search_status,
             rc,
             local_count,
             (unsigned long long)elapsed_ms,
             local_err[0] ? local_err : "-");
    yt_log(log_msg);

    start_next = 0;
    next_provider[0] = '\0';
    next_query[0] = '\0';
    if (inst->queued_search_pending &&
        inst->queued_search_provider[0] != '\0' &&
        inst->queued_search_query[0] != '\0') {
        snprintf(next_provider, sizeof(next_provider), "%s", inst->queued_search_provider);
        snprintf(next_query, sizeof(next_query), "%s", inst->queued_search_query);
        inst->queued_search_pending = false;
        inst->queued_search_provider[0] = '\0';
        inst->queued_search_query[0] = '\0';
        start_next = 1;
    } else {
        inst->search_thread_running = false;
    }
    pthread_mutex_unlock(&inst->search_mutex);

    if (start_next) {
        pthread_mutex_lock(&inst->search_mutex);
        if (spawn_search_thread_locked(inst, next_provider, next_query) == 0) {
            snprintf(log_msg,
                     sizeof(log_msg),
                     "starting queued search provider=%s query=%s",
                     next_provider,
                     next_query);
            yt_log(log_msg);
        }
        pthread_mutex_unlock(&inst->search_mutex);
    }

    return NULL;
}

static int start_search_async(yt_instance_t *inst, const char *query) {
    char provider[PROVIDER_MAX];

    if (!inst || !query || query[0] == '\0') return -1;

    normalize_provider_value(inst->search_provider, provider, sizeof(provider));

    if (inst->search_thread_valid && !inst->search_thread_running) {
        pthread_join(inst->search_thread, NULL);
        inst->search_thread_valid = false;
    }

    if (inst->search_thread_running) {
        snprintf(inst->queued_search_provider, sizeof(inst->queued_search_provider), "%s", provider);
        snprintf(inst->queued_search_query, sizeof(inst->queued_search_query), "%s", query);
        inst->queued_search_pending = true;
        set_search_status(inst, "queued", "search queued");
        return 1;
    }

    return spawn_search_thread_locked(inst, provider, query);
}

static int resolve_stream_url_daemon(yt_instance_t *inst,
                                     const char *provider,
                                     const char *source_url,
                                     char *media_url,
                                     size_t media_url_len,
                                     char *user_agent,
                                     size_t user_agent_len,
                                     char *referer,
                                     size_t referer_len,
                                     char *err,
                                     size_t err_len) {
    char clean_provider[PROVIDER_MAX];
    char req[STREAM_URL_MAX + PROVIDER_MAX + 16];
    char line[DAEMON_LINE_MAX];
    char *fields[5];
    int field_count;

    if (!inst || !provider || !source_url || !media_url || media_url_len == 0) return -1;

    normalize_provider_value(provider, clean_provider, sizeof(clean_provider));

    pthread_mutex_lock(&inst->daemon_mutex);
    if (start_daemon_locked(inst, err, err_len) != 0) {
        pthread_mutex_unlock(&inst->daemon_mutex);
        return -1;
    }

    snprintf(req, sizeof(req), "RESOLVE\t%s\t%s\n", clean_provider, source_url);
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
                              const char *provider,
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
                                     provider,
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
    char source_provider[PROVIDER_MAX];
    char source_url[STREAM_URL_MAX];
    char media_url[STREAM_URL_MAX];
    char user_agent[HTTP_HEADER_MAX];
    char referer[HTTP_HEADER_MAX];
    char err[256];
    int rc;
    char log_msg[320];

    if (!inst) return NULL;

    pthread_mutex_lock(&inst->resolve_mutex);
    snprintf(source_provider, sizeof(source_provider), "%s", inst->stream_provider);
    snprintf(source_url, sizeof(source_url), "%s", inst->stream_url);
    pthread_mutex_unlock(&inst->resolve_mutex);
    infer_provider_from_url(source_url, source_provider, sizeof(source_provider));
    normalize_provider_value(source_provider, source_provider, sizeof(source_provider));
    snprintf(log_msg, sizeof(log_msg), "resolve started provider=%s url=%s", source_provider, source_url);
    yt_log(log_msg);

    media_url[0] = '\0';
    user_agent[0] = '\0';
    referer[0] = '\0';
    err[0] = '\0';
    {
        uint64_t resolve_start = now_ms();
        rc = resolve_stream_url(inst,
                                source_provider,
                                source_url,
                                media_url,
                                sizeof(media_url),
                                user_agent,
                                sizeof(user_agent),
                                referer,
                                sizeof(referer),
                                err,
                                sizeof(err));
        snprintf(log_msg, sizeof(log_msg), "resolve_stream_url took %llums", (unsigned long long)(now_ms() - resolve_start));
        yt_log(log_msg);
    }

    pthread_mutex_lock(&inst->resolve_mutex);
    if (strcmp(inst->stream_provider, source_provider) == 0 &&
        strcmp(inst->stream_url, source_url) == 0 &&
        source_url[0] != '\0') {
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
        snprintf(log_msg, sizeof(log_msg), "resolve finished provider=%s", source_provider);
        yt_log(log_msg);
    } else {
        snprintf(log_msg, sizeof(log_msg), "resolve failed provider=%s: %s", source_provider, err[0] ? err : "unknown");
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

static void* prefetch_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char source_url[STREAM_URL_MAX];
    char media_url[STREAM_URL_MAX];
    char user_agent[HTTP_HEADER_MAX];
    char referer[HTTP_HEADER_MAX];
    char err[256];
    int rc;
    char log_msg[320];

    if (!inst) return NULL;

    pthread_mutex_lock(&inst->prefetch_mutex);
    snprintf(source_url, sizeof(source_url), "%s", inst->prefetch_source_url);
    pthread_mutex_unlock(&inst->prefetch_mutex);

    if (source_url[0] == '\0') {
        pthread_mutex_lock(&inst->prefetch_mutex);
        inst->prefetch_thread_running = false;
        pthread_mutex_unlock(&inst->prefetch_mutex);
        return NULL;
    }

    snprintf(log_msg, sizeof(log_msg), "prefetch resolve started url=%s", source_url);
    yt_log(log_msg);

    media_url[0] = '\0';
    user_agent[0] = '\0';
    referer[0] = '\0';
    err[0] = '\0';
    rc = resolve_stream_url(inst, "youtube", source_url,
                            media_url, sizeof(media_url),
                            user_agent, sizeof(user_agent),
                            referer, sizeof(referer),
                            err, sizeof(err));

    pthread_mutex_lock(&inst->prefetch_mutex);
    if (rc == 0 && strcmp(inst->prefetch_source_url, source_url) == 0) {
        snprintf(inst->prefetch_media_url, sizeof(inst->prefetch_media_url), "%s", media_url);
        snprintf(inst->prefetch_user_agent, sizeof(inst->prefetch_user_agent), "%s", user_agent);
        snprintf(inst->prefetch_referer, sizeof(inst->prefetch_referer), "%s", referer);
        inst->prefetch_ready = true;
        snprintf(log_msg, sizeof(log_msg), "prefetch resolve done url=%s", source_url);
        yt_log(log_msg);
    } else {
        snprintf(log_msg, sizeof(log_msg), "prefetch resolve failed: %s", err[0] ? err : "unknown");
        yt_log(log_msg);
    }
    inst->prefetch_thread_running = false;
    pthread_mutex_unlock(&inst->prefetch_mutex);
    return NULL;
}

static void start_prefetch_next(yt_instance_t *inst) {
    int next_idx;
    char next_url[SEARCH_URL_MAX];

    if (!inst || !inst->cratedig_auto_advance) return;

    pthread_mutex_lock(&inst->search_mutex);
    next_idx = inst->cratedig_result_index + 1;
    if (next_idx < inst->search_count) {
        snprintf(next_url, sizeof(next_url), "%s", inst->search_results[next_idx].url);
    } else {
        next_url[0] = '\0';
    }
    pthread_mutex_unlock(&inst->search_mutex);

    if (next_url[0] == '\0') return;

    pthread_mutex_lock(&inst->prefetch_mutex);

    /* Already prefetching or prefetched this URL */
    if (strcmp(inst->prefetch_source_url, next_url) == 0) {
        pthread_mutex_unlock(&inst->prefetch_mutex);
        return;
    }

    if (inst->prefetch_thread_valid && !inst->prefetch_thread_running) {
        pthread_join(inst->prefetch_thread, NULL);
        inst->prefetch_thread_valid = false;
    }

    if (inst->prefetch_thread_running) {
        pthread_mutex_unlock(&inst->prefetch_mutex);
        return;
    }

    snprintf(inst->prefetch_source_url, sizeof(inst->prefetch_source_url), "%s", next_url);
    inst->prefetch_media_url[0] = '\0';
    inst->prefetch_user_agent[0] = '\0';
    inst->prefetch_referer[0] = '\0';
    inst->prefetch_ready = false;
    inst->prefetch_thread_running = true;

    if (pthread_create(&inst->prefetch_thread, NULL, prefetch_thread_main, inst) != 0) {
        inst->prefetch_thread_running = false;
        pthread_mutex_unlock(&inst->prefetch_mutex);
        return;
    }

    inst->prefetch_thread_valid = true;
    pthread_mutex_unlock(&inst->prefetch_mutex);
}

static bool try_use_prefetch(yt_instance_t *inst, const char *source_url) {
    bool hit = false;
    if (!inst || !source_url || source_url[0] == '\0') return false;

    pthread_mutex_lock(&inst->prefetch_mutex);
    if (inst->prefetch_ready && strcmp(inst->prefetch_source_url, source_url) == 0) {
        pthread_mutex_lock(&inst->resolve_mutex);
        inst->resolve_ready = true;
        inst->resolve_failed = false;
        snprintf(inst->resolved_media_url, sizeof(inst->resolved_media_url), "%s", inst->prefetch_media_url);
        snprintf(inst->resolved_user_agent, sizeof(inst->resolved_user_agent), "%s", inst->prefetch_user_agent);
        snprintf(inst->resolved_referer, sizeof(inst->resolved_referer), "%s", inst->prefetch_referer);
        inst->resolve_error[0] = '\0';
        pthread_mutex_unlock(&inst->resolve_mutex);
        inst->prefetch_ready = false;
        inst->prefetch_source_url[0] = '\0';
        hit = true;
        yt_log("prefetch cache hit — skipping resolve");
    }
    pthread_mutex_unlock(&inst->prefetch_mutex);
    return hit;
}

static void pump_pipe(yt_instance_t *inst) {
    uint8_t buf[4096];
    uint8_t merged[4100];
    int16_t samples[2048];

    while (inst->pipe && !inst->stream_eof) {
        /* Stop pumping when we have 45s of audio buffered ahead of play position.
         * This reserves ~15s of the 60s ring for rewind. */
        if (ring_available(inst) + 2048 >= (size_t)(MOVE_SAMPLE_RATE * 2 * 45)) {
            break;
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
            if (inst->active_stream_resolved &&
                !inst->resolved_fallback_attempted &&
                supports_legacy_fallback(inst)) {
                /* Save playback position before clearing so fallback can seek */
                inst->resume_offset_sec = (double)inst->played_samples / (double)(MOVE_SAMPLE_RATE * 2);
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
        if (inst->active_stream_resolved &&
            !inst->resolved_fallback_attempted &&
            supports_legacy_fallback(inst)) {
            /* Save playback position before clearing so fallback can seek */
            inst->resume_offset_sec = (double)inst->played_samples / (double)(MOVE_SAMPLE_RATE * 2);
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
    snprintf(inst->stream_provider, sizeof(inst->stream_provider), "youtube");
    snprintf(inst->search_provider, sizeof(inst->search_provider), "youtube");
    inst->stream_url[0] = '\0';
    inst->gain = 1.0f;
    inst->pipe_fd = -1;
    inst->stream_pid = -1;
    inst->daemon_pid = -1;

    pthread_mutex_init(&inst->search_mutex, NULL);
    pthread_mutex_init(&inst->daemon_mutex, NULL);
    pthread_mutex_init(&inst->resolve_mutex, NULL);
    pthread_mutex_init(&inst->prefetch_mutex, NULL);
    snprintf(inst->search_status, sizeof(inst->search_status), "idle");
    snprintf(inst->download_status, sizeof(inst->download_status), "idle");
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

    if (inst->prefetch_thread_valid) {
        pthread_join(inst->prefetch_thread, NULL);
        inst->prefetch_thread_valid = false;
        inst->prefetch_thread_running = false;
    }

    if (inst->download_thread_valid) {
        pthread_join(inst->download_thread, NULL);
        inst->download_thread_valid = false;
    }

    pthread_mutex_lock(&inst->daemon_mutex);
    stop_daemon_locked(inst);
    pthread_mutex_unlock(&inst->daemon_mutex);

    pthread_mutex_destroy(&inst->resolve_mutex);
    pthread_mutex_destroy(&inst->daemon_mutex);
    pthread_mutex_destroy(&inst->search_mutex);
    pthread_mutex_destroy(&inst->prefetch_mutex);
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

/* ---- WAV download -------------------------------------------------------- */

#define DOWNLOAD_DIR "/data/UserData/UserLibrary/Samples/Schwung/Webstream"

static void sanitize_filename(const char *in, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < out_len - 1; i++) {
        char c = in[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        out[j++] = c;
    }
    out[j] = '\0';
    while (j > 0 && (out[j-1] == ' ' || out[j-1] == '.')) out[--j] = '\0';
}

static int download_spawn(yt_instance_t *inst, const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        (void)setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    inst->download_pid = pid;
    return 0;
}

static void download_kill(yt_instance_t *inst) {
    if (inst->download_pid > 0) {
        kill(-inst->download_pid, SIGTERM); /* kill process group */
        kill(inst->download_pid, SIGKILL);
        waitpid(inst->download_pid, NULL, 0);
        inst->download_pid = -1;
    }
}

static void *download_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char cmd[8192];
    char safe_title[SEARCH_TEXT_MAX];
    char out_path[512];
    int status;

    yt_log("download_thread: starting");

    /* mkdir -p equivalent */
    mkdir("/data/UserData/UserLibrary", 0755);
    mkdir("/data/UserData/UserLibrary/Samples", 0755);
    mkdir("/data/UserData/UserLibrary/Samples/Schwung", 0755);
    mkdir(DOWNLOAD_DIR, 0755);

    sanitize_filename(inst->download_title, safe_title, sizeof(safe_title));
    if (safe_title[0] == '\0') snprintf(safe_title, sizeof(safe_title), "download");

    snprintf(out_path, sizeof(out_path), "%s/%s.wav", DOWNLOAD_DIR, safe_title);

    /* Avoid overwriting: append number if file exists */
    {
        FILE *probe = fopen(out_path, "r");
        if (probe) {
            fclose(probe);
            for (int n = 2; n < 100; n++) {
                snprintf(out_path, sizeof(out_path), "%s/%s (%d).wav",
                         DOWNLOAD_DIR, safe_title, n);
                probe = fopen(out_path, "r");
                if (!probe) break;
                fclose(probe);
            }
        }
    }

    /* Prefer resolved media URL (avoids re-resolving and disrupting stream) */
    if (inst->download_resolved_url[0] != '\0') {
        char ua_flag[512] = "";
        char ref_flag[512] = "";
        if (inst->download_user_agent[0] != '\0')
            snprintf(ua_flag, sizeof(ua_flag),
                     "-user_agent \"%s\" ", inst->download_user_agent);
        if (inst->download_referer[0] != '\0')
            snprintf(ref_flag, sizeof(ref_flag),
                     "-referer \"%s\" ", inst->download_referer);

        snprintf(cmd, sizeof(cmd),
            "\"%s/bin/ffmpeg\" -hide_banner -loglevel warning "
            "%s%s"
            "-i '%s' -vn -sn -dn "
            "-af \"aresample=44100\" "
            "-ac 2 -ar 44100 '%s' -y "
            "2>>" WS_RUNTIME_LOG_PATH,
            inst->module_dir, ua_flag, ref_flag,
            inst->download_resolved_url, out_path);
        yt_log("download_thread: using resolved URL");
    } else {
        /* Fallback: use yt-dlp (will re-resolve) */
        char provider[PROVIDER_MAX];
        const char *legacy_fmt = "bestaudio[ext=m4a]/bestaudio";
        const char *extractor_args = "--extractor-args \"youtube:player_skip=js\" ";
        normalize_provider_value(inst->download_source_provider, provider, sizeof(provider));
        if (strcmp(provider, "soundcloud") == 0) {
            legacy_fmt = "http_mp3_1_0/hls_mp3_1_0/bestaudio";
            extractor_args = "";
        }
        snprintf(cmd, sizeof(cmd),
            "\"%s/bin/yt-dlp\" --no-playlist "
            "%s"
            "-f \"%s\" -o - '%s' 2>/dev/null | "
            "\"%s/bin/ffmpeg\" -hide_banner -loglevel warning "
            "-i pipe:0 -vn -sn -dn "
            "-af \"aresample=44100\" "
            "-ac 2 -ar 44100 '%s' -y "
            "2>>" WS_RUNTIME_LOG_PATH,
            inst->module_dir, extractor_args, legacy_fmt,
            inst->download_source_url,
            inst->module_dir, out_path);
        yt_log("download_thread: using yt-dlp fallback");
    }

    if (inst->download_cancel) goto cancelled;

    snprintf(inst->download_active_path, sizeof(inst->download_active_path), "%s", out_path);

    if (download_spawn(inst, cmd) != 0) {
        snprintf(inst->download_error, sizeof(inst->download_error), "failed to start download");
        snprintf(inst->download_status, sizeof(inst->download_status), "error");
        inst->download_thread_running = false;
        return NULL;
    }

    /* Wait for process, checking cancel flag */
    while (1) {
        int w = waitpid(inst->download_pid, &status, WNOHANG);
        if (w > 0) break;          /* process exited */
        if (w < 0) break;          /* error */
        if (inst->download_cancel) {
            download_kill(inst);
            goto cancelled;
        }
        usleep(200000); /* 200ms */
    }
    inst->download_pid = -1;

    if (inst->download_cancel) goto cancelled;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        FILE *check = fopen(out_path, "r");
        if (check) {
            fseek(check, 0, SEEK_END);
            long sz = ftell(check);
            fclose(check);
            if (sz > 44) {
                snprintf(inst->download_path, sizeof(inst->download_path), "%s", out_path);
                snprintf(inst->download_status, sizeof(inst->download_status), "done");
                yt_log("download_thread: success");
            } else {
                snprintf(inst->download_error, sizeof(inst->download_error), "empty file");
                snprintf(inst->download_status, sizeof(inst->download_status), "error");
                remove(out_path);
            }
        } else {
            snprintf(inst->download_error, sizeof(inst->download_error), "output not created");
            snprintf(inst->download_status, sizeof(inst->download_status), "error");
        }
    } else {
        snprintf(inst->download_error, sizeof(inst->download_error), "download failed");
        snprintf(inst->download_status, sizeof(inst->download_status), "error");
        remove(out_path);
    }

    inst->download_thread_running = false;
    return NULL;

cancelled:
    remove(out_path);
    snprintf(inst->download_status, sizeof(inst->download_status), "cancelled");
    yt_log("download_thread: cancelled");
    inst->download_thread_running = false;
    return NULL;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    yt_instance_t *inst = (yt_instance_t *)instance;
    char log_msg[384];
    if (!inst || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        float gain;
        if (gain_state_decode(val, &gain) == 0) {
            char value[32];
            snprintf(value, sizeof(value), "%.6f", gain);
            v2_set_param(instance, "gain", value);
        }
        return;
    }

    if (strcmp(key, "gain") == 0) {
        float g = (float)atof(val);
        if (g < 0.0f) g = 0.0f;
        if (g > 2.0f) g = 2.0f;
        inst->gain = g;
        return;
    }

    if (strcmp(key, "stream_url") == 0) {
        char clean_url[STREAM_URL_MAX];
        char clean_provider[PROVIDER_MAX];
        if (val[0] == '\0') {
            stop_everything(inst);
            return;
        }

        if (!sanitize_stream_url(val, clean_url, sizeof(clean_url))) {
            set_error(inst, "invalid stream_url");
            return;
        }

        snprintf(clean_provider, sizeof(clean_provider), "%s", inst->stream_provider);
        infer_provider_from_url(clean_url, clean_provider, sizeof(clean_provider));
        normalize_provider_value(clean_provider, clean_provider, sizeof(clean_provider));
        snprintf(inst->stream_url, sizeof(inst->stream_url), "%s", clean_url);
        pthread_mutex_lock(&inst->resolve_mutex);
        snprintf(inst->stream_provider, sizeof(inst->stream_provider), "%s", clean_provider);
        inst->resolve_ready = false;
        inst->resolve_failed = false;
        inst->resolved_media_url[0] = '\0';
        inst->resolved_user_agent[0] = '\0';
        inst->resolved_referer[0] = '\0';
        inst->resolve_error[0] = '\0';
        pthread_mutex_unlock(&inst->resolve_mutex);
        snprintf(log_msg, sizeof(log_msg), "stream_url set provider=%s url=%s", clean_provider, clean_url);
        yt_log(log_msg);
        restart_stream_from_beginning(inst, 0);
        if (prefer_legacy_pipeline(inst)) {
            snprintf(log_msg, sizeof(log_msg), "stream_url using legacy pipeline provider=%s", clean_provider);
            yt_log(log_msg);
            if (start_stream_legacy(inst) != 0) {
                inst->stream_eof = true;
                inst->restart_countdown = 0;
            }
        } else {
            (void)start_resolve_async(inst);
        }
        return;
    }

    if (strcmp(key, "stream_provider") == 0) {
        char clean_provider[PROVIDER_MAX];
        normalize_provider_value(val, clean_provider, sizeof(clean_provider));
        pthread_mutex_lock(&inst->resolve_mutex);
        snprintf(inst->stream_provider, sizeof(inst->stream_provider), "%s", clean_provider);
        pthread_mutex_unlock(&inst->resolve_mutex);
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
            if (prefer_legacy_pipeline(inst)) {
                if (start_stream_legacy(inst) != 0) {
                    inst->stream_eof = true;
                    inst->restart_countdown = 0;
                }
            } else {
                (void)start_resolve_async(inst);
            }
        }
        return;
    }

    if (strcmp(key, "restart_step") == 0) {
        if (parse_trigger_value(val, &inst->restart_step)) {
            if (allow_trigger(&inst->last_restart_ms, DEBOUNCE_RESTART_MS) &&
                inst->stream_url[0] != '\0') {
                restart_stream_from_beginning(inst, 0);
                if (prefer_legacy_pipeline(inst)) {
                    if (start_stream_legacy(inst) != 0) {
                        inst->stream_eof = true;
                        inst->restart_countdown = 0;
                    }
                } else {
                    (void)start_resolve_async(inst);
                }
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

    if (strcmp(key, "next_track_step") == 0) {
        if (parse_trigger_value(val, &inst->next_track_step) &&
            allow_trigger(&inst->last_next_track_ms, DEBOUNCE_PLAY_PAUSE_MS)) {
            pthread_mutex_lock(&inst->search_mutex);
            if (inst->search_count > 0) {
                int next_idx = inst->cratedig_result_index + 1;
                if (next_idx >= inst->search_count) next_idx = 0;
                inst->cratedig_result_index = next_idx;
                if (next_idx < inst->search_count) {
                    char next_url[SEARCH_URL_MAX];
                    snprintf(next_url, sizeof(next_url), "%s", inst->search_results[next_idx].url);
                    pthread_mutex_unlock(&inst->search_mutex);
                    stop_stream(inst);
                    snprintf(inst->stream_provider, sizeof(inst->stream_provider), "youtube");
                    snprintf(inst->stream_url, sizeof(inst->stream_url), "%s", next_url);
                    restart_stream_from_beginning(inst, 0);
                    if (!try_use_prefetch(inst, next_url)) {
                        pthread_mutex_lock(&inst->resolve_mutex);
                        inst->resolve_ready = false;
                        inst->resolve_failed = false;
                        inst->resolved_media_url[0] = '\0';
                        inst->resolved_user_agent[0] = '\0';
                        inst->resolved_referer[0] = '\0';
                        inst->resolve_error[0] = '\0';
                        pthread_mutex_unlock(&inst->resolve_mutex);
                        (void)start_resolve_async(inst);
                    }
                    return;
                }
            }
            pthread_mutex_unlock(&inst->search_mutex);
        }
        return;
    }

    if (strcmp(key, "cratedig_result_index") == 0) {
        int idx = (int)strtol(val, NULL, 10);
        if (idx >= 0 && idx < SEARCH_MAX_RESULTS) {
            inst->cratedig_result_index = idx;
        }
        return;
    }

    if (strcmp(key, "cratedig_auto_advance") == 0) {
        inst->cratedig_auto_advance = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        return;
    }

    if (strcmp(key, "cratedig_filter") == 0) {
        /* Stash filter for the search thread to send (non-blocking) */
        pthread_mutex_lock(&inst->search_mutex);
        snprintf(inst->cratedig_pending_filter, sizeof(inst->cratedig_pending_filter), "%s", val);
        snprintf(inst->search_provider, sizeof(inst->search_provider), "cratedig");
        inst->cratedig_result_index = 0;
        (void)start_search_async(inst, "cratedig");
        pthread_mutex_unlock(&inst->search_mutex);
        return;
    }

    if (strcmp(key, "search_query") == 0) {
        pthread_mutex_lock(&inst->search_mutex);
        if (val[0] == '\0') {
            clear_search_locked(inst);
        } else {
            (void)start_search_async(inst, val);
        }
        pthread_mutex_unlock(&inst->search_mutex);
        return;
    }

    if (strcmp(key, "search_provider") == 0) {
        char clean_provider[PROVIDER_MAX];
        normalize_provider_value(val, clean_provider, sizeof(clean_provider));
        pthread_mutex_lock(&inst->search_mutex);
        snprintf(inst->search_provider, sizeof(inst->search_provider), "%s", clean_provider);
        pthread_mutex_unlock(&inst->search_mutex);
        return;
    }

    if (strcmp(key, "download_wav") == 0) {
        if (inst->download_thread_running) return;
        if (inst->stream_url[0] == '\0') {
            snprintf(inst->download_error, sizeof(inst->download_error), "nothing playing");
            snprintf(inst->download_status, sizeof(inst->download_status), "error");
            return;
        }
        if (inst->download_thread_valid) {
            pthread_join(inst->download_thread, NULL);
            inst->download_thread_valid = false;
        }

        /* Capture stream info before stopping */
        snprintf(inst->download_source_url, sizeof(inst->download_source_url),
                 "%s", inst->stream_url);
        snprintf(inst->download_source_provider, sizeof(inst->download_source_provider),
                 "%s", inst->stream_provider);
        pthread_mutex_lock(&inst->resolve_mutex);
        snprintf(inst->download_resolved_url, sizeof(inst->download_resolved_url),
                 "%s", inst->resolved_media_url);
        snprintf(inst->download_user_agent, sizeof(inst->download_user_agent),
                 "%s", inst->resolved_user_agent);
        snprintf(inst->download_referer, sizeof(inst->download_referer),
                 "%s", inst->resolved_referer);
        pthread_mutex_unlock(&inst->resolve_mutex);

        if (val[0] != '\0' && strcmp(val, "trigger") != 0) {
            snprintf(inst->download_title, sizeof(inst->download_title), "%s", val);
        } else {
            snprintf(inst->download_title, sizeof(inst->download_title), "webstream");
        }

        /* Stop the stream so yt-dlp/ffmpeg don't conflict */
        stop_stream(inst);
        inst->paused = true;

        inst->download_path[0] = '\0';
        inst->download_error[0] = '\0';
        inst->download_cancel = false;
        inst->download_pid = -1;
        inst->download_thread_running = true;
        snprintf(inst->download_status, sizeof(inst->download_status), "downloading");

        if (pthread_create(&inst->download_thread, NULL, download_thread_main, inst) == 0) {
            inst->download_thread_valid = true;
            yt_log("download_wav: thread started");
        } else {
            inst->download_thread_running = false;
            snprintf(inst->download_error, sizeof(inst->download_error), "thread create failed");
            snprintf(inst->download_status, sizeof(inst->download_status), "error");
        }
        return;
    }

    if (strcmp(key, "download_cancel") == 0) {
        if (inst->download_thread_running) {
            inst->download_cancel = true;
            yt_log("download_cancel: requested");
        }
        return;
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

    if (strcmp(key, "state") == 0) {
        if (!inst) return -1;
        return gain_state_encode(inst->gain, buf, buf_len);
    }

    if (strcmp(key, "gain") == 0) {
        return snprintf(buf, (size_t)buf_len, "%.2f", inst ? inst->gain : 1.0f);
    }
    if (strcmp(key, "playback_time") == 0) {
        if (!inst) return snprintf(buf, (size_t)buf_len, "0:00");
        uint64_t total_sec = inst->play_abs / ((uint64_t)MOVE_SAMPLE_RATE * 2ULL);
        unsigned int min = (unsigned int)(total_sec / 60);
        unsigned int sec = (unsigned int)(total_sec % 60);
        return snprintf(buf, (size_t)buf_len, "%u:%02u", min, sec);
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
        return snprintf(buf, (size_t)buf_len, "Webstream");
    }
    if (strcmp(key, "next_track_step") == 0) {
        return snprintf(buf, (size_t)buf_len, "idle");
    }
    if (strcmp(key, "cratedig_result_index") == 0) {
        return snprintf(buf, (size_t)buf_len, "%d", inst ? inst->cratedig_result_index : 0);
    }
    if (strcmp(key, "cratedig_auto_advance") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", (inst && inst->cratedig_auto_advance) ? "1" : "0");
    }
    if (strcmp(key, "stream_url") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", inst ? inst->stream_url : "");
    }
    if (strcmp(key, "stream_provider") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s", inst ? inst->stream_provider : "youtube");
    }
    if (strcmp(key, "download_status") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s",
                        inst ? inst->download_status : "idle");
    }
    if (strcmp(key, "download_path") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s",
                        inst ? inst->download_path : "");
    }
    if (strcmp(key, "download_error") == 0) {
        return snprintf(buf, (size_t)buf_len, "%s",
                        inst ? inst->download_error : "");
    }
    if (strcmp(key, "download_progress") == 0) {
        struct stat st;
        if (!inst || inst->download_active_path[0] == '\0' ||
            stat(inst->download_active_path, &st) != 0)
            return snprintf(buf, (size_t)buf_len, "0");
        return snprintf(buf, (size_t)buf_len, "%ld", (long)st.st_size);
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
    if (inst && strcmp(key, "search_provider") == 0) {
        int ret;
        pthread_mutex_lock(&inst->search_mutex);
        ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_provider);
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

        idx = get_result_index(key, "search_result_provider_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].provider);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_key_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_key);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_scale_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_scale);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_tempo_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_tempo);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_genre_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_genre);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_style_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_style);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_country_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_country);
            pthread_mutex_unlock(&inst->search_mutex);
            return ret;
        }

        idx = get_result_index(key, "search_result_year_");
        if (idx >= 0) {
            int ret = -1;
            pthread_mutex_lock(&inst->search_mutex);
            if (idx < inst->search_count) ret = snprintf(buf, (size_t)buf_len, "%s", inst->search_results[idx].meta_year);
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

    /* Anti-idle: host idle-gates slots whose output stays below abs(4).
     * Inject keepalive before early returns; re-inject after ring_pop at end. */
    out_interleaved_lr[0] = 5;

    if (inst->stream_eof) {
        if (inst->cratedig_auto_advance) {
            int next_idx;
            pthread_mutex_lock(&inst->search_mutex);
            next_idx = inst->cratedig_result_index + 1;
            if (next_idx < inst->search_count) {
                char next_url[SEARCH_URL_MAX];
                inst->cratedig_result_index = next_idx;
                snprintf(next_url, sizeof(next_url), "%s", inst->search_results[next_idx].url);
                pthread_mutex_unlock(&inst->search_mutex);
                stop_stream(inst);
                inst->stream_eof = false;
                snprintf(inst->stream_provider, sizeof(inst->stream_provider), "youtube");
                snprintf(inst->stream_url, sizeof(inst->stream_url), "%s", next_url);
                if (!try_use_prefetch(inst, next_url)) {
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
                } else {
                    restart_stream_from_beginning(inst, 0);
                }
            } else {
                pthread_mutex_unlock(&inst->search_mutex);
            }
        }
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
                } else {
                    start_prefetch_next(inst);
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
        size_t avail = ring_available(inst);
        if (avail < inst->prime_needed_samples && !inst->stream_eof) {
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

    /* Re-inject anti-idle after ring_pop/gain may have overwritten position 0 */
    if (out_interleaved_lr[0] < 5 && out_interleaved_lr[0] > -5)
        out_interleaved_lr[0] = 5;
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
    yt_log("webstream plugin v2 initialized");
    return &g_plugin_api_v2;
}
