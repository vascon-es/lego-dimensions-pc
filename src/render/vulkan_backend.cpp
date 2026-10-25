#include "render/vulkan_backend.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <tuple>
#include <vector>

#include <windows.h>

#include <io.h> // Fase 70: _commit

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace render {

std::atomic<u64> g_temp_staging_count{0}; // Fase 59: texturas que foram por um buffer temporario (Fase 61: definido aqui, usado tambem no staging do frame)

namespace {

std::atomic<bool> g_enabled{true};
// Camada de validacao: DESLIGADA por padrao desde a Fase 24.5. Medido: com ela, gravar um frame
// de 6000 draws leva ~60 ms (12 a 14 fps); sem ela, ~2 ms (30 fps, o limite do proprio jogo).
// --vulkan-validation liga (erro encerra o processo); --vulkan-validation-continue liga e so registra.
std::atomic<bool> g_validation{false};
std::atomic<bool> g_validation_abort{true}; // erro de validacao encerra o processo (--vulkan-validation-continue)
std::atomic<u64> g_validation_errors{0}, g_validation_warnings{0};

// ----------------------- funcoes do Vulkan, carregadas da DLL -----------------------
#define VK_GLOBAL_FUNCTIONS(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE_FUNCTIONS(X)                                                                   \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties)            \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)           \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)           \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr)       \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceMemoryProperties2)
#define VK_DEVICE_FUNCTIONS(X)                                                                     \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR)             \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)                   \
    X(vkQueuePresentKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdBeginRendering)         \
    X(vkCmdEndRendering) X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdDraw) X(vkCmdClearColorImage) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer)                    \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                                                  \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence)                  \
    X(vkWaitForFences) X(vkGetFenceStatus) X(vkResetFences) X(vkQueueSubmit) X(vkCreateBuffer) X(vkDestroyBuffer)      \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory)     \
    X(vkMapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout)      \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)                   \
    X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants)              \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCreateImage)         \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateSampler)      \
    X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)             \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCmdPipelineBarrier)                   \
    X(vkCmdCopyBufferToImage) X(vkQueueWaitIdle) X(vkResetDescriptorPool) X(vkCmdCopyImageToBuffer) X(vkCmdClearAttachments) X(vkCmdCopyImage) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp)    \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

#define DECLARE(name) PFN_##name name = nullptr;
struct Functions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    VK_GLOBAL_FUNCTIONS(DECLARE)
    VK_INSTANCE_FUNCTIONS(DECLARE)
    VK_DEVICE_FUNCTIONS(DECLARE)
} vk;
#undef DECLARE

const char* ResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    default: return "erro do Vulkan";
    }
}

// ------------------------------- estado -----------------------------------
// Fase 41: o que o draw fixa de rasterizacao e de profundidade / estencil. O pipeline monolitico grava
// isto como estado estatico; os pipelines ligados de bibliotecas (GPL) recebem como estado dinamico
// em cada draw, para cada par ter uma biblioteca so de VS e uma so de PS.
struct FixedState {
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkBool32 clamp = VK_FALSE;
    VkBool32 depth_test = VK_FALSE, depth_write = VK_FALSE;
    VkCompareOp depth_compare = VK_COMPARE_OP_NEVER;
    VkBool32 stencil_test = VK_FALSE;
    VkStencilOpState front_ops{}, back_ops{};
};

struct State {
    std::mutex mutex;
    bool active = false;
    std::string error;
    HMODULE library = nullptr;
    HWND hwnd = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    u32 queue_family = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format{};
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    // Fase 20: sem VkRenderPass nem VkFramebuffer; a renderizacao comeca com vkCmdBeginRendering
    // (dynamic rendering, nucleo do Vulkan 1.3) direto nas VkImageView.

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    // Um semaforo de "render terminou" por imagem do swapchain: a apresentacao segura o
    // semaforo ate a imagem voltar, entao um unico semaforo podia ser sinalizado ainda em uso
    // (VUID-vkQueueSubmit-pSignalSemaphores-00067, apontado pela validacao na Fase 19).
    std::vector<VkSemaphore> render_finished;
    VkFence in_flight = VK_NULL_HANDLE;

    VkClearColorValue clear{{0.05f, 0.05f, 0.07f, 1.0f}};
    std::atomic<u64> presented{0};
    std::atomic<u64> skipped{0};
    u32 swapchain_rebuilds = 0;

    // ---- geometria do jogo (Fase 17) ----
    struct HostBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u8* mapped = nullptr;
        u64 size = 0;
        u64 head = 0; // proxima posicao livre do anel
    };
    HostBuffer vertices; // 32 MiB, HOST_VISIBLE | HOST_COHERENT, mapeado o tempo todo
    HostBuffer indices;  // 16 MiB
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    // (stride, formato e offset da posicao, tem cor, offset da cor, strip, blend, formato e offset do UV)
    std::map<std::tuple<u32, u32, u32, bool, u32, bool, bool, u32, u32>, VkPipeline> pipelines;
    // ---- texturas (Fase 18) ----
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        u32 view_type = 0; // 0 = 2D, 1 = cubo, 3 = 3D (mesma numeracao de GameDraw::Image::view_type)
        VkImageView array_view = VK_NULL_HANDLE; // cubo: as 6 faces como array 2D (ha shaders que o declaram assim)
        VkImageView face_view = VK_NULL_HANDLE;  // cubo: so a face 0, como 2D, para slots que o shader declara 2D
        u32 width = 0, height = 0, depth = 1, levels = 1, layers = 1; // para reenvio (UpdateTexture)
        u64 bytes = 0;
        // Fase 59: memoria vinda de um bloco do pool (memory fica nulo): bloco, posicao e tamanho reservado
        u32 pool_chunk = ~0u;
        u64 pool_offset = 0, pool_size = 0;
        u32 last_used = 0; // Fase 59: ultimo frame (frame_serial) que a usou; descarte das esquecidas
    };
    std::unordered_map<u64, Texture> textures; // chave do jogo -> textura; 0 = branco 1x1 (Fase 59: hash, era arvore)
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    HostBuffer staging; // 40 MiB, origem das copias buffer -> imagem
    // Fase 39: staging dos envios gravados no proprio frame. Duas metades alternadas: a do frame em
    // voo so e reescrita depois da fence dele (um frame em voo por vez).
    HostBuffer frame_staging;
    u32 frame_half = 0;
    u64 frame_head = 0;
    // Fase 59: copias para o staging reservadas por threads de textura e ainda em andamento (por metade).
    // O DrawFrame espera zerar as da metade dele antes do vkQueueSubmit.
    std::atomic<int> staging_inflight[2]{};
    // Fase 59: buffers temporarios de envio (textura que nao coube no staging do frame) e o frame que os
    // gravou (0 = ainda nao gravado); destruidos depois que a fence desse frame volta.
    struct TempStaging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u32 recorded_serial = 0;
    };
    std::vector<TempStaging> temp_staging;
    struct PendingUpload {
        VkBuffer source = VK_NULL_HANDLE; // Fase 59: buffer temporario (staging do frame cheio); nulo = staging do frame
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        u32 levels = 1, layers = 1, region_count = 0;
        VkBufferImageCopy regions[16]{};
    };
    std::vector<PendingUpload> pending_uploads;
    VkCommandBuffer upload_command = VK_NULL_HANDLE;
    u64 texture_bytes = 0;
    std::atomic<u64> textured_draws{0};
    bool memory_budget = false; // Fase 59: VK_EXT_memory_budget ligado
    double draw_frame_ms[8] = {}; // Fase 59: etapas do ultimo DrawFrame (ver LastDrawFrameTimes)
    // Fase 75: tempo de GPU de cada quadro (timestamp no comeco e no fim do command buffer do frame). A espera
    // "frame anterior" do [QUADRO-LENTO] dominava os quadros lentos de um jogador (RX 9070 XT, 5 h): este numero
    // diz se a GPU demorou mesmo naquele quadro. LD_GPU_TIME=0 desliga.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double timestamp_period_ns = 0;
    bool timestamps_written = false;
    std::atomic<double> last_gpu_ms{0};
    // ---- shaders do jogo (Fase 19) ----
    bool game_shaders = false; // o dispositivo foi criado com os recursos que os modulos pedem
    bool validation = false;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    struct GamePair {
        VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        std::vector<GamePairLayout::Binding> bindings;
        VkPipeline vs_lib = VK_NULL_HANDLE, fs_lib = VK_NULL_HANDLE; // Fase 41: bibliotecas pre-rasterizacao / fragment shader
    };
    std::unordered_map<u64, GamePair> game_pairs; // Fase 59: hash (consultado a cada draw)
    std::unordered_map<u64, VkPipeline> game_pipelines; // hash de 64 bits do estado (Fase 28: antes, um vetor por draw)
    HostBuffer storage;                           // anel de storage buffers (constantes do jogo)
    HostBuffer statics;                           // geometria que nao muda (cache por endereco do jogo)
    bool static_reset = false;                    // encheu: esvazia depois do proximo frame
    std::atomic<u32> static_generation{1};
    VkDescriptorPool frame_pool = VK_NULL_HANDLE; // sets dos draws do jogo; esvaziado a cada frame
    std::unordered_map<u64, VkSampler> game_samplers;
    // ---- leitura de volta de alvos minusculos (Fase 25) ----
    struct Readback {
        u64 address = 0;
        u32 width = 0, height = 0, bytes_per_pixel = 0;
        bool pending = false; // copiado neste frame: os bytes estao no buffer quando a fence sinalizar
    };
    Readback readbacks[8];
    u32 readback_count = 0;
    HostBuffer readback; // 8 x 256 bytes
    std::atomic<u64> game_draws{0};
    // ---- alvos de render do jogo (Fase 20) ----
    struct RenderTarget {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;     // anexo, e amostragem pelos shaders do jogo
        VkImageView raw_view = VK_NULL_HANDLE; // os bytes como estao (UNORM), para copiar a janela
        VkImageView sample_view = VK_NULL_HANDLE; // profundidade: so o aspecto de profundidade, para os shaders
        VkDescriptorSet present_set = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        u32 width = 0, height = 0;
        bool depth = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // acompanhado enquanto o frame e gravado
        u64 address = 0;
        // Fase 32: a mesma imagem como array 2D de 1 camada (ha shaders que declaram assim o slot).
        VkImageView array_view = VK_NULL_HANDLE;
        // Fase 32: copia para draws que amostram o proprio alvo (distorcao, brilho, portais). No PS4
        // isso le o que ja esta no alvo; no Vulkan o draw amostra esta copia, tirada antes dele.
        VkImage copy_image = VK_NULL_HANDLE;
        VkDeviceMemory copy_memory = VK_NULL_HANDLE;
        VkImageView copy_view = VK_NULL_HANDLE;
        VkImageLayout copy_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool copy_fresh = false; // a copia ainda vale (nada novo foi desenhado no alvo por fora dela)
    };
    std::unordered_map<u64, RenderTarget> targets;            // (endereco, tamanho, formato) -> alvo (no estavel: ponteiros valem)
    std::unordered_map<u64, RenderTarget*> target_by_address; // o alvo mais recente de cada endereco
    RenderTarget* last_screen = nullptr;            // ultimo alvo de cor grande desenhado
    VkImage fallback_images[2]{};                   // branco 1x1: cubo / array, e 3D
    VkDeviceMemory fallback_memory[2]{};
    VkImageView fallback_views[4]{};                // por tipo de view: 2D (nao usado), cubo, array 2D, 3D
    bool independent_blend = false;
    bool gpl = false; // Fase 41: bibliotecas de pipeline ligadas (extensao + recurso presentes e nao desligado)
    PFN_vkCmdSetDepthClampEnableEXT set_depth_clamp = nullptr;
    // Fase 54: detector de GPU travada. A GPU grava o indice do draw que comecou (offset 0) e do que
    // terminou (offset 4); se o frame nao termina, o relatorio diz qual draw ficou preso.
    PFN_vkCmdWriteBufferMarkerAMD write_marker = nullptr;
    PFN_vkGetDeviceFaultInfoEXT get_fault_info = nullptr;
    HostBuffer markers;
    u32 frame_serial = 0;
    struct DrawInfo {
        u64 pair_key, pipeline_key;
        u32 queue_index, count, instance_count, index_type;
        s32 base_vertex;
        bool indexed;
        u32 color_count, color_w, color_h, color_format;
        u32 depth_w, depth_h;
        u32 image_count, target_images, buffer_count;
        u64 buffer_bytes, vertex_bytes;
        float viewport_w, viewport_h;
        struct Img {
            u32 binding, is_target, w, h, format, depth, levels, view_type, array;
            u64 key;
        } images[4];
        struct Smp {
            u32 binding, wrap_u, wrap_v, point, compare, border, lod, filter;
        } samplers[4];
        u32 sampler_count;
    };
    std::vector<DrawInfo> frame_draws; // draws gravados no ultimo frame enviado
    std::vector<DrawInfo> previous_draws; // Fase 54c: os do frame antes dele
    bool device_lost = false;
    u32 hang_timeouts = 0;             // esperas seguidas pela fence que estouraram o tempo
    bool hang_reported = false;
    // Fase 54b: o que o fim do frame fez (para o relatorio)
    struct TailInfo {
        u32 passes, feedback_copies, readbacks;
        u32 shown_w, shown_h, shown_format;
        u64 shown_address;
        u32 targets_total, targets_new;
    } tail{};
    u32 targets_before_frame = 0;
    // Fase 54e: envios de textura. No comeco do frame (staging do frame) e fora dele (submissao propria).
    u32 staged_last_frame = 0;
    u64 staged_bytes_last_frame = 0;
    u32 immediate_since_frame = 0; // submissoes proprias desde o ultimo frame enviado
    u64 frame_vertex_bytes = 0, frame_index_bytes = 0, frame_storage_bytes = 0;
    u64 peak_vertex_bytes = 0, peak_index_bytes = 0, peak_storage_bytes = 0; // Fase 54f: maior frame
    std::atomic<u64> feedback_skipped{0}, ring_rejected{0}, feedback_copies{0}, feedback_draws{0};
    std::atomic<u32> last_frame_passes{0};
    std::atomic<bool> last_frame_showed_target{false};
    struct QueuedGame {
        const GamePair* pair;
        u64 pair_key; // Fase 54: para o relatorio de GPU travada
        VkPipeline pipeline;
        u64 pipeline_key; // Fase 45: pipeline pendente (compilando nas threads) quando `pipeline` e nulo
        FixedState fixed; // Fase 41: estado dinamico do draw (usado pelos pipelines ligados)
        RenderTarget* colors[8];
        u32 color_count;
        RenderTarget* depth;
        bool clear_depth;
        bool depth_write; // o pipeline grava profundidade (z_enable e z_write do jogo)
        bool stencil_write;       // o teste de estencil esta ligado e alguma operacao altera o valor
        bool clear_stencil;
        u32 clear_stencil_value;
        u32 stencil_ref[2];       // DB_STENCILREFMASK da frente e de tras
        bool stencil_op_value[2]; // a referencia do Vulkan e o valor de operacao (ha REPLACE_OP)
        float clear_depth_value;
        u64 vertex_offsets[8];
        bool vertex_static[8];
        u32 binding_count;
        bool indexed;
        bool index_static;
        u64 index_offset;
        u32 count;
        VkIndexType index_type;
        u32 instance_count;
        s32 base_vertex;
        struct {
            u32 binding;
            u64 offset, bytes;
        } buffers[24];
        u32 buffer_count;
        struct {
            u32 binding;
            VkImageView view;
            RenderTarget* target; // nao nulo: a imagem e um alvo de render
            bool array;           // amostrado pela view de array 2D
            u64 texture_key;      // Fase 45: textura pendente (resolvida no DrawFrame); 0 = ja resolvida
            u32 view_type;
            u64 source_key;       // Fase 54d: textura do jogo (para o relatorio de GPU travada)
        } images[16];
        u32 image_count;
        u32 pending_images; // Fase 45: imagens ainda sem textura; o DrawFrame resolve antes de gravar
        struct {
            u32 binding;
            VkSampler sampler;
            u32 raw[7]; // Fase 54d: wrap_u, wrap_v, point, compare, border, lod, filter (relatorio de GPU travada)
        } samplers[16];
        u32 sampler_count;
        u8 push[120];
        VkViewport viewport;
        VkRect2D scissor;
        float blend_constants[4];
    };
    std::vector<QueuedGame> queue_game;
    struct Queued { // draw do pipeline base: desenhado por cima, direto na imagem da janela
        VkPipeline pipeline;
        u64 vertex_offset, index_offset;
        u32 index_count;
        VkIndexType index_type;
        bool has_color;
        bool extra_binding; // binding 1 (branco fixo) em uso: falta cor ou UV no vertice
        VkDescriptorSet texture;
        float mvp[16];
        VkViewport viewport;
    };
    std::vector<Queued> queue_draws;
    std::atomic<u64> draws_queued{0}, draws_rendered{0}, bytes_uploaded{0};
    std::atomic<u32> last_frame_draws{0};
    u32 pipelines_created = 0;
} g;

VkResult QueueWaitIdle(); // Fase 70b: fila do Vulkan com mutex proprio (ver g_queue_mutex)
VkResult QueueSubmit(u32 count, const VkSubmitInfo* submits, VkFence fence);

constexpr u64 kVertexRingBytes = 256ull << 20; // a cena 3D copia os vertices de cada draw (Fase 20)
constexpr u64 kIndexRingBytes = 128ull << 20; // Fase 54f: 64 -> 128 MiB (metade por frame)
// Inicio do anel de vertices: 4 bytes 0xFF (cor branca dos draws sem cor) e, a partir do byte 16,
// o quad que copia o alvo de render do jogo para a janela (4 vertices: posicao float3 + UV float2).
constexpr u64 kWhiteBytes = 128;
constexpr u64 kPresentQuadOffset = 16;
constexpr u64 kIndexReserved = 16; // inicio do anel de indices: os 6 indices do quad de apresentacao
constexpr size_t kMaxDrawsPerFrame = 16384;
constexpr u32 kMaxTargets = 256;
constexpr u64 kStagingBytes = 40ull << 20;
constexpr u64 kFrameStagingHalf = 128ull << 20; // Fase 39: por metade; Fase 45: 48 -> 128 MiB (entrada de cena com 187 MiB de texturas)
VulkanRenderer::UploadStats g_upload_stats; // com g.mutex
// Fase 43: modulos e layouts compartilhados entre pares (ver SharedModule)
std::mutex g_shared_mutex;
std::unordered_map<u64, VkShaderModule> g_module_cache;                                     // hash do SPIR-V
std::map<std::vector<u32>, std::pair<VkDescriptorSetLayout, VkPipelineLayout>> g_layout_cache; // bindings
constexpr u32 kMaxTextures = 4096;
constexpr u64 kStorageRingBytes = 128ull << 20; // Fase 54f: 64 -> 128 MiB (metade por frame)
constexpr u64 kStaticBytes = 768ull << 20;
constexpr u64 kZeroBytes = 4096; // inicio do anel de storage: zeros, para bindings sem buffer do jogo

void DestroyPipelines();
bool CreateTexture(u64 key, const TextureData& data);
bool CreateFallbackViews();
void PrewarmTargetPool(); // Fase 61

bool Fail(const char* what, VkResult result = VK_SUCCESS) {
    char text[256];
    if (result != VK_SUCCESS) {
        std::snprintf(text, sizeof(text), "%s: %s (%d)", what, ResultName(result), static_cast<int>(result));
    } else {
        std::snprintf(text, sizeof(text), "%s", what);
    }
    g.error = text;
    return false;
}

bool LoadLibraryAndGlobals() {
    g.library = LoadLibraryA("vulkan-1.dll");
    if (!g.library) {
        return Fail("vulkan-1.dll nao encontrada (driver de video sem suporte a Vulkan?)");
    }
    vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g.library, "vkGetInstanceProcAddr"));
    if (!vk.vkGetInstanceProcAddr) {
        return Fail("vkGetInstanceProcAddr ausente em vulkan-1.dll");
    }
#define LOAD(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(nullptr, #name));
    VK_GLOBAL_FUNCTIONS(LOAD)
#undef LOAD
    return vk.vkCreateInstance ? true : Fail("vkCreateInstance ausente");
}

// Mensagens das camadas de validacao: as 3 primeiras de cada tipo vao para o log, o resto so conta.
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    static std::mutex mutex;
    static std::map<s32, u32> seen;
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ++(error ? g_validation_errors : g_validation_warnings);
    std::lock_guard lock(mutex);
    const u32 count = ++seen[data->messageIdNumber];
    if (error && g_validation_abort) {
        // Erro de validacao e tratado como fatal: a mensagem vai para o log e o processo termina
        // ali mesmo, para o erro nao se perder no meio de milhares de linhas.
        std::printf("[VK-VALIDACAO] ERRO FATAL %s: %s\n[VK-VALIDACAO] execucao abortada (use --vulkan-validation-continue para "
                    "so registrar)\n",
                    data->pMessageIdName ? data->pMessageIdName : "?", data->pMessage ? data->pMessage : "");
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 70);
    }
    if (count <= 3) {
        std::printf("[VK-VALIDACAO] %s %s: %.1400s\n", error ? "ERRO" : "aviso", data->pMessageIdName ? data->pMessageIdName : "?",
                    data->pMessage ? data->pMessage : "");
        if (count == 3) {
            std::printf("[VK-VALIDACAO]   (repeticoes seguintes deste tipo so entram na contagem)\n");
        }
        std::fflush(stdout);
    }
    return VK_FALSE;
}

bool CreateInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LEGO Dimensions PC";
    app.pEngineName = "lego-dimensions-pc";
    app.apiVersion = VK_API_VERSION_1_3; // os modulos do jogo usam recursos do Vulkan 1.2 / 1.3
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    // Camadas de validacao (Vulkan SDK), so quando pedidas e instaladas.
    static const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation";
    g.validation = false;
    if (g_validation && vk.vkEnumerateInstanceLayerProperties) {
        u32 layer_count = 0;
        vk.vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vk.vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
        for (const VkLayerProperties& layer : layers) {
            g.validation |= !std::strcmp(layer.layerName, kValidationLayer);
        }
        if (g.validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &kValidationLayer;
            info.enabledExtensionCount = 3;
        } else {
            std::printf("[VULKAN] camada %s nao encontrada (Vulkan SDK instalado?): seguindo sem validacao\n", kValidationLayer);
        }
    }
    const VkResult result = vk.vkCreateInstance(&info, nullptr, &g.instance);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateInstance (VK_KHR_surface + VK_KHR_win32_surface)", result);
    }
    bool complete = true;
#define LOAD(name)                                                                                 \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(g.instance, #name));           \
    complete = complete && vk.name;
    VK_INSTANCE_FUNCTIONS(LOAD)
#undef LOAD
    if (g.validation) {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vk.vkGetInstanceProcAddr(g.instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = DebugCallback;
        if (create) {
            create(g.instance, &messenger, nullptr, &g.messenger);
        }
    }
    return complete ? true : Fail("funcao de instancia do Vulkan ausente");
}

bool CreateSurface(HWND hwnd) {
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleA(nullptr);
    info.hwnd = hwnd;
    const VkResult result = vk.vkCreateWin32SurfaceKHR(g.instance, &info, nullptr, &g.surface);
    return result == VK_SUCCESS ? true : Fail("vkCreateWin32SurfaceKHR", result);
}

// GPU discreta de preferencia; tem de ter fila grafica que apresente na superficie e a
// extensao VK_KHR_swapchain.
bool PickPhysicalDevice() {
    u32 count = 0;
    vk.vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(g.instance, &count, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
        u32 extension_count = 0;
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
        const bool has_swapchain = std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& e) {
            return !std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        });
        if (!has_swapchain) {
            continue;
        }
        u32 family_count = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
        int family = -1;
        for (u32 i = 0; i < family_count && family < 0; ++i) {
            VkBool32 can_present = VK_FALSE;
            vk.vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g.surface, &can_present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                family = static_cast<int>(i);
            }
        }
        if (family < 0) {
            continue;
        }
        VkPhysicalDeviceProperties properties;
        vk.vkGetPhysicalDeviceProperties(device, &properties);
        const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                          : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                          : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 1
                                                                                            : 0;
        if (score > best_score) {
            best_score = score;
            g.physical = device;
            g.properties = properties;
            g.queue_family = static_cast<u32>(family);
        }
    }
    return best_score >= 0 ? true : Fail("nenhuma GPU com VK_KHR_swapchain e fila grafica que apresente nesta janela");
}

// --gpl. Fase 41: desligado por padrao. No driver AMD 26.6.1 (RX 6600) so LIGAR o recurso
// graphicsPipelineLibrary no dispositivo, sem usar, ja deixa a tela preta e o frame em ~380 ms de GPU
// (testado isolando cada parte; a validacao do Vulkan nao acusa nada). O extended_dynamic_state3
// sozinho nao causa isso. Conferir de novo quando o driver for atualizado.
// Fase 74: tres estados. Automatico (padrao): ligado em NVIDIA (0x10DE) e Intel (0x8086), onde as bibliotecas sao o
// caminho recomendado pelos drivers; desligado na AMD (o problema acima) e em outros fabricantes. --gpl / --no-gpl
// (o launcher passa a escolha dele) e LD_GPL=1 / 0 forcam. NVIDIA e Intel NAO foram testadas em hardware real.
int g_gpl_mode = -1; // -1 automatico, 0 desligado, 1 ligado
const char* g_gpl_reason = "";

bool CreateDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = g.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* extensions[12] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    // Fase 41: bibliotecas de pipeline + depth clamp dinamico, se o driver tiver
    bool has_pipeline_library = false, has_gpl = false, has_eds3 = false, has_marker = false, has_fault = false, has_budget = false;
    {
        u32 count = 0;
        vk.vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vk.vkEnumerateDeviceExtensionProperties(g.physical, nullptr, &count, available.data());
        for (const auto& e : available) {
            has_pipeline_library |= !std::strcmp(e.extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
            has_gpl |= !std::strcmp(e.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
            has_eds3 |= !std::strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            has_marker |= !std::strcmp(e.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
            has_fault |= !std::strcmp(e.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            has_budget |= !std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT have_eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT want_gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT want_eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    // Recursos que os modulos recompilados do jogo declaram (inteiros de 8/16/64 bits, acesso de
    // 8/16 bits a storage buffers, draw parameters, demote) e depth clamp para o espaco de recorte
    // do PS4. Se faltar algum, o dispositivo e criado sem eles e so o pipeline base funciona.
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &have13};
    VkPhysicalDeviceVulkan11Features have11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &have12};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have11};
    VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &want13};
    VkPhysicalDeviceVulkan11Features want11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &want12};
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &want11};
    const bool v13 = g.properties.apiVersion >= VK_API_VERSION_1_3;
    if (has_gpl && has_pipeline_library && has_eds3) {
        have_gpl.pNext = &have_eds3;
        have13.pNext = &have_gpl;
    }
    VkPhysicalDeviceFaultFeaturesEXT have_fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceFaultFeaturesEXT want_fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (has_fault) {
        have_fault.pNext = have13.pNext;
        have13.pNext = &have_fault;
    }
    if (v13) {
        vk.vkGetPhysicalDeviceFeatures2(g.physical, &have);
    }
    bool fast_linking = false;
    if (v13 && has_gpl) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &gpl_props};
        vk.vkGetPhysicalDeviceProperties2(g.physical, &props);
        fast_linking = gpl_props.graphicsPipelineLibraryFastLinking;
    }
    g.game_shaders = v13 && have.features.shaderInt16 && have.features.shaderInt64 && have.features.depthClamp &&
                     have11.storageBuffer16BitAccess && have11.shaderDrawParameters && have12.shaderInt8 &&
                     have12.storageBuffer8BitAccess && have13.shaderDemoteToHelperInvocation;
    if (!v13 || !have13.dynamicRendering) {
        return Fail("a GPU nao oferece dynamic rendering (Vulkan 1.3)");
    }
    g.independent_blend = have.features.independentBlend;
    want13.dynamicRendering = VK_TRUE; // nucleo do 1.3; e o VK_KHR_dynamic_rendering promovido
    want.features.independentBlend = have.features.independentBlend;
    want.features.shaderImageGatherExtended = have.features.shaderImageGatherExtended; // textureGather com offsets (sombras)
    want.features.sampleRateShading = have.features.sampleRateShading;                 // modulos que leem gl_SampleID / interpolam por amostra
    info.pNext = &want;
    if (g.game_shaders) {
        want.features.shaderInt16 = want.features.shaderInt64 = want.features.depthClamp = VK_TRUE;
        want11.storageBuffer16BitAccess = want11.shaderDrawParameters = VK_TRUE;
        want12.shaderInt8 = want12.storageBuffer8BitAccess = VK_TRUE;
        want13.shaderDemoteToHelperInvocation = VK_TRUE;
        info.pNext = &want;
    }
    int gpl_mode = g_gpl_mode;
    if (const char* env = std::getenv("LD_GPL"); gpl_mode < 0 && env && env[0]) {
        gpl_mode = env[0] != '0';
    }
    const u32 vendor = g.properties.vendorID;
    const bool gpl_wanted = gpl_mode >= 0 ? gpl_mode == 1 : vendor == 0x10DE || vendor == 0x8086;
    g_gpl_reason = gpl_mode >= 0     ? (gpl_mode ? "forcado ligado" : "forcado desligado")
                   : gpl_wanted       ? "automatico: NVIDIA / Intel"
                   : vendor == 0x1002 ? "automatico: AMD (tela preta no driver 26.6.1)"
                                      : "automatico: fabricante nao testado";
    g.gpl = gpl_wanted && g.game_shaders && has_gpl && has_pipeline_library && has_eds3 && have_gpl.graphicsPipelineLibrary &&
            have_eds3.extendedDynamicState3DepthClampEnable && fast_linking;
    if (g.gpl) {
        extensions[info.enabledExtensionCount++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        extensions[info.enabledExtensionCount++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        extensions[info.enabledExtensionCount++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
        want_gpl.graphicsPipelineLibrary = VK_TRUE;
        want_eds3.extendedDynamicState3DepthClampEnable = VK_TRUE;
        want_gpl.pNext = &want_eds3;
        want13.pNext = &want_gpl;
    }
    // DIAGNOSTICO (Fase 45): LD_DEV_SO=so|vi|blend liga um recurso no dispositivo, sem usar
    VkPhysicalDeviceShaderObjectFeaturesEXT want_so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT want_vi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT want_blend{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (const char* test = std::getenv("LD_DEV_SO")) {
        if (!std::strcmp(test, "so")) {
            extensions[info.enabledExtensionCount++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
            want_so.shaderObject = VK_TRUE;
            want_so.pNext = want13.pNext;
            want13.pNext = &want_so;
        } else if (!std::strcmp(test, "vi")) {
            extensions[info.enabledExtensionCount++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
            want_vi.vertexInputDynamicState = VK_TRUE;
            want_vi.pNext = want13.pNext;
            want13.pNext = &want_vi;
        } else if (!g.gpl) {
            extensions[info.enabledExtensionCount++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
            want_blend.extendedDynamicState3ColorBlendEnable = want_blend.extendedDynamicState3ColorBlendEquation =
                want_blend.extendedDynamicState3ColorWriteMask = want_blend.extendedDynamicState3DepthClampEnable = VK_TRUE;
            want_blend.pNext = want13.pNext;
            want13.pNext = &want_blend;
        }
        std::printf("[VULKAN] DIAGNOSTICO: recurso '%s' ligado no dispositivo (sem uso)\n", test);
    }
    std::printf("[VULKAN] bibliotecas de pipeline (GPL): %s, %s (extensao %s, recurso %s, link rapido %s, depth clamp dinamico %s)\n",
                g.gpl ? "LIGADAS" : "desligadas", g_gpl_reason, has_gpl ? "sim" : "nao", have_gpl.graphicsPipelineLibrary ? "sim" : "nao",
                fast_linking ? "sim" : "nao", have_eds3.extendedDynamicState3DepthClampEnable ? "sim" : "nao");
    // Fase 54: marcadores de draw e relatorio de falha do dispositivo (diagnostico de GPU travada)
    if (has_marker && !std::getenv("LD_NO_GPU_MARKERS")) {
        extensions[info.enabledExtensionCount++] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME;
    } else {
        has_marker = false;
    }
    if (has_fault && have_fault.deviceFault) {
        extensions[info.enabledExtensionCount++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
        want_fault.deviceFault = VK_TRUE;
        want_fault.pNext = want13.pNext;
        want13.pNext = &want_fault;
    } else {
        has_fault = false;
    }
    if (has_budget) { // Fase 59: uso e orcamento da memoria de video (linha [STREAM])
        extensions[info.enabledExtensionCount++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
        g.memory_budget = true;
    }
    const VkResult result = vk.vkCreateDevice(g.physical, &info, nullptr, &g.device);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateDevice (VK_KHR_swapchain)", result);
    }
    if (has_marker) {
        g.write_marker = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(vk.vkGetDeviceProcAddr(g.device, "vkCmdWriteBufferMarkerAMD"));
    }
    if (has_fault) {
        g.get_fault_info = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(vk.vkGetDeviceProcAddr(g.device, "vkGetDeviceFaultInfoEXT"));
    }
    std::printf("[VULKAN] detector de GPU travada: marcadores por draw %s, relatorio de falha do driver %s\n",
                g.write_marker ? "sim" : "nao", g.get_fault_info ? "sim" : "nao");
    bool complete = true;
#define LOAD(name)                                                                                 \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(g.device, #name));               \
    complete = complete && vk.name;
    VK_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    if (!complete) {
        return Fail("funcao de dispositivo do Vulkan ausente");
    }
    vk.vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);
    if (g.gpl) {
        g.set_depth_clamp = reinterpret_cast<PFN_vkCmdSetDepthClampEnableEXT>(vk.vkGetDeviceProcAddr(g.device, "vkCmdSetDepthClampEnableEXT"));
        g.gpl = g.set_depth_clamp != nullptr;
    }
    return true;
}

void DestroySwapchainObjects() {
    for (VkImageView view : g.views) {
        vk.vkDestroyImageView(g.device, view, nullptr);
    }
    for (VkSemaphore semaphore : g.render_finished) {
        vk.vkDestroySemaphore(g.device, semaphore, nullptr);
    }
    g.render_finished.clear();
    g.views.clear();
    g.images.clear();
}

// Swapchain 1280x720 B8G8R8A8_UNORM em modo FIFO. O tamanho segue o que a superficie exige
// (a area cliente da janela, criada com 1280x720); extent 0 = janela minimizada, sem swapchain.
bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VkResult result = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical, g.surface, &caps);
    if (result != VK_SUCCESS) {
        return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp<u32>(1280, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp<u32>(720, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) {
        return Fail("janela minimizada (area de 0 pixels): swapchain adiado");
    }
    u32 format_count = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &format_count, formats.data());
    if (formats.empty()) {
        return Fail("a superficie nao oferece nenhum formato");
    }
    g.format = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            g.format = candidate;
        }
    }
    g.present_mode = VK_PRESENT_MODE_FIFO_KHR; // sempre disponivel; apresentacao no VSync

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = g.surface;
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount) {
        info.minImageCount = caps.maxImageCount;
    }
    info.imageFormat = g.format.format;
    info.imageColorSpace = g.format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = g.present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = g.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = vk.vkCreateSwapchainKHR(g.device, &info, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateSwapchainKHR", result);
    }
    DestroySwapchainObjects();
    if (g.swapchain) {
        vk.vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    }
    g.swapchain = swapchain;
    g.extent = extent;

    // Os pipelines base dependem do formato do swapchain: recria junto. Os do jogo desenham nos
    // alvos de render e nao dependem da janela.
    DestroyPipelines();

    u32 image_count = 0;
    vk.vkGetSwapchainImagesKHR(g.device, g.swapchain, &image_count, nullptr);
    g.images.resize(image_count);
    vk.vkGetSwapchainImagesKHR(g.device, g.swapchain, &image_count, g.images.data());
    g.render_finished.assign(image_count, VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : g.render_finished) {
        const VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vk.vkCreateSemaphore(g.device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            return Fail("vkCreateSemaphore (por imagem do swapchain)");
        }
    }
    for (VkImage image : g.images) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g.format.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView handle = VK_NULL_HANDLE;
        result = vk.vkCreateImageView(g.device, &view, nullptr, &handle);
        if (result != VK_SUCCESS) {
            return Fail("vkCreateImageView", result);
        }
        g.views.push_back(handle);
    }
    return true;
}

bool CreateFrameObjects() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g.queue_family;
    VkResult result = vk.vkCreateCommandPool(g.device, &pool, nullptr, &g.pool);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateCommandPool", result);
    }
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = g.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    result = vk.vkAllocateCommandBuffers(g.device, &allocate, &g.command);
    if (result != VK_SUCCESS) {
        return Fail("vkAllocateCommandBuffers", result);
    }
    // Fase 75: dois timestamps por quadro (so ha um quadro em voo: a fence do frame anterior e esperada antes)
    if (const char* env = std::getenv("LD_GPU_TIME"); !(env && env[0] == '0') && g.properties.limits.timestampComputeAndGraphics &&
                                                       g.properties.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = 2;
        if (vk.vkCreateQueryPool(g.device, &query, nullptr, &g.timestamps) == VK_SUCCESS) {
            g.timestamp_period_ns = g.properties.limits.timestampPeriod;
        } else {
            g.timestamps = VK_NULL_HANDLE;
        }
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vk.vkCreateSemaphore(g.device, &semaphore, nullptr, &g.image_available) != VK_SUCCESS ||
        vk.vkCreateFence(g.device, &fence, nullptr, &g.in_flight) != VK_SUCCESS) {
        return Fail("criacao de semaforos / fence");
    }
    return true;
}

