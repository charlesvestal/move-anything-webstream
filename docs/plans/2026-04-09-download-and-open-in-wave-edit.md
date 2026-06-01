# Download WAV & Open in Wave Edit — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a "Open in Wave Edit" action to the Now Playing menu that downloads the current stream as WAV using yt-dlp/ffmpeg, saves it to `/data/UserData/Samples/Schwung/Webstream/`, then opens Wave Edit with that file.

**Architecture:** Three layers of changes: (1) C plugin gets a background download thread triggered by a new `download_wav` param, with status/path readable via `get_param`. (2) Host shadow_ui.js exposes a new `host_open_file_in_tool(filePath, toolId)` global to chain module UIs, which finds the tool in `toolModules` and calls `startInteractiveTool`. (3) Webstream UI adds the menu action, triggers download, polls for completion, then calls the new host API.

**Tech Stack:** C (DSP plugin), JavaScript/QuickJS (UI + host shadow)

---

### Task 1: Add download fields to C plugin instance struct

**Files:**
- Modify: `src/dsp/yt_stream_plugin.c:70-160` (yt_instance_t struct)

**Step 1: Add download state fields to yt_instance_t**

After the `search_results` field (line 159), before the closing `}`, add:

```c
    /* WAV download */
    pthread_t download_thread;
    bool download_thread_valid;
    bool download_thread_running;
    char download_status[32];       /* idle, downloading, done, error */
    char download_path[512];        /* output file path after success */
    char download_error[256];
    char download_source_url[STREAM_URL_MAX];
    char download_source_provider[PROVIDER_MAX];
    char download_title[SEARCH_TEXT_MAX]; /* for filename */
```

**Step 2: Initialize fields in v2_create_instance**

Find `v2_create_instance` and add after other field initialization:

```c
    snprintf(inst->download_status, sizeof(inst->download_status), "idle");
    inst->download_thread_valid = false;
    inst->download_thread_running = false;
    inst->download_path[0] = '\0';
    inst->download_error[0] = '\0';
    inst->download_source_url[0] = '\0';
    inst->download_source_provider[0] = '\0';
    inst->download_title[0] = '\0';
```

**Step 3: Commit**

```bash
git add src/dsp/yt_stream_plugin.c
git commit -m "feat(dsp): add download state fields to webstream instance"
```

---

### Task 2: Add download thread function in C plugin

**Files:**
- Modify: `src/dsp/yt_stream_plugin.c` (add new function before `v2_set_param`)

**Step 1: Add sanitize_filename helper and download_thread_main**

Add before `v2_set_param` (around line 2055):

```c
#define DOWNLOAD_DIR "/data/UserData/Samples/Schwung/Webstream"

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
    /* Trim trailing spaces/dots */
    while (j > 0 && (out[j-1] == ' ' || out[j-1] == '.')) out[--j] = '\0';
}

static void *download_thread_main(void *arg) {
    yt_instance_t *inst = (yt_instance_t *)arg;
    char cmd[8192];
    char safe_title[SEARCH_TEXT_MAX];
    char out_path[512];
    char provider[PROVIDER_MAX];
    int ret;

    snprintf(inst->download_status, sizeof(inst->download_status), "downloading");
    yt_log("download_thread: starting");

    /* Ensure output directory exists */
    mkdir(DOWNLOAD_DIR, 0755);

    /* Build safe filename from title */
    sanitize_filename(inst->download_title, safe_title, sizeof(safe_title));
    if (safe_title[0] == '\0') snprintf(safe_title, sizeof(safe_title), "download");

    snprintf(out_path, sizeof(out_path), "%s/%s.wav", DOWNLOAD_DIR, safe_title);

    /* Avoid overwriting: append number if exists */
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

    normalize_provider_value(inst->download_source_provider, provider, sizeof(provider));

    /* Use yt-dlp + ffmpeg to download as WAV */
    {
        const char *legacy_fmt = "bestaudio[ext=m4a]/bestaudio";
        const char *extractor_args = "--extractor-args \"youtube:player_skip=js\" ";
        if (strcmp(provider, "soundcloud") == 0) {
            legacy_fmt = "http_mp3_1_0/hls_mp3_1_0/bestaudio";
            extractor_args = "";
        }

        snprintf(cmd, sizeof(cmd),
            "\"%s/bin/yt-dlp\" --no-playlist "
            "%s"
            "-f \"%s\" -o - \"%s\" 2>/dev/null | "
            "\"%s/bin/ffmpeg\" -hide_banner -loglevel error "
            "-i pipe:0 -vn -sn -dn "
            "-af \"aresample=48000\" "
            "-ac 2 -ar 48000 \"%s\" -y",
            inst->module_dir, extractor_args, legacy_fmt,
            inst->download_source_url,
            inst->module_dir, out_path);
    }

    {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "download_thread: cmd=%s", cmd);
        yt_log(log_msg);
    }

    ret = system(cmd);
    if (ret == 0) {
        /* Verify file exists and is non-empty */
        FILE *check = fopen(out_path, "r");
        if (check) {
            fseek(check, 0, SEEK_END);
            long sz = ftell(check);
            fclose(check);
            if (sz > 44) { /* WAV header is 44 bytes minimum */
                snprintf(inst->download_path, sizeof(inst->download_path), "%s", out_path);
                snprintf(inst->download_status, sizeof(inst->download_status), "done");
                yt_log("download_thread: success");
            } else {
                snprintf(inst->download_error, sizeof(inst->download_error), "download produced empty file");
                snprintf(inst->download_status, sizeof(inst->download_status), "error");
                remove(out_path);
                yt_log("download_thread: empty file");
            }
        } else {
            snprintf(inst->download_error, sizeof(inst->download_error), "output file not created");
            snprintf(inst->download_status, sizeof(inst->download_status), "error");
            yt_log("download_thread: no output file");
        }
    } else {
        snprintf(inst->download_error, sizeof(inst->download_error), "yt-dlp/ffmpeg failed (%d)", ret);
        snprintf(inst->download_status, sizeof(inst->download_status), "error");
        yt_log("download_thread: command failed");
    }

    inst->download_thread_running = false;
    return NULL;
}
```

