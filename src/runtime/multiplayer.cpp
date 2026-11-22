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
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
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
std::string CurrentArea();

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
u64 kPlayerBrainCtorFn = 0xebfef0; // 4.4.23: PlayerBrain(brain, owner) isolado para o fantasma atacar/pular/voar
u64 kSetActionFn = 0xd6ea10;       // 4.4.23: Character::SetAction(inst, ctx_id, param, flags, sub)
u64 kStartMeleeFn = 0x12214f0;     // 4.4.24: FightingComponent::StartMeleeAttack(fight, bind, 0x30, 0) - soco/combo real
u64 kCanSwapFn = 0x11aa030;        // 4.4.24: CanSwapToCharacter - impede o Triangulo de tentar entrar em fantasma
u64 kPartySwitchFn = 0x14b4e30;    // 4.4.24: PlayerParty::SwitchControlledCharacter - blinda fc->ctrl e fantasmas
u64 g_context_registry_ptr = 0;    // 4.4.23: deslocamento de g_ContextRegistry (resolvido via RIP em kSetActionFn+0xc1)
u64 g_main_camera_ptr = 0;         // 4.4.23: deslocamento da camera ativa (resolvido via RIP em fn_getcam)

// 4.4.23: hash FNV-1 em maiusculas usado pelo ContextRegistry do motor (eboot+0x4a4390). Identico em USA e EUR.
constexpr u32 NuHash(const char* s) {
    u32 h = 0x811c9dc5u;
    if (!s) return h;
    for (; *s; ++s) {
        const int c = static_cast<signed char>(*s);
        const int u = (c >= 'a' && c <= 'z') ? (c - 0x20) : c;
        h = (h * 0x1000193u) ^ static_cast<u32>(u);
    }
    return h;
}
constexpr u32 kCtxDefault     = NuHash("Default");
constexpr u32 kCtxJumping     = NuHash("Jumping");
constexpr u32 kCtxFalling     = NuHash("Falling");
constexpr u32 kCtxLandSlam    = NuHash("LandSlam");
constexpr u32 kCtxFighting    = NuHash("Fighting");
constexpr u32 kCtxConeAttack  = NuHash("ConeAttack");
constexpr u32 kCtxSpecialMove = NuHash("SpecialMove");
constexpr u32 kCtxFlying      = NuHash("Flying");
constexpr u32 kCtxGliding     = NuHash("Gliding");

constexpr u32 kCross = 0x40004000, kSquare = 0x10008000, kCircle = 0x20002000; // mascara de botoes do jogo
// 4.4.21: os quatro botoes de face (nas duas metades da palavra). Antes so cruz e quadrado eram copiados e o soco
// dependia de um bit que pode nao ser o mesmo em toda figura; agora o que o outro apertar chega inteiro.
constexpr u32 kSentButtons = 0xf000f000;
constexpr int kMaxPuppets = 24;   // 4.5.1: cada amigo pode aparecer com mais de um personagem (a party dele)
constexpr int kPuppetStride = 64;
constexpr int kFirstVehiclePuppet = 8;
constexpr int kMaxRemoteVehicles = 2;
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
thread_local Region g_regions[32];
thread_local int g_region_count = 0, g_region_next = 0;

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

u64 ReadPointer(u64 address) {
    return Readable(address, sizeof(u64)) ? At<u64>(address) : 0;
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
SetupFn g_find_native_slot = nullptr;
u64 g_begin_slot_load = 0, g_find_native_slot_fn = 0;
std::array<std::atomic<u64>, kMaxSlotsSeen> g_replica_slots{};
thread_local bool g_replica_slot_work = false;
SetupFn g_set_action = nullptr;
using SpawnFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64);
using DtorFn = u64(HLE_SYSV_ABI*)(u64, u64, u64, u64, u64, u64);
ControlFn g_control = nullptr;
SpawnFn g_spawn = nullptr;
DtorFn g_dtor = nullptr;

// ---- area atual (radar) -----------------------------------------------------------------------------------------
std::mutex g_area_mutex;
std::string g_area;
std::atomic<unsigned> g_area_epoch{0};
std::atomic<unsigned> g_world_session{0};
double g_area_time = 0;

// ---- spawners vistos nesta area (o jogo cria os personagens da party por eles) ----------------------------------
struct SpawnerSeen {
    u64 spawner;
    unsigned epoch;
    std::string area;
};
std::mutex g_spawner_mutex;
std::vector<SpawnerSeen> g_spawners;
std::atomic<bool> g_spawning{false}; // nos mesmos chamando o spawn

struct LocalVehicle {
    u64 inst, request, identity, portal_slot;
    int slot;
    std::string model;
};
std::mutex g_local_vehicle_mutex;
std::vector<LocalVehicle> g_local_vehicles;
bool ValidVehicleName(const std::string& name);
std::string LocalCharacter(u64 inst);

constexpr u64 kMechVehicleVtable = 0x2bfe200;
constexpr u64 kMechVehicleRequestVtable = 0x2bfe540;
u64 g_vehicle_registry = 0, g_vehicle_resource_fn = 0;

bool IsMechVehicle(u64 inst) {
    return inst && ReadPointer(inst) == reinterpret_cast<u64>(g_base) + kMechVehicleVtable;
}

u64 VehicleIdentity(u64 inst) {
    return IsMechVehicle(inst) ? ReadPointer(inst + 0x48) : ReadPointer(inst + 0x10);
}

u64 EngineMethod(u64 object, u64 slot) {
    const u64 table = ReadPointer(object);
    const u64 base = reinterpret_cast<u64>(g_base);
    if (table < base + 0x2300000 || table >= base + 0x3400000) return 0;
    const u64 method = ReadPointer(table + slot);
    return method >= base && method < base + 0x2300000 ? method - base : 0;
}

bool IsReplicaSlot(u64 slot) {
    return slot && std::any_of(g_replica_slots.begin(), g_replica_slots.end(), [=](const auto& entry) {
        return entry.load(std::memory_order_acquire) == slot;
    });
}

bool ReserveReplicaSlot(u64 slot) {
    if (IsReplicaSlot(slot)) return true;
    for (auto& entry : g_replica_slots) {
        u64 empty = 0;
        if (entry.compare_exchange_strong(empty, slot)) return true;
    }
    return false;
}

void ForgetReplicaSlot(u64 slot) {
    if (!slot) return;
    for (auto& entry : g_replica_slots) {
        u64 expected = slot;
        if (entry.compare_exchange_strong(expected, 0)) return;
    }
}

std::string VehicleModelLabel(std::string label) {
    const size_t separator = label.find_last_of("/\\");
    std::string model = Lower(separator == std::string::npos ? label : label.substr(separator + 1));
    if (model.ends_with(".cd")) model.resize(model.size() - 3);
    return ValidVehicleName(model) ? model : std::string{};
}

std::string PortalVehicleModel(u64 slot) {
    if (!slot || !Readable(slot + 0xb8, 48)) return {};
    const auto* label = reinterpret_cast<const char*>(slot + 0xb8);
    return VehicleModelLabel(std::string(label, strnlen(label, 48)));
}

void RememberLocalVehicle(u64 inst, u64 request = 0, u64 portal_slot = 0, std::string model = {}) {
    if (!inst) return;
    const u64 identity = VehicleIdentity(inst);
    if (!identity) return;
    if (model.empty() && !IsMechVehicle(inst)) model = Lower(LocalCharacter(inst));
    if (!ValidVehicleName(model)) return;
    std::lock_guard lock(g_local_vehicle_mutex);
    for (auto& existing : g_local_vehicles) {
        if (existing.inst != inst) continue;
        if (existing.identity != identity) {
            existing.request = 0;
            existing.portal_slot = 0;
        }
        if (request) {
            existing.request = request;
            if (portal_slot) existing.portal_slot = portal_slot;
        }
        existing.identity = identity;
        existing.model = model;
        return;
    }
    constexpr int kMaxVehicleIds = 48;
    for (int slot = 1; slot <= kMaxVehicleIds; ++slot) {
        if (std::none_of(g_local_vehicles.begin(), g_local_vehicles.end(),
                         [=](const LocalVehicle& vehicle) { return vehicle.slot == slot; })) {
            g_local_vehicles.push_back({inst, request, identity, portal_slot, slot, model});
            return;
        }
    }
}

void ForgetLocalVehicle(u64 inst, u64 request = 0) {
    std::lock_guard lock(g_local_vehicle_mutex);
    std::erase_if(g_local_vehicles, [=](const LocalVehicle& vehicle) {
        return (inst && (vehicle.inst == inst || (IsMechVehicle(vehicle.inst) && vehicle.identity == inst))) ||
               (request && vehicle.request == request);
    });
}

u64 SpawnedInstance(u64 request) {
    const u64 comp = request ? ReadPointer(request + 0x40) : 0;
    // O portal registra Character+0x370 ou MechVehicle+0x58 no pedido.
    if (comp > 0x58 && ReadPointer(comp) == reinterpret_cast<u64>(g_base) + kMechVehicleRequestVtable) {
        const u64 vehicle = comp - 0x58;
        return IsMechVehicle(vehicle) ? vehicle : 0;
    }
    return comp ? ReadPointer(comp + 0x20) : 0;
}

// ---- fantasmas --------------------------------------------------------------------------------------------------
struct RemoteVehicle {
    int slot;
    std::string name;
    float x, y, z, fx, fz;
};

struct Remote {
    bool valid = false;
    std::string area, chr;
    float x = 0, y = 0, z = 0;
    int gear = 3;
    float dx = 0, dz = 1, fx = 0, fz = 1;
    u32 held = 0;
    bool ingame = false;
    std::string vehicle;
    std::vector<RemoteVehicle> vehicles;
    int rtt_ms = 0; // latencia dele ate o servidor
    std::string level; // 4.1.1: fase atual ("LevelPack\...\Nome#ordem"); vazio = versao antiga / nao sabe
    std::string region; // 4.5.1: CUSA01176 (EUR) ou CUSA00935 (USA): as fases sao mapeadas pelo pacote, mas o aviso ajuda
    std::vector<std::string> party; // 4.5.1: os outros personagens equipados dele (o ativo e o `chr`)
    u32 ctx_hash = 0; // 4.4.23: hash do ActionContext ativo no outro jogo ("Default", "Fighting", "Flying"...)
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
    std::string spawn_level; // 4.4.32: fase em que este fantasma nasceu (se a fase mudou, o mundo dele nao existe mais)
    bool standin = false;  // 4.1.1: sem slot para o personagem dele: aparece com um personagem ja carregado
    double same_area_since = 0; // 4.2: desde quando esta na nossa area
    bool replayed = false;      // 4.2: o dono ja reenviou o registro da area para ele
    float distance = 0;  // ate o nosso jogador 1 (quem esta mais perto ganha os slots)
    float still_x = 0, still_z = 0; // ultimo lugar onde o fantasma andou (para ver se ficou preso)
    double still_time = 0;
    int stuck_side = 0; // 4.5.1: para que lado ele desvia quando fica preso (sem teleporte)
    int idx = 0;        // 4.5.1: 0 = personagem ativo do amigo, 1.. = os outros da party dele
    unsigned seq = 0;
    float move_dx = 0, move_dz = 0; // 4.4.8: deslocamento real entre os dois ultimos pacotes (diagnostico)
    float last_px = 0, last_pz = 0, last_wx = 0, last_wz = 0; // 4.4.8: sonda da convencao da direcao
    float last_dist = 99.0f; // 4.4.8: distancia do quadro anterior (a sonda so vale sem correcao acontecendo)
    std::string dropped_for_local; // 4.4.14: (reservado) personagem que o jogador local tomou
    int gear_shown = 3;            // 4.4.13/4.4.21: marcha mostrada (so troca depois de 80 ms estavel)
    double gear_time = 0;          // 4.4.13: desde quando a marcha nova esta pedindo para entrar
    double punch_until = 0;        // 4.4.21: ate quando o motor toca o soco do outro jogo (sem forcar a marcha)
    double fly_pulse_start = -100; // 4.4.23: pulso duplo de X para entrar em voo quando o outro jogo esta em Flying
    double fly_cooldown = -100;
    double native_flight_retry = -100;
    u32 prev_held = 0;
    u32 last_remote_ctx = 0;       // 4.4.24: ultimo ActionContext visto do outro jogo (bordas de pulo/soco/especial)
    bool pending_melee = false;    // 4.4.24: bordas latentes consumidas no ControlHook (nunca se perdem entre Ticks)
    bool pending_special = false;
    bool pending_jump = false;
    bool pending_slam = false;
    u32 pending_action = 0;
    double pending_action_until = 0;
    double jump_until = 0;         // 4.4.24: janela minima do pulo antes de espelhar pouso em Default
    float face_ang = 0.0f;         // 4.4.16: cara suavizada (angulo)
    bool have_face = false;
    bool have_last = false;
    Remote cur;
    // 4.4.17: ultimos estados (posicao + cara) para desenhar ~100 ms atras e interpolar de verdade, sem adivinhar
    std::deque<std::pair<double, Remote>> hist;
    double cur_time = 0;
    alignas(16) u8 load_buffer[256] = {}; // descritor + saida do carregamento do slot (o jogo escreve aqui)
    char load_name[64] = {};
};

std::map<int, Puppet> g_puppets; // so a thread principal mexe

// 4.4.23: eboot+0xd72248..0xd722f3 prova que loc+0x80/0x88 e' espaco do MUNDO (passado direto ao atan2f como
// inst+0x170/0x178). So muda para corpo se LD_MP_MOVE=local for pedido explicitamente para teste.
bool MoveSpaceLocal() {
    static const bool local = [] {
        const char* v = std::getenv("LD_MP_MOVE");
        return v && std::strcmp(v, "local") == 0;
    }();
    return local;
}

// Pedidos e instancias dos fantasmas vivos (atomicos para ganchos que podem rodar na NuBgProc sem trava).
std::atomic<u64> g_live[32];
std::atomic<u64> g_live_insts[32];
std::atomic<u64> g_fake_ctrls[32];

// a fase atual deste jogo (atualizada no Tick). Declarada aqui em cima porque o Despawn precisa dela (4.4.32).
std::string g_level_key;

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
        if (request && e.load(std::memory_order_relaxed) == request) return true;
    }
    return false;
}

void LiveInstAdd(u64 inst) {
    if (!inst) return;
    for (auto& e : g_live_insts) {
        if (e.load(std::memory_order_relaxed) == inst) return;
    }
    for (auto& e : g_live_insts) {
        u64 expected = 0;
        if (e.compare_exchange_strong(expected, inst)) return;
    }
}

void LiveInstDrop(u64 inst) {
    if (!inst) return;
    for (auto& e : g_live_insts) {
        u64 expected = inst;
        e.compare_exchange_strong(expected, 0);
    }
}

void LiveInstClear() {
    for (auto& e : g_live_insts) e.store(0, std::memory_order_relaxed);
}

bool IsPuppetFast(u64 inst) {
    if (!inst) return false;
    for (const auto& e : g_live_insts) {
        if (e.load(std::memory_order_relaxed) == inst) return true;
    }
    return false;
}

// O que o gancho de controle aplica (so thread principal, montado no Tick).
struct Drive {
    u64 inst;
    Puppet* puppet = nullptr;
    int slot_key = 0;
    int gear;
    float speed; // 0 = a da tabela da marcha
    float dx, dz, fx, fz;
    u32 held, prev;
    u32 ctx_hash = 0;
    bool hold = false; // 4.4.9: espelho puro - rapidez 0 e a posicao vem do Teleport (nada de IA andando sozinha)
    bool punching = false; // 4.4.21: o outro jogo socou - o motor toca a animacao, o gancho nao forca a marcha
    bool jump_active = false;
    bool vehicle = false;
    bool ground = false;
    float x = 0, y = 0, z = 0;
    u64 name = 0;      // 4.4.10: inst+0x10 do fantasma (se mudou, o personagem morreu/trocou: nao mexer)
};

// nomes em cima dos fantasmas (o overlay le de outra thread)
std::mutex g_tags_mutex;
std::vector<NameTag> g_tags;
constexpr float kNameHeight = 0.26f; // acima da posicao do personagem (no meio do corpo; o boneco tem ~0.4)
Drive g_drive[kMaxPuppets];
int g_drive_count = 0;

double g_last_spawn = 0;
std::array<double, 4> g_button_until{-100, -100, -100, -100};
std::atomic<u32> g_last_held{0};
u32 g_held_sent = 0; // 4.4.21: ultimo botao enviado (a sonda usa para dar a rajada no soco)
unsigned g_seen_epoch = 0;
double g_last_log = 0;

bool IsOurRequest(u64 request) { return LiveHas(request); }

std::vector<std::string> g_our_chars; // personagens que NOS carregamos (minusculas), os unicos que cancelamos/descarregamos
double g_stuck_since[kMaxSlotsSeen] = {};

bool SomeoneLoading() {
    for (auto& [id, p] : g_puppets) {
        if (p.stage == Stage::kLoading) return true;
    }
    return false;
}

// 4.4.23: consulta e busca no ContextRegistry (eboot+0xa3d4d0 / 0xa3d5c0) para saber o ActionContext atual
// (inst+0x38, ou inst+0x450 se inst+0x38 == inst+0x454) pelo hash FNV-1 do nome ("Default", "Fighting", "Flying"...).
s32 ActiveContextId(u64 inst) {
    if (!inst || !Readable(inst + 0x38, 4) || !Readable(inst + 0x450, 8)) return -1;
    s32 ctx = At<s32>(inst + 0x38);
    if (ctx == At<s32>(inst + 0x454)) ctx = At<s32>(inst + 0x450);
    return ctx;
}

u32 ActiveContextHash(u64 inst) {
    if (!g_context_registry_ptr) return 0;
    const u64 reg_addr = reinterpret_cast<u64>(g_base) + g_context_registry_ptr;
    if (!Readable(reg_addr, 8)) return 0;
    const u64 reg = At<u64>(reg_addr);
    if (!reg || !Readable(reg + 0x10b0, 4)) return 0;
    const s32 ctx = ActiveContextId(inst);
    const s32 count = At<s32>(reg + 0x10b0);
    if (ctx < 0 || ctx >= count || count > 1024 || !Readable(reg + 0xb0 + static_cast<u64>(ctx) * 8, 8)) return 0;
    const u64 factory = At<u64>(reg + 0xb0 + static_cast<u64>(ctx) * 8);
    return factory && Readable(factory + 0x34, 4) ? At<u32>(factory + 0x34) : 0;
}

s32 ContextIdByHash(u32 want_hash) {
    if (!want_hash || !g_context_registry_ptr) return -1;
    const u64 reg_addr = reinterpret_cast<u64>(g_base) + g_context_registry_ptr;
    if (!Readable(reg_addr, 8)) return -1;
    const u64 reg = At<u64>(reg_addr);
    if (!reg || !Readable(reg + 0x10b0, 4)) return -1;
    const s32 count = std::min<s32>(At<s32>(reg + 0x10b0), 1024);
    for (s32 i = 0; i < count; ++i) {
        if (!Readable(reg + 0xb0 + static_cast<u64>(i) * 8, 8)) break;
        const u64 factory = At<u64>(reg + 0xb0 + static_cast<u64>(i) * 8);
        if (factory && Readable(factory + 0x34, 4) && At<u32>(factory + 0x34) == want_hash) return i;
    }
    return -1;
}

bool SharedAction(u32 hash) {
    return hash == kCtxLandSlam || hash == kCtxFlying || hash == kCtxGliding;
}

bool IsLocalPlayer(u64 inst);
bool RunFlightAddon(u64 inst, bool start);

HLE_SYSV_ABI u64 SetActionHook(u64 inst, u64 id, u64 param, u64 flags, u64 sub, u64 extra) {
    const bool local = runtime::mp_net::SelfId() && IsLocalPlayer(inst);
    const u32 before = local ? ActiveContextHash(inst) : 0;
    const u64 result = g_set_action(inst, id, param, flags, sub, extra);
    if (local) {
        const u32 after = ActiveContextHash(inst);
        if (after != before && SharedAction(after)) {
            const std::string character = LocalCharacter(inst);
            if (!character.empty()) Announce("a|" + CurrentArea() + "|" + character + "|" + std::to_string(after));
        }
    }
    return result;
}

// ---- ganchos ----------------------------------------------------------------------------------------------------
std::atomic<u64> g_hook_calls{0}, g_hook_other_thread{0}, g_hook_driven{0};

