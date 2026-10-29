#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <objbase.h>
#include <objidl.h>
#include <propidl.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "common/types.h"
#include "hle/hle_log.h"
#include "hle/overrides/hle_filesystem.h"
#include "hle/overrides/hle_override.h"
#include "hle/overrides/hle_radar.h"

// libSceAvPlayer: o unico video do jogo e o episodio de Teen Titans Go! do pacote da DLC (opus_teentitansgo\movies\
// xboxone_ps4\teentitansgo_edit_mute 10000br.mp4, H.264 sem audio: o audio e um som do proprio jogo). O jogo so passa
// "myfile.mp4" ao AddSource e le o arquivo pelas proprias funcoes de leitura; aqui o .mp4 e achado pelo indice do
// radar e lido direto do pacote no disco (guardado sem compressao), decodificado pelo Media Foundation do Windows.
//
// O que o jogo faz com o player (eboot 1.24):
// - eboot+0x843150 chama sceAvPlayerStart e espera em "while (!IsActive()) usleep(1000)".
// - eboot+0x8434b0 (a cada quadro): GetVideoData -> copia o NV12 (plano Y, depois UV, com a largura / altura do
//   quadro) para as texturas dele; sem quadro e IsActive = 0 -> o video terminou. Ele guarda ate 6 quadros e mostra
//   cada um no horario (ms desde o Start).
// - O callback de eventos (eboot+0x842010) so registra PLAY / STOP / PAUSE; o READY configura os streams.
// Sem o .mp4 (ou sem decodificador) o jogo ve um video que comeca e termina sem quadros.
namespace {

using namespace hle;

using CallbackFn = void (HLE_SYSV_ABI*)(void* user_data, int event_id, int source_id, void* event_data);

constexpr int kStateStop = 1;
constexpr int kStateReady = 2;
constexpr int kStatePlay = 3;

struct Frame {
    std::vector<u8> nv12;
    u64 timestamp_ms = 0;
};

u32 AlignUp(u32 value, u32 align) {
    return (value + align - 1) / align * align;
}

// IStream sobre um trecho de um arquivo (o .mp4 dentro do pacote .DAT2); o Media Foundation le por ele.
class RangeStream final : public IStream {
public:
    RangeStream(HANDLE file, u64 base, u64 size) : file_(file), base_(base), size_(size) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IStream) || riid == __uuidof(ISequentialStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) {
            CloseHandle(file_);
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override {
        const u64 left = pos_ < size_ ? size_ - pos_ : 0;
        const ULONG want = static_cast<ULONG>(std::min<u64>(size, left));
        OVERLAPPED ov{};
        const u64 at = base_ + pos_;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (want && !ReadFile(file_, data, want, &got, &ov)) {
            return STG_E_READFAULT;
        }
        pos_ += got;
        if (read) {
            *read = got;
        }
        return got == size ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* new_pos) override {
        s64 base = 0;
        if (origin == STREAM_SEEK_CUR) {
            base = static_cast<s64>(pos_);
        } else if (origin == STREAM_SEEK_END) {
            base = static_cast<s64>(size_);
        }
        const s64 target = base + move.QuadPart;
        if (target < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        pos_ = static_cast<u64>(target);
        if (new_pos) {
            new_pos->QuadPart = pos_;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        std::memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = size_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    HANDLE file_;
    u64 base_;
    u64 size_;
    u64 pos_ = 0;
    std::atomic<ULONG> refs_{1};
};

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Decodifica o video numa thread propria e guarda ate kQueue quadros NV12 (largura / altura alinhadas a 16, pitch a
// 64, como o shadPS4 entrega).
class Decoder {
public:
    static constexpr size_t kQueue = 8;

    bool Open(const std::filesystem::path& pack, u64 offset, u64 size) {
        thread_ = std::thread([this, pack, offset, size] { Run(pack, offset, size); });
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return opened_ || finished_; });
        return opened_;
    }

    ~Decoder() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    u32 width = 0, height = 0, pitch = 0; // do quadro entregue ao jogo
    u64 duration_ms = 0;

    // Proximo quadro, se ja estiver na hora dele.
    bool Pop(u64 now_ms, Frame* out) {
        std::lock_guard lock(mutex_);
        if (queue_.empty() || queue_.front().timestamp_ms > now_ms) {
            return false;
        }
        *out = std::move(queue_.front());
        queue_.pop_front();
        cv_.notify_all();
        return true;
    }

    bool Done() {
        std::lock_guard lock(mutex_);
        return finished_ && queue_.empty();
    }

    u64 frames = 0;

private:
    void Run(const std::filesystem::path& pack, u64 offset, u64 size) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        IMFSourceReader* reader = OpenReader(pack, offset, size);
        u32 src_w = 0, src_h = 0;
        s32 src_stride = 0;
        if (reader) {
            ReadType(reader, &src_w, &src_h, &src_stride); // antes de liberar o Open: o GetStreamInfo usa o tamanho
        }
        {
            std::lock_guard lock(mutex_);
            opened_ = reader != nullptr;
            finished_ = !reader;
        }
        cv_.notify_all();
        while (reader) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || queue_.size() < kQueue; });
                if (quit_) {
                    break;
                }
            }
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample))) {
                log::Tagged("AVPLAYER", "Media Foundation: ReadSample failed, video stops here");
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                ReadType(reader, &src_w, &src_h, &src_stride);
            }
            if (sample) {
                Frame frame;
                if (CopySample(sample, src_w, src_h, src_stride, &frame)) {
                    frame.timestamp_ms = ts > 0 ? static_cast<u64>(ts / 10000) : 0;
                    std::lock_guard lock(mutex_);
                    queue_.push_back(std::move(frame));
                    ++frames;
                }
                sample->Release();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                break;
            }
        }
        SafeRelease(reader);
        MFShutdown();
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }
        std::lock_guard lock(mutex_);
        finished_ = true;
    }

    IMFSourceReader* OpenReader(const std::filesystem::path& pack, u64 offset, u64 size) {
        const HANDLE file = CreateFileW(pack.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            log::Tagged("AVPLAYER", "could not open %s", pack.string().c_str());
            return nullptr;
        }
        auto* stream = new RangeStream(file, offset, size);
        IMFByteStream* bytes = nullptr;
        IMFAttributes* bytes_attrs = nullptr;
        IMFSourceReader* reader = nullptr;
        IMFMediaType* type = nullptr;
        HRESULT hr = MFCreateMFByteStreamOnStream(stream, &bytes);
        stream->Release();
        if (SUCCEEDED(hr) && SUCCEEDED(bytes->QueryInterface(IID_PPV_ARGS(&bytes_attrs)))) {
            bytes_attrs->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateSourceReaderFromByteStream(bytes, nullptr, &reader);
        }
        if (SUCCEEDED(hr)) {
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateMediaType(&type);
        }
        if (SUCCEEDED(hr)) {
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
        }
        if (SUCCEEDED(hr)) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
                var.vt == VT_UI8) {
                duration_ms = var.uhVal.QuadPart / 10000;
            }
            PropVariantClear(&var);
        }
        SafeRelease(type);
        SafeRelease(bytes_attrs);
        SafeRelease(bytes);
        if (FAILED(hr)) {
            log::Tagged("AVPLAYER", "Media Foundation could not open the video (0x%08lX)", static_cast<unsigned long>(hr));
            SafeRelease(reader);
        }
        return reader;
    }

    void ReadType(IMFSourceReader* reader, u32* w, u32* h, s32* stride) {
        IMFMediaType* type = nullptr;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) {
            return;
        }
        UINT32 fw = 0, fh = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &fw, &fh);
        UINT32 s = 0;
        *stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? static_cast<s32>(s) : static_cast<s32>(fw);
        *w = fw;
        *h = fh;
        type->Release();
        std::lock_guard lock(mutex_);
        width = AlignUp(fw, 16);
        height = AlignUp(fh, 16);
        pitch = AlignUp(fw, 64);
    }

    // NV12 do decodificador (stride e linhas dele) -> NV12 do jogo: pitch x altura alinhada, Y e depois UV.
    bool CopySample(IMFSample* sample, u32 src_w, u32 src_h, s32 src_stride, Frame* out) {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            return false;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        bool ok = false;
        u32 w, h, p;
        {
            std::lock_guard lock(mutex_);
            w = width;
            h = height;
            p = pitch;
        }
        const u32 stride = static_cast<u32>(src_stride > 0 ? src_stride : static_cast<s32>(src_w));
        if (stride && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            const u32 src_rows = static_cast<u32>(static_cast<u64>(length) * 2 / 3 / stride); // linhas do plano Y no buffer
            if (src_rows >= src_h && src_h) {
                out->nv12.assign(static_cast<size_t>(p) * h * 3 / 2, 0);
                const u32 copy_w = std::min(src_w, p);
                for (u32 y = 0; y < h; ++y) {
                    std::memcpy(&out->nv12[static_cast<size_t>(y) * p], data + static_cast<size_t>(std::min(y, src_rows - 1)) * stride, copy_w);
                }
                const BYTE* uv = data + static_cast<size_t>(src_rows) * stride;
                u8* dst_uv = out->nv12.data() + static_cast<size_t>(p) * h;
                for (u32 y = 0; y < h / 2; ++y) {
                    std::memcpy(dst_uv + static_cast<size_t>(y) * p, uv + static_cast<size_t>(std::min(y, src_rows / 2 - 1)) * stride, copy_w);
                }
                ok = true;
            }
            buffer->Unlock();
        }
        buffer->Release();
        return ok;
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    bool opened_ = false;
    bool finished_ = false;
    bool quit_ = false;
};

