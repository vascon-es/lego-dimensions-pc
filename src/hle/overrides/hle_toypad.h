#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

// Protocolo do LEGO Dimensions Toy Pad (USB 0e6f:0241), pacotes de 32 bytes:
//   comando  (OUT 0x01): 55 <len> <cmd> <seq> <payload...> <checksum>
//   resposta (IN  0x81): 55 <len> <seq> <payload...> <checksum>
//   evento   (IN  0x81): 56 0b <pad> 00 <indice> <0 = colocada | 1 = removida> <uid x7> <checksum>
// com <len> = bytes entre ele e o checksum, e checksum = soma dos bytes anteriores mod 256.
//
// Referencia do protocolo e do formato das tags: a emulacao de Toy Pad do fork local do shadPS4
// (src/core/libraries/usbd/emulated/dimensions.cpp).
//
// Figuras: uma por zona do portal (1 = centro, 2 = esquerda, 3 = direita). A tag (NTAG213,
// 45 paginas de 4 bytes) e SINTETIZADA a partir de um UID e de um id de personagem: nao e um
// dump de figura real, e nada alem de UID, id e senha esta preenchido.
namespace hle::toypad {

using Packet = std::array<u8, 32>;
using Uid = std::array<u8, 7>;

constexpr int kPadCount = 3;
constexpr int kSlotCount = 7; // posicoes de figura do portal (indice 0..6 no protocolo)
constexpr int kTagPages = 0x2D;
using TagData = std::array<u8, kTagPages * 4>;

// Processa um comando e devolve a resposta a entregar no endpoint IN (nullopt se o pacote
// nao for um comando valido).
std::optional<Packet> HandleCommand(const u8* command, u32 length);

// true depois que o jogo completou WAKE + desafio: so entao eventos de figura fazem sentido.
bool SessionReady();
// Fase 60: numero da sessao aberta (0 = fechada); muda a cada sessao nova, mesmo se WAKE e desafio vierem
// entre duas passadas da vigia do portal.
u32 SessionNumber();

// Coloca / remove a figura de uma zona (1..3) e devolve o pacote de evento a enviar ao jogo.
// PlaceFigure devolve nullopt se a zona for invalida ou ja tiver a mesma figura.
std::optional<Packet> PlaceFigure(int pad, const Uid& uid, u32 character_id);
std::optional<Packet> RemoveFigure(int pad);

// Persistencia do estado do portal (portal.json)
void SetPortalFilePath(const std::filesystem::path& path);
std::filesystem::path GetPortalFilePath();
void SavePortalState(const std::filesystem::path& path = {});
void LoadPortalState(const std::filesystem::path& path = {});

// Fase 27 (app LegoToypad): figuras por INDICE (0..6), cada uma numa zona, com a tag inteira vinda
// de fora (dump de 180 bytes). `path` nao vazio = arquivo que recebe as escritas do jogo.
std::optional<Packet> LoadTag(int pad, int index, const TagData& tag, const std::string& path, u32* character_id = nullptr);
std::optional<Packet> RemoveIndex(int index);
std::vector<Packet> MoveIndex(int new_pad, int new_index, int old_index); // remocao + colocacao
std::vector<Packet> PresentEvents();                                      // "colocada" de tudo o que esta no portal
// Id do personagem / veiculo guardado numa tag (0 se nao der para ler).
u32 TagCharacterId(const TagData& tag);
// Fase 60: estado dos LEDs no formato v2 do GET_LED (o do listener do Cemu "Remote Toypad", que o app
// LegoToypad 1.6+ le): 'L', contador, versao 2, 3 zonas, e por zona 12 bytes (zona, modo, r, g, b, cor de
// origem do fade r/g/b, aceso, apagado, vezes, velocidade).
constexpr u32 kLedSnapshotBytes = 4 + 3 * 12;
void LedSnapshot(u8 out[kLedSnapshotBytes]);

// Monta a tag de um personagem (id < 1000) ou veiculo/gadget (id >= 1000).
TagData BuildTag(const Uid& uid, u32 character_id);
// Chave TEA derivada do UID, que cifra o id nas paginas 0x24-0x25.
std::array<u32, 4> FigureKey(const Uid& uid);

// TEA. Sem `key`, usa a chave fixa dos comandos do portal (0xB1, 0xB3, 0xD4).
std::array<u8, 8> Encrypt(const u8* block, const u32* key = nullptr);
std::array<u8, 8> Decrypt(const u8* block, const u32* key = nullptr);

u8 Checksum(const u8* data, u32 count);

struct Stats {
    u64 commands = 0;
    u64 wake = 0, seed = 0, challenge = 0, led = 0, read = 0, write = 0, model = 0, unknown = 0;
    u64 figures_placed = 0, figures_removed = 0;
};
Stats GetStats();

} // namespace hle::toypad
