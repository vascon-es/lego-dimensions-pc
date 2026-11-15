#include "runtime/mp_net.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace runtime::mp_net {
namespace {

struct Url {
    bool secure = false;
    std::wstring host, path;
    INTERNET_PORT port = 0;
};

std::mutex g_mutex;
std::map<int, Peer> g_peers;
std::string g_local_state;
bool g_local_dirty = false;
std::string g_status = "desligado";
std::atomic<int> g_self{0};
std::atomic<int> g_rtt_ms{0};
std::atomic<int> g_owner{0};
std::atomic<bool> g_locked{false};
std::string g_refused;               // codigo da recusa definitiva (g_mutex)
std::vector<std::string> g_commands; // L/x do dono e eventos de jogo, mandados pela thread (g_mutex)
std::vector<std::pair<int, std::string>> g_events_in; // eventos de jogo recebidos (g_mutex)
std::string g_password;
std::atomic<bool> g_in_session{false}; // conectado (Stop espera o "q" sair)
std::atomic<bool> g_end_session{false}; // o servidor recusou: encerra (o proxy do Render nao repassa o fechamento)
std::atomic<bool> g_stop{false};
bool g_started = false; // a thread fica solta (detach): no fim do processo nao ha join
Url g_url;
std::string g_room, g_name;
const auto g_epoch = std::chrono::steady_clock::now();

// ---- Fase 82: conexao direta (P2P) ----
// O servidor de salas continua achando os jogadores e levando os eventos do mundo (confiaveis, em ordem). O estado de
// cada jogador (posicao, ~30 por segundo) vai direto de PC para PC por UDP quando a NAT deixa:
//  1. cada um abre um socket UDP e pergunta a um servidor STUN publico o seu endereco visto da internet;
//  2. manda pela sala (evento "~p", que as versoes antigas ignoram) um codigo secreto + os seus enderecos
//     (internet, rede local e 127.0.0.1);
//  3. os dois lados mandam "ola" para os enderecos um do outro ao mesmo tempo (abre o caminho nas duas NATs);
//  4. a ligacao so vale como direta depois de uma resposta ("a") ao nosso ola: o caminho funciona nos dois sentidos.
// Sem caminho direto com alguem (NAT simetrica, firewall, versao antiga), o estado continua indo pelo servidor.
// Pacotes: LD2P|h|de|codigo|tempo  LD2P|a|de|codigo|tempo  LD2P|s|de|codigo|seq|estado
struct Link {
    unsigned long long token = 0; // codigo do outro (chegou pela sala)
    std::vector<sockaddr_in> candidates;
    sockaddr_in addr{};           // de onde os pacotes dele chegam
    bool has_addr = false;
    bool direct = false;          // recebemos resposta ao nosso ola ha pouco
    double last_ack = -100, last_state_rx = -100, last_punch = -100, last_hello = -100, announced = -100;
    int rtt_ms = 0;
    unsigned last_seq = 0;
};
std::map<int, Link> g_links; // g_mutex
std::atomic<bool> g_p2p_on{true};
SOCKET g_udp = INVALID_SOCKET;
std::thread g_udp_thread;
std::atomic<bool> g_udp_run{false};
unsigned long long g_token = 0;
std::string g_candidates;    // "ip:porta,ip:porta" (g_mutex)
std::string g_public_ep;     // endereco visto pelo STUN (g_mutex)
std::vector<sockaddr_in> g_stun_servers;
unsigned char g_stun_tx[12] = {};
std::atomic<bool> g_announce_now{false};
double g_last_announce = -100, g_last_stun = -100, g_last_announce_reply = -100;
int g_stun_tries = 0;
unsigned g_udp_seq = 0;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool ParseUrl(const std::string& text, Url* out) {
    std::string rest;
    if (text.rfind("wss://", 0) == 0) {
        out->secure = true;
        rest = text.substr(6);
    } else if (text.rfind("ws://", 0) == 0) {
        rest = text.substr(5);
    } else if (text.rfind("https://", 0) == 0) { // aceita o endereco do site do servidor
        out->secure = true;
        rest = text.substr(8);
    } else if (text.rfind("http://", 0) == 0) {
        rest = text.substr(7);
    } else {
        out->secure = true;
        rest = text;
    }
    const size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    out->path = Widen(slash == std::string::npos ? "/" : rest.substr(slash));
    const size_t colon = hostport.rfind(':');
    out->port = out->secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (colon != std::string::npos) {
        out->port = static_cast<INTERNET_PORT>(std::atoi(hostport.c_str() + colon + 1));
        hostport.resize(colon);
    }
    out->host = Widen(hostport);
    return !out->host.empty() && out->port != 0;
}

void SetStatus(const std::string& s) {
    {
        std::lock_guard lock(g_mutex);
        if (g_status == s) return;
        g_status = s;
    }
    std::printf("[MP] %s\n", s.c_str());
    std::fflush(stdout);
}

std::string Clean(std::string s, size_t limit) { // nada de '|' nem quebra de linha nos campos
    for (char& c : s) {
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    }
    if (s.size() > limit) s.resize(limit);
    return s;
}

// ------------------------------ P2P: utilidades ------------------------------
std::string EpText(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool ParseEp(const std::string& text, sockaddr_in* out) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    const int port = std::atoi(text.c_str() + colon + 1);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, text.substr(0, colon).c_str(), &a.sin_addr) != 1) return false;
    a.sin_port = htons(static_cast<u_short>(port));
    *out = a;
    return true;
}

