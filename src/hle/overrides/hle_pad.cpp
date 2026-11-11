// libScePad - os 7 simbolos importados - com entrada real.
//
// Fontes de entrada, combinadas (OR dos botoes; analogico = o de maior deflexao):
//   1. Controle XInput 0 (Xbox ou compativel).
//   2. Teclado, SO quando a janela do jogo esta em foco (GetAsyncKeyState e global: sem esse
//      filtro, digitar em outro programa apertaria botoes no jogo).
//   3. Roteiro de --input-script "seg:botao[:ms],...", para testes sem ninguem no teclado.
//
// Layout de ScePadData conferido na Fase 9 contra o consumidor do jogo (eboot+0x4b9040 e
// eboot+0x4bae00), que le SO estes campos:
//   +0x00 u32 buttons (bit 31 = "interceptado": o jogo zera a entrada)   +0x04..0x09 analogicos / gatilhos
//   +0x34 u8 touchNum, +0x3C/+0x3E u16 x/y, +0x40 u8 id                  +0x4C u8 connected
// timestamp (+0x50), connectedCount (+0x68) e dados do dispositivo NAO sao lidos pelo jogo; sao
// preenchidos mesmo assim. Nao ha userId em ScePadData: o vinculo com o usuario e o scePadOpen.
//
// Bits de botao do Orbis (ScePadButtonDataOffset). Atencao: NAO sao os do DualShock 3.
//   L3 0x0002  R3 0x0004  OPTIONS 0x0008  UP 0x0010  RIGHT 0x0020  DOWN 0x0040  LEFT 0x0080
//   L2 0x0100  R2 0x0200  L1 0x0400  R1 0x0800  TRIANGLE 0x1000  CIRCLE 0x2000  CROSS 0x4000
//   SQUARE 0x8000  TOUCHPAD 0x100000
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <xinput.h>

#include "hle/overrides/hle_override.h"
#include "hle/overrides/hle_pad_sdl.h"
#include "hle/overrides/hle_videoout.h"

