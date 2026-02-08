import * as std from 'std';
import * as os from 'os';

import {
  openTextEntry,
  isTextEntryActive,
  handleTextEntryMidi,
  drawTextEntry,
  tickTextEntry
} from '/data/UserData/move-anything/shared/text_entry.mjs';

import {
  MidiNoteOn,
  MoveShift,
  MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob7, MoveKnob8,
  MoveKnob1Touch, MoveKnob2Touch, MoveKnob3Touch, MoveKnob7Touch, MoveKnob8Touch
} from '/data/UserData/move-anything/shared/constants.mjs';

import { isCapacitiveTouchMessage, decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';

import { createAction } from '/data/UserData/move-anything/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/move-anything/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/move-anything/shared/menu_stack.mjs';
import { drawStackMenu } from '/data/UserData/move-anything/shared/menu_render.mjs';

const MAX_MENU_RESULTS = 20;
const MAX_SEARCH_HISTORY = 20;
const SEARCH_HISTORY_PATH = '/data/UserData/move-anything/config/webstream_search_history.json';
const LEGACY_SEARCH_HISTORY_PATH = '/data/UserData/move-anything/webstream_search_history.json';
const LEGACY_SEARCH_HISTORY_PATH_2 = '/data/UserData/move-anything/yt_search_history.json';
const SPINNER = ['-', '/', '|', '\\'];
const PROVIDERS = [
  { id: 'youtube', label: 'YouTube' },
  { id: 'freesound', label: 'FreeSound' },
  { id: 'archive', label: 'Archive.org' },
  { id: 'soundcloud', label: 'SoundCloud' }
];
const PROVIDER_TAGS = {
  youtube: '[YT]',
  freesound: '[FS]',
  archive: '[AR]',
  soundcloud: '[SC]'
};

let searchQuery = '';
let searchProvider = 'youtube';
let searchStatus = 'idle';
let searchCount = 0;
let streamStatus = 'stopped';
let selectedIndex = 0;
let statusMessage = 'Click: select';
let results = [];
let searchHistory = [];
let shiftHeld = false;

let menuState = createMenuState();
let menuStack = createMenuStack();

let tickCounter = 0;
let spinnerTick = 0;
let spinnerFrame = 0;
let needsRedraw = true;
let pendingKnobAction = null;

function normalizeProvider(value) {
  const raw = String(value || '').trim().toLowerCase();
  if (!raw) return 'youtube';
  if (raw === 'yt') return 'youtube';
  if (raw === 'fs') return 'freesound';
  if (raw === 'ia' || raw === 'archiveorg' || raw === 'internetarchive') return 'archive';
  if (raw === 'sc') return 'soundcloud';
  return raw;
}

function providerLabel(providerId) {
  const id = normalizeProvider(providerId);
  const found = PROVIDERS.find((p) => p.id === id);
  return found ? found.label : id;
}

function providerTag(providerId) {
  const id = normalizeProvider(providerId);
  return PROVIDER_TAGS[id] || '[??]';
}

function historyEntry(providerId, query) {
  return {
    provider: normalizeProvider(providerId),
    query: String(query || '').trim()
  };
}

function cleanLabel(text, maxLen = 24) {
  let s = String(text || '');
  s = s.replace(/[^\x20-\x7E]+/g, ' ').replace(/\s+/g, ' ').trim();
  if (!s) s = '(untitled)';
  if (s.length > maxLen) s = `${s.slice(0, Math.max(0, maxLen - 1))}…`;
  return s;
}

function currentActivityLabel() {
  if (searchStatus === 'searching') return 'Searching';
  if (searchStatus === 'queued') return 'Queued';
  if (streamStatus === 'loading') return 'Loading';
  if (streamStatus === 'buffering') return 'Buffering';
  if (streamStatus === 'seeking') return 'Seeking';
  return '';
}

function setPendingKnobAction(cc, action, prompt) {
  pendingKnobAction = { cc, action };
  statusMessage = prompt;
  needsRedraw = true;
}

function runKnobAction(action) {
  if (action === 'play_pause') {
    host_module_set_param('play_pause_step', 'trigger');
    statusMessage = 'Toggling pause...';
    return;
  }
  if (action === 'rewind_15') {
    host_module_set_param('rewind_15_step', 'trigger');
    statusMessage = 'Rewind 15s...';
    return;
  }
  if (action === 'forward_15') {
    host_module_set_param('forward_15_step', 'trigger');
    statusMessage = 'Forward 15s...';
    return;
  }
  if (action === 'stop') {
    host_module_set_param('stop_step', 'trigger');
    statusMessage = 'Stopping...';
    return;
  }
  if (action === 'restart') {
    host_module_set_param('restart_step', 'trigger');
    statusMessage = 'Restarting...';
  }
}

function clampSelectedIndex() {
  const current = menuStack.current();
  if (!current || !current.items || current.items.length === 0) {
    menuState.selectedIndex = 0;
    return;
  }
  if (menuState.selectedIndex < 0) menuState.selectedIndex = 0;
  if (menuState.selectedIndex >= current.items.length) {
    menuState.selectedIndex = current.items.length - 1;
  }
}

function addSearchToHistory(providerId, query) {
  const entry = historyEntry(providerId, query);
  if (!entry.query) return;
  searchHistory = searchHistory.filter((item) => !(item.provider === entry.provider && item.query === entry.query));
  searchHistory.unshift(entry);
  if (searchHistory.length > MAX_SEARCH_HISTORY) {
    searchHistory = searchHistory.slice(0, MAX_SEARCH_HISTORY);
  }
}

function writeTextFile(path, content) {
  let file;
  try {
    file = std.open(path, 'w');
    if (!file) return false;
    file.puts(content);
    file.close();
    return true;
  } catch (e) {
    if (file) {
      try { file.close(); } catch (_) {}
    }
    return false;
  }
}

function loadSearchHistoryFromDisk() {
  let raw = null;
  let fromLegacy = false;

  try {
    raw = std.loadFile(SEARCH_HISTORY_PATH);
    if (!raw) {
      raw = std.loadFile(LEGACY_SEARCH_HISTORY_PATH);
      if (!raw) {
        raw = std.loadFile(LEGACY_SEARCH_HISTORY_PATH_2);
      }
      fromLegacy = !!raw;
    }

    if (!raw) {
      searchHistory = [];
      return;
    }

    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) {
      searchHistory = [];
      return;
    }

    const next = [];
    for (const entry of parsed) {
      let item = null;
      if (typeof entry === 'string') {
        item = historyEntry('youtube', entry);
      } else if (entry && typeof entry === 'object') {
        item = historyEntry(entry.provider, entry.query);
      }
      if (!item || !item.query) continue;
      if (next.some((x) => x.provider === item.provider && x.query === item.query)) continue;
      next.push(item);
      if (next.length >= MAX_SEARCH_HISTORY) break;
    }
    searchHistory = next;

    if (fromLegacy) {
      saveSearchHistoryToDisk();
    }
  } catch (e) {
    searchHistory = [];
  }
}

