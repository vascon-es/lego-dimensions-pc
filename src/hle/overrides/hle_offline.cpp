// Servicos que nesta maquina simplesmente nao existem: rede, PSN, trofeus, PlayGo, DLC.
// A regra e responder de forma coerente com um console OFFLINE e com o jogo todo instalado:
// inicializadores e criacao de handles funcionam, qualquer coisa que dependa de rede ou de
// conta PSN falha com o erro de "sem rede" / "deslogado". Nada bloqueia.
//
// Cobertura: libSceNpTrophy 13/13, libSceNetCtl 9/9, libSceNet 21/21, libSceHttp 17/17,
// libSceSsl 3/3, libSceNpManager 17/17, libScePlayGo 7/7, libSceSysmodule 3/3,
// libSceRemoteplay 2/2, libSceCoredump 2/2, libSceScreenShot 2/2, libSceGameLiveStreaming 1/1,
// libSceVideoRecording 1/1 e o resto de libSceAppContent (DLC).
// Ficam nos stubs gerados (retornam 0): NpMatching2, NpScore, NpTus, NpUtility, NpWebApi,
// NpAuth, NpSns, NpCommerce e os dialogos de convite/navegador/Facebook - nenhum foi chamado.
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "hle/overrides/hle_override.h"
#include "hle/trophy_manager.h"