namespace hle {

namespace {

constexpr int kUserId = 1;
constexpr int kPadHandle = 1;
constexpr int kPadErrorInvalidArg = static_cast<int>(0x80920001);
constexpr int kPadErrorInvalidHandle = static_cast<int>(0x80920003);

constexpr u32 kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
constexpr u32 kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800;
constexpr u32 kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;

struct PadData {
    u32 buttons;
    u8 left_x, left_y;
    u8 right_x, right_y;
    u8 l2, r2;
    u8 padding[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    struct {
        u8 touch_num;
        u8 reserve[3];
        u32 reserve1;
        struct {
            u16 x, y;
            u8 id;
            u8 reserve[3];
        } touch[2];
    } touch_data;
    bool connected;
    u64 timestamp;
    struct {
        u32 extension_unit_id;
        u8 reserve[1];
        u8 data_length;
        u8 data[10];
    } extension_unit_data;
    u8 connected_count;
    u8 reserve[2];
    u8 device_unique_data_len;
    u8 device_unique_data[12];
};
static_assert(sizeof(PadData) == 0x78);
static_assert(offsetof(PadData, left_x) == 0x04 && offsetof(PadData, touch_data) == 0x34);
static_assert(offsetof(PadData, connected) == 0x4C && offsetof(PadData, timestamp) == 0x50);
static_assert(offsetof(PadData, connected_count) == 0x68);

struct Input {
    u32 buttons = 0;
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    u8 l2 = 0, r2 = 0;
};

struct ScriptStep {
    double start_s;
    double end_s;
    u32 button;    // bit de botao, ou 0 se o passo e de eixo
    int axis = -1; // 0 = LeftX, 1 = LeftY, 2 = RightX, 3 = RightY
    u8 value = 0x80;
    // Passo "por leituras" (macro): depois de start_s fica ativo ate o jogo ter LIDO o controle
    // `reads_left` vezes, em vez de por tempo. O jogo nem sempre le o controle a 30 Hz (no
    // menu principal, recem-carregado, ha intervalos de 0,6 s), e um toque curto por tempo
    // pode cair inteiro entre duas leituras.
    int reads_left = 0;
    int player = 0; // Fase 61: 0 = jogador 1; 1 = jogador 2 (LD_PAD_LIVE2, testes do co-op)
};

const auto g_epoch = std::chrono::steady_clock::now();
std::vector<ScriptStep> g_script;
std::atomic<u32> g_last_buttons{0};
std::atomic<u64> g_reads{0};
double Seconds();
bool g_keyboard_enabled = true;
u64 g_last_timestamp = 0;
// 2.0.2: de onde veio a ultima entrada do jogador 1 (os icones dos botoes seguem): -1 = nada ainda, 0 = controle, 1 = teclado
std::atomic<int> g_last_device{-1};
// 2.0.2: de qual jogador e o teclado (0 = jogador 1; 1 = jogador 2, ligado pelo F2 com o jogador 1 num controle, como no PC)
std::atomic<int> g_keyboard_player{0};

// ---- sonda: o que o JOGO enxerga -------------------------------------------------------------
// O unico ponto que chama scePadReadState e eboot+0x4b9004, dentro de eboot+0x4b8ce0, chamada
// por eboot+0x4b8940 com r15 = gerenciador de controles. O prologo de 0x4b8ce0 salva r15 em
// [rbp-8], entao da para achar o gerenciador pela pilha do jogo e ler o objeto de controle:
//   gerenciador: +0x18 lista de objetos, +0x24 quantidade
//   objeto:      +0x0C slot, +0x10 tipo, +0x24 conectado, +0x25 interceptado, +0x28 mascara de botoes
// A mascara e a que eboot+0x4b9040 montou no frame anterior a partir de ScePadData.buttons.
constexpr u64 kReadStateReturn = 0x4b9009;
constexpr u64 kPadTable = 0x2FB5710;    // 16 x {s32 userId, s32 handle, ScePadData*, u8 em_uso}
// Layout: se == 1, X -> 0x80000000 e O -> 0x40000000 na mascara do jogo; senao o contrario.
// NAO vem de sceSystemServiceParamGetInt (o jogo so consulta o idioma, id 1).
constexpr u64 kEnterAssign = 0x2FB5890;

bool Readable(u64 address, u64 size) {
    MEMORY_BASIC_INFORMATION info;
    if (!address || !VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) {
        return false;
    }
    const u64 region_end = reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize;
    return info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && address + size <= region_end;
}

template <typename T>
T Peek(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

struct GameView {
    bool valid = false;
    u32 objects = 0;
    u32 slot = 0;
    u32 type = 0;
    u8 connected = 0;
    u8 intercepted = 0;
    u32 mask = 0;
    bool operator==(const GameView&) const = default;
};
GameView g_game_view;
std::atomic<u64> g_game_mask_changes{0};
std::atomic<u32> g_game_mask_or{0};
bool g_table_logged = false;

void ProbeGame(u64 return_address, u64 guest_rbp, const void* data) {
    if ((return_address & 0xFFFFFF) != kReadStateReturn) {
        return;
    }
    const u64 base = return_address - kReadStateReturn;
    if (!g_table_logged && Readable(base + kPadTable, 0x18 * 16) && Readable(base + kEnterAssign, 4)) {
        g_table_logged = true;
        for (int slot = 0; slot < 16; ++slot) {
            const u64 entry = base + kPadTable + slot * 0x18;
            if (Peek<s32>(entry) != -1 || Peek<u64>(entry + 8) == reinterpret_cast<u64>(data)) {
                log::Tagged("PAD", "game table, slot %d: userId=%d handle=%d ScePadData=%p in_use=%u", slot,
                            Peek<s32>(entry), Peek<s32>(entry + 4), reinterpret_cast<void*>(Peek<u64>(entry + 8)),
                            Peek<u8>(entry + 0x10));
            }
        }
        const u32 assign = Peek<u32>(base + kEnterAssign);
        log::Tagged("PAD", "game button layout (eboot+0x%llx) = %u -> in game mask, X becomes 0x%08x and O becomes 0x%08x",
                    static_cast<unsigned long long>(kEnterAssign), assign, assign == 1 ? 0x80000000u : 0x40000000u,
                    assign == 1 ? 0x40000000u : 0x80000000u);
    }
    GameView view;
    if (Readable(guest_rbp - 8, 8)) {
        const u64 manager = Peek<u64>(guest_rbp - 8);
        if (Readable(manager, 0x38)) {
            view.objects = Peek<u32>(manager + 0x24);
            const u64 list = Peek<u64>(manager + 0x18);
            if (view.objects >= 1 && view.objects <= 16 && Readable(list, 8)) {
                const u64 object = Peek<u64>(list);
                if (Readable(object, 0x30)) {
                    view.valid = true;
                    view.slot = Peek<u32>(object + 0x0C);
                    view.type = Peek<u32>(object + 0x10);
                    view.connected = Peek<u8>(object + 0x24);
                    view.intercepted = Peek<u8>(object + 0x25);
                    view.mask = Peek<u32>(object + 0x28);
                }
            }
        }
    }
    if (view == g_game_view) {
        return;
    }
    if (view.valid) {
        if (view.mask != g_game_view.mask) {
            g_game_mask_changes.fetch_add(1);
            g_game_mask_or.fetch_or(view.mask);
        }
        log::Tagged("PAD", "t=%.1fs GAME sees: controller 0 of %u (slot %u, type %u) connected=%u intercepted=%u mask=0x%08x",
                    Seconds(), view.objects, view.slot, view.type, view.connected, view.intercepted, view.mask);
    } else {
        log::Tagged("PAD", "t=%.1fs probe: game controller object not found", Seconds());
    }
    g_game_view = view;
}

double Seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count();
}

const struct {
    const char* name;
    u32 bit;
} kButtonNames[] = {
    {"options", kOptions}, {"cross", kCross},   {"circle", kCircle}, {"square", kSquare}, {"triangle", kTriangle},
    {"up", kUp},           {"right", kRight},   {"down", kDown},     {"left", kLeft},     {"l1", kL1},
    {"r1", kR1},           {"l2", kL2},         {"r2", kR2},         {"l3", kL3},         {"r3", kR3},
    {"touchpad", kTouchPad},
};

std::string ButtonList(u32 buttons) {
    std::string out;
    for (const auto& b : kButtonNames) {
        if (buttons & b.bit) {
            out += out.empty() ? "" : "+";
            out += b.name;
        }
    }
    return out.empty() ? "(none)" : out;
}

bool g_stick_log = false; // --stick-log

// Eixo do XInput (-32768..32767, +Y para cima) -> eixo do Orbis (0..255, 0 = cima/esquerda).
u8 Axis(SHORT value, bool invert) {
    int v = invert ? -static_cast<int>(value) - 1 : value;
    if (std::abs(v) < 7849) { // zona morta padrao do XInput
        v = 0;
    }
    return static_cast<u8>((v + 32768) >> 8);
}

void ReadXInput(Input& in, DWORD index = 0) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
        return;
    }
    const WORD b = state.Gamepad.wButtons;
    const struct {
        WORD xinput;
        u32 orbis;
    } map[] = {
        {XINPUT_GAMEPAD_START, kOptions},        {XINPUT_GAMEPAD_BACK, kTouchPad},
        {XINPUT_GAMEPAD_A, kCross},              {XINPUT_GAMEPAD_B, kCircle},
        {XINPUT_GAMEPAD_X, kSquare},             {XINPUT_GAMEPAD_Y, kTriangle},
        {XINPUT_GAMEPAD_DPAD_UP, kUp},           {XINPUT_GAMEPAD_DPAD_RIGHT, kRight},
        {XINPUT_GAMEPAD_DPAD_DOWN, kDown},       {XINPUT_GAMEPAD_DPAD_LEFT, kLeft},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, kL1},     {XINPUT_GAMEPAD_RIGHT_SHOULDER, kR1},
        {XINPUT_GAMEPAD_LEFT_THUMB, kL3},        {XINPUT_GAMEPAD_RIGHT_THUMB, kR3},
    };
    for (const auto& m : map) {
        if (b & m.xinput) {
            in.buttons |= m.orbis;
        }
    }
    in.l2 = state.Gamepad.bLeftTrigger;
    in.r2 = state.Gamepad.bRightTrigger;
    if (in.l2 > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        in.buttons |= kL2;
    }
    if (in.r2 > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        in.buttons |= kR2;
    }
    in.lx = Axis(state.Gamepad.sThumbLX, false);
    in.ly = Axis(state.Gamepad.sThumbLY, true);
    in.rx = Axis(state.Gamepad.sThumbRX, false);
    in.ry = Axis(state.Gamepad.sThumbRY, true);
    // --stick-log: analogico esquerdo fora da zona morta (no maximo duas linhas por segundo)
    if (g_stick_log && (in.lx > 150 || in.lx < 100 || in.ly > 150 || in.ly < 100)) {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 500) {
            last = now;
            std::printf("[PAD] left stick: LeftX %u LeftY %u (XInput %d, %d) buttons 0x%04x\n", in.lx, in.ly, state.Gamepad.sThumbLX,
                        state.Gamepad.sThumbLY, b);
        }
    }
}

