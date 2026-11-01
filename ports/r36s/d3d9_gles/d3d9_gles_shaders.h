#pragma once

// D3D9 shader translation for the GLES 2.0 backend.
//
// Rules (see HANDOVER_FOR_OPENCODE.md section 6): this layer must reproduce what D3D9 would do with
// the game's own shaders, constants and textures. No per-mesh heuristics, no invented material
// values, no rewriting of the game's shader math.
//
// * Programmable shaders are translated by MojoShader (GLSL ES 1.00). Variants are compiled for
//   - pixel shaders 1.x: which sampler stages hold cube textures (ps_1_x has no dcl, the texture type
//     comes from what is bound, exactly like D3D9),
//   - vertex shaders: which inputs are fed D3DCOLOR (BGRA in memory) so they are swizzled in-shader.
// * Render-to-texture uses MojoShader's vpFlip (MOJOSHADER_FLIP_RENDERTARGET) so texture rows keep
//   D3D orientation.
// * Fixed-function draws use generated shaders built from the texture-stage states.

#include "d3d9_gles.h"
#include "../../../third_party/mojoshader/mojoshader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace d3d9_gles {

constexpr int kMaxFloatRegs = 256;
constexpr int kMaxIntRegs = 16;
constexpr int kMaxBoolRegs = 16;

struct ShaderUniformInfo {
    MOJOSHADER_uniformType type;
    int registerIndex;
    int registerCount;
};

struct ShaderAttributeInfo {
    std::string name;  // GLSL name actually declared as attribute
    MOJOSHADER_usage usage;
    int index;
};

struct ShaderSamplerInfo {
    std::string name;
    int stage;
    MOJOSHADER_samplerType type;
};

// ---- Instanced vertex-shader variants (world draw replay, src/runtime/world_cull.c) ----
// Consecutive replayed world items that draw the same mesh differ only in the constant registers that follow their
// world matrix. One instanced draw covers them: the variant reads those registers from nfs_inst[base + j] with
// base = instance * k (a uniform array: GLSL ES 1.00 allows any index into vertex-shader uniforms), the instance
// number coming from one per-instance attribute (divisor 1). The rest of the translation, and the pixel shaders,
// are unchanged. A layout is the list of instanced registers; variant key = bgra mask | layout id << 16 | bit 31.
constexpr uint32_t kInstancedVariantBit = 1u << 31;
constexpr int kMaxInstancesPerDraw = 64;
constexpr int kMaxInstanceRegisters = 16;
// GL_MAX_VERTEX_UNIFORM_VECTORS (set when instancing starts). An instanced variant's nfs_inst array takes what the
// shader's own constants (at most 256 vectors) leave: instances per draw = that room / registers, at most 64.
inline int& maxVertexUniformVectors() { static int vectors = 0; return vectors; }
inline int instancesPerDraw(int registerCount) {
    if (registerCount <= 0) return 0;
    return std::min(kMaxInstancesPerDraw, std::max(0, (maxVertexUniformVectors() - 256 - 8) / registerCount));
}
inline std::vector<std::vector<uint16_t>>& instanceLayouts() { static std::vector<std::vector<uint16_t>> layouts; return layouts; }
// 1-based id of a register list (render thread only)
inline uint32_t instanceLayoutId(const uint16_t* registers, uint32_t count) {
    auto& layouts = instanceLayouts();
    for (size_t i = 0; i < layouts.size(); ++i)
        if (layouts[i].size() == count && std::equal(registers, registers + count, layouts[i].begin())) return static_cast<uint32_t>(i + 1);
    layouts.emplace_back(registers, registers + count);
    return static_cast<uint32_t>(layouts.size());
}

struct ShaderVariant {
    GLuint glShader = 0;
    int instanceRegisterCount = 0;  // > 0: instanced variant (uniform nfs_inst, attribute nfs_instance_in after the others)
    std::string source;
    std::vector<ShaderUniformInfo> uniforms;  // non-constant uniforms in MojoShader emission order
    std::vector<ShaderAttributeInfo> attributes;
    std::vector<ShaderSamplerInfo> samplers;
    int float4Count = 0;
    int int4Count = 0;
    int boolCount = 0;
    bool usesFog = false;
    bool valid = false;
};

inline uint64_t fnv64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
// Source hash of every compiled GL shader (program binary cache keys).
inline std::unordered_map<GLuint, uint64_t>& shaderSourceHashes() { static std::unordered_map<GLuint, uint64_t> m; return m; }