namespace {

using namespace hle;

constexpr int kNetErrorNetDown = static_cast<int>(0x80410132);      // SCE_NET_ERROR_ENETDOWN
constexpr int kNetCtlErrorNotConnected = static_cast<int>(0x80412108);
constexpr int kHttpErrorNetwork = static_cast<int>(0x80431063);
constexpr int kHttpErrorBeforeSend = static_cast<int>(0x80431065);
constexpr int kNpErrorSignedOut = static_cast<int>(0x80550006);
constexpr int kTrophyErrorInvalidArgument = static_cast<int>(0x80551604);
constexpr int kTrophyErrorIconNotFound = static_cast<int>(0x80551614);
constexpr int kAppContentErrorNotFound = static_cast<int>(0x80D90005);

std::atomic<int> g_next_id{1};

int NewId() {
    return g_next_id.fetch_add(1);
}

// Funcoes sem argumentos relevantes que so devolvem um valor fixo.
#define HLE_FIXED(name, value)                                                                     \
    HLE_IMPL(int, name) {                                                                          \
        HLE_TRACE("-> 0x%x", static_cast<unsigned>(value));                                        \
        return value;                                                                              \
    }                                                                                              \
    HLE_OVERRIDE(name)

#define HLE_FIXED_QUIET(name, value)                                                               \
    HLE_IMPL(int, name) {                                                                          \
        HLE_QUIET("-> 0x%x", static_cast<unsigned>(value));                                        \
        return value;                                                                              \
    }                                                                                              \
    HLE_OVERRIDE(name)

// Criacao de handle/contexto/request: devolve um id novo (>= 1) no retorno.
#define HLE_NEW_ID(name)                                                                           \
    HLE_IMPL(int, name) {                                                                          \
        const int id = NewId();                                                                    \
        HLE_TRACE("-> id %d", id);                                                                 \
        return id;                                                                                 \
    }                                                                                              \
    HLE_OVERRIDE(name)

// ================================ NpTrophy ==================================
// Trofeus sao aceitos e registrados no log; nada e persistido.
HLE_IMPL(int, sceNpTrophyCreateContext, s32* context, s32 user_id, u32 service_label, u64 options) {
    if (!context) {
        return kTrophyErrorInvalidArgument;
    }
    *context = NewId();
    HLE_TRACE("user=%d label=%u -> context %d", user_id, service_label, *context);
    return err::OK;
}
HLE_OVERRIDE(sceNpTrophyCreateContext);

HLE_IMPL(int, sceNpTrophyCreateHandle, s32* handle) {
    if (!handle) {
        return kTrophyErrorInvalidArgument;
    }
    *handle = NewId();
    HLE_TRACE("-> handle %d", *handle);
    return err::OK;
}
HLE_OVERRIDE(sceNpTrophyCreateHandle);

HLE_IMPL(int, sceNpTrophyRegisterContext, s32 context, s32 handle, u64 options) {
    HLE_TRACE("context=%d handle=%d", context, handle);
    return err::OK;
}
HLE_OVERRIDE(sceNpTrophyRegisterContext);

HLE_IMPL(int, sceNpTrophyUnlockTrophy, s32 context, s32 handle, s32 trophy_id, s32* platinum_id) {
    HLE_TRACE("TROPHY %d unlocked (context=%d)", trophy_id, context);
    int plat = -1;
    hle::trophy::TrophyManager::Instance().UnlockTrophy(trophy_id, &plat);
    if (platinum_id) {
        *platinum_id = plat;
    }
    return err::OK;
}
HLE_OVERRIDE(sceNpTrophyUnlockTrophy);

// Mapa de 128 bits de trofeus desbloqueados
HLE_IMPL(int, sceNpTrophyGetTrophyUnlockState, s32 context, s32 handle, u32* flags, u32* count) {
    HLE_TRACE("contexto=%d", context);
    if (!flags || !count) {
        return kTrophyErrorInvalidArgument;
    }
    hle::trophy::TrophyManager::Instance().GetUnlockState(flags, count);
    return err::OK;
}
HLE_OVERRIDE(sceNpTrophyGetTrophyUnlockState);

HLE_FIXED(sceNpTrophyDestroyContext, 0);
HLE_FIXED(sceNpTrophyDestroyHandle, 0);
// Icones e textos viriam do trophy00.trp, que nao e interpretado.
HLE_FIXED(sceNpTrophyGetGameIcon, kTrophyErrorIconNotFound);
HLE_FIXED(sceNpTrophyGetGroupIcon, kTrophyErrorIconNotFound);
HLE_FIXED(sceNpTrophyGetTrophyIcon, kTrophyErrorIconNotFound);
HLE_FIXED(sceNpTrophyGetGameInfo, kTrophyErrorIconNotFound);
HLE_FIXED(sceNpTrophyGetGroupInfo, kTrophyErrorIconNotFound);
HLE_FIXED(sceNpTrophyGetTrophyInfo, kTrophyErrorIconNotFound);

// ================================= NetCtl ===================================
HLE_FIXED(sceNetCtlInit, 0);
HLE_FIXED(sceNetCtlTerm, 0);
HLE_FIXED_QUIET(sceNetCtlCheckCallback, 0);
HLE_FIXED(sceNetCtlUnregisterCallback, 0);

HLE_IMPL(int, sceNetCtlRegisterCallback, void* callback, void* arg, int* callback_id) {
    if (callback_id) {
        *callback_id = NewId();
    }
    HLE_TRACE("callback=%p -> registered (will never be called: network state does not change)", callback);
    return err::OK;
}
HLE_OVERRIDE(sceNetCtlRegisterCallback);

HLE_IMPL(int, sceNetCtlGetState, int* state) {
    HLE_QUIET("-> disconnected");
    if (state) {
        *state = 0; // SCE_NET_CTL_STATE_DISCONNECTED
    }
    return err::OK;
}
HLE_OVERRIDE(sceNetCtlGetState);

HLE_IMPL(int, sceNetCtlGetResult, int event_type, int* error_code) {
    HLE_TRACE("event=%d", event_type);
    if (error_code) {
        *error_code = kNetCtlErrorNotConnected;
    }
    return err::OK;
}
HLE_OVERRIDE(sceNetCtlGetResult);

HLE_FIXED_QUIET(sceNetCtlGetInfo, kNetCtlErrorNotConnected);
HLE_FIXED_QUIET(sceNetCtlGetNatInfo, kNetCtlErrorNotConnected);

// ================================== Net =====================================
HLE_FIXED(sceNetInit, 0);
HLE_FIXED(sceNetTerm, 0);
HLE_NEW_ID(sceNetPoolCreate);
HLE_FIXED(sceNetPoolDestroy, 0);
HLE_NEW_ID(sceNetResolverCreate);
HLE_FIXED(sceNetResolverDestroy, 0);
HLE_FIXED(sceNetResolverStartNtoa, kNetErrorNetDown);
// Nenhum socket pode ser criado: a interface de rede esta fora do ar.
HLE_FIXED(sceNetSocket, kNetErrorNetDown);
HLE_FIXED(sceNetConnect, kNetErrorNetDown);
HLE_FIXED(sceNetBind, kNetErrorNetDown);
HLE_FIXED(sceNetListen, kNetErrorNetDown);
HLE_FIXED(sceNetAccept, kNetErrorNetDown);
HLE_FIXED(sceNetSend, kNetErrorNetDown);
HLE_FIXED(sceNetSendto, kNetErrorNetDown);
HLE_FIXED(sceNetRecv, kNetErrorNetDown);
HLE_FIXED(sceNetRecvfrom, kNetErrorNetDown);
HLE_FIXED(sceNetGetpeername, kNetErrorNetDown);
HLE_FIXED(sceNetSetsockopt, kNetErrorNetDown);
HLE_FIXED(sceNetShutdown, kNetErrorNetDown);
HLE_FIXED(sceNetSocketClose, kNetErrorNetDown);

// So IPv4 (AF_INET = 2). Retorna 1 se converteu, 0 se o texto e invalido.
HLE_IMPL(int, sceNetInetPton, int family, const char* text, u8* out) {
    unsigned a, b, c, d;
    char extra;
    const bool ok = family == 2 && text && out && std::sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 &&
                    a < 256 && b < 256 && c < 256 && d < 256;
    HLE_TRACE("\"%s\" -> %d", text ? text : "", ok);
    if (ok) {
        out[0] = static_cast<u8>(a);
        out[1] = static_cast<u8>(b);
        out[2] = static_cast<u8>(c);
        out[3] = static_cast<u8>(d);
    }
    return ok ? 1 : 0;
}
HLE_OVERRIDE(sceNetInetPton);

// ================================ Ssl / Http ================================
HLE_NEW_ID(sceSslInit);
HLE_FIXED(sceSslTerm, 0);
HLE_FIXED(sceSslGetSerialNumber, kHttpErrorNetwork);

HLE_NEW_ID(sceHttpInit);
HLE_FIXED(sceHttpTerm, 0);
HLE_NEW_ID(sceHttpCreateTemplate);
HLE_FIXED(sceHttpDeleteTemplate, 0);
HLE_FIXED(sceHttpDeleteConnection, 0);
HLE_FIXED(sceHttpDeleteRequest, 0);
HLE_FIXED(sceHttpsDisableOption, 0);
HLE_FIXED(sceHttpsSetSslCallback, 0);
HLE_FIXED(sceHttpAddRequestHeader, 0);

HLE_IMPL(int, sceHttpCreateConnectionWithURL, int template_id, const char* url, int keep_alive) {
    const int id = NewId();
    HLE_TRACE("\"%.200s\" -> connection %d", url ? url : "", id);
    return id;
}
HLE_OVERRIDE(sceHttpCreateConnectionWithURL);

HLE_IMPL(int, sceHttpCreateRequestWithURL, int connection_id, int method, const char* url, u64 content_length) {
    const int id = NewId();
    HLE_TRACE("method=%d \"%.200s\" -> request %d", method, url ? url : "", id);
    return id;
}
HLE_OVERRIDE(sceHttpCreateRequestWithURL);

HLE_IMPL(int, sceHttpCreateRequestWithURL2, int connection_id, const char* method, const char* url,
         u64 content_length) {
    const int id = NewId();
    HLE_TRACE("%s \"%.200s\" -> request %d", method ? method : "?", url ? url : "", id);
    return id;
}
HLE_OVERRIDE(sceHttpCreateRequestWithURL2);

// O envio e onde a falta de rede aparece; sem envio nao ha status nem corpo.
HLE_FIXED(sceHttpSendRequest, kHttpErrorNetwork);
HLE_FIXED(sceHttpGetStatusCode, kHttpErrorBeforeSend);
HLE_FIXED(sceHttpGetResponseContentLength, kHttpErrorBeforeSend);
HLE_FIXED(sceHttpReadData, kHttpErrorBeforeSend);

// Percent-encoding (RFC 3986). `required` recebe o tamanho necessario, com o terminador.
HLE_IMPL(int, sceHttpUriEscape, char* out, u64* required, u64 out_size, const char* in) {
    HLE_TRACE("\"%.100s\"", in ? in : "");
    if (!in) {
        return kHttpErrorBeforeSend;
    }
    std::string escaped;
    for (const char* p = in; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            escaped += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            escaped += hex;
        }
    }
    if (required) {
        *required = escaped.size() + 1;
    }
    if (out && out_size > escaped.size()) {
        std::memcpy(out, escaped.c_str(), escaped.size() + 1);
    } else if (out) {
        return kHttpErrorBeforeSend; // buffer pequeno
    }
    return err::OK;
}
HLE_OVERRIDE(sceHttpUriEscape);