bool SameEp(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string Hex(unsigned long long v) {
    char t[24];
    std::snprintf(t, sizeof(t), "%016llx", v);
    return t;
}

void SendUdp(const sockaddr_in& to, const std::string& data) {
    if (g_udp == INVALID_SOCKET) return;
    sendto(g_udp, data.data(), static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

std::string Packet(char kind, const std::string& tail) {
    return std::string("LD2P|") + kind + "|" + std::to_string(g_self.load()) + "|" + Hex(g_token) + "|" + tail;
}

// Enderecos para os outros tentarem: o da internet (STUN), os da rede local e o do proprio PC. g_mutex.
void RebuildCandidates() {
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (g_udp == INVALID_SOCKET || getsockname(g_udp, reinterpret_cast<sockaddr*>(&bound), &len) != 0) return;
    const u_short port = bound.sin_port;
    std::vector<std::string> list;
    if (!g_public_ep.empty()) list.push_back(g_public_ep);
    char host[256] = {};
    if (gethostname(host, sizeof(host)) == 0) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* r = res; r && list.size() < 6; r = r->ai_next) {
                sockaddr_in a = *reinterpret_cast<sockaddr_in*>(r->ai_addr);
                a.sin_port = port;
                const std::string t = EpText(a);
                if (std::find(list.begin(), list.end(), t) == list.end()) list.push_back(t);
            }
            freeaddrinfo(res);
        }
    }
    sockaddr_in loop{};
    loop.sin_family = AF_INET;
    loop.sin_port = port;
    inet_pton(AF_INET, "127.0.0.1", &loop.sin_addr);
    list.push_back(EpText(loop)); // dois jogos no mesmo PC (testes)
    std::string text;
    for (const std::string& t : list) text += (text.empty() ? "" : ",") + t;
    if (text != g_candidates) {
        g_candidates = text;
        g_announce_now = true;
    }
}