// Program binary cache (GL_OES_get_program_binary). Linking a program costs the Mali driver 70-230 ms on
// the R36S, and new shader/state combinations appear while driving (new cars, materials): visible hitches
// (NFSU2 HITCH lines). Linked programs are stored in $NFSU2_SHADER_CACHE (default ./shadercache in the game
// folder), keyed by the shader sources, attribute bindings and GL renderer/version, and loaded instead of
// linked on later runs. NFSU2_SHADER_CACHE=0 disables it. A binary the driver rejects is relinked.
struct ProgramBinaryCache {
    typedef void (*GetBinary)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
    typedef void (*LoadBinary)(GLuint, GLenum, const void*, GLint);
    GetBinary get = nullptr; LoadBinary load = nullptr; bool ok = false, inited = false; std::string dir; uint64_t salt = 0;
    uint32_t loaded = 0, saved = 0, rejected = 0;
    void init() {
        if (inited) return;
        inited = true;
        const char* e = std::getenv("NFSU2_SHADER_CACHE");
        if (e && !std::strcmp(e, "0")) return;
        dir = e && *e ? e : "shadercache";
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        GLint formats = 0;
        glGetIntegerv(0x87FE /* GL_NUM_PROGRAM_BINARY_FORMATS_OES */, &formats);
        if (!ext || !std::strstr(ext, "GL_OES_get_program_binary") || formats <= 0) {
            std::printf("[D3D9 GLES] program binary cache unavailable (extension %d, formats %d)\n", ext && std::strstr(ext, "GL_OES_get_program_binary") ? 1 : 0, formats);
            return;
        }
#ifdef __ANDROID__
        get = reinterpret_cast<GetBinary>(eglGetProcAddress("glGetProgramBinaryOES"));
        load = reinterpret_cast<LoadBinary>(eglGetProcAddress("glProgramBinaryOES"));
#else
        get = reinterpret_cast<GetBinary>(SDL_GL_GetProcAddress("glGetProgramBinaryOES"));
        load = reinterpret_cast<LoadBinary>(SDL_GL_GetProcAddress("glProgramBinaryOES"));
#endif
        if (!get || !load) return;
        mkdir(dir.c_str(), 0755);
        const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        salt = fnv64(r ? r : "", r ? std::strlen(r) : 0, fnv64(v ? v : "", v ? std::strlen(v) : 0));
        ok = true;
        std::printf("[D3D9 GLES] program binary cache in %s/\n", dir.c_str());
    }
    std::string path(uint64_t key) const { char b[40]; std::snprintf(b, sizeof b, "/%016llx.bin", static_cast<unsigned long long>(key)); return dir + b; }
    bool tryLoad(GLuint prog, uint64_t key) {
        FILE* f = std::fopen(path(key).c_str(), "rb");
        if (!f) return false;
        uint32_t hdr[3] = {};
        std::vector<uint8_t> data;
        bool good = std::fread(hdr, 4, 3, f) == 3 && hdr[0] == 0x4e465342u && hdr[2] > 0 && hdr[2] < (16u << 20);
        if (good) { data.resize(hdr[2]); good = std::fread(data.data(), 1, data.size(), f) == data.size(); }
        std::fclose(f);
        if (!good) return false;
        load(prog, hdr[1], data.data(), static_cast<GLint>(data.size()));
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) { ++rejected; std::remove(path(key).c_str()); return false; }
        ++loaded;
        return true;
    }
    void save(GLuint prog, uint64_t key) {
        GLint len = 0;
        glGetProgramiv(prog, 0x8741 /* GL_PROGRAM_BINARY_LENGTH_OES */, &len);
        if (len <= 0) return;
        std::vector<uint8_t> data(static_cast<size_t>(len));
        GLsizei got = 0; GLenum format = 0;
        get(prog, len, &got, &format, data.data());
        if (got <= 0) return;
        const std::string final_ = path(key), tmp = final_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        const uint32_t hdr[3] = {0x4e465342u, format, static_cast<uint32_t>(got)};
        const bool good = std::fwrite(hdr, 4, 3, f) == 3 && std::fwrite(data.data(), 1, static_cast<size_t>(got), f) == static_cast<size_t>(got);
        std::fclose(f);
        if (good) { std::rename(tmp.c_str(), final_.c_str()); ++saved; } else std::remove(tmp.c_str());
    }
};
inline ProgramBinaryCache& programBinaryCache() { static ProgramBinaryCache c; return c; }

inline bool compileGLShader(GLenum type, const std::string& src, GLuint& out, const char* what) {
    HitchTimer hitch(0);
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[D3D9 GLES] %s compile failed: %s\nSource:\n%s\n", what, log, src.c_str());
        glDeleteShader(sh);
        out = 0;
        return false;
    }
    shaderSourceHashes()[sh] = fnv64(src.data(), src.size(), type);
    out = sh;
    return true;
}

// Alpha test, evaluated in 8-bit space like D3D9 (ALPHAREF is 0..255).
// u_alphaTest.x = D3DCMPFUNC (0 = test disabled), u_alphaTest.y = reference.
inline const char* alphaTestGLSL() {
    return
        "\tif (u_alphaTest.x > 0.5) {\n"
        "\t\tfloat at_a = floor(clamp(gl_FragColor.a, 0.0, 1.0) * 255.0 + 0.5);\n"
        "\t\tfloat at_r = u_alphaTest.y;\n"
        "\t\tfloat at_f = u_alphaTest.x;\n"
        "\t\tbool at_pass = (at_f < 1.5) ? false : (at_f < 2.5) ? (at_a < at_r) : (at_f < 3.5) ? (at_a == at_r) :\n"
        "\t\t               (at_f < 4.5) ? (at_a <= at_r) : (at_f < 5.5) ? (at_a > at_r) : (at_f < 6.5) ? (at_a != at_r) :\n"
        "\t\t               (at_f < 7.5) ? (at_a >= at_r) : true;\n"
        "\t\tif (!at_pass) discard;\n"
        "\t}\n";
}

// D3D9 fixed-function fog applied after the pixel shader (ps < 3.0). PS variant key bits 16..18:
enum FogKey : uint32_t { kFogNone = 0, kFogVertex = 1, kFogLinear = 2, kFogExp = 3, kFogExp2 = 4 };
constexpr uint32_t kFogShift = 16;
// ps_1_1..ps_1_3: D3DTTFF_PROJECTED on a stage makes its `tex` divide the coordinate by its last element
// (D3D9 texture-coordinate processing). Two bits per stage from bit 20: 1 = by .z (COUNT3), 2 = by .w.
constexpr uint32_t kProjShift = 20;
inline std::string fogGLSL(uint32_t fogMode) {
    if (fogMode == kFogNone) return "";
    std::string s = "\t{\n";
    if (fogMode == kFogVertex) {
        s += "\t\tfloat fog_f = clamp(v_fog, 0.0, 1.0);\n";
    } else {
        // Pixel (table) fog from eye depth (W-friendly projection).
        s += "\t\tfloat fog_d = 1.0 / gl_FragCoord.w;\n";
        if (fogMode == kFogLinear) s += "\t\tfloat fog_f = clamp((u_fogParams.y - fog_d) / max(u_fogParams.y - u_fogParams.x, 1e-6), 0.0, 1.0);\n";
        else if (fogMode == kFogExp) s += "\t\tfloat fog_f = clamp(exp(-fog_d * u_fogParams.z), 0.0, 1.0);\n";
        else s += "\t\tfloat fog_f = clamp(exp(-(fog_d * u_fogParams.z) * (fog_d * u_fogParams.z)), 0.0, 1.0);\n";
    }
    // Draw distance (NFSU2_DRAW_DIST, src/runtime/world_cull.c culls objects wholly beyond it): fog reaches
    // opaque at the draw distance, measured as the distance from the camera (not the depth), so nothing
    // that gets culled is visible when it goes. u_fogParams.w = draw distance (0 = off), u_fogRad turns
    // the window position into the view ray's length factor.
    s += "\t\tif (u_fogParams.w > 0.0) {\n"
         "\t\t\thighp float dd_z = 1.0 / gl_FragCoord.w;\n"
         "\t\t\thighp float dd_x = gl_FragCoord.x * u_fogRad.x - u_fogRad.y;\n"
         "\t\t\thighp float dd_y = gl_FragCoord.y * u_fogRad.z - u_fogRad.w;\n"
         "\t\t\thighp float dd_r = dd_z * sqrt(1.0 + dd_x * dd_x + dd_y * dd_y);\n"
         "\t\t\tfog_f = min(fog_f, clamp((u_fogParams.w - dd_r) / (0.4 * u_fogParams.w), 0.0, 1.0));\n"
         "\t\t}\n";
    s += "\t\tgl_FragColor.rgb = mix(u_fogColor.rgb, gl_FragColor.rgb, fog_f);\n\t}\n";
    return s;
}
inline const char* fogDeclsGLSL() {
    return "uniform vec4 u_fogColor;\nuniform vec4 u_fogParams;\nuniform highp vec4 u_fogRad;\nvarying highp float v_fog;\n";
}

