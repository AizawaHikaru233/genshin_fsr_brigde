// 通过 CDP 驱动安装器里的 WebView2（先设 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222 再启动 exe）。
// 用法：
//   node wv.mjs eval "document.querySelector('.navitem[data-page=versions]').click()"
//   node wv.mjs shot <file.png> [--full]
//   node wv.mjs targets
import { writeFileSync } from 'node:fs';

const PORT = process.env.WV_PORT || '9222';
const base = `http://127.0.0.1:${PORT}`;

async function listTargets() {
  for (let i = 0; i < 40; i++) {
    try {
      const r = await fetch(`${base}/json/list`);
      const list = await r.json();
      const page = list.filter(t => t.type === 'page');
      if (page.length) return page;
    } catch {}
    await new Promise(r => setTimeout(r, 250));
  }
  throw new Error(`连不上 CDP (${base})：exe 是否用 --remote-debugging-port 启动了？`);
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.onopen = () => resolve(ws);
    ws.onerror = e => reject(new Error('websocket 失败: ' + (e.message || e.type)));
  });
}

let nextId = 1;
function send(ws, method, params = {}) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const onMsg = ev => {
      const msg = JSON.parse(ev.data);
      if (msg.id !== id) return;
      ws.removeEventListener('message', onMsg);
      if (msg.error) reject(new Error(`${method}: ${msg.error.message}`));
      else resolve(msg.result);
    };
    ws.addEventListener('message', onMsg);
    ws.send(JSON.stringify({ id, method, params }));
  });
}

const [cmd, ...rest] = process.argv.slice(2);
const targets = await listTargets();
if (cmd === 'targets') {
  for (const t of targets) console.log(`${t.title}  ${t.url}`);
  process.exit(0);
}

const target = targets.find(t => t.url.includes('index.html')) || targets[0];
const ws = await connect(target.webSocketDebuggerUrl);

if (cmd === 'eval') {
  const expr = rest.join(' ');
  const res = await send(ws, 'Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  console.log(JSON.stringify(res.result?.value ?? res.result, null, 2));
} else if (cmd === 'shot') {
  const file = rest[0];
  const full = rest.includes('--full');
  const res = await send(ws, 'Page.captureScreenshot', { format: 'png', captureBeyondViewport: full });
  writeFileSync(file, Buffer.from(res.data, 'base64'));
  console.log(`saved: ${file}`);
} else if (cmd === 'press') {
  // 真按一下：按住时读 computed transform，松开再读一次，用来验证 :active 效果
  const sel = rest[0];
  const box = await send(ws, 'Runtime.evaluate', {
    expression: `(()=>{const el=document.querySelector(${JSON.stringify(sel)});if(!el)return null;el.scrollIntoView({block:'center'});const r=el.getBoundingClientRect();return JSON.stringify({x:Math.round(r.x+r.width/2),y:Math.round(r.y+r.height/2)});})()`,
    returnByValue: true,
  });
  const raw = box.result?.value;
  if (!raw) { console.error(`找不到元素: ${sel}`); process.exit(3); }
  const { x, y } = JSON.parse(raw);
  const read = async () => {
    const r = await send(ws, 'Runtime.evaluate', {
      expression: `(()=>{const el=document.querySelector(${JSON.stringify(sel)});const s=getComputedStyle(el);return JSON.stringify({transform:s.transform,background:s.backgroundColor,active:el.matches(':active')});})()`,
      returnByValue: true,
    });
    return JSON.parse(r.result.value);
  };
  const before = await read();
  await send(ws, 'Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, button: 'none' });
  await send(ws, 'Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
  await new Promise(r => setTimeout(r, 220));
  const during = await read();
  await send(ws, 'Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  await new Promise(r => setTimeout(r, 320));
  const after = await read();
  console.log(JSON.stringify({ sel, before, during, after }, null, 2));
} else {
  console.error('用法: node wv.mjs eval "<js>" | shot <file.png> | press "<css>" | targets');
  process.exit(2);
}
ws.close();
