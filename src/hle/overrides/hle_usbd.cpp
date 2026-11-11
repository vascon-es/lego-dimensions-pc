// libSceUsbd - os 20 simbolos importados. A API e a da libusb 1.0 com outro prefixo; os
// layouts abaixo (device/config/interface/endpoint descriptor, libusb_transfer) sao os da
// libusb e batem com os offsets que o eboot le (FASE5_callsites_usbd.txt):
//   descritor de device: idVendor em +8, idProduct em +10, bNumConfigurations em +17
//   interface: num_altsetting em +8; altsetting: bInterfaceNumber +2, bNumEndpoints +4,
//              endpoint* em +0x10; endpoint (0x20 bytes): bEndpointAddress +2, bmAttributes +3
//
// Por padrao nenhum dispositivo esta conectado. Com --toypad aparece um LEGO Dimensions Toy Pad
// virtual (0e6f:0241). Cada pacote de saida (endpoint 0x01) e entregue ao emulador do portal
// (hle_toypad.cpp) e a resposta completa uma das leituras pendentes do endpoint 0x81, cujo
// callback roda em sceUsbdHandleEvents.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "hle/overrides/hle_override.h"
#include "hle/overrides/hle_toypad.h"
#include "hle/overrides/hle_toypad_hid.h"
#include "runtime/crash_handler.h"

