// Fase 79 (4.0): multiplayer online. Ver multiplayer.h e FASE79_MULTIPLAYER_PESQUISA.md.
//
// Como funciona (tudo achado no eboot 1.24 / CUSA01176):
//  - Cada jogo manda o estado do seu jogador 1 (area, personagem, posicao, marcha, direcao, rumo, botoes) ao
//    servidor da sala; o servidor repassa aos outros.
//  - Os outros jogadores viram "fantasmas": personagens criados pelo sistema do portal (o mesmo que faz a figura
//    chegar pelo portal), com o personagem carregado num slot livre do portal.
//  - O fantasma e dirigido como um personagem de verdade: um gancho na atualizacao de controle de cada personagem
//    (eboot+d70730) grava os botoes antes (pulo, ataque) e a marcha + direcao depois (andar/correr com animacao).
#include "runtime/multiplayer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <Zydis/Zydis.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <vector>

#include "common/types.h"
#include "hle/generated/hle_stub.h"
#include "runtime/mp_net.h"

namespace runtime::multiplayer {
namespace {

// ---- enderecos do eboot (deslocamentos a partir da base) --------------------------------------------------------
constexpr u64 kPlayerTable = 0x3163788;    // instancia do jogador 1 (+8 = jogador 2)
constexpr u64 kControlFn = 0xd70730;       // atualizacao de controle de cada personagem (rdi = inst, xmm0 = dt)
constexpr u64 kSpawnFn = 0x158c2c0;        // AttemptToSpawnObject_Internal(spawner, pedido, 1)
constexpr u64 kRequestDtorFn = 0x15857b0;  // destrutor do pedido (desfaz o personagem)
constexpr u64 kRequestDeleteFn = 0x1585d40; // destrutor com delete (vtable[2] do pedido)
constexpr u64 kDescFn = 0x937c20;          // descritor do personagem (desc, nome, 0x73db, 0)
constexpr u64 kSlotLoadFn = 0x157f1f0;     // carrega o personagem num slot livre do portal
constexpr u64 kAllocatorA = 0x4adb10, kAllocatorB = 0x4adb90, kAllocFn = 0x4a83d0, kFreeFn = 0x4a94a0;
constexpr u64 kListPushFn = 0x22c0d90;     // lista do spawner (+0x1d0): {cabeca, cauda, contagem}, no {prox, ant, valor}
constexpr u64 kSpawnerVtable = 0x2cfada0, kRequestVtable = 0x2cfaca0;
// Listas de slots: vetor {dados, tamanho, capacidade | 0x40000000 (memoria que o jogo nao libera)}.
// Portal ("MechPortal Load Slot N", 7 na memoria do eboot) e jogador ("MechPortal Player Load Slot 0..3").
// Slot: nome em +0xb8, estado em +0x2c8, passo 0x2e0.
constexpr u64 kPortalSlots = 0x315ddd0, kPlayerSlots = 0x315f200, kSlotStep = 0x2e0;
constexpr int kMaxSlotsSeen = 64;
int g_nslots = 7, g_nall = 11; // atualizados a cada Tick (a lista do portal cresce no preparo da fase)
constexpr int kKeepFreeSlots = 1; // nunca ocupar o ultimo slot livre do portal: o jogador pode por uma figura nova
constexpr u64 kSlotStateFn = 0x157cdd0; // (slot, estado, 0): 0 = cancela pedido / esvazia, 3 = descarrega (o jogo leva a 0)
constexpr u64 kLevelSetupFn = 0x1577ba0; // prepara a memoria dos slots ao carregar a fase (rdi = config, +0x233)

constexpr u32 kCross = 0x40004000, kSquare = 0x10008000; // mascara de botoes do jogo
constexpr u32 kSentButtons = kCross | kSquare;           // pulo e ataque
constexpr int kMaxPuppets = 15;

const char kAllocTag[] = "MPGHOST";

Config g_config;
bool g_enabled = false;
u8* g_base = nullptr;
DWORD g_main_tid = 0;

template <typename T>
T& At(u64 address) {
    return *reinterpret_cast<T*>(address);
}

// O VirtualQuery custa ~1 ms neste processo (o jogo reserva muita memoria): as regioes ja conferidas ficam num
// cache pequeno, esvaziado a cada 2 s pelo Tick. So a thread principal usa.
struct Region {
    u64 lo, hi;
};
Region g_regions[32];
int g_region_count = 0, g_region_next = 0;

bool Readable(u64 address, u64 size) {
    if (address < 0x10000 || address + size < address) return false;
    for (int i = 0; i < g_region_count; ++i) {
        if (address >= g_regions[i].lo && address + size <= g_regions[i].hi) return true;
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
    const u64 lo = reinterpret_cast<u64>(info.BaseAddress), hi = lo + info.RegionSize;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !info.Protect) return false;
    g_regions[g_region_next] = {lo, hi};
    g_region_next = (g_region_next + 1) % 32;
    g_region_count = std::min(g_region_count + 1, 32);
    return address + size <= hi;
}

u64 Call(u64 offset, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0, u64 e = 0, u64 f = 0) {
    using Fn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64);
    return reinterpret_cast<Fn>(g_base + offset)(a, b, c, d, e, f);
}

u64 GameAllocator() { return Call(kAllocatorB, Call(kAllocatorA)); }
u64 GameAlloc(u64 size) { return Call(kAllocFn, GameAllocator(), size, 0x10, 1, reinterpret_cast<u64>(kAllocTag), 0); }
void GameFree(u64 pointer) { Call(kFreeFn, GameAllocator(), pointer, 0); }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- ganchos (mesmo metodo do carregador Anomaly: jmp [rip+0] + trampolim) -------------------------------------
void WriteJump(u8* at, const void* to) {
    at[0] = 0xFF;
    at[1] = 0x25;
    std::memset(at + 2, 0, 4);
    const u64 target = reinterpret_cast<u64>(to);
    std::memcpy(at + 6, &target, 8);
}

void* Hook(u64 offset, const void* replacement) {
    u8* target = g_base + offset;
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t length = 0;
    while (length < 14) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, target + length, 32, &insn, ops)) ||
            (insn.attributes & ZYDIS_ATTRIB_IS_RELATIVE)) {
            std::printf("[MP] eboot+0x%llx: prologo inesperado, gancho nao instalado\n", static_cast<unsigned long long>(offset));
            return nullptr;
        }
        length += insn.length;
    }
    auto* trampoline = static_cast<u8*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    std::memcpy(trampoline, target, length);
    WriteJump(trampoline + length, target + length);
    DWORD old = 0;
    VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old);
    WriteJump(target, replacement);
    std::memset(target + 14, 0x90, length - 14);
    VirtualProtect(target, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return trampoline;
}

using ControlFn = u64(HLE_SYSV_ABI*)(u64, float);
using SetupFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64);
SetupFn g_setup = nullptr;
SetupFn g_slot_load = nullptr;
using SpawnFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64);
using DtorFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64);
ControlFn g_control = nullptr;
SpawnFn g_spawn = nullptr;
DtorFn g_dtor = nullptr;

// ---- area atual (radar) -----------------------------------------------------------------------------------------
std::mutex g_area_mutex;
std::string g_area;
std::atomic<unsigned> g_area_epoch{0};
double g_area_time = 0;

// ---- spawners vistos nesta area (o jogo cria os personagens da party por eles) ----------------------------------
struct SpawnerSeen {
    u64 spawner;
    unsigned epoch;
};
std::mutex g_spawner_mutex;
std::vector<SpawnerSeen> g_spawners;
bool g_spawning = false; // nos mesmos chamando o spawn

