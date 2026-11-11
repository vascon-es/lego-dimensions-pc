#include "hle/overrides/hle_toypad.h"

#include <bit>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hle/hle_log.h"

namespace hle::toypad {

namespace {

// Chave TEA dos comandos do portal, como quatro palavras little-endian.
constexpr u32 kCommandKey[4] = {0xB0F6FE55, 0x410BBF62, 0xB47CB3C9, 0x7B293E97};
constexpr u32 kDelta = 0x9E3779B9;

// Constantes que entram, junto com o UID, na chave de cada figura e na senha da tag.
constexpr u8 kCharConstant[17] = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8,
                                  0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};
constexpr char kPasswordText[] = "(c) Copyright LEGO 2014"; // 23 bytes, seguidos de AA AA

struct Figure {
    bool present = false;
    int pad = 0; // zona (1..3)
    Uid uid{};
    u32 id = 0;
    TagData data{};
    std::string path; // arquivo que recebe as escritas (figuras do app LegoToypad)
};

std::mutex g_mutex;
Stats g_stats;
u32 g_rng[4] = {}; // gerador do desafio, semeado pelo jogo no comando 0xB1
bool g_session_ready = false;
u32 g_session_number = 0; // Fase 60: conta as sessoes abertas (cada desafio depois de um WAKE)
// Indice da figura no protocolo = posicao neste vetor (0..6). As figuras do portal.json usam o
// indice igual a zona (1..3), como desde a Fase 8; o app LegoToypad escolhe o indice.
Figure g_figures[kSlotCount];
std::filesystem::path g_portal_file_path;

void SavePortalStateLocked(const std::filesystem::path& path = {}) {
    const std::filesystem::path target = !path.empty() ? path : g_portal_file_path;
    if (target.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::trunc);
    if (!out) {
        return;
    }

    const Figure* pad_figs[kPadCount + 1] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < kSlotCount; ++i) {
        if (g_figures[i].present && g_figures[i].pad >= 1 && g_figures[i].pad <= kPadCount) {
            if (!pad_figs[g_figures[i].pad]) {
                pad_figs[g_figures[i].pad] = &g_figures[i];
            }
        }
    }

    out << "{\n";
    for (int p = 1; p <= kPadCount; ++p) {
        out << "  \"pad" << p << "\": ";
        if (pad_figs[p]) {
            const auto& f = *pad_figs[p];
            out << "{\"uid\": ["
                << static_cast<int>(f.uid[0]) << ","
                << static_cast<int>(f.uid[1]) << ","
                << static_cast<int>(f.uid[2]) << ","
                << static_cast<int>(f.uid[3]) << ","
                << static_cast<int>(f.uid[4]) << ","
                << static_cast<int>(f.uid[5]) << ","
                << static_cast<int>(f.uid[6]) << "], \"id\": " << f.id << "}";
        } else {
            out << "null";
        }
        out << ",\n";
    }

    out << "  \"slots\": [\n";
    bool first_slot = true;
    for (int i = 0; i < kSlotCount; ++i) {
        if (g_figures[i].present) {
            if (!first_slot) out << ",\n";
            first_slot = false;
            const auto& f = g_figures[i];
            out << "    {\"index\": " << i << ", \"pad\": " << f.pad << ", \"id\": " << f.id
                << ", \"uid\": ["
                << static_cast<int>(f.uid[0]) << ","
                << static_cast<int>(f.uid[1]) << ","
                << static_cast<int>(f.uid[2]) << ","
                << static_cast<int>(f.uid[3]) << ","
                << static_cast<int>(f.uid[4]) << ","
                << static_cast<int>(f.uid[5]) << ","
                << static_cast<int>(f.uid[6]) << "]";
            if (!f.path.empty()) {
                std::string esc;
                for (char c : f.path) {
                    if (c == '\\') esc += "\\\\";
                    else esc += c;
                }
                out << ", \"path\": \"" << esc << "\"";
            }
            out << "}";
        }
    }
    out << "\n  ]\n";
    out << "}\n";
}
// Fase 60: estado dos LEDs de cada zona (0 = centro, 1 = esquerda, 2 = direita), como no listener do Cemu
// "Remote Toypad" que o app LegoToypad le: o app anima piscadas e fades sozinho a partir deste estado.
// Tempos em "ticks" do portal (~40 ms cada), como chegam do jogo.
struct LedState {
    u8 pad = 0, mode = 0; // mode: 0 apagado, 1 cor fixa, 2 piscando, 3 fade
    u8 r = 0, g = 0, b = 0;
    u8 from_r = 0, from_g = 0, from_b = 0; // cor de onde o fade parte (a que estava acesa)
    u8 on = 0, off = 0, count = 0, speed = 0;
};
LedState g_led[kPadCount];
u8 g_led_counter = 0; // muda a cada estado novo (o app so redesenha quando muda)