struct AvPlayer {
    void* user_data = nullptr;
    void* callback = nullptr;
    std::string source;
    bool looping = false;
    std::atomic<bool> active{false};    // sem decodificador: Start ja foi chamado e o IsActive ainda nao respondeu 1
    std::atomic<bool> stop_sent{false}; // STATE_STOP do fim do video ja enviado
    std::unique_ptr<Decoder> decoder;
    Frame current; // o ultimo quadro entregue (o jogo copia na hora)
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point paused_at;
    bool started = false;
    bool paused = false;
};

void Event(AvPlayer* player, int event_id) {
    if (player->callback) {
        reinterpret_cast<CallbackFn>(player->callback)(player->user_data, event_id, 0, nullptr);
    }
}

u64 NowMs(AvPlayer* player) {
    if (!player->started) {
        return 0;
    }
    const auto now = player->paused ? player->paused_at : std::chrono::steady_clock::now();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now - player->start).count());
}

std::unique_ptr<Decoder> OpenVideo() {
    radar::StoredFile file;
    if (!radar::FindStoredFile(".mp4", &file)) {
        log::Tagged("AVPLAYER", "no .mp4 found in the indexes of the packs read so far: the video will have no picture");
        return nullptr;
    }
    const fs::Resolved resolved = fs::Resolve(file.guest_pack.c_str());
    auto decoder = std::make_unique<Decoder>();
    if (resolved.host.empty() || !decoder->Open(resolved.host, file.offset, file.size)) {
        return nullptr;
    }
    log::Tagged("AVPLAYER", "video %s (%s, %.0f MB): %ux%u, %.1f s, decoded by Media Foundation", file.name.c_str(),
                file.guest_pack.c_str(), file.size / 1048576.0, decoder->width, decoder->height, decoder->duration_ms / 1000.0);
    return decoder;
}

