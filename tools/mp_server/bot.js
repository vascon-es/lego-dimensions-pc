// Robo de teste do multiplayer: entra na sala e repete o primeiro jogador que aparecer, deslocado para o lado,
// com outro personagem. Serve para testar com UM jogo aberto.
//   node bot.js ws://localhost:8080 SALA [personagem] [deslocamento_x] [atraso_ms] [deslocamento_z]
'use strict';
const [url = 'ws://localhost:8080', room = 'TESTE', chr = '', dx = '1.5', delay = '0', dz = '0'] = process.argv.slice(2);
const ws = new WebSocket(url);
let target = 0;
const bots = new Set(); // ids de outros robos (nao copia robo)
ws.onopen = () => ws.send('j|' + room + '|' + (chr ? 'Robo ' + chr : 'Robo') + '|1');
ws.onmessage = (ev) => {
  const msg = String(ev.data);
  if (msg[0] === 'w') console.log('robo entrou, id', msg.slice(2));
  if (msg[0] === 'p') {
    console.log('na sala:', msg.slice(2));
    const [, pid, pname = ''] = msg.split('|');
    if (pname.startsWith('Robo')) bots.add(Number(pid));
  }
  if (msg[0] === 'l' && Number(msg.slice(2)) === target) target = 0; // saiu: copia o proximo que aparecer
  if (msg[0] !== 's') return;
  const parts = msg.split('|');
  const id = Number(parts[1]);
  if (bots.has(id)) return;
  if (!target) { target = id; console.log('copiando o jogador', id); }
  if (id !== target) return;
  const f = parts.slice(2); // area|char|x|y|z|gear|dx|dz|fx|fz|held|ingame
  if (f.length < 12) return;
  if (chr && f[1] !== '-') f[1] = chr;
  f[2] = (Number(f[2]) + Number(dx)).toFixed(3);
  f[4] = (Number(f[4]) + Number(dz)).toFixed(3);
  const out = 's|' + f.join('|');
  if (Number(delay) > 0) setTimeout(() => ws.send(out), Number(delay)); else ws.send(out);
};
ws.onclose = () => { console.log('conexao fechada'); process.exit(0); };
ws.onerror = (e) => console.log('erro', e.message || e);