namespace hle {

namespace {

constexpr int kErrorInvalidParam = static_cast<int>(0x80240002);
constexpr int kErrorNoDevice = static_cast<int>(0x80240004);
constexpr int kErrorNotFound = static_cast<int>(0x80240005);

constexpr u16 kToypadVendor = 0x0E6F;
constexpr u16 kToypadProduct = 0x0241;
constexpr u8 kEndpointIn = 0x81;
constexpr u8 kEndpointOut = 0x01;
constexpr u16 kPacketSize = 32;

// Quanto sceUsbdHandleEvents espera por uma transferencia concluida antes de retornar.
constexpr auto kEventWait = std::chrono::milliseconds(16);

#pragma pack(push, 1)
struct DeviceDescriptor {
    u8 bLength, bDescriptorType;
    u16 bcdUSB;
    u8 bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    u16 idVendor, idProduct, bcdDevice;
    u8 iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
#pragma pack(pop)
static_assert(sizeof(DeviceDescriptor) == 18);

struct EndpointDescriptor {
    u8 bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    u16 wMaxPacketSize;
    u8 bInterval, bRefresh, bSynchAddress;
    const u8* extra;
    int extra_length;
};
static_assert(sizeof(EndpointDescriptor) == 0x20);

struct InterfaceDescriptor {
    u8 bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    u8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
    const EndpointDescriptor* endpoint;
    const u8* extra;
    int extra_length;
};

struct Interface {
    const InterfaceDescriptor* altsetting;
    int num_altsetting;
};

struct ConfigDescriptor {
    u8 bLength, bDescriptorType;
    u16 wTotalLength;
    u8 bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const Interface* interfaces; // "interface" e macro em alguns headers do Windows
    const u8* extra;
    int extra_length;
};

struct Transfer;
using TransferCallback = void (HLE_SYSV_ABI*)(Transfer*);

struct Transfer { // libusb_transfer
    void* dev_handle;
    u8 flags;
    u8 endpoint;
    u8 type;
    u32 timeout;
    int status; // 0 = COMPLETED
    int length;
    int actual_length;
    TransferCallback callback;
    void* user_data;
    u8* buffer;
    int num_iso_packets;
};
static_assert(offsetof(Transfer, callback) == 32 && offsetof(Transfer, buffer) == 48);

// O "device" e o "handle" sao objetos unicos: so existe o Toy Pad virtual.
struct Device {
    u32 magic = 0x55534244; // "USBD"
} g_device;
struct Handle {
    u32 magic = 0x55534248; // "USBH"
} g_handle;

const EndpointDescriptor g_endpoints[2] = {
    {7, 5, kEndpointIn, 3, kPacketSize, 1, 0, 0, nullptr, 0},
    {7, 5, kEndpointOut, 3, kPacketSize, 1, 0, 0, nullptr, 0},
};
const InterfaceDescriptor g_altsetting = {9, 4, 0, 0, 2, 3 /*HID*/, 0, 0, 0, g_endpoints, nullptr, 0};
const Interface g_interface = {&g_altsetting, 1};
const ConfigDescriptor g_config = {9, 2, 41, 1, 1, 0, 0x80, 250, &g_interface, nullptr, 0};
const DeviceDescriptor g_device_descriptor = {18, 1, 0x0200, 0, 0, 0, 32, kToypadVendor, kToypadProduct,
                                              0x0200, 1, 2, 0, 1};

bool g_toypad_connected = false;
bool g_real_toypad = false; // Fase 61: o portal fisico atende o jogo (sem emulador, portal.json nem app)

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Transfer*> g_completed;       // aguardando callback em sceUsbdHandleEvents
std::deque<Transfer*> g_pending_reads;   // leituras do endpoint IN ainda sem dado
std::deque<toypad::Packet> g_responses;  // respostas do portal ainda sem leitura para recebe-las
// Fase 60: avisos de figura colocada / removida esperam numa fila propria e so saem quando nao ha resposta
// de comando pendente (como no portal emulado do Cemu): o jogo recebe a resposta do que acabou de pedir
// antes de um aviso, e nenhum aviso fica no lugar de uma resposta.
std::deque<toypad::Packet> g_events;
u64 g_reads_completed = 0;

// Entrega uma resposta do portal a uma leitura. Chamar com g_mutex travado.
void CompleteRead(Transfer* transfer, const toypad::Packet& packet) {
    const int n = transfer->length < 32 ? transfer->length : 32;
    std::memcpy(transfer->buffer, packet.data(), n);
    transfer->actual_length = n;
    transfer->status = 0;
    g_completed.push_back(transfer);
    ++g_reads_completed;
}

HLE_IMPL(int, sceUsbdInit) {
    HLE_TRACE("Virtual Toy Pad %s", g_toypad_connected ? "CONNECTED (--toypad)" : "desconectado");
    return err::OK;
}
HLE_OVERRIDE(sceUsbdInit);

HLE_IMPL(void, sceUsbdExit) {
    HLE_TRACE("");
    std::lock_guard lock(g_mutex);
    g_completed.clear();
    g_pending_reads.clear();
    g_responses.clear();
    g_events.clear();
}
HLE_OVERRIDE(sceUsbdExit);

// Devolve a contagem e uma lista terminada em nullptr, como libusb_get_device_list.
HLE_IMPL(s64, sceUsbdGetDeviceList, Device*** list) {
    if (!list) {
        return kErrorInvalidParam;
    }
    const int count = g_toypad_connected ? 1 : 0;
    auto** devices = new Device*[2]{g_toypad_connected ? &g_device : nullptr, nullptr};
    *list = devices;
    HLE_QUIET("-> %d device(s)", count);
    return count;
}
HLE_OVERRIDE(sceUsbdGetDeviceList);

HLE_IMPL(void, sceUsbdFreeDeviceList, Device** list, int unref_devices) {
    HLE_QUIET("%p", static_cast<void*>(list));
    delete[] list;
}
HLE_OVERRIDE(sceUsbdFreeDeviceList);

HLE_IMPL(int, sceUsbdGetDeviceDescriptor, Device* device, DeviceDescriptor* descriptor) {
    HLE_TRACE("%p -> %04x:%04x", static_cast<void*>(device), kToypadVendor, kToypadProduct);
    if (device != &g_device || !descriptor) {
        return kErrorInvalidParam;
    }
    *descriptor = g_device_descriptor;
    return err::OK;
}
HLE_OVERRIDE(sceUsbdGetDeviceDescriptor);

HLE_IMPL(int, sceUsbdGetBusNumber, Device* device) {
    HLE_QUIET("%p", static_cast<void*>(device));
    return 1;
}
HLE_OVERRIDE(sceUsbdGetBusNumber);

HLE_IMPL(int, sceUsbdGetDeviceAddress, Device* device) {
    HLE_QUIET("%p", static_cast<void*>(device));
    return 2;
}
HLE_OVERRIDE(sceUsbdGetDeviceAddress);

HLE_IMPL(Handle*, sceUsbdOpenDeviceWithVidPid, u16 vendor, u16 product) {
    const bool match = g_toypad_connected && vendor == kToypadVendor && product == kToypadProduct;
    HLE_TRACE("%04x:%04x -> %s", vendor, product, match ? "opened" : "not found");
    return match ? &g_handle : nullptr;
}
HLE_OVERRIDE(sceUsbdOpenDeviceWithVidPid);

HLE_IMPL(int, sceUsbdGetConfigDescriptor, Device* device, u8 index, const ConfigDescriptor** config) {
    HLE_TRACE("config %u", index);
    if (device != &g_device || !config) {
        return kErrorInvalidParam;
    }
    if (index != 0) {
        return kErrorNotFound;
    }
    *config = &g_config; // estatico: FreeConfigDescriptor nao libera nada
    return err::OK;
}
HLE_OVERRIDE(sceUsbdGetConfigDescriptor);

HLE_IMPL(void, sceUsbdFreeConfigDescriptor, const ConfigDescriptor* config) {
    HLE_QUIET("%p", static_cast<const void*>(config));
}
HLE_OVERRIDE(sceUsbdFreeConfigDescriptor);

// O jogo exige 1 aqui ("driver do kernel ativo") para seguir com Detach + Claim.
HLE_IMPL(int, sceUsbdKernelDriverActive, Handle* handle, int interface_number) {
    HLE_TRACE("interface %d -> 1", interface_number);
    return handle == &g_handle ? 1 : kErrorNoDevice;
}
HLE_OVERRIDE(sceUsbdKernelDriverActive);

HLE_IMPL(int, sceUsbdDetachKernelDriver, Handle* handle, int interface_number) {
    HLE_TRACE("interface %d", interface_number);
    return handle == &g_handle ? err::OK : kErrorNoDevice;
}
HLE_OVERRIDE(sceUsbdDetachKernelDriver);

HLE_IMPL(int, sceUsbdClaimInterface, Handle* handle, int interface_number) {
    HLE_TRACE("interface %d", interface_number);
    return handle == &g_handle ? err::OK : kErrorNoDevice;
}
HLE_OVERRIDE(sceUsbdClaimInterface);

HLE_IMPL(int, sceUsbdResetDevice, Handle* handle) {
    HLE_TRACE("%p", static_cast<void*>(handle));
    std::lock_guard lock(g_mutex);
    g_pending_reads.clear(); // o jogo realoca e rearma as transferencias depois do reset
    g_responses.clear();
    g_events.clear();
    return handle == &g_handle ? err::OK : kErrorNoDevice;
}
HLE_OVERRIDE(sceUsbdResetDevice);

HLE_IMPL(int, sceUsbdCheckConnected, Handle* handle) {
    HLE_QUIET("%p", static_cast<void*>(handle));
    return (handle == &g_handle && g_toypad_connected) ? err::OK : kErrorNoDevice;
}
HLE_OVERRIDE(sceUsbdCheckConnected);

HLE_IMPL(int, sceUsbdGetMaxPacketSize, Device* device, u8 endpoint) {
    HLE_TRACE("endpoint 0x%02x -> %u", endpoint, kPacketSize);
    return kPacketSize;
}
HLE_OVERRIDE(sceUsbdGetMaxPacketSize);

HLE_IMPL(Transfer*, sceUsbdAllocTransfer, int iso_packets) {
    HLE_QUIET("%d", iso_packets);
    return new Transfer{};
}
HLE_OVERRIDE(sceUsbdAllocTransfer);

// user_data e timeout sao o 7o e 8o argumentos: chegam pela pilha.
HLE_IMPL(void, sceUsbdFillInterruptTransfer, Transfer* transfer, void* dev_handle, u8 endpoint, u8* buffer,
         int length, TransferCallback callback, void* user_data, u32 timeout) {
    HLE_QUIET("endpoint 0x%02x, %d bytes", endpoint, length);
    transfer->dev_handle = dev_handle;
    transfer->endpoint = endpoint;
    transfer->type = 3; // LIBUSB_TRANSFER_TYPE_INTERRUPT
    transfer->timeout = timeout;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->user_data = user_data;
    transfer->callback = callback;
}
HLE_OVERRIDE(sceUsbdFillInterruptTransfer);

HLE_IMPL(int, sceUsbdSubmitTransfer, Transfer* transfer) {
    HLE_QUIET("endpoint 0x%02x, %d bytes", transfer ? transfer->endpoint : 0, transfer ? transfer->length : 0);
    if (!transfer || transfer->dev_handle != &g_handle || !transfer->buffer) {
        return kErrorNoDevice;
    }
    if (transfer->endpoint & 0x80) {
        // Leitura: recebe uma resposta ja pronta, ou fica pendente ate o portal ter o que dizer.
        std::lock_guard lock(g_mutex);
        if (!g_responses.empty()) {
            CompleteRead(transfer, g_responses.front());
            g_responses.pop_front();
            g_cv.notify_all();
        } else if (!g_events.empty()) {
            CompleteRead(transfer, g_events.front());
            g_events.pop_front();
            g_cv.notify_all();
        } else {
            g_pending_reads.push_back(transfer);
        }
        return err::OK;
    }
    if (g_real_toypad) { // Fase 61: o comando vai para o portal fisico; a resposta chega pela leitura do HID
        const bool sent = toypadhid::Write(transfer->buffer, static_cast<u32>(transfer->length));
        {
            std::lock_guard lock(g_mutex);
            transfer->status = sent ? 0 : 4; // 4 = LIBUSB_TRANSFER_NO_DEVICE
            transfer->actual_length = sent ? transfer->length : 0;
            g_completed.push_back(transfer);
        }
        g_cv.notify_all();
        return err::OK;
    }
    // Escrita: o comando vai para o portal (fora do lock: ele registra no log).
    const std::optional<toypad::Packet> reply =
        toypad::HandleCommand(transfer->buffer, static_cast<u32>(transfer->length));
    {
        std::lock_guard lock(g_mutex);
        transfer->status = 0;
        transfer->actual_length = transfer->length;
        g_completed.push_back(transfer);
        if (reply) {
            if (!g_pending_reads.empty()) {
                Transfer* read = g_pending_reads.front();
                g_pending_reads.pop_front();
                CompleteRead(read, *reply);
            } else {
                g_responses.push_back(*reply);
            }
        }
    }
    g_cv.notify_all();
    return err::OK;
}
HLE_OVERRIDE(sceUsbdSubmitTransfer);

// Bloqueia ate haver transferencia concluida ou passar kEventWait; os callbacks rodam aqui,
// na thread que chamou (usb_events), como na libusb.
HLE_IMPL(int, sceUsbdHandleEvents) {
    HLE_QUIET("");
    std::deque<Transfer*> ready;
    {
        runtime::WaitScope waiting("sceUsbdHandleEvents");
        std::unique_lock lock(g_mutex);
        g_cv.wait_for(lock, kEventWait, [] { return !g_completed.empty(); });
        ready.swap(g_completed);
    }
    for (Transfer* transfer : ready) {
        if (transfer->callback) {
            transfer->callback(transfer);
        }
    }
    return err::OK;
}
HLE_OVERRIDE(sceUsbdHandleEvents);

// ---- portal.json ------------------------------------------------------------
// {"pad1": {"uid": [4,163,55,18,34,55,66], "id": 1}, "pad2": null, "pad3": null}
// pad1 = centro, pad2 = esquerda, pad3 = direita; "id" = numero do personagem (1 = Batman).
struct PadConfig {
    bool present = false;
    toypad::Uid uid{};
    u32 id = 0;
    bool operator==(const PadConfig&) const = default;
};

// Parser minimo, so para este formato. Entradas malformadas contam como zona vazia.
PadConfig ParsePad(const std::string& json, int pad) {
    PadConfig config;
    const std::string key = "\"pad" + std::to_string(pad) + "\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return config;
    }
    pos = json.find(':', pos + key.size());
    if (pos == std::string::npos) {
        return config;
    }
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos] != '{') {
        return config; // null
    }
    const size_t end = json.find('}', pos);
    if (end == std::string::npos) {
        return config;
    }
    const std::string object = json.substr(pos, end - pos);
    const size_t uid_pos = object.find("\"uid\"");
    const size_t id_pos = object.find("\"id\"");
    if (uid_pos == std::string::npos || id_pos == std::string::npos) {
        return config;
    }
    const size_t open = object.find('[', uid_pos);
    const size_t close = object.find(']', uid_pos);
    if (open == std::string::npos || close == std::string::npos) {
        return config;
    }
    size_t cursor = open + 1;
    for (int i = 0; i < 7; ++i) {
        cursor = object.find_first_of("0123456789", cursor);
        if (cursor == std::string::npos || cursor > close) {
            return config;
        }
        config.uid[i] = static_cast<u8>(std::strtoul(object.c_str() + cursor, nullptr, 10));
        cursor = object.find_first_not_of("0123456789", cursor);
    }
    const size_t id_digits = object.find_first_of("0123456789", object.find(':', id_pos));
    if (id_digits == std::string::npos) {
        return config;
    }
    config.id = static_cast<u32>(std::strtoul(object.c_str() + id_digits, nullptr, 10));
    config.present = true;
    return config;
}