// ---- fantasmas --------------------------------------------------------------------------------------------------
struct Remote {
    bool valid = false;
    std::string area, chr;
    float x = 0, y = 0, z = 0;
    int gear = 3;
    float dx = 0, dz = 1, fx = 0, fz = 1;
    u32 held = 0;
    bool ingame = false;
    int rtt_ms = 0; // latencia dele ate o servidor
};

enum class Stage { kNone, kLoading, kReady, kSpawning, kActive };

struct Puppet {
    int peer = 0;
    std::string name;
    Stage stage = Stage::kNone;
    std::string loading; // personagem que esta carregando
    u64 slot = 0, spawner = 0, request = 0, inst = 0, inst_name = 0;
    double stage_time = 0, retry_time = 0, ready_time = 0, wait_since = 0, load_fail_time = -100;
    bool want = false;   // esta na mesma area e no jogo (aparece, ou so o nome enquanto nao ha slot)
    float distance = 0;  // ate o nosso jogador 1 (quem esta mais perto ganha os slots)
    float still_x = 0, still_z = 0; // ultimo lugar onde o fantasma andou (para ver se ficou preso)
    double still_time = 0;
    unsigned seq = 0;
    Remote cur, prev;
    double cur_time = 0, prev_time = 0;
    alignas(16) u8 load_buffer[256] = {}; // descritor + saida do carregamento do slot (o jogo escreve aqui)
    char load_name[64] = {};
};

std::map<int, Puppet> g_puppets; // so a thread principal mexe

// Pedidos dos fantasmas vivos. O destrutor do jogo pode rodar em outra thread: ele so zera a entrada (sem trava,
// para nunca esperar a thread principal enquanto ela chama funcoes do jogo).
std::atomic<u64> g_live[32];

void LiveAdd(u64 request) {
    for (auto& e : g_live) {
        u64 expected = 0;
        if (e.compare_exchange_strong(expected, request)) return;
    }
}

bool LiveDrop(u64 request) {
    for (auto& e : g_live) {
        u64 expected = request;
        if (request && e.compare_exchange_strong(expected, 0)) return true;
    }
    return false;
}

bool LiveHas(u64 request) {
    for (auto& e : g_live) {
        if (request && e.load() == request) return true;
    }
    return false;
}

// O que o gancho de controle aplica (so thread principal, montado no Tick).
struct Drive {
    u64 inst;
    int gear;
    float speed; // 0 = a da tabela da marcha
    float dx, dz, fx, fz;
    u32 held, prev;
};

// nomes em cima dos fantasmas (o overlay le de outra thread)
std::mutex g_tags_mutex;
std::vector<NameTag> g_tags;
constexpr float kNameHeight = 0.26f; // acima da posicao do personagem (no meio do corpo; o boneco tem ~0.4)
Drive g_drive[kMaxPuppets];
int g_drive_count = 0;

double g_last_spawn = 0;
u32 g_held_latch = 0;
double g_held_latch_time = 0;
unsigned g_seen_epoch = 0;
double g_last_log = 0;

bool IsOurRequest(u64 request) { return LiveHas(request); }

bool CanLoad(); // slots (abaixo)
std::vector<std::string> g_our_chars; // personagens que NOS carregamos (minusculas), os unicos que cancelamos/descarregamos
double g_stuck_since[kMaxSlotsSeen] = {};

bool SomeoneLoading() {
    for (auto& [id, p] : g_puppets) {
        if (p.stage == Stage::kLoading) return true;
    }
    return false;
}

// ---- ganchos ----------------------------------------------------------------------------------------------------
std::atomic<u64> g_hook_calls{0}, g_hook_other_thread{0}, g_hook_driven{0};

HLE_SYSV_ABI u64 ControlHook(u64 inst, float dt) {
    Drive* d = nullptr;
    ++g_hook_calls;
    if (GetCurrentThreadId() != g_main_tid) ++g_hook_other_thread;
    if (g_drive_count && GetCurrentThreadId() == g_main_tid) {
        for (int i = 0; i < g_drive_count; ++i) {
            if (g_drive[i].inst == inst) d = &g_drive[i];
        }
    }
    if (d) ++g_hook_driven;
    if (d) { // botoes antes: o pulo/ataque e lido dentro do controle
        At<u32>(inst + 0x4f8 + 0xc) = d->held;
        At<u32>(inst + 0x4f8 + 0x10) = d->held & ~d->prev;
        At<u32>(inst + 0x4f8 + 0x14) = d->prev & ~d->held;
        d->prev = d->held;
        At<u8>(inst + 0x399) = 1; // sem isto o quadrado nao ataca (so durante o controle: fora dele trava o andar)
    }
    const u64 result = g_control(inst, dt);
    if (d) { // marcha e direcao depois: o controle (IA) acabou de definir as dele
        At<u8>(inst + 0x399) = 0;
        const u64 loc = At<u64>(inst + 0x90);
        if (loc) {
            if (d->speed > 0.0f) At<float>(loc + 0x70) = d->speed; // rapidez propria (alcanca o ponto recebido)
            else At<u32>(loc + 0x70) = 0xff7fffffu; // rapidez = tabela da marcha
            At<s32>(loc + 0x238) = d->gear;
            if (d->gear > 3) {
                At<float>(loc + 0x80) = d->dx;
                At<float>(loc + 0x84) = 0.0f;
                At<float>(loc + 0x88) = d->dz;
            }
        }
        At<float>(inst + 0x170) = d->fx; // rumo desejado
        At<float>(inst + 0x178) = d->fz;
    }
    return result;
}

// Os "Player Load Slots" so recebem memoria quando o byte +0x233 da config esta ligado no preparo da fase. O byte fica
// ligado SO durante o preparo: o resto do jogo continua vendo 0 (nao usa esses slots fora do co-op), e eles ficam
// livres para os personagens dos outros jogadores.
int g_extra_slots = 1; // 7 + 1 = 8 slots do portal (com 10 ou mais o jogo cai ao carregar; LD_MP_SLOTS = 7 desliga)

// O preparo comeca construindo os slots que faltam ate a capacidade (eboot+1577c30, construtor eboot+157d6e0) e depois
// da a cada um a sua area de 30 MB. Na primeira vez a lista passa para uma memoria nossa, maior (tamanho 0: o jogo
// constroi todos). Os 7 slots originais ficam parados (todo o codigo acha os slots pela lista).
void GrowPortalSlots() {
    const u64 desc = reinterpret_cast<u64>(g_base) + kPortalSlots;
    const u64 inline_storage = desc + 0x10;
    if (g_extra_slots <= 0 || At<u64>(desc) != inline_storage) return;
    const int total = 7 + g_extra_slots;
    void* block = VirtualAlloc(nullptr, total * kSlotStep + 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!block) return;
    At<u64>(desc) = reinterpret_cast<u64>(block);
    At<s32>(desc + 8) = 0;
    At<u32>(desc + 0xc) = static_cast<u32>(total) | 0x40000000u;
    std::printf("[MP] slots do portal: %d (para os personagens dos outros jogadores)\n", total);
    std::fflush(stdout);
}

// A area dos slots e UM bloco do heap principal: com mais slots ela nao cabe em nenhuma regiao de 512 MB ("MEM1 out of
// memory"). Logo depois de o jogo montar o heap, ele ganha uma regiao nossa (1 GB).
// A area dos slots e UM bloco do heap principal (MEM1, 4 regioes de 512 MB): mais de 11 slots do portal nao cabem
// ("MEM1 out of memory"). Dar ao heap uma regiao nossa fora da janela de 4 GB do jogo trava o carregamento.