u8 LedRandomByte() {
    static u32 state = 0x2545F491u; // independente do gerador do desafio do portal
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<u8>(state >> 24);
}

// pad: 1 centro, 2 esquerda, 3 direita, 0 todas.
void SetLed(u8 pad, u8 mode, u8 r, u8 g, u8 b, u8 on, u8 off, u8 count, u8 speed) {
    auto apply = [&](u8 target) {
        if (target < 1 || target > kPadCount) {
            return;
        }
        LedState& state = g_led[target - 1];
        if (state.pad == target && state.mode == mode && state.r == r && state.g == g && state.b == b && state.on == on && state.off == off &&
            state.count == count && state.speed == speed) {
            return; // nada mudou
        }
        if (mode == 3) { // fade: parte da cor que estava na zona
            state.from_r = state.r;
            state.from_g = state.g;
            state.from_b = state.b;
        }
        state.pad = target;
        state.mode = mode;
        state.r = r;
        state.g = g;
        state.b = b;
        state.on = on;
        state.off = off;
        state.count = count;
        state.speed = speed;
        ++g_led_counter;
    };
    if (pad == 0) {
        for (u8 target = 1; target <= kPadCount; ++target) {
            apply(target);
        }
    } else {
        apply(pad);
    }
}

// Comandos de LED do jogo (0xC0..0xC8); `p` = payload (bytes depois de 0x55, tamanho, comando, sequencia).
void HandleLedCommand(u8 cmd, const u8* p, u32 size) {
    auto has = [&](u32 bytes) { return size >= bytes; };
    switch (cmd) {
    case 0xC0: // cor: zona, r, g, b
        if (has(4)) {
            SetLed(p[0], 1, p[1], p[2], p[3], 0, 0, 0, 0);
        }
        break;
    case 0xC2: // fade: zona, tempo do passo, passos, r, g, b
        if (has(6)) {
            SetLed(p[0], 3, p[3], p[4], p[5], 0, 0, p[2], p[1]);
        }
        break;
    case 0xC3: // piscar: zona, aceso, apagado, vezes (0xFF = sempre), r, g, b
        if (has(7)) {
            SetLed(p[0], 2, p[4], p[5], p[6], p[1], p[2], p[3] == 0xFF ? 0 : p[3], 0);
        }
        break;
    case 0xC4: // fade para uma cor aleatoria: zona, tempo do passo, passos (cada zona sorteia a sua)
        if (has(3)) {
            for (u8 target = 1; target <= kPadCount; ++target) {
                if (p[0] == 0 || p[0] == target) {
                    SetLed(target, 3, LedRandomByte(), LedRandomByte(), LedRandomByte(), 0, 0, p[2], p[1]);
                }
            }
        }
        break;
    case 0xC6: // fade em todas: por zona [ligada][tempo][passos][r][g][b]
        for (u8 zone = 0; zone < kPadCount && has((zone + 1) * 6u); ++zone) {
            const u8* z = p + zone * 6;
            if (z[0]) {
                SetLed(zone + 1, 3, z[3], z[4], z[5], 0, 0, z[2], z[1]);
            } else {
                SetLed(zone + 1, 0, 0, 0, 0, 0, 0, 0, 0);
            }
        }
        break;
    case 0xC7: // piscar em todas: por zona [ligada][aceso][apagado][vezes][r][g][b]
        for (u8 zone = 0; zone < kPadCount && has((zone + 1) * 7u); ++zone) {
            const u8* z = p + zone * 7;
            if (z[0]) {
                SetLed(zone + 1, 2, z[4], z[5], z[6], z[1], z[2], z[3] == 0xFF ? 0 : z[3], 0);
            } else {
                SetLed(zone + 1, 0, 0, 0, 0, 0, 0, 0, 0);
            }
        }
        break;
    case 0xC8: // cor em todas: por zona [ligada][r][g][b]
        for (u8 zone = 0; zone < kPadCount && has((zone + 1) * 4u); ++zone) {
            const u8* z = p + zone * 4;
            if (z[0]) {
                SetLed(zone + 1, 1, z[1], z[2], z[3], 0, 0, 0, 0);
            } else {
                SetLed(zone + 1, 0, 0, 0, 0, 0, 0, 0, 0);
            }
        }
        break;
    default: // 0xC1: consulta, nao muda nada
        break;
    }
}