// ================================ NpManager =================================
// Usuario local sem conta PSN conectada.
HLE_FIXED_QUIET(sceNpCheckCallback, 0);
HLE_FIXED(sceNpSetContentRestriction, 0);
HLE_FIXED(sceNpRegisterStateCallback, 0);
HLE_FIXED(sceNpUnregisterStateCallback, 0);
HLE_FIXED(sceNpSetNpTitleId, 0);
HLE_NEW_ID(sceNpCreateRequest);
HLE_FIXED(sceNpDeleteRequest, 0);
HLE_FIXED(sceNpGetAccountDateOfBirth, kNpErrorSignedOut);
HLE_FIXED(sceNpGetOnlineId, kNpErrorSignedOut);
HLE_FIXED(sceNpGetNpId, kNpErrorSignedOut);
HLE_FIXED(sceNpCheckNpAvailability, kNpErrorSignedOut);
HLE_FIXED(sceNpInGameMessageInitialize, 0);
HLE_FIXED(sceNpInGameMessageTerminate, 0);
HLE_NEW_ID(sceNpInGameMessageCreateHandle);
HLE_FIXED(sceNpInGameMessageDeleteHandle, 0);
HLE_FIXED(sceNpInGameMessagePrepare, kNpErrorSignedOut);
HLE_FIXED(sceNpInGameMessageSendData, kNpErrorSignedOut);