// 4.4.23: PlayerParty (0x280 bytes) + PartyOwnerHandle (0x28) + PlayerBrain (0x118, eboot+0xebfef0) + holder (0x100)
// totalmente isolados por fantasma (reaproveitados por slot_key, sem entrar na lista global de reset eboot+0x199f990 e
// sem compartilhar ponteiros com o jogador 1). Assim o PlayerBrain dispara soco, combo, pulo e voo no fantasma sem
// nunca ler o controle fisico do jogador 1 nem interferir na party local.
struct FakeController {
    u64 ctrl = 0, holder = 0, owner = 0, brain = 0, bound_inst = 0;
};
std::map<int, FakeController> g_fake_ctrl;
std::atomic<bool> g_player_mode{[] {
    const char* v = std::getenv("LD_MP_PLAYERMODE");
    return !(v && v[0] == '0' && v[1] == 0);
}()};
u64 GameAlloc(u64 size);
void Teleport(u64 inst, float x, float y, float z);
u64 g_vehicle_facing_fn = 0;

void SetReplicaFacing(u64 inst, float fx, float fz) {
    // A matriz da figura tem a frente na terceira linha com sinal invertido.
    // A direcao de controle (+0x170) pode estar certa mesmo com a malha de lado.
    const float back_x = At<float>(inst + 0x150), back_z = At<float>(inst + 0x158);
    const float length = std::hypot(back_x, back_z);
    if (length > 0.01f && -(back_x * fx + back_z * fz) >= length * 0.9998f &&
        std::abs(At<float>(inst + 0x170) - fx) + std::abs(At<float>(inst + 0x178) - fz) < 0.002f) return;
    if (g_vehicle_facing_fn && Readable(inst, 0x500)) {
        alignas(16) const float direction[] = {fx, 0.0f, fz, 0.0f};
        Call(g_vehicle_facing_fn, inst, reinterpret_cast<u64>(direction));
    } else {
        At<float>(inst + 0x170) = fx;
        At<float>(inst + 0x178) = fz;
    }
}

bool IsOurFakeCtrl(u64 ctrl) {
    if (!ctrl) return false;
    for (const auto& e : g_fake_ctrls) {
        if (e.load(std::memory_order_relaxed) == ctrl) return true;
    }
    return false;
}

void RememberFakeCtrl(u64 ctrl) {
    if (!ctrl) return;
    for (auto& e : g_fake_ctrls) {
        if (e.load(std::memory_order_relaxed) == ctrl) return;
    }
    for (auto& e : g_fake_ctrls) {
        u64 expected = 0;
        if (e.compare_exchange_strong(expected, ctrl)) return;
    }
}

FakeController* FakeControllerFor(int slot_key, u64 inst) {
    auto it = g_fake_ctrl.find(slot_key);
    if (it != g_fake_ctrl.end() && it->second.ctrl && it->second.holder && it->second.owner && it->second.brain) {
        FakeController& fc = it->second;
        if (fc.bound_inst != inst) {
            fc.bound_inst = inst;
            At<u64>(fc.brain + 0xf8) = 0;
            At<u64>(fc.brain + 0x100) = 0;
            At<u16>(fc.brain + 0x114) = 0xffff;
            At<u8>(fc.brain + 0x116) = 0;
        }
        At<u64>(fc.ctrl + 0x18) = fc.holder;
        At<u64>(fc.ctrl + 0xd0) = fc.owner;
        At<u64>(fc.ctrl + 0xd8) = fc.brain;
        At<u64>(fc.ctrl + 0xf8) = inst + 0x370;
        At<u8>(fc.ctrl + 0x218) = 1;
        At<u64>(fc.holder + 0x80) = 0;
        return &fc;
    }
    const u64 p1 = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
    const u64 src = p1 && Readable(p1 + 0x3a0, 8) ? At<u64>(p1 + 0x3a0) : 0;
    if (!src || !Readable(src, 0x280) || !kPlayerBrainCtorFn) return nullptr;
    const u64 src_holder = At<u64>(src + 0x18);
    const u64 src_owner = At<u64>(src + 0xd0);
    if (!src_holder || !Readable(src_holder, 0x80) || !src_owner || !Readable(src_owner, 0x28)) return nullptr;
    const u64 c = GameAlloc(0x2c0), h = GameAlloc(0x100), owner = GameAlloc(0x30), brain = GameAlloc(0x140);
    if (!c || !h || !owner || !brain) return nullptr;
    std::memset(reinterpret_cast<void*>(c), 0, 0x2c0);
    std::memset(reinterpret_cast<void*>(h), 0, 0x100);
    std::memset(reinterpret_cast<void*>(owner), 0, 0x30);
    std::memset(reinterpret_cast<void*>(brain), 0, 0x140);
    std::memcpy(reinterpret_cast<void*>(c), reinterpret_cast<const void*>(src), 0x280);
    std::memcpy(reinterpret_cast<void*>(h), reinterpret_cast<const void*>(src_holder), 0x80);
    std::memcpy(reinterpret_cast<void*>(owner), reinterpret_cast<const void*>(src_owner), 0x28);
    At<u64>(owner + 0x08) = 0;
    At<u64>(owner + 0x10) = 0;
    At<u64>(owner + 0x18) = 0;
    At<u64>(owner + 0x20) = c;
    Call(kPlayerBrainCtorFn, brain, owner);
    if (!At<u64>(brain)) return nullptr;
    std::memset(reinterpret_cast<void*>(h), 0, 0x20);
    std::memset(reinterpret_cast<void*>(h + 0x58), 0x80, 8);
    At<u64>(h + 0x80) = 0; // sem controle fisico em +0x80: eboot+0xd710a2 nao sobrescreve botoes nem analogico
    At<u64>(c + 0x18) = h;
    At<u64>(c + 0xd0) = owner;
    At<u64>(c + 0xd8) = brain;
    std::memset(reinterpret_cast<void*>(c + 0xf8), 0, 0xc0);
    At<u64>(c + 0xf8) = inst + 0x370;
    At<u64>(c + 0x1f8) = 0;
    At<u8>(c + 0x218) = 1;
    At<u8>(c + 0x21a) = 0;
    At<u64>(c + 0x228) = c;
    At<u64>(c + 0x230) = 0;
    RememberFakeCtrl(c);
    std::printf("[MP] fantasma %llx ganhou PlayerParty %llx + PlayerBrain %llx + holder %llx\n",
                static_cast<unsigned long long>(inst), static_cast<unsigned long long>(c),
                static_cast<unsigned long long>(brain), static_cast<unsigned long long>(h));
    std::fflush(stdout);
    return &(g_fake_ctrl[slot_key] = FakeController{c, h, owner, brain, inst});
}

// 4.4.24: eboot+0xd718ce..0xd71c02 usa cam+0x90 (+Z_cam = TRAS da camera), nega para Forward = (-cx, -cz) em 0xd719ca
// e monta Right = (+cz, -cx) em 0xd71b87. A inversa exata (sx = ux*cz - uz*cx, sy = -ux*cx - uz*cz) casa bit a bit
// tanto PadInput+0xb0..0xb8 quanto o angulo binario em PadInput+0x94 (que soma +0x8000 = 180 graus em 0xd7182f).
void WriteCameraStick(u64 holder, int gear, float dx, float dz, bool active) {
    u8 bx = 0x80, by = 0x80;
    if (active && gear > 3 && std::isfinite(dx) && std::isfinite(dz)) {
        const float dlen = std::hypot(dx, dz);
        if (dlen > 0.01f) {
            const float ux = dx / dlen, uz = dz / dlen;
            float cx = 0.0f, cz = 1.0f;
            if (g_main_camera_ptr) {
                const u64 cam_ptr_addr = reinterpret_cast<u64>(g_base) + g_main_camera_ptr;
                const u64 cam = Readable(cam_ptr_addr, 8) ? At<u64>(cam_ptr_addr) : 0;
                if (cam && Readable(cam + 0x90, 12)) {
                    const float fx = At<float>(cam + 0x90), fz = At<float>(cam + 0x98);
                    const float flen = std::hypot(fx, fz);
                    if (std::isfinite(flen) && flen > 0.01f) {
                        cx = fx / flen;
                        cz = fz / flen;
                    }
                }
            }
            const float sx_u =  ux * cz - uz * cx;
            const float sy_u = -ux * cx - uz * cz;
            const float m = gear == 4 ? 0.35f : (gear == 5 ? 0.65f : 1.0f);
            const float rad = 42.5f + 85.0f * m;
            bx = static_cast<u8>(std::clamp(static_cast<int>(std::lround(127.5f + rad * sx_u)), 1, 254));
            by = static_cast<u8>(std::clamp(static_cast<int>(std::lround(127.5f - rad * sy_u)), 1, 254));
        }
    }
    At<u8>(holder + 0x5a) = bx;
    At<u8>(holder + 0x5b) = by;
}

SetupFn g_can_swap = nullptr;
SetupFn g_party_switch = nullptr;

// 4.4.24: ao apertar Triangulo para trocar entre os SEUS personagens, eboot+0x11aa030 varre os personagens do mundo;
// retornando 0 para qualquer fantasma, o jogo troca limpo apenas entre as figuras do jogador local (sem crash).
HLE_SYSV_ABI u64 CanSwapHook(u64 a, u64 from_chr, u64 to_chr, u64 d, u64 e, u64 f) {
    if (g_enabled && IsPuppetFast(to_chr)) return 0;
    return g_can_swap(a, from_chr, to_chr, d, e, f);
}

// 4.4.24: blinda PlayerParty::SwitchControlledCharacter (eboot+0x14b4e30) contra tocar em fc->ctrl (evita o crash em
// eboot+0x14b77db ao desfazer nos de lista) ou capturar fantasmas na party do jogador local (~Character / roda).
HLE_SYSV_ABI u64 PartySwitchHook(u64 party, u64 world, u64 chr_370, u64 a, u64 b, u64 c) {
    if (IsOurFakeCtrl(party)) return 0;
    if (g_enabled && chr_370 > 0x370 && IsPuppetFast(chr_370 - 0x370)) return 0;
    return g_party_switch(party, world, chr_370, a, b, c);
}

