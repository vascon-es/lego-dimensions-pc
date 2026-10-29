// libSceNgs2 (HLE: maquina de estados das vozes + mixagem, Fase 24) e libSceAudioOut (saida real
// pelo waveOut do Windows; --no-audio volta ao modo mudo).
//
// Assinaturas e offsets conferidos nos call sites do eboot (FASE5_callsites_audio.txt):
//   sceNgs2SystemQueryBufferSize(option=0, info)            -> jogo le info+0x08 (hostBufferSize),
//                                                              aloca e grava o ponteiro em info+0x00
//   sceNgs2SystemCreate(option, info, &system)
//   sceNgs2RackQueryBufferSize(rackId, option, info)
//   sceNgs2RackCreate(system, rackId, option, info, &rack)
//   sceNgs2RackCreateWithAllocator(system, rackId, option, allocator, &rack)
//   sceNgs2RackGetVoiceHandle(rack, voiceIndex, &voice)
//   sceNgs2VoiceControl(voice, paramList)                   -> so o sinal do retorno e testado
//   sceNgs2VoiceGetState(voice, state, 0x30)                -> jogo le state+0x18
//   sceNgs2SystemRender(system, {buffer, size, waveform, channels}, 1)
//   sceAudioOutOpen(0xFF, 0, 0, 256, 48000, 5)              -> 256 amostras, 48 kHz, float 8 canais
//   sceAudioOutOutput(handle, buffer)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include "hle/hle_perf.h"
#include "hle/overrides/hle_override.h"
#include "runtime/crash_handler.h"

namespace {

using namespace hle;

constexpr int kNgs2ErrorInvalidHandle = static_cast<int>(0x804A0230);
constexpr int kNgs2ErrorInvalidArg = static_cast<int>(0x804A0001);
constexpr int kAudioOutErrorInvalidPort = static_cast<int>(0x80260003);

// O Ngs2 de verdade dimensiona isto pelo numero de vozes; nada e guardado aqui, entao basta
// um tamanho pequeno e valido para o alocador do jogo.
constexpr u64 kHostBufferSize = 64 * 1024;

struct ContextBufferInfo {
    void* host_buffer;
    u64 host_buffer_size;
    u64 reserved[5];
    u64 user_data;
};
static_assert(sizeof(ContextBufferInfo) == 0x40);

struct RenderBufferInfo {
    void* buffer;
    u64 buffer_size;
    u32 waveform_type;
    u32 num_channels;
};

constexpr u32 kSystemMagic = 0x4E475331; // "NGS1"
constexpr u32 kRackMagic = 0x4E475231;   // "NGR1"
constexpr u32 kVoiceMagic = 0x4E475631;  // "NGV1"

struct Rack;

// ---- maquina de estados de voz (Fase 10) ----------------------------------------------------
// O eboot nao importa sceNgs2VoicePlay/Pause/Stop: tudo passa por sceNgs2VoiceControl, uma
// lista de blocos {u16 size; s16 next; u32 id; ...} (FASE10_callsites_ngs2.txt). Ids usados:
//   0x05 PATCH (0x18 bytes)        0x06 KICK_EVENT {u32 eventId}
//   0x10000000 SETUP do sampler {formato: tipo, canais, sampleRate, ...}
//   0x10000001 WAVEFORM_BLOCKS {data, flags, numBlocks, aBlock}
//   0x10000005 PITCH {float ratio}
// Eventos: 0 PLAY  1 STOP  2 STOP_IMM  3 KILL  4 PAUSE  5 RESUME
// O que o jogo le de sceNgs2VoiceGetState (0x30 bytes):
//   +0x00 stateFlags          == 0 -> "voz terminou" (eboot+0x813044)
//   +0x10 numDecodedSamples   posicao de reproducao, guardada pelo jogo (eboot+0x8133b4)
//   +0x18 decodedDataSize     bytes consumidos; o jogo alimenta o stream quando
//                             (bytes enviados - isto) <= limiar (eboot+0x812172, 0x812842)
// Sem dispositivo de som, o tempo de cada voz e um relogio virtual: amostras = tempo real
// decorrido * sampleRate * pitch, consumidas dos blocos de waveform na ordem em que chegaram.
constexpr u32 kStateInUse = 0x01, kStatePlaying = 0x02, kStatePaused = 0x04, kStateEmpty = 0x20;
constexpr u32 kParamPatch = 0x05, kParamEvent = 0x06;
constexpr u32 kParamSamplerSetup = 0x10000000, kParamSamplerBlocks = 0x10000001, kParamSamplerPitch = 0x10000005;
constexpr u32 kEventPlay = 0, kEventStop = 1, kEventStopImm = 2, kEventKill = 3, kEventPause = 4, kEventResume = 5;

struct WaveBlock {
    u64 data_size = 0;
    u64 samples = 0;  // por passada
    u64 passes = 1;   // 1 + numRepeats; 0 = repete para sempre
    u64 position = 0; // amostras ja tocadas neste bloco (todas as passadas)
};

struct SamplerState {
    u32 state_flags;
    float envelope_height;
    float peak_height;
    u32 reserved;
    u64 num_decoded_samples;
    u64 decoded_data_size;
    u64 user_data;
    const void* waveform_data;
};
static_assert(sizeof(SamplerState) == 0x30);

// ---- audio de verdade (Fase 24) --------------------------------------------------------------
// Cada bloco de waveform e decodificado para float no momento em que o jogo o envia (PCM de 16
// bits ou VAG / ADPCM do PlayStation) e entra na fila de mixagem da voz. sceNgs2SystemRender
// mistura as vozes que estao tocando no buffer que o jogo pede, e o jogo entrega esse buffer a
// sceAudioOutOutput, que o manda para o dispositivo de som. O relogio virtual das fases
// anteriores continua mandando no ESTADO das vozes (o que o jogo consulta); a fila de mixagem so
// decide o que se ouve.
struct MixBlock {
    std::vector<float> pcm; // intercalado, `channels` por quadro
    u64 frames = 0;
    u64 passes = 1; // 0 = repete para sempre
    u64 done = 0;
};

struct Voice {
    u32 magic = kVoiceMagic;
    Rack* rack;
    u32 index;

    std::deque<MixBlock> mix;
    double mix_position = 0.0;  // quadro (fracionario) dentro da passada atual do primeiro bloco
    bool mix_active = false;    // recebeu PLAY e ainda nao recebeu STOP
    bool mix_exit_loop = false; // EXIT_LOOP: o bloco em repeticao termina na passada atual
    float gain_left[8] = {}, gain_right[8] = {}; // por canal de entrada
    float port_volume = 1.0f;
    float vag_history[8][4] = {}; // HEVAG: 4 amostras de historico por canal
    float mix_prev[8] = {}; // ultima amostra do bloco anterior (interpolacao atravessando blocos)

