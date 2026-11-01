// 記録（対局の結果）を PC のフォルダに保存・読み込みする（Chrome / Edge の File System Access API）
// エアハンドル・ドライブの records.js と同じ方式。選んだフォルダに records.csv を作り、1局ごとに1行足す。Excel でそのまま開ける（UTF-8 BOM 付き）。
// フォルダの場所はブラウザ（IndexedDB）に覚えておき、次に起動したときも同じフォルダを使う。
// ブラウザを開き直すと書き込みの許可が「確認待ち」に戻るので、最初のクリックかキー操作のときに許可を取り直す。
// どのブラウザでも、記録はブラウザの中（localStorage）にも残す。フォルダを選べないブラウザ（Safari・iPad・Firefox）や、
// フォルダを選んでいないときはこちらで集計し、「records.csv を書き出す」でダウンロードできる。

const FILE = 'records.csv';
const LKEY = 'othello.records';
export const COLS = ['日時', '受付番号', '手番', 'AIの強さ', '結果', '自分の石', 'AIの石', '石差', '手数', 'かかった秒', 'ヒント', '待った'];

function idb() {
  return new Promise((res, rej) => {
    const r = indexedDB.open('othello', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
async function kv(key, value) {
  const db = await idb();
  return new Promise((res, rej) => {
    const tx = db.transaction('kv', value === undefined ? 'readonly' : 'readwrite'), st = tx.objectStore('kv');
    const r = value === undefined ? st.get(key) : st.put(value, key);
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}

const csvCell = (v) => { const s = String(v ?? ''); return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s; };
const toCsv = (rows) => '﻿' + [COLS.join(','), ...rows.map((r) => COLS.map((c) => csvCell(r[c])).join(','))].join('\r\n') + '\r\n';
const norm = (rec) => Object.fromEntries(COLS.map((c) => [c, String(rec[c] ?? '')]));
function parseCsv(text) {
  const rows = []; let row = [], cell = '', q = false;
  text = text.replace(/^﻿/, '');
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (q) { if (ch === '"' && text[i + 1] === '"') { cell += '"'; i++; } else if (ch === '"') q = false; else cell += ch; }
    else if (ch === '"') q = true;
    else if (ch === ',') { row.push(cell); cell = ''; }
    else if (ch === '\n') { row.push(cell.replace(/\r$/, '')); rows.push(row); row = []; cell = ''; }
    else cell += ch;
  }
  if (cell || row.length) { row.push(cell); rows.push(row); }
  return rows;
}

export class Records {
  // state はフォルダの状態: none（未設定）/ prompt（許可待ち）/ ok / error
  constructor() {
    this.dir = null; this.state = 'none'; this.rows = []; this.later = []; this.local = !('showDirectoryPicker' in window);
    try { this.localRows = JSON.parse(localStorage.getItem(LKEY)) || []; } catch { this.localRows = []; }
  }
  get supported() { return !this.local; }
  get useFolder() { return !this.local && this.state === 'ok'; }
  get name() { return this.useFolder ? this.dir.name : 'ブラウザ内'; }
  // 集計に使う行（フォルダが使えるときは records.csv、使えないときはブラウザ内の記録）
  get all() { return this.useFolder ? this.rows : this.localRows; }
  get csv() { return toCsv(this.all); }
  // records.csv をダウンロードする（フォルダを選べないブラウザ用。いつでも使える）
  download() {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([this.csv], { type: 'text/csv' })); a.download = FILE;
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  }

  // 起動時：前に選んだフォルダを思い出す（許可が残っていれば読み込みまでする）
  async restore() {
    if (this.local) return;
    try { this.dir = (await kv('dir')) || null; } catch { this.dir = null; }
    if (!this.dir) return;
    this.state = (await this.dir.queryPermission({ mode: 'readwrite' })) === 'granted' ? 'ok' : 'prompt';
    if (this.state === 'ok') await this.load();
  }
  // クリックやキー操作の中で呼ぶ（ブラウザの決まりで、許可の確認は操作の中でしか出せない）
  async grant() {
    if (this.local) return false;
    if (!this.dir || this.state === 'ok') return this.state === 'ok';
    try { this.state = (await this.dir.requestPermission({ mode: 'readwrite' })) === 'granted' ? 'ok' : 'prompt'; } catch { this.state = 'prompt'; }
    if (this.state === 'ok') { await this.load(); await this.flush(); }
    return this.state === 'ok';
  }
  // 設定の「フォルダを選ぶ」
  async pick() {
    this.dir = await window.showDirectoryPicker({ id: 'othello-records', mode: 'readwrite' });
    await kv('dir', this.dir);
    this.state = 'ok';
    await this.load();
  }

  async load() {
    if (this.local || !this.dir) return this.all;
    try {
      const fh = await this.dir.getFileHandle(FILE, { create: true });
      const text = await (await fh.getFile()).text();
      const rows = parseCsv(text).filter((r) => r.length > 1);
      const head = rows[0] && rows[0][0] === COLS[0] ? rows.shift() : COLS;
      this.rows = rows.map((r) => Object.fromEntries(head.map((h, i) => [h, r[i] ?? ''])));
      this.state = 'ok';
    } catch (e) { this.state = 'error'; this.error = e.message; throw e; }
    return this.rows;
  }

  // 1行足す。ブラウザ内にはすぐ残し、フォルダが使えるときは records.csv の最後にも書き足す。
  // 続けて呼ばれても1行ずつ順に書く（同時に書くと後の行が前の行を上書きするため）。戻り値 true = フォルダに書けた
  append(rec) {
    this.localRows.push(norm(rec));
    try { localStorage.setItem(LKEY, JSON.stringify(this.localRows)); } catch {}
    this.queue = (this.queue || Promise.resolve()).catch(() => {}).then(() => this.appendNow(rec));
    return this.queue;
  }
  // 許可待ちの間に終わった局は、許可されたあとで records.csv にまとめて書く
  async flush() { for (const r of this.later.splice(0)) await this.appendNow(r); }
  async appendNow(rec) {
    if (!this.useFolder) { if (this.dir && this.state === 'prompt') this.later.push(rec); return false; }
    try { await this.write(rec); }
    catch (e) { this.state = 'error'; this.error = e.message; throw e; }   // 書けない（Excel で開いたまま など）→「記録: 読めません」と出し、集計はブラウザ内の記録に切り替わる
    this.rows.push(norm(rec));
    return true;
  }
  async write(rec) {
    const fh = await this.dir.getFileHandle(FILE, { create: true });
    const file = await fh.getFile();
    const w = await fh.createWritable({ keepExistingData: true });
    await w.seek(file.size);
    await w.write((file.size ? '' : '﻿' + COLS.join(',') + '\r\n') + COLS.map((c) => csvCell(rec[c])).join(',') + '\r\n');
    await w.close();
  }
}