void SendStun() {
    if (g_stun_servers.empty()) return;
    unsigned char req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    std::memcpy(req + 8, g_stun_tx, 12);
    const sockaddr_in& to = g_stun_servers[g_stun_tries++ % g_stun_servers.size()];
    sendto(g_udp, reinterpret_cast<const char*>(req), sizeof(req), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Resposta do STUN: o nosso endereco visto da internet (XOR-MAPPED-ADDRESS, ou MAPPED-ADDRESS nos antigos).
void HandleStun(const unsigned char* b, int n) {
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01 || b[4] != 0x21 || b[5] != 0x12 || b[6] != 0xA4 || b[7] != 0x42) return;
    if (std::memcmp(b + 8, g_stun_tx, 12) != 0) return;
    int pos = 20;
    while (pos + 4 <= n) {
        const int type = (b[pos] << 8) | b[pos + 1], len = (b[pos + 2] << 8) | b[pos + 3];
        const unsigned char* v = b + pos + 4;
        if (pos + 4 + len > n) break;
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            u_short port = static_cast<u_short>((v[2] << 8) | v[3]);
            unsigned char ip[4] = {v[4], v[5], v[6], v[7]};
            if (type == 0x0020) {
                port ^= 0x2112;
                const unsigned char cookie[4] = {0x21, 0x12, 0xA4, 0x42};
                for (int i = 0; i < 4; ++i) ip[i] ^= cookie[i];
            }
            a.sin_port = htons(port);
            std::memcpy(&a.sin_addr, ip, 4);
            const std::string ep = EpText(a);
            bool changed = false;
            {
                std::lock_guard lock(g_mutex);
                changed = g_public_ep != ep;
                g_public_ep = ep;
                RebuildCandidates();
            }
            if (changed) {
                std::printf("[MP] P2P: endereco na internet descoberto (STUN)\n");
                std::fflush(stdout);
            }
            return;
        }
        pos += 4 + ((len + 3) & ~3);
    }
}

std::vector<std::string> Split(const std::string& s, char sep, size_t max_parts) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (out.size() + 1 < max_parts) {
        const size_t bar = s.find(sep, pos);
        if (bar == std::string::npos) break;
        out.push_back(s.substr(pos, bar - pos));
        pos = bar + 1;
    }
    out.push_back(s.substr(pos));
    return out;
}

void HandleUdp(const char* data, int n, const sockaddr_in& from) {
    if (n >= 20 && static_cast<unsigned char>(data[0]) == 0x01) {
        HandleStun(reinterpret_cast<const unsigned char*>(data), n);
        return;
    }
    if (n < 8 || std::memcmp(data, "LD2P|", 5) != 0 || !g_p2p_on) return;
    const std::vector<std::string> f = Split(std::string(data, n), '|', 6);
    if (f.size() < 5 || f[1].size() != 1) return;
    const char kind = f[1][0];
    const int id = std::atoi(f[2].c_str());
    const unsigned long long token = std::strtoull(f[3].c_str(), nullptr, 16);
    const double now = Now();
    std::string reply, joined_name;
    {
        std::lock_guard lock(g_mutex);
        auto peer = g_peers.find(id);
        auto it = g_links.find(id);
        if (id == g_self || peer == g_peers.end() || it == g_links.end() || it->second.token != token || !token) return;
        Link& l = it->second;
        l.addr = from;
        l.has_addr = true;
        if (kind == 'h') {
            reply = Packet('a', f[4]); // devolve o tempo dele: ele mede a ida e volta
        } else if (kind == 'a') {
            const int rtt = static_cast<int>((now - std::atof(f[4].c_str())) * 1000.0);
            if (rtt >= 0 && rtt < 5000) l.rtt_ms = l.rtt_ms ? (l.rtt_ms * 3 + rtt) / 4 : rtt;
            l.last_ack = now;
            if (!l.direct) {
                l.direct = true;
                joined_name = peer->second.name;
            }
        } else if (kind == 's' && f.size() >= 6) {
            const unsigned seq = static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
            if (seq > l.last_seq || l.last_seq - seq > 1000000u) { // mais novo (ou o outro reiniciou a contagem)
                l.last_seq = seq;
                l.last_state_rx = now;
                Peer& p = peer->second;
                p.state = f[5];
                p.state_time = now;
                ++p.seq;
            }
        }
    }
    if (!reply.empty()) SendUdp(from, reply);
    if (!joined_name.empty()) {
        std::printf("[MP] conexao direta (P2P) com %s\n", joined_name.c_str());
        std::fflush(stdout);
    }
}

