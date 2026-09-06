const $ = (id) => document.getElementById(id);

const VIEWS = [
  { id: 'all', label: 'Overview' },
  { id: 'iv', label: 'V–I plane' },
  { id: 'series', label: 'Time series' },
];

const PREFS_KEY = 'gan-mppt-bench';

const state = {
  schema: null,
  panels: [],
  iv: null,
  ivChannel: null,
  ivPair: null,
  fields: new Map(),
  readouts: new Map(),
  view: 'all',
  now: 0,
  wall: performance.now(),
  connected: false,
  sequence: { state: 'idle' },
  history: [],
  historyAt: 0,
};

let prefs = {};

function loadPrefs() {
  try {
    prefs = JSON.parse(localStorage.getItem(PREFS_KEY)) || {};
  } catch (err) {
    prefs = {};
  }
}

function savePrefs() {
  prefs.view = state.view;
  prefs.hidden = state.panels.filter((p) => p.hidden).map((p) => p.id);
  for (const id of ['window', 'persist', 'point-size', 'fade', 'x-max', 'y-max']) {
    prefs[id] = $(id).value;
  }
  for (const id of ['show-trail', 'show-best', 'show-iso', 'lock-axes']) {
    prefs[id] = $(id).checked;
  }
  try {
    localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
  } catch (err) {   }
}

function applyPrefsToInputs() {
  for (const id of ['window', 'persist', 'point-size', 'fade', 'x-max', 'y-max']) {
    if (prefs[id] != null) $(id).value = prefs[id];
  }
  for (const id of ['show-trail', 'show-best', 'show-iso', 'lock-axes']) {
    if (prefs[id] != null) $(id).checked = prefs[id];
  }
}

let socket = null;

function openSocket() {
  socket = new WebSocket(`ws://${location.host}/ws`);
  socket.onmessage = (event) => onMessage(JSON.parse(event.data));
  socket.onclose = () => {
    setPill('err', 'server gone');
    setTimeout(openSocket, 1500);
  };
}

function send(message) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

function onMessage(msg) {
  if (msg.type === 'schema') buildFromSchema(msg);
  else if (msg.type === 'data') onData(msg);
  else if (msg.type === 'status') onStatus(msg);
  else if (msg.type === 'log') msg.lines.forEach(logLine);
  else if (msg.type === 'reset') clearAll();
}

function buildTabs() {
  const nav = $('tabs');
  nav.innerHTML = '';
  for (const view of VIEWS) {
    const tab = document.createElement('button');
    tab.className = 'tab';
    tab.dataset.view = view.id;
    tab.textContent = view.label;
    tab.onclick = () => setView(view.id);
    nav.appendChild(tab);
  }
}

function setView(id) {
  state.view = VIEWS.some((v) => v.id === id) ? id : 'all';
  $('main').className = `view-${state.view}`;
  for (const tab of $('tabs').children) {
    tab.classList.toggle('active', tab.dataset.view === state.view);
  }
  savePrefs();
}

function buildFromSchema(schema) {
  state.schema = schema;
  state.fields = new Map(schema.fields.map((f) => [f.key, f]));
  const multiChannel = schema.channels.length > 1;
  const traceLabel = (f) =>
    (multiChannel && f.channel ? `${f.channel}:${f.label}` : f.label);

  buildReadouts(schema, traceLabel);
  buildPanels(schema, traceLabel);

  const channels = Object.keys(schema.iv);
  const select = $('iv-channel');
  select.innerHTML = '';
  for (const ch of channels) {
    const option = document.createElement('option');
    option.value = ch;
    option.textContent = `channel ${ch.toUpperCase()}`;
    select.appendChild(option);
  }
  select.hidden = channels.length < 2;
  buildPhasePlot(channels[0]);

  const quick = $('quick');
  quick.innerHTML = '';
  for (const verb of schema.commands) {
    const button = document.createElement('button');
    button.textContent = verb;
    if (verb === 'stop') button.className = 'stop';
    button.onclick = () => send({ type: 'command', verb });
    quick.appendChild(button);
  }

  buildSequences(schema.sequences);
  refreshCaptures();
}

function swatch(colour) {
  const el = document.createElement('span');
  el.className = 'swatch';
  el.style.background = colour;
  return el;
}