void ApplyGroundLocomotion(u64 inst, const Drive& drive) {
    const u64 loc = ReadPointer(inst + 0x90);
    if (!loc || !Readable(loc + 0x70, 0x1d0)) return;
    if (drive.hold || drive.gear <= 3) At<float>(loc + 0x70) = 0.0f;
    else if (drive.speed > 0.0f) At<float>(loc + 0x70) = drive.speed;
    else At<u32>(loc + 0x70) = 0xff7fffffu; // rapidez da tabela da marcha
    At<s32>(loc + 0x238) = drive.gear;
    const bool walking = drive.gear > 3;
    const bool body_space = MoveSpaceLocal();
    At<float>(loc + 0x80) = walking && !body_space ? drive.dx : 0.0f;
    At<float>(loc + 0x84) = 0.0f;
    At<float>(loc + 0x88) = walking ? (body_space ? 1.0f : drive.dz) : 0.0f;
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
    // 4.4.10: o personagem pode ter sido desfeito/trocado (troca de figura com Y, portal). Se a instancia nao e'
    // mais a nossa, nao escreve nada nela - era assim que o jogo caia ao trocar de personagem.
    if (d && (!Readable(inst + 0x10, 0x500) || (d->name && At<u64>(inst + 0x10) != d->name))) d = nullptr;

    if (d && d->vehicle) {
        const u64 result = g_control(inst, dt);
        if (Readable(inst + 0x10, 0x500) && At<u64>(inst + 0x10) == d->name) {
            Teleport(inst, d->x, d->y, d->z);
            SetReplicaFacing(inst, d->fx, d->fz);
        }
        return result;
    }
    bool want_melee = false, want_special = false, want_jump = false, want_slam = false;
    u32 action = 0;
    u32 held = d ? d->held : 0, prev_btn = d ? d->prev : 0;
    if (d && d->puppet && d->puppet->inst == inst) {
        if (d->puppet->pending_melee)   { want_melee = true;   d->puppet->pending_melee = false;   held |= kSquare; prev_btn &= ~kSquare; }
        if (d->puppet->pending_special) { want_special = true; d->puppet->pending_special = false; held |= kCircle; prev_btn &= ~kCircle; }
        if (d->puppet->pending_jump)    { want_jump = true;    d->puppet->pending_jump = false;    held |= kCross;  prev_btn &= ~kCross; }
        if (d->puppet->pending_slam)    { want_slam = true;    d->puppet->pending_slam = false; }
        action = d->puppet->pending_action;
    }
    const u32 pressed = held & ~prev_btn;
    if (pressed & kSquare) want_melee = true;
    if (pressed & kCircle) want_special = true;
    if (pressed & kCross)  want_jump = true;

    const u32 ctx_before = d ? ActiveContextHash(inst) : 0;
    const bool airborne = ctx_before == kCtxJumping || ctx_before == kCtxFalling;
    const bool slam_combo = d && (pressed & kSquare) && ((held | prev_btn) & kCross);
    want_slam |= slam_combo && airborne;
    if (slam_combo && ctx_before == kCtxDefault && want_jump && d->puppet) {
        d->puppet->pending_action = kCtxLandSlam;
        d->puppet->pending_action_until = runtime::mp_net::Now() + 0.45;
        action = kCtxLandSlam;
    }
    if (want_slam) action = kCtxLandSlam;
    if (!action && d && (d->ctx_hash == kCtxFlying || d->ctx_hash == kCtxGliding)) action = d->ctx_hash;
    if (action == kCtxLandSlam) {
        want_melee = false;
        held &= ~kSquare;
    }
    const bool walking = d && d->ground && (ctx_before == 0 || ctx_before == kCtxDefault) &&
                         !d->punching && !held && !want_jump && !want_slam;
    if (d) {
        if (walking) ApplyGroundLocomotion(inst, *d);
        At<float>(inst + 0x170) = d->fx;
        At<float>(inst + 0x178) = d->fz;
    }
    FakeController* active_fc = nullptr;
    bool used_player_ctrl = false;
    // No chao, o eixo da locomocao vem do caminho recebido. O analogo falso
    // depende da camera deste PC e fica reservado para os contextos de acao.
    if (d && g_player_mode.load()) {
        if (FakeController* fc = FakeControllerFor(d->slot_key, inst)) {
            used_player_ctrl = true;
            active_fc = fc;
            At<u64>(inst + 0x3a0) = fc->ctrl;
            At<u32>(fc->holder + 0x00) = held;
            At<u32>(fc->holder + 0x04) = prev_btn;
            At<u32>(fc->holder + 0x08) = held & ~prev_btn;
            At<u32>(fc->holder + 0x0c) = prev_btn & ~held;
            // 4.4.24: BrainModule::PollButtons (eboot+0xec3230 via owner->vt[0x7d8] = ctrl+0x18) le 3 triplas de botoes:
            // +0x24..0x2c (indice 0), +0x30..0x38 (indice 1 = gamepad INPG!), e +0x3c..0x44 (indice 2).
            for (u64 off : {0x24ull, 0x30ull, 0x3cull}) {
                At<u32>(fc->ctrl + off + 0) = held;
                At<u32>(fc->ctrl + off + 4) = held & ~prev_btn;
                At<u32>(fc->ctrl + off + 8) = prev_btn & ~held;
            }
            if (Readable(inst + 0x4f8, 0x18)) {
                At<u32>(inst + 0x4f8 + 0xc) = held;
                At<u32>(inst + 0x4f8 + 0x10) = held & ~prev_btn;
                At<u32>(inst + 0x4f8 + 0x14) = prev_btn & ~held;
            }
            d->prev = held;
            // 4.4.24: destrava o ping-pong PreUpdate/PostUpdate (byte [brain+0x117] bits 0x08/0x10) e liga bit 0x01
            At<u8>(fc->brain + 0x117) = static_cast<u8>((At<u8>(fc->brain + 0x117) & ~0x18u) | 0x01u);
            const bool airborne_move = (ctx_before == kCtxFlying || ctx_before == kCtxGliding ||
                                        ctx_before == kCtxJumping || ctx_before == kCtxFalling) &&
                                       (d->gear > 3 || std::hypot(d->dx, d->dz) > 0.05f);
            const int stick_gear = airborne_move ? std::max(d->gear, 5) : d->gear;
            const bool walk = !walking && (stick_gear > 3) && !d->hold && !d->punching;
            WriteCameraStick(fc->holder, stick_gear, d->dx, d->dz, walk);
            At<u8>(inst + 0x399) = 1;
        }
    }
    if (d && !used_player_ctrl) {
        if (Readable(inst + 0x3a0, 8) && IsOurFakeCtrl(At<u64>(inst + 0x3a0))) At<u64>(inst + 0x3a0) = 0;
        if (Readable(inst + 0x4f8, 0x18)) {
            At<u32>(inst + 0x4f8 + 0xc) = held;
            At<u32>(inst + 0x4f8 + 0x10) = held & ~prev_btn;
            At<u32>(inst + 0x4f8 + 0x14) = prev_btn & ~held;
        }
        d->prev = held;
        At<u8>(inst + 0x399) = walking ? 0 : 1;
    }
    if (action == kCtxFlying && ctx_before != kCtxFlying && d->puppet) {
        const double now = runtime::mp_net::Now();
        if (now >= d->puppet->native_flight_retry) {
            d->puppet->native_flight_retry = now + 0.45;
            RunFlightAddon(inst, true);
        }
    } else if (action && action != ctx_before && (action != kCtxLandSlam || airborne || !want_jump)) {
        const s32 id = ContextIdByHash(action);
        if (id >= 0) Call(kSetActionFn, inst, static_cast<u64>(id), 0, 0, 0xffffffffu);
    }
    if (action && d && ActiveContextHash(inst) == action && d->puppet) {
        d->puppet->pending_action = 0;
        if (action == kCtxLandSlam) {
            d->punching = false;
            d->puppet->punch_until = 0;
        }
    }
    const u64 result = g_control(inst, dt);
    if (!d || !Readable(inst + 0x10, 0x500) || At<u64>(inst + 0x10) != d->name) return result;

    // 4.4.24: fecha o ciclo PostUpdate do PlayerBrain (vtable[0x80] = eboot+0xec1220) para o proximo quadro funcionar.
    if (active_fc && active_fc->brain && Readable(active_fc->brain, 8)) {
        const u64 vt = At<u64>(active_fc->brain);
        if (vt && Readable(vt + 0x80, 8)) {
            if (const u64 post_fn = At<u64>(vt + 0x80)) {
                reinterpret_cast<ControlFn>(post_fn)(active_fc->brain, dt);
            }
        }
    }

    u32 local_ctx = ActiveContextHash(inst);
    // 4.4.24: disparo direto no motor enquanto inst+0x399==1:
    //  - Soco e combo de 3 golpes: FightingComponent::StartMeleeAttack (eboot+0x12214f0 em [inst+0x798])
    //  - Especial (bola): SetAction("SpecialMove") (eboot+0x123811e)
    //  - Pulo (X): SetAction("Jumping", param=0) (eboot+0xda2230 / 0xd9db70)
    //  - Golpe aereo (quadrado no ar / LandSlam): StartMeleeAttack ou SetAction("LandSlam")
    if (want_melee && kStartMeleeFn && !(ctx_before != kCtxFighting && local_ctx == kCtxFighting)) {
        const u64 fight = Readable(inst + 0x798, 8) ? At<u64>(inst + 0x798) : 0;
        if (fight && Readable(fight, 0x140)) {
            alignas(16) u64 dummy_bind[4] = {};
            Call(kStartMeleeFn, fight, reinterpret_cast<u64>(dummy_bind), 0x30, 0);
            local_ctx = ActiveContextHash(inst);
        }
    }
    if (want_special && local_ctx == kCtxDefault && kSetActionFn) {
        const s32 spec_id = ContextIdByHash(kCtxSpecialMove);
        if (spec_id >= 0) {
            Call(kSetActionFn, inst, static_cast<u64>(spec_id), 0, 0, 0xffffffffu);
            local_ctx = ActiveContextHash(inst);
        }
    }
    if (want_jump && local_ctx == kCtxDefault && kSetActionFn) {
        const s32 jump_id = ContextIdByHash(kCtxJumping);
        if (jump_id >= 0) {
            Call(kSetActionFn, inst, static_cast<u64>(jump_id), 0, 0, 0xffffffffu);
            local_ctx = ActiveContextHash(inst);
        }
    }

    // 4.4.23/4.4.24: se o outro jogo ja pousou (Default) e o fantasma continuava em voo/planar/queda (ex.: Teleport em rampa), pousa na hora
    if (d->ctx_hash == kCtxDefault && !action &&
        (local_ctx == kCtxFlying || local_ctx == kCtxGliding || local_ctx == kCtxFalling ||
         (local_ctx == kCtxJumping && !d->jump_active)) &&
        kSetActionFn) {
        const bool stopped = local_ctx == kCtxFlying && RunFlightAddon(inst, false);
        const s32 def_id = stopped ? -1 : ContextIdByHash(kCtxDefault);
        if (def_id >= 0) {
            Call(kSetActionFn, inst, static_cast<u64>(def_id), 0, 0, 0xffffffffu);
        }
        local_ctx = ActiveContextHash(inst);
    }
    // O controle da replica so precisa estar anexado durante a atualizacao.
    At<u8>(inst + 0x399) = 0;
    if (IsOurFakeCtrl(ReadPointer(inst + 0x3a0))) At<u64>(inst + 0x3a0) = 0;
    const bool in_action = d->punching || (local_ctx != 0 && local_ctx != kCtxDefault);
    const u64 loc = Readable(inst + 0x90, 8) ? At<u64>(inst + 0x90) : 0;
    if (in_action) {
        if ((d->punching || local_ctx == kCtxFighting || local_ctx == kCtxConeAttack) && loc && Readable(loc + 0x70, 4)) {
            At<u32>(loc + 0x70) = 0;
        }
    } else if (d->ground) {
        ApplyGroundLocomotion(inst, *d);
    }
    Teleport(inst, d->x, d->y, d->z);
    if (walking || (d->ground && !in_action)) {
        SetReplicaFacing(inst, d->fx, d->fz);
    } else {
        At<float>(inst + 0x170) = d->fx;
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

HLE_SYSV_ABI u64 FindNativeSlotHook(u64 out, u64 a, u64 b, u64 c, u64 d, u64 e) {
    const u64 result = g_find_native_slot(out, a, b, c, d, e);
    const u64 slot = ReadPointer(out);
    if (!g_replica_slot_work && IsReplicaSlot(slot)) {
        // A busca nativa reteve uma referencia. Devolve-a e deixa o jogo
        // carregar a figura local num slot proprio.
        if (const u64 release = EngineMethod(slot, 0x30)) {
            At<u64>(out) = 0;
            Call(release, slot);
        }
    }
    return result;
}

HLE_SYSV_ABI u64 SpawnHook(u64 spawner, u64 request, u64 flag, u64 a, u64 b, u64 c) {
    const bool local = !g_spawning && !IsOurRequest(request);
    const u64 portal_slot = local && Readable(spawner, 0x200) &&
        At<u64>(spawner) == reinterpret_cast<u64>(g_base) + kSpawnerVtable ? ReadPointer(spawner + 0x1f0) : 0;
    const std::string vehicle_model = PortalVehicleModel(portal_slot);
    if (local) {
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
    const u64 result = g_spawn(spawner, request, flag, a, b, c);
    // O spawner pode liberar sua referencia ao slot logo depois de criar o
    // carro. O pedido nativo continua apontando para a instancia criada.
    if (local) {
        if (const u64 inst = SpawnedInstance(request); inst && !IsPuppetFast(inst)) {
            RememberLocalVehicle(inst, request, portal_slot, vehicle_model);
        }
    }
    return result;
}

// ---- 4.5: eventos de interface (dialogo, camera, cutscene, minigame) ---------------------------------------------
// Acoes replicadas podem executar a camera de uma fase. So os eventos de interface
// modal e de cinematica completa devem ser filtrados durante a chamada do motor.
thread_local bool g_quest_silent = false;
thread_local bool g_applying = false;
u64 g_find_gizmo_fn = 0;
u64 g_play_gizmo_fn = 0;
u64 g_script_spawn_fn = 0;
constexpr u64 kGizmoAboutToPlayEvent = 0x3174710;
constexpr u64 kVehicleCreatedEvent = 0x30e1500;
constexpr u64 kVehicleRemovedEvent = 0x30e1480;
std::map<std::pair<std::string, int>, double> g_recent_gizmos;

std::string GizmoName(u64 gizmo) {
    std::string name;
    for (u64 i = 0; i < 48 && Readable(gizmo + 0x88 + i, 1); ++i) {
        const char c = At<char>(gizmo + 0x88 + i);
        if (!c) return name;
        if (c < ' ' || c > '~' || c == '|') return {};
        name += c;
    }
    return {};
}

int GizmoClipIndex(u64 gizmo, u64 clip) {
    if (!gizmo || !clip) return -1;
    u64 node = ReadPointer(gizmo + 0x198);
    for (int i = 0; i < 64 && node && Readable(node, 0x18); ++i) {
        if (At<u64>(node + 0x10) == clip) return i;
        node = At<u64>(node);
    }
    return -1;
}

void OnGameplayCutscene(u64 payload) {
    if (GetCurrentThreadId() != g_main_tid || !g_play_gizmo_fn || !Readable(payload, 0x20)) return;
    const u64 gizmo = At<u64>(payload + 0x10);
    const int clip = GizmoClipIndex(gizmo, At<u64>(payload + 0x18));
    if (clip < 0) return;
    const std::string name = GizmoName(gizmo);
    if (name.empty()) return;
    const double now = runtime::mp_net::Now();
    std::erase_if(g_recent_gizmos, [&](const auto& entry) { return now - entry.second > 8.0; });
    g_recent_gizmos[{name, clip}] = now;
    if (!g_applying) Announce("g|" + CurrentArea() + "|" + name + "|" + std::to_string(clip));
}

bool IsModalEventName(const char* name) {
    if (std::strstr(name, "GizCutscene") || std::strstr(name, "GizmoCutscene")) return false;
    static const char* const modal[] = {"Dialog", "Speech", "Subtitle", "Cutscene", "CutScene", "Cinematic", "Movie",
                                        "Keystone", "Hack", "Minigame", "MiniGame", "Wheel", "Terminal", "Prompt",
                                        "Menu", "Hud", "HUD", "Banner", "Tutorial"};
    for (const char* part : modal) {
        if (std::strstr(name, part)) return true;
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
    const bool silent = g_quest_silent;
    const u64 type = ev - reinterpret_cast<u64>(g_base);
    if (g_enabled && type == kGizmoAboutToPlayEvent) OnGameplayCutscene(payload);
    if (type == kVehicleRemovedEvent && payload) ForgetLocalVehicle(ReadPointer(payload + 0x10));
    if (type == kVehicleCreatedEvent && payload && !g_spawning) {
        const u64 inst = ReadPointer(payload + 0x10);
        if (!IsPuppetFast(inst)) RememberLocalVehicle(inst);
    }
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
    if (silent && name[0] && IsModalEventName(name)) return 0;
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
struct WorldReplay {
    explicit WorldReplay(bool suppress_modal = true)
        : was_applying(g_applying), was_silent(g_quest_silent) {
        g_applying = true;
        g_quest_silent = suppress_modal;
    }
    ~WorldReplay() {
        g_quest_silent = was_silent;
        g_applying = was_applying;
    }
    const bool was_applying, was_silent;
};

// classes de objeto do level (todas com o nome em +0xf0): explodivel, objeto simples, gatilho, multiestado,
// obstaculo, pedra de dica, manivela, gancho, ponto de salvar, impulso de veiculo
// 4.2: todas as classes de gadget do eboot (vtable >70% igual a do multiestado): fixit, plataformas, escadas,
// empurraveis, transmat, montagens (reform 2cbeae0, 2cbfba0)...
constexpr u64 kGadgetVtables[] = {0x2c0d3e0, 0x2c13830, 0x2c16120, 0x2c0e3e0, 0x2c11930, 0x2c1ad70, 0x2c12de0,
                                  0x2c16f80, 0x2c19070, 0x2ca6c70, 0x2cbeae0, 0x2cbfba0, 0x2c0c430, 0x2c0ee40,
                                  0x2c0f8a0, 0x2c10520, 0x2c12390, 0x2c142a0, 0x2c15480, 0x2c92820, 0x2cacc30,
                                  0x2cafc30, 0x2cb2300, 0x2cb33e0, 0x2cbaa70, 0x2cc2180};
constexpr u64 kReformVtables[] = {0x2cbeae0, 0x2cbfba0};
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
    if (!Readable(ctx + 0xf0, 8)) return {};
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

constexpr u64 kScriptSpawnerVtable = 0x2d6e640;
constexpr u64 kSpawnerProduced = 0x28c;
constexpr u64 kSpawnerPending = 0x2a8;
std::vector<u64> g_wave_spawners; // ultima varredura, sob g_gadget_mutex

constexpr u64 kCharacterVtable = 0x2bdf860; // personagens (jogadores, capangas, Doc Brown...): nome em +0x10
std::vector<u64> g_characters; // da ultima varredura (g_gadget_mutex)
std::vector<u64> g_builds;     // montinhos de pecas (g_gadget_mutex)
constexpr u64 kHubFlightAddonVtable = 0x2cc5500, kLevelFlightAddonVtable = 0x2cc5940;
std::vector<u64> g_flight_addons;
constexpr u64 kShiftAreaVtable = 0x2c9b710, kShiftAddonVtable = 0x2c9c220;
constexpr u64 kShiftAddonId = 0x3153638;
u64 g_shift_position_fn = 0, g_shift_queue_fn = 0, g_shift_arrive_fn = 0;
u64 g_character_addons_fn = 0, g_find_addon_fn = 0;
using ShiftFn = void(HLE_SYSV_ABI*)(u64, u64);
ShiftFn g_shift_arrive = nullptr;
std::vector<u64> g_shift_areas;
unsigned g_shift_area_epoch = 0;

struct ShiftReplay {
    u64 target;
    double time;
};
std::map<u64, ShiftReplay> g_shift_replays;
std::map<u64, ShiftReplay> g_shift_arrivals;

bool ShiftPosition(u64 target, std::array<float, 4>& position) {
    if (!g_shift_position_fn || !Readable(target, 0xe0)) return false;
    const u64 marker = ReadPointer(target + 0xb8);
    if (!EngineMethod(marker, 0x68)) return false;
    Call(g_shift_position_fn, target, reinterpret_cast<u64>(position.data()));
    return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]);
}

HLE_SYSV_ABI void ShiftArriveHook(u64 target, u64 character_ref) {
    const u64 actor = character_ref > 0x370 ? character_ref - 0x370 : 0;
    if (GetCurrentThreadId() != g_main_tid || !runtime::mp_net::SelfId() || !IsLocalPlayer(actor)) {
        g_shift_arrive(target, character_ref);
        return;
    }
    const auto pending = g_shift_replays.find(actor);
    const bool replayed = pending != g_shift_replays.end() && pending->second.target == target &&
                          runtime::mp_net::Now() - pending->second.time <= 20.0;
    if (pending != g_shift_replays.end()) g_shift_replays.erase(pending);
    alignas(16) std::array<float, 4> position{};
    const bool identified = ShiftPosition(target, position);
    g_shift_arrive(target, character_ref);
    if (!identified) return;
    g_shift_arrivals[actor] = {target, runtime::mp_net::Now()};
    if (replayed) {
        std::printf("[MP] Shift: chegada da quest remota concluida neste jogo\n");
        std::fflush(stdout);
    } else if (!g_applying) {
        char marker[112];
        std::snprintf(marker, sizeof(marker), "%.3f,%.3f,%.3f", position[0], position[1], position[2]);
        Announce("h|" + CurrentArea() + "|" + LocalCharacter(actor) + "|" + marker);
        std::printf("[MP] Shift: chegada confirmada, destino %s enviado\n", marker);
        std::fflush(stdout);
    }
}

u64 ShiftAddon(u64 character) {
    if (!g_character_addons_fn || !g_find_addon_fn || !Readable(character, 0x500)) return 0;
    const u64 addons = Call(g_character_addons_fn, character);
    if (!addons) return 0;
    const u64 base = reinterpret_cast<u64>(g_base);
    const u64 addon = Call(g_find_addon_fn, addons, base + kShiftAddonId, 0);
    if (ReadPointer(addon) != base + kShiftAddonVtable || !Readable(addon, 0x110)) return 0;
    const u64 owner = ReadPointer(addon + 0xa8);
    const u64 getter = EngineMethod(owner, 0x570);
    return getter && Call(getter, owner) == character ? addon : 0;
}

u64 FindShiftTarget(float x, float y, float z) {
    std::vector<u64> areas;
    {
        std::lock_guard lock(g_gadget_mutex);
        if (g_shift_area_epoch != g_area_epoch) return 0;
        areas = g_shift_areas;
    }
    const u64 vtable = reinterpret_cast<u64>(g_base) + kShiftAreaVtable;
    u64 selected = 0;
    float best = 0.25f;
    for (u64 area : areas) {
        if (ReadPointer(area) != vtable) continue;
        for (u64 field : {0x1a8ull, 0x1b0ull, 0x1b8ull}) {
            const u64 target = ReadPointer(area + field);
            if (!target || !Readable(target + 0xc0, 4)) continue;
            const int state = At<s32>(target + 0xc0);
            if (state < 0 || state > 8) continue;
            alignas(16) std::array<float, 4> position{};
            if (!ShiftPosition(target, position)) continue;
            const float distance = std::hypot(x - position[0], y - position[1], z - position[2]);
            if (distance >= best) continue;
            best = distance;
            selected = target;
        }
    }
    return selected;
}

bool RunFlightAddon(u64 inst, bool start) {
    const u64 base = reinterpret_cast<u64>(g_base);
    u64 addon = 0, method = 0;
    {
        std::lock_guard lock(g_gadget_mutex);
        for (const u64 candidate : g_flight_addons) {
            const u64 vtable = ReadPointer(candidate);
            if ((vtable != base + kHubFlightAddonVtable && vtable != base + kLevelFlightAddonVtable) ||
                ReadPointer(candidate + 0x80) != inst || !ReadPointer(candidate + 0x88)) continue;
            method = ReadPointer(vtable + (start ? 0x2f0 : 0x300));
            if (method < base || method >= base + 0x2300000 || !Readable(method, 4) ||
                At<u32>(method) != 0xe5894855) continue;
            addon = candidate;
            break;
        }
    }
    if (!addon) return false;
    using FlightFn = void(HLE_SYSV_ABI*)(u64);
    reinterpret_cast<FlightFn>(method)(addon);
    return true;
}

void ScanGadgets() {
    const unsigned epoch = g_area_epoch;
    const u64 want_char = reinterpret_cast<u64>(g_base) + kCharacterVtable;
    const u64 want_spawner = reinterpret_cast<u64>(g_base) + kScriptSpawnerVtable;
    const u64 hub_flight = reinterpret_cast<u64>(g_base) + kHubFlightAddonVtable;
    const u64 level_flight = reinterpret_cast<u64>(g_base) + kLevelFlightAddonVtable;
    const u64 shift_area = reinterpret_cast<u64>(g_base) + kShiftAreaVtable;
    std::vector<u64> found, chars, builds_found, spawners, flight_addons, shift_areas;
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
                    else if (buf[i] == want_spawner) spawners.push_back(p + i * 8);
                    else if (buf[i] == hub_flight || buf[i] == level_flight) flight_addons.push_back(p + i * 8);
                    else if (buf[i] == shift_area) shift_areas.push_back(p + i * 8);
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
    g_wave_spawners.swap(spawners);
    g_flight_addons.swap(flight_addons);
    g_shift_areas.swap(shift_areas);
    g_shift_area_epoch = epoch;
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
std::vector<u64> GadgetsNamed(const std::string& name, u64 type = 0) {
    std::vector<u64> out;
    std::lock_guard lock(g_gadget_mutex);
    for (u64 g : g_gadgets) {
        if (ValidGadget(g) && (!type || At<u64>(g) == reinterpret_cast<u64>(g_base) + type) &&
            GadgetName(g) == name && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }
    std::sort(out.begin(), out.end());
    return out;
}

SetupFn g_script_spawn = nullptr;

bool IsScriptSpawner(u64 spawner) {
    return Readable(spawner, 0x2b0) && At<u64>(spawner) == reinterpret_cast<u64>(g_base) + kScriptSpawnerVtable;
}

u64 FindScriptSpawner(const std::string& name) {
    u64 match = 0;
    std::lock_guard lock(g_gadget_mutex);
    for (const u64 spawner : g_wave_spawners) {
        if (!IsScriptSpawner(spawner) || GadgetName(spawner) != name) continue;
        if (match && match != spawner) return 0;
        match = spawner;
    }
    return match;
}

HLE_SYSV_ABI u64 ScriptSpawnHook(u64 spawner, u64 count, u64 c, u64 d, u64 e, u64 f) {
    const unsigned epoch = g_area_epoch.load();
    const u64 result = g_script_spawn(spawner, count, c, d, e, f);
    if (g_applying || !g_enabled || !static_cast<int>(result) ||
        epoch != g_area_epoch || GetCurrentThreadId() != g_main_tid || !IsScriptSpawner(spawner)) return result;
    const int self = runtime::mp_net::SelfId();
    if (self && self == runtime::mp_net::OwnerId()) {
        const std::string name = GadgetName(spawner);
        const int total = At<s32>(spawner + kSpawnerProduced);
        if (!name.empty() && total > 0) Announce("n|" + CurrentArea() + "|" + name + "|" + std::to_string(total));
    }
    return result;
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
// O estado concluido de um objetivo vale para a sala toda. A entrada em um
// minigame e as janelas modais continuam locais.
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
    unsigned epoch;
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
    unsigned epoch;
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
        if (g_reform_changes.size() < 128) g_reform_changes.push_back({obj, GadgetState(obj), g_area_epoch.load()});
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
    if (!g_applying && g_enabled && Readable(obj, 0x460) && ValidGadget(obj)) {
        std::lock_guard lock(g_ms_mutex);
        if (g_ms_changes.size() < 256) g_ms_changes.push_back({obj, static_cast<int>(state), static_cast<int>(param), g_area_epoch.load()});
    }
    return g_ms_request(obj, state, param, force, immediate, f);
}

HLE_SYSV_ABI u64 TriggerObstacleHook(u64 unused, u64 ctx, u64 who, u64 a, u64 b, u64 c) {
    const int self = runtime::mp_net::SelfId();
    const bool component = who > 0x370 && IsLocalPlayer(who - 0x370);
    if (!g_applying && self && ValidGadget(ctx) && (IsLocalPlayer(who) || component)) {
        const std::string name = GadgetName(ctx);
        if (!name.empty()) {
            const u64 type = At<u64>(ctx) - reinterpret_cast<u64>(g_base);
            const std::vector<u64> same = GadgetsNamed(name, type);
            const auto it = std::find(same.begin(), same.end(), ctx);
            const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
            const char* actor_form = component ? "component" : "character";
            Announce("o|" + CurrentArea() + "|" + name + "|" + std::to_string(order) + "|" +
                     std::to_string(type) + "|" + actor_form);
            std::printf("[MP] acionei %s (#%d): avisando a sala\n", name.c_str(), order);
            std::fflush(stdout);
        }
    }
    return g_trigger_obstacle(unused, ctx, who, a, b, c);
}

HLE_SYSV_ABI u64 RequestDtorHook(u64 request, u64 a, u64 b, u64 c, u64 d, u64 e) {
    ForgetLocalVehicle(0, request);
    if (IsOurRequest(request)) {
        const u64 inst = SpawnedInstance(request);
        if (inst) {
            LiveInstDrop(inst);
            if (!IsMechVehicle(inst) && Readable(inst + 0x399, 0x10) && !IsLocalPlayer(inst)) {
                At<u8>(inst + 0x399) = 0;
                if (IsOurFakeCtrl(At<u64>(inst + 0x3a0))) At<u64>(inst + 0x3a0) = 0;
            }
        }
    }
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
int SlotIndexOf(u64 slot);

void PruneReplicaSlots() {
    for (auto it = g_our_slots.begin(); it != g_our_slots.end();) {
        const u64 slot = it->first;
        const int index = SlotIndexOf(slot);
        if (index < 0 || SlotState(index) <= 0) {
            ForgetReplicaSlot(slot);
            it = g_our_slots.erase(it);
        } else {
            ++it;
        }
    }
}

bool LocalPlayerUses(const std::string& slot_name);
bool PortalSlotsAllowed();

u64 FindLoadedSlot(const std::string& chr, bool vehicle = false) {
    const std::string name = Lower(chr);
    const std::string label = name + ".cd";
    // Uma replica so nasce de um slot que foi carregado para as replicas.
    for (const auto& [slot, model] : g_our_slots) {
        if (model != name) continue;
        const int index = SlotIndexOf(slot);
        if (index >= 0 && (vehicle || index >= g_nslots || IsReplicaSlot(slot) || PortalSlotsAllowed()) &&
            SlotState(index) == 1 && SlotName(index) == label) return slot;
    }
    return 0;
}

int FreeSlots() { // do portal
    int n = 0;
    for (int i = 0; i < g_nslots; ++i) n += SlotState(i) == 0;
    return n;
}

bool SlotJobDone(u64 slot);

u64 FreeReplicaSlot(bool player) {
    const int first = player ? g_nslots : 0;
    const int last = player ? g_nall : g_nslots;
    for (int i = last - 1; i >= first; --i) {
        const u64 slot = SlotAddress(i);
        if (!slot || !Readable(slot, 0x2dc) || IsReplicaSlot(slot)) continue;
        // Os ultimos Player Load Slots usam memoria geral e nao carregam por aqui.
        if (SlotState(i) == 0 && At<s32>(slot + 0x2d8) <= 0 && SlotJobDone(slot) &&
            At<u32>(slot + 0x58) && !At<u8>(slot + 0x2d4) &&
            (!g_find_native_slot || (EngineMethod(slot, 0x18) && EngineMethod(slot, 0x30)))) return slot;
    }
    return 0;
}

u64 CharacterReplicaSlot(const std::string& chr) {
    const int free = FreeSlots();
    if (free < kKeepFreeSlots) return 0;
    const bool portal_preferred = PortalSlotsAllowed();
    const bool independent_load = g_begin_slot_load && g_find_native_slot;
    if (independent_load && portal_preferred && free > kKeepFreeSlots) {
        if (const u64 slot = FreeReplicaSlot(false)) return slot;
    }
    if (const u64 slot = FreeReplicaSlot(true)) return slot;
    // Quando as figuras coincidem, um slot fisico extra evita pegar o da party local.
    if (independent_load && free > kKeepFreeSlots && LocalPlayerUses(Lower(chr) + ".cd")) {
        return FreeReplicaSlot(false);
    }
    return 0;
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

void Log(const char* fmt, const std::string& a, const std::string& b = {});

bool LocalPlayerUses(const std::string& slot_name) {
    for (int k = 0; k < 2; ++k) {
        const u64 inst = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable + 8 * k);
        if (inst && Lower(LocalCharacter(inst)) + ".cd" == slot_name) return true;
    }
    for (int i = 0; i < g_nall; ++i) {
        if (SlotState(i) != 1 || SlotName(i) != slot_name) continue;
        const u64 slot = SlotAddress(i);
        if (slot && !g_our_slots.count(slot) && !IsReplicaSlot(slot)) return true;
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

// A carga de um slot corre em segundo plano (NuBgProc, trabalho em slot+0x88) e continua mesmo depois de o pedido ser
// solto (estado 2). Esvaziar o slot com a carga em andamento derrubava a NuBgProc (eboot+add9a0): era sempre no meio
// da carga de uma variante que um fantasma pediu (Homer bravo, o proximo Doctor, o chi do Laval). Como o jogo
// (eboot+157d9c0): so descarrega com o trabalho terminado, e sempre pelo estado 3, que espera o resto e leva a 0.
bool SlotJobDone(u64 slot) {
    if (!slot || !Readable(slot + 0x88 + 0x18, 8)) return false; // sem endereco de slot: nada a descarregar
    return At<u64>(slot + 0x88 + 0x18) == 0; // trabalho em andamento (eboot+d193b0)
}

bool SlotUsedByLocalPlayer(u64 slot);
void Unload(u64 slot);

void SlotMaintenance(double now) {
    for (int i = 0; i < g_nall; ++i) {
        const u64 slot = SlotAddress(i);
        const std::string name = SlotName(i);
        const bool loading_for_us = std::any_of(g_puppets.begin(), g_puppets.end(),
                                                [&](const auto& e) { return e.second.stage == Stage::kLoading && e.second.slot == slot; });
        // 4.4.23: NUNCA descarrega slots que o proprio jogador local pediu (carros ti_*, figuras recem-colocadas na
        // mesa). So cancela reservas presas que vieram de personagens nossos (g_our_slots, Ours ou OursFamily).
        const bool cancelable = g_our_slots.count(slot) != 0 || Ours(name) || OursFamily(name);
        if (SlotState(i) != 2 || loading_for_us || !cancelable || SlotUsedByLocalPlayer(slot) || PuppetUses(slot)) {
            g_stuck_since[i] = 0;
            continue;
        }
        if (!g_stuck_since[i]) g_stuck_since[i] = now;
        if (now - g_stuck_since[i] > 6.0 && SlotJobDone(slot)) {
            Log("[MP] descarregando a variante solta %s", name);
            if (IsReplicaSlot(slot)) Unload(slot);
            else Call(kSlotStateFn, slot, 3, 0); // o pedido foi solto (estado 2): o jogo faz o mesmo (eboot+157f10f)
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
    const u64 release = IsReplicaSlot(slot) ? EngineMethod(slot, 0x30) : 0;
    Call(kSlotStateFn, slot, 3, 0);
    if (release) Call(release, slot);
    // Mantem a identidade ate o motor esvaziar o slot.
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
            if (SlotState(i) != 1 || (!g_our_slots.count(slot) && !Ours(name)) || !Settled(slot) || SlotUsedByLocalPlayer(slot)) continue;
            Log("[MP] liberando o slot de %s", name);
            Unload(slot);
            return true;
        }
    }
    return false;
}

void Despawn(struct Puppet& p);
void ReleasePuppet(struct Puppet& p); // 4.4.32: solta o fantasma sem tocar no mundo do jogo
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
        if (SlotState(i) != 1 || !Ours(name) || SlotUsedByLocalPlayer(slot)) continue;
        if (pass == 0 && !Settled(slot)) continue;
        if (pass == 1) { // em uso: tira os fantasmas agora; o slot e descarregado (passo 0) quando assentar
            if (!PuppetUses(slot)) continue;
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
        if (SlotState(i) != 1 || (!g_our_slots.count(slot) && !Ours(name)) || SlotUsedByLocalPlayer(slot)) continue;
        float nearest = 1e9f;
        bool used = false;
        bool recent = false;
        bool reserve_only = true;
        for (auto& [id, q] : g_puppets) {
            if (q.slot == slot && q.stage != Stage::kNone) {
                used = true;
                reserve_only &= q.idx > 0 && q.idx < kFirstVehiclePuppet;
                nearest = std::min(nearest, q.distance);
                recent |= now - q.stage_time < 20.0; // apareceu ha pouco: fica pelo menos 20 s
            }
        }
        if (recent || (!used && !Settled(slot)) ||
            (wanting.idx > 0 && wanting.idx < kFirstVehiclePuppet && used && !reserve_only)) continue;
        if (wanting.idx == 0 && used && reserve_only) nearest = 1e8f;
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
    // 4.4.22: SUBSTITUTO DESLIGADO. Era ele que enchia a fase de personagens que nao estao na mesa: um fantasma sem
    // slot pegava um boneco nosso ja carregado - muitas vezes o carro do amigo ou a figura de outro fantasma - e ele
    // ficava plantado no chao. Sem slot, o amigo aparece so com o nome flutuante ate sobrar um.
    return 0;
    for (int i = 0; i < g_nall; ++i) {
        const u64 slot = SlotAddress(i);
        if (!slot) continue;
        const auto it = g_our_slots.find(slot);
        if (SlotState(i) == 1 && it != g_our_slots.end() && SlotName(i) == it->second + ".cd") return slot;
    }
    // 4.5: antes o substituto caia no slot do personagem do jogador local - o mesmo problema da party compartilhada
    // (Task 6). Sem um personagem NOSSO livre, so o nome flutuando aparece ate sobrar um slot.
    // 4.4.11: NAO usa slot de personagem alheio (era assim que o amigo aparecia vestido de outro personagem que ele
    // nem tinha equipado). Sem um personagem NOSSO livre, so o nome flutuando aparece.
    return 0;
}

// O slot (endereco) esta carregado com o personagem que o jogador 1/2 deste PC esta usando agora?
// 4.4.17: slot do portal = aparece na roda de figuras (o jogador local consegue pegar o personagem do outro).
// Por padrao o fantasma so usa slot de JOGADOR. LD_MP_SLOT=portal volta ao antigo.
bool PortalSlotsAllowed() {
    static const bool allowed = [] {
        const char* v = std::getenv("LD_MP_SLOT");
        return v && std::strcmp(v, "portal") == 0;
    }();
    return allowed;
}

int SlotIndexOf(u64 slot) {
    for (int i = 0; i < g_nall; ++i) {
        if (SlotAddress(i) == slot) return i;
    }
    return -1;
}

bool SlotUsedByLocalPlayer(u64 slot) {
    if (!slot) return false;
    for (int i = 0; i < g_nall; ++i) {
        if (SlotAddress(i) != slot) continue;
        const std::string name = SlotName(i);
        // 4.4.23: um Player Load Slot (i >= g_nslots) carregado por NOS (g_our_slots) e' dedicado ao fantasma, mesmo
        // quando os dois jogadores usam o mesmo personagem (ex.: dois Batmans independentes).
        const auto it = g_our_slots.find(slot);
        if (it != g_our_slots.end() && name == it->second + ".cd") {
            if (i >= g_nslots || IsReplicaSlot(slot)) return false;
            if (ValidVehicleName(it->second)) {
                // O jogo permite dois Batmoveis, desde que o pedido de cada um use seu slot.
                std::lock_guard lock(g_local_vehicle_mutex);
                return std::any_of(g_local_vehicles.begin(), g_local_vehicles.end(), [=](const LocalVehicle& v) {
                    return v.portal_slot == slot && VehicleIdentity(v.inst) == v.identity &&
                           (!v.request || SpawnedInstance(v.request) == v.inst);
                });
            }
        }
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

bool PickSpawner(u64* spawner, u64* request, bool vehicle) {
    std::lock_guard lock(g_spawner_mutex);
    const unsigned epoch = g_area_epoch;
    if (vehicle) {
        for (auto it = g_spawners.rbegin(); it != g_spawners.rend(); ++it) {
            if (it->epoch != epoch || !ValidSpawner(it->spawner) ||
                PortalVehicleModel(ReadPointer(it->spawner + 0x1f0)).empty()) continue;
            if (const u64 r = TemplateRequest(it->spawner); r && ReadPointer(r + 0x38)) {
                *spawner = it->spawner;
                *request = r;
                return true;
            }
        }
    }
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
    if (!PickSpawner(&spawner, &model, p.idx >= kFirstVehiclePuppet)) {
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
    p.spawn_level = g_level_key; // 4.4.32
    LiveAdd(request);
    std::printf("[MP] spawn: spawner %llx (slot %s) com slot %s%s\n", static_cast<unsigned long long>(spawner),
                SlotLabel(original_slot), SlotLabel(p.slot), p.standin ? " (substituto)" : "");
    std::fflush(stdout);
    g_spawning = true;
    g_spawning_flag_for_load = true;
    const bool previous_slot_work = g_replica_slot_work;
    g_replica_slot_work = true;
    // 4.4.23: flag = 0 (3o argumento edx em eboot+0x158c952): cria o personagem normalmente mas PULA 100% do registro
    // Party::AddCharacter (0x14b4e30). O fantasma nunca entra na party local nem na roda de figuras (triangulo/Y).
    g_spawn(spawner, request, 0, 0, 0, 0);
    g_replica_slot_work = previous_slot_work;
    // Registra a replica antes do proximo Tick que consulta os veiculos nativos.
    if (const u64 inst = SpawnedInstance(request); inst && !IsLocalPlayer(inst)) LiveInstAdd(inst);
    g_spawning_flag_for_load = false;
    g_spawning = false;
    At<u64>(spawner + 0x1f0) = original_slot;
    return true;
}

// Solta o no do pedido da lista do spawner, libera o no e apaga o pedido (o jogo desfaz o personagem).
bool PuppetAlive(const Puppet& p);

// 4.4.32: solta o fantasma SEM chamar nada do jogo. Serve para quando o mundo dele nao existe mais (fim de missao, troca
// de fase) ou quando ja pedimos outra fase: aí o pedido/spawner/instancia sao memoria reaproveitada e chamar o destrutor
// do jogo derrubava a partida inteira (foi o crash ao terminar uma missao e seguir para a proxima).
void ReleasePuppet(Puppet& p) {
    if (p.slot) g_slot_used[p.slot] = runtime::mp_net::Now();
    LiveDrop(p.request);
    LiveInstDrop(p.inst);
    p.request = p.inst = p.inst_name = 0;
    p.spawner = 0;
    p.slot = 0;
    p.stage = Stage::kNone;
    p.spawn_level.clear();
}

void Despawn(Puppet& p) {
    // 4.4.32: fantasma de outra fase (ou de um mundo ja desmontado): so solta, nunca mexe no jogo.
    if (!p.spawn_level.empty() && p.spawn_level != g_level_key) {
        ReleasePuppet(p);
        return;
    }
    const u64 request = p.request, spawner = p.spawner, inst = p.inst;
    struct DropInstOnExit {
        u64 i;
        ~DropInstOnExit() { LiveInstDrop(i); }
    } drop_guard{inst};
    // 4.4.12: se o personagem ja foi desfeito pelo jogo (troca de figura, portal, fim de fase), o pedido pode ter
    // sido reaproveitado por outra carga: nao chama o destrutor dele (derrubava o personagem do jogador / caia).
    const bool alive = inst && PuppetAlive(p);
    if (alive && !IsMechVehicle(inst) && !IsLocalPlayer(inst) && Readable(inst + 0x399, 0x10)) {
        At<u8>(inst + 0x399) = 0;
        if (IsOurFakeCtrl(At<u64>(inst + 0x3a0))) At<u64>(inst + 0x3a0) = 0;
    }
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
            if (!alive) { // 4.4.12: personagem ja desfeito: so tira o no da lista (nao ha o que desfazer)
                GameFree(node);
                return;
            }
            Call(kRequestDeleteFn, request); // passa pelo gancho do destrutor (ja zerado acima)
            GameFree(node);
            return;
        }
        node = At<u64>(node);
    }
}

bool IsVehicleName(const std::string& s) {
    const std::string low = Lower(s);
    return low.rfind("ti_", 0) == 0;
}

bool ValidVehicleName(const std::string& name) {
    return name.size() > 3 && name.size() < 48 && IsVehicleName(name) &&
           name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}

void ParseRemoteVehicles(const std::string& text, Remote& remote) {
    std::stringstream entries(text);
    std::string entry;
    while (remote.vehicles.size() < kMaxRemoteVehicles && std::getline(entries, entry, ';')) {
        std::stringstream fields(entry);
        std::string slot, name, x, y, z, fx, fz;
        if (!std::getline(fields, slot, ',') || !std::getline(fields, name, ',') ||
            !std::getline(fields, x, ',') || !std::getline(fields, y, ',') ||
            !std::getline(fields, z, ',') || !std::getline(fields, fx, ',') ||
            !std::getline(fields, fz, ',') || !ValidVehicleName(name)) continue;
        char* end = nullptr;
        const long id = std::strtol(slot.c_str(), &end, 10);
        if (end == slot.c_str() || *end || id < 0 || id >= kPuppetStride - kFirstVehiclePuppet) continue;
        const RemoteVehicle v{static_cast<int>(id), name, std::strtof(x.c_str(), nullptr), std::strtof(y.c_str(), nullptr),
                              std::strtof(z.c_str(), nullptr), std::strtof(fx.c_str(), nullptr), std::strtof(fz.c_str(), nullptr)};
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
        if (std::none_of(remote.vehicles.begin(), remote.vehicles.end(), [&](const auto& other) { return v.slot == other.slot; })) {
            remote.vehicles.push_back(v);
        }
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
            if (!one.empty() && one != "-" && one.size() < 48 && !IsVehicleName(one)) r->party.push_back(one);
        }
    }
    r->ctx_hash = f.size() > 16 ? static_cast<u32>(std::strtoul(f[16].c_str(), nullptr, 16)) : 0;
    if (f.size() > 17 && ValidVehicleName(f[17])) r->vehicle = f[17];
    if (f.size() > 18) ParseRemoteVehicles(f[18], *r);
    if (!r->vehicle.empty() && r->vehicles.empty()) {
        r->vehicles.push_back({0, r->vehicle, r->x, r->y, r->z, r->fx, r->fz});
    }
    r->valid = std::isfinite(r->x) && std::isfinite(r->y) && std::isfinite(r->z);
    return r->valid;
}

u64 Transform(u64 inst) { return Readable(inst + 0x1b0, 8) ? At<u64>(inst + 0x1b0) : 0; }

u64 PositionAddress(u64 inst) {
    if (!inst) return 0;
    const u64 t = Transform(inst);
    if (t && Readable(t + 0x90, 12)) return t + 0x90;
    return Readable(inst + 0x160, 12) ? inst + 0x160 : 0;
}

void Teleport(u64 inst, float x, float y, float z) {
    const u64 pos = PositionAddress(inst);
    if (!pos || !Readable(inst + 0x160, 12)) return;
    const float offset = At<float>(inst + 0x164) - At<float>(pos + 4); // a matriz propria fica um pouco abaixo
    At<float>(pos) = x;
    At<float>(pos + 4) = y;
    At<float>(pos + 8) = z;
    At<float>(inst + 0x160) = x;
    At<float>(inst + 0x164) = y + (std::isfinite(offset) ? offset : 0.0f);
    At<float>(inst + 0x168) = z;
}

bool PuppetAlive(const Puppet& p) {
    if (p.idx >= kFirstVehiclePuppet) {
        return p.inst && SpawnedInstance(p.request) == p.inst && VehicleIdentity(p.inst) == p.inst_name;
    }
    return p.inst && Readable(p.inst + 0x10, 0x500) && At<u64>(p.inst + 0x10) == p.inst_name;
}

u64 VehicleMatrix(u64 inst) {
    if (!IsMechVehicle(inst)) return 0;
    const u64 body = ReadPointer(inst + 0x48);
    const u64 transform = body ? ReadPointer(body + 0x70) : 0;
    return transform && Readable(transform + 0x30, 0x40) ? transform + 0x30 : 0;
}

void PlaceVehicle(u64 inst, float x, float y, float z, float fx, float fz) {
    const u64 matrix = VehicleMatrix(inst);
    if (!matrix) return;
    At<float>(matrix + 0x30) = x;
    At<float>(matrix + 0x34) = y;
    At<float>(matrix + 0x38) = z;
    const float heading_x = At<float>(matrix + 0x20), heading_z = At<float>(matrix + 0x28);
    const float current = std::hypot(heading_x, heading_z), desired = std::hypot(fx, fz);
    if (!std::isfinite(current) || !std::isfinite(desired) || current <= 0.01f || desired <= 0.01f) return;
    const float yaw = std::atan2(fx, fz) - std::atan2(heading_x, heading_z);
    const float sine = std::sin(yaw), cosine = std::cos(yaw);
    for (u64 row : {0ull, 0x10ull, 0x20ull}) {
        const float rx = At<float>(matrix + row), rz = At<float>(matrix + row + 8);
        At<float>(matrix + row) = cosine * rx + sine * rz;
        At<float>(matrix + row + 8) = cosine * rz - sine * rx;
    }
}

struct RemotePose {
    float x, y, z, fx, fz;
    int gear;
    float dx, dz;
};

RemotePose SampleRemotePose(const Puppet& p, double now) {
    const Remote& latest = p.cur;
    RemotePose pose{latest.x, latest.y, latest.z, latest.fx, latest.fz, latest.gear, 0.0f, 0.0f};
    if (p.hist.size() < 2) return pose;
    // Apresenta posicao, marcha e rumo no mesmo instante do buffer. O RTT nao data o pacote.
    const double shown = now - 0.11;
    auto after = std::upper_bound(p.hist.begin(), p.hist.end(), shown,
                                  [](double time, const auto& sample) { return time < sample.first; });
    if (after == p.hist.begin()) ++after;
    if (after == p.hist.end()) --after;
    const auto& [t0, a] = *std::prev(after);
    const auto& [t1, b] = *after;
    if (t1 <= t0) return pose;

    const float alpha = static_cast<float>(std::clamp((shown - t0) / (t1 - t0), 0.0, 1.0));
    const Remote& nearest = alpha < 0.5f ? a : b;
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    // Um teleporte ou uma pausa na conexao nao deve virar um percurso animado.
    if (t1 - t0 > 0.5 || std::hypot(dx, dz) > 8.0f || std::abs(dy) > 8.0f) {
        return {nearest.x, nearest.y, nearest.z, nearest.fx, nearest.fz, nearest.gear, 0.0f, 0.0f};
    }
    // O rumo de uma curva e a tangente do percurso. Suaviza a tangente com
    // as amostras adjacentes sem atravessar teleporte ou parar de costas.
    float heading_dx = dx, heading_dz = dz;
    if (std::hypot(dx, dz) > 0.002f) {
        const auto before = std::prev(after);
        const auto blend_tangent = [&](float x, float z) {
            if (std::hypot(x, z) <= 0.5f && dx * x + dz * z > 0.0f) {
                heading_dx += 0.35f * x;
                heading_dz += 0.35f * z;
            }
        };
        if (before != p.hist.begin()) {
            const auto& previous = std::prev(before)->second;
            blend_tangent(a.x - previous.x, a.z - previous.z);
        }
        if (const auto following = std::next(after); following != p.hist.end()) {
            blend_tangent(following->second.x - b.x, following->second.z - b.z);
        }
    }
    return {std::lerp(a.x, b.x, alpha), std::lerp(a.y, b.y, alpha), std::lerp(a.z, b.z, alpha),
            std::lerp(a.fx, b.fx, alpha), std::lerp(a.fz, b.fz, alpha), nearest.gear, heading_dx, heading_dz};
}

void UpdateRemote(Puppet& p, Remote state, double time) {
    if (!state.ingame || !p.cur.ingame || state.level != p.cur.level ||
        (state.level.empty() && state.area != p.cur.area) || state.chr != p.cur.chr ||
        state.vehicle != p.cur.vehicle || time - p.cur_time > 0.5) {
        p.hist.clear();
    }
    p.hist.emplace_back(time, state);
    while (p.hist.size() > 12) p.hist.pop_front();
    p.cur = std::move(state);
    p.cur_time = time;
}

void Steer(Puppet& p, double now, Drive* d) {
    const Remote& r = p.cur;
    const double age = now - p.cur_time;
    const RemotePose pose = SampleRemotePose(p, now);
    if (IsMechVehicle(p.inst)) {
        PlaceVehicle(p.inst, pose.x, pose.y, pose.z, pose.fx, pose.fz);
        d->inst = 0;
        return;
    }
    const u64 pos = PositionAddress(p.inst);
    if (!pos) {
        d->inst = 0;
        return;
    }
    const float px = At<float>(pos), pz = At<float>(pos + 8);
    Teleport(p.inst, pose.x, pose.y, pose.z);
    d->inst = p.inst;
    d->x = pose.x;
    d->y = pose.y;
    d->z = pose.z;
    d->name = p.inst_name;
    d->vehicle = p.idx >= kFirstVehiclePuppet;
    if (d->vehicle) {
        const float facing = std::hypot(pose.fx, pose.fz);
        d->fx = std::isfinite(facing) && facing > 0.01f ? pose.fx / facing : 0.0f;
        d->fz = std::isfinite(facing) && facing > 0.01f ? pose.fz / facing : 1.0f;
        SetReplicaFacing(p.inst, d->fx, d->fz);
        return;
    }
    d->ctx_hash = r.ctx_hash;
    d->ground = r.ctx_hash == 0 || r.ctx_hash == kCtxDefault;
    // 4.4.10: a posicao continua sendo escrita aqui todo quadro (espelho), mas com rapidez e direcao de verdade: sem
    // isso o animador nao tem velocidade e toca a animacao de PARADO enquanto o boneco desliza. O erro de um quadro
    // de caminhada e' refeito no quadro seguinte pelo Teleport.
    d->speed = 0.0f; // 0 = a rapidez da tabela da marcha (e' dela que sai a animacao)
    d->hold = false;
    d->gear = age < 0.5 ? std::clamp(pose.gear, 3, 7) : 3;
    d->held = age < 1.0 ? (r.held & kSentButtons) : 0;
    const u32 edge = d->held & ~p.prev_held;
    const bool ctx_changed = age < 0.5 && r.ctx_hash != 0 && r.ctx_hash != p.last_remote_ctx;
    if (now > p.pending_action_until) p.pending_action = 0;
    p.last_remote_ctx = r.ctx_hash;

    if (r.ctx_hash != kCtxLandSlam &&
        ((edge & kSquare) || (ctx_changed && (r.ctx_hash == kCtxFighting || r.ctx_hash == kCtxConeAttack)))) {
        p.pending_melee = true;
        p.punch_until = now + 0.35;
        std::printf("[MP] soco do outro jogo (%s) botao %x ctx %x\n", p.name.c_str(), edge, r.ctx_hash);
        std::fflush(stdout);
    }
    if ((edge & kCircle) || (ctx_changed && r.ctx_hash == kCtxSpecialMove)) {
        p.pending_special = true;
        p.punch_until = now + 0.35;
    }
    if ((edge & kCross) || (ctx_changed && r.ctx_hash == kCtxJumping)) {
        p.pending_jump = true;
        p.jump_until = now + 0.45;
    }
    if (ctx_changed && SharedAction(r.ctx_hash)) {
        p.pending_action = r.ctx_hash;
        p.pending_action_until = now + 0.65;
        if (r.ctx_hash == kCtxLandSlam) p.pending_slam = true;
    }
    d->jump_active = now < p.jump_until;

    // 4.4.23: quando o outro jogo ja entrou em voo ("Flying") e o fantasma ainda esta no chao/pulo, gera um pulso
    // duplo limpo de X para o FlightComponent iniciar o voo real com os parametros da figura.
    const u32 local_ctx = ActiveContextHash(p.inst);
    if (age < 1.0 && r.ctx_hash == kCtxFlying && local_ctx != kCtxFlying) {
        if (p.fly_pulse_start < 0 && now >= p.fly_cooldown) {
            p.fly_pulse_start = now;
            p.fly_cooldown = now + 1.2;
        }
    } else if (local_ctx == kCtxFlying || r.ctx_hash != kCtxFlying) {
        p.fly_pulse_start = -100;
    }
    if (p.fly_pulse_start > 0) {
        const double dt_fly = now - p.fly_pulse_start;
        if (dt_fly < 0.06) d->held |= kCross;
        else if (dt_fly < 0.13) d->held &= ~kCross;
        else if (dt_fly < 0.22) d->held |= kCross;
        else p.fly_pulse_start = -100;
    }
    p.prev_held = d->held;
    d->punching = now < p.punch_until || local_ctx == kCtxFighting || local_ctx == kCtxConeAttack;
    if (d->punching) d->gear = 3; // parado: o motor toca o soco
    const float motion = std::hypot(pose.dx, pose.dz);
    const float facing = std::hypot(pose.fx, pose.fz);
    const bool has_facing = std::isfinite(facing) && facing > 0.01f;
    d->fx = has_facing ? pose.fx / facing : 0.0f;
    d->fz = has_facing ? pose.fz / facing : 1.0f;
    const bool moving = motion > 0.004f && age < 0.5;
    if (moving) {
        const float mx = pose.dx / motion, mz = pose.dz / motion;
        // Marcha e orientacao sao dirigidas pela tangente do mesmo percurso.
        if (!has_facing || (d->ground && !d->punching && d->gear > 3)) {
            d->fx = mx;
            d->fz = mz;
        }
    }
    // O controle falso recebe mundo. A conversao de teste para corpo vale so para o campo do motor.
    // A animacao usa o mesmo rumo do corpo, como na 4.4.12.
    d->dx = d->gear > 3 || moving ? d->fx : 0.0f;
    d->dz = d->gear > 3 || moving ? d->fz : 0.0f;
    if (d->ground && !d->punching && (local_ctx == 0 || local_ctx == kCtxDefault)) {
        ApplyGroundLocomotion(p.inst, *d);
        SetReplicaFacing(p.inst, d->fx, d->fz);
    }
    p.move_dx = pose.dx;
    p.move_dz = pose.dz;
    p.last_px = px;
    p.last_pz = pz;
    p.last_dist = std::hypot(pose.x - px, pose.z - pz);
    p.last_wx = d->dx;
    p.last_wz = d->dz;
    p.have_last = true;
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
u64 CurrentLevelState() {
    u64 p = ReadPointer(reinterpret_cast<u64>(g_base) + kGameGlobal);
    for (u64 off : {0x120ull, 0x70ull, 0xdf8ull}) {
        if (!p) return 0;
        p = ReadPointer(p + off);
    }
    return p;
}

u64 CurrentLevelObj() {
    const u64 level = CurrentLevelState();
    const u64 obj = level ? ReadPointer(level + 0x18) : 0;
    return obj && Readable(obj, 0x88) ? obj : 0;
}

// A lista de GizCutscenes pertence ao nivel carregado. O script original procura
// o nome pela mesma tabela antes de acionar a cena de camera.
u64 FindGameplayCutscene(const std::string& name) {
    if (!g_find_gizmo_fn || !g_play_gizmo_fn || name.empty() || name.size() >= 48) return 0;
    constexpr u64 kGizmoClass = 0x327e5a0, kClassRegistry = 0x327ea20;
    const u64 base = reinterpret_cast<u64>(g_base);
    const u64 category = ReadPointer(base + kGizmoClass);
    const u64 registry = ReadPointer(base + kClassRegistry);
    if (!category || !registry || !Readable(category + 0xbc, 4) || !Readable(registry + 0x98, 4)) return 0;
    const int index = At<s32>(category + 0xbc);
    if (index < 0 || index >= At<s32>(registry + 0x98)) return 0;
    const u64 level = CurrentLevelState();
    const u64 resources = level ? ReadPointer(level + 0xa8) : 0;
    const u64 table = resources ? ReadPointer(resources + 0x88) : 0;
    const u64 manager = table ? ReadPointer(table + sizeof(u64) * static_cast<u64>(index)) : 0;
    if (!manager || !Readable(manager, 0xec)) return 0;
    const u64 gizmo = Call(g_find_gizmo_fn, manager, reinterpret_cast<u64>(name.c_str()));
    return gizmo && GizmoName(gizmo) == name ? gizmo : 0;
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

double g_level_request_time = -100; // quando este jogo pediu uma troca de fase (seguindo o dono)
u64 g_level_obj = 0;

// 4.4.31: "esta fase e' jogavel" (nao e' tela de carregamento, titulo, startup nem a cena do tubo). Usado tanto para
// avisar os outros quanto para decidir se podemos seguir a fase de alguem.
bool PlayableLevel(const std::string& key) {
    const std::string low = Lower(key);
    return !low.empty() && low != "-" && low.find("loadingscreen") == std::string::npos &&
           low.find("loading_screen") == std::string::npos && low.find("titles") == std::string::npos &&
           low.find("startup") == std::string::npos && low.find("slideways") == std::string::npos;
}

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
        const bool playable = PlayableLevel(key);
        if (g_enabled && playable && runtime::mp_net::SelfId()) runtime::mp_net::SendEvent("L|" + key);
    }
}

// Todos seguem o dono da sala: quando ele esta em outra fase (2 s parado nela), este jogo pede a mesma fase.
// Uma fase que nao deu (DLC que falta...) espera 60 s para tentar de novo.
std::atomic<bool> g_follow_owner{true};

// 4.5: ele ja esteve num mundo alguma vez nesta sessao (nao puxa o jogador para a fase do dono estando no menu).
std::atomic<bool> g_seen_world{false};
// 4.4.30: aviso de fase recebido enquanto este jogo ainda estava na tela inicial/menu (aplicado depois, com o jogador
// dentro do jogo). Antes ele era aplicado na hora e forcava o carregamento sem o save carregado.
std::string g_deferred_level;
double g_deferred_time = -100;

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
    // 4.4.30: o mesmo cuidado do aviso "L|": fora de uma fase (tela inicial, carregando o save, menu) este jogo
    // nunca e puxado para a fase do dono.
    if (!(g_seen_world && g_in_world && PlayableLevel(g_level_key))) return;
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
    // os fantasmas saem antes (a fase vai ser desmontada) - 4.4.32: sem chamar nada do jogo (o mundo esta de saida)
    for (auto& [pid, p] : g_puppets) ReleasePuppet(p);
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

// As figuras de reserva podem estar nos slots fisicos do portal ou nos slots de jogador.
// Vao no estado para os outros verem a party inteira, nao so o personagem ativo.
std::vector<std::string> LocalPartyNames(const std::string& active) {
    std::vector<std::string> out;
    const std::string active_name = Lower(active);
    for (int i = 0; i < g_nall && out.size() < 6; ++i) {
        if (SlotState(i) != 1) continue;
        std::string name = Lower(SlotName(i));
        if (!name.ends_with(".cd")) continue;
        name.resize(name.size() - 3);
        if (name.empty() || IsVehicleName(name) || name == active_name || name.ends_with("_bigfig") ||
            std::find(out.begin(), out.end(), name) != out.end()) continue;
        const u64 slot = SlotAddress(i);
        if (g_our_slots.count(slot) || IsReplicaSlot(slot)) continue;
        out.push_back(name);
    }
    return out;
}

u64 VehicleDescriptor(u64 handle) {
    if (!g_vehicle_resource_fn || !ReadPointer(handle)) return 0;
    const u64 desc = Call(g_vehicle_resource_fn, handle);
    const u64 type = EngineMethod(desc, 0x48);
    return type && static_cast<int>(Call(type, desc)) == 9 ? desc : 0;
}

u64 VehicleModelData(u64 descriptor) {
    const u64 properties = descriptor ? ReadPointer(descriptor + 0x388) : 0;
    return properties ? ReadPointer(properties + 0x10) : 0;
}

std::string VehicleModelString(u64 label) {
    std::string text;
    for (u64 i = 0; i < 200 && Readable(label + i, 1); ++i) {
        const char c = At<char>(label + i);
        if (!c) return VehicleModelLabel(text);
        if (c < ' ' || c > '~') break;
        text += c;
    }
    return {};
}

std::pair<u64, std::string> RegisteredVehicleModel(u64 body) {
    const u64 object = ReadPointer(body + 0x70);
    const u64 descriptor = object ? VehicleDescriptor(object + 0x10) : 0;
    const u64 data = VehicleModelData(descriptor);
    if (!Readable(data + 8, 0x18)) return {};
    const s32 id = At<s16>(data + 8);
    if (id < 0) return {};
    // A instancia de ApiVehicle e o slot do Toy Pad usam a mesma definicao.
    // O rótulo do slot conserva o nome do arquivo usado pelo carregador.
    for (int i = 0; i < g_nslots; ++i) {
        if (SlotState(i) != 1) continue;
        const u64 slot = SlotAddress(i);
        if (!slot || g_our_slots.count(slot)) continue;
        std::string model = PortalVehicleModel(slot);
        if (model.empty()) continue;
        const u64 slot_data = VehicleModelData(VehicleDescriptor(slot + 0x68));
        if (slot_data && Readable(slot_data + 8, 2) && At<s16>(slot_data + 8) == id) return {slot, std::move(model)};
    }
    if (std::string model = VehicleModelString(ReadPointer(data + 0x18)); !model.empty()) return {0, std::move(model)};
    return {0, VehicleModelString(ReadPointer(descriptor + 0x18))};
}

void DiscoverRegisteredVehicles(double now) {
    if (!g_vehicle_registry || !g_vehicle_resource_fn) return;
    static double next_scan = 0;
    if (now < next_scan) return;
    next_scan = now + 0.3;
    std::vector<u64> live;
    const u64 base = reinterpret_cast<u64>(g_base);
    for (u64 body = ReadPointer(base + g_vehicle_registry); body && live.size() < 128;
         body = ReadPointer(body + 0x118)) {
        if (!Readable(body, 0x228) || std::find(live.begin(), live.end(), body) != live.end()) break;
        live.push_back(body);
        const u64 component = ReadPointer(body + 0x220);
        if (component <= 0x10) continue;
        const u64 vehicle = component - 0x10;
        if (!IsMechVehicle(vehicle) || VehicleIdentity(vehicle) != body || IsPuppetFast(vehicle)) continue;
        auto [slot, model] = RegisteredVehicleModel(body);
        if (!model.empty()) RememberLocalVehicle(vehicle, 0, slot, std::move(model));
    }
    std::lock_guard lock(g_local_vehicle_mutex);
    std::erase_if(g_local_vehicles, [&](const LocalVehicle& vehicle) {
        return !vehicle.request && IsMechVehicle(vehicle.inst) &&
               std::find(live.begin(), live.end(), vehicle.identity) == live.end();
    });
}

u64 VehicleForPlayer(u64 player, const std::string& character) {
    if (ValidVehicleName(character)) return player;
    if (!player || !Readable(player, 0xab8)) return 0;
    // O proprio tratamento de entrada/saida do motor usa Character+0xab0
    // e a referencia ao veiculo em RidingData+0x630.
    const u64 riding = ReadPointer(player + 0xab0);
    const u64 vehicle = riding ? ReadPointer(riding + 0x630) : 0;
    if (!vehicle) return 0;
    const u64 base = reinterpret_cast<u64>(g_base);
    const u64 vtable = ReadPointer(vehicle);
    u64 mech = 0;
    if (vtable == base + kCharacterVtable) {
        const u64 getter = ReadPointer(vtable + 0x588);
        if (getter > base && getter < base + 0x2300000) mech = Call(getter - base, vehicle);
    }
    std::lock_guard lock(g_local_vehicle_mutex);
    for (const auto& entry : g_local_vehicles) {
        if (entry.identity != VehicleIdentity(entry.inst) ||
            (entry.request && SpawnedInstance(entry.request) != entry.inst)) continue;
        if (entry.inst == vehicle || entry.inst == mech ||
            (IsMechVehicle(entry.inst) && entry.identity == vehicle)) return entry.inst;
    }
    return !IsMechVehicle(vehicle) && ValidVehicleName(LocalCharacter(vehicle)) ? vehicle : 0;
}

bool VehiclePose(const LocalVehicle& vehicle, RemoteVehicle& pose) {
    const u64 inst = vehicle.inst;
    if (!inst || IsPuppetFast(inst) || VehicleIdentity(inst) != vehicle.identity) return false;
    const bool mech = IsMechVehicle(inst);
    const u64 matrix = VehicleMatrix(inst);
    const u64 pos = mech ? (matrix ? matrix + 0x30 : 0) : PositionAddress(inst);
    const u64 facing = mech ? (matrix ? matrix + 0x20 : 0) : inst + 0x170;
    if (!pos || !Readable(facing, 12)) return false;
    pose = {vehicle.slot, vehicle.model, At<float>(pos), At<float>(pos + 4), At<float>(pos + 8),
            At<float>(facing), At<float>(facing + 8)};
    return std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(pose.z);
}

std::string LocalVehicles(u64 driven) {
    std::vector<RemoteVehicle> vehicles;
    {
        std::lock_guard lock(g_local_vehicle_mutex);
        std::erase_if(g_local_vehicles, [&](const LocalVehicle& v) {
            if (v.request && SpawnedInstance(v.request) != v.inst) return true;
            RemoteVehicle pose{};
            if (VehicleIdentity(v.inst) != v.identity) return true;
            if (!VehiclePose(v, pose)) return false;
            if (v.inst == driven) vehicles.insert(vehicles.begin(), std::move(pose));
            else if (vehicles.size() < kMaxRemoteVehicles) vehicles.push_back(std::move(pose));
            return false;
        });
    }
    std::string result, models;
    for (size_t i = 0; i < vehicles.size() && i < kMaxRemoteVehicles; ++i) {
        const RemoteVehicle& v = vehicles[i];
        char item[144];
        std::snprintf(item, sizeof(item), "%d,%s,%.3f,%.3f,%.3f,%.3f,%.3f", v.slot, v.name.c_str(),
                      v.x, v.y, v.z, v.fx, v.fz);
        if (!result.empty()) {
            result += ';';
            models += ", ";
        }
        result += item;
        models += v.name;
    }
    static std::string last_models;
    if (models != last_models) {
        last_models = models;
        Log("[MP] veiculos locais enviados: %s", models.empty() ? "-" : models);
    }
    return result;
}

void SendLocal(u64 p1, double now) {
    std::string area;
    {
        std::lock_guard lock(g_area_mutex);
        area = g_area;
    }
    char text[768];
    const u64 pos = PositionAddress(p1);
    const u64 loc0 = p1 && Readable(p1 + 0x90, 8) ? At<u64>(p1 + 0x90) : 0;
    const u64 loc = loc0 && Readable(loc0 + 0x80, 0x1c0) ? loc0 : 0; // na troca de fase o ponteiro fica solto (lixo)
    const std::string active = LocalCharacter(p1);
    if (p1 && pos && !area.empty()) DiscoverRegisteredVehicles(now);
    const u64 driven = VehicleForPlayer(p1, active);
    std::string vehicle;
    if (driven) {
        RememberLocalVehicle(driven);
        std::lock_guard lock(g_local_vehicle_mutex);
        for (const auto& entry : g_local_vehicles) {
            if (entry.inst == driven) {
                vehicle = entry.model;
                break;
            }
        }
    }
    if (!p1 || !pos || (!loc && vehicle.empty()) || area.empty() || !Readable(p1 + 0x170, 0x240)) {
        std::snprintf(text, sizeof(text), "%s|-|0|0|0|3|0|1|0|1|0|0|0|%s", area.empty() ? "-" : area.c_str(), g_level_key.c_str());
        runtime::mp_net::SetLocalState(text);
        g_in_world = false;
        return;
    }
    g_in_world = true;
    g_seen_world = true; // 4.5: ja jogou num mundo (o seguidor de fase so age depois disto)
    // Quando o veiculo esta ativo, manda o motorista no campo original e o
    // modelo/pose do veiculo no campo de veiculos.
    static std::string s_last_figure;
    std::string sent_chr = active;
    if (driven && driven == p1) {
        const u64 linked = ReadPointer(p1 + 0xcc8);
        if (linked && Readable(linked, 0x500) && At<u64>(linked) == reinterpret_cast<u64>(g_base) + kCharacterVtable) {
            const std::string rider = LocalCharacter(linked);
            if (!rider.empty() && !IsVehicleName(rider)) sent_chr = rider;
        }
        if (IsVehicleName(sent_chr)) sent_chr = s_last_figure.empty() ? std::string("-") : s_last_figure;
    } else if (!sent_chr.empty()) {
        s_last_figure = sent_chr;
    }
    // 4.4.21: os botoes vem de varias fontes porque o jogo nao guarda o soco num lugar so: o controle do personagem
    // (party+0x24 segurados, +0x28 apertados agora) e o estado bruto apontado por party+0x18 (+0 atual / +4 anterior
    // / +8 apertados / +0xc soltos). Antes so uma delas era lida e o soco nunca chegava no outro lado.
    u32 held = 0, tapped = 0, raw[4] = {0, 0, 0, 0};
    const u64 party = At<u64>(p1 + 0x3a0);
    if (party && Readable(party + 0x18, 0x18)) {
        held |= At<u32>(party + 0x24);
        tapped |= At<u32>(party + 0x28) | At<u32>(party + 0x1c);
        if (const u64 holder = At<u64>(party + 0x18); holder && Readable(holder, 0x10)) {
            for (int k = 0; k < 4; ++k) {
                raw[k] = At<u32>(holder + 4 * k);
            }
            held |= raw[0];
            tapped |= raw[2];
        }
    }
    held &= kSentButtons;
    g_last_held.store(held);
    static const bool log_btn = std::getenv("LD_MP_DEBUG") != nullptr || std::getenv("LD_MP_ANIM") != nullptr;
    if (held != g_held_sent) {
        g_held_sent = held;
        if (log_btn) {
            std::printf("[MP] botoes do jogador local: %x\n", held);
            std::fflush(stdout);
        }
    }
    if (log_btn) {
        static u32 shown = 0xffffffffu;
        const u32 now_bits = held | raw[0] | (raw[2] << 8);
        if (now_bits != shown) {
            shown = now_bits;
            std::printf("[BTN] %x | controle: %x %x %x %x | bruto: %x %x %x %x\n", held,
                        party && Readable(party + 0x18, 0x18) ? At<u32>(party + 0x1c) : 0,
                        party && Readable(party + 0x18, 0x18) ? At<u32>(party + 0x20) : 0,
                        party && Readable(party + 0x18, 0x18) ? At<u32>(party + 0x24) : 0,
                        party && Readable(party + 0x18, 0x18) ? At<u32>(party + 0x28) : 0,
                        raw[0], raw[1], raw[2], raw[3]);
            std::fflush(stdout);
        }
    }
    // Um toque curto sobrevive ate o proximo envio, com prazo por botao.
    static u64 last_input_player = 0;
    if (p1 != last_input_player) {
        g_button_until.fill(-100);
        last_input_player = p1;
    }
    constexpr std::array<u32, 4> buttons{kCross, kSquare, kCircle, kSentButtons & ~(kCross | kSquare | kCircle)};
    u32 sent_buttons = held;
    for (size_t i = 0; i < buttons.size(); ++i) {
        if ((held | tapped) & buttons[i]) g_button_until[i] = now + 0.08;
        if (now < g_button_until[i]) sent_buttons |= buttons[i];
    }
    std::string party_list; // 4.5.1: "nome;nome;nome" dos outros personagens equipados
    for (const std::string& one : LocalPartyNames(sent_chr)) {
        if (!party_list.empty()) party_list += ';';
        party_list += one;
    }
    const u32 ctx_hash = vehicle.empty() ? ActiveContextHash(p1) : 0;
    std::snprintf(text, sizeof(text), "%s|%s|%.3f|%.3f|%.3f|%d|%.3f|%.3f|%.3f|%.3f|%x|1|%d|%s|%s|%s|%x", area.c_str(),
                  sent_chr.c_str(), At<float>(pos), At<float>(pos + 4), At<float>(pos + 8),
                  loc ? At<s32>(loc + 0x238) : 3, loc ? At<float>(loc + 0x80) : 0.0f,
                  loc ? At<float>(loc + 0x88) : 0.0f, At<float>(p1 + 0x170), At<float>(p1 + 0x178),
                  sent_buttons, runtime::mp_net::RttMs(), g_level_key.c_str(), LocalRegion().c_str(),
                  party_list.c_str(), ctx_hash);
    std::string packet = text;
    const std::string vehicles = LocalVehicles(driven);
    const std::string vehicle_header = "|" + vehicle + "|";
    // O relay aceita 600 bytes incluindo "s|". Preserve o estado antigo se os dados opcionais nao couberem.
    if (packet.size() + vehicle_header.size() <= 598) {
        packet += vehicle_header;
        const size_t available = 598 - packet.size();
        if (vehicles.size() <= available) packet += vehicles;
        else if (const size_t first_end = vehicles.find(';'); first_end <= available) packet += vehicles.substr(0, first_end);
    }
    runtime::mp_net::SetLocalState(packet);
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
                   &kPlayerBrainCtorFn, &kSetActionFn, &kStartMeleeFn, &kCanSwapFn, &kPartySwitchFn,
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
    // As funcoes opcionais seguem o mesmo deslocamento do spawner na tabela alta.
    // Confere parte do corpo tambem para nao confundir com o prologo vizinho.
    const u64 high_shift = kSpawnFn - 0x158c2c0;
    const auto checked = [](u64 offset, std::initializer_list<u8> code) {
        const u64 addr = reinterpret_cast<u64>(g_base) + offset;
        return Readable(addr, code.size()) && std::memcmp(g_base + offset, code.begin(), code.size()) == 0 ? offset : 0;
    };
    const auto native = [&](u64 offset, std::initializer_list<u8> code) {
        const u64 adjusted = checked(offset + high_shift, code);
        return adjusted ? adjusted : checked(offset, code);
    };
    const auto slot_load_callee = [&](u64 instruction, std::initializer_list<u8> prologue) -> u64 {
        const u64 ip = kSlotLoadFn + instruction;
        const u64 base = reinterpret_cast<u64>(g_base);
        if (!Readable(base + ip, 5) || g_base[ip] != 0xe8) return 0;
        const s64 target = static_cast<s64>(ip + 5) + At<s32>(base + ip + 1);
        return target > 0 && target < 0x2300000 ? checked(static_cast<u64>(target), prologue) : 0;
    };
    g_find_native_slot_fn = slot_load_callee(0xd1, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                   0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38, 0x48, 0x89, 0x4d, 0xb0});
    g_begin_slot_load = slot_load_callee(0x2bc, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xa8, 0, 0, 0, 0x49, 0x89, 0xf7, 0x49, 0x89, 0xfd});
    g_vehicle_resource_fn = native(0xd16de0, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x3b});
    // O spawner consulta a lista de ApiVehicle antes de aceitar uma tag repetida.
    for (u64 ref = kSpawnFn + 0x110; ref < kSpawnFn + 0x160; ++ref) {
        if (!checked(ref, {0x48, 0x8d, 0x05}) || !checked(ref + 7, {0x48, 0x8b, 0x18, 0x48, 0x85, 0xdb, 0x0f, 0x84})) continue;
        const s64 relative = static_cast<s64>(ref + 7) + At<s32>(reinterpret_cast<u64>(g_base) + ref + 3);
        if (relative < 0x2e00000 || relative >= 0x3400000) continue;
        g_vehicle_registry = static_cast<u64>(relative);
        break;
    }
    g_shift_position_fn = native(0x1271260, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x89, 0xf3});
    g_shift_queue_fn = native(0x12710c0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xf0});
    g_shift_arrive_fn = native(0x1273c70, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x50});
    g_character_addons_fn = native(0xd6bbd0, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x87, 0x80, 0x00, 0x00, 0x00});
    g_find_addon_fn = native(0xa3ed50, {0x48, 0x83, 0xc7, 0x08, 0x48, 0x85, 0xd2, 0x48, 0x8d, 0x42, 0x20, 0x48, 0x0f, 0x44, 0xc7});
    g_vehicle_facing_fn = checked(0x98a700, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec});
    g_find_gizmo_fn = checked(0x19b3f00 + high_shift, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                    0x55, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89, 0xf6});
    g_play_gizmo_fn = checked(0x197f0f0 + high_shift, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb,
                                                    0xc5, 0xf8, 0x57, 0xc0});
    g_script_spawn_fn = checked(0x1ba1b70 + high_shift, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68});

    // 4.4.23: resolve g_ContextRegistry (RIP-relativo em kSetActionFn+0xc1) e g_main_camera_ptr (via call fn_getcam em
    // g_control_fn + 0x117a -> primeiro lea rax, [rip+disp32]) de forma 100% independente de USA x EUR.
    {
        const u64 base = reinterpret_cast<u64>(g_base);
        const u64 lea_reg = kSetActionFn + 0xc1;
        if (Readable(base + lea_reg, 7) && g_base[lea_reg] == 0x48 && g_base[lea_reg + 1] == 0x8d && g_base[lea_reg + 2] == 0x05) {
            const s64 disp = At<s32>(base + lea_reg + 3);
            g_context_registry_ptr = static_cast<u64>(static_cast<s64>(lea_reg + 7) + disp);
        }
        const u64 call_getcam = g_control_fn + (0xd718aa - 0xd70730);
        if (Readable(base + call_getcam, 5) && g_base[call_getcam] == 0xe8) {
            const s64 call_disp = At<s32>(base + call_getcam + 1);
            const u64 fn_getcam = static_cast<u64>(static_cast<s64>(call_getcam + 5) + call_disp);
            if (Readable(base + fn_getcam, 7) && g_base[fn_getcam] == 0x48 && g_base[fn_getcam + 1] == 0x8d && g_base[fn_getcam + 2] == 0x05) {
                const s64 cam_disp = At<s32>(base + fn_getcam + 3);
                g_main_camera_ptr = static_cast<u64>(static_cast<s64>(fn_getcam + 7) + cam_disp);
            }
        }
    }
    if (kSpawnFn != 0x158c2c0 || control != kControlFn) {
        std::printf("[MP] eboot com as funcoes deslocadas em %+lld bytes: enderecos ajustados\n",
                    static_cast<long long>(kSpawnFn) - static_cast<long long>(0x158c2c0));
        std::fflush(stdout);
    }
    // 4.4.15: mostra no log quais opcoes de teste estao ligadas (aparece na aba MULTIPLAYER do F9)
    {
        std::string opts = "MP ligado";
        opts += g_player_mode.load() ? " | PLAYERMODE=1" : " | PLAYERMODE=0";
        if (const char* v = std::getenv("LD_MP_MOVE")) opts += std::string(" | MOVE=") + v;
        if (std::getenv("LD_MP_DEBUG")) opts += " | DEBUG=1";
        if (std::getenv("LD_MP_EVENTS")) opts += " | EVENTS=1";
        std::printf("[MP] opcoes: %s | ctx_reg=0x%llx cam=0x%llx\n", opts.c_str(),
                    static_cast<unsigned long long>(g_context_registry_ptr),
                    static_cast<unsigned long long>(g_main_camera_ptr));
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

// 4.4.17: se sobrou personagem nosso num slot do portal (rodas antigas), descarrega: a roda de figuras volta a ficar
// so com as SUAS figuras.
void UnloadOurPortalSlots() {
    if (PortalSlotsAllowed()) return;
    for (auto it = g_our_slots.begin(); it != g_our_slots.end();) {
        const int idx = SlotIndexOf(it->first);
        if (idx < 0 || idx >= g_nslots || IsReplicaSlot(it->first)) {
            ++it;
            continue;
        }
        if (kSlotStateFn) Call(kSlotStateFn, it->first, 3, 0); // 3 = descarrega
        std::printf("[MP] soltei o slot do portal %s (para nao aparecer na roda de figuras)\n", it->second.c_str());
        std::fflush(stdout);
        it = g_our_slots.erase(it);
    }
}

bool InstallHooks() {
    if (g_hooks_installed) return g_control && g_spawn && g_dtor;
    g_hooks_installed = true;
    UnloadOurPortalSlots(); // 4.4.17
    g_control = reinterpret_cast<ControlFn>(Hook(g_control_fn ? g_control_fn : kControlFn, reinterpret_cast<const void*>(&ControlHook)));
    g_set_action = reinterpret_cast<SetupFn>(Hook(kSetActionFn, reinterpret_cast<const void*>(&SetActionHook)));
    if (g_shift_arrive_fn && g_shift_position_fn && g_shift_queue_fn && g_character_addons_fn && g_find_addon_fn) {
        g_shift_arrive = reinterpret_cast<ShiftFn>(Hook(g_shift_arrive_fn, reinterpret_cast<const void*>(&ShiftArriveHook)));
    }
    std::printf("[MP] registro nativo de veiculos: %s | conclusao Shift: %s\n",
                g_vehicle_registry && g_vehicle_resource_fn ? "ativo" : "indisponivel",
                g_shift_arrive ? "ativa" : "indisponivel");
    std::fflush(stdout);
    g_spawn = reinterpret_cast<SpawnFn>(Hook(kSpawnFn, reinterpret_cast<const void*>(&SpawnHook)));
    if (g_script_spawn_fn) {
        g_script_spawn = reinterpret_cast<SetupFn>(Hook(g_script_spawn_fn, reinterpret_cast<const void*>(&ScriptSpawnHook)));
    }
    g_dtor = reinterpret_cast<DtorFn>(Hook(kRequestDtorFn, reinterpret_cast<const void*>(&RequestDtorHook)));
    if (const char* env = std::getenv("LD_MP_SLOTS")) g_extra_slots = std::clamp(std::atoi(env) - 7, 0, 1);
    g_setup = reinterpret_cast<SetupFn>(Hook(kLevelSetupFn, reinterpret_cast<const void*>(&LevelSetupHook))); // sem ele: so o portal
    if (g_find_native_slot_fn && g_begin_slot_load) {
        g_find_native_slot = reinterpret_cast<SetupFn>(Hook(g_find_native_slot_fn, reinterpret_cast<const void*>(&FindNativeSlotHook)));
    }
    std::printf("[MP] slots independentes para figuras repetidas: %s\n", g_find_native_slot ? "ativos" : "indisponiveis");
    std::fflush(stdout);
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
    if (!g_can_swap) g_can_swap = reinterpret_cast<SetupFn>(Hook(kCanSwapFn, reinterpret_cast<const void*>(&CanSwapHook)));
    if (!g_party_switch) g_party_switch = reinterpret_cast<SetupFn>(Hook(kPartySwitchFn, reinterpret_cast<const void*>(&PartySwitchHook)));

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
    ++g_world_session;
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
void SetPlayerMode(bool on) { g_player_mode = on; }
bool PlayerModeEnabled() { return g_player_mode; }

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

bool IsPuppet(u64 inst) { return IsPuppetFast(inst); }

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
    std::string payload = "k|" + CurrentArea() + "|" + name + "|" + std::to_string(order);
    if (const u64 t = Transform(obj); t && Readable(t + 0x90, 12)) {
        const float x = At<float>(t + 0x90), y = At<float>(t + 0x94), z = At<float>(t + 0x98);
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
            char position[96];
            std::snprintf(position, sizeof(position), "|%.3f|%.3f|%.3f", x, y, z);
            payload += position;
        }
    }
    Announce(payload);
    std::printf("[MP] derrubei %s (#%d): avisando a sala\n", name.c_str(), order);
    std::fflush(stdout);
}