u32 Load32(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24);
}

void Store32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

u32 NextRandom() {
    const u32 e = g_rng[0] - std::rotl(g_rng[1], 21);
    g_rng[0] = g_rng[1] ^ std::rotl(g_rng[2], 19);
    g_rng[1] = g_rng[2] + std::rotl(g_rng[3], 6);
    g_rng[2] = g_rng[3] + e;
    g_rng[3] = e + g_rng[0];
    return g_rng[3];
}

void SeedRandom(u32 seed) {
    g_rng[0] = 0xF1EA5EED;
    g_rng[1] = g_rng[2] = g_rng[3] = seed;
    for (int i = 0; i < 42; ++i) {
        NextRandom();
    }
}

// Mistura `words` palavras little-endian de `bytes` (hash usado na chave e na senha).
u32 Scramble(const u8* bytes, int words) {
    u32 value = 0;
    for (int i = 0; i < words; ++i) {
        value = Load32(bytes + i * 4) + std::rotr(value, 25) + std::rotr(value, 10) - value;
    }
    return value;
}

// 55 <len> <seq> <payload> <checksum>, resto zerado
Packet Reply(u8 sequence, const u8* payload, u32 payload_size) {
    Packet reply{};
    reply[0] = 0x55;
    reply[1] = static_cast<u8>(payload_size + 1);
    reply[2] = sequence;
    if (payload_size) {
        std::memcpy(&reply[3], payload, payload_size);
    }
    reply[3 + payload_size] = Checksum(reply.data(), 3 + payload_size);
    return reply;
}

Packet FigureEvent(int pad, int index, bool removed, const Uid& uid) {
    Packet event{};
    event[0] = 0x56;
    event[1] = 0x0B;
    event[2] = static_cast<u8>(pad);
    event[3] = 0x00;
    event[4] = static_cast<u8>(index); // indice da figura
    event[5] = removed ? 1 : 0;
    std::memcpy(&event[6], uid.data(), uid.size());
    event[13] = Checksum(event.data(), 13);
    return event;
}

std::string Hex(const u8* data, u32 count) {
    std::string out;
    char byte[4];
    for (u32 i = 0; i < count; ++i) {
        std::snprintf(byte, sizeof(byte), "%02x ", data[i]);
        out += byte;
    }
    return out;
}

// Figura pelo indice do protocolo (0..6), ou nullptr.
Figure* FigureAt(u8 index) {
    if (index >= kSlotCount || !g_figures[index].present) {
        return nullptr;
    }
    return &g_figures[index];
}

} // namespace

u8 Checksum(const u8* data, u32 count) {
    u32 sum = 0;
    for (u32 i = 0; i < count; ++i) {
        sum += data[i];
    }
    return static_cast<u8>(sum);
}

std::array<u8, 8> Encrypt(const u8* block, const u32* key) {
    const u32* k = key ? key : kCommandKey;
    u32 v0 = Load32(block), v1 = Load32(block + 4), sum = 0;
    for (int i = 0; i < 32; ++i) {
        sum += kDelta;
        v0 += ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        v1 += ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
    }
    std::array<u8, 8> out;
    Store32(out.data(), v0);
    Store32(out.data() + 4, v1);
    return out;
}