class ShaderBase {
public:
    virtual ~ShaderBase() {
        for (auto& kv : m_variants) {
            if (kv.second.glShader) glDeleteShader(kv.second.glShader);
        }
    }

    bool init(GLenum shaderType, const DWORD* pFunction, size_t byteLength) {
        if (!pFunction) return false;
        m_type = shaderType;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(pFunction);
        m_bytecode.assign(b, b + byteLength);
        m_version = pFunction[0];
        const ShaderVariant* v = getVariant(0);
        return v != nullptr;
    }

    unsigned majorVersion() const { return (m_version >> 8) & 0xFF; }
    unsigned minorVersion() const { return m_version & 0xFF; }
    GLenum type() const { return m_type; }

    // VS key: bit i set => attribute i (GL location i) is fed D3DCOLOR data (BGRA) and is swizzled.
    // PS key: bit s set => sampler stage s holds a cube texture (only used for ps_1_x).
    const ShaderVariant* getVariant(uint32_t key) {
        if (m_type == GL_FRAGMENT_SHADER && majorVersion() >= 2) key &= ~0xFFFFu;  // no cube map (dcl gives the type)
        auto it = m_variants.find(key);
        if (it != m_variants.end()) return it->second.valid ? &it->second : nullptr;
        ShaderVariant& v = m_variants[key];
        build(v, key);
        return v.valid ? &v : nullptr;
    }

    const ShaderVariant* base() { return getVariant(0); }

