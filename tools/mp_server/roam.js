// Robo de teste que anda SOZINHO (nao copia ninguem): pega a area e o ponto de partida do primeiro jogador e corre
// em circulo em volta dele, para, pula de vez em quando.
//   node roam.js ws://localhost:8080 SALA personagem [raio] [centro_dx] [centro_dz]
'use strict';
const [url = 'ws://localhost:8080', room = 'TESTE', chr = 'Eris', radius = '3', cdx = '0', cdz = '0'] = process.argv.slice(2);
const R = Number(radius), speed = 1.4;
const ws = new WebSocket(url);
let area = null, cx = 0, cy = 0, cz = 0, angle = 0, t0 = Date.now();
const bots = new Set();
ws.onopen = () => ws.send('j|' + room + '|Livre ' + chr + '|1');
ws.onmessage = (ev) => {
  const msg = String(ev.data);
  if (msg[0] === 'p') {
    const [, pid, pname = ''] = msg.split('|');
    if (pname.startsWith('Robo') || pname.startsWith('Livre')) bots.add(Number(pid));
  }
  if (msg[0] !== 's' || area) return;
  const parts = msg.split('|');
  if (bots.has(Number(parts[1]))) return;
  const f = parts.slice(2);
  if (f.length < 12 || f[11] !== '1') return;
  area = f[0]; cx = Number(f[2]) + Number(cdx); cy = Number(f[3]); cz = Number(f[4]) + Number(cdz);
  console.log('area', area, 'centro', cx.toFixed(2), cz.toFixed(2));
};
setInterval(() => {
  if (!area) return;
  const t = (Date.now() - t0) / 1000;
  const moving = (t % 10) < 8; // 8 s correndo, 2 s parado
  if (moving) angle += speed / R * 0.05;
  const x = cx + R * Math.cos(angle), z = cz + R * Math.sin(angle);
  const dx = -Math.sin(angle), dz = Math.cos(angle); // tangente
  const jump = moving && (t % 3) < 0.15;
  const state = [area, chr, x.toFixed(3), cy.toFixed(3), z.toFixed(3), moving ? 6 : 3, dx.toFixed(3), dz.toFixed(3),
                 dx.toFixed(3), dz.toFixed(3), (jump ? 0x40004000 : 0).toString(16), 1].join('|');
  ws.send('s|' + state);
}, 50);
ws.onclose = () => process.exit(0);
ws.onerror = (e) => console.log('erro', e.message || e);
