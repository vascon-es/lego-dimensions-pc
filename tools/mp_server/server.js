// Servidor de salas do multiplayer online do LEGO Dimensions PC (Fase 79, 4.0).
// Node.js puro (sem dependencias): `node server.js` (porta = PORT ou 8080). Serve no Render como "Web Service".
// So repassa mensagens: cada jogo manda o estado do seu jogador e recebe o dos outros da mesma sala.
//   cliente -> servidor: j|sala|nome|versao|senha   s|estado   k (mantem viva)   t|hora (ping)   q (saindo)
//                        L|1 ou L|0 (dono: tranca/destranca)   x|id (dono: expulsa)
//   servidor -> cliente: w|id   p|id|nome   l|id   s|id|estado   o|id (dono)   L|0/1 (trancada)   e|codigo|texto
// Salas: ninguem lista as salas; entra quem sabe o codigo (e a senha, se quem criou pos uma). O primeiro a entrar e o
// dono; se ele sair, o mais antigo vira dono. Expulso nao volta para a mesma sala (pelo IP) enquanto ela existir.
// Contra quem tenta adivinhar codigos/senhas: limite de entradas e de erros por IP.
'use strict';
const http = require('http');
const crypto = require('crypto');

const PORT = Number(process.env.PORT || 8080);
const MAX_PLAYERS = 16;
const PROTOCOLS = new Set([1, 2]); // 1 = sem senha nem dono (os primeiros testes)
const MAX_SOCKETS_PER_IP = 32;     // uma LAN inteira atras do mesmo IP cabe
const JOINS_PER_MINUTE = 20;       // tentativas de entrar por IP
const FAILS_TO_BLOCK = 8;          // erros (senha, trancada, expulso) em 10 min -> IP bloqueado 10 min
const rooms = new Map(); // sala -> { next, clients: Map(id -> client), owner, locked, pass, banned: Set(ip) }
const ips = new Map();   // ip -> { sockets, joins: [horas], fails: [horas], blockedUntil }

function frame(text) {
  const data = Buffer.from(text, 'utf8');
  let head;
  if (data.length < 126) {
    head = Buffer.from([0x81, data.length]);
  } else if (data.length < 65536) {
    head = Buffer.alloc(4);
    head[0] = 0x81; head[1] = 126; head.writeUInt16BE(data.length, 2);
  } else {
    head = Buffer.alloc(10);
    head[0] = 0x81; head[1] = 127; head.writeBigUInt64BE(BigInt(data.length), 2);
  }
  return Buffer.concat([head, data]);
}

function send(client, text) {
  if (!client.closed) client.socket.write(frame(text));
}

function broadcast(room, from, text) {
  for (const c of room.clients.values()) if (c !== from) send(c, text);
}

function clean(s, n) { return String(s || '').replace(/[|\r\n]/g, ' ').slice(0, n); }

function ipInfo(ip) {
  let info = ips.get(ip);
  if (!info) { info = { sockets: 0, joins: [], fails: [], blockedUntil: 0 }; ips.set(ip, info); }
  return info;
}

function recent(list, ms) {
  const now = Date.now();
  while (list.length && now - list[0] > ms) list.shift();
  return list.length;
}

function hashPass(pass) { return crypto.createHash('sha256').update('ldpc-mp|' + pass).digest(); }

// Recusa a entrada; os erros que alguem tentando adivinhar comete contam para bloquear o IP.
function refuse(client, code, text, counts) {
  if (counts) {
    const info = ipInfo(client.ip);
    info.fails.push(Date.now());
    if (recent(info.fails, 10 * 60000) >= FAILS_TO_BLOCK) info.blockedUntil = Date.now() + 10 * 60000;
  }
  send(client, 'e|' + code + '|' + text);
  setTimeout(() => leave(client), 200); // da tempo de a mensagem chegar
}

function setOwner(room, id) {
  room.owner = id;
  for (const c of room.clients.values()) send(c, 'o|' + id);
}