    // IDirect3D{Vertex,Pixel}Shader9::GetFunction. The game (through D3DX) reads the bytecode back to
    // parse its constant table (CTAB comment block) and find which registers to set. Without this it
    // never calls Set*ShaderConstantF at all.
    HRESULT getFunction(void* pData, UINT* pSizeOfData) const {
        if (!pSizeOfData) return D3DERR_INVALIDCALL;
        const UINT size = static_cast<UINT>(m_bytecode.size());
        if (!pData) { *pSizeOfData = size; return D3D_OK; }
        if (*pSizeOfData < size) return D3DERR_INVALIDCALL;
        std::memcpy(pData, m_bytecode.data(), size);
        return D3D_OK;
    }

private:
    void build(ShaderVariant& v, uint32_t key) {
        MOJOSHADER_samplerMap smap[16];
        int smapCount = 0;
        if (m_type == GL_FRAGMENT_SHADER) {
            for (int i = 0; i < 16; ++i) {
                smap[i].index = i;
                smap[i].type = (key & (1u << i)) ? MOJOSHADER_SAMPLER_CUBE : MOJOSHADER_SAMPLER_2D;
            }
            smapCount = 16;
        }
        const MOJOSHADER_parseData* data = MOJOSHADER_parse(
            MOJOSHADER_PROFILE_GLSLES, nullptr, m_bytecode.data(), static_cast<unsigned int>(m_bytecode.size()),
            nullptr, 0, smapCount ? smap : nullptr, static_cast<unsigned int>(smapCount), nullptr, nullptr, nullptr);
        if (!data || data->error_count > 0) {
            if (data) {
                for (int i = 0; i < data->error_count; ++i) {
                    std::fprintf(stderr, "[D3D9 GLES] MojoShader error: %s\n", data->errors[i].error);
                }
                MOJOSHADER_freeParseData(data);
            }
            return;
        }

        std::string src = data->output ? data->output : "";
        for (int i = 0; i < data->uniform_count; ++i) {
            const MOJOSHADER_uniform& u = data->uniforms[i];
            if (u.constant) continue;  // def-array constants are emitted inline by MojoShader
            ShaderUniformInfo info;
            info.type = u.type;
            info.registerIndex = u.index;
            info.registerCount = u.array_count > 0 ? u.array_count : 1;
            v.uniforms.push_back(info);
            if (u.type == MOJOSHADER_UNIFORM_FLOAT) v.float4Count += info.registerCount;
            else if (u.type == MOJOSHADER_UNIFORM_INT) v.int4Count += info.registerCount;
            else if (u.type == MOJOSHADER_UNIFORM_BOOL) v.boolCount += info.registerCount;
        }
        for (int i = 0; i < data->attribute_count; ++i) {
            ShaderAttributeInfo a;
            a.name = data->attributes[i].name;
            a.usage = data->attributes[i].usage;
            a.index = data->attributes[i].index;
            v.attributes.push_back(a);
        }
        for (int i = 0; i < data->sampler_count; ++i) {
            ShaderSamplerInfo s;
            s.name = data->samplers[i].name;
            s.stage = data->samplers[i].index;
            s.type = data->samplers[i].type;
            v.samplers.push_back(s);
        }
        MOJOSHADER_freeParseData(data);

        size_t mainPos = src.find("void main()");
        size_t mainBrace = (mainPos == std::string::npos) ? std::string::npos : src.find('{', mainPos);
        if (mainBrace == std::string::npos) return;

        if (m_type == GL_VERTEX_SHADER) {
            std::string prologue;
            if (key & kInstancedVariantBit) {
                const uint32_t layoutId = (key >> 16) & 0x7FFFu;
                if (!layoutId || layoutId > instanceLayouts().size()) return;
                const std::vector<uint16_t>& registers = instanceLayouts()[layoutId - 1];
                // Relative addressing (vs_uniforms_vec4[a0 + n]) could read an instanced register: no variant.
                for (size_t p = src.find("vs_uniforms_vec4["); p != std::string::npos; p = src.find("vs_uniforms_vec4[", p + 1)) {
                    size_t q = p + std::strlen("vs_uniforms_vec4[");
                    while (q < src.size() && src[q] >= '0' && src[q] <= '9') ++q;
                    if (q >= src.size() || src[q] != ']') return;
                }
                for (size_t j = 0; j < registers.size(); ++j) {
                    const std::string define = "#define vs_c" + std::to_string(registers[j]) + " ";
                    const size_t d = src.find(define);
                    if (d == std::string::npos) continue;   // the shader does not read this register
                    const size_t e = src.find('\n', d);
                    src.replace(d, e - d, define + "nfs_inst[nfs_instanceBase + " + std::to_string(j) + "]");
                }
                const std::string k = std::to_string(registers.size());
                if (instancesPerDraw(static_cast<int>(registers.size())) < 2) return;
                src.insert(src.find("void main()"), "uniform vec4 nfs_inst[" + std::to_string(instancesPerDraw(static_cast<int>(registers.size())) * static_cast<int>(registers.size())) +
                           "];\nattribute float nfs_instance_in;\nint nfs_instanceBase;\n");
                prologue += "\tnfs_instanceBase = int(nfs_instance_in + 0.5) * " + k + ";\n";
                v.instanceRegisterCount = static_cast<int>(registers.size());
            }
            // D3D9 vertex colour outputs that the shader never writes read as opaque white.
            if (src.find("io_10_0") != std::string::npos && src.find("vs_oD0 =") == std::string::npos) {
                prologue += "\tio_10_0 = vec4(1.0);\n";
            }
            // Fog: every vertex shader writes the common varying v_fog (1.0 = no fog unless oFog is written).
            {
                const std::string defKey = "#define vs_oFog ";
                size_t d = src.find(defKey);
                if (d != std::string::npos) {
                    size_t e = src.find('\n', d);
                    const std::string name = src.substr(d + defKey.size(), e - d - defKey.size());
                    src.replace(d, e - d, "#define vs_oFog v_fog");
                    for (const char* ty : {"varying highp float ", "varying highp vec4 ", "varying float ", "varying vec4 "}) {
                        const std::string decl = std::string(ty) + name + ";";
                        size_t q = src.find(decl);
                        if (q != std::string::npos) { src.erase(q, decl.size()); break; }
                    }
                }
                size_t mp = src.find("void main()");
                src.insert(mp, "varying highp float v_fog;\n");
                prologue += "\tv_fog = 1.0;\n";
            }
            for (size_t i = 0; i < v.attributes.size(); ++i) {
                std::string& n = v.attributes[i].name;
                const std::string decl = "attribute vec4 " + n + ";";
                size_t p = src.find(decl);
                if (p == std::string::npos) continue;
                src.replace(p, decl.size(), "attribute vec4 " + n + "_in;\nvec4 " + n + ";");
                prologue += "\t" + n + " = " + n + "_in" + (((key >> i) & 1u) ? ".zyxw" : "") + ";\n";
                n += "_in";
            }
            mainBrace = src.find('{', src.find("void main()"));
            src.insert(mainBrace + 1, "\n" + prologue);
        } else {
            // Texture coordinates at highp: Mali's mediump is fp16, which visibly snaps UVs on tiled
            // surfaces (roads, buildings). Colour maths can stay mediump.
            size_t p = 0;
            while ((p = src.find("\nvec4 ps_t", p)) != std::string::npos) {
                src.insert(p + 1, "highp ");
                p += 7;
            }
            for (int st = 0; st < 4; ++st) {
                const uint32_t proj = (key >> (kProjShift + 2 * st)) & 3u;
                if (!proj) continue;
                const std::string tc = "ps_t" + std::to_string(st), from = "texture2D(ps_s" + std::to_string(st) + ", " + tc + ".xy)";
                const std::string to = "texture2D(ps_s" + std::to_string(st) + ", " + tc + ".xy / " + tc + (proj == 1 ? ".z" : ".w") + ")";
                for (size_t q = src.find(from); q != std::string::npos; q = src.find(from, q + to.size())) src.replace(q, from.size(), to);
            }
            const uint32_t fogMode = (key >> kFogShift) & 7u;
            const char* prec = "precision mediump float;";
            size_t precisionPos = src.find(prec);
            if (precisionPos != std::string::npos) {
                src.insert(precisionPos + std::strlen(prec), std::string("\nuniform vec4 u_alphaTest;\n") + (fogMode ? fogDeclsGLSL() : ""));
            }
            size_t lastBrace = src.rfind('}');
            if (lastBrace != std::string::npos) src.insert(lastBrace, fogGLSL(fogMode) + alphaTestGLSL());
        }

        v.source = src;
        char what[64];
        std::snprintf(what, sizeof(what), "%s %u.x variant 0x%X", m_type == GL_VERTEX_SHADER ? "VS" : "PS",
                      majorVersion(), key);
        if (!compileGLShader(m_type, src, v.glShader, what)) return;
        v.valid = true;

        static const bool s_dump = std::getenv("NFSU2_DUMP_SHADERS") != nullptr;
        if (s_dump) std::printf("[D3D9 SHADER %s obj=%p]\n%s\n", what, static_cast<void*>(this), src.c_str());
    }

    GLenum m_type = 0;
    DWORD m_version = 0;
    std::vector<uint8_t> m_bytecode;
    std::unordered_map<uint32_t, ShaderVariant> m_variants;
};

class IDirect3DVertexShader9GLES : public D3D9ComObject<IDirect3DVertexShader9>, public ShaderBase {
public:
    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(GetFunction)(void* pData, UINT* pSizeOfData) override { return getFunction(pData, pSizeOfData); }
};

class IDirect3DPixelShader9GLES : public D3D9ComObject<IDirect3DPixelShader9>, public ShaderBase {
public:
    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(GetFunction)(void* pData, UINT* pSizeOfData) override { return getFunction(pData, pSizeOfData); }
};

// -----------------------------------------------------------------------------
// Fixed-function pipeline emulation (generated GLSL)
// -----------------------------------------------------------------------------
// Fixed attribute slots for FFP programs.
enum FFPAttrib : GLuint { kFFPPos = 0, kFFPDiffuse = 1, kFFPSpecular = 2, kFFPTex0 = 3, kFFPNumAttribs = 7 };

enum FFPVSKeyBits : uint32_t {
    kFFPVS_RHW = 1u << 0,
    kFFPVS_DiffuseBGRA = 1u << 1,
    kFFPVS_SpecularBGRA = 1u << 2,
};