void UdpReceiver() {
    char buffer[2048];
    while (g_udp_run) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(g_udp, &set);
        timeval tv{0, 100000};
        if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int n = recvfrom(g_udp, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n > 0 && from.sin_family == AF_INET) HandleUdp(buffer, n, from);
    }
}

void P2POpen() {
    static bool wsa = false;
    if (!wsa) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        wsa = true;
    }
    if (!g_token) {
        std::random_device rd;
        g_token = (static_cast<unsigned long long>(rd()) << 32) ^ rd() ^ static_cast<unsigned long long>(GetTickCount64());
        if (!g_token) g_token = 1;
    }
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET) {
        std::printf("[MP] P2P indisponivel (socket UDP); tudo vai pelo servidor\n");
        return;
    }
    // Sem isto, um "porta fechada" (ICMP) de um endereco que nao responde faz o proximo recvfrom falhar.
    BOOL no_reset = FALSE;
    DWORD ignored = 0;
    WSAIoctl(g_udp, _WSAIOW(IOC_VENDOR, 12), &no_reset, sizeof(no_reset), nullptr, 0, &ignored, nullptr, nullptr);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    const char* port_env = std::getenv("LD_MP_P2P_PORT"); // porta fixa (quem quiser abrir no roteador)
    any.sin_port = htons(static_cast<u_short>(port_env ? std::atoi(port_env) : 0));
    if (bind(g_udp, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        any.sin_port = 0;
        bind(g_udp, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    }
    std::random_device rd;
    for (unsigned char& c : g_stun_tx) c = static_cast<unsigned char>(rd());
    g_stun_servers.clear();
    for (const char* server : {"stun.cloudflare.com", "stun.l.google.com"}) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        const char* port = std::strcmp(server, "stun.l.google.com") == 0 ? "19302" : "3478";
        if (getaddrinfo(server, port, &hints, &res) == 0) {
            if (res) g_stun_servers.push_back(*reinterpret_cast<sockaddr_in*>(res->ai_addr));
            freeaddrinfo(res);
        }
    }
    {
        std::lock_guard lock(g_mutex);
        g_public_ep.clear();
        RebuildCandidates();
    }
    g_stun_tries = 0;
    g_last_stun = -100;
    g_udp_run = true;
    g_udp_thread = std::thread(UdpReceiver);
}

void P2PClose() {
    g_udp_run = false;
    if (g_udp_thread.joinable()) g_udp_thread.join();
    if (g_udp != INVALID_SOCKET) closesocket(g_udp);
    g_udp = INVALID_SOCKET;
}