HLE_SYSV_ABI u64 LevelSetupHook(u64 config, u64 a, u64 b, u64 c, u64 d, u64 e) {
    GrowPortalSlots();
    const u8 old = At<u8>(config + 0x233);
    At<u8>(config + 0x233) = 1;
    const u64 result = g_setup(config, a, b, c, d, e);
    At<u8>(config + 0x233) = old;
    return result;
}

bool g_spawning_flag_for_load = false;

// Ao aparecer, alguns personagens pedem a carga de uma variante (Bane/Cyborg "_bigfig", o Homer bravo, o proximo
// Doctor, o chi do Laval) e soltam o pedido em seguida: o slot fica em estado 2 e e descarregado por SlotMaintenance
// quando a carga em segundo plano termina. Com LD_MP_DEBUG registra cada carga (lista, slot).
u64 SlotAddress(int i);
HLE_SYSV_ABI u64 SlotLoadHook(u64 out, u64 a, u64 b, u64 c, u64 d, u64 e) {
    const s32 type = Readable(b, 4) ? At<s32>(b) : -1;
    const u64 r = g_slot_load(out, a, b, c, d, e);
    static const bool dbg = std::getenv("LD_MP_DEBUG") != nullptr;
    if (dbg) {
        const u64 slot = Readable(out, 8) ? At<u64>(out) : 0;
        int idx = -1;
        for (int i = 0; i < g_nall; ++i) {
            if (SlotAddress(i) == slot) idx = i;
        }
        std::printf("[MP] carga%s: tipo %d -> slot %d (%s) st %d 2d4 %d\n", g_spawning_flag_for_load ? " (no spawn)" : "", type,
                    idx, idx >= 0 ? reinterpret_cast<const char*>(slot + 0xb8) : "-", idx >= 0 ? At<s32>(slot + 0x2c8) : -1,
                    idx >= 0 ? At<u8>(slot + 0x2d4) : -1);
        std::fflush(stdout);
    }
    return r;
}

HLE_SYSV_ABI u64 SpawnHook(u64 spawner, u64 request, u64 flag, u64 a, u64 b, u64 c) {
    if (!g_spawning) {
        std::lock_guard lock(g_spawner_mutex);
        const unsigned epoch = g_area_epoch;
        bool known = false;
        for (auto& s : g_spawners) {
            if (s.spawner == spawner) {
                s.epoch = epoch;
                known = true;
            }
        }
        if (!known) {
            if (g_spawners.size() > 64) g_spawners.erase(g_spawners.begin());
            g_spawners.push_back({spawner, epoch});
        }
    }
    return g_spawn(spawner, request, flag, a, b, c);
}

HLE_SYSV_ABI u64 RequestDtorHook(u64 request, u64 a, u64 b, u64 c, u64 d, u64 e) {
    LiveDrop(request); // o jogo desfez um fantasma (a figura do spawner saiu, a fase acabou...): o Tick ve e recria
    return g_dtor(request, a, b, c, d, e);
}

// ---- slots do portal --------------------------------------------------------------------------------------------
// 0..6 = slots do portal, 7..10 = slots de jogador
u64 SlotAddress(int i) {
    const u64 portal = At<u64>(reinterpret_cast<u64>(g_base) + kPortalSlots);
    const u64 player = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerSlots);
    return i < g_nslots ? portal + i * kSlotStep : player + (i - g_nslots) * kSlotStep;
}

void UpdateSlotCounts() {
    g_nslots = std::clamp(At<s32>(reinterpret_cast<u64>(g_base) + kPortalSlots + 8), 0, 48);
    g_nall = g_nslots + std::clamp(At<s32>(reinterpret_cast<u64>(g_base) + kPlayerSlots + 8), 0, 4);
}
int SlotState(int i) { return At<s32>(SlotAddress(i) + 0x2c8); }
std::string SlotName(int i) {
    const char* name = reinterpret_cast<const char*>(SlotAddress(i) + 0xb8);
    return std::string(name, strnlen(name, 48));
}

std::map<std::string, int> g_wrong_count; // personagem -> vezes que o fantasma apareceu com outro
std::set<std::string> g_unsupported;       // personagens que nao aparecem certos: so o nome
std::map<u64, std::string> g_our_slots; // slot -> personagem que NOS carregamos nele
bool OursFamily(const std::string& slot_name);

// Um slot que o jogo carregou como VARIANTE de um fantasma (o proximo Doctor, o Homer bravo...) pertence ao fantasma
// que o pediu: e solto quando ele some. Fantasma nenhum nasce dele (nascer de la e depois perder o slot derrubava a
// thread de fundo). Servem: os slots que nos carregamos e os das figuras do proprio jogador.
u64 FindLoadedSlot(const std::string& chr) {
    const std::string want = Lower(chr) + ".cd";
    for (int i = 0; i < g_nall; ++i) {
        if (SlotState(i) != 1 || SlotName(i) != want) continue;
        const u64 slot = SlotAddress(i);
        const auto it = g_our_slots.find(slot);
        if (it != g_our_slots.end() && it->second + ".cd" == want) return slot; // nosso, deste personagem
        if (it == g_our_slots.end() && !OursFamily(want)) return slot;           // figura do jogador
    }
    return 0;
}

int FreeSlots() { // do portal
    int n = 0;
    for (int i = 0; i < g_nslots; ++i) n += SlotState(i) == 0;
    return n;
}

bool FreePlayerSlot();
// Alguns personagens (Bane, Cyborg...) fazem o jogo reservar mais um slot do portal ("<nome>_bigfig", fica em
// estado 2): conta com ele para sobrar sempre kKeepFreeSlots livres para uma figura nova no portal.
bool UsePlayerSlot() { return FreePlayerSlot() && FreeSlots() >= kKeepFreeSlots; } // a reserva bigfig e cancelada (SlotMaintenance)
bool CanLoad() { return UsePlayerSlot() || FreeSlots() >= kKeepFreeSlots + 1; }

bool FreePlayerSlot() { // slot de jogador vazio e com memoria (preparo da fase com o gancho)
    for (int i = g_nslots; i < g_nall; ++i) {
        // com memoria propria (os 2 ultimos usam a memoria geral e nao carregam: +0x2d4 = 1)
        if (SlotState(i) == 0 && At<u32>(SlotAddress(i) + 0x58) != 0 && !At<u8>(SlotAddress(i) + 0x2d4)) return true;
    }
    return false;
}

bool IsSlot(u64 slot) {
    for (int i = 0; i < g_nall; ++i) {
        if (SlotAddress(i) == slot) return true;
    }
    return false;
}

// ---- manutencao dos slots ---------------------------------------------------------------------------------------
// "bane.cd", "bane_bigfig.cd", "homersimpson_angry.cd"... -> personagem (ou variante dele) carregado por nos?
bool Ours(const std::string& slot_name) {
    const std::string base = slot_name.size() > 3 ? slot_name.substr(0, slot_name.size() - 3) : slot_name;
    for (const std::string& c : g_our_chars) {
        if (base == c || (base.size() > c.size() && base.compare(0, c.size(), c) == 0 && base[c.size()] == '_')) return true;
    }
    return false;
}

std::string LocalCharacter(u64 inst);
void Log(const char* fmt, const std::string& a, const std::string& b = {});