function buildReadouts(schema, traceLabel) {
  const strip = $('readouts');
  strip.innerHTML = '';
  state.readouts = new Map();
  for (const f of schema.fields) {
    const el = document.createElement('div');
    el.className = 'readout';
    el.appendChild(swatch(f.colour));

    const name = document.createElement('span');
    name.className = 'name';
    name.textContent = traceLabel(f);
    el.appendChild(name);

    if (f.kind === 'bits') {
      const bits = document.createElement('span');
      bits.className = 'bits';
      for (const bit of schema.flag_bits) {
        const chip = document.createElement('span');
        chip.className = 'bit';
        chip.dataset.mask = bit.mask;
        chip.textContent = bit.label;
        bits.appendChild(chip);
      }
      el.appendChild(bits);
    } else {
      const value = document.createElement('span');
      value.className = 'value';
      value.textContent = '–';
      el.appendChild(value);
      const unit = document.createElement('span');
      unit.className = 'unit';
      unit.textContent = f.unit;
      el.appendChild(unit);
    }
    strip.appendChild(el);
    state.readouts.set(f.key, el);
  }
}

function buildPanels(schema, traceLabel) {
  const holder = $('panels');
  const toggles = $('panel-toggles');
  holder.innerHTML = '';
  toggles.innerHTML = '';
  state.panels = [];
  const hidden = new Set(prefs.hidden || []);
  const window_s = Number($('window').value) || 30;

  for (const panel of schema.panels) {
    const traces = panel.fields
      .map((key) => state.fields.get(key))
      .filter(Boolean)
      .map((f) => ({ key: f.key, label: traceLabel(f), colour: f.colour,
                     scale: f.scale, step: f.step }));
    if (!traces.length) continue;

    const div = document.createElement('div');
    div.className = 'panel' + (panel.id === 'flags' ? ' short' : '');
    const title = document.createElement('div');
    title.className = 'title label';
    title.textContent = panel.label;
    const wrap = document.createElement('div');
    wrap.className = 'canvas-wrap';
    const canvas = document.createElement('canvas');
    wrap.appendChild(canvas);
    div.append(title, wrap);
    holder.appendChild(div);

    const entry = {
      id: panel.id,
      label: panel.label,
      colour: traces[0].colour,
      div,
      chart: new StripChart(canvas, { traces, window: window_s }),
      hidden: hidden.has(panel.id),
    };
    state.panels.push(entry);

    const chip = document.createElement('button');
    chip.className = 'chip';
    chip.appendChild(swatch(entry.colour));
    chip.appendChild(document.createTextNode(panel.label));
    chip.onclick = () => {
      entry.hidden = !entry.hidden;
      applyPanelVisibility();
      savePrefs();
    };
    entry.chip = chip;
    toggles.appendChild(chip);
  }

  const empty = document.createElement('div');
  empty.className = 'panels-empty';
  empty.textContent = 'every panel is hidden — turn one back on above.';
  holder.appendChild(empty);
  state.panelsEmpty = empty;

  applyPanelVisibility();
}

function applyPanelVisibility() {
  let shown = 0;
  for (const panel of state.panels) {
    panel.div.hidden = panel.hidden;
    panel.chip.setAttribute('aria-pressed', String(!panel.hidden));
    if (!panel.hidden) shown++;
  }
  if (state.panelsEmpty) state.panelsEmpty.hidden = shown > 0;
}

function buildPhasePlot(channel) {
  if (!channel) return;
  if (state.iv) state.iv.destroy();
  const pair = state.schema.iv[channel];
  const x = state.fields.get(pair.x);
  const y = state.fields.get(pair.y);
  state.ivChannel = channel;
  state.ivPair = { x, y, power: pair.power };
  state.iv = new PhasePlot($('iv'), {
    xLabel: `${x.label} (${x.unit})`,
    yLabel: `${y.label} (${y.unit})`,
    colour: y.colour,
    persist: Number($('persist').value) || 60,
  });
  applyIvControls();
}