// ------------------- shaders proprios, montados palavra a palavra -------------------
// Nao ha compilador GLSL nesta maquina (sem Vulkan SDK), entao os dois shaders base sao
// escritos direto em SPIR-V. Equivalem a:
//   vertice:   layout(location=0) in vec4 pos; layout(location=1) in vec4 cor;
//              layout(push_constant) uniform PC { mat4 mvp; vec4 tinta; };
//              gl_Position = mvp * vec4(pos.xyz, 1); saida = cor * tinta;
//   fragmento: cor de saida = cor recebida (sem textura).
class SpirvBuilder {
public:
    void Op(u32 opcode, std::initializer_list<u32> operands) {
        words_.push_back((static_cast<u32>(operands.size() + 1) << 16) | opcode);
        words_.insert(words_.end(), operands);
    }
    void OpString(u32 opcode, std::initializer_list<u32> before, const char* text, std::initializer_list<u32> after) {
        std::vector<u32> operands(before);
        const size_t length = std::strlen(text) + 1;
        for (size_t i = 0; i < length; i += 4) {
            u32 word = 0;
            std::memcpy(&word, text + i, std::min<size_t>(4, length - i));
            operands.push_back(word);
        }
        operands.insert(operands.end(), after);
        words_.push_back((static_cast<u32>(operands.size() + 1) << 16) | opcode);
        words_.insert(words_.end(), operands.begin(), operands.end());
    }
    std::vector<u32> Finish(u32 bound) {
        std::vector<u32> module{0x07230203, 0x00010000, 0, bound, 0};
        module.insert(module.end(), words_.begin(), words_.end());
        return module;
    }

private:
    std::vector<u32> words_;
};

std::vector<u32> BuildVertexShader() {
    // layout(location=0) in vec4 pos; (1) in vec4 cor; (2) in vec4 uv;
    // layout(push_constant) uniform PC { mat4 mvp; vec4 tinta; };
    // gl_Position = mvp * vec4(pos.xyz, 1); out(0) = cor * tinta; out(1) = uv;
    enum : u32 { kVoid = 1, kFn, kFloat, kV4, kM4, kPtrInV4, kInPos, kInColor, kInUv, kPtrOutV4, kOutColor, kOutUv, kPc, kPtrPc,
                 kPcVar, kInt, kI0, kI1, kPtrPcM4, kPtrPcV4, kPerVertex, kPtrPerVertex, kPerVertexVar, kOne, kMain, kLabel, kPos,
                 kX, kY, kZ, kVec, kMPtr, kM, kClip, kOutPosPtr, kCol, kTPtr, kT, kTinted, kUv, kBound };
    SpirvBuilder b;
    b.Op(17, {1});       // OpCapability Shader
    b.Op(14, {0, 1});    // OpMemoryModel Logical GLSL450
    b.OpString(15, {0, kMain}, "main", {kInPos, kInColor, kInUv, kOutColor, kOutUv, kPerVertexVar}); // OpEntryPoint Vertex
    b.Op(71, {kInPos, 30, 0});    // Location 0
    b.Op(71, {kInColor, 30, 1});  // Location 1
    b.Op(71, {kInUv, 30, 2});     // Location 2
    b.Op(71, {kOutColor, 30, 0}); // Location 0
    b.Op(71, {kOutUv, 30, 1});    // Location 1
    b.Op(71, {kPc, 2});           // Block
    b.Op(72, {kPc, 0, 35, 0});    // mvp: Offset 0
    b.Op(72, {kPc, 0, 5});        // ColMajor
    b.Op(72, {kPc, 0, 7, 16});    // MatrixStride 16
    b.Op(72, {kPc, 1, 35, 64});   // tinta: Offset 64
    b.Op(71, {kPerVertex, 2});    // Block
    b.Op(72, {kPerVertex, 0, 11, 0}); // BuiltIn Position
    b.Op(19, {kVoid});
    b.Op(33, {kFn, kVoid});
    b.Op(22, {kFloat, 32});
    b.Op(23, {kV4, kFloat, 4});
    b.Op(24, {kM4, kV4, 4});
    b.Op(32, {kPtrInV4, 1, kV4});
    b.Op(59, {kPtrInV4, kInPos, 1});
    b.Op(59, {kPtrInV4, kInColor, 1});
    b.Op(59, {kPtrInV4, kInUv, 1});
    b.Op(32, {kPtrOutV4, 3, kV4});
    b.Op(59, {kPtrOutV4, kOutColor, 3});
    b.Op(59, {kPtrOutV4, kOutUv, 3});
    b.Op(30, {kPc, kM4, kV4});
    b.Op(32, {kPtrPc, 9, kPc});
    b.Op(59, {kPtrPc, kPcVar, 9});
    b.Op(21, {kInt, 32, 1});
    b.Op(43, {kInt, kI0, 0});
    b.Op(43, {kInt, kI1, 1});
    b.Op(32, {kPtrPcM4, 9, kM4});
    b.Op(32, {kPtrPcV4, 9, kV4});
    b.Op(30, {kPerVertex, kV4});
    b.Op(32, {kPtrPerVertex, 3, kPerVertex});
    b.Op(59, {kPtrPerVertex, kPerVertexVar, 3});
    b.Op(43, {kFloat, kOne, 0x3F800000});
    b.Op(54, {kVoid, kMain, 0, kFn});
    b.Op(248, {kLabel});
    b.Op(61, {kV4, kPos, kInPos});
    b.Op(81, {kFloat, kX, kPos, 0});
    b.Op(81, {kFloat, kY, kPos, 1});
    b.Op(81, {kFloat, kZ, kPos, 2});
    b.Op(80, {kV4, kVec, kX, kY, kZ, kOne});
    b.Op(65, {kPtrPcM4, kMPtr, kPcVar, kI0});
    b.Op(61, {kM4, kM, kMPtr});
    b.Op(145, {kV4, kClip, kM, kVec}); // OpMatrixTimesVector
    b.Op(65, {kPtrOutV4, kOutPosPtr, kPerVertexVar, kI0});
    b.Op(62, {kOutPosPtr, kClip});
    b.Op(61, {kV4, kCol, kInColor});
    b.Op(65, {kPtrPcV4, kTPtr, kPcVar, kI1});
    b.Op(61, {kV4, kT, kTPtr});
    b.Op(133, {kV4, kTinted, kCol, kT}); // OpFMul
    b.Op(62, {kOutColor, kTinted});
    b.Op(61, {kV4, kUv, kInUv});
    b.Op(62, {kOutUv, kUv});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(kBound);
}

std::vector<u32> BuildFragmentShader() {
    // layout(location=0) in vec4 cor; (1) in vec4 uv;
    // layout(set=0, binding=0) uniform sampler2D textura;
    // saida = cor * texture(textura, uv.xy);
    enum : u32 { kVoid = 1, kFn, kFloat, kV4, kV2, kPtrIn, kInColor, kInUv, kPtrOut, kOut, kImage, kSampled, kPtrSampled, kTexture,
                 kMain, kLabel, kColor, kUv4, kUv2, kLoaded, kTexel, kResult, kBound };
    SpirvBuilder b;
    b.Op(17, {1});
    b.Op(14, {0, 1});
    b.OpString(15, {4, kMain}, "main", {kInColor, kInUv, kOut}); // OpEntryPoint Fragment
    b.Op(16, {kMain, 7});                                        // OriginUpperLeft
    b.Op(71, {kInColor, 30, 0});
    b.Op(71, {kInUv, 30, 1});
    b.Op(71, {kOut, 30, 0});
    b.Op(71, {kTexture, 34, 0}); // DescriptorSet 0
    b.Op(71, {kTexture, 33, 0}); // Binding 0
    b.Op(19, {kVoid});
    b.Op(33, {kFn, kVoid});
    b.Op(22, {kFloat, 32});
    b.Op(23, {kV4, kFloat, 4});
    b.Op(23, {kV2, kFloat, 2});
    b.Op(32, {kPtrIn, 1, kV4});
    b.Op(59, {kPtrIn, kInColor, 1});
    b.Op(59, {kPtrIn, kInUv, 1});
    b.Op(32, {kPtrOut, 3, kV4});
    b.Op(59, {kPtrOut, kOut, 3});
    b.Op(25, {kImage, kFloat, 1, 0, 0, 0, 1, 0}); // OpTypeImage: 2D, sem profundidade, amostrada
    b.Op(27, {kSampled, kImage});                 // OpTypeSampledImage
    b.Op(32, {kPtrSampled, 0, kSampled});         // UniformConstant
    b.Op(59, {kPtrSampled, kTexture, 0});
    b.Op(54, {kVoid, kMain, 0, kFn});
    b.Op(248, {kLabel});
    b.Op(61, {kV4, kColor, kInColor});
    b.Op(61, {kV4, kUv4, kInUv});
    b.Op(79, {kV2, kUv2, kUv4, kUv4, 0, 1}); // OpVectorShuffle: uv.xy
    b.Op(61, {kSampled, kLoaded, kTexture});
    b.Op(87, {kV4, kTexel, kLoaded, kUv2}); // OpImageSampleImplicitLod
    b.Op(133, {kV4, kResult, kColor, kTexel});
    b.Op(62, {kOut, kResult});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(kBound);
}

bool CreateHostBuffer(State::HostBuffer& out, u64 size, VkBufferUsageFlags usage, const char* what) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vk.vkCreateBuffer(g.device, &info, nullptr, &out.buffer);
    if (result != VK_SUCCESS) {
        return Fail(what, result);
    }
    VkMemoryRequirements requirements;
    vk.vkGetBufferMemoryRequirements(g.device, out.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &memory);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32 type = ~0u;
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
            break;
        }
    }
    if (type == ~0u) {
        return Fail("nenhum tipo de memoria HOST_VISIBLE | HOST_COHERENT para o buffer");
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = vk.vkAllocateMemory(g.device, &allocate, nullptr, &out.memory);
    if (result != VK_SUCCESS) {
        return Fail("vkAllocateMemory", result);
    }
    vk.vkBindBufferMemory(g.device, out.buffer, out.memory, 0);
    void* mapped = nullptr;
    result = vk.vkMapMemory(g.device, out.memory, 0, size, 0, &mapped);
    if (result != VK_SUCCESS) {
        return Fail("vkMapMemory", result);
    }
    out.mapped = static_cast<u8*>(mapped);
    out.size = size;
    out.head = 0;
    return true;
}

bool CreateGeometryObjects() {
    if (!CreateHostBuffer(g.vertices, kVertexRingBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vkCreateBuffer (vertices, 32 MiB)") ||
        !CreateHostBuffer(g.indices, kIndexRingBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "vkCreateBuffer (indices, 16 MiB)")) {
        return false;
    }
    if (g.write_marker && !CreateHostBuffer(g.markers, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "vkCreateBuffer (marcadores)")) {
        g.write_marker = nullptr; // Fase 54: sem o detector, o resto segue
        g.error.clear();
    }
    std::memset(g.vertices.mapped, 0xFF, kWhiteBytes); // "cor branca" para draws sem cor por vertice
    g.vertices.head = kWhiteBytes;
    // Quad de apresentacao: posicao em coordenadas de recorte e UV; (-1, -1) e o canto de cima.
    static const float kQuad[4][5] = {{-1, -1, 0, 0, 0}, {1, -1, 0, 1, 0}, {1, 1, 0, 1, 1}, {-1, 1, 0, 0, 1}};
    std::memcpy(g.vertices.mapped + kPresentQuadOffset, kQuad, sizeof(kQuad));
    static const u16 kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(g.indices.mapped, kQuadIndices, sizeof(kQuadIndices));
    g.indices.head = kIndexReserved;
    const std::vector<u32> vertex = BuildVertexShader(), fragment = BuildFragmentShader();
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = vertex.size() * 4;
    module.pCode = vertex.data();
    VkResult result = vk.vkCreateShaderModule(g.device, &module, nullptr, &g.vertex_shader);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateShaderModule (vertice)", result);
    }
    module.codeSize = fragment.size() * 4;
    module.pCode = fragment.data();
    result = vk.vkCreateShaderModule(g.device, &module, nullptr, &g.fragment_shader);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateShaderModule (fragmento)", result);
    }
    // Descriptor set 0, binding 0: a textura do draw (imagem + sampler linear), no fragmento.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    result = vk.vkCreateDescriptorSetLayout(g.device, &set_layout, nullptr, &g.set_layout);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateDescriptorSetLayout", result);
    }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; // Fase 59: o set de uma textura descartada volta
    pool.maxSets = kMaxTextures;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    result = vk.vkCreateDescriptorPool(g.device, &pool, nullptr, &g.descriptor_pool);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateDescriptorPool", result);
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    result = vk.vkCreateSampler(g.device, &sampler, nullptr, &g.sampler);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateSampler", result);
    }
    if (!CreateHostBuffer(g.staging, kStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "vkCreateBuffer (staging de texturas, 40 MiB)")) {
        return false;
    }
    if (!CreateHostBuffer(g.frame_staging, 2 * kFrameStagingHalf, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          "vkCreateBuffer (staging de texturas do frame, 96 MiB)")) {
        return false;
    }
    if (!CreateHostBuffer(g.readback, 8 * 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "vkCreateBuffer (leitura de volta de alvos)")) {
        return false;
    }
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = g.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    result = vk.vkAllocateCommandBuffers(g.device, &allocate, &g.upload_command);
    if (result != VK_SUCCESS) {
        return Fail("vkAllocateCommandBuffers (upload)", result);
    }

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 80}; // mat4 + vec4
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &g.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    result = vk.vkCreatePipelineLayout(g.device, &layout, nullptr, &g.pipeline_layout);
    if (result != VK_SUCCESS) {
        return Fail("vkCreatePipelineLayout", result);
    }
    // Shaders do jogo: anel de storage buffers e um pool de descriptor sets refeito a cada frame.
    if (!CreateHostBuffer(g.storage, kStorageRingBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "vkCreateBuffer (storage)") ||
        !CreateHostBuffer(g.statics, kStaticBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          "vkCreateBuffer (geometria estatica, 768 MiB)")) {
        return false;
    }
    std::memset(g.storage.mapped, 0, kZeroBytes);
    g.storage.head = kZeroBytes;
    const VkDescriptorPoolSize frame_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(kMaxDrawsPerFrame * 24)},
                                                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<u32>(kMaxDrawsPerFrame * 16)},
                                                {VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<u32>(kMaxDrawsPerFrame * 16)}};
    VkDescriptorPoolCreateInfo frame_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    frame_pool.maxSets = static_cast<u32>(kMaxDrawsPerFrame);
    frame_pool.poolSizeCount = 3;
    frame_pool.pPoolSizes = frame_sizes;
    result = vk.vkCreateDescriptorPool(g.device, &frame_pool, nullptr, &g.frame_pool);
    if (result != VK_SUCCESS) {
        return Fail("vkCreateDescriptorPool (frame)", result);
    }
    if (!CreateFallbackViews()) {
        return false;
    }
    PrewarmTargetPool(); // Fase 61
    // Textura 0: branco 1x1, usada pelos draws sem textura reconhecida.
    const u32 white = 0xFFFFFFFFu;
    TextureData data;
    data.format = TextureData::kRgba8;
    data.width = data.height = 1;
    data.data = &white;
    data.bytes = 4;
    return CreateTexture(0, data);
}

VkComponentSwizzle SwizzleComponent(u32 selector, VkComponentSwizzle identity) {
    switch (selector) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 1: return VK_COMPONENT_SWIZZLE_ONE;
    case 4: return VK_COMPONENT_SWIZZLE_R;
    case 5: return VK_COMPONENT_SWIZZLE_G;
    case 6: return VK_COMPONENT_SWIZZLE_B;
    case 7: return VK_COMPONENT_SWIZZLE_A;
    default: return identity;
    }
}

// Fase 39: copia os pixels para a metade do staging deste frame e anota a copia; o DrawFrame grava
// todas no comeco do command buffer do frame, antes dos draws. Sem espera nenhuma. Devolve false
// se nao coube (o chamador faz a copia antiga, com submissao propria e vkQueueWaitIdle). Com g.mutex.
// Fase 61: buffer de envio proprio (visivel pela CPU), liberado quando o frame em que foi gravado terminar.
bool CreateTempStaging(u64 bytes, State::TempStaging* temp, void** mapped) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.vkCreateBuffer(g.device, &info, nullptr, &temp->buffer) != VK_SUCCESS) {
        temp->buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements;
    vk.vkGetBufferMemoryRequirements(g.device, temp->buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &memory);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32 type = ~0u;
    for (u32 i = 0; i < memory.memoryTypeCount && type == ~0u; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
        }
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == ~0u || vk.vkAllocateMemory(g.device, &allocate, nullptr, &temp->memory) != VK_SUCCESS ||
        vk.vkBindBufferMemory(g.device, temp->buffer, temp->memory, 0) != VK_SUCCESS ||
        vk.vkMapMemory(g.device, temp->memory, 0, bytes, 0, mapped) != VK_SUCCESS) {
        if (temp->memory) vk.vkFreeMemory(g.device, temp->memory, nullptr);
        vk.vkDestroyBuffer(g.device, temp->buffer, nullptr);
        temp->buffer = VK_NULL_HANDLE;
        temp->memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool StageFrameUpload(VkImage image, VkImageLayout old_layout, const TextureData& data, u32 levels, u32 layers, const VkBufferImageCopy* regions) {
    const u64 offset = (g.frame_head + 15) & ~15ull;
    if (!g.frame_staging.mapped) {
        return false;
    }
    u64 base = 0;
    VkBuffer source = VK_NULL_HANDLE;
    if (offset + data.bytes > kFrameStagingHalf) {
        // Fase 61: o staging do frame encheu (entrada de area: centenas de texturas juntas). Antes a thread de render
        // caia no envio imediato, que espera a GPU ficar ociosa (~6 ms por textura); agora vai por um buffer proprio,
        // como as threads de textura ja faziam (UploadThroughTempStaging).
        State::TempStaging temp;
        void* mapped = nullptr;
        if (!CreateTempStaging(data.bytes, &temp, &mapped)) {
            return false;
        }
        std::memcpy(mapped, data.data, data.bytes);
        g.temp_staging.push_back(temp);
        ++g_temp_staging_count;
        source = temp.buffer;
    } else {
        base = g.frame_half * kFrameStagingHalf + offset;
        std::memcpy(g.frame_staging.mapped + base, data.data, data.bytes);
        g.frame_head = offset + data.bytes;
    }
    State::PendingUpload pending;
    pending.source = source;
    pending.image = image;
    pending.old_layout = old_layout;
    pending.levels = levels;
    pending.layers = layers;
    pending.region_count = levels;
    for (u32 i = 0; i < levels; ++i) {
        pending.regions[i] = regions[i];
        pending.regions[i].bufferOffset += base;
    }
    g.pending_uploads.push_back(pending);
    ++g_upload_stats.deferred;
    g_upload_stats.deferred_bytes += data.bytes;
    return true;
}

// No comeco do command buffer do frame (DrawFrame, com g.mutex).
void RecordFrameUploads(VkCommandBuffer command) {
    g.staged_last_frame = static_cast<u32>(g.pending_uploads.size());
    g.staged_bytes_last_frame = g.frame_head;
    for (const State::PendingUpload& pending : g.pending_uploads) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = pending.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, pending.levels, 0, pending.layers};
        barrier.oldLayout = pending.old_layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = pending.old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &barrier);
        vk.vkCmdCopyBufferToImage(command, pending.source ? pending.source : g.frame_staging.buffer, pending.image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pending.region_count, pending.regions);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &barrier);
    }
    g.pending_uploads.clear();
    for (State::TempStaging& temp : g.temp_staging) { // Fase 59: gravados neste frame
        if (!temp.recorded_serial) {
            temp.recorded_serial = g.frame_serial + 1;
        }
    }
    g.frame_half ^= 1; // o proximo frame escreve na outra metade (a deste so volta depois da fence dele)
    g.frame_head = 0;
}

// VkImage + memoria + VkImageView + copia dos pixels (staging -> imagem) + descriptor set.
// Chamado com g.mutex. Fase 39: a copia vai para o command buffer do frame (StageFrameUpload); so
// a textura que nao cabe no staging do frame usa a copia antiga, sincrona (vkQueueWaitIdle).
// Fase 45: imagem + memoria + views, sem g.mutex (as chamadas de criacao do Vulkan podem vir de
// qualquer thread). E a parte cara de uma textura nova (vkAllocateMemory por textura): roda na
// thread que leu a textura, e a thread de render so faz a copia para o staging e o registro.
std::atomic<int> g_prepare_inflight{0};
std::atomic<bool> g_prepare_open{false}; // dispositivo vivo

const VkFormat kTextureFormats[TextureData::kFormatCount] = {
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_BC2_UNORM_BLOCK,
    VK_FORMAT_BC4_UNORM_BLOCK,      VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
    // Fase 22: ponto flutuante e 16 bits por canal (8, 8 e 16 bytes por pixel no staging)
    VK_FORMAT_R16G16B16A16_SFLOAT,  VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT};
const VkFormat kTextureSrgbFormats[TextureData::kFormatCount] = {
    VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,  VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_BC2_SRGB_BLOCK,
    VK_FORMAT_BC4_UNORM_BLOCK,     VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT};

// ---- Fase 59: pool de memoria das texturas ----
// Cada textura nova pedia o proprio vkAllocateMemory (~0,5-1 ms no driver da AMD, e em serie: o driver
// tranca a alocacao). Na entrada de uma area (50-130 texturas) isso virava a fila mais longa da espera do
// frame, e mais threads de textura so pioravam (8 threads: 100 ms; 2 threads: 60 ms). Agora as texturas
// sao reservadas dentro de blocos de 128 MiB (primeiro encaixe, com os buracos devolvidos e juntados), e
// o teto de alocacoes do driver (4096 no Windows) deixa de pesar. Texturas maiores que meio bloco
// continuam com alocacao propria.
constexpr u64 kTexturePoolChunk = 128ull << 20;
struct TexturePool {
    struct Chunk {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u32 type = 0;
        std::map<u64, u64> free; // posicao -> tamanho livre
    };
    std::mutex mutex;
    std::vector<Chunk> chunks;
    u64 used = 0;
} g_texture_pool;

bool PoolTake(TexturePool::Chunk& chunk, const VkMemoryRequirements& requirements, u64* offset_out) {
    const u64 alignment = std::max<u64>(requirements.alignment, 256);
    for (auto it = chunk.free.begin(); it != chunk.free.end(); ++it) {
        const u64 start = (it->first + alignment - 1) / alignment * alignment;
        const u64 end = it->first + it->second;
        if (start + requirements.size <= end) {
            const u64 block = it->first;
            chunk.free.erase(it);
            if (start > block) {
                chunk.free[block] = start - block;
            }
            if (end > start + requirements.size) {
                chunk.free[start + requirements.size] = end - (start + requirements.size);
            }
            *offset_out = start;
            return true;
        }
    }
    return false;
}

// Fase 61: o proximo bloco do pool e reservado ANTES de faltar, numa thread propria. Alocar 128 MiB no driver leva
// 10-15 ms, e era pago por quem pedisse a primeira reserva que nao coubesse - na entrada de uma area (225 texturas),
// com o pool travado para todos: uma textura de 4 KB na thread de render levou 13 ms. Livre < 48 MiB de um tipo de
// memoria -> um bloco a mais desse tipo em segundo plano (um por vez por tipo).
constexpr u64 kTexturePoolLowWater = 48ull << 20;
std::atomic<u32> g_pool_growing_types{0}; // bit por tipo de memoria com reserva em andamento
std::atomic<u64> g_pool_grown_ahead{0};

void GrowPoolAhead(u32 type) { // com g_texture_pool.mutex
    u64 free_bytes = 0;
    for (const TexturePool::Chunk& chunk : g_texture_pool.chunks) {
        if (chunk.type == type) {
            for (const auto& [offset, size] : chunk.free) {
                free_bytes += size;
            }
        }
    }
    if (free_bytes >= kTexturePoolLowWater || type >= 32 || (g_pool_growing_types.fetch_or(1u << type) & (1u << type))) {
        return;
    }
    std::thread([type] {
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = kTexturePoolChunk;
        allocate.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (g.device && vk.vkAllocateMemory(g.device, &allocate, nullptr, &memory) == VK_SUCCESS) {
            const TimedLock lock(g_texture_pool.mutex);
            TexturePool::Chunk chunk;
            chunk.type = type;
            chunk.memory = memory;
            chunk.free[0] = kTexturePoolChunk;
            g_texture_pool.chunks.push_back(std::move(chunk));
            ++g_pool_grown_ahead;
        }
        g_pool_growing_types.fetch_and(~(1u << type));
    }).detach();
}

bool PoolAllocate(const VkMemoryRequirements& requirements, u32 type, u32* chunk_out, u64* offset_out, VkDeviceMemory* memory_out) {
    if (requirements.size > kTexturePoolChunk / 2 || std::getenv("LD_NO_TEXTURE_POOL")) {
        return false;
    }
    const TimedLock lock(g_texture_pool.mutex);
    for (u32 c = 0; c < g_texture_pool.chunks.size(); ++c) {
        TexturePool::Chunk& chunk = g_texture_pool.chunks[c];
        if (chunk.type == type && PoolTake(chunk, requirements, offset_out)) {
            *chunk_out = c;
            *memory_out = chunk.memory;
            g_texture_pool.used += requirements.size;
            GrowPoolAhead(type); // Fase 61
            return true;
        }
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = kTexturePoolChunk;
    allocate.memoryTypeIndex = type;
    TexturePool::Chunk chunk;
    chunk.type = type;
    if (vk.vkAllocateMemory(g.device, &allocate, nullptr, &chunk.memory) != VK_SUCCESS) {
        return false;
    }
    chunk.free[0] = kTexturePoolChunk;
    g_texture_pool.chunks.push_back(std::move(chunk));
    TexturePool::Chunk& fresh = g_texture_pool.chunks.back();
    if (!PoolTake(fresh, requirements, offset_out)) {
        return false;
    }
    *chunk_out = static_cast<u32>(g_texture_pool.chunks.size() - 1);
    *memory_out = fresh.memory;
    g_texture_pool.used += requirements.size;
    GrowPoolAhead(type); // Fase 61
    return true;
}

void PoolFree(u32 chunk_index, u64 offset, u64 size) {
    const TimedLock lock(g_texture_pool.mutex);
    if (chunk_index >= g_texture_pool.chunks.size() || !size) {
        return;
    }
    auto& free = g_texture_pool.chunks[chunk_index].free;
    auto next = free.lower_bound(offset);
    u64 start = offset, length = size;
    if (next != free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) { // junta com o buraco de antes
            start = previous->first;
            length += previous->second;
            free.erase(previous);
        }
    }
    if (next != free.end() && next->first == offset + size) { // e com o de depois
        length += next->second;
        free.erase(next);
    }
    free[start] = length;
    g_texture_pool.used -= std::min(g_texture_pool.used, size);
}

