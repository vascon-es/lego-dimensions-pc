// Backend grafico base em Vulkan (Fase 16): instancia, superficie Win32, dispositivo, swapchain
// e um frame que so limpa a tela e apresenta. Ainda nao desenha nada do jogo.
//
// A biblioteca do Vulkan (vulkan-1.dll, instalada pelo driver da placa de video) e carregada em
// tempo de execucao: o projeto nao depende do Vulkan SDK para ligar, so dos cabecalhos.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"

namespace render {

// Fase 69: a thread que processa os pacotes do jogo e grava o frame (marcada no Submit). So o tempo dela
// trava o quadro; o das threads de textura / pipeline / pares corre em paralelo.
inline thread_local bool t_render_thread = false;
inline void MarkRenderThread() { t_render_thread = true; }

// Fase 28: de onde vem os engasgos. Cada categoria soma o tempo gasto desde o ultimo flip; quem
// fecha o frame (sceGnmSubmitAndFlipCommandBuffers) le e zera.
// Fase 69: so a thread de render entra em ns/count (antes a leitura/destile e o envio das threads de textura
// caiam nas mesmas categorias e pareciam engasgo); as outras threads vao para worker_ns/worker_count.
// +kLive (tradutor de shaders rodando na hora, Fase 56) e kLock (espera por um mutex do backend, TimedLock).
struct HitchClock {
    enum Kind { kPipeline, kTexture, kPair, kDetile, kExtract, kDma, kGeometry, kTarget, kReady, kLive, kLock, kKinds }; // Fase 59: +geometria, alvos, pares prontos
    static inline std::atomic<u64> ns[kKinds]{};
    static inline std::atomic<u32> count[kKinds]{};
    static inline std::atomic<u64> worker_ns[kKinds]{};
    static inline std::atomic<u64> worker_count[kKinds]{};
    Kind kind;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit HitchClock(Kind k) : kind(k) {}
    ~HitchClock() { Add(kind, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); }
    static void Add(Kind kind, u64 elapsed_ns) {
        if (t_render_thread) {
            ns[kind] += elapsed_ns;
            ++count[kind];
        } else {
            worker_ns[kind] += elapsed_ns;
            ++worker_count[kind];
        }
    }
};

// Fase 69: lock de um mutex compartilhado com as threads de trabalho (g.mutex, bibliotecas, pool de texturas).
// Tenta sem esperar; so quando outra thread esta com ele a espera e cronometrada (HitchClock::kLock).
class TimedLock {
public:
    explicit TimedLock(std::mutex& mutex) : lock_(mutex, std::try_to_lock) {
        if (!lock_.owns_lock()) {
            const auto start = std::chrono::steady_clock::now();
            lock_.lock();
            HitchClock::Add(HitchClock::kLock,
                            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }
    }
    TimedLock(const TimedLock&) = delete;
    TimedLock& operator=(const TimedLock&) = delete;
    void unlock() { lock_.unlock(); }
    void lock() {
        const auto start = std::chrono::steady_clock::now();
        lock_.lock();
        HitchClock::Add(HitchClock::kLock, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }

private:
    std::unique_lock<std::mutex> lock_;
};

// Um draw do jogo ja traduzido para o que o backend sabe desenhar (Fase 17): geometria com
// posicao e, se houver, cor por vertice; sem texturas. Os ponteiros so precisam valer durante
// a chamada de QueueDraw, que copia tudo para os buffers do Vulkan.
// Pixels de uma textura do jogo ja fora do tiling do PS4 (nivel 0), prontos para o Vulkan.
struct TextureData {
    enum Format : u32 { kBc1, kBc3, kRgba8, kBc2, kBc4, kBc5, kBc7, kRgba16F, kRgba16, kRgba32F, kFormatCount };
    Format format = kRgba8;
    u32 width = 0, height = 0;
    const void* data = nullptr; // blocos BC em ordem linear, ou RGBA8 linha a linha
    u64 bytes = 0;
    u32 depth = 1;   // > 1: textura 3D, fatias uma depois da outra
    bool cube = false; // cubemap: as 6 faces uma depois da outra em `data`
    bool srgb = false; // o descritor pede decodificacao sRGB na amostragem
    u32 swizzle = 0; // dst_sel do descritor T# (3 bits por componente: 0, 1, R=4, G=5, B=6, A=7)
    // Fase 25: cadeia de mipmaps. `data` traz os niveis um depois do outro (cada nivel com todas as
    // faces / fatias); level_offsets[n] e onde o nivel n comeca.
    u32 levels = 1;
    u64 level_offsets[16] = {};
};

struct DrawCall {
    enum PositionFormat : u32 { kFloat3, kFloat4, kHalf4 };
    enum UvFormat : u32 { kUvNone, kUvFloat2, kUvHalf2 };
    enum Primitive : u32 { kTriangleList, kTriangleStrip, kQuadList, kTriangleFan, kLineList, kLineStrip, kPointList };

    const void* vertex_data = nullptr; // `vertex_count` vertices de `stride` bytes
    u32 vertex_count = 0;
    u32 stride = 0;
    u32 position_offset = 0;
    PositionFormat position_format = kFloat3;
    bool has_color = false; // RGBA8 normalizado no mesmo buffer
    u32 color_offset = 0;
    const void* index_data = nullptr; // nullptr = indices 0..vertex_count-1
    u32 index_count = 0;
    u32 index_size = 2; // 2 ou 4 bytes
    Primitive primitive = kTriangleList;
    float mvp[16] = {}; // v_clip = v * mvp (vetor-linha), como os buffers de constantes do jogo
    float viewport[4] = {0, 0, 1920, 1080}; // x, y, largura, altura em pixels do alvo do jogo
    float target_size[2] = {1920, 1080};    // tamanho do alvo do jogo (para escalar ao swapchain)
    bool blend = true; // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    UvFormat uv_format = kUvNone; // coordenada de textura no mesmo buffer da posicao
    u32 uv_offset = 0;
    u64 texture_key = 0; // 0 = sem textura (branco); senao, chave de uma textura ja enviada
};

// ---- shaders do jogo (Fase 19) ----
// Um par (vertex shader, pixel shader) do jogo recompilado para SPIR-V, com a lista de bindings
// do descriptor set 0 na ordem em que o recompilador os numerou.
struct GamePairLayout {
    enum Kind : u32 { kBuffer, kImage, kSampler };
    struct Binding {
        u32 binding;
        Kind kind;
    };
    std::vector<u32> vs_spirv, ps_spirv;
    std::vector<Binding> bindings;
};

// Um draw desenhado com os shaders do proprio jogo (Fases 19 e 20): vertices no layout do fetch
// shader (um ou mais buffers), os buffers de constantes do jogo copiados para storage buffers,
// texturas e samplers separados, o bloco de push constants (120 bytes) e, desde a Fase 20, os
// alvos de render (cor e profundidade) e o estado de blend / profundidade / cull lidos do GNM.
struct GameDraw {
    enum Format : u32 {
        kF32x1, kF32x2, kF32x3, kF32x4, kF16x2, kF16x4, kUnorm8x4, kSnorm8x4,
        kUnorm16x2, kUnorm16x4, kSnorm16x2, kSnorm16x4, kUint8x4, kSint8x4, kUint16x4, kSint16x4,
        kUint32x1, kUnorm10x3A2, kF11F11F10, kUint16x2, kSint16x2,
        kUnorm8x4Bgra, // Fase 37: swizzle BGRA do V# feito pela entrada de vertice (pares do pacote AOT)
        kFormatCount
    };
    enum TargetFormat : u32 {
        kTargetR8, kTargetRg8, kTargetRgba8, kTargetRgba8Srgb, kTargetR16, kTargetRg16, kTargetRgba16, kTargetR16F, kTargetRg16F,
        kTargetRgba16F, kTargetR32F, kTargetRgba32F, kTargetR11G11B10F, kTargetA2Rgb10, kTargetDepth32F, kTargetFormatCount
    };
    struct Attribute {
        u32 location;
        Format format;
        u32 offset;
        u32 binding; // indice em `bindings`
    };
    struct VertexBinding {
        const void* data; // copiado para o anel do frame; nullptr = ja esta no buffer estatico
        u32 stride;
        u64 bytes;
        u64 static_offset = ~0ull; // posicao devolvida por StaticUpload (cache de geometria)
        bool per_instance = false; // atributos lidos uma vez por instancia
    };
    struct Buffer {
        u32 binding;
        const void* data; // nullptr = buffer de zeros
        u32 bytes;
    };
    struct Image {
        u32 binding;
        u64 texture_key;    // textura enviada por UploadTexture (0 = nenhuma)
        u64 target_address; // senao: alvo de render do jogo com este endereco (0 = nenhum)
        u32 view_type;      // o que o shader declara: 0 = 2D, 1 = cubo, 2 = array 2D, 3 = 3D
        bool pending;       // Fase 45: textura ainda sendo lida; o DrawFrame resolve (ou deixa o draw de fora)
    };
    struct Sampler {
        u32 binding;
        u32 wrap_u, wrap_v; // 0 = repete, 1 = espelha, 2 = prende no ultimo texel, 3 = cor de borda
        u32 border = 0;     // cor de borda: 0 = preto transparente, 1 = preto opaco, 2 = branco opaco
        bool point;
        u32 compare; // 0 = sem comparacao; senao funcao de comparacao do GNM (1..7) para sombras
        // Fase 25 (mipmaps): dword 1 do S# (LOD minimo nos bits 0..11, maximo nos 12..23, ponto fixo
        // 4.8) e dword 2 (bias de LOD nos bits 0..13, filtro de mip nos bits 26..27).
        u32 lod = 0, filter = 0;
    };
    struct Target {
        u64 address = 0;
        u32 width = 0, height = 0;
        TargetFormat format = kTargetRgba8;
    };
    u64 pair = 0;
    VertexBinding bindings[8]{};
    u32 binding_count = 0;
    Attribute attributes[16]{};
    u32 attribute_count = 0;
    u32 vertex_count = 0;             // sem indices: quantos vertices desenhar
    const void* index_data = nullptr; // nullptr = sem indices (ou indices no buffer estatico)
    u64 index_static_offset = ~0ull;  // indices ja no buffer estatico (cache de geometria)
    u32 index_count = 0, index_size = 2;
    s32 base_vertex = 0;
    // Os buffers de vertices copiados para o anel (data != nullptr, por vertice) comecam neste
    // vertice, nao no 0: so o trecho entre o menor e o maior indice do draw e copiado.
    u32 first_vertex = 0;
    u32 instance_count = 1;
    DrawCall::Primitive primitive = DrawCall::kTriangleList;
    Buffer buffers[24]{};
    u32 buffer_count = 0;
    Image images[16]{};
    u32 image_count = 0;
    Sampler samplers[16]{};
    u32 sampler_count = 0;
    u8 push[120] = {};
    // ---- alvos e estado (Fase 20) ----
    Target colors[8]{};
    u32 color_count = 0;
    bool has_depth = false;
    Target depth{};
    bool clear_depth = false; // DB_RENDER_CONTROL.depth_clear_enable: limpa antes do draw
    float clear_depth_value = 1.0f;
    u32 blend[8] = {};      // CB_BLENDn_CONTROL cru
    u32 color_mask = 0xF;   // CB_TARGET_MASK (4 bits por alvo)
    u32 depth_control = 0;  // DB_DEPTH_CONTROL cru
    // Fase 34: estencil. DB_STENCIL_CONTROL cru (0 = teste desligado), DB_STENCILREFMASK da frente e
    // de tras (valor de teste, mascara, mascara de escrita, valor de operacao) e a limpeza pedida.
    u32 stencil_control = 0;
    u32 stencil_ref[2] = {};
    bool clear_stencil = false;
    u32 clear_stencil_value = 0;
    u32 cull = 0;           // bit 0 = descarta frente, bit 1 = descarta verso, bit 2 = frente em sentido horario
    bool depth_clamp = true;
    float blend_constants[4] = {};
    float viewport[6] = {0, 0, 1920, 1080, 0, 1}; // x, y, largura, altura (com sinal), minZ, maxZ - em pixels do alvo
    s32 scissor[4] = {0, 0, 16384, 16384};       // x0, y0, x1, y1
};

class VulkanRenderer {
public:
    // Shaders do jogo: o dispositivo tem os recursos que os modulos recompilados declaram?
    bool GameShadersSupported() const;
    bool HasGamePair(u64 key) const;
    // Cria os dois VkShaderModule, o layout de descritores e o pipeline layout do par.
    bool RegisterGamePair(u64 key, const GamePairLayout& layout);
    // Fase 59: par registrado so com os layouts; os modulos sao pedidos ao carregador no primeiro pipeline
    // dele (thread de pipeline). false = sem carregador / GPL ligado (use RegisterGamePair).
    using ModuleLoader = bool (*)(u64 pair_key, std::vector<u32>* vs, std::vector<u32>* ps);
    void SetModuleLoader(ModuleLoader loader);
    bool RegisterGamePairLazy(u64 key, const std::vector<GamePairLayout::Binding>& bindings);
    void LazyModuleStats(u64* loaded, u64* failed) const;
    // Fase 59: tempos (ms) das etapas do ultimo DrawFrame: 0 espera de pipelines, 1 lock, 2 fence do
    // frame anterior, 3 acquire da imagem, 4 gravacao dos comandos, 5 vkQueueSubmit, 6 present.
    void LastDrawFrameTimes(double out[7]) const;
    // Fase 59: memoria de video usada pelo processo e orcamento (VK_EXT_memory_budget); false sem a extensao.
    bool VideoMemory(u64* used, u64* budget) const;
    // Fase 59: texturas esquecidas (sem uso ha ~30 s) descartadas para caber na memoria.
    u64 TexturesEvicted() const;
    bool QueueGameDraw(const GameDraw& draw);
    u64 GameDraws() const;
    u32 GamePairs() const;
    u32 GamePipelines() const;
    // Alvos de render (Fase 20).
    // Cache de geometria (Fase 21): copia `bytes` para o buffer estatico da GPU e devolve a
    // posicao, ou ~0 se nao coube (o buffer e esvaziado no fim do frame e a geracao avanca: as
    // posicoes antigas deixam de valer).
    u64 StaticUpload(const void* data, u64 bytes);
    u32 StaticGeneration() const;
    u64 StaticBytes() const;
    u32 RenderTargets() const;
    bool HasRenderTarget(u64 address) const; // o jogo ja desenhou num alvo com este endereco?
    // Fase 25: leitura de volta de alvos minusculos (luminancia media da cena, 2x1). O jogo le
    // esses pixels pela CPU na memoria do alvo; como os alvos vivem so no Vulkan, o conteudo e
    // copiado para um buffer do host no fim do frame e entregue no frame seguinte.
    void RequestReadback(u64 address, u32 width, u32 height);
    using ReadbackSink = void (*)(u64 address, const unsigned char* pixels, u32 width, u32 height, u32 bytes_per_pixel);
    u32 CollectReadbacks(ReadbackSink sink);
    u32 LastFramePasses() const;   // vkCmdBeginRendering no ultimo frame
    u64 FeedbackSkipped() const;
    u64 FeedbackCopies() const; // Fase 32: copias de alvo tiradas para draws que amostram o proprio alvo
    u64 FeedbackDraws() const;  // ... e quantos draws as usaram   // draws que amostravam o proprio alvo
    u64 RingRejected() const;      // draws recusados por nao caberem nos aneis do frame
    void RingPeaks(u64* vertex, u64* index, u64* storage) const; // Fase 54f: maior uso de cada anel num frame
    void PipelineFailures(u64* failures, u64* retries) const; // Fase 55: criacoes que falharam / novas tentativas
    void SetPipelineBurstWait(double ms); // Fase 55: espera maxima num frame com rajada de pipelines novos (0 = desliga)
    void SetFastPipelines(bool enabled); // Fase 56: pipeline urgente sem otimizacao + troca pela otimizada depois
    void FastPipelineStats(u64* fast, u64* upgraded) const;
    bool LastFrameShowedTarget() const;
    // Mensagens das camadas de validacao (--vulkan-validation): erros e avisos recebidos.
    bool ValidationActive() const;
    u64 ValidationErrors() const;
    u64 ValidationWarnings() const;

    // Unica instancia do processo.
    static VulkanRenderer& Instance();

    // Cria instancia, superficie para `hwnd` (HWND), dispositivo e swapchain. Devolve false e
    // deixa o motivo em LastError() se o Vulkan nao estiver disponivel. Pode ser chamada de
    // qualquer thread; chamadas repetidas com o renderizador ja ativo nao fazem nada.
    bool Initialize(void* hwnd);

    // Um frame: adquire a imagem do swapchain, limpa com a cor de destaque num render pass e
    // apresenta (modo FIFO: a chamada espera o VSync real do monitor). Devolve true se o frame
    // foi apresentado. Nunca bloqueia por mais de ~250 ms: janela minimizada ou swapchain
    // desatualizado fazem o frame ser pulado (e o swapchain recriado).
    // `display_address`: endereco do buffer de video que o jogo mandou exibir (0 = desconhecido).
    // O alvo de render com esse endereco e copiado para a janela; sem ele, o ultimo alvo grande.
    bool DrawFrame(u64 display_address = 0);

    void Shutdown();
    void RequestSwapchainRebuild();
    void SavePipelineCache();

    bool Active() const;
    const std::string& LastError() const;
    // Uma linha com GPU escolhida, formato e tamanho do swapchain, para o log.
    std::string Describe() const;
    u64 FramesPresented() const;
    u64 FramesSkipped() const;
    // Fase 35: espera o frame anterior terminar na GPU (ate 250 ms). Os envios de textura feitos logo
    // depois nao pagam mais essa espera um a um.
    void WaitPreviousFrame();
    // Fase 29: pipelines criados numa thread propria (o draw fica de fora ate o pipeline chegar).
    void SetAsyncPipelines(bool enabled);
    u64 AsyncPipelines() const;
    // Fase 38: pipeline que falta e criado NA HORA, na thread de render, e o draw sai no mesmo frame,
    // ate gastar `budget_ms` por frame; passou disso, volta ao modo da Fase 29 (thread de pipelines,
    // draw de fora ate ele chegar). enabled = false: sempre em segundo plano.
    void SetSyncPipelines(bool enabled, double budget_ms);
    struct SyncPipelineStats {
        u64 created = 0, over_budget = 0, frames_with_sync = 0, burst_frames = 0; // Fase 55: frames com rajada (espera maior)
        double total_ms = 0, worst_frame_ms = 0, worst_single_ms = 0;
    };
    SyncPipelineStats GetSyncPipelineStats() const;
    // Fase 45: tempo que o frame ja esperou por texturas; a espera por pipelines usa o que sobra do teto.
    void NoteFrameWait(double ms);
    // Fase 45: draws que ficaram de fora do frame porque a textura ou o pipeline nao chegou a tempo.
    u64 DrawsLateTexture() const;
    // Fase 39: envios de textura gravados no command buffer do proprio frame (sem vkQueueWaitIdle).
    struct UploadStats {
        u64 deferred = 0, immediate = 0; // na fila do frame / com espera (nao coube no staging do frame)
        u64 deferred_bytes = 0;
    };
    UploadStats GetUploadStats() const;
    // Fase 41: VK_EXT_graphics_pipeline_library. Os shaders de cada par viram bibliotecas compiladas
    // quando o par carrega; no draw o pipeline e so a ligacao rapida das partes. Chamar antes de
    // Initialize para desligar (--no-gpl).
    void SetGpl(bool enabled);
    struct GplStats {
        bool active = false;
        u64 shader_libs = 0, links = 0, vertex_libs = 0, output_libs = 0, monolithic = 0, lib_failures = 0, link_failures = 0;
        double shader_lib_ms = 0, shader_lib_worst_ms = 0, link_ms = 0, link_worst_ms = 0, monolithic_ms = 0, monolithic_worst_ms = 0;
    };
    GplStats GetGplStats() const;
    // Fase 43: modulos SPIR-V e layouts sao compartilhados entre pares (cache por conteudo).
    void GetSharedObjectCounts(u64* modules, u64* layouts) const;
    // Fase 36: aquecimento. Cada pipeline criado tem o seu estado gravado em `path` (uma linha por
    // pipeline); PrewarmPipelines le o arquivo e manda criar, em segundo plano, todos os que sao de
    // pares ja registrados. Devolve quantos foram enfileirados.
    void SetPipelineStateFile(const std::string& path);
    void SetPipelineCacheFile(const std::string& path);
    void SaveDiskPipelineCache();
    void LoadDiskPipelineCache();
    u32 PrewarmPipelines();
    void BenchPipelines(const std::string& path); // Fase 53: so medicao (LD_PIPE_BENCH)
    void PrewarmProgress(u32* done, u32* total) const;
    double LastGpuFrameMs() const;                               // Fase 75: tempo de GPU do ultimo quadro terminado
    void SavePipelineCache(const char* reason);                 // Fase 70: grava o cache de pipelines do Vulkan (arquivo temporario + troca)
    void SetCurrentLevel(const std::string& name);              // Fase 72: o jogo comecou a carregar esta area
    void PrewarmPriorityProgress(u32* done, u32* total) const; // Fase 72: so os pipelines da area prioritaria

    // Cor de limpeza (padrao desde a Fase 17: cinza bem escuro, para a geometria se destacar).
    void SetClearColor(float r, float g, float b, float a);

    // Copia vertices e indices do draw para os ring buffers do Vulkan e guarda o draw para o
    // proximo DrawFrame, que o desenha com vkCmdDrawIndexed. Devolve false se o draw nao coube
    // ou o renderizador nao esta ativo.
    bool QueueDraw(const DrawCall& draw);
    // Texturas: `key` identifica a textura do jogo (endereco + dimensoes + formato). HasTexture
    // evita reler a memoria do jogo; UploadTexture cria VkImage + VkImageView, copia os pixels
    // por um buffer de staging (vkCmdCopyBufferToImage) e aloca o descriptor set da textura.
    bool HasTexture(u64 key) const;
    // Fase 45: `prepared` = objetos criados antes por PrepareTexture (0 = cria tudo aqui).
    bool UploadTexture(u64 key, const TextureData& texture, u64 prepared = 0);
    // Fase 45: cria imagem, memoria e views SEM o lock do renderizador (qualquer thread). Devolve um
    // identificador para UploadTexture, ou 0 se falhou. DiscardPreparedTexture libera um nao usado.
    u64 PrepareTexture(const TextureData& texture);
    // Fase 59: a thread que destilou envia a textura (copia para o staging do frame sem o lock da thread
    // de render). Consome `prepared` sempre; false = falhou.
    bool UploadTextureFromWorker(u64 key, const TextureData& texture, u64 prepared);
    void DiscardPreparedTexture(u64 prepared);
    // Fase 25: reenvia os pixels de uma textura ja criada (mesmo tamanho e formato). Para as
    // texturas pequenas que o jogo reescreve pela CPU (exposicao, tabela de materiais, LUT de cor).
    bool UpdateTexture(u64 key, const TextureData& texture);
    u32 TexturesCreated() const;
    u64 TextureBytes() const;
    u64 TexturedDraws() const;

    u64 DrawsQueued() const;    // total desde o inicio
    u64 DrawsRendered() const;  // total de vkCmdDrawIndexed emitidos
    u32 LastFrameDraws() const; // draws desenhados no ultimo frame apresentado
    u64 BytesUploaded() const;
    u32 PipelinesCreated() const;

    // Tenta criar um VkShaderModule para cada .spv de `directory` (os modulos recompilados do
    // jogo). So confere que o driver aceita o binario; eles ainda nao sao usados em pipelines.
    // Devolve quantos foram aceitos; `total` recebe quantos arquivos havia.
    u32 CheckGameShaderModules(const std::string& directory, u32* total);

private:
    VulkanRenderer() = default;
};

// Pasta com os .spv recompilados do jogo (assets/shaders_spv), conferidos na inicializacao.
void SetGameSpirvDirectory(const std::string& directory);
const std::string& GameSpirvDirectory();

// Liga / desliga o uso do Vulkan (--no-vulkan). Com false, Initialize devolve false sem tentar.
// VK_LAYER_KHRONOS_validation (Vulkan SDK): ligada por padrao, mensagens no log; um ERRO encerra
// o processo, a menos que SetVulkanValidationAbort(false).
void SetVulkanValidation(bool enabled);
void SetVulkanValidationAbort(bool enabled);
void SetVulkanEnabled(bool enabled);
bool VulkanEnabled();

} // namespace render
