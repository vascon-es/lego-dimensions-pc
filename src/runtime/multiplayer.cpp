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
#include <thread>
#include <vector>

#include "common/game_region.h"
#include "common/types.h"
#include "hle/generated/hle_stub.h"
#include "hle/overrides/hle_filesystem.h"
#include "runtime/mp_net.h"

namespace runtime::multiplayer {
void OnLocalKill(unsigned long long obj, unsigned long long killer); // Fase 80 (no fim do arquivo)
void Announce(const std::string& payload); // 4.2: avisa a sala e guarda no registro da area (fora do namespace anonimo)
namespace {

// ---- enderecos do eboot (deslocamentos a partir da base) --------------------------------------------------------
constexpr u64 kPlayerTable = 0x3163788;    // instancia do jogador 1 (+8 = jogador 2)
constexpr u64 kControlFn = 0xd70730;       // atualizacao de controle de cada personagem (rdi = inst, xmm0 = dt)
// 4.1.1: as funcoes acima de ~0x1577000 ficam 0x10 depois em outro dump do eboot: FnAt() confere o prologo e escolhe
u64 kSpawnFn = 0x158c2c0;        // AttemptToSpawnObject_Internal(spawner, pedido, 1)
u64 kRequestDtorFn = 0x15857b0;  // destrutor do pedido (desfaz o personagem)
u64 kRequestDeleteFn = 0x1585d40; // destrutor com delete (vtable[2] do pedido)
u64 kDescFn = 0x937c20;          // descritor do personagem (desc, nome, 0x73db, 0)
u64 kSlotLoadFn = 0x157f1f0;     // carrega o personagem num slot livre do portal
constexpr u64 kAllocatorA = 0x4adb10, kAllocatorB = 0x4adb90;
u64 kAllocFn = 0x4a83d0, kFreeFn = 0x4a94a0;
u64 kListPushFn = 0x22c0d90;     // lista do spawner (+0x1d0): {cabeca, cauda, contagem}, no {prox, ant, valor}
constexpr u64 kSpawnerVtable = 0x2cfada0, kRequestVtable = 0x2cfaca0;
// Listas de slots: vetor {dados, tamanho, capacidade | 0x40000000 (memoria que o jogo nao libera)}.
// Portal ("MechPortal Load Slot N", 7 na memoria do eboot) e jogador ("MechPortal Player Load Slot 0..3").
// Slot: nome em +0xb8, estado em +0x2c8, passo 0x2e0.
constexpr u64 kPortalSlots = 0x315ddd0, kPlayerSlots = 0x315f200, kSlotStep = 0x2e0;
constexpr int kMaxSlotsSeen = 64;
int g_nslots = 7, g_nall = 11; // atualizados a cada Tick (a lista do portal cresce no preparo da fase)
constexpr int kKeepFreeSlots = 1; // nunca ocupar o ultimo slot livre do portal: o jogador pode por uma figura nova
u64 kSlotStateFn = 0x157cdd0; // (slot, estado, 0): 0 = cancela pedido / esvazia, 3 = descarrega (o jogo leva a 0)
u64 kLevelSetupFn = 0x1577ba0; // prepara a memoria dos slots ao carregar a fase (rdi = config, +0x233)

constexpr u32 kCross = 0x40004000, kSquare = 0x10008000; // mascara de botoes do jogo
constexpr u32 kSentButtons = kCross | kSquare;           // pulo e ataque
constexpr int kMaxPuppets = 24;   // 4.5.1: cada amigo pode aparecer com mais de um personagem (a party dele)
constexpr int kPuppetStride = 16; // chave no mapa dos fantasmas: id do amigo * 16 + indice do personagem
// 4.5.1: quantos personagens de cada amigo aparecem ao mesmo tempo. Com 2 pessoas na sala: 6 de cada (sobra sempre
// um personagem do outro para voce ver); com 3: 5 de cada; com mais: o que couber em 12 dividido pelas pessoas.
int PartyCap(size_t peers) {
    const int people = std::max<int>(2, static_cast<int>(peers) + 1);
    if (people <= 2) return 6;
    if (people == 3) return 5;
    return std::max(2, 12 / people);
}

const char kAllocTag[] = "MPGHOST";

Config g_config;
bool g_enabled = false;
u8* g_base = nullptr;
DWORD g_main_tid = 0;
// 4.4.5: o motivo da ultima falha de conexao (a janela do F3 mostra isso em vez de so "fora da rede").
std::string g_last_error;
u64 g_control_fn = 0; // kControlFn resolvido (o dump pode vir com a tabela deslocada)

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

u64 Call8(u64 offset, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) { // 7o e 8o na pilha
    using Fn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64, u64, u64);
    return reinterpret_cast<Fn>(g_base + offset)(a, b, c, d, e, f, g, h);
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
    std::string area;
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
    std::string level; // 4.1.1: fase atual ("LevelPack\...\Nome#ordem"); vazio = versao antiga / nao sabe
    std::string region; // 4.5.1: CUSA01176 (EUR) ou CUSA00935 (USA): as fases sao mapeadas pelo pacote, mas o aviso ajuda
    std::vector<std::string> party; // 4.5.1: os outros personagens equipados dele (o ativo e o `chr`)
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
    double want_since = 0; // desde quando quer aparecer
    bool standin = false;  // 4.1.1: sem slot para o personagem dele: aparece com um personagem ja carregado
    double same_area_since = 0; // 4.2: desde quando esta na nossa area
    bool replayed = false;      // 4.2: o dono ja reenviou o registro da area para ele
    float distance = 0;  // ate o nosso jogador 1 (quem esta mais perto ganha os slots)
    float still_x = 0, still_z = 0; // ultimo lugar onde o fantasma andou (para ver se ficou preso)
    double still_time = 0;
    int stuck_side = 0; // 4.5.1: para que lado ele desvia quando fica preso (sem teleporte)
    int idx = 0;        // 4.5.1: 0 = personagem ativo do amigo, 1.. = os outros da party dele
    int clashes = 0; // 4.5: vezes que o pedido acabou preso na party do jogador local (Task 6)
    unsigned seq = 0;
    float move_dx = 0, move_dz = 0; // 4.4.8: deslocamento real entre os dois ultimos pacotes (diagnostico)
    float last_px = 0, last_pz = 0, last_wx = 0, last_wz = 0; // 4.4.8: sonda da convencao da direcao
    float last_dist = 99.0f; // 4.4.8: distancia do quadro anterior (a sonda so vale sem correcao acontecendo)
    bool have_last = false;
    Remote cur, prev;
    double cur_time = 0, prev_time = 0;
    alignas(16) u8 load_buffer[256] = {}; // descritor + saida do carregamento do slot (o jogo escreve aqui)
    char load_name[64] = {};
};

std::map<int, Puppet> g_puppets; // so a thread principal mexe

// 4.4.8: a direcao que o gancho de controle recebe e' do mundo ou do espaco do corpo? O jogo nao documenta, entao o
// port descobre sozinho: se o fantasma anda perpendicular ao que foi mandado (de lado), troca a convencao. O
// LD_MP_MOVE=world|local fixa uma delas para teste.
std::atomic<bool> g_move_local{true};
std::atomic<int> g_move_probe_n{0};
std::atomic<float> g_move_probe_dot{0.0f};

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

// Fase 80 (teste, LD_MP_PLAYERMODE): o fantasma vira um "jogador" de verdade. O jogo so deixa personagens de jogador
// quebrar objetos, montar etc. (inst+0x399 = 1 e um controle em inst+0x3a0). Cada fantasma ganha uma copia do controle
// do jogador 1 (+0x18 = suporte de botoes, +0x54 forca do analogico, +0x58/+0x5c direcao, +0xf8 = inst+0x370), que
// a gente preenche com o que veio da rede. O sistema de controles do jogo nao conhece a copia (nao a sobrescreve).
struct FakeController {
    u64 ctrl = 0, holder = 0;
};
std::map<u64, FakeController> g_fake_ctrl;
bool PlayerMode() {
    static const bool on = std::getenv("LD_MP_PLAYERMODE") != nullptr;
    return on;
}
u64 GameAlloc(u64 size);

FakeController* FakeControllerFor(u64 inst) {
    auto it = g_fake_ctrl.find(inst);
    if (it != g_fake_ctrl.end()) return &it->second;
    const u64 p1 = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    const u64 src = p1 && Readable(p1 + 0x3a0, 8) ? At<u64>(p1 + 0x3a0) : 0;
    if (!src || !Readable(src, 0x200)) return nullptr;
    const u64 src_holder = At<u64>(src + 0x18);
    // o estado bruto do controle (+0x18) tem listas internas: o fantasma aponta para o do jogador 1 (so leitura)
    const u64 c = GameAlloc(0x200), h = src_holder;
    if (!c || !h) return nullptr;
    std::memcpy(reinterpret_cast<void*>(c), reinterpret_cast<const void*>(src), 0x200);
    At<u64>(c + 0xf8) = inst + 0x370;
    std::printf("[MP] fantasma %llx virou jogador: controle %llx (copia de %llx), botoes %llx\n",
                static_cast<unsigned long long>(inst), static_cast<unsigned long long>(c),
                static_cast<unsigned long long>(src), static_cast<unsigned long long>(h));
    std::fflush(stdout);
    return &(g_fake_ctrl[inst] = FakeController{c, h});
}

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
    if (d && PlayerMode()) {
        if (FakeController* fc = FakeControllerFor(inst)) {
            const bool moving = d->gear > 3;
            At<u64>(inst + 0x3a0) = fc->ctrl;
            // botoes: no controle (+0x24 segurados, +0x28 apertados agora, +0x2c soltos agora) e no estado bruto do
            // controle (+0x18 -> +0 atual, +4 anterior, +8 apertados, +0xc soltos), como o do jogador 1
            const u32 pressed = d->held & ~d->prev, released = d->prev & ~d->held;
            At<u32>(fc->ctrl + 0x24) = d->held;
            At<u32>(fc->ctrl + 0x28) = pressed;
            At<u32>(fc->ctrl + 0x2c) = released;
            d->prev = d->held;
            At<float>(fc->ctrl + 0x54) = moving ? 1.4f : 0.0f;
            At<float>(fc->ctrl + 0x58) = moving ? d->dx : 0.0f;
            At<float>(fc->ctrl + 0x5c) = moving ? d->dz : 0.0f;
            At<u8>(inst + 0x399) = 1;
            const u64 result = g_control(inst, dt);
            // 4.5: o analogico alimenta so a caminhada; a cara continua sendo a do outro jogo (sem isto o motor vira
            // o personagem na direcao do analógico e o fantasma "anda de lado")
            if (std::isfinite(d->fx) && std::isfinite(d->fz) && std::fabs(d->fx) + std::fabs(d->fz) > 0.01f) {
                At<float>(inst + 0x170) = d->fx;
                At<float>(inst + 0x178) = d->fz;
            }
            return result;
        }
    }
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
            if (d->gear > 3 && std::isfinite(d->dx) && std::isfinite(d->dz)) {
                At<float>(loc + 0x80) = d->dx; // loc+0x80: SO a velocidade (direcao da caminhada)
                At<float>(loc + 0x84) = 0.0f;
                At<float>(loc + 0x88) = d->dz;
            }
        }
        // 4.5: inst+0x170 e SO o rumo do corpo (a cara), que vem do outro jogo. A correcao de posicao vive em
        // loc+0x80 - se ela for escrita aqui tambem, o fantasma anda de lado/gira ao corrigir a deriva lateral.
        if (std::isfinite(d->fx) && std::isfinite(d->fz) && std::fabs(d->fx) + std::fabs(d->fz) > 0.01f) {
            At<float>(inst + 0x170) = d->fx;
            At<float>(inst + 0x178) = d->fz;
        }
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
        std::string area;
        {
            std::lock_guard area_lock(g_area_mutex);
            area = g_area;
        }
        bool known = false;
        for (auto& s : g_spawners) {
            if (s.spawner == spawner) {
                s.epoch = epoch;
                s.area = area;
                known = true;
            }
        }
        if (!known) {
            if (g_spawners.size() > 64) g_spawners.erase(g_spawners.begin());
            g_spawners.push_back({spawner, epoch, area});
        }
    }
    return g_spawn(spawner, request, flag, a, b, c);
}

// ---- 4.5: eventos de interface (dialogo, camera, cutscene, minigame) ---------------------------------------------
// Aplicar o progresso de missao de outro jogador nao pode abrir janela/camera neste jogo. Enquanto g_quest_silent esta
// ligado (so durante o pedido de multiestado que aplica um aviso "q"), os eventos de interface que o motor dispara
// por causa daquele estado morrem no gancho; o estado em si ja foi escrito pelo pedido.
std::atomic<bool> g_quest_silent{false};

bool IsUiEventName(const char* name) {
    static const char* kUi[] = {"Dialog", "Speech", "Subtitle", "Camera", "Cutscene", "Cinematic", "Movie",
                                "Keystone", "Hack", "Minigame", "MiniGame", "Wheel", "Terminal", "Prompt",
                                "Menu", "Hud", "HUD", "Banner", "Tutorial"};
    for (const char* ui : kUi) {
        if (std::strstr(name, ui)) return true;
    }
    return false;
}

// ---- pesquisa (sincronizar os jogos): LD_MP_EVENTS registra os eventos do jogo (eboot+4c2d00 = dispara um evento:
// rdi = tipo do evento, objeto global com o nome em +8; rsi = quem manda; rdx = dados) ----
u64 kFireEventFn = 0x4c2d00;
SetupFn g_fire_event = nullptr;
std::mutex g_event_mutex;
struct EventStat {
    u64 count = 0, shown = 0, window = 0; // window: neste relatorio de 2 s
    u64 last_sender = 0, last_payload = 0;
};
std::map<std::string, EventStat> g_events;
void ReportEventsLocked(double now);

HLE_SYSV_ABI u64 FireEventHook(u64 ev, u64 sender, u64 payload, u64 a, u64 b, u64 c) {
    static const bool log_events = std::getenv("LD_MP_EVENTS") != nullptr;
    const bool silent = g_quest_silent.load();
    if (!silent && !log_events) return g_fire_event(ev, sender, payload, a, b, c); // custo zero no caminho normal
    char name[64] = {};
    if (ev && Readable(ev + 8, 8)) {
        const u64 name_ptr = At<u64>(ev + 8);
        if (name_ptr && Readable(name_ptr, 1)) {
            for (int i = 0; i < 63 && Readable(name_ptr + i, 1); ++i) {
                name[i] = At<char>(name_ptr + i);
                if (!name[i]) break;
            }
        }
    }
    // 4.5: aviso de missao aplicado aqui: dialogo, camera, cutscene ou minigame do outro jogador nao abrem neste
    if (silent && name[0] && IsUiEventName(name)) return 0;
    if (!log_events) return g_fire_event(ev, sender, payload, a, b, c);
    if (ev && Readable(ev + 8, 8)) {
        const double now = runtime::mp_net::Now();
        std::lock_guard lock(g_event_mutex);
        EventStat& st = g_events[name[0] ? name : "?"];
        if (!st.count) std::printf("[EV0] %s = %llx\n", name, static_cast<unsigned long long>(ev)); // o tipo (objeto global)
        ++st.count;
        ++st.window;
        st.last_sender = sender;
        st.last_payload = payload;
        // eventos raros (os que interessam: quebrar, montar, bater...): cada um com a hora e os dados
        if (st.window <= 3 && st.count > 0 && (st.count <= 40 || st.window <= 2)) {
            u64 q[6] = {};
            if (payload && Readable(payload, sizeof(q))) std::memcpy(q, reinterpret_cast<const void*>(payload), sizeof(q));
            std::printf("[EV1] t=%.2f %s de %llx dados %llx: %llx %llx %llx %llx %llx %llx\n", now, name,
                        static_cast<unsigned long long>(sender), static_cast<unsigned long long>(payload),
                        static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]), static_cast<unsigned long long>(q[2]),
                        static_cast<unsigned long long>(q[3]), static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]));
        }
        // pesquisa da campanha junto: eventos de troca de fase/area com os dados inteiros (e os textos para onde apontam)
        const std::string nm = name;
        if ((nm.find("Level") != std::string::npos || nm.find("Area") != std::string::npos || nm.find("Flow") != std::string::npos) &&
            nm.find("onProcess") == std::string::npos && nm.find("SafeArea") == std::string::npos &&
            nm.find("MenuFlow2") == std::string::npos && nm.find("Subtitle") == std::string::npos &&
            nm.find("GameDeactivate") == std::string::npos && nm.find("AvailableLevel") == std::string::npos) {
            std::printf("[EVL] t=%.2f %s ev %llx de %llx dados %llx chamado de eboot+%llx\n", now, name, static_cast<unsigned long long>(ev),
                        static_cast<unsigned long long>(sender), static_cast<unsigned long long>(payload),
                        static_cast<unsigned long long>(reinterpret_cast<u64>(__builtin_return_address(0)) - reinterpret_cast<u64>(g_base)));
            for (u64 off = 0; payload && off < 0x80 && Readable(payload + off, 8); off += 8) {
                const u64 v = At<u64>(payload + off);
                std::string text;
                if (v > 0x10000 && Readable(v, 1)) {
                    for (int i = 0; i < 64 && Readable(v + i, 1); ++i) {
                        const char ch = At<char>(v + i);
                        if (!ch) break;
                        if (ch < 32 || ch > 126) { text.clear(); break; }
                        text += ch;
                    }
                }
                std::printf("[EVL]   +%02llx %016llx %s\n", static_cast<unsigned long long>(off), static_cast<unsigned long long>(v),
                            text.size() >= 3 ? ("\"" + text + "\"").c_str() : "");
            }
            std::fflush(stdout);
        }
        ReportEventsLocked(now);
    }
    return g_fire_event(ev, sender, payload, a, b, c);
}

