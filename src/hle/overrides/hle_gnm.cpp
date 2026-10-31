// libSceGnmDriver - camada base dos 57 simbolos importados. NAO ha renderizacao.
//
// O eboot so chama o driver por thunks da biblioteca Gnm embutida nele (eboot+0x1f59xxx), entao
// as assinaturas sao as publicas do GNM. O que esta camada garante:
//   1. Toda funcao que escreve em command buffer (SetVsShader, DrawInit..., marcadores) deixa
//      os `size` dwords como um pacote PM4 NOP bem formado. O estado que o jogo queria
//      programar e descartado, mas o buffer continua percorrivel.
//   2. Submit*: os DCB/CCB sao percorridos e os pacotes de sincronizacao sao EXECUTADOS -
//      EVENT_WRITE_EOP, EVENT_WRITE_EOS, RELEASE_MEM, WRITE_DATA e DMA_DATA escrevem de verdade
//      na memoria do jogo, e os que pedem interrupcao disparam o evento registrado por
//      sceGnmAddEqEvent. Tudo de forma sincrona: quando Submit retorna, os labels ja valem.
//   3. sceGnmSubmitAndFlipCommandBuffers pede o flip ao VideoOut.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

#include "hle/hle_perf.h"
#include "common/shader_metadata.h"
#include "hle/overrides/hle_equeue.h"
#include "hle/overrides/hle_override.h"
#include "hle/overrides/hle_videoout.h"
#include "render/vulkan_backend.h"
#include "runtime/crash_handler.h"
#include "runtime/lang.h"

namespace hle::gnm {

namespace {

constexpr u32 kOpNop = 0x10;
constexpr u32 kOpWriteData = 0x37;
constexpr u32 kOpWaitRegMem = 0x3C;
constexpr u32 kOpEventWriteEop = 0x47;
constexpr u32 kOpEventWriteEos = 0x48;
constexpr u32 kOpReleaseMem = 0x49;
constexpr u32 kOpDmaData = 0x50;

constexpr u64 kEventGfxEop = 0x40;    // GnmEventType: fim de pipeline grafico
constexpr u32 kHwInitPacketSize = 0x100; // dwords escritos por *InitDefaultHardwareState

// ------------------------------ estatisticas --------------------------------
std::mutex g_mutex;
std::array<u64, 256> g_opcode_count{};
u64 g_submits = 0, g_packets = 0, g_label_writes = 0, g_interrupts = 0, g_bad_writes = 0, g_malformed = 0;
u32 g_logged_labels = 0;
// Janela para o monitor de "draws por frame": uma mudanca brusca indica troca de tela.
u64 g_window_draws = 0, g_window_frames = 0, g_window_packets = 0;
constexpr u64 kWindowFrames = 120;
u64 g_dma_non_memory = 0; // DMA_DATA cujo destino/origem nao e memoria (prefetch, GDS)
double g_window_start_seconds = 0.0;
bool g_window_had_dump = false;
u32 g_auto_profiles = 0;
std::atomic<bool> g_profile_request{false};
} // namespace
} // namespace hle::gnm
namespace hle {
void PrintNgs2VoiceReport(); // hle_audio.cpp
void PadMacroTick(u64 draws, u64 stable_frames); // hle_pad.cpp
bool PadControllerPresent(int slot); // hle_pad.cpp (Fase 61)
}
#include "hle/overrides/hle_pad_sdl.h"
namespace hle::gnm {
namespace {

// Fase 45: gravacoes de arquivo fora da thread de render. O D: e um HD: abrir e acrescentar num
// arquivo custa de 3 a 10 ms, e uma troca de cena nova trazia centenas de shaders para gravar
// (na sessao do usuario, 182 num frame = 1,1 s parado). Uma thread so, na ordem dos pedidos.
struct FileWrite {
    std::filesystem::path path;
    std::string data;
    bool append = false;
    bool skip_if_exists = false;
    std::string log_line; // impresso depois de gravar (so se gravou)
};
std::mutex g_file_mutex;
std::condition_variable g_file_cv, g_file_idle_cv;
std::deque<FileWrite> g_file_jobs;
bool g_file_started = false; // com g_file_mutex
bool g_file_busy = false;
std::atomic<u64> g_file_writes{0};

void FileWriterLoop() {
    for (;;) {
        std::deque<FileWrite> batch;
        {
            std::unique_lock lock(g_file_mutex);
            g_file_cv.wait(lock, [] { return !g_file_jobs.empty(); });
            batch.swap(g_file_jobs);
            g_file_busy = true;
        }
        std::FILE* open = nullptr; // acrescimos seguidos no mesmo arquivo: abre uma vez so
        std::filesystem::path open_path;
        std::error_code ec;
        for (FileWrite& job : batch) {
            if (!(open && job.append && job.path == open_path)) {
                if (open) {
                    std::fclose(open);
                    open = nullptr;
                }
                if (job.skip_if_exists && std::filesystem::exists(job.path, ec)) {
                    continue;
                }
                open = _wfopen(job.path.c_str(), job.append ? L"ab" : L"wb");
                open_path = job.path;
                if (!open) {
                    std::filesystem::create_directories(job.path.parent_path(), ec);
                    open = _wfopen(job.path.c_str(), job.append ? L"ab" : L"wb");
                }
            }
            if (open) {
                std::fwrite(job.data.data(), 1, job.data.size(), open);
                ++g_file_writes;
                if (!job.log_line.empty()) {
                    log::Tagged("SHADER EXTRACT", "%s", job.log_line.c_str());
                }
                if (!job.append) {
                    std::fclose(open);
                    open = nullptr;
                }
            }
        }
        if (open) {
            std::fclose(open);
        }
        {
            std::lock_guard lock(g_file_mutex);
            g_file_busy = false;
        }
        g_file_idle_cv.notify_all();
    }
}

void WriteFileAsync(FileWrite job) {
    {
        std::lock_guard lock(g_file_mutex);
        if (!g_file_started) { // (joinable() nao serve: depois do detach ele volta a ser falso)
            g_file_started = true;
            std::thread(FileWriterLoop).detach(); // o processo sai sem esperar; o relatorio final chama FlushFileWrites
        }
        g_file_jobs.push_back(std::move(job));
    }
    g_file_cv.notify_one();
}

void FlushFileWrites() {
    std::unique_lock lock(g_file_mutex);
    g_file_idle_cv.wait_for(lock, std::chrono::seconds(10), [] { return g_file_jobs.empty() && !g_file_busy; });
}

std::string Format(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer, n < 0 ? 0 : std::min<size_t>(n, sizeof(buffer) - 1));
}

struct EopListener {
    equeue::Handle queue;
    u64 id;
};
std::vector<EopListener> g_listeners;

const auto g_epoch = std::chrono::steady_clock::now();

u64 GpuClock() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_epoch).count();
}


// VirtualQuery custa caro neste processo: numa regiao grande (os mapeamentos de varios GB do
// jogo) o kernel percorre as paginas para achar onde o estado muda, e cada consulta leva
// dezenas de microssegundos. O dump de frame faz milhares de consultas por draw, entao dentro
// dele (RegionCacheScope) as regioes ja consultadas ficam guardadas. Fora do dump nao ha
// cache: a memoria pode ser desmapeada entre uma chamada e outra.
struct CachedRegion {
    u64 begin = 0, end = 0;
    bool readable = false, writable = false;
};
thread_local std::vector<CachedRegion> t_region_cache;
thread_local int t_region_cache_depth = 0;

struct RegionCacheScope {
    RegionCacheScope() {
        ++t_region_cache_depth;
    }
    ~RegionCacheScope() {
        if (--t_region_cache_depth == 0) {
            t_region_cache.clear();
        }
    }
};

bool Readable(u64 address, u64 size) {
    if (!address) {
        return false;
    }
    if (t_region_cache_depth) {
        for (const CachedRegion& region : t_region_cache) {
            if (address >= region.begin && address < region.end) {
                return region.readable && address + size <= region.end;
            }
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        return false;
    }
    const u64 begin = reinterpret_cast<u64>(mbi.BaseAddress), end = begin + mbi.RegionSize;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    const DWORD write_bits = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (t_region_cache_depth && t_region_cache.size() < 4096) {
        t_region_cache.push_back({begin, end, readable, readable && (mbi.Protect & write_bits) != 0});
    }
    return readable && address + size <= end;
}

// Os enderecos vem de dentro de um command buffer do jogo: confere antes de escrever.
// Fase 35: usa o mesmo cache de regioes do Readable. Sem ele, cada DMA_DATA / WRITE_DATA pagava um
// VirtualQuery (centenas de microssegundos nas regioes grandes do jogo).
bool Writable(u64 address, u64 size) {
    if (!address) {
        return false;
    }
    if (t_region_cache_depth) {
        for (const CachedRegion& region : t_region_cache) {
            if (address >= region.begin && address < region.end) {
                return region.writable && address + size <= region.end;
            }
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        return false;
    }
    const u64 begin = reinterpret_cast<u64>(mbi.BaseAddress), end = begin + mbi.RegionSize;
    const DWORD write_bits = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    const bool writable = readable && (mbi.Protect & write_bits) != 0;
    if (t_region_cache_depth && t_region_cache.size() < 4096) {
        t_region_cache.push_back({begin, end, readable, writable});
    }
    return writable && address + size <= end;
}

u32 g_logged_refusals = 0;

// Destino de escrita que nao e memoria gravavel do processo: conta e mostra os primeiros.
void Refuse(const char* packet, u64 address, u64 size, u32 control) {
    ++g_bad_writes;
    if (g_logged_refusals < 6) {
        ++g_logged_refusals;
        log::Call("PM4", 0, "%s REFUSED: destination 0x%" PRIx64 " (%" PRIu64 " bytes) is not writable; control=0x%08x",
                  packet, address, size, control);
    }
}

void WriteLabel(const char* packet, u64 address, const void* data, u64 size) {
    if (!Writable(address, size)) {
        Refuse(packet, address, size, 0);
        return;
    }
    std::memcpy(reinterpret_cast<void*>(address), data, size);
    ++g_label_writes;
    if (g_logged_labels < 12) {
        ++g_logged_labels;
        u64 value = 0;
        std::memcpy(&value, data, size < 8 ? size : 8);
        log::Call("PM4", 0, "%s wrote %" PRIu64 " bytes at 0x%" PRIx64 " (value 0x%" PRIx64 ")", packet, size,
                  address, value);
    }
}

void SignalInterrupt(u64 event_id) {
    ++g_interrupts;
    for (const EopListener& listener : g_listeners) {
        if (listener.id == event_id) {
            // sceGnmGetEqEventType le o tipo de evento em `data`.
            equeue::Trigger(listener.queue, event_id, equeue::kFilterGraphicsCore, static_cast<s64>(event_id));
        }
    }
}

// data_sel dos pacotes EOP / RELEASE_MEM
void WriteSelected(const char* packet, u32 data_sel, u64 address, u32 data_lo, u32 data_hi) {
    switch (data_sel) {
    case 1: // 32 bits
        WriteLabel(packet, address, &data_lo, 4);
        break;
    case 2: { // 64 bits
        const u64 value = data_lo | (static_cast<u64>(data_hi) << 32);
        WriteLabel(packet, address, &value, 8);
        break;
    }
    case 3:   // relogio da GPU
    case 4: { // contador de performance
        const u64 value = GpuClock();
        WriteLabel(packet, address, &value, 8);
        break;
    }
    default: // 0 = nada, 5 = copia do GDS (sem GDS emulado)
        break;
    }
}

// ============================ dump de frame (Fase 10) ===========================
// Quando a contagem de draws fica IDENTICA por kStableFrames frames seguidos (tela parada) e
// essa contagem ainda nao foi despejada, os command buffers do frame sao percorridos de novo,
// agora acompanhando os registradores, e cada draw sai com viewport, alvo de cor e as
// texturas ligadas a ele. Nao ha renderizacao: e so a leitura do que o jogo mandou.
//
// De onde vem as texturas: o jogo nao usa a RAM do constant engine (nenhum WRITE_CONST_RAM);
// os recursos chegam por SET_SH_REG nos registradores de "user data" do pixel shader
// (0x2C0C..0x2C1B) e do vertex shader (0x2C4C..0x2C5B), como descritor T# embutido (8 dwords)
// ou como ponteiro para uma tabela de descritores na memoria. As duas formas sao testadas, e
// um descritor so e aceito se passar na validacao de ValidTexture (heuristica).
constexpr u64 kStableFrames = 45;
constexpr u64 kMaxDumpDraws = ~0ull; // sem limite de draws (Fase 13)
constexpr u32 kMaxDumps = 64;
constexpr u32 kMaxClassifyIndices = 1u << 21; // draws maiores nao sao analisados pelo radar de geometria

struct SubmittedBuffer {
    const u32* cmd;
    u64 size_bytes;
};
std::vector<SubmittedBuffer> g_frame_buffers; // DCBs do frame em montagem
u64 g_frame_draws = 0;
u64 g_stable_count = ~0ull, g_stable_frames = 0;
std::set<u64> g_dumped_counts, g_listed_counts;
std::filesystem::path g_dump_dir;
bool g_dump_raw_textures = true;

struct TextureInfo {
    u64 address = 0;
    u32 width = 0, height = 0, pitch = 0, depth = 0;
    u32 format = 0, num_format = 0, tiling = 0, type = 0, levels = 0;
    u32 swizzle = 0; // dst_sel x/y/z/w (3 bits cada): 0 = zero, 1 = um, 4 = R, 5 = G, 6 = B, 7 = A
    bool operator<(const TextureInfo& o) const {
        return std::tie(address, width, height, format) < std::tie(o.address, o.width, o.height, o.format);
    }
};

const char* FormatName(u32 format) {
    switch (format) {
    case 0x01: return "R8";
    case 0x02: return "R16";
    case 0x03: return "R8G8";
    case 0x04: return "R32";
    case 0x05: return "R16G16";
    case 0x06: return "R10G11B11";
    case 0x07: return "R11G11B10";
    case 0x08: return "R10G10B10A2";
    case 0x09: return "A2R10G10B10";
    case 0x0A: return "RGBA8";
    case 0x0B: return "R32G32";
    case 0x0C: return "RGBA16";
    case 0x0D: return "R32G32B32";
    case 0x0E: return "RGBA32";
    case 0x10: return "R5G6B5";
    case 0x11: return "A1R5G5B5";
    case 0x12: return "R5G5B5A1";
    case 0x13: return "RGBA4";
    case 0x14: return "D8_24";
    case 0x15: return "D24_8";
    case 0x16: return "X24_8_32";
    case 0x20: return "GB_GR";
    case 0x21: return "BG_RG";
    case 0x22: return "RGB9E5";
    case 0x23: return "BC1";
    case 0x24: return "BC2";
    case 0x25: return "BC3";
    case 0x26: return "BC4";
    case 0x27: return "BC5";
    case 0x28: return "BC6";
    case 0x29: return "BC7";
    default: return nullptr;
    }
}

// Bytes do nivel 0 (sem mips nem padding de tiling), so para o tamanho do arquivo bruto.
u64 Level0Bytes(const TextureInfo& t) {
    const u64 pixels = static_cast<u64>(t.pitch ? t.pitch : t.width) * t.height;
    switch (t.format) {
    case 0x23: case 0x26: return pixels / 2;
    case 0x24: case 0x25: case 0x27: case 0x28: case 0x29: case 0x01: return pixels;
    case 0x02: case 0x03: case 0x10: case 0x11: case 0x12: case 0x13: return pixels * 2;
    case 0x0B: case 0x0C: return pixels * 8;
    case 0x0E: return pixels * 16;
    default: return pixels * 4;
    }
}

// "RGBA", "BGRA", "RRR1"...: de qual canal da memoria sai cada componente lido pelo shader.
std::string SwizzleName(u32 swizzle) {
    static const char kNames[] = {'0', '1', '?', '?', 'R', 'G', 'B', 'A'};
    std::string out;
    for (u32 i = 0; i < 4; ++i) {
        out += kNames[(swizzle >> (i * 3)) & 7];
    }
    return out;
}

bool ValidTexture(const u32* w, TextureInfo* out) {
    TextureInfo t;
    t.type = w[3] >> 28;
    if (t.type != 0x9 && t.type != 0xA && t.type != 0xB && t.type != 0xD) { // 2D, 3D, cubo, array 2D
        return false;
    }
    t.format = (w[1] >> 20) & 0x3F;
    t.num_format = (w[1] >> 26) & 0xF;
    t.swizzle = w[3] & 0xFFF;
    if (!FormatName(t.format) || t.num_format > 0xB) {
        return false;
    }
    t.width = (w[2] & 0x3FFF) + 1;
    t.height = ((w[2] >> 14) & 0x3FFF) + 1;
    t.depth = (w[4] & 0x1FFF) + 1;
    t.pitch = ((w[4] >> 13) & 0x3FFF) + 1;
    const u32 base_level = (w[3] >> 12) & 0xF;
    const u32 last_level = (w[3] >> 16) & 0xF;
    t.levels = last_level + 1;
    t.tiling = (w[3] >> 20) & 0x1F;
    if (base_level > last_level || last_level > 14 || t.pitch < t.width || t.pitch > 16384) {
        return false;
    }
    if (t.width == 1 && t.height == 1 && t.pitch > 64) {
        return false;
    }
    t.address = (static_cast<u64>(w[0]) << 8) | (static_cast<u64>(w[1] & 0x3F) << 40);
    if (!Readable(t.address, 64)) {
        return false;
    }
    *out = t;
    return true;
}

// Buffer de vertices (descritor V#, 4 dwords), conferido nos registradores crus do jogo:
//   {base lo, base hi[0:15] | stride[16:29], numElements, formato}
// ex.: bb5752c0 00240010 00000004 2006f3ac = 4 vertices de 36 bytes em 0x10bb5752c0.
// O jogo liga um V# por ATRIBUTO: varios descritores do mesmo buffer, com offsets diferentes.
struct VertexBufferInfo {
    u64 address = 0;
    u32 stride = 0;
    u32 count = 0;
    u32 format = 0;
};

bool ValidVertexBuffer(const u32* w, u32 min_count, VertexBufferInfo* out) {
    VertexBufferInfo v;
    v.address = w[0] | (static_cast<u64>(w[1] & 0xFFFF) << 32);
    v.stride = (w[1] >> 16) & 0x3FFF;
    v.count = w[2];
    v.format = w[3];
    if (v.stride < 4 || v.stride > 128 || v.count < min_count || v.count > 0x400000 || (w[1] >> 30) != 0 ||
        v.address < 0x10000) {
        return false;
    }
    if (!Readable(v.address, static_cast<u64>(v.stride) * std::max<u32>(min_count, 1))) {
        return false;
    }
    *out = v;
    return true;
}

struct DumpState {
    std::filesystem::path directory;
    u64 frame_draws = 0;
    u32 index_size = 2;
    std::array<u32, 0x400> sh{};
    std::array<u32, 0x400> ctx{};
    u32 prim_type = 0;
    u32 draw_index = 0;
    std::FILE* file = nullptr;
    u64 index_base = 0; // INDEX_BASE, usado por DRAW_INDEX_OFFSET_2
    std::array<u32, 5> geometry{}; // contagem por classe do radar de geometria (GeometryClass)
    u32 depth_test_draws = 0;
    std::map<TextureInfo, u32> textures; // textura -> em quantos draws aparece
    std::map<std::string, u32> viewports;
};

void CollectTextures(const u32* regs, std::set<TextureInfo>* found) {
    TextureInfo t;
    for (u32 i = 0; i + 8 <= 16; ++i) { // T# embutido
        if (ValidTexture(regs + i, &t)) {
            found->insert(t);
        }
    }
    for (u32 i = 0; i + 1 < 16; ++i) { // ponteiro para tabela de T#
        const u64 pointer = regs[i] | (static_cast<u64>(regs[i + 1]) << 32);
        if (regs[i + 1] >= 0x100 || pointer < 0x10000) {
            continue;
        }
        for (u32 slot = 0; slot < 16; ++slot) {
            const u64 address = pointer + slot * 32ull;
            if (!Readable(address, 32)) {
                break;
            }
            u32 words[8];
            std::memcpy(words, reinterpret_cast<const void*>(address), sizeof(words));
            if (ValidTexture(words, &t)) {
                found->insert(t);
            }
        }
    }
}

// ======================= radar de geometria (Fase 12) =======================
// Classifica cada draw pela caixa envolvente das POSICOES dos vertices (atributo que comeca no
// offset 0 do buffer), no espaco do modelo, antes de qualquer matriz:
//   UI 2D          Z constante (ou nulo) em todos os vertices
//   plano          Z varia, mas todos os vertices estao no mesmo plano (Z linear em X e Y)
//   Geometria 3D   Z varia e os vertices nao sao coplanares
//   tela cheia     um unico triangulo com Z constante (ou sem buffer de vertices): passe de
//                  pos-processo que cobre a tela
//   nao analisado  formato de posicao desconhecido, draw grande demais ou memoria ilegivel
enum GeometryClass : u32 { kGeo3D, kGeoPlane, kGeoUi2D, kGeoNoBuffer, kGeoUnknown };
const char* const kGeometryNames[] = {"3D geometry", "plane (linear Z)", "UI 2D", "fullscreen (single post-processing triangle)",
                                      "not analyzed"};

float HalfToFloat(u16 h) {
    const u32 exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    }
    return (h & 0x8000) ? -value : value;
}

// Formato do V# (dword 3): num_format nos bits 12-14, data_format nos bits 15-18.
// Aceita os que carregam X, Y e Z: 32_32_32 / 32_32_32_32 float e 16_16_16_16 (half, norm, scaled).
const char* PositionFormatName(u32 format) {
    const u32 num = (format >> 12) & 7, data = (format >> 15) & 0xF;
    if ((data == 0xD || data == 0xE) && num == 7) {
        return data == 0xD ? "float3" : "float4";
    }
    if (data == 0xC) {
        return num == 7 ? "half4" : num == 1 ? "snorm16x4" : num == 0 ? "unorm16x4" : num == 5 ? "sscaled16x4" : nullptr;
    }
    return nullptr;
}

void ReadPosition(const u8* p, u32 format, float out[3]) {
    const u32 num = (format >> 12) & 7, data = (format >> 15) & 0xF;
    if (data != 0xC) {
        std::memcpy(out, p, 12);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        u16 raw;
        std::memcpy(&raw, p + i * 2, 2);
        out[i] = num == 7 ? HalfToFloat(raw) : num == 1 ? static_cast<s16>(raw) / 32767.0f : num == 0 ? raw / 65535.0f
                                                                                                  : static_cast<float>(static_cast<s16>(raw));
    }
}

// ================== layout de vertices pelo fetch shader (Fase 14) ==================
// O fetch shader diz, em ordem de semantica, de qual V# (tabela e posicao) sai cada atributo
// e quantos componentes sao carregados. O V# diz buffer, stride e formato. Juntando os dois o
// dump descreve o vertice de cada draw sem supor um struct fixo.
struct FetchAttribute {
    u32 semantic, table_sgpr, dword_offset, components;
};

// Decodifica so o que um fetch shader usa: s_load_dwordx4 (SMRD) e buffer_load_format_* (MUBUF).
std::vector<FetchAttribute> ParseFetchShaderLayout(const u32* code, u32 max_dwords) {
    struct Load {
        u32 base_sgpr = 0, dword_offset = 0;
    };
    std::array<Load, 128> loads{};
    std::vector<FetchAttribute> attributes;
    for (u32 i = 0; i < max_dwords; ++i) {
        const u32 d = code[i];
        if ((d & 0xFF80FF00) == 0xBE802000) { // s_setpc_b64: volta para o vertex shader
            break;
        }
        if ((d >> 27) == 0x18) { // SMRD: sdst[21:15] <- [sbase[14:9] * 2 + offset[7:0]]
            loads[(d >> 15) & 0x7F] = {((d >> 9) & 0x3F) * 2, d & 0xFF};
        } else if ((d >> 26) == 0x38 && i + 1 < max_dwords) { // MUBUF (2 dwords)
            const u32 op = (d >> 18) & 0x7F, d1 = code[++i];
            if (op <= 3) { // buffer_load_format_x / xy / xyz / xyzw
                const Load& load = loads[((d1 >> 16) & 0x1F) * 4];
                attributes.push_back({static_cast<u32>(attributes.size()), load.base_sgpr, load.dword_offset, op + 1});
            }
        } else if ((d >> 26) == 0x3A || (d >> 26) == 0x34) { // MTBUF / VOP3: segundo dword
            ++i;
        }
    }
    return attributes;
}

// Nome legivel do formato de um V# (dword 3): tipo numerico + bits por componente x componentes.
std::string VertexFormatName(u32 format) {
    static const char* const kNum[] = {"unorm", "snorm", "uscaled", "sscaled", "uint", "sint", "snorm_nz", "float"};
    const u32 num = (format >> 12) & 7, data = (format >> 15) & 0xF;
    u32 bits = 0, count = 0;
    switch (data) {
    case 0x1: bits = 8, count = 1; break;
    case 0x2: bits = 16, count = 1; break;
    case 0x3: bits = 8, count = 2; break;
    case 0x4: bits = 32, count = 1; break;
    case 0x5: bits = 16, count = 2; break;
    case 0xA: bits = 8, count = 4; break;
    case 0xB: bits = 32, count = 2; break;
    case 0xC: bits = 16, count = 4; break;
    case 0xD: bits = 32, count = 3; break;
    case 0xE: bits = 32, count = 4; break;
    case 0x6: return "packed10_11_11";
    case 0x7: return "packed11_11_10";
    case 0x8: return std::string(kNum[num]) + "10_10_10_2";
    case 0x9: return std::string(kNum[num]) + "2_10_10_10";
    default: return "unknown";
    }
    return std::string(kNum[num]) + std::to_string(bits) + "x" + std::to_string(count);
}

s32 FetchSgprOfVertexShader(u64 vs_address); // definido junto do extrator de shaders

// Grava os vertices (e indices) de um draw pequeno em arquivos brutos, para reconstruir a
// interface fora do jogo (tools/render_frame_ui.py). So para draws de ate 4096 vertices.
void DumpGeometry(DumpState& st, u32 count, u64 index_address) {
    auto not_analysed = [&](const char* why) {
        ++st.geometry[kGeoUnknown];
        std::fprintf(st.file, "            geometry: not analyzed (%s)\n", why);
    };
    if (!count || count > kMaxClassifyIndices) {
        not_analysed(count ? "draw too large" : "unknown count");
        return;
    }
    // Arquivos brutos: geometria 3D so para draws pequenos de telas pequenas; draws das classes
    // "UI 2D" e "plano" sao gravados SEMPRE, qualquer que seja o tamanho do frame (decidido
    // depois da classificacao, mais abaixo).
    bool write_files = count <= 4096 && st.frame_draws <= 200;
    u64 index_bytes = 0;
    u32 min_index = 0, max_index = count - 1;
    char name[96];
    if (index_address) {
        const u64 bytes = static_cast<u64>(count) * st.index_size;
        if (!Readable(index_address, bytes)) {
            not_analysed("unreadable indices");
            return;
        }
        min_index = ~0u;
        max_index = 0;
        for (u32 i = 0; i < count; ++i) {
            u32 index = 0;
            std::memcpy(&index, reinterpret_cast<const u8*>(index_address) + static_cast<u64>(i) * st.index_size, st.index_size);
            max_index = std::max(max_index, index);
            min_index = std::min(min_index, index);
        }
        if (max_index > 0x3FFFFF) {
            not_analysed("index out of range");
            return;
        }
        index_bytes = bytes;
    }
    // Descritores V# do vertex shader: embutidos nos registradores e nas tabelas apontadas por
    // eles. Os de stride != 16 com elementos suficientes sao atributos de vertice (agrupados
    // por buffer); os de stride 16 sao buffers de constantes (matrizes, cores).
    struct Source {
        VertexBufferInfo v;
        char label[24];
    };
    std::vector<Source> vertex_sources, constant_sources[2]; // [0] = VS, [1] = PS
    for (u32 stage = 0; stage < 2; ++stage) {
        const u32* regs = &st.sh[stage == 0 ? 0x4C : 0x0C];
        auto consider = [&](const u32* words, const char* label) {
            Source source;
            if (!ValidVertexBuffer(words, 1, &source.v)) {
                return;
            }
            std::snprintf(source.label, sizeof(source.label), "%s", label);
            if (source.v.stride == 16) {
                constant_sources[stage].push_back(source);
            } else if (stage == 0 && source.v.count >= max_index + 1 &&
                       Readable(source.v.address, static_cast<u64>(source.v.stride) * (max_index + 1))) {
                vertex_sources.push_back(source);
            }
        };
        char label[24];
        for (u32 i = 0; i + 4 <= 16; ++i) {
            std::snprintf(label, sizeof(label), "reg%u", i);
            consider(regs + i, label);
        }
        for (u32 i = 0; i + 1 < 16; ++i) {
            const u64 pointer = regs[i] | (static_cast<u64>(regs[i + 1]) << 32);
            if (regs[i + 1] >= 0x100 || pointer < 0x10000) {
                continue;
            }
            for (u32 slot = 0; slot < 16; ++slot) {
                const u64 address = pointer + slot * 16ull;
                if (!Readable(address, 16)) {
                    break;
                }
                u32 words[4];
                std::memcpy(words, reinterpret_cast<const void*>(address), sizeof(words));
                std::snprintf(label, sizeof(label), "tab%u.%u", i, slot);
                consider(words, label);
            }
        }
    }
    std::sort(vertex_sources.begin(), vertex_sources.end(), [](const Source& a, const Source& b) {
        return std::tie(a.v.stride, a.v.address) < std::tie(b.v.stride, b.v.address);
    });
    // ---- radar de geometria: caixa envolvente da posicao e classe do draw ----
    {
        GeometryClass draw_class = kGeoUnknown;
        const Source* position = nullptr;
        bool position_own = false;
        for (const Source& source : vertex_sources) {
            if (!PositionFormatName(source.v.format)) {
                continue;
            }
            bool starts_buffer = true; // nenhum outro atributo do mesmo buffer comeca antes dele
            for (const Source& other : vertex_sources) {
                starts_buffer &= !(other.v.stride == source.v.stride && other.v.address < source.v.address &&
                                   source.v.address - other.v.address < source.v.stride);
            }
            // a entrada 0 da tabela e os descritores embutidos sao do proprio draw; as demais
            // entradas podem ser de draws vizinhos
            const size_t length = std::strlen(source.label);
            const bool own = !std::strncmp(source.label, "reg", 3) || (length >= 2 && !std::strcmp(source.label + length - 2, ".0"));
            if (starts_buffer && (!position || (own && !position_own))) {
                position = &source;
                position_own = own;
            }
        }
        const u32 depth_control = st.ctx[0x200];
        const bool depth_test = (depth_control >> 1) & 1, depth_write = (depth_control >> 2) & 1;
        st.depth_test_draws += depth_test;
        if (vertex_sources.empty()) {
            ++st.geometry[kGeoNoBuffer];
            std::fprintf(st.file, "            geometry: %s  depth test=%d write=%d\n", kGeometryNames[kGeoNoBuffer], depth_test,
                         depth_write);
        } else if (!position) {
            not_analysed("no position attribute with known format at offset 0");
        } else {
            double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30}, sum[3] = {}, product[3][3] = {};
            u64 used = 0, rejected = 0;
            const u8* base = reinterpret_cast<const u8*>(position->v.address);
            for (u32 v = min_index; v <= max_index; ++v) {
                float xyz[3];
                ReadPosition(base + static_cast<u64>(v) * position->v.stride, position->v.format, xyz);
                if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]) || std::fabs(xyz[0]) > 1e7f ||
                    std::fabs(xyz[1]) > 1e7f || std::fabs(xyz[2]) > 1e7f) {
                    ++rejected;
                    continue;
                }
                ++used;
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min<double>(lo[a], xyz[a]);
                    hi[a] = std::max<double>(hi[a], xyz[a]);
                    sum[a] += xyz[a];
                    for (int b = 0; b < 3; ++b) {
                        product[a][b] += static_cast<double>(xyz[a]) * xyz[b];
                    }
                }
            }
            if (used < 3 || rejected > used) {
                not_analysed("invalid positions (attribute at offset 0 is not position?)");
            } else {
                // Covariancia das posicoes: determinante ~0 = vertices coplanares.
                double c[3][3];
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b) {
                        c[a][b] = product[a][b] / used - (sum[a] / used) * (sum[b] / used);
                    }
                }
                const double det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
                                   c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
                const double mean_variance = (c[0][0] + c[1][1] + c[2][2]) / 3.0;
                const double flatness = mean_variance > 0 ? det / (mean_variance * mean_variance * mean_variance) : 0.0;
                const double extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
                const double z_range = hi[2] - lo[2];
                // um unico triangulo com Z constante e o triangulo de tela cheia dos passes de pos-processo
                const bool flat_z = z_range <= 1e-5 * std::max(1.0, extent);
                const GeometryClass kind = flat_z ? (count == 3 ? kGeoNoBuffer : kGeoUi2D) : flatness < 1e-6 ? kGeoPlane : kGeo3D;
                ++st.geometry[kind];
                draw_class = kind;
                std::fprintf(st.file,
                             "            geometry: %s  X[%.3f, %.3f] Y[%.3f, %.3f] Z[%.3f, %.3f]  %llu vertices (%s at +0, stride %u, "
                             "source %s)  flatness %.2e  depth test=%d write=%d\n",
                             kGeometryNames[kind], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], static_cast<unsigned long long>(used),
                             PositionFormatName(position->v.format), position->v.stride, position->label, flatness, depth_test,
                             depth_write);
            }
        }
        if ((draw_class == kGeoUi2D || draw_class == kGeoPlane) && count <= 65536) {
            write_files = true;
        }
    }
    if (!write_files) {
        return;
    }
    if (index_bytes && max_index <= 65535) {
        std::snprintf(name, sizeof(name), "geo_%03llu_draw%03u_idx%u.bin", static_cast<unsigned long long>(st.frame_draws),
                      st.draw_index, st.index_size);
        if (std::FILE* f = _wfopen((st.directory / name).c_str(), L"wb")) {
            std::fwrite(reinterpret_cast<const void*>(index_address), 1, index_bytes, f);
            std::fclose(f);
        }
    }
    struct WrittenBuffer {
        u64 address;
        u32 stride;
        std::string name;
    };
    std::vector<WrittenBuffer> written_buffers;
    std::set<u64> written;
    u32 stream = 0;
    for (size_t i = 0; i < vertex_sources.size() && stream < 8; ++i) {
        const VertexBufferInfo& base = vertex_sources[i].v;
        if (!written.insert(base.address).second) {
            continue;
        }
        std::string attributes;
        char item[48];
        for (size_t k = i; k < vertex_sources.size() && vertex_sources[k].v.stride == base.stride &&
                           vertex_sources[k].v.address - base.address < base.stride; ++k) {
            written.insert(vertex_sources[k].v.address);
            std::snprintf(item, sizeof(item), " +%llu:%08x", static_cast<unsigned long long>(vertex_sources[k].v.address - base.address),
                          vertex_sources[k].v.format);
            if (attributes.find(item) == std::string::npos) {
                attributes += item;
            }
        }
        const u64 bytes = static_cast<u64>(base.stride) * (max_index + 1);
        std::snprintf(name, sizeof(name), "geo_%03llu_draw%03u_vb%u_stride%u.bin", static_cast<unsigned long long>(st.frame_draws),
                      st.draw_index, stream++, base.stride);
        if (std::FILE* f = _wfopen((st.directory / name).c_str(), L"wb")) {
            std::fwrite(reinterpret_cast<const void*>(base.address), 1, bytes, f);
            std::fclose(f);
        }
        std::fprintf(st.file, "            vertices: %s  %u elements @0x%" PRIx64 "  source %s  attributes (offset:format):%s\n", name,
                     base.count, base.address, vertex_sources[i].label, attributes.c_str());
        written_buffers.push_back({base.address, base.stride, name});
    }
    // ---- layout: um atributo por semantica, na ordem do fetch shader do draw ----
    {
        const u64 vs_address = (static_cast<u64>(st.sh[0x48]) << 8) | (static_cast<u64>(st.sh[0x49] & 0xFF) << 40);
        const s32 sgpr = vs_address ? FetchSgprOfVertexShader(vs_address) : -1;
        const u64 fetch = sgpr >= 0 ? st.sh[0x4C + sgpr] | (static_cast<u64>(st.sh[0x4C + sgpr + 1]) << 32) : 0;
        struct Row {
            FetchAttribute attribute;
            VertexBufferInfo v;
            std::string file;
            u32 offset = 0;
            const char* role = "?";
        };
        std::vector<Row> rows;
        if (fetch >= 0x10000 && !(fetch & 3) && Readable(fetch, 4096)) {
            for (const FetchAttribute& attribute : ParseFetchShaderLayout(reinterpret_cast<const u32*>(fetch), 1024)) {
                if (attribute.table_sgpr + 1 >= 16) {
                    continue;
                }
                const u64 table = st.sh[0x4C + attribute.table_sgpr] | (static_cast<u64>(st.sh[0x4C + attribute.table_sgpr + 1]) << 32);
                const u64 descriptor = table + attribute.dword_offset * 4ull;
                Row row{attribute, {}, "-", 0, "?"};
                u32 words[4];
                if (table < 0x10000 || !Readable(descriptor, 16)) {
                    continue;
                }
                std::memcpy(words, reinterpret_cast<const void*>(descriptor), sizeof(words));
                if (!ValidVertexBuffer(words, 1, &row.v)) {
                    continue;
                }
                auto match = [&] {
                    for (const WrittenBuffer& buffer : written_buffers) {
                        if (buffer.stride == row.v.stride && row.v.address >= buffer.address &&
                            row.v.address - buffer.address < buffer.stride) {
                            row.file = buffer.name;
                            row.offset = static_cast<u32>(row.v.address - buffer.address);
                            return true;
                        }
                    }
                    return false;
                };
                // Buffer que o fetch shader usa mas que ainda nao foi gravado (por exemplo um
                // buffer de vertices de stride 16, que a heuristica acima toma por constantes):
                // grava agora, a partir do endereco do atributo.
                const u64 bytes = static_cast<u64>(row.v.stride) * (max_index + 1);
                if (!match() && stream < 16 && row.v.count >= max_index + 1 && Readable(row.v.address, bytes)) {
                    std::snprintf(name, sizeof(name), "geo_%03llu_draw%03u_vb%u_stride%u.bin",
                                  static_cast<unsigned long long>(st.frame_draws), st.draw_index, stream++, row.v.stride);
                    if (std::FILE* f = _wfopen((st.directory / name).c_str(), L"wb")) {
                        std::fwrite(reinterpret_cast<const void*>(row.v.address), 1, bytes, f);
                        std::fclose(f);
                        written_buffers.push_back({row.v.address, row.v.stride, name});
                        match();
                    }
                }
                rows.push_back(row);
            }
        }
        // Papeis por heuristica: posicao = semantica 0; cor = primeiro atributo de 4 bytes
        // normalizados; UV = primeiro atributo de 2 componentes float/half (ou, na falta, o
        // primeiro float que nao seja a posicao).
        Row* color = nullptr;
        Row* uv = nullptr;
        u32 color_score = 0;
        for (Row& row : rows) {
            const u32 num = (row.v.format >> 12) & 7, data = (row.v.format >> 15) & 0xF;
            if (row.attribute.semantic == 0) {
                row.role = "position";
            } else if (data == 0xA && num <= 1) {
                // Pode haver mais de um atributo de 4 bytes (o vertice da interface tem dois: um
                // que muda a cada vertice e a tinta, igual no draw inteiro ou quase). A cor e o
                // mais uniforme entre os primeiros vertices.
                u32 score = 1;
                u8 first[4] = {};
                for (u32 v = 0; v < 16 && v <= max_index; ++v) {
                    const u64 at = row.v.address + static_cast<u64>(row.v.stride) * v;
                    if (!Readable(at, 4)) {
                        break;
                    }
                    if (v == 0) {
                        std::memcpy(first, reinterpret_cast<const void*>(at), 4);
                    } else {
                        score += !std::memcmp(first, reinterpret_cast<const void*>(at), 4);
                    }
                }
                if (score > color_score) {
                    color_score = score;
                    color = &row;
                }
            } else if (!uv && (data == 0xB || data == 0x5) && (num == 7 || num <= 1)) {
                uv = &row;
            }
        }
        if (!uv) {
            for (Row& row : rows) {
                if (row.attribute.semantic != 0 && &row != color && ((row.v.format >> 12) & 7) == 7 && !uv) {
                    uv = &row;
                }
            }
        }
        if (color) {
            color->role = "cor";
        }
        if (uv) {
            uv->role = "uv";
        }
        for (const Row& row : rows) {
            std::fprintf(st.file,
                         "            layout: semantic=%u file=%s stride=%u offset=%u format=%s loaded=%u role=%s  (V# at "
                         "s%u+%u, 0x%08x)\n",
                         row.attribute.semantic, row.file.c_str(), row.v.stride, row.offset, VertexFormatName(row.v.format).c_str(),
                         row.attribute.components, row.role, row.attribute.table_sgpr, row.attribute.dword_offset, row.v.format);
        }
        if (rows.empty()) {
            std::fprintf(st.file, "            layout: (no known fetch shader for this draw)\n");
        }
    }
    for (u32 stage = 0; stage < 2; ++stage) {
        std::set<u64> seen;
        u32 buffer_index = 0;
        for (const Source& source : constant_sources[stage]) {
            const u32 elements = std::min<u32>(source.v.count, 64);
            if (buffer_index >= 24 || !seen.insert(source.v.address).second || !Readable(source.v.address, elements * 16ull)) {
                continue;
            }
            std::snprintf(name, sizeof(name), "geo_%03llu_draw%03u_%s%u.bin", static_cast<unsigned long long>(st.frame_draws),
                          st.draw_index, stage == 0 ? "vcb" : "pcb", buffer_index++);
            if (std::FILE* f = _wfopen((st.directory / name).c_str(), L"wb")) {
                std::fwrite(reinterpret_cast<const void*>(source.v.address), 1, elements * 16ull, f);
                std::fclose(f);
            }
            std::fprintf(st.file, "            constants %s: %s  %u elements @0x%" PRIx64 "  source %s\n", stage == 0 ? "VS" : "PS", name,
                         source.v.count, source.v.address, source.label);
        }
    }
}

// Fatores de blend do registrador CB_BLENDn_CONTROL (GCN).
const char* BlendFactor(u32 factor) {
    static const char* const names[] = {"ZERO", "ONE", "SRC_COLOR", "ONE_MINUS_SRC_COLOR", "SRC_ALPHA", "ONE_MINUS_SRC_ALPHA",
                                        "DST_ALPHA", "ONE_MINUS_DST_ALPHA", "DST_COLOR", "ONE_MINUS_DST_COLOR", "SRC_ALPHA_SATURATE",
                                        "?11", "?12", "CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR", "SRC1_COLOR",
                                        "ONE_MINUS_SRC1_COLOR", "SRC1_ALPHA", "ONE_MINUS_SRC1_ALPHA", "CONSTANT_ALPHA",
                                        "ONE_MINUS_CONSTANT_ALPHA"};
    return factor < std::size(names) ? names[factor] : "?";
}

const char* BlendFunction(u32 function) {
    static const char* const names[] = {"ADD", "SUBTRACT", "MIN", "MAX", "REVERSE_SUBTRACT"};
    return function < std::size(names) ? names[function] : "?";
}

void DumpDraw(DumpState& st, const char* op, u32 count, u64 index_address = 0) {
    float vp[4]; // xscale, xoffset, yscale, yoffset
    std::memcpy(&vp[0], &st.ctx[0x10F], 4);
    std::memcpy(&vp[1], &st.ctx[0x110], 4);
    std::memcpy(&vp[2], &st.ctx[0x111], 4);
    std::memcpy(&vp[3], &st.ctx[0x112], 4);
    const float w = 2.0f * std::fabs(vp[0]), h = 2.0f * std::fabs(vp[2]);
    const u64 target = static_cast<u64>(st.ctx[0x318]) << 8;
    char viewport[96];
    std::snprintf(viewport, sizeof(viewport), "viewport %.0fx%.0f at (%.0f,%.0f) target 0x%" PRIx64, w, h, vp[1] - std::fabs(vp[0]),
                  vp[3] - std::fabs(vp[2]), target);
    ++st.viewports[viewport];

    std::set<TextureInfo> found;
    CollectTextures(&st.sh[0x0C], &found); // pixel shader
    const size_t ps_count = found.size();
    CollectTextures(&st.sh[0x4C], &found); // vertex shader
    std::fprintf(st.file, "draw %3u  %-16s %6u %s  prim=%u  %s  textures: %zu (PS %zu)\n", st.draw_index, op, count,
                 count % 6 == 0 && count >= 6 ? "(multiple of 6: quads)" : "", st.prim_type, viewport, found.size(), ps_count);
    for (const TextureInfo& t : found) {
        ++st.textures[t];
        std::fprintf(st.file, "            %5ux%-5u %-12s numfmt %u type 0x%x levels %u tiling %u pitch %u @0x%" PRIx64 " swizzle=%s%s\n",
                     t.width, t.height, FormatName(t.format), t.num_format, t.type, t.levels, t.tiling, t.pitch, t.address,
                     SwizzleName(t.swizzle).c_str(), t.width >= 4 * t.height ? "  <- wide (text/button?)" : "");
    }
    // Estado de blend do alvo de cor 0, como o jogo programou por SET_CONTEXT_REG neste frame:
    //   CB_BLEND0_CONTROL (0xA1E0): fatores e funcao; bit 30 = blend ligado
    //   CB_COLOR_CONTROL (0xA202), CB_TARGET_MASK (0xA08E), DB_DEPTH_CONTROL (0xA200),
    //   scissor generico (0xA090 / 0xA091)
    {
        const u32 blend = st.ctx[0x1E0];
        const bool enabled = (blend >> 30) & 1;
        const bool separate = (blend >> 29) & 1;
        const u32 tl = st.ctx[0x90], br = st.ctx[0x91];
        std::fprintf(st.file,
                     "            blend: %s color src=%s dst=%s func=%s | alpha src=%s dst=%s func=%s | control=0x%08x color_control=0x%08x "
                     "mask=0x%x depth_control=0x%08x scissor=(%u,%u)-(%u,%u)\n",
                     enabled ? "ENABLED" : "disabled", BlendFactor(blend & 0x1F), BlendFactor((blend >> 8) & 0x1F),
                     BlendFunction((blend >> 5) & 0x7), BlendFactor(separate ? (blend >> 16) & 0x1F : blend & 0x1F),
                     BlendFactor(separate ? (blend >> 24) & 0x1F : (blend >> 8) & 0x1F),
                     BlendFunction(separate ? (blend >> 21) & 0x7 : (blend >> 5) & 0x7), blend, st.ctx[0x202], st.ctx[0x8E] & 0xF,
                     st.ctx[0x200], tl & 0x7FFF, (tl >> 16) & 0x7FFF, br & 0x7FFF, (br >> 16) & 0x7FFF);
    }
    DumpGeometry(st, count, index_address);
    if (st.frame_draws <= 24) { // telas pequenas: registradores crus, para conferir as heuristicas
        for (const u32 base : {0x4Cu, 0x0Cu}) {
            std::fprintf(st.file, "            %s user data:", base == 0x4C ? "VS" : "PS");
            for (u32 i = 0; i < 16; ++i) {
                std::fprintf(st.file, " %08x", st.sh[base + i]);
            }
            std::fprintf(st.file, "\n");
            for (u32 i = 0; i + 1 < 16; ++i) {
                const u64 pointer = st.sh[base + i] | (static_cast<u64>(st.sh[base + i + 1]) << 32);
                if (st.sh[base + i + 1] >= 0x100 || pointer < 0x10000 || !Readable(pointer, 96)) {
                    continue;
                }
                std::fprintf(st.file, "              [%u] -> 0x%" PRIx64 ":", i, pointer);
                for (u32 k = 0; k < 24; ++k) {
                    u32 word;
                    std::memcpy(&word, reinterpret_cast<const u8*>(pointer) + k * 4, 4);
                    std::fprintf(st.file, " %08x", word);
                }
                std::fprintf(st.file, "\n");
            }
        }
    }
    ++st.draw_index;
}

void DumpWalk(DumpState& st, const u32* cmd, u64 size_bytes) {
    const u64 total = size_bytes / 4;
    u64 pos = 0;
    while (pos < total) {
        const u32 header = cmd[pos];
        const u32 type = header >> 30;
        if (type == 2) {
            ++pos;
            continue;
        }
        const u32 count = (header >> 16) & 0x3FFF;
        if (type == 0) {
            pos += count + 2;
            continue;
        }
        if (type != 3 || pos + count + 2 > total) {
            return;
        }
        const u32 opcode = (header >> 8) & 0xFF;
        const u32* body = cmd + pos + 1;
        const u32 body_dwords = count + 1;
        switch (opcode) {
        case 0x76: // SET_SH_REG
        case 0x69: // SET_CONTEXT_REG
            for (u32 i = 1; i < body_dwords; ++i) {
                const u32 reg = (body[0] & 0xFFFF) + i - 1;
                if (reg < 0x400) {
                    (opcode == 0x76 ? st.sh : st.ctx)[reg] = body[i];
                }
            }
            break;
        case 0x79: // SET_UCONFIG_REG: 0x242 = VGT_PRIMITIVE_TYPE
            if (body_dwords >= 2 && (body[0] & 0xFFFF) == 0x242) {
                st.prim_type = body[1];
            }
            break;
        case 0x2D: // DRAW_INDEX_AUTO {vertices, initiator}
            DumpDraw(st, "DRAW_INDEX_AUTO", body[0]);
            break;
        case 0x26: // INDEX_BASE {lo, hi}
            if (body_dwords >= 2) {
                st.index_base = body[0] | (static_cast<u64>(body[1] & 0xFF) << 32);
            }
            break;
        case 0x2A: // INDEX_TYPE: 0 = 16 bits, 1 = 32 bits
            st.index_size = (body[0] & 1) ? 4 : 2;
            break;
        case 0x27: // DRAW_INDEX_2 {max, base_lo, base_hi, indices, initiator}
            DumpDraw(st, "DRAW_INDEX_2", body_dwords >= 4 ? body[3] : 0,
                     body_dwords >= 4 ? (body[1] | (static_cast<u64>(body[2]) << 32)) : 0);
            break;
        case 0x35: // DRAW_INDEX_OFFSET_2 {max, offset, indices, initiator}
            DumpDraw(st, "DRAW_INDEX_OFFSET_2", body_dwords >= 3 ? body[2] : 0,
                     body_dwords >= 3 && st.index_base ? st.index_base + static_cast<u64>(body[1]) * st.index_size : 0);
            break;
        case 0x25:
        case 0x24:
            DumpDraw(st, "DRAW_INDIRECT", 0);
            break;
        default:
            break;
        }
        pos += count + 2;
    }
}

// Chamado com g_mutex, logo depois de processar o ultimo buffer do frame.
void DumpFrame(u64 draws) {
    static perf::Counter perf_counter("frame dump");
    const perf::Scope perf_scope(perf_counter);
    g_window_had_dump = true;
    std::error_code ec;
    std::filesystem::create_directories(g_dump_dir, ec);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count();
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%03llu_draws_t%03.0fs.txt", static_cast<unsigned long long>(draws), seconds);
    const std::filesystem::path path = g_dump_dir / name;
    const RegionCacheScope region_cache;
    DumpState st;
    st.directory = g_dump_dir;
    st.frame_draws = draws;
    st.file = _wfopen(path.c_str(), L"w");
    if (!st.file) {
        log::Tagged("FRAME DUMP", "could not open %s", path.string().c_str());
        return;
    }
    std::fprintf(st.file, "Stable frame with %llu draws, t=%.1fs, submit %llu, %zu command buffer(s)\n\n",
                 static_cast<unsigned long long>(draws), seconds, static_cast<unsigned long long>(g_submits), g_frame_buffers.size());
    for (const SubmittedBuffer& buffer : g_frame_buffers) {
        DumpWalk(st, buffer.cmd, buffer.size_bytes);
    }

    // texturas distintas, da mais usada para a menos usada
    std::vector<std::pair<u32, TextureInfo>> ranked;
    for (const auto& [texture, uses] : st.textures) {
        ranked.emplace_back(uses, texture);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(st.file, "\n==== %zu distinct textures ====\n", ranked.size());
    u32 raw_written = 0;
    for (const auto& [uses, t] : ranked) {
        char raw_name[96] = "";
        const u64 bytes = Level0Bytes(t);
        if (g_dump_raw_textures && raw_written < 600 && bytes && bytes <= (32u << 20) && Readable(t.address, bytes)) {
            std::snprintf(raw_name, sizeof(raw_name), "tex_%llx_%ux%u_p%u_%s_t%u.bin", static_cast<unsigned long long>(t.address),
                          t.width, t.height, t.pitch, FormatName(t.format), t.tiling);
            if (std::FILE* raw = _wfopen((g_dump_dir / raw_name).c_str(), L"wb")) {
                std::fwrite(reinterpret_cast<const void*>(t.address), 1, bytes, raw);
                std::fclose(raw);
                ++raw_written;
            }
        }
        std::fprintf(st.file, "%4u draw(s)  %5ux%-5u %-12s type 0x%x levels %u tiling %u @0x%" PRIx64 "  %s\n", uses, t.width, t.height,
                     FormatName(t.format), t.type, t.levels, t.tiling, t.address, raw_name);
    }
    std::fprintf(st.file, "\n==== geometry radar ====\n");
    for (u32 kind = 0; kind < st.geometry.size(); ++kind) {
        std::fprintf(st.file, "%4u draw(s)  %s\n", st.geometry[kind], kGeometryNames[kind]);
    }
    std::fprintf(st.file, "%4u draw(s)  with depth testing enabled\n", st.depth_test_draws);
    const char* verdict = st.geometry[kGeo3D] ? "3D SCENE" : st.geometry[kGeoUi2D] ? "2D interface only" : "no recognized geometry";
    std::fprintf(st.file, "verdict: %s\n", verdict);
    log::Tagged("GEO RADAR", "t=%.0fs frame with %llu draws: %u 3D geometry, %u plane, %u 2D UI, %u fullscreen (post-processing), %u not "
                             "analyzed; %u with depth testing -> %s",
                seconds, static_cast<unsigned long long>(draws), st.geometry[kGeo3D], st.geometry[kGeoPlane], st.geometry[kGeoUi2D],
                st.geometry[kGeoNoBuffer], st.geometry[kGeoUnknown], st.depth_test_draws, verdict);
    std::fprintf(st.file, "\n==== viewports ====\n");
    for (const auto& [viewport, uses] : st.viewports) {
        std::fprintf(st.file, "%4u draw(s)  %s\n", uses, viewport.c_str());
    }
    std::fclose(st.file);

    log::Tagged("FRAME DUMP", "t=%.0fs static screen with %llu draws: %u draws read, %zu distinct textures -> %s", seconds,
                static_cast<unsigned long long>(draws), st.draw_index, ranked.size(), path.string().c_str());
    // No terminal a lista so sai para telas novas: contagens vizinhas (+-3) de uma tela ja
    // listada sao quase sempre a mesma tela com um elemento a mais ou a menos.
    bool listed_neighbor = false;
    for (const u64 other : g_listed_counts) {
        listed_neighbor |= (other > draws ? other - draws : draws - other) <= 3;
    }
    g_listed_counts.insert(draws);
    if (listed_neighbor) {
        return;
    }
    u32 shown = 0;
    for (const auto& [uses, t] : ranked) {
        if (shown++ >= 14) {
            log::Tagged("FRAME DUMP", "  ... and %zu more (see file)", ranked.size() - 14);
            break;
        }
        log::Tagged("FRAME DUMP", "  %3u draw(s)  %5ux%-5u %-10s %u level(s)%s", uses, t.width, t.height, FormatName(t.format), t.levels,
                    t.width >= 4 * t.height ? "  <- wide (text/button?)" : "");
    }
}

// ====================== watchdog de cutscene (Fase 13) ======================
// Se a contagem de draws ficar identica por mais de g_watchdog_seconds, uma thread propria
// imprime o estado de todas as threads do jogo, das vozes do Ngs2 e das bibliotecas de video.
std::atomic<u64> g_wd_draws{0};      // contagem estavel atual
std::atomic<s64> g_wd_since_ms{0};   // desde quando (ms desde g_epoch)
std::atomic<s64> g_wd_last_frame_ms{0};
std::atomic<u32> g_watchdog_seconds{0};
std::atomic<bool> g_watchdog_started{false};

std::vector<s64> g_forced_dumps_ms; // horarios de --frame-dump-at ainda pendentes (-1 = feito)
std::vector<s64> g_profile_at_ms;   // horarios de --profile-at
std::atomic<u64> g_stable_count_seen{0}; // draws do ultimo frame, para o titulo do perfil

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_epoch).count();
}

void CutsceneWatchdogThread() {
    s64 reported_since = -1; // periodo estavel ja relatado
    s64 next_report_ms = 0;
    u32 reports = 0, reports_this_period = 0;
    for (;;) {
        Sleep(1000);
        // Perfil de CPU pedido por uma janela lenta ou por --profile-at.
        bool profile = g_profile_request.exchange(false);
        for (s64& at_ms : g_profile_at_ms) {
            if (at_ms >= 0 && NowMs() >= at_ms) {
                at_ms = -1;
                profile = true;
            }
        }
        if (profile) {
            char title[96];
            std::snprintf(title, sizeof(title), "t=%.0f s, frame with %llu draws", NowMs() / 1000.0,
                          static_cast<unsigned long long>(g_stable_count_seen.load()));
            log::FlushGameText();
            if (const char* deep = std::getenv("LD_PROFILE_DEEP")) { // Fase 59: pilha inteira de uma thread (ex.: Render)
                runtime::ProfileThreadDeep(deep, 6.0, title);
            } else {
                runtime::SampleThreads(4.0, title);
            }
        }
        const u32 limit = g_watchdog_seconds.load();
        const s64 since = g_wd_since_ms.load(), now = NowMs();
        const u64 draws = g_wd_draws.load();
        if (!limit || !since || now - since < limit * 1000ll || reports >= 24) {
            continue;
        }
        if (reported_since != since) {
            reported_since = since;
            reports_this_period = 0;
            next_report_ms = 0;
        }
        if (now < next_report_ms || reports_this_period >= 3) {
            continue;
        }
        ++reports;
        ++reports_this_period;
        next_report_ms = now + 150000; // repete a cada 150 s enquanto a tela nao mudar, ate 3 vezes
        const double stuck = (now - since) / 1000.0;
        const double since_frame = (now - g_wd_last_frame_ms.load()) / 1000.0;
        log::FlushGameText();
        char title[128];
        std::snprintf(title, sizeof(title), "CUTSCENE WATCHDOG #%u: %llu draws/frame for %.0f s (t=%.0f s)", reports,
                      static_cast<unsigned long long>(draws), stuck, now / 1000.0);
        runtime::DumpAllThreads(title);
        std::printf("---- frames: last frame submitted %.2f s ago (%s)\n", since_frame,
                    since_frame > 2.0 ? "THE GAME STOPPED SUBMITTING FRAMES" : "the game keeps rendering the same screen");
        PrintNgs2VoiceReport();
        std::string detail;
        const u64 av = log::StubCallsWithPrefix("libSceAvPlayer", &detail);
        const u64 vd = log::StubCallsWithPrefix("libSceVideodec", &detail) + log::StubCallsWithPrefix("libSceVideoCore", &detail);
        std::printf("---- video: libSceAvPlayer %llu call(s), libSceVideodec/VideoCore %llu call(s)%s%s\n",
                    static_cast<unsigned long long>(av), static_cast<unsigned long long>(vd), detail.empty() ? "" : ": ",
                    detail.c_str());
        std::printf("     (eboot imports libSceAvPlayer, all stubbed; does not import libSceVideodec. Zero calls = no "
                    "FMV in progress)\n");
        std::printf("======================== end of watchdog #%u ========================\n", reports);
        std::fflush(stdout);
    }
}

// Fim de frame: decide se a tela esta parada e ainda nao foi despejada. Chamado com g_mutex.
void EndFrame() {
    const u64 draws = g_frame_draws;
    g_wd_last_frame_ms = NowMs();
    g_stable_count_seen = draws;
    PadMacroTick(draws, draws == g_stable_count ? g_stable_frames + 1 : 1);
    if (draws == g_stable_count) {
        ++g_stable_frames;
    } else {
        g_stable_count = draws;
        g_stable_frames = 1;
        g_wd_draws = draws;
        g_wd_since_ms = NowMs();
    }
    // Dump pedido por horario (--frame-dump-at): para telas cuja contagem de draws oscila (jogo
    // em andamento) e por isso nunca ficam "paradas".
    bool forced = false;
    for (s64& at_ms : g_forced_dumps_ms) {
        if (at_ms >= 0 && NowMs() >= at_ms && draws >= 1) {
            at_ms = -1;
            forced = true;
        }
    }
    if (!g_dump_dir.empty() && (forced || (g_stable_frames == kStableFrames && draws >= 1 && draws <= kMaxDumpDraws &&
                                           g_dumped_counts.size() < kMaxDumps && g_dumped_counts.insert(draws).second))) {
        if (forced) {
            log::Tagged("FRAME DUMP", "dump requested by time (--frame-dump-at), frame with %llu draws",
                        static_cast<unsigned long long>(draws));
        }
        DumpFrame(draws);
    }
    g_frame_buffers.clear();
    g_frame_draws = 0;
}

// Registradores de shader acompanhados na submissao, so para achar o fetch shader de cada
// vertex shader: 0x48/0x49 = SPI_SHADER_PGM_LO/HI_VS, 0x4C..0x5B = user data do VS.
std::array<u32, 0x20> g_vs_track{}; // registradores 0x40..0x5F
void CaptureFetchShader();

// ===================== draws do jogo -> Vulkan (Fase 17) =====================
// Estado acompanhado na submissao, alem do vertex shader: registradores de contexto (viewport,
// blend, mascara de cor), tipo de primitiva e de indice, base dos indices.
std::array<u32, 0x400> g_ctx_track{};
std::array<u32, 0x20> g_ps_track{}; // registradores 0x00..0x1F: 0x08/0x09 = programa do PS, 0x0C..0x1B = user data do PS
u32 g_prim_track = 4;      // VGT_PRIMITIVE_TYPE: 4 = lista de triangulos
u32 g_index_size_track = 2;
u32 g_num_instances = 1; // NUM_INSTANCES
u64 g_index_base_track = 0;
std::atomic<bool> g_vulkan_draws{true};
struct VulkanDrawStats {
    u64 textures_uploaded = 0, textures_failed = 0;
    u64 texture_pending = 0; // Fase 35: draws adiados porque uma textura ainda estava sendo lida
    u64 stencil_draws = 0, stencil_clears = 0; // Fase 34
    u64 flatbuf_built = 0;        // Fase 32: buffers achatados montados pelo walker
    u64 textures_rewritten = 0;   // Fase 29: texturas grandes reenviadas porque a amostra do conteudo mudou
    u64 textures_refreshed = 0;   // Fase 25: texturas pequenas reenviadas porque a CPU do jogo mudou o conteudo
    u64 mip_levels_uploaded = 0; // Fase 25: niveis de mip enviados, somando todas as texturas
    // Fase 20: todo draw com indices ou vertices e candidato; o que fica de fora, por motivo.
    u64 game_sent = 0, size = 0, primitive = 0, rect_list = 0, color_mode = 0, no_target = 0, target_format = 0, no_layout = 0,
        unreadable = 0, viewport = 0, game_no_pair = 0, no_hash = 0, game_format = 0, game_layout = 0, game_rejected = 0,
        game_zero_buffers = 0, game_white_images = 0, target_sampled = 0, depth_clears = 0, depth_only = 0, instanced = 0,
        instanced_step = 0, instanced_expanded = 0, // Fase 44: buffers por instancia expandidos (passo > 1)
        rebased = 0;                                 // Fase 77: draws com os buffers rebaseados no primeiro vertice
    // cache de geometria (Fase 21): bytes reaproveitados da GPU, enviados ao buffer estatico, copiados para o anel
    u64 cache_hit_bytes = 0, cache_upload_bytes = 0, ring_bytes = 0, cache_hits = 0, cache_uploads = 0, cache_changed = 0,
        cache_rechecks = 0, cache_resets = 0;
    std::map<u32, u64> vertex_formats; // formato de vertice nao tratado (dados << 4 | tipo numerico) -> draws
    std::map<u32, u64> target_formats; // formato de alvo nao tratado (formato << 4 | tipo numerico) -> draws
    std::map<u32, u64> primitives;     // primitiva nao tratada -> draws
} g_vk_stats;
std::atomic<bool> g_game_shaders{true};
std::atomic<bool> g_target_diagnostics{false}; // --target-log
// --srgb-display: buffer de video como alvo sRGB. Desligado por padrao: medido numa captura do
// shadPS4 (RenderDoc), os bytes que chegam a tela sao os mesmos que o passe final do jogo grava.
std::atomic<bool> g_srgb_display{false};
std::atomic<bool> g_readback_enabled{true}; // --no-readback
std::atomic<u64> g_readbacks_delivered{0};
std::atomic<bool> g_geometry_cache_enabled{true}; // --no-geometry-cache: toda a geometria copiada a cada draw

__declspec(noinline) void RawCopy(void* dst, const void* src, size_t bytes) {
    std::memcpy(dst, src, bytes);
}

bool SafeCopyInner(void* dst, u64 src, u64 bytes) {
    __try {
        RawCopy(dst, reinterpret_cast<const void*>(src), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copia memoria do jogo sem VirtualQuery; endereco invalido devolve false em vez de derrubar.
bool SafeCopy(void* dst, u64 src, u64 bytes) {
    if (src < 0x10000 || bytes > (64u << 20)) {
        return false;
    }
    const runtime::GuardedReadScope guard;
    return SafeCopyInner(dst, src, bytes);
}

struct GuestVsharp {
    u64 address = 0;
    u32 stride = 0, count = 0, format = 0;
};

bool ReadVsharp(const u32* w, GuestVsharp* out) {
    out->address = w[0] | (static_cast<u64>(w[1] & 0xFFFF) << 32);
    out->stride = (w[1] >> 16) & 0x3FFF;
    out->count = w[2];
    out->format = w[3];
    return out->address >= 0x10000 && out->stride >= 4 && out->stride <= 256 && out->count && out->count <= 0x400000 && (w[1] >> 30) == 0;
}

s32 FetchSgprOfVertexShader(u64 vs_address);
std::vector<FetchAttribute> ParseFetchShaderLayout(const u32* code, u32 max_dwords);
float HalfToFloat(u16 h);
u64 ContentHashOfShader(u32 stage_index, u64 address); // 0 se o endereco nao e de um shader extraido
void NoteUiShaderPair(u64 vs_address, u64 ps_address, u64 signature, s32 fetch_sgpr, const std::vector<u32>* fetch_code);

// ----------------------- texturas do jogo -> Vulkan (Fase 18) -----------------------
struct GuestTexture {
    u64 address = 0;
    u32 width = 0, height = 0, pitch = 0, format = 0, tiling = 0, levels = 0, swizzle = 0;
    u32 depth = 1; // > 1: textura 3D (tabela de correcao de cor)
    bool cube = false, srgb = false;
    bool pow2 = false; // pow2pad do T#: os mips sao guardados com dimensoes arredondadas para potencia de 2
    u32 number = 0; // tipo numerico do T# (0 = unorm, 7 = float, 9 = sRGB)
};

// Formatos de bloco do T#: 0x23 BC1, 0x24 BC2, 0x25 BC3, 0x26 BC4, 0x27 BC5, 0x29 BC7.
bool BlockFormat(u32 format, u32* block_bytes, render::TextureData::Format* out) {
    switch (format) {
    case 0x23: *block_bytes = 8; *out = render::TextureData::kBc1; return true;
    case 0x24: *block_bytes = 16; *out = render::TextureData::kBc2; return true;
    case 0x25: *block_bytes = 16; *out = render::TextureData::kBc3; return true;
    case 0x26: *block_bytes = 8; *out = render::TextureData::kBc4; return true;
    case 0x27: *block_bytes = 16; *out = render::TextureData::kBc5; return true;
    case 0x29: *block_bytes = 16; *out = render::TextureData::kBc7; return true;
    default: return false;
    }
}

// Descritor T# (8 dwords) de uma textura 2D que o backend sabe enviar: BC1 / BC3 em tiling 13
// (microtiles) ou RGBA8 em tiling 8 (linear).
bool ReadTsharp(const u32* w, GuestTexture* out) {
    const u32 type = w[3] >> 28; // 9 = 2D, 0xA = 3D, 0xB = cubo
    if (type != 0x9 && type != 0xA && type != 0xB) {
        return false;
    }
    GuestTexture t;
    t.depth = type == 0xA ? (w[4] & 0x1FFF) + 1 : 1;
    t.cube = type == 0xB;
    t.srgb = ((w[1] >> 26) & 0xF) == 9; // tipo numerico do T#: 9 = sRGB
    t.format = (w[1] >> 20) & 0x3F;
    t.width = (w[2] & 0x3FFF) + 1;
    t.height = ((w[2] >> 14) & 0x3FFF) + 1;
    t.pitch = ((w[4] >> 13) & 0x3FFF) + 1;
    t.levels = ((w[3] >> 16) & 0xF) + 1;
    t.tiling = (w[3] >> 20) & 0x1F;
    t.swizzle = w[3] & 0xFFF;
    t.pow2 = (w[3] >> 25) & 1;
    t.address = (static_cast<u64>(w[0]) << 8) | (static_cast<u64>(w[1] & 0x3F) << 40);
    u32 block_bytes = 0;
    render::TextureData::Format block_format;
    const bool bc = BlockFormat(t.format, &block_bytes, &block_format) && t.tiling == 13 && t.depth == 1;
    // Sem compressao: RGBA8 (0x0A), RGBA16 / RGBA16F (0x0C) e RGBA32F (0x0E) em tiling 8 (linear);
    // RGBA32F tambem em tiling 13 (microtiles de 8x8 pixels na mesma ordem de Morton dos blocos BC).
    t.number = (w[1] >> 26) & 0xF;
    // Fase 60: 8 bits / 1 canal (0x01) linear: os planos Y / U / V dos videos .fmv da colecao de personagens;
    // 8_8 (0x03) linear: o plano UV do video NV12 (episodio de Teen Titans Go!)
    const bool linear = (t.format == 0x0A || t.format == 0x0C || t.format == 0x0E || ((t.format == 0x01 || t.format == 0x03) && t.number == 0)) &&
                        t.tiling == 8;
    const bool float_tiled = t.format == 0x0E && t.tiling == 13 && t.depth == 1 && !t.cube;
    const bool rgba = (linear || float_tiled) && (t.format != 0x0C || t.number == 7 || t.number == 0) &&
                      (t.format != 0x0E || t.number == 7);
    if ((!bc && !rgba) || t.address < 0x10000 || t.pitch < t.width || t.width > 8192 || t.height > 8192 || t.depth > 256) { // 8192: o atlas de personagens tem 4192x4160
        return false;
    }
    *out = t;
    return true;
}

// Fase 59: destile de um nivel inteiro direto da memoria do jogo (antes: copia para um vetor e depois
// um memcpy por elemento). Num microtile de 8x8 elementos em ordem de Morton (x0 y0 x1 y1 x2 y2), as
// colunas 0-1, 2-3, 4-5 e 6-7 de uma linha sao vizinhas: cada linha do microtile sai em 4 copias de 2
// elementos. Microtiles cortados pela borda direita / de baixo vao elemento por elemento.
template <u32 E>
void DetileMicrotiles(const u8* source, u8* target, u32 ew, u32 eh, u32 tiles_per_row) {
    static constexpr u32 kPair[4] = {0, 4, 16, 20};
    const size_t row_bytes = static_cast<size_t>(ew) * E;
    for (u32 ty = 0; ty * 8 < eh; ++ty) {
        for (u32 tx = 0; tx * 8 < ew; ++tx) {
            const u8* tile = source + (static_cast<size_t>(ty) * tiles_per_row + tx) * 64 * E;
            const bool full = tx * 8 + 8 <= ew && ty * 8 + 8 <= eh;
            for (u32 iy = 0; iy < 8 && ty * 8 + iy < eh; ++iy) {
                const u32 base = ((iy & 1) << 1) | ((iy & 2) << 2) | ((iy & 4) << 3);
                u8* row = target + static_cast<size_t>(ty * 8 + iy) * row_bytes + static_cast<size_t>(tx) * 8 * E;
                if (full) {
                    for (u32 pair = 0; pair < 4; ++pair) {
                        std::memcpy(row + pair * 2 * E, tile + (base + kPair[pair]) * E, 2 * E);
                    }
                } else {
                    for (u32 ix = 0; ix < 8 && tx * 8 + ix < ew; ++ix) {
                        const u32 inside = base | (ix & 1) | ((ix & 2) << 1) | ((ix & 4) << 2);
                        std::memcpy(row + ix * E, tile + inside * E, E);
                    }
                }
            }
        }
    }
}

bool DetileLevelInner(u64 source_address, u64 slice_bytes, u32 slice_count, u8* out, u32 ew, u32 eh, u32 element, bool linear,
                      u32 pitch_aligned) {
    __try {
        for (u32 slice = 0; slice < slice_count; ++slice) {
            const u8* source = reinterpret_cast<const u8*>(source_address + slice_bytes * slice);
            u8* target = out + static_cast<size_t>(ew) * eh * element * slice;
            if (linear) {
                for (u32 y = 0; y < eh; ++y) {
                    std::memcpy(target + static_cast<size_t>(y) * ew * element, source + static_cast<size_t>(y) * pitch_aligned * element,
                                static_cast<size_t>(ew) * element);
                }
            } else if (element == 16) {
                DetileMicrotiles<16>(source, target, ew, eh, pitch_aligned / 8);
            } else if (element == 8) {
                DetileMicrotiles<8>(source, target, ew, eh, pitch_aligned / 8);
            } else {
                DetileMicrotiles<4>(source, target, ew, eh, pitch_aligned / 8);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DetileLevel(u64 source_address, u64 slice_bytes, u32 slice_count, u8* out, u32 ew, u32 eh, u32 element, bool linear, u32 pitch_aligned) {
    if (source_address < 0x10000) {
        return false;
    }
    const runtime::GuardedReadScope guard; // memoria do jogo desmapeada: false em vez de derrubar o processo
    return DetileLevelInner(source_address, slice_bytes, slice_count, out, ew, eh, element, linear, pitch_aligned);
}

// Le a textura na memoria do jogo, nivel de mip por nivel, e desfaz o tiling (mesma logica do
// tools/decode_frame_textures.py): tiling 13 em microtiles de 8x8 elementos em ordem de Morton
// (o elemento e um bloco BC ou, no RGBA32F, um pixel); tiling 8 e linear.
// Disposicao dos niveis (como no AddrLib, conferida com o shadPS4): um nivel depois do outro, e
// dentro de cada nivel todas as faces / fatias. As dimensoes do nivel n saem do PITCH >> n (em
// blocos, se BC; potencia de 2 acima, se pow2pad), alinhadas a 8 elementos. Um cubo com pow2pad
// guarda 8 fatias por nivel (6 faces + 2 de enchimento).
// Fase 59: vetor de pixels que nao zera o que cresce (o destile escreve tudo em seguida; zerar antes era
// uma passada a mais pela memoria em cada textura).
template <class T>
struct NoInitAllocator : std::allocator<T> {
    template <class U>
    struct rebind {
        using other = NoInitAllocator<U>;
    };
    NoInitAllocator() = default;
    template <class U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}
    template <class U>
    void construct(U* p) noexcept {
        ::new (static_cast<void*>(p)) U;
    }
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
};
using PixelVector = std::vector<u8, NoInitAllocator<u8>>;

// ---- Fase 62: icones dos botoes de outras plataformas ----
// O jogo so desenha a folha de icones do PS4 (608x608 DXT5, com cross / circle / square / triangle ...). O Preparar
// (tools/setup/botoes.py) monta, a partir do dump do jogador, folhas no MESMO layout com os desenhos do PS3, Xbox 360,
// Xbox One e Wii U (assets/buttons/<plataforma>.rgba) e o hash dos blocos da folha do PS4. Quando o jogo envia essa
// textura, ela vai para a GPU como a folha da plataforma escolhida; se o controle mudar, a textura e reescrita.
const char* const kIconPlatforms[] = {"ps4", "ps3", "360", "xboxone", "wiiu"};
constexpr int kIconPlatformCount = 5;
std::atomic<int> g_icons_forced{-1}; // --button-icons: -1 = pelo controle
struct IconSheets {
    std::once_flag once;
    bool ok = false;
    u64 hash = 0, bytes = 0;
    u32 width = 0, height = 0;
    std::vector<u8> rgba[kIconPlatformCount];
} g_icons;
std::atomic<u64> g_icon_address{0}; // textura do jogo reconhecida como a folha de icones
std::atomic<int> g_icon_applied{-1}; // plataforma que esta na GPU agora
u64 g_icon_next_check = 0;           // so a thread de render

int IconIndex(const char* name) {
    for (int i = 0; i < kIconPlatformCount; ++i) {
        if (!std::strcmp(name, kIconPlatforms[i])) {
            return i;
        }
    }
    return -1;
}

bool LoadIconSheets() {
    std::call_once(g_icons.once, [] {
        const std::filesystem::path dir = std::filesystem::path(render::GameSpirvDirectory()).parent_path() / "buttons";
        if (std::FILE* f = _wfopen((dir / "ps4_dxt5.hash").c_str(), L"r")) {
            unsigned long long hash = 0, bytes = 0;
            unsigned width = 0, height = 0;
            g_icons.ok = std::fscanf(f, "%llx %u %u %llu", &hash, &width, &height, &bytes) == 4;
            std::fclose(f);
            g_icons.hash = hash;
            g_icons.bytes = bytes;
            g_icons.width = width;
            g_icons.height = height;
        }
        for (int i = 0; g_icons.ok && i < kIconPlatformCount; ++i) {
            std::ifstream in(dir / (std::string(kIconPlatforms[i]) + ".rgba"), std::ios::binary);
            g_icons.rgba[i].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (g_icons.rgba[i].size() != static_cast<size_t>(g_icons.width) * g_icons.height * 4) {
                g_icons.rgba[i].clear();
            }
        }
        g_icons.ok = g_icons.ok && !g_icons.rgba[0].empty();
        log::Tagged("ICONS", g_icons.ok ? "icon sheets loaded (%s)" : "no icon sheets at %s (run Prepare)",
                    dir.string().c_str());
    });
    return g_icons.ok;
}

// Plataforma pedida agora: a opcao do launcher, ou a do controle do jogador 1 (so XInput: Xbox; sem controle: PS4).
int WantedIconPlatform() {
    const int forced = g_icons_forced.load();
    if (forced >= 0) {
        return forced;
    }
    const int from_pad = IconIndex(padsdl::IconPlatform(0));
    if (from_pad >= 0) {
        return from_pad;
    }
    return hle::PadControllerPresent(0) ? IconIndex("xboxone") : 0;
}

// Fim da leitura de uma textura: a folha do PS4 vira a da plataforma pedida (RGBA8, mesmo tamanho).
void ReplaceIconSheet(const GuestTexture& t, PixelVector* out, render::TextureData* data) {
    if (data->format != render::TextureData::kBc3 || t.depth != 1 || t.cube || !LoadIconSheets() || t.width != g_icons.width ||
        t.height != g_icons.height || data->levels < 1 || out->size() < g_icons.bytes) {
        return;
    }
    u64 hash = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < g_icons.bytes; ++i) {
        hash = (hash ^ (*out)[i]) * 0x100000001b3ull;
    }
    if (hash != g_icons.hash) {
        return;
    }
    int wanted = WantedIconPlatform();
    if (g_icons.rgba[wanted].empty()) {
        wanted = 0;
    }
    const std::vector<u8>& rgba = g_icons.rgba[wanted];
    out->assign(rgba.begin(), rgba.end());
    data->format = render::TextureData::kRgba8;
    data->levels = 1;
    data->level_offsets[0] = 0;
    g_icon_address = t.address;
    g_icon_applied = wanted;
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 4) {
        log::Tagged("ICONS", "game icon sheet at 0x%llx: submitted with \"%s\" icons", static_cast<unsigned long long>(t.address),
                    kIconPlatforms[wanted]);
    }
}

// Bytes por elemento das texturas lineares sem compressao.
u32 LinearElementBytes(u32 format) {
    return format == 0x01 ? 1 : format == 0x03 ? 2 : format == 0x0A ? 4 : format == 0x0C ? 8 : 16;
}

bool ReadGuestTexture(const GuestTexture& t, PixelVector* out, render::TextureData* data) {
    const render::HitchClock hitch(render::HitchClock::kDetile);
    const bool linear = t.tiling == 8;
    u32 element = 16; // bytes por elemento
    bool block = false;
    if (linear) {
        element = LinearElementBytes(t.format);
        data->format = t.format == 0x0A || t.format == 0x01 || t.format == 0x03 ? render::TextureData::kRgba8
                       : t.format == 0x0E ? render::TextureData::kRgba32F
                       : t.number == 7    ? render::TextureData::kRgba16F
                                          : render::TextureData::kRgba16;
    } else if (t.format == 0x0E) {
        data->format = render::TextureData::kRgba32F;
    } else {
        BlockFormat(t.format, &element, &data->format);
        block = true;
    }
    auto pow2_above = [](u32 value) {
        u32 result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    };
    const u32 layers = t.cube ? 6 : 1;
    const u32 stored_layers = t.cube ? (t.pow2 ? 8 : 6) : 1;
    const u32 levels = t.depth > 1 ? 1u : std::clamp(t.levels, 1u, 14u);
    const u32 base_pitch = std::max(t.pitch, t.width);
    out->clear();
    u64 guest_offset = 0;
    u32 done = 0;
    for (u32 level = 0; level < levels; ++level) {
        const u32 width = std::max(1u, t.width >> level), height = std::max(1u, t.height >> level);
        const u32 ew = block ? (width + 3) / 4 : width, eh = block ? (height + 3) / 4 : height; // elementos de verdade
        u32 pw = base_pitch >> level, ph = t.height >> level;                                   // elementos guardados
        if (block) {
            pw = (pw + 3) / 4;
            ph = (ph + 3) / 4;
        }
        pw = std::max(pw, 1u);
        ph = std::max(ph, 1u);
        if (t.pow2 && level > 0) {
            pw = pow2_above(pw);
            ph = pow2_above(ph);
        }
        pw = std::max(pw, ew);
        ph = std::max(ph, eh);
        const u32 slices = level == 0 ? t.depth : 1;
        u32 pitch_aligned = 0;
        u64 slice_bytes = 0;
        if (linear) {
            const u32 pitch_align = std::max(8u, 64u / element), slice_align = std::max(64u, 256u / element);
            pitch_aligned = (pw + pitch_align - 1) & ~(pitch_align - 1);
            while ((static_cast<u64>(pitch_aligned) * ph) % slice_align) {
                pitch_aligned += pitch_align;
            }
            if (level == 0) {
                pitch_aligned = base_pitch; // o nivel 0 traz o pitch no proprio descritor
            }
            slice_bytes = static_cast<u64>(pitch_aligned) * ph * element;
        } else {
            pitch_aligned = (pw + 7) & ~7u;
            const u32 height_aligned = (ph + 7) & ~7u;
            slice_bytes = static_cast<u64>(pitch_aligned) * height_aligned * element;
            while (slice_bytes % 256) {
                pitch_aligned += 8;
                slice_bytes = static_cast<u64>(pitch_aligned) * height_aligned * element;
            }
        }
        const u64 level_out = static_cast<u64>(ew) * eh * element * layers * slices;
        if (out->size() + level_out > (36ull << 20)) {
            break; // nao cabe no staging: fica com os niveis ja lidos
        }
        const size_t level_start = out->size();
        out->resize(level_start + level_out);
        data->level_offsets[level] = level_start;
        if (!DetileLevel(t.address + guest_offset, slice_bytes, layers * slices, out->data() + level_start, ew, eh, element, linear,
                         pitch_aligned)) {
            out->resize(level_start);
            break;
        }
        guest_offset += slice_bytes * stored_layers * slices;
        ++done;
    }
    if (!done) {
        return false;
    }
    if (t.format == 0x01 || t.format == 0x03) { // Fase 60: R8 / R8G8 -> RGBA8 (r, g ou 0, 0, 1), o que o hardware devolve
        const size_t channels = t.format == 0x01 ? 1 : 2;
        static thread_local PixelVector expanded;
        const size_t texels = out->size() / channels;
        expanded.resize(texels * 4);
        const u8* source = out->data();
        u8* target = expanded.data();
        for (size_t i = 0; i < texels; ++i) {
            target[4 * i] = source[channels * i];
            target[4 * i + 1] = channels == 2 ? source[channels * i + 1] : 0;
            target[4 * i + 2] = 0;
            target[4 * i + 3] = 255;
        }
        out->swap(expanded);
        for (u32 level = 0; level < done; ++level) {
            data->level_offsets[level] = data->level_offsets[level] / channels * 4;
        }
    }
    data->levels = done;
    data->width = t.width;
    data->height = t.height;
    data->depth = t.depth;
    data->cube = t.cube;
    data->srgb = t.srgb;
    ReplaceIconSheet(t, out, data); // Fase 62
    data->data = out->data();
    data->bytes = out->size();
    data->swizzle = t.swizzle;
    g_vk_stats.mip_levels_uploaded += done;
    return true;
}

u64 g_texture_frame = 0; // avanca a cada flip (junto com g_cache_frame)

// DIAGNOSTICO Fase 60 (LD_DBG_CB=1): num frame a cada 600, guarda cada buffer de constantes copiado (endereco,
// tamanho, conteudo) e, no fim do Submit do flip, compara com a memoria do jogo: o que mudou depois da copia sai
// em [DBG-CB], por (VS, PS, estagio, binding).
struct DbgCbEntry {
    u64 address, vs, ps;
    u32 bytes, stage, binding, source;
    std::vector<u8> data;
};
bool g_dbg_cb_record = false;
std::mutex g_dbg_cb_mutex;
std::vector<DbgCbEntry> g_dbg_cb;

void DbgCbRecord(u64 address, u32 bytes, const u8* data, u64 vs, u64 ps, u32 stage, u32 binding, u32 source) {
    std::lock_guard lock(g_dbg_cb_mutex);
    if (g_dbg_cb.size() < 40000) {
        g_dbg_cb.push_back({address, vs, ps, bytes, stage, binding, source, std::vector<u8>(data, data + bytes)});
    }
}

// Garante a textura no backend e devolve a chave dela (0 = nao foi possivel).
// Fase 29: amostra do conteudo de uma textura na memoria do jogo - 64 bytes a cada 1 KiB. Serve
// para notar que a CPU do jogo reescreveu a textura depois do primeiro envio (atlas de personagens
// montado em tempo de execucao, texturas que chegam do disco depois do primeiro draw).
__declspec(noinline) u64 TextureSampleRaw(const u8* p, u64 bytes) {
    u64 hash = 0x9E3779B97F4A7C15ull ^ bytes;
    for (u64 offset = 0; offset + 64 <= bytes; offset += 1024) {
        for (u32 i = 0; i < 64; i += 8) {
            u64 word;
            std::memcpy(&word, p + offset + i, 8);
            hash = (hash ^ word) * 0x100000001B3ull;
            hash ^= hash >> 31;
        }
    }
    return hash;
}

bool TextureSampleInner(u64 address, u64 bytes, u64* out) {
    __try {
        *out = TextureSampleRaw(reinterpret_cast<const u8*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TextureSample(u64 address, u64 bytes, u64* out) {
    const runtime::GuardedReadScope guard;
    return TextureSampleInner(address, bytes, out);
}

std::atomic<bool> g_texture_refresh{true}; // --no-texture-refresh

// ---- Fase 35: leitura / destile de texturas fora da thread de render ----
// O primeiro uso de uma textura custava a leitura da memoria do jogo + o destile (1 a 3 ms cada);
// uma troca de cena traz dezenas no mesmo frame. Agora a thread de render so pede; a thread de
// texturas le e destila; o envio para a GPU (curto) e feito no comeco dos frames seguintes, dentro
// de um orcamento de tempo. Enquanto a textura nao chega, os draws que a usam ficam de fora.
constexpr u64 kTexturePending = ~0ull;
struct TexTracked {
    u64 sample = 0, next_frame = 0;
    u32 rewrites = 0; // conferencias seguidas em que o conteudo tinha mudado
};
std::unordered_map<u64, TexTracked> g_tex_tracked;
// Issue #6: texturas lineares grandes que o jogo reescreve sem parar (a tela dos fliperamas da Midway: Spy Hunter e
// Rampage em 512x480, Super Sprint em 512x384) passam a ser conferidas todo quadro, como as lineares pequenas. Pela
// amostragem a cada ~30 quadros a tela mostrava buffers antigos misturados (quadro dobrado / borrado).
std::unordered_set<u64> g_tex_hot_linear;
constexpr u32 kHotLinearRewrites = 2;
constexpr u64 kHotLinearMaxTexels = 1u << 20;

// Chamado a cada conferencia por amostragem de uma textura ja enviada.
void NoteTextureCheck(u64 key, const GuestTexture& texture, TexTracked& state, bool changed) {
    state.rewrites = changed ? state.rewrites + 1 : 0;
    if (state.rewrites < kHotLinearRewrites || texture.tiling != 8 ||
        static_cast<u64>(texture.width) * texture.height * texture.depth > kHotLinearMaxTexels || !g_tex_hot_linear.insert(key).second) {
        return;
    }
    std::printf("[TEXTURE] 0x%llx %ux%u format 0x%x: linear texture rewritten all the time; checked every frame from now on\n",
                static_cast<unsigned long long>(texture.address), texture.width, texture.height, texture.format);
}
// Fase 61 (medicao): texturas lidas na hora pela thread de render neste frame (lineares pequenas novas, conferencia
// por frame das lineares pequenas, reescritas no modo sem threads). Vai na linha [ENGASGO] e zera a cada frame.
u32 g_tex_inline_count = 0, g_tex_inline_checks = 0;
double g_tex_inline_ms = 0, g_tex_inline_check_ms = 0;
struct InlineTexClock {
    double* total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~InlineTexClock() { *total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};
// Fase 59: textura nova vai SEMPRE para o pool (nada de destile em serie na thread de render, que
// prendia o frame em ate 33 ms antes de o pool comecar) e a espera no fim do frame vai ate 200 ms, com
// a thread de render destilando junto. --no-parallel-textures volta ao esquema da Fase 39/45.
bool g_tex_parallel = true;

struct TextureJob {
    u64 key = 0, level0_bytes = 0, sample = 0;
    GuestTexture texture;
    PixelVector pixels;
    render::TextureData data;
    bool ok = false;
    u64 prepared = 0; // Fase 45: imagem + memoria + views ja criadas nesta thread
    bool worker_upload = false; // Fase 59: a propria thread enviou (ok = deu certo)
    bool replace = false;       // Fase 61: textura que ja existe e o jogo reescreveu (so le e destila; a de render reenvia)
};
std::mutex g_tex_mutex;
std::condition_variable g_tex_cv, g_tex_done_cv;
std::deque<TextureJob> g_tex_jobs;
std::deque<TextureJob> g_tex_done;
std::unordered_set<u64> g_tex_requested, g_tex_failed; // so a thread de render mexe
// Fase 61: texturas reescritas pelo jogo sendo lidas de novo pelas threads (a espera do fim do frame espera por elas)
std::unordered_set<u64> g_tex_replacing;
u64 g_tex_replaced = 0;
std::atomic<bool> g_async_textures{true};               // --no-async-textures
std::atomic<u64> g_tex_async_uploaded{0};

// Le, destila e cria a imagem de uma textura pedida; o resultado vai para g_tex_done.
void RunTextureJob(TextureJob& job) {
    // Fase 59: os pixels ficam num buffer da thread (sem alocar e soltar megabytes por textura) e a
    // propria thread envia a textura; so sem os objetos preparados o envio volta para a thread de render.
    static thread_local PixelVector pixels;
    // Fase 61: a amostra vem ANTES da leitura. Depois dela, uma escrita do jogo no meio da leitura (o jogo
    // ainda carregando a cena) deixava a amostra com o conteudo novo e a GPU com o velho: a conferencia a
    // cada 30 quadros nunca via diferenca (cabecas transparentes ao carregar um save direto numa cutscene).
    TextureSample(job.texture.address, job.level0_bytes, &job.sample);
    job.ok = ReadGuestTexture(job.texture, &pixels, &job.data);
    if (job.ok && job.replace) { // Fase 61: a imagem ja existe; a thread de render copia os pixels para ela
        job.pixels.assign(pixels.begin(), pixels.end());
        job.data.data = job.pixels.data();
    } else if (job.ok) {
        auto& renderer = render::VulkanRenderer::Instance();
        const u64 prepared = renderer.PrepareTexture(job.data);
        if (prepared && g_tex_parallel) {
            job.worker_upload = true;
            job.ok = renderer.UploadTextureFromWorker(job.key, job.data, prepared);
        } else {
            job.pixels.assign(pixels.begin(), pixels.end());
            job.data.data = job.pixels.data();
            job.prepared = prepared;
        }
    }
    {
        std::lock_guard lock(g_tex_mutex);
        g_tex_done.push_back(std::move(job));
    }
    g_tex_done_cv.notify_all();
}

void TextureWorker() {
    // Fase 59: abaixo do normal - numa rajada as threads de textura nao tiram a CPU da thread de render
    // nem das threads do jogo (que tambem carregam a area nova); a de render ajuda na espera do frame.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        TextureJob job;
        {
            std::unique_lock lock(g_tex_mutex);
            g_tex_cv.wait(lock, [] { return !g_tex_jobs.empty(); });
            job = std::move(g_tex_jobs.front());
            g_tex_jobs.pop_front();
        }
        RunTextureJob(job);
    }
}

// Fase 59: threads de textura (ver a medicao abaixo). Na entrada de uma area chegam 50 a 130 texturas novas no mesmo frame.
int TextureThreadCount() {
    static const int count = [] {
        if (const char* env = std::getenv("LD_TEX_THREADS")) { // so para medir
            return std::clamp(std::atoi(env), 1, 16);
        }
        // Medido na entrada de area (Fase 59): 8 threads = cada destile 3-5x mais lento (disputam a CPU com
        // as threads do jogo, que carregam a area ao mesmo tempo) e o Submit 15-30 ms mais lento; 2 threads
        // = melhor. Fica nucleos / 4, entre 2 e 4 (a thread de render ajuda na espera do frame).
        const int cores = static_cast<int>(std::thread::hardware_concurrency());
        return std::clamp(cores / 4, 2, 4);
    }();
    return count;
}

// Fase 45: o draw cuja textura ainda esta sendo lida NAO sai mais do frame: vai para a fila com a
// textura pendente, e antes de gravar o frame a thread de render espera as texturas pendentes (ate
// um teto) enquanto as threads leem e destilam em paralelo. Antes o draw sumia por alguns frames
// (pop-in: 1157 draws em 2 minutos de Vorton).
bool g_tex_wait_frame = true;
double g_tex_wait_cap_ms = 50.0;
u64 g_tex_pending_key = 0;                   // chave da ultima textura que EnsureGuestTexture deixou pendente
std::unordered_set<u64> g_tex_frame_waiting; // texturas pendentes de draws deste frame (thread de render)
u64 g_tex_wait_frames = 0, g_tex_wait_textures = 0;
double g_tex_wait_ms = 0, g_tex_wait_worst_ms = 0;

void RequestTexture(u64 key, const GuestTexture& texture, u64 level0_bytes) {
    static std::once_flag once;
    std::call_once(once, [] { // Fase 45: 4 threads; Fase 59: nucleos - 4 (TextureThreadCount)
        for (int i = 0; i < TextureThreadCount(); ++i) {
            std::thread(TextureWorker).detach();
        }
    });
    if (!g_tex_requested.insert(key).second) {
        return;
    }
    TextureJob job;
    job.key = key;
    job.level0_bytes = level0_bytes;
    job.texture = texture;
    {
        std::lock_guard lock(g_tex_mutex);
        g_tex_jobs.push_back(std::move(job));
    }
    g_tex_cv.notify_one();
}

// Fase 39: textura nova lida da memoria do jogo e destilada NA HORA, na thread de render, ate gastar
// o orcamento do frame; o envio para a GPU vai no command buffer do proprio frame (sem espera). O que
// passar do orcamento vai para a thread de texturas (Fase 35) e o draw espera alguns frames.
bool g_sync_textures = true;
double g_tex_budget_ms = 16.0;
double g_tex_frame_ms = 0; // so a thread de render
// Fase 40: pico de texturas novas (troca de cena). Passou de `g_tex_spike_count` texturas novas no
// mesmo frame, o orcamento do frame sobe para `g_tex_spike_budget_ms`: o frame demora mais, mas a cena
// nova aparece inteira em vez de faltar pedacos por um frame.
u32 g_tex_spike_count = 10;
double g_tex_spike_budget_ms = 33.0;
u32 g_tex_frame_new = 0; // texturas novas pedidas neste frame (so a thread de render)
bool g_tex_frame_spike = false;
u64 g_tex_spike_frames = 0;
u64 g_tex_sync_count = 0, g_tex_sync_deferred = 0, g_tex_sync_frames = 0;
double g_tex_sync_ms = 0, g_tex_sync_worst_ms = 0, g_tex_sync_worst_frame_ms = 0;

u64 EnsureGuestTexture(const GuestTexture& texture) {
    auto& renderer = render::VulkanRenderer::Instance();
    const u64 key = texture.address ^ (static_cast<u64>(texture.width) << 48) ^ (static_cast<u64>(texture.height) << 36) ^
                    (static_cast<u64>(texture.format) << 58) ^ (static_cast<u64>(texture.depth) << 24) ^
                    (texture.cube ? 0x40000ull : 0) ^ (texture.srgb ? 0x80000ull : 0) ^ 1; // nunca 0
    // Fase 25: texturas lineares pequenas (tiling 8: exposicao 4x4, tabela de materiais 256x1, LUT
    // 3D de cor, rampas) sao reescritas pela CPU do jogo. O conteudo e conferido uma vez por frame
    // e reenviado quando muda; antes ficava para sempre o que havia no primeiro uso.
    struct Dynamic {
        u64 hash = 0, frame = ~0ull;
    };
    static std::unordered_map<u64, Dynamic> dynamic;
    auto hash_of = [](const PixelVector& bytes) {
        u64 hash = 0xcbf29ce484222325ull;
        size_t i = 0;
        for (; i + 8 <= bytes.size(); i += 8) {
            u64 word;
            std::memcpy(&word, bytes.data() + i, 8);
            hash = (hash ^ word) * 0x100000001b3ull;
        }
        for (; i < bytes.size(); ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
        return hash;
    };
    if (texture.address == g_icon_address.load(std::memory_order_relaxed) && g_texture_frame >= g_icon_next_check) {
        // Fase 62: o controle (ou a opcao) mudou: a folha de icones que esta na GPU e reescrita com a outra plataforma
        g_icon_next_check = g_texture_frame + 30;
        const int wanted = WantedIconPlatform();
        if (wanted != g_icon_applied.load() && !g_icons.rgba[wanted].empty() && renderer.HasTexture(key)) {
            render::TextureData data;
            data.format = render::TextureData::kRgba8;
            data.width = texture.width;
            data.height = texture.height;
            data.srgb = texture.srgb;
            data.swizzle = texture.swizzle;
            data.data = g_icons.rgba[wanted].data();
            data.bytes = g_icons.rgba[wanted].size();
            if (renderer.UpdateTexture(key, data)) {
                g_icon_applied = wanted;
                log::Tagged("ICONS", "controller changed: \"%s\" icons", kIconPlatforms[wanted]);
            }
        }
    }
    const bool small_linear = texture.tiling == 8 && (static_cast<u64>(texture.width) * texture.height * texture.depth <= (128u << 10) ||
                                                      texture.format == 0x01 || texture.format == 0x03 || // Fase 60: planos de video mudam todo quadro
                                                      (g_tex_hot_linear.count(key) && !g_tex_replacing.count(key))); // Issue #6
    static thread_local PixelVector pixels;
    // Fase 29: as demais texturas sao conferidas por amostragem a cada ~30 frames (nivel 0).
    using Tracked = TexTracked;
    auto& tracked = g_tex_tracked;
    u64 level0_bytes = 0;
    {
        u32 block_bytes = 0;
        render::TextureData::Format unused;
        if (texture.tiling == 13 && BlockFormat(texture.format, &block_bytes, &unused)) {
            level0_bytes = static_cast<u64>((texture.pitch + 3) / 4) * ((texture.height + 3) / 4) * block_bytes;
        } else {
            const u32 element = LinearElementBytes(texture.format);
            level0_bytes = static_cast<u64>(texture.pitch) * texture.height * texture.depth * element;
        }
    }
    if (!renderer.HasTexture(key) && g_async_textures.load(std::memory_order_relaxed) && !small_linear) {
        if (g_tex_failed.count(key)) {
            return 0; // a leitura falhou: fica o branco, como antes
        }
        if (!g_tex_requested.count(key) && ++g_tex_frame_new > g_tex_spike_count && !g_tex_frame_spike) {
            g_tex_frame_spike = true;
            ++g_tex_spike_frames;
        }
        const double budget = g_tex_frame_spike ? std::max(g_tex_budget_ms, g_tex_spike_budget_ms) : g_tex_budget_ms;
        const bool to_pool = g_tex_parallel && g_tex_wait_frame; // Fase 59: o draw espera no fim do frame
        if (g_sync_textures && !to_pool && g_tex_frame_ms < budget && !g_tex_requested.count(key)) {
            const auto start = std::chrono::steady_clock::now();
            render::TextureData data;
            u64 sample = 0;
            TextureSample(texture.address, level0_bytes, &sample); // Fase 61: antes da leitura (ver RunTextureJob)
            const bool ok = ReadGuestTexture(texture, &pixels, &data) && renderer.UploadTexture(key, data);
            if (ok) {
                Tracked& state = tracked[key];
                state.sample = sample;
                state.next_frame = g_texture_frame + 20 + (key >> 12) % 20;
                ++g_vk_stats.textures_uploaded;
            } else {
                ++g_vk_stats.textures_failed;
                g_tex_failed.insert(key);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (g_tex_frame_ms == 0) {
                ++g_tex_sync_frames;
            }
            g_tex_frame_ms += ms;
            g_tex_sync_worst_frame_ms = std::max(g_tex_sync_worst_frame_ms, g_tex_frame_ms);
            ++g_tex_sync_count;
            g_tex_sync_ms += ms;
            g_tex_sync_worst_ms = std::max(g_tex_sync_worst_ms, ms);
            return ok ? key : 0;
        }
        g_tex_sync_deferred += g_sync_textures && !g_tex_requested.count(key); // conta texturas, nao draws
        RequestTexture(key, texture, level0_bytes);
        g_tex_pending_key = key;
        return kTexturePending;
    }
    if (!renderer.HasTexture(key)) {
        const InlineTexClock clock{&g_tex_inline_ms}; // Fase 61 (medicao)
        ++g_tex_inline_count;
        render::TextureData data;
        u64 sample = 0;
        TextureSample(texture.address, level0_bytes, &sample); // Fase 61: antes da leitura (ver RunTextureJob)
        const auto t0 = std::chrono::steady_clock::now(); // Fase 61 (medicao)
        const bool read = ReadGuestTexture(texture, &pixels, &data);
        const auto t1 = std::chrono::steady_clock::now();
        const bool sent = read && renderer.UploadTexture(key, data);
        const auto t2 = std::chrono::steady_clock::now();
        const double read_ms = std::chrono::duration<double, std::milli>(t1 - t0).count(),
                     send_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (read_ms + send_ms > 3.0) {
            std::printf("[TEXTURE-ON-DEMAND] 0x%llx %ux%ux%u format 0x%x tiling %u (%s): read %.1f ms, upload %.1f ms, %llu bytes\n",
                        static_cast<unsigned long long>(texture.address), texture.width, texture.height, texture.depth, texture.format,
                        texture.tiling, small_linear ? "small linear" : "no threads", read_ms, send_ms,
                        static_cast<unsigned long long>(data.bytes));
        }
        if (sent) {
            ++g_vk_stats.textures_uploaded;
            Tracked& state = tracked[key];
            state.sample = sample;
            state.next_frame = g_texture_frame + 20 + (key >> 12) % 20;
            if (small_linear) {
                dynamic[key] = {hash_of(pixels), g_texture_frame};
            }
        } else {
            ++g_vk_stats.textures_failed;
        }
    } else if (small_linear) {
        Dynamic& state = dynamic[key];
        if (state.frame != g_texture_frame) {
            const InlineTexClock clock{&g_tex_inline_check_ms}; // Fase 61 (medicao)
            ++g_tex_inline_checks;
            state.frame = g_texture_frame;
            render::TextureData data;
            if (ReadGuestTexture(texture, &pixels, &data)) {
                const u64 hash = hash_of(pixels);
                if (hash != state.hash && renderer.UpdateTexture(key, data)) {
                    state.hash = hash;
                    ++g_vk_stats.textures_refreshed;
                }
            }
        }
    }
    else if (g_tex_replacing.count(key)) { // Fase 61: a versao nova ainda esta sendo lida: este draw tambem espera por ela
        g_tex_pending_key = key;
        return kTexturePending;
    } else if (g_texture_refresh.load(std::memory_order_relaxed)) {
        Tracked& state = tracked[key];
        if (g_texture_frame >= state.next_frame && g_tex_parallel && g_tex_wait_frame && g_async_textures.load(std::memory_order_relaxed)) {
            // Fase 61: reescrita detectada vai para as threads de textura (em paralelo, a de render ajudando na espera do
            // fim do frame) em vez de ser relida e destilada aqui, uma por vez: na troca do menu para a tela de carga o
            // jogo reescreve ~25 texturas (duas de 4096x4096) no mesmo frame e eram 30-45 ms so nisso. O draw espera a
            // versao nova no fim do frame, como uma textura nova: nunca sai com a velha (sem pop-in).
            state.next_frame = g_texture_frame + 30;
            u64 sample = 0;
            const bool sampled = TextureSample(texture.address, level0_bytes, &sample);
            NoteTextureCheck(key, texture, state, sampled && sample != state.sample);
            if (sampled && sample != state.sample) {
                state.sample = sample;
                TextureJob job;
                job.key = key;
                job.level0_bytes = level0_bytes;
                job.texture = texture;
                job.replace = true;
                g_tex_replacing.insert(key);
                {
                    std::lock_guard lock(g_tex_mutex);
                    g_tex_jobs.push_back(std::move(job));
                }
                g_tex_cv.notify_one();
                static u32 logged = 0;
                if (logged < 30) {
                    ++logged;
                    std::printf("[TEXTURE] 0x%llx %ux%u format 0x%x: game rewrote content after first upload; reread by threads\n",
                                static_cast<unsigned long long>(texture.address), texture.width, texture.height, texture.format);
                }
                g_tex_pending_key = key;
                return kTexturePending;
            }
        } else if (g_texture_frame >= state.next_frame) {
            state.next_frame = g_texture_frame + 30;
            u64 sample = 0;
            const bool sampled = TextureSample(texture.address, level0_bytes, &sample);
            NoteTextureCheck(key, texture, state, sampled && sample != state.sample);
            if (sampled && sample != state.sample) {
                state.sample = sample;
                render::TextureData data;
                if (ReadGuestTexture(texture, &pixels, &data) && renderer.UpdateTexture(key, data)) {
                    ++g_vk_stats.textures_rewritten;
                    static u32 logged = 0;
                    if (logged < 30) {
                        ++logged;
                        std::printf("[TEXTURE] 0x%llx %ux%u format 0x%x: game rewrote content after first upload; uploaded again\n",
                                    static_cast<unsigned long long>(texture.address), texture.width, texture.height, texture.format);
                    }
                }
            }
        }
    }
    return renderer.HasTexture(key) ? key : 0;
}

void DrainTextureResults(double budget_ms);

// Fase 45: antes de gravar o frame, espera (ate o teto) as texturas pendentes dos draws dele e
// envia as que chegarem. Devolve o tempo esperado.
double WaitFrameTextures() {
    if (g_tex_frame_waiting.empty()) {
        return 0;
    }
    auto& renderer = render::VulkanRenderer::Instance();
    const auto start = std::chrono::steady_clock::now();
    const double cap_ms = g_tex_parallel ? std::max(g_tex_wait_cap_ms, 200.0) : g_tex_wait_cap_ms;
    const auto deadline = start + std::chrono::microseconds(static_cast<long long>(cap_ms * 1000.0));
    g_tex_wait_textures += g_tex_frame_waiting.size();
    for (;;) {
        DrainTextureResults(1e9);
        for (auto it = g_tex_frame_waiting.begin(); it != g_tex_frame_waiting.end();) {
            it = (renderer.HasTexture(*it) && !g_tex_replacing.count(*it)) || g_tex_failed.count(*it) ? g_tex_frame_waiting.erase(it)
                                                                                                     : std::next(it);
        }
        if (g_tex_frame_waiting.empty() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (g_tex_parallel) { // Fase 59: em vez de so esperar, a thread de render destila tambem
            TextureJob job;
            bool got = false;
            {
                std::lock_guard lock(g_tex_mutex);
                if (!g_tex_jobs.empty()) {
                    job = std::move(g_tex_jobs.front());
                    g_tex_jobs.pop_front();
                    got = true;
                }
            }
            if (got) {
                RunTextureJob(job);
                continue;
            }
        }
        std::unique_lock lock(g_tex_mutex);
        g_tex_done_cv.wait_until(lock, deadline, [] { return !g_tex_done.empty(); });
    }
    g_tex_frame_waiting.clear();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ++g_tex_wait_frames;
    g_tex_wait_ms += ms;
    g_tex_wait_worst_ms = std::max(g_tex_wait_worst_ms, ms);
    return ms;
}

// Thread de render, no comeco do Submit: envia para a GPU as texturas que a thread de texturas
// terminou, ate gastar o orcamento de tempo deste frame.
void DrainTextureResults(double budget_ms) {
    const auto start = std::chrono::steady_clock::now();
    auto& renderer = render::VulkanRenderer::Instance();
    for (;;) {
        TextureJob job;
        {
            std::lock_guard lock(g_tex_mutex);
            if (g_tex_done.empty()) {
                return;
            }
            job = std::move(g_tex_done.front());
            g_tex_done.pop_front();
        }
        job.data.data = job.pixels.data(); // o vetor mudou de dono: o ponteiro tambem
        if (job.replace) { // Fase 61
            g_tex_replacing.erase(job.key);
            if (job.ok && renderer.UpdateTexture(job.key, job.data)) {
                ++g_vk_stats.textures_rewritten;
                ++g_tex_replaced;
            }
            g_tex_tracked[job.key] = {job.sample, g_texture_frame + 30};
            continue;
        }
        if (job.worker_upload ? job.ok : job.ok && renderer.UploadTexture(job.key, job.data, job.prepared)) {
            ++g_vk_stats.textures_uploaded;
            ++g_tex_async_uploaded;
            g_tex_tracked[job.key] = {job.sample, g_texture_frame + 20 + (job.key >> 12) % 20};
        } else {
            ++g_vk_stats.textures_failed;
            g_tex_failed.insert(job.key);
        }
        g_tex_requested.erase(job.key);
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budget_ms) {
            return;
        }
    }
}

// ===================== shaders do jogo no pipeline (Fase 19) =====================
// Um par (VS, PS) recompilado pela ferramenta (assets/shaders_spv/pairs): os dois .spv e o .txt
// com o binding de cada recurso e de onde vem o descritor ("ud:R" = embutido a partir do
// registrador R do user data do estagio; "tab:R:D" = dword D da tabela apontada por R, R+1).
struct GamePairInfo {
    bool usable = false;
    u64 key = 0;
    std::string why; // motivo de nao ser usavel
    struct Resource {
        u32 stage; // 0 = VS, 1 = PS
        render::GamePairLayout::Kind kind;
        u32 binding;
        u32 source; // 0 = nenhuma (zeros), 1 = ud, 2 = tabela, 3 = flatbuf (montado pelo walker do estagio)
        u32 reg, dword;
        u32 view_type = 0; // imagens: 0 = 2D, 1 = cubo, 2 = array 2D, 3 = 3D (o que o shader declara)
    };
    // Registradores de user data que o shader le direto: vao para push_data.ud_regs, a partir de
    // first_ud[estagio], um por bit ligado de ud_mask[estagio] (como o shadPS4 faz).
    u32 first_ud[2] = {}, ud_mask[2] = {};
    std::vector<Resource> resources;
    struct Attribute {
        u32 location, table_sgpr, dword;
        u32 format = ~0u; // dst_sel + tipo numerico + formato de dados com que o VS foi especializado
        u32 instance = 0; // 0 = por vertice; 1 / 2 = por instancia com passo 0 / 1; 3 = por instancia
        u32 offset = 0;   // deslocamento em bytes que o fetch shader soma ao ler
    };
    std::vector<Attribute> attributes;
    // Fase 29: carga em segundo plano. `pending` = o trabalho esta na fila; `want_snapshot` = o par
    // nao existe em disco e o proximo draw deve mandar o retrato para o recompilador ao vivo.
    bool pending = false, want_snapshot = false;
    u64 vs_hash = 0, ps_hash = 0, signature = 0;
    std::array<u32, 32> cntl{}; // SPI_PS_INPUT_CNTL_0..31 no momento em que o par foi visto
    // Fase 32: flatbuf. Codigo do walker de cada estagio (memoria executavel) e tamanho do buffer.
    void* walker[2] = {};
    u32 flat_dwords[2] = {};
    // Fase 37: par vindo do pacote AOT (compilado a partir dos .DAT, sem o jogo rodando). O tipo de
    // cada textura (2D ou cubo) foi previsto pelo extrator: o primeiro draw confere.
    bool aot = false, aot_checked = false;
    // Fase 37: assinatura por CLASSE (hash do fetch + float / uint / sint de cada atributo), que e
    // a chave do pacote, e os formatos reais dos atributos no draw (entram no lugar dos formatos-
    // modelo do pacote; o swizzle vai para o VkFormat da entrada de vertice).
    u64 canon_signature = 0;
    std::array<u32, 16> vformats{};
    u32 vformat_count = 0;
    u32 ps_input_mask = 0; // Fase 43: Locations das entradas do PS (antes de religar pelo SPI_PS_INPUT_CNTL)
};
// Um par tem VARIANTES (Fase 20): o mesmo VS aparece com fetch shaders e formatos de vertice
// diferentes, e o recompilador especializa o SPIR-V para eles. A assinatura junta o conteudo do
// fetch shader e o formato de cada atributo.
std::map<std::tuple<u64, u64, u64>, GamePairInfo> g_game_pair_by_hash;
std::map<std::tuple<u64, u64, u64>, GamePairInfo*> g_game_pair_by_address;

bool LoadSpirv(const std::filesystem::path& path, std::vector<u32>* words) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    words->resize(bytes > 0 ? bytes / 4 : 0);
    const size_t read = words->empty() ? 0 : std::fread(words->data(), 4, words->size(), f);
    std::fclose(f);
    return read == words->size() && read >= 5;
}

// Tipo de view que o modulo declara para cada imagem (binding -> 0 = 2D, 1 = cubo, 2 = array 2D,
// 3 = 3D), lido do proprio SPIR-V: o descritor vinculado tem de ter exatamente esse tipo, e o
// recompilador nem sempre segue o tipo do T# (um cubo pode virar array 2D).
bool ReflectImageViewTypes(const std::vector<u32>& words, std::map<u32, u32>* view_types) {
    std::map<u32, u32> image_type, pointer_to, variable_type, binding_of;
    bool supported = true;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) {
            break;
        }
        const u32* a = &words[i + 1];
        if (op == 25 && count >= 8) { // OpTypeImage: id, tipo amostrado, dim, depth, arrayed, ms, sampled
            const u32 dim = a[2], arrayed = a[4], multisampled = a[5];
            u32 type = 0xFF;
            if (!multisampled && dim == 1) type = arrayed ? 2 : 0;
            else if (!multisampled && dim == 3 && !arrayed) type = 1;
            else if (!multisampled && dim == 2 && !arrayed) type = 3;
            image_type[a[0]] = type;
        } else if (op == 32 && count >= 4) { // OpTypePointer
            pointer_to[a[0]] = a[2];
        } else if (op == 59 && count >= 4) { // OpVariable
            variable_type[a[1]] = a[0];
        } else if (op == 71 && count >= 4 && a[1] == 33) { // OpDecorate Binding
            binding_of[a[0]] = a[2];
        }
        i += count;
    }
    for (const auto& [variable, pointer] : variable_type) {
        const auto pointee = pointer_to.find(pointer);
        const auto binding = binding_of.find(variable);
        if (pointee == pointer_to.end() || binding == binding_of.end()) {
            continue;
        }
        if (const auto image = image_type.find(pointee->second); image != image_type.end()) {
            (*view_types)[binding->second] = image->second;
            supported &= image->second != 0xFF;
        }
    }
    return supported;
}

// Fase 21.5: liga cada entrada do pixel shader a saida certa do vertex shader. No PS4 isso e dito
// pelos registradores SPI_PS_INPUT_CNTL_n (contexto 0x191 + n): a entrada n do PS le o parametro
// `offset` exportado pelo VS. A ferramenta offline nao conhece esses registradores e gera o modulo
// com entrada n <- parametro n; aqui as decoracoes Location das entradas do PS sao reescritas com o
// mapa real do draw que carregou o par. (Sem isso um passe recebia, por exemplo, UVs no lugar da
// escala/bias de profundidade: faixas e borroes na tela inteira.)
// Fase 43: quais Locations o PS le (bit por Location, ate 31).
u32 PsInputLocations(const std::vector<u32>& words) {
    std::set<u32> inputs;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) {
            break;
        }
        if (op == 59 && count >= 4 && words[i + 3] == 1) { // OpVariable, classe Input
            inputs.insert(words[i + 2]);
        }
        i += count;
    }
    u32 mask = 0;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) {
            break;
        }
        if (op == 71 && count >= 4 && words[i + 2] == 30 && inputs.count(words[i + 1]) && words[i + 3] < 32) { // Location
            mask |= 1u << words[i + 3];
        }
        i += count;
    }
    return mask;
}

u32 RemapFragmentInputs(std::vector<u32>& words, const u32* input_cntl, u32 num_inputs, u32* defaults) {
    std::set<u32> inputs, flat;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) {
            break;
        }
        if (op == 59 && count >= 4 && words[i + 3] == 1) { // OpVariable, classe Input
            inputs.insert(words[i + 2]);
        } else if (op == 71 && count >= 3 && words[i + 2] == 14) { // OpDecorate Flat
            flat.insert(words[i + 1]);
        }
        i += count;
    }
    u32 changed = 0;
    std::vector<u32> make_flat;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) {
            break;
        }
        if (op == 71 && count >= 4 && words[i + 2] == 30 && inputs.count(words[i + 1])) { // OpDecorate Location
            const u32 location = words[i + 3];
            if (location < num_inputs && location < 32) {
                const u32 cntl = input_cntl[location];
                if (cntl & 0x20) { // use_default: a entrada nao vem do VS (valor fixo)
                    ++*defaults;
                } else if ((cntl & 0x1F) != location) {
                    words[i + 3] = cntl & 0x1F;
                    ++changed;
                }
                if ((cntl & 0x400) && !flat.count(words[i + 1])) { // flat_shade
                    make_flat.push_back(words[i + 1]);
                }
            }
        }
        i += count;
    }
    // as decoracoes Flat que faltam entram junto das outras anotacoes (antes do primeiro tipo)
    if (!make_flat.empty()) {
        for (size_t i = 5; i < words.size();) {
            const u32 count = words[i] >> 16, op = words[i] & 0xFFFF;
            if (!count || i + count > words.size()) {
                break;
            }
            if (op == 71) {
                std::vector<u32> extra;
                for (u32 id : make_flat) {
                    extra.insert(extra.end(), {(3u << 16) | 71u, id, 14u});
                }
                words.insert(words.begin() + i, extra.begin(), extra.end());
                break;
            }
            i += count;
        }
    }
    return changed;
}

constexpr const char* kPairNotCompiled = "pair not recompiled (no .txt)";

// ---- Fase 37: pacote AOT (assets/shaders_aot/aot_pairs.pak + .idx) ----
// Gerado por tools/nu2_shader_extractor a partir dos .ps4_shaders dos .DAT: ~120 mil variantes de
// par, com os blocos (.txt, .vs.spv, .ps.spv) deduplicados e compactados em XPRESS pela API de
// compressao do Windows. So e consultado quando o par nao existe em shaders_spv/pairs (os pares
// compilados com o retrato real do jogo tem prioridade).
struct AotKey {
    u64 vs, ps, signature;
    bool operator==(const AotKey& o) const {
        return vs == o.vs && ps == o.ps && signature == o.signature;
    }
};
struct AotKeyHash {
    size_t operator()(const AotKey& k) const noexcept {
        return static_cast<size_t>(k.vs ^ (k.ps * 0x9E3779B97F4A7C15ull) ^ (k.signature * 0xC2B2AE3D27D4EB4Full));
    }
};
struct AotBlob {
    u64 offset = 0;
    u32 packed = 0, size = 0;
};
struct AotStore {
    std::mutex mutex;
    bool loaded = false;
    std::vector<AotBlob> blobs;
    std::unordered_map<AotKey, std::array<u32, 3>, AotKeyHash> pairs; // blocos do .txt, .vs.spv e .ps.spv
    std::unordered_set<AotKey, AotKeyHash> rejected; // previsao de textura errada: nao usar de novo nesta execucao
    std::FILE* pak = nullptr;
    void* decompressor = nullptr;
    BOOL(WINAPI* decompress)(void*, const void*, SIZE_T, void*, SIZE_T, SIZE_T*) = nullptr;
    BOOL(WINAPI* create_decompressor)(DWORD, void*, void**) = nullptr; // Fase 43: um por thread
    double load_ms = 0;
};
// Fase 43: cada thread le e descompacta com o proprio arquivo aberto e o proprio descompactador
// (o indice, depois de lido, nao muda); o lock do pacote fica so na consulta.
struct AotThreadIo {
    std::FILE* pak = nullptr;
    void* decompressor = nullptr;
};
thread_local AotThreadIo t_aot_io;
thread_local bool t_aot_preload = false; // carga completa na abertura: direto ao pacote (sem procurar .txt solto)
AotStore g_aot;
std::atomic<bool> g_aot_enabled{true}; // --no-aot
std::atomic<bool> g_aot_first{false};  // --aot-first (diagnostico): o pacote antes dos pares compilados com o jogo
std::atomic<u64> g_aot_loaded{0}, g_aot_confirmed{0}, g_aot_rejected{0};

std::filesystem::path AotDirectory() {
    return std::filesystem::path(render::GameSpirvDirectory()).parent_path() / "shaders_aot";
}

// Com g_aot.mutex.
void AotLoadIndex() {
    g_aot.loaded = true;
    const auto start = std::chrono::steady_clock::now();
    const std::filesystem::path dir = AotDirectory();
    std::FILE* idx = _wfopen((dir / "aot_pairs.idx").c_str(), L"r");
    g_aot.pak = _wfopen((dir / "aot_pairs.pak").c_str(), L"rb");
    if (!idx || !g_aot.pak) {
        if (idx) std::fclose(idx);
        if (g_aot.pak) std::fclose(g_aot.pak);
        g_aot.pak = nullptr;
        log::Tagged("AOT", "pack not found at %s (run tools/nu2_shader_extractor)", dir.string().c_str());
        return;
    }
    if (HMODULE cabinet = LoadLibraryW(L"cabinet.dll")) {
        using Create = BOOL(WINAPI*)(DWORD, void*, void**);
        const auto create = reinterpret_cast<Create>(GetProcAddress(cabinet, "CreateDecompressor"));
        g_aot.decompress = reinterpret_cast<decltype(g_aot.decompress)>(GetProcAddress(cabinet, "Decompress"));
        constexpr DWORD kXpressHuff = 4, kRaw = 1u << 29; // COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW
        if (!create || !g_aot.decompress || !create(kXpressHuff | kRaw, nullptr, &g_aot.decompressor)) {
            g_aot.decompressor = nullptr;
        }
        g_aot.create_decompressor = create;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), idx)) {
        unsigned long long a = 0, b = 0, c = 0;
        u32 id = 0, x = 0, y = 0, z = 0;
        if (line[0] == 'b' && std::sscanf(line, "blob %u %llu %u %u", &id, &a, &x, &y) == 4) {
            if (id >= g_aot.blobs.size()) {
                g_aot.blobs.resize(id + 1);
            }
            g_aot.blobs[id] = {a, x, y};
        } else if (line[0] == 'p' && std::sscanf(line, "pair %llx %llx %llx %u %u %u", &a, &b, &c, &x, &y, &z) == 6) {
            g_aot.pairs[{a, b, c}] = {x, y, z};
        }
    }
    std::fclose(idx);
    g_aot.load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    log::Tagged("AOT", "%zu pair variants in pack (%zu blocks), index read in %.0f ms%s", g_aot.pairs.size(), g_aot.blobs.size(),
                g_aot.load_ms, g_aot.decompressor ? "" : " | NO decompressor (cabinet.dll): pack ignored");
}

// Fase 48: durante a carga completa na abertura o pacote fica inteiro na memoria (lido uma vez, em
// sequencia). Antes as 6 threads liam os blocos do arquivo; com o cache do Windows cheio (outros
// programas abertos) cada leitura ia ao HD em ordem espalhada: 188 s de tela de carregamento.
std::shared_mutex g_aot_pak_memory_mutex;
std::vector<u8> g_aot_pak_memory;

// Sem lock: arquivo e descompactador da propria thread (Fase 43).
bool AotReadBlob(u32 id, std::vector<u8>* out) {
    if (id >= g_aot.blobs.size() || !g_aot.decompressor || !g_aot.create_decompressor) {
        return false;
    }
    if (!t_aot_io.decompressor) {
        constexpr DWORD kXpressHuff = 4, kRaw = 1u << 29; // COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW
        if (!g_aot.create_decompressor(kXpressHuff | kRaw, nullptr, &t_aot_io.decompressor)) {
            t_aot_io.decompressor = nullptr;
            return false;
        }
    }
    {
        std::shared_lock lock(g_aot_pak_memory_mutex);
        const AotBlob& blob = g_aot.blobs[id];
        if (!g_aot_pak_memory.empty() && blob.offset + blob.packed <= g_aot_pak_memory.size()) {
            out->resize(blob.size);
            SIZE_T got = 0;
            return g_aot.decompress(t_aot_io.decompressor, g_aot_pak_memory.data() + blob.offset, blob.packed, out->data(), blob.size, &got) &&
                   got == blob.size;
        }
    }
    if (!t_aot_io.pak) {
        t_aot_io.pak = _wfopen((AotDirectory() / "aot_pairs.pak").c_str(), L"rb");
        constexpr DWORD kXpressHuff = 4, kRaw = 1u << 29; // COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW
        (void)kXpressHuff;
        (void)kRaw;
    }
    if (!t_aot_io.pak || !t_aot_io.decompressor) {
        return false;
    }
    const AotBlob& blob = g_aot.blobs[id];
    static thread_local std::vector<u8> packed;
    packed.resize(blob.packed);
    if (_fseeki64(t_aot_io.pak, static_cast<long long>(blob.offset), SEEK_SET) ||
        std::fread(packed.data(), 1, blob.packed, t_aot_io.pak) != blob.packed) {
        return false;
    }
    out->resize(blob.size);
    SIZE_T got = 0;
    return g_aot.decompress(t_aot_io.decompressor, packed.data(), blob.packed, out->data(), blob.size, &got) && got == blob.size;
}

// O VS do pacote foi compilado com swizzle identidade (XYZW). O swizzle real do V# tem de caber
// no VkFormat da entrada de vertice: identidade (componente que o formato nao tem: 0 / 1, igual ao
// Vulkan) ou BGRA em 8_8_8_8 unorm (B8G8R8A8).
bool AotSwizzleFits(u32 format) {
    const u32 data = (format >> 15) & 0xF, sel = format & 0xFFF;
    if (data == 0xA && ((format >> 12) & 7) == 0 && sel == 0xF2E) {
        return true;
    }
    static const u8 kComponents[16] = {0, 1, 1, 2, 1, 2, 3, 3, 4, 4, 4, 2, 4, 3, 4, 0};
    const u32 components = kComponents[data];
    for (u32 i = 0; i < 4; ++i) {
        const u32 s = (sel >> (3 * i)) & 7;
        if (s != 4 + i && !(i >= components && s == (i == 3 ? 1u : 0u))) {
            return false;
        }
    }
    return true;
}

struct AotPair {
    bool ok = false;
    std::string text;
    std::vector<u32> vs, ps;
};

bool AotFind(u64 vs_hash, u64 ps_hash, u64 signature, AotPair* out, bool ignore_rejected = false) {
    if (!g_aot_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    std::array<u32, 3> ids;
    {
        std::lock_guard lock(g_aot.mutex);
        if (!g_aot.loaded) {
            AotLoadIndex();
        }
        const auto it = g_aot.pairs.find({vs_hash, ps_hash, signature});
        if (it == g_aot.pairs.end() || !g_aot.pak || (!ignore_rejected && g_aot.rejected.count({vs_hash, ps_hash, signature}))) {
            return false;
        }
        ids = it->second;
    }
    std::vector<u8> bytes[3];
    for (u32 i = 0; i < 3; ++i) {
        if (!AotReadBlob(ids[i], &bytes[i])) {
            return false;
        }
    }
    out->text.assign(reinterpret_cast<const char*>(bytes[0].data()), bytes[0].size());
    out->vs.resize(bytes[1].size() / 4);
    std::memcpy(out->vs.data(), bytes[1].data(), out->vs.size() * 4);
    out->ps.resize(bytes[2].size() / 4);
    std::memcpy(out->ps.data(), bytes[2].data(), out->ps.size() * 4);
    out->ok = out->vs.size() >= 5 && out->ps.size() >= 5;
    return out->ok;
}

// Fase 33: o recompilador as vezes marca ENTRADAS do vertex shader como Flat (decoracao 14), o que
// o SPIR-V nao permite nesse estagio; o modulo era recusado e o par (particulas / brilhos por
// instancia) ficava de fora. A decoracao nao muda nada numa entrada de vertice: e trocada por OpNop.
u32 StripFlatDecorations(std::vector<u32>& words) {
    // Fase 39: a instrucao sai do modulo (antes virava OpNop, que a validacao recusa fora de um bloco)
    u32 removed = 0;
    size_t out = 5;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, opcode = words[i] & 0xFFFF;
        if (!count || i + count > words.size()) {
            std::copy(words.begin() + i, words.end(), words.begin() + out); // resto malformado: fica como esta
            out += words.size() - i;
            break;
        }
        if (opcode == 71 && count == 3 && words[i + 2] == 14) { // OpDecorate <id> Flat
            ++removed;
        } else {
            std::copy(words.begin() + i, words.begin() + i + count, words.begin() + out);
            out += count;
        }
        i += count;
    }
    words.resize(out);
    return removed;
}

// Pode rodar fora da thread de render (Fase 29): nao le o estado rastreado do GNM, so `cntl`.
// Fase 38: carga de par que passa de 100 ms vai para o log com o tempo de cada etapa (a primeira
// carga de uma execucao sem aquecimento ja levou 5,5 s sem explicacao).
struct SlowPairLoad {
    const char* name;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double marks[3] = {-1, -1, -1};
    void Mark(int i) {
        marks[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    ~SlowPairLoad() {
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (total > 100.0) {
            std::printf("[SLOW-PAIR] %s: %.0f ms (find %.0f | read and build %.0f | reflection %.0f ms)\n", name, total, marks[0], marks[1],
                        marks[2]);
        }
    }
};

// Fase 59: pares do pacote registrados sem modulos (chave do backend -> chave no pacote).
std::atomic<bool> g_lazy_aot{true}; // --no-lazy-aot: os modulos de todo o pacote criados na abertura (como antes)
std::mutex g_lazy_mutex;
std::unordered_map<u64, AotKey> g_lazy_sources;

// Carregador dos modulos de um par do pacote (threads de pipeline): o mesmo SPIR-V que a carga da abertura
// montaria (decoracoes Flat fora do VS; entradas do PS no mapa padrao, como TakeReadyAot exige).
bool LoadLazyAotModules(u64 pair_key, std::vector<u32>* vs, std::vector<u32>* ps) {
    AotKey source;
    {
        std::lock_guard lock(g_lazy_mutex);
        const auto it = g_lazy_sources.find(pair_key);
        if (it == g_lazy_sources.end()) {
            return false;
        }
        source = it->second;
    }
    AotPair aot;
    if (!AotFind(source.vs, source.ps, source.signature, &aot, true)) {
        return false;
    }
    *vs = std::move(aot.vs);
    *ps = std::move(aot.ps);
    StripFlatDecorations(*vs);
    std::array<u32, 32> cntl{};
    for (u32 i = 0; i < 32; ++i) {
        cntl[i] = i;
    }
    u32 defaults = 0;
    RemapFragmentInputs(*ps, cntl.data(), 32, &defaults);
    return vs->size() >= 5 && ps->size() >= 5;
}

void LoadGamePair(u64 vs_hash, u64 ps_hash, u64 signature, const u32* cntl, GamePairInfo* info) {
    const render::HitchClock hitch(render::HitchClock::kPair);
    char name[96];
    std::snprintf(name, sizeof(name), "pair_%016llx_%016llx_%016llx", static_cast<unsigned long long>(vs_hash),
                  static_cast<unsigned long long>(ps_hash), static_cast<unsigned long long>(signature));
    SlowPairLoad slow{name};
    const std::filesystem::path base = std::filesystem::path(render::GameSpirvDirectory()) / "pairs" / name;
    AotPair aot; // Fase 37: sem o par compilado com o jogo, procura no pacote AOT
    std::FILE* f = nullptr;
    const u64 canon = info->canon_signature;
    if (!((g_aot_first.load(std::memory_order_relaxed) || t_aot_preload) && canon && AotFind(vs_hash, ps_hash, canon, &aot))) {
        f = _wfopen((base.wstring() + L".txt").c_str(), L"r");
        if (f) {
            // Fase 57: par antigo, traduzido sem os descritores reais ("real nao" na 1a linha). Antes ele
            // ficava de fora para sempre (o arquivo em disco tem prioridade e nao ia ao recompilador): a
            // plataforma do seletor de mundos em Vorton saia preta. Agora conta como inexistente: usa o
            // pacote AOT, ou vai ao recompilador ao vivo, que regrava o arquivo com os descritores reais.
            char first[512] = {};
            if (!std::fgets(first, sizeof(first), f) || !shader_metadata::HasActualDescriptors(first)) {
                std::fclose(f);
                f = nullptr;
                static std::atomic<u32> logged{0};
                if (logged.fetch_add(1) < 20) {
                    std::printf("[PAIR] %s: file lacks actual descriptors - using AOT pack or recompiler\n", name);
                }
            } else {
                std::rewind(f);
            }
        }
        if (!f && !(canon && AotFind(vs_hash, ps_hash, canon, &aot))) {
            info->why = kPairNotCompiled;
            return;
        }
    }
    slow.Mark(0);
    size_t aot_pos = 0;
    render::GamePairLayout layout;
    static thread_local std::vector<char> line_buffer(1u << 18); // a linha do walker passa de 512 caracteres
    char* const line = line_buffer.data();
    const int line_size = static_cast<int>(line_buffer.size());
    u32 stage = 0;
    bool real = false, bad = false;
    auto parse_source = [&](const char* text, GamePairInfo::Resource* r) {
        if (std::sscanf(text, "ud:%u", &r->reg) == 1) {
            r->source = 1;
        } else if (std::sscanf(text, "tab:%u:%u", &r->reg, &r->dword) == 2) {
            r->source = 2;
        } else if (text[0] == '-') {
            r->source = 0;
        } else {
            bad = true;
            info->why = "unknown descriptor source";
        }
    };
    auto next_line = [&]() -> bool {
        if (f) {
            return std::fgets(line, line_size, f) != nullptr;
        }
        if (aot_pos >= aot.text.size()) {
            return false;
        }
        size_t end = aot.text.find('\n', aot_pos);
        end = end == std::string::npos ? aot.text.size() : end + 1;
        const size_t n = std::min<size_t>(end - aot_pos, static_cast<size_t>(line_size) - 1);
        std::memcpy(line, aot.text.data() + aot_pos, n);
        line[n] = 0;
        aot_pos = end;
        return true;
    };
    while (next_line()) {
        char a[64] = {}, b[64] = {};
        u32 x = 0, y = 0, z = 0, mask = 0;
        GamePairInfo::Resource r{};
        if (!std::strncmp(line, "pair ", 5)) {
            real = shader_metadata::HasActualDescriptors(line);
        } else if (std::sscanf(line, "stage %63s hash %*s first_binding %*u first_user_data %u ud_mask %u", a, &y, &mask) == 3) {
            stage = !std::strcmp(a, "ps");
            info->first_ud[stage] = y;
            info->ud_mask[stage] = mask;
        } else if (std::sscanf(line, " buffer binding %u type %63s src %63s", &x, a, b) == 3) {
            r = {stage, render::GamePairLayout::kBuffer, x, 0, 0, 0};
            if (!std::strcmp(a, "guest")) {
                parse_source(b, &r);
            } else if (!std::strcmp(a, "flatbuf")) {
                r.source = 3;
            } else if (std::strcmp(a, "clip_planes")) {
                bad = true;
                info->why = std::string("unsupported buffer type: ") + a;
            }
            info->resources.push_back(r);
        } else if (std::sscanf(line, " srt_walker dwords %u hex", &x) == 1) {
            // codigo x86 do walker deste estagio, em hexadecimal ("-" = o shader nao segue ponteiros)
            const char* hex = std::strstr(line, " hex ");
            hex = hex ? hex + 5 : "";
            std::vector<u8> code;
            auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
            for (; nibble(hex[0]) >= 0 && nibble(hex[1]) >= 0; hex += 2) {
                code.push_back(static_cast<u8>(nibble(hex[0]) * 16 + nibble(hex[1])));
            }
            info->flat_dwords[stage] = std::clamp<u32>(x, 16, 4096);
            if (!code.empty()) {
                void* memory = VirtualAlloc(nullptr, code.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (memory) {
                    std::memcpy(memory, code.data(), code.size());
                    FlushInstructionCache(GetCurrentProcess(), memory, code.size());
                    info->walker[stage] = memory;
                }
            }
        } else if (std::sscanf(line, " image binding %u src %63s", &x, b) == 2) {
            r = {stage, render::GamePairLayout::kImage, x, 0, 0, 0};
            parse_source(b, &r);
            info->resources.push_back(r);
        } else if (std::sscanf(line, " sampler binding %u src %63s", &x, b) == 2) {
            r = {stage, render::GamePairLayout::kSampler, x, 0, 0, 0};
            parse_source(b, &r);
            info->resources.push_back(r);
        } else if (std::sscanf(line, " attribute location %u table_sgpr %u dword %u", &x, &y, &z) == 3) {
            u32 instance = 0, offset = 0;
            if (const char* extra = std::strstr(line, " instance ")) {
                std::sscanf(extra, " instance %u offset %u", &instance, &offset);
            }
            info->attributes.push_back({x, y, z, ~0u, instance, offset});
        } else if (std::sscanf(line, " vformat location %u format %u", &x, &y) == 2) {
            for (auto& attribute : info->attributes) {
                if (attribute.location == x) {
                    attribute.format = y;
                }
            }
        }
    }
    if (f) {
        std::fclose(f);
    }
    if (aot.ok) {
        // formatos reais do draw no lugar dos formatos-modelo (swizzle identidade) do pacote
        for (auto& attribute : info->attributes) {
            const bool fits = attribute.location < info->vformat_count && AotSwizzleFits(info->vformats[attribute.location]);
            if (!fits) {
                {
                    std::lock_guard lock(g_aot.mutex);
                    g_aot.rejected.insert({vs_hash, ps_hash, canon});
                }
                info->why = kPairNotCompiled; // vai para o recompilador ao vivo, como um par que nao existisse
                return;
            }
            attribute.format = info->vformats[attribute.location] & 0x7FFFF;
        }
    }
    u32 buffers = 0, images = 0, samplers = 0;
    for (const auto& r : info->resources) {
        layout.bindings.push_back({r.binding, r.kind});
        buffers += r.kind == render::GamePairLayout::kBuffer;
        images += r.kind == render::GamePairLayout::kImage;
        samplers += r.kind == render::GamePairLayout::kSampler;
    }
    for (const auto& attribute : info->attributes) {
        if (attribute.format == ~0u) {
            bad = true;
            info->why = "attribute has no registered format";
        }
    }
    if (!real) {
        info->why = "pair compiled without actual descriptors";
        return;
    }
    if (bad) {
        return;
    }
    if (buffers > 24 || images > 16 || samplers > 16 || info->attributes.size() > 16) {
        info->why = "too many resources for backend";
        return;
    }
    if (aot.ok) {
        layout.vs_spirv = std::move(aot.vs);
        layout.ps_spirv = std::move(aot.ps);
    } else if (!LoadSpirv(base.wstring() + L".vs.spv", &layout.vs_spirv) || !LoadSpirv(base.wstring() + L".ps.spv", &layout.ps_spirv)) {
        info->why = "missing .spv file";
        return;
    }
    slow.Mark(1);
    StripFlatDecorations(layout.vs_spirv);
    info->ps_input_mask = PsInputLocations(layout.ps_spirv);
    {
        // o modulo so declara as entradas que o PS usa (SPI_PS_IN_CONTROL nao passa pelo SET_CONTEXT_REG rastreado)
        const u32 num_inputs = 32;
        u32 defaults = 0;
        const u32 changed = RemapFragmentInputs(layout.ps_spirv, cntl, num_inputs, &defaults);
        static u32 logged = 0;
        if (g_target_diagnostics) {
            std::printf("[PS-IN?] pair %s: cntl %08x %08x %08x %08x %08x %08x\n", name, cntl[0], cntl[1], cntl[2], cntl[3], cntl[4], cntl[5]);
        }
        if ((changed || defaults) && logged < 40) {
            ++logged;
            std::printf("[PS-IN] pair %s: %u PS input(s) reconnected by SPI_PS_INPUT_CNTL, %u using default value (", name, changed, defaults);
            for (u32 i = 0; i < 8; ++i) {
                std::printf("%s%u", i ? " " : "", cntl[i] & 0x1F);
            }
            std::printf(")\n");
        }
    }
    std::map<u32, u32> view_types;
    if (!ReflectImageViewTypes(layout.vs_spirv, &view_types) || !ReflectImageViewTypes(layout.ps_spirv, &view_types)) {
        info->why = "unsupported texture type (1D, multisample or cube array)";
        return;
    }
    for (auto& resource : info->resources) {
        if (resource.kind == render::GamePairLayout::kImage) {
            resource.view_type = view_types.count(resource.binding) ? view_types[resource.binding] : 0;
        }
    }
    slow.Mark(2);
    info->key = (vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull) ^ (signature * 0xC2B2AE3D27D4EB4Full)) | 1;
    info->aot = aot.ok;
    if (aot.ok) {
        // chave propria: se o tipo de textura previsto estiver errado, o par recompilado ao vivo
        // entra no backend com a chave normal, sem esbarrar nos modulos deste
        info->key ^= 0xA070000000000A00ull;
    }
    // Fase 59: na carga do pacote inteiro (abertura), o par entra sem modulos: o SPIR-V e refeito do pacote
    // quando o primeiro pipeline dele for criado (LoadLazyAotModules).
    bool registered = false;
    if (aot.ok && t_aot_preload && g_lazy_aot.load(std::memory_order_relaxed) &&
        render::VulkanRenderer::Instance().RegisterGamePairLazy(info->key, layout.bindings)) {
        std::lock_guard lock(g_lazy_mutex);
        g_lazy_sources[info->key] = {vs_hash, ps_hash, canon};
        registered = true;
    }
    if (!registered && !render::VulkanRenderer::Instance().RegisterGamePair(info->key, layout)) {
        info->why = "Vulkan refused modules or layouts";
        return;
    }
    info->usable = true;
    g_aot_loaded += info->aot;
}

// ---- Fase 29: carga de pares fora da thread de render + recompilador ao vivo ----
// A thread de render so enfileira o trabalho; enquanto ele nao volta, os draws do par sao pulados
// (alguns frames). Par que nao existe em disco: o retrato do primeiro draw vai para a ferramenta
// (shader_compiler_tool --pairs-only), que roda como processo filho, e o par e carregado em seguida.
struct PairJob {
    u64 vs_hash = 0, ps_hash = 0, signature = 0;
    std::array<u32, 32> cntl{};
    GamePairInfo* target = nullptr; // no de g_game_pair_by_hash (endereco estavel)
    bool after_tool = false;
    u64 canon = 0; // Fase 37
    std::array<u32, 16> vformats{};
    u32 vformat_count = 0;
};
// Fase 37: assinatura por classe e formatos do draw atual (thread de render), para FindGamePair.
u64 g_draw_canon = 0;
std::array<u32, 16> g_draw_vformats{};
u32 g_draw_vformat_count = 0;

PairJob MakePairJob(const GamePairInfo& info, GamePairInfo* target, bool after_tool) {
    PairJob job{info.vs_hash, info.ps_hash, info.signature, info.cntl, target, after_tool};
    job.canon = info.canon_signature;
    job.vformats = info.vformats;
    job.vformat_count = info.vformat_count;
    return job;
}
struct PairDone {
    PairJob job;
    GamePairInfo result;
};
struct LiveSnapshot {
    std::string name, text; // "vs ps variante" e as linhas do retrato
    PairJob job;
};
std::mutex g_pair_mutex;
std::condition_variable g_pair_cv;
std::deque<PairJob> g_pair_jobs;
std::vector<PairDone> g_pair_done;
std::vector<LiveSnapshot> g_live_snapshots;
std::filesystem::path g_live_tool, g_live_root, g_live_shader_dir;
std::atomic<bool> g_pair_async{true};
// Fase 38: par que existe em disco (ou no pacote AOT) e carregado NA HORA, na thread de render; so o
// que nao existe vai para o recompilador ao vivo, em segundo plano. --no-sync-shaders volta ao modo
// da Fase 29 (tudo em segundo plano, draws de fora enquanto isso).
std::atomic<bool> g_sync_pairs{true};
// O projeto fica num HD: ler um par do disco custa um seek (10 a 30 ms, mais de 100 ms com o jogo
// lendo os .DAT ao mesmo tempo). Por isso so e carregado na hora o par que nao precisa do disco:
// os do pacote AOT depois que ele foi lido inteiro para o cache de arquivos do Windows na abertura.
// Par em arquivo solto (shaders_spv/pairs) que nao veio pelo aquecimento continua em segundo plano.
std::atomic<bool> g_aot_prefetched{false}, g_pairs_listed{false};
std::atomic<u64> g_aot_prefetch_done{0}, g_aot_prefetch_total{0}; // Fase 42: bytes do pacote lidos / tamanho
std::atomic<bool> g_aot_prefetch_finished{false};
std::unordered_set<AotKey, AotKeyHash> g_pairs_on_disk; // pares compilados com o jogo (preenchido antes de g_pairs_listed)
u64 g_sync_pairs_deferred = 0; // so a thread de render: par que existe mas iria ao disco -> segundo plano
// ---- Fase 43: o pacote AOT inteiro carregado na abertura ----
// Cada variante do pacote vira um par pronto (modulos + layouts no Vulkan), com o mapa padrao de
// entradas do PS e formatos-modelo de vertice. No draw, um par novo usa o pronto se o
// SPI_PS_INPUT_CNTL do draw for o padrao nas entradas que o PS le e o swizzle dos atributos couber no
// VkFormat; senao, carrega como antes (Fase 37/38). O que o driver compila (pipeline) continua no
// primeiro uso: depende do estado do draw.
std::mutex g_ready_mutex;
std::unordered_map<AotKey, GamePairInfo, AotKeyHash> g_aot_ready; // protegido por g_ready_mutex
std::atomic<bool> g_preload_all{true}, g_ready_finished{false};
std::atomic<u32> g_ready_done{0}, g_ready_total{0}, g_ready_failed{0};
std::atomic<u64> g_ready_used{0}, g_ready_refused{0};
double g_ready_seconds = 0;
u64 g_ready_memory_mb = 0;
// Fase 41: carregar um par agora inclui o driver compilar as bibliotecas de pipeline dos dois
// shaders (~1 ms com o cache do driver quente, 20 a 60 ms frio). Orcamento por frame: passou, o par
// vai para a thread de pares e o draw espera alguns frames, em vez de travar a tela.
double g_pair_budget_ms = 16.0;
double g_pair_frame_ms = 0; // so a thread de render
u64 g_sync_pairs_over_budget = 0;

// Fase 43: par novo (thread de render): usa o pronto da abertura, se servir para este draw.
bool TakeReadyAot(GamePairInfo& fresh) {
    if (!g_aot_enabled.load(std::memory_order_relaxed) || !fresh.canon_signature || g_aot_ready.empty()) {
        return false;
    }
    const render::HitchClock hitch(render::HitchClock::kReady); // Fase 59
    if (!g_aot_first.load(std::memory_order_relaxed) &&
        (!g_pairs_listed.load(std::memory_order_acquire) || g_pairs_on_disk.count({fresh.vs_hash, fresh.ps_hash, fresh.signature}))) {
        return false; // o par compilado com o jogo tem prioridade
    }
    GamePairInfo ready;
    {
        std::lock_guard lock(g_ready_mutex);
        const auto it = g_aot_ready.find({fresh.vs_hash, fresh.ps_hash, fresh.canon_signature});
        if (it == g_aot_ready.end()) {
            return false;
        }
        ready = it->second;
    }
    for (u32 i = 0; i < 32; ++i) { // carregado com o mapa padrao de entradas do PS
        const u32 cntl = fresh.cntl[i];
        if (((ready.ps_input_mask >> i) & 1) && ((cntl & 0x1F) != i || (cntl & 0x420))) {
            ++g_ready_refused;
            return false;
        }
    }
    for (auto& attribute : ready.attributes) { // formatos reais do draw no lugar dos formatos-modelo
        if (attribute.location >= fresh.vformat_count || !AotSwizzleFits(fresh.vformats[attribute.location])) {
            ++g_ready_refused;
            return false;
        }
        attribute.format = fresh.vformats[attribute.location] & 0x7FFFF;
    }
    ready.vs_hash = fresh.vs_hash;
    ready.ps_hash = fresh.ps_hash;
    ready.signature = fresh.signature;
    ready.cntl = fresh.cntl;
    ready.canon_signature = fresh.canon_signature;
    ready.vformats = fresh.vformats;
    ready.vformat_count = fresh.vformat_count;
    fresh = std::move(ready);
    if (g_ready_used < 200) { // Fase 61: quais pares usaram o pronto da abertura (diagnostico das cabecas transparentes)
        std::printf("[AOT-READY] pair pair_%016llx_%016llx_%016llx (canon %016llx), PS inputs %08x\n",
                    static_cast<unsigned long long>(fresh.vs_hash), static_cast<unsigned long long>(fresh.ps_hash),
                    static_cast<unsigned long long>(fresh.signature), static_cast<unsigned long long>(fresh.canon_signature),
                    fresh.ps_input_mask);
    }
    ++g_ready_used;
    return true;
}

bool AotServableNow(u64 vs, u64 ps, u64 canon, u64 signature) {
    if (!canon || !g_aot_prefetched.load(std::memory_order_acquire) || !g_pairs_listed.load(std::memory_order_acquire)) {
        return false;
    }
    if (!g_aot_first.load(std::memory_order_relaxed) && g_pairs_on_disk.count({vs, ps, signature})) {
        return false; // o par compilado com o jogo tem prioridade e esta no disco
    }
    std::unique_lock lock(g_aot.mutex, std::try_to_lock); // uma leitura em andamento na thread de pares: nao espera
    return lock.owns_lock() && g_aot.pairs.count({vs, ps, canon}) && !g_aot.rejected.count({vs, ps, canon});
}
u64 g_sync_pairs_loaded = 0, g_sync_pairs_missing = 0; // so a thread de render
double g_sync_pairs_ms = 0, g_sync_pairs_worst_ms = 0;
std::atomic<u64> g_pairs_async_loaded{0}, g_pairs_live_requested{0}, g_pairs_live_ok{0}, g_live_runs{0}, g_live_ms{0};

// ---- Fase 36: aquecimento na abertura ----
// Cada par usado fica anotado (hashes + SPI_PS_INPUT_CNTL com que foi carregado) em pair_warm.tsv.
// Na execucao seguinte uma thread carrega todos eles enquanto o jogo mostra os logos, e depois
// manda o backend criar os pipelines que ja viu (pipeline_states.txt). O draw que encontra o par
// pronto nao espera nada.
struct WarmPair {
    GamePairInfo info;
};
std::map<std::tuple<u64, u64, u64>, WarmPair> g_pair_warm; // protegido por g_pair_mutex
std::set<std::tuple<u64, u64, u64>> g_pair_warm_saved;     // ja presentes no arquivo (thread de render + thread de aquecimento, com g_pair_mutex)
std::atomic<u32> g_warm_pairs_total{0}, g_warm_pairs_done{0}, g_warm_pairs_used{0};
std::atomic<bool> g_warm_enabled{true}, g_warm_finished{false};

std::filesystem::path WarmFile() {
    return std::filesystem::path(render::GameSpirvDirectory()) / "pair_warm.tsv";
}

void SaveWarmPair(const GamePairInfo& info) {
    if (!g_warm_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::lock_guard lock(g_pair_mutex);
        if (!g_pair_warm_saved.insert({info.vs_hash, info.ps_hash, info.signature}).second) {
            return;
        }
    }
    // Fase 45: a linha vai para o gravador em segundo plano (isto roda na thread de render)
    std::string line = Format("%016llx %016llx %016llx", static_cast<unsigned long long>(info.vs_hash),
                              static_cast<unsigned long long>(info.ps_hash), static_cast<unsigned long long>(info.signature));
    for (const u32 value : info.cntl) {
        line += Format(" %x", value);
    }
    if (info.aot) { // Fase 37: par do pacote: a chave dele e a assinatura por classe
        line += Format(" c %llx %u", static_cast<unsigned long long>(info.canon_signature), info.vformat_count);
        for (u32 i = 0; i < info.vformat_count; ++i) {
            line += Format(" %x", info.vformats[i]);
        }
    }
    line += "\n";
    FileWrite job;
    job.path = WarmFile();
    job.data = std::move(line);
    job.append = true;
    WriteFileAsync(std::move(job));
}

std::mutex g_live_tool_mutex; // Fase 56: thread de pares e thread de render usam o mesmo arquivo de lote

// Fase 77: a ferramenta fica aberta (--serve) e recebe um arquivo de retratos por linha no stdin. Abrir a
// ferramenta custava ~250 ms por traducao na hora (memoria de sondagem, indices de shaders): era o engasgo de
// 200 a 400 ms de cada par novo. Aberta, um par custa ~10 ms. Ferramenta antiga (sem --serve): ela trata o
// arquivo vazio de --pairs-only e sai sem "DONE", e o jogo volta a abrir uma por lote. Com g_live_tool_mutex.
struct LiveServer {
    HANDLE process = nullptr;
    HANDLE input = nullptr;  // stdin dela
    HANDLE output = nullptr; // stdout dela
    std::string pending;
    bool unsupported = false;
};
LiveServer g_live_server;

void StopLiveServer() {
    auto& s = g_live_server;
    if (s.input) {
        CloseHandle(s.input);
    }
    if (s.output) {
        CloseHandle(s.output);
    }
    if (s.process) {
        if (WaitForSingleObject(s.process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(s.process, 1);
        }
        CloseHandle(s.process);
    }
    s.process = s.input = s.output = nullptr;
    s.pending.clear();
}

bool StartLiveServer(const std::filesystem::path& shader_dir, const std::filesystem::path& spv_dir,
                     const std::filesystem::path& empty_list) {
    auto& s = g_live_server;
    if (s.process) {
        if (WaitForSingleObject(s.process, 0) == WAIT_TIMEOUT) {
            return true;
        }
        StopLiveServer(); // caiu: abre outra
    }
    if (s.unsupported) {
        return false;
    }
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE child_in = nullptr, child_out = nullptr;
    if (!CreatePipe(&child_in, &s.input, &inherit, 0)) {
        return false;
    }
    if (!CreatePipe(&s.output, &child_out, &inherit, 0)) {
        CloseHandle(child_in);
        CloseHandle(s.input);
        s.input = nullptr;
        return false;
    }
    SetHandleInformation(s.input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(s.output, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + g_live_tool.wstring() + L"\" \"" + shader_dir.wstring() + L"\" \"" + spv_dir.wstring() +
                           L"\" --pairs-only \"" + empty_list.wstring() + L"\" --serve";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_in;
    startup.hStdOutput = child_out;
    startup.hStdError = child_out;
    PROCESS_INFORMATION process{};
    const bool ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   g_live_root.c_str(), &startup, &process);
    CloseHandle(child_in);
    CloseHandle(child_out);
    if (!ok) {
        StopLiveServer();
        return false;
    }
    CloseHandle(process.hThread);
    s.process = process.hProcess;
    return true;
}

// Manda um arquivo de retratos e espera o "DONE" (leitura bloqueante: volta assim que ela responde; se ela sair,
// o pipe quebra e a leitura falha). false = sem servidor.
bool ServeLiveRequest(const std::filesystem::path& list) {
    auto& s = g_live_server;
    const std::u8string path = list.u8string();
    const std::string line = std::string(path.begin(), path.end()) + "\n";
    DWORD written = 0;
    if (!WriteFile(s.input, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size()) {
        StopLiveServer();
        return false;
    }
    for (;;) {
        const size_t done = s.pending.find("DONE");
        if (done != std::string::npos) {
            const size_t end = s.pending.find('\n', done);
            if (end != std::string::npos) {
                s.pending.erase(0, end + 1);
                return true;
            }
        }
        char chunk[4096];
        DWORD got = 0;
        if (!ReadFile(s.output, chunk, sizeof(chunk), &got, nullptr) || !got) {
            // saiu sem responder: ferramenta sem --serve (ou caiu)
            const bool old_tool = s.pending.find("TOTAL") != std::string::npos;
            StopLiveServer();
            if (old_tool) {
                s.unsupported = true;
                log::Tagged("SHADER-LIVE", "recompiler without --serve: one run per batch");
            }
            return false;
        }
        s.pending.append(chunk, got);
        if (s.pending.size() > (1u << 20)) {
            s.pending.erase(0, s.pending.size() - 4096); // avisos de pares com falha: so interessa o fim
        }
    }
}

void RunLiveCompiler(const std::vector<LiveSnapshot>& batch) {
    std::lock_guard tool_lock(g_live_tool_mutex);
    const std::filesystem::path shader_dir = g_live_shader_dir;
    const std::filesystem::path spv_dir = render::GameSpirvDirectory();
    const std::filesystem::path list = spv_dir / "pair_snapshots_live.txt";
    if (std::FILE* f = _wfopen(list.c_str(), L"w")) {
        for (const LiveSnapshot& snapshot : batch) {
            std::fprintf(f, "pair %s\n%s", snapshot.name.c_str(), snapshot.text.c_str());
        }
        std::fclose(f);
    } else {
        return;
    }
    // Os retratos tambem vao para o arquivo geral, para uma execucao completa da ferramenta refazer
    // estes pares (antes so ficavam la se o jogo chegasse ao relatorio final).
    if (std::FILE* f = _wfopen((spv_dir / "pair_snapshots.txt").c_str(), L"a")) {
        for (const LiveSnapshot& snapshot : batch) {
            std::fprintf(f, "pair %s\n%s", snapshot.name.c_str(), snapshot.text.c_str());
        }
        std::fclose(f);
    }
    {
        const auto start = std::chrono::steady_clock::now();
        const std::filesystem::path empty_list = spv_dir / "pair_snapshots_vazio.txt";
        if (std::FILE* f = _wfopen(empty_list.c_str(), L"w")) {
            std::fclose(f);
        }
        if (StartLiveServer(shader_dir, spv_dir, empty_list) && ServeLiveRequest(list)) {
            const u64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            ++g_live_runs;
            g_live_ms += ms;
            log::Tagged("SHADER-LIVE", "%zu new pair(s) sent to recompiler (open): %llu ms", batch.size(), static_cast<unsigned long long>(ms));
            return;
        }
    }
    std::wstring command = L"\"" + g_live_tool.wstring() + L"\" \"" + shader_dir.wstring() + L"\" \"" + spv_dir.wstring() +
                           L"\" --pairs-only \"" + list.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto start = std::chrono::steady_clock::now();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                        g_live_root.c_str(), &startup, &process)) {
        log::Tagged("SHADER-LIVE", "could not start recompiler (error %lu): %s", GetLastError(), g_live_tool.string().c_str());
        return;
    }
    WaitForSingleObject(process.hProcess, 180000);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const u64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    ++g_live_runs;
    g_live_ms += ms;
    log::Tagged("SHADER-LIVE", "%zu new pair(s) sent to recompiler: %llu ms (exit code %lu)", batch.size(),
                static_cast<unsigned long long>(ms), code);
}

// Fase 41: duas threads (carregar um par agora inclui o driver compilar as bibliotecas de pipeline; com
// quatro, a CPU de 6 nucleos fica disputada e o jogo perde quadros);
// so a primeira roda o recompilador ao vivo, que usa um arquivo de lote fixo.
void PairWorker(int index) {
    for (;;) {
        PairJob job;
        bool have = false;
        std::vector<LiveSnapshot> batch;
        {
            std::unique_lock lock(g_pair_mutex);
            g_pair_cv.wait_for(lock, std::chrono::milliseconds(50), [] { return !g_pair_jobs.empty(); }); // Fase 56: era 400 ms
            if (!g_pair_jobs.empty()) {
                job = g_pair_jobs.front();
                g_pair_jobs.pop_front();
                have = true;
            } else if (index == 0) {
                batch.swap(g_live_snapshots); // juntou por ate 400 ms: uma execucao da ferramenta para todos
            }
        }
        if (have && !job.target) {
            continue;
        }
        if (have) {
            PairDone done;
            done.job = job;
            done.result.canon_signature = job.canon;
            done.result.vformats = job.vformats;
            done.result.vformat_count = job.vformat_count;
            LoadGamePair(job.vs_hash, job.ps_hash, job.signature, job.cntl.data(), &done.result);
            std::lock_guard lock(g_pair_mutex);
            g_pair_done.push_back(std::move(done));
        } else if (!batch.empty()) {
            RunLiveCompiler(batch);
            std::lock_guard lock(g_pair_mutex);
            for (const LiveSnapshot& snapshot : batch) {
                g_pair_jobs.push_back(snapshot.job);
            }
        }
    }
}

void QueuePairJob(const PairJob& job) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 2; ++i) {
            std::thread(PairWorker, i).detach();
        }
    });
    {
        std::lock_guard lock(g_pair_mutex);
        g_pair_jobs.push_back(job);
    }
    g_pair_cv.notify_one();
}

// Thread de render, com g_mutex: aplica os pares que a thread de carga terminou.
void DrainPairResults() {
    std::vector<PairDone> done;
    {
        std::lock_guard lock(g_pair_mutex);
        done.swap(g_pair_done);
    }
    for (PairDone& d : done) {
        GamePairInfo& target = *d.job.target;
        const bool missing = !d.result.usable && d.result.why == kPairNotCompiled;
        target = std::move(d.result);
        target.vs_hash = d.job.vs_hash;
        target.ps_hash = d.job.ps_hash;
        target.signature = d.job.signature;
        target.cntl = d.job.cntl;
        target.pending = false;
        if (target.usable) {
            SaveWarmPair(target);
            ++g_pairs_async_loaded;
            g_pairs_live_ok += d.job.after_tool;
        } else if (missing && !d.job.after_tool && !g_live_tool.empty()) {
            target.want_snapshot = true; // o proximo draw do par manda o retrato para a ferramenta
        } else if (missing && d.job.after_tool) {
            target.why = "live recompiler could not translate pair";
        }
    }
}

GamePairInfo* FindGamePair(u64 vs_address, u64 ps_address, u64 signature) {
    static u64 last_vs = 0, last_ps = 0, last_signature = 0;
    static GamePairInfo* last = nullptr;
    if (vs_address == last_vs && ps_address == last_ps && signature == last_signature && last) {
        return last;
    }
    last_vs = vs_address;
    last_ps = ps_address;
    last_signature = signature;
    const auto known = g_game_pair_by_address.find({vs_address, ps_address, signature});
    if (known != g_game_pair_by_address.end()) {
        return last = known->second;
    }
    GamePairInfo* info = nullptr;
    const u64 vs_hash = ContentHashOfShader(0, vs_address), ps_hash = ContentHashOfShader(1, ps_address); // 0 = VS, 1 = PS
    if (vs_hash && ps_hash) {
        const auto [it, inserted] = g_game_pair_by_hash.try_emplace({vs_hash, ps_hash, signature});
        if (inserted) {
            GamePairInfo& fresh = it->second;
            fresh.vs_hash = vs_hash;
            fresh.ps_hash = ps_hash;
            fresh.signature = signature;
            fresh.canon_signature = g_draw_canon;
            fresh.vformats = g_draw_vformats;
            fresh.vformat_count = g_draw_vformat_count;
            std::memcpy(fresh.cntl.data(), &g_ctx_track[0x191], sizeof(fresh.cntl));
            bool warm = false;
            {
                std::lock_guard lock(g_pair_mutex);
                const auto ready = g_pair_warm.find({vs_hash, ps_hash, signature});
                if (ready != g_pair_warm.end()) { // carregado pelo aquecimento: nada a esperar
                    const std::array<u32, 32> cntl = fresh.cntl;
                    fresh = std::move(ready->second.info);
                    fresh.vs_hash = vs_hash;
                    fresh.ps_hash = ps_hash;
                    fresh.signature = signature;
                    fresh.cntl = cntl;
                    g_pair_warm.erase(ready);
                    warm = true;
                    ++g_warm_pairs_used;
                }
            }
            const bool ready = !warm && TakeReadyAot(fresh); // Fase 43
            warm = warm || ready;
            const bool sync_mode = !warm && g_sync_pairs.load(std::memory_order_relaxed);
            const bool servable = sync_mode && AotServableNow(vs_hash, ps_hash, fresh.canon_signature, signature);
            const bool load_now = servable && g_pair_frame_ms < g_pair_budget_ms;
            g_sync_pairs_over_budget += servable && !load_now;
            g_sync_pairs_deferred += sync_mode && !load_now;
            if (warm) {
            } else if (load_now) {
                const auto start = std::chrono::steady_clock::now();
                LoadGamePair(vs_hash, ps_hash, signature, fresh.cntl.data(), &fresh);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                g_pair_frame_ms += ms;
                g_sync_pairs_ms += ms;
                g_sync_pairs_worst_ms = std::max(g_sync_pairs_worst_ms, ms);
                if (fresh.usable) {
                    ++g_sync_pairs_loaded;
                    SaveWarmPair(fresh);
                } else if (fresh.why == kPairNotCompiled) {
                    ++g_sync_pairs_missing;
                    // nao existe compilado: o proximo draw manda o retrato para o recompilador ao vivo
                    fresh.want_snapshot = !g_live_tool.empty();
                }
            } else if (g_pair_async.load(std::memory_order_relaxed)) {
                fresh.pending = true;
                fresh.why = "still loading in background";
                QueuePairJob(MakePairJob(fresh, &fresh, false));
            } else {
                LoadGamePair(vs_hash, ps_hash, signature, fresh.cntl.data(), &fresh);
            }
        }
        info = &it->second;
    }
    g_game_pair_by_address[{vs_address, ps_address, signature}] = info;
    return last = info;
}

// Fetch shader de um draw: atributos (pelo decodificador do dumper), o codigo ate o s_setpc (vai
// para o retrato, para a ferramenta recompilar com ELE) e o hash do codigo.
struct FetchInfo {
    std::vector<FetchAttribute> attributes;
    std::vector<u32> code;
    u64 hash = 0;
};

const FetchInfo* FindFetch(u64 pointer) {
    static std::unordered_map<u64, FetchInfo> cache; // os fetch shaders nao mudam depois de gerados
    const auto cached = cache.find(pointer);
    if (cached != cache.end()) {
        return &cached->second;
    }
    FetchInfo fetch;
    std::vector<u32> code(256);
    if (pointer >= 0x10000 && !(pointer & 3) && SafeCopy(code.data(), pointer, code.size() * 4)) {
        fetch.attributes = ParseFetchShaderLayout(code.data(), static_cast<u32>(code.size()));
        size_t length = code.size();
        for (size_t i = 0; i < code.size(); ++i) {
            if (code[i] == 0xBE802000) { // s_setpc_b64: fim do fetch shader
                length = i + 1;
                break;
            }
        }
        code.resize(length);
        fetch.hash = 0xCBF29CE484222325ull;
        for (u32 word : code) {
            fetch.hash = (fetch.hash ^ word) * 0x100000001B3ull;
        }
        fetch.code = std::move(code);
    }
    return &(cache[pointer] = std::move(fetch));
}

// ===================== cache de geometria (Fase 21) =====================
// Buffers de vertices e de indices do jogo ja copiados para o buffer estatico da GPU, por
// (endereco, tamanho). A cada uso so uma amostra da memoria do jogo e conferida (16 janelas de
// 32 bytes); a cada 240 frames o conteudo inteiro e conferido de novo. Mudou 3 vezes: o buffer e
// dinamico (particulas, interface) e passa a ser copiado para o anel a cada draw, como antes.
struct CachedGeometry {
    u64 offset = ~0ull;
    u64 bytes = 0; // quanto do buffer esta na GPU, a partir do endereco
    u64 sample = 0, full = 0;
    u64 checked_frame = 0;
    u64 seen_frame = ~0ull;
    u64 gate = 0; // amostra do comeco do buffer enquanto ele espera para ir para a GPU
    u32 seen = 0; // em quantos frames o buffer foi visto igual, antes de ir para a GPU
    u32 max_index = ~0u, max_count = 0, min_index = 0;
    u32 changes = 0;
    bool dynamic = false;
    u64 sampled_frame = ~0ull, sampled = 0; // Fase 59: amostra ja tirada neste frame (varios draws, mesmo buffer)
};
std::unordered_map<u64, CachedGeometry> g_geometry_cache; // por endereco do jogo (Fase 59: hash, era arvore)
u32 g_geometry_generation = 0;
u64 g_cache_frame = 0; // avanca a cada flip

__declspec(noinline) u64 RawSample(const u8* p, u64 bytes) {
    u64 hash = 0x9E3779B97F4A7C15ull ^ bytes;
    auto mix = [&](const u8* at, u64 count) {
        for (u64 i = 0; i + 8 <= count; i += 8) {
            u64 word;
            std::memcpy(&word, at + i, 8);
            hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
            hash ^= hash >> 29;
        }
    };
    if (bytes <= 512) {
        mix(p, bytes);
        return hash;
    }
    const u64 step = (bytes - 32) / 15;
    for (u32 i = 0; i < 16; ++i) {
        mix(p + i * step, 32);
    }
    return hash;
}

u64 FullHash(const u8* p, u64 bytes) {
    // Fase 61: 4 faixas independentes (a CPU faz as 4 multiplicacoes em paralelo; com uma faixa so, cada
    // passo esperava o anterior). So e comparado com outro FullHash da mesma execucao.
    u64 lane[4] = {0xC2B2AE3D27D4EB4Full ^ bytes, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull, 0x9E3779B97F4A7C15ull};
    u64 i = 0;
    for (; i + 32 <= bytes; i += 32) {
        u64 words[4];
        std::memcpy(words, p + i, 32);
        for (u32 k = 0; k < 4; ++k) {
            lane[k] = (lane[k] ^ words[k]) * 0x9E3779B97F4A7C15ull;
            lane[k] ^= lane[k] >> 32;
        }
    }
    u64 hash = lane[0] ^ (lane[1] * 0xC2B2AE3D27D4EB4Full) ^ (lane[2] * 0x165667B19E3779F9ull) ^ (lane[3] * 0x27D4EB2F165667C5ull);
    for (; i + 8 <= bytes; i += 8) {
        u64 word;
        std::memcpy(&word, p + i, 8);
        hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
        hash ^= hash >> 32;
    }
    for (; i < bytes; ++i) {
        hash = (hash ^ p[i]) * 0x100000001B3ull;
    }
    return hash;
}

// Fase 61: orcamento de geometria por frame. Uma area nova traz centenas de buffers que completam os 3
// frames de espera JUNTOS (e depois fazem a conferencia completa juntos, a cada 240 frames): 500 copias +
// hashes no mesmo frame = 10 a 30 ms. Passou do orcamento, o que pode esperar espera: a promocao ao buffer
// estatico (o draw vai pelo anel, como nos 3 primeiros frames) e a conferencia completa de um buffer cuja
// amostra nao mudou. Nada deixa de ser desenhado.
double g_geometry_budget_ms = 2.0; // --geometry-budget MS (0 = sem limite, como antes)
u64 g_geometry_budget_frame = ~0ull;
double g_geometry_spent_ms = 0;
u64 g_geometry_deferred = 0, g_geometry_rechecks_deferred = 0;

bool GeometryBudgetLeft() {
    if (g_geometry_budget_frame != g_cache_frame) {
        g_geometry_budget_frame = g_cache_frame;
        g_geometry_spent_ms = 0;
    }
    return g_geometry_budget_ms <= 0 || g_geometry_spent_ms < g_geometry_budget_ms;
}

struct GeometrySpend {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~GeometrySpend() {
        g_geometry_spent_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};

bool SampleInner(u64 address, u64 bytes, u64* out) {
    __try {
        *out = RawSample(reinterpret_cast<const u8*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeSample(u64 address, u64 bytes, u64* out) {
    if (address < 0x10000) {
        return false;
    }
    const runtime::GuardedReadScope guard;
    return SampleInner(address, bytes, out);
}

// Resolve um buffer do jogo. `needed` e o trecho que este draw usa a partir de `address`; `whole`
// e o maximo que o descritor declara (os V# do jogo costumam declarar muito mais que a malha).
// Devolve true e
//   (a) *static_offset != ~0: o trecho esta valido no buffer estatico (nada copiado), ou
//   (b) *data apontando para a copia de `*used` bytes em `scratch`, a enviar pelo anel.
// `scratch` precisa ter `whole` bytes livres. false = memoria ilegivel.
//
// Fase 21.5. A chave e so o endereco, e a entrada guarda ate onde o buffer ja esta na GPU: um
// draw que precisa de mais faz o trecho crescer (com folga). Um buffer so vai para a GPU depois
// de visto em 3 frames com o mesmo comeco; ate la, e sempre que for dinamico, vai pelo anel so o
// trecho usado. As duas versoes anteriores (chave por trecho do draw; chave pelo buffer inteiro
// declarado) enchiam o buffer estatico dezenas de vezes, os aneis enchiam e draws eram
// descartados - a tela piscando.
// `skip`: bytes do comeco que este draw nao usa (vertices abaixo do menor indice). A copia para o
// anel os deixa de fora; o buffer estatico guarda tudo desde `address`.
bool ResolveGeometry(u64 address, u64 whole, u64 needed, u64 skip, u8* scratch, u64* static_offset, const u8** data, u64* used,
                     CachedGeometry** entry_out) {
    auto& renderer = render::VulkanRenderer::Instance();
    VulkanDrawStats& stats = g_vk_stats;
    if (const u32 generation = renderer.StaticGeneration(); generation != g_geometry_generation) {
        // O buffer estatico foi esvaziado: as posicoes deixam de valer; o que ja se sabe sobre
        // cada buffer (dinamico ou nao) fica.
        if (g_geometry_cache.size() > 200000) {
            g_geometry_cache.clear();
        }
        for (auto& [key, entry] : g_geometry_cache) {
            entry.offset = ~0ull;
            entry.bytes = 0;
        }
        g_geometry_generation = generation;
        ++stats.cache_resets;
    }
    *static_offset = ~0ull;
    *data = nullptr;
    *used = needed;
    *entry_out = nullptr;
    // Fase 29 (CORRECAO DE CRASH): `scratch` so tem `whole` bytes, os deste draw. A entrada do cache e
    // por endereco e guarda o maior trecho ja visto; um draw menor do mesmo buffer copiava
    // entry.bytes para o scratch e estourava a arena, corrompendo o heap (a violacao de acesso em
    // SubmitDrawToVulkan depois de ~16 min de jogo). Copias maiores que `whole` vao para `big`.
    static thread_local std::vector<u8> big;
    auto room_for = [&](u64 bytes) -> u8* {
        if (bytes <= whole) {
            return scratch;
        }
        if (big.size() < bytes) {
            big.resize(bytes);
        }
        return big.data();
    };
    auto copy_to_ring = [&] {
        std::optional<render::HitchClock> hitch; // Fase 59: so as copias grandes entram na conta
        if (needed - skip >= 65536) {
            hitch.emplace(render::HitchClock::kGeometry);
        }
        if (!SafeCopy(scratch, address + skip, needed - skip)) {
            return false;
        }
        *data = scratch;
        *used = needed - skip;
        stats.ring_bytes += needed - skip;
        return true;
    };
    if (needed < 1024 || !g_geometry_cache_enabled.load(std::memory_order_relaxed)) {
        return copy_to_ring(); // pequeno demais para valer a conferencia
    }
    CachedGeometry& entry = g_geometry_cache[address];
    *entry_out = &entry;
    if (entry.dynamic) {
        return copy_to_ring();
    }
    const bool cached = entry.offset != ~0ull;
    if (cached && needed <= entry.bytes) {
        u64 sample = 0;
        if (entry.sampled_frame == g_cache_frame) {
            sample = entry.sampled; // Fase 59: o mesmo buffer em varios draws do frame: uma amostra so
        } else if (!SafeSample(address, entry.bytes, &sample)) {
            return copy_to_ring();
        } else {
            entry.sampled_frame = g_cache_frame;
            entry.sampled = sample;
        }
        // Fase 61: a conferencia completa vem a cada 240 a 479 frames (espalhada pelo endereco: os buffers de uma
        // area, promovidos no mesmo frame, nao conferem mais todos juntos) e, sem orcamento no frame, fica para depois
        const u64 recheck_after = 240 + ((address >> 8) * 0x9E3779B1u >> 7) % 240;
        if (entry.sample == sample && (g_cache_frame - entry.checked_frame < recheck_after || !GeometryBudgetLeft())) {
            g_geometry_rechecks_deferred += g_cache_frame - entry.checked_frame >= recheck_after;
            *static_offset = entry.offset;
            stats.cache_hit_bytes += needed;
            ++stats.cache_hits;
            return true;
        }
        // amostra diferente, ou hora da conferencia completa
        const render::HitchClock hitch(render::HitchClock::kGeometry); // Fase 59
        const GeometrySpend spend; // Fase 61
        u8* const work = room_for(entry.bytes);
        if (!SafeCopy(work, address, entry.bytes)) {
            return copy_to_ring();
        }
        const u64 full = FullHash(work, entry.bytes);
        if (entry.full == full) {
            entry.sample = sample;
            entry.checked_frame = g_cache_frame;
            *static_offset = entry.offset;
            stats.cache_hit_bytes += needed;
            ++stats.cache_hits;
            ++stats.cache_rechecks;
            return true;
        }
        ++stats.cache_changed;
        if (++entry.changes >= 3) { // muda toda hora: dinamico, sempre pelo anel
            entry.dynamic = true;
            entry.offset = ~0ull;
            return copy_to_ring();
        }
        const u64 offset = renderer.StaticUpload(work, entry.bytes);
        if (offset == ~0ull) {
            entry.offset = ~0ull;
            return copy_to_ring();
        }
        entry.offset = offset;
        entry.sample = sample;
        entry.full = full;
        entry.checked_frame = g_cache_frame;
        entry.max_index = ~0u;
        *static_offset = offset;
        stats.cache_upload_bytes += entry.bytes;
        ++stats.cache_uploads;
        return true;
    }
    if (!cached) {
        // ainda fora da GPU: conta os frames em que o comeco do buffer apareceu igual
        u64 gate = 0;
        if (!SafeSample(address, std::min<u64>(needed, 512), &gate)) {
            return false;
        }
        if (entry.seen == 0 || entry.gate != gate) {
            if (entry.seen != 0 && ++entry.changes >= 3) {
                entry.dynamic = true;
                ++stats.cache_changed;
            }
            entry.gate = gate;
            entry.seen = 1;
            entry.seen_frame = g_cache_frame;
            return copy_to_ring();
        }
        if (entry.seen_frame != g_cache_frame) {
            ++entry.seen;
            entry.seen_frame = g_cache_frame;
        }
        if (entry.seen < 3) {
            return copy_to_ring();
        }
    }
    // Envia (ou reenvia maior): o trecho deste draw com folga de 50% sobre o que ja havia, sem
    // passar do que o descritor declara.
    if (!GeometryBudgetLeft()) { // Fase 61: sem orcamento neste frame: este draw vai pelo anel, a promocao fica para depois
        ++g_geometry_deferred;
        return copy_to_ring();
    }
    const render::HitchClock hitch(render::HitchClock::kGeometry); // Fase 59
    const GeometrySpend spend; // Fase 61
    u64 extent = std::max(needed, entry.bytes); // sem folga: memoria alem do que os draws usam pode ser de outro buffer
    u8* const work = room_for(extent);
    if (!SafeCopy(work, address, extent)) {
        extent = needed;
        if (!SafeCopy(work, address, extent)) {
            return false;
        }
    }
    const u64 offset = renderer.StaticUpload(work, extent);
    if (offset == ~0ull) { // buffer estatico cheio: este frame vai pelo anel
        return copy_to_ring();
    }
    entry.offset = offset;
    entry.bytes = extent;
    entry.sample = RawSample(work, extent);
    entry.full = FullHash(work, extent);
    entry.checked_frame = g_cache_frame;
    entry.max_index = ~0u;
    *static_offset = offset;
    stats.cache_upload_bytes += extent;
    ++stats.cache_uploads;
    return true;
}

// Tamanho de um alvo de render. O pitch e o slice do registrador vem arredondados para o tile
// (1080 linhas viram 1088); o tamanho usado e o do viewport / scissor quando ele cabe nessa
// folga. A decisao e tomada uma vez por (endereco, pitch, slice) e nao muda mais, para o mesmo
// alvo nao virar duas imagens.
struct TargetSize {
    u32 width, height;
};
std::map<std::tuple<u64, u32, u32>, TargetSize> g_target_sizes;

// Fase 21.5: quando o jogo usa um alvo como textura, o T# traz o tamanho exato (o registrador
// do alvo so traz o tamanho arredondado para o tile: 480 de largura vira 512). Uma imagem maior
// que o T# fazia o shader amostrar a area que sobra (imagem encolhida, com a sobra aparecendo
// como faixas na borda). O tamanho do T# passa a mandar; o alvo e recriado com ele.
std::map<u64, TargetSize> g_target_hints;

void NoteTargetHint(u64 address, u32 width, u32 height) {
    g_target_hints[address] = {width, height};
}

TargetSize DecideTargetSize(u64 address, u32 raw_pitch, u32 raw_slice, u32 padded_width, u32 padded_height) {
    const auto key = std::make_tuple(address, raw_pitch, raw_slice);
    if (const auto hint = g_target_hints.find(address);
        hint != g_target_hints.end() && hint->second.width <= padded_width && hint->second.height <= padded_height &&
        (hint->second.width * 2 > padded_width || padded_width <= 128) && (hint->second.height * 2 > padded_height || padded_height <= 64)) {
        // Fase 25: alvos pequenos tem pitch minimo de 128 e altura minima de 64 (um macrotile). O mapa
        // de ambiente 64x64 e a cadeia de luminancia (30x16, 7x4, 2x1) ficavam com 128 de largura, e
        // quem os amostrava com UV 0..1 lia metade da imagem mais uma faixa preta.
        // (o buffer de profundidade de 1920 de largura vem com pitch 2048: a folga chega a 128 pixels)
        g_target_sizes[key] = hint->second;
        return hint->second;
    }
    if (const auto known = g_target_sizes.find(key); known != g_target_sizes.end()) {
        return known->second;
    }
    float vp[4]; // xscale, xoffset, yscale, yoffset
    std::memcpy(vp, &g_ctx_track[0x10F], sizeof(vp));
    const u32 candidates_w[3] = {static_cast<u32>(std::lround(vp[1] + std::fabs(vp[0]))), g_ctx_track[0x00D] & 0x7FFF,
                                 g_ctx_track[0x091] & 0x7FFF};
    const u32 candidates_h[3] = {static_cast<u32>(std::lround(vp[3] + std::fabs(vp[2]))), (g_ctx_track[0x00D] >> 16) & 0x7FFF,
                                 (g_ctx_track[0x091] >> 16) & 0x7FFF};
    TargetSize size{padded_width, padded_height};
    for (u32 candidate : candidates_w) {
        if (candidate && candidate <= padded_width && (padded_width - candidate < 64 || padded_width <= 128)) {
            size.width = candidate;
            break;
        }
    }
    for (u32 candidate : candidates_h) {
        if (candidate && candidate <= padded_height && padded_height - candidate < 64) {
            size.height = candidate;
            break;
        }
    }
    g_target_sizes[key] = size;
    return size;
}

// Formato de um alvo de cor: CB_COLORn_INFO.format (5 bits) e number_type (0 unorm, 6 sRGB, 7 float).
bool MapTargetFormat(u32 format, u32 number, render::GameDraw::TargetFormat* out) {
    using D = render::GameDraw;
    const bool is_float = number == 7;
    switch (format) {
    case 1: *out = D::kTargetR8; return number == 0;
    case 2: *out = is_float ? D::kTargetR16F : D::kTargetR16; return is_float || number == 0;
    case 3: *out = D::kTargetRg8; return number == 0;
    case 4: *out = D::kTargetR32F; return is_float;
    case 5: *out = is_float ? D::kTargetRg16F : D::kTargetRg16; return is_float || number == 0;
    case 6:
    case 7: *out = D::kTargetR11G11B10F; return is_float;
    case 9: *out = D::kTargetA2Rgb10; return number == 0;
    case 10: *out = number == 6 ? D::kTargetRgba8Srgb : D::kTargetRgba8; return number == 0 || number == 6;
    case 12: *out = is_float ? D::kTargetRgba16F : D::kTargetRgba16; return is_float || number == 0;
    case 14: *out = D::kTargetRgba32F; return is_float;
    default: return false;
    }
}

// Formato de um atributo de vertice (V#: formato de dados, tipo numerico) e quantos bytes ocupa.
bool MapVertexFormat(u32 data, u32 num, render::GameDraw::Format* out, u32* bytes) {
    using D = render::GameDraw;
    struct Entry {
        u32 data, num;
        D::Format format;
        u32 bytes;
    };
    static const Entry kTable[] = {
        {0x4, 7, D::kF32x1, 4},      {0xB, 7, D::kF32x2, 8},      {0xD, 7, D::kF32x3, 12},     {0xE, 7, D::kF32x4, 16},
        {0x5, 7, D::kF16x2, 4},      {0xC, 7, D::kF16x4, 8},      {0xA, 0, D::kUnorm8x4, 4},   {0xA, 1, D::kSnorm8x4, 4},
        {0x5, 0, D::kUnorm16x2, 4},  {0xC, 0, D::kUnorm16x4, 8},  {0x5, 1, D::kSnorm16x2, 4},  {0xC, 1, D::kSnorm16x4, 8},
        {0xA, 4, D::kUint8x4, 4},    {0xA, 5, D::kSint8x4, 4},    {0xC, 4, D::kUint16x4, 8},   {0xC, 5, D::kSint16x4, 8},
        {0x4, 4, D::kUint32x1, 4},   {0x9, 0, D::kUnorm10x3A2, 4}, {0x6, 7, D::kF11F11F10, 4}, {0x7, 7, D::kF11F11F10, 4},
        {0x5, 4, D::kUint16x2, 4},   {0x5, 5, D::kSint16x2, 4}};
    for (const Entry& entry : kTable) {
        if (entry.data == data && entry.num == num) {
            *out = entry.format;
            *bytes = entry.bytes;
            return true;
        }
    }
    return false;
}

// Traduz um draw do jogo para o backend do Vulkan, com os shaders do proprio jogo (Fase 20: todo
// draw e candidato - interface e cena 3D). Daqui saem: os alvos de cor e de profundidade que o
// jogo configurou, o estado de blend / profundidade / cull, a geometria no layout do fetch shader
// (um ou mais buffers de vertices) e cada recurso lido da origem que o par recompilado indica.
// Um par ainda nao recompilado fica registrado (pair_snapshots.txt) para a ferramenta offline.
// Fase 28: tempo de cada etapa de um draw, somado por frame (0 = estado + alvos + fetch, 1 = par,
// 2 = atributos + buffers + texturas, 3 = QueueGameDraw).
// Fase 59: os cronometros por draw contam ciclos (rdtsc, ~1 ns) em vez de steady_clock::now (~25 ns: eram
// ~1,5% da thread de render); a conversao para ms usa a taxa medida desde o comeco do processo.
inline u64 Ticks() {
    return __rdtsc();
}
double TicksPerNs() {
    static const u64 tick0 = __rdtsc();
    static const auto time0 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - time0).count();
    return ns > 1e8 ? static_cast<double>(__rdtsc() - tick0) / ns : 3.0; // antes de 0,1 s: um palpite
}
const bool g_ticks_started = (TicksPerNs(), true);
u64 g_lap_ns[4] = {}; // (Fase 59: em ciclos; convertidos no NoteFrameHitch)
// Fase 59: sub-etapas. 0 = FindGamePair, 1 = retrato / recompilador, 2 = indices + atributos + geometria,
// 3 = buffers de constantes, 4 = texturas e samplers.
u64 g_sub_ns[5] = {};
#define LD_SUB(n, t0) g_sub_ns[n] += Ticks() - (t0)
#define LD_LAP(n)                    \
    do {                             \
        const u64 lap_now = Ticks(); \
        g_lap_ns[n] += lap_now - lap_t; \
        lap_t = lap_now;             \
    } while (0)

void QueueLiveSnapshot(u64 vs_address, u64 ps_address, u64 signature, GamePairInfo* info); // definida depois dos retratos
bool LiveCompileNow(u64 vs_address, u64 ps_address, u64 signature, GamePairInfo* info); // Fase 56

void SubmitDrawToVulkan(u32 opcode, const u32* body, u32 body_dwords) {
    u64 lap_t = Ticks();
    auto& renderer = render::VulkanRenderer::Instance();
    if (!g_vulkan_draws.load(std::memory_order_relaxed) || !g_game_shaders.load(std::memory_order_relaxed) ||
        !renderer.GameShadersSupported()) {
        return;
    }
    VulkanDrawStats& stats = g_vk_stats;
    u32 count = 0;
    u64 index_address = 0;
    if (opcode == 0x2D) { // DRAW_INDEX_AUTO
        count = body[0];
    } else if (opcode == 0x27 && body_dwords >= 4) { // DRAW_INDEX_2
        count = body[3];
        index_address = body[1] | (static_cast<u64>(body[2]) << 32);
    } else if (opcode == 0x35 && body_dwords >= 3 && g_index_base_track) { // DRAW_INDEX_OFFSET_2
        count = body[2];
        index_address = g_index_base_track + static_cast<u64>(body[1]) * g_index_size_track;
    } else {
        return;
    }
    if (count < 3 || count > 3'000'000) {
        ++stats.size;
        return;
    }
    render::GameDraw draw;
    switch (g_prim_track) {
    case 1: draw.primitive = render::DrawCall::kPointList; break;
    case 2: draw.primitive = render::DrawCall::kLineList; break;
    case 3: draw.primitive = render::DrawCall::kLineStrip; break;
    case 4: draw.primitive = render::DrawCall::kTriangleList; break;
    case 5: draw.primitive = render::DrawCall::kTriangleFan; break;
    case 6: draw.primitive = render::DrawCall::kTriangleStrip; break;
    case 0x11: // lista de retangulos: 3 vertices definem o retangulo. Sai so o primeiro triangulo (metade).
        draw.primitive = render::DrawCall::kTriangleList;
        ++stats.rect_list;
        break;
    case 0x13: draw.primitive = render::DrawCall::kQuadList; break;
    default:
        ++stats.primitive;
        ++stats.primitives[g_prim_track];
        return;
    }
    // ---- alvos ----
    const u32 color_mode = (g_ctx_track[0x202] >> 4) & 7; // CB_COLOR_CONTROL.mode: 0 = sem cor, 1 = normal
    if (color_mode > 1) {
        ++stats.color_mode; // resolve de MSAA, eliminacao de fast clear, descompressao: nao sao desenho
        return;
    }
    const u32 target_mask = g_ctx_track[0x08E];
    for (u32 i = 0; color_mode == 1 && i < 8; ++i) {
        const u32* cb = &g_ctx_track[0x318 + i * 15]; // base, pitch, slice, view, info, attrib
        const u32 format = (cb[4] >> 2) & 0x1F, number = (cb[4] >> 8) & 7;
        if (!cb[0] || !format || !((target_mask >> (i * 4)) & 0xF)) {
            break; // os anexos do Vulkan sao contiguos a partir do 0
        }
        render::GameDraw::Target& target = draw.colors[draw.color_count];
        if (!MapTargetFormat(format, number, &target.format)) {
            ++stats.target_format;
            ++stats.target_formats[(format << 4) | number];
            return;
        }
        target.address = static_cast<u64>(cb[0]) << 8;
        // Fase 26: o buffer de video e sRGB para a saida de video mesmo com o registrador dizendo
        // UNORM. Sem isso a imagem final ia para a tela linear (escura e sem contraste).
        if (target.format == render::GameDraw::kTargetRgba8 && g_srgb_display.load(std::memory_order_relaxed) &&
            videoout::IsSrgbDisplayBuffer(target.address)) {
            target.format = render::GameDraw::kTargetRgba8Srgb;
        }
        const u32 padded_width = ((cb[1] & 0x7FF) + 1) * 8;
        const u32 padded_height = ((cb[2] & 0x3FFFFF) + 1) * 64 / padded_width;
        const TargetSize size = DecideTargetSize(target.address, cb[1], cb[2], padded_width, padded_height);
        target.width = size.width;
        target.height = size.height;
        ++draw.color_count;
    }
    draw.depth_control = g_ctx_track[0x200];
    const bool depth_clear = g_ctx_track[0x000] & 1; // DB_RENDER_CONTROL.depth_clear_enable
    const u32 z_format = g_ctx_track[0x010] & 3;      // DB_Z_INFO.format: 1 = Z16, 3 = Z32F
    // Fase 34: estencil (DB_STENCIL_INFO.format != 0). Um draw so com teste de estencil tambem precisa
    // do alvo de profundidade; sem superficie de estencil o bit de ligar e ignorado.
    const bool has_stencil = (g_ctx_track[0x011] & 1) != 0;
    if (!has_stencil) {
        draw.depth_control &= ~1u;
    }
    const bool stencil_clear = has_stencil && ((g_ctx_track[0x000] >> 1) & 1);
    if (z_format && g_ctx_track[0x014] && (((draw.depth_control >> 1) & 1) || depth_clear || (draw.depth_control & 1) || stencil_clear)) {
        draw.has_depth = true;
        draw.depth.address = static_cast<u64>(g_ctx_track[0x014]) << 8; // DB_Z_WRITE_BASE
        const u32 depth_size = g_ctx_track[0x016];
        const u32 padded_width = ((depth_size & 0x7FF) + 1) * 8, padded_height = (((depth_size >> 11) & 0x7FF) + 1) * 8;
        const TargetSize size = DecideTargetSize(draw.depth.address, depth_size, 0, padded_width, padded_height);
        draw.depth.width = size.width;
        draw.depth.height = size.height;
        draw.depth.format = render::GameDraw::kTargetDepth32F;
        draw.stencil_control = (draw.depth_control & 1) ? g_ctx_track[0x10B] : 0; // DB_STENCIL_CONTROL
        draw.stencil_ref[0] = g_ctx_track[0x10C];                                    // DB_STENCILREFMASK
        draw.stencil_ref[1] = g_ctx_track[0x10D];                                    // DB_STENCILREFMASK_BF
        if (stencil_clear) {
            draw.clear_stencil = true;
            draw.clear_stencil_value = g_ctx_track[0x00A] & 0xFF; // DB_STENCIL_CLEAR
            ++stats.stencil_clears;
        }
        stats.stencil_draws += draw.depth_control & 1;
        if (depth_clear) {
            draw.clear_depth = true;
            std::memcpy(&draw.clear_depth_value, &g_ctx_track[0x00B], 4); // DB_DEPTH_CLEAR
            draw.clear_depth_value = std::clamp(draw.clear_depth_value, 0.0f, 1.0f);
            ++stats.depth_clears;
        }
    }
    if (!draw.color_count && !draw.has_depth) {
        ++stats.no_target;
        return;
    }
    stats.depth_only += draw.color_count == 0;
    // ---- viewport e scissor, em pixels do alvo ----
    float vp[6]; // xscale, xoffset, yscale, yoffset, zscale, zoffset
    std::memcpy(vp, &g_ctx_track[0x10F], sizeof(vp));
    if (!(std::fabs(vp[0]) >= 0.5f) || !(std::fabs(vp[2]) >= 0.5f) || !std::isfinite(vp[1]) || !std::isfinite(vp[3])) {
        ++stats.viewport;
        return;
    }
    // O sinal de yscale vai junto: negativo (o normal no PS4, Y do recorte para cima) vira um
    // viewport de altura negativa no Vulkan. O shader nao e alterado.
    draw.viewport[0] = vp[1] - std::fabs(vp[0]);
    draw.viewport[1] = vp[3] - vp[2];
    draw.viewport[2] = 2.0f * std::fabs(vp[0]);
    draw.viewport[3] = 2.0f * vp[2];
    const float z0 = std::isfinite(vp[5]) ? vp[5] : 0.0f, z1 = std::isfinite(vp[4]) ? vp[5] + vp[4] : 1.0f;
    draw.viewport[4] = std::clamp(z0, 0.0f, 1.0f);
    draw.viewport[5] = std::clamp(z1, 0.0f, 1.0f);
    if (g_target_diagnostics && draw.has_depth) { // --target-log: convencao de profundidade por alvo
        static std::set<std::tuple<u64, u32, u32, u32, u32, u32>> logged;
        if (logged.size() < 200 && logged.insert(std::make_tuple(draw.depth.address, g_ctx_track[0x113], g_ctx_track[0x114], g_ctx_track[0x204],
                                                                 draw.depth_control, static_cast<u32>(draw.color_count))).second) {
            float clear = 0, offset[5] = {};
            std::memcpy(&clear, &g_ctx_track[0x00B], 4);
            std::memcpy(offset, &g_ctx_track[0x2DF], sizeof(offset)); // PA_SU_POLY_OFFSET_CLAMP, FRONT_SCALE/OFFSET, BACK_SCALE/OFFSET
            std::printf("[PROF] target 0x%llx %ux%u colors %u | zscale %g zoffset %g | PA_CL_CLIP_CNTL %08x | depth_control %08x | "
                        "DB_RENDER_CONTROL %08x clear %g | SU_SC_MODE %08x poly offset clamp %g front %g/%g back %g/%g | Z_INFO %08x\n",
                        static_cast<unsigned long long>(draw.depth.address), draw.depth.width, draw.depth.height, draw.color_count, vp[4], vp[5],
                        g_ctx_track[0x204], draw.depth_control, g_ctx_track[0x000], clear, g_ctx_track[0x205], offset[0], offset[1], offset[2],
                        offset[3], offset[4], g_ctx_track[0x010]);
        }
    }
    auto corner = [](u32 value, s32* x, s32* y) {
        *x = value & 0x7FFF;
        *y = (value >> 16) & 0x7FFF;
    };
    s32 ax, ay, bx, by, cx, cy, dx, dy;
    corner(g_ctx_track[0x00C], &ax, &ay); // PA_SC_SCREEN_SCISSOR
    corner(g_ctx_track[0x00D], &bx, &by);
    corner(g_ctx_track[0x090], &cx, &cy); // PA_SC_GENERIC_SCISSOR
    corner(g_ctx_track[0x091], &dx, &dy);
    draw.scissor[0] = std::max(ax, cx);
    draw.scissor[1] = std::max(ay, cy);
    draw.scissor[2] = std::min(bx ? bx : 16384, dx ? dx : 16384);
    draw.scissor[3] = std::min(by ? by : 16384, dy ? dy : 16384);
    // Fase 25: alvo minusculo (a luminancia media da cena acaba num 2x1) -> o conteudo volta para a
    // memoria do jogo no frame seguinte, para o caso de a CPU ler a exposicao dali.
    if (draw.color_count == 1 && !draw.has_depth && std::fabs(draw.viewport[2]) * std::fabs(draw.viewport[3]) <= 16.0f) {
        const u32 w = static_cast<u32>(std::lround(std::fabs(draw.viewport[2]))), h = static_cast<u32>(std::lround(std::fabs(draw.viewport[3])));
        if (w && h && g_readback_enabled.load(std::memory_order_relaxed)) {
            renderer.RequestReadback(draw.colors[0].address, w, h);
        }
    }
    if (g_target_diagnostics && draw.color_count) { // --target-log: uma linha por combinacao distinta de alvo + viewport + scissor
        static std::set<std::tuple<u64, u32, u32, s32, s32, s32, s32>> logged;
        const auto key = std::make_tuple(draw.colors[0].address, draw.colors[0].width, draw.colors[0].height,
                                         static_cast<s32>(draw.viewport[0]), static_cast<s32>(draw.viewport[1]),
                                         static_cast<s32>(draw.viewport[2]), static_cast<s32>(draw.viewport[3]));
        if (logged.size() < 200 && logged.insert(key).second) {
            const u32* cb = &g_ctx_track[0x318];
            std::printf("[TARGET] draw at 0x%llx image %ux%u (register: pitch %u, height %u, format %u type %u swap %u) viewport x %.1f y %.1f "
                        "w %.1f h %.1f scissor (%d, %d)-(%d, %d) targets %u (1: 0x%llx, 2: 0x%llx) depth %d\n",
                        static_cast<unsigned long long>(draw.colors[0].address), draw.colors[0].width, draw.colors[0].height,
                        ((cb[1] & 0x7FF) + 1) * 8, ((cb[2] & 0x3FFFFF) + 1) * 64 / (((cb[1] & 0x7FF) + 1) * 8), (cb[4] >> 2) & 0x1F, (cb[4] >> 8) & 7, (cb[4] >> 11) & 3,
                        draw.viewport[0], draw.viewport[1], draw.viewport[2], draw.viewport[3], draw.scissor[0], draw.scissor[1],
                        draw.scissor[2], draw.scissor[3], draw.color_count,
                        static_cast<unsigned long long>(draw.color_count > 1 ? draw.colors[1].address : 0),
                        static_cast<unsigned long long>(draw.color_count > 2 ? draw.colors[2].address : 0), draw.has_depth);
        }
    }
    // ---- estado ----
    for (u32 i = 0; i < 8; ++i) {
        draw.blend[i] = g_ctx_track[0x1E0 + i];
    }
    draw.color_mask = target_mask;
    const u32 polygon = g_ctx_track[0x205]; // PA_SU_SC_MODE_CNTL: cull_front, cull_back, face
    draw.cull = polygon & 7;
    draw.depth_clamp = !draw.has_depth; // interface: o Z pode sair de 0..1 e nao deve ser cortado
    std::memcpy(draw.blend_constants, &g_ctx_track[0x105], 16); // CB_BLEND_RED..ALPHA

    // ---- par de shaders ----
    const u32* vs_ud = &g_vs_track[0x0C];
    const u32* ps_ud = &g_ps_track[0x0C];
    const u64 vs_address = (static_cast<u64>(g_vs_track[0x08]) << 8) | (static_cast<u64>(g_vs_track[0x09] & 0xFF) << 40);
    const u64 ps_address = (static_cast<u64>(g_ps_track[0x08]) << 8) | (static_cast<u64>(g_ps_track[0x09] & 0xFF) << 40);
    // Variante do par: fetch shader deste draw + formato de cada atributo (lidos agora, usados
    // de novo mais abaixo).
    const s32 fetch_sgpr = vs_address ? FetchSgprOfVertexShader(vs_address) : -1;
    const FetchInfo* fetch = nullptr;
    u64 signature = 0, canon = 0;
    std::array<u32, 16> vformats{};
    u32 vformat_count = 0;
    if (fetch_sgpr >= 0 && fetch_sgpr + 1 < 16) {
        fetch = FindFetch(vs_ud[fetch_sgpr] | (static_cast<u64>(vs_ud[fetch_sgpr + 1]) << 32));
        signature = fetch->hash;
        canon = fetch->hash;
        for (const FetchAttribute& attribute : fetch->attributes) {
            u32 words[4] = {};
            if (attribute.table_sgpr + 1 < 16) {
                const u64 table = vs_ud[attribute.table_sgpr] | (static_cast<u64>(vs_ud[attribute.table_sgpr + 1]) << 32);
                if (!SafeCopy(words, table + attribute.dword_offset * 4ull, 16)) {
                    ++stats.game_layout;
                    return;
                }
            }
            signature = (signature ^ (words[3] & 0x7FFFF)) * 0x100000001B3ull;
            const u32 num = (words[3] >> 12) & 7;
            canon = (canon ^ (0xC0000u | (num == 4 ? 1u : num == 5 ? 2u : 0u))) * 0x100000001B3ull;
            if (vformat_count < 16) {
                vformats[vformat_count++] = words[3] & 0x7FFFF;
            }
        }
    }
    g_draw_canon = canon;
    g_draw_vformats = vformats;
    g_draw_vformat_count = vformat_count;
    LD_LAP(0);
    const u64 sub_find = Ticks();
    GamePairInfo* info = FindGamePair(vs_address, ps_address, signature);
    LD_SUB(0, sub_find);
    if (g_target_diagnostics && (g_ctx_track[0x200] & 1)) { // --target-log: draws com teste de estencil ligado
        static std::set<std::tuple<u64, u32, u32, u32>> logged;
        const u64 ps_hash = ContentHashOfShader(1, ps_address);
        if (logged.size() < 120 && logged.insert(std::make_tuple(ps_hash, g_ctx_track[0x200], g_ctx_track[0x10C], g_ctx_track[0x10B])).second) {
            std::printf("[STENCIL] VS %016llx PS %016llx depth_control %08x STENCIL_CONTROL %08x REFMASK %08x REFMASK_BF %08x Z_INFO %08x "
                        "STENCIL_INFO %08x color target 0x%llx depth 0x%llx\n",
                        static_cast<unsigned long long>(ContentHashOfShader(0, vs_address)), static_cast<unsigned long long>(ps_hash),
                        g_ctx_track[0x200], g_ctx_track[0x10B], g_ctx_track[0x10C], g_ctx_track[0x10D], g_ctx_track[0x010], g_ctx_track[0x011],
                        static_cast<unsigned long long>(draw.color_count ? draw.colors[0].address : 0),
                        static_cast<unsigned long long>(draw.depth.address));
        }
    }
    {   // DIAGNOSTICO (LD_SKIP_PS=<hash do PS em hexa>): deixa de fora os draws desse pixel shader
        static const u64 skip_ps = std::getenv("LD_SKIP_PS") ? std::strtoull(std::getenv("LD_SKIP_PS"), nullptr, 16) : 0;
        if (skip_ps && ContentHashOfShader(1, ps_address) == skip_ps) {
            return;
        }
    }
    if (!info) {
        ++stats.no_hash; // shader que o extrator nao viu
        return;
    }
    // conta o draw e, na primeira vez, tira o retrato para a ferramenta
    const u64 sub_note = Ticks();
    NoteUiShaderPair(vs_address, ps_address, signature, fetch_sgpr, fetch ? &fetch->code : nullptr);
    if (!info->usable) {
        if (info->want_snapshot) {
            info->want_snapshot = false;
            if (!LiveCompileNow(vs_address, ps_address, signature, info)) { // Fase 56: na hora, se couber no frame
                QueueLiveSnapshot(vs_address, ps_address, signature, info);
            }
        }
        if (!info->usable) {
            ++stats.game_no_pair;
            return;
        }
    }
    LD_SUB(1, sub_note);
    LD_LAP(1);
    const u64 sub_geometry = Ticks();
    const u32* stage_ud[2] = {vs_ud, ps_ud};
    auto pointer_of = [](const u32* ud, u32 reg) { return reg + 1 < 16 ? ud[reg] | (static_cast<u64>(ud[reg + 1]) << 32) : 0ull; };
    if (info->aot && !info->aot_checked) {
        // Fase 37: par do pacote AOT. O extrator previu 2D ou cubo para cada textura lendo o codigo
        // do PS (97% de acerto nos pares conhecidos); o T# deste draw confirma. Errou: o par sai e o
        // retrato deste draw vai para o recompilador ao vivo, como um par que nao existisse.
        bool same = true;
        for (const auto& r : info->resources) {
            if (r.kind != render::GamePairLayout::kImage) {
                continue;
            }
            u32 words[8] = {};
            bool have = false;
            if (r.source == 1 && r.reg + 8 <= 16) {
                std::memcpy(words, stage_ud[r.stage] + r.reg, 32);
                have = true;
            } else if (r.source == 2) {
                have = SafeCopy(words, pointer_of(stage_ud[r.stage], r.reg) + r.dword * 4ull, 32);
            }
            const u32 type = words[3] >> 28;
            const u32 expected = type == 0x9 ? 0 : type == 0xB ? 1 : type == 0xD ? 2 : type == 0xA ? 3 : ~0u;
            // o recompilador declara cubo como array 2D de 6 camadas: cubo aceita 1 ou 2
            if (have && expected != ~0u && expected != r.view_type && !(type == 0xB && r.view_type == 2)) {
                same = false;
                static std::atomic<u32> logged{0};
                if (logged.fetch_add(1) < 40) {
                    std::printf("[AOT] pair %016llx %016llx %016llx: image binding %u (stage %u, source %u reg %u dword %u) with T# type %x, "
                                "pack declared %u\n",
                                static_cast<unsigned long long>(info->vs_hash), static_cast<unsigned long long>(info->ps_hash),
                                static_cast<unsigned long long>(info->signature), r.binding, r.stage, r.source, r.reg, r.dword, type,
                                r.view_type);
                }
            }
        }
        info->aot_checked = true;
        if (same) {
            ++g_aot_confirmed;
        } else {
            ++g_aot_rejected;
            {
                std::lock_guard lock(g_aot.mutex);
                g_aot.rejected.insert({info->vs_hash, info->ps_hash, info->canon_signature});
            }
            info->usable = false;
            info->aot = false;
            info->why = "AOT pack: wrong predicted texture type (recompiling live)";
            if (!LiveCompileNow(vs_address, ps_address, signature, info)) { // Fase 56: na hora, se couber no frame
                if (!g_live_tool.empty()) {
                    QueueLiveSnapshot(vs_address, ps_address, signature, info);
                }
                ++stats.game_no_pair;
                return;
            }
        }
    }
    draw.pair = info->key;

    // ---- indices e quantidade de vertices ----
    static thread_local std::vector<u8> index_copy, vertex_arena;
    u32 vertex_count = count;
    const u32 instance_count = std::max(1u, g_num_instances);
    draw.instance_count = instance_count;
    stats.instanced += instance_count > 1;
    if (index_address) {
        const u64 index_bytes = static_cast<u64>(count) * g_index_size_track;
        if (index_copy.size() < index_bytes) {
            index_copy.resize(index_bytes);
        }
        u64 static_offset = ~0ull;
        const u8* data = nullptr;
        CachedGeometry* entry = nullptr;
        const bool quads = draw.primitive == render::DrawCall::kQuadList; // os indices sao reescritos: sem cache
        if (quads) {
            if (!SafeCopy(index_copy.data(), index_address, index_bytes)) {
                ++stats.unreadable;
                return;
            }
            data = index_copy.data();
            stats.ring_bytes += index_bytes;
        } else if (u64 used = 0; !ResolveGeometry(index_address, index_bytes, index_bytes, 0, index_copy.data(), &static_offset, &data, &used, &entry)) {
            ++stats.unreadable;
            return;
        }
        u32 max_index = 0, min_index = ~0u;
        if (static_offset != ~0ull && entry && entry->max_index != ~0u && entry->max_count == count) {
            max_index = entry->max_index; // ja medidos quando o buffer foi enviado
            min_index = entry->min_index;
        } else {
            // (num acerto do cache sem maximo guardado nao ha copia local: mede direto na memoria do jogo)
            if (!data && !SafeCopy(index_copy.data(), index_address, index_bytes)) {
                ++stats.unreadable;
                return;
            }
            if (g_index_size_track == 2) {
                const u16* indices = reinterpret_cast<const u16*>(index_copy.data());
                for (u32 i = 0; i < count; ++i) {
                    max_index = std::max<u32>(max_index, indices[i]);
                    min_index = std::min<u32>(min_index, indices[i]);
                }
            } else {
                const u32* indices = reinterpret_cast<const u32*>(index_copy.data());
                for (u32 i = 0; i < count; ++i) {
                    max_index = std::max(max_index, indices[i]);
                    min_index = std::min(min_index, indices[i]);
                }
            }
            if (entry) {
                entry->min_index = min_index;
                entry->max_index = max_index;
                entry->max_count = count; // o maximo vale para esta quantidade de indices
            }
        }
        const s32 base_vertex = static_cast<s32>(g_ctx_track[0x102]); // VGT_INDX_OFFSET
        // Fase 77: o vertice base passa de 4 milhoes no mar de pecas do Prime Time (malhas no fim de um buffer de
        // 128 MB); o limite antigo, de 4 milhoes, deixava esses draws de fora. O V# ainda confere a contagem.
        if (max_index > 4'000'000 || base_vertex < 0 || base_vertex > 64'000'000) {
            ++stats.size;
            return;
        }
        draw.base_vertex = base_vertex;
        vertex_count = max_index + 1 + base_vertex;
        draw.first_vertex = std::min(min_index, max_index) + base_vertex; // o que vai pelo anel comeca aqui
        draw.index_data = static_offset != ~0ull ? nullptr : index_copy.data();
        draw.index_static_offset = static_offset;
        draw.index_count = count;
        draw.index_size = g_index_size_track;
    }
    draw.vertex_count = count;

    // ---- atributos: agrupados em buffers de vertices (mesmo stride, a menos de um stride de distancia) ----
    struct Binding {
        u64 base;
        u32 stride, end; // `end` = maior (offset + tamanho) entre os atributos do buffer
        bool per_instance;
        u32 records; // quantos elementos o V# declara (o menor entre os atributos do buffer)
        u32 step;    // Fase 44: o elemento avanca a cada `step` instancias (VGT_INSTANCE_STEP_RATE)
    } bindings[8];
    for (const auto& attribute : info->attributes) {
        u32 words[4];
        GuestVsharp v;
        if (!SafeCopy(words, pointer_of(vs_ud, attribute.table_sgpr) + attribute.dword * 4ull, 16) || !ReadVsharp(words, &v)) {
            ++stats.game_layout;
            return;
        }
        if ((v.format & 0x7FFFF) != attribute.format) {
            ++stats.game_format; // o VS foi especializado para outro formato / ordem de canais
            return;
        }
        render::GameDraw::Format format;
        u32 bytes = 0;
        if (info->aot && (v.format & 0xFFF) == 0xF2E && ((v.format >> 15) & 0xF) == 0xA && ((v.format >> 12) & 7) == 0) {
            format = render::GameDraw::kUnorm8x4Bgra; // Fase 37: o VS do pacote nao troca os canais
            bytes = 4;
        } else if (!MapVertexFormat((v.format >> 15) & 0xF, (v.format >> 12) & 7, &format, &bytes)) {
            ++stats.game_format;
            ++stats.vertex_formats[(((v.format >> 15) & 0xF) << 4) | ((v.format >> 12) & 7)];
            return;
        }
        // Atributo por instancia: o fetch shader o indexa pelo numero da instancia dividido pelo
        // passo (VGT_INSTANCE_STEP_RATE_0 / 1, para instance 1 / 2). Fase 44: o passo maior que 1
        // era ignorado e o draw caia como "memoria ilegivel" (o V# so tem instancias / passo
        // elementos): todos os draws instanciados de Vorton (efeitos) ficavam de fora.
        const bool per_instance = attribute.instance != 0;
        const u32 step = attribute.instance == 1 ? std::max(1u, g_ctx_track[0x2A8])
                         : attribute.instance == 2 ? std::max(1u, g_ctx_track[0x2A9])
                                                   : 1u;
        stats.instanced_step += step > 1;
        const u32 records_needed = per_instance ? (instance_count + step - 1) / step : vertex_count;
        if (v.count < records_needed) {
            ++stats.unreadable;
            return;
        }
        // DIAGNOSTICO (LD_DBG_VS=<hash do VS em hexa>): V# de cada atributo e buffers de constantes
        static const u64 dbg_vs = std::getenv("LD_DBG_VS") ? std::strtoull(std::getenv("LD_DBG_VS"), nullptr, 16) : 0;
        static u32 dbg_attributes = 0;
        if (dbg_vs && dbg_attributes < 40 && ContentHashOfShader(0, vs_address) == dbg_vs) {
            ++dbg_attributes;
            u32 first[4] = {};
            SafeCopy(first, v.address + attribute.offset, sizeof(first));
            std::printf("[DBG-ATR] loc %u instance %u V# base 0x%llx stride %u elements %u format %05x offset %u | vertices %u | %08x %08x %08x %08x\n",
                        attribute.location, attribute.instance, static_cast<unsigned long long>(v.address), v.stride, v.count, v.format & 0x7FFFF,
                        attribute.offset, vertex_count, first[0], first[1], first[2], first[3]);
        }
        v.address += attribute.offset;
        u32 b = 0;
        for (; b < draw.binding_count; ++b) {
            if (bindings[b].stride != v.stride || bindings[b].per_instance != per_instance || bindings[b].step != step) {
                continue;
            }
            if (v.address >= bindings[b].base && v.address - bindings[b].base + bytes <= v.stride) {
                break;
            }
            if (v.address < bindings[b].base && bindings[b].base - v.address + bindings[b].end <= v.stride) {
                const u32 shift = static_cast<u32>(bindings[b].base - v.address); // este atributo vem antes: recua a base
                for (u32 k = 0; k < draw.attribute_count; ++k) {
                    if (draw.attributes[k].binding == b) {
                        draw.attributes[k].offset += shift;
                    }
                }
                bindings[b].base = v.address;
                bindings[b].end += shift;
                break;
            }
        }
        if (b == draw.binding_count) {
            if (draw.binding_count == 8) {
                ++stats.game_layout;
                return;
            }
            bindings[draw.binding_count++] = {v.address, v.stride, 0, per_instance, v.count, step};
        }
        bindings[b].records = std::min(bindings[b].records, v.count);
        const u32 offset = static_cast<u32>(v.address - bindings[b].base);
        bindings[b].end = std::max(bindings[b].end, offset + bytes);
        draw.attributes[draw.attribute_count++] = {attribute.location, format, offset, b};
    }
    // Fase 77: malha que comeca longe do inicio do buffer (o mar de pecas do Prime Time, 12 Foundation Prime: 5 draws
    // com vertice base de 3,7 a 4 milhoes,
    // 32 bytes por vertice): os buffers por vertice passam a comecar no primeiro vertice usado. Antes a copia ia do
    // inicio do buffer (~120 MB por draw), passava dos limites e o draw sumia. Abaixo disso nada muda: as submalhas
    // de um buffer continuam na mesma entrada do cache.
    if (const u32 rebase = draw.first_vertex; rebase >= 262'144) {
        for (u32 b = 0; b < draw.binding_count; ++b) {
            if (!bindings[b].per_instance) {
                bindings[b].base += static_cast<u64>(rebase) * bindings[b].stride;
                bindings[b].records -= std::min(bindings[b].records, rebase);
            }
        }
        vertex_count -= rebase;
        draw.base_vertex -= static_cast<s32>(rebase);
        draw.first_vertex = 0;
        if (++stats.rebased <= 8) {
            std::printf("[REBASE] VS %016llx: %u indices, buffers rebased to vertex %u\n", static_cast<unsigned long long>(info->vs_hash),
                        count, rebase);
        }
    }
    size_t arena_bytes = 0;
    u64 binding_bytes[8], binding_needed[8];
    for (u32 b = 0; b < draw.binding_count; ++b) {
        if (bindings[b].step > 1) {
            // Fase 44: copia dos elementos + versao expandida (um elemento por instancia) no arena
            const u64 source = static_cast<u64>((instance_count + bindings[b].step - 1) / bindings[b].step - 1) * bindings[b].stride + bindings[b].end;
            const u64 expanded = static_cast<u64>(instance_count - 1) * bindings[b].stride + bindings[b].end;
            binding_bytes[b] = source;
            binding_needed[b] = expanded;
            arena_bytes += ((source + 15) & ~u64{15}) + ((expanded + 15) & ~u64{15});
            continue;
        }
        // O buffer inteiro, como o V# o declara: submalhas diferentes do mesmo buffer caem na
        // mesma entrada do cache (com "ate o maior indice deste draw" cada uma virava uma copia).
        // Buffers enormes ou com contagem menor que a necessaria ficam com o trecho do draw.
        const u32 needed = bindings[b].per_instance ? instance_count : vertex_count;
        const u64 whole = static_cast<u64>(bindings[b].records - 1) * bindings[b].stride + bindings[b].end;
        const u32 elements = bindings[b].records >= needed && whole <= (48u << 20) ? bindings[b].records : needed;
        binding_bytes[b] = static_cast<u64>(elements - 1) * bindings[b].stride + bindings[b].end;
        binding_needed[b] = static_cast<u64>(needed - 1) * bindings[b].stride + bindings[b].end;
        arena_bytes += (binding_bytes[b] + 15) & ~size_t{15};
    }
    if (arena_bytes > (128u << 20)) {
        ++stats.size;
        return;
    }
    if (vertex_arena.size() < arena_bytes) {
        vertex_arena.resize(arena_bytes);
    }
    size_t arena_used = 0;
    for (u32 b = 0; b < draw.binding_count; ++b) {
        if (bindings[b].step > 1) {
            // Fase 44: o Vulkan le um elemento por instancia; o elemento k vale para as instancias
            // k * passo .. (k + 1) * passo - 1, entao cada um e repetido `passo` vezes.
            u8* source = vertex_arena.data() + arena_used;
            arena_used += (binding_bytes[b] + 15) & ~u64{15};
            u8* expanded = vertex_arena.data() + arena_used;
            arena_used += (binding_needed[b] + 15) & ~u64{15};
            if (!SafeCopy(source, bindings[b].base, binding_bytes[b])) {
                ++stats.unreadable;
                return;
            }
            const u32 stride = bindings[b].stride, step = bindings[b].step;
            for (u32 i = 0; i < instance_count; ++i) {
                const u64 bytes = i + 1 < instance_count ? stride : bindings[b].end;
                std::memcpy(expanded + static_cast<u64>(i) * stride, source + static_cast<u64>(i / step) * stride, bytes);
            }
            draw.bindings[b] = {expanded, stride, binding_needed[b], ~0ull, true};
            ++stats.instanced_expanded;
            static std::set<std::pair<u64, u32>> logged; // Fase 44: o que sao esses draws
            if (logged.size() < 24 && logged.insert({info->ps_hash, step}).second) {
                std::printf("[INSTANCES] VS %016llx PS %016llx: %u instances, step %u, %u indices/vertices, target 0x%llx\n",
                            static_cast<unsigned long long>(info->vs_hash), static_cast<unsigned long long>(info->ps_hash), instance_count, step,
                            count, static_cast<unsigned long long>(draw.color_count ? draw.colors[0].address : 0));
            }
            continue;
        }
        u64 static_offset = ~0ull;
        const u8* data = nullptr;
        CachedGeometry* entry = nullptr;
        u64 used = 0;
        const u64 skip = bindings[b].per_instance ? 0 : static_cast<u64>(draw.first_vertex) * bindings[b].stride;
        if (!ResolveGeometry(bindings[b].base, binding_bytes[b], binding_needed[b], skip, vertex_arena.data() + arena_used, &static_offset,
                             &data, &used, &entry)) {
            ++stats.unreadable;
            return;
        }
        draw.bindings[b] = {data, bindings[b].stride, used, static_offset, bindings[b].per_instance};
        arena_used += (binding_bytes[b] + 15) & ~size_t{15};
    }

    LD_SUB(2, sub_geometry);
    // ---- buffers de constantes, texturas e samplers, cada um lido da origem que o par indica ----
    u64 sub_images = 0;
    const u64 sub_resources = Ticks();
    static thread_local std::vector<u8> arena(2u << 20);
    size_t used = 0;
    bool texture_pending = false; // Fase 35: alguma textura do draw ainda esta sendo lida
    bool dbg_lut = false; // DIAGNOSTICO (LD_DBG_LUT=1): draws que amostram a tabela 256x128 RGBA16F das luzes
    static const bool dbg_lut_enabled = std::getenv("LD_DBG_LUT") != nullptr;
    std::string dbg_samplers;
    for (const auto& r : info->resources) {
        const u32* ud = stage_ud[r.stage];
        const u32 dwords = r.kind == render::GamePairLayout::kImage ? 8 : 4;
        u32 words[8] = {};
        bool have = false;
        if (r.source == 1 && r.reg + dwords <= 16) {
            std::memcpy(words, ud + r.reg, dwords * 4);
            have = true;
        } else if (r.source == 2) {
            have = SafeCopy(words, pointer_of(ud, r.reg) + r.dword * 4ull, dwords * 4);
        }
        if (r.kind == render::GamePairLayout::kBuffer) {
            render::GameDraw::Buffer buffer{r.binding, nullptr, 0};
            const u64 address = words[0] | (static_cast<u64>(words[1] & 0xFFF) << 32);
            const u32 stride = (words[1] >> 16) & 0x3FFF;
            const u64 bytes = stride ? static_cast<u64>(stride) * words[2] : words[2];
            if (r.source == 3) {
                // Fase 32: buffer achatado = os 16 registradores de user data do estagio + o que o
                // walker copia seguindo os ponteiros deles.
                const u64 flat_bytes = (info->flat_dwords[r.stage] ? info->flat_dwords[r.stage] : 16) * 4ull;
                if (used + flat_bytes <= arena.size()) {
                    u32* flat = reinterpret_cast<u32*>(arena.data() + used);
                    std::memset(flat, 0, flat_bytes);
                    std::memcpy(flat, ud, 64);
                    if (info->walker[r.stage] && !runtime::CallGuardedWalker(info->walker[r.stage], ud, flat)) {
                        ++stats.game_zero_buffers; // ponteiro invalido no meio do caminho: fica o que deu para copiar
                    }
                    buffer.data = arena.data() + used;
                    buffer.bytes = static_cast<u32>(flat_bytes);
                    used += (flat_bytes + 15) & ~15ull;
                    ++stats.flatbuf_built;
                }
            } else if (have && address >= 0x10000 && bytes && bytes <= 262144 && used + bytes <= arena.size() &&
                SafeCopy(arena.data() + used, address, bytes)) {
                buffer.data = arena.data() + used;
                buffer.bytes = static_cast<u32>(bytes);
                used += (bytes + 15) & ~15ull;
                if (g_dbg_cb_record) { // DIAGNOSTICO Fase 60 (LD_DBG_CB=1): o jogo muda as constantes depois da copia?
                    DbgCbRecord(address, static_cast<u32>(bytes), arena.data() + used - ((bytes + 15) & ~15ull), info->vs_hash, info->ps_hash,
                                r.stage, r.binding, r.source);
                }
                {   // DIAGNOSTICO Fase 59 (LD_COPY_STATS=1): bytes de constantes copiados por frame e quantos sao repetidos
                    static const bool copy_stats = std::getenv("LD_COPY_STATS") != nullptr;
                    if (copy_stats) {
                        static std::unordered_set<u64> seen;
                        static u64 frame = ~0ull, total = 0, unique = 0, count = 0, frames = 0;
                        if (frame != g_texture_frame) {
                            frame = g_texture_frame;
                            seen.clear();
                            if (++frames % 300 == 0) {
                                static u64 last_ring = 0;
                                std::printf("[COPIES] per frame: %.0f constant buffers, %.0f KB copied, %.0f KB unique | geometry via ring %.0f KB\n",
                                            count / 300.0, total / 300.0 / 1024.0, unique / 300.0 / 1024.0,
                                            (g_vk_stats.ring_bytes - last_ring) / 300.0 / 1024.0);
                                last_ring = g_vk_stats.ring_bytes;
                                total = unique = count = 0;
                            }
                        }
                        ++count;
                        total += bytes;
                        if (seen.insert(address ^ (bytes << 48)).second) {
                            unique += bytes;
                        }
                    }
                }
            } else if (r.source != 0) {
                ++stats.game_zero_buffers;
            }
            {
                static const u64 dbg_vs = std::getenv("LD_DBG_VS") ? std::strtoull(std::getenv("LD_DBG_VS"), nullptr, 16) : 0;
                static u32 dbg_buffers = 0;
                if (dbg_vs && dbg_buffers < 60 && ContentHashOfShader(0, vs_address) == dbg_vs) {
                    ++dbg_buffers;
                    float f[8] = {};
                    if (buffer.data) {
                        std::memcpy(f, buffer.data, std::min<size_t>(sizeof(f), buffer.bytes));
                    }
                    std::printf("[DBG-BUF] stage %u binding %u source %u reg %u dword %u | V# %08x %08x %08x %08x -> address 0x%llx stride %u bytes %llu "
                                "read %d | %g %g %g %g %g %g %g %g\n",
                                r.stage, r.binding, r.source, r.reg, r.dword, words[0], words[1], words[2], words[3],
                                static_cast<unsigned long long>(address), stride, static_cast<unsigned long long>(bytes), buffer.data != nullptr,
                                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                    if (buffer.data && r.stage == 1) {
                        float more[32] = {};
                        std::memcpy(more, buffer.data, std::min<size_t>(sizeof(more), buffer.bytes));
                        std::printf("[DBG-BUF+] binding %u:", r.binding);
                        for (u32 k = 8; k < 32; ++k) {
                            std::printf(" [%u]=%g", k, more[k]);
                        }
                        std::printf("\n");
                    }
                }
            }
            draw.buffers[draw.buffer_count++] = buffer;
        } else if (r.kind == render::GamePairLayout::kImage) {
            struct ImageTime { // Fase 59: soma o tempo deste ramo em sub_images (ciclos)
                u64 start;
                u64* total;
                ~ImageTime() { *total += Ticks() - start; }
            } image_time{Ticks(), &sub_images};
            render::GameDraw::Image image{r.binding, 0, 0, r.view_type};
            const u32 type = words[3] >> 28;
            const u64 address = (static_cast<u64>(words[0]) << 8) | (static_cast<u64>(words[1] & 0x3F) << 40);
            GuestTexture texture;
            if (have && ((type == 0x9 && (r.view_type == 0 || r.view_type == 2)) || (type == 0xA && r.view_type == 3) || // 2D como array: Fase 32
                         (type == 0xB && r.view_type <= 2))) { // cubo: como cubo, como array 2D ou (face 0) como 2D
                const bool is_target = type == 0x9 && renderer.HasRenderTarget(address);
                if (is_target || (type == 0x9 && ((words[3] >> 20) & 0x1F) == 14)) {
                    NoteTargetHint(address, (words[2] & 0x3FFF) + 1, ((words[2] >> 14) & 0x3FFF) + 1);
                }
                if (g_target_diagnostics && (is_target || ((words[3] >> 20) & 0x1F) == 14)) {
                    static std::set<std::tuple<u64, u32, u64>> logged;
                    const u64 into = draw.color_count ? draw.colors[0].address : draw.depth.address;
                    if (logged.size() < 4000 && logged.insert(std::make_tuple(address, words[2], into)).second) {
                        std::printf("[TARGET] sample 0x%llx T# %ux%u pitch %u tiling %u (known target: %d) sampler-binding ? | drawing to "
                                    "0x%llx %ux%u\n",
                                    static_cast<unsigned long long>(address), (words[2] & 0x3FFF) + 1, ((words[2] >> 14) & 0x3FFF) + 1,
                                    ((words[4] >> 13) & 0x3FFF) + 1, (words[3] >> 20) & 0x1F, is_target,
                                    static_cast<unsigned long long>(into), draw.color_count ? draw.colors[0].width : draw.depth.width,
                                    draw.color_count ? draw.colors[0].height : draw.depth.height);
                    }
                }
                if (g_target_diagnostics && is_target) { // --target-log: qual par amostra qual alvo, desenhando onde
                    static std::set<std::tuple<u64, u64, u64>> logged;
                    const u64 into = draw.color_count ? draw.colors[0].address : draw.depth.address;
                    if (logged.size() < 4000 && logged.insert(std::make_tuple(info->key, address, into)).second) {
                        std::printf("[PASS] VS %016llx PS %016llx sample 0x%llx (T# format 0x%x type %u swizzle %03x, binding %u) -> draw to 0x%llx\n",
                                    static_cast<unsigned long long>(ContentHashOfShader(0, vs_address)),
                                    static_cast<unsigned long long>(ContentHashOfShader(1, ps_address)),
                                    static_cast<unsigned long long>(address), (words[1] >> 20) & 0x3F, (words[1] >> 26) & 0xF, words[3] & 0xFFF, r.binding,
                                    static_cast<unsigned long long>(into));
                    }
                }
                if (is_target) {
                    image.target_address = address; // a textura e um alvo de render do jogo: usa a imagem dele
                    ++stats.target_sampled;
                } else if (ReadTsharp(words, &texture)) {
                    image.texture_key = EnsureGuestTexture(texture);
                    if (image.texture_key == kTexturePending && g_tex_wait_frame) {
                        image.texture_key = g_tex_pending_key; // Fase 45: o draw vai com a textura pendente
                        image.pending = true;
                        g_tex_frame_waiting.insert(g_tex_pending_key);
                    } else if (image.texture_key == kTexturePending) {
                        image.texture_key = 0;
                        texture_pending = true;
                    }
                    dbg_lut |= texture.format == 0x0C && texture.width == 256 && texture.height == 128;
                }
            }
            if (!image.target_address && !image.texture_key && !texture_pending) {
                ++stats.game_white_images;
                const bool readable = have && ReadTsharp(words, &texture);
                static std::set<u64> logged; // uma linha por descritor distinto que o backend nao sabe enviar
                const u64 id = (static_cast<u64>(words[1] >> 20) << 32) ^ (words[3] >> 20) ^ (static_cast<u64>(words[2]) << 20);
                if (have && type && logged.size() < 40 && logged.insert(id).second) {
                    std::printf("[VULKAN] unsupported texture: type 0x%x format 0x%x tiling %u %ux%u levels %u pitch %u address 0x%llx | "
                                "shader declares view %u; descriptor %s\n",
                                type, (words[1] >> 20) & 0x3F, (words[3] >> 20) & 0x1F, (words[2] & 0x3FFF) + 1,
                                ((words[2] >> 14) & 0x3FFF) + 1, ((words[3] >> 16) & 0xF) + 1, ((words[4] >> 13) & 0x3FFF) + 1,
                                static_cast<unsigned long long>(address), r.view_type,
                                readable ? "accepted (different view type or upload failed)" : "refused by T# reader");
                }
            }
            draw.images[draw.image_count++] = image;
        } else {
            // S#: clamp_x bits 0..2, clamp_y bits 3..5 (0 = repete, 1 = espelha, demais = borda);
            // funcao de comparacao de profundidade nos bits 12..14; filtro de ampliacao nos bits
            // 20..21 do dword 2 (0 = ponto).
            // Modos de endereco do GNM: 0 repete, 1 espelha, 2 / 3 prendem no ultimo texel,
            // 4..7 usam a cor de borda (tipo nos bits 30..31 do dword 3).
            auto mode = [](u32 clamp) { return clamp == 0 ? 0u : clamp == 1 ? 1u : clamp <= 3 ? 2u : 3u; };
            const u32 cx = words[0] & 7, cy = (words[0] >> 3) & 7;
            if (g_target_diagnostics) { // --target-log: uma linha por S# distinto
                static std::set<std::tuple<u32, u32, u32, u32, bool>> logged;
                if (logged.size() < 60 && logged.insert(std::make_tuple(words[0], words[1], words[2], words[3], have)).second) {
                    std::printf("[SAMPLER] S# %08x %08x %08x %08x read=%d source=%u reg=%u dword=%u stage=%u -> clamp x %u y %u\n", words[0],
                                words[1], words[2], words[3], have, r.source, r.reg, r.dword, r.stage, cx, cy);
                }
            }
            if (dbg_lut_enabled) {
                char text[160];
                std::snprintf(text, sizeof(text), " | S# binding %u stage %u source %u reg %u dword %u read %d: %08x %08x %08x %08x", r.binding,
                              r.stage, r.source, r.reg, r.dword, have, words[0], words[1], words[2], words[3]);
                dbg_samplers += text;
            }
            draw.samplers[draw.sampler_count++] = {r.binding, have ? mode(cx) : 2u, have ? mode(cy) : 2u, have ? words[3] >> 30 : 0u,
                                                   have && ((words[2] >> 20) & 3) == 0, have ? (words[0] >> 12) & 7 : 0,
                                                   have ? words[1] : 0u, have ? words[2] : 0u};
        }
    }
    g_sub_ns[4] += sub_images;
    g_sub_ns[3] += Ticks() - sub_resources - sub_images;
    if (texture_pending) {
        ++stats.texture_pending; // o draw volta quando a textura chegar (alguns frames)
        return;
    }
    if (dbg_lut_enabled && dbg_lut) {
        static std::set<u64> logged;
        if (logged.size() < 12 && logged.insert(info->key).second) {
            std::printf("[DBG-LUT] pair pair_%016llx_%016llx_%016llx samples 256x128 table%s\n", static_cast<unsigned long long>(info->vs_hash),
                        static_cast<unsigned long long>(info->ps_hash), static_cast<unsigned long long>(info->signature), dbg_samplers.c_str());
        }
    }
    // Push constants dos modulos: deslocamento / escala do viewport (0, 0, 1, 1), os registradores
    // de user data que cada estagio le direto, e os deslocamentos de buffer zerados (cada buffer
    // e vinculado ja no endereco exato).
    const float scale[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    std::memcpy(draw.push, scale, sizeof(scale));
    for (u32 stage = 0; stage < 2; ++stage) {
        u32 slot = info->first_ud[stage];
        for (u32 reg = 0; reg < 16; ++reg) {
            if (((info->ud_mask[stage] >> reg) & 1) && slot < 16) {
                std::memcpy(draw.push + 16 + slot++ * 4, &stage_ud[stage][reg], 4);
            }
        }
    }
    LD_LAP(2);
    const bool queued_ok = renderer.QueueGameDraw(draw);
    LD_LAP(3);
    if (queued_ok) {
        ++stats.game_sent;
    } else {
        ++stats.game_rejected;
    }
}

// Percorre um command buffer e executa os pacotes de sincronizacao. `release_event` e o id do
// evento disparado por RELEASE_MEM (0x40 no anel grafico, 0..6 nas filas de compute).
// ---- Fase 35: copias de memoria do DMA_DATA em paralelo ----
// Enquanto o jogo carrega texturas ele manda a GPU copiar ~400 MiB por frame, em pacotes de ate
// 2 MiB. No PS4 a GPU faz isso sem ocupar a CPU; aqui o memmove rodava na thread de render (70 ms
// por frame: a queda de quadros quando "aparecem objetos novos"). Os pacotes seguidos sao juntados
// e copiados por varias threads; tudo termina antes do proximo pacote que nao seja DMA, entao a
// ordem do stream de comandos continua valendo.
struct DmaJob {
    u8* dst;
    const u8* src; // nullptr = preenchimento com `pattern`
    u64 bytes;
    u32 pattern = 0;
};
u64 g_dma_fill_bytes = 0, g_dma_copy_bytes = 0;

// Preenche com um valor de 32 bits repetido. Antes era um memcpy de 4 bytes por vez: 2 MiB levavam
// 0,4 ms, e o jogo manda ~190 desses por frame enquanto carrega (limpeza de buffers).
__declspec(noinline) void DmaFill(u8* dst, u64 bytes, u32 pattern) {
    const u8 first = static_cast<u8>(pattern);
    if (pattern == first * 0x01010101u) {
        std::memset(dst, first, bytes);
        return;
    }
    u64 i = 0;
    if ((reinterpret_cast<u64>(dst) & 3) == 0) {
        std::fill_n(reinterpret_cast<u32*>(dst), bytes / 4, pattern);
        i = bytes & ~3ull;
    }
    for (; i < bytes; i += 4) {
        std::memcpy(dst + i, &pattern, bytes - i < 4 ? bytes - i : 4);
    }
}

inline void DmaRun(const DmaJob& job) {
    if (job.src) {
        std::memcpy(job.dst, job.src, job.bytes);
    } else {
        DmaFill(job.dst, job.bytes, job.pattern);
    }
}
std::vector<DmaJob> g_dma_pending;
u64 g_dma_parallel_bytes = 0, g_dma_batches = 0;
std::atomic<bool> g_dma_parallel{false}; // --parallel-dma (as copias sao pequenas; o custo estava na consulta de memoria)

// Fase 35: copia / preenchimento protegidos, SEM VirtualQuery. Cada DMA_DATA pagava duas consultas
// (destino gravavel, origem legivel) de ~0,3 ms: com ~190 pacotes por frame durante a carga de
// texturas, 60 ms de thread de render parada. Endereco invalido vira excecao tratada aqui.
__declspec(noinline) void DmaMoveRaw(u8* dst, const u8* src, u64 bytes) {
    std::memmove(dst, src, bytes);
}
__declspec(noinline) bool DmaMoveInner(u8* dst, const u8* src, u64 bytes) {
    __try {
        DmaMoveRaw(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool DmaMoveGuarded(u8* dst, const u8* src, u64 bytes) {
    if (reinterpret_cast<u64>(dst) < 0x10000 || reinterpret_cast<u64>(src) < 0x10000) {
        return false;
    }
    const runtime::GuardedReadScope guard;
    return DmaMoveInner(dst, src, bytes);
}
void DmaFill(u8* dst, u64 bytes, u32 pattern);
__declspec(noinline) bool DmaFillInner(u8* dst, u64 bytes, u32 pattern) {
    __try {
        DmaFill(dst, bytes, pattern);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool DmaFillGuarded(u8* dst, u64 bytes, u32 pattern) {
    if (reinterpret_cast<u64>(dst) < 0x10000) {
        return false;
    }
    const runtime::GuardedReadScope guard;
    return DmaFillInner(dst, bytes, pattern);
}

struct DmaPool {
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake, finished;
    const DmaJob* jobs = nullptr;
    size_t count = 0, active = 0;
    std::atomic<size_t> next{0};
    u64 generation = 0;

    void Work() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= count) {
                return;
            }
            DmaRun(jobs[i]);
        }
    }
    void Thread() {
        u64 seen = 0;
        for (;;) {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return generation != seen; });
            seen = generation;
            lock.unlock();
            Work();
            lock.lock();
            if (--active == 0) {
                finished.notify_one();
            }
        }
    }
    void Run(const DmaJob* list, size_t n) {
        if (threads.empty()) {
            const unsigned workers = std::clamp<unsigned>(std::thread::hardware_concurrency() / 2, 1, 5);
            for (unsigned i = 0; i < workers; ++i) {
                threads.emplace_back([this] { Thread(); });
                threads.back().detach();
            }
        }
        {
            std::lock_guard lock(mutex);
            jobs = list;
            count = n;
            next = 0;
            active = threads.size();
            ++generation;
        }
        wake.notify_all();
        Work(); // a thread de render tambem copia
        std::unique_lock lock(mutex);
        finished.wait(lock, [&] { return active == 0; });
    }
} g_dma_pool;

void FlushDma() {
    if (g_dma_pending.empty()) {
        return;
    }
    const render::HitchClock hitch(render::HitchClock::kDma);
    u64 bytes = 0;
    for (const DmaJob& job : g_dma_pending) {
        bytes += job.bytes;
    }
    if (g_dma_pending.size() < 2 || bytes < (1u << 20)) {
        for (const DmaJob& job : g_dma_pending) {
            DmaRun(job);
        }
    } else {
        g_dma_pool.Run(g_dma_pending.data(), g_dma_pending.size());
        g_dma_parallel_bytes += bytes;
        ++g_dma_batches;
    }
    g_dma_pending.clear();
}

// A faixa [a, a+na) cruza [b, b+nb)?
inline bool Overlaps(const u8* a, u64 na, const u8* b, u64 nb) {
    return a < b + nb && b < a + na;
}

void ProcessCommandBuffer(const u32* cmd, u64 size_bytes, u64 release_event) {
    struct FlushAtExit {
        ~FlushAtExit() {
            FlushDma();
        }
    } flush_at_exit;
    const u64 total = size_bytes / 4;
    u64 pos = 0;
    while (pos < total) {
        const u32 header = cmd[pos];
        const u32 type = header >> 30;
        if (type == 2) { // filler de 1 dword
            ++pos;
            continue;
        }
        const u32 count = (header >> 16) & 0x3FFF;
        if (type == 0) { // escrita de registradores: cabecalho + (count+1) valores
            pos += count + 2;
            continue;
        }
        if (type != 3 || pos + count + 2 > total) {
            ++g_malformed;
            return;
        }
        const u32 opcode = (header >> 8) & 0xFF;
        const u32* body = cmd + pos + 1;
        const u32 body_dwords = count + 1;
        ++g_packets;
        ++g_opcode_count[opcode];
        ++g_window_packets;
        if (opcode != kOpDmaData && !g_dma_pending.empty()) {
            FlushDma(); // qualquer outro pacote ve a memoria ja copiada
        }
        if (opcode == 0x2D || opcode == 0x27 || opcode == 0x35 || opcode == 0x25 || opcode == 0x24) {
            CaptureFetchShader();
            SubmitDrawToVulkan(opcode, body, body_dwords);
            ++g_frame_draws;
            ++g_window_draws; // DRAW_INDEX_AUTO, DRAW_INDEX_2, DRAW_INDEX_OFFSET_2, DRAW_INDEX_INDIRECT, DRAW_INDIRECT
        }

        switch (opcode) {
        case kOpEventWriteEop:
            if (body_dwords >= 5) {
                const u64 address = body[1] | (static_cast<u64>(body[2] & 0xFFFF) << 32);
                const u32 int_sel = (body[2] >> 24) & 0x3;
                const u32 data_sel = body[2] >> 29;
                WriteSelected("EVENT_WRITE_EOP", data_sel, address, body[3], body[4]);
                if (int_sel) {
                    SignalInterrupt(kEventGfxEop);
                }
            }
            break;
        case kOpEventWriteEos:
            if (body_dwords >= 4 && (body[2] >> 29) == 2) { // comando 2 = sinalizar fence
                const u64 address = body[1] | (static_cast<u64>(body[2] & 0xFFFF) << 32);
                WriteLabel("EVENT_WRITE_EOS", address, &body[3], 4);
            }
            break;
        case kOpReleaseMem:
            if (body_dwords >= 6) {
                const u64 address = body[2] | (static_cast<u64>(body[3]) << 32);
                const u32 int_sel = (body[1] >> 24) & 0x7;
                const u32 data_sel = body[1] >> 29;
                WriteSelected("RELEASE_MEM", data_sel, address, body[4], body[5]);
                if (int_sel) {
                    SignalInterrupt(release_event);
                }
            }
            break;
        case kOpWriteData:
            if (body_dwords > 3) {
                const u32 dst_sel = (body[0] >> 8) & 0xF;
                const u64 address = body[1] | (static_cast<u64>(body[2]) << 32);
                if (dst_sel == 1 || dst_sel == 5) { // memoria (sincrono / assincrono)
                    WriteLabel("WRITE_DATA", address, &body[3], (body_dwords - 3) * 4ULL);
                }
            }
            break;
        case kOpDmaData:
            if (body_dwords >= 6) {
                const u32 dst_sel = (body[0] >> 20) & 0x3;
                const u32 src_sel = (body[0] >> 29) & 0x3;
                const u64 dst = body[3] | (static_cast<u64>(body[4]) << 32);
                const u64 bytes = body[5] & 0x1FFFFF;
                // Bits SAS/DAS: 1 = o "endereco" e um registrador, nao memoria. O jogo usa isso
                // (controle 0x60000000, destino 0x3022c) como prefetch para a cache L2 da GPU:
                // le a memoria de origem e descarta. Nao ha nada a escrever.
                const bool src_is_register = (body[5] >> 26) & 1;
                const bool dst_is_register = (body[5] >> 27) & 1;
                const bool dst_memory = (dst_sel == 0 || dst_sel == 3) && !dst_is_register;
                if (!dst_memory || !bytes || (src_is_register && src_sel != 2)) {
                    ++g_dma_non_memory;
                    break;
                }
                if (src_sel == 2 && !g_dma_parallel.load(std::memory_order_relaxed)) { // dado imediato repetido
                    const render::HitchClock hitch(render::HitchClock::kDma);
                    if (DmaFillGuarded(reinterpret_cast<u8*>(dst), bytes, body[1])) {
                        g_dma_fill_bytes += bytes;
                        ++g_label_writes;
                    } else {
                        Refuse("DMA_DATA (immediate)", dst, bytes, body[0]);
                    }
                } else if ((src_sel == 0 || src_sel == 3) && !g_dma_parallel.load(std::memory_order_relaxed)) { // copia memoria -> memoria
                    const u64 src = body[1] | (static_cast<u64>(body[2]) << 32);
                    const render::HitchClock hitch(render::HitchClock::kDma);
                    if (DmaMoveGuarded(reinterpret_cast<u8*>(dst), reinterpret_cast<const u8*>(src), bytes)) {
                        g_dma_copy_bytes += bytes;
                        ++g_label_writes;
                    } else {
                        Refuse("DMA_DATA (copy)", dst, bytes, body[0]);
                    }
                } else if (src_sel == 2) { // caminho antigo, com consulta de memoria e lote em paralelo
                    if (Writable(dst, bytes)) {
                        u8* const to = reinterpret_cast<u8*>(dst);
                        bool conflict = false;
                        for (const DmaJob& job : g_dma_pending) {
                            conflict |= Overlaps(to, bytes, job.dst, job.bytes) || (job.src && Overlaps(to, bytes, job.src, job.bytes));
                        }
                        if (conflict) {
                            FlushDma();
                        }
                        if (!g_dma_parallel.load(std::memory_order_relaxed) || bytes < 65536) {
                            FlushDma();
                            const render::HitchClock hitch(render::HitchClock::kDma);
                            DmaFill(to, bytes, body[1]);
                        } else {
                            g_dma_pending.push_back({to, nullptr, bytes, body[1]});
                        }
                        g_dma_fill_bytes += bytes;
                        ++g_label_writes;
                    } else {
                        Refuse("DMA_DATA (immediate)", dst, bytes, body[0]);
                    }
                } else if (src_sel == 0 || src_sel == 3) { // copia memoria -> memoria
                    const u64 src = body[1] | (static_cast<u64>(body[2]) << 32);
                    if (Writable(dst, bytes) && Readable(src, bytes)) {
                        u8* const to = reinterpret_cast<u8*>(dst);
                        const u8* const from = reinterpret_cast<const u8*>(src);
                        bool conflict = false; // a ordem importa quando as faixas se cruzam
                        for (const DmaJob& job : g_dma_pending) {
                            conflict |= Overlaps(to, bytes, job.dst, job.bytes) || (job.src && Overlaps(to, bytes, job.src, job.bytes)) ||
                                        Overlaps(from, bytes, job.dst, job.bytes);
                        }
                        if (conflict) {
                            FlushDma();
                        }
                        if (!g_dma_parallel.load(std::memory_order_relaxed) || bytes < 65536 || Overlaps(to, bytes, from, bytes)) {
                            FlushDma();
                            const render::HitchClock hitch(render::HitchClock::kDma);
                            std::memmove(to, from, bytes);
                        } else {
                            g_dma_pending.push_back({to, from, bytes});
                        }
                        g_dma_copy_bytes += bytes;
                        ++g_label_writes;
                    } else {
                        Refuse("DMA_DATA (copy)", dst, bytes, body[0]);
                    }
                }
            }
            break;
        case 0x69: // SET_CONTEXT_REG: viewport, blend, mascara de cor...
            for (u32 i = 1; i < body_dwords; ++i) {
                const u32 reg = (body[0] & 0xFFFF) + i - 1;
                if (reg < g_ctx_track.size()) {
                    g_ctx_track[reg] = body[i];
                }
            }
            break;
        case 0x79: // SET_UCONFIG_REG: 0x242 = VGT_PRIMITIVE_TYPE
            if (body_dwords >= 2 && (body[0] & 0xFFFF) == 0x242) {
                g_prim_track = body[1];
            }
            break;
        case 0x2A: // INDEX_TYPE
            g_index_size_track = (body[0] & 1) ? 4 : 2;
            break;
        case 0x26: // INDEX_BASE
            if (body_dwords >= 2) {
                g_index_base_track = body[0] | (static_cast<u64>(body[1] & 0xFF) << 32);
            }
            break;
        case 0x76: // SET_SH_REG: so os registradores do vertex shader interessam aqui
            for (u32 i = 1; i < body_dwords; ++i) {
                const u32 reg = (body[0] & 0xFFFF) + i - 1;
                if (reg >= 0x40 && reg < 0x60) {
                    g_vs_track[reg - 0x40] = body[i];
                } else if (reg < 0x20) {
                    g_ps_track[reg] = body[i];
                }
            }
            break;
        case 0x2F: // NUM_INSTANCES
            g_num_instances = std::max(1u, body[0]);
            break;
        default: // NOP, WAIT_REG_MEM (nada a esperar: tudo e sincrono), draws, estado...
            break;
        }
        pos += count + 2;
    }
}

// Fase 69: tempo de todos os Submits (com e sem flip) desde o ultimo fechamento de intervalo ([HITCH-MIN])
std::atomic<u64> g_submit_all_ns{0};
double WaitRenderThreadIdle(); // Fase 76 (definida mais abaixo)
double g_render_wait_pending_ms = 0; // Fase 76: espera pela thread de render ainda nao somada a um intervalo

int Submit(u32 count, const u32* const* dcb_addrs, const u32* dcb_sizes, const u32* const* ccb_addrs,
           const u32* ccb_sizes) {
    static perf::Counter perf_counter("Gnm Submit (parser PM4)");
    const perf::Scope perf_scope(perf_counter);
    render::MarkRenderThread(); // Fase 69: o tempo desta thread e o que trava o quadro
    g_render_wait_pending_ms += WaitRenderThreadIdle(); // Fase 76: o quadro anterior termina antes de mexer no estado
    struct SubmitTimer {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~SubmitTimer() { g_submit_all_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(); }
    } submit_timer;
    const render::TimedLock lock(g_mutex);
    // Fase 28: as regioes de memoria consultadas valem pelo Submit inteiro. Sem isto cada shader
    // novo pagava dois VirtualQuery (~1 ms no total), e uma troca de cena traz mais de cem.
    const RegionCacheScope region_cache;
    DrainPairResults();
    ++g_submits;
    for (u32 i = 0; i < count; ++i) {
        // No hardware o CCB (constantes) e consumido junto com o DCB correspondente.
        if (ccb_addrs && ccb_sizes && ccb_addrs[i] && ccb_sizes[i] && Readable(reinterpret_cast<u64>(ccb_addrs[i]), ccb_sizes[i])) {
            ProcessCommandBuffer(ccb_addrs[i], ccb_sizes[i], kEventGfxEop);
        }
        if (dcb_addrs && dcb_sizes && dcb_addrs[i] && dcb_sizes[i] && Readable(reinterpret_cast<u64>(dcb_addrs[i]), dcb_sizes[i])) {
            ProcessCommandBuffer(dcb_addrs[i], dcb_sizes[i], kEventGfxEop);
            g_frame_buffers.push_back({dcb_addrs[i], dcb_sizes[i]});
        }
    }
    return err::OK;
}

// Preenche `size` dwords com um unico NOP. Retorna false se nao couber um pacote.
bool FillNop(u32* cmdbuf, u32 size) {
    if (!cmdbuf || !size) {
        return false;
    }
    if (size == 1) {
        cmdbuf[0] = 0x80000000; // filler tipo 2
        return true;
    }
    cmdbuf[0] = 0xC0000000u | ((size - 2) << 16) | (kOpNop << 8);
    std::memset(cmdbuf + 1, 0, (size - 1) * 4ULL);
    return true;
}

// ========================= extrator de shaders (Fase 12) ======================
// sceGnmSet*Shader / sceGnmUpdate*Shader recebem os registradores do estagio (o que o Gnmx
// guarda em VsStageRegisters, PsStageRegisters...). Os dois primeiros dwords sao
// SPI_SHADER_PGM_LO/HI: o endereco do codigo GCN dividido por 256. O codigo comeca com
// "mov vcc_hi, #imm" (0xBEEB03FF) e o imediato seguinte da a distancia ate o ShaderBinaryInfo
// ("OrbShdr"), que fica logo depois do codigo e das tabelas de uso de recursos:
//   info = codigo + (codigo[1] + 1) * 8 bytes;  info.length = bytes de codigo
// O arquivo gravado vai do inicio do codigo ate o fim do ShaderBinaryInfo (28 bytes), ou seja,
// codigo + tabelas de input usage + cabecalho: tudo que um recompilador precisa.
#pragma pack(push, 1)
struct ShaderBinaryInfo {
    char signature[7]; // "OrbShdr"
    u8 version;
    u32 flags_length; // bits 0-7: pssl/cached/tipo/origem; bits 8-31: bytes de codigo
    u8 chunk_usage_base_offset_dw;
    u8 num_input_usage_slots;
    u8 srt_flags;
    u8 reserved;
    u32 shader_hash0, shader_hash1;
    u32 crc32;
};
#pragma pack(pop)
static_assert(sizeof(ShaderBinaryInfo) == 28);

enum ShaderStage : u32 { kStageVs, kStagePs, kStageGs, kStageEs, kStageHs, kStageLs, kStageCs, kStageCount };
const char* const kStageFile[] = {"vs", "ps", "gs", "es", "hs", "ls", "cs"};
const char* const kStageName[] = {"VS", "PS", "GS", "ES", "HS", "LS", "CS"};

struct ShaderSeen {
    u32 info_offset; // distancia do codigo ate o ShaderBinaryInfo
    u32 code0, code1; // dois primeiros dwords do codigo
    u32 flags_length, crc32, hash0;
    u64 content_hash = 0; // hash do arquivo gravado
    s32 fetch_sgpr = -1;  // VS: registrador de user data com o ponteiro do fetch shader
};
std::mutex g_shader_mutex;
std::filesystem::path g_shader_dir;
std::unordered_map<u64, ShaderSeen> g_shader_at[kStageCount]; // endereco -> o que ja foi visto ali
std::set<u64> g_shader_hashes[kStageCount];
std::set<u64> g_shader_bad_addresses;
std::array<std::atomic<u64>, kStageCount> g_shader_calls{};
u64 g_shader_written = 0, g_shader_on_disk = 0, g_shader_bytes = 0, g_shader_not_found = 0;
// Fase 45: onde vai o tempo do caminho frio do extrator (ms): cabecalho, busca lenta, hash, resto
double g_extract_ms[4] = {};
u64 g_extract_slow = 0, g_extract_cold = 0, g_extract_dup = 0;

const ShaderBinaryInfo* FindShaderInfo(u64 address) {
    const u32* code = reinterpret_cast<const u32*>(address);
    auto valid = [](u64 where) {
        return Readable(where, sizeof(ShaderBinaryInfo)) && !std::memcmp(reinterpret_cast<const void*>(where), "OrbShdr", 7);
    };
    if (code[0] == 0xBEEB03FF && code[1] < 0x40000) {
        const u64 where = address + (static_cast<u64>(code[1]) + 1) * 8;
        if (valid(where)) {
            return reinterpret_cast<const ShaderBinaryInfo*>(where);
        }
    }
    // Caminho lento: procura a assinatura nos 64 KiB seguintes (dentro da mesma regiao).
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        return nullptr;
    }
    const u64 region_end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
    const u64 end = std::min(address + 0x10000, region_end - sizeof(ShaderBinaryInfo));
    for (u64 where = address; where < end; where += 4) {
        if (!std::memcmp(reinterpret_cast<const void*>(where), "OrbShdr", 7)) {
            return reinterpret_cast<const ShaderBinaryInfo*>(where);
        }
    }
    return nullptr;
}

// O shader visto antes neste endereco continua la? Sem VirtualQuery (caro neste processo,
// e isto roda milhares de vezes por frame): se a memoria tiver sido desmapeada, a leitura
// falha e a excecao e tratada aqui mesmo.
// (No Clang o __try so cobre excecoes vindas de CHAMADAS: a leitura fica numa funcao propria,
// nao embutida.)
__declspec(noinline) bool CompareShaderHeader(u64 address, const ShaderSeen& k) {
    const volatile u32* code = reinterpret_cast<const volatile u32*>(address);
    const auto* at = reinterpret_cast<const ShaderBinaryInfo*>(address + k.info_offset);
    return code[0] == k.code0 && code[1] == k.code1 && !std::memcmp(at->signature, "OrbShdr", 7) &&
           at->flags_length == k.flags_length && at->crc32 == k.crc32 && at->shader_hash0 == k.hash0;
}

// Fase 28: caminho rapido do shader novo, sem VirtualQuery (cada consulta custa centenas de
// microssegundos nas regioes grandes do jogo, e uma troca de cena traz mais de cem shaders). Le o
// cabecalho do Gnmx direto; se a memoria nao estiver la, a excecao e tratada pelo chamador.
__declspec(noinline) const ShaderBinaryInfo* ShaderInfoByHeader(u64 address) {
    const volatile u32* code = reinterpret_cast<const volatile u32*>(address);
    const u32 code0 = code[0], code1 = code[1];
    if (code0 != 0xBEEB03FF || code1 >= 0x40000) {
        return nullptr;
    }
    const auto* info = reinterpret_cast<const ShaderBinaryInfo*>(address + (static_cast<u64>(code1) + 1) * 8);
    volatile u8 last = reinterpret_cast<const volatile u8*>(info)[sizeof(ShaderBinaryInfo) - 1]; // a estrutura inteira esta mapeada
    (void)last;
    return std::memcmp(info->signature, "OrbShdr", 7) ? nullptr : info;
}

const ShaderBinaryInfo* ShaderInfoByHeaderGuarded(u64 address) {
    __try {
        return ShaderInfoByHeader(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SameShaderStillThere(u64 address, const ShaderSeen& k) {
    __try {
        return CompareShaderHeader(address, k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ExtractShader(ShaderStage stage, const u32* regs) {
    static perf::Counter perf_counter("shader extractor");
    const perf::Scope perf_scope(perf_counter);
    ++g_shader_calls[stage];
    if (!regs) {
        return; // ponteiro nulo = estagio desligado
    }
    const u64 address = (static_cast<u64>(regs[0]) << 8) | (static_cast<u64>(regs[1] & 0xFF) << 40);
    std::lock_guard lock(g_shader_mutex);
    if (g_shader_dir.empty() || !address) {
        return;
    }
    // Caminho quente (milhares de chamadas por frame): endereco ja visto. Uma unica consulta
    // de memoria, e o cabecalho e conferido no lugar onde estava - o mesmo endereco pode
    // receber outro shader depois de um descarregamento.
    const auto known = g_shader_at[stage].find(address);
    if (known != g_shader_at[stage].end()) {
        const runtime::GuardedReadScope guard;
        if (SameShaderStillThere(address, known->second)) {
            return;
        }
    }
    const render::HitchClock hitch(render::HitchClock::kExtract); // caminho frio: shader num endereco novo
    if (g_shader_bad_addresses.count(address)) {
        return;
    }
    ++g_extract_cold;
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](int slot) {
        const auto now = std::chrono::steady_clock::now();
        g_extract_ms[slot] += std::chrono::duration<double, std::milli>(now - t0).count();
        t0 = now;
    };
    struct LapAtEnd {
        decltype(lap)& f;
        ~LapAtEnd() { f(3); }
    } lap_at_end{lap};
    const ShaderBinaryInfo* info = nullptr;
    {
        const runtime::GuardedReadScope guard;
        info = ShaderInfoByHeaderGuarded(address);
    }
    lap(0);
    if (!info) { // formato fora do comum: caminho antigo, com as consultas de memoria
        ++g_extract_slow;
        if (!Readable(address, 8)) {
            return;
        }
        info = FindShaderInfo(address);
        lap(1);
    }
    if (!info) {
        g_shader_bad_addresses.insert(address);
        ++g_shader_not_found;
        if (g_shader_not_found <= 8) {
            const u32* code = reinterpret_cast<const u32*>(address);
            log::Tagged("SHADER EXTRACT", "%s em 0x%" PRIx64 ": 'OrbShdr' not found (code starts with %08x %08x)", kStageName[stage],
                        address, code[0], code[1]);
        }
        return;
    }
    const u64 bytes = reinterpret_cast<u64>(info) + sizeof(ShaderBinaryInfo) - address;
    const u32 code_bytes = info->flags_length >> 8;
    u64 hash = 0xcbf29ce484222325ull; // FNV-1a de 64 bits do conteudo gravado
    for (u64 i = 0; i < bytes; ++i) {
        hash = (hash ^ reinterpret_cast<const u8*>(address)[i]) * 0x100000001b3ull;
    }
    lap(2);
    {
        const u32* code = reinterpret_cast<const u32*>(address);
        ShaderSeen seen{static_cast<u32>(reinterpret_cast<u64>(info) - address), code[0], code[1],
                        info->flags_length, info->crc32, info->shader_hash0, hash, -1};
        // O vertex shader chama o fetch shader com "s_swappc_b64 s[0:1], s[N:N+1]" (SOP1, op 33):
        // N e o registrador de user data que guarda o ponteiro do fetch shader.
        for (u32 i = 0; stage == kStageVs && i < code_bytes / 4 && i < 64; ++i) {
            if ((code[i] & 0xFF80FF00) == 0xBE802100 && (code[i] & 0xFF) < 15) {
                seen.fetch_sgpr = static_cast<s32>(code[i] & 0xFF);
                break;
            }
        }
        g_shader_at[stage][address] = seen;
    }
    if (!g_shader_hashes[stage].insert(hash).second) {
        ++g_extract_dup;
        return;
    }
    char name[48];
    std::snprintf(name, sizeof(name), "%s_%016llx.bin", kStageFile[stage], static_cast<unsigned long long>(hash));
    // Fase 28: a pasta e listada UMA vez; antes cada shader novo custava um create_directories, um
    // exists e uma linha de log, tudo na thread de render, bem na troca de cena.
    static std::unordered_set<std::string> on_disk;
    static bool listed = false;
    std::error_code ec;
    if (!listed) {
        listed = true;
        std::filesystem::create_directories(g_shader_dir, ec);
        for (std::filesystem::directory_iterator it(g_shader_dir, ec), end; !ec && it != end; it.increment(ec)) {
            on_disk.insert(it->path().filename().string());
        }
    }
    const std::filesystem::path path = g_shader_dir / name;
    const bool existed = on_disk.count(name) != 0;
    if (existed) {
        ++g_shader_on_disk;
        return; // ja extraido numa execucao anterior: nada a gravar nem a anunciar
    }
    // Fase 45: copia o codigo agora (a memoria do jogo pode mudar) e grava em segundo plano
    FileWrite job;
    job.path = path;
    job.data.assign(reinterpret_cast<const char*>(address), bytes);
    job.log_line = Format("Dumped %s: %016llx (Size: %llu bytes; code %u bytes)", kStageName[stage],
                          static_cast<unsigned long long>(hash), static_cast<unsigned long long>(bytes), code_bytes);
    WriteFileAsync(std::move(job));
    on_disk.insert(name);
    ++g_shader_written;
    g_shader_bytes += bytes;
    // indice: uma linha por shader, com os registradores do estagio (RSRC1/RSRC2 etc.)
    std::string line = Format("%s\t%016llx\t%llu\t%u\t%08x%08x\t%08x\t0x%" PRIx64 "\t", kStageFile[stage],
                              static_cast<unsigned long long>(hash), static_cast<unsigned long long>(bytes), code_bytes, info->shader_hash1,
                              info->shader_hash0, info->crc32, address);
    // Fase 45: leitura protegida em vez de Readable (VirtualQuery: ~1,8 ms por shader novo neste processo)
    const u32 reg_count = stage == kStagePs ? 12 : 7;
    u32 reg_values[12];
    const bool regs_readable = SafeCopy(reg_values, reinterpret_cast<u64>(regs), reg_count * 4ull);
    for (u32 i = 0; i < reg_count && regs_readable; ++i) {
        line += Format("%s%08x", i ? " " : "", reg_values[i]);
    }
    line += "\n";
    FileWrite index;
    index.path = g_shader_dir / "index.tsv";
    index.data = std::move(line);
    index.append = true;
    WriteFileAsync(std::move(index));
}

// Fetch shader: trecho de codigo GCN gerado pelo Gnmx que carrega os atributos de vertice e
// volta para o vertex shader com s_setpc_b64 (SOP1, op 32). Nao tem OrbShdr; o tamanho e ate
// o s_setpc. Um mesmo VS pode ser usado com varios fetch shaders (layouts de vertice).
std::set<std::pair<u64, u64>> g_fetch_pairs; // (hash do VS, hash do fetch shader)
std::set<u64> g_fetch_hashes;
struct AddressPairHash {
    size_t operator()(const std::pair<u64, u64>& p) const noexcept {
        return std::hash<u64>()(p.first * 0x9E3779B97F4A7C15ull ^ p.second);
    }
};
std::unordered_set<std::pair<u64, u64>, AddressPairHash> g_fetch_address_pairs; // (endereco do VS, ponteiro do fetch)
u64 g_fetch_last_vs = 0, g_fetch_last_pointer = 0, g_fetch_written = 0, g_fetch_bad = 0, g_vs_without_fetch = 0;

u64 ContentHashOfShader(u32 stage_index, u64 address) {
    std::lock_guard lock(g_shader_mutex);
    const auto known = g_shader_at[stage_index].find(address);
    return known == g_shader_at[stage_index].end() ? 0 : known->second.content_hash;
}

// Quais modulos do jogo desenham a interface: pares (VS, PS) vistos nos draws enviados ao
// Vulkan, com a contagem de draws. Gravado em ui_shader_pairs.tsv no fim da execucao.
std::map<std::tuple<u64, u64, u64>, u64> g_ui_shader_pairs; // (endereco do VS, endereco do PS, assinatura) -> draws
u64 g_ui_pair_last_vs = 0, g_ui_pair_last_ps = 0, g_ui_pair_last_signature = 0;
u64* g_ui_pair_last_count = nullptr;

// Retrato do user data do par no primeiro draw (Fase 19): os 16 registradores de cada estagio e
// os 192 primeiros dwords de cada tabela apontada por eles (eram 64: materiais de cenario tem T#
// ate o dword 120, e a ferramenta lia lixo no lugar deles). O recompilador le os descritores V# /
// T# durante a traducao (formato e swizzle dos atributos de vertice, tipo da textura), entao a
// ferramenta precisa dos descritores REAIS para gerar o SPIR-V que vai para o pipeline.
struct PairSnapshot {
    std::array<u32, 16> ud[2]{};                 // 0 = VS, 1 = PS
    std::map<u32, std::array<u32, 192>> tables[2]; // registrador do ponteiro -> conteudo
    s32 fetch_sgpr = -1;
    std::vector<u32> fetch_code; // o fetch shader desta variante, ate o s_setpc
};
std::map<std::tuple<u64, u64, u64>, PairSnapshot> g_ui_pair_snapshots; // (VS, PS, assinatura)

void TakePairSnapshot(u64 vs_address, u64 ps_address, u64 signature, s32 fetch_sgpr, const std::vector<u32>* fetch_code) {
    PairSnapshot& snapshot = g_ui_pair_snapshots[{vs_address, ps_address, signature}];
    snapshot.fetch_sgpr = fetch_sgpr;
    if (fetch_code) {
        snapshot.fetch_code = *fetch_code;
    }
    const u32* sources[2] = {&g_vs_track[0x0C], &g_ps_track[0x0C]};
    for (u32 stage = 0; stage < 2; ++stage) {
        std::memcpy(snapshot.ud[stage].data(), sources[stage], 64);
        for (u32 i = 0; i + 1 < 16; ++i) {
            const u32* ud = sources[stage];
            const u64 pointer = ud[i] | (static_cast<u64>(ud[i + 1]) << 32);
            std::array<u32, 192> table{};
            if (ud[i + 1] < 0x100 && pointer >= 0x10000 && !(pointer & 3)) {
                // Fase 59: valor que parece ponteiro mas nao aponta para memoria: uma excecao so (antes eram
                // 12, uma por tamanho tentado, ~9 ms por frame na entrada de uma area nova)
                if (!SafeCopy(table.data(), pointer, 64)) {
                    continue;
                }
                // a tabela pode acabar antes de 192 dwords, no fim de uma pagina: copia o que der
                for (size_t bytes = sizeof(table); bytes >= 64; bytes -= 64) {
                    if (SafeCopy(table.data(), pointer, bytes)) {
                        snapshot.tables[stage][i] = table;
                        break;
                    }
                }
            }
        }
    }
}

std::string SnapshotText(const PairSnapshot& snapshot) {
    std::string text;
    char word[16];
    if (snapshot.fetch_sgpr >= 0 && !snapshot.fetch_code.empty()) {
        text += "fetch " + std::to_string(snapshot.fetch_sgpr);
        for (u32 value : snapshot.fetch_code) {
            std::snprintf(word, sizeof(word), " %08x", value);
            text += word;
        }
        text += "\n";
    }
    for (u32 stage = 0; stage < 2; ++stage) {
        const char* stage_name = stage == 0 ? "vs" : "ps";
        text += std::string(stage_name) + " ud";
        for (u32 value : snapshot.ud[stage]) {
            std::snprintf(word, sizeof(word), " %08x", value);
            text += word;
        }
        text += "\n";
        for (const auto& [reg, table] : snapshot.tables[stage]) {
            text += std::string(stage_name) + " tab " + std::to_string(reg);
            for (u32 value : table) {
                std::snprintf(word, sizeof(word), " %08x", value);
                text += word;
            }
            text += "\n";
        }
    }
    return text;
}

// Fase 29: par sem SPIR-V em disco -> retrato do primeiro draw para o recompilador ao vivo.
void QueueLiveSnapshot(u64 vs_address, u64 ps_address, u64 signature, GamePairInfo* info) {
    const auto snapshot = g_ui_pair_snapshots.find({vs_address, ps_address, signature});
    if (snapshot == g_ui_pair_snapshots.end()) {
        return;
    }
    char name[80];
    std::snprintf(name, sizeof(name), "%016llx %016llx %016llx", static_cast<unsigned long long>(info->vs_hash),
                  static_cast<unsigned long long>(info->ps_hash), static_cast<unsigned long long>(info->signature));
    LiveSnapshot live;
    live.name = name;
    live.text = SnapshotText(snapshot->second);
    live.job = MakePairJob(*info, info, true);
    info->pending = true;
    info->why = "being recompiled in background";
    ++g_pairs_live_requested;
    QueuePairJob({}); // garante a thread; o trabalho vazio e descartado abaixo
    std::lock_guard lock(g_pair_mutex);
    g_live_snapshots.push_back(std::move(live));
}

// Fase 56: par que nao existe (nem em disco nem no pacote AOT, ou o pacote errou o tipo de textura):
// traduz AGORA, na thread de render, e o draw entra neste mesmo frame. Antes ia para segundo plano e o
// objeto ficava de fora por 0,6 a 0,8 s (retrato -> lote de 400 ms -> ferramenta -> carga -> proximo
// Submit). Orcamento por frame: passou, volta ao segundo plano.
double g_live_sync_budget_ms = 400.0; // --live-sync-budget (0 = so em segundo plano)
double g_live_sync_frame_ms = 0;      // so a thread de render
u64 g_live_sync_pairs = 0;

bool LiveCompileNow(u64 vs_address, u64 ps_address, u64 signature, GamePairInfo* info) {
    if (g_live_tool.empty() || g_live_sync_frame_ms >= g_live_sync_budget_ms) {
        return false;
    }
    const auto snapshot = g_ui_pair_snapshots.find({vs_address, ps_address, signature});
    if (snapshot == g_ui_pair_snapshots.end()) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    char name[80];
    std::snprintf(name, sizeof(name), "%016llx %016llx %016llx", static_cast<unsigned long long>(info->vs_hash),
                  static_cast<unsigned long long>(info->ps_hash), static_cast<unsigned long long>(info->signature));
    LiveSnapshot live;
    live.name = name;
    live.text = SnapshotText(snapshot->second);
    live.job = MakePairJob(*info, info, true);
    ++g_pairs_live_requested;
    {
        const render::HitchClock hitch(render::HitchClock::kLive); // Fase 69: antes nao caia em categoria nenhuma
        RunLiveCompiler({live});
    }
    GamePairInfo result;
    result.canon_signature = live.job.canon;
    result.vformats = live.job.vformats;
    result.vformat_count = live.job.vformat_count;
    LoadGamePair(live.job.vs_hash, live.job.ps_hash, live.job.signature, live.job.cntl.data(), &result);
    *info = std::move(result);
    info->vs_hash = live.job.vs_hash;
    info->ps_hash = live.job.ps_hash;
    info->signature = live.job.signature;
    info->cntl = live.job.cntl;
    info->pending = false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g_live_sync_frame_ms += ms;
    if (info->usable) {
        SaveWarmPair(*info);
        ++g_pairs_live_ok;
        ++g_live_sync_pairs;
        static u32 logged = 0;
        if (++logged <= 40) {
            log::Tagged("SHADER-LIVE", "pair %s translated on demand, in same frame: %.0f ms", name, ms);
        }
        return true;
    }
    info->why = "live recompiler could not translate pair";
    return false;
}

void NoteUiShaderPair(u64 vs_address, u64 ps_address, u64 signature, s32 fetch_sgpr, const std::vector<u32>* fetch_code) {
    if (vs_address != g_ui_pair_last_vs || ps_address != g_ui_pair_last_ps || signature != g_ui_pair_last_signature ||
        !g_ui_pair_last_count) {
        g_ui_pair_last_vs = vs_address;
        g_ui_pair_last_ps = ps_address;
        g_ui_pair_last_signature = signature;
        g_ui_pair_last_count = &g_ui_shader_pairs[{vs_address, ps_address, signature}];
        if (*g_ui_pair_last_count == 0) {
            TakePairSnapshot(vs_address, ps_address, signature, fetch_sgpr, fetch_code);
        }
    }
    ++*g_ui_pair_last_count;
}

void WriteUiShaderPairs() {
    if (g_ui_shader_pairs.empty() || render::GameSpirvDirectory().empty()) {
        return;
    }
    std::map<std::tuple<u64, u64, u64>, u64> by_hash; // os enderecos mudam; o hash do conteudo nao
    for (const auto& [key, draws] : g_ui_shader_pairs) {
        by_hash[{ContentHashOfShader(kStageVs, std::get<0>(key)), ContentHashOfShader(kStagePs, std::get<1>(key)), std::get<2>(key)}] += draws;
    }
    const std::filesystem::path path = std::filesystem::path(render::GameSpirvDirectory()) / "ui_shader_pairs.tsv";
    if (std::FILE* f = _wfopen(path.c_str(), L"w")) {
        std::fprintf(f, "vs_hash\tps_hash\tvariant\tdraws\n");
        for (const auto& [hashes, draws] : by_hash) {
            std::fprintf(f, "%016llx\t%016llx\t%016llx\t%llu\n", static_cast<unsigned long long>(std::get<0>(hashes)),
                         static_cast<unsigned long long>(std::get<1>(hashes)), static_cast<unsigned long long>(std::get<2>(hashes)),
                         static_cast<unsigned long long>(draws));
        }
        std::fclose(f);
    }
    // Retratos: os desta execucao substituem os de mesmo nome; os de outras execucoes (outras
    // cenas do jogo) sao mantidos, para a ferramenta poder recompilar tudo o que ja foi visto.
    const std::filesystem::path snapshots = std::filesystem::path(render::GameSpirvDirectory()) / "pair_snapshots.txt";
    std::map<std::string, std::string> blocks; // "vs ps variante" -> linhas do retrato
    if (std::FILE* f = _wfopen(snapshots.c_str(), L"r")) {
        static char line[8192];
        std::string current;
        while (std::fgets(line, sizeof(line), f)) {
            if (!std::strncmp(line, "pair ", 5)) {
                current.assign(line + 5);
                while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) {
                    current.pop_back();
                }
                // retratos do formato antigo (sem variante) nao servem mais
                current = std::count(current.begin(), current.end(), ' ') == 2 ? current : std::string();
                if (!current.empty()) {
                    blocks[current].clear();
                }
            } else if (!current.empty()) {
                blocks[current] += line;
            }
        }
        std::fclose(f);
    }
    const size_t kept = blocks.size();
    size_t fresh = 0;
    for (const auto& [key, snapshot] : g_ui_pair_snapshots) {
        const u64 vs_hash = ContentHashOfShader(kStageVs, std::get<0>(key)), ps_hash = ContentHashOfShader(kStagePs, std::get<1>(key));
        if (!vs_hash || !ps_hash) {
            continue;
        }
        char name[80];
        std::snprintf(name, sizeof(name), "%016llx %016llx %016llx", static_cast<unsigned long long>(vs_hash),
                      static_cast<unsigned long long>(ps_hash), static_cast<unsigned long long>(std::get<2>(key)));
        fresh += blocks.count(name) == 0;
        blocks[name] = SnapshotText(snapshot);
    }
    if (std::FILE* f = _wfopen(snapshots.c_str(), L"w")) {
        for (const auto& [name, text] : blocks) {
            std::fprintf(f, "pair %s\n%s", name.c_str(), text.c_str());
        }
        std::fclose(f);
    }
    std::printf("  pair variants (VS, PS, vertex layout) seen in this run: %zu | snapshots in %s: %zu (%zu new this "
                "run, %zu already existed)\n",
                by_hash.size(), snapshots.string().c_str(), blocks.size(), fresh, kept);
}

s32 FetchSgprOfVertexShader(u64 vs_address) {
    std::lock_guard lock(g_shader_mutex);
    const auto known = g_shader_at[kStageVs].find(vs_address);
    return known == g_shader_at[kStageVs].end() ? -1 : known->second.fetch_sgpr;
}

void CaptureFetchShader() {
    const u64 vs_address = (static_cast<u64>(g_vs_track[0x08]) << 8) | (static_cast<u64>(g_vs_track[0x09] & 0xFF) << 40);
    if (!vs_address) {
        return;
    }
    std::lock_guard lock(g_shader_mutex);
    const auto known = g_shader_at[kStageVs].find(vs_address);
    if (g_shader_dir.empty() || known == g_shader_at[kStageVs].end()) {
        return;
    }
    const s32 sgpr = known->second.fetch_sgpr;
    if (sgpr < 0) {
        if (vs_address != g_fetch_last_vs) {
            g_fetch_last_vs = vs_address;
            g_fetch_last_pointer = 0;
            static std::set<u64> counted;
            g_vs_without_fetch += counted.insert(known->second.content_hash).second;
        }
        return;
    }
    const u64 pointer = g_vs_track[0x0C + sgpr] | (static_cast<u64>(g_vs_track[0x0C + sgpr + 1]) << 32);
    if (vs_address == g_fetch_last_vs && pointer == g_fetch_last_pointer) {
        return; // caminho quente: mesmo par do draw anterior
    }
    g_fetch_last_vs = vs_address;
    g_fetch_last_pointer = pointer;
    // Par de ENDERECOS ja examinado: nada a fazer. Sem isto cada troca de vertex shader custava
    // duas consultas VirtualQuery (dezenas de microssegundos cada neste processo) mais um hash,
    // milhares de vezes por frame - era o gargalo de 2 quadros/s em Vorton (Fase 14).
    // Limite conhecido: um fetch shader diferente gravado depois no mesmo endereco, para o
    // mesmo VS, nao e visto.
    if (!g_fetch_address_pairs.insert({vs_address, pointer}).second) {
        return;
    }
    if (pointer < 0x10000 || (pointer & 3)) {
        ++g_fetch_bad;
        return;
    }
    // Fase 45: copia protegida em blocos de 16 dwords ate o s_setpc, sem Readable/VirtualQuery
    // (duas consultas por par novo, cada uma de centenas de microssegundos a alguns ms).
    static thread_local std::vector<u32> fetch_code;
    fetch_code.clear();
    u32 dwords = 0;
    for (u32 block = 0; block < 64 && !dwords; ++block) {
        u32 chunk[16];
        if (!SafeCopy(chunk, pointer + block * sizeof(chunk), sizeof(chunk))) {
            break; // fim da regiao mapeada
        }
        for (u32 i = 0; i < 16; ++i) {
            fetch_code.push_back(chunk[i]);
            if ((chunk[i] & 0xFF80FF00) == 0xBE802000) { // s_setpc_b64
                dwords = block * 16 + i + 1;
                break;
            }
        }
    }
    const u32* code = fetch_code.data();
    if (!dwords) {
        ++g_fetch_bad;
        return;
    }
    const u64 bytes = dwords * 4ull;
    u64 hash = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < bytes; ++i) {
        hash = (hash ^ reinterpret_cast<const u8*>(code)[i]) * 0x100000001b3ull;
    }
    const u64 vs_hash = known->second.content_hash;
    if (!g_fetch_pairs.insert({vs_hash, hash}).second) {
        return;
    }
    // Fase 45: gravado em segundo plano (antes: create_directories + exists + 2 arquivos no HD por par)
    char name[48];
    std::snprintf(name, sizeof(name), "fs_%016llx.bin", static_cast<unsigned long long>(hash));
    if (g_fetch_hashes.insert(hash).second) {
        FileWrite job;
        job.path = g_shader_dir / name;
        job.data.assign(reinterpret_cast<const char*>(code), bytes);
        job.skip_if_exists = true;
        job.log_line = Format("Dumped FETCH: %016llx (Size: %llu bytes)", static_cast<unsigned long long>(hash),
                              static_cast<unsigned long long>(bytes));
        WriteFileAsync(std::move(job));
        ++g_fetch_written;
    }
    // pares VS <-> fetch shader (arquivo so de acrescimo)
    FileWrite index;
    index.path = g_shader_dir / "fetch_index.tsv";
    index.data = Format("%016llx\t%016llx\t%d\t%llu\n", static_cast<unsigned long long>(vs_hash), static_cast<unsigned long long>(hash), sgpr,
                        static_cast<unsigned long long>(bytes));
    index.append = true;
    WriteFileAsync(std::move(index));
}

// ===================== funcoes que escrevem em command buffer =================
HLE_IMPL(u32, sceGnmDrawInitDefaultHardwareState200, u32* cmdbuf, u32 size) {
    HLE_TRACE("%p, %u dwords", static_cast<void*>(cmdbuf), size);
    return (size >= kHwInitPacketSize && FillNop(cmdbuf, kHwInitPacketSize)) ? kHwInitPacketSize : 0;
}
HLE_OVERRIDE(sceGnmDrawInitDefaultHardwareState200);

HLE_IMPL(u32, sceGnmDispatchInitDefaultHardwareState, u32* cmdbuf, u32 size) {
    HLE_TRACE("%p, %u dwords", static_cast<void*>(cmdbuf), size);
    return (size >= kHwInitPacketSize && FillNop(cmdbuf, kHwInitPacketSize)) ? kHwInitPacketSize : 0;
}
HLE_OVERRIDE(sceGnmDispatchInitDefaultHardwareState);

// Todas tem a forma (cmdbuf, size, ...): os demais argumentos (registradores do shader, texto
// do marcador) nao sao usados nesta camada.
#define HLE_GNM_CMDBUF_WRITER(name)                                                                \
    HLE_IMPL(int, name, u32* cmdbuf, u32 size) {                                                   \
        HLE_QUIET("%p, %u dwords", static_cast<void*>(cmdbuf), size);                              \
        return FillNop(cmdbuf, size) ? err::OK : -1;                                               \
    }                                                                                              \
    HLE_OVERRIDE(name)

// Set*Shader / Update*Shader: (cmdbuf, size, registradores do estagio, ...). O terceiro
// argumento e o que interessa ao extrator; o pacote continua virando NOP.
#define HLE_GNM_SHADER_WRITER(name, stage)                                                         \
    HLE_IMPL(int, name, u32* cmdbuf, u32 size, const u32* regs) {                                  \
        HLE_QUIET("%p, %u dwords, regs %p", static_cast<void*>(cmdbuf), size,                      \
                  static_cast<const void*>(regs));                                                 \
        ExtractShader(stage, regs);                                                                \
        return FillNop(cmdbuf, size) ? err::OK : -1;                                               \
    }                                                                                              \
    HLE_OVERRIDE(name)

// Como no vertex shader: o pixel shader deixa no command buffer o SET_SH_REG de
// SPI_SHADER_PGM_LO/HI_PS (0x08 / 0x09), para a submissao saber qual PS esta ativo em cada draw.
#define HLE_GNM_PS_WRITER(name)                                                                    \
    HLE_IMPL(int, name, u32* cmdbuf, u32 size, const u32* regs) {                                  \
        HLE_QUIET("%p, %u dwords, regs %p", static_cast<void*>(cmdbuf), size,                      \
                  static_cast<const void*>(regs));                                                 \
        ExtractShader(kStagePs, regs);                                                             \
        if (!cmdbuf || size < 6) {                                                                 \
            return FillNop(cmdbuf, size) ? err::OK : -1;                                           \
        }                                                                                          \
        cmdbuf[0] = 0xC0000000u | (2u << 16) | (0x76u << 8); /* SET_SH_REG, 2 valores */           \
        cmdbuf[1] = 0x08;                                                                          \
        cmdbuf[2] = regs ? regs[0] : 0; /* ponteiro nulo = PS desligado */                         \
        cmdbuf[3] = regs ? regs[1] : 0;                                                            \
        return FillNop(cmdbuf + 4, size - 4) ? err::OK : -1;                                       \
    }                                                                                              \
    HLE_OVERRIDE(name)

HLE_GNM_PS_WRITER(sceGnmSetPsShader);
HLE_GNM_CMDBUF_WRITER(sceGnmSetEmbeddedPsShader);
HLE_GNM_PS_WRITER(sceGnmUpdatePsShader);
// O vertex shader, alem de extraido, deixa no command buffer o SET_SH_REG de
// SPI_SHADER_PGM_LO/HI_VS (como o driver real faz), para a submissao saber qual VS esta ativo
// em cada draw e achar o fetch shader dele. O resto do pacote continua NOP.
#define HLE_GNM_VS_WRITER(name)                                                                    \
    HLE_IMPL(int, name, u32* cmdbuf, u32 size, const u32* regs) {                                  \
        HLE_QUIET("%p, %u dwords, regs %p", static_cast<void*>(cmdbuf), size,                      \
                  static_cast<const void*>(regs));                                                 \
        ExtractShader(kStageVs, regs);                                                             \
        if (!cmdbuf || !regs || size < 6) {                                                        \
            return FillNop(cmdbuf, size) ? err::OK : -1;                                           \
        }                                                                                          \
        cmdbuf[0] = 0xC0000000u | (2u << 16) | (0x76u << 8); /* SET_SH_REG, 2 valores */           \
        cmdbuf[1] = 0x48;                                                                          \
        cmdbuf[2] = regs[0];                                                                       \
        cmdbuf[3] = regs[1];                                                                       \
        return FillNop(cmdbuf + 4, size - 4) ? err::OK : -1;                                       \
    }                                                                                              \
    HLE_OVERRIDE(name)

HLE_GNM_VS_WRITER(sceGnmSetVsShader);
HLE_GNM_CMDBUF_WRITER(sceGnmSetEmbeddedVsShader);
HLE_GNM_VS_WRITER(sceGnmUpdateVsShader);
HLE_GNM_SHADER_WRITER(sceGnmSetEsShader, kStageEs);
HLE_GNM_SHADER_WRITER(sceGnmSetHsShader, kStageHs);
HLE_GNM_SHADER_WRITER(sceGnmUpdateHsShader, kStageHs);
HLE_GNM_SHADER_WRITER(sceGnmSetLsShader, kStageLs);
HLE_GNM_SHADER_WRITER(sceGnmSetCsShader, kStageCs);
HLE_GNM_SHADER_WRITER(sceGnmSetGsShader, kStageGs);
HLE_GNM_SHADER_WRITER(sceGnmUpdateGsShader, kStageGs);
HLE_GNM_CMDBUF_WRITER(sceGnmInsertSetMarker);
HLE_GNM_CMDBUF_WRITER(sceGnmInsertSetColorMarker);
HLE_GNM_CMDBUF_WRITER(sceGnmInsertPushMarker);
HLE_GNM_CMDBUF_WRITER(sceGnmInsertPushColorMarker);
HLE_GNM_CMDBUF_WRITER(sceGnmInsertPopMarker);
// No hardware isto e um WAIT_REG_MEM sobre o label do buffer de video (a GPU espera o flip
// anterior terminar). Aqui o flip e sequenciado pela thread de VSync, entao vira NOP.
HLE_GNM_CMDBUF_WRITER(sceGnmInsertWaitFlipDone);

// ================================ submissao ==================================
// Fase 28: engasgos. Um frame "engasgou" quando o processamento dos pacotes (Submit) passou de
// 12 ms; as categorias do HitchClock dizem em que o tempo foi gasto. O intervalo entre flips
// mede o que o jogador ve (inclui o que o proprio jogo faz na CPU dele).
struct HitchStats {
    u64 frames = 0, hitch_frames = 0, long_intervals = 0;
    double hitch_ms = 0, worst_ms = 0, worst_interval_ms = 0;
    double kind_ms[render::HitchClock::kKinds]{};
    u64 kind_count[render::HitchClock::kKinds]{};
    double lap_ms[4]{}, submit_ms = 0;
    std::chrono::steady_clock::time_point last_flip{};
};
HitchStats g_hitch;
std::atomic<bool> g_hitch_log{false}; // --hitch-log

// Fase 59: relogio comum das linhas [ENGASGO] e [QUADRO-LENTO] (comeca no primeiro quadro).
std::chrono::steady_clock::time_point HitchEpoch() {
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}

// Fase 69: para onde foi o tempo de cada intervalo entre flips (o que o jogador ve como quadro). O intervalo
// que comeca num SubmitAndFlip vai ate o proximo e tem: os Submits da thread de render (por categoria do
// HitchClock + o que nenhuma categoria cobre), o fim do frame (etapas do [QUADRO-LENTO] e do DrawFrame) e o
// resto, que e o proprio jogo na CPU. Uma linha [HITCH-MIN] por minuto (--hitch-log ou LD_HITCH_MIN=1)
// soma so os intervalos acima de 25 ms; o fim da sessao traz a soma da sessao inteira.
constexpr int kHitchKinds = render::HitchClock::kKinds;
struct IntervalCost {
    double submit_ms = 0;          // todos os Submits (com e sem flip) da thread de render
    double submit_kind[kHitchKinds]{};
    double end_ms[5]{};            // leitura de volta, frame anterior, texturas pendentes, envio de texturas, DrawFrame
    double draw_frame[7]{};        // dentro do DrawFrame: pipelines, lock, fence, acquire, gravacao, submit, present
    double end_kind[kHitchKinds]{};
    double gpu_ms = 0; // Fase 75: GPU do quadro que este intervalo esperou (o anterior)
    double render_wait = 0; // Fase 76: a thread do jogo esperando a thread de render (antes do Submit)
    bool threaded = false;  // Fase 76: fim do frame na thread de render (nao conta no tempo da thread do jogo)
};
struct HitchMinute {
    u64 frames = 0, over25 = 0, over50 = 0;
    u64 gpu_over = 0; // Fase 75: quadros com GPU acima de 16,7 ms
    double worst = 0, slow_ms = 0, game_ms = 0, worst_gpu = 0;
    double all_ms = 0, all_submit = 0, all_record = 0, all_gpu = 0; // Fase 75: todos os quadros (para as medias)
    IntervalCost slow; // soma dos intervalos acima de 25 ms
};
IntervalCost g_iv;
HitchMinute g_minute, g_minutes_total;
std::chrono::steady_clock::time_point g_iv_start{}, g_minute_start{};
u64 g_minute_worker_ns[kHitchKinds]{};

// Le e zera as categorias da thread de render (elas tambem vao para o total da sessao).
void TakeHitchKinds(double* into) {
    for (int k = 0; k < kHitchKinds; ++k) {
        const double ms = render::HitchClock::ns[k].exchange(0) / 1e6;
        const u32 n = render::HitchClock::count[k].exchange(0);
        g_hitch.kind_ms[k] += ms;
        g_hitch.kind_count[k] += n;
        into[k] += ms;
    }
}

void AddCost(IntervalCost& to, const IntervalCost& from) {
    to.submit_ms += from.submit_ms;
    to.gpu_ms += from.gpu_ms;
    to.render_wait += from.render_wait;
    for (int k = 0; k < kHitchKinds; ++k) {
        to.submit_kind[k] += from.submit_kind[k];
        to.end_kind[k] += from.end_kind[k];
    }
    for (int i = 0; i < 5; ++i) {
        to.end_ms[i] += from.end_ms[i];
    }
    for (int i = 0; i < 7; ++i) {
        to.draw_frame[i] += from.draw_frame[i];
    }
}

void PrintHitchMinute(const char* tag, double t, double span_s, const HitchMinute& m, const u64* worker_ns) {
    const IntervalCost& c = m.slow;
    double categorized = 0;
    for (int k = 0; k < kHitchKinds; ++k) {
        categorized += c.submit_kind[k];
    }
    const double end_total = c.end_ms[0] + c.end_ms[1] + c.end_ms[2] + c.end_ms[3] + c.end_ms[4];
    const double per_min = span_s > 0 ? m.over25 * 60.0 / span_s : 0;
    std::printf("[%s] t=%.0fs: %" PRIu64 " frames, %" PRIu64 " over 25 ms (%.1f per minute; %" PRIu64 " over 50 ms), worst %.1f ms"
                " | in frames over 25 ms (%.0f ms): game %.0f, Submit %.0f (pipelines %.0f, textures %.0f, detile %.0f, pairs %.0f, "
                "extractor %.0f, DMA %.0f, geometry %.0f, targets %.0f, pack pairs %.0f, live translation %.0f, lock %.0f, unattributed %.0f)"
                ", end of frame %.0f (readback %.0f, previous frame %.0f, pending textures %.0f, upload %.0f, DrawFrame %.0f: "
                "pipeline wait %.0f, lock %.0f, fence %.0f, acquire %.0f, recording %.0f, submit %.0f, present %.0f; categories at end %.0f)"
                " | worker threads in period: detile %.0f, textures %.0f, pipelines %.0f, lock %.0f ms"
                " | GPU: %" PRIu64 " frames over 16.7 ms, worst %.1f ms, in slow frames %.0f ms"
                " | per-frame average: %.2f ms (Submit %.2f, recording %.2f, GPU %.2f)"
                " | wait for render thread: %.0f ms in slow frames\n",
                tag, t, m.frames, m.over25, per_min, m.over50, m.worst, m.slow_ms, m.game_ms, c.submit_ms, c.submit_kind[0], c.submit_kind[1],
                c.submit_kind[3], c.submit_kind[2], c.submit_kind[4], c.submit_kind[5], c.submit_kind[6], c.submit_kind[7], c.submit_kind[8],
                c.submit_kind[9], c.submit_kind[10], std::max(0.0, c.submit_ms - categorized), end_total, c.end_ms[0], c.end_ms[1], c.end_ms[2],
                c.end_ms[3], c.end_ms[4], c.draw_frame[0], c.draw_frame[1], c.draw_frame[2], c.draw_frame[3], c.draw_frame[4], c.draw_frame[5],
                c.draw_frame[6],
                [&] {
                    double s = 0;
                    for (int k = 0; k < kHitchKinds; ++k) {
                        s += c.end_kind[k];
                    }
                    return s;
                }(),
                worker_ns[render::HitchClock::kDetile] / 1e6, worker_ns[render::HitchClock::kTexture] / 1e6,
                worker_ns[render::HitchClock::kPipeline] / 1e6, worker_ns[render::HitchClock::kLock] / 1e6, m.gpu_over, m.worst_gpu, c.gpu_ms,
                m.frames ? m.all_ms / m.frames : 0.0, m.frames ? m.all_submit / m.frames : 0.0, m.frames ? m.all_record / m.frames : 0.0,
                m.frames ? m.all_gpu / m.frames : 0.0, c.render_wait);
}

// No comeco de cada SubmitAndFlip: fecha o intervalo anterior.
void CloseHitchInterval(std::chrono::steady_clock::time_point now) {
    TakeHitchKinds(g_iv.submit_kind); // Submits sem flip e o que mais rodou na thread de render desde o fim do frame
    g_iv.submit_ms += g_submit_all_ns.exchange(0) / 1e6;
    g_iv.render_wait += g_render_wait_pending_ms; // Fase 76
    g_render_wait_pending_ms = 0;
    if (g_iv_start.time_since_epoch().count()) {
        const double interval = std::chrono::duration<double, std::milli>(now - g_iv_start).count();
        ++g_minute.frames;
        g_minute.all_ms += interval;
        g_minute.all_submit += g_iv.submit_ms;
        g_minute.all_record += g_iv.draw_frame[4];
        g_minute.all_gpu += g_iv.gpu_ms;
        g_minute.gpu_over += g_iv.gpu_ms > 16.7;
        g_minute.worst_gpu = std::max(g_minute.worst_gpu, g_iv.gpu_ms);
        if (interval > 25.0) {
            ++g_minute.over25;
            g_minute.over50 += interval > 50.0;
            g_minute.worst = std::max(g_minute.worst, interval);
            g_minute.slow_ms += interval;
            const double end_total = g_iv.end_ms[0] + g_iv.end_ms[1] + g_iv.end_ms[2] + g_iv.end_ms[3] + g_iv.end_ms[4];
            // Fase 76: com a thread de render o fim do frame corre em paralelo; na thread do jogo fica a espera por ela
            g_minute.game_ms += std::max(0.0, interval - g_iv.submit_ms - (g_iv.threaded ? g_iv.render_wait : end_total));
            AddCost(g_minute.slow, g_iv);
        }
    } else {
        g_minute_start = now;
        for (int k = 0; k < kHitchKinds; ++k) {
            g_minute_worker_ns[k] = render::HitchClock::worker_ns[k].load();
        }
    }
    g_iv = IntervalCost{};
    g_iv_start = now;
    const double span = std::chrono::duration<double>(now - g_minute_start).count();
    if (span >= 60.0) {
        u64 worker[kHitchKinds];
        for (int k = 0; k < kHitchKinds; ++k) {
            const u64 total = render::HitchClock::worker_ns[k].load();
            worker[k] = total - g_minute_worker_ns[k];
            g_minute_worker_ns[k] = total;
        }
        static const bool minute_env = std::getenv("LD_HITCH_MIN") != nullptr; // Fase 69: so o resumo por minuto
        if (minute_env || g_hitch_log.load(std::memory_order_relaxed)) {
            PrintHitchMinute("HITCH-MIN", std::chrono::duration<double>(now - HitchEpoch()).count(), span, g_minute, worker);
        }
        g_minutes_total.frames += g_minute.frames;
        g_minutes_total.over25 += g_minute.over25;
        g_minutes_total.over50 += g_minute.over50;
        g_minutes_total.gpu_over += g_minute.gpu_over;
        g_minutes_total.all_ms += g_minute.all_ms;
        g_minutes_total.all_submit += g_minute.all_submit;
        g_minutes_total.all_record += g_minute.all_record;
        g_minutes_total.all_gpu += g_minute.all_gpu;
        g_minutes_total.worst_gpu = std::max(g_minutes_total.worst_gpu, g_minute.worst_gpu);
        g_minutes_total.worst = std::max(g_minutes_total.worst, g_minute.worst);
        g_minutes_total.slow_ms += g_minute.slow_ms;
        g_minutes_total.game_ms += g_minute.game_ms;
        AddCost(g_minutes_total.slow, g_minute.slow);
        g_minute = HitchMinute{};
        g_minute_start = now;
    }
}

// Depois do DrawFrame: etapas do fim do frame e as categorias que rodaram nele.
void NoteFrameEnd(const double end_ms[5], const double draw_frame[7], double gpu_ms) {
    g_iv.gpu_ms = gpu_ms; // Fase 75
    for (int i = 0; i < 5; ++i) {
        g_iv.end_ms[i] += end_ms[i];
    }
    for (int i = 0; i < 7; ++i) {
        g_iv.draw_frame[i] += draw_frame[i];
    }
    TakeHitchKinds(g_iv.end_kind);
}

void NoteFrameHitch(std::chrono::steady_clock::time_point submit_start) {
    (void)HitchEpoch(); // o relogio comeca no primeiro quadro
    const auto now = std::chrono::steady_clock::now();
    const double submit_ms = std::chrono::duration<double, std::milli>(now - submit_start).count();
    double kind_ms[render::HitchClock::kKinds];
    u32 kind_count[render::HitchClock::kKinds];
    for (int k = 0; k < render::HitchClock::kKinds; ++k) {
        kind_ms[k] = render::HitchClock::ns[k].exchange(0) / 1e6;
        kind_count[k] = render::HitchClock::count[k].exchange(0);
        g_hitch.kind_ms[k] += kind_ms[k];
        g_hitch.kind_count[k] += kind_count[k];
        g_iv.submit_kind[k] += kind_ms[k]; // Fase 69
    }
    double lap_ms[4];
    const double ticks_per_ms = TicksPerNs() * 1e6; // Fase 59: os laps estao em ciclos
    for (int i = 0; i < 4; ++i) {
        lap_ms[i] = g_lap_ns[i] / ticks_per_ms;
        g_lap_ns[i] = 0;
        g_hitch.lap_ms[i] += lap_ms[i];
    }
    double sub_ms[5];
    for (int i = 0; i < 5; ++i) {
        sub_ms[i] = g_sub_ns[i] / ticks_per_ms;
        g_sub_ns[i] = 0;
    }
    g_hitch.submit_ms += submit_ms;
    double interval_ms = 0;
    if (g_hitch.frames) {
        interval_ms = std::chrono::duration<double, std::milli>(submit_start - g_hitch.last_flip).count();
        if (interval_ms > 50.0) {
            ++g_hitch.long_intervals;
        }
        g_hitch.worst_interval_ms = std::max(g_hitch.worst_interval_ms, interval_ms);
    }
    g_hitch.last_flip = submit_start;
    ++g_hitch.frames;
    if (submit_ms > 12.0) {
        ++g_hitch.hitch_frames;
        g_hitch.hitch_ms += submit_ms;
        g_hitch.worst_ms = std::max(g_hitch.worst_ms, submit_ms);
    }
    if (g_hitch_log.load(std::memory_order_relaxed) && (submit_ms > 12.0 || interval_ms > 50.0)) {
        const auto hitch_epoch = HitchEpoch(); // Fase 59: o mesmo relogio do [QUADRO-LENTO]
        std::printf("[HITCH] t=%.1fs frame %" PRIu64 ": interval %.1f ms, Submit %.1f ms | pipelines %u/%.1f ms, textures %u/%.1f ms "
                    "(detile %u/%.1f ms), pairs %u/%.1f ms, extractor %u/%.1f ms, DMA %u/%.1f ms | draw stages: state %.1f, pair %.1f, resources %.1f, queue %.1f ms"
                    " | new geometry %u/%.1f ms, new targets %u/%.1f ms, pack pairs %u/%.1f ms"
                    " | pair: lookup %.1f, snapshot %.1f; resources: geometry %.1f, constants %.1f, textures %.1f ms"
                    " | on demand: %u new textures %.1f ms, %u linear checked %.1f ms"
                    " | live translation %u/%.1f ms, lock %u/%.1f ms\n", // Fase 69
                    std::chrono::duration<double>(now - hitch_epoch).count(), g_hitch.frames, interval_ms, submit_ms, kind_count[0],
                    kind_ms[0], kind_count[1], kind_ms[1], kind_count[3], kind_ms[3], kind_count[2], kind_ms[2], kind_count[4], kind_ms[4], kind_count[5], kind_ms[5],
                    lap_ms[0], lap_ms[1], lap_ms[2], lap_ms[3], kind_count[6], kind_ms[6], kind_count[7], kind_ms[7], kind_count[8], kind_ms[8],
                    sub_ms[0], sub_ms[1], sub_ms[2], sub_ms[3], sub_ms[4], g_tex_inline_count, g_tex_inline_ms, g_tex_inline_checks,
                    g_tex_inline_check_ms, kind_count[render::HitchClock::kLive], kind_ms[render::HitchClock::kLive],
                    kind_count[render::HitchClock::kLock], kind_ms[render::HitchClock::kLock]);
    }
    g_tex_inline_count = g_tex_inline_checks = 0;
    g_tex_inline_ms = g_tex_inline_check_ms = 0;
}

// Fase 59: pegador de travadas. Passou de 100 ms sem o jogo comecar um quadro novo (depois dos primeiros
// 600 quadros), sai no log onde cada thread do jogo esta naquele instante ([TRAVADA-PILHAS]). E o que
// atribui as travadas que nao caem em nenhuma etapa medida (o proprio jogo esperando outra thread,
// leitura de arquivo, o driver segurando o present...). Ate 40 por sessao; --no-stall-stacks desliga.
std::atomic<s64> g_last_flip_submit_ms{0};
std::atomic<u64> g_flip_submits{0};
std::atomic<bool> g_stall_stacks{true};

void StallWatchThread() {
    s64 reported_for = -1;
    u32 reports = 0;
    const char* limit_env = std::getenv("LD_STALL_MS"); // Fase 61 (medicao): limite menor que 100 ms
    const s64 limit_ms = limit_env ? std::max<s64>(30, std::atoll(limit_env)) : 100;
    for (;;) {
        Sleep(15);
        const s64 last = g_last_flip_submit_ms.load();
        if (!last || g_flip_submits.load() < 600 || reports >= 40 || last == reported_for) {
            continue;
        }
        const s64 now = NowMs();
        if (now - last >= limit_ms) {
            reported_for = last;
            ++reports;
            char reason[200];
            std::snprintf(reason, sizeof(reason), "t=%.1fs ([GAME STATE] clock), after frame %llu: %lld ms without a new frame; each thread location:",
                          now / 1000.0, static_cast<unsigned long long>(g_flip_submits.load()), static_cast<long long>(now - last));
            log::FlushGameText();
            runtime::DumpStallStacks(reason);
        }
    }
}

// ================= Fase 76: fim do frame numa thread propria =================
// O fim do frame (leituras de volta dos alvos minusculos, espera das texturas do quadro, envio, DrawFrame: gravacao,
// submit e o present FIFO, que espera o VSync) rodava na thread do jogo, dentro do SubmitAndFlip. Agora ele roda numa
// thread de render e o jogo volta a montar o proximo quadro na hora; o flip do jogo (SubmitFlip) continua sendo
// enfileirado so depois do present, como antes. Para o estado do quadro (filas de draws, aneis, alvos, texturas) nada
// muda de ordem: todo Submit seguinte (com ou sem flip) espera a thread de render terminar o quadro anterior antes de
// mexer em qualquer coisa. Desligado por padrao: na RX 6600 / Ryzen 5 5600X nenhuma cena testada e limitada por CPU
// (Vorton fica em 60 fps mesmo com o processo preso a 2 processadores logicos), entao o ganho nao foi medido; nos testes
// os dois caminhos deram os mesmos numeros. LD_RENDER_THREAD=1 liga (para medir em cenas de ~6000 draws).
bool RenderThreadEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("LD_RENDER_THREAD");
        return env && env[0] == '1';
    }();
    return enabled;
}

void RunFrameEnd(u32 buffer_index) {
    if (auto& renderer = render::VulkanRenderer::Instance(); renderer.Active()) {
        static perf::Counter perf_counter("Vulkan DrawFrame (clear + present)");
        const perf::Scope perf_scope(perf_counter);
        // Fase 25: pixels dos alvos minusculos copiados no frame anterior voltam para a memoria do
        // jogo, no lugar em que o tiling do PS4 os guardaria (microtile 8x8; so os primeiros pixels).
        // Fase 59: etapas do fim do frame cronometradas; passou de 40 ms, sai uma linha [QUADRO-LENTO]
        const auto end_start = std::chrono::steady_clock::now();
        double end_ms[5] = {};
        auto end_lap = [&, last = end_start](int i) mutable {
            const auto now = std::chrono::steady_clock::now();
            end_ms[i] = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
        };
        renderer.CollectReadbacks([](u64 address, const unsigned char* pixels, u32 width, u32 height, u32 bytes) {
            if (width > 8 || height > 8 || !Writable(address, 64ull * bytes)) {
                return;
            }
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const u32 x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1, y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
                    u32 index = 0; // ordem dentro do microtile, por tamanho do elemento (tabela do detiler do shadPS4)
                    if (bytes == 1) {
                        index = x0 | (x1 << 1) | (x2 << 2) | (y1 << 3) | (y0 << 4) | (y2 << 5);
                    } else if (bytes == 2) {
                        index = x0 | (x1 << 1) | (x2 << 2) | (y0 << 3) | (y1 << 4) | (y2 << 5);
                    } else if (bytes == 4) {
                        index = x0 | (x1 << 1) | (y0 << 2) | (x2 << 3) | (y1 << 4) | (y2 << 5);
                    } else {
                        index = x0 | (y0 << 1) | (x1 << 2) | (y1 << 3) | (x2 << 4) | (y2 << 5);
                    }
                    std::memcpy(reinterpret_cast<void*>(address + static_cast<u64>(index) * bytes), pixels + (static_cast<size_t>(y) * width + x) * bytes,
                                bytes);
                }
            }
            static u32 logged = 0;
            if (logged < 12) {
                ++logged;
                float f[2] = {};
                if (bytes == 4) {
                    u16 halves[2];
                    std::memcpy(halves, pixels, 4);
                    std::printf("[READBACK] target 0x%llx %ux%u, %u bytes per pixel: first pixel %04x %04x\n", static_cast<unsigned long long>(address),
                                width, height, bytes, halves[0], halves[1]);
                } else {
                    std::memcpy(f, pixels, std::min<u32>(bytes, 8));
                    std::printf("[READBACK] target 0x%llx %ux%u, %u bytes per pixel: %g %g\n", static_cast<unsigned long long>(address), width, height,
                                bytes, f[0], f[1]);
                }
            }
            ++g_readbacks_delivered;
        });
        // Fase 35: texturas prontas vao para a GPU aqui, com o frame anterior ja terminado.
        end_lap(0);
        renderer.WaitPreviousFrame();
        end_lap(1);
        renderer.NoteFrameWait(WaitFrameTextures()); // Fase 45: texturas pendentes dos draws deste frame
        end_lap(2);
        DrainTextureResults(8.0); // Fase 39: o envio agora e so uma copia para o staging do frame
        end_lap(3);
        videoout::QueueStatsOverlay(); // Fase 65: contador de FPS / info por cima do quadro
        renderer.DrawFrame(videoout::BufferAddress(static_cast<int>(buffer_index)));
        end_lap(4);
        double df[7];
        renderer.LastDrawFrameTimes(df);
        NoteFrameEnd(end_ms, df, renderer.LastGpuFrameMs()); // Fase 69 / 75
        if (const double total = end_ms[0] + end_ms[1] + end_ms[2] + end_ms[3] + end_ms[4]; total > 40.0) {
            static u32 logged = 0;
            if (logged++ < 400) {
                const auto t0 = HitchEpoch(); // Fase 59: mesmo relogio do [ENGASGO]
                std::printf("[SLOW-FRAME] t=%.1fs end of frame %.1f ms: readback %.1f, previous frame %.1f, pending textures %.1f, "
                            "texture upload %.1f, DrawFrame %.1f (pipelines %.1f, lock %.1f, fence %.1f, acquire %.1f, recording %.1f, "
                            "submit %.1f, present %.1f) | GPU of previous frame %.1f ms\n",
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), total, end_ms[0], end_ms[1], end_ms[2],
                            end_ms[3], end_ms[4], df[0], df[1], df[2], df[3], df[4], df[5], df[6], renderer.LastGpuFrameMs());
            }
        }
        ++g_cache_frame;
        ++g_texture_frame;
        g_tex_frame_ms = 0; // Fase 39: orcamento de texturas na hora, por frame
        g_tex_frame_new = 0;
        g_tex_frame_spike = false;
        g_pair_frame_ms = 0; // Fase 41
        g_live_sync_frame_ms = 0; // Fase 56
        if (g_texture_frame % 300 == 0) { // Fase 40: retrato do streaming a cada 300 frames (~5 s)
            static const auto t0 = std::chrono::steady_clock::now();
            static u64 last_sync = 0, last_async = 0, last_pending = 0, last_spikes = 0, last_pipes = 0;
            const u64 pipes = renderer.GetSyncPipelineStats().created + renderer.AsyncPipelines();
            PROCESS_MEMORY_COUNTERS_EX mem{}; // Fase 55: memoria do processo (sessoes longas)
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
            u64 failed = 0, retried = 0;
            renderer.PipelineFailures(&failed, &retried);
            u64 vram_used = 0, vram_budget = 0; // Fase 59
            renderer.VideoMemory(&vram_used, &vram_budget);
            std::printf("[STREAM] t=%.0fs: new textures %" PRIu64 " on demand + %" PRIu64 " from thread | draws waiting for texture %" PRIu64
                        " | new pipelines %" PRIu64 " | frames with peak %" PRIu64 " | memory %llu MB (RAM in use %llu MB) | video %llu of %llu MB, "
                        "textures %u (%llu MB, %llu discarded) | failed pipelines %" PRIu64 " (%" PRIu64 " retries)\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), g_tex_sync_count - last_sync,
                        g_tex_sync_deferred - last_async, g_vk_stats.texture_pending - last_pending, pipes - last_pipes,
                        g_tex_spike_frames - last_spikes, static_cast<unsigned long long>(mem.PrivateUsage >> 20),
                        static_cast<unsigned long long>(mem.WorkingSetSize >> 20), static_cast<unsigned long long>(vram_used >> 20),
                        static_cast<unsigned long long>(vram_budget >> 20), renderer.TexturesCreated(),
                        static_cast<unsigned long long>(renderer.TextureBytes() >> 20),
                        static_cast<unsigned long long>(renderer.TexturesEvicted()), failed, retried);
            last_sync = g_tex_sync_count;
            last_async = g_tex_sync_deferred;
            last_pending = g_vk_stats.texture_pending;
            last_spikes = g_tex_spike_frames;
            last_pipes = pipes;
        }
    }
}

struct FrameEndJob {
    u32 handle, buffer_index, flip_mode;
    s64 flip_arg;
};
std::mutex g_rt_mutex;
std::condition_variable g_rt_cv;
std::optional<FrameEndJob> g_rt_job;
bool g_rt_busy = false; // g_rt_mutex

void RenderThreadMain() {
    render::MarkRenderThread(); // o tempo dela tambem trava o quadro (HitchClock)
    for (;;) {
        FrameEndJob job;
        {
            std::unique_lock lock(g_rt_mutex);
            g_rt_cv.wait(lock, [] { return g_rt_job.has_value(); });
            job = *g_rt_job;
            g_rt_job.reset();
        }
        RunFrameEnd(job.buffer_index);
        videoout::SubmitFlip(static_cast<int>(job.handle), static_cast<int>(job.buffer_index), static_cast<int>(job.flip_mode), job.flip_arg);
        {
            std::lock_guard lock(g_rt_mutex);
            g_rt_busy = false;
        }
        g_rt_cv.notify_all();
    }
}

void QueueFrameEnd(const FrameEndJob& job) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(RenderThreadMain).detach(); });
    {
        std::lock_guard lock(g_rt_mutex);
        g_rt_job = job;
        g_rt_busy = true;
    }
    g_rt_cv.notify_all();
}

// Antes de qualquer Submit: o quadro anterior tem de ter terminado na thread de render. Devolve o tempo esperado (ms).
double WaitRenderThreadIdle() {
    if (!RenderThreadEnabled()) {
        return 0;
    }
    std::unique_lock lock(g_rt_mutex);
    if (!g_rt_busy) {
        return 0;
    }
    const auto start = std::chrono::steady_clock::now();
    g_rt_cv.wait(lock, [] { return !g_rt_busy; });
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

HLE_IMPL(int, sceGnmSubmitCommandBuffers, u32 count, const u32* const* dcb_addrs, const u32* dcb_sizes,
         const u32* const* ccb_addrs, const u32* ccb_sizes) {
    HLE_QUIET("%u buffers", count);
    return Submit(count, dcb_addrs, dcb_sizes, ccb_addrs, ccb_sizes);
}
HLE_OVERRIDE(sceGnmSubmitCommandBuffers);

// buffer_index, flip_mode e flip_arg chegam pela pilha.
HLE_IMPL(int, sceGnmSubmitAndFlipCommandBuffers, u32 count, const u32* const* dcb_addrs, const u32* dcb_sizes,
         const u32* const* ccb_addrs, const u32* ccb_sizes, u32 video_out_handle, u32 buffer_index, u32 flip_mode,
         s64 flip_arg) {
    HLE_QUIET("%u buffers, flip handle=%u buffer=%u mode=%u arg=%" PRId64, count, video_out_handle, buffer_index,
              flip_mode, flip_arg);
    g_render_wait_pending_ms += WaitRenderThreadIdle(); // Fase 76: antes de fechar a conta do quadro anterior
    const auto submit_start = std::chrono::steady_clock::now();
    CloseHitchInterval(submit_start); // Fase 69
    g_last_flip_submit_ms = NowMs(); // Fase 59
    ++g_flip_submits;
    static std::once_flag stall_once;
    std::call_once(stall_once, [] {
        if (g_stall_stacks.load()) {
            std::thread(StallWatchThread).detach();
        }
    });
    static const bool dbg_cb = std::getenv("LD_DBG_CB") != nullptr; // DIAGNOSTICO Fase 60
    auto dbg_cb_check = [](const char* when) {
        std::lock_guard lock(g_dbg_cb_mutex);
        std::map<std::tuple<u64, u64, u32, u32>, std::pair<u32, u32>> groups; // (VS, PS, estagio, binding) -> (mudaram, total)
        u32 changed = 0, printed = 0;
        std::vector<u8> now;
        for (const auto& e : g_dbg_cb) {
            now.resize(e.bytes);
            const bool same = SafeCopy(now.data(), e.address, e.bytes) && !std::memcmp(now.data(), e.data.data(), e.bytes);
            auto& g = groups[{e.vs, e.ps, e.stage, e.binding}];
            ++g.second;
            if (!same) {
                ++g.first;
                ++changed;
                if (printed < 30) {
                    ++printed;
                    u32 first = 0;
                    while (first + 4 <= e.bytes && !std::memcmp(now.data() + first, e.data.data() + first, 4)) {
                        first += 4;
                    }
                    const u32 row = std::min(first & ~15u, (e.bytes & ~15u) - 16);
                    const float* a = reinterpret_cast<const float*>(e.data.data() + row);
                    const float* b = reinterpret_cast<const float*>(now.data() + row);
                    std::printf("[DBG-CB] %s: pair %016llx %016llx stage %u b%u src %u address 0x%llx %u bytes, first different byte %u | copy %g %g %g %g -> now %g %g %g %g\n",
                                when, static_cast<unsigned long long>(e.vs), static_cast<unsigned long long>(e.ps), e.stage, e.binding, e.source,
                                static_cast<unsigned long long>(e.address), e.bytes, first, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                }
            }
        }
        u32 group_changed = 0;
        for (const auto& [key, g] : groups) {
            group_changed += g.first != 0;
        }
        std::printf("[DBG-CB] %s: %zu buffers copied, %u changed after copy (%u of %zu groups)\n", when, g_dbg_cb.size(), changed,
                    group_changed, groups.size());
    };
    if (dbg_cb && !g_dbg_cb.empty()) {
        dbg_cb_check("one frame later");
        g_dbg_cb.clear();
    }
    g_dbg_cb_record = dbg_cb && g_texture_frame % 600 == 300;
    const int result = Submit(count, dcb_addrs, dcb_sizes, ccb_addrs, ccb_sizes);
    if (g_dbg_cb_record) {
        g_dbg_cb_record = false;
        dbg_cb_check("end of Submit");
    }
    if (result != err::OK) {
        return result;
    }
    NoteFrameHitch(submit_start);
    // Frame do Vulkan, logo depois de os pacotes PM4 serem processados na CPU: por enquanto so
    // limpa a tela e apresenta. O swapchain e FIFO, entao esta chamada espera o VSync real do
    // monitor, e o flip do jogo so e enfileirado (SubmitFlip, abaixo) depois da apresentacao.
    // Fase 76: com a thread de render, o fim do frame (leituras de volta, texturas pendentes, DrawFrame com o present e o
    // flip) roda nela e o jogo volta a montar o proximo quadro na hora. O proximo Submit espera essa thread terminar.
    const bool threaded = RenderThreadEnabled() && render::VulkanRenderer::Instance().Active() &&
                          videoout::CheckFlip(static_cast<int>(video_out_handle), static_cast<int>(buffer_index)) == err::OK;
    if (!threaded) {
        RunFrameEnd(buffer_index);
    }
    {
        const render::TimedLock lock(g_mutex); // Fase 69
        EndFrame();
        if (++g_window_frames >= kWindowFrames) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count();
            log::Tagged("GAME STATE", "t=%.0fs frame %" PRIu64 ": %.1f draws/frame, %.0f PM4 packets/frame", seconds,
                        g_submits, static_cast<double>(g_window_draws) / g_window_frames,
                        static_cast<double>(g_window_packets) / g_window_frames);
            // Desempenho da janela: quadros/s, tempo por funcao medida e HLE mais chamados.
            const double window_seconds = seconds - g_window_start_seconds;
            perf::PrintWindow(window_seconds, g_window_frames);
            const double fps = window_seconds > 0 ? g_window_frames / window_seconds : 0.0;
            // Janela lenta (menos de 12 quadros/s) fora de um dump: pede um perfil de CPU a
            // thread do watchdog, no maximo 4 vezes por execucao.
            if (fps < 12.0 && seconds > 40.0 && g_auto_profiles < 4 && !g_window_had_dump) {
                ++g_auto_profiles;
                g_profile_request = true;
            }
            g_window_start_seconds = seconds;
            g_window_had_dump = false;
            g_window_draws = g_window_frames = g_window_packets = 0;
        }
    }
    if (threaded) {
        g_iv.threaded = true;
        QueueFrameEnd({video_out_handle, buffer_index, flip_mode, flip_arg});
        return err::OK;
    }
    return videoout::SubmitFlip(static_cast<int>(video_out_handle), static_cast<int>(buffer_index),
                                static_cast<int>(flip_mode), flip_arg);
}
HLE_OVERRIDE(sceGnmSubmitAndFlipCommandBuffers);

HLE_IMPL(int, sceGnmSubmitDone) {
    HLE_QUIET("");
    return err::OK;
}
HLE_OVERRIDE(sceGnmSubmitDone);

HLE_IMPL(int, sceGnmAreSubmitsAllowed) {
    HLE_QUIET("");
    return 1;
}
HLE_OVERRIDE(sceGnmAreSubmitsAllowed);

HLE_IMPL(int, sceGnmRequestFlipAndSubmitDone) {
    HLE_TRACE("");
    return err::OK;
}
HLE_OVERRIDE(sceGnmRequestFlipAndSubmitDone);

HLE_IMPL(void, sceGnmFlushGarlic) {
    HLE_QUIET("");
}
HLE_OVERRIDE(sceGnmFlushGarlic);

// ================================= eventos ===================================
HLE_IMPL(int, sceGnmAddEqEvent, equeue::Handle queue, u64 id, void* udata) {
    HLE_TRACE("id=0x%" PRIx64 " udata=%p", id, udata);
    if (!equeue::Register(queue, id, equeue::kFilterGraphicsCore, udata)) {
        return static_cast<int>(0x80020009); // EBADF
    }
    const render::TimedLock lock(g_mutex); // Fase 69
    g_listeners.push_back({queue, id});
    return err::OK;
}
HLE_OVERRIDE(sceGnmAddEqEvent);

HLE_IMPL(int, sceGnmDeleteEqEvent, equeue::Handle queue, u64 id) {
    HLE_TRACE("id=0x%" PRIx64, id);
    equeue::Unregister(queue, id, equeue::kFilterGraphicsCore);
    const render::TimedLock lock(g_mutex); // Fase 69
    std::erase_if(g_listeners, [&](const EopListener& l) { return l.queue == queue && l.id == id; });
    return err::OK;
}
HLE_OVERRIDE(sceGnmDeleteEqEvent);

HLE_IMPL(int, sceGnmGetEqEventType, const equeue::KernelEvent* event) {
    HLE_QUIET("%p", static_cast<const void*>(event));
    return event ? static_cast<int>(event->data) : 0;
}
HLE_OVERRIDE(sceGnmGetEqEventType);

HLE_IMPL(int, sceGnmGetEqTimeStamp) {
    HLE_QUIET("");
    return 0;
}
HLE_OVERRIDE(sceGnmGetEqTimeStamp);

// ============================== filas de compute =============================
struct ComputeQueue {
    const u32* ring;
    u32 ring_dwords;
    u32* read_ptr;
    u32 offset = 0;
};
std::map<u32, ComputeQueue> g_queues;
u32 g_next_vqid = 1;

int MapQueue(u32 pipe_id, u32 queue_id, const u32* ring, u32 ring_dwords, u32* read_ptr) {
    if (pipe_id > 6 || queue_id > 7 || !ring || !read_ptr || !ring_dwords) {
        return static_cast<int>(0x80D11000 | 0xFF); // parametro invalido
    }
    const render::TimedLock lock(g_mutex); // Fase 69
    const u32 vqid = g_next_vqid++;
    g_queues[vqid] = {ring, ring_dwords, read_ptr, 0};
    *read_ptr = 0;
    return static_cast<int>(vqid);
}

HLE_IMPL(int, sceGnmMapComputeQueue, u32 pipe_id, u32 queue_id, const u32* ring, u32 ring_dwords, u32* read_ptr) {
    const int result = MapQueue(pipe_id, queue_id, ring, ring_dwords, read_ptr);
    HLE_TRACE("pipe=%u queue=%u ring=%p (%u dwords) -> vqid %d", pipe_id, queue_id, static_cast<const void*>(ring),
              ring_dwords, result);
    return result;
}
HLE_OVERRIDE(sceGnmMapComputeQueue);

HLE_IMPL(int, sceGnmMapComputeQueueWithPriority, u32 pipe_id, u32 queue_id, const u32* ring, u32 ring_dwords,
         u32* read_ptr, u32 priority) {
    const int result = MapQueue(pipe_id, queue_id, ring, ring_dwords, read_ptr);
    HLE_TRACE("pipe=%u queue=%u ring=%p (%u dwords) prio=%u -> vqid %d", pipe_id, queue_id,
              static_cast<const void*>(ring), ring_dwords, priority, result);
    return result;
}
HLE_OVERRIDE(sceGnmMapComputeQueueWithPriority);

HLE_IMPL(int, sceGnmUnmapComputeQueue, u32 vqid) {
    HLE_TRACE("vqid %u", vqid);
    const render::TimedLock lock(g_mutex); // Fase 69
    return g_queues.erase(vqid) ? err::OK : -1;
}
HLE_OVERRIDE(sceGnmUnmapComputeQueue);

// "Campainha": o jogo escreveu comandos no anel ate `next_offset` (em dwords). Executa o trecho
// novo na hora e avanca o ponteiro de leitura que o jogo consulta.
HLE_IMPL(void, sceGnmDingDong, u32 vqid, u32 next_offset) {
    HLE_QUIET("vqid %u through %u", vqid, next_offset);
    WaitRenderThreadIdle(); // Fase 76: a fila de compute tambem le / escreve o estado do quadro
    const render::TimedLock lock(g_mutex); // Fase 69
    const auto it = g_queues.find(vqid);
    if (it == g_queues.end()) {
        return;
    }
    ComputeQueue& q = it->second;
    ++g_submits;
    if (next_offset >= q.offset && next_offset <= q.ring_dwords) {
        ProcessCommandBuffer(q.ring + q.offset, (next_offset - q.offset) * 4ULL, 0 /*Compute0RelMem*/);
    } else if (next_offset <= q.ring_dwords) { // deu a volta no anel
        ProcessCommandBuffer(q.ring + q.offset, (q.ring_dwords - q.offset) * 4ULL, 0);
        ProcessCommandBuffer(q.ring, next_offset * 4ULL, 0);
    }
    q.offset = next_offset % q.ring_dwords;
    *q.read_ptr = q.offset;
}
HLE_OVERRIDE(sceGnmDingDong);

// ======================== consultas e recursos fixos =========================
HLE_IMPL(int, sceGnmLogicalCuIndexToPhysicalCuIndex, int index) {
    HLE_TRACE("%d", index);
    return index;
}
HLE_OVERRIDE(sceGnmLogicalCuIndexToPhysicalCuIndex);

HLE_IMPL(int, sceGnmLogicalCuMaskToPhysicalCuMask, s64 unused, int logical_mask) {
    HLE_TRACE("0x%x", logical_mask);
    return logical_mask;
}
HLE_OVERRIDE(sceGnmLogicalCuMaskToPhysicalCuMask);

// Endereco fixo do Orbis. Nada e mapeado ali: so a GPU escreveria nesse anel.
HLE_IMPL(u64, sceGnmGetTheTessellationFactorRingBufferBaseAddress) {
    HLE_TRACE("-> 0xFF0000000");
    return 0xFF0000000ULL;
}
HLE_OVERRIDE(sceGnmGetTheTessellationFactorRingBufferBaseAddress);

// Funcoes que, sem GPU nem ferramentas de depuracao, so precisam responder "ok" / "nao".
#define HLE_GNM_CONST(name, value)                                                                 \
    HLE_IMPL(int, name) {                                                                          \
        HLE_QUIET("-> %d", static_cast<int>(value));                                               \
        return value;                                                                              \
    }                                                                                              \
    HLE_OVERRIDE(name)

HLE_GNM_CONST(sceGnmSetGsRingSizes, 0);
HLE_GNM_CONST(sceGnmDebugHardwareStatus, 0);
HLE_GNM_CONST(sceGnmSetupMipStatsReport, 0);
HLE_GNM_CONST(sceGnmRequestMipStatsReportAndReset, 0);
HLE_GNM_CONST(sceGnmDisableMipStatsReport, 0);
// Validacao e captura (Razor) so existem em kits de desenvolvimento.
HLE_GNM_CONST(sceGnmValidateGetVersion, 0);
HLE_GNM_CONST(sceGnmValidateOnSubmitEnabled, 0);
HLE_GNM_CONST(sceGnmValidateResetState, 0);
HLE_GNM_CONST(sceGnmValidateCommandBuffers, 0);
HLE_GNM_CONST(sceGnmValidationRegisterMemoryCheckCallback, 0);
HLE_GNM_CONST(sceRazorIsLoaded, 0);
HLE_GNM_CONST(sceRazorCaptureImmediate, 0);
HLE_GNM_CONST(sceRazorCaptureSinceLastFlip, 0);
HLE_GNM_CONST(sceRazorCaptureCommandBuffersOnlyImmediate, 0);
HLE_GNM_CONST(sceRazorCaptureCommandBuffersOnlySinceLastFlip, 0);
HLE_GNM_CONST(sceGnmDriverTriggerCapture, 0);
HLE_GNM_CONST(sceGnmDriverCaptureInProgress, 0);
HLE_GNM_CONST(sceGnmDriverTraceInProgress, 0);
HLE_GNM_CONST(sceGnmIsUserPaEnabled, 0);

const char* OpcodeName(u32 opcode) {
    switch (opcode) {
    case 0x10: return "NOP";
    case 0x12: return "CLEAR_STATE";
    case 0x15: return "DISPATCH_DIRECT";
    case 0x1D: return "OCCLUSION_QUERY";
    case 0x22: return "COND_EXEC";
    case 0x27: return "DRAW_INDEX_2";
    case 0x28: return "CONTEXT_CONTROL";
    case 0x2A: return "INDEX_TYPE";
    case 0x2D: return "DRAW_INDEX_AUTO";
    case 0x2F: return "NUM_INSTANCES";
    case 0x33: return "INDIRECT_BUFFER_CONST";
    case 0x35: return "DRAW_INDEX_OFFSET_2";
    case 0x37: return "WRITE_DATA";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x3F: return "INDIRECT_BUFFER";
    case 0x43: return "PFP_SYNC_ME";
    case 0x46: return "EVENT_WRITE";
    case 0x47: return "EVENT_WRITE_EOP";
    case 0x48: return "EVENT_WRITE_EOS";
    case 0x49: return "RELEASE_MEM";
    case 0x50: return "DMA_DATA";
    case 0x58: return "ACQUIRE_MEM";
    case 0x68: return "SET_CONFIG_REG";
    case 0x69: return "SET_CONTEXT_REG";
    case 0x76: return "SET_SH_REG";
    case 0x79: return "SET_UCONFIG_REG";
    case 0x81: return "WRITE_CONST_RAM";
    case 0x83: return "DUMP_CONST_RAM";
    case 0x86: return "INCREMENT_DE_COUNTER";
    case 0x8B: return "WAIT_ON_CE_COUNTER";
    case 0x88: return "WAIT_ON_DE_COUNTER_DIFF";
    default: return nullptr;
    }
}

} // namespace

// Chamado no resumo final: o que o jogo mandou para a "GPU".
void PrintStats() {
    FlushFileWrites(); // Fase 45: o que ainda estava na fila do gravador
    if (auto& renderer = render::VulkanRenderer::Instance(); renderer.Active()) {
        renderer.SavePipelineCache("game exit"); // Fase 70: fechar a janela encerra o processo sem o Shutdown
    }
    std::printf("  Phase 45 (extractor): %" PRIu64 " on cold path (%" PRIu64 " via slow lookup, %" PRIu64 " repeats) | header %.0f ms, slow lookup %.0f ms, hash %.0f ms, rest %.0f ms | %" PRIu64 " background recordings\n",
                g_extract_cold, g_extract_slow, g_extract_dup, g_extract_ms[0], g_extract_ms[1], g_extract_ms[2], g_extract_ms[3],
                g_file_writes.load());
    std::unique_lock lock(g_mutex, std::try_to_lock);
    std::printf("GnmDriver: %" PRIu64 " submits, %" PRIu64 " PM4 packets, %" PRIu64 " label/fence writes, %" PRIu64
                " interrupts, %" PRIu64 " refused writes (invalid address), %" PRIu64 " malformed buffers, %" PRIu64
                " DMA_DATA without memory destination (prefetch)\n",
                g_submits, g_packets, g_label_writes, g_interrupts, g_bad_writes, g_malformed, g_dma_non_memory);
    {
        std::unique_lock shader_lock(g_shader_mutex, std::try_to_lock);
        std::printf("Shaders: ");
        for (u32 stage = 0; stage < kStageCount; ++stage) {
            if (g_shader_calls[stage]) {
                std::printf("%s %zu distinct (%" PRIu64 " calls)  ", kStageName[stage], g_shader_hashes[stage].size(),
                            g_shader_calls[stage].load());
            }
        }
        std::printf(" | fetch shaders: %zu distinct, %zu VS-fetch pairs, %" PRIu64 " written now, %" PRIu64
                    " invalid pointers, %" PRIu64 " VS without fetch shader ",
                    g_fetch_hashes.size(), g_fetch_pairs.size(), g_fetch_written, g_fetch_bad, g_vs_without_fetch);
        std::printf("| %" PRIu64 " written now (%" PRIu64 " bytes), %" PRIu64 " already on disk, %" PRIu64 " without OrbShdr -> %s\n",
                    g_shader_written, g_shader_bytes, g_shader_on_disk, g_shader_not_found, g_shader_dir.string().c_str());
    }
    {
        const auto& renderer = render::VulkanRenderer::Instance();
        std::printf("Vulkan: %s | %" PRIu64 " frames presented, %" PRIu64 " skipped\n", renderer.Describe().c_str(),
                    renderer.FramesPresented(), renderer.FramesSkipped());
        const VulkanDrawStats& v = g_vk_stats;
        u32 usable = 0;
        for (const auto& [hashes, info] : g_game_pair_by_hash) {
            usable += info.usable;
        }
        std::printf("Vulkan draws: %" PRIu64 " draws rendered (all with the game's SPIR-V modules), %.1f MiB of vertices / indices / "
                    "constants copied | pairs: %u usable of %zu seen, %u pipelines\n",
                    renderer.DrawsRendered(), renderer.BytesUploaded() / 1048576.0, usable, g_game_pair_by_hash.size(),
                    renderer.GamePipelines());
        std::printf("  render targets: %u VkImage (color and depth), %u passes in last frame, last frame %s | clears of "
                    "depth %" PRIu64 ", depth-only draws %" PRIu64 ", textures previously used as render targets %" PRIu64 "\n",
                    renderer.RenderTargets(), renderer.LastFramePasses(),
                    renderer.LastFrameShowedTarget() ? "copied a game target to the window" : "NO target to display", v.depth_clears,
                    v.depth_only, v.target_sampled);
        std::printf("  submitted %" PRIu64 " | skipped: %" PRIu64 " pair not yet recompiled, %" PRIu64 " shader without hash, %" PRIu64
                    " vertex format differs from specialization or is unsupported, %" PRIu64 " vertex layout, %" PRIu64
                    " size, %" PRIu64 " primitive, %" PRIu64 " color mode (resolve / fast clear), %" PRIu64 " no target, %" PRIu64
                    " target format, %" PRIu64 " viewport, %" PRIu64 " unreadable memory, %" PRIu64 " refused by backend (%" PRIu64
                    " due to full rings), %" PRIu64 " sampled their own target\n",
                    v.game_sent, v.game_no_pair, v.no_hash, v.game_format, v.game_layout, v.size, v.primitive, v.color_mode, v.no_target,
                    v.target_format, v.viewport, v.unreadable, v.game_rejected, renderer.RingRejected(), renderer.FeedbackSkipped());
        {
            u64 pv = 0, pi = 0, ps = 0; // Fase 54f
            renderer.RingPeaks(&pv, &pi, &ps);
            std::printf("  frame rings (largest frame): vertices %.1f MiB, indices %.1f MiB, constants %.1f MiB (limit: half of each ring)\n",
                        pv / 1048576.0, pi / 1048576.0, ps / 1048576.0);
        }
        std::printf("  rectangle lists drawn halfway (first triangle only): %" PRIu64 " | unreadable buffers (zeros) %" PRIu64
                    ", unsupported textures (white) %" PRIu64 "\n",
                    v.rect_list, v.game_zero_buffers, v.game_white_images);
        const double geometry_total = static_cast<double>(v.cache_hit_bytes + v.cache_upload_bytes + v.ring_bytes);
        std::printf("  geometry cache: %.1f MiB reused from GPU (%.1f%%), %.1f MiB uploaded to static buffer, %.1f MiB "
                    "copied to ring | %" PRIu64 " hits, %" PRIu64 " uploads, %" PRIu64 " changes detected, %" PRIu64
                    " full checks, %" PRIu64 " flushes | static buffer in use: %.1f MiB | draws with more than one "
                    "instance: %" PRIu64 " (%" PRIu64 " with instance step greater than 1; %" PRIu64 " expanded buffers) | "
                    "draws rebased to their first vertex: %" PRIu64 "\n",
                    v.cache_hit_bytes / 1048576.0, geometry_total > 0 ? 100.0 * v.cache_hit_bytes / geometry_total : 0.0,
                    v.cache_upload_bytes / 1048576.0, v.ring_bytes / 1048576.0, v.cache_hits, v.cache_uploads, v.cache_changed,
                    v.cache_rechecks, v.cache_resets, renderer.StaticBytes() / 1048576.0, v.instanced, v.instanced_step, v.instanced_expanded,
                    v.rebased);
        auto print_map = [](const char* title, const std::map<u32, u64>& map) {
            if (map.empty()) {
                return;
            }
            std::printf("  %s:", title);
            for (const auto& [key, draws] : map) {
                std::printf(" 0x%x=%" PRIu64, key, draws);
            }
            std::printf("\n");
        };
        print_map("unsupported vertex formats (data << 4 | numeric type = draws)", v.vertex_formats);
        print_map("unsupported target formats (format << 4 | numeric type = draws)", v.target_formats);
        print_map("unsupported primitives (type = draws)", v.primitives);
        std::printf("  textures: %u VkImage created (%.1f MiB), %" PRIu64 " uploaded, %" PRIu64 " failed; textured draws %" PRIu64 "\n",
                    renderer.TexturesCreated(), renderer.TextureBytes() / 1048576.0, v.textures_uploaded, v.textures_failed,
                    renderer.TexturedDraws());
        std::printf("  Phase 25: %" PRIu64 " mip levels uploaded, %" PRIu64 " reuploads of small textures rewritten by CPU, %" PRIu64
                    " tiny target readbacks\n",
                    v.mip_levels_uploaded, v.textures_refreshed, g_readbacks_delivered.load());
        std::printf("  Phase 34: %" PRIu64 " draws with stencil testing, %" PRIu64 " stencil clears\n", v.stencil_draws, v.stencil_clears);
        std::printf("  Phase 32: %" PRIu64 " draws sampled their own target through a copy (%" PRIu64 " copies made) | %" PRIu64
                    " flattened buffers (flatbuf) built\n",
                    renderer.FeedbackDraws(), renderer.FeedbackCopies(), v.flatbuf_built);
        std::printf("  Phase 29: %" PRIu64 " pipelines created in background\n", renderer.AsyncPipelines());
        std::printf("  Phase 29: %" PRIu64 " pairs loaded in background | live recompiler: %" PRIu64 " pair(s) requested, %" PRIu64
                    " ready, %" PRIu64 " run(s), %" PRIu64 " ms | %" PRIu64 " textures reuploaded because the game rewrote their content\n",
                    g_pairs_async_loaded.load(), g_pairs_live_requested.load(), g_pairs_live_ok.load(), g_live_runs.load(), g_live_ms.load(),
                    v.textures_rewritten);
        std::printf("  Phase 28 (hitches): %" PRIu64 " frames, %" PRIu64 " with Submit > 12 ms (total %.0f ms, worst %.1f ms), %" PRIu64
                    " flip intervals > 50 ms (worst %.1f ms)\n",
                    g_hitch.frames, g_hitch.hitch_frames, g_hitch.hitch_ms, g_hitch.worst_ms, g_hitch.long_intervals,
                    g_hitch.worst_interval_ms);
        std::printf("    total time: pipelines %" PRIu64 " in %.0f ms | textures (VkImage + copy) %" PRIu64 " in %.0f ms | read/detile %"
                    PRIu64 " in %.0f ms | pairs (file + modules) %" PRIu64 " in %.0f ms | extractor (new shader) %" PRIu64 " em %.0f ms\n",
                    g_hitch.kind_count[0], g_hitch.kind_ms[0], g_hitch.kind_count[1], g_hitch.kind_ms[1], g_hitch.kind_count[3],
                    g_hitch.kind_ms[3], g_hitch.kind_count[2], g_hitch.kind_ms[2], g_hitch.kind_count[4], g_hitch.kind_ms[4]);
        { // Fase 69: o resumo por minuto da sessao inteira (inclui o minuto incompleto do fim)
            HitchMinute all = g_minutes_total;
            all.frames += g_minute.frames;
            all.over25 += g_minute.over25;
            all.over50 += g_minute.over50;
            all.gpu_over += g_minute.gpu_over;
            all.all_ms += g_minute.all_ms;
            all.all_submit += g_minute.all_submit;
            all.all_record += g_minute.all_record;
            all.all_gpu += g_minute.all_gpu;
            all.worst_gpu = std::max(all.worst_gpu, g_minute.worst_gpu);
            all.worst = std::max(all.worst, g_minute.worst);
            all.slow_ms += g_minute.slow_ms;
            all.game_ms += g_minute.game_ms;
            AddCost(all.slow, g_minute.slow);
            u64 worker[kHitchKinds];
            for (int k = 0; k < kHitchKinds; ++k) {
                worker[k] = render::HitchClock::worker_ns[k].load();
            }
            const double span = std::chrono::duration<double>(std::chrono::steady_clock::now() - HitchEpoch()).count();
            PrintHitchMinute("HITCH-SESSION", span, span, all, worker);
        }
        {
            u32 warm_done = 0, warm_total = 0;
            renderer.PrewarmProgress(&warm_done, &warm_total);
            std::printf("    Phase 36 (warmup): %u of %u pairs loaded at startup, %u used without waiting | %u of %u pipelines created before use\n",
                        g_warm_pairs_done.load(), g_warm_pairs_total.load(), g_warm_pairs_used.load(), warm_done, warm_total);
        }
        {
            const auto sync = renderer.GetSyncPipelineStats();
            std::printf("    Phase 38 (on demand): %" PRIu64 " pack pairs loaded on render thread (%.0f ms total, worst %.1f ms), %" PRIu64
                        " without compiled output, %" PRIu64 " deferred to background (disk or pack not yet read), %" PRIu64 " due to budget | %" PRIu64 " pipelines created on demand (%.0f ms, worst %.1f ms; worst frame %.1f ms;"
                        " %" PRIu64 " frames with creation), %" PRIu64 " deferred to worker due to budget\n",
                        g_sync_pairs_loaded, g_sync_pairs_ms, g_sync_pairs_worst_ms, g_sync_pairs_missing, g_sync_pairs_deferred, g_sync_pairs_over_budget,
                        sync.created, sync.total_ms,
                        sync.worst_single_ms, sync.worst_frame_ms, sync.frames_with_sync, sync.over_budget);
            u64 fast = 0, upgraded = 0;
            renderer.FastPipelineStats(&fast, &upgraded);
            std::printf("    Phase 56: %" PRIu64 " new pairs translated on demand in the same frame (no pop-in)\n", g_live_sync_pairs);
            std::printf("    Phase 55/56: %" PRIu64 " frames waited for new pipelines (to avoid missing objects) | %" PRIu64
                        " urgent unoptimized pipelines, %" PRIu64 " already replaced by optimized version\n", sync.burst_frames, fast, upgraded);
            u64 lazy_loaded = 0, lazy_failed = 0;
            renderer.LazyModuleStats(&lazy_loaded, &lazy_failed);
            std::printf("    Phase 59: %zu AOT pack pairs registered without modules; %" PRIu64 " received modules on first pipeline, "
                        "%" PRIu64 " failed | %" PRIu64 " textures discarded due to disuse\n",
                        g_lazy_sources.size(), lazy_loaded, lazy_failed, renderer.TexturesEvicted());
            std::printf("    Phase 61 (geometry budget %.1f ms/frame): %" PRIu64 " promotions to static buffer and %" PRIu64
                        " full checks deferred to another frame\n",
                        g_geometry_budget_ms, g_geometry_deferred, g_geometry_rechecks_deferred);
        }
        {
            const auto gpl = renderer.GetGplStats();
            std::printf("    Phase 41 (pipeline libraries): %s | %" PRIu64 " shader libraries in %.0f ms (worst pair %.1f ms), %" PRIu64
                        " failures | %" PRIu64 " pipelines linked in %.1f ms (average %.3f ms, worst %.3f ms), %" PRIu64 " failures | %" PRIu64
                        " vertex input libraries, %" PRIu64 " output libraries | %" PRIu64 " monolithic in %.0f ms (worst %.1f ms)\n",
                        gpl.active ? "ligadas" : "desligadas", gpl.shader_libs, gpl.shader_lib_ms, gpl.shader_lib_worst_ms, gpl.lib_failures, gpl.links,
                        gpl.link_ms, gpl.links ? gpl.link_ms / gpl.links : 0.0, gpl.link_worst_ms, gpl.link_failures, gpl.vertex_libs, gpl.output_libs,
                        gpl.monolithic, gpl.monolithic_ms, gpl.monolithic_worst_ms);
        }
        {
            const auto uploads = renderer.GetUploadStats();
            std::printf("    Phase 39 (on-demand textures): %" PRIu64 " created on render thread (%.0f ms total, worst %.1f ms; worst frame %.1f ms; %"
                        PRIu64 " frames, %" PRIu64 " of them with peaks over %u new textures), %" PRIu64 " deferred to worker due to budget | uploads in same frame %" PRIu64 " (%.0f MiB), with waiting %" PRIu64 "\n",
                        g_tex_sync_count, g_tex_sync_ms, g_tex_sync_worst_ms, g_tex_sync_worst_frame_ms, g_tex_sync_frames, g_tex_spike_frames,
                        g_tex_spike_count, g_tex_sync_deferred,
                        uploads.deferred, uploads.deferred_bytes / 1048576.0, uploads.immediate);
        }
        {
            u64 modules = 0, layouts = 0;
            renderer.GetSharedObjectCounts(&modules, &layouts);
            std::printf("    Phase 43 (full pack at startup): %u of %u variants ready in %.1f s (%u failures), memory %llu MB | %" PRIu64
                        " used in game, %" PRIu64 " refused (input map / format) | %llu modules and %llu layouts shared\n",
                        g_ready_done.load() - g_ready_failed.load(), g_ready_total.load(), g_ready_seconds, g_ready_failed.load(),
                        static_cast<unsigned long long>(g_ready_memory_mb), g_ready_used.load(), g_ready_refused.load(),
                        static_cast<unsigned long long>(modules), static_cast<unsigned long long>(layouts));
        }
        std::printf("    Phase 37 (AOT pack): %" PRIu64 " pairs loaded from pack (%zu variants in it, index in %.0f ms) | texture type "
                    "confirmed in %" PRIu64 ", wrong in %" PRIu64 " (recompiled live)\n",
                    g_aot_loaded.load(), g_aot.pairs.size(), g_aot.load_ms, g_aot_confirmed.load(), g_aot_rejected.load());
        std::printf("    Phase 45 (end-of-frame wait): textures %" PRIu64 " em %" PRIu64 " frames, %.0f ms (worst %.1f ms), %" PRIu64
                    " draws still without textures at deadline | pipelines: see Phase 38 (\"deferred to worker\" = draws without pipelines at deadline)\n",
                    g_tex_wait_textures, g_tex_wait_frames, g_tex_wait_ms, g_tex_wait_worst_ms, renderer.DrawsLateTexture());
        std::printf("    Phase 35: %" PRIu64 " textures read off render thread; %" PRIu64 " draws deferred waiting for textures\n",
                    g_tex_async_uploaded.load(), v.texture_pending);
        std::printf("    Phase 35: DMA_DATA %.1f MiB fill + %.1f MiB copy; %.1f MiB done in parallel, in %" PRIu64 " batches\n",
                    g_dma_fill_bytes / 1048576.0, g_dma_copy_bytes / 1048576.0, g_dma_parallel_bytes / 1048576.0, g_dma_batches);
        std::printf("    DMA_DATA: %" PRIu64 " packets in %.0f ms\n", g_hitch.kind_count[5], g_hitch.kind_ms[5]);
        std::printf("    Submit: %.0f ms total (%.2f ms per frame) | draw stages: state %.0f ms, pair %.0f ms, resources %.0f ms, "
                    "queue %.0f ms\n",
                    g_hitch.submit_ms, g_hitch.frames ? g_hitch.submit_ms / g_hitch.frames : 0.0, g_hitch.lap_ms[0], g_hitch.lap_ms[1],
                    g_hitch.lap_ms[2], g_hitch.lap_ms[3]);
        std::map<std::string, u32> reasons;
        for (const auto& [hashes, info] : g_game_pair_by_hash) {
            if (!info.usable) {
                ++reasons[info.why];
            }
        }
        for (const auto& [why, pairs] : reasons) {
            std::printf("    %u pair(s) skipped: %s\n", pairs, why.c_str());
        }
        if (renderer.ValidationActive()) {
            std::printf("  Vulkan validation: %" PRIu64 " errors, %" PRIu64 " warnings\n", renderer.ValidationErrors(),
                        renderer.ValidationWarnings());
        }
        WriteUiShaderPairs();
    }
    std::vector<std::pair<u64, u32>> ops;
    for (u32 i = 0; i < 256; ++i) {
        if (g_opcode_count[i]) {
            ops.emplace_back(g_opcode_count[i], i);
        }
    }
    std::sort(ops.rbegin(), ops.rend());
    for (const auto& [count, opcode] : ops) {
        const char* name = OpcodeName(opcode);
        std::printf("  %10" PRIu64 "x  0x%02X %s\n", count, opcode, name ? name : "(unnamed)");
    }
}

// Liga o extrator de shaders; `directory` recebe um arquivo por shader distinto.
void FlushShaderDumps() {
    FlushFileWrites();
}

void SetShaderDumpDirectory(const std::filesystem::path& directory) {
    std::lock_guard lock(g_shader_mutex);
    g_shader_dir = directory;
    // fetch_index.tsv so cresce: os pares VS <-> fetch shader de todas as execucoes ficam no
    // mesmo arquivo (a ferramenta de recompilacao ignora os repetidos). Ate a Fase 14 ele era
    // renomeado a cada execucao, e duas execucoes curtas seguidas apagavam os pares da maratona.
}

// Liga / desliga o envio dos draws do jogo ao Vulkan (--no-vulkan-draws: so limpa a tela).
// Liga / desliga o uso dos modulos SPIR-V do jogo (--no-game-shaders: so os shaders base).
void SetSrgbDisplay(bool enabled) {
    g_srgb_display = enabled;
}

void WarmThread() {
    auto& renderer = render::VulkanRenderer::Instance();
    for (int i = 0; i < 600 && !renderer.GameShadersSupported(); ++i) { // o Vulkan nasce com a janela do jogo
        Sleep(100);
    }
    if (!renderer.GameShadersSupported()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    struct Entry {
        u64 vs, ps, signature;
        std::array<u32, 32> cntl;
        u64 canon;
        std::array<u32, 16> vformats;
        u32 vformat_count;
    };
    std::vector<Entry> entries;
    if (std::FILE* f = _wfopen(WarmFile().c_str(), L"r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            Entry e{};
            char* p = line;
            char* end = nullptr;
            e.vs = std::strtoull(p, &end, 16);
            p = end;
            e.ps = std::strtoull(p, &end, 16);
            p = end;
            e.signature = std::strtoull(p, &end, 16);
            bool ok = end != p && e.vs && e.ps;
            p = end;
            for (u32 i = 0; ok && i < 32; ++i) {
                e.cntl[i] = static_cast<u32>(std::strtoul(p, &end, 16));
                ok = end != p;
                p = end;
            }
            while (ok && *p == ' ') {
                ++p;
            }
            if (ok && *p == 'c') { // Fase 37: par do pacote AOT
                e.canon = std::strtoull(p + 1, &end, 16);
                p = end;
                e.vformat_count = std::min<u32>(16, static_cast<u32>(std::strtoul(p, &end, 10)));
                p = end;
                for (u32 i = 0; i < e.vformat_count; ++i) {
                    e.vformats[i] = static_cast<u32>(std::strtoul(p, &end, 16));
                    p = end;
                }
            }
            if (ok) {
                std::lock_guard lock(g_pair_mutex);
                if (g_pair_warm_saved.insert({e.vs, e.ps, e.signature}).second) {
                    entries.push_back(e);
                }
            }
        }
        std::fclose(f);
    }
    g_warm_pairs_total = static_cast<u32>(entries.size());
    for (const Entry& e : entries) {
        WarmPair warm;
        warm.info.canon_signature = e.canon;
        warm.info.vformats = e.vformats;
        warm.info.vformat_count = e.vformat_count;
        LoadGamePair(e.vs, e.ps, e.signature, e.cntl.data(), &warm.info);
        if (warm.info.usable) {
            std::lock_guard lock(g_pair_mutex);
            g_pair_warm[{e.vs, e.ps, e.signature}] = std::move(warm);
        }
        ++g_warm_pairs_done;
    }
    const u32 pipelines = renderer.PrewarmPipelines();
    g_warm_finished = true;
    log::Tagged("WARMUP", "%zu pairs loaded and %u pipelines queued in %.1f s", entries.size(), pipelines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

void SetAot(bool enabled, bool first) {
    g_aot_enabled = enabled;
    g_aot_first = first;
}

// Liga o aquecimento (padrao): thread que carrega os pares e pipelines ja vistos em execucoes anteriores.

void StartShaderWarmup(bool enabled) {
    g_warm_enabled = enabled;
    auto& renderer = render::VulkanRenderer::Instance();
    if (!enabled || render::GameSpirvDirectory().empty()) {
        return;
    }
    const char* states = std::getenv("LD_PIPE_STATES"); // Fase 53: so para testes (outro arquivo de estados)
    renderer.SetPipelineStateFile(states ? std::string(states)
                                         : (std::filesystem::path(render::GameSpirvDirectory()) / "pipeline_states.txt").string());
    std::thread(WarmThread).detach();
}

// Fase 38: le o indice do pacote AOT numa thread propria logo na abertura (~160 ms); senao quem paga
// e a thread de render, no primeiro par que faltar.
// Fase 43: carrega todas as variantes do pacote (6 threads). Chamado na thread da abertura do pacote.
void PreloadAllAot() {
    auto& renderer = render::VulkanRenderer::Instance();
    for (int i = 0; i < 600 && !renderer.GameShadersSupported(); ++i) { // o Vulkan nasce com a janela
        Sleep(100);
    }
    std::vector<AotKey> keys;
    {
        std::lock_guard lock(g_aot.mutex);
        keys.reserve(g_aot.pairs.size());
        for (const auto& [key, blobs] : g_aot.pairs) {
            keys.push_back(key);
        }
    }
    g_ready_total = static_cast<u32>(keys.size());
    if (!renderer.GameShadersSupported() || keys.empty()) {
        g_ready_finished = true;
        return;
    }
    renderer.SetModuleLoader(&LoadLazyAotModules); // Fase 59
    const auto start = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0};
    auto worker = [&] {
        std::array<u32, 32> cntl{};
        for (u32 i = 0; i < 32; ++i) {
            cntl[i] = i; // mapa padrao: entrada N do PS <- parametro N do VS
        }
        t_aot_preload = true;
        for (size_t i; (i = next.fetch_add(1)) < keys.size();) {
            const AotKey& key = keys[i];
            GamePairInfo info;
            info.canon_signature = key.signature;
            info.vformat_count = 16;
            info.vformats.fill(0x77FAC); // formato-modelo (swizzle identidade); o draw poe o real
            LoadGamePair(key.vs, key.ps, key.signature, cntl.data(), &info);
            if (info.usable && info.aot) {
                std::lock_guard lock(g_ready_mutex);
                g_aot_ready.emplace(key, std::move(info));
            } else {
                ++g_ready_failed;
            }
            ++g_ready_done;
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    g_ready_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        g_ready_memory_mb = memory.PrivateUsage >> 20;
    }
    u64 modules = 0, layouts = 0;
    renderer.GetSharedObjectCounts(&modules, &layouts);
    log::Tagged("AOT", "full pack loaded: %u of %u variants ready in %.1f s (%u failures) | %llu modules and %llu layouts in Vulkan | "
                "process memory %llu MB", g_ready_done.load() - g_ready_failed.load(), g_ready_total.load(), g_ready_seconds,
                g_ready_failed.load(), static_cast<unsigned long long>(modules), static_cast<unsigned long long>(layouts),
                static_cast<unsigned long long>(g_ready_memory_mb));
    // Fase 48: os pipelines ja vistos dos pares do pacote (registrados so agora) tambem entram no
    // aquecimento; a tela de carregamento espera por eles junto com os outros.
    if (g_warm_enabled.load()) { // --no-warmup: nada de pipelines na abertura
        for (int i = 0; i < 600 && !g_warm_finished.load(); ++i) {
            Sleep(50); // a primeira passada do aquecimento precisa ter lido o arquivo
        }
        const u32 extra = renderer.PrewarmPipelines();
        log::Tagged("AOT", "%u previously seen pack-pair pipelines queued for warmup", extra);
    }
    if (const char* bench = std::getenv("LD_PIPE_BENCH")) { // Fase 53: so medicao
        renderer.BenchPipelines(bench);
    }
    g_ready_finished = true;
}

void PreloadAotIndex() {
    if (!g_aot_enabled.load() || render::GameSpirvDirectory().empty()) {
        g_aot_prefetch_finished = true;
        g_ready_finished = true;
        return;
    }
    std::thread([] {
        {
            std::lock_guard lock(g_aot.mutex);
            if (!g_aot.loaded) {
                AotLoadIndex();
            }
        }
        // pares compilados com o jogo: uma listagem da pasta
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::path(render::GameSpirvDirectory()) / "pairs", ec), end; !ec && it != end;
             it.increment(ec)) {
            unsigned long long a = 0, b = 0, c = 0;
            const std::string file = it->path().filename().string();
            if (file.size() > 4 && file.compare(file.size() - 4, 4, ".txt") == 0 &&
                std::sscanf(file.c_str(), "pair_%16llx_%16llx_%16llx.txt", &a, &b, &c) == 3) {
                g_pairs_on_disk.insert({a, b, c});
            }
        }
        g_pairs_listed.store(true, std::memory_order_release);
        // pacote inteiro, em sequencia: fica no cache de arquivos do Windows e, com a carga completa
        // (Fase 48), tambem na memoria do processo ate ela terminar. Sem a carga completa, prioridade
        // de E/S baixa (o jogo ja esta rodando).
        const bool keep = g_preload_all.load();
        const auto start = std::chrono::steady_clock::now();
        if (!keep) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        }
        u64 bytes = 0;
        if (std::FILE* f = _wfopen((AotDirectory() / "aot_pairs.pak").c_str(), L"rb")) {
            std::error_code size_ec;
            g_aot_prefetch_total = std::filesystem::file_size(AotDirectory() / "aot_pairs.pak", size_ec);
            std::vector<u8> memory;
            if (keep) {
                try {
                    memory.resize(g_aot_prefetch_total);
                } catch (const std::bad_alloc&) {
                    memory.clear(); // sem memoria: le para o cache, como antes
                }
            }
            std::vector<u8> chunk(memory.empty() ? (8u << 20) : 0);
            for (;;) {
                const size_t want = std::min<u64>(8u << 20, memory.empty() ? chunk.size() : memory.size() - bytes);
                if (!want) {
                    break;
                }
                const size_t got = std::fread(memory.empty() ? chunk.data() : memory.data() + bytes, 1, want, f);
                if (!got) {
                    break;
                }
                bytes += got;
                g_aot_prefetch_done = bytes;
            }
            std::fclose(f);
            if (!memory.empty() && bytes == memory.size()) {
                std::unique_lock lock(g_aot_pak_memory_mutex);
                g_aot_pak_memory = std::move(memory);
            }
        }
        g_aot_prefetch_finished = true;
        if (!keep) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }
        g_aot_prefetched.store(bytes > 0, std::memory_order_release);
        log::Tagged("AOT", "pack read %s: %.0f MB in %.1f s | %zu pairs compiled with the game listed",
                    g_aot_pak_memory.empty() ? "to file cache" : "to memory", bytes / 1048576.0,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), g_pairs_on_disk.size());
        if (g_preload_all.load()) {
            PreloadAllAot();
            if (!g_lazy_aot.load()) { // Fase 59: com os modulos sob demanda o pacote fica (e de onde eles saem)
                std::unique_lock lock(g_aot_pak_memory_mutex); // Fase 48: devolve os ~700 MB
                std::vector<u8>().swap(g_aot_pak_memory);
            }
        } else {
            g_ready_finished = true;
        }
    }).detach();
}

void SetTextureWait(bool enabled) {
    g_tex_wait_frame = enabled;
}

void SetParallelTextures(bool enabled) { // Fase 59
    g_tex_parallel = enabled;
}

void SetStallStacks(bool enabled) { // Fase 59
    g_stall_stacks = enabled;
}

void SetLazyAot(bool enabled) { // Fase 59
    g_lazy_aot = enabled;
}

// Fase 59 (selftest): o destile por microtile da o mesmo resultado que a formula elemento por elemento,
// inclusive com microtiles cortados na borda e com memoria do jogo lida direto.
bool SelftestDetile() {
    for (const u32 element : {8u, 16u}) {
        for (const auto [ew, eh] : {std::pair{16u, 16u}, std::pair{13u, 9u}, std::pair{5u, 3u}, std::pair{64u, 24u}, std::pair{1u, 1u}}) {
            const u32 pitch = (ew + 7) & ~7u, height = (eh + 7) & ~7u;
            std::vector<u8> source(static_cast<size_t>(pitch) * height * element);
            for (size_t i = 0; i < source.size(); ++i) {
                source[i] = static_cast<u8>(i * 131 + (i >> 7));
            }
            std::vector<u8> fast(static_cast<size_t>(ew) * eh * element), slow(fast.size());
            if (!DetileLevel(reinterpret_cast<u64>(source.data()), source.size(), 1, fast.data(), ew, eh, element, false, pitch)) {
                return false;
            }
            for (u32 y = 0; y < eh; ++y) {
                for (u32 x = 0; x < ew; ++x) {
                    const u32 ix = x & 7, iy = y & 7;
                    const u32 inside = (ix & 1) | ((iy & 1) << 1) | ((ix & 2) << 1) | ((iy & 2) << 2) | ((ix & 4) << 2) | ((iy & 4) << 3);
                    const size_t src = (static_cast<size_t>((y / 8) * (pitch / 8) + x / 8) * 64 + inside) * element;
                    std::memcpy(slow.data() + (static_cast<size_t>(y) * ew + x) * element, source.data() + src, element);
                }
            }
            if (fast != slow) {
                return false;
            }
        }
    }
    return true;
}

void SetSyncTextures(bool enabled, double budget_ms, double spike_budget_ms, u32 spike_count) {
    g_sync_textures = enabled;
    g_tex_budget_ms = budget_ms;
    g_tex_spike_budget_ms = spike_budget_ms;
    g_tex_spike_count = spike_count;
}

void SetPreloadAllAot(bool enabled) {
    g_preload_all = enabled;
}

void SetButtonIcons(const char* name) { // Fase 62: "auto" ou uma das plataformas
    g_icons_forced = IconIndex(name);
}

void SetPairBudget(double budget_ms) {
    g_pair_budget_ms = budget_ms;
}

void SetGeometryBudget(double budget_ms) { // Fase 61
    g_geometry_budget_ms = budget_ms;
}

void SetLiveSyncBudget(double ms) {
    g_live_sync_budget_ms = ms;
}

void SetSyncShaders(bool enabled, double pipeline_budget_ms) {
    g_sync_pairs = enabled;
    render::VulkanRenderer::Instance().SetSyncPipelines(enabled, pipeline_budget_ms);
}

// Fase 42: progresso para a tela de carregamento (antes de o jogo comecar).
void GetLoadingStatus(LoadingStatus* out) {
    out->pack_done = g_aot_prefetch_done.load();
    out->pack_total = g_aot_prefetch_total.load();
    out->pack_finished = g_aot_prefetch_finished.load();
    out->pairs_done = g_warm_pairs_done.load();
    out->pairs_total = g_warm_pairs_total.load();
    out->pairs_finished = g_warm_finished.load() || !g_warm_enabled.load();
    out->aot_done = g_ready_done.load();
    out->aot_total = g_ready_total.load();
    out->aot_finished = g_ready_finished.load();
    render::VulkanRenderer::Instance().PrewarmProgress(&out->pipelines_done, &out->pipelines_total);
    // Fase 72: a tela de carregamento espera so os pipelines da tela de abertura e das areas de entrada / ultima da
    // sessao anterior (o resto continua na fila, abaixo do normal). Sem sessao anterior registrada, espera tudo.
    // Medido na RX 6600 com o cache do driver vazio: 49,3 s -> 6,2 s, sem quadro esperando pipeline no jogo.
    // LD_PREWARM_GATE=tudo volta a esperar todos.
    static const bool area_gate = [] {
        const char* env = std::getenv("LD_PREWARM_GATE");
        return !(env && !std::strcmp(env, "tudo"));
    }();
    if (area_gate) {
        u32 done = 0, total = 0;
        render::VulkanRenderer::Instance().PrewarmPriorityProgress(&done, &total);
        if (total) {
            out->pipelines_done = done;
            out->pipelines_total = total;
        }
    }
}

// Texto curto para o titulo da janela enquanto o aquecimento roda ("" quando nao ha o que mostrar).
void WarmupStatus(char* text, size_t size) {
    u32 done = 0, total = 0;
    render::VulkanRenderer::Instance().PrewarmProgress(&done, &total);
    const u32 pairs_total = g_warm_pairs_total.load(), pairs_done = g_warm_pairs_done.load();
    if (!pairs_total || (g_warm_finished.load() && done >= total)) {
        text[0] = 0;
        return;
    }
    std::snprintf(text, size, lang::T(" | Compilando shaders: %u / %u", " | Compiling shaders: %u / %u"), pairs_done + done, pairs_total + total);
}

void SetAsyncTextures(bool enabled) {
    g_async_textures = enabled;
}

void SetParallelDma(bool enabled) { // experimental: lote em paralelo, com as consultas de memoria
    g_dma_parallel = enabled;
}

void SetAsyncPairs(bool enabled) {
    g_pair_async = enabled;
}

void SetTextureRefresh(bool enabled) {
    g_texture_refresh = enabled;
}

// Caminho do shader_compiler_tool.exe e pasta de trabalho dele (a raiz do projeto). Vazio = desligado.
void SetLiveCompiler(const std::filesystem::path& tool, const std::filesystem::path& root, const std::filesystem::path& shader_dir) {
    std::error_code ec;
    if (!tool.empty() && std::filesystem::exists(tool, ec)) {
        g_live_tool = tool;
        g_live_root = root;
        g_live_shader_dir = shader_dir;
        log::Tagged("SHADER-LIVE", "live recompiler: %s", tool.string().c_str());
        // Fase 56: uma execucao vazia na abertura poe o executavel e o indice de shaders no cache do
        // Windows (o D: e um HD): a 1a traducao na hora levava 1,8 s, as seguintes 0,1 a 0,3 s.
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            RunLiveCompiler({});
        }).detach();
    } else {
        g_live_tool.clear();
        if (!tool.empty()) {
            log::Tagged("SHADER-LIVE", "disabled: %s does not exist (new pairs deferred to next tool run)",
                        tool.string().c_str());
        }
    }
}

void SetHitchLog(bool enabled) {
    g_hitch_log = enabled;
}

void SetReadback(bool enabled) {
    g_readback_enabled = enabled;
}

void SetGeometryCache(bool enabled) {
    g_geometry_cache_enabled = enabled;
}

void SetTargetLog(bool enabled) {
    g_target_diagnostics = enabled;
}

void SetGameShaders(bool enabled) {
    g_game_shaders = enabled;
}

void SetVulkanDraws(bool enabled) {
    g_vulkan_draws = enabled;
}

// Pede um dump do frame que estiver na tela aos `seconds` de execucao.
void AddFrameDumpAt(u32 seconds) {
    const render::TimedLock lock(g_mutex); // Fase 69
    g_forced_dumps_ms.push_back(seconds * 1000ll);
}

// Pede um perfil de CPU (amostragem de 4 s) aos `seconds` de execucao. A thread que faz o
// perfil e a do watchdog; ela e criada aqui se ainda nao existir.
void AddProfileAt(u32 seconds) {
    g_profile_at_ms.push_back(seconds * 1000ll);
    if (!g_watchdog_started.exchange(true)) {
        std::thread(CutsceneWatchdogThread).detach();
    }
}

// Liga a thread de vigilancia mesmo sem watchdog de cutscene (perfil automatico de janela lenta).
void StartPerfThread() {
    if (!g_watchdog_started.exchange(true)) {
        std::thread(CutsceneWatchdogThread).detach();
    }
}

// Liga o watchdog de cutscene: relatorio quando a contagem de draws fica igual por `seconds`.
void SetCutsceneWatchdog(u32 seconds) {
    g_watchdog_seconds = seconds;
    if (seconds && !g_watchdog_started.exchange(true)) {
        std::thread(CutsceneWatchdogThread).detach();
    }
}

// Liga o dump de telas paradas; `directory` recebe um .txt por tela e as texturas brutas.
void SetFrameDumpDirectory(const std::filesystem::path& directory) {
    const render::TimedLock lock(g_mutex); // Fase 69
    g_dump_dir = directory;
}

u64 LabelWrites() {
    return g_label_writes;
}

} // namespace hle::gnm
