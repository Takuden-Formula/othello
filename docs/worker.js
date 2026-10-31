// AI search runs here so the page stays responsive while the engine thinks.
let ex = null;
const ready = WebAssembly.instantiateStreaming(fetch('othello.wasm'), {
  env: { millis: () => Math.floor(performance.now()) >>> 0 }
}).then(({ instance }) => {
  ex = instance.exports;
  ex._initialize();
  ex.init();
});

onmessage = async ({ data }) => {
  await ready;
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