function saveSearchHistoryToDisk() {
  const payload = `${JSON.stringify(searchHistory.slice(0, MAX_SEARCH_HISTORY))}\n`;
  const tmpPath = `${SEARCH_HISTORY_PATH}.tmp`;

  if (writeTextFile(tmpPath, payload)) {
    if (typeof os.rename === 'function') {
      const rc = os.rename(tmpPath, SEARCH_HISTORY_PATH);
      if (rc === 0) return;
    }

    writeTextFile(SEARCH_HISTORY_PATH, payload);
    if (typeof os.remove === 'function') {
      os.remove(tmpPath);
    }
    return;
  }

  writeTextFile(SEARCH_HISTORY_PATH, payload);
}

function submitSearch(providerId, query) {
  const provider = normalizeProvider(providerId);
  const q = String(query || '').trim();
  if (!q) return;

  searchProvider = provider;
  addSearchToHistory(provider, q);
  saveSearchHistoryToDisk();
  results = [];
  searchCount = 0;
  selectedIndex = 0;
  menuState.selectedIndex = 0;
  statusMessage = `Searching ${providerTag(provider)}...`;
  rebuildMenu();

  host_module_set_param('search_provider', provider);
  host_module_set_param('search_query', q);
}

function clearSearchState() {
  searchQuery = '';
  searchStatus = 'idle';
  searchCount = 0;
  results = [];
  selectedIndex = 0;
  menuState.selectedIndex = 0;
  host_module_set_param('search_query', '');
  statusMessage = 'New search';
  rebuildMenu();
}

function openSearchHistoryMenu() {
  loadSearchHistoryFromDisk();

  const items = [];
  if (searchHistory.length === 0) {
    items.push(createAction('(No previous searches)', () => {}));
  } else {
    for (const entry of searchHistory) {
      const query = String(entry?.query || '').trim();
      const provider = normalizeProvider(entry?.provider);
      if (!query) continue;
      const label = cleanLabel(`${providerTag(provider)} ${query}`, 24);
      items.push(createAction(label, () => {
        while (menuStack.depth() > 1) {
          menuStack.pop();
        }
        menuState.selectedIndex = 0;
        submitSearch(provider, query);
      }));
    }
    if (items.length === 0) {
      items.push(createAction('(No previous searches)', () => {}));
    }
  }

  menuStack.push({
    title: 'Previous',
    items,
    selectedIndex: 0
  });
  menuState.selectedIndex = 0;
  needsRedraw = true;
}