// eboot+1707dd0 = destroi um objeto do jogo (rdi = objeto, esi = motivo, rdx = quem destruiu, ...): dispara
// LegoGameObjects::EventShouldBeKilled (pode ser cancelado) e destroi. Pesquisa: registra o objeto (tipo, nome, posicao).
u64 kKillObjectFn = 0x1707dd0;
using KillFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64, u64, u64);
KillFn g_kill_object = nullptr;

std::string FirstString(u64 obj) { // o primeiro ponteiro dos primeiros 0x200 bytes que aponta para um texto legivel
    for (u64 off = 8; off < 0x200; off += 8) {
        if (!Readable(obj + off, 8)) break;
        const u64 p = At<u64>(obj + off);
        if (p < 0x10000 || !Readable(p, 4)) continue;
        std::string text;
        for (int i = 0; i < 48 && Readable(p + i, 1); ++i) {
            const char ch = At<char>(p + i);
            if (!ch) break;
            if (ch < 32 || ch > 126) {
                text.clear();
                break;
            }
            text += ch;
        }
        if (text.size() >= 4) {
            char item[96];
            std::snprintf(item, sizeof(item), "+%llx \"%s\"", static_cast<unsigned long long>(off), text.c_str());
            return item;
        }
    }
    return "-";
}

HLE_SYSV_ABI u64 KillObjectHook(u64 obj, u64 reason, u64 killer, u64 a, u64 b, u64 c, u64 st0, u64 st1) {
    OnLocalKill(obj, killer);
    static const bool log_kills = std::getenv("LD_MP_EVENTS") != nullptr;
    if (!log_kills) return g_kill_object(obj, reason, killer, a, b, c, st0, st1);
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0));
    float pos[3] = {};
    // posicao: como nos personagens, objeto+0x60 = transform (matriz) com a posicao em +0x90 (se for legivel)
    const u64 t = Readable(obj + 0x60, 8) ? At<u64>(obj + 0x60) : 0;
    if (t && Readable(t + 0x90, 12)) std::memcpy(pos, reinterpret_cast<const void*>(t + 0x90), sizeof(pos));
    std::printf("[KILL] t=%.2f obj %llx vtable %llx motivo %llu por %llx pos %.3f %.3f %.3f nome %s chamado de eboot+%llx\n",
                runtime::mp_net::Now(), static_cast<unsigned long long>(obj),
                static_cast<unsigned long long>(Readable(obj, 8) ? At<u64>(obj) : 0), static_cast<unsigned long long>(reason & 0xffffffff),
                static_cast<unsigned long long>(killer), pos[0], pos[1], pos[2], FirstString(obj).c_str(),
                static_cast<unsigned long long>(caller - reinterpret_cast<u64>(g_base)));
    std::fflush(stdout);
    return g_kill_object(obj, reason, killer, a, b, c, st0, st1);
}

// ---- pesquisa de montar (LD_MP_EVENTS): sondas nas funcoes candidatas. Chamam a original primeiro (registros e
// xmm intactos) e depois registram o objeto (classe, primeiro texto) e quem chamou. Ate 4 por objeto, depois 1 a cada 100.
struct ProbeDef {
    const char* name;
    u64 offset;
};
constexpr ProbeDef kProbes[] = {
    {"som_peca_ou_fim(189c8e0)", 0x189c8e0}, // toca buildit_piece / buildit_end
    {"montar_inicio(18a73b0)", 0x18a73b0},   // usa buildit_start
    {"montar_pecas(189c9a0)", 0x189c9a0},    // usa buildit_piece / buildit_end
    {"obstaculo_play(1802f50)", 0x1802f50},  // PlayObstacle (motor antigo): anda para a frente
    {"peca_voa(189e8a0)", 0x189e8a0},        // (peca, habilidade, edx, xmm0 = dt): toca buildit_end no fim
};
constexpr int kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);
KillFn g_probe_orig[kProbeCount] = {}; // 8 argumentos: 6 em registradores + 2 na pilha (18a73b0 le [rbp+0x18])
std::mutex g_probe_mutex;
std::map<std::pair<int, u64>, u64> g_probe_count;
std::map<std::pair<int, u64>, u64> g_probe_last_ret;
u64 g_probe_lines = 0;

std::string Describe(u64 p) {
    if (p < 0x10000 || !Readable(p, 8)) return "-";
    char buf[160];
    const u64 vt = At<u64>(p);
    const u64 base = reinterpret_cast<u64>(g_base);
    std::snprintf(buf, sizeof(buf), "%llx(vt %s%llx %s)", static_cast<unsigned long long>(p), vt >= base && vt < base + 0x3400000 ? "eboot+" : "",
                  static_cast<unsigned long long>(vt >= base && vt < base + 0x3400000 ? vt - base : vt), FirstString(p).c_str());
    return buf;
}

template <int K>
HLE_SYSV_ABI u64 ProbeHook(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 st0, u64 st1) {
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0)) - reinterpret_cast<u64>(g_base);
    const u64 r = g_probe_orig[K](a, b, c, d, e, f, st0, st1);
    u64 n;
    bool changed;
    {
        std::lock_guard lock(g_probe_mutex);
        n = ++g_probe_count[{K, a}];
        auto [it, fresh] = g_probe_last_ret.try_emplace({K, a}, r);
        changed = fresh || it->second != r;
        it->second = r;
    }
    // inicio de montagem: todos os argumentos e o alvo ([[pilha1+8]+8]: referencia com contagem em +0x10)
    if (K == 1) {
        u64 target = 0;
        if (Readable(st1 + 8, 8)) {
            const u64 ref = At<u64>(st1 + 8);
            if (Readable(ref + 8, 8)) target = At<u64>(ref + 8);
        }
        std::printf("[MONTAR] t=%.2f INICIO habilidade %llx personagem %s | dl=%llx rcx=%llx tipo(r8)=%llx r9=%llx pilha0=%s pilha1=%s | ALVO=%s | hab+598=%s\n",
                    runtime::mp_net::Now(), static_cast<unsigned long long>(a), Describe(b).c_str(), static_cast<unsigned long long>(c),
                    static_cast<unsigned long long>(d), static_cast<unsigned long long>(e), static_cast<unsigned long long>(f),
                    Describe(st0).c_str(), Describe(st1).c_str(), Describe(target).c_str(),
                    Readable(a + 0x598, 8) ? Describe(At<u64>(a + 0x598)).c_str() : "-");
        std::fflush(stdout);
    }
    // pecas: so quando o retorno muda (o fim da montagem deve mudar o retorno), ate 3000 linhas
    if ((K == 2 || K == 4) && (!changed || g_probe_lines > 3000)) return r;
    ++g_probe_lines;
    if (K == 2 || K == 4 || n <= 4 || n % 100 == 0) {
        std::printf("[MONTAR] t=%.2f %s #%llu ret %llx de eboot+%llx | a=%s | b=%s | c=%s | [a+18]=%s\n", runtime::mp_net::Now(),
                    kProbes[K].name, static_cast<unsigned long long>(n), static_cast<unsigned long long>(r),
                    static_cast<unsigned long long>(caller), Describe(a).c_str(), Describe(b).c_str(), Describe(c).c_str(),
                    Readable(a + 0x18, 8) ? Describe(At<u64>(a + 0x18)).c_str() : "-");
        std::fflush(stdout);
    }
    return r;
}

template <int... K>
void InstallProbes(std::integer_sequence<int, K...>) {
    ((g_probe_orig[K] = reinterpret_cast<KillFn>(Hook(kProbes[K].offset, reinterpret_cast<const void*>(&ProbeHook<K>)))), ...);
    for (int i = 0; i < kProbeCount; ++i) {
        std::printf("[MONTAR] sonda %s: %s\n", kProbes[i].name, g_probe_orig[i] ? "ligada" : "NAO ligada");
    }
    std::fflush(stdout);
}

// a cada 2 s: os eventos que dispararam desde o ultimo relatorio (os de todo quadro aparecem com contagem alta)
void ReportEventsLocked(double now) {
    static double last = 0;
    if (now - last < 2.0) return;
    last = now;
    if (!g_enabled) g_region_count = g_region_next = 0; // sem o Tick: renova o mapa de memoria daqui
    std::string line;
    for (auto& [name, st] : g_events) {
        if (st.count == st.shown) continue;
        char item[160];
        std::snprintf(item, sizeof(item), " %s x%llu (de %llx, dados %llx);", name.c_str(),
                      static_cast<unsigned long long>(st.count - st.shown), static_cast<unsigned long long>(st.last_sender),
                      static_cast<unsigned long long>(st.last_payload));
        line += item;
        st.shown = st.count;
        st.window = 0;
    }
    if (!line.empty()) {
        std::printf("[EV]%s\n", line.c_str());
        std::fflush(stdout);
    }
}

// ---- Fase 80: o que um jogador faz no mundo dele acontece no dos outros ----------------------------------------
// O jogo so deixa jogadores de verdade quebrarem objetos: o fantasma so faz a animacao. Entao cada jogo avisa a sala
// quando o SEU jogador quebra algo ("x|b|area|nome|ordem") e os outros quebram o objeto de mesmo nome.
// Objetos explodiveis (arbustos, lixeiras, carrinhos...): contexto com vtable eboot+2c0d3e0, nome em +0xf0 (do
// arquivo do level, ex. "BlowUp_Bush_INST08"), +0x260 != 0 = ja explodiu. eboot+f28ce0(ctx, quem, 0, 1, 0, 0, 0, 0)
// explode (o jogo chama assim em eboot+f2afc5). Construtores (por ponteiro): eboot+f28410, f285a0, f28730.
u64 kBlowUpFn = 0xf28ce0;
constexpr u64 kBlowUpVtable = 0x2c0d3e0;
using BlowUpFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64, u64, u64);
BlowUpFn g_blowup = nullptr;
std::mutex g_gadget_mutex;
std::vector<u64> g_gadgets; // contextos vistos nascer (validados no uso)
bool g_applying = false;    // quebrando por um aviso: nao avisa de volta

// classes de objeto do level (todas com o nome em +0xf0): explodivel, objeto simples, gatilho, multiestado,
// obstaculo, pedra de dica, manivela, gancho, ponto de salvar, impulso de veiculo
// 4.2: todas as classes de gadget do eboot (vtable >70% igual a do multiestado): fixit, plataformas, escadas,
// empurraveis, transmat, montagens (reform 2cbeae0, 2cbfba0)...
constexpr u64 kGadgetVtables[] = {0x2c0d3e0, 0x2c13830, 0x2c16120, 0x2c0e3e0, 0x2c11930, 0x2c1ad70, 0x2c12de0,
                                  0x2c16f80, 0x2c19070, 0x2ca6c70, 0x2cbeae0, 0x2cbfba0, 0x2c0c430, 0x2c0ee40,
                                  0x2c0f8a0, 0x2c10520, 0x2c12390, 0x2c142a0, 0x2c15480, 0x2c92820, 0x2cacc30,
                                  0x2cafc30, 0x2cb2300, 0x2cb33e0, 0x2cbaa70, 0x2cc2180};
constexpr u64 kReformVtables[] = {0x2cbeae0, 0x2cbfba0};
constexpr u64 kObstacleVtable = 0x2c11930;
u64 kTriggerObstacleFn = 0xf46140;
// ---- 4.2: historia dentro da fase. Objetos multiestado (portoes, predios montados por etapas, escombros...) mudam de
// estado por eboot+f31ac0(objeto, estado, parametro, forcar, imediato); eboot+a0a770(objeto) = estado atual (-1 nenhum).
// Mudar o estado pelo mesmo caminho dispara a animacao e as mensagens do level (o roteiro continua sozinho).
u64 kMsRequestFn = 0xf31ac0;
// ---- 4.2: montar. eboot+13b97f0(nao usado, montagem, quem) dispara a montagem (estado + evento OnTriggered: as pecas
// se juntam e o roteiro do level recebe a mensagem). O estado fica no mesmo componente dos multiestados.
u64 kReformTriggerFn = 0x13b97f0;
// ---- 4.2: montinhos de pecas (motor antigo do jogo). Montagem: classe eboot+2b7ef50; +0x24c estado (0 parado, 1..3
// montando, 4 pronto), +0x210 pecas, +0xa8 posicao, +0x220 tipo/etapa, +0x598 quem monta, +0xba bits 0|1 = "sendo
// montado" (o jogo recalcula todo quadro; sem eles a atualizacao eboot+18a60b0(montagem, dt) para logo no comeco).
// eboot+18a73b0(montagem, quem, 0, 0, tipo, 1, primeira peca, eboot+313e1d0) comeca a montar sem conferir o botao.
constexpr u64 kBuildVtable = 0x2b7ef50;
u64 kBuildStartFn = 0x18a73b0;
u64 kBuildUpdateFn = 0x18a60b0;
constexpr u64 kBuildInputEvent = 0x313e1d0;
u64 kMsCurrentFn = 0xa0a770;
constexpr u64 kMultiStateVtable = 0x2c0e3e0;
// ---- campanha junto (4.1.1): a fase vem do objeto da fase atual; GotoLevel e o que o portal/portas chamam ----
constexpr u64 kGameGlobal = 0x3130890;   // jogo (eboot+cd4910 devolve): +0x120 +0x70 +0xdf8 +0x18 = fase atual
constexpr u64 kLevelLoader = 0x3174668;  // LevelLoader global (rdi do GotoLevel)
u64 kGotoLevelFn = 0x1a61330;            // (LevelLoader, numero da fase, 0, opcoes|0): pede a troca de fase
u64 kLevelByIdFn = 0x1985250;            // (numero) -> objeto da fase (+0x48 caminho do pacote, +0x80 numero)
u64 kLevelValidFn = 0x1985bc0;           // (numero) -> existe? // (nao usado, obstaculo, quem acionou): alavancas, coisas de usar

bool IsGadgetVtable(u64 vt) {
    for (u64 v : kGadgetVtables) {
        if (vt == reinterpret_cast<u64>(g_base) + v) return true;
    }
    return false;
}

bool ValidGadget(u64 ctx) {
    return ctx && Readable(ctx, 0x290) && IsGadgetVtable(At<u64>(ctx));
}

std::string GadgetName(u64 ctx) {
    const u64 p = At<u64>(ctx + 0xf0);
    if (!p || !Readable(p, 1)) return {};
    std::string s;
    for (int i = 0; i < 63 && Readable(p + i, 1); ++i) {
        const char ch = At<char>(p + i);
        if (!ch) break;
        if (ch <= ' ' || ch == '|' || ch > '~') return {};
        s += ch;
    }
    return s;
}

// Os gadgets nao passam por um construtor que de para interceptar (vem prontos do pacote do level): uma varredura da
// memoria do jogo (0x1000000000..0x1100000000) em segundo plano acha os contextos pela vtable. ReadProcessMemory no
// proprio processo: uma pagina liberada no meio do caminho so falha a leitura (nao derruba o jogo).
std::atomic<bool> g_gadget_scan_busy{false};
double g_gadget_scan_time = -100;