bool Key(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Tecla que ja estava apertada quando a janela ganhou o foco (o Enter que lancou o programa, um
// Alt+Tab...) fica bloqueada ate ser solta: sem isto ela vira um toque fantasma no jogo.
bool g_had_focus = false;
bool g_blocked[256] = {};

bool FreshKey(int vk) {
    const bool down = Key(vk);
    if (g_blocked[vk]) {
        if (down) {
            return false;
        }
        g_blocked[vk] = false;
    }
    return down;
}

void ReadKeyboard(Input& in, bool player1 = true) {
    if (!g_keyboard_enabled || !videoout::WindowHasFocus()) {
        g_had_focus = false;
        return;
    }
    if (!g_had_focus) {
        g_had_focus = true;
        for (int vk = 1; vk < 256; ++vk) {
            g_blocked[vk] = Key(vk); // tambem descarta o bit "apertada desde a ultima consulta"
        }
    }
    const struct {
        int vk;
        u32 orbis;
    } map[] = {
        {VK_RETURN, kOptions}, {VK_SPACE, kCross}, {VK_ESCAPE, kCircle}, {'X', kSquare}, {'Y', kTriangle},
        {VK_UP, kUp},          {VK_RIGHT, kRight}, {VK_DOWN, kDown},     {VK_LEFT, kLeft},
        {'Q', kL1},            {'E', kR1},         {'1', kL2},           {'3', kR2},     {VK_TAB, kTouchPad},
    };
    bool any = false;
    for (const auto& m : map) {
        if (FreshKey(m.vk)) {
            in.buttons |= m.orbis;
            any = true;
        }
    }
    if (in.buttons & kL2) {
        in.l2 = 255;
    }
    if (in.buttons & kR2) {
        in.r2 = 255;
    }
    // WASD = analogico esquerdo, IJKL = direito
    if (FreshKey('A')) in.lx = 0x00;
    if (FreshKey('D')) in.lx = 0xFF;
    if (FreshKey('W')) in.ly = 0x00;
    if (FreshKey('S')) in.ly = 0xFF;
    if (FreshKey('J')) in.rx = 0x00;
    if (FreshKey('L')) in.rx = 0xFF;
    if (FreshKey('I')) in.ry = 0x00;
    if (FreshKey('K')) in.ry = 0xFF;
    for (const int vk : {'A', 'D', 'W', 'S', 'J', 'L', 'I', 'K'}) {
        any = any || (!g_blocked[vk] && Key(vk));
    }
    if (any && player1 && g_last_device.exchange(1) != 1) {
        log::Tagged("PAD", "t=%.1fs keyboard in use", Seconds());
    }
}

// 2.0.2: o controle mexeu (botao, gatilho ou analogico fora da zona morta)?
bool PadMoved(const Input& in) {
    auto off = [](u8 v) { return v < 128 - 48 || v > 128 + 48; };
    return in.buttons || in.l2 > 40 || in.r2 > 40 || off(in.lx) || off(in.ly) || off(in.rx) || off(in.ry);
}

std::mutex g_script_mutex; // o macro acrescenta passos de outra thread

// Fase 60: bloqueio do controle fora de foco. Sem a janela do jogo em primeiro plano, SDL e XInput sao
// ignorados (sticks no centro, nenhum botao); --background-pad desliga. Ao voltar o foco, os botoes que ja
// estavam apertados so contam depois de soltos (o clique / Alt+Tab de volta nao vira um X no jogo).
bool g_background_pad = false;
bool g_pad_blocked = false;
u32 g_pad_held_mask = 0;

bool PadFocusGate() {
    const bool allowed = g_background_pad || !videoout::WindowHandle() || videoout::WindowHasFocus();
    if (allowed != g_pad_blocked) {
        return allowed; // sem mudanca (liberado e nao bloqueado, ou fora de foco e ja bloqueado)
    }
    if (!allowed) {
        g_pad_blocked = true;
        log::Tagged("PAD", "game window not focused: controller blocked");
    }
    return allowed;
}

u32 PadHeldSinceFocus(u32 buttons) {
    if (g_pad_blocked && PadFocusGate()) { // acabou de voltar o foco
        g_pad_blocked = false;
        g_pad_held_mask = buttons;
        log::Tagged("PAD", "game window focused: controller enabled");
    }
    g_pad_held_mask &= buttons; // botao solto sai da mascara
    return g_pad_held_mask;
}

// Testes (LD_PAD_LIVE=<arquivo>): linhas "botao:ms", "ls_up:ms" ou "wait:ms" acrescentadas ao arquivo entram
// na hora, em sequencia. Serve para navegar num teste olhando a tela, sem teclado nem controle.
// Fase 61: LD_PAD_LIVE2 faz o mesmo para um controle virtual do jogador 2 (teste do co-op sem segundo controle).
void ReadLiveFile(double now, int player) {
    static const char* const paths[2] = {std::getenv("LD_PAD_LIVE"), std::getenv("LD_PAD_LIVE2")};
    static double next_checks[2] = {}, cursors[2] = {};
    static long long consumed_bytes[2] = {};
    const char* const path = paths[player];
    double& next_check = next_checks[player];
    long long& consumed = consumed_bytes[player];
    double& cursor = cursors[player];
    if (!path || now < next_check) {
        return;
    }
    next_check = now + 0.1;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long long size = std::ftell(f);
    if (size <= consumed) {
        std::fclose(f);
        return;
    }
    std::string text(static_cast<size_t>(size - consumed), '\0');
    std::fseek(f, static_cast<long>(consumed), SEEK_SET);
    text.resize(std::fread(text.data(), 1, text.size(), f));
    std::fclose(f);
    const size_t last_newline = text.rfind('\n');
    if (last_newline == std::string::npos) {
        return; // linha ainda incompleta
    }
    text.resize(last_newline + 1);
    consumed += static_cast<long long>(text.size());
    static const struct {
        const char* name;
        int axis;
        u8 value;
    } kAxes[] = {
        {"ls_left", 0, 0x00}, {"ls_right", 0, 0xFF}, {"ls_up", 1, 0x00}, {"ls_down", 1, 0xFF},
        {"rs_left", 2, 0x00}, {"rs_right", 2, 0xFF}, {"rs_up", 3, 0x00}, {"rs_down", 3, 0xFF},
    };
    cursor = std::max(cursor, now);
    std::lock_guard lock(g_script_mutex);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        for (char& c : line) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = line.substr(0, colon);
        const double hold_ms = std::atof(line.c_str() + colon + 1);
        if (name == "wait") {
            cursor += hold_ms / 1000.0;
            continue;
        }
        ScriptStep step{};
        step.start_s = cursor;
        step.end_s = cursor + hold_ms / 1000.0;
        step.player = player;
        for (const auto& b : kButtonNames) {
            if (name == b.name) {
                step.button = b.bit;
            }
        }
        for (const auto& a : kAxes) {
            if (name == a.name) {
                step.axis = a.axis;
                step.value = a.value;
            }
        }
        if ((!step.button && step.axis < 0) || hold_ms <= 0.0) {
            continue;
        }
        cursor = step.end_s + 0.15; // folga entre dois toques do mesmo botao
        g_script.push_back(step);
        log::Tagged("PAD", "t=%.1fs live (player %d): %s for %.0f ms", now, player + 1, name.c_str(), hold_ms);
    }
}

void ReadScript(Input& in, int player = 0) {
    const double now = Seconds();
    ReadLiveFile(now, player);
    std::lock_guard lock(g_script_mutex);
    for (ScriptStep& step : g_script) {
        if (step.player != player) {
            continue;
        }
        const bool by_reads = step.reads_left > 0 && now >= step.start_s;
        if (by_reads) {
            --step.reads_left;
        }
        if (by_reads || (now >= step.start_s && now < step.end_s)) {
            in.buttons |= step.button;
            switch (step.axis) {
            case 0: in.lx = step.value; break;
            case 1: in.ly = step.value; break;
            case 2: in.rx = step.value; break;
            case 3: in.ry = step.value; break;
            default: break;
            }
        }
    }
    // Uma linha de log quando o roteiro mexe (ou solta) um analogico.
    static std::atomic<u32> last_sticks{0x80808080};
    const u32 sticks = in.lx | (in.ly << 8) | (in.rx << 16) | (static_cast<u32>(in.ry) << 24);
    if (player == 0 && !g_script.empty() && last_sticks.exchange(sticks) != sticks) {
        log::Tagged("PAD", "t=%.1fs sticks: left (%u, %u) right (%u, %u)  [128 = center, 0 = up/left, 255 = down/right]",
                    now, in.lx, in.ly, in.rx, in.ry);
    }
}

// Fase 61 (co-op local): ha controle para a vaga (0 = jogador 1)? SDL primeiro; sem nenhum controle no SDL (ou com
// --no-sdl-pad), o XInput do mesmo indice. XInputGetState num indice vazio e lento (o driver procura o aparelho):
// vaga vazia e conferida de novo so a cada 2 s.
bool SlotPresent(int slot) {
    if (slot < 0 || slot >= padsdl::kSlots) {
        return false;
    }
    if (slot == 1 && std::getenv("LD_PAD_LIVE2")) {
        return true; // controle virtual do jogador 2 (testes)
    }
    if (slot == 1 && g_keyboard_player.load() == 1) {
        return true; // 2.0.2: o teclado e o jogador 2
    }
    padsdl::State sdl;
    if (padsdl::Read(&sdl, slot)) {
        return true;
    }
    if (padsdl::Count() > 0) {
        return false;
    }
    static std::mutex mutex;
    static bool present[padsdl::kSlots] = {};
    static double next_check[padsdl::kSlots] = {};
    std::lock_guard lock(mutex);
    const double now = Seconds();
    if (present[slot] || now >= next_check[slot]) {
        XINPUT_STATE state{};
        present[slot] = XInputGetState(static_cast<DWORD>(slot), &state) == ERROR_SUCCESS;
        next_check[slot] = now + 2.0;
    }
    return present[slot];
}

// Jogadores 2 a 4: so o controle da propria vaga (o teclado, os roteiros e o portal sao do jogador 1).
bool ReadOtherPlayer(int slot, Input& in) {
    if (!SlotPresent(slot)) {
        return false;
    }
    if (!PadFocusGate()) {
        return true; // conectado, mas a janela do jogo nao esta na frente: parado
    }
    padsdl::State sdl;
    if (padsdl::Read(&sdl, slot)) {
        in.buttons |= sdl.buttons;
        in.lx = sdl.lx;
        in.ly = sdl.ly;
        in.rx = sdl.rx;
        in.ry = sdl.ry;
        in.l2 = sdl.l2;
        in.r2 = sdl.r2;
    } else if (padsdl::Count() == 0) {
        ReadXInput(in, static_cast<DWORD>(slot));
    }
    if (slot == 1) {
        if (g_keyboard_player.load() == 1) {
            ReadKeyboard(in, false); // 2.0.2
        }
        ReadScript(in, 1);
    }
    return true;
}

HLE_IMPL(int, scePadInit) {
    HLE_TRACE("");
    padsdl::Start(); // Fase 24.5: thread do SDL (DualShock 4, touchpad)
    return err::OK;
}
HLE_OVERRIDE(scePadInit);

HLE_IMPL(int, scePadOpen, int user_id, int type, int index, const void* param) {
    // Fase 61: usuarios 1 a 4 (co-op local), um controle cada; o handle e o proprio id do usuario
    const int result = (user_id >= kUserId && user_id < kUserId + padsdl::kSlots && index == 0) ? user_id : kPadErrorInvalidArg;
    XINPUT_STATE state{};
    HLE_TRACE("user=%d type=%d index=%d -> %d (XInput controller %s; keyboard when window focused)", user_id, type,
              index, result, XInputGetState(0, &state) == ERROR_SUCCESS ? "detectado" : "not detected");
    return result;
}
HLE_OVERRIDE(scePadOpen);

bool ValidHandle(int handle) { // Fase 61
    return handle >= kPadHandle && handle < kPadHandle + padsdl::kSlots;
}

HLE_IMPL(int, scePadClose, int handle) {
    HLE_TRACE("%d", handle);
    return ValidHandle(handle) ? err::OK : kPadErrorInvalidHandle;
}
HLE_OVERRIDE(scePadClose);

HLE_IMPL(int, scePadReadState, int handle, PadData* data, u64 guest_rbp) {
    HLE_QUIET("%d", handle);
    if (!ValidHandle(handle)) {
        return kPadErrorInvalidHandle;
    }
    if (!data) {
        return kPadErrorInvalidArg;
    }
    if (handle != kPadHandle) { // Fase 61: jogadores 2 a 4
        const int slot = handle - kPadHandle;
        Input other;
        const bool present = ReadOtherPlayer(slot, other);
        std::memset(data, 0, sizeof(*data));
        data->buttons = other.buttons;
        data->left_x = other.lx;
        data->left_y = other.ly;
        data->right_x = other.rx;
        data->right_y = other.ry;
        data->l2 = other.l2;
        data->r2 = other.r2;
        data->orientation[3] = 1.0f;
        data->acceleration[1] = 1.0f;
        data->connected = present;
        data->connected_count = present ? 1 : 0;
        g_last_timestamp = std::max<u64>(static_cast<u64>(Seconds() * 1e6), g_last_timestamp + 1);
        data->timestamp = g_last_timestamp;
        static std::atomic<u32> last_buttons[padsdl::kSlots];
        if (last_buttons[slot].exchange(other.buttons) != other.buttons) {
            log::Tagged("PAD", "t=%.1fs player %d: buttons 0x%05x %s", Seconds(), slot + 1, other.buttons, ButtonList(other.buttons).c_str());
        }
        return err::OK;
    }
    ProbeGame(HLE_RETURN_ADDRESS(), guest_rbp, data);
    Input in;
    // Fase 24.5: o SDL e a primeira fonte (enxerga o DualShock 4 e o touchpad). O XInput so e lido
    // se o SDL nao abriu nenhum controle (um controle Xbox apareceria nos dois).
    padsdl::State sdl;
    // Fase 60: o controle so mexe no jogo com a janela do jogo em primeiro plano. Antes, com outra janela
    // aberta por cima (navegador, Discord...), o controle continuava andando com o personagem.
    const bool pad_allowed = PadFocusGate();
    const bool have_sdl = padsdl::Read(&sdl) && pad_allowed;
    if (have_sdl) {
        in.buttons |= sdl.buttons;
        in.lx = sdl.lx;
        in.ly = sdl.ly;
        in.rx = sdl.rx;
        in.ry = sdl.ry;
        in.l2 = sdl.l2;
        in.r2 = sdl.r2;
        if (g_stick_log && (in.lx > 150 || in.lx < 100 || in.ly > 150 || in.ly < 100)) {
            static ULONGLONG last = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - last >= 500) {
                last = now;
                std::printf("[PAD] left stick (SDL): LeftX %u LeftY %u buttons 0x%05x\n", in.lx, in.ly, in.buttons);
            }
        }
    } else if (pad_allowed && padsdl::Count() == 0) { // Fase 61: com outros controles no SDL, o XInput 0 seria um deles
        ReadXInput(in);
    }
    in.buttons &= ~PadHeldSinceFocus(in.buttons);
    if (PadMoved(in) && g_last_device.exchange(0) != 0) {
        log::Tagged("PAD", "t=%.1fs controller in use", Seconds());
    }
    if (g_keyboard_player.load() == 0) {
        ReadKeyboard(in);
    }
    ReadScript(in);

    // Menu de Configuracoes de Video (L3 + R3 no controle)
    static bool s_menu_combo_held = false;
    const bool menu_combo_now = ((in.buttons & (kL3 | kR3)) == (kL3 | kR3));
    if (menu_combo_now && !s_menu_combo_held) {
        videoout::ToggleVideoSettingsMenu();
    }
    s_menu_combo_held = menu_combo_now;

    if (videoout::IsVideoSettingsMenuOpen()) {
        videoout::HandleVideoMenuController(in.buttons);
        in.buttons = 0;
        in.lx = 128;
        in.ly = 128;
        in.rx = 128;
        in.ry = 128;
    }

    // Menu de Conquistas (RB + LB + RT no controle: kR1 | kL1 | kR2)
    static bool s_achievements_combo_held = false;
    const bool achievements_combo_now = ((in.buttons & (kL1 | kR1 | kR2)) == (kL1 | kR1 | kR2));
    if (achievements_combo_now && !s_achievements_combo_held) {
        videoout::ToggleAchievementsMenu();
    }
    s_achievements_combo_held = achievements_combo_now;

    if (videoout::IsAchievementsMenuOpen()) {
        videoout::HandleAchievementsMenuController(in.buttons);
        in.buttons = 0;
        in.lx = 128;
        in.ly = 128;
        in.rx = 128;
        in.ry = 128;
    }

    std::memset(data, 0, sizeof(*data));
    data->buttons = in.buttons;
    data->left_x = in.lx;
    data->left_y = in.ly;
    data->right_x = in.rx;
    data->right_y = in.ry;
    data->l2 = in.l2;
    data->r2 = in.r2;
    data->orientation[3] = 1.0f;  // quaternion identidade
    data->acceleration[1] = 1.0f; // controle parado sobre a mesa
    if (have_sdl) { // toques do touchpad, no formato do ScePadData (+0x34)
        data->touch_data.touch_num = sdl.touches;
        for (u32 i = 0; i < sdl.touches && i < 2; ++i) {
            data->touch_data.touch[i].x = sdl.touch[i].x;
            data->touch_data.touch[i].y = sdl.touch[i].y;
            data->touch_data.touch[i].id = sdl.touch[i].id;
        }
    }
    data->connected = true;
    data->connected_count = 1;
    // microssegundos, estritamente crescente mesmo com duas leituras no mesmo instante
    g_last_timestamp = std::max<u64>(static_cast<u64>(Seconds() * 1e6), g_last_timestamp + 1);
    data->timestamp = g_last_timestamp;
    g_reads.fetch_add(1);

    const u32 previous = g_last_buttons.exchange(in.buttons);
    if (previous != in.buttons) {
        log::Tagged("PAD", "t=%.1fs buttons 0x%05x %s", Seconds(), in.buttons, ButtonList(in.buttons).c_str());
    }
    return err::OK;
}