HLE_IMPL(u64, sceAvPlayerInit, const void* init_data) {
    auto* player = new AvPlayer();
    if (init_data) {
        // Layout de SceAvPlayerInitData no PS4/eboot:
        // [init_data + 0x50] = pUserData, [init_data + 0x58] = eventCallback
        player->user_data = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(init_data) + 0x50);
        player->callback = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(init_data) + 0x58);
    }
    log::Tagged("AVPLAYER", "sceAvPlayerInit(%p) -> player=%p (user_data=%p callback=%p)",
                init_data, player, player->user_data, player->callback);
    return reinterpret_cast<u64>(player);
}
HLE_OVERRIDE(sceAvPlayerInit);

HLE_IMPL(int, sceAvPlayerPostInit, AvPlayer* player, const void* reserved) {
    log::Tagged("AVPLAYER", "sceAvPlayerPostInit(%p)", player);
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerPostInit);

HLE_IMPL(int, sceAvPlayerAddSource, AvPlayer* player, const char* filename) {
    log::Tagged("AVPLAYER", "sceAvPlayerAddSource(%p, \"%s\")", player, filename ? filename : "");
    if (player) {
        if (filename) {
            player->source = filename;
        }
        player->decoder = OpenVideo();
        if (player->user_data) {
            // Desbloqueia diretamente o loop de espera do eboot na thread NuBgProc-ET (while(![rbx + 0x28]) usleep(1000))
            *reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(player->user_data) + 0x28) = 1;
        }
        // READY -> o jogo configura os streams
        log::Tagged("AVPLAYER", "Disparando evento READY (2) para callback %p (user_data=%p)", player->callback, player->user_data);
        Event(player, kStateReady);
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerAddSource);