std::array<u8, 8> Decrypt(const u8* block, const u32* key) {
    const u32* k = key ? key : kCommandKey;
    u32 v0 = Load32(block), v1 = Load32(block + 4), sum = kDelta * 32;
    for (int i = 0; i < 32; ++i) {
        v1 -= ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
        v0 -= ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        sum -= kDelta;
    }
    std::array<u8, 8> out;
    Store32(out.data(), v0);
    Store32(out.data() + 4, v1);
    return out;
}

std::array<u32, 4> FigureKey(const Uid& uid) {
    std::array<u32, 4> key;
    for (int i = 0; i < 4; ++i) {
        // UID + constante = 24 bytes; o ultimo byte da ultima palavra usada vira 0xAA.
        u8 base[24];
        std::memcpy(base, uid.data(), 7);
        std::memcpy(base + 7, kCharConstant, 17);
        const int words = 3 + i;
        base[words * 4 - 1] = 0xAA;
        key[i] = Scramble(base, words);
    }
    return key;
}

TagData BuildTag(const Uid& uid, u32 character_id) {
    TagData tag{};
    // Paginas 0-2: UID no formato NTAG (com os bytes de verificacao BCC0/BCC1)
    tag[0] = uid[0];
    tag[1] = uid[1];
    tag[2] = uid[2];
    tag[3] = static_cast<u8>(0x88 ^ uid[0] ^ uid[1] ^ uid[2]);
    std::memcpy(&tag[4], &uid[3], 4);
    tag[8] = static_cast<u8>(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]);
    tag[9] = 0x48;
    // Pagina 3: capability container de um NTAG213
    tag[12] = 0xE1;
    tag[13] = 0x10;
    tag[14] = 0x12;

    if (character_id < 1000) {
        // Personagem: [id, id] cifrado com a chave da figura nas paginas 0x24-0x25
        u8 plain[8];
        Store32(plain, character_id);
        Store32(plain + 4, character_id);
        const auto key = FigureKey(uid);
        std::memcpy(&tag[0x24 * 4], Encrypt(plain, key.data()).data(), 8);
    } else {
        // Veiculo/gadget: id em claro na pagina 0x24 e marcador na 0x26
        Store32(&tag[0x24 * 4], character_id);
        tag[0x26 * 4 + 1] = 0x01;
    }

    // Pagina 0x2B: senha de leitura derivada do UID
    u8 base[32];
    std::memcpy(base, uid.data(), 7);
    std::memcpy(base + 7, kPasswordText, 23);
    base[30] = base[31] = 0xAA;
    Store32(&tag[0x2B * 4], Scramble(base, 8));
    return tag;
}

bool SessionReady() {
    std::lock_guard lock(g_mutex);
    return g_session_ready;
}

u32 SessionNumber() {
    std::lock_guard lock(g_mutex);
    return g_session_ready ? g_session_number : 0;
}

std::optional<Packet> PlaceFigure(int pad, const Uid& uid, u32 character_id) {
    std::lock_guard lock(g_mutex);
    if (pad < 1 || pad > kPadCount) {
        return std::nullopt;
    }
    Figure& figure = g_figures[pad];
    if (figure.present && figure.uid == uid && figure.id == character_id) {
        return std::nullopt;
    }
    figure.present = true;
    figure.pad = pad;
    figure.uid = uid;
    figure.id = character_id;
    figure.data = BuildTag(uid, character_id);
    figure.path.clear();
    ++g_stats.figures_placed;
    SavePortalStateLocked();
    return FigureEvent(pad, pad, false, uid);
}

u32 TagCharacterId(const TagData& tag) {
    const Uid uid{tag[0], tag[1], tag[2], tag[4], tag[5], tag[6], tag[7]};
    if (tag[0x26 * 4 + 1] == 0x01) { // veiculo / gadget: id em claro
        return Load32(&tag[0x24 * 4]);
    }
    const auto key = FigureKey(uid);
    const auto plain = Decrypt(&tag[0x24 * 4], key.data());
    const u32 id = Load32(plain.data());
    return id == Load32(plain.data() + 4) ? id : 0;
}