// O simbolo registrado e um thunk que entrega o rbp do JOGO como terceiro argumento (rdx, livre:
// scePadReadState so tem dois) e salta para a implementacao; o endereco de retorno continua
// sendo o do jogo. E o que a sonda ProbeGame usa para achar o gerenciador de controles.
extern "C" void* g_pad_read_state_impl = reinterpret_cast<void*>(&impl_scePadReadState);
extern "C" __attribute__((naked)) void PadReadStateThunk() {
    __asm__("movq %rbp, %rdx; jmpq *g_pad_read_state_impl(%rip)");
}
static ::hle::OverrideRegistrar hle_override_scePadReadState{"scePadReadState", reinterpret_cast<void*>(&PadReadStateThunk)};

HLE_IMPL(int, scePadSetLightBar, int handle, const void* color) {
    HLE_QUIET("%d", handle);
    return ValidHandle(handle) ? err::OK : kPadErrorInvalidHandle;
}
HLE_OVERRIDE(scePadSetLightBar);

HLE_IMPL(int, scePadResetLightBar, int handle) {
    HLE_QUIET("%d", handle);
    return ValidHandle(handle) ? err::OK : kPadErrorInvalidHandle;
}
HLE_OVERRIDE(scePadResetLightBar);

// Vibracao: {u8 motor grande, u8 motor pequeno} -> motores do controle XInput.
HLE_IMPL(int, scePadSetVibration, int handle, const u8* params) {
    HLE_QUIET("%d", handle);
    if (!ValidHandle(handle)) {
        return kPadErrorInvalidHandle;
    }
    if (params) {
        const int slot = handle - kPadHandle; // Fase 61: o controle do proprio jogador
        XINPUT_VIBRATION vibration{static_cast<WORD>(params[0] * 257), static_cast<WORD>(params[1] * 257)};
        if (padsdl::Count() == 0) {
            XInputSetState(static_cast<DWORD>(slot), &vibration);
        }
        padsdl::Rumble(params[0], params[1], slot);
    }
    return err::OK;
}
HLE_OVERRIDE(scePadSetVibration);

} // namespace