bool LocalPlayerUses(const std::string& slot_name) { // o jogador 1 ou 2 deste PC esta com esse personagem
    for (int k = 0; k < 2; ++k) {
        const u64 inst = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable + 8 * k);
        if (inst && Lower(LocalCharacter(inst)) + ".cd" == slot_name) return true;
    }
    return false;
}

bool PuppetUses(u64 slot) {
    for (auto& [id, p] : g_puppets) {
        if (p.slot == slot && p.stage != Stage::kNone) return true;
    }
    return false;
}

// Pedidos que o jogo nunca atende (ex.: "<nome>_bigfig" dos personagens grandes, estado 2 para sempre) prendem slots.
// Os dos NOSSOS personagens sao cancelados depois de 6 s (testado: o fantasma continua normal).
// "doctor4th.cd" -> "doctor", "bane_bigfig.cd" -> "bane": a familia (o Doctor pre-carrega o proximo Doctor ao
// aparecer; o Bane, o bigfig).
std::string Family(const std::string& slot_name) {
    std::string f;
    for (char c : slot_name) {
        if (!std::isalpha(static_cast<unsigned char>(c))) break;
        f += c;
    }
    return f;
}

// O pedido preso e de uma figura do PROPRIO jogador (mesma familia de um personagem que o jogo carregou, nao nos)?
bool OursFamily(const std::string& slot_name) { // da mesma familia de algum personagem que nos carregamos
    const std::string fam = Family(slot_name);
    for (const std::string& c : g_our_chars) {
        if (Family(c) == fam) return true;
    }
    return false;
}

bool LocalFamily(const std::string& slot_name) {
    const std::string fam = Family(slot_name);
    for (int j = 0; j < g_nall; ++j) {
        if (SlotState(j) == 1 && !Ours(SlotName(j)) && Family(SlotName(j)) == fam) return true;
    }
    return false;
}

// A carga de um slot corre em segundo plano (NuBgProc, trabalho em slot+0x88) e continua mesmo depois de o pedido ser
// solto (estado 2). Esvaziar o slot com a carga em andamento derrubava a NuBgProc (eboot+add9a0): era sempre no meio
// da carga de uma variante que um fantasma pediu (Homer bravo, o proximo Doctor, o chi do Laval). Como o jogo
// (eboot+157d9c0): so descarrega com o trabalho terminado, e sempre pelo estado 3, que espera o resto e leva a 0.
bool SlotJobDone(u64 slot) {
    return At<u64>(slot + 0x88 + 0x18) == 0; // trabalho em andamento (eboot+d193b0)
}

void SlotMaintenance(double now) {
    for (int i = 0; i < g_nall; ++i) {
        const u64 slot = SlotAddress(i);
        const std::string name = SlotName(i);
        const bool loading_for_us = std::any_of(g_puppets.begin(), g_puppets.end(),
                                                [&](const auto& e) { return e.second.stage == Stage::kLoading && e.second.slot == slot; });
        // pedidos de variantes dos nossos personagens, ou que nenhuma figura do jogador explica (ex.: o proximo Doctor
        // que o fantasma de um Doctor pede ao aparecer)
        const bool cancelable = Ours(name) || !LocalFamily(name);
        if (SlotState(i) != 2 || loading_for_us || !cancelable || LocalPlayerUses(name) || PuppetUses(slot)) {
            g_stuck_since[i] = 0;
            continue;
        }
        if (!g_stuck_since[i]) g_stuck_since[i] = now;
        if (now - g_stuck_since[i] > 6.0 && SlotJobDone(slot)) {
            Log("[MP] descarregando a variante solta %s", name);
            Call(kSlotStateFn, slot, 3, 0); // o pedido foi solto (estado 2): o jogo faz o mesmo (eboot+157f10f)
            g_stuck_since[i] = 0;
        }
    }
}

// Sem slot livre: descarrega um personagem que NOS carregamos e que ninguem usa mais (amigo saiu ou trocou).
double g_last_load_start = -100;
// O estado do slot volta a 0 antes de a thread de fundo terminar de descarregar: carregar logo depois (no mesmo slot)
// derrubava a NuBgProc (eboot+add9a0). Depois de descarregar, espera 8 s para carregar ou descarregar de novo.
double g_last_unload = -100;
void Unload(u64 slot) {
    if (!SlotJobDone(slot)) return; // ainda carregando em segundo plano: tenta de novo depois
    g_last_unload = runtime::mp_net::Now();
    g_our_slots.erase(slot);
    Call(kSlotStateFn, slot, 3, 0);
}
double g_state2_since[kMaxSlotsSeen] = {};

void TrackState2(double now) {
    for (int i = 0; i < g_nall && i < kMaxSlotsSeen; ++i) {
        if (SlotState(i) != 2) g_state2_since[i] = 0;
        else if (!g_state2_since[i]) g_state2_since[i] = now;
    }
}

bool LoaderIdle() {
    if (runtime::mp_net::Now() - g_last_load_start < 6.0 || runtime::mp_net::Now() - g_last_unload < 8.0) return false;
    for (int i = 0; i < g_nall; ++i) {
        // algo carregando; um pedido parado ha mais de 8 s nao conta (fica preso para sempre, nao e carga)
        if (SlotState(i) == 2 && (!g_state2_since[i] || runtime::mp_net::Now() - g_state2_since[i] < 8.0)) return false;
    }
    for (auto& [id, p] : g_puppets) {
        if (p.stage == Stage::kLoading || p.stage == Stage::kSpawning) return false;
    }
    return true;
}

// Desfazer um personagem termina depois, em segundo plano: descarregar o slot dele logo em seguida derrubava a thread
// NuBgProc (eboot+add9a0). Um slot so e descarregado 10 s depois do ultimo fantasma que o usou.
std::map<u64, double> g_slot_used;

bool Settled(u64 slot) {
    if (PuppetUses(slot)) return false;
    const auto it = g_slot_used.find(slot);
    return it == g_slot_used.end() || runtime::mp_net::Now() - it->second > 10.0;
}

bool RecycleSlot() {
    if (!LoaderIdle()) return false;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < g_nall; ++i) {
            const bool player = i >= g_nslots;
            if (player != (pass == 0)) continue; // primeiro os de jogador
            const u64 slot = SlotAddress(i);
            const std::string name = SlotName(i);
            if (SlotState(i) != 1 || !Ours(name) || !Settled(slot) || LocalPlayerUses(name)) continue;
            Log("[MP] liberando o slot de %s", name);
            Unload(slot);
            return true;
        }
    }
    return false;
}

void Despawn(struct Puppet& p);
bool SomeoneLoading();

// O jogador pos figuras novas e o portal ficou sem slot livre: devolve um slot do portal que NOS carregamos (os
// fantasmas que o usavam aparecem com outro personagem). As figuras do jogador sempre tem prioridade.
void ReclaimReserve(double now) {
    static double last = 0, short_since = 0;
    if (FreeSlots() >= kKeepFreeSlots) {
        short_since = 0;
        return;
    }
    if (!short_since) short_since = now;
    // espera 8 s: muitas vezes quem tomou o slot e um pedido preso (bigfig) que a manutencao cancela sozinha
    if (now - short_since < 8.0 || !LoaderIdle() || now - last < 5.0) return;
    for (int pass = 0; pass < 2; ++pass) // primeiro um slot nosso sem uso; so depois um em uso
    for (int i = 0; i < g_nslots; ++i) {
        const u64 slot = SlotAddress(i);
        const std::string name = SlotName(i);
        if (SlotState(i) != 1 || !Ours(name) || LocalPlayerUses(name)) continue;
        if (pass == 0 && !Settled(slot)) continue;
        if (pass == 1) { // em uso: tira os fantasmas agora; o slot e descarregado (passo 0) quando assentar
            for (auto& [id, p] : g_puppets) {
                if (p.slot == slot && (p.stage == Stage::kSpawning || p.stage == Stage::kActive)) Despawn(p);
                if (p.slot == slot) {
                    p.stage = Stage::kNone;
                    p.load_fail_time = now; // nao tenta carregar de novo logo em seguida
                }
            }
            g_slot_used[slot] = now;
            last = now;
            return;
        }
        Log("[MP] devolvendo o slot de %s para as figuras do portal", name);
        Unload(slot);
        last = now;
        short_since = 0;
        return;
    }
}

