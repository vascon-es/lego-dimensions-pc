// shader_compiler_tool - recompilacao estatica (AOT) GCN -> SPIR-V dos shaders extraidos do jogo.
//
// Le assets/dumped_shaders/index.tsv (um shader por linha, com os registradores do estagio) e
// fetch_index.tsv (pares vertex shader <-> fetch shader vistos em draws), passa cada binario
// pelo shader_recompiler do shadPS4 e grava em assets/shaders_spv/:
//   <estagio>_<hash>[_<hash do fetch>].spv    modulo SPIR-V
//   <mesmo nome>.json                         metadados deduzidos pelo recompilador (buffers,
//                                             texturas, samplers, atributos de vertice, bindings)
//   recompile_report.tsv                      uma linha por shader: ok / motivo da falha
//
// O que e conhecido de forma estatica vem do index.tsv. O que no emulador viria do estado da
// GPU no momento do draw (mapeamento das entradas do pixel shader, formato dos alvos de cor,
// conteudo dos descritores) entra com valores padrao - ver FillRuntimeInfo. Os SPIR-V servem
// para medir a cobertura do recompilador e como base da pipeline; antes de usar um modulo com
// um estado diferente do padrao ele tem de ser recompilado com o RuntimeInfo real.
//
// uso: shader_compiler_tool [pasta dumped_shaders] [pasta shaders_spv] [--pairs-only ARQUIVO | --serve]
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include "../../src/common/shader_metadata.h"

#include "shader_recompiler/backend/bindings.h"
#include <magic_enum/magic_enum.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/abstract_syntax_list.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/regs_shader.h"

#include "tool_support.h"

namespace fs = std::filesystem;

namespace {

struct ShaderEntry {
    std::string stage; // "vs" / "ps"
    std::string hash;
    u32 code_bytes = 0;
    std::vector<u32> regs;
};

struct FetchPair {
    std::string fetch_hash;
    int sgpr = -1;
};

struct Result {
    bool ok = false;
    bool translation_failed = false;
    std::string reason;
    std::vector<u32> spv;
    std::string metadata;
};

std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, sep)) {
        out.push_back(item);
    }
    return out;
}

// Operando GCN em texto: v12, s4, vcc_lo, 0x3f800000 (1.0), -2, 0.5...
std::string OperandText(const Shader::Gcn::InstOperand& op) {
    using Shader::Gcn::OperandField;
    char buf[64];
    const u32 c = op.code;
    switch (op.field) {
    case OperandField::VectorGPR:
        std::snprintf(buf, sizeof(buf), "v%u", c);
        break;
    case OperandField::ScalarGPR:
        std::snprintf(buf, sizeof(buf), "s%u", c);
        break;
    case OperandField::LiteralConst: {
        float f;
        std::memcpy(&f, &c, 4);
        std::snprintf(buf, sizeof(buf), "0x%08x(%g)", c, f);
        break;
    }
    case OperandField::SignedConstIntPos:
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(c) - 128);
        break;
    case OperandField::SignedConstIntNeg:
        std::snprintf(buf, sizeof(buf), "%d", -(static_cast<int>(c) - 192));
        break;
    case OperandField::Undefined:
        return "";
    default:
        std::snprintf(buf, sizeof(buf), "%s", std::string(magic_enum::enum_name(op.field)).c_str());
        break;
    }
    std::string s = buf;
    if (op.input_modifier.abs) {
        s = "|" + s + "|";
    }
    if (op.input_modifier.neg) {
        s = "-" + s;
    }
    return s;
}

void DumpGcn(std::span<const u32> code, const fs::path& path) {
    std::ofstream out(path);
    Shader::Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Shader::Gcn::GcnDecodeContext decoder;
    u32 pc = 0;
    while (!slice.atEnd()) {
        const Shader::Gcn::GcnInst inst = decoder.decodeInstruction(slice);
        out << std::hex << pc << std::dec << ": " << magic_enum::enum_name(inst.opcode);
        for (u32 i = 0; i < inst.dst_count; ++i) {
            out << (i ? ", " : " ") << OperandText(inst.dst[i]);
        }
        for (u32 i = 0; i < inst.src_count; ++i) {
            out << ((i || inst.dst_count) ? ", " : " ") << OperandText(inst.src[i]);
        }
        if (inst.inst_class == Shader::Gcn::InstClass::ScalarMemRd) {
            out << " off=" << inst.control.smrd.offset << (inst.control.smrd.imm ? " imm" : "");
        }
        if (inst.encoding == Shader::Gcn::InstEncoding::VOP3 && inst.dst[0].output_modifier.clamp) {
            out << " clamp";
        }
        if (inst.encoding == Shader::Gcn::InstEncoding::SOPP) {
            out << " simm=" << inst.control.sopp.simm << " -> " << std::hex << inst.BranchTarget(pc) << std::dec;
        }
        out << '\n';
        pc += inst.length;
        if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
}

// Le um binario para um buffer de dwords com folga zerada no fim (o decodificador pode olhar um
// pouco alem do ultimo dword).
bool LoadWords(const fs::path& path, std::vector<u32>* words, size_t* dwords) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    *dwords = bytes.size() / 4;
    words->assign(*dwords + 64, 0);
    std::memcpy(words->data(), bytes.data(), *dwords * 4);
    return true;
}

Shader::Profile MakeProfile() {
    Shader::Profile p{};
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = 65536;
    p.supported_spirv = 0x00010600;
    p.subgroup_size = 4;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_float16 = p.support_float64 = true;
    p.supports_denorm_behavior_independence = true;
    p.supports_rounding_mode_independence = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = p.support_fp32_round_to_zero = true;
    p.support_fp32_signed_zero_inf_nan_preserve = true;
    p.supports_image_load_store_lod = true;
    p.supports_buffer_int64_atomics = true;
    p.supports_shared_int64_atomics = true;
    p.supports_workgroup_explicit_memory_layout = true;
    // Sem baricentricas explicitas: os modulos nao exigem VK_KHR_fragment_shader_barycentric.
    p.supports_fragment_shader_barycentric = false;
    p.supports_shader_stencil_export = true;
    return p;
}