inline std::string ffpVertexSource(uint32_t key) {
    std::string s =
        "#version 100\n"
        "precision highp float;\n"
        "attribute vec4 a_pos;\n"
        "attribute vec4 a_diffuse;\n"
        "attribute vec4 a_specular;\n"
        "attribute vec4 a_tc0;\nattribute vec4 a_tc1;\nattribute vec4 a_tc2;\nattribute vec4 a_tc3;\n"
        "uniform mat4 u_wvp;\n"
        "uniform vec4 u_rhw;\n"
        "uniform float vpFlip;\n"
        "varying vec4 v_diffuse;\n"
        "varying vec4 v_specular;\n"
        "varying highp vec4 v_tc0;\nvarying highp vec4 v_tc1;\nvarying highp vec4 v_tc2;\nvarying highp vec4 v_tc3;\n"
        "varying highp float v_fog;\n"
        "void main() {\n";
    if (key & kFFPVS_RHW) {
        // Pre-transformed (XYZRHW / POSITIONT) vertices: pixel coordinates with D3D9 pixel centres at
        // integer positions. u_rhw = (2/vpW, -2/vpH, offsetX, offsetY), offsets include the half pixel.
        s += "    float w = (a_pos.w > 0.0) ? 1.0 / a_pos.w : 1.0;\n"
             "    vec2 ndc = vec2(a_pos.x * u_rhw.x + u_rhw.z, a_pos.y * u_rhw.y + u_rhw.w);\n"
             "    gl_Position = vec4(ndc * w, (a_pos.z * 2.0 - 1.0) * w, w);\n";
    } else {
        s += "    gl_Position = u_wvp * vec4(a_pos.xyz, 1.0);\n"
             "    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n";
    }
    s += "    gl_Position.y = gl_Position.y * vpFlip;\n";
    s += (key & kFFPVS_DiffuseBGRA) ? "    v_diffuse = a_diffuse.zyxw;\n" : "    v_diffuse = a_diffuse;\n";
    s += (key & kFFPVS_SpecularBGRA) ? "    v_specular = a_specular.zyxw;\n" : "    v_specular = a_specular;\n";
    // Pre-transformed vertices carry their vertex fog factor in specular alpha (D3D9); otherwise no fog.
    s += (key & kFFPVS_RHW) ? "    v_fog = a_specular.w;\n" : "    v_fog = 1.0;\n";
    s += "    v_tc0 = a_tc0; v_tc1 = a_tc1; v_tc2 = a_tc2; v_tc3 = a_tc3;\n}\n";
    return s;
}

struct FFPStage {
    DWORD colorOp, colorArg0, colorArg1, colorArg2;
    DWORD alphaOp, alphaArg0, alphaArg1, alphaArg2;
    DWORD resultArg;
    DWORD texCoordIndex;
    DWORD transformFlags;
    int texType;  // 0 none, 1 2D, 2 cube
};

inline std::string ffpArg(DWORD a, bool alpha) {
    std::string b;
    switch (a & D3DTA_SELECTMASK) {
        case D3DTA_DIFFUSE:  b = "v_diffuse"; break;
        case D3DTA_CURRENT:  b = "cur"; break;
        case D3DTA_TEXTURE:  b = "tex"; break;
        case D3DTA_TFACTOR:  b = "u_tfactor"; break;
        case D3DTA_SPECULAR: b = "v_specular"; break;
        case D3DTA_TEMP:     b = "tmp"; break;
        case D3DTA_CONSTANT: b = "u_tfactor"; break;
        default:             b = "cur"; break;
    }
    if (a & D3DTA_ALPHAREPLICATE) b = "vec4(" + b + ".a)";
    if (a & D3DTA_COMPLEMENT) b = "(vec4(1.0) - " + b + ")";
    return b + (alpha ? ".a" : ".rgb");
}

// Returns the GLSL expression for a texture-stage operation (vec3 for colour, float for alpha).
inline std::string ffpOp(DWORD op, const FFPStage& st, bool alpha) {
    const DWORD a0 = alpha ? st.alphaArg0 : st.colorArg0;
    const DWORD a1 = alpha ? st.alphaArg1 : st.colorArg1;
    const DWORD a2 = alpha ? st.alphaArg2 : st.colorArg2;
    const std::string x0 = ffpArg(a0, alpha), x1 = ffpArg(a1, alpha), x2 = ffpArg(a2, alpha);
    const std::string x1a = ffpArg(a1, true);
    const std::string one = alpha ? "1.0" : "vec3(1.0)";
    switch (op) {
        case D3DTOP_SELECTARG1: return x1;
        case D3DTOP_SELECTARG2: return x2;
        case D3DTOP_MODULATE: return x1 + " * " + x2;
        case D3DTOP_MODULATE2X: return "(" + x1 + " * " + x2 + ") * 2.0";
        case D3DTOP_MODULATE4X: return "(" + x1 + " * " + x2 + ") * 4.0";
        case D3DTOP_ADD: return x1 + " + " + x2;
        case D3DTOP_ADDSIGNED: return x1 + " + " + x2 + " - 0.5";
        case D3DTOP_ADDSIGNED2X: return "(" + x1 + " + " + x2 + " - 0.5) * 2.0";
        case D3DTOP_SUBTRACT: return x1 + " - " + x2;
        case D3DTOP_ADDSMOOTH: return x1 + " + " + x2 + " - " + x1 + " * " + x2;
        case D3DTOP_BLENDDIFFUSEALPHA: return "mix(" + x2 + ", " + x1 + ", v_diffuse.a)";
        case D3DTOP_BLENDTEXTUREALPHA: return "mix(" + x2 + ", " + x1 + ", tex.a)";
        case D3DTOP_BLENDFACTORALPHA: return "mix(" + x2 + ", " + x1 + ", u_tfactor.a)";
        case D3DTOP_BLENDTEXTUREALPHAPM: return x1 + " + " + x2 + " * (1.0 - tex.a)";
        case D3DTOP_BLENDCURRENTALPHA: return "mix(" + x2 + ", " + x1 + ", cur.a)";
        case D3DTOP_PREMODULATE: return x1;
        case D3DTOP_MODULATEALPHA_ADDCOLOR: return alpha ? x1 : x1 + " + " + x1a + " * " + x2;
        case D3DTOP_MODULATECOLOR_ADDALPHA: return alpha ? x1 : x1 + " * " + x2 + " + " + x1a;
        case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return alpha ? x1 : x1 + " + (1.0 - " + x1a + ") * " + x2;
        case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return alpha ? x1 : "(" + one + " - " + x1 + ") * " + x2 + " + " + x1a;
        case D3DTOP_DOTPRODUCT3: {
            const std::string c1 = ffpArg(a1, false), c2 = ffpArg(a2, false);
            std::string d = "clamp(dot(" + c1 + " - 0.5, " + c2 + " - 0.5) * 4.0, 0.0, 1.0)";
            return alpha ? d : "vec3(" + d + ")";
        }
        case D3DTOP_MULTIPLYADD: return x0 + " + " + x1 + " * " + x2;
        case D3DTOP_LERP: return "mix(" + x2 + ", " + x1 + ", " + x0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";  // BUMPENVMAP* not used by this game
    }
}