// Sem slot para o personagem de um jogador: tira o de quem esta bem mais longe (o mais longe primeiro). Quem perde
// fica so com o nome flutuando ate voltar a ter vez. Margem de 4 unidades para os dois nao ficarem trocando.
bool ReclaimForCloser(const Puppet& wanting, double now) {
    static double last = 0;
    if (now - last < 5.0 || wanting.distance > 15.0f || !LoaderIdle()) return false; // longe demais / carregando
    u64 best = 0;
    float best_distance = wanting.distance + 6.0f;
    for (int i = 0; i < g_nall; ++i) {
        const u64 slot = SlotAddress(i);
        const std::string name = SlotName(i);
        if (SlotState(i) != 1 || !Ours(name) || LocalPlayerUses(name)) continue;
        float nearest = 1e9f;
        bool used = false;
        bool recent = false;
        for (auto& [id, q] : g_puppets) {
            if (q.slot == slot && q.stage != Stage::kNone) {
                used = true;
                nearest = std::min(nearest, q.distance);
                recent |= now - q.stage_time < 20.0; // apareceu ha pouco: fica pelo menos 20 s
            }
        }
        if (recent) continue;
        if (!used) nearest = 1e8f; // ninguem usa: o melhor candidato
        if (nearest > best_distance) {
            best_distance = nearest;
            best = slot;
        }
    }
    if (!best) return false;
    last = now;
    if (!Settled(best)) { // em uso: tira os fantasmas agora; o slot e liberado (RecycleSlot) quando assentar
        for (auto& [id, q] : g_puppets) {
            if (q.slot == best && (q.stage == Stage::kSpawning || q.stage == Stage::kActive)) Despawn(q);
            if (q.slot == best) {
                q.stage = Stage::kNone;
                q.retry_time = now + 3.0;
            }
        }
        g_slot_used[best] = now;
        Log("[MP] slot de %s para quem esta mais perto (libera em 10 s)", reinterpret_cast<const char*>(best + 0xb8));
        return true;
    }
    Log("[MP] slot de %s para quem esta mais perto", reinterpret_cast<const char*>(best + 0xb8));
    Unload(best);
    return true;
}

// ---- spawn / despawn --------------------------------------------------------------------------------------------
bool ValidSpawner(u64 spawner) {
    return Readable(spawner, 0x200) && At<u64>(spawner) == reinterpret_cast<u64>(g_base) + kSpawnerVtable;
}

u64 TemplateRequest(u64 spawner) {
    u64 node = At<u64>(spawner + 0x1d0);
    for (int guard = 0; node && guard < 64 && Readable(node, 0x18); ++guard) {
        const u64 request = At<u64>(node + 0x10);
        if (Readable(request, 0xf0) && At<u64>(request) == reinterpret_cast<u64>(g_base) + kRequestVtable &&
            At<u64>(request + 0x30) == spawner && !IsOurRequest(request)) {
            return request;
        }
        node = At<u64>(node);
    }
    return 0;
}

bool PickSpawner(u64* spawner, u64* request) {
    std::lock_guard lock(g_spawner_mutex);
    const unsigned epoch = g_area_epoch;
    for (auto it = g_spawners.rbegin(); it != g_spawners.rend(); ++it) {
        if (it->epoch != epoch || !ValidSpawner(it->spawner)) continue;
        if (u64 r = TemplateRequest(it->spawner)) {
            *spawner = it->spawner;
            *request = r;
            return true;
        }
    }
    return false;
}

bool Spawn(Puppet& p) {
    u64 spawner = 0, model = 0;
    if (!PickSpawner(&spawner, &model)) return false;
    const u64 request = GameAlloc(0xf0);
    if (!request) return false;
    std::memcpy(reinterpret_cast<void*>(request), reinterpret_cast<const void*>(model), 0xf0);
    // ligacoes, alcas e memoria propria do pedido original: o destrutor libera/desliga (copiar = queda)
    for (auto [a, b] : {std::pair{0x18, 0x30}, {0x40, 0x70}, {0xa8, 0xc0}, {0xc8, 0xe8}}) {
        std::memset(reinterpret_cast<void*>(request + a), 0, b - a);
    }
    u64 holder = request;
    Call(kListPushFn, spawner + 0x1d0, reinterpret_cast<u64>(&holder), 0);
    const u64 original_slot = At<u64>(spawner + 0x1f0);
    At<u64>(spawner + 0x1f0) = p.slot;
    p.request = request;
    p.spawner = spawner;
    LiveAdd(request);
    std::printf("[MP] spawn: spawner %llx (slot %s) com slot %s\n", static_cast<unsigned long long>(spawner),
                reinterpret_cast<const char*>(original_slot + 0xb8), reinterpret_cast<const char*>(p.slot + 0xb8));
    std::fflush(stdout);
    g_spawning = true;
    g_spawning_flag_for_load = true;
    g_spawn(spawner, request, 1, 0, 0, 0);
    g_spawning_flag_for_load = false;
    g_spawning = false;
    At<u64>(spawner + 0x1f0) = original_slot;
    return true;
}

// Solta o no do pedido da lista do spawner, libera o no e apaga o pedido (o jogo desfaz o personagem).
void Despawn(Puppet& p) {
    const u64 request = p.request, spawner = p.spawner;
    if (p.slot) g_slot_used[p.slot] = runtime::mp_net::Now();
    p.request = p.inst = p.inst_name = 0;
    p.stage = Stage::kNone;
    if (!LiveDrop(request) || !ValidSpawner(spawner)) return; // o jogo ja tinha desfeito
    const u64 list = spawner + 0x1d0;
    u64 node = At<u64>(list);
    for (int guard = 0; node && guard < 64 && Readable(node, 0x18); ++guard) {
        if (At<u64>(node + 0x10) == request) {
            const u64 next = At<u64>(node), prev = At<u64>(node + 8);
            if (prev) At<u64>(prev) = next;
            else At<u64>(list) = next;
            if (next) At<u64>(next + 8) = prev;
            else At<u64>(list + 8) = prev;
            At<s32>(list + 0x10) = At<s32>(list + 0x10) - 1;
            GameFree(node);
            Call(kRequestDeleteFn, request); // passa pelo gancho do destrutor (ja zerado acima)
            return;
        }
        node = At<u64>(node);
    }
}

