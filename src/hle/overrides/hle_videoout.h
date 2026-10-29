#pragma once

#include "common/types.h"

// Interface interna do VideoOut para o GnmDriver (o eboot nao importa sceVideoOutSubmitFlip
// nem sceVideoOutGetFlipStatus: os flips chegam por sceGnmSubmitAndFlipCommandBuffers).
namespace hle::videoout {

struct FlipStatus { // SceVideoOutFlipStatus
    u64 count;
    u64 process_time;
    u64 tsc;
    s64 flip_arg;
    u64 submit_tsc;
    u64 reserved0;
    s32 gc_queue_num;
    s32 flip_pending_num;
    s32 current_buffer;
    u32 reserved1;
};
static_assert(sizeof(FlipStatus) == 0x40);

// A janela so e criada se isto for chamado com true antes de sceVideoOutOpen.
void SetWindowEnabled(bool enabled);

// Enfileira um flip; ele e concluido no proximo VSync (respeitando sceVideoOutSetFlipRate),
// quando o status e atualizado e o evento de flip e disparado nas equeues registradas.
int SubmitFlip(int handle, int buffer_index, int flip_mode, s64 flip_arg);
int CheckFlip(int handle, int buffer_index); // Fase 76

// Endereco do buffer de video registrado com este indice (0 se nao existe): e o alvo de render
// que o jogo esta mandando exibir.
u64 BufferAddress(int buffer_index);

// O endereco e de um buffer de video registrado com formato de pixel sRGB (0x80000000 e
// 0x80002200)? O jogo desenha nele como UNORM, mas a saida de video o trata como sRGB: o alvo tem
// de ser sRGB para a escrita final ganhar a curva de gama (e assim que o shadPS4 faz).
bool IsSrgbDisplayBuffer(u64 address);
// Fase 28 (--fps60): ignora o sceVideoOutSetFlipRate(1) do jogo e conclui um flip a cada VSync de 60 Hz.
void SetForce60Hz(bool enabled);
void SetUnlockedFps(bool enabled);
bool IsUnlockedFps();
void InitVideoDefaults();
// Fase 38: confere na abertura todos os .spv soltos de shaders_spv (diagnostico; segura o backend por segundos).
void SetCheckModules(bool enabled);
// Fase 60 (--fullscreen): janela abre em tela cheia sem bordas. F11 alterna a qualquer momento.
void SetStartFullscreen(bool enabled);
// Fase 61: tamanho inicial da area util da janela (padrao 1280x720)
void SetWindowSize(u32 width, u32 height);
u32 WindowWidth();
u32 WindowHeight();

FlipStatus GetFlipStatus(int handle);
u64 FlipCount();

// Fase 42: abre a janela (e o Vulkan) antes de o jogo comecar, para a tela de carregamento. O
// sceVideoOutOpen do jogo reaproveita a mesma janela. Devolve false sem janela ou sem Vulkan.
bool OpenWindowEarly(u32 timeout_ms);
// Fase 42: tela de carregamento. Mostra o progresso do pacote AOT, dos pares e dos pipelines ja
// vistos, e volta quando tudo terminou (ou depois de `max_seconds`).
void RunLoadingScreen(u32 max_seconds);

// Fase 65: contador de FPS / info no canto de cima, a esquerda (--stats-overlay; F10 alterna no jogo).
void SetStatsOverlay(bool enabled);
// Chamado uma vez por quadro do jogo, antes do DrawFrame: mede o tempo do quadro e, ligado, enfileira o desenho.
void QueueStatsOverlay();

// Menu de configuracoes de video dentro do jogo (F2 ou L3+R3)
void SetWindowResolution(u32 width, u32 height);
void ToggleVideoSettingsMenu();
bool IsVideoSettingsMenuOpen();
void HandleVideoMenuController(u32 pressed_buttons);

// true se a janela do jogo existe e esta em primeiro plano (o teclado so vale nesse caso).
bool WindowHasFocus();
// HWND da janela do jogo (nullptr se nao ha janela): dona dos dialogos do port.
void* WindowHandle();

} // namespace hle::videoout

namespace hle::gnm {
// Fase 42: progresso do que a abertura prepara (hle_gnm.cpp).
struct LoadingStatus {
    u64 pack_done = 0, pack_total = 0; // bytes do pacote AOT lidos para o cache de arquivos
    bool pack_finished = false;
    u32 pairs_done = 0, pairs_total = 0; // pares de shaders ja usados, carregados de novo
    bool pairs_finished = false;
    u32 pipelines_done = 0, pipelines_total = 0; // pipelines ja vistos, criados de novo
    u32 aot_done = 0, aot_total = 0;              // Fase 43: variantes do pacote AOT carregadas
    bool aot_finished = false;
};
void GetLoadingStatus(LoadingStatus* out);
} // namespace hle::gnm