function buildSequences(sequences) {
  const holder = $('sequences');
  holder.innerHTML = '';
  for (const seq of sequences) {
    const div = document.createElement('div');
    div.className = 'seq';
    const steps = seq.steps.map(([t, v]) => `${t}s ${v}`).join('  ');
    div.innerHTML =
      '<div class="row"><span class="name"></span><span class="spacer"></span>' +
      '<button class="run">run</button></div>' +
      '<div class="meta"></div><div class="desc"></div>';
    div.querySelector('.name').textContent = seq.label;
    div.querySelector('.meta').textContent = `${seq.length}s   ${steps}`;
    const desc = div.querySelector('.desc');
    if (seq.description) desc.textContent = seq.description;
    else desc.remove();
    div.querySelector('.run').onclick = () =>
      send({ type: 'sequence', name: seq.name });
    holder.appendChild(div);
  }
}

function onData(msg) {
  state.now = msg.now;
  state.wall = performance.now();
  if (msg.ts.length) {

    for (const panel of state.panels) panel.chart.push(msg.ts, msg.series);
  }
  const points = msg.iv[state.ivChannel];
  if (points && state.iv) {
    const { x, y } = state.ivPair;
    for (const [t, px, py, p] of points) {
      state.iv.push(t, px / x.scale, py / y.scale, p);
    }
  }
  updateReadouts(msg.latest);
}

function updateReadouts(latest) {
  for (const [key, el] of state.readouts) {
    const field = state.fields.get(key);
    const raw = latest[key];
    if (field.kind === 'bits') {
      const value = raw || 0;
      for (const chip of el.querySelectorAll('.bit')) {
        chip.classList.toggle('set', (value & Number(chip.dataset.mask)) !== 0);
      }
      continue;
    }
    const node = el.querySelector('.value');
    node.textContent = raw == null ? '–' : (raw / field.scale).toFixed(field.digits);
  }
}

function onStatus(msg) {
  state.connected = msg.connected;
  $('connect').disabled = msg.connected;
  $('disconnect').disabled = !msg.connected;
  if (msg.connected) setPill('on', msg.port);
  else setPill(msg.error ? 'err' : '', msg.error ? 'link error' : 'disconnected');
  $('stats').textContent = msg.connected
    ? `${msg.sets} sets   ${msg.dropped} dropped   ${msg.stats.frames} frames   ` +
      `${msg.stats.resyncs} resynced   ${msg.stats.uptime.toFixed(0)} s`
    : (msg.error || '');
  state.sequence = msg.sequence || { state: 'idle' };
  renderRunState();
}

function setPill(kind, text) {
  const pill = $('pill');
  pill.className = kind ? `pill ${kind}` : 'pill';
  pill.textContent = text;
}

let runSignature = '';

function renderRunState() {
  const run = state.sequence || { state: 'idle' };
  const signature = [run.state, run.name, (run.summary || []).length,
                     Object.keys(run.files || {}).length].join('|');
  if (signature !== runSignature) {
    runSignature = signature;
    buildRunState(run);
    if (run.state === 'done' || run.state === 'error') refreshCaptures();
  }
  updateRunProgress(run);
}

function buildRunState(run) {
  const holder = $('run-state');
  if (run.state === 'idle') { holder.innerHTML = ''; return; }
  let html = '<div class="meta"><span class="run-label"></span></div>' +
             '<div class="progress"><span></span></div>';
  if (run.state === 'running') html += '<button class="danger" id="seq-cancel">cancel</button>';
  if (run.summary && run.summary.length) html += '<pre></pre>';
  if (run.files && Object.keys(run.files).length) {
    html += '<div class="file-links">' + fileLinks(run.files) + '</div>';
  }
  holder.innerHTML = html;
  const summary = holder.querySelector('pre');
  if (summary) summary.textContent = run.summary.join('\n');
  const cancel = $('seq-cancel');
  if (cancel) cancel.onclick = () => send({ type: 'sequence_cancel' });
  bindViewers(holder);
}

function updateRunProgress(run) {
  const holder = $('run-state');
  const bar = holder.querySelector('.progress span');
  const label = holder.querySelector('.run-label');
  if (!bar || !label) return;
  const elapsed = run.started != null ? Math.max(0, state.now - run.started) : 0;
  const fraction = run.state === 'running'
    ? (run.length ? Math.min(1, elapsed / run.length) : 0) : 1;
  bar.style.width = `${fraction * 100}%`;
  label.textContent = `${run.label || run.name} — ${run.state}` +
    (run.state === 'running' ? ` ${elapsed.toFixed(1)}/${run.length}s` : '');
}