std::filesystem::path g_portal_file;
PadConfig g_applied[toypad::kPadCount];
bool g_was_ready = false;
u32 g_seen_session = 0; // Fase 60: ultima sessao em que as figuras foram anunciadas
// Fase 27: o app LegoToypad assumiu o portal (mandou LOAD / REMOVE / MOVE). O portal.json deixa de
// valer; as figuras que ele tinha colocado saem na proxima passada da vigia.
std::atomic<bool> g_external_control{false};
bool g_external_applied = false;

// Entrega um pacote espontaneo do portal (evento de figura) ao jogo.
void InjectPacket(const toypad::Packet& packet) {
    {
        std::lock_guard lock(g_mutex);
        if (!g_pending_reads.empty()) {
            Transfer* read = g_pending_reads.front();
            g_pending_reads.pop_front();
            CompleteRead(read, packet);
        } else {
            g_events.push_back(packet);
        }
    }
    g_cv.notify_all();
}

// Roda a cada 500 ms em thread propria: compara o arquivo com o que ja foi anunciado ao jogo.
void PortalWatcher() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const u32 session = toypad::SessionNumber();
        if (!session) {
            g_was_ready = false;
            continue;
        }
        if (session != g_seen_session) { // sessao nova, mesmo que tenha fechado e aberto entre duas passadas
            g_seen_session = session;
            g_was_ready = false;
        }
        if (!g_was_ready) {
            g_was_ready = true;
            for (const toypad::Packet& event : toypad::PresentEvents()) {
                InjectPacket(event);
            }
        }
        if (g_external_control) {
            continue;
        }
        std::ifstream in(g_portal_file, std::ios::binary);
        if (!in) {
            continue;
        }
        const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (int pad = 1; pad <= toypad::kPadCount; ++pad) {
            const PadConfig wanted = ParsePad(json, pad);
            PadConfig& applied = g_applied[pad - 1];
            if (wanted == applied) {
                continue;
            }
            if (applied.present) {
                if (const auto event = toypad::RemoveFigure(pad)) {
                    log::Tagged("PORTAL", "figure REMOVED from zone %d (id %u)", pad, applied.id);
                    InjectPacket(*event);
                }
            }
            if (wanted.present) {
                if (const auto event = toypad::PlaceFigure(pad, wanted.uid, wanted.id)) {
                    log::Tagged("PORTAL", "figure PLACED in zone %d: id %u, uid %02x %02x %02x %02x %02x %02x %02x", pad,
                                wanted.id, wanted.uid[0], wanted.uid[1], wanted.uid[2], wanted.uid[3], wanted.uid[4],
                                wanted.uid[5], wanted.uid[6]);
                    InjectPacket(*event);
                }
            }
            applied = wanted;
        }
    }
}

} // namespace

