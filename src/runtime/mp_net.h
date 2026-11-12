#pragma once
// Fase 79 (4.0): cliente do servidor de salas do multiplayer online (WebSocket via WinHTTP, thread propria).
// Protocolo em texto, campos separados por '|':
//   cliente -> servidor: j|sala|nome|versao|senha   s|estado   L|1/0 (dono: tranca)   x|id (dono: expulsa)
//   servidor -> cliente: w|id   p|id|nome   l|id   s|id|estado   o|id (dono)   L|0/1   e|codigo|texto
#include <string>
#include <vector>

namespace runtime::mp_net {

constexpr int kProtocol = 2; // 2: senha, dono, trancar, expulsar

struct Peer {
    int id = 0;
    std::string name;
    std::string state;      // ultimo estado recebido (texto do jogo)
    double state_time = 0;  // quando chegou (segundos, relogio local)
    unsigned seq = 0;       // conta estados recebidos (para o jogo saber que e novo)
};

// url = ws://host[:porta][/caminho] ou wss://...  Volta false se a url nao serve.
// password: so vale para quem CRIA a sala (os outros precisam da mesma); vazia = sala sem senha.
bool Start(const std::string& url, const std::string& room, const std::string& name, const std::string& password = {});
void Stop();
// Estado do jogador local (o ultimo vence); a thread manda ~30 vezes por segundo.
void SetLocalState(const std::string& state);
std::vector<Peer> Peers();
int SelfId();             // 0 enquanto nao conectou
int RttMs();              // ida e volta ate o servidor (media), 0 = ainda nao medido
std::string StatusText(); // para o log
std::string RoomCode();
std::string SelfName();
int OwnerId();            // dono da sala (0 = ainda nao sabe)
bool RoomLocked();
void SetRoomLocked(bool locked); // so o dono (o servidor ignora os outros)
void Kick(int id);               // so o dono; o expulso nao volta para esta sala
// Recusa definitiva do servidor ("senha", "trancada", "expulso", "versao", "cheia"): nao tenta de novo. Vazio = nenhuma.
std::string RefusedCode();
double Now();

} // namespace runtime::mp_net