void DestroyTextureObjects(State::Texture& texture) {
    if (texture.array_view) vk.vkDestroyImageView(g.device, texture.array_view, nullptr);
    if (texture.face_view) vk.vkDestroyImageView(g.device, texture.face_view, nullptr);
    if (texture.view) vk.vkDestroyImageView(g.device, texture.view, nullptr);
    if (texture.image) vk.vkDestroyImage(g.device, texture.image, nullptr);
    if (texture.memory) vk.vkFreeMemory(g.device, texture.memory, nullptr);
    if (texture.pool_chunk != ~0u) {
        PoolFree(texture.pool_chunk, texture.pool_offset, texture.pool_size);
    }
    texture = State::Texture{};
}

bool CreateTextureObjects(const TextureData& data, State::Texture& texture) {
    if (!data.data || !data.bytes || data.bytes > kStagingBytes || !data.width || !data.height || data.format >= TextureData::kFormatCount) {
        return false;
    }
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    const bool volume = data.depth > 1;
    const bool cube = data.cube && !volume;
    const u32 layers = cube ? 6 : 1;
    texture.view_type = volume ? 3 : cube ? 1 : 0;
    texture.width = data.width;
    texture.height = data.height;
    texture.depth = std::max(1u, data.depth);
    texture.layers = layers;
    texture.bytes = data.bytes;
    image.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    image.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image.format = (data.srgb ? kTextureSrgbFormats : kTextureFormats)[data.format];
    image.extent = {data.width, data.height, std::max(1u, data.depth)};
    const u32 levels = volume ? 1u : std::clamp(data.levels, 1u, 16u);
    texture.levels = levels;
    image.mipLevels = levels;
    image.arrayLayers = layers;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(g.device, &image, nullptr, &texture.image) != VK_SUCCESS) {
        texture.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements;
    vk.vkGetImageMemoryRequirements(g.device, texture.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &memory);
    u32 type = ~0u;
    for (u32 pass = 0; pass < 2 && type == ~0u; ++pass) { // primeiro memoria da placa; senao, qualquer uma
        for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
            const bool local = memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            if ((requirements.memoryTypeBits & (1u << i)) && (local || pass == 1)) {
                type = i;
                break;
            }
        }
    }
    VkDeviceMemory pooled = VK_NULL_HANDLE; // Fase 59: primeiro o pool de blocos
    u32 chunk = ~0u;
    u64 offset = 0;
    if (type != ~0u && PoolAllocate(requirements, type, &chunk, &offset, &pooled)) {
        texture.pool_chunk = chunk;
        texture.pool_offset = offset;
        texture.pool_size = requirements.size;
        if (vk.vkBindImageMemory(g.device, texture.image, pooled, offset) != VK_SUCCESS) {
            DestroyTextureObjects(texture);
            return false;
        }
    } else {
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (type == ~0u || vk.vkAllocateMemory(g.device, &allocate, nullptr, &texture.memory) != VK_SUCCESS) {
            texture.memory = VK_NULL_HANDLE;
            DestroyTextureObjects(texture);
            return false;
        }
        vk.vkBindImageMemory(g.device, texture.image, texture.memory, 0);
    }

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture.image;
    view.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    if (data.swizzle) { // ordem dos canais do descritor do jogo (ex.: BGRA nas RGBA8)
        view.components = {SwizzleComponent(data.swizzle & 7, VK_COMPONENT_SWIZZLE_R),
                           SwizzleComponent((data.swizzle >> 3) & 7, VK_COMPONENT_SWIZZLE_G),
                           SwizzleComponent((data.swizzle >> 6) & 7, VK_COMPONENT_SWIZZLE_B),
                           SwizzleComponent((data.swizzle >> 9) & 7, VK_COMPONENT_SWIZZLE_A)};
    }
    bool views_ok = vk.vkCreateImageView(g.device, &view, nullptr, &texture.view) == VK_SUCCESS;
    if (views_ok && cube) {
        VkImageViewCreateInfo array_view = view;
        array_view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        views_ok = vk.vkCreateImageView(g.device, &array_view, nullptr, &texture.array_view) == VK_SUCCESS;
        VkImageViewCreateInfo face_view = view;
        face_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        face_view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}; // baseArrayLayer 0, layerCount 1
        views_ok = views_ok && vk.vkCreateImageView(g.device, &face_view, nullptr, &texture.face_view) == VK_SUCCESS;
    } else if (views_ok && !volume) { // Fase 32: 2D num slot que o shader declara como array 2D
        VkImageViewCreateInfo array_view = view;
        array_view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        views_ok = vk.vkCreateImageView(g.device, &array_view, nullptr, &texture.array_view) == VK_SUCCESS;
    }
    if (!views_ok) {
        DestroyTextureObjects(texture);
        return false;
    }
    return true;
}

// Fase 54e: envio de textura com submissao propria. Se a fila voltar com o dispositivo perdido, este
// envio (ou o que ja estava na fila antes dele) foi o culpado: diz qual textura era.
void CheckImmediateUpload(VkResult waited, const char* where, u64 key, u32 width, u32 height, u32 depth, u32 layers, u32 levels,
                          u32 format, u64 bytes, const VkBufferImageCopy* regions) {
    ++g.immediate_since_frame;
    if (waited == VK_SUCCESS) {
        return;
    }
    std::printf("[GPU TRAVADA] envio imediato de textura (%s) voltou %s: chave 0x%llx %ux%ux%u, %u camadas, %u niveis, formato %u, %llu bytes%s",
                where, ResultName(waited), static_cast<unsigned long long>(key), width, height, depth, layers, levels, format,
                static_cast<unsigned long long>(bytes), "\n");
    for (u32 i = 0; i < levels && i < 16; ++i) {
        std::printf("[GPU TRAVADA]   nivel %u: offset %llu, %ux%ux%u%s", i, static_cast<unsigned long long>(regions[i].bufferOffset),
                    regions[i].imageExtent.width, regions[i].imageExtent.height, regions[i].imageExtent.depth, "\n");
    }
    std::fflush(stdout);
}

// Um trecho por nivel de mip; em cada nivel, as faces do cubo vem em sequencia no staging.
void TextureRegions(const TextureData& data, const State::Texture& texture, VkBufferImageCopy* regions) {
    for (u32 level = 0; level < texture.levels; ++level) {
        regions[level] = {};
        regions[level].bufferOffset = level == 0 ? 0 : data.level_offsets[level];
        regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, texture.layers};
        regions[level].imageExtent = {std::max(1u, data.width >> level), std::max(1u, data.height >> level),
                                      level == 0 ? std::max(1u, data.depth) : 1u};
    }
}

bool RegisterTexture(u64 key, const TextureData& data, State::Texture& texture);

// Copia dos pixels (staging do frame, ou a copia antiga com espera) + descriptor set + registro.
// Com g.mutex. Em caso de falha os objetos sao destruidos.
bool FinishTexture(u64 key, const TextureData& data, State::Texture& texture) {
    const bool volume = data.depth > 1;
    const bool cube = data.cube && !volume;
    const u32 layers = texture.layers, levels = texture.levels;
    if (g.textures.size() >= kMaxTextures) {
        DestroyTextureObjects(texture);
        return false;
    }
    VkBufferImageCopy regions[16]{};
    TextureRegions(data, texture, regions);
    const bool staged = StageFrameUpload(texture.image, VK_IMAGE_LAYOUT_UNDEFINED, data, levels, layers, regions);
    if (!staged) {
    ++g_upload_stats.immediate;
    // pixels -> staging -> imagem
    std::memcpy(g.staging.mapped, data.data, data.bytes);
    vk.vkResetCommandBuffer(g.upload_command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(g.upload_command, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.vkCmdPipelineBarrier(g.upload_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &barrier);
    vk.vkCmdCopyBufferToImage(g.upload_command, g.staging.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vk.vkCmdPipelineBarrier(g.upload_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &barrier);
    vk.vkEndCommandBuffer(g.upload_command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.upload_command;
    // Fase 54e: espera o frame em voo terminar antes da copia propria (com esta espera a cutscene da casa
    // dos Simpsons deixou de derrubar o driver) e confere se o que ja estava na fila terminou bem.
    const VkResult before = QueueWaitIdle();
    if (before != VK_SUCCESS) {
        std::printf("[GPU TRAVADA] a fila ja voltou %s ANTES do envio imediato da textura 0x%llx%s", ResultName(before),
                    static_cast<unsigned long long>(key), "\n");
    }
    QueueSubmit(1, &submit, VK_NULL_HANDLE);
    CheckImmediateUpload(before == VK_SUCCESS ? QueueWaitIdle() : VK_SUCCESS, "nova", key, data.width, data.height, data.depth,
                         layers, levels, static_cast<u32>(data.format), data.bytes, regions);
    }
    (void)cube;
    return RegisterTexture(key, data, texture);
}

// Descriptor set do pipeline base + entrada no mapa. Com g.mutex. A copia dos pixels ja foi gravada.
bool RegisterTexture(u64 key, const TextureData& data, State::Texture& texture) {
    const bool volume = data.depth > 1;
    const bool cube = data.cube && !volume;
    // (o descriptor set e do pipeline base, que so amostra 2D: as 3D e os cubos ficam sem ele)
    const bool flat = !volume && !cube;
    if (flat) {
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool = g.descriptor_pool;
        sets.descriptorSetCount = 1;
        sets.pSetLayouts = &g.set_layout;
        if (vk.vkAllocateDescriptorSets(g.device, &sets, &texture.set) != VK_SUCCESS) {
            // a copia ja pode estar na fila do frame: a imagem so e destruida no fim (fica orfa, rara)
            texture.set = VK_NULL_HANDLE;
            return false;
        }
        VkDescriptorImageInfo image_info{g.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = texture.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vk.vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
    }
    texture.last_used = g.frame_serial + 1; // Fase 59: recem-chegada nao e descartada
    g.textures[key] = texture;
    g.texture_bytes += data.bytes;
    return true;
}

// VkImage + memoria + VkImageView + copia dos pixels (staging -> imagem) + descriptor set.
// Chamado com g.mutex. Fase 39: a copia vai para o command buffer do frame (StageFrameUpload); so
// a textura que nao cabe no staging do frame usa a copia antiga, sincrona (vkQueueWaitIdle).
bool CreateTexture(u64 key, const TextureData& data) {
    const HitchClock hitch(HitchClock::kTexture);
    if (g.textures.size() >= kMaxTextures) {
        return false;
    }
    State::Texture texture;
    return CreateTextureObjects(data, texture) && FinishTexture(key, data, texture);
}

void DestroyPipelines() {
    for (auto& [key, pipeline] : g.pipelines) {
        vk.vkDestroyPipeline(g.device, pipeline, nullptr);
    }
    g.pipelines.clear();
    g.queue_draws.clear(); // os draws na fila apontam para pipelines que deixaram de existir
}

// ---- Fase 70: cache de pipelines do Vulkan em disco ----
// O driver da AMD ja guarda os pipelines compilados, mas pelo caminho do .exe: outra pasta, outro nome ou
// o cache do driver limpo e a tela de carregamento recompila os ~14 mil pipelines do aquecimento (49 s no
// lugar de 6 s na RX 6600, medido na Fase 69). Este cache e do port (assets\shaders_spv\pipeline_cache.bin):
// lido na abertura, passado a todo vkCreateGraphicsPipelines (inclusive bibliotecas GPL) e salvo quando o
// aquecimento termina, a cada 5 min com pipelines novos e na saida, sempre por arquivo temporario + troca
// (uma sessao que caiu no meio nao estraga o arquivo). Cabecalho proprio (marca, versao, fabricante,
// dispositivo, versao do driver, tamanho e FNV-1a dos dados) alem do cabecalho do Vulkan (fabricante,
// dispositivo, UUID do cache): qualquer diferenca descarta o arquivo. LD_PIPELINE_CACHE=0 desliga.
VkPipelineCache g_vk_cache = VK_NULL_HANDLE;
// Fase 70b: a fila do Vulkan (submit, espera, present) tem um mutex proprio, tomado sempre depois do g.mutex
// quando os dois sao necessarios. Assim o present do frame roda sem o g.mutex.
std::mutex g_queue_mutex;
std::mutex g_vk_cache_save_mutex;
std::atomic<u64> g_vk_cache_new{0};     // pipelines criados desde o ultimo salvamento
std::atomic<bool> g_vk_cache_saving{false};
u64 g_vk_cache_loaded_bytes = 0;
std::chrono::steady_clock::time_point g_vk_cache_saved_at{};
bool g_vk_cache_saved_after_prewarm = false;

struct PipelineCacheFileHeader {
    u32 magic = 0x4350444C; // "LDPC"
    u32 version = 1;
    u32 vendor = 0, device = 0, driver = 0, reserved = 0;
    u64 bytes = 0, checksum = 0;
};

u64 Fnv1a64(const u8* data, size_t size) {
    u64 hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 1099511628211ull;
    }
    return hash;
}

std::filesystem::path PipelineCachePath() {
    return std::filesystem::path(GameSpirvDirectory()) / "pipeline_cache.bin";
}

// Depois do CreateDevice. Le e confere o arquivo; sem arquivo valido, o cache comeca vazio.
void CreatePipelineCache() {
    g_vk_cache_loaded_bytes = 0;
    // Medido na RX 6600 (driver 26.6.1): com um VkPipelineCache do app, o driver da AMD deixa de usar o
    // cache proprio dele. Aquecimento quente 5,8 s -> 27 s (com o arquivo cheio, 171 MB) e o frio nao melhora
    // (49 s -> 48,6 s: as entradas nao valem para outro .exe, o arquivo so dobrou). Por isso desligado na AMD
    // por padrao; ligado nos outros fabricantes. LD_PIPELINE_CACHE=1 / 0 forca.
    const char* env = std::getenv("LD_PIPELINE_CACHE");
    const bool amd = g.properties.vendorID == 0x1002;
    const bool wanted = env && env[0] ? env[0] != '0' : !amd;
    if (!wanted || GameSpirvDirectory().empty()) {
        std::printf("[PIPELINE-CACHE] desligado%s\n", GameSpirvDirectory().empty() ? " (sem pasta de dados)"
                                                     : env && env[0]              ? " (LD_PIPELINE_CACHE=0)"
                                                                                  : " (AMD: o cache do proprio driver e mais rapido; LD_PIPELINE_CACHE=1 liga)");
        return;
    }
    std::vector<u8> data;
    const char* why = "sem arquivo";
    if (std::FILE* f = _wfopen(PipelineCachePath().c_str(), L"rb")) {
        PipelineCacheFileHeader header;
        const PipelineCacheFileHeader expected{0x4350444C, 1, g.properties.vendorID, g.properties.deviceID, g.properties.driverVersion};
        if (std::fread(&header, sizeof(header), 1, f) != 1 || header.magic != expected.magic || header.version != expected.version) {
            why = "formato desconhecido, descartado";
        } else if (header.vendor != expected.vendor || header.device != expected.device || header.driver != expected.driver) {
            why = "de outra placa ou outro driver, descartado";
        } else if (header.bytes < 32 || header.bytes > (2ull << 30)) {
            why = "tamanho invalido, descartado";
        } else {
            data.resize(header.bytes);
            if (std::fread(data.data(), 1, data.size(), f) != data.size() || Fnv1a64(data.data(), data.size()) != header.checksum) {
                why = "corrompido (soma nao confere), descartado";
                data.clear();
            } else {
                // cabecalho do Vulkan: tamanho, versao 1, fabricante, dispositivo, UUID
                u32 vk_header[4];
                std::memcpy(vk_header, data.data(), sizeof(vk_header));
                if (vk_header[0] < 32 || vk_header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vk_header[2] != g.properties.vendorID ||
                    vk_header[3] != g.properties.deviceID ||
                    std::memcmp(data.data() + 16, g.properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                    why = "UUID do cache do driver mudou, descartado";
                    data.clear();
                } else {
                    why = nullptr;
                }
            }
        }
        std::fclose(f);
        if (why) {
            data.clear();
        }
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    VkResult result = vk.vkCreatePipelineCache(g.device, &info, nullptr, &g_vk_cache);
    if (result != VK_SUCCESS && !data.empty()) { // o driver recusou os dados: comeca vazio
        why = "recusado pelo driver, descartado";
        data.clear();
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        result = vk.vkCreatePipelineCache(g.device, &info, nullptr, &g_vk_cache);
    }
    if (result != VK_SUCCESS) {
        g_vk_cache = VK_NULL_HANDLE;
        std::printf("[PIPELINE-CACHE] vkCreatePipelineCache falhou (%s): segue sem cache\n", ResultName(result));
        return;
    }
    g_vk_cache_loaded_bytes = data.size();
    g_vk_cache_saved_at = std::chrono::steady_clock::now();
    if (why) {
        std::printf("[PIPELINE-CACHE] comecando vazio: %s\n", why);
    } else {
        std::printf("[PIPELINE-CACHE] %.1f MB lidos de %s\n", data.size() / 1048576.0, PipelineCachePath().string().c_str());
    }
}

// Grava o cache (qualquer thread). Arquivo temporario + MoveFileEx: o anterior so e trocado com o novo inteiro.
bool SavePipelineCacheFile(const char* reason) {
    if (!g_vk_cache || !g.device) {
        return false;
    }
    std::lock_guard lock(g_vk_cache_save_mutex);
    const auto start = std::chrono::steady_clock::now();
    const u64 created = g_vk_cache_new.exchange(0);
    size_t size = 0;
    if (vk.vkGetPipelineCacheData(g.device, g_vk_cache, &size, nullptr) != VK_SUCCESS || size < 32) {
        return false;
    }
    std::vector<u8> data(size);
    if (vk.vkGetPipelineCacheData(g.device, g_vk_cache, &size, data.data()) != VK_SUCCESS) {
        return false;
    }
    data.resize(size);
    PipelineCacheFileHeader header{0x4350444C, 1, g.properties.vendorID, g.properties.deviceID, g.properties.driverVersion};
    header.bytes = data.size();
    header.checksum = Fnv1a64(data.data(), data.size());
    const std::filesystem::path path = PipelineCachePath();
    std::filesystem::path temp = path;
    temp += L".novo";
    bool ok = false;
    if (std::FILE* f = _wfopen(temp.c_str(), L"wb")) {
        ok = std::fwrite(&header, sizeof(header), 1, f) == 1 && std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0 &&
             _commit(_fileno(f)) == 0;
        ok = std::fclose(f) == 0 && ok;
    }
    ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DeleteFileW(temp.c_str());
        g_vk_cache_new += created;
    }
    g_vk_cache_saved_at = std::chrono::steady_clock::now();
    std::printf("[PIPELINE-CACHE] %s (%s): %.1f MB, %llu pipelines novos desde o ultimo, %.0f ms\n", ok ? "salvo" : "NAO salvo", reason,
                data.size() / 1048576.0, static_cast<unsigned long long>(created),
                std::chrono::duration<double, std::milli>(g_vk_cache_saved_at - start).count());
    return ok;
}

// Thread de render, no fim do DrawFrame: salva em segundo plano quando o aquecimento termina e a cada 5 min.
void MaybeSavePipelineCache(bool prewarm_finished) {
    if (!g_vk_cache || g_vk_cache_saving.load() || !g_vk_cache_new.load()) {
        return;
    }
    const char* reason = nullptr;
    if (prewarm_finished && !g_vk_cache_saved_after_prewarm) {
        g_vk_cache_saved_after_prewarm = true;
        reason = "aquecimento terminou";
    } else if (std::chrono::steady_clock::now() - g_vk_cache_saved_at > std::chrono::minutes(5)) {
        reason = "5 min";
        g_vk_cache_saved_at = std::chrono::steady_clock::now();
    }
    if (reason) {
        g_vk_cache_saving = true;
        std::thread([reason] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SavePipelineCacheFile(reason);
            g_vk_cache_saving = false;
        }).detach();
    }
}

VkResult QueueWaitIdle() {
    std::lock_guard lock(g_queue_mutex);
    return vk.vkQueueWaitIdle(g.queue);
}

VkResult QueueSubmit(u32 count, const VkSubmitInfo* submits, VkFence fence) {
    std::lock_guard lock(g_queue_mutex);
    return vk.vkQueueSubmit(g.queue, count, submits, fence);
}

// Todo pipeline grafico do port passa por aqui (com o cache, se houver).
VkResult CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo* info, VkPipeline* pipeline) {
    const VkResult result = vk.vkCreateGraphicsPipelines(g.device, g_vk_cache, 1, info, nullptr, pipeline);
    if (result == VK_SUCCESS) {
        ++g_vk_cache_new;
    }
    return result;
}

// Pipeline base: sem cull, blend alfa padrao (ou desligado), viewport e scissor dinamicos, e a
// entrada de vertice montada com o stride e os offsets que o fetch shader do draw descreve.
VkPipeline GetPipeline(const DrawCall& draw) {
    const bool strip = draw.primitive == DrawCall::kTriangleStrip;
    const auto key = std::make_tuple(draw.stride, static_cast<u32>(draw.position_format), draw.position_offset, draw.has_color,
                                     draw.color_offset, strip, draw.blend, static_cast<u32>(draw.uv_format), draw.uv_offset);
    if (const auto found = g.pipelines.find(key); found != g.pipelines.end()) {
        return found->second;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.fragment_shader;
    stages[1].pName = "main";

    // binding 0 = vertices do jogo; binding 1 = cor branca fixa (por instancia) quando nao ha cor
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0] = {0, draw.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    bindings[1] = {1, 4, VK_VERTEX_INPUT_RATE_INSTANCE};
    static const VkFormat kPositionFormats[] = {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                VK_FORMAT_R16G16B16A16_SFLOAT};
    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0] = {0, 0, kPositionFormats[draw.position_format], draw.position_offset};
    attributes[1] = draw.has_color ? VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R8G8B8A8_UNORM, draw.color_offset}
                                   : VkVertexInputAttributeDescription{1, 1, VK_FORMAT_R8G8B8A8_UNORM, 0};
    // UV: do vertice, ou (sem UV) dois bytes do branco fixo -> (1, 1), que cai na textura branca
    attributes[2] = draw.uv_format == DrawCall::kUvFloat2  ? VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, draw.uv_offset}
                    : draw.uv_format == DrawCall::kUvHalf2 ? VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R16G16_SFLOAT, draw.uv_offset}
                                                           : VkVertexInputAttributeDescription{2, 1, VK_FORMAT_R8G8_UNORM, 0};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = (draw.has_color && draw.uv_format != DrawCall::kUvNone) ? 1 : 2;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = draw.blend ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = g.pipeline_layout;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g.format.format;
    info.pNext = &rendering;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = CreateGraphicsPipeline(&info, &pipeline);
    if (result != VK_SUCCESS) {
        Fail("vkCreateGraphicsPipelines", result);
        return VK_NULL_HANDLE;
    }
    ++g.pipelines_created;
    g.pipelines[key] = pipeline;
    return pipeline;
}

VkFormat TargetVkFormat(GameDraw::TargetFormat format) {
    static const VkFormat kFormats[GameDraw::kTargetFormatCount] = {
        VK_FORMAT_R8_UNORM,           VK_FORMAT_R8G8_UNORM,          VK_FORMAT_R8G8B8A8_UNORM,       VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_R16_UNORM,          VK_FORMAT_R16G16_UNORM,        VK_FORMAT_R16G16B16A16_UNORM,   VK_FORMAT_R16_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT,      VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,           VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_D32_SFLOAT_S8_UINT}; // Fase 34: profundidade com estencil de 8 bits
    return format < GameDraw::kTargetFormatCount ? kFormats[format] : VK_FORMAT_UNDEFINED;
}

u32 FindMemoryType(const VkMemoryRequirements& requirements) {
    VkPhysicalDeviceMemoryProperties memory;
    vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &memory);
    for (u32 pass = 0; pass < 2; ++pass) { // primeiro memoria da placa; senao, qualquer uma
        for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
            const bool local = memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            if ((requirements.memoryTypeBits & (1u << i)) && (local || pass == 1)) {
                return i;
            }
        }
    }
    return ~0u;
}

// Barreira de layout simples (estagios "todos os comandos" dos dois lados: correta, sem ajuste fino).
void Transition(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, u32 layers = 1) {
    if (from == to) {
        return;
    }
    auto access = [](VkImageLayout layout) -> VkAccessFlags {
        switch (layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
        default: return 0;
        }
    };
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, layers};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = access(from);
    barrier.dstAccessMask = access(to);
    vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
}

void TransitionTarget(State::RenderTarget* target, VkImageLayout to) {
    Transition(g.command, target->image, target->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
               target->layout, to);
    target->layout = to;
}

// Fase 61: memoria de um alvo de render (ou da copia dele) dentro dos blocos do pool das texturas. A alocacao
// propria de cada alvo (8 a 16 MiB, zerada pelo driver) levava ~1 ms cada e ate 16 ms para dois alvos grandes
// no mesmo frame. Os alvos so sao liberados no fim do jogo (os blocos inteiros saem juntos). *memory fica
// nulo quando veio do pool; maior que meio bloco, alocacao propria como antes.
bool BindTargetMemory(VkImage image, VkDeviceMemory* memory) {
    VkMemoryRequirements requirements;
    vk.vkGetImageMemoryRequirements(g.device, image, &requirements);
    const u32 type = FindMemoryType(requirements);
    if (type == ~0u) {
        return false;
    }
    u32 chunk = ~0u;
    u64 offset = 0;
    VkDeviceMemory pooled = VK_NULL_HANDLE;
    if (PoolAllocate(requirements, type, &chunk, &offset, &pooled)) {
        *memory = VK_NULL_HANDLE;
        if (vk.vkBindImageMemory(g.device, image, pooled, offset) == VK_SUCCESS) {
            return true;
        }
        PoolFree(chunk, offset, requirements.size);
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vk.vkAllocateMemory(g.device, &allocate, nullptr, memory) != VK_SUCCESS) {
        *memory = VK_NULL_HANDLE;
        return false;
    }
    return vk.vkBindImageMemory(g.device, image, *memory, 0) == VK_SUCCESS;
}

