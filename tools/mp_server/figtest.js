// Teste de personagens: copia o primeiro jogador (1.5 ao lado) e troca de personagem a cada N segundos,
// passando por uma lista. O jogo registra "[MP] Teste apareceu como X" para comparar.
//   node figtest.js ws://localhost:8080 SALA segundos Nome1,Nome2,...
'use strict';
const [url = 'ws://localhost:8080', room = 'TESTE', secs = '14', list = 'Batman'] = process.argv.slice(2);
const names = list.split(',').filter(Boolean);
const ws = new WebSocket(url);
let target = 0, index = 0, since = 0;
ws.onopen = () => ws.send('j|' + room + '|Teste|1');
ws.onmessage = (ev) => {
  const msg = String(ev.data);
  if (msg[0] === 'l' && Number(msg.slice(2)) === target) target = 0;
  if (msg[0] !== 's') return;
  const parts = msg.split('|');
  const id = Number(parts[1]);
  if (!target) target = id;
  if (id !== target) return;
  const f = parts.slice(2);
  if (f.length < 12 || f[1] === '-') return;
  const now = Date.now();
  if (!since) { since = now; console.log(new Date().toISOString(), 'personagem', names[index]); }
  if (now - since > Number(secs) * 1000) {
    index++;
    since = now;
    if (index >= names.length) { console.log('fim'); process.exit(0); }
    console.log(new Date().toISOString(), 'personagem', names[index]);
  }
  f[1] = names[index];
  f[2] = (Number(f[2]) + 1.5).toFixed(3);
  ws.send('s|' + f.join('|'));
};
ws.onclose = () => process.exit(0);