std::optional<Packet> LoadTag(int pad, int index, const TagData& tag, const std::string& path, u32* character_id) {
    std::lock_guard lock(g_mutex);
    if (pad < 1 || pad > kPadCount || index < 0 || index >= kSlotCount) {
        return std::nullopt;
    }
    Figure& figure = g_figures[index];
    // Fase 60: o app manda sempre o mesmo arquivo para um personagem; colocar uma segunda copia dele com o
    // mesmo UID e um estado impossivel num portal de verdade, e o jogo descarta a segunda figura (como no
    // listener do Cemu "Remote Toypad"). A copia ganha um UID novo, com o id e a senha recifrados para ele.
    TagData data = tag;
    const Uid incoming{tag[0], tag[1], tag[2], tag[4], tag[5], tag[6], tag[7]};
    bool collides = false;
    for (int other = 0; other < kSlotCount; ++other) {
        collides |= other != index && g_figures[other].present && g_figures[other].uid == incoming;
    }
    if (collides) {
        const u32 id = TagCharacterId(tag);
        static u32 seed = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
        Uid fresh{0x04, 0, 0, 0, 0, 0, 0x80};
        for (int i = 1; i < 6; ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            fresh[i] = static_cast<u8>(seed >> 24);
        }
        const TagData rebuilt = BuildTag(fresh, id);
        std::memcpy(data.data(), rebuilt.data(), 16);                         // UID e paginas 0-3
        std::memcpy(&data[0x24 * 4], &rebuilt[0x24 * 4], id < 1000 ? 8 : 4); // id (cifrado com a chave nova)
        std::memcpy(&data[0x2B * 4], &rebuilt[0x2B * 4], 4);                  // senha
        log::Tagged("PORTAL", "second copy of character %u on portal: new UID %02x %02x %02x %02x %02x %02x %02x", id, fresh[0],
                    fresh[1], fresh[2], fresh[3], fresh[4], fresh[5], fresh[6]);
        if (!path.empty()) { // como no Cemu: o arquivo da copia guarda o UID novo
            std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::in | std::ios::out);
            if (out) {
                out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
        }
    }
    figure.present = true;
    figure.pad = pad;
    figure.uid = {data[0], data[1], data[2], data[4], data[5], data[6], data[7]};
    figure.data = data;
    figure.id = TagCharacterId(data);
    figure.path = path;
    if (character_id) {
        *character_id = figure.id;
    }
    ++g_stats.figures_placed;
    SavePortalStateLocked();
    return FigureEvent(pad, index, false, figure.uid);
}

std::optional<Packet> RemoveIndex(int index) {
    std::lock_guard lock(g_mutex);
    if (index < 0 || index >= kSlotCount || !g_figures[index].present) {
        return std::nullopt;
    }
    Figure& figure = g_figures[index];
    figure.present = false;
    ++g_stats.figures_removed;
    SavePortalStateLocked();
    return FigureEvent(figure.pad, index, true, figure.uid);
}

std::vector<Packet> MoveIndex(int new_pad, int new_index, int old_index) {
    std::lock_guard lock(g_mutex);
    std::vector<Packet> events;
    if (new_pad < 1 || new_pad > kPadCount || new_index < 0 || new_index >= kSlotCount || old_index < 0 || old_index >= kSlotCount ||
        !g_figures[old_index].present) {
        return events;
    }
    Figure moved = g_figures[old_index];
    events.push_back(FigureEvent(moved.pad, old_index, true, moved.uid));
    g_figures[old_index].present = false;
    if (new_index != old_index && g_figures[new_index].present) { // destino ocupado: sai o que estava la
        events.push_back(FigureEvent(g_figures[new_index].pad, new_index, true, g_figures[new_index].uid));
    }
    moved.pad = new_pad;
    g_figures[new_index] = moved;
    events.push_back(FigureEvent(new_pad, new_index, false, moved.uid));
    SavePortalStateLocked();
    return events;
}

std::vector<Packet> PresentEvents() {
    std::lock_guard lock(g_mutex);
    std::vector<Packet> events;
    for (int index = 0; index < kSlotCount; ++index) {
        if (g_figures[index].present) {
            events.push_back(FigureEvent(g_figures[index].pad, index, false, g_figures[index].uid));
        }
    }
    return events;
}

