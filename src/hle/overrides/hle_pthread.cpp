// libkernel: scePthreadMutex*, scePthreadMutexattr*, scePthreadCond*, scePthreadAttr*.
//
// ScePthreadMutex / ScePthreadCond / *attr sao ponteiros opacos: o jogo guarda um ponteiro e
// nos alocamos o objeto de verdade no heap. Mutex = CRITICAL_SECTION (recursiva por natureza),
// com dono e contagem proprios para reproduzir os tipos nao recursivos do Orbis.
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include "hle/overrides/hle_override.h"
#include "runtime/crash_handler.h"
#include "runtime/orbis_tls.h"

namespace {

using namespace hle;

constexpr u32 kMutexMagic = 0x4D545831; // "MTX1"
constexpr u32 kCondMagic = 0x434E4431;  // "CND1"

// Tipos de mutex do Orbis
constexpr int kTypeErrorCheck = 1;
constexpr int kTypeRecursive = 2;
constexpr int kTypeNormal = 3;
constexpr int kTypeAdaptive = 4;

// Inicializadores estaticos: SCE_PTHREAD_MUTEX_INITIALIZER = NULL,
// SCE_PTHREAD_ADAPTIVE_MUTEX_INITIALIZER = (void*)1.
constexpr u64 kStaticAdaptive = 1;

struct HostMutex {
    u32 magic = kMutexMagic;
    int type = kTypeErrorCheck;
    CRITICAL_SECTION cs;
    DWORD owner = 0;
    u32 count = 0;
    char name[32] = {};
};

struct HostMutexAttr {
    int type = kTypeErrorCheck;
    int protocol = 0;
};

struct HostCond {
    u32 magic = kCondMagic;
    CONDITION_VARIABLE cv;
    char name[32] = {};
};

struct HostThreadAttr {
    u64 stack_size = 0x10000;
    u64 affinity = 0x7F;
    int detach_state = 0;
};

using GuestMutex = HostMutex*;
using GuestMutexAttr = HostMutexAttr*;
using GuestCond = HostCond*;
using GuestThreadAttr = HostThreadAttr*;

SRWLOCK g_lazy_lock = SRWLOCK_INIT; // protege a criacao preguicosa de mutex/cond estaticos

// 4.4.25: leituras de memoria do jogo a partir de um handle. O jogo guarda o ponteiro do mutex dentro de structs
// dele; se esse ponteiro estiver corrompido/solto (troca de personagem, cutscene, thread do audio) o magic check
// lia memoria invalida e derrubava o processo - era o crash do Ngs2WatchThread em `cmp dword ptr [rdi], 0x4D545831`.
// Estas duas funcoes nunca falham: devolvem false em vez de tocar no endereco.
__declspec(noinline) bool ReadGuestPtr(const void* at, void** out) {
    __try {
        *out = *static_cast<void* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool ReadGuestU32(const void* at, u32* out) {
    __try {
        *out = *static_cast<const u32*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Handle que nao e' nosso (ponteiro velho ou lixo): em vez de estourar ou travar o jogo, usa um mutex global. O
// jogo acha que travou/destravou e segue.
HostMutex* FallbackMutex() {
    static HostMutex* m = [] {
        auto* created = new HostMutex;
        InitializeCriticalSection(&created->cs);
        std::strncpy(created->name, "fallback", sizeof(created->name) - 1);
        return created;
    }();
    return m;
}

void WarnBadHandle(const char* what, const void* value) {
    static std::atomic<int> loud{0};
    if (loud.fetch_add(1) < 8) {
        std::printf("[SYNC] %s: handle invalido %p (o jogo guardou um ponteiro morto); usando o mutex interno\n", what,
                    value);
        std::fflush(stdout);
    }
}

HostMutex* CreateMutex_(int type, const char* name) {
    auto* m = new HostMutex;
    m->type = type;
    InitializeCriticalSection(&m->cs);
    if (name) {
        std::strncpy(m->name, name, sizeof(m->name) - 1);
    }
    return m;
}

bool IsStatic(const void* handle) {
    return reinterpret_cast<u64>(handle) <= kStaticAdaptive;
}

// Devolve o mutex do host, criando-o se o jogo usou um inicializador estatico.
HostMutex* GetMutex_(GuestMutex* guest) {
    if (!guest) {
        return nullptr;
    }
    void* raw = nullptr;
    if (!ReadGuestPtr(guest, &raw)) {
        WarnBadHandle("mutex", guest);
        return FallbackMutex();
    }
    HostMutex* m = static_cast<HostMutex*>(raw);
    if (IsStatic(m)) {
        AcquireSRWLockExclusive(&g_lazy_lock);
        m = *guest;
        if (IsStatic(m)) {
            m = CreateMutex_(reinterpret_cast<u64>(m) == kStaticAdaptive ? kTypeAdaptive : kTypeErrorCheck,
                             "static");
            *guest = m;
        }
        ReleaseSRWLockExclusive(&g_lazy_lock);
    }
    u32 magic = 0;
    if (!ReadGuestU32(m, &magic)) {
        WarnBadHandle("mutex", m);
        return FallbackMutex();
    }
    if (magic != kMutexMagic) {
        WarnBadHandle("mutex", m); // ponteiro velho (destruido) ou de outro tipo
        return FallbackMutex();
    }
    return m;
}

HostMutex* GetMutex(GuestMutex* guest) {
    return GetMutex_(guest);
}

HostCond* GetCond(GuestCond* guest) {
    if (!guest) {
        return nullptr;
    }
    void* raw = nullptr;
    if (!ReadGuestPtr(guest, &raw)) {
        return nullptr;
    }
    HostCond* c = static_cast<HostCond*>(raw);
    if (!c) {
        AcquireSRWLockExclusive(&g_lazy_lock);
        c = *guest;
        if (!c) {
            c = new HostCond;
            InitializeConditionVariable(&c->cv);
            *guest = c;
        }
        ReleaseSRWLockExclusive(&g_lazy_lock);
    }
    u32 magic = 0;
    return ReadGuestU32(c, &magic) && magic == kCondMagic ? c : nullptr;
}

// ---- mutexattr -------------------------------------------------------------
HLE_IMPL(int, scePthreadMutexattrInit, GuestMutexAttr* attr) {
    HLE_QUIET("%p", static_cast<void*>(attr));
    if (!attr) {
        return err::kEINVAL;
    }
    *attr = new HostMutexAttr;
    return err::OK;
}
HLE_OVERRIDE(scePthreadMutexattrInit);

HLE_IMPL(int, scePthreadMutexattrDestroy, GuestMutexAttr* attr) {
    HLE_QUIET("%p", static_cast<void*>(attr));
    if (!attr || !*attr) {
        return err::kEINVAL;
    }
    delete *attr;
    *attr = nullptr;
    return err::OK;
}
HLE_OVERRIDE(scePthreadMutexattrDestroy);

HLE_IMPL(int, scePthreadMutexattrSettype, GuestMutexAttr* attr, int type) {
    HLE_QUIET("%p type=%d", static_cast<void*>(attr), type);
    if (!attr || !*attr || type < kTypeErrorCheck || type > kTypeAdaptive) {
        return err::kEINVAL;
    }
    (*attr)->type = type;
    return err::OK;
}
HLE_OVERRIDE(scePthreadMutexattrSettype);

// ---- mutex -----------------------------------------------------------------
HLE_IMPL(int, scePthreadMutexInit, GuestMutex* mutex, const GuestMutexAttr* attr, const char* name) {
    if (!mutex) {
        return err::kEINVAL;
    }
    const int type = (attr && *attr) ? (*attr)->type : kTypeErrorCheck;
    *mutex = CreateMutex_(type, name);
    HLE_QUIET("%p type=%d name=%s", static_cast<void*>(mutex), type, name ? name : "(unnamed)");
    return err::OK;
}
HLE_OVERRIDE(scePthreadMutexInit);

HLE_IMPL(int, scePthreadMutexDestroy, GuestMutex* mutex) {
    HLE_QUIET("%p", static_cast<void*>(mutex));
    if (!mutex) {
        return err::kEINVAL;
    }
    HostMutex* m = *mutex;
    if (IsStatic(m)) {
        return err::OK; // estatico que nunca chegou a ser usado
    }
    if (m->magic != kMutexMagic) {
        return err::kEINVAL;
    }
    if (m->count) {
        return err::kEBUSY;
    }
    // 4.4.25: o bloco NAO e' liberado. O jogo as vezes guarda o ponteiro do mutex em mais de um lugar e chama
    // lock/unlock depois do destroy: com o bloco vivo o magic check da INVALID_HANDLE (ou cai no mutex interno) em
    // vez de ler memoria do heap ja devolvida. Sao poucos bytes por mutex, o vazamento e' proposital.
    DeleteCriticalSection(&m->cs);
    m->magic = 0;
    *mutex = nullptr;
    return err::OK;
}
HLE_OVERRIDE(scePthreadMutexDestroy);

int LockImpl(HostMutex* m, bool try_only) {
    if (try_only) {
        if (!TryEnterCriticalSection(&m->cs)) {
            return err::kEBUSY;
        }
    } else {
        EnterCriticalSection(&m->cs);
    }
    if (m->count && m->type != kTypeRecursive) {
        // A CRITICAL_SECTION deixou a mesma thread entrar de novo, mas o mutex do jogo nao e
        // recursivo. ERRORCHECK devolve erro; NORMAL/ADAPTIVE travariam para sempre no Orbis.
        LeaveCriticalSection(&m->cs);
        if (try_only) {
            return err::kEBUSY;
        }
        if (m->type != kTypeErrorCheck) {
            log::Call("scePthreadMutexLock", 0, "WARNING: relocking non-recursive mutex '%s' (deadlock on Orbis)",
                      m->name);
        }
        return err::kEDEADLK;
    }
    m->owner = GetCurrentThreadId();
    ++m->count;
    return err::OK;
}

HLE_IMPL(int, scePthreadMutexLock, GuestMutex* mutex) {
    HLE_QUIET("%p", static_cast<void*>(mutex));
    HostMutex* m = GetMutex(mutex);
    return m ? LockImpl(m, false) : err::kEINVAL;
}
HLE_OVERRIDE(scePthreadMutexLock);

HLE_IMPL(int, scePthreadMutexTrylock, GuestMutex* mutex) {
    HLE_QUIET("%p", static_cast<void*>(mutex));
    HostMutex* m = GetMutex(mutex);
    return m ? LockImpl(m, true) : err::kEINVAL;
}
HLE_OVERRIDE(scePthreadMutexTrylock);

HLE_IMPL(int, scePthreadMutexUnlock, GuestMutex* mutex) {
    HLE_QUIET("%p", static_cast<void*>(mutex));
    HostMutex* m = GetMutex(mutex);
    if (!m) {
        return err::kEINVAL;
    }
    if (!m->count || m->owner != GetCurrentThreadId()) {
        return err::kEPERM;
    }
    if (--m->count == 0) {
        m->owner = 0;
    }
    LeaveCriticalSection(&m->cs);
    return err::OK;
}
HLE_OVERRIDE(scePthreadMutexUnlock);

// ---- cond ------------------------------------------------------------------
HLE_IMPL(int, scePthreadCondattrInit, void** attr) {
    HLE_QUIET("%p", static_cast<void*>(attr));
    if (!attr) {
        return err::kEINVAL;
    }
    *attr = new u64(0);
    return err::OK;
}
HLE_OVERRIDE(scePthreadCondattrInit);

HLE_IMPL(int, scePthreadCondattrDestroy, void** attr) {
    HLE_QUIET("%p", static_cast<void*>(attr));
    if (!attr || !*attr) {
        return err::kEINVAL;
    }
    delete static_cast<u64*>(*attr);
    *attr = nullptr;
    return err::OK;
}
HLE_OVERRIDE(scePthreadCondattrDestroy);

HLE_IMPL(int, scePthreadCondInit, GuestCond* cond, const void* attr, const char* name) {
    if (!cond) {
        return err::kEINVAL;
    }
    auto* c = new HostCond;
    InitializeConditionVariable(&c->cv);
    if (name) {
        std::strncpy(c->name, name, sizeof(c->name) - 1);
    }
    *cond = c;
    HLE_QUIET("%p name=%s", static_cast<void*>(cond), name ? name : "(unnamed)");
    return err::OK;
}
HLE_OVERRIDE(scePthreadCondInit);

HLE_IMPL(int, scePthreadCondDestroy, GuestCond* cond) {
    HLE_QUIET("%p", static_cast<void*>(cond));
    if (!cond) {
        return err::kEINVAL;
    }
    if (HostCond* c = *cond) {
        if (c->magic != kCondMagic) {
            return err::kEINVAL;
        }
        c->magic = 0;
        delete c;
        *cond = nullptr;
    }
    return err::OK;
}
HLE_OVERRIDE(scePthreadCondDestroy);

HLE_IMPL(int, scePthreadCondSignal, GuestCond* cond) {
    HLE_QUIET("%p", static_cast<void*>(cond));
    HostCond* c = GetCond(cond);
    if (!c) {
        return err::kEINVAL;
    }
    WakeConditionVariable(&c->cv);
    return err::OK;
}
HLE_OVERRIDE(scePthreadCondSignal);

HLE_IMPL(int, scePthreadCondBroadcast, GuestCond* cond) {
    HLE_QUIET("%p", static_cast<void*>(cond));
    HostCond* c = GetCond(cond);
    if (!c) {
        return err::kEINVAL;
    }
    WakeAllConditionVariable(&c->cv);
    return err::OK;
}
HLE_OVERRIDE(scePthreadCondBroadcast);

int WaitImpl(GuestCond* cond, GuestMutex* mutex, DWORD timeout_ms) {
    HostCond* c = GetCond(cond);
    HostMutex* m = GetMutex(mutex);
    if (!c || !m) {
        return err::kEINVAL;
    }
    if (m->count != 1 || m->owner != GetCurrentThreadId()) {
        return err::kEPERM; // precisa estar travado exatamente uma vez por esta thread
    }
    m->count = 0;
    m->owner = 0;
    runtime::WaitScope waiting("scePthreadCondWait");
    const BOOL ok = SleepConditionVariableCS(&c->cv, &m->cs, timeout_ms);
    m->owner = GetCurrentThreadId();
    m->count = 1;
    return ok ? err::OK : err::kETIMEDOUT;
}

HLE_IMPL(int, scePthreadCondWait, GuestCond* cond, GuestMutex* mutex) {
    HLE_QUIET("%p mutex=%p", static_cast<void*>(cond), static_cast<void*>(mutex));
    return WaitImpl(cond, mutex, INFINITE);
}
HLE_OVERRIDE(scePthreadCondWait);

HLE_IMPL(int, scePthreadCondTimedwait, GuestCond* cond, GuestMutex* mutex, u32 micros) {
    HLE_QUIET("%p mutex=%p %u us", static_cast<void*>(cond), static_cast<void*>(mutex), micros);
    return WaitImpl(cond, mutex, micros / 1000 + (micros % 1000 ? 1 : 0));
}
HLE_OVERRIDE(scePthreadCondTimedwait);

// ---- atributos de thread -----------------------------------------------------
HLE_IMPL(int, scePthreadAttrInit, GuestThreadAttr* attr) {
    HLE_QUIET("%p", static_cast<void*>(attr));
    if (!attr) {
        return err::kEINVAL;
    }
    *attr = new HostThreadAttr;
    return err::OK;
}
HLE_OVERRIDE(scePthreadAttrInit);

HLE_IMPL(int, scePthreadAttrDestroy, GuestThreadAttr* attr) {
    HLE_QUIET("%p", static_cast<void*>(attr));
    if (!attr || !*attr) {
        return err::kEINVAL;
    }
    delete *attr;
    *attr = nullptr;
    return err::OK;
}
HLE_OVERRIDE(scePthreadAttrDestroy);

HLE_IMPL(int, scePthreadAttrSetstacksize, GuestThreadAttr* attr, u64 size) {
    HLE_TRACE("stack=0x%" PRIx64, size);
    if (!attr || !*attr) {
        return err::kEINVAL;
    }
    (*attr)->stack_size = size;
    return err::OK;
}
HLE_OVERRIDE(scePthreadAttrSetstacksize);

HLE_IMPL(int, scePthreadAttrSetaffinity, GuestThreadAttr* attr, u64 mask) {
    HLE_TRACE("mascara=0x%" PRIx64, mask);
    if (!attr || !*attr) {
        return err::kEINVAL;
    }
    (*attr)->affinity = mask;
    return err::OK;
}
HLE_OVERRIDE(scePthreadAttrSetaffinity);

HLE_IMPL(int, scePthreadAttrSetdetachstate, GuestThreadAttr* attr, int state) {
    HLE_TRACE("detach=%d", state);
    if (!attr || !*attr) {
        return err::kEINVAL;
    }
    (*attr)->detach_state = state;
    return err::OK;
}
HLE_OVERRIDE(scePthreadAttrSetdetachstate);

HLE_IMPL(void, scePthreadYield) {
    HLE_QUIET("");
    SwitchToThread();
}
HLE_OVERRIDE(scePthreadYield);

// ---- threads ---------------------------------------------------------------
constexpr u32 kThreadMagic = 0x54485231; // "THR1"
// O jogo pede pilhas minusculas (ex.: 32 KiB) dimensionadas para o codigo dele; os overrides
// HLE e as APIs do Windows rodam nessa mesma pilha e precisam de mais.
constexpr u64 kMinHostStack = 1 << 20;

using ThreadEntry = void* (HLE_SYSV_ABI*)(void*);

struct HostThread {
    u32 magic = kThreadMagic;
    HANDLE handle = nullptr;
    ThreadEntry entry = nullptr;
    void* arg = nullptr;
    void* result = nullptr;
    char name[32] = {};
    runtime::GuestThread* registry = nullptr;
};

thread_local HostThread* t_self = nullptr;

void FinishThread(HostThread* thread, void* result) {
    thread->result = result;
    if (thread->registry) {
        thread->registry->finished.store(true);
    }
    runtime::ReleaseTlsForCurrentThread();
}

DWORD WINAPI ThreadTrampoline(void* param) {
    auto* thread = static_cast<HostThread*>(param);
    t_self = thread;
    thread->registry = runtime::RegisterCurrentThread(thread->name);
    // Antes de qualquer instrucao do jogo: TCB/TLS proprio no slot que o patcher de FS le.
    if (!runtime::SetupTlsForCurrentThread()) {
        log::Fatal("thread '%s': could not initialize Orbis TLS", thread->name);
    }
    log::Call("scePthreadCreate", reinterpret_cast<u64>(thread->entry), "thread '%s' started, tcb=%p, arg=%p",
              thread->name, static_cast<void*>(runtime::CurrentTcb()), thread->arg);
    void* result = thread->entry(thread->arg);
    log::Call("scePthreadCreate", reinterpret_cast<u64>(thread->entry), "thread '%s' returned %p", thread->name,
              result);
    FinishThread(thread, result);
    return 0;
}

HLE_IMPL(int, scePthreadCreate, HostThread** out, const GuestThreadAttr* attr, ThreadEntry entry, void* arg,
         const char* name) {
    if (!out || !entry) {
        return err::kEINVAL;
    }
    auto* thread = new HostThread;
    thread->entry = entry;
    thread->arg = arg;
    std::strncpy(thread->name, name && *name ? name : "unnamed", sizeof(thread->name) - 1);
    const u64 requested = (attr && *attr) ? (*attr)->stack_size : 0;
    const u64 stack = requested > kMinHostStack ? requested : kMinHostStack;
    // Suspensa ate *out estar gravado: a thread pode ler o proprio handle logo no inicio.
    // Sem STACK_SIZE_PARAM_IS_A_RESERVATION o tamanho e de COMMIT: a pilha inteira ja nasce
    // utilizavel. Isso e obrigatorio - o codigo do Orbis nao faz stack probing (nao ha __chkstk),
    // entao uma funcao com "sub rsp, 0x5868" pula a guard page do Windows e o acesso seguinte
    // cai em memoria so reservada: access violation sem pilha valida para o VEH rodar.
    thread->handle = CreateThread(nullptr, stack, ThreadTrampoline, thread, CREATE_SUSPENDED, nullptr);
    if (!thread->handle) {
        delete thread;
        HLE_TRACE("\"%s\" -> CreateThread failed (%lu)", name ? name : "", GetLastError());
        return err::kEAGAIN;
    }
    *out = thread;
    HLE_TRACE("\"%s\" entry=%p arg=%p requested stack=0x%" PRIx64 " (host 0x%" PRIx64 ")", thread->name,
              reinterpret_cast<void*>(entry), arg, requested, stack);
    ResumeThread(thread->handle);
    return err::OK;
}
HLE_OVERRIDE(scePthreadCreate);

HLE_IMPL(int, scePthreadJoin, HostThread* thread, void** result) {
    if (!thread || thread->magic != kThreadMagic) {
        return err::kEINVAL;
    }
    HLE_TRACE("\"%s\"", thread->name);
    {
        runtime::WaitScope waiting("scePthreadJoin");
        WaitForSingleObject(thread->handle, INFINITE);
    }
    if (result) {
        *result = thread->result;
    }
    CloseHandle(thread->handle);
    thread->magic = 0;
    delete thread;
    return err::OK;
}
HLE_OVERRIDE(scePthreadJoin);

HLE_IMPL(void, scePthreadExit, void* result) {
    HLE_TRACE("%p", result);
    if (!t_self) {
        std::printf("\nThe main thread called scePthreadExit.\n");
        log::Shutdown(0);
    }
    FinishThread(t_self, result);
    ExitThread(0);
}
HLE_OVERRIDE(scePthreadExit);

// Prioridades do Orbis nao sao mapeadas: todas as threads rodam com prioridade normal.
HLE_IMPL(int, scePthreadSetprio, HostThread* thread, int priority) {
    HLE_TRACE("\"%s\" prio=%d (ignored)", thread && thread->magic == kThreadMagic ? thread->name : "?", priority);
    return err::OK;
}
HLE_OVERRIDE(scePthreadSetprio);

} // namespace