// RSRC1/RSRC2 do estagio (iguais para VS e PS): quantos registradores o shader usa e os modos
// de ponto flutuante.
void FillCommon(Shader::RuntimeInfo& rt, const std::vector<u32>& regs) {
    const u32 rsrc1 = regs.size() > 2 ? regs[2] : 0, rsrc2 = regs.size() > 3 ? regs[3] : 0;
    rt.props.num_user_data = (rsrc2 >> 1) & 0x1F;
    rt.props.num_input_vgprs = (rsrc1 >> 24) & 0x3;
    rt.props.num_allocated_vgprs = ((rsrc1 & 0x3F) + 1) * 4;
    rt.props.fp_round_mode32 = static_cast<AmdGpu::FpRoundMode>((rsrc1 >> 12) & 3);
    rt.props.fp_round_mode16_64 = static_cast<AmdGpu::FpRoundMode>((rsrc1 >> 14) & 3);
    rt.props.fp_denorm_mode32 = static_cast<AmdGpu::FpDenormMode>((rsrc1 >> 16) & 3);
    rt.props.fp_denorm_mode16_64 = static_cast<AmdGpu::FpDenormMode>((rsrc1 >> 18) & 3);
}

void FillRuntimeInfo(Shader::RuntimeInfo& rt, const ShaderEntry& entry) {
    const bool vertex = entry.stage == "vs";
    rt.Initialize(vertex ? Shader::HwStage::Vertex : Shader::HwStage::Fragment,
                  vertex ? Shader::SwStage::Vertex : Shader::SwStage::Fragment);
    FillCommon(rt, entry.regs);
    if (vertex) {
        // regs: PGM_LO, PGM_HI, RSRC1, RSRC2, SPI_VS_OUT_CONFIG, SPI_SHADER_POS_FORMAT, PA_CL_VS_OUT_CNTL.
        // Saidas extras (point size, clip/cull distance) dependem de PA_CL_VS_OUT_CNTL; aqui
        // ficam desligadas (padrao), e o valor do registrador vai para os metadados.
        rt.hw.vs.num_outputs = 0;
        rt.hw.vs.clip_disable = false;
        return;
    }
    // regs: PGM_LO, PGM_HI, RSRC1, RSRC2, Z_FORMAT, COL_FORMAT, INPUT_ENA, INPUT_ADDR, IN_CONTROL,
    //       BARYC_CNTL, DB_SHADER_CONTROL, CB_SHADER_MASK
    const auto reg = [&](size_t i) { return entry.regs.size() > i ? entry.regs[i] : 0u; };
    static_assert(sizeof(AmdGpu::PsInput) == 4);
    const u32 ena = reg(6), addr = reg(7);
    std::memcpy(&rt.hw.fs.en_flags, &ena, 4);
    std::memcpy(&rt.hw.fs.addr_flags, &addr, 4);
    rt.hw.fs.num_inputs = reg(8) & 0x3F;
    for (u32 i = 0; i < rt.hw.fs.num_inputs && i < 32; ++i) {
        // SPI_PS_INPUT_CNTL_n nao esta no index.tsv: entrada i <- parametro i do VS (padrao).
        rt.hw.fs.inputs[i] = {.param_index = static_cast<u8>(i), .is_default = false, .is_flat = false, .default_value = 0};
    }
    for (u32 i = 0; i < Shader::MaxColorBuffers; ++i) {
        if (((reg(11) >> (i * 4)) & 0xF) == 0) {
            continue;
        }
        auto& cb = rt.hw.fs.color_buffers[i];
        cb.data_format = AmdGpu::DataFormat::Format8_8_8_8; // formato real do alvo: so no draw
        cb.num_format = AmdGpu::NumberFormat::Unorm;
        cb.export_format = static_cast<AmdGpu::ShaderExportFormat>((reg(5) >> (i * 4)) & 0xF);
        cb.swizzle = AmdGpu::IdentityMapping;
    }
    rt.hw.fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(reg(4) & 0xF);
    const u32 db = reg(10);
    rt.hw.fs.mrtz_mask = static_cast<u8>((db & 1) | ((((db >> 1) | (db >> 2)) & 1) << 1) | (((db >> 8) & 1) << 2) |
                                         (((db >> 11) & 1) << 3));
}

template <typename T>
std::string SharpJson(const Shader::SharpFetch<T>& fetch) {
    std::string out = "{\"load_mask\": " + std::to_string(fetch.load_mask) + ", \"offsets\": [";
    for (size_t i = 0; i < fetch.offsets.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(fetch.offsets[i]);
    }
    out += "], \"immediates\": [";
    for (size_t i = 0; i < fetch.immediates.size(); ++i) {
        out += (i ? ", " : "") + std::to_string(fetch.immediates[i]);
    }
    return out + "]}";
}

const char* BufferTypeName(Shader::BufferType type) {
    switch (type) {
    case Shader::BufferType::Guest: return "guest";
    case Shader::BufferType::Flatbuf: return "flatbuf";
    case Shader::BufferType::BdaPagetable: return "bda_pagetable";
    case Shader::BufferType::FaultBuffer: return "fault_buffer";
    case Shader::BufferType::GdsBuffer: return "gds_buffer";
    case Shader::BufferType::SharedMemory: return "shared_memory";
    case Shader::BufferType::ClipPlanes: return "clip_planes";
    }
    return "?";
}

