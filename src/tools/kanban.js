'use strict';
const query = new URLSearchParams(location.search);
const endpoint = new URL('board.json', location.href);
if (query.has('board')) endpoint.searchParams.set('board', query.get('board'));
let active = false;
let timer;
let previous;
function render(board) {
  if (!board.ok) throw new Error(board.message || 'Board unavailable');
  document.getElementById('board').textContent = board.board_name;
  for (const lane of ['todo', 'in_process', 'done']) {
    const container = document.getElementById(lane);
    const items = (board.items || []).filter(item => item.status === lane);
    const cards = items.map(item => {
      const card = document.createElement('article');
      const id = document.createElement('small');
      const title = document.createElement('p');
      id.textContent = item.id;
      title.textContent = item.title;
      card.append(id, title);
      return card;
    });
    container.replaceChildren(...cards);
    document.getElementById(lane + '-count').textContent = '(' + items.length + ')';
  }
}
async function refresh() {
  clearTimeout(timer);
  if (document.hidden || active) return;
  active = true;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(endpoint, {cache: 'no-store', signal: controller.signal});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const board = await response.json();
    const signature = JSON.stringify(board);
    if (signature !== previous) { render(board); previous = signature; }
    document.getElementById('status').textContent = board.truncated
      ? 'Result limit reached; some cards are omitted. Read-only view.'
      : 'Updated ' + new Date().toLocaleTimeString() + ' · Read-only view';
  } catch (error) {
    document.getElementById('status').textContent = 'Update failed: ' + error.message + ' · Retrying';
  } finally {
    clearTimeout(timeout);
    active = false;
    if (!document.hidden) timer = setTimeout(refresh, 2000);
  }
}
document.addEventListener('visibilitychange', () => {
  clearTimeout(timer);
  if (!document.hidden) refresh();
});
refresh();