void SetPadStickLog(bool enabled) {
    g_stick_log = enabled;
}

bool PadControllerPresent(int slot) { // Fase 61: usado pelo UserService (login / logout dos jogadores 2 a 4)
    return SlotPresent(slot);
}

// 2.0.2: F2 = o teclado vira o jogador 2 (o jogo ve um segundo usuario entrar e mostra "aperte F2 ou qualquer botao"), e F2
// de novo devolve o teclado ao jogador 1. So com um controle no jogador 1: sem ele o jogador 1 ficaria sem comandos.
bool TogglePadKeyboardPlayer() {
    if (g_keyboard_player.load() == 1) {
        g_keyboard_player = 0;
        log::Tagged("PAD", "keyboard back to player 1");
        return true;
    }
    if (!g_keyboard_enabled || !SlotPresent(0)) {
        log::Tagged("PAD", "F2: no controller for player 1, the keyboard stays with player 1");
        return false;
    }
    g_keyboard_player = 1;
    g_last_device = 0; // os icones seguem o controle do jogador 1
    log::Tagged("PAD", "keyboard is now player 2");
    return true;
}

bool PadKeyboardInUse() { // 2.0.2: o jogador 1 esta no teclado (ultima entrada; antes de qualquer uma: sem controle ligado)
    if (g_keyboard_player.load() == 1) {
        return false;
    }
    const int last = g_last_device.load();
    return last >= 0 ? last == 1 : (g_keyboard_enabled && !SlotPresent(0));
}