std::string Metadata(const ShaderEntry& entry, const FetchPair* fetch, const Shader::Info& info,
                     const Shader::RuntimeInfo& rt, const Shader::Backend::Bindings& bindings, size_t spv_bytes) {
    std::ostringstream j;
    j << "{\n  \"stage\": \"" << entry.stage << "\",\n  \"hash\": \"" << entry.hash << "\",\n";
    j << "  \"fetch_shader\": " << (fetch ? "\"" + fetch->fetch_hash + "\"" : std::string("null")) << ",\n";
    j << "  \"spirv_bytes\": " << spv_bytes << ",\n  \"gcn_code_bytes\": " << entry.code_bytes << ",\n";
    j << "  \"stage_registers\": [";
    for (size_t i = 0; i < entry.regs.size(); ++i) {
        j << (i ? ", " : "") << entry.regs[i];
    }
    j << "],\n  \"num_user_data\": " << rt.props.num_user_data << ",\n  \"user_data_mask\": " << info.ud_mask.mask << ",\n";
    j << "  \"flattened_user_data_dwords\": " << info.srt_info.flattened_bufsize_dw << ",\n";
    j << "  \"bindings_used\": {\"unified\": " << bindings.unified << ", \"buffer\": " << bindings.buffer
      << ", \"user_data\": " << bindings.user_data << "},\n";
    j << "  \"buffers\": [\n";
    for (size_t i = 0; i < info.buffers.size(); ++i) {
        const auto& b = info.buffers[i];
        j << "    {\"binding\": " << i << ", \"type\": \"" << BufferTypeName(b.buffer_type) << "\", \"used_types\": "
          << static_cast<u32>(b.used_types) << ", \"written\": " << (b.is_written ? "true" : "false") << ", \"formatted\": "
          << (b.is_formatted ? "true" : "false") << ", \"sharp\": " << SharpJson(b.sharp_fetch) << "}"
          << (i + 1 < info.buffers.size() ? "," : "") << "\n";
    }
    j << "  ],\n  \"images\": [\n";
    for (size_t i = 0; i < info.images.size(); ++i) {
        const auto& im = info.images[i];
        j << "    {\"index\": " << i << ", \"depth\": " << (im.is_depth ? "true" : "false") << ", \"array\": "
          << (im.is_array ? "true" : "false") << ", \"written\": " << (im.is_written ? "true" : "false") << ", \"atomic\": "
          << (im.is_atomic ? "true" : "false") << ", \"sharp\": " << SharpJson(im.sharp_fetch) << "}"
          << (i + 1 < info.images.size() ? "," : "") << "\n";
    }
    j << "  ],\n  \"samplers\": [\n";
    for (size_t i = 0; i < info.samplers.size(); ++i) {
        j << "    {\"index\": " << i << ", \"sharp\": " << SharpJson(info.samplers[i].sharp_fetch) << "}"
          << (i + 1 < info.samplers.size() ? "," : "") << "\n";
    }
    j << "  ],\n  \"vertex_attributes\": [\n";
    if (const auto fetch_data = Shader::Gcn::ParseFetchShader(info)) {
        for (size_t i = 0; i < fetch_data->attributes.size(); ++i) {
            const auto& a = fetch_data->attributes[i];
            j << "    {\"semantic\": " << int(a.semantic) << ", \"dest_vgpr\": " << int(a.dest_vgpr) << ", \"num_elements\": "
              << int(a.num_elements) << ", \"table_sgpr\": " << int(a.sgpr_base) << ", \"vsharp_dword_offset\": "
              << int(a.dword_offset) << ", \"instance_data\": " << int(a.instance_data) << "}"
              << (i + 1 < fetch_data->attributes.size() ? "," : "") << "\n";
        }
    }
    j << "  ],\n  \"attributes_loaded\": [";
    bool first = true;
    for (size_t i = 0; i < info.loads.flags.size(); ++i) {
        if (info.loads.flags[i]) {
            j << (first ? "" : ", ") << i;
            first = false;
        }
    }
    j << "],\n  \"attributes_stored\": [";
    first = true;
    for (size_t i = 0; i < info.stores.flags.size(); ++i) {
        if (info.stores.flags[i]) {
            j << (first ? "" : ", ") << i;
            first = false;
        }
    }
    j << "],\n";
    if (entry.stage == "ps") {
        j << "  \"ps_num_inputs\": " << rt.hw.fs.num_inputs << ",\n  \"mrt_mask\": " << int(info.mrt_mask) << ",\n";
    }
    j << "  \"flags\": {\"discard\": " << (info.has_discard ? "true" : "false") << ", \"readconst\": "
      << (info.has_readconst ? "true" : "false") << ", \"direct_memory_access\": " << (info.uses_dma ? "true" : "false")
      << ", \"storage_images\": " << (info.has_storage_images ? "true" : "false") << ", \"translation_failed\": "
      << (info.translation_failed ? "true" : "false") << "}\n}\n";
    return j.str();
}

// Ver MapFakeUserData, mais abaixo.
constexpr u32 kFakeWord = 0x10;
constexpr u64 kFakeBase = static_cast<u64>(kFakeWord) << 32; // 0x10_0000_0000
constexpr u64 kFakeSize = 16ull << 20;

struct Job {
    const ShaderEntry* entry;
    const FetchPair* fetch;
    fs::path shader_dir;
    Result* result;
    // Modo "pares" (Fase 18): o contador de bindings continua do vertex shader para o pixel
    // shader, como no pipeline do shadPS4, e o user data usa a memoria de sondagem para descobrir
    // de onde vem cada descritor. `layout` recebe a descricao do estagio para o pipeline.
    Shader::Backend::Bindings* shared_bindings = nullptr;
    std::string* layout = nullptr;
    // Fase 19: retrato do user data real do estagio (gravado pelo jogo em pair_snapshots.txt).
    // Com ele o SPIR-V sai especializado para os descritores verdadeiros (formato e swizzle dos
    // atributos de vertice, tipo de textura) em vez dos nulos.
    const struct StageSnapshot* snapshot = nullptr;
    // Fase 20: o fetch shader da variante vem do proprio retrato (codigo ate o s_setpc).
    const std::vector<u32>* fetch_code = nullptr;
    int fetch_sgpr = -1;
};

struct StageSnapshot {
    std::array<u32, 16> ud{};
    std::map<u32, std::array<u32, 192>> tables;
};

// ---- memoria de sondagem (modo pares) ----
// Cada registrador de user data j vale 0x100 * (j + 1). O par (i, i+1), lido como ponteiro, cai
// numa regiao propria, preenchida com marcadores (i << 20 | indice do dword). Depois da traducao
// o "user data achatado" do recompilador e montado sobre essa memoria: cada posicao >= 16 traz o
// marcador de onde foi lida, ou seja, (registrador do ponteiro, dword dentro da tabela).
// Os marcadores tem o nibble alto zerado, entao continuam valendo como descritores nulos.
constexpr u32 ProbeRegister(u32 j) {
    return 0x100 * (j + 1);
}