// A sala repassa os avisos por WebSocket. O pacote da fase delimita a area e o
// destinatario permite atualizar quem acabou de chegar sem repetir a acao para todos.
std::mutex g_outgoing_world_mutex;
std::vector<std::pair<unsigned, std::string>> g_outgoing_world;
std::map<std::string, std::vector<std::string>> g_world_log;
std::string g_world_log_level;
unsigned g_world_serial = 0;

std::vector<std::string> SplitEvent(const std::string& value, size_t limit = 16) {
    std::vector<std::string> fields;
    size_t begin = 0;
    while (fields.size() + 1 < limit) {
        const size_t end = value.find('|', begin);
        if (end == std::string::npos) break;
        fields.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    fields.push_back(value.substr(begin));
    return fields;
}

void LogAreaEvent(const std::string& payload) {
    const auto fields = SplitEvent(payload);
    if (fields.size() < 4 || fields[0] == "o" || fields[0] == "k" || fields[0] == "g" ||
        fields[0] == "a" || fields[0] == "h") return;
    if (g_world_log_level != g_level_key) {
        g_world_log.clear();
        g_world_log_level = g_level_key;
    }
    auto& entries = g_world_log[fields[1]];
    if (fields[0] == "q" || fields[0] == "m" || fields[0] == "n") {
        const std::string prefix = fields[0] + "|" + fields[1] + "|" + fields[2] + "|" +
                                   (fields[0] == "m" ? fields[3] + "|" : "");
        std::erase_if(entries, [&](const auto& previous) { return previous.starts_with(prefix); });
    } else if (std::find(entries.begin(), entries.end(), payload) != entries.end()) {
        return;
    }
    if (entries.size() == 160) entries.erase(entries.begin());
    entries.push_back(payload);
}

void SendWorldEvent(const std::string& payload, int to = 0) {
    runtime::mp_net::SendEvent("W|" + std::to_string(++g_world_serial) + "|" + std::to_string(to) + "|" +
                               g_level_key + "|" + payload);
}

void Announce(const std::string& payload) {
    if (!runtime::mp_net::SelfId()) return;
    std::lock_guard lock(g_outgoing_world_mutex);
    if (g_outgoing_world.size() < 256) g_outgoing_world.emplace_back(g_area_epoch.load(), payload);
}

void FlushWorldEvents(unsigned epoch) {
    std::vector<std::pair<unsigned, std::string>> outgoing;
    {
        std::lock_guard lock(g_outgoing_world_mutex);
        outgoing.swap(g_outgoing_world);
    }
    if (!PlayableLevel(g_level_key)) return;
    for (const auto& [event_epoch, payload] : outgoing) {
        if (event_epoch != epoch) continue;
        SendWorldEvent(payload);
        LogAreaEvent(payload);
    }
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
        if (r.epoch != g_area_epoch || !Readable(r.obj, 0x300)) continue;
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
        if (c.epoch != g_area_epoch || !Readable(c.obj, 0x460) || !ValidGadget(c.obj)) continue;
        const std::string name = GadgetName(c.obj);
        if (name.empty() || name.rfind("VFX_", 0) == 0 || g_ms_noisy.count(name)) continue;
        auto& times = g_ms_times[name];
        times.push_back(now);
        while (!times.empty() && now - times.front() > 30.0) times.erase(times.begin());
        if (times.size() > 6) {
            g_ms_noisy.insert(name);
            Log("[MP] %s muda sozinho o tempo todo: fica fora da sincronizacao", name);
            continue;
        }
        const u64 type = At<u64>(c.obj) - reinterpret_cast<u64>(g_base);
        const std::vector<u64> same = GadgetsNamed(name, type);
        const auto it = std::find(same.begin(), same.end(), c.obj);
        const int order = it == same.end() ? 0 : static_cast<int>(it - same.begin());
        if (IsQuestGadget(name) || IsLocalOnlyGadget(name)) {
            Announce("q|" + CurrentArea() + "|" + name + "#" + std::to_string(order) + "|" +
                     std::to_string(c.state) + "|" + std::to_string(c.param) + "|" + std::to_string(type));
        } else {
            Announce("m|" + CurrentArea() + "|" + name + "|" + std::to_string(order) + "|" +
                     std::to_string(c.state) + "|" + std::to_string(c.param) + "|" + std::to_string(type));
        }
        std::fflush(stdout);
    }
}