inline std::string ffpPixelSource(const FFPStage* stages, int count, bool specularEnable, uint32_t fogMode) {
    std::string s =
        "#version 100\n"
        "precision mediump float;\n"
        "uniform vec4 u_alphaTest;\n"
        "uniform vec4 u_tfactor;\n";
    if (fogMode) s += "uniform vec4 u_fogColor;\nuniform vec4 u_fogParams;\nuniform highp vec4 u_fogRad;\n";
    for (int i = 0; i < count; ++i) {
        if (stages[i].texType == 2) s += "uniform samplerCube u_s" + std::to_string(i) + ";\n";
        else if (stages[i].texType == 1) s += "uniform sampler2D u_s" + std::to_string(i) + ";\n";
    }
    s += "varying vec4 v_diffuse;\n"
         "varying vec4 v_specular;\n"
         "varying highp vec4 v_tc0;\nvarying highp vec4 v_tc1;\nvarying highp vec4 v_tc2;\nvarying highp vec4 v_tc3;\n"
         "varying highp float v_fog;\n"
         "void main() {\n"
         "    vec4 cur = v_diffuse;\n"
         "    vec4 tmp = vec4(0.0);\n"
         "    vec4 tex = vec4(1.0);\n";
    for (int i = 0; i < count; ++i) {
        const FFPStage& st = stages[i];
        if (st.colorOp == D3DTOP_DISABLE) break;
        const std::string tc = "v_tc" + std::to_string(st.texCoordIndex & 3);
        const std::string si = "u_s" + std::to_string(i);
        const bool projected = (st.transformFlags & D3DTTFF_PROJECTED) != 0;
        if (st.texType == 2) {
            s += "    tex = textureCube(" + si + ", " + tc + ".xyz);\n";
        } else if (st.texType == 1) {
            if (projected) {
                const DWORD cnt = st.transformFlags & 0xFF;
                const char* q = (cnt == D3DTTFF_COUNT3) ? ".z" : (cnt == D3DTTFF_COUNT2) ? ".y" : ".w";
                s += "    tex = texture2D(" + si + ", " + tc + ".xy / " + tc + q + ");\n";
            } else {
                s += "    tex = texture2D(" + si + ", " + tc + ".xy);\n";
            }
        } else {
            s += "    tex = vec4(1.0);\n";
        }
        const std::string dst = (st.resultArg == D3DTA_TEMP) ? "tmp" : "cur";
        s += "    {\n";
        s += "        vec3 c = " + ffpOp(st.colorOp, st, false) + ";\n";
        if (st.alphaOp == D3DTOP_DISABLE) {
            s += "        float a = cur.a;\n";
        } else {
            s += "        float a = " + ffpOp(st.alphaOp, st, true) + ";\n";
        }
        s += "        " + dst + " = clamp(vec4(c, a), 0.0, 1.0);\n";
        s += "    }\n";
    }
    if (specularEnable) s += "    cur.rgb = clamp(cur.rgb + v_specular.rgb, 0.0, 1.0);\n";
    s += "    gl_FragColor = cur;\n";
    s += fogGLSL(fogMode);
    s += alphaTestGLSL();
    s += "}\n";
    return s;
}

// -----------------------------------------------------------------------------
// Program cache and constant register files
// -----------------------------------------------------------------------------
struct LinkedProgram {
    GLuint program = 0;
    const ShaderVariant* vs = nullptr;  // null => FFP vertex shader
    const ShaderVariant* ps = nullptr;  // null => FFP pixel shader
    GLint vsFloatLoc = -1, vsIntLoc = -1, vsBoolLoc = -1;
    GLint psFloatLoc = -1, psIntLoc = -1, psBoolLoc = -1;
    GLint vpFlipLoc = -1, alphaTestLoc = -1;
    GLint wvpLoc = -1, rhwLoc = -1, tfactorLoc = -1;
    GLint fogColorLoc = -1, fogParamsLoc = -1, fogRadLoc = -1;
    GLint instanceRegistersLoc = -1;  // nfs_inst of an instanced vertex-shader variant
    float lastFogRad[4] = {-1, -1, -1, -1};
    float lastFog[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<float> vsFloatShadow, psFloatShadow;
    std::vector<int> vsIntShadow, psIntShadow, vsBoolShadow, psBoolShadow;
    float lastFlip = 0.0f;
    float lastAlpha[2] = {-1.0f, -1.0f};
    float lastRhw[4] = {0, 0, 0, 0};
    float lastWvp[16] = {};
    float lastTFactor[4] = {-1, -1, -1, -1};
    uint64_t vsGen = 0, psGen = 0;  // register-file generation last gathered into the shadows
};

class ProgramPipelineManager {
public:
    ProgramPipelineManager() {
        std::memset(m_vsF, 0, sizeof(m_vsF));
        std::memset(m_psF, 0, sizeof(m_psF));
        std::memset(m_vsI, 0, sizeof(m_vsI));
        std::memset(m_psI, 0, sizeof(m_psI));
        std::memset(m_vsB, 0, sizeof(m_vsB));
        std::memset(m_psB, 0, sizeof(m_psB));
    }

    ~ProgramPipelineManager() {
        for (auto& kv : m_programs) {
            if (kv.second.program) glDeleteProgram(kv.second.program);
        }
        for (auto& kv : m_ffpVS) if (kv.second) glDeleteShader(kv.second);
        for (auto& kv : m_ffpPS) if (kv.second) glDeleteShader(kv.second);
    }

