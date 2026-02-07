import {
  openTextEntry,
  isTextEntryActive,
  handleTextEntryMidi,
  drawTextEntry,
  tickTextEntry
} from '/data/UserData/move-anything/shared/text_entry.mjs';

import {
  MoveMainKnob,
  MoveMainButton,
  MoveBack
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta, isCapacitiveTouchMessage } from '/data/UserData/move-anything/shared/input_filter.mjs';

let searchQuery = '';
let searchStatus = 'idle';
let searchError = '';
let searchCount = 0;
let searchElapsedMs = 0;
let selectedIndex = 0;
let statusMessage = 'Click: search';
let streamStatus = 'stopped';

let results = [];
let tickCounter = 0;

function truncate(text, maxLen) {
  if (!text) return '';
  if (text.length <= maxLen) return text;
  return text.slice(0, Math.max(0, maxLen - 1)) + '…';
}

function loadResults() {
  const out = [];
  for (let i = 0; i < searchCount; i++) {
    const title = host_module_get_param(`search_result_title_${i}`) || '';
    const channel = host_module_get_param(`search_result_channel_${i}`) || '';
    const duration = host_module_get_param(`search_result_duration_${i}`) || '';
    const url = host_module_get_param(`search_result_url_${i}`) || '';
    out.push({ title, channel, duration, url });
  }
  results = out;
  if (selectedIndex >= results.length) {
    selectedIndex = Math.max(0, results.length - 1);
  }
}

function refreshState() {
  const prevStatus = searchStatus;
  const prevCount = searchCount;

  streamStatus = host_module_get_param('stream_status') || 'stopped';
  searchQuery = host_module_get_param('search_query') || '';
  searchStatus = host_module_get_param('search_status') || 'idle';
  searchError = host_module_get_param('search_error') || '';
  searchCount = parseInt(host_module_get_param('search_count') || '0', 10) || 0;
  searchElapsedMs = parseInt(host_module_get_param('search_elapsed_ms') || '0', 10) || 0;

  if (prevStatus !== searchStatus || prevCount !== searchCount) {
    loadResults();
  }
}

function openSearchPrompt() {
  openTextEntry({
    title: 'Search YouTube',
    initialText: searchQuery,
    onConfirm: (text) => {
      const query = (text || '').trim();
      if (!query) {
        statusMessage = 'Search cancelled';
        return;
      }

      selectedIndex = 0;
      statusMessage = `Searching: ${truncate(query, 14)}`;
      host_module_set_param('search_query', query);
    },
    onCancel: () => {
      statusMessage = 'Search cancelled';
    }
  });
}

function selectCurrentResult() {
  if (selectedIndex < 0 || selectedIndex >= results.length) return;
  const row = results[selectedIndex];
  if (!row || !row.url) return;

  statusMessage = `Load #${selectedIndex + 1}`;
  host_module_set_param('stream_url', row.url);
}

function drawHeader() {
  clear_screen();
  print(2, 2, 'YT Stream', 1);
  fill_rect(0, 11, 128, 1, 1);

  print(2, 14, `Stream: ${truncate(streamStatus, 12)}`, 1);

  const q = searchQuery ? truncate(searchQuery, 16) : '(none)';
  print(2, 24, `Q: ${q}`, 1);

  const s = `${searchStatus} ${searchElapsedMs}ms`;
  print(2, 34, `Search: ${truncate(s, 16)}`, 1);
}

function drawResults() {
  if (searchStatus === 'searching') {
    print(2, 45, 'Searching...', 1);
    return;
  }

  if (searchStatus === 'error') {
    print(2, 45, truncate(searchError || 'Search error', 20), 1);
    return;
  }

  if (results.length === 0) {
    print(2, 45, 'No results yet', 1);
    return;
  }

  const row = results[selectedIndex];
  print(2, 45, `${selectedIndex + 1}/${results.length} ${truncate(row.title, 15)}`, 1);
  print(2, 55, `${truncate(row.channel, 10)} ${truncate(row.duration, 8)}`, 1);
}

function drawFooter() {
  fill_rect(0, 62, 128, 1, 1);
}

globalThis.init = function () {
  searchQuery = '';
  searchStatus = 'idle';
  searchError = '';
  searchCount = 0;
  searchElapsedMs = 0;
  selectedIndex = 0;
  statusMessage = 'Click: search';
  streamStatus = 'stopped';
  results = [];
  tickCounter = 0;
};

globalThis.tick = function () {
  if (isTextEntryActive()) {
    tickTextEntry();
    drawTextEntry();
    return;
  }

  /* Poll host params at ~10Hz to keep UI cheap. */
  tickCounter = (tickCounter + 1) % 6;
  if (tickCounter === 0) {
    refreshState();
  }

  drawHeader();
  drawResults();
  drawFooter();
};

globalThis.onMidiMessageInternal = function (data) {
  const status = data[0] & 0xF0;
  const cc = data[1];
  const val = data[2];

  if (isCapacitiveTouchMessage(data)) return;

  if (isTextEntryActive()) {
    handleTextEntryMidi(data);
    return;
  }

  if (status !== 0xB0) return;

  const isDown = val > 0;

  if (cc === MoveMainKnob) {
    if (results.length > 0) {
      const delta = decodeDelta(val);
      if (delta !== 0) {
        selectedIndex += delta;
        if (selectedIndex < 0) selectedIndex = 0;
        if (selectedIndex >= results.length) selectedIndex = results.length - 1;
      }
    }
    return;
  }

  if (cc === MoveMainButton && isDown) {
    if (results.length > 0 && (searchStatus === 'done' || searchStatus === 'no_results')) {
      selectCurrentResult();
    } else {
      openSearchPrompt();
    }
    return;
  }

  if (cc === MoveBack && isDown) {
    host_return_to_menu();
  }
};

globalThis.onMidiMessageExternal = function (data) {
  if (isTextEntryActive()) {
    handleTextEntryMidi(data);
  }
};

/* Also expose chain_ui so shadow component loader can consume this file directly. */
globalThis.chain_ui = {
  init: globalThis.init,
  tick: globalThis.tick,
  onMidiMessageInternal: globalThis.onMidiMessageInternal,
  onMidiMessageExternal: globalThis.onMidiMessageExternal
};