struct PendingEvent {
    int from;
    std::string level, payload;
    double time;
};
std::vector<PendingEvent> g_pending_events;
std::map<int, unsigned> g_world_last_serial;

// Fase 80: aplica os avisos dos outros jogadores ("b|area|nome|ordem" = quebrou um objeto explodivel)
bool g_was_loading = false;        // 4.4.11: a fase atual e' tela de carregamento (os fantasmas ja foram desfeitos)
double g_push_request_time = -100; // 4.4.8: quando este jogo pediu a fase do outro por aviso ("L|")
bool g_push_retried = false;       // 4.4.10: ja pediu a mesma fase uma segunda vez
int g_push_target_id = -1;         // 4.4.10: numero da fase pedida, resolvido AQUI (USA/EUR numeram diferente)
std::string g_push_target;         // 4.4.10: a fase pedida (para o log)

// 4.4.30: "este jogo ja esta numa fase de verdade" (saiu da tela inicial e tem um mundo estavel). O seguidor de fase
// nunca pode puxar quem ainda esta no menu: isso forcava o carregamento sem o save e bagunçava a sessao inteira.
bool ThisGameInLevel() {
    // 4.4.31: tela inicial, "Titles", tela de carregamento e a cena do tubo NAO contam como estar numa fase - era por
    // aqui que o aviso ainda puxava o jogador no menu (o jogo informa "Titles#2" como fase atual).
    return g_seen_world && g_in_world && PlayableLevel(g_level_key) &&
           runtime::mp_net::Now() - g_level_request_time > 3.0;
}