function fileLinks(files) {
  return Object.entries(files).map(([kind, name]) =>
    (name.endsWith('.svg')
      ? `<button data-view="${name}">${kind}</button>`
      : `<a href="/captures/${name}" download>${kind}</a>`)).join('');
}

function bindViewers(root) {
  root.querySelectorAll('[data-view]').forEach((el) => {
    el.onclick = () => showPlot(el.dataset.view);
  });
}

async function refreshCaptures() {
  const { captures } = await (await fetch('/api/captures')).json();
  const holder = $('captures');
  holder.innerHTML = '';
  if (!captures.length) {
    holder.innerHTML = '<div class="empty">no captures yet</div>';
    return;
  }
  for (const item of captures) {
    const div = document.createElement('div');
    div.className = 'capture';
    div.innerHTML = '<span class="stem"></span>' +
                    `<span class="file-links">${fileLinks(item.files)}</span>`;
    div.querySelector('.stem').textContent = item.name;
    holder.appendChild(div);
  }
  bindViewers(holder);
}

function showPlot(name) {
  $('viewer-title').textContent = name;
  const img = document.createElement('img');
  img.src = `/captures/${name}`;
  img.alt = name;
  $('viewer-body').replaceChildren(img);
  $('viewer').classList.remove('hidden');
}

function logLine(entry) {
  const holder = $('log');
  const atBottom = holder.scrollHeight - holder.scrollTop - holder.clientHeight < 30;
  const div = document.createElement('div');
  div.innerHTML = '<span class="t"></span><span class="msg"></span>';
  div.firstChild.textContent = entry.t.toFixed(2).padStart(7);
  div.lastChild.className = `msg ${entry.level}`;
  div.lastChild.textContent = entry.text;
  holder.appendChild(div);
  while (holder.childElementCount > 500) holder.removeChild(holder.firstChild);
  if (atBottom) holder.scrollTop = holder.scrollHeight;
}

function localLog(text, level = 'info') {
  logLine({ t: state.now, level, text });
}

const LOCAL_HELP = [
  'verbs come from console.OPCODES; anything else here is local:',
  '  raw <op-hex> [byte-hex ...]   send an arbitrary frame',
  '  clear [iv | series | all]     empty a plot; both if not named',
  '  persist <seconds>             how long a point stays',
  '  window <seconds>              time-series span',
  '  view all | iv | series        switch the tab',
  '  run <sequence>                start a predefined capture',
  '  connect [port] | disconnect | help',
];

function runCommand(line) {
  const parts = line.trim().split(/\s+/);
  const verb = parts[0].toLowerCase();
  const args = parts.slice(1);
  if (!verb) return;

  if (state.schema && state.schema.commands.includes(verb)) {
    send({ type: 'command', verb });
  } else if (verb === 'raw') {
    if (!args.length) return localLog('usage: raw <op-hex> [byte-hex ...]', 'error');
    send({ type: 'raw', op: args[0], payload: args.slice(1).join(' ') });
  } else if (verb === 'help') {
    LOCAL_HELP.forEach((l) => localLog(l));
  } else if (verb === 'clear') {
    const what = (args[0] || 'all').toLowerCase();
    if (!['iv', 'series', 'all'].includes(what)) {
      return localLog('usage: clear [iv | series | all]', 'error');
    }
    if (what !== 'series') clearIv();
    if (what !== 'iv') clearSeries();
    localLog(`cleared ${what === 'all' ? 'both plots' : what}`);
  } else if (verb === 'view') {
    setView(args[0]);
    localLog(`view ${state.view}`);
  } else if (verb === 'persist' || verb === 'window') {
    const seconds = Number(args[0]);
    if (!(seconds > 0)) return localLog(`usage: ${verb} <seconds>`, 'error');
    $(verb).value = seconds;
    applyIvControls();
    applyWindow();
    localLog(`${verb} ${seconds} s`);
  } else if (verb === 'run') {
    send({ type: 'sequence', name: args[0] || '' });
  } else if (verb === 'connect') {
    connect(args[0]);
  } else if (verb === 'disconnect') {
    fetch('/api/disconnect', { method: 'POST' });
  } else {
    localLog(`unknown: ${verb}  (try help)`, 'error');
  }
}