constexpr u64 kCharacterVtable = 0x2bdf860; // personagens (jogadores, capangas, Doc Brown...): nome em +0x10
std::vector<u64> g_characters; // da ultima varredura (g_gadget_mutex)
std::vector<u64> g_builds;     // montinhos de pecas (g_gadget_mutex)

void ScanGadgets() {
    const u64 want_char = reinterpret_cast<u64>(g_base) + kCharacterVtable;
    std::vector<u64> found, chars, builds_found;
    std::vector<u64> buf(1 << 17); // 1 MB
    HANDLE self = GetCurrentProcess();
    for (u64 addr = 0x1000000000ull; addr < 0x1100000000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
        const u64 base = reinterpret_cast<u64>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD)) {
            for (u64 p = base; p < end; p += buf.size() * 8) {
                const SIZE_T want_bytes = static_cast<SIZE_T>(std::min<u64>(buf.size() * 8, end - p));
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, reinterpret_cast<void*>(p), buf.data(), want_bytes, &got)) continue;
                for (SIZE_T i = 0; i < got / 8; ++i) {
                    if (buf[i] == want_char) chars.push_back(p + i * 8);
                    else if (buf[i] == reinterpret_cast<u64>(g_base) + kBuildVtable) builds_found.push_back(p + i * 8);
                    else if ((buf[i] >> 24) == ((reinterpret_cast<u64>(g_base) + 0x2c00000) >> 24) && IsGadgetVtable(buf[i])) {
                        found.push_back(p + i * 8);
                    }
                }
            }
        }
        addr = end;
    }
    std::lock_guard lock(g_gadget_mutex);
    g_gadgets.swap(found);
    g_characters.swap(chars);
    g_builds.swap(builds_found);
}

void StartGadgetScan(double now) {
    if (g_gadget_scan_busy.exchange(true)) return;
    g_gadget_scan_time = now;
    std::thread([] {
        ScanGadgets();
        g_gadget_scan_busy = false;
    }).detach();
}