// Aviso de fase do outro jogador, aplicado na hora (o jogo troca de fase). So deve ser chamado com ThisGameInLevel().
void GoToLevelNow(const std::string& key, double now) {
    if (key.empty() || key == g_level_key) return;
    if (now - g_level_request_time < 25.0) { // 4.4.11: ja estamos carregando uma fase (pedido duplo = tela branca)
        std::printf("[MP] aviso da fase %s ignorado: este jogo ja esta trocando de fase\n", key.c_str());
        std::fflush(stdout);
        return;
    }
    const int id = LevelIdForKey(key);
    if (id < 0) {
        std::printf("[MP] o outro entrou na fase %s e este jogo nao achou o numero dela\n", key.c_str());
        std::fflush(stdout);
        return;
    }
    std::printf("[MP] seguindo na hora: fase %s (id %d)\n", key.c_str(), id);
    std::fflush(stdout);
    for (auto& [pid, p] : g_puppets) ReleasePuppet(p); // 4.4.32: o mundo vai ser desmontado agora
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
    g_push_retried = false;
    g_push_target_id = id;
    g_push_target = key;
    g_deferred_level.clear();
}

// Aviso guardado enquanto estavamos fora de uma fase: aplica assim que o jogador entra no jogo (e so entao).
void ApplyDeferredLevel(double now) {
    if (g_deferred_level.empty()) return;
    if (g_deferred_level == g_level_key || now - g_deferred_time > 180.0) {
        g_deferred_level.clear();
        return;
    }
    if (!ThisGameInLevel() || now - g_level_request_time < 25.0) return;
    const std::string target = std::move(g_deferred_level);
    g_deferred_level.clear();
    std::printf("[MP] entrando na fase %s que ficou guardada\n", target.c_str());
    std::fflush(stdout);
    GoToLevelNow(target, now);
}