bool MapProbeMemory() {
    for (u32 i = 0; i + 1 < 16; ++i) {
        const u64 pointer = (static_cast<u64>(ProbeRegister(i + 1)) << 32) | ProbeRegister(i);
        const u64 base = pointer & ~0xFFFFull;
        const u64 size = 1ull << 20;
        void* mapped = VirtualAlloc(reinterpret_cast<void*>(base), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mapped != reinterpret_cast<void*>(base)) {
            return false;
        }
        u32* words = static_cast<u32*>(mapped);
        for (u64 w = 0; w < size / 4; ++w) {
            const u64 address = base + w * 4;
            words[w] = address >= pointer ? (i << 20) | static_cast<u32>((address - pointer) / 4) : 0x00FFFFFFu;
        }
    }
    return true;
}

// "ud:R" (descritor embutido a partir do registrador R) ou "tab:R:D" (dword D da tabela apontada
// pelo par de registradores R, R+1), a partir das posicoes do descritor no user data achatado.
template <typename T>
std::string SharpSource(const Shader::SharpFetch<T>& fetch, const std::vector<u32>& flat) {
    const u32 first = fetch.offsets[0];
    if (first == Shader::UNKNOWN_LOCATION || !(fetch.load_mask & 1)) {
        return "?";
    }
    if (first < 16) {
        return "ud:" + std::to_string(first);
    }
    if (first >= flat.size() || (flat[first] >> 24) != 0) {
        return "?";
    }
    return "tab:" + std::to_string(flat[first] >> 20) + ":" + std::to_string(flat[first] & 0xFFFFF);
}

void Recompile(const Job& job) {
    const ShaderEntry& entry = *job.entry;
    Result& result = *job.result;
    std::vector<u32> words, fetch_words;
    size_t dwords = 0, fetch_dwords = 0;
    if (!LoadWords(job.shader_dir / (entry.stage + "_" + entry.hash + ".bin"), &words, &dwords)) {
        result.reason = ".bin file not found";
        return;
    }
    const auto& bininfo = AmdGpu::SearchBinaryInfo(words.data());
    const std::span<const u32> code{words.data(), bininfo.length / sizeof(u32)};

    // User data: ver MapFakeUserData. O unico ponteiro verdadeiro e o do fetch shader
    // (registrador indicado pelo s_swappc do vertex shader).
    std::array<u32, 16> user_data;
    user_data.fill(kFakeWord);
    if (job.layout) {
        for (u32 j = 0; j < 16; ++j) {
            user_data[j] = ProbeRegister(j);
        }
    }
    if (job.snapshot) {
        // Registradores reais; os ponteiros de tabela passam a apontar para as copias locais.
        user_data = job.snapshot->ud;
        for (const auto& [reg, table] : job.snapshot->tables) {
            const u32* pointer = table.data();
            std::memcpy(&user_data[reg], &pointer, sizeof(pointer));
        }
    }
    if (job.fetch_code && job.fetch_sgpr >= 0 && job.fetch_sgpr + 1 < 16) {
        const u32* pointer = job.fetch_code->data();
        std::memcpy(&user_data[job.fetch_sgpr], &pointer, sizeof(pointer));
    } else if (job.fetch) {
        if (!LoadWords(job.shader_dir / ("fs_" + job.fetch->fetch_hash + ".bin"), &fetch_words, &fetch_dwords)) {
            result.reason = "fetch shader .bin not found";
            return;
        }
        const u32* pointer = fetch_words.data();
        std::memcpy(&user_data[job.fetch->sgpr], &pointer, sizeof(pointer));
    }
    const Shader::ShaderParams params{
        .user_data = std::span<const u32, 16>{user_data},
        .code = code,
        .hash = bininfo.shader_hash,
    };
    Shader::RuntimeInfo rt{};
    FillRuntimeInfo(rt, entry);
    Shader::Info info(rt.hw_stage, rt.sw_stage, params);
    static const Shader::Profile profile = MakeProfile();
    Shader::Pools pools;
    Shader::Backend::Bindings local_bindings{};
    Shader::Backend::Bindings& bindings = job.shared_bindings ? *job.shared_bindings : local_bindings;
    const Shader::Backend::Bindings start = bindings;
    try {
        // LD_DUMP_GCN=<hash>: listagem das instrucoes GCN desse shader em gcn_<hash>.txt (operandos v/s, literais)
        if (const char* want = std::getenv("LD_DUMP_GCN"); want && entry.hash == want) {
            DumpGcn(code, job.shader_dir / ("gcn_" + entry.hash + ".txt"));
        }
        const auto program = Shader::TranslateProgram(code, pools, info, rt, profile);
        // LD_DUMP_IR=<hash>: grava o IR final (blocos + lista estruturada) desse shader em ir_<hash>.txt na pasta
        // dos shaders extraidos (depuracao de traducao, ex. lacos que viram Phi/Select)
        if (const char* want = std::getenv("LD_DUMP_IR"); want && entry.hash == want) {
            std::map<const Shader::IR::Block*, size_t> block_to_index;
            std::map<const Shader::IR::Inst*, size_t> inst_to_index;
            size_t index = 0;
            for (const Shader::IR::Block* block : program.blocks) {
                block_to_index.emplace(block, index++);
            }
            std::ofstream dump(job.shader_dir / ("ir_" + entry.hash + ".txt"));
            for (const Shader::IR::Block* block : program.blocks) {
                dump << Shader::IR::DumpBlock(*block, block_to_index, inst_to_index, index) << '\n';
            }
            dump << "\n===== lista estruturada =====\n";
            for (const auto& node : program.syntax_list) {
                dump << Shader::IR::DumpASLNode(node, block_to_index, inst_to_index) << '\n';
            }
        }
        result.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
        if (bindings == start) {
            info.AddBindings(bindings); // garante que o proximo estagio comece depois deste
        }
    } catch (const tool::RecompilerAbort& e) {
        result.reason = std::string(e.what());
        // Fase 48: a mensagem do assert (o que o recompilador nao suporta), numa linha so
        std::string log = tool::TakeLog();
        for (char& c : log) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        if (!log.empty()) {
            result.reason += ": " + log.substr(0, 400);
        }
        return;
    } catch (const std::exception& e) {
        result.reason = std::string("excecao: ") + e.what();
        return;
    }
    result.translation_failed = info.translation_failed;
    if (result.spv.size() < 5 || result.spv[0] != 0x07230203) {
        result.reason = "SPIR-V empty or missing magic number";
        return;
    }
    result.metadata = Metadata(entry, job.fetch, info, rt, bindings, result.spv.size() * 4);
    if (job.layout) {
        // Descricao do estagio para o pipeline: binding de cada recurso e de onde vem o descritor.
        info.RefreshFlatBuf(); // roda o "walker" gerado pelo recompilador sobre a memoria de sondagem
        const std::vector<u32>& flat = info.flattened_ud_buf;
        std::ostringstream out;
        out << "stage " << entry.stage << " hash " << entry.hash << " first_binding " << start.unified << " first_user_data "
            << start.user_data << " ud_mask " << info.ud_mask.mask << " flattened_dwords " << flat.size() << "\n";
        // Fase 32: shaders que leem a tabela de recursos por ponteiros encadeados recebem um buffer
        // "achatado" (flatbuf), montado por um trecho de codigo x86 que o recompilador gera (o
        // "walker": rdi = user data, rsi = destino). O codigo e independente de posicao; vai em
        // hexadecimal para o jogo executar a cada draw.
        bool uses_flatbuf = false;
        for (const auto& buffer : info.buffers) {
            uses_flatbuf |= buffer.buffer_type == Shader::BufferType::Flatbuf;
        }
        if (uses_flatbuf) {
            out << "  srt_walker dwords " << info.srt_info.flattened_bufsize_dw << " hex ";
            const u8* code = reinterpret_cast<const u8*>(info.srt_info.walker_func);
            if (!code || !info.srt_info.walker_func_size) {
                out << "-";
            }
            for (size_t i = 0; code && i < info.srt_info.walker_func_size; ++i) {
                static const char kHex[] = "0123456789abcdef";
                out << kHex[code[i] >> 4] << kHex[code[i] & 15];
            }
            out << "\n";
        }
        u32 binding = start.unified;
        for (const auto& buffer : info.buffers) {
            out << "  buffer binding " << binding++ << " type " << BufferTypeName(buffer.buffer_type) << " src "
                << (buffer.buffer_type == Shader::BufferType::Guest ? SharpSource(buffer.sharp_fetch, flat) : std::string("-")) << "\n";
        }
        for (const auto& image : info.images) {
            out << "  image binding " << binding++ << " src " << SharpSource(image.sharp_fetch, flat) << "\n";
        }
        for (const auto& sampler : info.samplers) {
            out << "  sampler binding " << binding++ << " src " << SharpSource(sampler.sharp_fetch, flat) << "\n";
        }
        if (const auto fetch_data = Shader::Gcn::ParseFetchShader(info)) {
            for (const auto& a : fetch_data->attributes) {
                out << "  attribute location " << int(a.semantic) << " table_sgpr " << int(a.sgpr_base) << " dword " << int(a.dword_offset)
                    << " components " << int(a.num_elements) << " instance " << int(a.instance_data) << " offset " << int(a.inst_offset)
                    << "\n";
            }
        }
        *job.layout = out.str();
    }
    result.ok = true;
}

