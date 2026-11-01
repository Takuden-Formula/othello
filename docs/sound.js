// 効果音（Web Audio で合成。音声ファイルは使わない）
// 石を置く「コトッ」、ひっくり返る「カチカチ」、パス、勝ち・負け・引き分けのジングル、ボタン、開始、ヒント。
// AudioContext はブラウザの決まりで最初のクリック/キー操作のあとでないと鳴らないので、unlock() をその中で呼ぶ。

export class Sfx {
  constructor() { this.ctx = null; this.on = true; this.vol = 0.7; }
  unlock() {
    try {
      if (!this.ctx) {
        const AC = window.AudioContext || window.webkitAudioContext;
        if (!AC) return;
        this.ctx = new AC();
        this.master = this.ctx.createGain(); this.master.connect(this.ctx.destination);
        const n = this.ctx.sampleRate, buf = this.ctx.createBuffer(1, n, n), d = buf.getChannelData(0);
        for (let i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
        this.noise = buf;
      }
      if (this.ctx.state !== 'running') this.ctx.resume();
    } catch {}
  }
  get ok() { return this.on && this.ctx && this.ctx.state === 'running'; }
  // 1つの音（周波数 f [Hz] から f2 へ、dur 秒、音量 v、delay 秒後）
  tone(f, dur, v, type = 'sine', delay = 0, f2 = f) {
    if (!this.ok) return;
    const c = this.ctx, t = c.currentTime + delay, o = c.createOscillator(), g = c.createGain();
    this.master.gain.value = this.vol;
    o.type = type; o.frequency.setValueAtTime(f, t); if (f2 !== f) o.frequency.exponentialRampToValueAtTime(f2, t + dur);
    g.gain.setValueAtTime(0.0001, t); g.gain.exponentialRampToValueAtTime(v, t + 0.008); g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    o.connect(g); g.connect(this.master); o.start(t); o.stop(t + dur + 0.05);
  }
  // 短いノイズ（木や石の当たる感じ）
  tick(f, q, dur, v, delay = 0) {
    if (!this.ok) return;
    const c = this.ctx, t = c.currentTime + delay, n = c.createBufferSource(), b = c.createBiquadFilter(), g = c.createGain();
    this.master.gain.value = this.vol;
    n.buffer = this.noise; b.type = 'bandpass'; b.frequency.value = f; b.Q.value = q;
    g.gain.setValueAtTime(v, t); g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    n.connect(b); b.connect(g); g.connect(this.master); n.start(t, Math.random() * 0.5); n.stop(t + dur + 0.05);
  }
  // 石を置く「コトッ」
  place() { this.tick(900, 1.2, 0.09, 0.9); this.tone(170, 0.12, 0.5, 'sine', 0, 120); }
  // ひっくり返る「カチ」。delays は石ごとの遅れ [秒]（アニメーションに合わせる）
  flip(delays) { delays.forEach((d, i) => { this.tick(2400 + i * 120, 4, 0.05, 0.45, d + 0.12); this.tone(1100 + i * 60, 0.05, 0.08, 'triangle', d + 0.12); }); }
  pass() { this.tone(520, 0.18, 0.25, 'triangle'); this.tone(390, 0.28, 0.25, 'triangle', 0.18); }
  button() { this.tone(880, 0.07, 0.2, 'triangle'); }
  start() { [523, 659, 784].forEach((f, i) => this.tone(f, 0.14, 0.25, 'triangle', i * 0.09)); }
  hint() { this.tone(1320, 0.12, 0.18, 'sine'); this.tone(1760, 0.18, 0.15, 'sine', 0.1); }
  win() {
    [523, 659, 784, 1047].forEach((f, i) => this.tone(f, 0.22, 0.3, 'triangle', i * 0.12));
    [1047, 1319, 1568].forEach((f) => this.tone(f, 0.7, 0.16, 'sine', 0.5));
    this.tone(2093, 0.5, 0.08, 'sine', 0.62);
  }
  lose() { [392, 370, 349, 262].forEach((f, i) => this.tone(f, i === 3 ? 0.7 : 0.3, 0.28, 'triangle', i * 0.28)); }
  draw() { [523, 659].forEach((f, i) => this.tone(f, 0.3, 0.25, 'triangle', i * 0.2)); this.tone(587, 0.6, 0.22, 'triangle', 0.45); }
}