bool ParseRemote(const std::string& text, Remote* r) {
    std::vector<std::string> f;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, '|')) f.push_back(item);
    if (f.size() < 12) return false;
    r->area = f[0];
    r->chr = f[1];
    r->x = std::strtof(f[2].c_str(), nullptr);
    r->y = std::strtof(f[3].c_str(), nullptr);
    r->z = std::strtof(f[4].c_str(), nullptr);
    r->gear = std::atoi(f[5].c_str());
    r->dx = std::strtof(f[6].c_str(), nullptr);
    r->dz = std::strtof(f[7].c_str(), nullptr);
    r->fx = std::strtof(f[8].c_str(), nullptr);
    r->fz = std::strtof(f[9].c_str(), nullptr);
    r->held = static_cast<u32>(std::strtoul(f[10].c_str(), nullptr, 16));
    r->ingame = f[11] == "1";
    r->rtt_ms = f.size() > 12 ? std::clamp(std::atoi(f[12].c_str()), 0, 2000) : 0;
    r->valid = std::isfinite(r->x) && std::isfinite(r->y) && std::isfinite(r->z);
    return r->valid;
}

u64 Transform(u64 inst) { return Readable(inst + 0x1b0, 8) ? At<u64>(inst + 0x1b0) : 0; }

void Teleport(u64 inst, float x, float y, float z) {
    const u64 t = Transform(inst);
    if (!t) return;
    const float offset = At<float>(inst + 0x164) - At<float>(t + 0x94); // a matriz propria fica um pouco abaixo
    At<float>(t + 0x90) = x;
    At<float>(t + 0x94) = y;
    At<float>(t + 0x98) = z;
    At<float>(inst + 0x160) = x;
    At<float>(inst + 0x164) = y + offset;
    At<float>(inst + 0x168) = z;
}

bool PuppetAlive(const Puppet& p) {
    return p.inst && Readable(p.inst + 0x10, 0x500) && At<u64>(p.inst + 0x10) == p.inst_name;
}

// Rapidez da marcha na tabela do componente de locomocao (+0x280, indice = marcha).
float TableSpeed(u64 inst, int gear) {
    const u64 loc = Readable(inst + 0x90, 8) ? At<u64>(inst + 0x90) : 0;
    if (!loc || gear < 0 || gear > 15 || !Readable(loc + 0x280 + gear * 4, 4)) return 0.0f;
    const float v = At<float>(loc + 0x280 + gear * 4);
    return std::isfinite(v) && v > 0.05f && v < 20.0f ? v : 0.0f;
}

// Calcula a marcha/direcao do fantasma para este quadro (aplicada pelo gancho de controle).
void Steer(Puppet& p, double now, Drive* d) {
    const Remote& r = p.cur;
    const double age = now - p.cur_time;
    // Onde o amigo esta AGORA: a posicao recebida + a velocidade dele (marcha e direcao que ele mandou, a mesma conta
    // do jogo) x o tempo desde que ele mandou = idade do pacote + metade da nossa ida e volta + metade da dele.
    float vx = 0, vz = 0;
    if (r.gear > 3) {
        const float n = std::sqrt(r.dx * r.dx + r.dz * r.dz);
        const float speed = TableSpeed(p.inst, r.gear);
        if (n > 0.01f && speed > 0.0f) {
            vx = r.dx / n * speed;
            vz = r.dz / n * speed;
        }
    }
    const double latency = (runtime::mp_net::RttMs() + r.rtt_ms) * 0.5e-3;
    const float lead = static_cast<float>(std::clamp(age + latency, 0.0, 0.5));
    const float tx = r.x + vx * lead, tz = r.z + vz * lead;
    const u64 t = Transform(p.inst);
    const float px = At<float>(t + 0x90), py = At<float>(t + 0x94), pz = At<float>(t + 0x98);
    const float ex = tx - px, ez = tz - pz;
    const float dist = std::sqrt(ex * ex + ez * ez);
    if (dist > 4.0f || std::fabs(r.y - py) > 4.0f) {
        Teleport(p.inst, r.x, r.y, r.z); // muito longe (entrou agora, caiu, atalho): pula para la
    }
    d->inst = p.inst;
    d->speed = 0.0f;
    d->held = age < 1.0 ? (r.held & kSentButtons) : 0;
    const bool stale = age > 3.0;
    if (!stale && r.gear > 3) {
        const float k = 0.8f * std::min(dist, 1.5f);
        float dx = r.dx + (dist > 0.001f ? ex / dist * k : 0), dz = r.dz + (dist > 0.001f ? ez / dist * k : 0);
        const float n = std::sqrt(dx * dx + dz * dz);
        if (n > 0.001f) {
            dx /= n;
            dz /= n;
        }
        d->gear = r.gear;
        d->dx = d->fx = dx;
        d->dz = d->fz = dz;
        // atras do ponto (na direcao do movimento): um pouco mais rapido ate encostar; na frente: um pouco mais devagar
        const float base = TableSpeed(p.inst, r.gear);
        if (base > 0.0f) {
            const float along = ex * dx + ez * dz;
            d->speed = std::clamp(base + 3.0f * along, base * 0.5f, base * 1.6f);
        }
    } else if (dist > 0.12f) { // parado la e o fantasma longe: vai ate o lugar (devagar quando perto)
        d->gear = 6;
        const float base = TableSpeed(p.inst, 6);
        if (base > 0.0f) d->speed = std::clamp(dist * 4.0f, 0.3f, base);
        d->dx = d->fx = ex / dist;
        d->dz = d->fz = ez / dist;
    } else {
        d->gear = 3;
        d->dx = d->fx = r.fx;
        d->dz = d->fz = r.fz;
    }
    // preso (outro personagem, parede): tentando andar ha 1 s sem sair do lugar -> vai direto para o ponto
    if (std::hypot(px - p.still_x, pz - p.still_z) > 0.05f || d->gear <= 3 || dist < 0.6f) {
        p.still_x = px;
        p.still_z = pz;
        p.still_time = now;
    } else if (now - p.still_time > 1.0) {
        Teleport(p.inst, r.x, r.y, r.z);
        p.still_time = now;
    }
}

std::string LocalCharacter(u64 inst) {
    if (!inst || !Readable(inst + 0x10, 8)) return {};
    const u64 name = At<u64>(inst + 0x10);
    if (!Readable(name, 32)) return {};
    std::string s(reinterpret_cast<const char*>(name), strnlen(reinterpret_cast<const char*>(name), 31));
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return {};
    }
    return s;
}

std::atomic<bool> g_in_world{false}; // o jogador 1 esta numa fase (para os avisos na tela)

void SendLocal(u64 p1, double now) {
    std::string area;
    {
        std::lock_guard lock(g_area_mutex);
        area = g_area;
    }
    char text[256];
    const u64 t = p1 ? Transform(p1) : 0;
    const u64 loc = p1 && Readable(p1 + 0x90, 8) ? At<u64>(p1 + 0x90) : 0;
    if (!p1 || !t || !loc || area.empty()) {
        std::snprintf(text, sizeof(text), "%s|-|0|0|0|3|0|1|0|1|0|0", area.empty() ? "-" : area.c_str());
        runtime::mp_net::SetLocalState(text);
        g_in_world = false;
        return;
    }
    g_in_world = true;
    u32 held = 0;
    const u64 party = At<u64>(p1 + 0x3a0);
    if (party && Readable(party + 0x18, 0x10)) held = At<u32>(party + 0x18 + 0xc);
    held &= kSentButtons;
    // um toque curto (1-2 quadros) tem de sobreviver ate o proximo envio (~50 ms)
    if (held) {
        g_held_latch |= held;
        g_held_latch_time = now;
    } else if (now - g_held_latch_time > 0.08) {
        g_held_latch = 0;
    }
    std::snprintf(text, sizeof(text), "%s|%s|%.3f|%.3f|%.3f|%d|%.3f|%.3f|%.3f|%.3f|%x|1|%d", area.c_str(),
                  LocalCharacter(p1).c_str(), At<float>(t + 0x90), At<float>(t + 0x94), At<float>(t + 0x98),
                  At<s32>(loc + 0x238), At<float>(loc + 0x80), At<float>(loc + 0x88), At<float>(p1 + 0x170),
                  At<float>(p1 + 0x178), held | g_held_latch, runtime::mp_net::RttMs());
    runtime::mp_net::SetLocalState(text);
}

