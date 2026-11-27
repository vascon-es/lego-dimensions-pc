// Listener do app LegoToypad (HarrysofXD), Fase 27.
//
// O app e um cliente TCP: abre uma conexao com 127.0.0.1:<porta> (9191 por padrao, secao
// [Listener] do LegoToypad.ini) para cada acao. No shadPS4 quem atende e o ShadToypadBridge, que
// traduz para os comandos de IPC do emulador; aqui o proprio port atende. Protocolo (conferido no
// bridge descompilado):
//
//   cabecalho de 5 bytes: cmd, pad (1 = centro, 2 = esquerda, 3 = direita), indice (0..6),
//                         pad de origem, indice de origem
//   cmd 1 LOAD    + 180 bytes da tag (NTAG213, 45 paginas) + u16 tamanho + caminho UTF-8 do .bin
//                   (se o arquivo existir com >= 180 bytes, e ele que vale e recebe as escritas)
//   cmd 2 REMOVE
//   cmd 3 MOVE    da posicao de origem para (pad, indice)
//   cmd 4 GET_LED -> 40 bytes (formato v2, Fase 60): 'L', contador, 2, 3, e 12 bytes por zona
//   Varios comandos podem vir na mesma conexao (como no listener do Cemu); cada um e atendido na ordem.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "hle/overrides/hle_toypad.h"
#include "common/toypad_catalog_protocol.h"

#pragma comment(lib, "ws2_32.lib")