function openProviderMenu() {
  const items = PROVIDERS.map((provider) => createAction(provider.label, () => {
    while (menuStack.depth() > 1) {
      menuStack.pop();
    }
    menuState.selectedIndex = 0;
    openSearchPrompt(provider.id);
  }));

  menuStack.push({
    title: 'Provider',
    items,
    selectedIndex: 0
  });
  menuState.selectedIndex = 0;
  needsRedraw = true;
}

function buildRootItems() {
  const items = [
    createAction('[New Search...]', () => {
      clearSearchState();
      openProviderMenu();
    }),
    createAction('[Previous searches]', () => {
      openSearchHistoryMenu();
    })
  ];

  const count = Math.min(results.length, MAX_MENU_RESULTS);
  for (let i = 0; i < count; i++) {
    const row = results[i];
    const rowProvider = normalizeProvider(row?.provider || searchProvider);
    const title = cleanLabel(row?.title || `Result ${i + 1}`);
    items.push(
      createAction(title, () => {
        if (!row || !row.url) return;
        host_module_set_param('stream_provider', rowProvider);
        host_module_set_param('stream_url', row.url);
        statusMessage = `Loading ${providerTag(rowProvider)} stream...`;
        needsRedraw = true;
      })
    );
  }

  return items;
}

function rebuildMenu() {
  const items = buildRootItems();
  const current = menuStack.current();
  if (!current) {
    menuStack.push({
      title: `Webstream ${providerTag(searchProvider)}`,
      items,
      selectedIndex: 0
    });
    menuState.selectedIndex = 0;
  } else {
    current.title = `Webstream ${providerTag(searchProvider)}`;
    current.items = items;
    clampSelectedIndex();
  }
  needsRedraw = true;
}

function loadResults() {
  const out = [];
  for (let i = 0; i < searchCount && i < MAX_MENU_RESULTS; i++) {
    const provider = normalizeProvider(host_module_get_param(`search_result_provider_${i}`) || searchProvider);
    const title = host_module_get_param(`search_result_title_${i}`) || '';
    const url = host_module_get_param(`search_result_url_${i}`) || '';
    out.push({ provider, title, url });
  }
  results = out;
}

function refreshState() {
  const prevSearchProvider = searchProvider;
  const prevSearchStatus = searchStatus;
  const prevSearchCount = searchCount;
  const prevStreamStatus = streamStatus;

  streamStatus = host_module_get_param('stream_status') || 'stopped';
  searchProvider = normalizeProvider(host_module_get_param('search_provider') || searchProvider);
  searchQuery = host_module_get_param('search_query') || '';
  searchStatus = host_module_get_param('search_status') || 'idle';
  searchCount = parseInt(host_module_get_param('search_count') || '0', 10) || 0;

  if (prevSearchProvider !== searchProvider || prevSearchStatus !== searchStatus || prevSearchCount !== searchCount) {
    loadResults();
    rebuildMenu();

    if (searchStatus === 'searching') {
      statusMessage = `Searching ${providerTag(searchProvider)}...`;
    } else if (searchStatus === 'queued') {
      statusMessage = 'Search queued...';
    } else if (searchStatus === 'done') {
      statusMessage = `${searchCount} results`;
    } else if (searchStatus === 'no_results') {
      statusMessage = 'No results';
    } else if (searchStatus === 'error') {
      statusMessage = 'Search failed';
    } else if (searchStatus === 'busy') {
      statusMessage = 'Search busy';
    }
  }

  if (prevStreamStatus !== streamStatus) {
    if (streamStatus === 'loading') statusMessage = 'Loading stream...';
    else if (streamStatus === 'buffering') statusMessage = 'Buffering...';
    else if (streamStatus === 'seeking') statusMessage = 'Seeking...';
    else if (streamStatus === 'paused') statusMessage = 'Paused';
    else if (streamStatus === 'streaming') statusMessage = 'Playing';
    else if (streamStatus === 'eof') statusMessage = 'Ended';
    else if (streamStatus === 'stopped') statusMessage = 'Stopped';
    needsRedraw = true;
  }
}

function openSearchPrompt(providerId = searchProvider) {
  const provider = normalizeProvider(providerId);
  searchProvider = provider;
  openTextEntry({
    title: `Search ${providerTag(provider)}`,
    initialText: '',
    onConfirm: (text) => {
      const query = (text || '').trim();
      if (!query) {
        statusMessage = 'Search cancelled';
        needsRedraw = true;
        return;
      }
      submitSearch(provider, query);
    },
    onCancel: () => {
      statusMessage = 'Search cancelled';
      needsRedraw = true;
    }
  });
}