**Step 2: Add `#include <sys/stat.h>` near the top if not already present**

Check includes — `mkdir` needs it. Add after existing includes if missing:

```c
#include <sys/stat.h>
```

**Step 3: Commit**

```bash
git add src/dsp/yt_stream_plugin.c
git commit -m "feat(dsp): add WAV download thread with yt-dlp/ffmpeg"
```

---

### Task 3: Wire up download params in set_param / get_param

**Files:**
- Modify: `src/dsp/yt_stream_plugin.c` — `v2_set_param` and `v2_get_param`

**Step 1: Add download_wav handler in v2_set_param**

Add before the closing `}` of `v2_set_param` (before line 2286), after the `search_provider` handler:

```c
    if (strcmp(key, "download_wav") == 0) {
        /* Don't start if already downloading */
        if (inst->download_thread_running) return;
        if (inst->stream_url[0] == '\0') {
            snprintf(inst->download_error, sizeof(inst->download_error), "nothing playing");
            snprintf(inst->download_status, sizeof(inst->download_status), "error");
            return;
        }

        /* Join previous thread if any */
        if (inst->download_thread_valid) {
            pthread_join(inst->download_thread, NULL);
            inst->download_thread_valid = false;
        }

        /* Copy current stream info for download */
        snprintf(inst->download_source_url, sizeof(inst->download_source_url),
                 "%s", inst->stream_url);
        snprintf(inst->download_source_provider, sizeof(inst->download_source_provider),
                 "%s", inst->stream_provider);
        /* Title comes from val if provided, otherwise use URL */
        if (val[0] != '\0' && strcmp(val, "trigger") != 0) {
            snprintf(inst->download_title, sizeof(inst->download_title), "%s", val);
        } else {
            snprintf(inst->download_title, sizeof(inst->download_title), "webstream");
        }

        inst->download_path[0] = '\0';
        inst->download_error[0] = '\0';
        inst->download_thread_running = true;
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
```

**Step 2: Add download status/path getters in v2_get_param**

Add before the search result getters in `v2_get_param`:

```c
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
```

**Step 3: Join download thread in v2_destroy_instance**

Find `v2_destroy_instance` and add cleanup before `free(inst)`:

```c
    if (inst->download_thread_valid) {
        pthread_join(inst->download_thread, NULL);
        inst->download_thread_valid = false;
    }
```

**Step 4: Commit**

```bash
git add src/dsp/yt_stream_plugin.c
git commit -m "feat(dsp): wire up download_wav set/get params and cleanup"
```

---

### Task 4: Add host_open_file_in_tool to shadow_ui.js

**Files:**
- Modify: `schwung/src/shadow/shadow_ui.js:2067-2085` (setupModuleParamShims function)

**Step 1: Add host_open_file_in_tool to setupModuleParamShims**

After the `host_swap_module` definition (after line 2084), add:

```javascript
    globalThis.host_open_file_in_tool = function(filePath, toolId) {
        if (!filePath || !toolId) return false;
        /* Ensure tool list is populated */
        if (!toolModules || !toolModules.length) {
            toolModules = scanForToolModules();
        }
        const tool = toolModules.find(t => t.id === toolId);
        if (!tool) {
            debugLog("host_open_file_in_tool: tool not found: " + toolId);
            return false;
        }
        debugLog("host_open_file_in_tool: opening " + filePath + " in " + toolId);
        unloadModuleUi();
        startInteractiveTool(tool, filePath);
        return true;
    };
```

**Step 2: Clean up the new global in clearModuleParamShims**

In `clearModuleParamShims` (around line 2088), add:

```javascript
    delete globalThis.host_open_file_in_tool;
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(host): expose host_open_file_in_tool for chain module UIs"
```

---

### Task 5: Add "Open in Wave Edit" to webstream UI Now Playing menu

**Files:**
- Modify: `schwung-webstream/src/ui.js:767-800` (openNowPlayingMenu function)

**Step 1: Add the download + open action to Now Playing menu**

After line 795 (`if (r.meta_year) ...`), before the `menuStack.push`, add:

```javascript
  if (r.url) {
    items.push(createAction('[Open in Wave Edit]', function() {
      if (typeof host_module_get_param !== 'function') return;
      const status = host_module_get_param('download_status');
      if (status === 'downloading') {
        // Already downloading, just show status
        openDownloadStatusMenu(r);
        return;
      }
      // Reset and trigger download, passing title for filename
      host_module_set_param('download_wav', r.title || r.channel || 'webstream');
      openDownloadStatusMenu(r);
    }));
  }
```

**Step 2: Add the download status polling menu function**

Add a new function after `openNowPlayingMenu`:

```javascript
function openDownloadStatusMenu(r) {
  let pollTimer = null;

  function cleanup() {
    if (pollTimer !== null) {
      os.clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function refreshStatus() {
    const status = host_module_get_param('download_status');
    const items = [];
    items.push(createAction('[Back]', function() {
      cleanup();
      menuStack.pop();
      menuState.selectedIndex = 0;
      needsRedraw = true;
    }));

    if (status === 'downloading') {
      items.push(createAction('Downloading...', () => {}));
      items.push(createAction('(this may take a moment)', () => {}));
    } else if (status === 'done') {
      const path = host_module_get_param('download_path') || '';
      const filename = path.split('/').pop() || 'file';
      items.push(createAction('Saved: ' + filename, () => {}));
      if (typeof host_open_file_in_tool === 'function') {
        items.push(createAction('[Open in Wave Edit]', function() {
          cleanup();
          host_open_file_in_tool(path, 'waveform-editor');
        }));
      }
    } else if (status === 'error') {
      const err = host_module_get_param('download_error') || 'unknown error';
      items.push(createAction('Error: ' + err, () => {}));
    } else {
      items.push(createAction('Starting download...', () => {}));
    }

    // Replace current menu
    const top = menuStack.peek();
    if (top) {
      top.items = items;
      top.title = 'Download';
    }
    needsRedraw = true;

    // Keep polling while downloading
    if (status === 'downloading' || status === 'idle') {
      pollTimer = os.setTimeout(refreshStatus, 500);
    }
  }

  menuStack.push({ title: 'Download', items: [createAction('Starting...', () => {})], selectedIndex: 0 });
  menuState.selectedIndex = 0;
  needsRedraw = true;

  // Start polling after a brief delay
  pollTimer = os.setTimeout(refreshStatus, 300);
}
```

**Step 3: Commit**

```bash
git add src/ui.js
git commit -m "feat(ui): add 'Open in Wave Edit' download action to Now Playing menu"
```

---

## Summary of changes by repo

| Repo | File | Change |
|------|------|--------|
| schwung-webstream | `src/dsp/yt_stream_plugin.c` | Download thread, set/get params, instance fields |
| schwung-webstream | `src/ui.js` | Download menu action + status polling |
| schwung (host) | `src/shadow/shadow_ui.js` | New `host_open_file_in_tool` global for chain modules |

## Testing

1. Play any YouTube track in webstream
2. Navigate to Now Playing menu
3. Select "Open in Wave Edit"
4. Verify download status shows "Downloading..."
5. After download completes, verify "Open in Wave Edit" button appears
6. Press it — Wave Edit should open with the downloaded WAV
7. Check `/data/UserData/Samples/Schwung/Webstream/` has the WAV file
8. Test duplicate filename handling (download same track twice)
9. Test error case (stop playback, try download)