namespace hle {

void InjectToypadPacket(const toypad::Packet& packet); // hle_usbd.cpp
void TakeToypadExternalControl();                      // hle_usbd.cpp

namespace {

std::atomic<bool> g_started{false};
std::atomic<u64> g_requests{0};

bool RecvAll(SOCKET s, void* buffer, int length) {
    char* p = static_cast<char*>(buffer);
    for (int done = 0; done < length;) {
        const int n = recv(s, p + done, length - done, 0);
        if (n <= 0) {
            return false;
        }
        done += n;
    }
    return true;
}

bool SendSnapshot(SOCKET s) {
    static_assert(toypad_catalog::kSlots == toypad::kSlotCount);
    toypad_catalog::Snapshot snapshot{'T', 1};
    const auto slots = toypad::Slots();
    for (int i = 0; i < toypad::kSlotCount; ++i) {
        auto* out = snapshot.data() + 2 + i * 5;
        out[0] = static_cast<u8>(slots[i].pad);
        toypad_catalog::WriteId(out + 1, slots[i].id);
    }
    for (int offset = 0; offset < static_cast<int>(snapshot.size());) {
        const int n = send(s, reinterpret_cast<const char*>(snapshot.data()) + offset,
                           static_cast<int>(snapshot.size()) - offset, 0);
        if (n <= 0) return false;
        offset += n;
    }
    return true;
}

// Fase 60: entre tirar uma figura e pousar outra (ou a mesma) o jogo precisa de um intervalo, como num
// portal de verdade; sem ele, o "colocada" que chega junto com o "removida" as vezes nao conta e a figura
// tinha de ser colocada de novo. O listener do Cemu "Remote Toypad" usa os mesmos 500 ms.
constexpr auto kPickupDelay = std::chrono::milliseconds(500);

void Announce(const std::vector<toypad::Packet>& packets) {
    if (!toypad::SessionReady()) {
        return; // o jogo ainda nao abriu a sessao: a vigia do portal anuncia tudo quando abrir
    }
    for (const toypad::Packet& packet : packets) {
        InjectToypadPacket(packet);
    }
}

// Um comando; false = a conexao acabou (ou veio algo depois do qual nao da para achar o proximo cabecalho).
bool HandleCommand(SOCKET s) {
    u8 header[5];
    if (!RecvAll(s, header, 5)) {
        return false;
    }
    const u8 cmd = header[0], pad = header[1], index = header[2];
    ++g_requests;
    if (cmd == toypad_catalog::kSnapshot) return SendSnapshot(s);
    if (cmd == 4) { // GET_LED
        u8 snapshot[toypad::kLedSnapshotBytes];
        toypad::LedSnapshot(snapshot);
        send(s, reinterpret_cast<const char*>(snapshot), sizeof(snapshot), 0);
        return true;
    }
    if (pad < 1 || pad > 3 || index >= toypad::kSlotCount) {
        std::printf("[TOYPAD-APP] request refused: cmd %u pad %u index %u\n", cmd, pad, index);
        return false;
    }
    TakeToypadExternalControl();
    switch (cmd) {
    case toypad_catalog::kSelect: {
        u8 bytes[4];
        if (!RecvAll(s, bytes, sizeof(bytes))) return false;
        const u32 id = toypad_catalog::ReadId(bytes);
        if (id > 65535) return false;
        const auto before = toypad::Slots()[index];
        if (before.id == id && (id == 0 || before.pad == pad)) return true;
        if (const auto removed = toypad::RemoveIndex(index)) {
            Announce({*removed});
            if (id) std::this_thread::sleep_for(kPickupDelay);
        }
        if (id) {
            const toypad::Uid uid{4, 0x4c, 0x50, index, static_cast<u8>(id), static_cast<u8>(id >> 8), 0x80};
            if (const auto added = toypad::LoadTag(pad, index, toypad::BuildTag(uid, id), {})) Announce({*added});
        }
        break;
    }
    case 1: { // LOAD
        toypad::TagData tag{};
        u8 length_bytes[2];
        if (!RecvAll(s, tag.data(), static_cast<int>(tag.size())) || !RecvAll(s, length_bytes, 2)) {
            return false;
        }
        const u32 length = length_bytes[0] | (length_bytes[1] << 8);
        std::string path(length, '\0');
        if (length && !RecvAll(s, path.data(), static_cast<int>(length))) {
            return false;
        }
        bool from_file = false;
        if (!path.empty()) { // o arquivo da figura manda, se existir: e nele que o progresso fica gravado
            std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
            toypad::TagData file{};
            if (in && in.read(reinterpret_cast<char*>(file.data()), file.size())) {
                tag = file;
                from_file = true;
            }
        }
        std::vector<toypad::Packet> events;
        if (const auto removed = toypad::RemoveIndex(index)) { // a posicao estava ocupada: sai antes, com intervalo
            Announce({*removed});
            std::this_thread::sleep_for(kPickupDelay);
        }
        u32 id = 0;
        if (const auto added = toypad::LoadTag(pad, index, tag, from_file ? path : std::string(), &id)) {
            events.push_back(*added);
        }
        std::printf("[TOYPAD-APP] LOAD pad %u index %u: id %u, uid %02x %02x %02x %02x %02x %02x %02x%s%s\n", pad, index, id, tag[0], tag[1],
                    tag[2], tag[4], tag[5], tag[6], tag[7], from_file ? ", file " : "", from_file ? path.c_str() : "");
        Announce(events);
        break;
    }
    case 2: { // REMOVE
        std::vector<toypad::Packet> events;
        if (const auto removed = toypad::RemoveIndex(index)) {
            events.push_back(*removed);
        }
        std::printf("[TOYPAD-APP] REMOVE pad %u index %u%s\n", pad, index, events.empty() ? " (already empty)" : "");
        Announce(events);
        break;
    }
    case 3: { // MOVE
        const u8 old_pad = header[3], old_index = header[4];
        if (old_pad < 1 || old_pad > 3 || old_index >= toypad::kSlotCount) {
            std::printf("[TOYPAD-APP] MOVE refused: source pad %u index %u\n", old_pad, old_index);
            return true;
        }
        std::vector<toypad::Packet> events = toypad::MoveIndex(pad, index, old_index);
        std::printf("[TOYPAD-APP] MOVE %u/%u -> %u/%u%s\n", old_pad, old_index, pad, index, events.empty() ? " (empty source)" : "");
        if (events.size() > 1) { // "removida" (e o que ocupava o destino) agora, "colocada" depois do intervalo
            const toypad::Packet placed = events.back();
            events.pop_back();
            Announce(events);
            std::this_thread::sleep_for(kPickupDelay);
            events = {placed};
        }
        Announce(events);
        break;
    }
    default:
        std::printf("[TOYPAD-APP] unknown command 0x%02x\n", cmd);
        return false; // tamanho desconhecido: o proximo cabecalho se perde
    }
    return true;
}

// Fase 60: varios comandos na mesma conexao, atendidos em ordem (como no listener do Cemu); o app
// LegoToypad abre uma conexao por acao e por consulta de LEDs, o que tambem funciona.
void HandleClient(SOCKET s) {
    while (HandleCommand(s)) {
    }
}

void ListenerThread(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("[TOYPAD-APP] WSAStartup failed\n");
        return;
    }
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // so a propria maquina
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        std::printf("[TOYPAD-APP] could not listen on 127.0.0.1:%d (error %d). Port in use? (ShadToypadBridge uses "
                    "same)\n",
                    port, WSAGetLastError());
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
        }
        return;
    }
    std::printf("[TOYPAD-APP] LegoToypad listener active on 127.0.0.1:%d\n", port);
    for (;;) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            break;
        }
        DWORD timeout_ms = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        HandleClient(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
}

} // namespace

void StartToypadListener(int port) {
    if (port <= 0 || g_started.exchange(true)) {
        return;
    }
    std::thread(ListenerThread, port).detach();
}

u64 ToypadListenerRequests() {
    return g_requests.load();
}

} // namespace hle