// Chamado pela thread da conexao a cada ~33 ms: STUN, anuncio, "ola" para abrir caminho e manter vivo.
void P2PTick(double now) {
    if (g_udp == INVALID_SOCKET) return;
    bool have_public = false;
    {
        std::lock_guard lock(g_mutex);
        have_public = !g_public_ep.empty();
    }
    // STUN: no comeco ate responder (uma tentativa por segundo, 8 no maximo); depois a cada 25 s (a NAT muda o mapa)
    if ((!have_public && g_stun_tries < 8 && now - g_last_stun > 1.0) || now - g_last_stun > 25.0) {
        g_last_stun = now;
        SendStun();
    }
    if (!g_p2p_on || !g_self) return;
    std::vector<std::pair<sockaddr_in, std::string>> out;
    std::vector<std::string> lost;
    bool any_missing = false;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [id, p] : g_peers) {
            if (id == g_self) continue;
            auto it = g_links.find(id);
            if (it == g_links.end() || !it->second.direct) any_missing = true;
        }
        char t[32];
        std::snprintf(t, sizeof(t), "%.4f", now);
        for (auto& [id, l] : g_links) {
            if (!l.token) continue;
            if (l.direct && now - l.last_ack > 4.0) { // sem resposta: o caminho fechou
                l.direct = false;
                auto peer = g_peers.find(id);
                lost.push_back(peer == g_peers.end() ? std::to_string(id) : peer->second.name);
            }
            if (l.direct) {
                if (now - l.last_hello > 1.0) { // mantem o caminho aberto e mede a ida e volta
                    l.last_hello = now;
                    out.push_back({l.addr, Packet('h', t)});
                }
                continue;
            }
            const double every = now - l.announced < 20.0 ? 0.25 : 2.0; // logo depois do anuncio: insiste
            if (now - l.last_punch < every) continue;
            l.last_punch = now;
            for (const sockaddr_in& c : l.candidates) out.push_back({c, Packet('h', t)});
            if (l.has_addr && std::none_of(l.candidates.begin(), l.candidates.end(),
                                           [&](const sockaddr_in& c) { return SameEp(c, l.addr); })) {
                out.push_back({l.addr, Packet('h', t)}); // o endereco de onde ele ja falou com a gente
            }
        }
        // Anuncio pela sala: quando algo mudou, ou de 5 em 5 s enquanto falta caminho direto com alguem
        const bool announce = g_announce_now.exchange(false) || (any_missing && now - g_last_announce > 5.0);
        if (announce && !g_candidates.empty() && g_commands.size() < 256) {
            g_last_announce = now;
            g_commands.push_back("s|!~p|" + Hex(g_token) + "|" + g_candidates);
        }
    }
    for (const auto& [to, data] : out) SendUdp(to, data);
    for (const std::string& name : lost) {
        std::printf("[MP] conexao direta com %s caiu; usando o servidor\n", name.c_str());
    }
    if (!lost.empty()) std::fflush(stdout);
}

// "~p|codigo|ip:porta,..." chegou pela sala (g_mutex travado)
void OnAnnouncement(int id, const std::string& body) {
    if (id == g_self) return;
    const std::vector<std::string> f = Split(body, '|', 2);
    if (f.size() < 2) return;
    Link& l = g_links[id];
    const unsigned long long token = std::strtoull(f[0].c_str(), nullptr, 16);
    if (l.token != token) { // outro jogo (reconectou): comeca do zero
        l = Link{};
        l.token = token;
    }
    l.candidates.clear();
    for (const std::string& ep : Split(f[1], ',', 8)) {
        sockaddr_in a{};
        if (ParseEp(ep, &a)) l.candidates.push_back(a);
    }
    l.announced = Now();
    l.last_punch = -100;
    // quem chegou depois ainda nao tem o nosso: responde (no maximo 1 vez por segundo)
    if (!l.direct && l.announced - g_last_announce_reply > 1.0) {
        g_last_announce_reply = l.announced;
        g_announce_now = true;
    }
}

// Estado vai direto para quem tem caminho; devolve true se ainda falta alguem (entao vai pelo servidor tambem).
bool SendStateDirect(const std::string& state) {
    std::vector<sockaddr_in> to;
    bool need_relay = false;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [id, p] : g_peers) {
            if (id == g_self) continue;
            auto it = g_links.find(id);
            if (g_p2p_on && it != g_links.end() && it->second.direct) {
                to.push_back(it->second.addr);
            } else {
                need_relay = true;
            }
        }
    }
    if (!to.empty()) {
        const std::string packet = Packet('s', std::to_string(++g_udp_seq) + "|" + state);
        for (const sockaddr_in& a : to) SendUdp(a, packet);
    }
    return need_relay || to.empty();
}