// ================================= PlayGo ===================================
// O jogo esta todo "instalado": todo chunk esta disponivel no disco rapido.
constexpr int kPlayGoHandle = 1;

struct PlayGoProgress {
    u64 progress_size;
    u64 total_size;
};

HLE_FIXED(scePlayGoInitialize, 0);
HLE_FIXED(scePlayGoTerminate, 0);
HLE_FIXED(scePlayGoClose, 0);
HLE_FIXED_QUIET(scePlayGoPrefetch, 0);

HLE_IMPL(int, scePlayGoOpen, int* handle, const void* param) {
    HLE_TRACE("-> handle %d", kPlayGoHandle);
    if (!handle) {
        return static_cast<int>(0x80B2000A); // BAD_POINTER
    }
    *handle = kPlayGoHandle;
    return err::OK;
}
HLE_OVERRIDE(scePlayGoOpen);

HLE_IMPL(int, scePlayGoGetLocus, int handle, const u16* chunk_ids, u32 count, u8* loci) {
    HLE_QUIET("%u chunks", count);
    if (!chunk_ids || !loci) {
        return static_cast<int>(0x80B2000A);
    }
    std::memset(loci, 3, count); // SCE_PLAYGO_LOCUS_LOCAL_FAST
    return err::OK;
}
HLE_OVERRIDE(scePlayGoGetLocus);

HLE_IMPL(int, scePlayGoGetProgress, int handle, const u16* chunk_ids, u32 count, PlayGoProgress* progress) {
    HLE_QUIET("%u chunks", count);
    if (!progress) {
        return static_cast<int>(0x80B2000A);
    }
    progress->progress_size = progress->total_size = 0x40000000; // 100%
    return err::OK;
}
HLE_OVERRIDE(scePlayGoGetProgress);

// ============================ modulos e servicos ============================
// Os modulos de sistema que o jogo "carrega" ja estao implementados em HLE (ou sao stubs).
HLE_IMPL(int, sceSysmoduleLoadModule, u32 module_id) {
    HLE_QUIET("0x%x", module_id);
    return err::OK;
}
HLE_OVERRIDE(sceSysmoduleLoadModule);

HLE_IMPL(int, sceSysmoduleUnloadModule, u32 module_id) {
    HLE_QUIET("0x%x", module_id);
    return err::OK;
}
HLE_OVERRIDE(sceSysmoduleUnloadModule);

HLE_IMPL(int, sceSysmoduleIsLoaded, u32 module_id) {
    HLE_QUIET("0x%x", module_id);
    return err::OK; // 0 = carregado
}
HLE_OVERRIDE(sceSysmoduleIsLoaded);

HLE_FIXED(sceRemoteplayInitialize, 0);
HLE_FIXED_QUIET(sceRemoteplayProhibit, 0);
HLE_FIXED(sceCoredumpRegisterCoredumpHandler, 0);
HLE_FIXED(sceCoredumpWriteUserData, 0);
HLE_FIXED(sceScreenShotEnable, 0);
HLE_FIXED(sceScreenShotDisable, 0);
HLE_FIXED(sceGameLiveStreamingPermitServerSideRecording, 0);
HLE_FIXED(sceVideoRecordingSetInfo, 0);

// ============================= AppContent (DLC) =============================
// Os DLCs instalados ficam em hle_userservice_pad.cpp (Fase 46); apagar e baixar nao existem aqui.
HLE_FIXED(sceAppContentAddcontDelete, kAppContentErrorNotFound);
HLE_FIXED(sceAppContentAddcontEnqueueDownload, kAppContentErrorNotFound);

} // namespace