void SetPadBackgroundInput(bool enabled) {
    g_background_pad = enabled;
}

void SetPadKeyboardEnabled(bool enabled) {
    g_keyboard_enabled = enabled;
}

void PrintPadStats() {
    std::printf("Pad        : %llu reads; button mask INSIDE the game changed %llu time(s) (bits seen: 0x%08x)\n",
                static_cast<unsigned long long>(g_reads.load()), static_cast<unsigned long long>(g_game_mask_changes.load()),
                g_game_mask_or.load());
}

// Roteiro de entrada, itens separados por virgula. Duas formas de item:
//   "seg:nome[:ms]"  comeca aos `seg` segundos do processo e dura `ms` (padrao 200);
//   "nome:ms"        comeca quando o item anterior termina (sequencia), ex.: "LS_UP:3000".
// `nome` e um botao (cross, circle, options, up...) ou um eixo analogico mantido no extremo:
//   ls_up / ls_down / ls_left / ls_right (analogico esquerdo), rs_* (direito).
// Cima e esquerda = 0, baixo e direita = 255; o que o roteiro nao toca fica centrado em 128.
// Maiusculas e minusculas tanto faz.
// ---------------------- macro "Carregar Jogo" (Fase 16) ----------------------
// Em vez de horarios fixos, o macro reage a tela: a contagem de draws por frame identifica a
// tela de titulo (134 / 136) e o menu principal (175 / 176). Os horarios variam de execucao
// para execucao (o titulo so aparece depois da cutscene de abertura, ~220 s), entao um roteiro
// por relogio erra a tela; o macro nao.
//   titulo parado            -> OPTIONS
//   menu principal parado    -> D-PAD BAIXO (de "Novo Jogo" para "Carregar Jogo"), depois X
//   depois                   -> X a cada 3 s, tres vezes (slot e avisos)
//
// Resultado medido na Fase 16 com os saves que existem hoje: "Carregar Jogo" esta DESABILITADO
// no menu (aparece apagado). O cursor pula de "Novo Jogo" direto para "Opcoes", e o X abre o
// menu de opcoes (151 draws). Os saves so guardam o estado de criacao do jogo, sem progresso
// para carregar. O macro fica pronto para quando houver um save com progresso; se a tela de
// 151 draws aparecer, ele avisa e sai dela com O.
namespace {

std::atomic<bool> g_macro_enabled{false};
std::atomic<bool> g_skip_spam{false};
double g_spam_time = 0.0;
int g_spam_count = 0;
int g_macro_state = 0;
double g_macro_time = 0.0;
int g_macro_presses = 0;

void MacroPress(u32 button, double delay_s, double hold_s, const char* what) {
    const double start = Seconds() + delay_s;
    {
        std::lock_guard lock(g_script_mutex);
        ScriptStep step{};
        step.start_s = start;
        step.end_s = start + hold_s;
        step.button = button;
        step.reads_left = 2; // garante que o jogo veja o botao em pelo menos duas leituras
        g_script.push_back(step);
    }
    log::Tagged("MACRO", "t=%.1fs %s", start, what);
}

} // namespace