void Handle(const std::string& msg) {
    const size_t a = msg.find('|');
    const std::string kind = msg.substr(0, a);
    const std::string rest = a == std::string::npos ? std::string() : msg.substr(a + 1);
    if (kind == "w") {
        g_self = std::atoi(rest.c_str());
        SetStatus("conectado na sala " + g_room + " (id " + std::to_string(g_self.load()) + ")");
        g_announce_now = true;
        return;
    }
    if (kind == "t") { // nosso ping voltou: ida e volta ate o servidor
        const double sent = std::atof(rest.c_str());
        const int rtt = static_cast<int>((Now() - sent) * 1000.0);
        if (rtt >= 0 && rtt < 5000) {
            const int old = g_rtt_ms.load();
            g_rtt_ms = old ? (old * 3 + rtt) / 4 : rtt;
        }
        return;
    }
    if (kind == "o") {
        g_owner = std::atoi(rest.c_str());
        return;
    }
    if (kind == "L") {
        g_locked = rest == "1";
        return;
    }
    if (kind == "e") { // e|codigo|texto (o servidor fecha a conexao em seguida)
        const size_t bar = rest.find('|');
        const std::string code = rest.substr(0, bar);
        SetStatus("servidor recusou: " + (bar == std::string::npos ? rest : rest.substr(bar + 1)));
        g_end_session = true;
        if (code == "senha" || code == "trancada" || code == "expulso" || code == "versao" || code == "cheia") {
            std::lock_guard lock(g_mutex);
            g_refused = code;
        }
        return;
    }
    const size_t b = rest.find('|');
    const int id = std::atoi(rest.c_str());
    const std::string tail = b == std::string::npos ? std::string() : rest.substr(b + 1);
    std::lock_guard lock(g_mutex);
    if (kind == "p") {
        Peer& p = g_peers[id];
        p.id = id;
        p.name = tail;
        std::printf("[MP] %s entrou na sala\n", tail.c_str());
        g_announce_now = true; // o novo precisa dos nossos enderecos
    } else if (kind == "l") {
        auto it = g_peers.find(id);
        if (it != g_peers.end()) {
            std::printf("[MP] %s saiu da sala\n", it->second.name.c_str());
            g_peers.erase(it);
        }
        g_links.erase(id);
    } else if (kind == "s" && tail.rfind("!~p|", 0) == 0) { // Fase 82: enderecos para a conexao direta
        OnAnnouncement(id, tail.substr(4));
    } else if (kind == "s" && !tail.empty() && tail[0] == '!') { // s|id|!payload: evento de jogo (quebrou um objeto...)
        if (g_events_in.size() < 512) g_events_in.emplace_back(id, tail.substr(1));
    } else if (kind == "s") {
        auto link = g_links.find(id);
        if (link != g_links.end() && Now() - link->second.last_state_rx < 1.5) return; // ja chega direto (mais novo)
        Peer& p = g_peers[id];
        p.id = id;
        p.state = tail;
        p.state_time = Now();
        ++p.seq;
    }
}

bool SendText(HINTERNET ws, const std::string& text) {
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()),
                                static_cast<DWORD>(text.size())) == NO_ERROR;
}

void Receiver(HINTERNET ws, std::atomic<bool>* alive) {
    std::string message;
    char buffer[4096];
    while (!g_stop) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        if (WinHttpWebSocketReceive(ws, buffer, sizeof(buffer), &read, &type) != NO_ERROR) break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        message.append(buffer, read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            if (message.size() > (1u << 20)) message.clear();
            continue;
        }
        Handle(message);
        message.clear();
    }
    *alive = false;
}