// Fase 61: os blocos do pool (texturas e alvos de render) sao reservados na abertura, e nao no meio do jogo. Uma
// imagem de teste so diz o tipo de memoria.
void PrewarmTargetPool() {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image.extent = {64, 64, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage probe = VK_NULL_HANDLE;
    if (vk.vkCreateImage(g.device, &image, nullptr, &probe) != VK_SUCCESS) {
        return;
    }
    VkMemoryRequirements requirements;
    vk.vkGetImageMemoryRequirements(g.device, probe, &requirements);
    vk.vkDestroyImage(g.device, probe, nullptr);
    const u32 type = FindMemoryType(requirements);
    if (type == ~0u || std::getenv("LD_NO_TEXTURE_POOL")) {
        return;
    }
    // O driver da AMD trava por dentro durante uma alocacao grande: um bloco de 128 MiB alocado no meio do jogo (em
    // qualquer thread) segurava ate a criacao de uma textura de 4 KB na thread de render por 13-15 ms. Por isso o pool
    // ja nasce com 1/8 da memoria de video (1 GiB numa placa de 8 GiB; Vorton usa ~350 MiB, 33 min de jogo ~1,1 GiB).
    VkPhysicalDeviceMemoryProperties properties;
    vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &properties);
    const u64 heap = properties.memoryHeaps[properties.memoryTypes[type].heapIndex].size;
    const u32 chunks = static_cast<u32>(std::clamp<u64>(heap / 8 / kTexturePoolChunk, 2, 8));
    const auto start = std::chrono::steady_clock::now();
    u32 made = 0;
    for (u32 i = 0; i < chunks; ++i) {
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = kTexturePoolChunk;
        allocate.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vk.vkAllocateMemory(g.device, &allocate, nullptr, &memory) != VK_SUCCESS) {
            break;
        }
        const TimedLock lock(g_texture_pool.mutex);
        TexturePool::Chunk chunk;
        chunk.type = type;
        chunk.memory = memory;
        chunk.free[0] = kTexturePoolChunk;
        g_texture_pool.chunks.push_back(std::move(chunk));
        ++made;
    }
    std::printf("[VULKAN] pool de texturas e alvos: %u blocos de %llu MiB reservados na abertura em %.0f ms\n", made,
                static_cast<unsigned long long>(kTexturePoolChunk >> 20),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

// Alvo de render do jogo: VkImage + VkImageView, criado na primeira vez que o jogo desenha nele e
// guardado por (endereco, tamanho, formato). Cor: anexo + amostragem; os sRGB sao mutaveis para
// terem tambem uma view UNORM (os bytes como estao), usada ao copiar para a janela.
State::RenderTarget* GetTarget(const GameDraw::Target& wanted, bool depth) {
    const VkFormat format = TargetVkFormat(depth ? GameDraw::kTargetDepth32F : wanted.format);
    if (!wanted.address || !wanted.width || !wanted.height || wanted.width > 8192 || wanted.height > 8192 || format == VK_FORMAT_UNDEFINED) {
        return nullptr;
    }
    const u64 key = wanted.address ^ (static_cast<u64>(wanted.width) << 44) ^ (static_cast<u64>(wanted.height) << 30) ^
                    (static_cast<u64>(format) << 52) ^ (depth ? 1ull << 63 : 0);
    if (const auto found = g.targets.find(key); found != g.targets.end()) {
        if (State::RenderTarget*& latest = g.target_by_address[wanted.address]; latest != &found->second) {
            latest = &found->second;
        }
        return &found->second;
    }
    if (g.targets.size() >= kMaxTargets) {
        return nullptr;
    }
    const HitchClock hitch(HitchClock::kTarget); // Fase 59
    State::RenderTarget target;
    target.format = format;
    target.width = wanted.width;
    target.height = wanted.height;
    target.depth = depth;
    target.address = wanted.address;
    const bool srgb = format == VK_FORMAT_R8G8B8A8_SRGB;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.flags = srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {wanted.width, wanted.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(g.device, &image, nullptr, &target.image) != VK_SUCCESS) {
        return nullptr;
    }
    if (!BindTargetMemory(target.image, &target.memory)) {
        vk.vkDestroyImage(g.device, target.image, nullptr);
        return nullptr;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    bool ok = vk.vkCreateImageView(g.device, &view, nullptr, &target.view) == VK_SUCCESS;
    if (ok && depth) {
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ok = vk.vkCreateImageView(g.device, &view, nullptr, &target.sample_view) == VK_SUCCESS;
    }
    if (ok && srgb) {
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        ok = vk.vkCreateImageView(g.device, &view, nullptr, &target.raw_view) == VK_SUCCESS;
    }
    if (ok && !depth) { // descriptor set do pipeline base, para o quad de apresentacao
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool = g.descriptor_pool;
        sets.descriptorSetCount = 1;
        sets.pSetLayouts = &g.set_layout;
        ok = vk.vkAllocateDescriptorSets(g.device, &sets, &target.present_set) == VK_SUCCESS;
        if (ok) {
            VkDescriptorImageInfo image_info{g.sampler, target.raw_view ? target.raw_view : target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = target.present_set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_info;
            vk.vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
        }
    }
    if (!ok) {
        if (target.raw_view) vk.vkDestroyImageView(g.device, target.raw_view, nullptr);
        if (target.view) vk.vkDestroyImageView(g.device, target.view, nullptr);
        vk.vkDestroyImage(g.device, target.image, nullptr);
        if (target.memory) vk.vkFreeMemory(g.device, target.memory, nullptr); // (nulo: veio do pool, fica para outro alvo)
        return nullptr;
    }
    State::RenderTarget* stored = &(g.targets[key] = target);
    g.target_by_address[wanted.address] = stored;
    return stored;
}

// View de array 2D (1 camada) de um alvo de cor, criada na primeira vez que um shader a pede.
VkImageView TargetArrayView(State::RenderTarget* target) {
    if (!target->array_view && !target->depth) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = target->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = target->format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk.vkCreateImageView(g.device, &view, nullptr, &target->array_view) != VK_SUCCESS) {
            target->array_view = VK_NULL_HANDLE;
        }
    }
    return target->array_view;
}

// Imagem de copia de um alvo (mesmo formato e tamanho), para os draws que amostram o proprio alvo.
bool EnsureTargetCopy(State::RenderTarget* target) {
    if (target->copy_view) {
        return true;
    }
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = target->format;
    image.extent = {target->width, target->height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(g.device, &image, nullptr, &target->copy_image) != VK_SUCCESS) {
        target->copy_image = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = target->format;
    view.subresourceRange = {static_cast<VkImageAspectFlags>(target->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    bool ok = BindTargetMemory(target->copy_image, &target->copy_memory);
    if (ok) {
        view.image = target->copy_image;
        ok = vk.vkCreateImageView(g.device, &view, nullptr, &target->copy_view) == VK_SUCCESS;
    }
    if (!ok) {
        if (target->copy_memory) vk.vkFreeMemory(g.device, target->copy_memory, nullptr);
        vk.vkDestroyImage(g.device, target->copy_image, nullptr);
        target->copy_image = VK_NULL_HANDLE;
        target->copy_memory = VK_NULL_HANDLE;
        target->copy_view = VK_NULL_HANDLE;
        return false;
    }
    target->copy_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

// Texturas brancas 1x1 para os tipos de view que o backend ainda nao envia (cubo, array 2D, 3D):
// o shader declara o tipo, e o descritor tem de ter exatamente esse tipo.
bool CreateFallbackViews() {
    for (u32 i = 0; i < 2; ++i) {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.flags = i == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        image.imageType = i == 0 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {1, 1, 1};
        image.mipLevels = 1;
        image.arrayLayers = i == 0 ? 6 : 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk.vkCreateImage(g.device, &image, nullptr, &g.fallback_images[i]) != VK_SUCCESS) {
            return Fail("vkCreateImage (textura branca de reserva)");
        }
        VkMemoryRequirements requirements;
        vk.vkGetImageMemoryRequirements(g.device, g.fallback_images[i], &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(requirements);
        if (allocate.memoryTypeIndex == ~0u || vk.vkAllocateMemory(g.device, &allocate, nullptr, &g.fallback_memory[i]) != VK_SUCCESS) {
            return Fail("vkAllocateMemory (textura branca de reserva)");
        }
        vk.vkBindImageMemory(g.device, g.fallback_images[i], g.fallback_memory[i], 0);
    }
    vk.vkResetCommandBuffer(g.upload_command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(g.upload_command, &begin);
    const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    for (u32 i = 0; i < 2; ++i) {
        const u32 layers = i == 0 ? 6 : 1;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        Transition(g.upload_command, g.fallback_images[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers);
        vk.vkCmdClearColorImage(g.upload_command, g.fallback_images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        Transition(g.upload_command, g.fallback_images[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layers);
    }
    vk.vkEndCommandBuffer(g.upload_command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.upload_command;
    QueueSubmit(1, &submit, VK_NULL_HANDLE);
    QueueWaitIdle();
    const struct {
        u32 image;
        VkImageViewType type;
        u32 layers;
    } kViews[4] = {{0, VK_IMAGE_VIEW_TYPE_2D, 1}, {0, VK_IMAGE_VIEW_TYPE_CUBE, 6}, {0, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1}, {1, VK_IMAGE_VIEW_TYPE_3D, 1}};
    for (u32 i = 0; i < 4; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = g.fallback_images[kViews[i].image];
        view.viewType = kViews[i].type;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kViews[i].layers};
        if (vk.vkCreateImageView(g.device, &view, nullptr, &g.fallback_views[i]) != VK_SUCCESS) {
            return Fail("vkCreateImageView (textura branca de reserva)");
        }
    }
    return true;
}

VkBlendFactor BlendFactor(u32 factor) {
    switch (factor) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1) < 12) {
            std::printf("[BLEND] fator de blend nao tratado: %u (15..18 = segunda fonte); usado ONE\n", factor);
        }
        return VK_BLEND_FACTOR_ONE; // fatores de segunda fonte (15..18): nao tratados
    }
    }
}

VkBlendOp BlendOp(u32 func) {
    static const VkBlendOp kOps[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT};
    return func < 5 ? kOps[func] : VK_BLEND_OP_ADD;
}

// Pipeline com os modulos do jogo. A chave leva tudo o que o Vulkan fixa no pipeline: par, layout
// de vertice, primitiva, formatos dos alvos e o estado lido do GNM - CB_BLENDn_CONTROL,
// CB_TARGET_MASK, DB_DEPTH_CONTROL, cull. Quando o jogo muda um deles, outro pipeline e criado
// (e fica em cache). Viewport, scissor e constantes de blend sao dinamicos.
// Fase 29: so monta e cria o pipeline; nao toca no estado compartilhado (pode rodar na thread de pipelines).
FixedState MakeFixedState(const GameDraw& draw, bool has_depth) {
    FixedState f;
    f.clamp = draw.depth_clamp ? VK_TRUE : VK_FALSE;
    f.cull = ((draw.cull & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((draw.cull & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    f.front = (draw.cull & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const u32 depth_state = has_depth ? (draw.depth_control & 0x76) : 0; // z_enable, z_write, funcao
    f.depth_test = (depth_state >> 1) & 1;
    f.depth_write = ((depth_state >> 1) & 1) && ((depth_state >> 2) & 1);
    f.depth_compare = static_cast<VkCompareOp>((depth_state >> 4) & 7); // mesma numeracao do GNM
    if (has_depth && (draw.depth_control & 1)) {
        // Fase 34: teste de estencil. Operacoes do GNM: 0 mantem, 1 zera, 2 uns, 3 troca pelo valor de
        // teste, 4 troca pelo valor de operacao, 5 / 6 soma / subtrai com trava, 7 inverte, 8 / 9 com
        // volta; as bit a bit (10..15) nao existem no Vulkan e ficam como "mantem".
        auto op = [](u32 value) {
            static const VkStencilOp kOps[16] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                                                 VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                                 VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP,
                                                 VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                                 VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP};
            return kOps[value & 15];
        };
        const u32 sc = draw.stencil_control;
        f.stencil_test = VK_TRUE;
        f.front_ops.failOp = op(sc);
        f.front_ops.passOp = op(sc >> 4);
        f.front_ops.depthFailOp = op(sc >> 8);
        f.front_ops.compareOp = static_cast<VkCompareOp>((draw.depth_control >> 8) & 7);
        if ((draw.depth_control >> 7) & 1) { // backface_enable: as faces de tras tem estado proprio
            f.back_ops.failOp = op(sc >> 12);
            f.back_ops.passOp = op(sc >> 16);
            f.back_ops.depthFailOp = op(sc >> 20);
            f.back_ops.compareOp = static_cast<VkCompareOp>((draw.depth_control >> 20) & 7);
        } else {
            f.back_ops = f.front_ops;
        }
    }
    return f;
}

// ---- Fase 41: bibliotecas de pipeline (VK_EXT_graphics_pipeline_library) ----
// Pre-rasterizacao (VS) e fragment shader (PS): uma de cada por par, compiladas quando o par carrega.
// Entrada de vertice e saida de cor (blend + formatos dos alvos): sem shader, baratas, em cache pela
// chave do estado. No draw, o pipeline e so a ligacao rapida das quatro (sem otimizacao no link).
std::mutex g_lib_mutex;
std::unordered_map<u64, VkPipeline> g_vertex_libs, g_output_libs; // protegidos por g_lib_mutex
VulkanRenderer::GplStats g_gpl_stats;                              // protegido por g_lib_mutex

u64 HashBytes(u64 hash, const void* data, size_t bytes) {
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    }
    return hash;
}

const VkSpecializationInfo* ColdSpecialization() {
    // Fase 38 (so para medir): LD_COLD_PIPELINES=<n> poe uma constante de especializacao que o shader
    // nao usa. Nao muda nada no resultado, mas muda a chave do cache de shaders do driver: todo
    // pipeline sai compilado do zero, como num shader nunca visto.
    static const u32 cold_seed = std::getenv("LD_COLD_PIPELINES") ? static_cast<u32>(std::strtoul(std::getenv("LD_COLD_PIPELINES"), nullptr, 10)) : 0;
    static const VkSpecializationMapEntry cold_entry{0xFFFF, 0, 4};
    static const VkSpecializationInfo cold_spec{1, &cold_entry, 4, &cold_seed};
    return cold_seed ? &cold_spec : nullptr;
}

// Biblioteca de um estagio de shader do par (thread de pares / aquecimento; sem g.mutex).
VkPipeline CreateShaderLibrary(VkShaderModule module, bool vertex, VkPipelineLayout layout) {
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    stage.module = module;
    stage.pName = "main";
    stage.pSpecializationInfo = ColdSpecialization();
    VkGraphicsPipelineLibraryCreateInfoEXT part{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    part.flags = vertex ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &part};
    info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    info.stageCount = 1;
    info.pStages = &stage;
    info.layout = layout;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    static const VkDynamicState kVertexDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                                                    VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT};
    static const VkDynamicState kFragmentDynamic[] = {VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                                      VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                                      VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                                      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    if (vertex) {
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        dynamic.dynamicStateCount = 5;
        dynamic.pDynamicStates = kVertexDynamic;
    } else {
        info.pDepthStencilState = &depth_stencil;
        info.pMultisampleState = &multisample;
        dynamic.dynamicStateCount = 8;
        dynamic.pDynamicStates = kFragmentDynamic;
    }
    info.pDynamicState = &dynamic;
    VkPipeline library = VK_NULL_HANDLE;
    if (CreateGraphicsPipeline(&info, &library) != VK_SUCCESS) {
        library = VK_NULL_HANDLE;
    }
    return library;
}

// Bibliotecas sem shader (entrada de vertice / saida de cor), em cache. Com g_lib_mutex.
VkPipeline VertexInputLibrary(const VkPipelineVertexInputStateCreateInfo& vertex_input, const VkPipelineInputAssemblyStateCreateInfo& assembly) {
    u64 key = 0xcbf29ce484222325ull;
    key = HashBytes(key, &assembly.topology, sizeof(assembly.topology));
    key = HashBytes(key, vertex_input.pVertexBindingDescriptions, vertex_input.vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription));
    key = HashBytes(key, vertex_input.pVertexAttributeDescriptions, vertex_input.vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
    key ^= vertex_input.vertexBindingDescriptionCount * 0x9E37u + vertex_input.vertexAttributeDescriptionCount;
    if (const auto found = g_vertex_libs.find(key); found != g_vertex_libs.end()) {
        return found->second;
    }
    VkGraphicsPipelineLibraryCreateInfoEXT part{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    part.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &part};
    info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    VkPipeline library = VK_NULL_HANDLE;
    if (CreateGraphicsPipeline(&info, &library) != VK_SUCCESS) {
        library = VK_NULL_HANDLE;
    }
    g_vertex_libs[key] = library;
    g_gpl_stats.vertex_libs += library != VK_NULL_HANDLE;
    return library;
}

VkPipeline OutputLibrary(const VkPipelineColorBlendStateCreateInfo& blend, const VkPipelineMultisampleStateCreateInfo& multisample,
                         const VkPipelineRenderingCreateInfo& rendering) {
    u64 key = 0xcbf29ce484222325ull;
    key = HashBytes(key, blend.pAttachments, blend.attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
    key = HashBytes(key, rendering.pColorAttachmentFormats, rendering.colorAttachmentCount * sizeof(VkFormat));
    key = HashBytes(key, &rendering.depthAttachmentFormat, sizeof(VkFormat));
    key ^= blend.attachmentCount * 0x9E3779B9u;
    if (const auto found = g_output_libs.find(key); found != g_output_libs.end()) {
        return found->second;
    }
    VkGraphicsPipelineLibraryCreateInfoEXT part{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    part.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    VkPipelineRenderingCreateInfo rendering_copy = rendering;
    rendering_copy.pNext = &part;
    static const VkDynamicState kOutputDynamic[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 1;
    dynamic.pDynamicStates = kOutputDynamic;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering_copy};
    info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    info.pColorBlendState = &blend;
    info.pMultisampleState = &multisample;
    info.pDynamicState = &dynamic;
    VkPipeline library = VK_NULL_HANDLE;
    if (CreateGraphicsPipeline(&info, &library) != VK_SUCCESS) {
        library = VK_NULL_HANDLE;
    }
    g_output_libs[key] = library;
    g_gpl_stats.output_libs += library != VK_NULL_HANDLE;
    return library;
}

thread_local VkPipelineCreateFlags g_pipeline_flags = 0; // Fase 53: medicao (DISABLE_OPTIMIZATION)

VkPipeline BuildGamePipeline(const GameDraw& draw, VkShaderModule vs, VkShaderModule ps, VkPipelineLayout layout,
                             const VkFormat* target_formats, VkFormat depth_format, bool has_depth,
                             VkPipeline vs_lib = VK_NULL_HANDLE, VkPipeline fs_lib = VK_NULL_HANDLE) {
    static const VkFormat kFormats[GameDraw::kFormatCount] = {
        VK_FORMAT_R32_SFLOAT,     VK_FORMAT_R32G32_SFLOAT,       VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT,  VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,   VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R16G16_UNORM,   VK_FORMAT_R16G16B16A16_UNORM,  VK_FORMAT_R16G16_SNORM,     VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R8G8B8A8_UINT,  VK_FORMAT_R8G8B8A8_SINT,       VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32_UINT,       VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16_UINT,
        VK_FORMAT_R16G16_SINT,    VK_FORMAT_B8G8R8A8_UNORM};
    static const VkPrimitiveTopology kTopologies[] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                                                      VK_PRIMITIVE_TOPOLOGY_LINE_LIST,     VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                                      VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";
    stages[0].pSpecializationInfo = stages[1].pSpecializationInfo = ColdSpecialization();
    VkVertexInputBindingDescription bindings[8]{};
    for (u32 i = 0; i < draw.binding_count; ++i) {
        bindings[i] = {i, draw.bindings[i].stride, draw.bindings[i].per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkVertexInputAttributeDescription attributes[16]{};
    for (u32 i = 0; i < draw.attribute_count; ++i) {
        attributes[i] = {draw.attributes[i].location, draw.attributes[i].binding, kFormats[draw.attributes[i].format],
                         draw.attributes[i].offset};
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = draw.binding_count;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = draw.attribute_count;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = kTopologies[draw.primitive];
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    const FixedState fixed = MakeFixedState(draw, has_depth);
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = fixed.clamp;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = fixed.cull;
    raster.frontFace = fixed.front;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = fixed.depth_test;
    depth_stencil.depthWriteEnable = fixed.depth_write;
    depth_stencil.depthCompareOp = fixed.depth_compare;
    depth_stencil.stencilTestEnable = fixed.stencil_test;
    depth_stencil.front = fixed.front_ops;
    depth_stencil.back = fixed.back_ops;
    VkPipelineColorBlendAttachmentState attachments[8]{};
    VkFormat color_formats[8]{};
    for (u32 i = 0; i < draw.color_count; ++i) {
        const u32 control = draw.blend[g.independent_blend ? i : 0];
        VkPipelineColorBlendAttachmentState& a = attachments[i];
        a.blendEnable = (control >> 30) & 1;
        a.srcColorBlendFactor = BlendFactor(control & 0x1F);
        a.colorBlendOp = BlendOp((control >> 5) & 7);
        a.dstColorBlendFactor = BlendFactor((control >> 8) & 0x1F);
        const bool separate = (control >> 29) & 1;
        a.srcAlphaBlendFactor = separate ? BlendFactor((control >> 16) & 0x1F) : a.srcColorBlendFactor;
        a.alphaBlendOp = separate ? BlendOp((control >> 21) & 7) : a.colorBlendOp;
        a.dstAlphaBlendFactor = separate ? BlendFactor((control >> 24) & 0x1F) : a.dstColorBlendFactor;
        if (a.colorBlendOp == VK_BLEND_OP_MIN || a.colorBlendOp == VK_BLEND_OP_MAX) {
            a.srcColorBlendFactor = a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        if (a.alphaBlendOp == VK_BLEND_OP_MIN || a.alphaBlendOp == VK_BLEND_OP_MAX) {
            a.srcAlphaBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        a.colorWriteMask = (draw.color_mask >> ((g.independent_blend ? i : 0) * 4)) & 0xF; // R, G, B, A: mesmos bits do Vulkan
        color_formats[i] = target_formats[i];
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = draw.color_count;
    blend.pAttachments = attachments;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                             VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 6;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = draw.color_count;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = has_depth ? depth_format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.flags = g_pipeline_flags;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const HitchClock hitch(HitchClock::kPipeline);
    const auto start = std::chrono::steady_clock::now();
    if (vs_lib && fs_lib) {
        // Fase 41: ligacao rapida das bibliotecas (o driver ja compilou os shaders quando o par carregou)
        const TimedLock lock(g_lib_mutex);
        const VkPipeline libraries[4] = {VertexInputLibrary(vertex_input, assembly), vs_lib, fs_lib, OutputLibrary(blend, multisample, rendering)};
        if (libraries[0] && libraries[3]) {
            VkPipelineLibraryCreateInfoKHR link{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
            link.libraryCount = 4;
            link.pLibraries = libraries;
            VkGraphicsPipelineCreateInfo linked{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &link};
            linked.layout = layout;
            if (CreateGraphicsPipeline(&linked, &pipeline) != VK_SUCCESS) {
                pipeline = VK_NULL_HANDLE;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (pipeline) {
            ++g_gpl_stats.links;
            g_gpl_stats.link_ms += ms;
            g_gpl_stats.link_worst_ms = std::max(g_gpl_stats.link_worst_ms, ms);
            if (g_gpl_stats.links <= 12) {
                std::printf("[GPL] pipeline ligado de bibliotecas em %.3f ms\n", ms);
            }
            return pipeline;
        }
        ++g_gpl_stats.link_failures; // cai no pipeline monolitico abaixo
    }
    const VkResult result = CreateGraphicsPipeline(&info, &pipeline);
    if (result != VK_SUCCESS) {
        Fail("vkCreateGraphicsPipelines (shaders do jogo)", result);
        pipeline = VK_NULL_HANDLE;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const TimedLock lock(g_lib_mutex);
    ++g_gpl_stats.monolithic;
    g_gpl_stats.monolithic_ms += ms;
    g_gpl_stats.monolithic_worst_ms = std::max(g_gpl_stats.monolithic_worst_ms, ms);
    return pipeline;
}

// ---- Fase 29: pipelines criados fora da thread de render ----
struct PipelineJob {
    u64 hash = 0;
    GameDraw draw; // so o estado e lido (os ponteiros de dados do draw nao sao usados)
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    VkPipeline vs_lib = VK_NULL_HANDLE, fs_lib = VK_NULL_HANDLE; // Fase 41
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkFormat formats[8]{};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool has_depth = false;
    std::vector<u32> key; // estado completo (o mesmo vetor da chave): gravado no arquivo de aquecimento
    bool prewarm = false; // veio do arquivo: nao regrava, e conta no progresso
    bool fast = false;    // Fase 56: pedido por um frame: sai sem otimizacao (~1,5x mais rapido) ...
    bool upgrade = false; // ... e depois este job, em segundo plano, troca pela versao otimizada
    u32 level = 0;         // Fase 72: area em que o pipeline foi usado pela primeira vez (0 = desconhecida)
    bool priority = false; // Fase 72: aquecimento da area atual / da ultima sessao: prioridade normal e na frente
};
// Fase 56: pipelines trocados pela versao otimizada. So sao destruidos quando nenhum frame em voo pode
// mais usar (3 frames depois). Protegido por g.mutex.
struct RetiredPipeline {
    VkPipeline pipeline;
    u32 serial;
};
std::vector<RetiredPipeline> g_retired_pipelines;
bool g_fast_first = true; // --no-fast-pipelines
u64 g_fast_built = 0, g_upgraded = 0;
std::string g_pipeline_state_path;
std::mutex g_pipeline_file_mutex;
std::unordered_set<u64> g_pipeline_saved; // hashes ja presentes no arquivo (protegido por g_pipeline_file_mutex)
std::atomic<u32> g_prewarm_total{0}, g_prewarm_done{0};

// ---- Fase 72: aquecimento por area ----
// O radar avisa quando o jogo carrega uma area (\levels\...\<nome>\<nome>.txt). Por 2 min depois disso, cada
// pipeline usado pela primeira vez ganha essa area (pipeline_levels.txt: "<hash> <area>", e "ultima <area>" a cada
// troca; a base distribuida e pipeline_levels_base.txt). No aquecimento, os pipelines da ultima area da sessao
// anterior (e da tela de abertura) vao na frente, na prioridade normal; o resto abaixo do normal. Quando o jogo
// carrega uma area, o que dela ainda estiver na fila passa para a frente. LD_PREWARM_GATE=area faz a tela de
// carregamento do port esperar so pelos prioritarios (o resto termina com o jogo ja rodando; um pipeline que
// faltar no uso ainda espera ate 400 ms, sem sumir objeto). Padrao: espera tudo, como antes.
std::mutex g_level_mutex;
std::vector<std::string> g_level_names{""}; // id -> nome (0 = sem area)
std::unordered_map<std::string, u32> g_level_ids;
std::atomic<u32> g_current_level{0};
std::unordered_map<u64, u32> g_pipeline_level;      // hash da chave -> area (g_level_mutex)
std::vector<std::pair<u64, u32>> g_level_pending;   // a gravar (hash 0 = linha "ultima"); g_level_mutex
std::unordered_set<u64> g_level_noted;              // ja visto nesta sessao (so a thread de render)
std::atomic<u32> g_level_note_until{0};             // frame_serial ate onde o primeiro uso e anotado
std::unordered_set<u32> g_priority_levels;          // g_level_mutex
std::atomic<u32> g_prewarm_priority_total{0}, g_prewarm_priority_done{0};
std::atomic<bool> g_priority_from_session{false}; // ha areas de uma sessao anterior (a trava por area vale)
std::atomic<bool> g_level_writing{false};
std::chrono::steady_clock::time_point g_level_written_at{};
bool g_levels_loaded = false; // g_level_mutex

// Com g_level_mutex.
u32 LevelIdLocked(const std::string& name) {
    if (name.empty()) {
        return 0;
    }
    const auto [it, added] = g_level_ids.emplace(name, static_cast<u32>(g_level_names.size()));
    if (added) {
        g_level_names.push_back(name);
    }
    return it->second;
}

std::filesystem::path LevelsPath(const char* file) {
    return std::filesystem::path(GameSpirvDirectory()) / file;
}

// Uma vez, antes do primeiro aquecimento: base distribuida e depois o arquivo do jogador (vale a primeira area).
void LoadPipelineLevels() {
    std::lock_guard lock(g_level_mutex);
    if (g_levels_loaded || GameSpirvDirectory().empty()) {
        return;
    }
    g_levels_loaded = true;
    std::string last;                // ultima area da sessao anterior
    std::vector<std::string> entry;  // as 3 primeiras areas depois da abertura na sessao anterior (por onde o save entra)
    bool in_session = false;
    u32 entries = 0;
    for (const char* file : {"pipeline_levels_base.txt", "pipeline_levels.txt"}) {
        std::ifstream in(LevelsPath(file));
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line == "sessao") { // uma sessao nova comecou: a entrada e a primeira area dela
                in_session = true;
                entry.clear();
                continue;
            }
            const size_t space = line.find(' ');
            if (line.empty() || line[0] == '#' || space == std::string::npos) {
                continue;
            }
            const std::string name = line.substr(space + 1);
            if (line.compare(0, space, "ultima") == 0) {
                last = name;
                if (in_session && entry.size() < 3 && name != "loading_screen_startup" &&
                    std::find(entry.begin(), entry.end(), name) == entry.end()) {
                    entry.push_back(name);
                }
                continue;
            }
            const u64 hash = std::strtoull(line.c_str(), nullptr, 16);
            if (hash && g_pipeline_level.emplace(hash, LevelIdLocked(name)).second) {
                ++entries;
            }
        }
    }
    g_priority_levels.insert(LevelIdLocked("loading_screen_startup"));
    std::string named = "tela de abertura";
    g_priority_from_session = !last.empty();
    entry.push_back(last);
    for (const std::string& name : entry) {
        if (!name.empty() && g_priority_levels.insert(LevelIdLocked(name)).second) {
            named += " + " + name;
        }
    }
    std::printf("[PIPELINES] areas: %u pipelines com area conhecida, %zu areas; prioridade no aquecimento: %s\n", entries, g_level_names.size() - 1,
                named.c_str());
}

// Thread de render (GetGamePipeline, com g.mutex), so na janela de 2 min depois de uma troca de area.
void NotePipelineLevel(u64 hash) {
    if (!g_level_noted.insert(hash).second) {
        return;
    }
    std::lock_guard lock(g_level_mutex);
    const u32 level = g_current_level.load();
    if (level && g_pipeline_level.emplace(hash, level).second) {
        g_level_pending.emplace_back(hash, level);
    }
}

// Fim do DrawFrame: grava as anotacoes novas em segundo plano, no maximo a cada 2 s.
void FlushLevelNotes() {
    if (g_level_writing.load() || std::chrono::steady_clock::now() - g_level_written_at < std::chrono::seconds(2)) {
        return;
    }
    std::vector<std::string> lines;
    {
        std::lock_guard lock(g_level_mutex);
        if (g_level_pending.empty()) {
            return;
        }
        for (const auto& [hash, level] : g_level_pending) {
            char text[32];
            if (hash == 1 && !level) {
                lines.push_back("sessao\n");
                continue;
            }
            std::snprintf(text, sizeof(text), hash ? "%llx " : "ultima ", static_cast<unsigned long long>(hash));
            lines.push_back(text + g_level_names[level] + "\n");
        }
        g_level_pending.clear();
    }
    g_level_written_at = std::chrono::steady_clock::now();
    g_level_writing = true;
    std::thread([lines = std::move(lines)] {
        if (std::FILE* f = _wfopen(LevelsPath("pipeline_levels.txt").c_str(), L"a")) {
            for (const std::string& line : lines) {
                std::fputs(line.c_str(), f);
            }
            std::fclose(f);
        }
        g_level_writing = false;
    }).detach();
}

u64 HashPipelineKey(const std::vector<u32>& key) {
    u64 key_hash = 0xcbf29ce484222325ull ^ key.size();
    for (const u32 word : key) {
        key_hash = (key_hash ^ word) * 0x100000001b3ull;
        key_hash ^= key_hash >> 29;
    }
    return key_hash;
}
std::mutex g_pipeline_mutex;
std::condition_variable g_pipeline_cv;
std::deque<PipelineJob> g_pipeline_jobs;
std::unordered_set<u64> g_pipelines_pending; // protegido por g.mutex
std::unordered_map<u64, u32> g_pipeline_failed_at; // Fase 55: hash -> frame da falha (g.mutex)
u64 g_pipeline_failures = 0, g_pipeline_retries = 0;  // Fase 55 (g.mutex)
std::thread g_pipeline_thread;
std::atomic<bool> g_pipeline_stop{false};
std::atomic<bool> g_async_pipelines{true};
std::atomic<u64> g_pipelines_async{0};
// Fase 38: criacao sincrona com orcamento por frame (so a thread de render mexe, com g.mutex)
bool g_sync_pipelines = true;
double g_sync_budget_ms = 50.0;
double g_sync_frame_ms = 0;
bool g_sync_frame_any = false;
VulkanRenderer::SyncPipelineStats g_sync_stats;

void SavePipelineState(u64 hash, const std::vector<u32>& key) {
    if (g_pipeline_state_path.empty() || key.empty()) {
        return;
    }
    std::lock_guard file_lock(g_pipeline_file_mutex);
    if (g_pipeline_saved.insert(hash).second) {
        if (std::FILE* f = std::fopen(g_pipeline_state_path.c_str(), "a")) {
            std::fseek(f, 0, SEEK_END);
            if (std::ftell(f) == 0) {
                std::fprintf(f, "# ld-pipeline-states 2\n"); // Fase 71: arquivo novo comeca com a versao do formato
            }
            for (const u32 word : key) {
                std::fprintf(f, "%x ", word);
            }
            std::fprintf(f, "\n");
            std::fclose(f);
        }
    }
}

// Fase 45: varias threads de pipeline (6). O driver compila em paralelo; um frame que pede 5 pipelines
// novos espera o mais lento deles, e nao a soma (na sessao do usuario chegavam a 134 ms em fila).
int PipelineThreadCount() {
    static const int count = [] {
        const char* env = std::getenv("LD_PIPE_THREADS"); // so para medir
        const int n = env ? std::atoi(env) : 6; // 6 > 4 no teste a frio: 1345 x 4144 draws adiados
        return std::clamp(n, 1, 8);
    }();
    return count;
}
std::vector<std::thread> g_pipeline_threads;
std::mutex g_pipeline_done_mutex;
std::condition_variable g_pipeline_done_cv;
std::atomic<u64> g_pipelines_delivered{0};

void PipelineWorker();
bool ResolvePairModules(u64 key, VkShaderModule* vs_out, VkShaderModule* ps_out); // Fase 59 (definidas mais abaixo)
bool ResolvePairModulesLocked(u64 key, State::GamePair& pair);

// Com g_pipeline_mutex.
void EnsurePipelineWorkers() {
    if (g_pipeline_threads.empty()) {
        for (int i = 0; i < PipelineThreadCount(); ++i) {
            g_pipeline_threads.emplace_back(PipelineWorker);
        }
    }
}

void PipelineWorker() {
    for (;;) {
        PipelineJob job;
        {
            std::unique_lock lock(g_pipeline_mutex);
            g_pipeline_cv.wait(lock, [] { return g_pipeline_stop.load() || !g_pipeline_jobs.empty(); });
            if (g_pipeline_stop.load()) {
                return;
            }
            job = g_pipeline_jobs.front();
            g_pipeline_jobs.pop_front();
        }
        // Fase 59: so o pipeline que um frame espera roda na prioridade normal; versao otimizada (troca),
        // aquecimento e pedidos sem pressa ficam abaixo do normal e nao disputam a CPU com o jogo.
        const bool urgent = job.fast || (!job.upgrade && !job.prewarm) || job.priority; // Fase 72: area atual
        SetThreadPriority(GetCurrentThread(), urgent ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
        g_pipeline_flags = job.fast ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0; // Fase 56
        if (!job.vs || !job.ps) {
            ResolvePairModules(job.draw.pair, &job.vs, &job.ps); // Fase 59: par do pacote registrado sem modulos
        }
        const VkPipeline pipeline = job.vs && job.ps ? BuildGamePipeline(job.draw, job.vs, job.ps, job.layout, job.formats, job.depth_format,
                                                                         job.has_depth, job.vs_lib, job.fs_lib)
                                                     : VK_NULL_HANDLE;
        g_pipeline_flags = 0;
        if (job.upgrade) { // Fase 56: versao otimizada pronta: troca (a rapida sai de uso aos poucos)
            if (pipeline) {
                const TimedLock lock(g.mutex);
                VkPipeline& slot = g.game_pipelines[job.hash];
                if (slot) {
                    g_retired_pipelines.push_back({slot, g.frame_serial});
                }
                slot = pipeline;
                ++g_upgraded;
            }
            continue;
        }
        if (job.fast && pipeline) {
            PipelineJob upgrade = job;
            upgrade.fast = false;
            upgrade.upgrade = true;
            std::lock_guard lock(g_pipeline_mutex);
            g_pipeline_jobs.push_back(std::move(upgrade)); // depois de tudo o que um frame espera
        }
        if (job.prewarm) {
            ++g_prewarm_done;
            g_prewarm_priority_done += job.priority; // Fase 72
        } else if (pipeline) {
            SavePipelineState(job.hash, job.key);
        }
        {
            const TimedLock lock(g.mutex);
            g.game_pipelines[job.hash] = pipeline; // falha tambem fica guardada: nao tenta de novo a cada draw
            if (!pipeline) {
                // Fase 55: antes a falha ficava para sempre (o material sumia ate fechar o jogo; cabecas
                // "transparentes" quando so a pele falhava). Agora vai para o log e e tentada de novo.
                g_pipeline_failed_at[job.hash] = g.frame_serial;
                if (++g_pipeline_failures <= 30) {
                    std::printf("[PIPELINE] criacao FALHOU: par %016llx pipeline %016llx (%s) - nova tentativa em ~300 frames%s",
                                static_cast<unsigned long long>(job.draw.pair), static_cast<unsigned long long>(job.hash), g.error.c_str(),
                                "\n");
                }
            }
            g_pipelines_pending.erase(job.hash);
            ++g_pipelines_async;
            g_fast_built += job.fast && pipeline;
        }
        {
            std::lock_guard done_lock(g_pipeline_done_mutex);
            ++g_pipelines_delivered;
        }
        g_pipeline_done_cv.notify_all();
    }
}

void StopPipelineWorker() {
    g_pipeline_stop = true;
    g_pipeline_cv.notify_all();
    for (auto& thread : g_pipeline_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    g_pipeline_threads.clear();
}

// Pipeline com os modulos do jogo, pelo estado do draw. Chamado com g.mutex. Se ainda nao existe,
// o pedido vai para a thread de pipelines e o draw fica de fora ate ele chegar (VK_NULL_HANDLE).
VkPipeline GetGamePipeline(const GameDraw& draw, const State::GamePair& pair, State::RenderTarget* const* colors,
                           const State::RenderTarget* depth, u64* key_out = nullptr, bool* pending_out = nullptr) {
    const u32 depth_state = depth ? (draw.depth_control & 0x76) : 0; // z_enable, z_write, funcao
    // Chamado uma vez por draw (milhares por frame): a chave e montada num vetor reaproveitado e
    // reduzida a um hash de 64 bits, sem alocacao nem comparacao de vetores.
    static thread_local std::vector<u32> key;
    key.assign({static_cast<u32>(draw.pair), static_cast<u32>(draw.pair >> 32), static_cast<u32>(draw.primitive), draw.color_mask,
                depth_state, draw.cull & 7, draw.depth_clamp, depth != nullptr, draw.color_count, draw.binding_count,
                draw.attribute_count, depth != nullptr ? draw.depth_control & 0x00700781u : 0u,
                depth != nullptr && (draw.depth_control & 1) ? draw.stencil_control : 0u});
    for (u32 i = 0; i < draw.color_count; ++i) {
        key.push_back(static_cast<u32>(colors[i]->format));
        key.push_back((draw.blend[i] >> 30) & 1 ? draw.blend[i] & 0x7FFF1FFF : 0);
    }
    for (u32 i = 0; i < draw.binding_count; ++i) {
        key.push_back(draw.bindings[i].stride | (draw.bindings[i].per_instance ? 0x80000000u : 0));
    }
    for (u32 i = 0; i < draw.attribute_count; ++i) {
        key.insert(key.end(), {draw.attributes[i].location, static_cast<u32>(draw.attributes[i].format), draw.attributes[i].offset,
                               draw.attributes[i].binding});
    }
    const u64 key_hash = HashPipelineKey(key);
    if (key_out) {
        *key_out = key_hash;
    }
    if (g.frame_serial < g_level_note_until.load(std::memory_order_relaxed)) {
        NotePipelineLevel(key_hash); // Fase 72
    }
    if (const auto found = g.game_pipelines.find(key_hash); found != g.game_pipelines.end()) {
        if (found->second || !g_pipeline_failed_at.count(key_hash) || g.frame_serial - g_pipeline_failed_at[key_hash] < 300) {
            return found->second;
        }
        g.game_pipelines.erase(found); // Fase 55: falhou ha mais de ~5 s: tenta de novo
        g_pipeline_failed_at.erase(key_hash);
        ++g_pipeline_retries;
    }
    VkFormat formats[8]{};
    for (u32 i = 0; i < draw.color_count; ++i) {
        formats[i] = colors[i]->format;
    }
    if (!g_async_pipelines.load(std::memory_order_relaxed)) {
        ResolvePairModulesLocked(draw.pair, const_cast<State::GamePair&>(pair)); // Fase 59
        const VkPipeline pipeline = pair.vs && pair.ps ? BuildGamePipeline(draw, pair.vs, pair.ps, pair.layout, formats,
                                                                           depth ? depth->format : VK_FORMAT_UNDEFINED, depth != nullptr,
                                                                           pair.vs_lib, pair.fs_lib)
                                                       : VK_NULL_HANDLE;
        g.game_pipelines[key_hash] = pipeline;
        return pipeline;
    }
    // Fase 45: o pedido vai para as threads de pipeline (na frente da fila) e o draw fica no frame
    // como "pendente"; o DrawFrame espera os pendentes ate o teto do frame antes de gravar. A
    // compilacao corre em paralelo e enquanto o jogo ainda monta o resto do frame.
    const bool for_frame = g_sync_pipelines;
    if (for_frame && pending_out) {
        *pending_out = true;
    }
    if (g_pipelines_pending.insert(key_hash).second) {
        PipelineJob job;
        job.hash = key_hash;
        job.draw = draw;
        job.vs = pair.vs;
        job.ps = pair.ps;
        job.vs_lib = pair.vs_lib;
        job.fs_lib = pair.fs_lib;
        job.layout = pair.layout;
        std::memcpy(job.formats, formats, sizeof(formats));
        job.depth_format = depth ? depth->format : VK_FORMAT_UNDEFINED;
        job.has_depth = depth != nullptr;
        job.key = key;
        job.fast = for_frame && g_fast_first; // Fase 56
        {
            std::lock_guard lock(g_pipeline_mutex);
            EnsurePipelineWorkers();
            if (for_frame) {
                g_pipeline_jobs.push_front(job); // o frame atual espera por ele
                ++g_sync_stats.created;
            } else {
                g_pipeline_jobs.push_back(job);
            }
        }
        g_pipeline_cv.notify_one();
    }
    return VK_NULL_HANDLE;
}

// Fase 45: antes de gravar o frame (sem g.mutex), espera os pipelines pendentes dos draws do frame,
// ate o teto. O que nao ficar pronto fica de fora deste frame e entra no proximo.
double g_frame_wait_spent_ms = 0; // Fase 45: espera por texturas deste frame (thread de render)
u64 g_draws_late_texture = 0;

// Fase 55: frame com muitos pipelines novos ao mesmo tempo (comeco de cutscene ou de area: 90 de uma
// vez na cutscene do Mestre Chen). Com o teto normal de 50 ms o frame saia sem esses objetos por quase
// 1 s: a pele das cabecas, que e outro material, sumia e ficava so o rosto. Nesses frames a espera vai
// ate g_burst_wait_ms: um engasgo curto no corte, no lugar de objetos faltando.
// Fase 56: limiar 1 e teto 400 ms: o frame nao sai sem objetos (a 1a vez vira um engasgo no corte, sem
// pop-in). Os pipelines urgentes saem sem otimizacao (Fase 56), o que encurta esse engasgo.
double g_burst_wait_ms = 400.0;
u32 g_burst_threshold = 1;

void WaitFramePipelines() {
    const auto start = std::chrono::steady_clock::now();
    double budget = std::max(g_sync_budget_ms - g_frame_wait_spent_ms, 8.0); // teto do frame inteiro
    g_frame_wait_spent_ms = 0;
    if (g_burst_wait_ms > budget) {
        std::unordered_set<u64> pending;
        {
            const TimedLock lock(g.mutex);
            for (const auto& draw : g.queue_game) {
                if (!draw.pipeline && draw.pipeline_key && !g.game_pipelines.count(draw.pipeline_key)) {
                    pending.insert(draw.pipeline_key);
                }
            }
        }
        if (pending.size() >= g_burst_threshold) {
            budget = g_burst_wait_ms;
            const TimedLock lock(g.mutex);
            ++g_sync_stats.burst_frames;
        }
    }
    const auto deadline = start + std::chrono::microseconds(static_cast<long long>(budget * 1000.0));
    bool waited = false;
    for (;;) {
        u32 missing = 0;
        u64 seen = 0;
        {
            const TimedLock lock(g.mutex);
            for (const auto& draw : g.queue_game) {
                if (!draw.pipeline && draw.pipeline_key && !g.game_pipelines.count(draw.pipeline_key)) {
                    ++missing;
                }
            }
        }
        if (!missing || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        waited = true;
        std::unique_lock done_lock(g_pipeline_done_mutex);
        seen = g_pipelines_delivered.load();
        g_pipeline_done_cv.wait_until(done_lock, deadline, [&] { return g_pipelines_delivered.load() != seen; });
    }
    if (waited) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const TimedLock lock(g.mutex);
        g_sync_stats.total_ms += ms;
        g_sync_stats.worst_single_ms = std::max(g_sync_stats.worst_single_ms, ms);
        g_sync_frame_ms += ms;
        g_sync_frame_any = true;
    }
}

// View de uma textura enviada para o tipo que o shader declara. Com g.mutex. false = textura ainda
// nao existe; `*view` fica com o fallback quando o tipo nao bate.
bool ResolveTextureView(u64 key, u32 view_type, VkImageView* view) {
    const auto texture = g.textures.find(key);
    if (!key || texture == g.textures.end()) {
        return false;
    }
    texture->second.last_used = g.frame_serial + 1; // Fase 59: o frame em montagem (ainda nao contado)
    if (texture->second.view_type == view_type) {
        *view = texture->second.view;
    } else if ((texture->second.view_type == 1 || texture->second.view_type == 0) && view_type == 2 && texture->second.array_view) {
        *view = texture->second.array_view; // cubo ou 2D num binding declarado como array 2D
    } else if (texture->second.view_type == 1 && view_type == 0) {
        *view = texture->second.face_view; // cubo num binding declarado como 2D: a face 0
    } else {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1) < 20) {
            std::printf("[VULKAN] textura com tipo de view diferente do que o shader declara (textura %u, shader %u): sai branca\n",
                        texture->second.view_type, view_type);
        }
    }
    return true;
}

VkSampler GetGameSampler(u32 wrap_u, u32 wrap_v, bool point, u32 compare, u32 border, u32 lod, u32 filter) {
    const u32 mip_filter = (filter >> 26) & 3; // 0 = sem mip, 1 = ponto, 2 = linear
    const u64 key = (wrap_u & 3) | ((wrap_v & 3) << 2) | (point ? 16u : 0u) | ((compare & 7) << 5) | ((border & 3) << 8) |
                    (static_cast<u64>(lod & 0xFFFFFF) << 12) | (static_cast<u64>(mip_filter) << 36) | (static_cast<u64>(filter & 0x3FFF) << 38);
    if (const auto found = g.game_samplers.find(key); found != g.game_samplers.end()) {
        return found->second;
    }
    // Fase 21.5: os modos "borda" do GNM viram CLAMP_TO_BORDER com a cor que o S# pede. Antes
    // caiam em CLAMP_TO_EDGE, e os passes de brilho que contam com preto fora da imagem
    // esticavam a ultima linha / coluna em listras.
    static const VkSamplerAddressMode kModes[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    static const VkBorderColor kBorders[] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                                             VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    // Fase 25: mipmaps. Sem filtro de mip o S# pede so o nivel base (no Vulkan: maxLod 0.25).
    info.mipmapMode = mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.minLod = static_cast<float>(lod & 0xFFF) / 256.0f;
    info.maxLod = std::max(info.minLod, static_cast<float>((lod >> 12) & 0xFFF) / 256.0f);
    if (mip_filter == 0) {
        info.maxLod = std::min(info.maxLod, info.minLod + 0.25f);
    }
    info.mipLodBias = static_cast<float>(static_cast<s32>((filter & 0x3FFF) << 18) >> 18) / 256.0f; // 14 bits com sinal, 6.8
    info.addressModeU = kModes[wrap_u & 3];
    info.addressModeV = kModes[wrap_v & 3];
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.borderColor = kBorders[border & 3];
    info.compareEnable = (compare & 7) ? VK_TRUE : VK_FALSE; // sombras: comparacao de profundidade
    info.compareOp = static_cast<VkCompareOp>(compare & 7);
    {   // EXPERIMENTO (LD_SHADOW=always|never): forca o resultado da comparacao dos samplers de sombra
        static const char* force = std::getenv("LD_SHADOW");
        if (force && (compare & 7) >= 1 && (compare & 7) <= 6) {
            info.compareOp = force[0] == 'a' ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_NEVER;
        }
    }
    VkSampler sampler = VK_NULL_HANDLE;
    if (vk.vkCreateSampler(g.device, &info, nullptr, &sampler) != VK_SUCCESS) {
        sampler = g.sampler;
    } else {
        g.game_samplers[key] = sampler;
    }
    return sampler;
}

// Reserva `bytes` (alinhados a `alignment`) no anel; quando nao cabe ate o fim, volta ao inicio.
// O anel e muito maior que um frame, entao os dados do frame que a GPU ainda le nao sao pisados.
// Fase 54f: o jogo monta os draws do frame seguinte enquanto a GPU ainda le os aneis do frame atual.
// Antes um frame podia usar o anel quase inteiro; num frame enorme (entrada da cutscene da casa dos
// Simpsons, 2404 draws) o frame seguinte dava a volta e reescrevia indices e constantes que a GPU
// ainda lia: indices trocados no meio do caminho -> leitura fora dos buffers -> "GPU fault" e queda
// do driver. Agora cada frame usa no maximo metade do anel menos uma margem, e nenhuma alocacao
// passa de 1/16 dele: dois frames seguidos (com o pedaco perdido na volta) nunca se sobrepoem.
constexpr u64 RingMaxAllocation(u64 size) {
    return size / 16;
}
constexpr u64 RingFrameBudget(u64 size, u64 reserved) {
    return (size - reserved) / 2 - RingMaxAllocation(size);
}

u64 RingAllocate(State::HostBuffer& ring, u64 bytes, u64 alignment, u64 reserved) {
    u64 offset = (ring.head + alignment - 1) / alignment * alignment;
    if (offset + bytes > ring.size) {
        offset = (reserved + alignment - 1) / alignment * alignment;
    }
    ring.head = offset + bytes;
    return offset;
}

void DestroyAll() {
    if (g.device) {
        vk.vkDeviceWaitIdle(g.device);
        DestroyPipelines();
        if (g.timestamps) { // Fase 75
            vk.vkDestroyQueryPool(g.device, g.timestamps, nullptr);
            g.timestamps = VK_NULL_HANDLE;
            g.timestamps_written = false;
        }
        if (g_vk_cache) { // Fase 70
            vk.vkDestroyPipelineCache(g.device, g_vk_cache, nullptr);
            g_vk_cache = VK_NULL_HANDLE;
        }
        for (auto& [key, texture] : g.textures) {
            if (texture.array_view) vk.vkDestroyImageView(g.device, texture.array_view, nullptr);
        if (texture.face_view) vk.vkDestroyImageView(g.device, texture.face_view, nullptr);
            vk.vkDestroyImageView(g.device, texture.view, nullptr);
            vk.vkDestroyImage(g.device, texture.image, nullptr);
            vk.vkFreeMemory(g.device, texture.memory, nullptr);
        }
        g.textures.clear();
        g.texture_bytes = 0;
        {
            const TimedLock pool_lock(g_texture_pool.mutex); // Fase 59
            for (auto& chunk : g_texture_pool.chunks) {
                vk.vkFreeMemory(g.device, chunk.memory, nullptr);
            }
            g_texture_pool.chunks.clear();
            g_texture_pool.used = 0;
        }
        for (auto& [key, pair] : g.game_pairs) {
            if (pair.vs_lib) vk.vkDestroyPipeline(g.device, pair.vs_lib, nullptr);
            if (pair.fs_lib) vk.vkDestroyPipeline(g.device, pair.fs_lib, nullptr);
        }
        g.game_pairs.clear();
        {
            std::lock_guard shared_lock(g_shared_mutex); // Fase 43: modulos e layouts sao do cache
            for (auto& [hash, module] : g_module_cache) {
                vk.vkDestroyShaderModule(g.device, module, nullptr);
            }
            g_module_cache.clear();
            for (auto& [list, layouts] : g_layout_cache) {
                vk.vkDestroyPipelineLayout(g.device, layouts.second, nullptr);
                vk.vkDestroyDescriptorSetLayout(g.device, layouts.first, nullptr);
            }
            g_layout_cache.clear();
        }
        for (auto& [key, pipeline] : g.game_pipelines) {
            if (pipeline) vk.vkDestroyPipeline(g.device, pipeline, nullptr);
        }
        for (auto* libs : {&g_vertex_libs, &g_output_libs}) {
            for (auto& [key, library] : *libs) {
                if (library) vk.vkDestroyPipeline(g.device, library, nullptr);
            }
            libs->clear();
        }
        g.game_pipelines.clear();
        g.queue_game.clear();
        for (auto& [key, target] : g.targets) {
            if (target.sample_view) vk.vkDestroyImageView(g.device, target.sample_view, nullptr);
            if (target.array_view) vk.vkDestroyImageView(g.device, target.array_view, nullptr);
            if (target.copy_view) vk.vkDestroyImageView(g.device, target.copy_view, nullptr);
            if (target.copy_image) vk.vkDestroyImage(g.device, target.copy_image, nullptr);
            if (target.copy_memory) vk.vkFreeMemory(g.device, target.copy_memory, nullptr);
            if (target.raw_view) vk.vkDestroyImageView(g.device, target.raw_view, nullptr);
            vk.vkDestroyImageView(g.device, target.view, nullptr);
            vk.vkDestroyImage(g.device, target.image, nullptr);
            if (target.memory) vk.vkFreeMemory(g.device, target.memory, nullptr); // Fase 61: nulo = bloco do pool
        }
        g.targets.clear();
        g.target_by_address.clear();
        g.last_screen = nullptr;
        for (VkImageView& view : g.fallback_views) {
            if (view) vk.vkDestroyImageView(g.device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        for (u32 i = 0; i < 2; ++i) {
            if (g.fallback_images[i]) vk.vkDestroyImage(g.device, g.fallback_images[i], nullptr);
            if (g.fallback_memory[i]) vk.vkFreeMemory(g.device, g.fallback_memory[i], nullptr);
            g.fallback_images[i] = VK_NULL_HANDLE;
            g.fallback_memory[i] = VK_NULL_HANDLE;
        }
        for (auto& [key, sampler] : g.game_samplers) {
            vk.vkDestroySampler(g.device, sampler, nullptr);
        }
        g.game_samplers.clear();
        if (g.frame_pool) vk.vkDestroyDescriptorPool(g.device, g.frame_pool, nullptr);
        g.frame_pool = VK_NULL_HANDLE;
        if (g.sampler) vk.vkDestroySampler(g.device, g.sampler, nullptr);
        if (g.descriptor_pool) vk.vkDestroyDescriptorPool(g.device, g.descriptor_pool, nullptr); // libera os sets
        if (g.set_layout) vk.vkDestroyDescriptorSetLayout(g.device, g.set_layout, nullptr);
        g.sampler = VK_NULL_HANDLE;
        g.descriptor_pool = VK_NULL_HANDLE;
        g.set_layout = VK_NULL_HANDLE;
        if (g.pipeline_layout) vk.vkDestroyPipelineLayout(g.device, g.pipeline_layout, nullptr);
        if (g.vertex_shader) vk.vkDestroyShaderModule(g.device, g.vertex_shader, nullptr);
        if (g.fragment_shader) vk.vkDestroyShaderModule(g.device, g.fragment_shader, nullptr);
        for (State::HostBuffer* buffer : {&g.vertices, &g.indices, &g.staging, &g.frame_staging, &g.storage, &g.statics, &g.readback}) {
            if (buffer->buffer) vk.vkDestroyBuffer(g.device, buffer->buffer, nullptr);
            if (buffer->memory) vk.vkFreeMemory(g.device, buffer->memory, nullptr); // desfaz o mapeamento junto
            *buffer = State::HostBuffer{};
        }
        g.pipeline_layout = VK_NULL_HANDLE;
        g.vertex_shader = g.fragment_shader = VK_NULL_HANDLE;
        DestroySwapchainObjects();
        if (g.in_flight) vk.vkDestroyFence(g.device, g.in_flight, nullptr);
        if (g.image_available) vk.vkDestroySemaphore(g.device, g.image_available, nullptr);
        if (g.pool) vk.vkDestroyCommandPool(g.device, g.pool, nullptr);
        if (g.swapchain) vk.vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
        vk.vkDestroyDevice(g.device, nullptr);
    }
    if (g.surface) vk.vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
    if (g.messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vk.vkGetInstanceProcAddr(g.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(g.instance, g.messenger, nullptr);
        g.messenger = VK_NULL_HANDLE;
    }
    if (g.instance) vk.vkDestroyInstance(g.instance, nullptr);
    g.in_flight = VK_NULL_HANDLE;
    g.image_available = VK_NULL_HANDLE;
    g.pool = VK_NULL_HANDLE;
    g.command = VK_NULL_HANDLE;
    g.swapchain = VK_NULL_HANDLE;
    g.device = VK_NULL_HANDLE;
    g.surface = VK_NULL_HANDLE;
    g.instance = VK_NULL_HANDLE;
    g.physical = VK_NULL_HANDLE;
    g.active = false;
}

} // namespace

static std::string g_spirv_directory;

void SetGameSpirvDirectory(const std::string& directory) {
    g_spirv_directory = directory;
}

const std::string& GameSpirvDirectory() {
    return g_spirv_directory;
}


void SetVulkanValidation(bool enabled) {
    g_validation = enabled;
}

void SetVulkanValidationAbort(bool enabled) {
    g_validation_abort = enabled;
}

void SetVulkanEnabled(bool enabled) {
    g_enabled = enabled;
}

bool VulkanEnabled() {
    return g_enabled;
}

VulkanRenderer& VulkanRenderer::Instance() {
    static VulkanRenderer renderer;
    return renderer;
}

bool VulkanRenderer::Initialize(void* hwnd) {
    const TimedLock lock(g.mutex);
    if (g.active) {
        return true;
    }
    if (!g_enabled) {
        return Fail("Vulkan desligado (--no-vulkan)");
    }
    g.hwnd = static_cast<HWND>(hwnd);
    g.error.clear();
    const bool ok = (g.library || LoadLibraryAndGlobals()) && CreateInstance() && CreateSurface(g.hwnd) && PickPhysicalDevice() &&
                    CreateDevice() && CreateSwapchain() && CreateFrameObjects() && CreateGeometryObjects();
    if (!ok) {
        const std::string error = g.error;
        DestroyAll();
        g.error = error;
        return false;
    }
    CreatePipelineCache(); // Fase 70
    g.active = true;
    g_prepare_open = true;
    return true;
}

// Fase 54b: marcador de etapa (0xFFFF0 + etapa) nos dois offsets: comecou e terminou.
void TailMarker(u32 phase) {
    if (g.write_marker) {
        const u32 marker = ((g.frame_serial & 0xFFFu) << 20) | (0xFFFF0u + phase);
        g.write_marker(g.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.markers.buffer, 0, marker);
        g.write_marker(g.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.markers.buffer, 4, marker);
    }
}

const char* TailPhaseName(u32 value) {
    switch (value) {
    case 0xFFFF1: return "fim dos passes do jogo";
    case 0xFFFF2: return "leitura de volta dos alvos minusculos";
    case 0xFFFF3: return "quad da janela";
    case 0xFFFF4: return "fim do frame";
    default: return nullptr;
    }
}

// Fase 54: a fence do frame nao sinalizou no tempo. Na 4a vez seguida (~1 s), le os marcadores e diz
// qual draw a GPU comecou e nao terminou; se o dispositivo caiu, pede o relatorio de falha ao driver.
// Com g.mutex.
void NoteGpuTimeout() {
    ++g.hang_timeouts;
    if (g.hang_reported || g.hang_timeouts < 4) {
        return;
    }
    g.hang_reported = true;
    const VkResult status = vk.vkGetFenceStatus(g.device, g.in_flight);
    std::printf("[GPU TRAVADA] o frame %u nao terminou em %.1f s (%s); %zu draws do jogo gravados nele\n", g.frame_serial,
                g.hang_timeouts * 0.25, status == VK_ERROR_DEVICE_LOST ? "dispositivo PERDIDO" : "a GPU ainda esta nele",
                g.frame_draws.size());
    std::printf("[GPU TRAVADA] aneis: maior frame ate aqui vertices %.1f MiB, indices %.1f MiB, constantes %.1f MiB | frame em montagem %.1f / %.1f / %.1f MiB%s",
                g.peak_vertex_bytes / 1048576.0, g.peak_index_bytes / 1048576.0, g.peak_storage_bytes / 1048576.0, g.frame_vertex_bytes / 1048576.0,
                g.frame_index_bytes / 1048576.0, g.frame_storage_bytes / 1048576.0, "\n");
    std::printf("[GPU TRAVADA] envios de textura: %u gravados no comeco do ultimo frame (%llu bytes de staging), %u com submissao propria depois dele%s",
                g.staged_last_frame, static_cast<unsigned long long>(g.staged_bytes_last_frame), g.immediate_since_frame, "\n");
    if (g.write_marker && g.markers.mapped) {
        u32 started = 0, finished = 0;
        std::memcpy(&started, g.markers.mapped, 4);
        std::memcpy(&finished, g.markers.mapped + 4, 4);
        u32 serial = g.frame_serial & 0xFFFu;
        const std::vector<State::DrawInfo>* draws = &g.frame_draws;
        if ((started >> 20) == ((g.frame_serial - 1) & 0xFFFu)) { // Fase 54c: os marcadores sao do frame anterior
            serial = (g.frame_serial - 1) & 0xFFFu;
            draws = &g.previous_draws;
            std::printf("[GPU TRAVADA] os marcadores sao do frame ANTERIOR (%u, %zu draws do jogo): foi ele que travou%s", g.frame_serial - 1,
                        draws->size(), "\n");
        }
        u32 first = (started >> 20) == serial ? started & 0xFFFFF : 0;
        u32 last_done = (finished >> 20) == serial ? finished & 0xFFFFF : 0;
        const State::TailInfo& t = g.tail;
        std::printf("[GPU TRAVADA] fim do frame: %u passes, %u copias de realimentacao, %u leituras de volta, alvo exibido %ux%u formato %u (0x%llx), "
                    "%u alvos de render (%u criados neste frame)%s",
                    t.passes, t.feedback_copies, t.readbacks, t.shown_w, t.shown_h, t.shown_format,
                    static_cast<unsigned long long>(t.shown_address), t.targets_total, t.targets_new, "\n");
        if (TailPhaseName(first) || TailPhaseName(last_done)) {
            std::printf("[GPU TRAVADA] etapas: a ultima que COMECOU foi \"%s\", a ultima que TERMINOU foi \"%s\"%s",
                        TailPhaseName(first) ? TailPhaseName(first) : "(draw do jogo)",
                        TailPhaseName(last_done) ? TailPhaseName(last_done) : "(draw do jogo)", "\n");
            first = TailPhaseName(first) ? static_cast<u32>(draws->size()) : first;
            last_done = TailPhaseName(last_done) ? static_cast<u32>(draws->size()) : last_done;
        }
        std::printf("[GPU TRAVADA] marcadores: o ultimo draw que COMECOU foi o #%u, o ultimo que TERMINOU foi o #%u%s\n", first, last_done,
                    first == 0 ? " (nenhum draw do jogo comecou: travou antes, nas copias/envios do inicio do frame)"
                    : last_done >= draws->size() ? " (todos os draws do jogo terminaram: travou depois deles)" : "");
        const u32 from = last_done + 1, to = std::max(first, from);
        for (u32 n = from; n <= to && n <= draws->size() && n < from + 8; ++n) {
            const State::DrawInfo& d = (*draws)[n - 1];
            std::printf("[GPU TRAVADA]   draw #%u (fila %u): par %016llx pipeline %016llx | %s %u %s x %u instancias, base %d | "
                        "alvo %ux%u (%u cores, formato %u) prof %ux%u | viewport %.0fx%.0f | %u imagens (%u alvos de render), %u buffers (%llu bytes)\n",
                        n, d.queue_index, static_cast<unsigned long long>(d.pair_key), static_cast<unsigned long long>(d.pipeline_key),
                        d.indexed ? "indexado" : "sem indices", d.count, d.indexed ? (d.index_type == 32 ? "indices u32" : "indices u16") : "vertices",
                        d.instance_count, d.base_vertex, d.color_w, d.color_h, d.color_count, d.color_format, d.depth_w, d.depth_h, d.viewport_w,
                        d.viewport_h, d.image_count, d.target_images, d.buffer_count, static_cast<unsigned long long>(d.buffer_bytes));
            for (u32 i = 0; i < d.image_count && i < 4; ++i) {
                auto im = d.images[i];
                if (!im.is_target) {
                    if (const auto tex = g.textures.find(im.key); tex != g.textures.end()) {
                        im.w = tex->second.width;
                        im.h = tex->second.height;
                        im.levels = tex->second.levels;
                        im.depth = tex->second.depth;
                    } else {
                        im.w = im.h = 0; // textura branca / reserva
                    }
                }
                std::printf("[GPU TRAVADA]     imagem binding %u: %s 0x%llx %ux%u formato %u profundidade %u niveis %u | o shader declara tipo %u%s%s", im.binding,
                            im.is_target ? "ALVO" : "textura", static_cast<unsigned long long>(im.key), im.w, im.h, im.format, im.depth, im.levels,
                            im.view_type, im.array ? " (view de array)" : "", "\n");
            }
            for (u32 i = 0; i < d.sampler_count && i < 4; ++i) {
                const auto& sm = d.samplers[i];
                std::printf("[GPU TRAVADA]     sampler binding %u: wrap %u/%u ponto %u comparacao %u borda %u lod 0x%x filtro 0x%x%s", sm.binding, sm.wrap_u,
                            sm.wrap_v, sm.point, sm.compare, sm.border, sm.lod, sm.filter, "\n");
            }
        }
    }
    if ((status == VK_ERROR_DEVICE_LOST || g.device_lost) && g.get_fault_info) {
        VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (g.get_fault_info(g.device, &counts, nullptr) == VK_SUCCESS) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
            VkDeviceFaultInfoEXT fault{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            fault.pAddressInfos = addresses.data();
            fault.pVendorInfos = vendor.data();
            counts.vendorBinarySize = 0;
            const VkResult got = g.get_fault_info(g.device, &counts, &fault);
            if (got == VK_SUCCESS || got == VK_INCOMPLETE) {
                std::printf("[GPU TRAVADA] relatorio do driver: %s\n", fault.description);
                for (const auto& a : addresses) {
                    std::printf("[GPU TRAVADA]   endereco: tipo %d, 0x%llx (precisao 0x%llx)\n", static_cast<int>(a.addressType),
                                static_cast<unsigned long long>(a.reportedAddress), static_cast<unsigned long long>(a.addressPrecision));
                }
                for (const auto& v : vendor) {
                    std::printf("[GPU TRAVADA]   driver: %s (codigo 0x%llx, dado 0x%llx)\n", v.description,
                                static_cast<unsigned long long>(v.vendorFaultCode), static_cast<unsigned long long>(v.vendorFaultData));
                }
            }
        }
    }
    std::fflush(stdout);
}

// Fase 59: o port nunca descartava textura. Cada fase traz ~500 (0,5 GB) e o teto era 4096: numa sessao
// longa a memoria de video enchia (o Windows passa a mover memoria entre a placa e a RAM: travadas longas)
// e, no teto, texturas novas saiam brancas. Agora, passou de 3000 texturas, de 2,5 GB ou de 85% do
// orcamento de video do processo, as que nenhum frame usou nos ultimos ~30 s sao destruidas (64 por frame,
// as mais antigas primeiro). Se o jogo voltar a usar uma, ela e lida de novo da memoria dele.
u64 g_textures_evicted = 0;
void EvictForgottenTextures() {
    static u32 last_check = 0;
    static bool over_video = false;
    if (!g.game_shaders || std::getenv("LD_NO_TEXTURE_EVICTION")) {
        return;
    }
    if (g.memory_budget && g.frame_serial - last_check >= 120) { // a consulta ao driver a cada ~2 s
        last_check = g.frame_serial;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
        vk.vkGetPhysicalDeviceMemoryProperties2(g.physical, &props);
        u64 used = 0, total = 0;
        for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
            if (props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                used += budget.heapUsage[i];
                total += budget.heapBudget[i];
            }
        }
        over_video = total && used > total / 100 * 85;
    }
    // TESTE (LD_EVICT_TEST=N): limite de N texturas e ~2 s sem uso, para exercitar o descarte numa rota curta
    static const u32 test_limit = std::getenv("LD_EVICT_TEST") ? static_cast<u32>(std::atoi(std::getenv("LD_EVICT_TEST"))) : 0;
    const bool over = test_limit ? g.textures.size() > test_limit : g.textures.size() > 3000 || g.texture_bytes > (2560ull << 20) || over_video;
    if (!over) {
        return;
    }
    const u32 kForgottenFrames = test_limit ? 120 : 1800; // ~30 s a 60 quadros/s
    std::vector<std::pair<u32, u64>> candidates; // (ultimo uso, chave)
    for (const auto& [key, texture] : g.textures) {
        if (key && texture.last_used + kForgottenFrames < g.frame_serial) {
            candidates.emplace_back(texture.last_used, key);
        }
    }
    const size_t count = std::min<size_t>(candidates.size(), 64);
    std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end());
    for (size_t i = 0; i < count; ++i) {
        const auto it = g.textures.find(candidates[i].second);
        State::Texture& texture = it->second;
        if (texture.set) {
            vk.vkFreeDescriptorSets(g.device, g.descriptor_pool, 1, &texture.set);
        }
        g.texture_bytes -= std::min(g.texture_bytes, texture.bytes);
        DestroyTextureObjects(texture);
        g.textures.erase(it);
        ++g_textures_evicted;
    }
}

bool VulkanRenderer::DrawFrame(u64 display_address) {
    // Fase 59: etapas cronometradas (0 pipelines, 1 lock, 2 fence, 3 acquire, 4 gravacao, 5 submit, 6 present)
    auto lap_start = std::chrono::steady_clock::now();
    auto lap = [&](int i) {
        const auto now = std::chrono::steady_clock::now();
        g.draw_frame_ms[i] = std::chrono::duration<double, std::milli>(now - lap_start).count();
        lap_start = now;
    };
    for (double& ms : g.draw_frame_ms) {
        ms = 0;
    }
    WaitFramePipelines(); // Fase 45: sem g.mutex (as threads de pipeline precisam dele para entregar)
    lap(0);
    TimedLock lock(g.mutex);
    lap(1);
    for (State::QueuedGame& draw : g.queue_game) { // Fase 45: pendentes que ficaram prontos
        for (u32 i = 0; i < draw.image_count && draw.pending_images; ++i) {
            auto& image = draw.images[i];
            if (image.texture_key && ResolveTextureView(image.texture_key, image.view_type, &image.view)) {
                image.texture_key = 0;
                --draw.pending_images;
            }
        }
        g_draws_late_texture += draw.pending_images != 0;
        if (!draw.pipeline && draw.pipeline_key) {
            if (const auto found = g.game_pipelines.find(draw.pipeline_key); found != g.game_pipelines.end()) {
                draw.pipeline = found->second;
            }
            if (!draw.pipeline) {
                ++g_sync_stats.over_budget; // nao ficou pronto a tempo: o draw fica de fora deste frame
            }
        }
    }
    // Fase 38: fecha a conta de criacao sincrona do frame que termina aqui
    g_sync_stats.worst_frame_ms = std::max(g_sync_stats.worst_frame_ms, g_sync_frame_ms);
    g_sync_stats.frames_with_sync += g_sync_frame_any;
    g_sync_frame_ms = 0;
    g_sync_frame_any = false;
    if (!g.active) {
        return false;
    }
    auto skip = [&](bool rebuild) {
        ++g.skipped;
        if (rebuild) {
            vk.vkDeviceWaitIdle(g.device);
            ++g.swapchain_rebuilds;
            CreateSwapchain(); // se falhar (janela minimizada), o proximo frame tenta de novo
        }
        return false;
    };
    if (g.views.empty()) {
        return skip(true);
    }
    const u64 timeout_ns = 250'000'000ull; // nunca prende a thread de render do jogo
    if (vk.vkWaitForFences(g.device, 1, &g.in_flight, VK_TRUE, timeout_ns) != VK_SUCCESS) {
        NoteGpuTimeout();
        return skip(false);
    }
    lap(2);
    g.hang_timeouts = 0;
    EvictForgottenTextures(); // Fase 59
    if (!g.temp_staging.empty()) { // Fase 59: buffers temporarios de envio cujo frame ja terminou
        size_t kept = 0;
        for (const State::TempStaging& temp : g.temp_staging) {
            if (temp.recorded_serial && temp.recorded_serial <= g.frame_serial) {
                vk.vkDestroyBuffer(g.device, temp.buffer, nullptr);
                vk.vkFreeMemory(g.device, temp.memory, nullptr);
            } else {
                g.temp_staging[kept++] = temp;
            }
        }
        g.temp_staging.resize(kept);
    }
    if (!g_retired_pipelines.empty()) { // Fase 56: versoes rapidas que nenhum frame em voo usa mais
        size_t kept = 0;
        for (const RetiredPipeline& r : g_retired_pipelines) {
            if (g.frame_serial - r.serial >= 3) {
                vk.vkDestroyPipeline(g.device, r.pipeline, nullptr);
            } else {
                g_retired_pipelines[kept++] = r;
            }
        }
        g_retired_pipelines.resize(kept);
    }
    static const u32 marker_check = std::getenv("LD_GPU_MARKER_CHECK") ? std::atoi(std::getenv("LD_GPU_MARKER_CHECK")) : 0;
    if (marker_check && g.frame_serial == marker_check && g.markers.mapped) { // Fase 54: so para conferir os marcadores
        u32 started = 0, finished = 0;
        std::memcpy(&started, g.markers.mapped, 4);
        std::memcpy(&finished, g.markers.mapped + 4, 4);
        std::printf("[GPU TESTE] frame %u: comecou #%u (frame %u), terminou #%u (frame %u), %zu draws gravados\n", g.frame_serial,
                    started & 0xFFFFF, started >> 20, finished & 0xFFFFF, finished >> 20, g.frame_draws.size());
    }
    u32 image_index = 0;
    VkResult result = vk.vkAcquireNextImageKHR(g.device, g.swapchain, timeout_ns, g.image_available, VK_NULL_HANDLE, &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return skip(true);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return skip(false);
    }
    vk.vkResetFences(g.device, 1, &g.in_flight);
    lap(3);
    if (g.timestamps && g.timestamps_written) { // Fase 75: o frame anterior ja terminou (fence esperada acima)
        u64 stamps[2] = {};
        if (vk.vkGetQueryPoolResults(g.device, g.timestamps, 0, 2, sizeof(stamps), stamps, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            stamps[1] >= stamps[0]) {
            g.last_gpu_ms = (stamps[1] - stamps[0]) * g.timestamp_period_ns / 1e6;
        }
    }

    vk.vkResetCommandBuffer(g.command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(g.command, &begin);
    if (g.timestamps) {
        vk.vkCmdResetQueryPool(g.command, g.timestamps, 0, 2);
        vk.vkCmdWriteTimestamp(g.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.timestamps, 0);
    }
    vk.vkResetDescriptorPool(g.device, g.frame_pool, 0); // o frame anterior ja terminou (fence)
    const u32 upload_half = g.frame_half; // Fase 59: metade do staging que este frame envia
    RecordFrameUploads(g.command); // Fase 39: texturas criadas / reescritas desde o ultimo frame

    // ---------- 1. draws do jogo, cada um no alvo de render que o jogo escolheu ----------
    // Draws seguidos com os mesmos alvos ficam no mesmo vkCmdBeginRendering. Um passe novo comeca
    // quando os alvos mudam, quando o draw pede limpeza de profundidade, ou quando ele amostra um
    // alvo que ainda esta em layout de anexo (a barreira tem de ficar fora do passe).
    bool open = false;
    State::RenderTarget* current_colors[8]{};
    u32 current_count = 0;
    State::RenderTarget* current_depth = nullptr;
    VkImageLayout current_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D area{};
    VkPipeline bound = VK_NULL_HANDLE;
    u32 passes = 0;
    auto end_pass = [&] {
        if (open) {
            vk.vkCmdEndRendering(g.command);
            open = false;
        }
    };
    for (auto& [key, target] : g.targets) {
        target.copy_fresh = false;
    }
    ++g.frame_serial;
    g.previous_draws.swap(g.frame_draws);
    g.frame_draws.clear();
    g.hang_reported = false;
    g.tail = {};
    const u64 feedback_before = g.feedback_copies;
    u32 queue_index = 0;
    // Fase 59: ultimo estado dinamico gravado. Todos os pipelines do jogo tem os mesmos estados dinamicos
    // (viewport, scissor, constantes de blend, mascaras e referencia do estencil), entao o valor vale de um
    // draw para o outro; so o que muda e gravado (eram 9 chamadas ao driver por draw). Zera a cada passe.
    struct DynamicCache {
        bool valid = false, stencil = false;
        VkViewport viewport{};
        VkRect2D scissor{};
        float blend[4]{};
        u32 stencil_values[6]{};
    } dynamic;
    for (const State::QueuedGame& draw : g.queue_game) {
        ++queue_index;
        if (!draw.pipeline || draw.pending_images) {
            continue; // Fase 45: pipeline ainda compilando ou textura ainda sendo lida
        }
        // Fase 26: os passes diferidos de luz (sol, nevoa) AMOSTRAM o buffer de profundidade com ele
        // ligado para o teste de profundidade, sem gravar. Isso e valido com o layout somente-leitura;
        // antes o draw era tratado como realimentacao e descartado, e a cena ficava sem a luz do sol.
        bool feedback = false, barrier = false, depth_read = false;
        u32 copy_mask = 0; // imagens deste draw que sao um alvo em que ele mesmo grava: amostram a copia
        for (u32 i = 0; i < draw.image_count; ++i) {
            State::RenderTarget* sampled = draw.images[i].target;
            if (!sampled) {
                continue;
            }
            feedback |= sampled->layout == VK_IMAGE_LAYOUT_UNDEFINED; // nada foi desenhado nele ainda
            if (sampled == draw.depth) {
                if (draw.depth_write || draw.clear_depth) {
                    copy_mask |= 1u << i;
                } else {
                    depth_read = true;
                }
                continue;
            }
            bool own = false;
            for (u32 k = 0; k < draw.color_count; ++k) {
                own |= sampled == draw.colors[k];
            }
            if (own) {
                copy_mask |= 1u << i;
                feedback |= draw.images[i].array; // (a copia so tem view 2D)
            } else {
                barrier |= sampled->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        for (u32 i = 0; i < draw.image_count && !feedback; ++i) {
            if ((copy_mask >> i) & 1) {
                feedback |= !EnsureTargetCopy(draw.images[i].target);
            }
        }
        if (feedback) { // amostra um alvo em que nada foi desenhado ainda (ou a copia nao pode ser criada)
            ++g.feedback_skipped;
            continue;
        }
        // Fase 32: tira a copia do alvo antes do draw que o amostra e grava nele ao mesmo tempo.
        for (u32 i = 0; i < draw.image_count; ++i) {
            State::RenderTarget* sampled = draw.images[i].target;
            if (!((copy_mask >> i) & 1) || sampled->copy_fresh) {
                continue;
            }
            end_pass();
            const VkImageAspectFlags aspect = sampled->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            TransitionTarget(sampled, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const VkImageAspectFlags whole = sampled->depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : aspect;
            Transition(g.command, sampled->copy_image, whole, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = {aspect, 0, 0, 1};
            region.dstSubresource = {aspect, 0, 0, 1};
            region.extent = {sampled->width, sampled->height, 1};
            vk.vkCmdCopyImage(g.command, sampled->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sampled->copy_image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            Transition(g.command, sampled->copy_image, whole, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            sampled->copy_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sampled->copy_fresh = true;
            ++g.feedback_copies;
        }
        g.feedback_draws += copy_mask != 0;
        // quem grava num alvo sem estar usando a copia dele a deixa desatualizada
        for (u32 k = 0; k < draw.color_count; ++k) {
            bool uses_copy = false;
            for (u32 i = 0; i < draw.image_count; ++i) {
                uses_copy |= ((copy_mask >> i) & 1) && draw.images[i].target == draw.colors[k];
            }
            if (!uses_copy) {
                draw.colors[k]->copy_fresh = false;
            }
        }
        if (draw.depth && (draw.depth_write || draw.clear_depth)) {
            bool uses_copy = false;
            for (u32 i = 0; i < draw.image_count; ++i) {
                uses_copy |= ((copy_mask >> i) & 1) && draw.images[i].target == draw.depth;
            }
            if (!uses_copy) {
                draw.depth->copy_fresh = false;
            }
        }
        // Fase 34: profundidade amostrada (so leitura) com estencil gravado -> layout misto.
        const VkImageLayout wanted_depth_layout = !depth_read            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                  : draw.stencil_write || draw.clear_stencil ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
                                                                         : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        if (depth_read && draw.depth && draw.depth->layout != wanted_depth_layout) {
            barrier = true;
        }
        bool same = open && !barrier && draw.color_count == current_count && draw.depth == current_depth &&
                    (!draw.depth || wanted_depth_layout == current_depth_layout);
        for (u32 k = 0; same && k < draw.color_count; ++k) {
            same = draw.colors[k] == current_colors[k];
        }
        if (!same) {
            end_pass();
            for (u32 i = 0; i < draw.image_count; ++i) {
                if (draw.images[i].target && draw.images[i].target != draw.depth && !((copy_mask >> i) & 1)) {
                    TransitionTarget(draw.images[i].target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            VkRenderingAttachmentInfo colors[8]{};
            area = {16384, 16384};
            for (u32 k = 0; k < draw.color_count; ++k) {
                State::RenderTarget* target = draw.colors[k];
                colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colors[k].imageView = target->view;
                colors[k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colors[k].loadOp = target->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                TransitionTarget(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                area.width = std::min(area.width, target->width);
                area.height = std::min(area.height, target->height);
                current_colors[k] = target;
            }
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (draw.depth) {
                depth.imageView = draw.depth->view;
                const VkImageLayout depth_layout = wanted_depth_layout;
                depth.imageLayout = depth_layout;
                depth.loadOp = draw.depth->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                depth.storeOp = depth_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ? VK_ATTACHMENT_STORE_OP_NONE
                                                                                                : VK_ATTACHMENT_STORE_OP_STORE;
                depth.clearValue.depthStencil = {1.0f, 0};
                TransitionTarget(draw.depth, depth_layout);
                area.width = std::min(area.width, draw.depth->width);
                area.height = std::min(area.height, draw.depth->height);
            }
            current_count = draw.color_count;
            current_depth = draw.depth;
            current_depth_layout = wanted_depth_layout;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea = {{0, 0}, area};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = draw.color_count;
            rendering.pColorAttachments = colors;
            rendering.pDepthAttachment = draw.depth ? &depth : nullptr;
            rendering.pStencilAttachment = draw.depth ? &depth : nullptr;
            vk.vkCmdBeginRendering(g.command, &rendering);
            open = true;
            ++passes;
            dynamic = {};
        }
        if (draw.pipeline != bound) {
            vk.vkCmdBindPipeline(g.command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
            bound = draw.pipeline;
        }
        if (!dynamic.valid || std::memcmp(&dynamic.viewport, &draw.viewport, sizeof(VkViewport))) {
            vk.vkCmdSetViewport(g.command, 0, 1, &draw.viewport);
            dynamic.viewport = draw.viewport;
        }
        VkRect2D scissor = draw.scissor;
        scissor.offset.x = std::clamp<s32>(scissor.offset.x, 0, static_cast<s32>(area.width));
        scissor.offset.y = std::clamp<s32>(scissor.offset.y, 0, static_cast<s32>(area.height));
        scissor.extent.width = std::min<u32>(scissor.extent.width, area.width - scissor.offset.x);
        scissor.extent.height = std::min<u32>(scissor.extent.height, area.height - scissor.offset.y);
        if (!dynamic.valid || std::memcmp(&dynamic.scissor, &scissor, sizeof(VkRect2D))) {
            vk.vkCmdSetScissor(g.command, 0, 1, &scissor);
            dynamic.scissor = scissor;
        }
        // Fase 26: limpeza de profundidade do jogo (DB_RENDER_CONTROL.depth_clear). No PS4 o draw de
        // limpeza grava o valor de DB_DEPTH_CLEAR so dentro do seu retangulo; antes a imagem INTEIRA
        // era limpa (loadOp CLEAR). No mapa de sombras, que tem 4 cascatas na mesma imagem e uma
        // limpeza por cascata, sobrava so a ultima: quase tudo saia "na sombra". O draw em si nao e
        // desenhado quando so tem profundidade (o Z do quad nao e o valor de limpeza).
        if ((draw.clear_depth || draw.clear_stencil) && draw.depth) {
            if (scissor.extent.width && scissor.extent.height) {
                VkClearAttachment clear{};
                clear.aspectMask = (draw.clear_depth && !depth_read ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                   (draw.clear_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                clear.clearValue.depthStencil = {draw.clear_depth_value, draw.clear_stencil_value};
                const VkClearRect rect{scissor, 0, 1};
                if (clear.aspectMask) {
                    vk.vkCmdClearAttachments(g.command, 1, &clear, 1, &rect);
                }
            }
            if (draw.color_count == 0) {
                continue;
            }
        }
        if (draw.depth) { // mascaras e referencia do estencil (dinamicas)
            u32 values[6];
            for (u32 face = 0; face < 2; ++face) {
                const u32 ref = draw.stencil_ref[face];
                values[face * 3 + 0] = (ref >> 8) & 0xFF;
                values[face * 3 + 1] = draw.clear_stencil ? 0u : (ref >> 16) & 0xFF;
                values[face * 3 + 2] = draw.stencil_op_value[face] ? (ref >> 24) & 0xFF : ref & 0xFF;
            }
            if (!dynamic.valid || !dynamic.stencil || std::memcmp(dynamic.stencil_values, values, sizeof(values))) {
                for (u32 face = 0; face < 2; ++face) {
                    const VkStencilFaceFlags which = face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
                    vk.vkCmdSetStencilCompareMask(g.command, which, values[face * 3 + 0]);
                    vk.vkCmdSetStencilWriteMask(g.command, which, values[face * 3 + 1]);
                    vk.vkCmdSetStencilReference(g.command, which, values[face * 3 + 2]);
                }
                std::memcpy(dynamic.stencil_values, values, sizeof(values));
                dynamic.stencil = true;
            }
        }
        if (!dynamic.valid || std::memcmp(dynamic.blend, draw.blend_constants, sizeof(dynamic.blend))) {
            vk.vkCmdSetBlendConstants(g.command, draw.blend_constants);
            std::memcpy(dynamic.blend, draw.blend_constants, sizeof(dynamic.blend));
        }
        dynamic.valid = true;
        if (g.gpl) { // Fase 41: o que os pipelines ligados deixam dinamico (os monoliticos ignoram)
            const FixedState& f = draw.fixed;
            vk.vkCmdSetCullMode(g.command, f.cull);
            vk.vkCmdSetFrontFace(g.command, f.front);
            if (g.set_depth_clamp) {
                g.set_depth_clamp(g.command, f.clamp);
            }
            vk.vkCmdSetDepthTestEnable(g.command, f.depth_test);
            vk.vkCmdSetDepthWriteEnable(g.command, f.depth_write);
            vk.vkCmdSetDepthCompareOp(g.command, f.depth_compare);
            vk.vkCmdSetStencilTestEnable(g.command, f.stencil_test);
            vk.vkCmdSetStencilOp(g.command, VK_STENCIL_FACE_FRONT_BIT, f.front_ops.failOp, f.front_ops.passOp, f.front_ops.depthFailOp,
                                 f.front_ops.compareOp);
            vk.vkCmdSetStencilOp(g.command, VK_STENCIL_FACE_BACK_BIT, f.back_ops.failOp, f.back_ops.passOp, f.back_ops.depthFailOp,
                                 f.back_ops.compareOp);
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = g.frame_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &draw.pair->set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vk.vkAllocateDescriptorSets(g.device, &allocate, &set) != VK_SUCCESS) {
            continue;
        }
        VkDescriptorBufferInfo buffer_infos[24];
        VkDescriptorImageInfo image_infos[32];
        VkWriteDescriptorSet writes[56]{};
        u32 write_count = 0, image_count = 0;
        for (u32 i = 0; i < draw.buffer_count; ++i) {
            buffer_infos[i] = {g.storage.buffer, draw.buffers[i].offset, draw.buffers[i].bytes};
            VkWriteDescriptorSet& write = writes[write_count++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = draw.buffers[i].binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer_infos[i];
        }
        for (u32 i = 0; i < draw.image_count; ++i) {
            const bool from_copy = (copy_mask >> i) & 1;
            image_infos[image_count] = {VK_NULL_HANDLE, from_copy ? draw.images[i].target->copy_view : draw.images[i].view,
                                        !from_copy && draw.images[i].target && draw.images[i].target == draw.depth
                                            ? current_depth_layout
                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet& write = writes[write_count++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = draw.images[i].binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.pImageInfo = &image_infos[image_count++];
        }
        for (u32 i = 0; i < draw.sampler_count; ++i) {
            image_infos[image_count] = {draw.samplers[i].sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet& write = writes[write_count++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = draw.samplers[i].binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            write.pImageInfo = &image_infos[image_count++];
        }
        vk.vkUpdateDescriptorSets(g.device, write_count, writes, 0, nullptr);
        vk.vkCmdBindDescriptorSets(g.command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pair->layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(g.command, draw.pair->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 120, draw.push);
        if (draw.binding_count) {
            VkBuffer buffers[8];
            for (u32 i = 0; i < draw.binding_count; ++i) {
                buffers[i] = draw.vertex_static[i] ? g.statics.buffer : g.vertices.buffer;
            }
            vk.vkCmdBindVertexBuffers(g.command, 0, draw.binding_count, buffers, draw.vertex_offsets);
        }
        // Fase 54: marcador antes (comecou) e depois (terminou) de cada draw; o indice vai com o numero do frame
        const u32 marker = ((g.frame_serial & 0xFFFu) << 20) | static_cast<u32>(g.frame_draws.size() + 1);
        if (g.write_marker) {
            State::DrawInfo info{};
            info.pair_key = draw.pair_key;
            info.pipeline_key = draw.pipeline_key;
            info.queue_index = queue_index;
            info.count = draw.count;
            info.instance_count = draw.instance_count;
            info.index_type = draw.indexed ? (draw.index_type == VK_INDEX_TYPE_UINT32 ? 32 : 16) : 0;
            info.base_vertex = draw.base_vertex;
            info.indexed = draw.indexed;
            info.color_count = draw.color_count;
            if (draw.color_count) {
                info.color_w = draw.colors[0]->width;
                info.color_h = draw.colors[0]->height;
                info.color_format = static_cast<u32>(draw.colors[0]->format);
            }
            if (draw.depth) {
                info.depth_w = draw.depth->width;
                info.depth_h = draw.depth->height;
            }
            info.image_count = draw.image_count;
            for (u32 i = 0; i < draw.image_count; ++i) {
                info.target_images += draw.images[i].target != nullptr;
            }
            info.buffer_count = draw.buffer_count;
            for (u32 i = 0; i < draw.buffer_count; ++i) {
                info.buffer_bytes += draw.buffers[i].bytes;
            }
            info.viewport_w = draw.viewport.width;
            info.viewport_h = draw.viewport.height;
            for (u32 i = 0; i < draw.image_count && i < 4; ++i) {
                auto& im = info.images[i];
                im.binding = draw.images[i].binding;
                im.view_type = draw.images[i].view_type;
                im.array = draw.images[i].array;
                if (const State::RenderTarget* t = draw.images[i].target) {
                    im.is_target = 1;
                    im.w = t->width;
                    im.h = t->height;
                    im.format = static_cast<u32>(t->format);
                    im.depth = t->depth;
                    im.levels = 1;
                    im.key = t->address;
                } else {
                    im.key = draw.images[i].source_key; // detalhes so na hora do relatorio
                }
            }
            info.sampler_count = draw.sampler_count;
            for (u32 i = 0; i < draw.sampler_count && i < 4; ++i) {
                const u32* r = draw.samplers[i].raw;
                info.samplers[i] = {draw.samplers[i].binding, r[0], r[1], r[2], r[3], r[4], r[5], r[6]};
            }
            g.frame_draws.push_back(info);
            g.write_marker(g.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.markers.buffer, 0, marker);
        }
        if (draw.indexed) {
            vk.vkCmdBindIndexBuffer(g.command, draw.index_static ? g.statics.buffer : g.indices.buffer, draw.index_offset, draw.index_type);
            vk.vkCmdDrawIndexed(g.command, draw.count, draw.instance_count, 0, draw.base_vertex, 0);
        } else {
            vk.vkCmdDraw(g.command, draw.count, draw.instance_count, 0, 0);
        }
        if (g.write_marker) {
            g.write_marker(g.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.markers.buffer, 4, marker);
        }
    }
    end_pass();

    TailMarker(1);
    g.tail.passes = passes;
    g.tail.feedback_copies = static_cast<u32>(g.feedback_copies - feedback_before);
    g.tail.targets_total = static_cast<u32>(g.targets.size());
    g.tail.targets_new = static_cast<u32>(g.targets.size()) - std::min<u32>(g.targets_before_frame, static_cast<u32>(g.targets.size()));
    g.targets_before_frame = static_cast<u32>(g.targets.size());
    // ---------- 1b. leitura de volta dos alvos minusculos (luminancia) ----------
    for (u32 i = 0; i < g.readback_count; ++i) {
        State::Readback& readback = g.readbacks[i];
        const auto found = g.target_by_address.find(readback.address);
        if (found == g.target_by_address.end() || found->second->depth || found->second->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            continue;
        }
        State::RenderTarget* target = found->second;
        u32 bytes = 0;
        switch (target->format) {
        case VK_FORMAT_R8_UNORM: bytes = 1; break;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: bytes = 2; break;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: bytes = 4; break;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT: bytes = 8; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: bytes = 16; break;
        default: break;
        }
        const u32 width = std::min(readback.width, target->width), height = std::min(readback.height, target->height);
        if (!bytes || !width || !height || static_cast<u64>(width) * height * bytes > 256) {
            continue;
        }
        const VkImageLayout before = target->layout;
        TransitionTarget(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.bufferOffset = i * 256ull;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vk.vkCmdCopyImageToBuffer(g.command, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.buffer, 1, &region);
        TransitionTarget(target, before);
        readback.width = width;
        readback.height = height;
        readback.bytes_per_pixel = bytes;
        readback.pending = true;
        ++g.tail.readbacks;
    }
    TailMarker(2);

    // ---------- 2. janela: copia do alvo que o jogo mandou exibir + draws do pipeline base ----------
    State::RenderTarget* shown = nullptr;
    if (const auto found = g.target_by_address.find(display_address); display_address && found != g.target_by_address.end()) {
        shown = found->second;
    }
    // Fase 21.5: se o alvo que o jogo mandou exibir ainda nao esta pronto (nunca desenhado, ou
    // recem-recriado), repete o alvo do frame anterior. Antes caia no "ultimo alvo grande
    // desenhado", que podia ser um buffer intermediario (normais, velocidade): um quadro de
    // cores piscando na tela.
    if (!shown || shown->depth || shown->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        shown = g.last_screen;
    } else {
        g.last_screen = shown;
    }
    {   // DIAGNOSTICO (LD_SHOW_RT=<endereco em hexa>): mostra na janela um alvo intermediario do jogo
        static const u64 show = std::getenv("LD_SHOW_RT") ? std::strtoull(std::getenv("LD_SHOW_RT"), nullptr, 16) : 0;
        if (const auto found = g.target_by_address.find(show); show && found != g.target_by_address.end()) {
            shown = found->second;
        }
    }
    if (shown && (shown->depth || shown->layout == VK_IMAGE_LAYOUT_UNDEFINED || !shown->present_set)) {
        shown = nullptr;
    }
    VkPipeline present_pipeline = VK_NULL_HANDLE;
    if (shown) {
        g.tail.shown_w = shown->width;
        g.tail.shown_h = shown->height;
        g.tail.shown_format = static_cast<u32>(shown->format);
        g.tail.shown_address = shown->address;
        DrawCall quad; // posicao float3 + UV float2, sem cor, sem blend
        quad.stride = 20;
        quad.uv_format = DrawCall::kUvFloat2;
        quad.uv_offset = 12;
        quad.blend = false;
        present_pipeline = GetPipeline(quad);
        TransitionTarget(shown, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    Transition(g.command, g.images[image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo window{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    window.imageView = g.views[image_index];
    window.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    window.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    window.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    window.clearValue.color = g.clear;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, g.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &window;
    vk.vkCmdBeginRendering(g.command, &rendering);
    if (g.gpl) {
        // Fase 41: o driver nao restaura o estado estatico de um pipeline ligado depois de vkCmdSet*:
        // sem isto o quad de apresentacao herdava o cull / profundidade do ultimo draw do jogo (tela
        // preta ou branca). Volta aos valores dos pipelines estaticos daqui (sem cull, sem profundidade).
        vk.vkCmdSetCullMode(g.command, VK_CULL_MODE_NONE);
        vk.vkCmdSetFrontFace(g.command, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        if (g.set_depth_clamp) {
            g.set_depth_clamp(g.command, VK_FALSE);
        }
        vk.vkCmdSetDepthTestEnable(g.command, VK_FALSE);
        vk.vkCmdSetDepthWriteEnable(g.command, VK_FALSE);
        vk.vkCmdSetDepthCompareOp(g.command, VK_COMPARE_OP_ALWAYS);
        vk.vkCmdSetStencilTestEnable(g.command, VK_FALSE);
    }
    static const float kTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const VkBuffer vertex_buffers[2] = {g.vertices.buffer, g.vertices.buffer};
    if (present_pipeline) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(g.extent.width), static_cast<float>(g.extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, g.extent};
        const VkDeviceSize offsets[2] = {kPresentQuadOffset, 0};
        vk.vkCmdBindPipeline(g.command, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline);
        vk.vkCmdSetViewport(g.command, 0, 1, &viewport);
        vk.vkCmdSetScissor(g.command, 0, 1, &scissor);
        vk.vkCmdPushConstants(g.command, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, kIdentity);
        vk.vkCmdPushConstants(g.command, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 16, kTint);
        vk.vkCmdBindDescriptorSets(g.command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 1, &shown->present_set, 0, nullptr);
        vk.vkCmdBindVertexBuffers(g.command, 0, 2, vertex_buffers, offsets);
        vk.vkCmdBindIndexBuffer(g.command, g.indices.buffer, 0, VK_INDEX_TYPE_UINT16);
        vk.vkCmdDrawIndexed(g.command, 6, 1, 0, 0, 0);
    }
    bound = present_pipeline;
    for (const State::Queued& draw : g.queue_draws) {
        if (draw.pipeline != bound) {
            vk.vkCmdBindPipeline(g.command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
            bound = draw.pipeline;
        }
        const VkRect2D scissor{{std::max<s32>(0, static_cast<s32>(draw.viewport.x)), std::max<s32>(0, static_cast<s32>(draw.viewport.y))},
                               {std::min<u32>(g.extent.width, static_cast<u32>(draw.viewport.width + 0.5f)),
                                std::min<u32>(g.extent.height, static_cast<u32>(draw.viewport.height + 0.5f))}};
        vk.vkCmdSetScissor(g.command, 0, 1, &scissor);
        vk.vkCmdSetViewport(g.command, 0, 1, &draw.viewport);
        vk.vkCmdPushConstants(g.command, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, draw.mvp);
        vk.vkCmdPushConstants(g.command, g.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 16, kTint);
        vk.vkCmdBindDescriptorSets(g.command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 1, &draw.texture, 0, nullptr);
        const VkDeviceSize offsets[2] = {draw.vertex_offset, 0}; // binding 1 = branco fixo, no inicio do anel
        vk.vkCmdBindVertexBuffers(g.command, 0, draw.extra_binding ? 2 : 1, vertex_buffers, offsets);
        vk.vkCmdBindIndexBuffer(g.command, g.indices.buffer, draw.index_offset, draw.index_type);
        vk.vkCmdDrawIndexed(g.command, draw.index_count, 1, 0, 0, 0);
    }
    vk.vkCmdEndRendering(g.command);
    TailMarker(3);
    Transition(g.command, g.images[image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    TailMarker(4);
    g.draws_rendered += g.queue_draws.size() + g.queue_game.size();
    g.last_frame_draws = static_cast<u32>(g.queue_draws.size() + g.queue_game.size());
    g.last_frame_passes = passes;
    g.last_frame_showed_target = shown != nullptr;
    g.queue_draws.clear();
    g.queue_game.clear();
    g.peak_vertex_bytes = std::max(g.peak_vertex_bytes, g.frame_vertex_bytes);
    g.peak_index_bytes = std::max(g.peak_index_bytes, g.frame_index_bytes);
    g.peak_storage_bytes = std::max(g.peak_storage_bytes, g.frame_storage_bytes);
    g.frame_vertex_bytes = g.frame_index_bytes = g.frame_storage_bytes = 0;
    if (g.timestamps) {
        vk.vkCmdWriteTimestamp(g.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.timestamps, 1);
        g.timestamps_written = true;
    }
    vk.vkEndCommandBuffer(g.command);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g.render_finished[image_index];
    // Fase 59: copias de pixels que threads de textura reservaram nesta metade e ainda estao copiando
    // (sem o g.mutex): a GPU so pode ler o staging depois delas.
    for (int spin = 0; g.staging_inflight[upload_half].load(std::memory_order_acquire) > 0; ++spin) {
        if (spin < 64) {
            std::this_thread::yield();
        } else {
            Sleep(0);
        }
    }
    lap(4);
    result = QueueSubmit(1, &submit, g.in_flight);
    lap(5);
    if (result == VK_SUCCESS) {
        g.immediate_since_frame = 0; // Fase 54e
    }
    if (result != VK_SUCCESS) {
        Fail("vkQueueSubmit", result);
        std::printf("[VULKAN] ERRO: vkQueueSubmit do frame %u falhou: %s\n", g.frame_serial, g.error.c_str()); // Fase 54b
        if (result == VK_ERROR_DEVICE_LOST && !g.device_lost) { // Fase 54c: relatorio na hora
            g.device_lost = true;
            g.hang_timeouts = 4;
            NoteGpuTimeout();
        }
        return skip(false);
    }
    if (g.static_reset) {
        // O buffer estatico encheu: espera este frame terminar de le-lo e recomeca do zero. A
        // geracao avanca, e quem guardou posicoes (o cache do GnmDriver) as descarta.
        QueueWaitIdle();
        g.statics.head = 0;
        g.static_reset = false;
        ++g.static_generation;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &g.render_finished[image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain;
    present.pImageIndices = &image_index;
    // Fase 70b: o present (FIFO: pode esperar o VSync e depender da thread da janela) roda sem o g.mutex. Antes
    // as threads de pipeline / textura esperavam por ele a cada quadro para entregar o resultado, e fechar a janela
    // com o present preso podia travar a saida. LD_PRESENT_LOCK=1 volta ao jeito antigo.
    static const bool present_locked = [] {
        const char* env = std::getenv("LD_PRESENT_LOCK");
        return env && env[0] == '1';
    }();
    if (!present_locked) {
        lock.unlock();
    }
    {
        std::lock_guard queue_lock(g_queue_mutex);
        result = vk.vkQueuePresentKHR(g.queue, &present);
    }
    if (!present_locked) {
        lock.lock();
    }
    lap(6);
    MaybeSavePipelineCache(g_prewarm_total.load() && g_prewarm_done.load() >= g_prewarm_total.load()); // Fase 70
    FlushLevelNotes(); // Fase 72
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // apresentou (SUBOPTIMAL) ou nao (OUT_OF_DATE); de todo modo o swapchain e refeito
        const bool was_shown = result == VK_SUBOPTIMAL_KHR;
        vk.vkDeviceWaitIdle(g.device);
        ++g.swapchain_rebuilds;
        CreateSwapchain();
        if (was_shown) {
            ++g.presented;
        } else {
            ++g.skipped;
        }
        return was_shown;
    }
    if (result != VK_SUCCESS) {
        Fail("vkQueuePresentKHR", result);
        ++g.skipped;
        return false;
    }
    ++g.presented;
    return true;
}

namespace {

// Copia vertices e indices para os aneis e preenche a parte de geometria do draw na fila.
// Indices: os do jogo (copia direta), ou gerados - sequenciais, ou dois triangulos por quad.
bool UploadGeometry(State::Queued& queued, const void* vertex_data, u32 vertex_count, u32 stride, const void* index_data,
                    u32 draw_index_count, u32 draw_index_size, DrawCall::Primitive primitive) {
    const bool quads = primitive == DrawCall::kQuadList;
    const u32 source_count = index_data ? draw_index_count : vertex_count;
    const u32 index_count = quads ? source_count / 4 * 6 : source_count;
    const bool copy_direct = index_data && !quads;
    const u32 index_size = copy_direct ? draw_index_size : 4;
    const u64 vertex_bytes = static_cast<u64>(vertex_count) * stride;
    const u64 index_bytes = static_cast<u64>(index_count) * index_size;
    if (!index_count || vertex_bytes > kVertexRingBytes / 4 || index_bytes > kIndexRingBytes / 4) {
        return false;
    }
    queued.vertex_offset = RingAllocate(g.vertices, vertex_bytes, 4, kWhiteBytes);
    std::memcpy(g.vertices.mapped + queued.vertex_offset, vertex_data, vertex_bytes);
    queued.index_offset = RingAllocate(g.indices, index_bytes, 4, kIndexReserved);
    u8* out = g.indices.mapped + queued.index_offset;
    auto source_index = [&](u32 i) -> u32 {
        if (!index_data) {
            return i;
        }
        return draw_index_size == 2 ? static_cast<const u16*>(index_data)[i] : static_cast<const u32*>(index_data)[i];
    };
    if (copy_direct) {
        std::memcpy(out, index_data, index_bytes);
    } else {
        u32* out32 = reinterpret_cast<u32*>(out);
        if (quads) {
            for (u32 q = 0; q < source_count / 4; ++q) {
                static const u32 kCorner[6] = {0, 1, 2, 0, 2, 3};
                for (u32 k = 0; k < 6; ++k) {
                    out32[q * 6 + k] = source_index(q * 4 + kCorner[k]);
                }
            }
        } else {
            for (u32 i = 0; i < index_count; ++i) {
                out32[i] = i;
            }
        }
    }
    queued.index_count = index_count;
    queued.index_type = index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    g.bytes_uploaded += vertex_bytes + index_bytes;
    return true;
}

} // namespace

bool VulkanRenderer::GameShadersSupported() const {
    return g.active && g.game_shaders;
}

bool VulkanRenderer::HasGamePair(u64 key) const {
    const TimedLock lock(g.mutex);
    return g.game_pairs.count(key) != 0;
}

// ---- Fase 43: modulos e layouts compartilhados ----
// Com todos os pares do pacote AOT carregados na abertura (82 mil variantes), cada uma com seus dois
// modulos e dois layouts, seriam ~330 mil objetos. As variantes repetem os mesmos 68 mil shaders e
// poucas centenas de combinacoes de bindings: o modulo sai do cache pelo conteudo do SPIR-V e os
// layouts pela lista de bindings. Os objetos do cache vivem ate o fim (os pares nao os destroem).
// (os caches ficam no topo do arquivo: g_shared_mutex, g_module_cache, g_layout_cache)

VkShaderModule SharedModule(const std::vector<u32>& words) {
    u64 hash = 0xcbf29ce484222325ull ^ (words.size() * 0x9E3779B97F4A7C15ull);
    for (const u32 w : words) {
        hash = (hash ^ w) * 0x100000001b3ull;
    }
    {
        std::lock_guard lock(g_shared_mutex);
        if (const auto found = g_module_cache.find(hash); found != g_module_cache.end()) {
            return found->second;
        }
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * 4;
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.vkCreateShaderModule(g.device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    std::lock_guard lock(g_shared_mutex);
    const auto [it, inserted] = g_module_cache.try_emplace(hash, module);
    if (!inserted) { // outra thread criou o mesmo ao mesmo tempo
        vk.vkDestroyShaderModule(g.device, module, nullptr);
    }
    return it->second;
}

bool SharedLayouts(const std::vector<GamePairLayout::Binding>& list, VkDescriptorSetLayout* set_out, VkPipelineLayout* layout_out) {
    std::vector<u32> key;
    for (const auto& b : list) {
        key.push_back(b.binding);
        key.push_back(static_cast<u32>(b.kind));
    }
    std::lock_guard lock(g_shared_mutex); // criar layout e rapido: pode ser com o lock
    if (const auto found = g_layout_cache.find(key); found != g_layout_cache.end()) {
        *set_out = found->second.first;
        *layout_out = found->second.second;
        return true;
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const GamePairLayout::Binding& b : list) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = b.binding;
        binding.descriptorType = b.kind == GamePairLayout::kBuffer  ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                 : b.kind == GamePairLayout::kImage ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                                    : VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(binding);
    }
    VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = static_cast<u32>(bindings.size());
    set_layout.pBindings = bindings.data();
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    if (vk.vkCreateDescriptorSetLayout(g.device, &set_layout, nullptr, &set) != VK_SUCCESS) {
        return false;
    }
    // Bloco de push constants dos modulos recompilados: 120 bytes, visto pelos dois estagios.
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 120};
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &set;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vk.vkCreatePipelineLayout(g.device, &pipeline_layout, nullptr, &layout) != VK_SUCCESS) {
        vk.vkDestroyDescriptorSetLayout(g.device, set, nullptr);
        return false;
    }
    g_layout_cache[key] = {set, layout};
    *set_out = set;
    *layout_out = layout;
    return true;
}

void VulkanRenderer::GetSharedObjectCounts(u64* modules, u64* layouts) const {
    std::lock_guard lock(g_shared_mutex);
    *modules = g_module_cache.size();
    *layouts = g_layout_cache.size();
}

// ---- Fase 59: modulos sob demanda ----
// A abertura criava os VkShaderModule dos 170 mil pares do pacote AOT (106 mil modulos): +2,65 GB de RAM em
// uso so com a copia que o driver guarda de cada um. Agora os pares do pacote entram registrados SEM
// modulos (layouts sim); o primeiro pipeline de um par pede os modulos ao carregador (o SPIR-V sai do
// pacote, que fica na memoria comprimido, ~790 MB) na propria thread de pipeline, longe da de render.
namespace {

VulkanRenderer::ModuleLoader g_module_loader = nullptr;
std::atomic<u64> g_lazy_modules_loaded{0}, g_lazy_modules_failed{0};

// Com g.mutex ja tomado (caminho sincrono, raro).
bool ResolvePairModulesLocked(u64 key, State::GamePair& pair) {
    if (pair.vs && pair.ps) {
        return true;
    }
    std::vector<u32> vs, ps;
    if (!g_module_loader || !g_module_loader(key, &vs, &ps)) {
        ++g_lazy_modules_failed;
        return false;
    }
    pair.vs = SharedModule(vs);
    pair.ps = SharedModule(ps);
    g_lazy_modules_loaded += pair.vs && pair.ps;
    return pair.vs && pair.ps;
}

// Sem g.mutex (threads de pipeline).
bool ResolvePairModules(u64 key, VkShaderModule* vs_out, VkShaderModule* ps_out) {
    {
        const TimedLock lock(g.mutex);
        const auto pair = g.game_pairs.find(key);
        if (pair == g.game_pairs.end()) {
            return false;
        }
        if (pair->second.vs && pair->second.ps) {
            *vs_out = pair->second.vs;
            *ps_out = pair->second.ps;
            return true;
        }
    }
    std::vector<u32> vs_code, ps_code;
    if (!g_module_loader || !g_module_loader(key, &vs_code, &ps_code)) {
        ++g_lazy_modules_failed;
        return false;
    }
    const VkShaderModule vs = SharedModule(vs_code), ps = SharedModule(ps_code); // (cache por conteudo: sem duplicata)
    if (!vs || !ps) {
        ++g_lazy_modules_failed;
        return false;
    }
    const TimedLock lock(g.mutex);
    if (const auto pair = g.game_pairs.find(key); pair != g.game_pairs.end()) {
        pair->second.vs = vs;
        pair->second.ps = ps;
    }
    *vs_out = vs;
    *ps_out = ps;
    ++g_lazy_modules_loaded;
    return true;
}

} // namespace

void VulkanRenderer::SetModuleLoader(ModuleLoader loader) {
    g_module_loader = loader;
}

bool VulkanRenderer::RegisterGamePairLazy(u64 key, const std::vector<GamePairLayout::Binding>& bindings) {
    {
        const TimedLock lock(g.mutex);
        if (!g.active || !g.game_shaders || g.gpl || !g_module_loader) { // GPL compila os modulos no registro
            return false;
        }
        if (g.game_pairs.count(key)) {
            return true;
        }
    }
    State::GamePair pair;
    pair.bindings = bindings;
    if (!SharedLayouts(bindings, &pair.set_layout, &pair.layout)) {
        return false;
    }
    const TimedLock lock(g.mutex);
    g.game_pairs.emplace(key, std::move(pair));
    return true;
}

void VulkanRenderer::LazyModuleStats(u64* loaded, u64* failed) const {
    *loaded = g_lazy_modules_loaded.load();
    *failed = g_lazy_modules_failed.load();
}

bool VulkanRenderer::RegisterGamePair(u64 key, const GamePairLayout& layout) {
    // Fase 29: chamado pela thread de carga. Os modulos e layouts sao criados SEM o g.mutex (a
    // thread de render o usa em cada draw); o lock so cobre a consulta e a insercao no mapa.
    {
        const TimedLock lock(g.mutex);
        if (!g.active || !g.game_shaders || layout.vs_spirv.size() < 5 || layout.ps_spirv.size() < 5) {
            return false;
        }
        if (g.game_pairs.count(key)) {
            return true;
        }
    }
    State::GamePair pair;
    pair.bindings = layout.bindings;
    // Fase 43: modulos e layouts do cache compartilhado (nao sao destruidos com o par)
    pair.vs = SharedModule(layout.vs_spirv);
    pair.ps = SharedModule(layout.ps_spirv);
    if (!pair.vs || !pair.ps || !SharedLayouts(layout.bindings, &pair.set_layout, &pair.layout)) {
        return false;
    }
    if (g.gpl) {
        // Fase 41: o driver compila os shaders do par agora (thread de pares / aquecimento), nao no draw
        const auto start = std::chrono::steady_clock::now();
        pair.vs_lib = CreateShaderLibrary(pair.vs, true, pair.layout);
        pair.fs_lib = CreateShaderLibrary(pair.ps, false, pair.layout);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const TimedLock lock(g_lib_mutex);
        if (pair.vs_lib && pair.fs_lib) {
            g_gpl_stats.shader_libs += 2;
            g_gpl_stats.shader_lib_ms += ms;
            g_gpl_stats.shader_lib_worst_ms = std::max(g_gpl_stats.shader_lib_worst_ms, ms);
        } else {
            ++g_gpl_stats.lib_failures;
            if (pair.vs_lib) vk.vkDestroyPipeline(g.device, pair.vs_lib, nullptr);
            if (pair.fs_lib) vk.vkDestroyPipeline(g.device, pair.fs_lib, nullptr);
            pair.vs_lib = pair.fs_lib = VK_NULL_HANDLE; // o par usa o pipeline monolitico
        }
    }
    const TimedLock lock(g.mutex);
    g.game_pairs[key] = std::move(pair);
    return true;
}

bool VulkanRenderer::QueueGameDraw(const GameDraw& draw) {
    const TimedLock lock(g.mutex);
    if (!g.active || !g.game_shaders || g.queue_game.size() >= kMaxDrawsPerFrame || draw.attribute_count > 16 || draw.binding_count > 8 ||
        draw.buffer_count > 24 || draw.image_count > 16 || draw.sampler_count > 16 || draw.color_count > 8 ||
        (!draw.color_count && !draw.has_depth) || static_cast<u32>(draw.primitive) > DrawCall::kPointList) {
        return false;
    }
    const auto pair = g.game_pairs.find(draw.pair);
    if (pair == g.game_pairs.end()) {
        return false;
    }
    // Fase 28: o draw e montado direto na fila (a estrutura tem ~1,6 KiB; antes era copiada inteira)
    g.queue_game.emplace_back();
    State::QueuedGame& queued = g.queue_game.back();
    struct Rollback {
        bool keep = false;
        ~Rollback() {
            if (!keep) {
                g.queue_game.pop_back();
            }
        }
    } rollback;
    queued.pair = &pair->second;
    queued.pair_key = draw.pair;
    // ---- alvos ----
    for (u32 i = 0; i < draw.color_count; ++i) {
        queued.colors[i] = GetTarget(draw.colors[i], false);
        if (!queued.colors[i]) {
            return false;
        }
    }
    queued.color_count = draw.color_count;
    if (draw.has_depth) {
        queued.depth = GetTarget(draw.depth, true);
        if (!queued.depth) {
            return false;
        }
    }
    queued.clear_depth = draw.clear_depth && queued.depth;
    queued.depth_write = queued.depth && ((draw.depth_control >> 1) & 1) && ((draw.depth_control >> 2) & 1);
    queued.clear_depth_value = draw.clear_depth_value;
    {
        const bool stencil_on = queued.depth && (draw.depth_control & 1);
        const u32 sc = draw.stencil_control;
        const bool back = (draw.depth_control >> 7) & 1;
        auto replace_op = [](u32 ops) { return (ops & 15) == 4 || ((ops >> 4) & 15) == 4 || ((ops >> 8) & 15) == 4; };
        queued.clear_stencil = draw.clear_stencil && queued.depth;
        queued.clear_stencil_value = draw.clear_stencil_value;
        queued.stencil_ref[0] = draw.stencil_ref[0];
        queued.stencil_ref[1] = back ? draw.stencil_ref[1] : draw.stencil_ref[0];
        queued.stencil_op_value[0] = replace_op(sc);
        queued.stencil_op_value[1] = back ? replace_op(sc >> 12) : queued.stencil_op_value[0];
        queued.stencil_write = stencil_on && ((sc & 0xFFF) != 0 || (back && (sc >> 12) != 0)) &&
                               (((draw.stencil_ref[0] >> 16) & 0xFF) != 0 || (back && ((draw.stencil_ref[1] >> 16) & 0xFF) != 0));
    }
    bool pipeline_pending = false;
    queued.pipeline_key = 0;
    queued.pipeline = GetGamePipeline(draw, pair->second, queued.colors, queued.depth, &queued.pipeline_key, &pipeline_pending);
    queued.fixed = MakeFixedState(draw, queued.depth != nullptr);
    if (!queued.pipeline && !pipeline_pending) {
        return false;
    }
    if (queued.pipeline) {
        queued.pipeline_key = 0; // Fase 45: chave so fica nos draws que esperam o pipeline
    }
    // ---- geometria: um trecho do anel por buffer de vertices; indices do jogo, ou gerados ----
    const bool quads = draw.primitive == DrawCall::kQuadList;
    const bool static_indices = draw.index_static_offset != ~0ull && !quads;
    const bool has_indices = draw.index_data || static_indices;
    const u32 source_count = has_indices ? draw.index_count : draw.vertex_count;
    const u32 out_count = quads ? source_count / 4 * 6 : source_count;
    const bool indexed = has_indices || quads;
    const u32 index_size = (has_indices && !quads) ? draw.index_size : 4;
    const u64 index_bytes = indexed && !static_indices ? static_cast<u64>(out_count) * index_size : 0;
    u64 vertex_bytes = 0;
    for (u32 i = 0; i < draw.binding_count; ++i) {
        const bool is_static = draw.bindings[i].static_offset != ~0ull;
        if ((!draw.bindings[i].data && !is_static) || !draw.bindings[i].stride || !draw.bindings[i].bytes) {
            return false;
        }
        if (!is_static && draw.bindings[i].bytes + 16 > RingMaxAllocation(kVertexRingBytes)) {
            ++g.ring_rejected; // Fase 54f: um buffer so maior que 1/16 do anel
            return false;
        }
        vertex_bytes += is_static ? 0 : draw.bindings[i].bytes + 16;
    }
    u64 storage_bytes = 0;
    for (u32 i = 0; i < draw.buffer_count; ++i) {
        storage_bytes += draw.buffers[i].bytes + 256;
    }
    // Fase 54f: metade de cada anel por frame (o frame anterior ainda esta sendo lido pela GPU)
    if (!out_count || index_bytes + 8 > RingMaxAllocation(kIndexRingBytes) ||
        g.frame_vertex_bytes + vertex_bytes > RingFrameBudget(kVertexRingBytes, kWhiteBytes) ||
        g.frame_index_bytes + index_bytes + 8 > RingFrameBudget(kIndexRingBytes, kIndexReserved) ||
        g.frame_storage_bytes + storage_bytes > RingFrameBudget(kStorageRingBytes, kZeroBytes)) {
        ++g.ring_rejected; // nao cabe no que resta dos aneis para este frame
        return false;
    }
    g.frame_vertex_bytes += vertex_bytes;
    g.frame_index_bytes += index_bytes + 8;
    g.frame_storage_bytes += storage_bytes;
    for (u32 i = 0; i < draw.binding_count; ++i) {
        queued.vertex_static[i] = draw.bindings[i].static_offset != ~0ull;
        if (queued.vertex_static[i]) {
            // o vertice base do draw desconta first_vertex (por causa dos buffers do anel); os
            // buffers estaticos, que estao inteiros, compensam avancando o inicio
            queued.vertex_offsets[i] = draw.bindings[i].static_offset +
                                       (draw.bindings[i].per_instance ? 0ull : static_cast<u64>(draw.first_vertex) * draw.bindings[i].stride);
            continue;
        }
        queued.vertex_offsets[i] = RingAllocate(g.vertices, draw.bindings[i].bytes, 4, kWhiteBytes);
        std::memcpy(g.vertices.mapped + queued.vertex_offsets[i], draw.bindings[i].data, draw.bindings[i].bytes);
    }
    queued.binding_count = draw.binding_count;
    queued.indexed = indexed;
    queued.count = out_count;
    queued.instance_count = std::max(1u, draw.instance_count);
    queued.base_vertex = draw.base_vertex - static_cast<s32>(draw.first_vertex);
    queued.index_static = static_indices;
    if (static_indices) {
        queued.index_offset = draw.index_static_offset;
        queued.index_type = index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    } else if (indexed) {
        queued.index_offset = RingAllocate(g.indices, index_bytes, 4, kIndexReserved);
        queued.index_type = index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        u8* out = g.indices.mapped + queued.index_offset;
        if (!quads) {
            std::memcpy(out, draw.index_data, index_bytes);
        } else {
            u32* out32 = reinterpret_cast<u32*>(out);
            for (u32 q = 0; q < source_count / 4; ++q) {
                static const u32 kCorner[6] = {0, 1, 2, 0, 2, 3};
                for (u32 k = 0; k < 6; ++k) {
                    const u32 i = q * 4 + kCorner[k];
                    out32[q * 6 + k] = !draw.index_data          ? i
                                       : draw.index_size == 2 ? static_cast<const u16*>(draw.index_data)[i]
                                                              : static_cast<const u32*>(draw.index_data)[i];
                }
            }
        }
    }
    g.bytes_uploaded += vertex_bytes + index_bytes;
    // ---- constantes ----
    const u64 alignment = std::max<u64>(16, g.properties.limits.minStorageBufferOffsetAlignment);
    for (u32 i = 0; i < draw.buffer_count; ++i) {
        const GameDraw::Buffer& buffer = draw.buffers[i];
        auto& out = queued.buffers[queued.buffer_count++];
        out.binding = buffer.binding;
        if (!buffer.data || !buffer.bytes || buffer.bytes > kStorageRingBytes / 16) {
            out.offset = 0; // zeros
            out.bytes = kZeroBytes;
            continue;
        }
        out.bytes = (buffer.bytes + 3ull) & ~3ull;
        out.offset = RingAllocate(g.storage, out.bytes, alignment, kZeroBytes);
        std::memcpy(g.storage.mapped + out.offset, buffer.data, buffer.bytes);
        g.bytes_uploaded += buffer.bytes;
    }
    // ---- texturas: alvo de render do jogo, textura enviada, ou branco do tipo que o shader declara ----
    bool textured = false;
    queued.pending_images = 0;
    for (u32 i = 0; i < draw.image_count; ++i) {
        const GameDraw::Image& image = draw.images[i];
        auto& out = queued.images[queued.image_count++];
        out.binding = image.binding;
        out.target = nullptr;
        out.array = false;
        out.texture_key = 0;
        out.source_key = image.texture_key;
        out.view_type = image.view_type;
        out.view = image.view_type >= 1 && image.view_type <= 3 ? g.fallback_views[image.view_type] : g.textures[0].view;
        if (image.target_address) {
            if (const auto target = g.target_by_address.find(image.target_address); target != g.target_by_address.end()) {
                if (image.view_type == 0) {
                    out.target = target->second;
                    out.view = target->second->sample_view ? target->second->sample_view : target->second->view;
                    textured = true;
                } else if (image.view_type == 2 && TargetArrayView(target->second)) {
                    out.target = target->second;
                    out.view = target->second->array_view;
                    out.array = true;
                    textured = true;
                }
            }
        } else if (ResolveTextureView(image.texture_key, image.view_type, &out.view)) {
            textured = true;
        } else if (image.pending && image.texture_key) {
            out.texture_key = image.texture_key; // Fase 45: chega ate o DrawFrame (ou o draw fica de fora)
            ++queued.pending_images;
            textured = true;
        }
    }
    for (u32 i = 0; i < draw.sampler_count; ++i) {
        const GameDraw::Sampler& sm = draw.samplers[i];
        queued.samplers[queued.sampler_count++] = {sm.binding, GetGameSampler(sm.wrap_u, sm.wrap_v, sm.point, sm.compare, sm.border, sm.lod, sm.filter),
                                                   {sm.wrap_u, sm.wrap_v, sm.point ? 1u : 0u, sm.compare, sm.border, sm.lod, sm.filter}};
    }
    std::memcpy(queued.push, draw.push, sizeof(queued.push));
    g.textured_draws += textured;
    queued.viewport = {draw.viewport[0], draw.viewport[1], draw.viewport[2], draw.viewport[3], draw.viewport[4], draw.viewport[5]};
    queued.scissor = {{std::max(0, draw.scissor[0]), std::max(0, draw.scissor[1])},
                      {static_cast<u32>(std::max(0, draw.scissor[2] - std::max(0, draw.scissor[0]))),
                       static_cast<u32>(std::max(0, draw.scissor[3] - std::max(0, draw.scissor[1])))}};
    std::memcpy(queued.blend_constants, draw.blend_constants, sizeof(queued.blend_constants));
    rollback.keep = true;
    ++g.draws_queued;
    ++g.game_draws;
    return true;
}

u64 VulkanRenderer::StaticUpload(const void* data, u64 bytes) {
    const TimedLock lock(g.mutex);
    if (!g.active || !data || !bytes || g.static_reset) {
        return ~0ull;
    }
    const u64 offset = (g.statics.head + 15) & ~15ull;
    if (offset + bytes > g.statics.size) {
        g.static_reset = true; // esvaziado no fim do proximo frame; ate la, tudo vai pelo anel
        return ~0ull;
    }
    std::memcpy(g.statics.mapped + offset, data, bytes);
    g.statics.head = offset + bytes;
    return offset;
}

u32 VulkanRenderer::StaticGeneration() const {
    return g.static_generation.load();
}

u64 VulkanRenderer::StaticBytes() const {
    const TimedLock lock(g.mutex);
    return g.statics.head;
}

u32 VulkanRenderer::RenderTargets() const {
    const TimedLock lock(g.mutex);
    return static_cast<u32>(g.targets.size());
}

void VulkanRenderer::RequestReadback(u64 address, u32 width, u32 height) {
    const TimedLock lock(g.mutex);
    for (u32 i = 0; i < g.readback_count; ++i) {
        if (g.readbacks[i].address == address) {
            if (!g.readbacks[i].pending) {
                g.readbacks[i].width = width;
                g.readbacks[i].height = height;
            }
            return;
        }
    }
    if (g.readback_count < 8) {
        g.readbacks[g.readback_count++] = {address, width, height, 0, false};
    }
}

u32 VulkanRenderer::CollectReadbacks(ReadbackSink sink) {
    const TimedLock lock(g.mutex);
    if (!g.active || !g.readback.mapped) {
        return 0;
    }
    bool any = false;
    for (u32 i = 0; i < g.readback_count; ++i) {
        any |= g.readbacks[i].pending;
    }
    // os bytes so valem depois de o frame que os copiou terminar na GPU
    if (!any || vk.vkWaitForFences(g.device, 1, &g.in_flight, VK_TRUE, 250'000'000ull) != VK_SUCCESS) {
        return 0;
    }
    u32 delivered = 0;
    for (u32 i = 0; i < g.readback_count; ++i) {
        State::Readback& readback = g.readbacks[i];
        if (readback.pending) {
            readback.pending = false;
            sink(readback.address, static_cast<const unsigned char*>(g.readback.mapped) + i * 256ull, readback.width, readback.height,
                 readback.bytes_per_pixel);
            ++delivered;
        }
    }
    return delivered;
}

bool VulkanRenderer::HasRenderTarget(u64 address) const {
    const TimedLock lock(g.mutex);
    return g.target_by_address.count(address) != 0;
}

u32 VulkanRenderer::LastFramePasses() const {
    return g.last_frame_passes.load();
}

u64 VulkanRenderer::FeedbackCopies() const {
    return g.feedback_copies.load();
}

u64 VulkanRenderer::FeedbackDraws() const {
    return g.feedback_draws.load();
}

u64 VulkanRenderer::FeedbackSkipped() const {
    return g.feedback_skipped.load();
}

void VulkanRenderer::PipelineFailures(u64* failures, u64* retries) const {
    const TimedLock lock(g.mutex);
    *failures = g_pipeline_failures;
    *retries = g_pipeline_retries;
}

void VulkanRenderer::RingPeaks(u64* vertex, u64* index, u64* storage) const {
    *vertex = g.peak_vertex_bytes;
    *index = g.peak_index_bytes;
    *storage = g.peak_storage_bytes;
}

u64 VulkanRenderer::RingRejected() const {
    return g.ring_rejected.load();
}

bool VulkanRenderer::LastFrameShowedTarget() const {
    return g.last_frame_showed_target.load();
}

u64 VulkanRenderer::GameDraws() const {
    return g.game_draws.load();
}

u32 VulkanRenderer::GamePairs() const {
    const TimedLock lock(g.mutex);
    return static_cast<u32>(g.game_pairs.size());
}

u32 VulkanRenderer::GamePipelines() const {
    const TimedLock lock(g.mutex);
    return static_cast<u32>(g.game_pipelines.size());
}

bool VulkanRenderer::ValidationActive() const {
    return g.validation;
}

u64 VulkanRenderer::ValidationErrors() const {
    return g_validation_errors.load();
}

u64 VulkanRenderer::ValidationWarnings() const {
    return g_validation_warnings.load();
}

bool VulkanRenderer::QueueDraw(const DrawCall& draw) {
    const TimedLock lock(g.mutex);
    if (!g.active || g.views.empty() || g.queue_draws.size() >= kMaxDrawsPerFrame || !draw.vertex_data || !draw.vertex_count ||
        !draw.stride || draw.primitive > DrawCall::kQuadList) {
        return false;
    }
    const VkPipeline pipeline = GetPipeline(draw);
    if (!pipeline) {
        return false;
    }
    State::Queued queued{};
    queued.pipeline = pipeline;
    queued.has_color = draw.has_color;
    queued.extra_binding = !draw.has_color || draw.uv_format == DrawCall::kUvNone;
    const auto texture = g.textures.find(draw.uv_format == DrawCall::kUvNone ? 0 : draw.texture_key);
    queued.texture = (texture != g.textures.end() ? texture->second : g.textures[0]).set;
    g.textured_draws += texture != g.textures.end() && texture->first != 0;
    if (!UploadGeometry(queued, draw.vertex_data, draw.vertex_count, draw.stride, draw.index_data, draw.index_count, draw.index_size,
                        draw.primitive)) {
        return false;
    }
    // v_clip = v * mvp (vetor-linha, 16 floats em ordem de linha). Lidos como mat4 por coluna no
    // shader, "mvp * v" da exatamente isso. Dois ajustes para o Vulkan: Y do clip cresce para
    // baixo (inverte), e Z vai para 0 (nao ha buffer de profundidade nesta etapa).
    std::memcpy(queued.mvp, draw.mvp, sizeof(queued.mvp));
    for (int row = 0; row < 4; ++row) {
        queued.mvp[row * 4 + 1] = -queued.mvp[row * 4 + 1];
        queued.mvp[row * 4 + 2] = 0.0f;
    }
    const float sx = g.extent.width / std::max(1.0f, draw.target_size[0]);
    const float sy = g.extent.height / std::max(1.0f, draw.target_size[1]);
    queued.viewport = {draw.viewport[0] * sx, draw.viewport[1] * sy, std::max(1.0f, draw.viewport[2] * sx),
                       std::max(1.0f, draw.viewport[3] * sy), 0.0f, 1.0f};
    g.queue_draws.push_back(queued);
    ++g.draws_queued;
    return true;
}

bool VulkanRenderer::HasTexture(u64 key) const {
    const TimedLock lock(g.mutex);
    return g.textures.count(key) != 0;
}

bool VulkanRenderer::UpdateTexture(u64 key, const TextureData& data) {
    const TimedLock lock(g.mutex);
    const auto found = g.textures.find(key);
    if (!g.active || !key || found == g.textures.end() || !data.data) {
        return false;
    }
    State::Texture& texture = found->second;
    if (data.width != texture.width || data.height != texture.height || std::max(1u, data.depth) != texture.depth ||
        data.bytes != texture.bytes || data.bytes > kStagingBytes || std::clamp(data.levels, 1u, 16u) < texture.levels) {
        return false;
    }
    {
        // Fase 39: reescrita no comeco do proximo frame (a barreira espera o frame anterior parar de amostrar)
        VkBufferImageCopy regions[16]{};
        for (u32 level = 0; level < texture.levels; ++level) {
            regions[level].bufferOffset = level == 0 ? 0 : data.level_offsets[level];
            regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, texture.layers};
            regions[level].imageExtent = {std::max(1u, texture.width >> level), std::max(1u, texture.height >> level),
                                          level == 0 ? texture.depth : 1u};
        }
        if (StageFrameUpload(texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, data, texture.levels, texture.layers, regions)) {
            return true;
        }
    }
    ++g_upload_stats.immediate;
    const VkResult before = QueueWaitIdle(); // o frame anterior ainda pode estar amostrando a imagem
    if (before != VK_SUCCESS) {
        std::printf("[GPU TRAVADA] a fila ja voltou %s ANTES do reenvio imediato da textura 0x%llx%s", ResultName(before),
                    static_cast<unsigned long long>(key), "\n");
    }
    std::memcpy(g.staging.mapped, data.data, data.bytes);
    vk.vkResetCommandBuffer(g.upload_command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(g.upload_command, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, texture.layers};
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.vkCmdPipelineBarrier(g.upload_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
    VkBufferImageCopy regions[16]{};
    for (u32 level = 0; level < texture.levels; ++level) {
        regions[level].bufferOffset = level == 0 ? 0 : data.level_offsets[level];
        regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, texture.layers};
        regions[level].imageExtent = {std::max(1u, texture.width >> level), std::max(1u, texture.height >> level),
                                      level == 0 ? texture.depth : 1u};
    }
    vk.vkCmdCopyBufferToImage(g.upload_command, g.staging.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.levels, regions);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vk.vkCmdPipelineBarrier(g.upload_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
    vk.vkEndCommandBuffer(g.upload_command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g.upload_command;
    QueueSubmit(1, &submit, VK_NULL_HANDLE);
    CheckImmediateUpload(before == VK_SUCCESS ? QueueWaitIdle() : VK_SUCCESS, "reescrita", key, texture.width, texture.height,
                         texture.depth, texture.layers, texture.levels, static_cast<u32>(data.format), data.bytes, regions);
    return true;
}

bool VulkanRenderer::UploadTexture(u64 key, const TextureData& texture, u64 prepared) {
    std::unique_ptr<State::Texture> objects(reinterpret_cast<State::Texture*>(prepared));
    const TimedLock lock(g.mutex);
    if (!g.active || !key) {
        if (objects) {
            DestroyTextureObjects(*objects);
        }
        return false;
    }
    if (g.textures.count(key)) {
        if (objects) {
            DestroyTextureObjects(*objects);
        }
        return true;
    }
    if (objects) { // Fase 45: imagem e memoria ja criadas na thread que leu a textura
        const HitchClock hitch(HitchClock::kTexture);
        return FinishTexture(key, texture, *objects);
    }
    return CreateTexture(key, texture);
}


// Fase 59: textura de uma rajada que nao coube no staging do frame (128 MiB por frame). Antes ela ia
// pelo envio imediato (copia propria + vkQueueWaitIdle COM o g.mutex): a thread de render parava 20-40 ms
// na entrada de area. Agora ganha um buffer so dela, gravado no frame e destruido depois da fence.
bool UploadThroughTempStaging(u64 key, const TextureData& data, std::unique_ptr<State::Texture> objects) {
    State::TempStaging temp;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = data.bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    void* mapped = nullptr;
    bool ok = vk.vkCreateBuffer(g.device, &info, nullptr, &temp.buffer) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements requirements;
        vk.vkGetBufferMemoryRequirements(g.device, temp.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory;
        vk.vkGetPhysicalDeviceMemoryProperties(g.physical, &memory);
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        u32 type = ~0u;
        for (u32 i = 0; i < memory.memoryTypeCount && type == ~0u; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
            }
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        ok = type != ~0u && vk.vkAllocateMemory(g.device, &allocate, nullptr, &temp.memory) == VK_SUCCESS &&
             vk.vkBindBufferMemory(g.device, temp.buffer, temp.memory, 0) == VK_SUCCESS &&
             vk.vkMapMemory(g.device, temp.memory, 0, data.bytes, 0, &mapped) == VK_SUCCESS;
    }
    if (ok) {
        std::memcpy(mapped, data.data, data.bytes);
    }
    const TimedLock lock(g.mutex);
    auto drop_temp = [&] {
        if (temp.buffer) vk.vkDestroyBuffer(g.device, temp.buffer, nullptr);
        if (temp.memory) vk.vkFreeMemory(g.device, temp.memory, nullptr);
    };
    if (!g.active) {
        drop_temp();
        DestroyTextureObjects(*objects);
        return false;
    }
    if (!ok) {
        drop_temp();
        return FinishTexture(key, data, *objects); // sem memoria para o temporario: o caminho antigo
    }
    if (g.textures.count(key)) {
        drop_temp();
        DestroyTextureObjects(*objects);
        return true;
    }
    VkBufferImageCopy regions[16]{};
    TextureRegions(data, *objects, regions);
    State::PendingUpload pending;
    pending.source = temp.buffer;
    pending.image = objects->image;
    pending.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    pending.levels = objects->levels;
    pending.layers = objects->layers;
    pending.region_count = objects->levels;
    for (u32 i = 0; i < objects->levels; ++i) {
        pending.regions[i] = regions[i];
    }
    g.pending_uploads.push_back(pending);
    g.temp_staging.push_back(temp);
    ++g_upload_stats.deferred;
    g_upload_stats.deferred_bytes += data.bytes;
    ++g_temp_staging_count;
    return RegisterTexture(key, data, *objects);
}

// Fase 59: envio feito pela thread que destilou a textura. A vaga no staging do frame e reservada com
// o g.mutex, a copia dos pixels (o que mais custava na thread de render: ~1 ms por textura grande) e
// feita sem ele, e o registro volta a pegar o lock. Os objetos de `prepared` sao consumidos sempre.
bool VulkanRenderer::UploadTextureFromWorker(u64 key, const TextureData& data, u64 prepared) {
    std::unique_ptr<State::Texture> objects(reinterpret_cast<State::Texture*>(prepared));
    u64 base = 0;
    u32 half = 0;
    bool use_temp = false;
    {
        const TimedLock lock(g.mutex);
        if (!g.active || !key || !objects || g.textures.size() >= kMaxTextures) {
            if (objects) {
                DestroyTextureObjects(*objects);
            }
            return false;
        }
        if (g.textures.count(key)) {
            DestroyTextureObjects(*objects);
            return true;
        }
        const HitchClock hitch(HitchClock::kTexture);
        const u64 offset = (g.frame_head + 15) & ~15ull;
        if (!g.frame_staging.mapped) {
            return FinishTexture(key, data, *objects);
        }
        if (offset + data.bytes > kFrameStagingHalf) {
            use_temp = true; // Fase 59: nao coube no staging do frame -> buffer temporario (abaixo, fora do lock)
        }
    }
    if (use_temp) {
        return UploadThroughTempStaging(key, data, std::move(objects));
    }
    {
        const TimedLock lock(g.mutex);
        if (!g.active) {
            DestroyTextureObjects(*objects);
            return false;
        }
        if (g.textures.count(key)) {
            DestroyTextureObjects(*objects);
            return true;
        }
        const u64 offset = (g.frame_head + 15) & ~15ull;
        if (offset + data.bytes > kFrameStagingHalf) { // encheu enquanto o lock estava solto
            return FinishTexture(key, data, *objects);
        }
        half = g.frame_half;
        base = half * kFrameStagingHalf + offset;
        g.frame_head = offset + data.bytes;
        VkBufferImageCopy regions[16]{};
        TextureRegions(data, *objects, regions);
        State::PendingUpload pending;
        pending.image = objects->image;
        pending.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        pending.levels = objects->levels;
        pending.layers = objects->layers;
        pending.region_count = objects->levels;
        for (u32 i = 0; i < objects->levels; ++i) {
            pending.regions[i] = regions[i];
            pending.regions[i].bufferOffset += base;
        }
        g.pending_uploads.push_back(pending);
        ++g_upload_stats.deferred;
        g_upload_stats.deferred_bytes += data.bytes;
        g.staging_inflight[half].fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(g.frame_staging.mapped + base, data.data, data.bytes);
    g.staging_inflight[half].fetch_sub(1, std::memory_order_release);
    const TimedLock lock(g.mutex);
    if (!g.active) {
        return false;
    }
    if (g.textures.count(key)) {
        return true; // (outra copia da mesma textura chegou antes; esta imagem fica orfa, rarissimo)
    }
    return RegisterTexture(key, data, *objects); // a copia ja esta na fila do frame: em falha a imagem fica orfa
}

u64 VulkanRenderer::PrepareTexture(const TextureData& texture) {
    ++g_prepare_inflight;
    u64 result = 0;
    if (g_prepare_open.load()) {
        auto objects = std::make_unique<State::Texture>();
        if (CreateTextureObjects(texture, *objects)) {
            result = reinterpret_cast<u64>(objects.release());
        }
    }
    --g_prepare_inflight;
    return result;
}

void VulkanRenderer::DiscardPreparedTexture(u64 prepared) {
    if (std::unique_ptr<State::Texture> objects{reinterpret_cast<State::Texture*>(prepared)}; objects && g.device) {
        DestroyTextureObjects(*objects);
    }
}

u32 VulkanRenderer::TexturesCreated() const {
    const TimedLock lock(g.mutex);
    return g.textures.empty() ? 0 : static_cast<u32>(g.textures.size() - 1); // sem contar o branco
}

double VulkanRenderer::LastGpuFrameMs() const {
    return g.last_gpu_ms.load(); // Fase 75: GPU do ultimo quadro terminado (0 = sem timestamps)
}

void VulkanRenderer::LastDrawFrameTimes(double out[7]) const {
    for (int i = 0; i < 7; ++i) {
        out[i] = g.draw_frame_ms[i];
    }
}

// Fase 59: memoria de video do dispositivo (heaps DEVICE_LOCAL) usada por este processo e o
// orcamento que o Windows da a ele. Uso acima do orcamento = o sistema passa a mover alocacoes entre
// a placa e a RAM (paginacao de video), com travadas longas.
bool VulkanRenderer::VideoMemory(u64* used, u64* budget) const {
    *used = *budget = 0;
    if (!g.active || !g.memory_budget) {
        return false;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget_props};
    vk.vkGetPhysicalDeviceMemoryProperties2(g.physical, &props);
    for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
        if (props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            *used += budget_props.heapUsage[i];
            *budget += budget_props.heapBudget[i];
        }
    }
    return true;
}

u64 VulkanRenderer::TexturesEvicted() const {
    return g_textures_evicted;
}

u64 VulkanRenderer::TextureBytes() const {
    return g.texture_bytes;
}

u64 VulkanRenderer::TexturedDraws() const {
    return g.textured_draws.load();
}

u64 VulkanRenderer::DrawsQueued() const {
    return g.draws_queued.load();
}

u64 VulkanRenderer::DrawsRendered() const {
    return g.draws_rendered.load();
}

u32 VulkanRenderer::LastFrameDraws() const {
    return g.last_frame_draws.load();
}

u64 VulkanRenderer::BytesUploaded() const {
    return g.bytes_uploaded.load();
}

u32 VulkanRenderer::PipelinesCreated() const {
    return g.pipelines_created;
}

u32 VulkanRenderer::CheckGameShaderModules(const std::string& directory, u32* total) {
    const TimedLock lock(g.mutex);
    u32 accepted = 0, files = 0;
    std::error_code ec;
    if (g.active) {
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (entry.path().extension() != ".spv") {
                continue;
            }
            ++files;
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() < 20 || bytes.size() % 4) {
                continue;
            }
            std::vector<u32> words(bytes.size() / 4);
            std::memcpy(words.data(), bytes.data(), bytes.size());
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = bytes.size();
            info.pCode = words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (vk.vkCreateShaderModule(g.device, &info, nullptr, &module) == VK_SUCCESS) {
                ++accepted;
                vk.vkDestroyShaderModule(g.device, module, nullptr);
            }
        }
    }
    if (total) {
        *total = files;
    }
    return accepted;
}

void VulkanRenderer::Shutdown() {
    StopPipelineWorker(); // antes do lock: a thread de pipelines pega o g.mutex para entregar o resultado
    SavePipelineCacheFile("saida"); // Fase 70
    g_prepare_open = false; // Fase 45: nenhuma textura nova criada fora do lock depois daqui
    while (g_prepare_inflight.load()) {
        std::this_thread::yield();
    }
    const TimedLock lock(g.mutex);
    DestroyAll();
}

bool VulkanRenderer::Active() const {
    return g.active;
}

const std::string& VulkanRenderer::LastError() const {
    return g.error;
}

std::string VulkanRenderer::Describe() const {
    const TimedLock lock(g.mutex);
    if (!g.active) {
        return "Vulkan inativo: " + (g.error.empty() ? std::string("nao inicializado") : g.error);
    }
    static const char* const kTypes[] = {"outro", "integrada", "discreta", "virtual", "CPU"};
    char text[384];
    std::snprintf(text, sizeof(text),
                  "GPU \"%s\" (%s), Vulkan %u.%u.%u, fila %u; swapchain %ux%u %s, %zu imagens, modo %s; limpeza (%.1f, %.1f, %.1f, %.1f)",
                  g.properties.deviceName, g.properties.deviceType <= 4 ? kTypes[g.properties.deviceType] : "?",
                  VK_API_VERSION_MAJOR(g.properties.apiVersion), VK_API_VERSION_MINOR(g.properties.apiVersion),
                  VK_API_VERSION_PATCH(g.properties.apiVersion), g.queue_family, g.extent.width, g.extent.height,
                  g.format.format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" : "outro formato", g.images.size(),
                  g.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (VSync)" : "?", g.clear.float32[0], g.clear.float32[1],
                  g.clear.float32[2], g.clear.float32[3]);
    return text;
}

u64 VulkanRenderer::FramesPresented() const {
    return g.presented.load();
}

void VulkanRenderer::SetPipelineStateFile(const std::string& path) {
    g_pipeline_state_path = path;
}


// Fase 70: salva o cache de pipelines agora (saida do jogo pela janela, que nao passa pelo Shutdown).
// Roda numa thread propria com espera de ate 5 s: chamada pela thread da janela, o driver pode estar preso
// num present que depende dessa mesma janela (o fechamento travava). Se nao der tempo, fica o ultimo
// salvamento periodico; o arquivo antigo so e trocado por um novo inteiro.
void VulkanRenderer::SavePipelineCache(const char* reason) {
    if (!g_vk_cache) {
        return;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([reason, done] {
        SavePipelineCacheFile(reason);
        *done = true;
    }).detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!*done && std::chrono::steady_clock::now() < deadline) {
        Sleep(10);
    }
    if (!*done) {
        std::printf("[PIPELINE-CACHE] salvamento da saida nao terminou em 5 s: fica o ultimo arquivo salvo\n");
    }
}

void VulkanRenderer::PrewarmProgress(u32* done, u32* total) const {
    *done = g_prewarm_done.load();
    *total = g_prewarm_total.load();
}

// Le o arquivo de estados e manda criar os pipelines dos pares ja registrados. A linha e o vetor da
// chave de GetGamePipeline: par (2), primitiva, mascara de cor, estado de profundidade, cull, clamp,
// tem profundidade, numero de alvos / buffers / atributos, bits de estencil, controle de estencil,
// (formato, blend) por alvo, (stride | por instancia) por buffer, (location, formato, offset, buffer)
// por atributo.
// Fase 48: linhas do arquivo cujo par ainda nao estava registrado na chamada anterior. Os pares do
// pacote AOT so ficam registrados quando a carga completa termina (depois do aquecimento): sem isto
// os pipelines deles (quase todos os das DLCs) nunca eram criados antes do uso, nem na 2a visita.
std::vector<std::vector<u32>> g_prewarm_waiting;
std::mutex g_prewarm_waiting_mutex;
bool JobFromKey(const std::vector<u32>& key, PipelineJob& job);

u32 VulkanRenderer::PrewarmPipelines() {
    if (g_pipeline_state_path.empty() || !g.active || !g_async_pipelines.load()) {
        return 0;
    }
    LoadPipelineLevels(); // Fase 72
    std::lock_guard waiting_lock(g_prewarm_waiting_mutex);
    std::vector<std::vector<u32>> records;
    records.swap(g_prewarm_waiting);
    {
        std::lock_guard file_lock(g_pipeline_file_mutex);
        // Fase 71: a base distribuida com o port (refeita a cada versao por tools/pipeline_states.py) e depois o
        // arquivo do jogador. O hash da chave tira os repetidos; o que ja esta na base nao e gravado de novo no do
        // jogador. Linhas # sao comentarios; "# ld-pipeline-states N" com N diferente de 2 = formato de outra
        // versao, arquivo ignorado. LD_PIPE_BASE=0 ignora a base.
        static const bool use_base = [] {
            const char* env = std::getenv("LD_PIPE_BASE");
            return !(env && env[0] == '0');
        }();
        std::vector<std::string> paths;
        if (use_base) {
            paths.push_back((std::filesystem::path(g_pipeline_state_path).parent_path() / "pipeline_states_base.txt").string());
        }
        paths.push_back(g_pipeline_state_path);
        for (const std::string& path : paths) {
            std::ifstream in(path);
            std::string line;
            u32 read = 0, added = 0;
            bool first = true;
            while (std::getline(in, line)) {
                if (first && line.rfind("# ld-pipeline-states", 0) == 0) {
                    first = false;
                    if (std::atoi(line.c_str() + 20) != 2) {
                        std::printf("[PIPELINES] %s: formato de estados desconhecido (%s), ignorado\n", path.c_str(), line.c_str());
                        break;
                    }
                    continue;
                }
                first = false;
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::vector<u32> key;
                const char* p = line.c_str();
                char* end = nullptr;
                for (;;) {
                    const unsigned long value = std::strtoul(p, &end, 16);
                    if (end == p) {
                        break;
                    }
                    key.push_back(static_cast<u32>(value));
                    p = end;
                }
                ++read;
                if (key.size() >= 13 && g_pipeline_saved.insert(HashPipelineKey(key)).second) {
                    records.push_back(std::move(key));
                    ++added;
                }
            }
            static std::unordered_set<std::string> logged;
            if (read && logged.insert(path).second) {
                std::printf("[PIPELINES] estados de %s: %u linhas, %u novas\n", std::filesystem::path(path).filename().string().c_str(), read, added);
            }
        }
    }
    u32 queued = 0;
    std::vector<PipelineJob> first, rest; // Fase 72: area prioritaria na frente da fila
    for (const std::vector<u32>& key : records) {
        PipelineJob job;
        if (!JobFromKey(key, job)) {
            continue; // linha de outra versao do formato
        }
        job.prewarm = true;
        {
            std::lock_guard level_lock(g_level_mutex);
            const auto level = g_pipeline_level.find(job.hash);
            job.level = level == g_pipeline_level.end() ? 0 : level->second;
            job.priority = job.level && g_priority_levels.count(job.level);
        }
        {
            const TimedLock lock(g.mutex);
            const auto pair = g.game_pairs.find(job.draw.pair);
            if (pair == g.game_pairs.end()) {
                g_prewarm_waiting.push_back(key); // o par pode chegar depois (pacote AOT)
                continue;
            }
            if (g.game_pipelines.count(job.hash) || !g_pipelines_pending.insert(job.hash).second) {
                continue;
            }
            job.vs = pair->second.vs;
            job.ps = pair->second.ps;
            job.vs_lib = pair->second.vs_lib;
            job.fs_lib = pair->second.fs_lib;
            job.layout = pair->second.layout;
        }
        g_prewarm_priority_total += job.priority;
        (job.priority ? first : rest).push_back(std::move(job));
        ++g_prewarm_total;
        ++queued;
    }
    if (queued) {
        std::lock_guard lock(g_pipeline_mutex);
        EnsurePipelineWorkers();
        for (PipelineJob& job : first) {
            g_pipeline_jobs.push_back(std::move(job));
        }
        for (PipelineJob& job : rest) {
            g_pipeline_jobs.push_back(std::move(job));
        }
    }
    g_pipeline_cv.notify_all();
    return queued;
}

// Fase 72: o jogo comecou a carregar uma area (radar). Anota o primeiro uso dos pipelines por 2 min e passa para a
// frente da fila, na prioridade normal, o que do aquecimento for desta area e ainda nao foi criado.
void VulkanRenderer::SetCurrentLevel(const std::string& name) {
    u32 level = 0;
    {
        std::lock_guard lock(g_level_mutex);
        level = LevelIdLocked(name);
        if (level == g_current_level.load()) {
            return;
        }
        g_current_level = level;
        static bool session_marked = false;
        if (!session_marked) {
            session_marked = true;
            g_level_pending.emplace_back(1, 0); // linha "sessao": a primeira area depois dela e a de entrada
        }
        g_level_pending.emplace_back(0, level); // linha "ultima <area>"
    }
    g_level_note_until = g.frame_serial + 7200;
    u32 moved = 0;
    {
        std::lock_guard lock(g_pipeline_mutex);
        std::stable_partition(g_pipeline_jobs.begin(), g_pipeline_jobs.end(), [&](PipelineJob& job) {
            if (!job.prewarm) {
                return true; // pedidos de frames continuam na frente
            }
            if (job.level == level) {
                if (!job.priority) {
                    job.priority = true;
                    ++moved;
                }
                return true;
            }
            return false;
        });
    }
    std::printf("[PIPELINES] area %s: %u pipelines do aquecimento dela passaram para a frente da fila\n", name.c_str(), moved);
}

// Fase 72: progresso so dos pipelines prioritarios (LD_PREWARM_GATE=area).
void VulkanRenderer::PrewarmPriorityProgress(u32* done, u32* total) const {
    *done = g_prewarm_priority_done.load();
    *total = g_priority_from_session.load() ? g_prewarm_priority_total.load() : 0; // sem sessao anterior: 0 = espera tudo
}

// Linha do arquivo de estados -> job (sem os modulos do par). Falso se for de outra versao do formato.
bool JobFromKey(const std::vector<u32>& key, PipelineJob& job) {
    if (key.size() < 13) {
        return false;
    }
    {
        const u32 colors = key[8], bindings = key[9], attributes = key[10];
        if (colors > 8 || bindings > 8 || attributes > 16 || key.size() != 13 + colors * 2 + bindings + attributes * 4 ||
            key[2] > static_cast<u32>(DrawCall::kPointList)) {
            return false;
        }
        job.hash = HashPipelineKey(key);
        GameDraw& d = job.draw;
        d.pair = key[0] | (static_cast<u64>(key[1]) << 32);
        d.primitive = static_cast<DrawCall::Primitive>(key[2]);
        d.color_mask = key[3];
        d.depth_control = key[4] | key[11];
        d.cull = key[5];
        d.depth_clamp = key[6];
        job.has_depth = key[7] != 0;
        d.has_depth = job.has_depth;
        d.color_count = colors;
        d.binding_count = bindings;
        d.attribute_count = attributes;
        d.stencil_control = key[12];
        size_t at = 13;
        for (u32 i = 0; i < colors; ++i) {
            job.formats[i] = static_cast<VkFormat>(key[at++]);
            d.blend[i] = key[at++];
        }
        for (u32 i = 0; i < bindings; ++i) {
            d.bindings[i].stride = key[at] & 0x7FFFFFFF;
            d.bindings[i].per_instance = (key[at++] >> 31) != 0;
        }
        bool valid = true;
        for (u32 i = 0; i < attributes; ++i) {
            d.attributes[i].location = key[at++];
            valid = valid && key[at] < static_cast<u32>(GameDraw::kFormatCount);
            d.attributes[i].format = static_cast<GameDraw::Format>(key[at++]);
            d.attributes[i].offset = key[at++];
            d.attributes[i].binding = key[at++];
            valid = valid && d.attributes[i].binding < 8;
        }
        job.depth_format = job.has_depth ? TargetVkFormat(GameDraw::kTargetDepth32F) : VK_FORMAT_UNDEFINED;
        return valid;
    }
}

// Fase 53 (medicao): cada linha do arquivo e uma chave de estado (como o pipeline_states.txt). Cria cada
// uma na hora e de novo com so o blend trocado, so a profundidade trocada e so o formato de cor trocado:
// mostra se o driver reaproveita a compilacao dos shaders entre estados (o que decide como prever).
void VulkanRenderer::BenchPipelines(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    int n = 0;
    double sums[6]{};
    const char* salt_env = std::getenv("LD_PIPE_BENCH_SALT"); // outra mascara de cor: estados que o driver nunca viu
    const u32 salt = salt_env ? static_cast<u32>(std::strtoul(salt_env, nullptr, 16)) : 0;
    while (std::getline(in, line)) {
        std::vector<u32> key;
        const char* p = line.c_str();
        char* end = nullptr;
        for (;;) {
            const unsigned long value = std::strtoul(p, &end, 16);
            if (end == p) {
                break;
            }
            key.push_back(static_cast<u32>(value));
            p = end;
        }
        if (key.size() < 13) {
            continue;
        }
        const u32 colors = key[8], bindings = key[9], attributes = key[10];
        if (colors > 8 || bindings > 8 || attributes > 16 || key.size() != 13 + colors * 2 + bindings + attributes * 4) {
            continue;
        }
        GameDraw d{};
        VkFormat formats[8]{};
        d.pair = key[0] | (static_cast<u64>(key[1]) << 32);
        d.primitive = static_cast<DrawCall::Primitive>(key[2]);
        d.color_mask = key[3];
        d.depth_control = (key[4] | key[11]) ^ (salt << 4); // funcao de profundidade: estados que o driver nunca viu
        d.cull = key[5];
        d.depth_clamp = key[6];
        const bool has_depth = key[7] != 0;
        d.has_depth = has_depth;
        d.color_count = colors;
        d.binding_count = bindings;
        d.attribute_count = attributes;
        d.stencil_control = key[12];
        size_t at = 13;
        for (u32 i = 0; i < colors; ++i) {
            formats[i] = static_cast<VkFormat>(key[at++]);
            d.blend[i] = key[at++];
        }
        for (u32 i = 0; i < bindings; ++i) {
            d.bindings[i].stride = key[at] & 0x7FFFFFFF;
            d.bindings[i].per_instance = (key[at++] >> 31) != 0;
        }
        for (u32 i = 0; i < attributes; ++i) {
            d.attributes[i].location = key[at++];
            d.attributes[i].format = static_cast<GameDraw::Format>(key[at++]);
            d.attributes[i].offset = key[at++];
            d.attributes[i].binding = key[at++];
        }
        State::GamePair pair;
        {
            const TimedLock lock(g.mutex);
            const auto found = g.game_pairs.find(d.pair);
            if (found == g.game_pairs.end()) {
                continue;
            }
            pair = found->second;
        }
        const VkFormat depth_format = has_depth ? TargetVkFormat(GameDraw::kTargetDepth32F) : VK_FORMAT_UNDEFINED;
        double ms[6]{};
        {
            // sem otimizacao, num estado proprio (cull trocado): quanto custa o pipeline "rapido"
            GameDraw v = d;
            v.cull ^= 1;
            g_pipeline_flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
            const auto start = std::chrono::steady_clock::now();
            const VkPipeline pipeline = BuildGamePipeline(v, pair.vs, pair.ps, pair.layout, formats,
                                                          has_depth ? TargetVkFormat(GameDraw::kTargetDepth32F) : VK_FORMAT_UNDEFINED, has_depth);
            ms[5] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            g_pipeline_flags = 0;
            if (pipeline) {
                vk.vkDestroyPipeline(g.device, pipeline, nullptr);
            }
            sums[5] += ms[5];
        }
        for (int variant = 0; variant < 5; ++variant) {
            GameDraw v = d;
            VkFormat f[8];
            std::memcpy(f, formats, sizeof(f));
            static const bool alt = std::getenv("LD_PIPE_BENCH_ALT") != nullptr; // outras trocas (estados nunca vistos)
            if (variant == 1) { // so o blend
                v.blend[0] = alt ? 0x40000000u | (5u << 8) | 4u : (v.blend[0] >> 30) & 1 ? 0 : 0x40000000u | (1u << 8) | 1u;
            } else if (variant == 2) { // so a profundidade
                v.depth_control ^= alt ? 0x20 : 0x4; // funcao / escrita de profundidade
                v.cull = alt ? v.cull ^ 1 : v.cull ? 0 : 2;
            } else if (variant == 3 && colors) { // so o formato do alvo 0
                f[0] = alt ? VK_FORMAT_R8G8B8A8_UNORM : f[0] == VK_FORMAT_R16G16B16A16_SFLOAT ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
            } else if (variant == 4 && bindings) { // so a leitura de vertices (stride e offsets)
                v.bindings[0].stride += alt ? 32 : 16;
                for (u32 i = 0; i < attributes; ++i) {
                    v.attributes[i].offset += v.attributes[i].binding == 0 ? 4 : 0;
                }
            }
            const auto start = std::chrono::steady_clock::now();
            const VkPipeline pipeline = BuildGamePipeline(v, pair.vs, pair.ps, pair.layout, f, depth_format, has_depth);
            ms[variant] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (pipeline) {
                vk.vkDestroyPipeline(g.device, pipeline, nullptr);
            }
            sums[variant] += ms[variant];
        }
        ++n;
        std::printf("[BENCH] par %016llx: sem otimizacao %.1f ms | estado novo %.1f ms | so blend %.1f | so profundidade/cull %.1f | so formato %.1f | so vertices %.1f\n",
                    static_cast<unsigned long long>(d.pair), ms[5], ms[0], ms[1], ms[2], ms[3], ms[4]);
    }
    if (n) {
        std::printf("[BENCH] media de %d: sem otimizacao %.1f ms | estado novo %.1f ms | so blend %.1f | so profundidade/cull %.1f | so formato %.1f | so vertices %.1f\n", n,
                    sums[5] / n, sums[0] / n, sums[1] / n, sums[2] / n, sums[3] / n, sums[4] / n);
    }
    std::fflush(stdout);
}

void VulkanRenderer::SetAsyncPipelines(bool enabled) {
    g_async_pipelines = enabled;
}

void VulkanRenderer::SetPipelineBurstWait(double ms) {
    g_burst_wait_ms = ms;
}

void VulkanRenderer::SetFastPipelines(bool enabled) {
    g_fast_first = enabled;
}

void VulkanRenderer::FastPipelineStats(u64* fast, u64* upgraded) const {
    const TimedLock lock(g.mutex);
    *fast = g_fast_built;
    *upgraded = g_upgraded;
}

void VulkanRenderer::SetSyncPipelines(bool enabled, double budget_ms) {
    const TimedLock lock(g.mutex);
    g_sync_pipelines = enabled;
    g_sync_budget_ms = budget_ms;
}

void VulkanRenderer::SetGpl(bool enabled) {
    g_gpl_mode = enabled ? 1 : 0; // Fase 74: escolha explicita (linha de comando / launcher)
}

VulkanRenderer::GplStats VulkanRenderer::GetGplStats() const {
    const TimedLock lock(g_lib_mutex);
    VulkanRenderer::GplStats stats = g_gpl_stats;
    stats.active = g.gpl;
    return stats;
}

VulkanRenderer::UploadStats VulkanRenderer::GetUploadStats() const {
    const TimedLock lock(g.mutex);
    return g_upload_stats;
}

void VulkanRenderer::NoteFrameWait(double ms) {
    g_frame_wait_spent_ms = ms;
}

u64 VulkanRenderer::DrawsLateTexture() const {
    return g_draws_late_texture;
}

VulkanRenderer::SyncPipelineStats VulkanRenderer::GetSyncPipelineStats() const {
    const TimedLock lock(g.mutex);
    return g_sync_stats;
}

u64 VulkanRenderer::AsyncPipelines() const {
    return g_pipelines_async.load();
}

void VulkanRenderer::WaitPreviousFrame() {
    const TimedLock lock(g.mutex);
    if (g.active && vk.vkWaitForFences(g.device, 1, &g.in_flight, VK_TRUE, 250'000'000ull) == VK_TIMEOUT) {
        NoteGpuTimeout();
    }
}

u64 VulkanRenderer::FramesSkipped() const {
    return g.skipped.load();
}

void VulkanRenderer::SetClearColor(float r, float g_, float b, float a) {
    const TimedLock lock(g.mutex);
    g.clear = VkClearColorValue{{r, g_, b, a}};
}

} // namespace render