    bool in_use = false;
    bool playing = false;
    bool paused = false;
    u32 waveform_type = 0;
    u32 channels = 2;
    u32 sample_rate = 48000;
    float pitch = 1.0f;
    std::deque<WaveBlock> blocks;
    u64 decoded_samples = 0;
    u64 decoded_bytes = 0;
    u64 total_samples = 0; // total declarado de um stream (0 = som de um bloco so)
    double fraction = 0.0;
    std::chrono::steady_clock::time_point clock{};      // ultimo avanco
    std::chrono::steady_clock::time_point play_start{}; // ultimo PLAY
    std::chrono::steady_clock::time_point last_log{};
    u32 last_logged_flags = 0xFFFFFFFF;
    u64 queries = 0;
    // Alimentacao continua (Fase 13): quantas vezes o jogo mandou blocos desde o ultimo SETUP,
    // e o estado "fila seca" - a voz segue PLAYING|EMPTY por um tempo de tolerancia.
    u32 block_submissions = 0;
    bool starving = false;
    bool finished_dry = false; // terminou por falta de dados (nao por STOP): novos blocos a retomam
    std::chrono::steady_clock::time_point starve_since{};
    std::chrono::steady_clock::time_point last_block{};
    std::chrono::steady_clock::time_point last_query{};
};

// Todas as vozes ja criadas, para o relatorio do watchdog de cutscene.
std::mutex g_registry_mutex;
std::vector<Voice*> g_all_voices;

struct System {
    u32 magic = kSystemMagic;
};

struct Rack {
    u32 magic = kRackMagic;
    System* system;
    u32 rack_id;
    std::mutex mutex;
    std::map<u32, Voice*> voices;
};

int FillBufferInfo(ContextBufferInfo* info) {
    if (!info) {
        return kNgs2ErrorInvalidArg;
    }
    std::memset(info, 0, sizeof(*info));
    info->host_buffer_size = kHostBufferSize;
    return err::OK;
}

HLE_IMPL(int, sceNgs2SystemQueryBufferSize, const void* option, ContextBufferInfo* info) {
    const int result = FillBufferInfo(info);
    HLE_TRACE("option=%p -> hostBufferSize=0x%" PRIx64, option, kHostBufferSize);
    return result;
}
HLE_OVERRIDE(sceNgs2SystemQueryBufferSize);

HLE_IMPL(int, sceNgs2SystemCreate, const void* option, const ContextBufferInfo* info, System** out) {
    if (!out) {
        return kNgs2ErrorInvalidArg;
    }
    *out = new System;
    HLE_TRACE("buffer=%p (0x%" PRIx64 " bytes) -> system %p", info ? info->host_buffer : nullptr,
              info ? info->host_buffer_size : 0, static_cast<void*>(*out));
    return err::OK;
}
HLE_OVERRIDE(sceNgs2SystemCreate);

HLE_IMPL(int, sceNgs2RackQueryBufferSize, u32 rack_id, const void* option, ContextBufferInfo* info) {
    const int result = FillBufferInfo(info);
    HLE_TRACE("rack=0x%x -> hostBufferSize=0x%" PRIx64, rack_id, kHostBufferSize);
    return result;
}
HLE_OVERRIDE(sceNgs2RackQueryBufferSize);

int CreateRack(System* system, u32 rack_id, Rack** out) {
    if (!system || system->magic != kSystemMagic) {
        return kNgs2ErrorInvalidHandle;
    }
    if (!out) {
        return kNgs2ErrorInvalidArg;
    }
    auto* rack = new Rack;
    rack->system = system;
    rack->rack_id = rack_id;
    *out = rack;
    return err::OK;
}

HLE_IMPL(int, sceNgs2RackCreate, System* system, u32 rack_id, const void* option, const ContextBufferInfo* info,
         Rack** out) {
    const int result = CreateRack(system, rack_id, out);
    HLE_TRACE("rack=0x%x -> %s", rack_id, result ? "ERROR" : "OK");
    return result;
}
HLE_OVERRIDE(sceNgs2RackCreate);

HLE_IMPL(int, sceNgs2RackCreateWithAllocator, System* system, u32 rack_id, const void* option,
         const void* allocator, Rack** out) {
    const int result = CreateRack(system, rack_id, out);
    HLE_TRACE("rack=0x%x -> %s", rack_id, result ? "ERROR" : "OK");
    return result;
}
HLE_OVERRIDE(sceNgs2RackCreateWithAllocator);

HLE_IMPL(int, sceNgs2RackDestroy, Rack* rack, ContextBufferInfo* info) {
    HLE_TRACE("%p", static_cast<void*>(rack));
    if (!rack || rack->magic != kRackMagic) {
        return kNgs2ErrorInvalidHandle;
    }
    for (auto& [index, voice] : rack->voices) {
        voice->magic = 0;
        delete voice;
    }
    rack->magic = 0;
    delete rack;
    return err::OK;
}
HLE_OVERRIDE(sceNgs2RackDestroy);

HLE_IMPL(int, sceNgs2RackGetVoiceHandle, Rack* rack, u32 voice_index, Voice** out) {
    HLE_QUIET("%p, voice %u", static_cast<void*>(rack), voice_index);
    if (!rack || rack->magic != kRackMagic) {
        return kNgs2ErrorInvalidHandle;
    }
    if (!out) {
        return kNgs2ErrorInvalidArg;
    }
    std::lock_guard lock(rack->mutex);
    Voice*& voice = rack->voices[voice_index];
    if (!voice) {
        voice = new Voice;
        voice->rack = rack;
        voice->index = voice_index;
        std::lock_guard registry(g_registry_mutex);
        g_all_voices.push_back(voice);
    }
    *out = voice;
    return err::OK;
}
HLE_OVERRIDE(sceNgs2RackGetVoiceHandle);

std::mutex g_voice_mutex; // estado de todas as vozes (o jogo controla e consulta de threads diferentes)
bool g_voice_clock = true; // --ngs2-static: volta ao estado zerado das fases anteriores (controle A/B)
bool g_voice_log = true;
const auto g_audio_epoch = std::chrono::steady_clock::now();
struct VoiceStats {
    u64 queries = 0, plays = 0, stops = 0, finished = 0, blocks = 0, block_bytes = 0, samples_guessed = 0;
    u64 max_active = 0, underruns = 0, starved = 0, revived = 0, starved_out = 0, params_on_free = 0;
    std::map<u32, u64> params;  // id -> vezes
    std::map<u32, u64> events;  // eventId -> vezes
    std::map<u32, u64> formats; // tipo de waveform -> vezes
} g_voice_stats;
std::set<Voice*> g_active;

double AudioSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_audio_epoch).count();
}

template <typename T>
T ParamField(const u8* param, u32 offset) {
    T value;
    std::memcpy(&value, param + offset, sizeof(T));
    return value;
}

bool g_audio_enabled = true; // --no-audio
// Os racks de submix e masterizacao do jogo nao sao emulados (so o volume por voz); este ganho
// fixo faz o papel deles. O limitador (tanh) segura os picos.
constexpr float kMasterGain = 2.0f;
struct AudioStats {
    u64 blocks_decoded = 0, frames_decoded = 0, blocks_unreadable = 0, formats_unknown = 0;
    u64 renders = 0, frames_mixed = 0, voices_mixed_max = 0, matrix_params = 0, matrix_ignored = 0;
    double peak = 0.0, energy = 0.0;
    u64 device_buffers = 0, device_dropped = 0, device_underruns = 0;
    u64 vag_packets = 0, vag_extended = 0; // pacotes VAG; quantos pedem preditor fora da tabela classica (HEVAG)
    u64 limited_samples = 0;               // amostras que entraram na faixa do limitador
    bool device_open = false, device_failed = false;
} g_audio_stats;

// Copia memoria do jogo sem VirtualQuery (que leva centenas de microssegundos neste processo):
// endereco invalido devolve false em vez de derrubar. A leitura que pode falhar fica numa funcao
// separada, sem inline, para o __try do Clang pega-la.
__declspec(noinline) void RawGuestCopy(void* dst, const void* src, u64 bytes) {
    std::memcpy(dst, src, bytes);
}