// Uma conexao: abre, entra na sala, manda o estado ~20 Hz ate cair.
void Session(HINTERNET session) {
    HINTERNET connect = WinHttpConnect(session, g_url.host.c_str(), g_url.port, 0);
    if (!connect) {
        SetStatus("sem conexao com o servidor");
        return;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", g_url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, g_url.secure ? WINHTTP_FLAG_SECURE : 0);
    HINTERNET ws = nullptr;
    if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 101) {
            ws = WinHttpWebSocketCompleteUpgrade(request, 0);
        } else {
            SetStatus("o servidor respondeu " + std::to_string(status) + " (esperava 101)");
        }
    } else {
        SetStatus("sem conexao com o servidor (erro " + std::to_string(GetLastError()) + ")");
    }
    if (request) WinHttpCloseHandle(request);
    if (!ws) {
        WinHttpCloseHandle(connect);
        return;
    }
    SetStatus("entrando na sala " + g_room + "...");
    g_end_session = false;
    g_in_session = true;
    SendText(ws, "j|" + g_room + "|" + g_name + "|" + std::to_string(kProtocol) + "|" + g_password);
    std::atomic<bool> alive{true};
    std::thread receiver(Receiver, ws, &alive);
    auto last_keepalive = std::chrono::steady_clock::now();
    auto last_ping = last_keepalive - std::chrono::seconds(2);
    auto last_state = last_keepalive;
    while (!g_stop && alive && !g_end_session) {
        P2PTick(Now());
        std::string state;
        std::vector<std::string> commands;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(g_mutex);
            commands.swap(g_commands);
            // ate 4 na sala: 30 por segundo; ate 8: 20; mais: 12 (com 16, cada um recebe 15 x 12 = 180 por segundo)
            const size_t others = g_peers.size();
            const auto interval = std::chrono::milliseconds(others <= 3 ? 30 : others <= 7 ? 48 : 80);
            if (g_local_dirty && now - last_state >= interval) {
                state = g_local_state;
                g_local_dirty = false;
                last_state = now;
            }
        }
        if (now - last_ping > std::chrono::seconds(1)) { // mede a latencia (o servidor devolve o "t")
            char ping[48];
            std::snprintf(ping, sizeof(ping), "t|%.4f", Now());
            if (!SendText(ws, ping)) break;
            last_ping = now;
        }
        bool sent = true;
        for (const std::string& c : commands) sent = sent && SendText(ws, c);
        if (!sent) break;
        // Fase 82: direto para quem tem caminho P2P; pelo servidor so se ainda falta alguem
        if (!state.empty() && SendStateDirect(state)) {
            if (!SendText(ws, "s|" + state)) break;
            last_keepalive = now;
        } else if (now - last_keepalive > std::chrono::seconds(5)) {
            if (!SendText(ws, "k")) break; // fora do jogo (menus) ou tudo direto: so mantem a conexao viva
            last_keepalive = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 estados por segundo
    }
    SendText(ws, "q"); // saindo: o servidor tira da sala na hora (o fechamento da conexao nao chega pelo proxy)
    g_in_session = false;
    WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle(ws); // destrava o Receive
    receiver.join();
    WinHttpCloseHandle(connect);
    g_self = 0;
    g_owner = 0;
    {
        std::lock_guard lock(g_mutex);
        g_peers.clear();
        g_links.clear();
        g_commands.clear();
    }
    if (!g_stop) SetStatus("conexao caiu; tentando de novo");
}

void Run() {
    HINTERNET session = WinHttpOpen(L"LegoDimensionsPC-MP/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"LegoDimensionsPC-MP/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        SetStatus("WinHTTP indisponivel");
        return;
    }
    P2POpen();
    // o servidor gratuito pode estar "dormindo": a primeira conexao demora ate ~1 minuto
    WinHttpSetTimeouts(session, 15000, 60000, 15000, 0);
    int wait_ms = 2000;
    while (!g_stop) {
        SetStatus("conectando em " + std::string(g_url.host.begin(), g_url.host.end()) + "...");
        const auto started = std::chrono::steady_clock::now();
        Session(session);
        {
            std::lock_guard lock(g_mutex);
            if (!g_refused.empty()) break; // senha errada, expulso...: tentar de novo nao adianta
        }
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(30)) wait_ms = 2000;
        for (int waited = 0; waited < wait_ms && !g_stop; waited += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        wait_ms = std::min(wait_ms * 2, 30000);
    }
    P2PClose();
    WinHttpCloseHandle(session);
    g_in_session = false;
    g_started = false;
}

} // namespace

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count(); }