    // --- register files (D3D9 semantics: state is per device, independent of the bound shader) ---
    // m_vsGen / m_psGen change with every write: a draw whose program already gathered the current
    // generation skips uploadPacked (gather, compare) entirely.
    void setVSConstantF(UINT start, const float* data, UINT count) { copyRegs(m_vsF, kMaxFloatRegs, start, data, count); ++m_vsGen; }
    void setPSConstantF(UINT start, const float* data, UINT count) { copyRegs(m_psF, kMaxFloatRegs, start, data, count); ++m_psGen; }
    void getVSConstantF(UINT start, float* out, UINT count) const { readRegs(m_vsF, kMaxFloatRegs, start, out, count); }
    void getPSConstantF(UINT start, float* out, UINT count) const { readRegs(m_psF, kMaxFloatRegs, start, out, count); }
    void setVSConstantI(UINT start, const int* data, UINT count) { copyRegs(m_vsI, kMaxIntRegs, start, data, count); ++m_vsGen; }
    void setPSConstantI(UINT start, const int* data, UINT count) { copyRegs(m_psI, kMaxIntRegs, start, data, count); ++m_psGen; }
    void getVSConstantI(UINT start, int* out, UINT count) const { readRegs(m_vsI, kMaxIntRegs, start, out, count); }
    void getPSConstantI(UINT start, int* out, UINT count) const { readRegs(m_psI, kMaxIntRegs, start, out, count); }
    void setVSConstantB(UINT start, const BOOL* data, UINT count) {
        for (UINT i = 0; i < count && start + i < kMaxBoolRegs; ++i) m_vsB[start + i] = data[i] ? 1 : 0;
        ++m_vsGen;
    }
    void setPSConstantB(UINT start, const BOOL* data, UINT count) {
        for (UINT i = 0; i < count && start + i < kMaxBoolRegs; ++i) m_psB[start + i] = data[i] ? 1 : 0;
        ++m_psGen;
    }
    void getVSConstantB(UINT start, BOOL* out, UINT count) const {
        for (UINT i = 0; i < count && start + i < kMaxBoolRegs; ++i) out[i] = m_vsB[start + i];
    }
    void getPSConstantB(UINT start, BOOL* out, UINT count) const {
        for (UINT i = 0; i < count && start + i < kMaxBoolRegs; ++i) out[i] = m_psB[start + i];
    }
    float getVSConstant(size_t idx) const { return idx < sizeof(m_vsF) / sizeof(float) ? m_vsF[idx] : 0.0f; }
    float getPSConstant(size_t idx) const { return idx < sizeof(m_psF) / sizeof(float) ? m_psF[idx] : 0.0f; }

    GLuint getFFPVertexShader(uint32_t key) {
        auto it = m_ffpVS.find(key);
        if (it != m_ffpVS.end()) return it->second;
        GLuint sh = 0;
        compileGLShader(GL_VERTEX_SHADER, ffpVertexSource(key), sh, "FFP VS");
        m_ffpVS[key] = sh;
        return sh;
    }

    GLuint getFFPPixelShader(const std::string& src) {
        auto it = m_ffpPS.find(src);
        if (it != m_ffpPS.end()) return it->second;
        GLuint sh = 0;
        compileGLShader(GL_FRAGMENT_SHADER, src, sh, "FFP PS");
        m_ffpPS[src] = sh;
        return sh;
    }

    // vs/ps == nullptr => the corresponding GL shader is a generated FFP shader.
    LinkedProgram* getProgram(GLuint vsHandle, GLuint psHandle, const ShaderVariant* vs, const ShaderVariant* ps) {
        if (!vsHandle || !psHandle) return nullptr;
        const uint64_t key = (static_cast<uint64_t>(vsHandle) << 32) | psHandle;
        auto it = m_programs.find(key);
        if (it != m_programs.end()) return it->second.program ? &it->second : nullptr;

        LinkedProgram& lp = m_programs[key];
        HitchTimer hitch(1);
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vsHandle);
        glAttachShader(prog, psHandle);
        if (vs) {
            for (size_t i = 0; i < vs->attributes.size(); ++i) {
                glBindAttribLocation(prog, static_cast<GLuint>(i), vs->attributes[i].name.c_str());
            }
            if (vs->instanceRegisterCount) glBindAttribLocation(prog, static_cast<GLuint>(vs->attributes.size()), "nfs_instance_in");
        } else {
            glBindAttribLocation(prog, kFFPPos, "a_pos");
            glBindAttribLocation(prog, kFFPDiffuse, "a_diffuse");
            glBindAttribLocation(prog, kFFPSpecular, "a_specular");
            glBindAttribLocation(prog, kFFPTex0 + 0, "a_tc0");
            glBindAttribLocation(prog, kFFPTex0 + 1, "a_tc1");
            glBindAttribLocation(prog, kFFPTex0 + 2, "a_tc2");
            glBindAttribLocation(prog, kFFPTex0 + 3, "a_tc3");
        }
        ProgramBinaryCache& cache = programBinaryCache();
        cache.init();
        uint64_t cacheKey = 0;
        if (cache.ok) {
            const auto& hashes = shaderSourceHashes();
            auto hv = hashes.find(vsHandle), hp = hashes.find(psHandle);
            if (hv != hashes.end() && hp != hashes.end()) {
                uint64_t parts[3] = {hv->second, hp->second, cache.salt};
                cacheKey = fnv64(parts, sizeof parts);
                if (vs) for (const auto& a : vs->attributes) cacheKey = fnv64(a.name.data(), a.name.size(), cacheKey ^ 0x2f);
                else cacheKey ^= 0x46465000ull;
            }
        }
        GLint ok = 0;
        if (cacheKey && cache.tryLoad(prog, cacheKey)) ok = 1;
        else {
            glLinkProgram(prog);
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (ok && cacheKey) cache.save(prog, cacheKey);
        }
        if (!ok) {
            char log[2048]{};
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[D3D9 GLES] Program link failed: %s\n", log);
            glDeleteProgram(prog);
            return nullptr;
        }
        glUseProgram(prog);
        lp.program = prog;
        lp.vs = vs;
        lp.ps = ps;
        lp.vpFlipLoc = glGetUniformLocation(prog, "vpFlip");
        lp.alphaTestLoc = glGetUniformLocation(prog, "u_alphaTest");
        lp.fogColorLoc = glGetUniformLocation(prog, "u_fogColor");
        lp.fogParamsLoc = glGetUniformLocation(prog, "u_fogParams");
        lp.fogRadLoc = glGetUniformLocation(prog, "u_fogRad");
        if (vs) {
            lp.vsFloatLoc = glGetUniformLocation(prog, "vs_uniforms_vec4");
            lp.vsIntLoc = glGetUniformLocation(prog, "vs_uniforms_ivec4");
            lp.vsBoolLoc = glGetUniformLocation(prog, "vs_uniforms_bool");
            if (vs->instanceRegisterCount) lp.instanceRegistersLoc = glGetUniformLocation(prog, "nfs_inst");
            lp.vsFloatShadow.assign(vs->float4Count * 4, 0.0f);
            lp.vsIntShadow.assign(vs->int4Count * 4, 0);
            lp.vsBoolShadow.assign(vs->boolCount, 0);
            uploadPacked(lp, true, true);
            lp.vsGen = m_vsGen;
        } else {
            lp.wvpLoc = glGetUniformLocation(prog, "u_wvp");
            lp.rhwLoc = glGetUniformLocation(prog, "u_rhw");
        }
        if (ps) {
            lp.psFloatLoc = glGetUniformLocation(prog, "ps_uniforms_vec4");
            lp.psIntLoc = glGetUniformLocation(prog, "ps_uniforms_ivec4");
            lp.psBoolLoc = glGetUniformLocation(prog, "ps_uniforms_bool");
            lp.psFloatShadow.assign(ps->float4Count * 4, 0.0f);
            lp.psIntShadow.assign(ps->int4Count * 4, 0);
            lp.psBoolShadow.assign(ps->boolCount, 0);
            for (const auto& s : ps->samplers) {
                GLint loc = glGetUniformLocation(prog, s.name.c_str());
                if (loc != -1) glUniform1i(loc, s.stage);
            }
            uploadPacked(lp, false, true);
            lp.psGen = m_psGen;
        } else {
            lp.tfactorLoc = glGetUniformLocation(prog, "u_tfactor");
            for (int i = 0; i < 4; ++i) {
                GLint loc = glGetUniformLocation(prog, ("u_s" + std::to_string(i)).c_str());
                if (loc != -1) glUniform1i(loc, i);
            }
        }
        if (lp.vpFlipLoc != -1) glUniform1f(lp.vpFlipLoc, 1.0f);
        lp.lastFlip = 1.0f;
        return &lp;
    }