async function refreshPorts() {
  const { ports, suggested } = await (await fetch('/api/ports')).json();
  const select = $('port');
  select.innerHTML = '';
  if (!ports.length) {
    select.innerHTML = '<option value="">no ports found</option>';
    return;
  }
  for (const port of ports) {
    const option = document.createElement('option');
    option.value = port.device;
    option.textContent = `${port.device} — ${port.description}`;
    if (port.device === suggested) option.selected = true;
    select.appendChild(option);
  }
}

async function connect(port) {
  const body = JSON.stringify({ port: port || $('port').value || null });
  const response = await fetch('/api/connect',
    { method: 'POST', headers: { 'Content-Type': 'application/json' }, body });
  if (!response.ok) {
    const detail = await response.json().catch(() => ({ detail: response.statusText }));
    localLog(detail.detail || 'connect failed', 'error');
  }
}

function clearIv() {
  if (state.iv) state.iv.clearData();
}

function clearSeries() {
  for (const panel of state.panels) panel.chart.clearData();
}

function clearAll() {
  clearSeries();
  clearIv();
  state.now = 0;
}

function applyIvControls() {
  const iv = state.iv;
  if (!iv) return;
  iv.persist = Number($('persist').value) || 60;
  iv.pointSize = Number($('point-size').value);
  iv.fade = $('fade').value;
  iv.showTrail = $('show-trail').checked;
  iv.showBest = $('show-best').checked;
  iv.showIso = $('show-iso').checked;
  iv.lockAxes = $('lock-axes').checked;
  iv.xMaxLock = Number($('x-max').value) || 1;
  iv.yMaxLock = Number($('y-max').value) || 1;
  $('iv-controls').classList.toggle('axes-locked', iv.lockAxes);
  savePrefs();
}

function applyWindow() {
  const seconds = Number($('window').value) || 30;
  for (const panel of state.panels) panel.chart.window = seconds;
  savePrefs();
}

function tick() {

  const now = state.connected
    ? state.now + (performance.now() - state.wall) / 1000
    : state.now;
  if (state.view !== 'iv') {
    for (const panel of state.panels) if (!panel.hidden) panel.chart.draw(now);
  }
  if (state.iv && state.view !== 'series') state.iv.draw(now);
  if (state.sequence.state === 'running') renderRunState();
  requestAnimationFrame(tick);
}

function boot() {
  loadPrefs();
  applyPrefsToInputs();
  buildTabs();
  setView(prefs.view || 'all');
  refreshPorts();
  openSocket();

  $('connect').onclick = () => connect();
  $('disconnect').onclick = () => fetch('/api/disconnect', { method: 'POST' });
  $('rescan').onclick = refreshPorts;
  $('iv-clear').onclick = clearIv;
  $('series-clear').onclick = clearSeries;
  $('log-clear').onclick = () => { $('log').innerHTML = ''; send({ type: 'clear_log' }); };
  $('captures-refresh').onclick = refreshCaptures;
  $('viewer-close').onclick = () => $('viewer').classList.add('hidden');
  $('viewer').onclick = (e) => {
    if (e.target === $('viewer')) $('viewer').classList.add('hidden');
  };
  $('iv-channel').onchange = (e) => buildPhasePlot(e.target.value);
  $('window').oninput = applyWindow;
  for (const id of ['persist', 'point-size', 'fade', 'show-trail', 'show-best',
                    'show-iso', 'lock-axes', 'x-max', 'y-max']) {
    $(id).oninput = applyIvControls;
    $(id).onchange = applyIvControls;
  }

  $('cmd-form').onsubmit = (e) => {
    e.preventDefault();
    const line = $('cmd').value.trim();
    if (!line) return;
    state.history.push(line);
    state.historyAt = state.history.length;
    $('cmd').value = '';
    runCommand(line);
  };
  $('cmd').onkeydown = (e) => {
    if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
    e.preventDefault();
    state.historyAt += e.key === 'ArrowUp' ? -1 : 1;
    state.historyAt = Math.max(0, Math.min(state.history.length, state.historyAt));
    $('cmd').value = state.history[state.historyAt] || '';
  };
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') $('viewer').classList.add('hidden');
  });

  requestAnimationFrame(tick);
}

boot();