function currentFooter() {
  if (isTextEntryActive()) return '';
  const activity = currentActivityLabel();
  if (activity) return `${activity} ${SPINNER[spinnerFrame]}`;
  if (statusMessage) return statusMessage;
  return 'Click:select Back:exit';
}

globalThis.init = function () {
  searchQuery = '';
  searchProvider = normalizeProvider(host_module_get_param('search_provider') || 'youtube');
  searchStatus = 'idle';
  searchCount = 0;
  streamStatus = 'stopped';
  selectedIndex = 0;
  statusMessage = 'Click: select';
  results = [];
  loadSearchHistoryFromDisk();
  shiftHeld = false;

  menuState = createMenuState();
  menuStack = createMenuStack();
  tickCounter = 0;
  spinnerTick = 0;
  spinnerFrame = 0;
  needsRedraw = true;
  pendingKnobAction = null;

  host_module_set_param('search_query', '');
  rebuildMenu();
};

globalThis.tick = function () {
  if (isTextEntryActive()) {
    tickTextEntry();
    drawTextEntry();
    return;
  }

  tickCounter = (tickCounter + 1) % 6;
  if (tickCounter === 0) {
    refreshState();
  }

  if (currentActivityLabel()) {
    spinnerTick = (spinnerTick + 1) % 3;
    if (spinnerTick === 0) {
      spinnerFrame = (spinnerFrame + 1) % SPINNER.length;
      needsRedraw = true;
    }
  } else {
    spinnerTick = 0;
  }

  if (needsRedraw) {
    const current = menuStack.current();
    if (!current) {
      rebuildMenu();
    }

    clear_screen();
    drawStackMenu({
      stack: menuStack,
      state: menuState,
      footer: currentFooter()
    });

    needsRedraw = false;
  }
};

globalThis.onMidiMessageInternal = function (data) {
  const status = data[0] & 0xF0;
  const cc = data[1];
  const val = data[2];

  if (status === MidiNoteOn && val > 0) {
    if (cc === MoveKnob1Touch) {
      setPendingKnobAction(MoveKnob1, 'play_pause', streamStatus === 'paused' ? 'Resume?' : 'Pause?');
      return;
    }
    if (cc === MoveKnob2Touch) {
      setPendingKnobAction(MoveKnob2, 'rewind_15', 'Rewind 15s?');
      return;
    }
    if (cc === MoveKnob3Touch) {
      setPendingKnobAction(MoveKnob3, 'forward_15', 'Forward 15s?');
      return;
    }
    if (cc === MoveKnob7Touch) {
      setPendingKnobAction(MoveKnob7, 'stop', 'Stop stream?');
      return;
    }
    if (cc === MoveKnob8Touch) {
      setPendingKnobAction(MoveKnob8, 'restart', 'Start over?');
      return;
    }
  }

  if (status !== 0xB0) return;

  if (cc === MoveKnob1 || cc === MoveKnob2 || cc === MoveKnob3 || cc === MoveKnob7 || cc === MoveKnob8) {
    const delta = decodeDelta(val);
    if (delta > 0 && pendingKnobAction && pendingKnobAction.cc === cc) {
      runKnobAction(pendingKnobAction.action);
      pendingKnobAction = null;
      needsRedraw = true;
    } else if (delta < 0 && pendingKnobAction && pendingKnobAction.cc === cc) {
      pendingKnobAction = null;
      statusMessage = 'Cancelled';
      needsRedraw = true;
    }
    return;
  }

  if (isCapacitiveTouchMessage(data)) return;

  if (cc === MoveShift) {
    shiftHeld = val > 0;
    return;
  }

  if (isTextEntryActive()) {
    handleTextEntryMidi(data);
    return;
  }

  const current = menuStack.current();
  if (!current) {
    rebuildMenu();
    return;
  }

  const result = handleMenuInput({
    cc,
    value: val,
    items: current.items,
    state: menuState,
    stack: menuStack,
    onBack: () => {
      host_return_to_menu();
    },
    shiftHeld
  });

  if (result.needsRedraw) {
    selectedIndex = menuState.selectedIndex;
    needsRedraw = true;
  }
};

globalThis.onMidiMessageExternal = function (data) {
  if (isTextEntryActive()) {
    handleTextEntryMidi(data);
  }
};

/* Expose chain_ui for shadow component loader compatibility. */
globalThis.chain_ui = {
  init: globalThis.init,
  tick: globalThis.tick,
  onMidiMessageInternal: globalThis.onMidiMessageInternal,
  onMidiMessageExternal: globalThis.onMidiMessageExternal
};