// Chamado pelo GnmDriver no fim de cada frame.
void PadMacroTick(u64 draws, u64 stable_frames) {
    if (!g_macro_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const double now = Seconds();
    switch (g_macro_state) {
    case 0: // espera a tela de titulo
        // Tentativa de pular a abertura (Fase 17): entre 35 s e 200 s, enquanto a tela de titulo
        // nao aparece, alterna X, O e triangulo - um toque a cada 1,5 s e, a cada quarta vez, os
        // tres segurados por 1,2 s (alguns jogos da TT pedem segurar).
        // Tambem para no menu principal (175/176 draws): um toque que passa da tela de titulo
        // antes de ela ser reconhecida nao pode continuar e acionar "Novo Jogo" (Fase 19).
        if (g_skip_spam.load(std::memory_order_relaxed) && now >= 35.0 && now < 200.0 && draws != 134 && draws != 136 &&
            draws != 175 && draws != 176 && now - g_spam_time >= 1.5) {
            static const u32 kSkipButtons[3] = {kCross, kCircle, kTriangle};
            g_spam_time = now;
            ++g_spam_count;
            std::lock_guard lock(g_script_mutex);
            ScriptStep step{};
            step.start_s = now;
            if (g_spam_count % 4 == 0) {
                step.button = kCross | kCircle | kTriangle;
                step.end_s = now + 1.2;
            } else {
                step.button = kSkipButtons[g_spam_count % 3];
                step.end_s = now + 0.3;
                step.reads_left = 2;
            }
            g_script.push_back(step);
        }
        if ((draws == 134 || draws == 136) && stable_frames >= 60) {
            if (g_spam_count) {
                log::Tagged("MACRO", "t=%.1fs %d X / O / triangle taps sent since 35 s to try skipping intro", now,
                            g_spam_count);
            }
            MacroPress(kOptions, 0.0, 0.3, "title screen (134/136 draws) idle: OPTIONS");
            g_macro_state = 1;
            g_macro_time = now;
        } else if ((draws == 175 || draws == 176) && stable_frames >= 30) {
            log::Tagged("MACRO", "t=%.1fs already at main menu (one of %d intro-skip taps passed the title screen)", now,
                        g_spam_count);
            g_macro_state = 1;
            g_macro_time = now;
        }
        break;
    case 1: // espera o menu principal
        if ((draws == 175 || draws == 176) && stable_frames >= 45) {
            // Fase 32: com saves em disco o cursor ja comeca em "Carregar Jogo" (o D-PAD BAIXO que havia
            // aqui o levava para "Opcoes"). O macro so e usado com saves.
            if (std::getenv("LD_MACRO_NEWGAME")) { // testes: sobe para "Novo Jogo"
                MacroPress(kUp, 0.5, 0.1, "main menu idle: D-PAD UP (Load Game -> New Game)");
            }
            MacroPress(kCross, 2.5, 0.3, "main menu (175/176 draws) idle: X");
            g_macro_state = 2;
            g_macro_time = now + 2.5;
            g_macro_presses = 0;
        } else if (now - g_macro_time > 60.0) {
            log::Tagged("MACRO", "t=%.1fs main menu (175/176 draws) did not appear within 60 s of OPTIONS (current screen: %llu "
                                 "draws). Without a save the game passes through other screens; macro ended.",
                        now, static_cast<unsigned long long>(draws));
            g_macro_state = 3;
        }
        break;
    case 2: // confirma o slot e os avisos
        if (now - g_macro_time >= 3.0 && g_macro_presses < 3) {
            ++g_macro_presses;
            g_macro_time = now;
            char what[96];
            std::snprintf(what, sizeof(what), "confirmation X %d of 3 (current screen: %llu draws)", g_macro_presses,
                          static_cast<unsigned long long>(draws));
            if (draws == 151) {
                log::Tagged("MACRO", "t=%.1fs screen is OPTIONS (151 draws): \"Load Game\" is disabled and cursor "
                                     "jumped to \"Options\". Returning with O; macro ended.", now);
                MacroPress(kCircle, 0.0, 0.3, "O (return to main menu)");
                g_macro_state = 3;
                break;
            }
            MacroPress(kCross, 0.0, 0.3, what);
        } else if (g_macro_presses >= 3) {
            log::Tagged("MACRO", "t=%.1fs \"Load Game\" macro finished", now);
            g_macro_state = 3;
        }
        break;
    default:
        break;
    }
}

void SetLoadGameMacro(bool enabled) {
    g_macro_enabled = enabled;
}

// Com o macro ligado: tambem tenta pular a cutscene de abertura (X / O / triangulo, 35-200 s).
void SetSkipIntroSpam(bool enabled) {
    g_skip_spam = enabled;
}

bool SetPadInputScript(const char* text) {
    static const struct {
        const char* name;
        int axis;
        u8 value;
    } kAxisNames[] = {
        {"ls_left", 0, 0x00}, {"ls_right", 0, 0xFF}, {"ls_up", 1, 0x00}, {"ls_down", 1, 0xFF},
        {"rs_left", 2, 0x00}, {"rs_right", 2, 0xFF}, {"rs_up", 3, 0x00}, {"rs_down", 3, 0xFF},
    };
    std::lock_guard lock(g_script_mutex);
    g_script.clear();
    std::string s = text ? text : "";
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    double cursor = 0.0; // fim do item anterior, para os itens em sequencia
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t end = s.find(',', pos);
        const std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? s.size() : end + 1;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) {
            return false;
        }
        const size_t c2 = item.find(':', c1 + 1);
        const bool timed = std::isdigit(static_cast<unsigned char>(item[0])) || item[0] == '.';
        if (!timed && c2 != std::string::npos) {
            return false; // "nome:ms" nao tem terceiro campo
        }
        const std::string name = timed ? item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1)
                                       : item.substr(0, c1);
        ScriptStep step{};
        step.start_s = timed ? std::atof(item.c_str()) : cursor;
        const double hold_ms = timed ? (c2 == std::string::npos ? 200.0 : std::atof(item.c_str() + c2 + 1))
                                     : std::atof(item.c_str() + c1 + 1);
        step.end_s = step.start_s + hold_ms / 1000.0;
        for (const auto& b : kButtonNames) {
            if (name == b.name) {
                step.button = b.bit;
            }
        }
        for (const auto& a : kAxisNames) {
            if (name == a.name) {
                step.axis = a.axis;
                step.value = a.value;
            }
        }
        if ((!step.button && step.axis < 0) || hold_ms <= 0.0) {
            return false;
        }
        cursor = step.end_s;
        g_script.push_back(step);
    }
    return true;
}

} // namespace hle