bool Start(const std::string& url, const std::string& room, const std::string& name, const std::string& password) {
    if (g_started) {
        Stop();
    }
    if (!ParseUrl(url, &g_url) || room.empty()) {
        return false;
    }
    g_room = Clean(room, 32);
    g_name = Clean(name.empty() ? std::string("Jogador") : name, 24);
    g_password = Clean(password, 64);
    g_stop = false;
    g_started = true;
    g_self = 0;
    g_owner = 0;
    g_rtt_ms = 0;
    {
        std::lock_guard lock(g_mutex);
        g_refused.clear();
        g_peers.clear();
        g_links.clear();
        g_commands.clear();
    }
    std::thread(Run).detach();
    static bool atexit_set = false;
    if (!atexit_set) {
        std::atexit([] { Stop(); }); // fechar o jogo: avisa o servidor (senao os outros so veem a saida 12 s depois)
        atexit_set = true;
    }
    return true;
}

void Stop() {
    g_stop = true;
    for (int i = 0; i < 25 && (g_in_session || g_started); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); // o "q"
}

void SetLocalState(const std::string& state) {
    std::lock_guard lock(g_mutex);
    g_local_state = state;
    g_local_dirty = true;
}

std::vector<Peer> Peers() {
    std::lock_guard lock(g_mutex);
    std::vector<Peer> out;
    for (const auto& [id, p] : g_peers) {
        out.push_back(p);
        auto it = g_links.find(id);
        if (g_p2p_on && it != g_links.end() && it->second.direct) {
            out.back().direct = true;
            out.back().direct_rtt_ms = it->second.rtt_ms;
        }
    }
    return out;
}

int SelfId() { return g_self; }

int RttMs() { return g_rtt_ms; }

std::string StatusText() {
    std::lock_guard lock(g_mutex);
    int direct = 0, others = 0;
    for (const auto& [id, p] : g_peers) {
        if (id == g_self) continue;
        ++others;
        auto it = g_links.find(id);
        direct += g_p2p_on && it != g_links.end() && it->second.direct;
    }
    if (!others) return g_status;
    return g_status + (g_p2p_on ? " | P2P " + std::to_string(direct) + "/" + std::to_string(others) : " | P2P desligado");
}

std::string RoomCode() { return g_room; }
std::string SelfName() { return g_name; }
int OwnerId() { return g_owner; }
bool RoomLocked() { return g_locked; }

void SetRoomLocked(bool locked) {
    g_locked = locked;
    std::lock_guard lock(g_mutex);
    g_commands.push_back(locked ? "L|1" : "L|0");
}

void SendEvent(const std::string& payload) {
    std::lock_guard lock(g_mutex);
    if (g_commands.size() < 256 && payload.size() < 560) g_commands.push_back("s|!" + payload);
}

std::vector<std::pair<int, std::string>> TakeEvents() {
    std::lock_guard lock(g_mutex);
    std::vector<std::pair<int, std::string>> out;
    out.swap(g_events_in);
    return out;
}

void Kick(int id) {
    std::lock_guard lock(g_mutex);
    g_commands.push_back("x|" + std::to_string(id));
}

std::string RefusedCode() {
    std::lock_guard lock(g_mutex);
    return g_refused;
}

void SetP2P(bool on) {
    g_p2p_on = on;
    if (!on) {
        std::lock_guard lock(g_mutex);
        for (auto& [id, l] : g_links) {
            l.direct = false;
            l.last_state_rx = -100;
        }
    } else {
        g_announce_now = true;
    }
}

bool P2PEnabled() { return g_p2p_on; }

double LatencySeconds(int peer, int their_server_rtt_ms) {
    {
        std::lock_guard lock(g_mutex);
        auto it = g_links.find(peer);
        if (g_p2p_on && it != g_links.end() && it->second.direct && it->second.rtt_ms > 0 &&
            Now() - it->second.last_state_rx < 1.5) {
            return it->second.rtt_ms * 0.5e-3; // direto: metade da nossa ida e volta com ele
        }
    }
    return (g_rtt_ms.load() + their_server_rtt_ms) * 0.5e-3; // pelo servidor: nossa metade + a dele
}

} // namespace runtime::mp_net
