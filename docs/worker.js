// AI search runs here so the page stays responsive while the engine thinks.
let ex = null;
const ready = fetch('othello.wasm?v=3').then(r => r.arrayBuffer()).then(buf => WebAssembly.instantiate(buf, {
  env: { millis: () => Math.floor(performance.now()) >>> 0 }
})).then(({ instance }) => {
  ex = instance.exports;
  ex._initialize();
  // hash table size from the page (2^tt entries); fall back to the default if it cannot be allocated
  const tt = +new URLSearchParams(location.search).get('tt') || 0;
  if (!ex.init(tt)) ex.init(0);
});
ready.catch(e => postMessage({ type: 'error', msg: String(e) }));

onmessage = async ({ data }) => {
  try { await ready; } catch { return; }
  if (data.type === 'new') { ex.new_game(); return; }
  if (data.type === 'search') {
    const [plo, phi, olo, ohi] = data.board;
    const move = ex.search(plo, phi, olo, ohi, data.ms);
    postMessage({
      id: data.id, move,
      discs: ex.score_to_discs(ex.get_score()),
      depth: ex.get_depth(), exact: ex.get_exact(),
      nodes: ex.get_nodes(), ms: ex.get_ms()
    });
  }
};