function leave(client) {
  if (client.closed) return;
  client.closed = true;
  const info = ips.get(client.ip);
  if (info && --info.sockets <= 0 && !info.fails.length && !info.joins.length && info.blockedUntil < Date.now()) ips.delete(client.ip);
  const room = client.room && rooms.get(client.room);
  if (room && room.clients.get(client.id) === client) {
    room.clients.delete(client.id);
    broadcast(room, client, 'l|' + client.id);
    if (room.clients.size === 0) {
      rooms.delete(client.room);
    } else if (room.owner === client.id) {
      setOwner(room, Math.min(...room.clients.keys())); // o mais antigo vira dono
    }
  }
  // fecha como manda o WebSocket (quadro de fechamento): o proxy do Render so repassa o fim da conexao assim; sem
  // ele o outro lado so percebia ~20 s depois
  try { client.socket.end(Buffer.from([0x88, 0x02, 0x03, 0xe8])); } catch (e) { /* ja fechada */ }
  setTimeout(() => client.socket.destroy(), 1000);
}

function join(client, text) {
  const [, roomName, name, version, pass = ''] = text.split('|');
  if (!PROTOCOLS.has(Number(version))) { refuse(client, 'versao', 'versao diferente do servidor', false); return; }
  const info = ipInfo(client.ip);
  if (info.blockedUntil > Date.now()) { refuse(client, 'limite', 'muitas tentativas; espere 10 minutos', false); return; }
  info.joins.push(Date.now());
  if (recent(info.joins, 60000) > JOINS_PER_MINUTE) { refuse(client, 'limite', 'muitas tentativas; espere um minuto', true); return; }
  const key = clean(roomName, 32).trim().toUpperCase();
  if (!key) { refuse(client, 'sala', 'sala vazia', false); return; }
  const secret = clean(pass, 64);
  let room = rooms.get(key);
  if (!room) { // quem cria a sala escolhe a senha (vazia = sem senha)
    room = { next: 1, clients: new Map(), owner: 0, locked: false, pass: secret ? hashPass(secret) : null, banned: new Set() };
    rooms.set(key, room);
  } else {
    if (room.banned.has(client.ip)) { refuse(client, 'expulso', 'voce foi expulso desta sala', true); return; }
    if (room.locked) { refuse(client, 'trancada', 'a sala esta trancada', true); return; }
    if (room.pass && !crypto.timingSafeEqual(room.pass, hashPass(secret))) { refuse(client, 'senha', 'senha errada', true); return; }
    if (room.clients.size >= MAX_PLAYERS) { refuse(client, 'cheia', 'sala cheia (16)', false); return; }
  }
  client.room = key;
  client.id = room.next++;
  client.name = clean(name, 24) || 'Jogador';
  send(client, 'w|' + client.id);
  for (const other of room.clients.values()) send(client, 'p|' + other.id + '|' + other.name);
  room.clients.set(client.id, client);
  broadcast(room, client, 'p|' + client.id + '|' + client.name);
  if (!room.owner) room.owner = client.id;
  send(client, 'o|' + room.owner);
  send(client, 'L|' + (room.locked ? 1 : 0));
  console.log(new Date().toISOString(), 'sala +', client.name, '(' + room.clients.size + ')'); // o codigo nao vai para o log
}

function onMessage(client, text) {
  client.last = Date.now();
  const kind = text[0];
  if (kind === 't') { send(client, text.slice(0, 40)); return; } // ping: volta para quem mandou (latencia)
  if (kind === 'q') { leave(client); return; } // saindo (o proxy do Render nao repassa o fechamento da conexao)
  if (kind === 'j') { if (!client.room) join(client, text); return; }
  const room = client.room && rooms.get(client.room);
  if (!room) return;
  if (kind === 's') {
    if (text.length > 600) return;
    broadcast(room, client, 's|' + client.id + '|' + text.slice(2));
  } else if (kind === 'L' && room.owner === client.id) {
    room.locked = text[2] === '1';
    for (const c of room.clients.values()) send(c, 'L|' + (room.locked ? 1 : 0));
  } else if (kind === 'x' && room.owner === client.id) {
    const target = room.clients.get(Number(text.slice(2)));
    if (!target || target === client) return;
    room.banned.add(target.ip);
    send(target, 'e|expulso|o dono da sala te expulsou');
    setTimeout(() => leave(target), 200);
  }
}