void InjectToypadPacket(const toypad::Packet& packet) {
    InjectPacket(packet);
}

void TakeToypadExternalControl() {
    if (!g_external_control.exchange(true)) {
        log::Tagged("PORTAL", "LegoToypad app active on portal");
    }
}

void SetVirtualToypadConnected(bool connected) {
    g_toypad_connected = connected;
}

// Fase 61: tudo o que o portal fisico manda (respostas e avisos de figura) vai para as leituras do jogo, na ordem.
void RealToypadPacket(const u8* packet, u32 length) {
    toypad::Packet copy{};
    std::memcpy(copy.data(), packet, std::min<u32>(length, 32));
    InjectPacket(copy);
}

bool UseRealToypad() {
    std::string description;
    const bool opened = toypadhid::Open(&description);
    std::printf("[TOYPAD-USB] %s\n", description.c_str());
    if (opened) {
        g_real_toypad = true;
        g_toypad_connected = true;
        toypadhid::Start(&RealToypadPacket);
    }
    return opened;
}

// Le uma zona ("pad1".."pad3") de um texto no formato do portal.json. Devolve false se a zona
// estiver vazia (null), ausente ou malformada. As tres zonas sao independentes.
bool ParsePortalJson(const std::string& json, int pad, std::array<u8, 7>* uid, u32* id) {
    const PadConfig config = ParsePad(json, pad);
    if (!config.present) {
        return false;
    }
    *uid = config.uid;
    *id = config.id;
    return true;
}

// Liga a vigia de `portal_file` (criado com as tres zonas vazias se nao existir).
void StartPortalWatcher(const std::filesystem::path& portal_file) {
    g_portal_file = portal_file;
    toypad::SetPortalFilePath(portal_file);
    std::error_code ec;
    if (!std::filesystem::exists(portal_file, ec)) {
        std::ofstream out(portal_file);
        // Trio inicial do jogo: Batman no centro, Gandalf a esquerda, Megaestilo a direita.
        out << "{\"pad1\": {\"uid\": [4,163,55,18,34,55,66], \"id\": 1},\n"
               " \"pad2\": {\"uid\": [4,81,112,26,91,64,129], \"id\": 2},\n"
               " \"pad3\": {\"uid\": [4,202,17,74,153,54,128], \"id\": 3}}\n";
    }
    toypad::LoadPortalState(portal_file);
    std::thread(PortalWatcher).detach();
}

} // namespace hle