__declspec(noinline) bool GuestCopyInner(void* dst, const void* src, u64 bytes) {
    __try {
        RawGuestCopy(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuestCopy(void* dst, const void* src, u64 bytes) {
    if (reinterpret_cast<u64>(src) < 0x10000 || bytes > (64u << 20)) {
        return false;
    }
    const runtime::GuardedReadScope guard;
    return GuestCopyInner(dst, src, bytes);
}

void ResetMix(Voice& voice) {
    voice.mix.clear();
    voice.mix_position = 0.0;
    voice.mix_exit_loop = false;
    std::memset(voice.vag_history, 0, sizeof(voice.vag_history));
    std::memset(voice.mix_prev, 0, sizeof(voice.mix_prev));
}

// Fase 30: gravacao da saida final em WAV (--audio-dump ARQUIVO), para medir a qualidade.
std::FILE* g_audio_dump = nullptr;
u64 g_audio_dump_frames = 0;

void AudioDumpWrite(const s16* stereo, u32 frames) {
    if (g_audio_dump) {
        std::fwrite(stereo, 4, frames, g_audio_dump);
        g_audio_dump_frames += frames;
    }
}

// Limitador: linear ate 0,75 (o som normal passa intacto) e joelho suave dali ate 1,0. Antes era
// tanh(2x) em tudo, que ja comprimia e distorcia em volume medio.
inline float SoftLimit(float x) {
    const float magnitude = std::fabs(x);
    if (magnitude <= 0.75f) {
        return x;
    }
    ++g_audio_stats.limited_samples;
    const float limited = 0.75f + 0.25f * std::tanh((magnitude - 0.75f) / 0.25f);
    return x < 0.0f ? -limited : limited;
}

// Ganhos padrao enquanto o jogo nao manda a matriz: mono no centro, estereo esquerda / direita.
void DefaultGains(Voice& voice) {
    for (u32 c = 0; c < 8; ++c) {
        voice.gain_left[c] = voice.gain_right[c] = 0.0f;
    }
    if (voice.channels <= 1) {
        voice.gain_left[0] = voice.gain_right[0] = 0.7071f;
    } else {
        for (u32 c = 0; c < std::min<u32>(voice.channels, 8); ++c) {
            (c & 1 ? voice.gain_right : voice.gain_left)[c] = voice.channels > 2 ? 0.5f : 1.0f;
        }
    }
    voice.port_volume = 1.0f;
}

// Decodifica um bloco de waveform do jogo para float intercalado.
bool DecodeBlock(Voice& voice, const u8* data, u64 size, MixBlock* out) {
    const u32 channels = std::clamp<u32>(voice.channels, 1, 8);
    switch (voice.waveform_type) {
    case 0x12: { // PCM de 16 bits, little endian, intercalado
        const u64 frames = size / (2ull * channels);
        out->pcm.resize(frames * channels);
        for (u64 i = 0; i < frames * channels; ++i) {
            s16 sample;
            std::memcpy(&sample, data + i * 2, 2);
            out->pcm[i] = static_cast<float>(sample) / 32768.0f;
        }
        out->frames = frames;
        return true;
    }
    case 0x18: { // float de 32 bits
        const u64 frames = size / (4ull * channels);
        out->pcm.resize(frames * channels);
        std::memcpy(out->pcm.data(), data, frames * channels * 4);
        out->frames = frames;
        return true;
    }
    case 0x1C: { // VAG do PS4 = HEVAG: quadros de 16 bytes -> 28 amostras; canais em quadros alternados
        // Fase 30: o jogo usa o VAG estendido (preditor de 7 bits, 4 taps). O decodificador antigo so
        // conhecia os 5 preditores classicos: 86% dos pacotes saiam com o preditor errado.
#include "hevag_table.inc"
        const u64 packets = size / 16;
        const u64 frames = packets / channels * 28;
        out->pcm.assign(frames * channels, 0.0f);
        for (u64 p = 0; p + channels <= packets; p += channels) {
            for (u32 c = 0; c < channels; ++c) {
                const u8* packet = data + (p + c) * 16;
                const u32 shift = packet[0] & 0x0F, flag = packet[1] & 0x0F;
                u32 predictor = (packet[0] >> 4) | (packet[1] & 0xF0);
                ++g_audio_stats.vag_packets;
                g_audio_stats.vag_extended += predictor > 4;
                if (predictor > 127) {
                    predictor = 0;
                }
                const float* k = kHevagCoefficients[predictor];
                float h1 = voice.vag_history[c][0], h2 = voice.vag_history[c][1], h3 = voice.vag_history[c][2], h4 = voice.vag_history[c][3];
                for (u32 n = 0; n < 28; ++n) {
                    float sample = 0.0f;
                    if (flag < 7) { // com flag 7 o quadro e silencio
                        const u8 byte = packet[2 + n / 2];
                        s32 nibble = (n & 1) ? (byte >> 4) : (byte & 0x0F);
                        if (nibble & 8) {
                            nibble -= 16;
                        }
                        const s32 code = (nibble * 4096) >> shift;
                        sample = std::trunc(static_cast<float>(code) + h1 * k[0] + h2 * k[1] + h3 * k[2] + h4 * k[3]);
                    }
                    h4 = h3;
                    h3 = h2;
                    h2 = h1;
                    h1 = sample; // o historico guarda o valor sem corte; o corte e so na saida
                    out->pcm[(p / channels * 28 + n) * channels + c] = std::clamp(sample, -32768.0f, 32767.0f) / 32768.0f;
                }
                voice.vag_history[c][0] = h1;
                voice.vag_history[c][1] = h2;
                voice.vag_history[c][2] = h3;
                voice.vag_history[c][3] = h4;
            }
        }
        out->frames = frames;
        return true;
    }
    default:
        return false;
    }
}

// Mistura `frames` quadros estereo de todas as vozes do sampler que estao tocando.
// Chamado com g_voice_mutex.
void MixVoices(const std::vector<Voice*>& voices, float* left, float* right, u32 frames, u32 output_rate) {
    u64 mixed = 0;
    for (Voice* pointer : voices) {
        Voice& voice = *pointer;
        if (!voice.mix_active || voice.paused || voice.mix.empty() || voice.rack->rack_id != 0x1000) {
            continue;
        }
        const double step = static_cast<double>(voice.sample_rate) * voice.pitch / output_rate;
        const u32 channels = std::clamp<u32>(voice.channels, 1, 8);
        bool any = false;
        for (u32 i = 0; i < frames; ++i) {
            while (!voice.mix.empty()) {
                MixBlock& block = voice.mix.front();
                if (block.frames && voice.mix_position < static_cast<double>(block.frames)) {
                    break;
                }
                if (block.frames) {
                    voice.mix_position -= static_cast<double>(block.frames);
                    ++block.done;
                }
                if (!block.frames || (block.passes ? block.done >= block.passes : voice.mix_exit_loop)) {
                    for (u32 c = 0; c < channels && block.frames; ++c) {
                        voice.mix_prev[c] = block.pcm[(block.frames - 1) * channels + c];
                    }
                    voice.mix.pop_front();
                    voice.mix_exit_loop = false;
                    if (voice.mix_position < 0.0 || voice.mix.empty()) {
                        voice.mix_position = std::max(0.0, voice.mix_position);
                    }
                }
            }
            if (voice.mix.empty()) {
                voice.mix_position = 0.0;
                break;
            }
            const MixBlock& block = voice.mix.front();
            const u64 index = static_cast<u64>(voice.mix_position);
            const float t = static_cast<float>(voice.mix_position - static_cast<double>(index));
            // Fase 30: interpolacao cubica (Hermite de 4 pontos) que atravessa a emenda entre blocos.
            // Antes era linear e repetia a ultima amostra de cada bloco: um degrau a cada emenda.
            const bool looping = !(block.passes ? block.done + 1 >= block.passes : voice.mix_exit_loop);
            const MixBlock* following = looping ? &block : voice.mix.size() > 1 ? &voice.mix[1] : nullptr;
            auto at = [&](s64 k, u32 c) -> float {
                if (k < 0) {
                    return block.done ? block.pcm[(block.frames - 1) * channels + c] : voice.mix_prev[c];
                }
                if (static_cast<u64>(k) < block.frames) {
                    return block.pcm[static_cast<u64>(k) * channels + c];
                }
                const u64 beyond = static_cast<u64>(k) - block.frames;
                if (following && beyond < following->frames) {
                    return following->pcm[beyond * channels + c];
                }
                return block.pcm[(block.frames - 1) * channels + c];
            };
            float l = 0.0f, r = 0.0f;
            for (u32 c = 0; c < channels; ++c) {
                const s64 k = static_cast<s64>(index);
                const float y0 = at(k - 1, c), y1 = at(k, c), y2 = at(k + 1, c), y3 = at(k + 2, c);
                const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                const float sample = ((c3 * t + c2) * t + c1) * t + y1;
                l += sample * voice.gain_left[c];
                r += sample * voice.gain_right[c];
            }
            left[i] += l * voice.port_volume;
            right[i] += r * voice.port_volume;
            voice.mix_position += step;
            any = true;
        }
        mixed += any;
    }
    g_audio_stats.voices_mixed_max = std::max(g_audio_stats.voices_mixed_max, mixed);
}

// ---- dispositivo de som: waveOut, 48 kHz estereo de 16 bits ---------------------------------
struct Device {
    static constexpr u32 kBuffers = 48;
    HWAVEOUT handle = nullptr;
    WAVEHDR headers[kBuffers]{};
    std::vector<s16> data[kBuffers];
    bool tried = false;

    bool Open(u32 rate) {
        if (handle || tried) {
            return handle != nullptr;
        }
        tried = true;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = rate * 4;
        if (waveOutOpen(&handle, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            handle = nullptr;
            g_audio_stats.device_failed = true;
            std::printf("[AUDIO] waveOutOpen failed: no audio device (game will run without sound)\n");
            return false;
        }
        g_audio_stats.device_open = true;
        std::printf("[AUDIO] audio device opened: %u Hz, stereo, 16-bit (waveOut)\n", rate);
        return true;
    }

    u32 Queued() const {
        u32 count = 0;
        for (const WAVEHDR& header : headers) {
            count += (header.dwFlags & WHDR_INQUEUE) != 0;
        }
        return count;
    }

    void Write(const s16* stereo, u32 frames) {
        if (!handle) {
            return;
        }
        const u32 queued = Queued();
        if (queued >= kBuffers - 10) { // so por seguranca: quem segura o ritmo e a espera em sceAudioOutOutput
            ++g_audio_stats.device_dropped;
            return;
        }
        u32 repeat = 1;
        if (queued == 0) { // fila seca: recomeca com uma folga de 8 buffers (~43 ms) para nao estalar de novo
            ++g_audio_stats.device_underruns;
            repeat = 9;
        }
        for (u32 n = 0; n < repeat; ++n) {
            for (u32 i = 0; i < kBuffers; ++i) {
                WAVEHDR& header = headers[i];
                if (header.dwFlags & WHDR_INQUEUE) {
                    continue;
                }
                if (header.dwFlags & WHDR_PREPARED) {
                    waveOutUnprepareHeader(handle, &header, sizeof(header));
                }
                data[i].assign(frames * 2, 0);
                if (n == repeat - 1) {
                    std::memcpy(data[i].data(), stereo, frames * 4);
                }
                header = {};
                header.lpData = reinterpret_cast<LPSTR>(data[i].data());
                header.dwBufferLength = frames * 4;
                waveOutPrepareHeader(handle, &header, sizeof(header));
                waveOutWrite(handle, &header, sizeof(header));
                ++g_audio_stats.device_buffers;
                break;
            }
        }
    }
} g_device;
std::mutex g_device_mutex;

// Amostras de um bloco quando o jogo nao informa (numSamples == 0): pelo formato.
u64 GuessSamples(const Voice& voice, u64 data_size) {
    const u32 channels = std::max<u32>(voice.channels, 1);
    switch (voice.waveform_type) {
    case 0x10: case 0x11: return data_size / channels;       // PCM 8 bits
    case 0x12: case 0x13: return data_size / (2 * channels); // PCM 16 bits
    case 0x14: case 0x15: return data_size / (3 * channels); // PCM 24 bits
    case 0x16: case 0x17: case 0x18: case 0x19: return data_size / (4 * channels); // PCM 32 / float
    case 0x1A: case 0x1B: return data_size / (8 * channels); // float 64
    case 0x1C: return data_size / (16 * channels) * 28;      // VAG: 16 bytes -> 28 amostras
    default: return data_size / (2 * channels);              // desconhecido: trata como PCM16
    }
}

// Quanto tempo uma voz alimentada aos poucos pode ficar com a fila vazia antes de terminar.
constexpr auto kStarveGrace = std::chrono::milliseconds(500);

u32 StateFlags(const Voice& voice) {
    if (!voice.in_use) {
        return 0;
    }
    u32 flags = kStateInUse;
    if (voice.playing) {
        flags |= voice.paused ? kStatePaused : kStatePlaying;
    }
    if (voice.rack->rack_id == 0x1000 && voice.blocks.empty()) {
        flags |= kStateEmpty;
    }
    return flags;
}

const char* FlagsText(u32 flags, char* buffer, size_t size) {
    if (!flags) {
        return "LIVRE/TERMINADA";
    }
    std::snprintf(buffer, size, "%s%s%s%s", (flags & kStateInUse) ? "INUSE" : "", (flags & kStatePlaying) ? "|PLAYING" : "",
                  (flags & kStatePaused) ? "|PAUSED" : "", (flags & kStateEmpty) ? "|EMPTY" : "");
    return buffer;
}

void VoiceLog(Voice& voice, const char* what) {
    const u32 flags = StateFlags(voice);
    voice.last_log = std::chrono::steady_clock::now();
    voice.last_logged_flags = flags;
    if (!g_voice_log) {
        return;
    }
    char text[48];
    const double played = voice.sample_rate ? static_cast<double>(voice.decoded_samples) / voice.sample_rate : 0.0;
    log::Tagged("NGS2_VOICE", "t=%.2fs rack 0x%x voice %u %s: state=0x%02x %s, played %.2fs (%" PRIu64 " samples, %" PRIu64
                " bytes; stream of %" PRIu64 " samples), queued blocks %zu, type 0x%x %u Hz x%u pitch %.2f",
                AudioSeconds(), voice.rack->rack_id, voice.index, what, flags, FlagsText(flags, text, sizeof(text)), played,
                voice.decoded_samples, voice.decoded_bytes, voice.total_samples, voice.blocks.size(), voice.waveform_type,
                voice.sample_rate,
                voice.channels, voice.pitch);
}

void Finish(Voice& voice, const char* why) {
    voice.in_use = voice.playing = voice.paused = false;
    voice.blocks.clear();
    g_active.erase(&voice);
    VoiceLog(voice, why);
}

// Avanca o relogio virtual da voz ate agora. Chamado com g_voice_mutex.
void Advance(Voice& voice) {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - voice.clock).count();
    voice.clock = now;
    if (!voice.in_use || !voice.playing || voice.paused || voice.rack->rack_id != 0x1000) {
        return;
    }
    const double exact = dt * voice.sample_rate * voice.pitch + voice.fraction;
    u64 budget = static_cast<u64>(exact);
    voice.fraction = exact - static_cast<double>(budget);
    bool consumed = false;
    while (budget && !voice.blocks.empty()) {
        WaveBlock& block = voice.blocks.front();
        if (!block.samples) {
            voice.blocks.pop_front();
            continue;
        }
        const u64 total = block.passes ? block.samples * block.passes : ~0ull;
        const u64 take = std::min(budget, total - block.position);
        const u64 bytes_before = block.data_size * (block.position % block.samples) / block.samples +
                                 block.data_size * (block.position / block.samples);
        block.position += take;
        const u64 bytes_after = block.data_size * (block.position % block.samples) / block.samples +
                                block.data_size * (block.position / block.samples);
        voice.decoded_samples += take;
        voice.decoded_bytes += bytes_after - bytes_before;
        budget -= take;
        consumed = true;
        if (block.position >= total) {
            voice.blocks.pop_front();
        }
    }
    if (voice.blocks.empty() && (consumed || budget)) {
        if (voice.total_samples && voice.decoded_samples < voice.total_samples) {
            // Stream: o jogo declarou mais amostras do que ja enviou. Faltou dado (underrun); a
            // voz continua tocando, vazia, e o relogio dela para ate chegar o proximo bloco.
            if (consumed) {
                ++g_voice_stats.underruns;
            }
            voice.fraction = 0.0;
            return;
        }
        // Voz alimentada aos poucos (o jogo ja mandou blocos mais de uma vez desde o SETUP):
        // a fila secar entre duas remessas nao e o fim do som. A voz continua PLAYING|EMPTY,
        // com o relogio parado, e so termina se nenhum bloco chegar dentro da tolerancia.
        if (voice.block_submissions >= 2) {
            voice.fraction = 0.0;
            if (!voice.starving) {
                voice.starving = true;
                voice.starve_since = now;
                ++g_voice_stats.starved;
                return;
            }
            if (now - voice.starve_since < kStarveGrace) {
                return;
            }
            ++g_voice_stats.starved_out;
        }
        ++g_voice_stats.finished;
        Finish(voice, voice.starving ? "END (queue empty beyond tolerance)" : "END OF DATA");
        voice.starving = false;
        voice.finished_dry = true;
    }
}

// Primeiras ocorrencias de cada parametro do sampler, em hexadecimal (para conferir o layout).
void DumpParam(const Voice& voice, const u8* param, u16 size, u32 id) {
    static std::map<u32, u32> shown;
    if (!g_voice_log || shown[id]++ >= 4) {
        return;
    }
    std::string hex;
    char item[12];
    for (u32 i = 0; i + 4 <= size && i < 0x40; i += 4) {
        std::snprintf(item, sizeof(item), "%08x ", ParamField<u32>(param, i));
        hex += item;
    }
    log::Tagged("NGS2_VOICE", "rack 0x%x voice %u param 0x%08x (%u bytes): %s", voice.rack->rack_id, voice.index, id, size, hex.c_str());
    if (id == kParamSamplerBlocks && size >= 0x20) {
        const u32 count = ParamField<u32>(param, 20);
        const u8* blocks = ParamField<const u8*>(param, 24);
        for (u32 b = 0; blocks && b < count && b < 4; ++b) {
            hex.clear();
            for (u32 i = 0; i < 0x20; i += 4) {
                std::snprintf(item, sizeof(item), "%08x ", ParamField<u32>(blocks + b * 0x20, i));
                hex += item;
            }
            log::Tagged("NGS2_VOICE", "    block %u: %s", b, hex.c_str());
        }
    }
}

void ApplyParam(Voice& voice, const u8* param, u16 size, u32 id) {
    ++g_voice_stats.params[id];
    if ((id >> 16) == 0x1000) {
        DumpParam(voice, param, size, id);
    }
    if (id == kParamEvent && size >= 0x0C) {
        const u32 event = ParamField<u32>(param, 8);
        ++g_voice_stats.events[event];
        switch (event) {
        case kEventPlay:
            voice.in_use = voice.playing = true;
            voice.paused = false;
            voice.starving = voice.finished_dry = false;
            voice.clock = voice.play_start = std::chrono::steady_clock::now();
            voice.mix_active = true;
            g_active.insert(&voice);
            g_voice_stats.max_active = std::max<u64>(g_voice_stats.max_active, g_active.size());
            ++g_voice_stats.plays;
            if (voice.rack->rack_id == 0x1000) {
                VoiceLog(voice, "PLAY");
            }
            break;
        case kEventStop:
        case kEventStopImm:
        case kEventKill:
            ++g_voice_stats.stops;
            voice.starving = voice.finished_dry = false;
            voice.mix_active = false; // STOP corta o som; "fim dos dados" do relogio virtual nao
            ResetMix(voice);
            if (voice.in_use) {
                if (voice.rack->rack_id == 0x1000) {
                    Finish(voice, event == kEventStop ? "STOP" : event == kEventStopImm ? "STOP_IMM" : "KILL");
                } else {
                    voice.in_use = voice.playing = false;
                    g_active.erase(&voice);
                }
            }
            break;
        case kEventPause:
            voice.paused = true;
            VoiceLog(voice, "PAUSE");
            break;
        case kEventResume:
            voice.paused = false;
            VoiceLog(voice, "RESUME");
            break;
        default:
            log::Tagged("NGS2_VOICE", "rack 0x%x voice %u: UNKNOWN event %u", voice.rack->rack_id, voice.index, event);
            break;
        }
    } else if (id == kParamSamplerSetup && size >= 0x20) {
        voice.waveform_type = ParamField<u32>(param, 8);
        voice.channels = std::max<u32>(ParamField<u32>(param, 12), 1);
        const u32 rate = ParamField<u32>(param, 16);
        voice.sample_rate = (rate >= 4000 && rate <= 192000) ? rate : 48000;
        ++g_voice_stats.formats[voice.waveform_type];
        voice.in_use = true;
        voice.playing = voice.paused = false;
        voice.pitch = 1.0f;
        voice.blocks.clear();
        voice.decoded_samples = voice.decoded_bytes = 0;
        voice.total_samples = 0;
        voice.fraction = 0.0;
        voice.block_submissions = 0;
        voice.starving = voice.finished_dry = false;
        voice.mix_active = false;
        ResetMix(voice);
        DefaultGains(voice);
    } else if (id == 0x10000004) { // EXIT_LOOP do sampler
        voice.mix_exit_loop = true;
    } else if (id == kParamSamplerBlocks && size >= 0x20) {
        const u32 count = ParamField<u32>(param, 20);
        const u8* blocks = ParamField<const u8*>(param, 24);
        const u8* wave_data = ParamField<const u8*>(param, 8);
        for (u32 i = 0; blocks && i < count && i < 64; ++i) {
            // {dataOffset, dataSize, numRepeats, numSkipSamples, numSamples, reserved, userData}
            const u8* b = blocks + i * 0x20;
            WaveBlock block;
            block.data_size = ParamField<u32>(b, 4);
            const u32 repeats = ParamField<u32>(b, 8);
            block.passes = repeats >= 0x7FFFFFFF ? 0 : static_cast<u64>(repeats) + 1;
            // numSamples: em som curto e o tamanho do bloco; em stream o jogo poe ali o TOTAL do
            // stream (ex.: 0x7916fd para blocos de 16 KB), entao o bloco vale o que cabe nos bytes.
            const u64 declared = ParamField<u32>(b, 16);
            const u64 by_size = GuessSamples(voice, block.data_size);
            if (declared && (declared <= by_size || !by_size)) {
                block.samples = declared;
            } else {
                block.samples = by_size;
                ++g_voice_stats.samples_guessed;
                voice.total_samples = std::max(voice.total_samples, declared);
            }
            ++g_voice_stats.blocks;
            g_voice_stats.block_bytes += block.data_size;
            voice.blocks.push_back(block);
            if (g_audio_enabled && voice.rack->rack_id == 0x1000 && block.data_size && block.data_size <= (64u << 20)) {
                static thread_local std::vector<u8> copy;
                copy.resize(block.data_size);
                const u8* bytes = copy.data();
                MixBlock mix;
                mix.passes = block.passes;
                if (!wave_data || !GuestCopy(copy.data(), wave_data + ParamField<u32>(b, 0), block.data_size)) {
                    ++g_audio_stats.blocks_unreadable;
                } else if (!DecodeBlock(voice, bytes, block.data_size, &mix)) {
                    ++g_audio_stats.formats_unknown;
                } else if (mix.frames) {
                    ++g_audio_stats.blocks_decoded;
                    g_audio_stats.frames_decoded += mix.frames;
                    voice.mix.push_back(std::move(mix));
                    if (voice.finished_dry) {
                        voice.mix_active = true;
                    }
                }
            }
        }
        voice.in_use = true;
        ++voice.block_submissions;
        voice.last_block = std::chrono::steady_clock::now();
        if (voice.starving && !voice.blocks.empty()) {
            voice.starving = false; // chegou dado: o relogio volta a andar daqui
            voice.clock = voice.last_block;
        }
        if (voice.finished_dry && !voice.blocks.empty()) {
            // A voz tinha terminado por falta de dados e o jogo continuou alimentando o mesmo
            // handle, sem novo SETUP nem PLAY: ela volta a tocar.
            voice.finished_dry = false;
            voice.playing = true;
            voice.paused = false;
            voice.clock = voice.last_block;
            g_active.insert(&voice);
            ++g_voice_stats.revived;
            VoiceLog(voice, "RESUMED (new blocks after queue ran dry)");
        }
    } else if (id == kParamSamplerPitch && size >= 0x0C) {
        const float ratio = ParamField<float>(param, 8);
        if (std::isfinite(ratio) && ratio > 0.01f && ratio < 16.0f) {
            voice.pitch = ratio;
        }
    } else if (id == 0x01 && size >= 0x18 && voice.rack->rack_id == 0x1000) {
        // MATRIX_LEVELS {u32 matrixId; u32 numLevels; const float* aLevel}: aLevel[saida * entradas +
        // entrada], 8 saidas (FL FR FC LFE RL RR SL SR). Conferido no log: um stream 5.1 manda 48
        // niveis com a diagonal em 0, 7, 14... Vira ganho estereo (centro e surround a -3 dB).
        const u32 count = ParamField<u32>(param, 12);
        float levels[64];
        const u32 in_channels = std::clamp<u32>(voice.channels, 1, 8);
        bool ok = count >= in_channels * 2 && count <= 64 && count % in_channels == 0 &&
                  GuestCopy(levels, ParamField<const float*>(param, 16), count * 4ull);
        for (u32 i = 0; ok && i < count; ++i) {
            ok = std::isfinite(levels[i]) && levels[i] >= -8.0f && levels[i] <= 8.0f;
        }
        if (ok) {
            const u32 out_channels = count / in_channels;
            for (u32 c = 0; c < in_channels; ++c) {
                auto at = [&](u32 out) { return out < out_channels ? levels[out * in_channels + c] : 0.0f; };
                voice.gain_left[c] = at(0) + 0.7071f * (at(2) + at(4) + at(6)) + 0.5f * at(3);
                voice.gain_right[c] = at(1) + 0.7071f * (at(2) + at(5) + at(7)) + 0.5f * at(3);
            }
            static u32 shown = 0;
            if (shown < 10) {
                ++shown;
                std::printf("[AUDIO] volume matrix: voice %u, %u input x %u output channels, id %u:", voice.index, in_channels,
                            out_channels, ParamField<u32>(param, 8));
                for (u32 i = 0; i < count && i < 16; ++i) {
                    std::printf(" %.3f", levels[i]);
                }
                std::printf(" -> E %.3f %.3f | D %.3f %.3f\n", voice.gain_left[0], voice.gain_left[1], voice.gain_right[0], voice.gain_right[1]);
            }
            ++g_audio_stats.matrix_params;
        } else {
            ++g_audio_stats.matrix_ignored;
        }
        if (!voice.in_use) {
            ++g_voice_stats.params_on_free;
        }
    } else if (id == 0x02 && size >= 0x10 && voice.rack->rack_id == 0x1000) {
        // PORT_VOLUME {u32 port; float level}
        const float level = ParamField<float>(param, 12);
        if (ParamField<u32>(param, 8) == 0 && std::isfinite(level) && level >= 0.0f && level <= 8.0f) {
            voice.port_volume = level;
        }
        if (!voice.in_use) {
            ++g_voice_stats.params_on_free;
        }
    } else if (id == kParamPatch || (id >> 16) != 0x1000) {
        // Parametros genericos (patch, volume, matriz). Em mixers / masterizacao a voz "so existe
        // e toca". Numa voz do SAMPLER eles nao podem ressuscitar o INUSE: o jogo ajusta volume
        // de vozes que ja terminaram, e uma voz terminada que volta a aparecer como INUSE|EMPTY
        // (sem PLAYING) nunca mais e dada como livre - foi o que o watchdog de cutscene mostrou
        // na Fase 13 (7 de 10 vozes presas assim, consultadas a cada frame).
        if (voice.rack->rack_id != 0x1000) {
            voice.in_use = true;
        } else if (!voice.in_use) {
            ++g_voice_stats.params_on_free;
        }
    }
}

HLE_IMPL(int, sceNgs2VoiceControl, Voice* voice, const void* param_list) {
    HLE_QUIET("%p", static_cast<void*>(voice));
    static perf::Counter perf_counter("Ngs2 VoiceControl");
    const perf::Scope perf_scope(perf_counter);
    if (!voice || voice->magic != kVoiceMagic) {
        return kNgs2ErrorInvalidHandle;
    }
    std::lock_guard lock(g_voice_mutex);
    Advance(*voice);
    const u8* param = static_cast<const u8*>(param_list);
    for (int guard = 0; param && guard < 64; ++guard) {
        const u16 size = ParamField<u16>(param, 0);
        const s16 next = ParamField<s16>(param, 2);
        ApplyParam(*voice, param, size, ParamField<u32>(param, 4));
        if (!next) {
            break;
        }
        param += next;
    }
    return err::OK;
}
HLE_OVERRIDE(sceNgs2VoiceControl);

HLE_IMPL(int, sceNgs2VoiceGetState, Voice* voice, void* state, u64 state_size) {
    HLE_QUIET("%p, %" PRIu64 " bytes", static_cast<void*>(voice), state_size);
    static perf::Counter perf_counter("Ngs2 VoiceGetState (voice clock)");
    const perf::Scope perf_scope(perf_counter);
    if (!voice || voice->magic != kVoiceMagic) {
        return kNgs2ErrorInvalidHandle;
    }
    if (!state || !state_size) {
        return err::OK;
    }
    std::memset(state, 0, state_size);
    if (!g_voice_clock) {
        return err::OK; // comportamento antigo: tudo zerado
    }
    std::lock_guard lock(g_voice_mutex);
    ++g_voice_stats.queries;
    ++voice->queries;
    voice->last_query = std::chrono::steady_clock::now();
    Advance(*voice);
    SamplerState out{};
    out.state_flags = StateFlags(*voice);
    out.num_decoded_samples = voice->decoded_samples;
    out.decoded_data_size = voice->decoded_bytes;
    std::memcpy(state, &out, std::min<u64>(state_size, sizeof(out)));
    // Log por consulta seria uma linha por voz por frame: sai quando o estado muda e, enquanto
    // a voz toca, no maximo uma vez por segundo.
    const bool changed = out.state_flags != voice->last_logged_flags;
    if ((changed && (out.state_flags || voice->last_logged_flags != 0xFFFFFFFF)) ||
        (voice->playing && std::chrono::steady_clock::now() - voice->last_log > std::chrono::seconds(1))) {
        VoiceLog(*voice, "consulta");
    }
    voice->last_logged_flags = out.state_flags;
    return err::OK;
}
HLE_OVERRIDE(sceNgs2VoiceGetState);

HLE_IMPL(int, sceNgs2SystemRender, System* system, const RenderBufferInfo* buffers, u32 buffer_count) {
    HLE_QUIET("%p, %u buffers", static_cast<void*>(system), buffer_count);
    static perf::Counter perf_counter("Ngs2 SystemRender");
    const perf::Scope perf_scope(perf_counter);
    if (!system || system->magic != kSystemMagic) {
        return kNgs2ErrorInvalidHandle;
    }
    for (u32 i = 0; buffers && i < buffer_count; ++i) {
        if (buffers[i].buffer && buffers[i].buffer_size) {
            std::memset(buffers[i].buffer, 0, buffers[i].buffer_size); // silencio
        }
    }
    // Fase 24: mistura as vozes no primeiro buffer (float de 32 bits ou PCM de 16, N canais; o
    // som vai nos dois primeiros canais, esquerda e direita).
    if (g_audio_enabled && buffers && buffer_count && buffers[0].buffer && buffers[0].num_channels >= 1 && buffers[0].num_channels <= 8 &&
        (buffers[0].waveform_type == 0x18 || buffers[0].waveform_type == 0x12)) {
        const u32 channels = buffers[0].num_channels;
        const u32 sample_bytes = buffers[0].waveform_type == 0x18 ? 4 : 2;
        const u32 frames = static_cast<u32>(std::min<u64>(buffers[0].buffer_size / (sample_bytes * channels), 4096));
        static thread_local std::vector<float> left, right;
        left.assign(frames, 0.0f);
        right.assign(frames, 0.0f);
        std::vector<Voice*> voices;
        {
            std::lock_guard registry(g_registry_mutex);
            voices = g_all_voices;
        }
        {
            std::lock_guard lock(g_voice_mutex);
            MixVoices(voices, left.data(), right.data(), frames, 48000);
            ++g_audio_stats.renders;
            g_audio_stats.frames_mixed += frames;
            for (u32 i = 0; i < frames; ++i) {
                // ganho geral + limitador suave (varias vozes somadas passam de 1,0)
                left[i] = SoftLimit(left[i] * kMasterGain);
                right[i] = SoftLimit(right[i] * kMasterGain);
                g_audio_stats.peak = std::max<double>(g_audio_stats.peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
                g_audio_stats.energy += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
            }
        }
        for (u32 i = 0; i < frames; ++i) {
            const float l = left[i], r = channels >= 2 ? right[i] : 0.0f;
            if (sample_bytes == 4) {
                float* out = static_cast<float*>(buffers[0].buffer) + static_cast<size_t>(i) * channels;
                out[0] = channels >= 2 ? l : (left[i] + right[i]) * 0.5f;
                if (channels >= 2) {
                    out[1] = r;
                }
            } else {
                s16* out = static_cast<s16*>(buffers[0].buffer) + static_cast<size_t>(i) * channels;
                out[0] = static_cast<s16>((channels >= 2 ? l : (left[i] + right[i]) * 0.5f) * 32767.0f);
                if (channels >= 2) {
                    out[1] = static_cast<s16>(r * 32767.0f);
                }
            }
        }
    }
    return err::OK;
}
HLE_OVERRIDE(sceNgs2SystemRender);

// ================================ AudioOut ==================================
struct Port {
    u32 samples;
    u32 frequency;
    std::chrono::steady_clock::time_point next_deadline;
    u32 format = 5; // 0 / 1 / 2: PCM16 mono / estereo / 8 canais; 3 / 4 / 5: float; 6 / 7: 8 canais "std"
};

std::mutex g_ports_mutex;
std::map<int, Port> g_ports;
int g_next_port = 1;

HLE_IMPL(int, sceAudioOutInit) {
    HLE_TRACE("");
    return err::OK;
}
HLE_OVERRIDE(sceAudioOutInit);

HLE_IMPL(int, sceAudioOutOpen, int user_id, int type, int index, u32 samples, u32 frequency, u32 format) {
    int handle;
    {
        std::lock_guard lock(g_ports_mutex);
        handle = g_next_port++;
        g_ports[handle] = {samples ? samples : 256, frequency ? frequency : 48000, std::chrono::steady_clock::now(), format & 0xFF};
    }
    HLE_TRACE("user=%d type=%d index=%d samples=%u freq=%u format=%u -> port %d", user_id, type, index, samples,
              frequency, format, handle);
    if (g_audio_enabled) { // abrir aqui, e nao no primeiro Output: waveOutOpen leva dezenas de ms
        std::lock_guard device(g_device_mutex);
        g_device.Open(frequency ? frequency : 48000);
    }
    return handle;
}
HLE_OVERRIDE(sceAudioOutOpen);

// As amostras vao para o dispositivo de som (Fase 24) e a chamada bloqueia pelo tempo que o buffer
// leva para tocar (256 amostras a 48 kHz = 5,33 ms). O prazo e absoluto, para a thread de som
// manter o ritmo real mesmo com o jitter do Sleep; a fila do dispositivo absorve a diferenca.
HLE_IMPL(int, sceAudioOutOutput, int handle, const void* buffer) {
    HLE_QUIET("port %d, %p", handle, buffer);
    std::chrono::steady_clock::time_point deadline;
    u32 samples;
    {
        std::lock_guard lock(g_ports_mutex);
        const auto it = g_ports.find(handle);
        if (it == g_ports.end()) {
            return kAudioOutErrorInvalidPort;
        }
        Port& port = it->second;
        const auto now = std::chrono::steady_clock::now();
        const auto period = std::chrono::nanoseconds(1000000000LL * port.samples / port.frequency);
        if (port.next_deadline + period * 4 < now) {
            port.next_deadline = now; // a thread ficou para tras: nao tenta "recuperar" em rajada
        }
        port.next_deadline += period;
        deadline = port.next_deadline;
        samples = port.samples;
        if (g_audio_enabled && buffer && samples <= 4096) {
            static const u32 kChannels[8] = {1, 2, 8, 1, 2, 8, 8, 8};
            const u32 format = port.format & 7, channels = kChannels[format];
            const bool is_float = format == 3 || format == 4 || format == 5 || format == 7;
            static thread_local std::vector<s16> stereo;
            static thread_local std::vector<u8> guest;
            stereo.resize(samples * 2);
            guest.resize(static_cast<size_t>(samples) * channels * (is_float ? 4 : 2));
            if (GuestCopy(guest.data(), buffer, guest.size())) {
                buffer = guest.data();
                for (u32 i = 0; i < samples; ++i) {
                    float l, r;
                    if (is_float) {
                        const float* in = static_cast<const float*>(buffer) + static_cast<size_t>(i) * channels;
                        l = in[0];
                        r = channels >= 2 ? in[1] : in[0];
                    } else {
                        const s16* in = static_cast<const s16*>(buffer) + static_cast<size_t>(i) * channels;
                        l = in[0] / 32768.0f;
                        r = channels >= 2 ? in[1] / 32768.0f : l;
                    }
                    stereo[i * 2] = static_cast<s16>(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
                    stereo[i * 2 + 1] = static_cast<s16>(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
                }
                std::lock_guard device(g_device_mutex);
                AudioDumpWrite(stereo.data(), samples);
                g_device.Write(stereo.data(), samples); // (o dispositivo e aberto em sceAudioOutOpen)
            }
        }
    }
    runtime::WaitScope waiting("sceAudioOutOutput");
    std::this_thread::sleep_until(deadline);
    // Fase 30: o relogio do jogo e o da placa de som nao andam iguais. Antes, quando a fila passava
    // de 20 buffers, o buffer era jogado fora (148 cortes de 5 ms numa sessao de 6 minutos). Agora a
    // thread de audio do jogo espera a placa consumir, como no console.
    for (u32 waited = 0; waited < 200; ++waited) {
        {
            std::lock_guard device(g_device_mutex);
            if (!g_device.handle || g_device.Queued() <= 12) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return static_cast<int>(samples);
}
HLE_OVERRIDE(sceAudioOutOutput);

} // namespace

namespace hle {

void SetNgs2VoiceClock(bool enabled) {
    g_voice_clock = enabled;
}

void SetNgs2VoiceLog(bool enabled) {
    g_voice_log = enabled;
}

void SetAudioDump(const char* path) {
    g_audio_dump = std::fopen(path, "wb");
    if (g_audio_dump) {
        const u8 header[44] = {}; // preenchido em FinishAudioDump
        std::fwrite(header, 1, sizeof(header), g_audio_dump);
    }
}

void FinishAudioDump() {
    if (!g_audio_dump) {
        return;
    }
    std::lock_guard device(g_device_mutex);
    const u32 data_bytes = static_cast<u32>(g_audio_dump_frames * 4);
    u8 header[44];
    auto put32 = [&](u32 offset, u32 value) { std::memcpy(header + offset, &value, 4); };
    auto put16 = [&](u32 offset, u16 value) { std::memcpy(header + offset, &value, 2); };
    std::memcpy(header, "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, 48000);
    put32(28, 48000 * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(header + 36, "data", 4);
    put32(40, data_bytes);
    std::fseek(g_audio_dump, 0, SEEK_SET);
    std::fwrite(header, 1, sizeof(header), g_audio_dump);
    std::fclose(g_audio_dump);
    g_audio_dump = nullptr;
}

void SetAudioOutput(bool enabled) {
    g_audio_enabled = enabled;
}

void PrintAudioStats() {
    FinishAudioDump();
    std::printf("  audio (Phase 30): VAG %llu packets, %llu with predictor outside classic table (HEVAG) | %llu samples in limiter range\n",
                static_cast<unsigned long long>(g_audio_stats.vag_packets), static_cast<unsigned long long>(g_audio_stats.vag_extended),
                static_cast<unsigned long long>(g_audio_stats.limited_samples));
    const AudioStats& s = g_audio_stats;
    const double seconds = s.frames_mixed / 48000.0;
    const double rms = s.frames_mixed ? std::sqrt(s.energy / (2.0 * s.frames_mixed)) : 0.0;
    std::printf("  audio (Phase 24): %s | device %s, %" PRIu64 " buffers submitted, %" PRIu64 " discarded (queue full), %" PRIu64
                " restarts (queue empty)\n",
                g_audio_enabled ? "enabled" : "disabled (--no-audio)",
                s.device_open ? "opened" : s.device_failed ? "FAILED to open" : "not opened", s.device_buffers, s.device_dropped,
                s.device_underruns);
    std::printf("    decoded blocks %" PRIu64 " (%.1f s of audio), unreadable %" PRIu64 ", unsupported format %" PRIu64
                " | mixing: %.1f s in %" PRIu64 " calls, up to %" PRIu64 " concurrent voices, peak %.3f, RMS %.4f | volume matrix: %" PRIu64
                " applied, %" PRIu64 " ignored\n",
                s.blocks_decoded, s.frames_decoded / 48000.0, s.blocks_unreadable, s.formats_unknown, seconds, s.renders, s.voices_mixed_max,
                s.peak, rms, s.matrix_params, s.matrix_ignored);
}

// Relatorio do watchdog de cutscene: todas as vozes alocadas e o que cada uma esta fazendo.
void PrintNgs2VoiceReport() {
    std::vector<Voice*> voices;
    {
        std::lock_guard registry(g_registry_mutex);
        voices = g_all_voices;
    }
    std::lock_guard lock(g_voice_mutex);
    const auto now = std::chrono::steady_clock::now();
    auto ago = [&](std::chrono::steady_clock::time_point t) {
        return t.time_since_epoch().count() ? std::chrono::duration<double>(now - t).count() : -1.0;
    };
    std::map<u32, std::array<u32, 3>> racks; // rack -> {alocadas, em uso, tocando}
    u32 playing = 0;
    for (const Voice* v : voices) {
        auto& r = racks[v->rack->rack_id];
        ++r[0];
        r[1] += v->in_use;
        r[2] += v->in_use && v->playing && !v->paused;
        playing += v->rack->rack_id == 0x1000 && v->in_use && v->playing;
    }
    std::printf("---- Ngs2: %zu voices allocated;", voices.size());
    for (const auto& [id, r] : racks) {
        std::printf("  rack 0x%x: %u allocated, %u in use, %u playing;", id, r[0], r[1], r[2]);
    }
    std::printf("\n     sampler voices (rack 0x1000) in use: (all others are FREE/FINISHED)\n");
    u32 shown = 0;
    for (const Voice* v : voices) {
        if (v->rack->rack_id != 0x1000 || !v->in_use) {
            continue;
        }
        if (++shown > 64) {
            std::printf("     ... (more voices omitted)\n");
            break;
        }
        char text[48];
        const u32 flags = StateFlags(*v);
        const double played = v->sample_rate ? static_cast<double>(v->decoded_samples) / v->sample_rate : 0.0;
        const double total = v->sample_rate ? static_cast<double>(v->total_samples) / v->sample_rate : 0.0;
        std::printf("     voice %3u  %-22s %s for %.1fs; decoded %.2fs", v->index, FlagsText(flags, text, sizeof(text)),
                    v->playing ? (v->paused ? "PAUSED, PLAY" : "PLAY") : "no PLAY, last PLAY", ago(v->play_start), played);
        if (v->total_samples) {
            std::printf(" of %.2fs (stream)", total);
        }
        std::printf("; queue %zu block(s)%s; %u batch(es), last %.1fs ago; last query %.1fs ago (%llu); type 0x%x %u Hz x%u\n",
                    v->blocks.size(), v->starving ? " [QUEUE EMPTY, waiting for data]" : "", v->block_submissions, ago(v->last_block),
                    ago(v->last_query), static_cast<unsigned long long>(v->queries), v->waveform_type, v->sample_rate, v->channels);
    }
    if (!shown) {
        std::printf("     (none: no sampler voice in use)\n");
    }
    std::printf("     total: %u sampler voice(s) with PLAY active\n", playing);
}

void PrintNgs2Stats() {
    PrintAudioStats();
    // Fase 70b: no fechamento pela janela outra thread pode estar presa com o mutex das vozes; esperar por ele
    // travava a saida (o processo ficava aberto). Tenta por ate 1 s e, sem ele, pula este resumo.
    std::unique_lock lock(g_voice_mutex, std::defer_lock);
    for (int i = 0; i < 100 && !lock.try_lock(); ++i) {
        Sleep(10);
    }
    if (!lock.owns_lock()) {
        std::printf("Ngs2 voices: (summary skipped: voice state busy at shutdown)\n");
        return;
    }
    const VoiceStats& st = g_voice_stats;
    std::printf("Ngs2 voices: virtual clock %s; %llu state queries, %llu PLAY, %llu STOP, %llu finished automatically, "
                "peak %llu active; %llu blocks (%.1f MiB), %llu sized by bytes, %llu stream underruns\n",
                g_voice_clock ? "ENABLED" : "disabled (--ngs2-static)", static_cast<unsigned long long>(st.queries),
                static_cast<unsigned long long>(st.plays), static_cast<unsigned long long>(st.stops),
                static_cast<unsigned long long>(st.finished), static_cast<unsigned long long>(st.max_active),
                static_cast<unsigned long long>(st.blocks), st.block_bytes / 1048576.0,
                static_cast<unsigned long long>(st.samples_guessed), static_cast<unsigned long long>(st.underruns));
    std::printf("             continuous feed: %llu time(s) the queue ran dry with the voice kept PLAYING, %llu ended after exceeding "
                "the 0.5 s tolerance, %llu resumed after new blocks arrived after ending\n",
                static_cast<unsigned long long>(st.starved), static_cast<unsigned long long>(st.starved_out),
                static_cast<unsigned long long>(st.revived));
    std::printf("             %llu generic parameter(s) received by already-finished sampler voices (ignored: do not return to INUSE)\n",
                static_cast<unsigned long long>(st.params_on_free));
    std::printf("             parameters (id=count):");
    for (const auto& [id, count] : st.params) {
        std::printf(" 0x%x=%llu", id, static_cast<unsigned long long>(count));
    }
    std::printf("\n             events (id=count):");
    for (const auto& [id, count] : st.events) {
        std::printf(" %u=%llu", id, static_cast<unsigned long long>(count));
    }
    std::printf("\n             waveform formats (type=voices):");
    for (const auto& [id, count] : st.formats) {
        std::printf(" 0x%x=%llu", id, static_cast<unsigned long long>(count));
    }
    std::printf("\n");
}

} // namespace hle
