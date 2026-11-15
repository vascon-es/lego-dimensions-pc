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
    bool direct = false;    // Fase 82: ligacao direta (P2P) funcionando com ele
    int direct_rtt_ms = 0;  // ida e volta direta (0 = ainda nao medida)
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
std::string RoomPassword(); // Fase 83: senha da sala em que esta (para o convite)
std::string SelfName();
int OwnerId();            // dono da sala (0 = ainda nao sabe)
bool RoomLocked();
void SetRoomLocked(bool locked); // so o dono (o servidor ignora os outros)
void Kick(int id);               // so o dono; o expulso nao volta para esta sala
// Recusa definitiva do servidor ("senha", "trancada", "expulso", "versao", "cheia"): nao tenta de novo. Vazio = nenhuma.
std::string RefusedCode();
// Fase 80: eventos de jogo (o que um jogador fez no mundo dele, ex. quebrou um objeto) para os outros da sala.
void SendEvent(const std::string& payload);                // vai dentro do "s" ("s|!payload"): o servidor so repassa
std::vector<std::pair<int, std::string>> TakeEvents();     // (id de quem mandou, payload) chegados desde a ultima vez
double Now();
// Fase 82: conexao direta (P2P) por UDP. O servidor continua achando os jogadores e levando os eventos; o estado de
// cada jogador vai direto quando a NAT deixa (STUN + abertura dos dois lados), senao continua pelo servidor.
void SetP2P(bool on);
bool P2PEnabled();
// Atraso de um estado desse jogador ate nos (segundos): direto = metade da ida e volta com ele; servidor = as duas metades.
double LatencySeconds(int peer, int their_server_rtt_ms);

// Fase 83: painel da rede (F4, estilo Radmin VPN)
enum class Nat {
    kTesting,     // perguntando aos servidores STUN
    kOpen,        // a mesma porta para qualquer destino: a conexao direta costuma funcionar
    kSymmetric,   // uma porta por destino: a conexao direta quase sempre falha (fica pelo servidor)
    kBlocked,     // nenhuma resposta UDP: firewall ou rede que bloqueia UDP
    kUnavailable, // sem socket UDP
};
struct NetInfo {
    Nat nat = Nat::kTesting;
    bool p2p_on = true, connected = false;
    int server_rtt_ms = 0, udp_port = 0, peers = 0, direct = 0;
};
NetInfo GetNetInfo();
struct ChatLine {
    int from = 0;
    std::string name, text;
    double time = 0;
    bool self = false;
};
void SendChat(const std::string& text);   // vai pela sala (as versoes antigas ignoram)
std::vector<ChatLine> Chat();             // ultimas 60 mensagens
unsigned ChatSeq();                       // cresce a cada mensagem nova (para o aviso na tela)
struct PingResult {
    bool valid = false, pending = false;  // pending: esperando a resposta pelo servidor (ate 4 s)
    int direct_ms = -1, server_ms = -1;   // -1 = sem ligacao direta / sem resposta
    double started = 0;
};
void PingPeer(int id);                    // teste: ida e volta pelo servidor (+ a direta, se houver)
PingResult LastPing(int id);
std::vector<std::pair<std::string, double>> LeftPlayers(); // quem saiu da sala nesta sessao (nome, quando)

} // namespace runtime::mp_net
