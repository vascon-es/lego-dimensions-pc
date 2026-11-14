# Online multiplayer room server

A tiny relay for the port's online multiplayer (4.0). Each game sends its own player's state (area, character,
position, movement, jump/attack buttons) about 20 times per second; the server forwards it to the other players in
the same room. Up to 16 players per room. No game data passes through it, and nothing is stored.

It is plain Node.js (18 or newer) with no dependencies.

## Run it locally

```
node server.js            # listens on port 8080 (or $PORT)
```

The launcher comes with the public server `wss://lego-dimensions-mp-server.onrender.com`. To use your own, in the
launcher: Settings > Game > Multiplayer room > server `ws://localhost:8080` (or `ws://<your LAN IP>:8080` for
players on the same network).

## Host it on Render (free)

1. New > Web Service > connect this repository.
2. Root Directory: `tools/mp_server`. Runtime: Node. Build command: (empty). Start command: `node server.js`.
3. Instance type: Free. Create.
4. The address is `wss://<service-name>.onrender.com`. Opening `https://<service-name>.onrender.com` in a browser
   shows how many rooms and players are online.

A free Render service sleeps after 15 minutes without players; the first connection then takes up to a minute
(the game keeps retrying on its own).

## Test with one game

`bot.js` joins a room and copies the first player it sees, 1.5 units to the side, as another character:

```
node bot.js ws://localhost:8080 TEST Eris 1.5
```

`roam.js` joins the area of the first player and runs on its own in a circle around them (stops and jumps now
and then), to check that other players move freely:

```
node roam.js ws://localhost:8080 TEST Marty 3
```

## Private rooms

Rooms are never listed: only who knows the code gets in. On top of that:

- **Password (optional)**: whoever creates the room sets it; everyone else needs the same one.
- **Owner**: the first player in the room. If they leave, the oldest player becomes the owner. In the game (F3, or
  "Multiplayer room" in the F1 menu) the owner can lock the room (nobody new gets in) and kick players. A kicked
  player cannot come back to that room (by IP) while it exists.
- **Limits per IP**: 20 join attempts per minute; 8 refusals (wrong password, locked, kicked) in 10 minutes block
  the IP for 10 minutes; at most 32 connections at once.

## Protocol

Text messages, fields separated by `|`.

Game to server:

- `j|room|name|2|password`: join a room (protocol 2; 1 = no password, still accepted)
- `s|area|character|x|y|z|gear|dx|dz|fx|fz|buttons|ingame|rtt`: own player state
- `k`: keep-alive (in menus); `t|time`: ping (echoed back)
- `L|1` / `L|0`: lock / unlock (owner only); `x|id`: kick (owner only)

Server to game:

- `w|id`: joined, this is your id
- `p|id|name`: a player is in / joined the room
- `l|id`: a player left
- `s|id|state`: another player's state
- `o|id`: the room owner; `L|0` / `L|1`: unlocked / locked
- `e|code|text`: refused (`senha` wrong password, `trancada` locked, `expulso` kicked, `cheia` full, `versao`,
  `limite` too many attempts); the server then closes the connection
- `e|reason`: refused (room full, wrong version)