    // Upload the game's constant registers into the program's packed arrays (only when changed).
    void commitShaderConstants(LinkedProgram& lp) {
        if (lp.vs && lp.vsGen != m_vsGen) { uploadPacked(lp, true, false); lp.vsGen = m_vsGen; }
        if (lp.ps && lp.psGen != m_psGen) { uploadPacked(lp, false, false); lp.psGen = m_psGen; }
    }

private:
    template <typename T>
    static void copyRegs(T* file, int maxRegs, UINT start, const T* data, UINT count) {
        if (!data || start >= static_cast<UINT>(maxRegs)) return;
        if (start + count > static_cast<UINT>(maxRegs)) count = maxRegs - start;
        std::memcpy(file + start * 4, data, count * 4 * sizeof(T));
    }
    template <typename T>
    static void readRegs(const T* file, int maxRegs, UINT start, T* out, UINT count) {
        if (!out || start >= static_cast<UINT>(maxRegs)) return;
        if (start + count > static_cast<UINT>(maxRegs)) count = maxRegs - start;
        std::memcpy(out, file + start * 4, count * 4 * sizeof(T));
    }

    void uploadPacked(LinkedProgram& lp, bool vertex, bool force) {
        const ShaderVariant* sv = vertex ? lp.vs : lp.ps;
        const float* F = vertex ? m_vsF : m_psF;
        const int* I = vertex ? m_vsI : m_psI;
        const int* B = vertex ? m_vsB : m_psB;
        std::vector<float>& fs = vertex ? lp.vsFloatShadow : lp.psFloatShadow;
        std::vector<int>& is = vertex ? lp.vsIntShadow : lp.psIntShadow;
        std::vector<int>& bs = vertex ? lp.vsBoolShadow : lp.psBoolShadow;
        const GLint fl = vertex ? lp.vsFloatLoc : lp.psFloatLoc;
        const GLint il = vertex ? lp.vsIntLoc : lp.psIntLoc;
        const GLint bl = vertex ? lp.vsBoolLoc : lp.psBoolLoc;

        m_scratchF.assign(fs.size(), 0.0f);
        m_scratchI.assign(is.size(), 0);
        m_scratchB.assign(bs.size(), 0);
        size_t fo = 0, io = 0, bo = 0;
        for (const auto& u : sv->uniforms) {
            for (int r = 0; r < u.registerCount; ++r) {
                const int reg = u.registerIndex + r;
                if (u.type == MOJOSHADER_UNIFORM_FLOAT) {
                    if (reg < kMaxFloatRegs && fo + 4 <= m_scratchF.size()) std::memcpy(&m_scratchF[fo], &F[reg * 4], 16);
                    fo += 4;
                } else if (u.type == MOJOSHADER_UNIFORM_INT) {
                    if (reg < kMaxIntRegs && io + 4 <= m_scratchI.size()) std::memcpy(&m_scratchI[io], &I[reg * 4], 16);
                    io += 4;
                } else if (u.type == MOJOSHADER_UNIFORM_BOOL) {
                    if (reg < kMaxBoolRegs && bo < m_scratchB.size()) m_scratchB[bo] = B[reg];
                    bo += 1;
                }
            }
        }
        if (fl != -1 && !fs.empty() && (force || std::memcmp(fs.data(), m_scratchF.data(), fs.size() * sizeof(float)) != 0)) {
            fs = m_scratchF;
            glUniform4fv(fl, static_cast<GLsizei>(fs.size() / 4), fs.data());
        }
        if (il != -1 && !is.empty() && (force || std::memcmp(is.data(), m_scratchI.data(), is.size() * sizeof(int)) != 0)) {
            is = m_scratchI;
            glUniform4iv(il, static_cast<GLsizei>(is.size() / 4), is.data());
        }
        if (bl != -1 && !bs.empty() && (force || std::memcmp(bs.data(), m_scratchB.data(), bs.size() * sizeof(int)) != 0)) {
            bs = m_scratchB;
            glUniform1iv(bl, static_cast<GLsizei>(bs.size()), bs.data());
        }
    }

    std::unordered_map<uint64_t, LinkedProgram> m_programs;
    std::unordered_map<uint32_t, GLuint> m_ffpVS;
    std::unordered_map<std::string, GLuint> m_ffpPS;
    float m_vsF[kMaxFloatRegs * 4];
    float m_psF[kMaxFloatRegs * 4];
    int m_vsI[kMaxIntRegs * 4];
    int m_psI[kMaxIntRegs * 4];
    int m_vsB[kMaxBoolRegs];
    int m_psB[kMaxBoolRegs];
    uint64_t m_vsGen = 1, m_psGen = 1;
    std::vector<float> m_scratchF;
    std::vector<int> m_scratchI;
    std::vector<int> m_scratchB;
};

} // namespace d3d9_gles