function onData(client, chunk) {
  client.buffer = Buffer.concat([client.buffer, chunk]);
  for (;;) {
    const b = client.buffer;
    if (b.length < 2) return;
    const opcode = b[0] & 0x0f;
    const masked = (b[1] & 0x80) !== 0;
    let length = b[1] & 0x7f;
    let offset = 2;
    if (length === 126) { if (b.length < 4) return; length = b.readUInt16BE(2); offset = 4; }
    else if (length === 127) { if (b.length < 10) return; length = Number(b.readBigUInt64BE(2)); offset = 10; }
    if (length > 65536) { leave(client); return; }
    const maskOffset = offset;
    if (masked) offset += 4;
    if (b.length < offset + length) return;
    const payload = Buffer.from(b.subarray(offset, offset + length));
    if (masked) for (let i = 0; i < payload.length; i++) payload[i] ^= b[maskOffset + (i & 3)];
    client.buffer = b.subarray(offset + length);
    if (opcode === 0x8) { leave(client); return; }
    if (opcode === 0x9) { // ping -> pong
      client.socket.write(Buffer.concat([Buffer.from([0x8a, payload.length]), payload]));
      continue;
    }
    if (opcode === 0x1 || opcode === 0x0) {
      client.parts.push(payload);
      if (b[0] & 0x80) { onMessage(client, Buffer.concat(client.parts).toString('utf8')); client.parts = []; }
    }
    if (opcode === 0xa) client.last = Date.now();
  }
}

// IP de quem conecta: no Render vem do proxy (primeiro de x-forwarded-for)
function clientIp(req) {
  const fwd = String(req.headers['x-forwarded-for'] || '').split(',')[0].trim();
  return fwd || req.socket.remoteAddress || '?';
}

const server = http.createServer((req, res) => {
  let players = 0;
  for (const r of rooms.values()) players += r.clients.size;
  res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8' });
  res.end('LEGO Dimensions PC - servidor do multiplayer online\nsalas: ' + rooms.size + ', jogadores: ' + players + '\n');
});

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key || String(req.headers.upgrade).toLowerCase() !== 'websocket') { socket.destroy(); return; }
  const ip = clientIp(req);
  const info = ipInfo(ip);
  if (info.sockets >= MAX_SOCKETS_PER_IP) { socket.destroy(); return; }
  info.sockets++;
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  socket.setNoDelay(true);
  const client = { socket, ip, buffer: Buffer.alloc(0), parts: [], room: null, id: 0, name: '', closed: false, last: Date.now() };
  socket.on('data', (chunk) => onData(client, chunk));
  socket.on('close', () => leave(client));
  socket.on('error', () => leave(client));
  setTimeout(() => { if (!client.room && !client.closed) leave(client); }, 30000); // conectou e nao entrou em sala
});

// Quem sumiu: o proxy do Render nao avisa quando a conexao cai, entao vale o silencio. O jogo manda um ping por
// segundo (e o estado 30 vezes por segundo); 12 s sem nada = saiu.
setInterval(() => {
  const now = Date.now();
  for (const room of rooms.values()) {
    for (const c of room.clients.values()) if (now - c.last > 12000) leave(c);
  }
}, 3000);

setInterval(() => { // ping (o proxy do Render fecha conexoes paradas)
  const now = Date.now();
  for (const room of rooms.values()) {
    for (const c of room.clients.values()) c.socket.write(Buffer.from([0x89, 0]));
  }
  for (const [ip, info] of ips) { // esquece IPs parados
    recent(info.joins, 60000);
    recent(info.fails, 10 * 60000);
    if (!info.sockets && !info.joins.length && !info.fails.length && info.blockedUntil < now) ips.delete(ip);
  }
}, 20000);

server.listen(PORT, () => console.log('multiplayer: porta', PORT));