void LedSnapshot(u8 out[kLedSnapshotBytes]) {
    std::lock_guard lock(g_mutex);
    std::memset(out, 0, kLedSnapshotBytes);
    out[0] = 'L';
    out[1] = g_led_counter;
    out[2] = 2; // versao do formato (a que o LegoToypad 1.6+ aceita)
    out[3] = kPadCount;
    for (int zone = 0; zone < kPadCount; ++zone) { // 12 bytes por zona
        const LedState& led = g_led[zone];
        u8* z = out + 4 + zone * 12;
        z[0] = static_cast<u8>(zone + 1); // a zona e conhecida mesmo antes do primeiro comando
        z[1] = led.mode;
        z[2] = led.r;
        z[3] = led.g;
        z[4] = led.b;
        z[5] = led.from_r;
        z[6] = led.from_g;
        z[7] = led.from_b;
        z[8] = led.on;
        z[9] = led.off;
        z[10] = led.count;
        z[11] = led.speed;
    }
}

std::optional<Packet> RemoveFigure(int pad) {
    std::lock_guard lock(g_mutex);
    if (pad < 1 || pad > kPadCount || !g_figures[pad].present) {
        return std::nullopt;
    }
    Figure& figure = g_figures[pad];
    figure.present = false;
    ++g_stats.figures_removed;
    SavePortalStateLocked();
    return FigureEvent(figure.pad, pad, true, figure.uid);
}

std::optional<Packet> HandleCommand(const u8* command, u32 length) {
    if (!command || length < 5 || command[0] != 0x55) {
        return std::nullopt;
    }
    const u8 len = command[1];
    const u8 cmd = command[2];
    const u8 sequence = command[3];
    const u8* payload = command + 4;
    if (len < 2 || 2u + len >= length) {
        return std::nullopt;
    }
    const bool checksum_ok = Checksum(command, 2 + len) == command[2 + len];

    std::lock_guard lock(g_mutex);
    ++g_stats.commands;
    const char* name = "?";
    Packet reply{};
    bool quiet = false;

    switch (cmd) {
    case 0xB0: { // ativacao: o portal devolve o mesmo texto de copyright
        name = "WAKE";
        ++g_stats.wake;
        g_session_ready = false; // o jogo recomeca a sessao (e reanuncia as figuras depois)
        static const char kText[] = "(c) LEGO 2014";
        reply = Reply(sequence, reinterpret_cast<const u8*>(kText), 13);
        break;
    }
    case 0xB1: { // semente do gerador: [seed LE][confirmacao], cifrado
        name = "SEED";
        ++g_stats.seed;
        const auto plain = Decrypt(payload);
        SeedRandom(Load32(plain.data()));
        const u8 answer[8] = {plain[4], plain[5], plain[6], plain[7], 0, 0, 0, 0};
        reply = Reply(sequence, Encrypt(answer).data(), 8);
        break;
    }
    case 0xB3: { // desafio: responde [proximo aleatorio LE][confirmacao], cifrado
        name = "CHALLENGE";
        ++g_stats.challenge;
        const auto plain = Decrypt(payload);
        u8 answer[8];
        Store32(answer, NextRandom());
        std::memcpy(answer + 4, plain.data(), 4);
        reply = Reply(sequence, Encrypt(answer).data(), 8);
        if (!g_session_ready) {
            ++g_session_number;
        }
        g_session_ready = true;
        break;
    }
    case 0xC0: // cor de um pad
    case 0xC1: // consulta de cor
    case 0xC2: // fade
    case 0xC3: // flash
    case 0xC4: // fade aleatorio
    case 0xC6: // fade em todos
    case 0xC7: // flash em todos
    case 0xC8: // cor em todos
        name = "LED";
        quiet = ++g_stats.led > 4; // chegam aos montes
        HandleLedCommand(cmd, payload, len >= 2 ? len - 2u : 0u); // len conta comando + sequencia + payload
        reply = Reply(sequence, nullptr, 0);
        break;
    case 0xD2: { // leitura: [indice][pagina] -> status + 4 paginas (16 bytes)
        name = "READ";
        ++g_stats.read;
        u8 body[17] = {};
        const Figure* figure = FigureAt(payload[0]);
        const u32 offset = payload[1] * 4u;
        if (figure && offset + 16 <= figure->data.size()) {
            std::memcpy(body + 1, figure->data.data() + offset, 16);
        }
        reply = Reply(sequence, body, 17);
        break;
    }
    case 0xD3: { // escrita: [indice][pagina][4 bytes]
        name = "WRITE";
        ++g_stats.write;
        Figure* figure = FigureAt(payload[0]);
        if (figure && payload[1] < kTagPages) {
            std::memcpy(figure->data.data() + payload[1] * 4u, payload + 2, 4);
            if (!figure->path.empty()) { // figura do app: o progresso volta para o arquivo dela
                std::ofstream out(std::filesystem::u8path(figure->path), std::ios::binary | std::ios::in | std::ios::out);
                if (out) {
                    out.write(reinterpret_cast<const char*>(figure->data.data()), static_cast<std::streamsize>(figure->data.size()));
                }
            }
        }
        const u8 status = 0;
        reply = Reply(sequence, &status, 1);
        break;
    }
    case 0xD4: { // modelo: [indice][...][confirmacao] cifrado -> [id LE][confirmacao] cifrado
        name = "MODEL";
        ++g_stats.model;
        const auto plain = Decrypt(payload);
        const Figure* figure = FigureAt(plain[0]);
        u8 answer[8] = {};
        if (figure) {
            Store32(answer, figure->id);
            std::memcpy(answer + 4, plain.data() + 4, 4);
        }
        u8 body[9] = {0};
        std::memcpy(body + 1, Encrypt(answer).data(), 8);
        reply = Reply(sequence, body, 9);
        break;
    }
    default: // 0xD0 lista de tags, 0xE1 senha, 0xE5 ativo, 0xFF consulta de LEDs...
        name = "NOT IMPLEMENTED";
        ++g_stats.unknown;
        reply = Reply(sequence, nullptr, 0);
        break;
    }

    if (!quiet) {
        log::Tagged("PORTAL", "cmd 0x%02X %s seq=%u%s: %s-> %s", cmd, name, sequence,
                    checksum_ok ? "" : " (invalid checksum)", Hex(command, 3u + len).c_str(),
                    Hex(reply.data(), 3u + reply[1]).c_str());
    }
    return reply;
}