// 4.4.9: estado do multiplayer para o painel F9 (linha viva, atualizada ~2x por segundo na thread principal).
std::mutex g_summary_mutex;
std::string g_summary;

std::string DebugSummary() {
    std::lock_guard lock(g_summary_mutex);
    return g_summary;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

// Acha a posicao de um jogador pelo nome: aqui (este PC) ou um amigo da sala. false = nao achou.
bool PositionOfPlayer(const std::string& name, float& x, float& y, float& z) {
    if (Lower(name) == Lower(runtime::mp_net::SelfName())) {
        const u64 inst = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
        const u64 t = inst ? Transform(inst) : 0;
        if (!t) return false;
        x = At<float>(t + 0x90);
        y = At<float>(t + 0x94);
        z = At<float>(t + 0x98);
        return true;
    }
    for (const auto& peer : runtime::mp_net::Peers()) {
        if (Lower(peer.name) != Lower(name)) continue;
        const auto it = g_puppets.find(peer.id * kPuppetStride);
        if (it != g_puppets.end()) {
            if (const u64 t = it->second.inst ? Transform(it->second.inst) : 0) {
                x = At<float>(t + 0x90);
                y = At<float>(t + 0x94);
                z = At<float>(t + 0x98);
                return true;
            }
            if (it->second.cur.valid) { // ainda sem boneco: usa a posicao que veio no estado
                x = it->second.cur.x;
                y = it->second.cur.y;
                z = it->second.cur.z;
                return true;
            }
        }
    }
    return false;
}

// /tp <quem> to <para quem>  (ou "/tp <para quem>" = eu). O comando vai como mensagem do chat: cada jogo ve e so
// leva os SEUS personagens - quem foi citado. 4.4.9
void DoTeleport(const std::string& from, const std::string& to) {
    if (to.empty()) {
        std::printf("[MP] /tp: falta o nome de quem vai receber (use /tp <quem> to <para quem>)\n");
        std::fflush(stdout);
        return;
    }
    float x = 0, y = 0, z = 0;
    if (!PositionOfPlayer(to, x, y, z)) {
        std::printf("[MP] /tp: nao achei %s na sala\n", to.c_str());
        std::fflush(stdout);
        return;
    }
    if (!from.empty() && Lower(from) != Lower(runtime::mp_net::SelfName())) return; // nao sou eu: outro jogo leva
    u64 moved = 0;
    for (int k = 0; k < 2; ++k) { // os dois personagens deste PC (se houver dois jogadores locais)
        const u64 inst = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable + 8 * k);
        if (!inst || !Transform(inst)) continue;
        Teleport(inst, x + 0.55f * static_cast<float>(k), y, z - 0.55f * static_cast<float>(k));
        ++moved;
    }
    std::printf("[MP] /tp: %s foi para %s (%.2f %.2f %.2f) - %llu personagem(ns)\n",
                from.empty() ? "eu" : from.c_str(), to.c_str(), x, y, z, static_cast<unsigned long long>(moved));
    std::fflush(stdout);
}

// Le as mensagens do chat que ainda nao foram vistas e executa os comandos ("/tp ..."). Chamado no Tick.
void HandleChatCommands(double now) {
    static double last_time = -1;
    static bool started = false;
    const auto lines = runtime::mp_net::Chat();
    if (!started) { // a primeira vez so marca o fim do historico (comando antigo nao vale)
        if (!lines.empty()) last_time = lines.back().time;
        started = true;
        return;
    }
    for (const auto& l : lines) {
        if (started && l.time <= last_time) continue;
        if (l.text.rfind("/tp", 0) != 0) continue;
        last_time = l.time;
        std::string rest = Trim(l.text.substr(3));
        std::string a, b;
        const size_t to = rest.find(" to ");
        if (to != std::string::npos) {
            a = Trim(rest.substr(0, to));
            b = Trim(rest.substr(to + 4));
        } else {
            const size_t sp = rest.find(' ');
            if (sp == std::string::npos) {
                b = rest;
            } else {
                a = Trim(rest.substr(0, sp));
                b = Trim(rest.substr(sp + 1));
            }
        }
        DoTeleport(a, b);
    }
    if (!lines.empty()) last_time = std::max(last_time, lines.back().time);
    (void)now;
}

u64 WorldEventActor(int from) {
    const auto puppet = g_puppets.find(from * kPuppetStride);
    if (puppet != g_puppets.end() && puppet->second.stage == Stage::kActive && PuppetAlive(puppet->second)) {
        return puppet->second.inst;
    }
    return At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
}

u64 EventGadget(const std::string& name, int order, u64 type) {
    const std::vector<u64> objects = GadgetsNamed(name, type);
    if (order < 0 || static_cast<size_t>(order) >= objects.size()) return 0;
    return objects[order];
}

bool ApplyWorldEvent(const PendingEvent& event, const std::string& area, double now) {
    const auto f = SplitEvent(event.payload);
    if (f.size() < 4) return true;
    const std::string& kind = f[0];
    if (kind != "b" && kind != "k" && kind != "o" && kind != "m" && kind != "r" &&
        kind != "c" && kind != "q" && kind != "g" && kind != "n" && kind != "a" && kind != "h") return true;
    if (!ThisGameInLevel() || (!event.level.empty() && event.level != g_level_key) ||
        (f[1] != area && kind != "a")) return false;

    if (kind == "a") {
        const u32 action = static_cast<u32>(std::strtoul(f[3].c_str(), nullptr, 10));
        if (event.level.empty() || !SharedAction(action) || now - event.time > 1.5) return true;
        const auto sender = g_puppets.find(event.from * kPuppetStride);
        if (sender == g_puppets.end() || sender->second.stage != Stage::kActive) return false;
        Puppet& puppet = sender->second;
        if (puppet.standin || puppet.cur.chr != f[2] || puppet.spawn_level != g_level_key) return true;
        puppet.pending_action = action;
        puppet.pending_action_until = now + 0.65;
        return true;
    }
    if (kind == "h") {
        if (!g_shift_arrive || event.level.empty() || f.size() != 4) return true;
        float x, y, z;
        char trailing;
        if (std::sscanf(f[3].c_str(), "%f,%f,%f%c", &x, &y, &z, &trailing) != 3 ||
            !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return true;
        const u64 target = FindShiftTarget(x, y, z);
        if (!target) return false;
        const u64 base = reinterpret_cast<u64>(g_base);
        u64 actor = 0, addon = 0;
        for (int i = 0; i < 2; ++i) {
            const u64 candidate = ReadPointer(base + kPlayerTable + i * 8);
            const auto arrival = g_shift_arrivals.find(candidate);
            if (arrival != g_shift_arrivals.end() && arrival->second.target == target &&
                arrival->second.time >= event.time - 1.0) return true;
            const u64 candidate_addon = ShiftAddon(candidate);
            if (!candidate_addon) continue;
            if (!actor || LocalCharacter(candidate) == f[2]) {
                actor = candidate;
                addon = candidate_addon;
            }
        }
        if (!addon || At<s32>(target + 0xc0) < 2) return false;
        const u64 queued = ReadPointer(addon + 0xc0);
        if (queued) return false;
        g_shift_replays[actor] = {target, now};
        Call(g_shift_queue_fn, addon, target);
        std::printf("[MP] Shift: animacao nativa agendada para %s, destino %s\n", LocalCharacter(actor).c_str(), f[3].c_str());
        std::fflush(stdout);
        return false; // o aviso so termina quando o motor registra a chegada
    }

    const int order = std::atoi(f[3].c_str());
    if (kind == "n") {
        if (!g_script_spawn || event.from != runtime::mp_net::OwnerId() || order <= 0 || order > 4096) return true;
        const u64 spawner = FindScriptSpawner(f[2]);
        if (!spawner) return false;
        const int current = At<s32>(spawner + kSpawnerProduced);
        if (current < 0) return false;
        if (current < order) {
            WorldReplay replay;
            const int requested = std::min(16, order - current);
            const int made = static_cast<int>(Call(g_script_spawn_fn, spawner, static_cast<u64>(requested)));
            if (made <= 0 || made > requested || !IsScriptSpawner(spawner)) return false;
            // Os chamadores nativos da mesma funcao descontam a quantidade criada da fila da onda.
            const int pending = At<s32>(spawner + kSpawnerPending);
            if (pending > 0) At<s32>(spawner + kSpawnerPending) = std::max(0, pending - made);
            if (At<s32>(spawner + kSpawnerProduced) < order) return false;
        }
        LogAreaEvent(event.payload);
        return true;
    }
    if (kind == "g") {
        if (now - event.time > 6.0 || order < 0 || order >= 64 || !g_play_gizmo_fn) return true;
        const auto already = g_recent_gizmos.find({f[2], order});
        if (already != g_recent_gizmos.end() && now - already->second < 2.0) return true;
        const u64 gizmo = FindGameplayCutscene(f[2]);
        if (!gizmo) return false;
        u64 clip = ReadPointer(gizmo + 0x198);
        for (int i = 0; clip && i < order; ++i) clip = ReadPointer(clip);
        if (!clip || !ReadPointer(clip + 0x10)) return true;
        WorldReplay replay;
        Call(g_play_gizmo_fn, gizmo, static_cast<u64>(order));
        return true;
    }
    if (kind == "c") {
        const u64 build = FindBuild(f[2]);
        if (!build) return false;
        if (At<s32>(build + 0x24c) == 4) return true;
        const u64 actor = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
        const u64 first = At<u64>(build + 0x200);
        if (!actor || !Readable(actor, 0x500) || !first) return false;
        {
            std::lock_guard lock(g_build_mutex);
            if (!g_forced_builds.emplace(build, now).second) return true;
        }
        WorldReplay replay;
        Call8(kBuildStartFn, build, actor, 0, 0, static_cast<u64>(std::max(1, order)), 1, first,
              reinterpret_cast<u64>(g_base) + kBuildInputEvent);
        LogAreaEvent(event.payload);
        return true;
    }
    if (kind == "k") {
        if (now - event.time > 6.0) return true;
        const auto actors = CharactersNamed(f[2]);
        u64 victim = 0;
        if (f.size() >= 7) {
            const float x = std::strtof(f[4].c_str(), nullptr);
            const float y = std::strtof(f[5].c_str(), nullptr);
            const float z = std::strtof(f[6].c_str(), nullptr);
            float closest = 12.0f;
            for (const u64 candidate : actors) {
                const u64 t = Transform(candidate);
                if (!t || !Readable(t + 0x90, 12)) continue;
                const float distance = std::hypot(x - At<float>(t + 0x90), y - At<float>(t + 0x94), z - At<float>(t + 0x98));
                if (distance < closest) {
                    closest = distance;
                    victim = candidate;
                }
            }
        } else if (order >= 0 && static_cast<size_t>(order) < actors.size()) {
            victim = actors[order]; // pacote antigo
        }
        if (!victim) return false;
        const u64 actor = WorldEventActor(event.from);
        if (!actor) return false;
        WorldReplay replay;
        Call8(kKillObjectFn, victim, 3, actor, 0, 0, 0, 0, 0);
        return true;
    }
    if (kind == "q" || kind == "m") {
        if (kind == "m" && f.size() < 6) return true;
        std::string name = f[2];
        int instance = order;
        if (kind == "q") {
            const size_t hash = name.rfind('#');
            instance = hash == std::string::npos ? 0 : std::atoi(name.c_str() + hash + 1);
            if (hash != std::string::npos) name.resize(hash);
        }
        const size_t type_field = kind == "q" ? 5 : 6;
        const u64 type = f.size() > type_field ? std::strtoull(f[type_field].c_str(), nullptr, 10)
                                                : kMultiStateVtable;
        const u64 obj = EventGadget(name, instance, type);
        if (!obj || !Readable(obj, 0x460)) return false;
        const int state = kind == "q" ? order : std::atoi(f[4].c_str());
        const int param = f.size() >= type_field ? std::atoi(f[type_field - 1].c_str()) : 0;
        LogAreaEvent(event.payload);
        if (GadgetState(obj) != state) {
            WorldReplay replay;
            Call(kMsRequestFn, obj, static_cast<u64>(static_cast<s64>(state)),
                 static_cast<u64>(static_cast<s64>(param)), 0, 0);
        }
        return true;
    }
    const u64 type = kind == "b" ? kBlowUpVtable :
                     kind == "o" && f.size() > 4 ? std::strtoull(f[4].c_str(), nullptr, 10) : 0;
    const u64 obj = EventGadget(f[2], order, type);
    if (!obj) return false;
    if (kind == "r") {
        if (f.size() < 5 || !IsReform(obj)) return true;
        LogAreaEvent(event.payload);
        if (GadgetState(obj) == std::atoi(f[4].c_str())) {
            const u64 local = At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable);
            if (!local || !Readable(local, 0x500)) return false;
            WorldReplay replay;
            Call(kReformTriggerFn, 0, obj, local);
        }
        return true;
    }
    if (kind == "b" && (!Readable(obj + 0x260, 1) || At<u8>(obj + 0x260))) return true;
    // A alavanca aciona os scripts locais com um jogador real. O boneco remoto n?o
    // ? membro da party e os scripts da fase podem ignor?-lo.
    const u64 actor = kind == "o" ? At<u64>(reinterpret_cast<u64>(g_base) + kPlayerTable) : WorldEventActor(event.from);
    if (!actor || !Readable(actor, 0x500)) return false;
    WorldReplay replay;
    if (kind == "o") {
        const std::string form = f.size() > 5 ? f[5] : "character";
        const u64 source = form == "none" ? 0 : form == "component" ? actor + 0x370 : actor;
        Call(kTriggerObstacleFn, 0, obj, source);
        Log("[MP] %s acionado pelo outro jogo", f[2]);
    } else {
        Call8(kBlowUpFn, obj, actor, 0, 1, 0, 0, 0, 0);
        LogAreaEvent(event.payload);
    }
    return true;
}

void ReceiveEvents(const std::string& area, double now) {
    std::erase_if(g_shift_replays, [=](const auto& replay) { return now - replay.second.time > 20.0; });
    std::erase_if(g_shift_arrivals, [=](const auto& arrival) { return now - arrival.second.time > 20.0; });
    for (auto& [from, payload] : runtime::mp_net::TakeEvents()) {
        if (payload.starts_with("L|")) {
            const std::string key = payload.substr(2);
            if (!g_follow_owner || !PlayableLevel(key) || key == g_level_key) continue;
            const int owner = runtime::mp_net::OwnerId();
            if (owner && from != owner) continue;
            if (ThisGameInLevel()) GoToLevelNow(key, now);
            else {
                g_deferred_level = key;
                g_deferred_time = now;
            }
            continue;
        }
        PendingEvent event{from, {}, std::move(payload), now};
        if (event.payload.starts_with("W|")) {
            auto envelope = SplitEvent(event.payload, 5);
            if (envelope.size() != 5 || !PlayableLevel(envelope[3])) continue;
            const int to = std::atoi(envelope[2].c_str());
            if (to && to != runtime::mp_net::SelfId()) continue;
            const auto number = static_cast<unsigned>(std::strtoul(envelope[1].c_str(), nullptr, 10));
            auto& last = g_world_last_serial[from];
            if (!number || number <= last) continue;
            last = number;
            event.level = std::move(envelope[3]);
            event.payload = std::move(envelope[4]);
        }
        if (g_pending_events.size() < 512) g_pending_events.push_back(std::move(event));
    }

    static double last_retry = -100;
    if (g_pending_events.empty() || now - last_retry < 0.15) return;
    last_retry = now;
    if (!g_gadget_scan_busy && now - g_gadget_scan_time > 1.0) StartGadgetScan(now);
    std::erase_if(g_pending_events, [&](const auto& event) {
        if (ApplyWorldEvent(event, area, now)) return true;
        if (now - event.time < 20.0) return false;
        std::printf("[MP] aviso de %d expirou nesta fase (%s)\n", event.from, event.payload.c_str());
        return true;
    });
}