void RecompileThunk(void* job) {
    // Qualquer excecao C++ que escape (inclusive fora da traducao em si) vira falha deste shader.
    const Job& typed = *static_cast<const Job*>(job);
    try {
        Recompile(typed);
    } catch (const std::exception& e) {
        typed.result->ok = false;
        typed.result->reason = std::string("excecao: ") + e.what();
    } catch (...) {
        typed.result->ok = false;
        typed.result->reason = "unknown exception";
    }
}

// Violacao de acesso dentro do recompilador (ponteiro de user data que nao existe fora do jogo)
// vira falha do shader, nao queda da ferramenta. Excecoes C++ passam direto.
u64 g_fault_address = 0;

int FaultFilter(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_fault_address = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool Guarded(void (*fn)(void*), void* arg, u32* code) {
    __try {
        fn(arg);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        *code = GetExceptionCode();
        return false;
    }
}

// Memoria de mentira para os ponteiros de user data.
//
// O recompilador nao e puramente estatico: durante a traducao ele segue os ponteiros que o jogo
// deixa nos registradores de user data (tabelas de descritores, "extended user data") e le os
// descritores T#/V# para decidir tipos de textura e de buffer. Fora do jogo esses ponteiros nao
// existem. Para a traducao poder terminar, todo registrador de user data vale kFakeWord, de modo
// que qualquer par (lo, hi) forme o ponteiro kFakeBase + kFakeWord, e a regiao apontada e
// preenchida com o mesmo valor: qualquer ponteiro lido de la cai de novo na regiao, e qualquer
// descritor lido de la e invalido (tipo 0), o que o recompilador trata como recurso nulo.
// Consequencia: o SPIR-V sai especializado para recursos NULOS (textura 2D comum, buffer sem
// formato). Serve para medir cobertura e como ponto de partida; para a pipeline final o
// descritor real de cada draw tem de entrar aqui.

bool MapFakeUserData() {
    void* base = VirtualAlloc(reinterpret_cast<void*>(kFakeBase), kFakeSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base != reinterpret_cast<void*>(kFakeBase)) {
        return false;
    }
    u32* words = static_cast<u32*>(base);
    for (u64 i = 0; i < kFakeSize / 4; ++i) {
        words[i] = kFakeWord;
    }
    return true;
}

// Primeira linha util do log do recompilador, encurtada, para o relatorio.
std::string FirstLogLine(const std::string& log) {
    std::string best;
    std::stringstream ss(log);
    std::string line;
    while (std::getline(ss, line)) {
        const size_t marker = line.rfind(": ");
        std::string text = line;
        // formato "[classe] <nivel> (thread) arquivo:linha funcao: mensagem"
        const size_t close = line.find(") ");
        if (close != std::string::npos) {
            text = line.substr(close + 2);
        }
        (void)marker;
        if (text.find("Assertion Failed") != std::string::npos || text.find("Unreachable code") != std::string::npos) {
            std::string next;
            if (std::getline(ss, next) && !next.empty()) {
                text += " " + next;
            }
            return text.substr(0, 220);
        }
        if (best.empty() && !text.empty()) {
            best = text;
        }
    }
    return best.substr(0, 220);
}

// Pares (VS, PS) dos retratos de snapshots_file (gravados pelo jogo). live: so os que ainda nao existem em pairs/.
void TranslatePairs(const fs::path& snapshots_file, bool live, bool quiet, const std::vector<ShaderEntry>& entries,
                    const fs::path& shader_dir, const fs::path& out_dir, u32* done, u32* total) {
    std::error_code ec;
    std::map<std::string, const ShaderEntry*> by_name;
    for (const ShaderEntry& entry : entries) {
        by_name[entry.stage + entry.hash] = &entry;
    }
    // retratos do user data real de cada variante de par: "pair <vs> <ps> <variante>"
    struct PairSnapshot {
        std::array<StageSnapshot, 2> stages;
        std::vector<u32> fetch_code;
        int fetch_sgpr = -1;
    };
    std::map<std::string, PairSnapshot> snapshots;
    {
        std::ifstream snap(snapshots_file);
        std::string text, key;
        while (std::getline(snap, text)) {
            std::istringstream fields(text);
            std::string a, b, c, d;
            fields >> a >> b >> c;
            if (a == "pair") {
                fields >> d;
                key = d.empty() ? std::string() : b + "_" + c + "_" + d;
                if (!key.empty()) {
                    snapshots[key] = {}; // o mesmo par pode aparecer duas vezes no arquivo: vale o ultimo
                }
                continue;
            }
            if (key.empty()) {
                continue;
            }
            if (a == "fetch") {
                PairSnapshot& pair = snapshots[key];
                pair.fetch_sgpr = std::stoi(b);
                pair.fetch_code.push_back(static_cast<u32>(std::stoul(c, nullptr, 16)));
                u32 word = 0;
                while (fields >> std::hex >> word) {
                    pair.fetch_code.push_back(word);
                }
                continue;
            }
            StageSnapshot& stage = snapshots[key].stages[a == "ps"];
            if (b == "ud") {
                stage.ud[0] = static_cast<u32>(std::stoul(c, nullptr, 16));
                for (u32 i = 1; i < 16; ++i) {
                    fields >> std::hex >> stage.ud[i];
                }
            } else if (b == "tab") {
                auto& table = stage.tables[static_cast<u32>(std::stoul(c))];
                for (u32& word : table) {
                    fields >> std::hex >> word;
                }
            }
        }
    }
    u32 pairs = 0, pairs_ok = 0, pairs_real = 0;
    fs::create_directories(out_dir / "pairs", ec);
    for (const auto& [snapshot_key, pair_snapshot] : snapshots) {
        const auto cols = Split(snapshot_key, '_');
        if (cols.size() != 3) {
            continue;
        }
        const auto vs = by_name.find("vs" + cols[0]);
        const auto ps = by_name.find("ps" + cols[1]);
        if (vs == by_name.end() || ps == by_name.end()) {
            continue;
        }
        if (live) { // ja existe (ou foi desativado a mao: .txt.invalido)
            bool present = false;
            for (const char* suffix : {".txt", ".txt.invalido", ".txt.invalido2"}) {
                present |= fs::exists(out_dir / "pairs" / ("pair_" + snapshot_key + suffix), ec);
            }
            if (present) {
                continue;
            }
        }
        ++pairs;
        const FetchPair* fetch = nullptr;
        const std::vector<u32>* fetch_code = pair_snapshot.fetch_code.empty() ? nullptr : &pair_snapshot.fetch_code;
        Shader::Backend::Bindings shared{};
        std::string vs_layout, ps_layout;
        Result vs_result, ps_result;
        Job vs_job{vs->second, fetch, shader_dir, &vs_result, &shared, &vs_layout, nullptr, fetch_code, pair_snapshot.fetch_sgpr};
        Job ps_job{ps->second, nullptr, shader_dir, &ps_result, &shared, &ps_layout};
        u32 seh = 0;
        tool::TakeLog();
        const bool vs_ok = Guarded(RecompileThunk, &vs_job, &seh) && vs_result.ok;
        const bool ps_ok = vs_ok && Guarded(RecompileThunk, &ps_job, &seh) && ps_result.ok;
        const std::string name = "pair_" + snapshot_key;
        // Segunda traducao, agora com os descritores reais: e este o SPIR-V que vai para o
        // pipeline. A primeira (memoria de sondagem) so serve para saber as origens.
        // Imagens cujo T# veio zerado no retrato (retratos antigos so tinham 64 dwords por
        // tabela, ou o slot estava vazio no primeiro draw): o recompilador leria tipo 0 e
        // declararia textura 1D. Entram como 2D RGBA8, que e o caso de longe mais comum; o jogo
        // confere o tipo real do T# em cada draw.
        std::array<StageSnapshot, 2> patched = pair_snapshot.stages;
        for (u32 stage = 0; stage < 2; ++stage) {
            std::istringstream image_lines(stage == 0 ? vs_layout : ps_layout);
            std::string text, word, source;
            while (std::getline(image_lines, text)) {
                std::istringstream fields(text);
                u32 binding = 0, reg = 0, dword = 0;
                fields >> word;
                if (word != "image") {
                    continue;
                }
                fields >> word >> binding >> word >> source;
                u32* t = nullptr;
                if (std::sscanf(source.c_str(), "ud:%u", &reg) == 1 && reg + 8 <= 16) {
                    t = &patched[stage].ud[reg];
                } else if (std::sscanf(source.c_str(), "tab:%u:%u", &reg, &dword) == 2 && dword + 8 <= 192) {
                    const auto table = patched[stage].tables.find(reg);
                    if (table != patched[stage].tables.end()) {
                        t = &table->second[dword];
                    }
                }
                if (t && (t[3] >> 28) == 0) {
                    t[1] = 0x0Au << 20;  // RGBA8 unorm
                    t[3] = 0x90000FACu;  // 2D, swizzle RGBA
                }
            }
        }
        const std::array<StageSnapshot, 2>* snapshot = &patched;
        std::string specialized = shader_metadata::DescriptorFlag(false);
        if (vs_ok && ps_ok) {
            Shader::Backend::Bindings real{};
            Result vs_real, ps_real;
            Job vs_real_job{vs->second, fetch, shader_dir, &vs_real, &real, nullptr, &(*snapshot)[0], fetch_code, pair_snapshot.fetch_sgpr};
            Job ps_real_job{ps->second, nullptr, shader_dir, &ps_real, &real, nullptr, &(*snapshot)[1]};
            const bool real_ok = Guarded(RecompileThunk, &vs_real_job, &seh) && vs_real.ok &&
                                 Guarded(RecompileThunk, &ps_real_job, &seh) && ps_real.ok;
            if (real_ok && real == shared) {
                vs_result.spv = vs_real.spv;
                ps_result.spv = ps_real.spv;
                specialized = shader_metadata::DescriptorFlag(true);
                ++pairs_real;
                // Formato (dst_sel + tipo numerico + formato de dados do V#) com que cada
                // atributo foi especializado: o jogo so usa o par se o draw tiver o mesmo.
                std::istringstream lines(vs_layout);
                std::string text, word;
                std::ostringstream formats;
                while (std::getline(lines, text)) {
                    std::istringstream fields(text);
                    u32 location = 0, sgpr = 0, dword = 0;
                    fields >> word;
                    if (word != "attribute") {
                        continue;
                    }
                    fields >> word >> location >> word >> sgpr >> word >> dword;
                    const auto table = (*snapshot)[0].tables.find(sgpr);
                    if (table != (*snapshot)[0].tables.end() && dword + 3 < 192) {
                        formats << "  vformat location " << location << " format " << (table->second[dword + 3] & 0x7FFFF) << "\n";
                    }
                }
                // Tipo do T# (2D, cubo, array, 3D) com que cada imagem foi especializada: o
                // descritor que o jogo vincular tem de ter o mesmo tipo de view.
                for (u32 stage = 0; stage < 2; ++stage) {
                    std::istringstream image_lines(stage == 0 ? vs_layout : ps_layout);
                    const StageSnapshot& snap = (*snapshot)[stage];
                    while (std::getline(image_lines, text)) {
                        std::istringstream fields(text);
                        u32 binding = 0;
                        std::string source;
                        fields >> word;
                        if (word != "image") {
                            continue;
                        }
                        fields >> word >> binding >> word >> source;
                        u32 reg = 0, dword = 0, type = 0;
                        if (std::sscanf(source.c_str(), "ud:%u", &reg) == 1 && reg + 3 < 16) {
                            type = snap.ud[reg + 3] >> 28;
                        } else if (std::sscanf(source.c_str(), "tab:%u:%u", &reg, &dword) == 2) {
                            const auto table = snap.tables.find(reg);
                            if (table != snap.tables.end() && dword + 3 < 192) {
                                type = table->second[dword + 3] >> 28;
                            }
                        }
                        formats << "  itype binding " << binding << " type " << type << "\n";
                    }
                }
                ps_layout += formats.str();
            } else {
                std::printf("  pair %s: translation with actual descriptors failed (%s)\n", name.c_str(),
                            !real_ok ? (vs_real.ok ? ps_real.reason : vs_real.reason).c_str() : "bindings differ from probe");
            }
        }
        if (vs_ok && ps_ok) {
            ++pairs_ok;
            std::ofstream(out_dir / "pairs" / (name + ".vs.spv"), std::ios::binary)
                .write(reinterpret_cast<const char*>(vs_result.spv.data()), vs_result.spv.size() * 4);
            std::ofstream(out_dir / "pairs" / (name + ".ps.spv"), std::ios::binary)
                .write(reinterpret_cast<const char*>(ps_result.spv.data()), ps_result.spv.size() * 4);
            std::ofstream(out_dir / "pairs" / (name + ".txt"))
                << "pair vs " << cols[0] << " ps " << cols[1] << " variante " << cols[2] << " fetch_sgpr " << pair_snapshot.fetch_sgpr
                << " real " << specialized << "\n"
                << vs_layout << ps_layout;
        } else {
            std::printf("  pair %s: failed in %s (%s)\n", name.c_str(), vs_ok ? "PS" : "VS", (vs_ok ? ps_result.reason : vs_result.reason).c_str());
        }
    }
    if (!quiet) {
        std::printf("UI PAIRS: %u of %u compiled with contiguous bindings, %u with actual descriptors -> %s\n", pairs_ok,
                    pairs, pairs_real, (out_dir / "pairs").string().c_str());
    }
    *done = pairs_ok;
    *total = pairs;
}

// index.tsv a partir de *offset (so linhas inteiras): o jogo acrescenta shaders novos enquanto roda.
bool LoadEntries(const fs::path& shader_dir, std::vector<ShaderEntry>* entries, std::set<std::string>* seen,
                 std::streamoff* offset) {
    std::ifstream in(shader_dir / "index.tsv", std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(*offset);
    std::string line;
    while (std::getline(in, line)) {
        if (in.eof()) {
            break; // linha sem \n: o jogo ainda esta escrevendo, fica para a proxima
        }
        *offset = in.tellg();
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto cols = Split(line, '\t');
        if (cols.size() < 8 || !seen->insert(cols[0] + cols[1]).second) {
            continue;
        }
        ShaderEntry e;
        e.stage = cols[0];
        e.hash = cols[1];
        e.code_bytes = std::strtoul(cols[3].c_str(), nullptr, 10);
        for (const auto& reg : Split(cols[7], ' ')) {
            e.regs.push_back(std::strtoul(reg.c_str(), nullptr, 16));
        }
        entries->push_back(std::move(e));
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path shader_dir = argc > 1 ? argv[1] : "assets/dumped_shaders";
    const fs::path out_dir = argc > 2 ? argv[2] : "assets/shaders_spv";
    // --pairs-only ARQUIVO (usado pelo jogo, ao vivo): so os pares dos retratos de ARQUIVO que ainda
    // nao existem em pairs/; nao refaz os shaders avulsos nem o relatorio.
    fs::path pairs_only;
    bool serve = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pairs-only") && i + 1 < argc) {
            pairs_only = argv[i + 1];
        } else if (!std::strcmp(argv[i], "--serve")) {
            serve = true;
        }
    }
    const bool live = !pairs_only.empty() || serve;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    tool::InitLogCapture();
    if (!MapProbeMemory()) {
        std::printf("ERROR: could not reserve probe memory\n");
        return 1;
    }
    if (!MapFakeUserData()) {
        std::printf("ERROR: could not reserve fake memory at 0x%llx\n", static_cast<unsigned long long>(kFakeBase));
        return 1;
    }

    std::vector<ShaderEntry> entries;
    std::set<std::string> seen_entries;
    std::streamoff index_offset = 0;
    if (!LoadEntries(shader_dir, &entries, &seen_entries, &index_offset)) {
        std::printf("ERROR: could not open %s\n", (shader_dir / "index.tsv").string().c_str());
        return 1;
    }
    // --serve (usado pelo jogo): fica aberto e traduz, a cada linha do stdin, os pares novos do arquivo de retratos
    // daquela linha; responde "DONE <traduzidos> <pedidos>". Abrir a ferramenta custa ~250 ms (memoria de sondagem,
    // indices); aberta, um par novo custa ~30 ms.
    if (serve) {
        char request[4096];
        while (std::fgets(request, sizeof(request), stdin)) {
            std::string path = request;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
                path.pop_back();
            }
            if (path.empty()) {
                continue;
            }
            LoadEntries(shader_dir, &entries, &seen_entries, &index_offset);
            u32 done = 0, total = 0;
            TranslatePairs(fs::u8path(path), true, true, entries, shader_dir, out_dir, &done, &total);
            std::printf("DONE %u %u\n", done, total);
            std::fflush(stdout);
        }
        return 0;
    }
    // pares VS <-> fetch shader (o arquivo da execucao mais recente e o da anterior)
    std::map<std::string, std::vector<FetchPair>> fetch_of;
    for (const char* name : {"fetch_index.tsv", "fetch_index.prev.tsv"}) {
        std::ifstream in(shader_dir / name);
        std::string line;
        while (std::getline(in, line)) {
            const auto cols = Split(line, '\t');
            if (cols.size() < 3) {
                continue;
            }
            auto& list = fetch_of[cols[0]];
            bool known = false;
            for (const auto& pair : list) {
                known |= pair.fetch_hash == cols[1];
            }
            if (!known) {
                list.push_back({cols[1], std::atoi(cols[2].c_str())});
            }
        }
    }

    std::ofstream report;
    if (!live) {
        report.open(out_dir / "recompile_report.tsv");
    }
    report << "file\tstatus\tspirv_bytes\treason\n";
    struct Totals {
        u32 shaders = 0, ok = 0, failed = 0, skipped = 0, modules = 0, modules_ok = 0;
    };
    std::map<std::string, Totals> totals;
    std::map<std::string, u32> reasons;
    u64 spv_bytes = 0;

    for (const ShaderEntry& entry : entries) {
        if (live) {
            break;
        }
        Totals& t = totals[entry.stage];
        ++t.shaders;
        // O vertex shader chama o fetch shader com s_swappc_b64 (SOP1, op 33)?
        std::vector<u32> words;
        size_t dwords = 0;
        bool needs_fetch = false;
        if (entry.stage == "vs" && LoadWords(shader_dir / ("vs_" + entry.hash + ".bin"), &words, &dwords)) {
            for (size_t i = 0; i < entry.code_bytes / 4 && i < dwords; ++i) {
                needs_fetch |= (words[i] & 0xFF80FF00) == 0xBE802100;
            }
        }
        std::vector<const FetchPair*> variants;
        if (needs_fetch) {
            for (const auto& pair : fetch_of[entry.hash]) {
                variants.push_back(&pair);
            }
            if (variants.empty()) {
                ++t.skipped;
                ++reasons["vs: fetch shader not captured (VS did not appear in any draw during this run)"];
                report << entry.stage << "_" << entry.hash << "\tsem_fetch\t0\tfetch shader not captured\n";
                continue;
            }
        } else {
            variants.push_back(nullptr);
        }
        bool any_ok = false, any_fail = false;
        for (const FetchPair* fetch : variants) {
            ++t.modules;
            const std::string name = entry.stage + "_" + entry.hash + (fetch ? "_" + fetch->fetch_hash : "");
            Result result;
            Job job{&entry, fetch, shader_dir, &result};
            u32 seh = 0;
            tool::TakeLog();
            if (!Guarded(RecompileThunk, &job, &seh)) {
                char text[112];
                std::snprintf(text, sizeof(text), "system exception 0x%08x reading %s", seh,
                              g_fault_address < 0x10000 ? "null pointer" : g_fault_address >= kFakeBase && g_fault_address < kFakeBase + (1ull << 32) ? "beyond fake memory" : "game address");
                result.ok = false;
                result.reason = text;
            }
            const std::string log = FirstLogLine(tool::TakeLog());
            if (result.ok) {
                std::ofstream spv(out_dir / (name + ".spv"), std::ios::binary);
                spv.write(reinterpret_cast<const char*>(result.spv.data()), result.spv.size() * 4);
                std::ofstream(out_dir / (name + ".json")) << result.metadata;
                spv_bytes += result.spv.size() * 4;
                ++t.modules_ok;
                any_ok = true;
                report << name << "\t" << (result.translation_failed ? "ok_com_avisos" : "ok") << "\t" << result.spv.size() * 4
                       << "\t" << log << "\n";
            } else {
                any_fail = true;
                const std::string why = result.reason + (log.empty() ? "" : ": " + log);
                ++reasons[entry.stage + ": " + why.substr(0, 160)];
                report << name << "\tfalha\t0\t" << why << "\n";
            }
        }
        if (any_ok && !any_fail) {
            ++t.ok;
        } else {
            ++t.failed;
        }
    }

    {
        u32 done = 0, total = 0;
        TranslatePairs(live ? pairs_only : out_dir / "pair_snapshots.txt", live, false, entries, shader_dir, out_dir, &done, &total);
    }

    u32 shaders = 0, ok = 0, failed = 0, skipped = 0;
    for (const auto& [stage, t] : totals) {
        std::printf("%s: %u shaders -> %u translated, %u failed, %u without captured fetch shader  (%u SPIR-V modules of %u attempted)\n",
                    stage.c_str(), t.shaders, t.ok, t.failed, t.skipped, t.modules_ok, t.modules);
        shaders += t.shaders;
        ok += t.ok;
        failed += t.failed;
        skipped += t.skipped;
    }
    std::printf("TOTAL: %u shaders, %u translated to SPIR-V, %u failed, %u skipped; %.1f KiB of SPIR-V in %s\n", shaders, ok,
                failed, skipped, spv_bytes / 1024.0, out_dir.string().c_str());
    if (!reasons.empty()) {
        std::printf("Motivos:\n");
        for (const auto& [reason, count] : reasons) {
            std::printf("  %4u  %s\n", count, reason.c_str());
        }
    }
    return 0;
}