HLE_IMPL(int, sceAvPlayerStreamCount, AvPlayer* player) {
    log::Tagged("AVPLAYER", "sceAvPlayerStreamCount(%p) -> 1", player);
    return 1;
}
HLE_OVERRIDE(sceAvPlayerStreamCount);

#pragma pack(push, 1)
struct SceAvPlayerVideoInfo {
    u32 width;
    u32 height;
    float aspect_ratio;
    u32 language_code;
};

struct SceAvPlayerStreamInfo {
    u32 type;
    u32 reserved;
    SceAvPlayerVideoInfo video;
    u64 duration;
    u64 start_time;
};

struct SceAvPlayerFrameInfo {
    u8* data;
    u8 reserved[8];
    u64 timestamp;
    SceAvPlayerVideoInfo video;
};
#pragma pack(pop)
static_assert(sizeof(SceAvPlayerFrameInfo) == 0x28);

HLE_IMPL(int, sceAvPlayerGetStreamInfo, AvPlayer* player, u32 stream_index, void* info) {
    log::Tagged("AVPLAYER", "sceAvPlayerGetStreamInfo(%p, stream=%u, info=%p)", player, stream_index, info);
    if (info) {
        std::memset(info, 0, sizeof(SceAvPlayerStreamInfo));
        auto* s = reinterpret_cast<SceAvPlayerStreamInfo*>(info);
        const Decoder* decoder = player ? player->decoder.get() : nullptr;
        s->type = 0; // video
        s->video.width = decoder ? decoder->width : 1920;
        s->video.height = decoder ? decoder->height : 1080;
        s->video.aspect_ratio = 1.0f;
        s->duration = decoder && decoder->duration_ms ? decoder->duration_ms : 1000;
        s->start_time = 0;
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerGetStreamInfo);

HLE_IMPL(int, sceAvPlayerEnableStream, AvPlayer* player, u32 stream_index) {
    log::Tagged("AVPLAYER", "sceAvPlayerEnableStream(%p, %u)", player, stream_index);
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerEnableStream);

HLE_IMPL(int, sceAvPlayerSetLooping, AvPlayer* player, u32 loop) {
    log::Tagged("AVPLAYER", "sceAvPlayerSetLooping(%p, %u)", player, loop);
    if (player) {
        player->looping = (loop != 0);
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerSetLooping);

HLE_IMPL(int, sceAvPlayerStart, AvPlayer* player) {
    log::Tagged("AVPLAYER", "sceAvPlayerStart(%p)", player);
    if (player) {
        player->active = true;
        player->stop_sent = false;
        player->started = true;
        player->paused = false;
        player->start = std::chrono::steady_clock::now();
        if (player->user_data) {
            *reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(player->user_data) + 0x28) = 1;
        }
        // o STATE_STOP do fim do video sai no sceAvPlayerIsActive, como o shadPS4 faz no EOF
        Event(player, kStatePlay);
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerStart);

HLE_IMPL(int, sceAvPlayerIsActive, AvPlayer* player) {
    if (!player) {
        return 0;
    }
    if (player->decoder) {
        if (player->started && !player->decoder->Done()) {
            return 1;
        }
    } else if (player->active.exchange(false)) {
        return 1; // sem decodificador: 1 uma vez (o jogo sai da espera do Start) e depois o fim
    }
    if (!player->stop_sent.exchange(true)) {
        log::Tagged("AVPLAYER", "sceAvPlayerIsActive(%p): video ended (%llu frames decoded) -> STATE_STOP", player,
                    static_cast<unsigned long long>(player->decoder ? player->decoder->frames : 0));
        Event(player, kStateStop);
    }
    return 0;
}
HLE_OVERRIDE(sceAvPlayerIsActive);

HLE_IMPL(int, sceAvPlayerGetVideoData, AvPlayer* player, void* frame_info) {
    if (!player || !player->decoder || !frame_info || player->paused) {
        return 0;
    }
    Decoder& decoder = *player->decoder;
    if (!decoder.Pop(NowMs(player), &player->current)) {
        return 0;
    }
    auto* info = reinterpret_cast<SceAvPlayerFrameInfo*>(frame_info);
    std::memset(info, 0, sizeof(*info));
    info->data = player->current.nv12.data();
    info->timestamp = player->current.timestamp_ms;
    info->video.width = decoder.width;
    info->video.height = decoder.height;
    info->video.aspect_ratio = 1.0f;
    return 1;
}
HLE_OVERRIDE(sceAvPlayerGetVideoData);

HLE_IMPL(int, sceAvPlayerGetAudioData, AvPlayer* player, void* audio_info) {
    return 0; // o .mp4 nao tem audio (o som do episodio e tocado pelo jogo)
}
HLE_OVERRIDE(sceAvPlayerGetAudioData);

HLE_IMPL(u64, sceAvPlayerCurrentTime, AvPlayer* player) {
    return player ? NowMs(player) : 0;
}
HLE_OVERRIDE(sceAvPlayerCurrentTime);

HLE_IMPL(int, sceAvPlayerStop, AvPlayer* player) {
    log::Tagged("AVPLAYER", "sceAvPlayerStop(%p)", player);
    if (player) {
        player->active = false;
        player->stop_sent = true;
        player->decoder.reset();
        // Evento 1 = SCE_AVPLAYER_STATE_STOP
        Event(player, kStateStop);
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerStop);

HLE_IMPL(int, sceAvPlayerPause, AvPlayer* player) {
    if (player && player->started && !player->paused) {
        player->paused = true;
        player->paused_at = std::chrono::steady_clock::now();
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerPause);

HLE_IMPL(int, sceAvPlayerResume, AvPlayer* player) {
    if (player && player->paused) {
        player->start += std::chrono::steady_clock::now() - player->paused_at; // o relogio do video nao anda na pausa
        player->paused = false;
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerResume);

HLE_IMPL(int, sceAvPlayerClose, AvPlayer* player) {
    log::Tagged("AVPLAYER", "sceAvPlayerClose(%p)", player);
    if (player) {
        delete player;
    }
    return err::OK;
}
HLE_OVERRIDE(sceAvPlayerClose);

} // namespace