Stats GetStats() {
    std::lock_guard lock(g_mutex);
    return g_stats;
}

void SetPortalFilePath(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    g_portal_file_path = path;
}

std::filesystem::path GetPortalFilePath() {
    std::lock_guard lock(g_mutex);
    return g_portal_file_path;
}

void SavePortalState(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    SavePortalStateLocked(path);
}

void LoadPortalState(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    const std::filesystem::path target = !path.empty() ? path : g_portal_file_path;
    if (target.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) return;

    std::ifstream in(target, std::ios::binary);
    if (!in) return;
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    for (int i = 0; i < kSlotCount; ++i) {
        g_figures[i] = {};
    }

    size_t slots_pos = json.find("\"slots\"");
    if (slots_pos != std::string::npos) {
        size_t arr_open = json.find('[', slots_pos);
        size_t arr_close = json.find(']', arr_open);
        if (arr_open != std::string::npos && arr_close != std::string::npos) {
            size_t pos = arr_open + 1;
            while (pos < arr_close) {
                size_t obj_open = json.find('{', pos);
                if (obj_open == std::string::npos || obj_open >= arr_close) break;
                size_t obj_close = json.find('}', obj_open);
                if (obj_close == std::string::npos || obj_close > arr_close) break;

                std::string obj = json.substr(obj_open, obj_close - obj_open + 1);
                auto get_num = [&](const std::string& key) -> int {
                    size_t kp = obj.find(key);
                    if (kp == std::string::npos) return -1;
                    size_t col = obj.find(':', kp);
                    if (col == std::string::npos) return -1;
                    return std::atoi(obj.c_str() + col + 1);
                };
                int idx = get_num("\"index\"");
                int pad = get_num("\"pad\"");
                int id = get_num("\"id\"");

                Uid uid{};
                size_t uid_pos = obj.find("\"uid\"");
                if (uid_pos != std::string::npos) {
                    size_t u_open = obj.find('[', uid_pos);
                    size_t u_close = obj.find(']', u_open);
                    if (u_open != std::string::npos && u_close != std::string::npos) {
                        size_t cur = u_open + 1;
                        for (int k = 0; k < 7; ++k) {
                            cur = obj.find_first_of("0123456789", cur);
                            if (cur == std::string::npos || cur > u_close) break;
                            uid[k] = static_cast<u8>(std::strtoul(obj.c_str() + cur, nullptr, 10));
                            cur = obj.find_first_not_of("0123456789", cur);
                        }
                    }
                }

                std::string fpath;
                size_t p_pos = obj.find("\"path\"");
                if (p_pos != std::string::npos) {
                    size_t str_open = obj.find('\"', obj.find(':', p_pos) + 1);
                    if (str_open != std::string::npos) {
                        size_t str_close = obj.find('\"', str_open + 1);
                        while (str_close != std::string::npos && obj[str_close - 1] == '\\') {
                            str_close = obj.find('\"', str_close + 1);
                        }
                        if (str_close != std::string::npos) {
                            fpath = obj.substr(str_open + 1, str_close - str_open - 1);
                            std::string unesc;
                            for (size_t c = 0; c < fpath.size(); ++c) {
                                if (fpath[c] == '\\' && c + 1 < fpath.size() && fpath[c + 1] == '\\') {
                                    unesc += '\\';
                                    ++c;
                                } else {
                                    unesc += fpath[c];
                                }
                            }
                            fpath = unesc;
                        }
                    }
                }

                if (idx >= 0 && idx < kSlotCount && pad >= 1 && pad <= kPadCount) {
                    Figure& f = g_figures[idx];
                    f.present = true;
                    f.pad = pad;
                    f.id = static_cast<u32>(id);
                    f.uid = uid;
                    f.path = fpath;
                    bool loaded_from_file = false;
                    if (!fpath.empty()) {
                        std::ifstream fin(std::filesystem::u8path(fpath), std::ios::binary);
                        if (fin && fin.read(reinterpret_cast<char*>(f.data.data()), f.data.size())) {
                            loaded_from_file = true;
                        }
                    }
                    if (!loaded_from_file) {
                        f.data = BuildTag(uid, static_cast<u32>(id));
                    }
                    log::Tagged("PORTAL", "Restored figure in slot %d pad %d (id %u)", idx, pad, f.id);
                }
                pos = obj_close + 1;
            }
            return;
        }
    }

    // Fallback: legacy format without slots
    for (int pad = 1; pad <= kPadCount; ++pad) {
        const std::string key = "\"pad" + std::to_string(pad) + "\"";
        size_t pos = json.find(key);
        if (pos == std::string::npos) continue;
        pos = json.find(':', pos + key.size());
        if (pos == std::string::npos) continue;
        pos = json.find_first_not_of(" \t\r\n", pos + 1);
        if (pos == std::string::npos || json[pos] != '{') continue;
        size_t end = json.find('}', pos);
        if (end == std::string::npos) continue;
        std::string obj = json.substr(pos, end - pos);
        size_t uid_pos = obj.find("\"uid\"");
        size_t id_pos = obj.find("\"id\"");
        if (uid_pos == std::string::npos || id_pos == std::string::npos) continue;
        size_t u_open = obj.find('[', uid_pos);
        size_t u_close = obj.find(']', u_open);
        if (u_open == std::string::npos || u_close == std::string::npos) continue;

        Uid uid{};
        size_t cur = u_open + 1;
        for (int k = 0; k < 7; ++k) {
            cur = obj.find_first_of("0123456789", cur);
            if (cur == std::string::npos || cur > u_close) break;
            uid[k] = static_cast<u8>(std::strtoul(obj.c_str() + cur, nullptr, 10));
            cur = obj.find_first_not_of("0123456789", cur);
        }
        size_t id_digits = obj.find_first_of("0123456789", obj.find(':', id_pos));
        if (id_digits == std::string::npos) continue;
        u32 id = static_cast<u32>(std::strtoul(obj.c_str() + id_digits, nullptr, 10));

        Figure& f = g_figures[pad];
        f.present = true;
        f.pad = pad;
        f.uid = uid;
        f.id = id;
        f.data = BuildTag(uid, id);
        f.path.clear();
        log::Tagged("PORTAL", "Restored legacy figure in pad %d (id %u)", pad, id);
    }
}

} // namespace hle::toypad
