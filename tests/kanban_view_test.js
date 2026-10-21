'use strict';
// No browser, server, packages, sleeps or actual clock deadlines are needed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
(async () => {
  const nodes = new Map();
  const listeners = new Map();
  const timers = new Map();
  const pending = [];
  let nextTimer = 0;
  let requests = 0;
  let aborts = 0;
  function element() {
    return {textContent: '', children: [], append(...items) { this.children.push(...items); },
      replaceChildren(...items) { this.children = items; }};
  }
  const document = {hidden: false, createElement: element,
    getElementById(id) { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); },
    addEventListener(name, callback) { listeners.set(name, callback); }};
  const context = vm.createContext({document, URL, URLSearchParams, Date,
    location: {href: 'https://host.test/kanban/?board=team%20one', search: '?board=team%20one'},
    AbortController: class { constructor() { this.signal = {}; } abort() { aborts++; } },
    setTimeout(fn, ms) { const id = ++nextTimer; timers.set(id, {fn, ms}); return id; },
    clearTimeout(id) { timers.delete(id); },
    fetch(url, options) {
      requests++;
      assert.equal(url.toString(), 'https://host.test/kanban/board.json?board=team+one');
      assert.equal(options.cache, 'no-store');
      return new Promise((resolve, reject) => pending.push({resolve, reject}));
    }});
  const settle = () => new Promise(resolve => setImmediate(resolve));
  const runTimer = ms => {
    const entry = [...timers.entries()].find(([, timer]) => timer.ms === ms);
    assert.ok(entry, 'expected timer ' + ms);
    timers.delete(entry[0]); entry[1].fn();
  };
  const snapshot = {ok: true, board_name: '<team>', truncated: false, items: [
    {id: 'T-001', title: '<script>bad()</script>', status: 'todo'},
    {id: 'T-002', title: 'Working', status: 'in_process'},
    {id: 'T-003', title: 'Finished', status: 'done'}]};
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/tools/kanban.js'), 'utf8'), context);
  assert.equal(requests, 1);
  listeners.get('visibilitychange')();
  assert.equal(requests, 1, 'in-flight requests cannot overlap');
  pending.shift().resolve({ok:true, json:async () => snapshot}); await settle();
  assert.equal(nodes.get('board').textContent, '<team>');
  assert.equal(nodes.get('todo').children[0].children[1].textContent, '<script>bad()</script>');
  assert.equal(nodes.get('done').children.length, 1);
  const firstCard = nodes.get('todo').children[0];
  runTimer(2000);
  pending.shift().resolve({ok:true, json:async () => snapshot}); await settle();
  assert.equal(nodes.get('todo').children[0], firstCard, 'unchanged data preserves DOM');
  document.hidden = true; listeners.get('visibilitychange')();
  assert.equal(timers.size, 0, 'hidden pages do not schedule polling');
  document.hidden = false; listeners.get('visibilitychange')();
  pending.shift().resolve({ok:true, json:async () => ({...snapshot, truncated:true})}); await settle();
  assert.match(nodes.get('status').textContent, /limit reached/);
  runTimer(2000); pending.shift().resolve({ok:false, status:503}); await settle();
  assert.match(nodes.get('status').textContent, /503.*Retrying/);
  runTimer(2000); runTimer(5000);
  assert.equal(aborts, 1, 'fetch deadline aborts the active request');
  pending.shift().reject(new Error('aborted')); await settle();
  assert.match(nodes.get('status').textContent, /aborted/);
  runTimer(2000); document.hidden = true; listeners.get('visibilitychange')();
  pending.shift().reject(new Error('offline')); await settle();
  assert.equal(timers.size, 0, 'hidden failure does not restart polling');
  document.hidden = false; listeners.get('visibilitychange')();
  pending.shift().resolve({ok:true, json:async () => ({ok:false, message:'Missing board'})}); await settle();
  assert.match(nodes.get('status').textContent, /Missing board/);
  console.log('kanban view behavior passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
