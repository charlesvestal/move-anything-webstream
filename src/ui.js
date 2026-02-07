import {
  openTextEntry,
  isTextEntryActive,
  handleTextEntryMidi,
  drawTextEntry,
  tickTextEntry
} from '/data/UserData/move-anything/shared/text_entry.mjs';

import {
  MoveShift
} from '/data/UserData/move-anything/shared/constants.mjs';

import { isCapacitiveTouchMessage } from '/data/UserData/move-anything/shared/input_filter.mjs';

import { createAction } from '/data/UserData/move-anything/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/move-anything/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/move-anything/shared/menu_stack.mjs';
import { drawStackMenu } from '/data/UserData/move-anything/shared/menu_render.mjs';

const MAX_MENU_RESULTS = 20;

let searchQuery = '';
let searchStatus = 'idle';
let searchCount = 0;
let streamStatus = 'stopped';
let selectedIndex = 0;
let statusMessage = 'Click: select';
let results = [];
let shiftHeld = false;

let menuState = createMenuState();
let menuStack = createMenuStack();

let tickCounter = 0;
let needsRedraw = true;

function cleanLabel(text, maxLen = 24) {
  let s = String(text || '');
  s = s.replace(/[^\x20-\x7E]+/g, ' ').replace(/\s+/g, ' ').trim();
  if (!s) s = '(untitled)';
  if (s.length > maxLen) s = `${s.slice(0, Math.max(0, maxLen - 1))}…`;
  return s;
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

function buildRootItems() {
  const items = [
    createAction('[New Search...]', () => {
      openSearchPrompt();
    })
  ];

  const count = Math.min(results.length, MAX_MENU_RESULTS);
  for (let i = 0; i < count; i++) {
    const row = results[i];
    const title = cleanLabel(row?.title || `Result ${i + 1}`);
    items.push(
      createAction(title, () => {
        if (!row || !row.url) return;
        host_module_set_param('stream_url', row.url);
        statusMessage = 'Loading stream...';
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
      title: 'YT Search',
      items,
      selectedIndex: 0
    });
    menuState.selectedIndex = 0;
  } else {
    current.items = items;
    clampSelectedIndex();
  }
  needsRedraw = true;
}

function loadResults() {
  const out = [];
  for (let i = 0; i < searchCount && i < MAX_MENU_RESULTS; i++) {
    const title = host_module_get_param(`search_result_title_${i}`) || '';
    const url = host_module_get_param(`search_result_url_${i}`) || '';
    out.push({ title, url });
  }
  results = out;
}

function refreshState() {
  const prevSearchStatus = searchStatus;
  const prevSearchCount = searchCount;
  const prevStreamStatus = streamStatus;

  streamStatus = host_module_get_param('stream_status') || 'stopped';
  searchQuery = host_module_get_param('search_query') || '';
  searchStatus = host_module_get_param('search_status') || 'idle';
  searchCount = parseInt(host_module_get_param('search_count') || '0', 10) || 0;

  if (prevSearchStatus !== searchStatus || prevSearchCount !== searchCount) {
    loadResults();
    rebuildMenu();

    if (searchStatus === 'searching') {
      statusMessage = 'Searching...';
    } else if (searchStatus === 'done') {
      statusMessage = `${searchCount} results`;
    } else if (searchStatus === 'no_results') {
      statusMessage = 'No results';
    } else if (searchStatus === 'error') {
      statusMessage = 'Search failed';
    }
  }

  if (prevStreamStatus !== streamStatus) {
    if (streamStatus === 'streaming') {
      statusMessage = 'Playing';
    } else if (streamStatus === 'eof' || streamStatus === 'stopped') {
      statusMessage = 'Stopped';
    }
    needsRedraw = true;
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
        needsRedraw = true;
        return;
      }

      results = [];
      searchCount = 0;
      selectedIndex = 0;
      menuState.selectedIndex = 0;
      statusMessage = 'Searching...';
      rebuildMenu();

      host_module_set_param('search_query', query);
    },
    onCancel: () => {
      statusMessage = 'Search cancelled';
      needsRedraw = true;
    }
  });
}

function currentFooter() {
  if (isTextEntryActive()) return '';
  if (searchStatus === 'searching') return 'Searching...';
  if (statusMessage) return statusMessage;
  return 'Click:select Back:exit';
}

globalThis.init = function () {
  searchQuery = '';
  searchStatus = 'idle';
  searchCount = 0;
  streamStatus = 'stopped';
  selectedIndex = 0;
  statusMessage = 'Click: select';
  results = [];
  shiftHeld = false;

  menuState = createMenuState();
  menuStack = createMenuStack();
  tickCounter = 0;
  needsRedraw = true;

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

  if (isCapacitiveTouchMessage(data)) return;

  if (status !== 0xB0) return;

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