void LoadPuppetSlot(Puppet& p, double now, u64 dedicated, bool vehicle) {
    std::memset(p.load_buffer, 0, sizeof(p.load_buffer));
    const bool player_slot = dedicated && SlotIndexOf(dedicated) >= g_nslots;
    At<s32>(reinterpret_cast<u64>(p.load_buffer) + 96) = player_slot ? 1 : 0; // tipo: 0 portal, 1 jogador
    std::snprintf(p.load_name, sizeof(p.load_name), "%s", p.cur.chr.c_str());
    const u64 buffer = reinterpret_cast<u64>(p.load_buffer);
    Call(kDescFn, buffer, reinterpret_cast<u64>(p.load_name), 0x73db, 0);
    u64 slot = 0;
    if (dedicated && g_begin_slot_load && g_find_native_slot) {
        const u64 retain = EngineMethod(dedicated, 0x18);
        if (retain && EngineMethod(dedicated, 0x30) && ReserveReplicaSlot(dedicated)) {
            const bool previous_slot_work = g_replica_slot_work;
            g_replica_slot_work = true;
            // Comeca a carga no endereco escolhido, sem a busca nativa pela figura local.
            Call(g_begin_slot_load, dedicated, buffer);
            g_replica_slot_work = previous_slot_work;
            if (At<s32>(dedicated + 0x2c8) != 0) {
                Call(retain, dedicated);
                slot = dedicated;
            } else {
                ForgetReplicaSlot(dedicated);
            }
        }
    } else if (vehicle || (player_slot && !LocalPlayerUses(Lower(p.cur.chr) + ".cd"))) {
        Call(kSlotLoadFn, buffer + 64, 0, buffer + 96, buffer, vehicle ? 1 : 0, 1);
        slot = At<u64>(buffer + 64);
    }
    p.loading = p.cur.chr;
    p.standin = false;
    p.slot = IsSlot(slot) ? slot : 0;
    if (p.slot) {
        const std::string name = Lower(p.cur.chr);
        g_our_slots[p.slot] = name;
        if (std::find(g_our_chars.begin(), g_our_chars.end(), name) == g_our_chars.end()) g_our_chars.push_back(name);
    }
    p.stage = p.slot ? Stage::kLoading : Stage::kNone;
    p.stage_time = now;
    if (!p.slot) p.load_fail_time = now; // nao deu agora: so o nome, e tenta de novo depois
    g_last_load_start = now;
    Log("[MP] carregando %s para %s", p.cur.chr, p.name);
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
    PruneReplicaSlots();
    UpdateLevelKey();
    // 4.4.32: a fase mudou (fim de missao -> proxima fase, volta ao hub, tela de carregamento): os fantasmas da fase
    // antiga nao existem mais. Solta todos SEM tocar no jogo - era isso que derrubava o jogo de quem segue o outro
    // jogador ao terminar uma missao (o destrutor do pedido mexia numa memoria ja liberada pela troca de fase).
    {
        static std::string last_level_here;
        if (g_level_key != last_level_here) {
            last_level_here = g_level_key;
            g_shift_replays.clear();
            g_shift_arrivals.clear();
            if (PlayableLevel(g_level_key) && g_world_log_level != g_level_key) {
                g_world_log.clear();
                g_world_log_level = g_level_key;
            }
            for (auto& [id, p] : g_puppets) ReleasePuppet(p);
            g_drive_count = 0;
            if (PlayableLevel(g_level_key)) {
                // chegamos numa fase de verdade: nao ha mais pedido pendente nem fase autorizada de antes (um portao
                // velho autorizava o numero de outra fase depois do fim da missao)
                g_level_gate_id = ~0ull;
                g_push_request_time = -100;
                g_push_retried = false;
                g_push_target.clear();
            }
        }
    }
    // 4.4.11: numa troca de fase o mundo esta sendo desmontado: nao escreve em personagem nenhum (era daqui que
    // vinham travadas, tela branca e crash ao entrar em missao).
    const bool loading_now = Lower(g_level_key).find("loading") != std::string::npos || g_level_key == "-" || g_level_key.empty();
    if (loading_now) {
        if (!g_was_loading) { // entrou agora: desfaz os fantasmas uma vez
            g_was_loading = true;
            for (auto& [pid, p] : g_puppets) ReleasePuppet(p); // 4.4.32: o mundo ja foi desmontado
            LiveInstClear();
            g_drive_count = 0;
        }
        return;
    }
    g_was_loading = false;
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
            LiveInstDrop(p.inst);
            p.request = p.inst = p.inst_name = 0;
            p.stage = Stage::kNone;
            p.retry_time = now + 2.5; // 4.4.7: menos pisca-pisca (o jogo as vezes desfaz e refaz o personagem)
        }
    }
    if (epoch != g_seen_epoch) { // troca de area: tira os fantasmas que ainda existirem
        g_seen_epoch = epoch;
        g_build_seen_state.clear();
        g_recent_gizmos.clear();
        g_ms_times.clear();
        g_ms_noisy.clear();
        {
            std::lock_guard lock(g_build_mutex);
            g_forced_builds.clear();
        }
        for (auto& [id, p] : g_puppets) {
            p.replayed = false;
            p.same_area_since = 0;
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
    }
    static unsigned seen_session = 0;
    if (const unsigned session = g_world_session.load(); session != seen_session) {
        seen_session = session;
        g_world_log.clear();
        g_pending_events.clear();
        g_world_last_serial.clear();
        std::lock_guard lock(g_outgoing_world_mutex);
        g_outgoing_world.clear();
    }
    ReceiveEvents(area, now);
    SendStateChanges(now, area_time);
    FlushWorldEvents(epoch);
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
    if (g_push_request_time > 0 && now - g_push_request_time > 20.0 && !g_push_retried) {
        g_push_retried = true; // 4.4.10: primeiro tenta a mesma fase de novo (o pedido pode ter se perdido na carga)
        const std::string low = Lower(g_level_key);
        const bool loading = low.empty() || low == "-" || low.find("loading") != std::string::npos;
        if (loading && g_push_target_id >= 0) {
            std::printf("[MP] ainda carregando: pedindo a fase %s (id %d) de novo\n", g_push_target.c_str(), g_push_target_id);
            std::fflush(stdout);
            g_level_gate_id = static_cast<u64>(g_push_target_id);
            g_level_gate_time = now;
            g_level_request_time = now;
            Call(kGotoLevelFn, reinterpret_cast<u64>(g_base) + kLevelLoader, static_cast<u64>(g_push_target_id), 0, 0);
            g_push_request_time = now;
        }
    }
    if (g_push_request_time > 0 && now - g_push_request_time > 30.0) {
        g_push_request_time = -100;
        g_push_retried = false;
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
    HandleChatCommands(now);
    ApplyDeferredLevel(now); // 4.4.30: aviso de fase que chegou com o jogador ainda no menu
    FollowOwner(now);
    Drive old[kMaxPuppets];
    const int old_count = g_drive_count;
    std::copy(g_drive, g_drive + old_count, old);
    g_drive_count = 0;
    for (const auto& peer : peers) {
        const int base_key = peer.id * kPuppetStride;
        if (g_puppets.size() >= kMaxPuppets && !g_puppets.count(base_key)) continue;
        Puppet& main = g_puppets[base_key];
        main.peer = peer.id;
        main.name = peer.name;
        main.idx = 0;
        const bool new_state = peer.seq != main.seq && !peer.state.empty();
        if (new_state) {
            Remote r;
            if (ParseRemote(peer.state, &r)) UpdateRemote(main, std::move(r), peer.state_time);
            main.seq = peer.seq;
        }
        std::vector<int> indices{0};
        const std::string active_figure = Lower(main.cur.chr);
        // A reserva que coincide com uma figura deste jogador ganha sua propria replica.
        for (size_t i = 0; i < main.cur.party.size(); ++i) {
            const std::string figure = Lower(main.cur.party[i]);
            if (figure != active_figure && LocalPlayerUses(figure + ".cd")) {
                indices.push_back(static_cast<int>(i) + 1);
            }
        }
        for (const RemoteVehicle& v : main.cur.vehicles) indices.push_back(kFirstVehiclePuppet + v.slot);
        for (auto it = g_puppets.upper_bound(base_key); it != g_puppets.end() && it->first < base_key + kPuppetStride;) {
            if (std::find(indices.begin(), indices.end(), it->second.idx) != indices.end()) {
                ++it;
                continue;
            }
            Despawn(it->second);
            it = g_puppets.erase(it);
        }
        for (const int idx : indices) {
            const bool vehicle = idx >= kFirstVehiclePuppet;
            if (idx && !(main.cur.valid && main.cur.ingame)) continue;
            const int key = base_key + idx;
            if (idx && g_puppets.size() >= kMaxPuppets && !g_puppets.count(key)) continue;
            Puppet& p = idx == 0 ? main : g_puppets[key];
            p.peer = peer.id;
            p.name = peer.name;
            p.idx = idx;
            if (idx && (new_state || !p.cur.valid)) {
                Remote state = main.cur;
                state.vehicles.clear();
                if (vehicle) {
                    const auto it = std::find_if(main.cur.vehicles.begin(), main.cur.vehicles.end(),
                                                 [&](const auto& v) { return idx == kFirstVehiclePuppet + v.slot; });
                    if (it == main.cur.vehicles.end()) continue;
                    state.chr = it->name;
                    state.x = it->x;
                    state.y = it->y;
                    state.z = it->z;
                    state.fx = it->fx;
                    state.fz = it->fz;
                    state.held = state.ctx_hash = 0;
                    state.vehicle.clear();
                } else {
                    state.chr = main.cur.party[idx - 1];
                    state.held = state.ctx_hash = 0;
                    state.gear = 3;
                    state.dx = state.dz = 0;
                    state.vehicle.clear();
                    const double angle = 2.4 * idx;
                    state.x += static_cast<float>(std::cos(angle)) * 0.55f;
                    state.z += static_cast<float>(std::sin(angle)) * 0.55f;
                }
                UpdateRemote(p, std::move(state), main.cur_time);
            }
        // 4.5.1: sumia ao se afastar porque a area muda de nome nas zonas do mundo (a fase e a mesma: as posicoes sao
        // do mesmo mundo). Mesma fase = mesmo mundo. E a tolerancia do estado subiu para 10 s (engasgo do outro jogo).
        const bool same_world = p.cur.level.empty() ? p.cur.area == area : p.cur.level == g_level_key;
        const bool show_actor = vehicle || (!IsVehicleName(p.cur.chr) &&
            (idx != 0 || main.cur.vehicle.empty() || std::none_of(main.cur.vehicles.begin(), main.cur.vehicles.end(),
                [&](const RemoteVehicle& item) {
                    if (item.name != main.cur.vehicle) return false;
                    const auto replica = g_puppets.find(base_key + kFirstVehiclePuppet + item.slot);
                    return replica != g_puppets.end() && replica->second.stage == Stage::kActive &&
                           PuppetAlive(replica->second) && replica->second.loading == item.name;
                })));
        const bool want = settled && p.cur.valid && p.cur.ingame && same_world && now - p.cur_time < 10.0 &&
                          p.cur.chr != "-" && !p.cur.chr.empty() && show_actor;
        p.want = want;
        // 4.2: chegou na nossa area ha 6 s (a lista de objetos dele ja esta pronta): o dono reenvia o que ja mudou aqui
        const bool here = settled && p.cur.valid && p.cur.ingame && same_world && p.cur.area == area &&
                          now - p.cur_time < 10.0;
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
            if (self && owner == self && g_world_log_level == g_level_key) {
                if (const auto log = g_world_log.find(area); log != g_world_log.end()) {
                    for (const std::string& event : log->second) SendWorldEvent(event, p.peer);
                }
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
        case Stage::kNone: {
            if (now < p.retry_time || now - g_last_spawn < 0.5) break;
            p.retry_time = now + 1.0;
            // 4.4.13: sem boneco e o personagem ja esta carregado: entra na hora (as esperas de carga nao valem aqui).
            // Era isso que deixava "so o nome" por 2-3 s cada vez que o jogo desfazia o fantasma.
            if (!g_unsupported.count(Lower(p.cur.chr))) {
                if (const u64 loaded = FindLoadedSlot(p.cur.chr, vehicle);
                    loaded && !SlotUsedByLocalPlayer(loaded)) {
                    p.slot = loaded;
                    p.standin = false;
                    p.stage = Stage::kReady;
                    p.retry_time = 0;
                    break;
                }
            }
            if (g_unsupported.count(Lower(p.cur.chr))) { // personagem que nao e figura: aparece com um substituto
                if (u64 slot = StandInSlot()) {
                    p.slot = slot;
                    p.standin = true;
                    p.stage = Stage::kReady;
                }
                break;
            }
            if (vehicle) {
                // Tag items usam a carga do portal, com um slot novo, assim
                // como o carregador nativo da figura veicular.
                if (!SomeoneLoading() && now - p.load_fail_time > 5.0 && FreeSlots() > kKeepFreeSlots) {
                    LoadPuppetSlot(p, now, 0, true);
                }
                break;
            }
            const u64 dedicated = CharacterReplicaSlot(p.cur.chr);
            const bool busy = SomeoneLoading();
            if (busy) {
                // o jogo carrega um personagem por vez: espera a vez (enquanto isso so o nome aparece)
                if (!p.wait_since) p.wait_since = now;
            } else if (dedicated && now - p.load_fail_time > 15.0 && now - g_last_unload > 8.0) {
                p.wait_since = 0;
                LoadPuppetSlot(p, now, dedicated, false);
            } else if (!dedicated && now - p.load_fail_time > 15.0 && RecycleSlot()) {
                p.retry_time = now + 1.0; // o slot esvazia em seguida; carrega na proxima tentativa
            } else if (!dedicated && ReclaimForCloser(p, now)) {
                // sem slot: tirou de quem esta bem mais longe (esse aparece com um substituto)
            } else if (now - p.want_since > 3.0) {
                if (u64 slot = StandInSlot()) { // sem slot por enquanto: aparece com um personagem ja carregado
                    p.slot = slot;
                    p.standin = true;
                    p.stage = Stage::kReady;
                }
            }
            break;
        }
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
            const u64 inst = SpawnedInstance(p.request);
            if (IsLocalPlayer(inst)) {
                ReleasePuppet(p);
                p.retry_time = now + 3.0;
                break;
            }
            const bool mech = IsMechVehicle(inst);
            if (inst && (mech ? vehicle && VehicleMatrix(inst) : Readable(inst + 0x10, 0x500))) {
                p.inst = inst;
                p.inst_name = vehicle ? VehicleIdentity(inst) : At<u64>(inst + 0x10);
                LiveInstAdd(inst);
                p.stage = Stage::kActive;
                p.stage_time = now;
                // 4.4.23: com flag = 0 no Spawn, o fantasma nunca entra na party nem na roda de figuras. O slot
                // (Player Load Slot dedicado) permanece carregado (estado 1) para os assets do boneco nunca sumirem.
                if (mech) PlaceVehicle(inst, p.cur.x, p.cur.y, p.cur.z, p.cur.fx, p.cur.fz);
                else Teleport(inst, p.cur.x, p.cur.y, p.cur.z);
                Log("[MP] %s apareceu como %s", p.name, mech ? PortalVehicleModel(p.slot) : LocalCharacter(inst));
                // nome que o jogo nao conhece como figura (ex.: Wheatley): o descritor cai no personagem do spawner.
                // A primeira vez pode ser so a carga recente; na terceira desiste e mostra so o nome.
                const std::string low = Lower(p.cur.chr);
                if (p.standin) {
                    // substituto: ja sabe que e outro personagem
                } else if (mech ? PortalVehicleModel(p.slot) == low : Lower(LocalCharacter(inst)) == low) {
                    g_wrong_count.erase(low);
                } else if (vehicle || ++g_wrong_count[low] >= 3) {
                    if (!vehicle) g_unsupported.insert(low);
                    Log("[MP] este jogo nao carregou o modelo %s de %s", p.cur.chr, p.name);
                    Despawn(p);
                    p.load_fail_time = now;
                    p.retry_time = now + 5.0;
                }
                if (vehicle && p.stage == Stage::kActive) p.loading = p.cur.chr;
            } else if (now - p.stage_time > 10.0) {
                Despawn(p);
            }
            break;
        }
        case Stage::kActive: {
            if (IsLocalPlayer(p.inst)) {
                LiveInstDrop(p.inst);
                LiveDrop(p.request);
                p.request = p.inst = p.inst_name = 0;
                p.stage = Stage::kNone;
                p.retry_time = now + 3.0;
                break;
            }
            if (p.slot) {
                const int sidx = SlotIndexOf(p.slot);
                const std::string name = sidx >= 0 ? SlotName(sidx) : std::string();
                if (sidx < 0 || SlotState(sidx) != 1 || name.empty() || SlotUsedByLocalPlayer(p.slot)) {
                    Log("[MP] slot do fantasma de %s mudou de estado/dono: desfazendo o boneco", p.name);
                    Despawn(p);
                    p.load_fail_time = 0;
                    p.retry_time = now + 2.0;
                    break;
                }
            }
            if (!PuppetAlive(p)) {
                LiveInstDrop(p.inst);
                p.stage = Stage::kNone;
                p.request = p.inst = 0;
                break;
            }
            LiveInstAdd(p.inst);
            // 4.5: o pedido acabou preso na party do jogador local (inst+0x3a0 igual): desfaz agora para nao derrubar o
            // jogo na troca de figura e volta com um slot independente
            const u64 local_party = p1 && Readable(p1 + 0x3a0, 8) ? At<u64>(p1 + 0x3a0) : 0;
            if (!vehicle && local_party && Readable(p.inst + 0x3a0, 8) && At<u64>(p.inst + 0x3a0) == local_party) {
                Despawn(p);
                Log("[MP] %s ficou com a party do jogador local: desfazendo o fantasma", p.name);
                p.retry_time = now + 2.0;
                p.load_fail_time = 0;
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
            if (vehicle && p.loading != p.cur.chr) {
                Despawn(p);
                p.retry_time = 0;
                break;
            }
            if (!vehicle && !p.standin && Lower(LocalCharacter(p.inst)) != Lower(p.cur.chr) && now - p.stage_time > 1.0 &&
                !g_unsupported.count(Lower(p.cur.chr)) &&
                (FindLoadedSlot(p.cur.chr) || (!SomeoneLoading() && now - p.load_fail_time > 15.0 &&
                                             (CharacterReplicaSlot(p.cur.chr) || RecycleSlot())))) {
                Despawn(p);
                p.retry_time = 0;
                break;
            }
            if ((vehicle || now - p.stage_time > 1.5) && g_drive_count < kMaxPuppets) {
                Drive& d = g_drive[g_drive_count];
                d.puppet = &p;
                d.slot_key = key;
                d.hold = false; // 4.4.9: zera antes do Steer (a struct e' reaproveitada de quadros anteriores)
                d.jump_active = false;
                d.name = 0;
                d.ctx_hash = 0;
                d.speed = 0.0f;
                d.dx = d.dz = 0.0f;
                u32 prev = 0;
                for (int k = 0; k < old_count; ++k) {
                    if (old[k].inst == p.inst) prev = old[k].prev; // botoes do quadro anterior (bordas)
                }
                Steer(p, now, &d);
                d.prev = prev;
                if (d.inst) ++g_drive_count;
            }
            break;
        }
        }
        } // 4.5.1: fim dos personagens da party deste amigo
    }
    // 4.4.18 / 4.4.23: sonda das opcoes de animacao/dano apenas com LD_MP_ANIM=1 (evita logs de dezenas de MB por sessao).
    static const bool log_anim_probe = std::getenv("LD_MP_ANIM") != nullptr;
    if (log_anim_probe && p1) {
        // 4.4.21: janela inst+0x000..0x800 (a animacao nao estava em 0x100..0x400) e rajada de 1 s quando um botao
        // muda, para o soco aparecer no log com o campo que ele mexe.
        static u32 prev[0x200];
        static double last = 0, burst = 0;
        static bool init = false;
        static u32 last_btn = 0xffffffffu;
        const u32 btn = g_last_held.load();
        if (btn != last_btn) {
            last_btn = btn;
            burst = now + 1.0;
        }
        const bool fast = now < burst;
        if ((fast ? now - last > 0.16 : now - last > 0.5) && Readable(p1, 4 * 0x200)) {
            last = now;
            std::string line;
            for (int i = 0; i < 0x200; ++i) {
                const u32 v = At<u32>(p1 + 4 * i);
                if (!init || v == prev[i]) {
                    prev[i] = v;
                    continue;
                }
                if (line.size() < 2200) {
                    char b[48];
                    std::snprintf(b, sizeof(b), " %03x:%08x>%08x", 4 * i, prev[i], v);
                    line += b;
                }
                prev[i] = v;
            }
            init = true;
            if (!line.empty()) {
                const u64 loc = Readable(p1 + 0x90, 8) ? At<u64>(p1 + 0x90) : 0;
                std::printf("[ANIM] marcha %d cara %.2f %.2f%s\n", loc && Readable(loc + 0x238, 4) ? At<s32>(loc + 0x238) : -1,
                            At<float>(p1 + 0x170), At<float>(p1 + 0x178), line.c_str());
                std::fflush(stdout);
            }
        }
    }
    { // 4.4.9: resumo para o painel F9 (MULTIPLAYER): posicao do amigo x posicao do fantasma, marcha e atraso
        std::string line = "MP: ";
        const int self = runtime::mp_net::SelfId();
        if (!self) {
            line += "fora da rede";
        } else {
            line += "sala " + runtime::mp_net::RoomCode() + " (id " + std::to_string(self) + ")";
        }
        line += " | " + runtime::mp_net::StatusText();
        for (const auto& [id, p] : g_puppets) {
            if (!p.cur.valid) continue;
            const u64 t = p.stage == Stage::kActive ? Transform(p.inst) : 0;
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          " | %s remoto (%.1f %.1f %.1f) m%d %s fantasma (%.1f %.1f %.1f) erro %.2fm pacote %.1fs",
                          p.name.c_str(), p.cur.x, p.cur.y, p.cur.z, p.cur.gear, t ? "|" : "SEM BONECO", t ? At<float>(t + 0x90) : 0.f,
                          t ? At<float>(t + 0x94) : 0.f, t ? At<float>(t + 0x98) : 0.f,
                          t ? std::hypot(At<float>(t + 0x90) - p.cur.x, At<float>(t + 0x98) - p.cur.z) : 0.f,
                          now - p.cur_time);
            line += buf;
        }
        std::lock_guard lock(g_summary_mutex);
        g_summary = line;
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