// os gadgets vivos com este nome, em ordem de endereco (o "ordem" do aviso desempata nomes repetidos)
std::vector<u64> GadgetsNamed(const std::string& name) {
    std::vector<u64> out;
    std::lock_guard lock(g_gadget_mutex);
    for (u64 g : g_gadgets) {
        if (ValidGadget(g) && GadgetName(g) == name && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool IsLocalPlayer(u64 inst) {
    if (!inst) return false;
    for (int k = 0; k < 2; ++k) {
        if (At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable + 8 * k) == inst) return true;
    }
    return false;
}

std::string CurrentArea() {
    std::lock_guard lock(g_area_mutex);
    return g_area;
}

// ---- 4.5: historia/objetivo x minigame local -------------------------------------------------------------------
// O roteiro do level progride por objetos de estado (gatilho de objetivo, NPC, marco da missao). Esses avisos vao para
// a sala como "q|area|nome#ordem|estado" e sao aplicados no outro jogo SEM abrir dialogo/camera/menu (g_quest_silent).
// O que e claramente uma interacao local (roda de keystone, terminal de hack, minigame) nao sai daqui e tambem nao e
// aplicado quando chega: a janela do minigame de um jogador nunca aparece na tela do outro.
bool IsLocalOnlyGadget(const std::string& name) {
    static const char* kLocal[] = {"Keystone", "Hack", "Terminal", "Minigame", "MiniGame", "Arcade", "WheelUI"};
    for (const char* local : kLocal) {
        if (name.find(local) != std::string::npos) return true;
    }
    return false;
}

bool IsQuestGadget(const std::string& name) {
    static const char* kQuest[] = {"Quest", "Objective", "Mission", "Story", "Goal", "Dialog", "Dialogue", "Npc", "NPC"};
    for (const char* quest : kQuest) {
        if (name.find(quest) != std::string::npos) return true;
    }
    return false;
}

HLE_SYSV_ABI u64 BlowUpHook(u64 ctx, u64 who, u64 a, u64 b, u64 c, u64 d, u64 st0, u64 st1) {
    if (!g_applying && g_enabled && ValidGadget(ctx) && At<u64>(ctx) == reinterpret_cast<u64>(g_base) + kBlowUpVtable &&
        !At<u8>(ctx + 0x260) && IsLocalPlayer(who)) {
        const std::string name = GadgetName(ctx);
        if (!name.empty()) {
            const std::vector<u64> same = GadgetsNamed(name);
            const auto it = std::find(same.begin(), same.end(), ctx);
            const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
            static std::map<u64, double> announced; // 4.4.7: o gancho roda varias vezes no mesmo objeto
            const double now = runtime::mp_net::Now();
            auto seen = announced.find(ctx);
            if (seen == announced.end() || now - seen->second > 3.0) {
                announced[ctx] = now;
                Announce("b|" + CurrentArea() + "|" + name + "|" + std::to_string(order));
                std::printf("[MP] quebrei %s (#%d): avisando a sala\n", name.c_str(), order);
                std::fflush(stdout);
            }
        }
    }
    return g_blowup(ctx, who, a, b, c, d, st0, st1);
}

SetupFn g_trigger_obstacle = nullptr;

struct MsChange {
    u64 obj;
    int state, param;
};
std::mutex g_ms_mutex;
std::vector<MsChange> g_ms_changes; // mudancas de estado deste jogo (nao as que nos aplicamos), para o Tick avisar
SetupFn g_ms_request = nullptr;

// ---- montinhos de pecas ----
using BuildUpdateFn = u64(HLE_SYSV_ABI*)(u64, float);
BuildUpdateFn g_build_update = nullptr;
std::mutex g_build_mutex;
std::map<u64, double> g_forced_builds; // montagens que um aviso mandou montar aqui -> quando comecou

bool IsBuild(u64 b) { return b && Readable(b, 0x600) && At<u64>(b) == reinterpret_cast<u64>(g_base) + kBuildVtable; }

HLE_SYSV_ABI u64 BuildUpdateHook(u64 build, float dt) {
    bool forced = false;
    {
        std::lock_guard lock(g_build_mutex);
        forced = g_forced_builds.count(build) != 0;
    }
    if (forced && IsBuild(build) && At<s32>(build + 0x24c) != 4) {
        At<u16>(build + 0xba) |= 3; // "sendo montado": a atualizacao do proprio jogo anda o relogio e as pecas
        At<u64>(build + 0x5e0) = 0; // sem a pergunta ao controle de quem monta (o padrao e "segurando")
        At<s32>(build + 0x1d8) = 0; // pendencias (pecas que saem de objetos quebrados): o outro jogador ja resolveu
        static const bool dbg = std::getenv("LD_MP_BUILDDBG") != nullptr;
        if (dbg) {
            static int shown = 0;
            const u64 vfn = At<u64>(At<u64>(build) + 0x718);
            const u16 ba = At<u16>(build + 0xba);
            const u64 r = g_build_update(build, dt);
            if (shown++ < 60) {
                std::printf("[MONTAR-DBG] %llx dt %.3f ba antes %x depois %x st %d clock %.3f v718 %llx -> %d 218 %llx 210 %d builder %llx\n",
                            static_cast<unsigned long long>(build), dt, ba, At<u16>(build + 0xba), At<s32>(build + 0x24c),
                            At<float>(build + 0x390), static_cast<unsigned long long>(vfn - reinterpret_cast<u64>(g_base)),
                            static_cast<int>(reinterpret_cast<u64(HLE_SYSV_ABI*)(u64)>(vfn)(build) & 0xff),
                            static_cast<unsigned long long>(At<u64>(build + 0x218)), At<s32>(build + 0x210),
                            static_cast<unsigned long long>(At<u64>(build + 0x598)));
                std::fflush(stdout);
            }
            return r;
        }
    }
    return g_build_update(build, dt);
}

std::string BuildKey(u64 b) {
    char key[96];
    std::snprintf(key, sizeof(key), "%.1f,%.1f,%.1f,%d", At<float>(b + 0xa8), At<float>(b + 0xac), At<float>(b + 0xb0),
                  At<s32>(b + 0x210));
    return key;
}

u64 FindBuild(const std::string& key) { // a mais perto da posicao, com o mesmo numero de pecas
    float x = 0, y = 0, z = 0;
    int n = 0;
    if (std::sscanf(key.c_str(), "%f,%f,%f,%d", &x, &y, &z, &n) != 4) return 0;
    u64 best = 0;
    float best_d = 0.6f;
    std::lock_guard lock(g_gadget_mutex);
    for (u64 b : g_builds) {
        if (!IsBuild(b) || At<s32>(b + 0x210) != n) continue;
        const float d = std::hypot(At<float>(b + 0xa8) - x, std::hypot(At<float>(b + 0xac) - y, At<float>(b + 0xb0) - z));
        if (d < best_d) {
            best_d = d;
            best = b;
        }
    }
    return best;
}

int GadgetState(u64 obj) { // = eboot+a0a770
    const u64 c = Readable(obj + 0x120, 8) ? At<u64>(obj + 0x120) : 0;
    return c && Readable(c + 0xb9, 1) ? static_cast<int>(At<std::int8_t>(c + 0xb9)) : -1;
}

bool IsReform(u64 obj) {
    if (!Readable(obj, 0x300)) return false;
    for (u64 v : kReformVtables) {
        if (At<u64>(obj) == reinterpret_cast<u64>(g_base) + v) return true;
    }
    return false;
}

struct ReformChange {
    u64 obj;
    int before;
};
std::vector<ReformChange> g_reform_changes; // montagens deste jogo (g_ms_mutex)
SetupFn g_reform_trigger = nullptr;

HLE_SYSV_ABI u64 ReformTriggerHook(u64 unused, u64 obj, u64 who, u64 a, u64 b, u64 c) {
    if (Readable(obj, 0x300)) {
        std::printf("[MP] montagem: objeto %llx classe eboot+%llx nome %s estado %d%s\n", static_cast<unsigned long long>(obj),
                    static_cast<unsigned long long>(At<u64>(obj) - reinterpret_cast<u64>(g_base)), GadgetName(obj).c_str(),
                    GadgetState(obj), g_applying ? " (aviso de outro jogador)" : "");
        std::fflush(stdout);
    }
    if (!g_applying && g_enabled && Readable(obj, 0x300) && !GadgetName(obj).empty()) {
        std::lock_guard lock(g_ms_mutex);
        if (g_reform_changes.size() < 128) g_reform_changes.push_back({obj, GadgetState(obj)});
    }
    return g_reform_trigger(unused, obj, who, a, b, c);
}

HLE_SYSV_ABI u64 MsRequestHook(u64 obj, u64 state, u64 param, u64 force, u64 immediate, u64 f) {
    static const bool log_ms = std::getenv("LD_MP_EVENTS") != nullptr;
    if (log_ms && Readable(obj, 0x460)) {
        std::printf("[MONTAR] t=%.2f multiestado %s (eboot+%llx) estado %d -> %d param %d de eboot+%llx\n", runtime::mp_net::Now(),
                    GadgetName(obj).c_str(), static_cast<unsigned long long>(At<u64>(obj) - reinterpret_cast<u64>(g_base)),
                    GadgetState(obj), static_cast<int>(state), static_cast<int>(param),
                    static_cast<unsigned long long>(reinterpret_cast<u64>(__builtin_return_address(0)) - reinterpret_cast<u64>(g_base)));
        std::fflush(stdout);
    }
    if (!g_applying && g_enabled && Readable(obj, 0x460) && At<u64>(obj) == reinterpret_cast<u64>(g_base) + kMultiStateVtable) {
        std::lock_guard lock(g_ms_mutex);
        if (g_ms_changes.size() < 256) g_ms_changes.push_back({obj, static_cast<int>(state), static_cast<int>(param)});
    }
    return g_ms_request(obj, state, param, force, immediate, f);
}

HLE_SYSV_ABI u64 TriggerObstacleHook(u64 unused, u64 ctx, u64 who, u64 a, u64 b, u64 c) {
    if (!g_applying && g_enabled && ValidGadget(ctx) && IsLocalPlayer(who)) {
        const std::string name = GadgetName(ctx);
        if (!name.empty()) {
            const std::vector<u64> same = GadgetsNamed(name);
            const auto it = std::find(same.begin(), same.end(), ctx);
            const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
            runtime::mp_net::SendEvent("o|" + CurrentArea() + "|" + name + "|" + std::to_string(order));
            std::printf("[MP] acionei %s (#%d): avisando a sala\n", name.c_str(), order);
            std::fflush(stdout);
        }
    }
    return g_trigger_obstacle(unused, ctx, who, a, b, c);
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
    if (!portal || !player) return 0; // antes do preparo da fase nao ha lista de slots
    return i < g_nslots ? portal + i * kSlotStep : player + (i - g_nslots) * kSlotStep;
}

void UpdateSlotCounts() {
    g_nslots = std::clamp(At<s32>(reinterpret_cast<u64>(g_base) + kPortalSlots + 8), 0, 48);
    g_nall = g_nslots + std::clamp(At<s32>(reinterpret_cast<u64>(g_base) + kPlayerSlots + 8), 0, 4);
}
int SlotState(int i) {
    const u64 slot = SlotAddress(i);
    return slot && Readable(slot + 0x2c8, 4) ? At<s32>(slot + 0x2c8) : -1;
}
std::string SlotName(int i) {
    const u64 slot = SlotAddress(i);
    if (!slot || !Readable(slot + 0xb8, 48)) return {};
    const char* name = reinterpret_cast<const char*>(slot + 0xb8);
    return std::string(name, strnlen(name, 48));
}

std::map<std::string, int> g_wrong_count; // personagem -> vezes que o fantasma apareceu com outro
std::set<std::string> g_unsupported;       // personagens que nao aparecem certos: so o nome
std::map<u64, std::string> g_our_slots; // slot -> personagem que NOS carregamos nele
bool OursFamily(const std::string& slot_name);

// Um slot que o jogo carregou como VARIANTE de um fantasma (o proximo Doctor, o Homer bravo...) pertence ao fantasma
// que o pediu: e solto quando ele some. Fantasma nenhum nasce dele (nascer de la e depois perder o slot derrubava a
// thread de fundo). Servem: os slots que nos carregamos e os das figuras do proprio jogador.
std::string LocalCharacter(u64 inst);
bool LocalPlayerUses(const std::string& slot_name);

// 4.5: personagem repetido (P1 e P2 escolhem o mesmo, ex. o Batman). O slot da figura do jogador local NAO serve para
// o fantasma: o motor liga o pedido ao mesmo personagem da party (inst+0x3a0), um dos dois some e a troca de figura
// derruba o jogo. Se for o mesmo personagem que o jogador local usa, esta funcao devolve 0 e o Tick carrega o
// personagem num "Player Load Slot" proprio (duas instancias isoladas).
u64 FindLoadedSlot(const std::string& chr) {
    const std::string want = Lower(chr) + ".cd";
    u64 ours = 0;
    for (int i = 0; i < g_nall; ++i) {
        if (SlotState(i) != 1) continue;
        const std::string name = SlotName(i);
        if (name != want) continue;
        const u64 slot = SlotAddress(i);
        if (!slot) continue;
        const auto it = g_our_slots.find(slot);
        if (it != g_our_slots.end()) { // nosso, deste personagem: instancia separada, serve
            if (it->second + ".cd" == want && !ours) ours = slot;
            continue;
        }
        if (OursFamily(want)) continue;        // variante de um personagem nosso: pertence a outro fantasma
        if (LocalPlayerUses(name)) continue;   // figura do jogador local: nunca compartilha a party com um fantasma
        return slot;                           // figura do jogador, livre
    }
    return ours;
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
        const u64 slot = SlotAddress(i);
        if (!slot || !Readable(slot + 0x58, 8) || !Readable(slot + 0x2d4, 4)) continue;
        // com memoria propria (os 2 ultimos usam a memoria geral e nao carregam: +0x2d4 = 1)
        if (SlotState(i) == 0 && At<u32>(slot + 0x58) != 0 && !At<u8>(slot + 0x2d4)) return true;
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

std::string LocalCharacter(u64 inst); // (declarado acima, antes de FindLoadedSlot)
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
    if (!slot || !Readable(slot + 0x88 + 0x18, 8)) return false; // sem endereco de slot: nada a descarregar
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

// 4.1.1: os slots nao dao para 16 personagens diferentes (o jogo tem memoria para 8). Quem fica sem slot aparece com
// um personagem que ja esta carregado: primeiro um que NOS carregamos (nunca e descarregado com fantasma em cima), senao
// o do proprio jogador. O nome em cima continua sendo o dele; quando sobra um slot, vira o personagem certo.
u64 StandInSlot() {
    for (int i = 0; i < g_nall; ++i) {
        const u64 slot = SlotAddress(i);
        if (!slot) continue;
        const auto it = g_our_slots.find(slot);
        if (SlotState(i) == 1 && it != g_our_slots.end() && SlotName(i) == it->second + ".cd") return slot;
    }
    // 4.5: antes o substituto caia no slot do personagem do jogador local - o mesmo problema da party compartilhada
    // (Task 6). Sem um personagem NOSSO livre, so o nome flutuando aparece ate sobrar um slot.
    return 0;
}

// O slot (endereco) esta carregado com o personagem que o jogador 1/2 deste PC esta usando agora?
bool SlotUsedByLocalPlayer(u64 slot) {
    if (!slot) return false;
    for (int i = 0; i < g_nall; ++i) {
        if (SlotAddress(i) != slot) continue;
        const std::string name = SlotName(i);
        return !name.empty() && LocalPlayerUses(name);
    }
    return false;
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
    // Fases de historia: o jogo le sub-areas depois que a figura nasce (a epoca muda) e nenhuma figura nasce de novo.
    // Vale um spawner de antes se o personagem do pedido dele e o jogador 1 ou 2 vivo agora (entao o spawner existe).
    for (int k = 0; k < 2; ++k) {
        const u64 player = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable + 8 * k);
        if (!player) continue;
        for (auto it = g_spawners.rbegin(); it != g_spawners.rend(); ++it) {
            if (!ValidSpawner(it->spawner)) continue;
            const u64 r = TemplateRequest(it->spawner);
            const u64 comp = r && Readable(r + 0x40, 8) ? At<u64>(r + 0x40) : 0;
            if (comp && Readable(comp + 0x20, 8) && At<u64>(comp + 0x20) == player) {
                it->epoch = epoch; // vale nesta area
                *spawner = it->spawner;
                *request = r;
                return true;
            }
        }
    }
    // 4.1.1: o jogador trocou ou tirou a figura (o pedido nao aponta mais para ele) e a area foi relida: vale o spawner
    // mais recente visto nesta mesma area que ainda tenha um pedido modelo
    std::string area;
    {
        std::lock_guard area_lock(g_area_mutex);
        area = g_area;
    }
    for (auto it = g_spawners.rbegin(); it != g_spawners.rend(); ++it) {
        if (area.empty() || it->area != area || !ValidSpawner(it->spawner)) continue;
        if (u64 r = TemplateRequest(it->spawner)) {
            it->epoch = epoch;
            *spawner = it->spawner;
            *request = r;
            return true;
        }
    }
    return false;
}

const char* SlotLabel(u64 slot) {
    return slot && Readable(slot + 0xb8, 48) ? reinterpret_cast<const char*>(slot + 0xb8) : "-";
}

bool Spawn(Puppet& p) {
    // 4.5: nunca prende o pedido ao slot do personagem do jogador 1/2 (a party seria a mesma: um some e a troca cai)
    if (SlotUsedByLocalPlayer(p.slot)) {
        Log("[MP] %s: o slot de %s e do jogador local, carrego um slot de jogador proprio", p.cur.chr, p.name);
        p.slot = 0;
        p.stage = Stage::kNone;
        p.load_fail_time = 0;
        p.retry_time = runtime::mp_net::Now() + 1.0;
        return false;
    }
    u64 spawner = 0, model = 0;
    if (!PickSpawner(&spawner, &model)) {
        static double last_note = -100;
        if (runtime::mp_net::Now() - last_note > 10.0) {
            last_note = runtime::mp_net::Now();
            std::printf("[MP] sem spawner para o fantasma (%zu vistos)\n", g_spawners.size());
            std::fflush(stdout);
        }
        return false;
    }
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
    std::printf("[MP] spawn: spawner %llx (slot %s) com slot %s%s\n", static_cast<unsigned long long>(spawner),
                SlotLabel(original_slot), SlotLabel(p.slot), p.standin ? " (substituto)" : "");
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
            // 4.4.7: primeiro o destrutor do jogo desfaz o personagem, depois solto o no da lista. Na ordem inversa o
            // destrutor mexia num no ja liberado e o jogo caia depois ("call [rax+0x88]" com rax=0).
            Call(kRequestDeleteFn, request); // passa pelo gancho do destrutor (ja zerado acima)
            GameFree(node);
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
    r->level = f.size() > 13 ? f[13] : std::string();
    r->region = f.size() > 14 ? f[14] : std::string();
    r->party.clear();
    if (f.size() > 15) { // 4.5.1: "nome;nome;nome" (os outros personagens equipados dele)
        std::stringstream party(f[15]);
        std::string one;
        while (std::getline(party, one, ';') && r->party.size() < 6) {
            if (!one.empty() && one != "-" && one.size() < 48) r->party.push_back(one);
        }
    }
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
    // 4.4.7: a direcao do fantasma vem do DESLOCAMENTO real entre dois pacotes. O vetor que o jogo guarda em
    // loc+0x80/0x88 e local ao corpo (x = lado, z = frente): interpretado como mundo, o fantasma andava de lado
    // sempre que o outro jogador girava.
    float mx = 0.0f, mz = 0.0f;
    if (p.prev.valid && p.cur_time > p.prev_time + 0.001) {
        mx = r.x - p.prev.x;
        mz = r.z - p.prev.z;
        p.move_dx = mx;
        p.move_dz = mz;
        const float len = std::sqrt(mx * mx + mz * mz);
        if (len > 0.004f) { // andou o bastante para saber para onde (a posicao vem exata, sem ruido)
            mx /= len;
            mz /= len;
        } else {
            mx = mz = 0.0f;
        }
    }
    float vx = 0, vz = 0;
    if (r.gear > 3 && (mx != 0.0f || mz != 0.0f)) {
        const float speed = TableSpeed(p.inst, r.gear);
        if (speed > 0.0f) {
            vx = mx * speed;
            vz = mz * speed;
        }
    }
    const double latency = runtime::mp_net::LatencySeconds(p.peer, r.rtt_ms); // Fase 82: direto (P2P) ou pelo servidor
    // 4.4.7: mostra o outro onde ele REALMENTE estava quando o pacote saiu (agora - metade da ida e volta) e caminha
    // entre o pacote anterior e o atual. Antes era so "pacote + velocidade x atraso": a posicao pulava a cada pacote.
    const double shown = now - latency;
    float tx = r.x, ty = r.y, tz = r.z;
    if (p.prev.valid && p.cur_time > p.prev_time + 0.001) {
        const double span = p.cur_time - p.prev_time;
        double a = (shown - p.prev_time) / span;
        a = std::clamp(a, 0.0, 1.0 + 0.35); // no maximo um terco de pacote para a frente
        tx = static_cast<float>(p.prev.x + (r.x - p.prev.x) * a);
        ty = static_cast<float>(p.prev.y + (r.y - p.prev.y) * a);
        tz = static_cast<float>(p.prev.z + (r.z - p.prev.z) * a);
    } else {
        const float lead = static_cast<float>(std::clamp(age + latency, 0.0, 0.5));
        tx += vx * lead;
        tz += vz * lead;
    }
    const u64 t = Transform(p.inst);
    if (!t || !Readable(t + 0x90, 12)) { // sem matriz de posicao: nada a dirigir neste quadro (nada velho no Drive)
        d->inst = 0;
        d->gear = 3;
        d->speed = 0.0f;
        d->dx = d->dz = 0.0f;
        d->fx = d->fz = 0.0f;
        d->held = d->prev = 0;
        return;
    }
    const float px = At<float>(t + 0x90), py = At<float>(t + 0x94), pz = At<float>(t + 0x98);
    // 4.4.8: o fantasma andou para onde foi mandado? Se andou de lado, a convencao esta trocada (calibra sozinho).
    if (p.have_last) {
        const float mvx = px - p.last_px, mvz = pz - p.last_pz;
        const float mlen = std::sqrt(mvx * mvx + mvz * mvz);
        const float wlen = std::sqrt(p.last_wx * p.last_wx + p.last_wz * p.last_wz);
        if (mlen > 0.01f && wlen > 0.001f && p.cur.gear > 3 && age < 1.0 && p.last_dist < 0.5f) {
            const float dot = (mvx * p.last_wx + mvz * p.last_wz) / (mlen * wlen);
            const float sum = g_move_probe_dot.load(std::memory_order_relaxed) + dot;
            g_move_probe_dot.store(sum, std::memory_order_relaxed);
            const int cnt = g_move_probe_n.load(std::memory_order_relaxed) + 1;
            g_move_probe_n.store(cnt, std::memory_order_relaxed);
            if (cnt >= 20) {
                const float mean = sum / static_cast<float>(cnt);
                g_move_probe_n.store(0, std::memory_order_relaxed);
                g_move_probe_dot.store(0.0f, std::memory_order_relaxed);
                const bool flip = mean < 0.35f && std::getenv("LD_MP_MOVE") == nullptr; // anda de lado: troca
                if (flip) g_move_local.store(!g_move_local.load(std::memory_order_relaxed), std::memory_order_relaxed);
                std::printf("[MP] direcao do fantasma: %s%s (alinha %.2f)\n",
                            flip ? "convencao trocada para " : "",
                            g_move_local.load() ? "espaco do corpo" : "espaco do mundo", static_cast<double>(mean));
                std::fflush(stdout);
            }
        }
    }
    const float ex = tx - px, ez = tz - pz;
    const float dist = std::sqrt(ex * ex + ez * ez);
    // 4.5.1: teleporte so quando o fantasma esta realmente longe (entrou agora, trocou de area, caiu). Antes ele
    // pulava a cada 4 m e o outro parecia estar com a conexao ruim.
    if (dist > 20.0f || std::fabs(ty - py) > 8.0f) {
        Teleport(p.inst, tx, ty, tz);
    } else if (dist > 1.2f) {
        // 4.5.1: a posicao de quem joga manda: corrige a deriva aos poucos (sem pulo) - antes o fantasma chegava a
        // ficar 2-3 m atras e os golpes do outro nao encostavam em ninguem.
        const float blend = dist > 6.0f ? 0.5f : (dist > 2.0f ? 0.3f : 0.15f);
        Teleport(p.inst, px + ex * blend, py + (ty - py) * blend, pz + ez * blend);
    }
    d->inst = p.inst;
    d->speed = 0.0f;
    d->held = age < 1.0 ? (r.held & kSentButtons) : 0;
    // 4.5: rumo do corpo x direcao da caminhada (coisas separadas). A cara vem SEMPRE do outro jogo (r.fx, r.fz =
    // inst+0x170 do dono). Antes a correcao de posicao era escrita tambem na cara e o fantasma andava de lado/girava
    // a cada correcao da deriva lateral.
    {
        const float fx = r.fx, fz = r.fz;
        const float n = std::sqrt(fx * fx + fz * fz);
        if (!std::isfinite(n) || n < 0.01f) { // versao antiga (sem rumo) ou estado zerado: a cara segue o movimento
            const float mx = r.gear > 3 ? r.dx : 1.0f, mz = r.gear > 3 ? r.dz : 0.0f;
            const float m = std::sqrt(mx * mx + mz * mz);
            d->fx = m > 0.001f ? mx / m : 0.0f;
            d->fz = m > 0.001f ? mz / m : 1.0f;
        } else {
            d->fx = fx / n;
            d->fz = fz / n;
        }
    }
    d->dx = 0.0f; // parado: sem velocidade (a velocidade so vale para marcha > 3)
    d->dz = 0.0f;
    const bool stale = age > 3.0;
    // 4.4.8: o gancho de controle espera a direcao no espaco DO CORPO (x = lado, z = frente): o jogo gira o corpo
    // pela cara e anda para frente. Mandando o vetor do mundo, o boneco andava de lado quando o outro virava.
    static const bool move_env = [] { // LD_MP_MOVE=world|local fixa a convencao (teste)
        const char* v = std::getenv("LD_MP_MOVE");
        if (!v) return false;
        g_move_local = std::strcmp(v, "world") != 0;
        return true;
    }();
    (void)move_env;
    const auto to_body = [&](float& dx, float& dz) {
        if (!g_move_local.load(std::memory_order_relaxed)) return;
        const float fx = d->fx, fz = d->fz;    // cara ja normalizada
        const float forward = dx * fx + dz * fz; // quanto do movimento e' para frente
        const float side = dx * fz - dz * fx;    // e quanto e' para o lado
        dx = side;
        dz = forward;
    };
    if (!stale && r.gear > 3 && (mx != 0.0f || mz != 0.0f)) {
        const float k = 0.8f * std::min(dist, 1.5f);
        float dx = mx + (dist > 0.001f ? ex / dist * k : 0), dz = mz + (dist > 0.001f ? ez / dist * k : 0);
        const float n = std::sqrt(dx * dx + dz * dz);
        if (n > 0.001f) {
            dx /= n;
            dz /= n;
        }
        d->gear = r.gear;
        // atras do ponto (na direcao do movimento): um pouco mais rapido ate encostar; na frente: um pouco mais devagar
        const float base = TableSpeed(p.inst, r.gear);
        if (base > 0.0f) {
            const float along = ex * dx + ez * dz;
            // 4.5.1: alcanca o amigo andando, nao pulando: ate 3x a velocidade dele quando o erro e grande
            const float boost = dist > 3.0f ? 3.0f : 1.9f;
            d->speed = std::clamp(base + 3.0f * along, base * 0.5f, base * boost);
        }
        to_body(dx, dz); // mundo -> corpo (por ultimo: vale para a direcao e para a correcao juntas)
        d->dx = dx;      // loc+0x80: so a velocidade
        d->dz = dz;
    } else if (r.gear > 3 && mx == 0.0f && mz == 0.0f && dist < 0.6f) {
        d->gear = 3; // girando no lugar: nao anda, so a cara (r.fx/r.fz) muda
    } else if (dist > 0.12f) { // parado la e o fantasma longe: vai ate o lugar (devagar quando perto)
        d->gear = 6;
        const float base = TableSpeed(p.inst, 6);
        if (base > 0.0f) d->speed = std::clamp(dist * 4.0f, 0.3f, base * (dist > 3.0f ? 3.0f : 1.0f));
        float dx = ex / dist, dz = ez / dist;
        to_body(dx, dz);
        d->dx = dx;
        d->dz = dz;
    } else {
        d->gear = 3;
    }
    p.last_px = px;
    p.last_pz = pz;
    p.last_dist = dist;
    p.have_last = true;
    { // direcao no mundo equivalente ao que foi mandado (para a sonda do proximo quadro)
        float wx = d->dx, wz = d->dz;
        if (g_move_local.load(std::memory_order_relaxed)) {
            const float fx = d->fx, fz = d->fz; // corpo -> mundo: frente na cara, lado na perpendicular
            wx = d->dz * fx + d->dx * fz;
            wz = d->dz * fz - d->dx * fx;
        }
        p.last_wx = wx;
        p.last_wz = wz;
    }
    // preso (outro personagem, parede): em vez de teleportar (aquilo parecia lag), desvia de lado ate passar
    if (std::hypot(px - p.still_x, pz - p.still_z) > 0.05f || d->gear <= 3 || dist < 0.6f) {
        p.still_x = px;
        p.still_z = pz;
        p.still_time = now;
        p.stuck_side = 0;
    } else if (now - p.still_time > 0.8 && dist > 0.6f) {
        // gira a direcao em ~65 graus para um lado (troca de lado a cada 1.2 s) e segue tentando
        static const float kCos = 0.42f, kSin = 0.91f;
        const float side = p.stuck_side % 2 ? 1.0f : -1.0f;
        const float sx = d->dx * kCos - d->dz * kSin * side, sz = d->dx * kSin * side + d->dz * kCos;
        const float n = std::sqrt(sx * sx + sz * sz);
        if (n > 0.001f) {
            d->dx = sx / n;
            d->dz = sz / n;
            d->gear = std::max(d->gear, 6);
            const float base = TableSpeed(p.inst, d->gear);
            if (base > 0.0f) d->speed = base;
        }
        if (now - p.still_time > 2.0) {
            p.still_time = now;
            ++p.stuck_side;
        }
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

// ---- campanha junto ------------------------------------------------------------------------------------------
u64 CurrentLevelObj() {
    u64 p = At<u64>(reinterpret_cast<u64>(g_base) + kGameGlobal);
    for (u64 off : {0x120ull, 0x70ull, 0xdf8ull, 0x18ull}) {
        if (!p || !Readable(p + off, 8)) return 0;
        p = At<u64>(p + off);
    }
    return p && Readable(p, 0x88) ? p : 0;
}

std::string LevelPath(u64 obj) {
    const u64 s = obj && Readable(obj + 0x48, 8) ? At<u64>(obj + 0x48) : 0;
    if (!s || !Readable(s, 1)) return {};
    std::string out;
    for (int i = 0; i < 160 && Readable(s + i, 1); ++i) {
        const char c = At<char>(s + i);
        if (!c) break;
        if (c < 32 || c > 126 || c == '|') return {};
        out += c;
    }
    return out;
}

// caminho do pacote -> numeros das fases com ele, em ordem (o numero muda com as DLCs; a ordem dentro do pacote nao)
std::map<std::string, std::vector<int>> g_level_index;
double g_level_index_time = -100;

// ---- 4.5: mundo do dono da sala x mundo liberado no save do convidado -----------------------------------------
// O convidado pode nao ter no savedata0 dele o mundo/DLC que o dono esta jogando (os gold bricks dele nao abrem o
// mundo). O que conta e o pacote estar instalado: com DLC no addcont a fase do dono fica autorizada - so ela, e so
// durante a sessao da sala. Sem DLC instalado o jogo responde como sempre (a fase nao existe).
bool DlcContentInstalled() {
    static int cached = -1; // -1 ainda nao perguntou, 0 sem DLC, 1 com DLC
    if (cached >= 0) return cached == 1;
    cached = 0;
    const std::filesystem::path game = hle::fs::App0();
    const char* appdata = std::getenv("APPDATA");
    if (!game.empty() && game_region::HasAddcontContent(game, appdata ? appdata : "")) {
        cached = 1; // a pasta de DLC do shadPS4 (ou <jogo>/assets/addcont)
    } else if (const std::filesystem::path writable = hle::fs::WritableRoot();
               !writable.empty() && !game_region::AddcontPacks(writable / "addcont").empty()) {
        cached = 1; // a pasta de DLC do port (main.cpp: <port>/assets/addcont)
    }
    return cached == 1;
}

u64 g_level_gate_id = ~0ull;        // fase do dono autorizada agora (pula o "mundo liberado" do save local)
double g_level_gate_time = -100;    // quando foi autorizada
// 4.5.1: as missoes (e as DLCs) mudam de numero com os pacotes instalados. O caminho do pacote da fase do dono fica
// guardado durante a sessao: o convidado pode abrir o portal da missao pelo jogo dele (numero local) e o portao de
// fases autoriza pelo caminho, nao pelo numero (era o que deixava o convidado preso no tubo para sempre).
std::string g_owner_level_path;
double g_owner_level_time = -100;
bool g_level_index_building = false; // so a NOSSA lista de fases aceita as fases de DLC que o save local nao liberou
SetupFn g_level_valid = nullptr;
constexpr int kDlcLevelBase = 2600; // ate aqui as fases do jogo base; as das DLCs vem depois deste buraco

// (fase) -> existe? Gancho em kLevelValidFn (o portao do jogo para carregar uma fase).
//  - durante a sessao: responde "sim" para a fase que o dono da sala mandou carregar (FollowOwner autoriza);
//  - enquanto montamos a nossa lista de fases: aceita as fases de DLC instaladas, para o LevelKey do dono achar o
//    numero certo aqui (a lista e so nossa; o jogo continua com a dele).
// Sem DLC instalado (e para as fases do jogo base fora da fase do dono) a resposta e a do jogo, sem mudanca nenhuma.
HLE_SYSV_ABI u64 LevelValidHook(u64 id, u64 a, u64 b, u64 c, u64 d, u64 e) {
    const u64 result = g_level_valid(id, a, b, c, d, e);
    if (result & 0xff) return result;
    if (!g_enabled || !runtime::mp_net::SelfId()) return result; // fora de uma sala: nada muda
    static thread_local bool inside = false; // o proprio jogo pode consultar de novo na busca pelo numero
    if (inside) return result;
    bool authorized = id == g_level_gate_id && runtime::mp_net::Now() - g_level_gate_time < 60.0;
    u64 obj = 0;
    if (!authorized && !g_owner_level_path.empty() && runtime::mp_net::Now() - g_owner_level_time < 900.0) {
        // 4.5.1: o dono esta numa missao: o numero pedido aqui e o desta copia do jogo, mas o pacote e o mesmo.
        inside = true;
        obj = Call(kLevelByIdFn, id);
        inside = false;
        authorized = obj && LevelPath(obj) == g_owner_level_path;
        if (authorized) Log("[MP] fase %s liberada pelo pacote da missao do dono", std::to_string(id));
    }
    if (!authorized && !g_level_index_building) return result;
    const bool from_dlc = id >= static_cast<u64>(kDlcLevelBase);
    if (!from_dlc && !authorized) return result;                  // o jogo base fica como o jogo respondeu
    if (from_dlc && !DlcContentInstalled()) return result;        // DLC: so com o pacote instalado
    if (!obj) {
        inside = true;
        obj = Call(kLevelByIdFn, id);
        inside = false;
    }
    if (!obj || LevelPath(obj).empty()) return result; // a fase nao existe neste jogo
    static u64 last_logged = ~0ull;
    if (authorized && last_logged != id) {
        last_logged = id;
        Log("[MP] fase %s liberada para a sessao (mundo nao liberado no save local)", std::to_string(id));
    }
    return 1;
}

// As listas de fases das DLCs sao lidas depois da tela de carregamento: uma fase que nao esta no indice refaz o
// indice (no maximo a cada 10 s).
// 4.5.1: `deep` tambem entra na lista das fases que o jogo nao reconhece agora (as missoes so respondem "existe"
// com a missao ativa naquele jogo): e assim que o convidado acha a fase da missao que o dono esta jogando. So roda
// quando o caminho do dono nao esta na lista normal, e o ponteiro e conferido antes de ler (Readable em LevelPath).
void BuildLevelIndex(bool again = false, bool deep = false) {
    const double now = runtime::mp_net::Now();
    if (g_level_index_time > -100 && !(again && now - g_level_index_time > 10.0)) return;
    g_level_index_time = now;
    g_level_index.clear();
    int found = 0;
    g_level_index_building = true; // as fases de DLC instaladas entram na NOSSA lista mesmo sem o save liberar
    for (int id = 0; id < 8192; ++id) { // as das DLCs ficam depois de um buraco grande (~2600)
        const bool gated = !(Call(kLevelValidFn, static_cast<u64>(id)) & 0xff);
        if (gated && !deep && (id < kDlcLevelBase || !DlcContentInstalled())) continue;
        const u64 obj = Call(kLevelByIdFn, static_cast<u64>(id));
        const std::string path = LevelPath(obj);
        if (path.empty()) continue;
        g_level_index[path].push_back(id);
        ++found;
    }
    g_level_index_building = false;
    std::printf("[MP] fases: %d (%zu pacotes)\n", found, g_level_index.size());
    std::fflush(stdout);
}

std::string LevelKeyIn(const std::string& path, int id) {
    const auto it = g_level_index.find(path);
    if (it == g_level_index.end()) return {};
    const auto pos = std::find(it->second.begin(), it->second.end(), id);
    if (pos == it->second.end()) return {};
    return path + "#" + std::to_string(pos - it->second.begin());
}

std::string LevelKey(u64 obj) {
    if (!obj) return {};
    const std::string path = LevelPath(obj);
    if (path.empty()) return {};
    const int id = At<s16>(obj + 0x80);
    BuildLevelIndex();
    std::string key = LevelKeyIn(path, id);
    if (key.empty()) {
        BuildLevelIndex(true);
        key = LevelKeyIn(path, id);
    }
    if (key.empty() && !g_owner_level_path.empty() && g_owner_level_path == path) { // 4.5.1: missao/DLC
        BuildLevelIndex(true, true);
        key = LevelKeyIn(path, id);
    }
    return key;
}

int LevelIdIn(const std::string& key) {
    const size_t hash = key.rfind('#');
    if (hash == std::string::npos) return -1;
    const auto it = g_level_index.find(key.substr(0, hash));
    const int order = std::atoi(key.c_str() + hash + 1);
    if (it == g_level_index.end() || order < 0 || order >= static_cast<int>(it->second.size())) return -1;
    return it->second[order];
}

int LevelIdForKey(const std::string& key) {
    BuildLevelIndex();
    int id = LevelIdIn(key);
    if (id < 0) {
        BuildLevelIndex(true);
        id = LevelIdIn(key);
    }
    if (id < 0) { // 4.5.1: fase de missao (o jogo so a reconhece com a missao ativa): procura no indice profundo
        const std::string path = key.substr(0, key.rfind('#'));
        BuildLevelIndex(true, true);
        id = LevelIdIn(key);
        if (id < 0 && !path.empty()) {
            const auto it = g_level_index.find(path);
            if (it != g_level_index.end() && !it->second.empty()) id = it->second.front();
        }
    }
    return id;
}

std::string g_level_key; // a fase atual deste jogo (atualizada no Tick)
double g_level_request_time = -100; // quando este jogo pediu uma troca de fase (seguindo o dono)
u64 g_level_obj = 0;

void UpdateLevelKey() {
    const u64 obj = CurrentLevelObj();
    if (obj == g_level_obj) return;
    g_level_obj = obj;
    const std::string key = LevelKey(obj);
    if (key != g_level_key) {
        g_level_key = key;
        std::printf("[MP] fase atual: %s (%d)\n", key.empty() ? "-" : key.c_str(), obj ? At<s16>(obj + 0x80) : -1);
        std::fflush(stdout);
        // 4.4.7: quem entrou na missao/fase avisa na hora: o outro jogo pede a mesma carga no mesmo instante em vez
        // de esperar a estabilidade do seguidor (era isso que deixava o convidado preso no tubo).
        // 4.4.8: so fases jogaveis. Antes o aviso levava junto "Loading_Screen_Default"/"Titles" e o outro jogo
        // entrava numa tela de carregamento que nao era missao nenhuma.
        const std::string low = Lower(key);
        const bool playable = low.find("loadingscreen") == std::string::npos &&
                              low.find("loading_screen") == std::string::npos && low.find("titles") == std::string::npos &&
                              low.find("startup") == std::string::npos && low.find("slideways") == std::string::npos && low != "-";
        if (g_enabled && playable && runtime::mp_net::SelfId()) runtime::mp_net::SendEvent("L|" + key);
    }
}

// Todos seguem o dono da sala: quando ele esta em outra fase (2 s parado nela), este jogo pede a mesma fase.
// Uma fase que nao deu (DLC que falta...) espera 60 s para tentar de novo.
std::atomic<bool> g_follow_owner{true};

// 4.5: ele ja esteve num mundo alguma vez nesta sessao (nao puxa o jogador para a fase do dono estando no menu).
std::atomic<bool> g_seen_world{false};

// A cena do tubo do vorton tranca o controle: o mundo local fica sem jogador/area por varios segundos. Enquanto isso o
// seguidor nao pode exigir estabilidade - era por isso que o convidado ficava preso caindo no tubo enquanto o dono ja
// estava na fase.
bool LocalTransition() {
    if (!g_in_world || g_level_key.empty()) return true;
    {
        std::lock_guard lock(g_area_mutex);
        if (g_area.empty() || runtime::mp_net::Now() - g_area_time < 4.0) return true;
    }
    const u64 p1 = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    return !p1 || !Readable(p1, 0x500);
}

// 4.5: interrompe a queda/animacao do tubo antes de desmontar a fase. So usa os campos que este modulo ja escreve no
// gancho de controle (velocidade em loc+0x80, marcha em loc+0x238): nenhum deslocamento novo no personagem.
void StopLocalFall() {
    const u64 p1 = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    if (!p1 || !Readable(p1 + 0x90, 8)) return;
    const u64 loc = At<u64>(p1 + 0x90);
    if (!loc || !Readable(loc + 0x70, 0x1d0)) return;
    At<float>(loc + 0x80) = 0.0f;
    At<float>(loc + 0x84) = 0.0f;
    At<float>(loc + 0x88) = 0.0f;
    At<u32>(loc + 0x70) = 0xff7fffffu; // rapidez = tabela da marcha
    At<s32>(loc + 0x238) = 3;          // parado
}

void FollowOwner(double now) {
    const bool off = !g_follow_owner || std::getenv("LD_MP_NOFOLLOW") != nullptr;
    static std::string owner_key, failed_key, pending_key;
    static double owner_since = 0, cooldown = 0, failed_until = 0, pending_time = 0;
    static int pending_tries = 0;
    const int owner = runtime::mp_net::OwnerId(), self = runtime::mp_net::SelfId();
    if (off || !owner || !self || owner == self) return;
    const auto it = g_puppets.find(owner * kPuppetStride); // o personagem ativo do dono
    if (it == g_puppets.end()) return;
    const Remote& r = it->second.cur;
    if (!r.valid || !r.ingame || now - it->second.cur_time > 3.0 || r.level.empty()) return;
    if (r.level != owner_key) {
        owner_key = r.level;
        owner_since = now;
    }
    if (!pending_key.empty()) { // pediu a troca: deu certo?
        if (g_level_key == pending_key) {
            pending_key.clear();
            pending_tries = 0;
            g_level_gate_id = ~0ull; // a fase do dono ja e a nossa: o portao volta ao normal
        } else if (now - pending_time > 25.0) {
            if (pending_tries < 1 && !r.level.empty()) { // 4.5.1: o jogo pode ter engolido o pedido (missao): pede de novo
                pending_tries = 1;
                pending_time = now;
                g_level_gate_time = now;
                g_owner_level_time = now;
                const int again = LevelIdForKey(r.level);
                if (again >= 0) {
                    g_level_gate_id = static_cast<u64>(again);
                    Log("[MP] a fase %s do dono ainda nao carregou: pedindo de novo", r.level);
                    Call(kGotoLevelFn, reinterpret_cast<u64>(g_base) + kLevelLoader, static_cast<u64>(again), 0, 0);
                    return;
                }
            }
            Log("[MP] nao consegui ir para a fase %s do dono da sala (tento de novo em 60 s)", pending_key);
            failed_key = pending_key;
            failed_until = now + 60.0;
            pending_key.clear();
            pending_tries = 0;
            g_level_gate_id = ~0ull;
        }
        return;
    }
    if (r.level == g_level_key || now < cooldown) return;
    if (!g_seen_world) return; // ainda no menu/front-end: nao puxa o jogador para a fase do dono
    // 4.5: na cena do tubo/queda nao ha mundo estavel para esperar: segue o dono no quadro seguinte
    const bool transition = LocalTransition();
    if (now - owner_since < (transition ? 0.25 : 2.0)) return;
    if (!transition) {
        std::lock_guard lock(g_area_mutex);
        if (now - g_area_time < 4.0) return; // ainda carregando a area (mundo estavel)
    }
    if (r.level == failed_key && now < failed_until) return;
    // 4.5.1: guarda o pacote da fase do dono (o portao de fases autoriza a missao pelo caminho, nao pelo numero)
    {
        const size_t hash = r.level.rfind('#');
        g_owner_level_path = hash == std::string::npos ? r.level : r.level.substr(0, hash);
        g_owner_level_time = now;
    }
    const int id = LevelIdForKey(r.level);
    if (id < 0) {
        static std::string logged;
        if (logged != r.level) { // 4.5.1: a lista pode ganhar a fase depois (missao que o jogo so reconhece ativa)
            logged = r.level;
            Log("[MP] o dono da sala esta na fase %s, que este jogo ainda nao achou (tentando de novo)", r.level);
        }
        failed_key = r.level;
        failed_until = now + 8.0;
        return;
    }
    Log("[MP] seguindo o dono da sala %s: indo para a fase %s", it->second.name, r.level);
    StopLocalFall(); // a queda/animacao do tubo para aqui: a fase vai ser desmontada agora
    // os fantasmas saem antes (a fase vai ser desmontada)
    for (auto& [pid, p] : g_puppets) {
        if (p.stage == Stage::kSpawning || p.stage == Stage::kActive) Despawn(p);
        p.stage = Stage::kNone;
    }
    g_drive_count = 0;
    g_level_request_time = now;
    // 4.5: a fase do dono fica autorizada durante a carga (o convidado pode nao ter o mundo liberado no save local;
    // o pacote da DLC, se for o caso, esta instalado - ver LevelValidHook)
    g_level_gate_id = static_cast<u64>(id);
    g_level_gate_time = now;
    Call(kGotoLevelFn, reinterpret_cast<u64>(g_base) + kLevelLoader, static_cast<u64>(id), 0, 0);
    pending_key = r.level;
    pending_time = now;
    pending_tries = 0;
    cooldown = now + 8.0;
}

// 4.5.1: EUR (CUSA01176) x USA (CUSA00935). As fases das duas versoes tem numeros diferentes, por isso a sala troca
// o caminho do pacote (LevelKey) e nunca o numero. O id da versao vai no estado para avisar quando sao diferentes.
std::string LocalRegion() {
    static const std::string id = game_region::TitleId(hle::fs::App0());
    return id.empty() ? std::string("?") : id;
}

// 4.5.1: os outros personagens equipados deste jogador (os slots de jogador do portal, menos o que esta em uso agora).
// Vao no estado para os outros verem a party inteira, nao so o personagem ativo.
std::vector<std::string> LocalPartyNames(const std::string& active) {
    std::vector<std::string> out;
    for (int i = g_nslots; i < g_nall && out.size() < 6; ++i) {
        if (SlotState(i) != 1) continue;
        std::string name = SlotName(i);
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".cd") == 0) name.resize(name.size() - 3);
        if (name.empty() || Lower(name) == Lower(active)) continue;
        if (g_our_slots.count(SlotAddress(i))) continue; // slot que NOS carregamos para um fantasma
        out.push_back(name);
    }
    return out;
}

void SendLocal(u64 p1, double now) {
    std::string area;
    {
        std::lock_guard lock(g_area_mutex);
        area = g_area;
    }
    char text[768];
    const u64 t0 = p1 ? Transform(p1) : 0;
    const u64 t = t0 && Readable(t0 + 0x90, 12) ? t0 : 0;
    const u64 loc0 = p1 && Readable(p1 + 0x90, 8) ? At<u64>(p1 + 0x90) : 0;
    const u64 loc = loc0 && Readable(loc0 + 0x80, 0x1c0) ? loc0 : 0; // na troca de fase o ponteiro fica solto (lixo)
    if (!p1 || !t || !loc || area.empty() || !Readable(p1 + 0x170, 0x240)) {
        std::snprintf(text, sizeof(text), "%s|-|0|0|0|3|0|1|0|1|0|0|0|%s", area.empty() ? "-" : area.c_str(), g_level_key.c_str());
        runtime::mp_net::SetLocalState(text);
        g_in_world = false;
        return;
    }
    g_in_world = true;
    g_seen_world = true; // 4.5: ja jogou num mundo (o seguidor de fase so age depois disto)
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
    std::string party_list; // 4.5.1: "nome;nome;nome" dos outros personagens equipados
    for (const std::string& one : LocalPartyNames(LocalCharacter(p1))) {
        if (!party_list.empty()) party_list += ';';
        party_list += one;
    }
    std::snprintf(text, sizeof(text), "%s|%s|%.3f|%.3f|%.3f|%d|%.3f|%.3f|%.3f|%.3f|%x|1|%d|%s|%s|%s", area.c_str(),
                  LocalCharacter(p1).c_str(), At<float>(t + 0x90), At<float>(t + 0x94), At<float>(t + 0x98),
                  At<s32>(loc + 0x238), At<float>(loc + 0x80), At<float>(loc + 0x88), At<float>(p1 + 0x170),
                  At<float>(p1 + 0x178), held | g_held_latch, runtime::mp_net::RttMs(), g_level_key.c_str(), LocalRegion().c_str(),
                  party_list.c_str());
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

// 4.1.1: inicio de funcao (push rbp; mov rbp, rsp) no deslocamento ou 0x10 depois (outro dump do eboot); 0 = nenhum
// 4.4.5: o dump pode vir com a tabela inteira deslocada (no USA as funcoes altas ja ficam +0x10). Em vez de exigir
// o endereco exato, procura o comeco de verdade numa janela de +-0x30 e usa o que achar.
// 4.4.6: o `Readable` precisa do endereco ABSOLUTO (g_base + offset) - passando so o offset ele sempre dava falso e
// nenhuma funcao era reconhecida (era o "eboot+0xd70730 nao reconhecido (bytes 55 48 89 e5)").
u64 FnAt(u64 offset) {
    static const u8 kPrologue[4] = {0x55, 0x48, 0x89, 0xe5};
    const u64 base = reinterpret_cast<u64>(g_base);
    for (long long shift : {0ll, 0x10ll, 0x20ll, 0x30ll, -0x10ll, -0x20ll}) {
        const long long at = static_cast<long long>(offset) + shift;
        if (at < 0x10000) continue;
        const u64 o = static_cast<u64>(at);
        if (Readable(base + o, 4) && std::memcmp(g_base + o, kPrologue, 4) == 0) return o;
    }
    return 0;
}

// confere as funcoes do eboot (so leitura). false = eboot desconhecido: o multiplayer nao liga, e o motivo fica
// guardado (g_last_error) para a janela do F3 mostrar - antes so aparecia "fora da rede", sem pista nenhuma.
// 4.4.5: `again` refaz a conferencia (o F3 chama assim quando o jogo ja esta aberto).
void RememberError(const char* text) {
    g_last_error = text;
    std::printf("[MP] %s\n", text);
    std::fflush(stdout);
}

// Detalhe do eboot vai para o log; na janela fica so um aviso curto (o texto longo nao cabe e nao ajuda em jogo).
void ReportEbootFailure(u64 offset) {
    char detail[220];
    const u8* at = g_base + offset;
    std::snprintf(detail, sizeof(detail), "eboot+0x%llx nao reconhecido (bytes %02x %02x %02x %02x)",
                  static_cast<unsigned long long>(offset), at[0], at[1], at[2], at[3]);
    std::printf("[MP] %s: multiplayer desligado\n", detail);
    std::fflush(stdout);
    RememberError("eboot do jogo nao reconhecido por este build - veja o log (build/sessao_*.log)");
}

bool ResolveFunctions(bool again = false) {
    static int s_ok = -1;
    if (s_ok == 1) return true;            // ja resolvido: nao mexe nos ponteiros de novo (eles ja foram ajustados)
    if (s_ok == 0 && !again) return false; // ja falhou: so tenta de novo quando pedem (a janela do F3 pede)
    s_ok = 0;
    if (!g_base) {
        RememberError("o eboot do jogo ainda nao esta carregado (o multiplayer liga dentro do jogo)");
        return false;
    }
    const u64 control = FnAt(kControlFn);
    if (!control) {
        ReportEbootFailure(kControlFn);
        return false;
    }
    g_control_fn = control;
    for (u64* f : {&kSpawnFn, &kRequestDtorFn, &kRequestDeleteFn, &kSlotLoadFn, &kListPushFn, &kSlotStateFn, &kLevelSetupFn,
                   &kDescFn, &kAllocFn, &kFreeFn, &kFireEventFn, &kKillObjectFn, &kBlowUpFn, &kTriggerObstacleFn,
                   &kGotoLevelFn, &kLevelByIdFn, &kLevelValidFn, &kMsRequestFn, &kReformTriggerFn, &kBuildStartFn,
                   &kBuildUpdateFn}) { // (eboot+a0a770 nao tem prologo: fica fixo, antes da parte deslocada)
        const u64 o = FnAt(*f);
        if (!o) {
            ReportEbootFailure(*f);
            return false;
        }
        *f = o;
    }
    if (kSpawnFn != 0x158c2c0 || control != kControlFn) {
        std::printf("[MP] eboot com as funcoes deslocadas em %+lld bytes: enderecos ajustados\n",
                    static_cast<long long>(kSpawnFn) - static_cast<long long>(0x158c2c0));
        std::fflush(stdout);
    }
    g_last_error.clear();
    s_ok = 1;
    return true;
}

// Os ganchos so entram com o multiplayer ligado (desligado, o jogo fica intocado). Pela sala do F3 entram no Tick,
// na thread do jogo, para nao trocar o comeco de uma funcao que ela esteja executando.
std::atomic<bool> g_hooks_wanted{false};
bool g_hooks_installed = false;

bool InstallHooks() {
    if (g_hooks_installed) return g_control && g_spawn && g_dtor;
    g_hooks_installed = true;
    g_control = reinterpret_cast<ControlFn>(Hook(g_control_fn ? g_control_fn : kControlFn, reinterpret_cast<const void*>(&ControlHook)));
    g_spawn = reinterpret_cast<SpawnFn>(Hook(kSpawnFn, reinterpret_cast<const void*>(&SpawnHook)));
    g_dtor = reinterpret_cast<DtorFn>(Hook(kRequestDtorFn, reinterpret_cast<const void*>(&RequestDtorHook)));
    if (const char* env = std::getenv("LD_MP_SLOTS")) g_extra_slots = std::clamp(std::atoi(env) - 7, 0, 1);
    g_setup = reinterpret_cast<SetupFn>(Hook(kLevelSetupFn, reinterpret_cast<const void*>(&LevelSetupHook))); // sem ele: so o portal
    g_slot_load = reinterpret_cast<SetupFn>(Hook(kSlotLoadFn, reinterpret_cast<const void*>(&SlotLoadHook)));
    // Fase 80: objetos que os outros quebram no mundo deles quebram no nosso
    g_blowup = reinterpret_cast<BlowUpFn>(Hook(kBlowUpFn, reinterpret_cast<const void*>(&BlowUpHook)));
    g_trigger_obstacle = reinterpret_cast<SetupFn>(Hook(kTriggerObstacleFn, reinterpret_cast<const void*>(&TriggerObstacleHook)));
    if (!g_ms_request) g_ms_request = reinterpret_cast<SetupFn>(Hook(kMsRequestFn, reinterpret_cast<const void*>(&MsRequestHook)));
    if (!g_build_update) g_build_update = reinterpret_cast<BuildUpdateFn>(Hook(kBuildUpdateFn, reinterpret_cast<const void*>(&BuildUpdateHook)));
    if (!g_reform_trigger) g_reform_trigger = reinterpret_cast<SetupFn>(Hook(kReformTriggerFn, reinterpret_cast<const void*>(&ReformTriggerHook)));
    if (!g_kill_object) g_kill_object = reinterpret_cast<KillFn>(Hook(kKillObjectFn, reinterpret_cast<const void*>(&KillObjectHook)));
    // 4.5: avisos de missao aplicados aqui nao abrem dialogo/camera/minigame (FireEvent) e a fase do dono da sala
    // passa pelo portao do save quando o pacote (DLC) esta instalado (LevelValid)
    if (!g_fire_event) g_fire_event = reinterpret_cast<SetupFn>(Hook(kFireEventFn, reinterpret_cast<const void*>(&FireEventHook)));
    if (!g_level_valid) g_level_valid = reinterpret_cast<SetupFn>(Hook(kLevelValidFn, reinterpret_cast<const void*>(&LevelValidHook)));

    if (!g_control || !g_spawn || !g_dtor) {
        std::printf("[MP] eboot diferente: multiplayer desligado\n");
        g_enabled = false; // os ganchos que entraram so repassam (sem fantasmas nada muda)
        return false;
    }
    std::printf("[MP] ganchos instalados\n");
    return true;
}

void Install(std::uint8_t* eboot_base) {
    g_base = eboot_base;
    ResolveFunctions(); // antes de qualquer gancho (o prologo e conferido)
    if (std::getenv("LD_MP_EVENTS") && !g_fire_event) { // pesquisa: registra os eventos do jogo (mesmo sem multiplayer)
        g_fire_event = reinterpret_cast<SetupFn>(Hook(kFireEventFn, reinterpret_cast<const void*>(&FireEventHook)));
        g_kill_object = reinterpret_cast<KillFn>(Hook(kKillObjectFn, reinterpret_cast<const void*>(&KillObjectHook)));
        std::printf("[EV] registro de eventos ligado\n");
        InstallProbes(std::make_integer_sequence<int, kProbeCount>{});
        // estados dos multiestados e montagens (reform) tambem sem multiplayer
        g_ms_request = reinterpret_cast<SetupFn>(Hook(kMsRequestFn, reinterpret_cast<const void*>(&MsRequestHook)));
        g_reform_trigger = reinterpret_cast<SetupFn>(Hook(kReformTriggerFn, reinterpret_cast<const void*>(&ReformTriggerHook)));
    }
    if (!g_enabled) return; // desligado: nenhum gancho (a sala do F3 liga depois, pelo Connect)
    if (!ResolveFunctions() || !InstallHooks()) {
        g_enabled = false;
        return;
    }
    if (!runtime::mp_net::Start(g_config.server, g_config.room, g_config.name, g_config.password)) {
        std::printf("[MP] endereco do servidor invalido: %s\n", g_config.server.c_str());
        g_enabled = false;
        return;
    }
    std::printf("[MP] multiplayer online ligado: sala %s, servidor %s\n", g_config.room.c_str(), g_config.server.c_str());
}

// Fase 83: sair da sala pelo painel. A rede para; os fantasmas somem no proximo Tick (os pares da sala ficam
// vazios) e so entao o multiplayer desliga - desligar antes deixaria os personagens dos outros parados no mundo.
std::atomic<bool> g_leaving{false};

void Disconnect() {
    if (!g_enabled) return;
    std::printf("[MP] saindo da sala %s\n", g_config.room.c_str());
    std::fflush(stdout);
    runtime::mp_net::Stop();
    if (!g_control) { // ganchos ainda nao instalados: nao ha fantasma nenhum
        g_enabled = false;
        return;
    }
    g_leaving = true;
}

bool Leaving() { return g_leaving; }

bool Connect(const std::string& server, const std::string& room, const std::string& name, const std::string& password) {
    g_leaving = false;
    g_config.server = server;
    g_config.room = room;
    g_config.name = name;
    g_config.password = password;
    g_enabled = false;
    runtime::mp_net::Stop();
    if (server.empty() || room.empty()) {
        RememberError(server.empty() ? "endereco do servidor vazio (use wss://...)" : "codigo da sala vazio");
        return false;
    }
    if (!g_hooks_installed) {
        if (!ResolveFunctions(true)) return false; // g_last_error ja diz qual funcao do eboot nao foi reconhecida
    } else if (!(g_control && g_spawn && g_dtor)) {
        RememberError("os ganchos do eboot nao entraram (veja o log do jogo)");
        return false;
    }
    if (!g_hooks_installed) g_hooks_wanted = true; // o Tick instala na thread do jogo
    g_enabled = true;
    if (!runtime::mp_net::Start(g_config.server, g_config.room, g_config.name, g_config.password)) {
        RememberError("endereco do servidor invalido (use wss://...) ou codigo da sala vazio");
        g_enabled = false;
        return false;
    }
    g_last_error.clear();
    std::printf("[MP] multiplayer online conectado: sala %s, servidor %s\n", g_config.room.c_str(), g_config.server.c_str());
    return true;
}

bool InWorld() { return g_enabled && g_in_world; }

// 4.4.5: texto do ultimo erro de conexao (a janela do F3 mostra isso em vez de so "fora da rede").
std::string LastError() { return g_last_error; }
void ClearLastError() { g_last_error.clear(); } // 4.4.6: abrir o F3 comeca uma tentativa nova (sem erro velho na tela)
void SetFollowOwner(bool on) { g_follow_owner = on; }
bool FollowOwnerEnabled() { return g_follow_owner; }

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

bool IsPuppet(u64 inst) {
    for (const auto& [id, p] : g_puppets) {
        if (p.inst == inst) return true;
    }
    return false;
}

// personagens vivos com este nome (sem jogadores nem fantasmas), em ordem de endereco
std::vector<u64> CharactersNamed(const std::string& name) {
    std::vector<u64> out;
    const u64 want = reinterpret_cast<u64>(g_base) + kCharacterVtable;
    std::lock_guard lock(g_gadget_mutex);
    for (u64 c : g_characters) {
        if (Readable(c, 0x500) && At<u64>(c) == want && !IsLocalPlayer(c) && !IsPuppet(c) && LocalCharacter(c) == name) out.push_back(c);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// O nosso jogador derrubou um personagem (capanga...): avisa a sala ("k|area|nome|ordem")
void OnLocalKill(u64 obj, u64 killer) {
    if (g_applying || !g_enabled || !IsLocalPlayer(killer) || !Readable(obj, 0x500)) return;
    if (At<u64>(obj) != reinterpret_cast<u64>(g_base) + kCharacterVtable || IsLocalPlayer(obj) || IsPuppet(obj)) return;
    const std::string name = LocalCharacter(obj);
    if (name.empty()) return;
    const std::vector<u64> same = CharactersNamed(name);
    const auto it = std::find(same.begin(), same.end(), obj);
    const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
    Announce("k|" + CurrentArea() + "|" + name + "|" + std::to_string(order));
    std::printf("[MP] derrubei %s (#%d): avisando a sala\n", name.c_str(), order);
    std::fflush(stdout);
}

// ---- 4.2: o que ja mudou nesta area (quebrados, derrubados, estados). Quem chega depois (seguindo o dono) recebe
// tudo de novo do dono da sala e o mundo dele fica igual. Aplicar duas vezes nao faz nada (ja quebrado / mesmo estado).
std::string g_log_area;
std::vector<std::string> g_area_log;
std::set<std::string> g_area_log_seen;

void LogAreaEvent(const std::string& payload) {
    const size_t a = payload.find('|'), b = a == std::string::npos ? a : payload.find('|', a + 1);
    if (b == std::string::npos) return;
    const std::string area = payload.substr(a + 1, b - a - 1);
    if (area != g_log_area) {
        g_log_area = area;
        g_area_log.clear();
        g_area_log_seen.clear();
    }
    if (payload[0] == 'm') { // estado: so o ultimo de cada objeto
        const size_t last = payload.rfind('|'), state_bar = payload.rfind('|', last - 1);
        const std::string key = payload.substr(0, state_bar);
        for (auto it = g_area_log.begin(); it != g_area_log.end(); ++it) {
            if (it->compare(0, key.size() + 1, key + "|") == 0) {
                g_area_log.erase(it);
                break;
            }
        }
        g_area_log.push_back(payload);
        return;
    }
    if (g_area_log_seen.insert(payload).second && g_area_log.size() < 600) g_area_log.push_back(payload);
}

void Announce(const std::string& payload) {
    runtime::mp_net::SendEvent(payload);
    LogAreaEvent(payload);
}

// mudancas de estado deste jogo -> sala. Objetos que mudam sozinhos o tempo todo (efeitos em ciclo) ficam de fora.
std::map<std::string, std::vector<double>> g_ms_times;
std::set<std::string> g_ms_noisy;

std::map<u64, int> g_build_seen_state;

void WatchBuilds(double now, double area_time) {
    static double last = 0;
    if (now - last < 0.25) return;
    last = now;
    std::vector<u64> list;
    {
        std::lock_guard lock(g_gadget_mutex);
        list = g_builds;
    }
    for (u64 b : list) {
        if (!IsBuild(b)) continue;
        const int st = At<s32>(b + 0x24c);
        auto [it, fresh] = g_build_seen_state.try_emplace(b, st);
        const int before = it->second;
        it->second = st;
        if (fresh || st == before || st != 4) continue;
        bool forced;
        {
            std::lock_guard lock(g_build_mutex);
            forced = g_forced_builds.erase(b) != 0;
        }
        if (forced) {
            std::printf("[MP] montagem %s pronta aqui (aviso de outro jogador)\n", BuildKey(b).c_str());
        } else if (now - area_time > 5.0) {
            Announce("c|" + CurrentArea() + "|" + BuildKey(b) + "|" + std::to_string(At<s32>(b + 0x220)));
            std::printf("[MP] montei %s (tipo %d): avisando a sala\n", BuildKey(b).c_str(), At<s32>(b + 0x220));
        }
        std::fflush(stdout);
    }
    std::lock_guard lock(g_build_mutex);
    for (auto it = g_forced_builds.begin(); it != g_forced_builds.end();) { // desiste depois de 40 s
        it = now - it->second > 40.0 ? g_forced_builds.erase(it) : std::next(it);
    }
}

void SendStateChanges(double now, double area_time) {
    WatchBuilds(now, area_time);
    std::vector<MsChange> changes;
    std::vector<ReformChange> reforms;
    {
        std::lock_guard lock(g_ms_mutex);
        changes.swap(g_ms_changes);
        reforms.swap(g_reform_changes);
    }
    for (const ReformChange& r : reforms) { // montagens: sempre de alguem (nao ha montagem no comeco da area)
        if (!Readable(r.obj, 0x300)) continue;
        const std::string name = GadgetName(r.obj);
        if (name.empty()) continue;
        const std::vector<u64> same = GadgetsNamed(name);
        const auto it = std::find(same.begin(), same.end(), r.obj);
        const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
        Announce("r|" + CurrentArea() + "|" + name + "|" + std::to_string(order) + "|" + std::to_string(r.before));
        std::printf("[MP] montei %s (estado antes %d): avisando a sala\n", name.c_str(), r.before);
        std::fflush(stdout);
    }
    if (changes.empty() || now - area_time < 5.0) return; // estados iniciais da area: nao sao de ninguem
    for (const MsChange& c : changes) {
        if (!Readable(c.obj, 0x460) || At<u64>(c.obj) != reinterpret_cast<u64>(g_base) + kMultiStateVtable) continue;
        const std::string name = GadgetName(c.obj);
        if (name.empty() || name.rfind("VFX_", 0) == 0 || g_ms_noisy.count(name)) continue;
        if (IsLocalOnlyGadget(name)) continue; // 4.5: minigame local (roda de keystone, terminal de hack): nao sai daqui
        auto& times = g_ms_times[name];
        times.push_back(now);
        while (!times.empty() && now - times.front() > 30.0) times.erase(times.begin());
        if (times.size() > 6) {
            g_ms_noisy.insert(name);
            Log("[MP] %s muda sozinho o tempo todo: fica fora da sincronizacao", name);
            continue;
        }
        const std::vector<u64> same = GadgetsNamed(name);
        const auto it = std::find(same.begin(), same.end(), c.obj);
        const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
        if (IsQuestGadget(name)) {
            // 4.5: historia/objetivo (gatilho de missao, NPC, marco): vai como "q" e o outro jogo aplica o estado em
            // memoria sem abrir dialogo/camera/menu (ver g_quest_silent). Nao vai como "m" para nao aplicar 2 vezes.
            Announce("q|" + CurrentArea() + "|" + name + "#" + std::to_string(order) + "|" + std::to_string(c.state));
            std::printf("[MP] missao %s no estado %d: avisando a sala (sem janela no outro)\n", name.c_str(), c.state);
        } else {
            Announce("m|" + CurrentArea() + "|" + name + "|" + std::to_string(order) + "|" + std::to_string(c.state) + "|" +
                     std::to_string(c.param));
            std::printf("[MP] %s mudou para o estado %d: avisando a sala\n", name.c_str(), c.state);
        }
        std::fflush(stdout);
    }
}

struct PendingEvent {
    int from;
    std::string payload;
    double time;
};
std::vector<PendingEvent> g_pending_events; // esperando a varredura achar o objeto

// Fase 80: aplica os avisos dos outros jogadores ("b|area|nome|ordem" = quebrou um objeto explodivel)
double g_push_request_time = -100; // 4.4.8: quando este jogo pediu a fase do outro por aviso ("L|")

void ApplyEvent(int from, const std::string& payload, double now) {
    std::vector<std::string> f;
    size_t pos = 0;
    while (pos <= payload.size()) {
        const size_t bar = payload.find('|', pos);
        f.push_back(payload.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    if (f.size() >= 2 && f[0] == "L") { // 4.4.7: o outro jogador entrou numa missao/fase
        const std::string& key = f[1];
        if (key.empty() || key == g_level_key || g_level_key.empty()) return;
        const int id = LevelIdForKey(key);
        if (id < 0) {
            std::printf("[MP] o outro entrou na fase %s e este jogo nao achou o numero dela\n", key.c_str());
            std::fflush(stdout);
            return;
        }
        std::printf("[MP] seguindo na hora: fase %s (id %d)\n", key.c_str(), id);
        std::fflush(stdout);
        for (auto& [pid, p] : g_puppets) {
            if (p.stage == Stage::kSpawning || p.stage == Stage::kActive) Despawn(p);
            p.stage = Stage::kNone;
        }
        g_drive_count = 0;
        g_level_request_time = now;
        g_level_gate_id = static_cast<u64>(id);
        g_level_gate_time = now;
        {
            const size_t hash = key.rfind('#');
            g_owner_level_path = hash == std::string::npos ? key : key.substr(0, hash);
            g_owner_level_time = now;
        }
        Call(kGotoLevelFn, reinterpret_cast<u64>(g_base) + kLevelLoader, static_cast<u64>(id), 0, 0);
        g_push_request_time = now; // 4.4.8: para sair do tubo se a carga nao terminar
        return;
    }
    if (f.size() < 4 || (f[0] != "b" && f[0] != "k" && f[0] != "o" && f[0] != "m" && f[0] != "r" && f[0] != "c" &&
                         f[0] != "q"))
        return;
    if (f[1] != CurrentArea()) return; // outra area: o objeto nem existe aqui
    if (IsLocalOnlyGadget(f[2])) return; // 4.5: minigame local do outro: nunca aparece neste jogo
    if (f[0] == "q") { // 4.5: historia/objetivo de outro jogador: progride o estado em memoria, sem janela/camera
        std::string name = f[2];
        int order = 0;
        if (const size_t hash = name.rfind('#'); hash != std::string::npos) {
            order = std::max(0, std::atoi(name.c_str() + hash + 1));
            name = name.substr(0, hash);
        }
        const std::vector<u64> same = GadgetsNamed(name);
        if (same.empty()) {
            g_pending_events.push_back({from, payload, now});
            if (now - g_gadget_scan_time > 2.0) StartGadgetScan(now);
            return;
        }
        const u64 obj = same[std::min(static_cast<size_t>(order), same.size() - 1)];
        const int state = std::atoi(f[3].c_str());
        LogAreaEvent(payload);
        if (GadgetState(obj) == state) return; // ja esta assim aqui
        g_applying = true;
        g_quest_silent = true; // dialogo/camera/minigame disparados por este estado nao abrem aqui
        Call(kMsRequestFn, obj, static_cast<u64>(static_cast<s64>(state)), 0, 0, 0);
        g_quest_silent = false;
        g_applying = false;
        std::printf("[MP] %d: missao %s no estado %d aqui tambem (sem janela)\n", from, name.c_str(), state);
        std::fflush(stdout);
        return;
    }
    if (f[0] == "c") { // alguem montou um montinho de pecas: monta aqui tambem (o fantasma dele monta, se aparece)
        const u64 b = FindBuild(f[2]);
        if (!b) {
            g_pending_events.push_back({from, payload, now});
            if (now - g_gadget_scan_time > 2.0) StartGadgetScan(now);
            return;
        }
        LogAreaEvent(payload);
        if (At<s32>(b + 0x24c) == 4) return; // ja montado aqui
        u64 who = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
        const auto pit = g_puppets.find(from * kPuppetStride);
        if (pit != g_puppets.end() && pit->second.stage == Stage::kActive && pit->second.inst) who = pit->second.inst;
        const u64 first = At<u64>(b + 0x200);
        if (!who || !first) return;
        {
            std::lock_guard lock(g_build_mutex);
            g_forced_builds[b] = now;
        }
        g_applying = true;
        Call8(kBuildStartFn, b, who, 0, 0, static_cast<u64>(std::max(1, std::atoi(f[3].c_str()))), 1, first,
              reinterpret_cast<u64>(g_base) + kBuildInputEvent);
        g_applying = false;
        std::printf("[MP] %d montou %s: montando aqui tambem\n", from, f[2].c_str());
        std::fflush(stdout);
        return;
    }
    if (f[0] == "r") { // alguem montou (as pecas se juntam aqui tambem)
        if (f.size() < 5) return;
        const std::vector<u64> same = GadgetsNamed(f[2]);
        if (same.empty()) {
            g_pending_events.push_back({from, payload, now});
            if (now - g_gadget_scan_time > 2.0) StartGadgetScan(now);
            return;
        }
        const u64 obj = same[std::min(static_cast<size_t>(std::max(0, std::atoi(f[3].c_str()))), same.size() - 1)];
        LogAreaEvent(payload);
        if (GadgetState(obj) != std::atoi(f[4].c_str())) return; // ja montado aqui (ou em outra etapa)
        g_applying = true;
        Call(kReformTriggerFn, 0, obj, 0);
        g_applying = false;
        std::printf("[MP] %d montou %s: montado aqui tambem\n", from, f[2].c_str());
        std::fflush(stdout);
        return;
    }
    if (f[0] == "m") { // um objeto multiestado mudou de estado (historia)
        if (f.size() < 6 || g_ms_noisy.count(f[2])) return;
        const std::vector<u64> same = GadgetsNamed(f[2]);
        if (same.empty()) {
            g_pending_events.push_back({from, payload, now});
            if (now - g_gadget_scan_time > 2.0) StartGadgetScan(now);
            return;
        }
        const u64 obj = same[std::min(static_cast<size_t>(std::max(0, std::atoi(f[3].c_str()))), same.size() - 1)];
        if (At<u64>(obj) != reinterpret_cast<u64>(g_base) + kMultiStateVtable) return;
        const int state = std::atoi(f[4].c_str()), param = std::atoi(f[5].c_str());
        LogAreaEvent(payload);
        if (GadgetState(obj) == state) return; // ja esta assim aqui
        g_applying = true;
        Call(kMsRequestFn, obj, static_cast<u64>(static_cast<s64>(state)), static_cast<u64>(static_cast<s64>(param)), 0, 0);
        g_applying = false;
        std::printf("[MP] %d: %s no estado %d aqui tambem\n", from, f[2].c_str(), state);
        std::fflush(stdout);
        return;
    }
    if (f[0] == "k") { // derrubou um personagem
        const std::vector<u64> same = CharactersNamed(f[2]);
        if (same.empty()) {
            g_pending_events.push_back({from, payload, now});
            if (now - g_gadget_scan_time > 2.0) StartGadgetScan(now);
            return;
        }
        const size_t order = static_cast<size_t>(std::max(0, std::atoi(f[3].c_str())));
        const u64 victim = same[std::min(order, same.size() - 1)];
        u64 who = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
        const auto it = g_puppets.find(from * kPuppetStride);
        if (it != g_puppets.end() && it->second.stage == Stage::kActive && it->second.inst) who = it->second.inst;
        g_applying = true;
        Call8(kKillObjectFn, victim, 3, who, 0, 0, 0, 0, 0);
        g_applying = false;
        LogAreaEvent(payload);
        std::printf("[MP] %d derrubou %s: derrubado aqui tambem\n", from, f[2].c_str());
        std::fflush(stdout);
        return;
    }
    const std::vector<u64> same = GadgetsNamed(f[2]);
    const size_t order = static_cast<size_t>(std::max(0, std::atoi(f[3].c_str())));
    if (same.empty()) {
        g_pending_events.push_back({from, payload, now});
        if (now - g_gadget_scan_time > 2.0) StartGadgetScan(now);
        return;
    }
    const u64 ctx = same[std::min(order, same.size() - 1)];
    if (f[0] == "o") { // acionou um obstaculo (alavanca, coisa de usar...)
        u64 user = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
        const auto pit = g_puppets.find(from * kPuppetStride);
        if (pit != g_puppets.end() && pit->second.stage == Stage::kActive && pit->second.inst) user = pit->second.inst;
        if (!user) return;
        g_applying = true;
        Call(kTriggerObstacleFn, 0, ctx, user);
        g_applying = false;
        std::printf("[MP] %d acionou %s: acionado aqui tambem\n", from, f[2].c_str());
        std::fflush(stdout);
        return;
    }
    if (At<u64>(ctx) != reinterpret_cast<u64>(g_base) + kBlowUpVtable) return;
    LogAreaEvent(payload);
    if (At<u8>(ctx + 0x260)) return; // ja quebrado aqui tambem
    // quem quebrou: o fantasma dele (se esta aparecendo), senao o nosso jogador (o jogo precisa de alguem)
    u64 who = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    const auto it = g_puppets.find(from * kPuppetStride);
    if (it != g_puppets.end() && it->second.stage == Stage::kActive && it->second.inst) who = it->second.inst;
    if (!who) return;
    g_applying = true;
    Call8(kBlowUpFn, ctx, who, 0, 1, 0, 0, 0, 0);
    g_applying = false;
    std::printf("[MP] %d quebrou %s: quebrado aqui tambem\n", from, f[2].c_str());
    std::fflush(stdout);
    (void)now;
}

void Tick() {
    if (!g_enabled) return;
    if (!g_main_tid) g_main_tid = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_main_tid) return;
    if (g_hooks_wanted.exchange(false) && !InstallHooks()) {
        runtime::mp_net::Stop();
        return;
    }
    if (!g_control) return; // ganchos ainda nao instalados
    const double now = runtime::mp_net::Now();
    static double regions_time = 0;
    if (now - regions_time > 2.0) { // a memoria pode mudar: confere de novo de vez em quando
        regions_time = now;
        g_region_count = g_region_next = 0;
    }
    u64 p1 = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    // 4.1.1: na troca de fase o jogador 1 da tabela pode ja ter sido liberado (vtable lixo): nao mexe nele. E logo
    // depois de NOS pedirmos a troca (seguindo o dono), espera 3 s.
    if (p1 && (!Readable(p1, 0x500) || At<u64>(p1) < reinterpret_cast<u64>(g_base) ||
               At<u64>(p1) >= reinterpret_cast<u64>(g_base) + 0x3400000 || now - g_level_request_time < 3.0)) {
        p1 = 0;
    }
    UpdateSlotCounts();
    UpdateLevelKey();
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
            p.retry_time = now + 2.5; // 4.4.7: menos pisca-pisca (o jogo as vezes desfaz e refaz o personagem)
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
    {
        static unsigned scanned_epoch = ~0u;
        if (scanned_epoch != g_area_epoch && now - g_area_time > 4.0) { // area nova carregada: lista os objetos
            scanned_epoch = g_area_epoch;
            StartGadgetScan(now);
        }
        if (!g_pending_events.empty() && !g_gadget_scan_busy) { // a varredura terminou: tenta de novo
            std::vector<PendingEvent> retry;
            retry.swap(g_pending_events);
            for (const PendingEvent& e : retry) {
                if (now - e.time > 6.0) {
                    std::printf("[MP] aviso de %d (%s): objeto nao achado aqui\n", e.from, e.payload.c_str());
                    std::fflush(stdout);
                } else if (e.time < g_gadget_scan_time) {
                    ApplyEvent(e.from, e.payload, e.time); // a lista ja inclui a varredura feita depois do aviso
                } else {
                    g_pending_events.push_back(e);
                }
            }
        }
    }
    for (const auto& [from, payload] : runtime::mp_net::TakeEvents()) ApplyEvent(from, payload, now);
    SendStateChanges(now, area_time);
    TrackState2(now);
    SlotMaintenance(now);
    ReclaimReserve(now);
    const bool settled = p1 && !area.empty() && now - area_time > 3.0;

    // pares atuais da sala
    const auto peers = runtime::mp_net::Peers();
    for (auto it = g_puppets.begin(); it != g_puppets.end();) {
        const bool present = std::any_of(peers.begin(), peers.end(), [&](const auto& q) { return q.id == it->second.peer; });
        if (!present) {
            if (it->second.stage == Stage::kSpawning || it->second.stage == Stage::kActive) Despawn(it->second);
            it = g_puppets.erase(it);
        } else {
            ++it;
        }
    }
    if (g_leaving && g_puppets.empty()) { // Fase 83: saiu da sala e os fantasmas ja sumiram
        g_leaving = false;
        g_enabled = false;
        {
            std::lock_guard lock(g_tags_mutex);
            g_tags.clear();
        }
        std::printf("[MP] fora da sala (multiplayer desligado ate entrar de novo)\n");
        std::fflush(stdout);
        return;
    }
    // 4.4.8: tubo infinito. Pedimos a fase do outro e ficamos presos numa tela de carregamento: depois de 30 s
    // volta para o Hub para nao travar o jogo de vez (o id do Hub e' resolvido AQUI - USA e EUR numeram diferente,
    // nunca usar o numero que veio do outro jogo).
    if (g_push_request_time > 0 && now - g_push_request_time > 30.0) {
        g_push_request_time = -100;
        const std::string low = Lower(g_level_key);
        const bool loading = low.empty() || low == "-" || low.find("loading") != std::string::npos ||
                             low.find("titles") != std::string::npos;
        if (loading) {
            const int hub = LevelIdForKey("Hub#0");
            if (hub >= 0) {
                std::printf("[MP] preso no carregamento da fase do outro: voltando para o Hub (id %d)\n", hub);
                std::fflush(stdout);
                g_level_gate_id = static_cast<u64>(hub);
                g_level_gate_time = now;
                g_level_request_time = now;
                Call(kGotoLevelFn, reinterpret_cast<u64>(g_base) + kLevelLoader, static_cast<u64>(hub), 0, 0);
            } else {
                std::printf("[MP] preso no carregamento e sem o numero do Hub: saindo para o menu\n");
                std::fflush(stdout);
            }
        }
    }
    FollowOwner(now);
    Drive old[kMaxPuppets];
    const int old_count = g_drive_count;
    std::copy(g_drive, g_drive + old_count, old);
    g_drive_count = 0;
    const int party_cap = PartyCap(peers.size());
    for (const auto& peer : peers) {
        const int base_key = peer.id * kPuppetStride;
        if (g_puppets.size() >= kMaxPuppets && !g_puppets.count(base_key)) continue;
        Puppet& main = g_puppets[base_key];
        main.peer = peer.id;
        main.name = peer.name;
        main.idx = 0;
        if (peer.seq != main.seq && !peer.state.empty()) {
            Remote r;
            if (ParseRemote(peer.state, &r)) {
                main.prev = main.cur;
                main.prev_time = main.cur_time;
                main.cur = r;
                main.cur_time = peer.state_time;
            }
            main.seq = peer.seq;
        }
        // 4.5.1: alem do personagem ativo, a party dele (os outros equipados) aparece do lado dele. O limite por pessoa
        // depende de quantas pessoas estao na sala (PartyCap): 2 pessoas = 6 cada, 3 = 5 cada...
        const int cap = std::min<int>(party_cap, 1 + static_cast<int>(main.cur.party.size()));
        for (int idx = 0; idx < cap; ++idx) {
            if (idx && !(main.cur.valid && main.cur.ingame)) continue;
            const int key = base_key + idx;
            if (idx && g_puppets.size() >= kMaxPuppets && !g_puppets.count(key)) continue;
            Puppet& p = idx == 0 ? main : g_puppets[key];
            p.peer = peer.id;
            p.name = peer.name;
            p.idx = idx;
            if (idx) { // o estado e o do amigo; so mudam o personagem e o lugar (em volta dele, sem empilhar)
                p.cur = main.cur;
                p.cur.chr = main.cur.party[idx - 1];
                const double angle = 2.4 * idx;
                p.cur.x = main.cur.x + static_cast<float>(std::cos(angle)) * 0.55f;
                p.cur.z = main.cur.z + static_cast<float>(std::sin(angle)) * 0.55f;
                p.cur_time = main.cur_time;
                p.seq = main.seq;
            }
        // 4.5.1: sumia ao se afastar porque a area muda de nome nas zonas do mundo (a fase e a mesma: as posicoes sao
        // do mesmo mundo). Mesma fase = mesmo mundo. E a tolerancia do estado subiu para 10 s (engasgo do outro jogo).
        const bool same_world = p.cur.area == area || (!p.cur.level.empty() && p.cur.level == g_level_key);
        const bool want = settled && p.cur.valid && p.cur.ingame && same_world && now - p.cur_time < 10.0 &&
                          p.cur.chr != "-" && !p.cur.chr.empty();
        p.want = want;
        // 4.2: chegou na nossa area ha 6 s (a lista de objetos dele ja esta pronta): o dono reenvia o que ja mudou aqui
        const bool here = settled && p.cur.valid && p.cur.ingame && same_world && now - p.cur_time < 10.0;
        if (!p.cur.region.empty() && p.cur.region != "?" && p.cur.region != LocalRegion()) {
            static std::set<std::string> warned;
            if (warned.insert(p.cur.region).second) {
                std::printf("[MP] %s usa outra versao do jogo (%s x %s): as fases vao pelo pacote, mas os numeros mudam\n",
                            p.name.c_str(), p.cur.region.c_str(), LocalRegion().c_str());
                std::fflush(stdout);
            }
        }
        if (idx == 0 && !here) {
            p.same_area_since = 0;
            p.replayed = false;
        } else if (idx == 0 && !p.same_area_since) {
            p.same_area_since = now;
        } else if (idx == 0 && !p.replayed && now - p.same_area_since > 6.0) {
            p.replayed = true;
            const int self = runtime::mp_net::SelfId(), owner = runtime::mp_net::OwnerId();
            if (self && (owner == self || !owner) && g_log_area == area && !g_area_log.empty()) {
                for (const std::string& e : g_area_log) runtime::mp_net::SendEvent(e);
                std::printf("[MP] %s chegou na area: reenviando %zu mudancas\n", p.name.c_str(), g_area_log.size());
                std::fflush(stdout);
            }
        }
        if (!want) p.want_since = 0;
        else if (!p.want_since) p.want_since = now;
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
            p.retry_time = now + 1.0;
            if (g_unsupported.count(Lower(p.cur.chr))) { // personagem que nao e figura: aparece com um substituto
                if (u64 slot = StandInSlot()) {
                    p.slot = slot;
                    p.standin = true;
                    p.stage = Stage::kReady;
                }
                break;
            }
            if (u64 slot = FindLoadedSlot(p.cur.chr)) {
                p.slot = slot;
                p.standin = false;
                p.stage = Stage::kReady;
            } else if (CanLoad() && SomeoneLoading()) {
                // o jogo carrega um personagem por vez: espera a vez (enquanto isso so o nome aparece)
                if (!p.wait_since) p.wait_since = now;
            } else if (LocalPlayerUses(Lower(p.cur.chr) + ".cd") && !FreePlayerSlot()) {
                // 4.4.7: o outro jogo esta com o MESMO personagem e nao ha slot de jogador livre: entra com um
                // personagem ja carregado (nome dele em cima) - antes ficava so o nome e o boneco ia e voltava.
                if (u64 slot = StandInSlot()) {
                    p.slot = slot;
                    p.standin = true;
                    p.stage = Stage::kReady;
                } else {
                    p.retry_time = now + 3.0;
                }
                break;
            } else if (const bool shares_local = LocalPlayerUses(Lower(p.cur.chr) + ".cd");
                       CanLoad() && now - p.load_fail_time > 15.0 && now - g_last_unload > 8.0 &&
                       (!shares_local || FreePlayerSlot())) { // carrega num slot livre
                p.wait_since = 0;
                // 4.5: personagem igual ao do jogador local (o mesmo Batman nos dois): o slot TEM de ser um "Player Load
                // Slot" proprio, nunca o do portal/party do jogador (duas instancias isoladas, sem colisao de ponteiro)
                const bool player_slot = shares_local || UsePlayerSlot(); // primeiro os de jogador (nao disputam com as figuras)
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
                p.standin = false;
                p.slot = IsSlot(slot) ? slot : 0;
                if (p.slot) g_our_slots[p.slot] = Lower(p.cur.chr);
                p.stage = p.slot ? Stage::kLoading : Stage::kNone;
                p.stage_time = now;
                if (!p.slot) p.load_fail_time = now; // nao deu agora: so o nome, e tenta de novo depois
                g_last_load_start = now;
                Log("[MP] carregando %s para %s", p.cur.chr, p.name);
            } else if (!SomeoneLoading() && now - p.load_fail_time > 15.0 && RecycleSlot()) {
                p.retry_time = now + 1.0; // o slot esvazia em seguida; carrega na proxima tentativa
            } else if (!SomeoneLoading() && ReclaimForCloser(p, now)) {
                // sem slot: tirou de quem esta bem mais longe (esse aparece com um substituto)
            } else if (now - p.want_since > 3.0) {
                if (u64 slot = StandInSlot()) { // sem slot por enquanto: aparece com um personagem ja carregado
                    p.slot = slot;
                    p.standin = true;
                    p.stage = Stage::kReady;
                }
            }
            break;
        case Stage::kLoading:
            if (p.slot && Readable(p.slot + 0x2c8, 4) && At<s32>(p.slot + 0x2c8) == 1) {
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
            if (!p.slot || !Readable(p.slot + 0x2c8, 4) || At<s32>(p.slot + 0x2c8) != 1) {
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
                if (p.standin) {
                    // substituto: ja sabe que e outro personagem
                } else if (Lower(LocalCharacter(inst)) == low) {
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
        case Stage::kActive: {
            if (!PuppetAlive(p)) {
                p.stage = Stage::kNone;
                p.request = p.inst = 0;
                break;
            }
            // 4.5: o pedido acabou preso na party do jogador local (inst+0x3a0 igual): desfaz agora para nao derrubar o
            // jogo na troca de figura e volta com um slot independente
            const u64 local_party = p1 && Readable(p1 + 0x3a0, 8) ? At<u64>(p1 + 0x3a0) : 0;
            if (local_party && Readable(p.inst + 0x3a0, 8) && At<u64>(p.inst + 0x3a0) == local_party) {
                Despawn(p);
                if (++p.clashes >= 3) { // ja tentou 3 vezes: entra como substituto, sem nome solto
                    if (u64 slot = StandInSlot()) {
                        p.slot = slot;
                        p.standin = true;
                        p.stage = Stage::kReady;
                        p.retry_time = 0;
                        Log("[MP] %s fica como substituto (party do jogador local): %s", p.cur.chr, p.name);
                    } else {
                        p.retry_time = now + 10.0;
                    }
                } else {
                    Log("[MP] %s ficou com a party do jogador local: desfazendo o fantasma", p.name);
                    p.retry_time = now + 2.0;
                    p.load_fail_time = 0;
                }
                break;
            }
            // o amigo trocou de personagem: refaz o fantasma (se o novo ja esta carregado ou cabe num slot)
            if (p.standin && now - p.stage_time > 8.0) { // 4.4.7: substituto vira o personagem certo quando sobrar slot
                const u64 real = FindLoadedSlot(p.cur.chr);
                if (real && !SlotUsedByLocalPlayer(real) && !g_unsupported.count(Lower(p.cur.chr))) {
                    Log("[MP] %s volta ao personagem certo (%s)", p.name, p.cur.chr.c_str());
                    p.standin = false;
                    p.slot = real;
                    Despawn(p);
                    p.retry_time = 0;
                    break;
                }
            }
            if (!p.standin && Lower(LocalCharacter(p.inst)) != Lower(p.cur.chr) && now - p.stage_time > 1.0 &&
                !g_unsupported.count(Lower(p.cur.chr)) &&
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
        } // 4.5.1: fim dos personagens da party deste amigo
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
            // 4.4.8: diagnostico do movimento - "vetor" e' o loc+0x80/0x88 que o jogo do outro manda, "delta" e' o
            // deslocamento real entre dois pacotes e "cara" e' inst+0x170/0x178. Angulo vetor x delta ~0 = vetor do
            // mundo; ~90 graus = vetor local ao corpo.
            std::printf("[MP]   %s: remoto (%.2f %.2f %.2f) marcha %d ha %.1fs | fantasma (%.2f %.2f %.2f) | vetor (%.2f %.2f) "
                        "delta (%.2f %.2f) cara (%.2f %.2f)\n", p.name.c_str(), p.cur.x,
                        p.cur.y, p.cur.z, p.cur.gear, now - p.cur_time, t ? At<float>(t + 0x90) : 0.f, t ? At<float>(t + 0x94) : 0.f,
                        t ? At<float>(t + 0x98) : 0.f, p.cur.dx, p.cur.dz, p.move_dx, p.move_dz, p.cur.fx, p.cur.fz);
        }
        std::fflush(stdout);
    }
}

} // namespace runtime::multiplayer
