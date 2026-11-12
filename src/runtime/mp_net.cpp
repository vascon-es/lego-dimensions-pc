#include "runtime/mp_net.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
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
std::vector<std::string> g_commands; // L/x do dono, mandados pela thread (g_mutex)
std::string g_password;
std::atomic<bool> g_in_session{false}; // conectado (Stop espera o "q" sair)
std::atomic<bool> g_end_session{false}; // o servidor recusou: encerra (o proxy do Render nao repassa o fechamento)
std::atomic<bool> g_stop{false};
bool g_started = false; // a thread fica solta (detach): no fim do processo nao ha join
Url g_url;
std::string g_room, g_name;
const auto g_epoch = std::chrono::steady_clock::now();

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

void Handle(const std::string& msg) {
    const size_t a = msg.find('|');
    const std::string kind = msg.substr(0, a);
    const std::string rest = a == std::string::npos ? std::string() : msg.substr(a + 1);
    if (kind == "w") {
        g_self = std::atoi(rest.c_str());
        SetStatus("conectado na sala " + g_room + " (id " + std::to_string(g_self.load()) + ")");
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
    } else if (kind == "l") {
        auto it = g_peers.find(id);
        if (it != g_peers.end()) {
            std::printf("[MP] %s saiu da sala\n", it->second.name.c_str());
            g_peers.erase(it);
        }
    } else if (kind == "s") {
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
    while (!g_stop && alive && !g_end_session) {
        std::string state;
        std::vector<std::string> commands;
        {
            std::lock_guard lock(g_mutex);
            commands.swap(g_commands);
            if (g_local_dirty) {
                state = g_local_state;
                g_local_dirty = false;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ping > std::chrono::seconds(1)) { // mede a latencia (o servidor devolve o "t")
            char ping[48];
            std::snprintf(ping, sizeof(ping), "t|%.4f", Now());
            if (!SendText(ws, ping)) break;
            last_ping = now;
        }
        bool sent = true;
        for (const std::string& c : commands) sent = sent && SendText(ws, c);
        if (!sent) break;
        if (!state.empty()) {
            if (!SendText(ws, "s|" + state)) break;
            last_keepalive = now;
        } else if (now - last_keepalive > std::chrono::seconds(5)) {
            if (!SendText(ws, "k")) break; // fora do jogo (menus): so mantem a conexao viva
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
    WinHttpCloseHandle(session);
}

} // namespace

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count(); }

bool Start(const std::string& url, const std::string& room, const std::string& name, const std::string& password) {
    if (g_started || !ParseUrl(url, &g_url) || room.empty()) {
        return false;
    }
    g_room = Clean(room, 32);
    g_name = Clean(name.empty() ? std::string("Jogador") : name, 24);
    g_password = Clean(password, 64);
    g_stop = false;
    g_started = true;
    std::thread(Run).detach();
    std::atexit([] { Stop(); }); // fechar o jogo: avisa o servidor (senao os outros so veem a saida 12 s depois)
    return true;
}

void Stop() {
    g_stop = true;
    for (int i = 0; i < 25 && g_in_session; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); // o "q"
}

void SetLocalState(const std::string& state) {
    std::lock_guard lock(g_mutex);
    g_local_state = state;
    g_local_dirty = true;
}

std::vector<Peer> Peers() {
    std::lock_guard lock(g_mutex);
    std::vector<Peer> out;
    for (const auto& [id, p] : g_peers) out.push_back(p);
    return out;
}

int SelfId() { return g_self; }

int RttMs() { return g_rtt_ms; }

std::string StatusText() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

std::string RoomCode() { return g_room; }
std::string SelfName() { return g_name; }
int OwnerId() { return g_owner; }
bool RoomLocked() { return g_locked; }

void SetRoomLocked(bool locked) {
    std::lock_guard lock(g_mutex);
    g_commands.push_back(locked ? "L|1" : "L|0");
}

void Kick(int id) {
    std::lock_guard lock(g_mutex);
    g_commands.push_back("x|" + std::to_string(id));
}

std::string RefusedCode() {
    std::lock_guard lock(g_mutex);
    return g_refused;
}

} // namespace runtime::mp_net