void Log(const char* fmt, const std::string& a, const std::string& b) {
    std::printf(fmt, a.c_str(), b.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

} // namespace

void Configure(const Config& config) {
    g_config = config;
    g_enabled = !config.server.empty() && !config.room.empty();
}

bool Enabled() { return g_enabled; }

std::vector<NameTag> NameTags() {
    if (!g_enabled) return {};
    std::lock_guard lock(g_tags_mutex);
    return g_tags;
}

void Install(std::uint8_t* eboot_base) {
    if (!g_enabled) return;
    g_base = eboot_base;
    g_control = reinterpret_cast<ControlFn>(Hook(kControlFn, reinterpret_cast<const void*>(&ControlHook)));
    g_spawn = reinterpret_cast<SpawnFn>(Hook(kSpawnFn, reinterpret_cast<const void*>(&SpawnHook)));
    g_dtor = reinterpret_cast<DtorFn>(Hook(kRequestDtorFn, reinterpret_cast<const void*>(&RequestDtorHook)));
    if (const char* env = std::getenv("LD_MP_SLOTS")) g_extra_slots = std::clamp(std::atoi(env) - 7, 0, 1);
    g_setup = reinterpret_cast<SetupFn>(Hook(kLevelSetupFn, reinterpret_cast<const void*>(&LevelSetupHook))); // sem ele: so o portal
    g_slot_load = reinterpret_cast<SetupFn>(Hook(kSlotLoadFn, reinterpret_cast<const void*>(&SlotLoadHook)));
    if (!g_control || !g_spawn || !g_dtor) {
        std::printf("[MP] eboot diferente: multiplayer desligado\n");
        g_enabled = false; // os ganchos que entraram so repassam (sem fantasmas nada muda)
        return;
    }
    if (!runtime::mp_net::Start(g_config.server, g_config.room, g_config.name, g_config.password)) {
        std::printf("[MP] endereco do servidor invalido: %s\n", g_config.server.c_str());
        g_enabled = false;
        return;
    }
    std::printf("[MP] multiplayer online ligado: sala %s, servidor %s\n", g_config.room.c_str(), g_config.server.c_str());
}

bool InWorld() { return g_enabled && g_in_world; }

void OnArea(const std::string& area) {
    if (!g_enabled) return;
    std::lock_guard lock(g_area_mutex);
    const double now = runtime::mp_net::Now();
    if (area != g_area || now - g_area_time > 3.0) {
        ++g_area_epoch; // area nova (ou a mesma de novo): o mundo anterior esta sendo desmontado
    }
    g_area = area;
    g_area_time = now;
}

void Tick() {
    if (!g_enabled) return;
    if (!g_main_tid) g_main_tid = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_main_tid) return;
    const double now = runtime::mp_net::Now();
    static double regions_time = 0;
    if (now - regions_time > 2.0) { // a memoria pode mudar: confere de novo de vez em quando
        regions_time = now;
        g_region_count = g_region_next = 0;
    }
    const u64 p1 = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    UpdateSlotCounts();
    SendLocal(p1, now);

    std::string area;
    double area_time = 0;
    {
        std::lock_guard lock(g_area_mutex);
        area = g_area;
        area_time = g_area_time;
    }
    const unsigned epoch = g_area_epoch;
    // fantasmas que o jogo desfez sozinho
    for (auto& [id, p] : g_puppets) {
        if ((p.stage == Stage::kSpawning || p.stage == Stage::kActive) && !LiveHas(p.request)) {
            p.request = p.inst = p.inst_name = 0;
            p.stage = Stage::kNone;
            p.retry_time = now + 1.0;
        }
    }
    if (epoch != g_seen_epoch) { // troca de area: tira os fantasmas que ainda existirem
        g_seen_epoch = epoch;
        for (auto& [id, p] : g_puppets) {
            if (p.stage == Stage::kSpawning || p.stage == Stage::kActive) Despawn(p);
            p.stage = Stage::kNone;
        }
    }
    const std::string local_chr = LocalCharacter(p1);
    for (auto& [id, p] : g_puppets) {
        if (p.slot && p.stage != Stage::kNone) g_slot_used[p.slot] = now;
    }
    TrackState2(now);
    SlotMaintenance(now);
    ReclaimReserve(now);
    const bool settled = p1 && !area.empty() && now - area_time > 3.0;

    // pares atuais da sala
    const auto peers = runtime::mp_net::Peers();
    for (auto it = g_puppets.begin(); it != g_puppets.end();) {
        const bool present = std::any_of(peers.begin(), peers.end(), [&](const auto& q) { return q.id == it->first; });
        if (!present) {
            if (it->second.stage == Stage::kSpawning || it->second.stage == Stage::kActive) Despawn(it->second);
            it = g_puppets.erase(it);
        } else {
            ++it;
        }
    }
    Drive old[kMaxPuppets];
    const int old_count = g_drive_count;
    std::copy(g_drive, g_drive + old_count, old);
    g_drive_count = 0;
    for (const auto& peer : peers) {
        if (g_puppets.size() >= kMaxPuppets && !g_puppets.count(peer.id)) continue;
        Puppet& p = g_puppets[peer.id];
        p.peer = peer.id;
        p.name = peer.name;
        if (peer.seq != p.seq && !peer.state.empty()) {
            Remote r;
            if (ParseRemote(peer.state, &r)) {
                p.prev = p.cur;
                p.prev_time = p.cur_time;
                p.cur = r;
                p.cur_time = peer.state_time;
            }
            p.seq = peer.seq;
        }
        const bool want = settled && p.cur.valid && p.cur.ingame && p.cur.area == area && now - p.cur_time < 6.0 &&
                          p.cur.chr != "-" && !p.cur.chr.empty();
        p.want = want;
        if (const u64 t = p1 ? Transform(p1) : 0) {
            p.distance = std::hypot(p.cur.x - At<float>(t + 0x90), p.cur.z - At<float>(t + 0x98));
        }
        if (!want) {
            if (p.stage == Stage::kSpawning || p.stage == Stage::kActive) Despawn(p);
            p.stage = Stage::kNone;
            continue;
        }
        switch (p.stage) {
        case Stage::kNone:
            if (now < p.retry_time || now - g_last_spawn < 0.5) break;
            if (g_unsupported.count(Lower(p.cur.chr))) break; // personagem que nao e figura (so o nome aparece)
            p.retry_time = now + 1.0;
            if (u64 slot = FindLoadedSlot(p.cur.chr)) {
                p.slot = slot;
                p.stage = Stage::kReady;
            } else if (CanLoad() && SomeoneLoading()) {
                // o jogo carrega um personagem por vez: espera a vez (enquanto isso so o nome aparece)
                if (!p.wait_since) p.wait_since = now;
            } else if (CanLoad() && now - p.load_fail_time > 15.0 && now - g_last_unload > 8.0) { // carrega num slot livre
                p.wait_since = 0;
                const bool player_slot = UsePlayerSlot(); // primeiro os de jogador (nao disputam com as figuras)
                std::memset(p.load_buffer, 0, sizeof(p.load_buffer));
                At<s32>(reinterpret_cast<u64>(p.load_buffer) + 96) = player_slot ? 1 : 0; // tipo: 0 portal, 1 jogador
                std::snprintf(p.load_name, sizeof(p.load_name), "%s", p.cur.chr.c_str());
                const u64 buffer = reinterpret_cast<u64>(p.load_buffer);
                Call(kDescFn, buffer, reinterpret_cast<u64>(p.load_name), 0x73db, 0);
                Call(kSlotLoadFn, buffer + 64, 0, buffer + 96, buffer, 0, 1);
                const u64 slot = At<u64>(buffer + 64);
                if (const std::string low = Lower(p.cur.chr); std::find(g_our_chars.begin(), g_our_chars.end(), low) == g_our_chars.end()) {
                    g_our_chars.push_back(low);
                }
                p.loading = p.cur.chr;
                p.slot = IsSlot(slot) ? slot : 0;
                if (p.slot) g_our_slots[p.slot] = Lower(p.cur.chr);
                p.stage = p.slot ? Stage::kLoading : Stage::kNone;
                p.stage_time = now;
                if (!p.slot) p.load_fail_time = now; // nao deu agora: so o nome, e tenta de novo depois
                g_last_load_start = now;
                Log("[MP] carregando %s para %s", p.cur.chr, p.name);
            } else if (!SomeoneLoading() && now - p.load_fail_time > 15.0 && RecycleSlot()) {
                p.retry_time = now + 1.0; // o slot esvazia em seguida; carrega na proxima tentativa
            } else if (!SomeoneLoading()) {
                ReclaimForCloser(p, now); // sem slot: tira de quem esta bem mais longe (senao, so o nome por enquanto)
            }
            break;
        case Stage::kLoading:
            if (At<s32>(p.slot + 0x2c8) == 1) {
                if (!p.ready_time) p.ready_time = now;
                if (now - p.ready_time < 2.0) break;
                p.ready_time = 0;
                p.stage = Stage::kReady;
            } else if (now - p.stage_time > 20.0) {
                p.load_fail_time = now;
                p.slot = 0;
                p.stage = Stage::kNone;
            }
            break;
        case Stage::kReady:
            if (now - g_last_spawn < 0.5) break;
            if (At<s32>(p.slot + 0x2c8) != 1) {
                p.stage = Stage::kNone;
                break;
            }
            if (Spawn(p)) {
                g_last_spawn = now;
                p.stage = Stage::kSpawning;
                p.stage_time = now;
            }
            break;
        case Stage::kSpawning: {
            const u64 request = p.request;
            const u64 comp = request && Readable(request + 0x40, 8) ? At<u64>(request + 0x40) : 0;
            const u64 inst = comp && Readable(comp + 0x20, 8) ? At<u64>(comp + 0x20) : 0;
            if (inst && Readable(inst + 0x10, 0x500)) {
                p.inst = inst;
                p.inst_name = At<u64>(inst + 0x10);
                p.stage = Stage::kActive;
                p.stage_time = now;
                Teleport(inst, p.cur.x, p.cur.y, p.cur.z);
                Log("[MP] %s apareceu como %s", p.name, LocalCharacter(inst));
                // nome que o jogo nao conhece como figura (ex.: Wheatley): o descritor cai no personagem do spawner.
                // A primeira vez pode ser so a carga recente; na terceira desiste e mostra so o nome.
                const std::string low = Lower(p.cur.chr);
                if (Lower(LocalCharacter(inst)) == low) {
                    g_wrong_count.erase(low);
                } else if (++g_wrong_count[low] >= 3) {
                    g_unsupported.insert(low);
                    Log("[MP] %s nao e uma figura que este jogo carrega: so o nome de %s aparece", p.cur.chr, p.name);
                    Despawn(p);
                }
            } else if (now - p.stage_time > 10.0) {
                Despawn(p);
            }
            break;
        }
        case Stage::kActive:
            if (!PuppetAlive(p)) {
                p.stage = Stage::kNone;
                p.request = p.inst = 0;
                break;
            }
            // o amigo trocou de personagem: refaz o fantasma (se o novo ja esta carregado ou cabe num slot)
            if (Lower(LocalCharacter(p.inst)) != Lower(p.cur.chr) && now - p.stage_time > 1.0 &&
                (FindLoadedSlot(p.cur.chr) || (!SomeoneLoading() && now - p.load_fail_time > 15.0 && (CanLoad() || RecycleSlot())))) {
                Despawn(p);
                p.retry_time = 0;
                break;
            }
            if (now - p.stage_time > 1.5 && g_drive_count < kMaxPuppets) { // deixa a chegada pelo portal terminar
                Drive& d = g_drive[g_drive_count];
                u32 prev = 0;
                for (int k = 0; k < old_count; ++k) {
                    if (old[k].inst == p.inst) prev = old[k].prev; // botoes do quadro anterior (bordas)
                }
                Steer(p, now, &d);
                d.prev = prev;
                ++g_drive_count;
            }
            break;
        }
    }
    {
        std::vector<NameTag> tags;
        for (auto& [id, p] : g_puppets) {
            if (p.stage == Stage::kActive && PuppetAlive(p)) {
                if (const u64 t = Transform(p.inst)) {
                    tags.push_back({At<float>(t + 0x90), At<float>(t + 0x94) + kNameHeight, At<float>(t + 0x98), p.name});
                }
            } else if (p.want && p.cur.valid) { // ainda sem personagem (sem slot / carregando): so o nome, onde ele esta
                tags.push_back({p.cur.x, p.cur.y + kNameHeight, p.cur.z, p.name});
            }
        }
        static const bool debug_self = std::getenv("LD_MP_TAG_SELF") != nullptr; // teste: nome no proprio jogador
        if (debug_self && p1) {
            if (const u64 t = Transform(p1)) tags.push_back({At<float>(t + 0x90), At<float>(t + 0x94) + kNameHeight, At<float>(t + 0x98), "P1"});
        }
        std::lock_guard lock(g_tags_mutex);
        g_tags.swap(tags);
    }
    if (now - g_last_log > (std::getenv("LD_MP_DEBUG") ? 3.0 : 30.0)) {
        g_last_log = now;
        int active = 0;
        for (auto& [id, p] : g_puppets) active += p.stage == Stage::kActive;
        std::printf("[MP] %s | area %s | %zu na sala, %d visiveis | gancho %llu (outra thread %llu, dirigidos %llu)\n",
                    runtime::mp_net::StatusText().c_str(), area.empty() ? "-" : area.c_str(), peers.size(), active,
                    static_cast<unsigned long long>(g_hook_calls.load()), static_cast<unsigned long long>(g_hook_other_thread.load()),
                    static_cast<unsigned long long>(g_hook_driven.load()));
        for (auto& [id, p] : g_puppets) {
            if (p.stage != Stage::kActive) continue;
            const u64 t = Transform(p.inst);
            std::printf("[MP]   %s: remoto (%.2f %.2f %.2f) marcha %d ha %.1fs | fantasma (%.2f %.2f %.2f)\n", p.name.c_str(), p.cur.x,
                        p.cur.y, p.cur.z, p.cur.gear, now - p.cur_time, t ? At<float>(t + 0x90) : 0.f, t ? At<float>(t + 0x94) : 0.f,
                        t ? At<float>(t + 0x98) : 0.f);
        }
        std::fflush(stdout);
    }
}

} // namespace runtime::multiplayer
