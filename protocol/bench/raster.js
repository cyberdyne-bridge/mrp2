// raster.js — 同一份工作負載的 Node/V8 版（Uint8Array + fill）
const W = 1232, H = 1488, LINES = 44, COLS = 72;
const buf = new Uint8Array(W * H * 3);

function clear() {
  buf.fill(0x0B);
}

function solid() {
  for (let ln = 0; ln < LINES; ln++) {
    const y = 20 + ln * 16;
    for (let c = 0; c < COLS; c++) {
      const x = 20 + c * 8;
      for (let dy = 0; dy < 12; dy++) {
        buf.fill(0xE8, ((y + dy) * W + x) * 3, ((y + dy) * W + x) * 3 + 7 * 3);
      }
    }
  }
}

function ink() {
  let seed = 12345;
  for (let ln = 0; ln < LINES; ln++) {
    const y = 20 + ln * 16;
    for (let c = 0; c < COLS; c++) {
      const x = 20 + c * 8;
      for (let dy = 0; dy < 12; dy++) {
        seed = (seed * 1103515245 + 12345) | 0;
        if (((seed >>> 16) % 100) < 22) {
          const x0 = (seed >>> 5) % 6;
          const off = ((y + dy) * W + x + x0) * 3;
          buf.fill(0xE8, off, off + 3 * 3);
        }
      }
    }
  }
}

const fns = { clear, solid, ink };
const best = { clear: 1e9, solid: 1e9, ink: 1e9 };
for (const k of Object.keys(fns)) {
  for (let i = 0; i < 20; i++) {
    const t = performance.now();
    fns[k]();
    const dt = performance.now() - t;
    if (dt < best[k]) best[k] = dt;
  }
}
console.log(`Node ${process.version}  ${W}x${H}`);
for (const k of Object.keys(best)) console.log(`  ${k.padEnd(6)} ${best[k].toFixed(2)} ms`);
