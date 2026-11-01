#pragma once

#include "d3d9_gles.h"

#include <algorithm>
#include <iterator>
#include <vector>

// Set by the D3D9 bridge (android/.../d3d9_render_thread.inc) while it creates a vertex/index buffer on the
// game thread: the GL buffer is then created on first use on the render thread (ensureGL).
extern thread_local bool g_glesDeferBufferGL;
extern thread_local bool g_glesDeferTextureGL;   // 2D textures created off the render thread: GL storage on first use

namespace d3d9_gles {

class IDirect3DDevice9GLES;
class IDirect3DVertexShader9GLES;
class IDirect3DPixelShader9GLES;

// Set once by the device after the GL context exists.
struct GLESCaps {
    bool npot = true;              // GL_OES_texture_npot (full NPOT incl. mipmaps and wrap)
    bool depth24Stencil8 = false;  // GL_OES_packed_depth_stencil
    bool elementIndexUint = false; // GL_OES_element_index_uint
    bool blendMinMax = false;      // GL_EXT_blend_minmax
    bool halfFloatVertex = false;  // GL_OES_vertex_half_float
};
inline GLESCaps& glesCaps() { static GLESCaps c; return c; }

inline UINT d3dFormatBytesPerPixel(D3DFORMAT f) {
    switch (static_cast<DWORD>(f)) {
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_D16:
            return 2;
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8:
            return 1;
        default:
            return 4;
    }
}

// Converts one row of a D3D format to RGBA8 (GL_RGBA/GL_UNSIGNED_BYTE byte order).
inline void convertRowToRGBA8(D3DFORMAT f, const uint8_t* src, uint8_t* dst, UINT width) {
    switch (static_cast<DWORD>(f)) {
        case D3DFMT_A8R8G8B8:
            for (UINT x = 0; x < width; ++x, src += 4, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; }
            break;
        case D3DFMT_X8R8G8B8:
            for (UINT x = 0; x < width; ++x, src += 4, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 0xFF; }
            break;
        case D3DFMT_X8B8G8R8:
            for (UINT x = 0; x < width; ++x, src += 4, dst += 4) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF; }
            break;
        case D3DFMT_R5G6B5:
            for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
                uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
                dst[0] = static_cast<uint8_t>(((p >> 11) & 31) * 255 / 31);
                dst[1] = static_cast<uint8_t>(((p >> 5) & 63) * 255 / 63);
                dst[2] = static_cast<uint8_t>((p & 31) * 255 / 31);
                dst[3] = 0xFF;
            }
            break;
        case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
            for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
                uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
                dst[0] = static_cast<uint8_t>(((p >> 10) & 31) * 255 / 31);
                dst[1] = static_cast<uint8_t>(((p >> 5) & 31) * 255 / 31);
                dst[2] = static_cast<uint8_t>((p & 31) * 255 / 31);
                dst[3] = (f == D3DFMT_X1R5G5B5 || (p & 0x8000)) ? 0xFF : 0x00;
            }
            break;
        case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
            for (UINT x = 0; x < width; ++x, src += 2, dst += 4) {
                uint16_t p = static_cast<uint16_t>(src[0] | (src[1] << 8));
                dst[0] = static_cast<uint8_t>(((p >> 8) & 15) * 17);
                dst[1] = static_cast<uint8_t>(((p >> 4) & 15) * 17);
                dst[2] = static_cast<uint8_t>((p & 15) * 17);
                dst[3] = (f == D3DFMT_X4R4G4B4) ? 0xFF : static_cast<uint8_t>(((p >> 12) & 15) * 17);
            }
            break;
        case D3DFMT_A8:
            for (UINT x = 0; x < width; ++x, ++src, dst += 4) { dst[0] = dst[1] = dst[2] = 0; dst[3] = src[0]; }
            break;
        case D3DFMT_L8:
            for (UINT x = 0; x < width; ++x, ++src, dst += 4) { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 0xFF; }
            break;
        case D3DFMT_A8L8:
            for (UINT x = 0; x < width; ++x, src += 2, dst += 4) { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; }
            break;
        default:  // A8B8G8R8 and anything 32-bit RGBA-ordered
            std::memcpy(dst, src, width * 4);
            break;
    }
}

struct SamplerParamCache {
    GLint wrapS = -1, wrapT = -1, minF = -1, magF = -1;
};

// Memory accounting for the [NFSU2 PERF] report (the R36S has 1 GB of RAM for everything).
inline std::atomic<int64_t> g_texShadowBytes{0};   // CPU copies of texture levels held by surfaces
inline std::atomic<int64_t> g_texGLBytes{0};       // GL texture storage (RGBA8) we asked the driver for
inline std::atomic<int64_t> g_texRelocks{0};       // locks of a texture level that was already uploaded
inline std::atomic<int64_t> g_texReadbackFails{0}; // re-locks whose previous contents could not be read back
// Buffer updates (cumulative): count, bytes written, and size of the whole buffer they landed in (what a
// driver that copies a buffer still in use by the GPU has to copy).
inline std::atomic<int64_t> g_bufUnlocks{0}, g_bufUnlockBytes{0}, g_bufUnlockWholeBytes{0}, g_bufDynamicUnlocks{0};

// GL texture object shared by a D3D texture and its surfaces.
struct GLTextureObject {
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    UINT d3dLevels = 1;
    UINT glLevels = 1;  // levels allocated in GL (full chain when mipmapped)
    bool pot = true;
    bool renderTarget = false;
    uint32_t facesUploaded = 0;
    SamplerParamCache cache;
    // Storage format. Textures the bridge decoded from DXT (A8B8G8R8) whose first upload is fully opaque are
    // re-specified as RGB565: DXT colours are 565 already, and it halves their memory on the R36S.
    UINT width = 0, height = 0;
    bool rgb565 = false, formatDecided = false;
    int64_t* glBytes = nullptr;
    // Textures created off the render thread (the bridge's async CreateTexture) get their GL name and storage
    // on first use, on the render thread, in the format decided by then: an opaque DXT texture is allocated
    // as RGB565 once, instead of an RGBA8 chain that its first upload re-specifies.
    bool storagePending = false;

    // The GL texture name; creates the storage of a texture made off the render thread.
    GLuint name() const {
        if (storagePending) const_cast<GLTextureObject*>(this)->createStorage();
        return tex;
    }
    void createStorage() {  // 2D textures only (cube textures are never deferred)
        storagePending = false;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        const GLenum format = rgb565 ? GL_RGB : GL_RGBA, type = rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
        const int64_t bytesPerTexel = rgb565 ? 2 : 4;
        int64_t bytes = 0;
        UINT w = width, h = height;
        for (UINT level = 0; level < glLevels; ++level) {
            glTexImage2D(GL_TEXTURE_2D, level, format, std::max(w, 1u), std::max(h, 1u), 0, format, type, nullptr);
            bytes += static_cast<int64_t>(std::max(w, 1u)) * std::max(h, 1u) * bytesPerTexel;
            w >>= 1; h >>= 1;
        }
        if (glBytes) *glBytes += bytes;
        g_texGLBytes += bytes;
    }

    void respecifyAs565() {
        if (storagePending) { rgb565 = true; return; }   /* allocated as RGB565 directly on first use */
        rgb565 = true;
        glBindTexture(target, tex);
        const int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
        for (int f = 0; f < faces; ++f) {
            const GLenum t = target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D;
            UINT w = width, h = height;
            for (UINT i = 0; i < glLevels; ++i) {
                glTexImage2D(t, i, GL_RGB, std::max(w, 1u), std::max(h, 1u), 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, nullptr);
                w >>= 1; h >>= 1;
            }
        }
        if (glBytes) { g_texGLBytes -= *glBytes / 2; *glBytes -= *glBytes / 2; }
    }

    // Whether sampling may use mipmap filters: the GL chain is allocated complete only when D3D
    // has more than one level (D3D ignores MIPFILTER for single-level textures).
    bool mipmapped() const { return d3dLevels > 1 && (pot || glesCaps().npot); }

    void onUpload(UINT level, GLenum faceTarget) {
        // Generate only the GL levels D3D does not expose; when the game supplies every level itself,
        // regenerating on each level-0 upload is wasted work (and then overwritten).
        if (level != 0 || !mipmapped() || renderTarget || d3dLevels >= glLevels) return;
        if (target == GL_TEXTURE_2D) {
            glBindTexture(GL_TEXTURE_2D, name());
            glsc::DriverCpuTimer timer;
            glGenerateMipmap(GL_TEXTURE_2D);
        } else {
            facesUploaded |= 1u << (faceTarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
            if (facesUploaded == 0x3F) {
                glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
                glsc::DriverCpuTimer timer;
                glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
            }
        }
    }
};

inline UINT fullMipCount(UINT w, UINT h) {
    UINT d = std::max(w, h), n = 1;
    while (d > 1) { d >>= 1; ++n; }
    return n;
}

// -----------------------------------------------------------------------------
// IDirect3DSurface9GLES
// -----------------------------------------------------------------------------
class IDirect3DSurface9GLES : public D3D9ComObject<IDirect3DSurface9> {
public:
    IDirect3DSurface9GLES(UINT width, UINT height, D3DFORMAT format, GLuint glTexture = 0, UINT level = 0,
                          GLenum target = GL_TEXTURE_2D, GLTextureObject* owner = nullptr)
        : m_width(width), m_height(height), m_format(format), m_glTexture(glTexture), m_level(level),
          m_target(target), m_owner(owner) {
        m_bpp = d3dFormatBytesPerPixel(format);
        const bool isDepth = (format == D3DFMT_D24S8 || format == D3DFMT_D16 || format == D3DFMT_D24X8 ||
                              format == D3DFMT_D24FS8 || format == D3DFMT_D32 || format == D3DFMT_D15S1 ||
                              format == D3DFMT_D24X4S4);
        if (!glTexture && !owner && isDepth) {   // a plain depth surface (a texture level's GL texture may be deferred)
            glGenRenderbuffers(1, &m_glRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, m_glRenderbuffer);
            if (glesCaps().depth24Stencil8) {
                glRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /* GL_DEPTH24_STENCIL8_OES */, width, height);
                m_hasStencil = true;
            } else {
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
            }
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }
        // Texture levels get their CPU copy only while locked (see LockRect/UnlockRect).
    }

    // D3D9: a texture's surfaces share the texture's reference count (AddRef/Release go to the container).
    // NFSU2 relies on it: it releases the cube texture after every GetCubeMapSurface.
    void setContainer(IUnknown* container) { m_container = container; }
    STDMETHOD_(ULONG, AddRef)() override { return m_container ? m_container->AddRef() : D3D9ComObject<IDirect3DSurface9>::AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return m_container ? m_container->Release() : D3D9ComObject<IDirect3DSurface9>::Release(); }

    ~IDirect3DSurface9GLES() override {
        g_texShadowBytes -= static_cast<int64_t>(m_cpuBuffer.capacity());
        if (m_glRenderbuffer) {
            glDeleteRenderbuffers(1, &m_glRenderbuffer);
            m_glRenderbuffer = 0;
        }
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { return E_NOTIMPL; }
    STDMETHOD(FreePrivateData)(REFGUID refguid) override { return S_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_SURFACE; }
    STDMETHOD(GetContainer)(REFIID riid, void** ppContainer) override {
        if (!ppContainer) return D3DERR_INVALIDCALL;
        if (!m_container) return E_NOINTERFACE;
        m_container->AddRef();
        *ppContainer = m_container;
        return D3D_OK;
    }

    STDMETHOD(GetDesc)(D3DSURFACE_DESC* pDesc) override {
        if (!pDesc) return D3DERR_INVALIDCALL;
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_SURFACE;
        pDesc->Usage = (m_owner && m_owner->renderTarget) ? D3DUSAGE_RENDERTARGET : 0;
        pDesc->Pool = D3DPOOL_DEFAULT;
        pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
        pDesc->MultiSampleQuality = 0;
        pDesc->Width = m_width;
        pDesc->Height = m_height;
        return D3D_OK;
    }

    // Texture levels keep no CPU copy once uploaded: on the R36S (1 GB shared by CPU and GPU) a permanent
    // copy of every level (RGBA8 for the DXT textures the Mali cannot sample) doubled texture memory and
    // pushed the game into swap. Assets are locked once at load. A level locked again gets its previous
    // contents read back from GL and then keeps its copy for good (dynamic textures).
    STDMETHOD(LockRect)(D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) override {
        if (!pLockedRect) return D3DERR_INVALIDCALL;
        if (m_cpuBuffer.empty()) {
            m_cpuBuffer.resize(static_cast<size_t>(m_width) * m_height * m_bpp, 0);
            g_texShadowBytes += static_cast<int64_t>(m_cpuBuffer.capacity());
            if (isTextureLevel() && m_uploaded) {
                ++g_texRelocks;
                m_keepShadow = true;
                if (!(Flags & D3DLOCK_DISCARD) && !readBack()) ++g_texReadbackFails;
            }
        }
        const size_t pitch = static_cast<size_t>(m_width) * m_bpp;
        size_t offset = 0;
        if (pRect) offset = static_cast<size_t>(pRect->top) * pitch + static_cast<size_t>(pRect->left) * m_bpp;
        pLockedRect->Pitch = static_cast<INT>(pitch);
        pLockedRect->pBits = m_cpuBuffer.data() + offset;
        m_readOnlyLock = (Flags & D3DLOCK_READONLY) != 0;
        return D3D_OK;
    }

    STDMETHOD(UnlockRect)() override {
        if (!isTextureLevel()) return D3D_OK;
        if (!m_readOnlyLock) {
            HitchTimer hitch(2);
            m_upload.resize(static_cast<size_t>(m_width) * m_height * 4);
            const size_t pitch = static_cast<size_t>(m_width) * m_bpp;
            for (UINT y = 0; y < m_height; ++y) {
                convertRowToRGBA8(m_format, m_cpuBuffer.data() + y * pitch, m_upload.data() + static_cast<size_t>(y) * m_width * 4, m_width);
            }
            const size_t texels = static_cast<size_t>(m_width) * m_height;
            if (m_owner && !m_owner->formatDecided && !m_owner->renderTarget) {
                m_owner->formatDecided = true;
                bool opaque = m_format == D3DFMT_A8B8G8R8;
                for (size_t i = 0; opaque && i < texels; ++i) opaque = m_upload[i * 4 + 3] == 255;
                if (opaque) m_owner->respecifyAs565();
            }
            glBindTexture(m_owner ? m_owner->target : m_target, glTexture());   // after the format decision above
            if (m_owner && m_owner->rgb565) {
                // In place: RGBA8 -> RGB565 into the front half of the same buffer.
                uint16_t* out = reinterpret_cast<uint16_t*>(m_upload.data());
                for (size_t i = 0; i < texels; ++i) {
                    const uint8_t* c = &m_upload[i * 4];
                    out[i] = static_cast<uint16_t>(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
                }
                { glsc::DriverCpuTimer timer; glTexSubImage2D(m_target, m_level, 0, 0, m_width, m_height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, m_upload.data()); }
            } else {
                { glsc::DriverCpuTimer timer; glTexSubImage2D(m_target, m_level, 0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, m_upload.data()); }
            }
            if (m_owner) m_owner->onUpload(m_level, m_target);
            std::vector<uint8_t>().swap(m_upload);
            m_uploaded = true;
        }
        if (!m_keepShadow && m_uploaded) {
            g_texShadowBytes -= static_cast<int64_t>(m_cpuBuffer.capacity());
            std::vector<uint8_t>().swap(m_cpuBuffer);
        }
        return D3D_OK;
    }

    // Previous contents of an uploaded level, from GL (RGBA8) back into the D3D layout. Works for the
    // 32-bit formats (A8R8G8B8, X8R8G8B8, A8B8G8R8: all DXT fallbacks); anything else stays zero.
    bool readBack() {
        if (m_bpp != 4) return false;
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        static GLuint fbo = 0;
        if (!fbo) glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_target, glTexture(), static_cast<GLint>(m_level));
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, m_cpuBuffer.data());
            if (m_format == D3DFMT_A8R8G8B8 || m_format == D3DFMT_X8R8G8B8) {
                for (size_t i = 0; i + 3 < m_cpuBuffer.size(); i += 4) std::swap(m_cpuBuffer[i], m_cpuBuffer[i + 2]);
            }
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_target, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
        return ok;
    }

    STDMETHOD(GetDC)(HDC* phdc) override { return E_NOTIMPL; }
    STDMETHOD(ReleaseDC)(HDC hdc) override { return E_NOTIMPL; }

    GLuint getGLTexture() const { return glTexture(); }
    // The level was uploaded without going through LockRect/UnlockRect (IDirect3DTexture9GLES::uploadDecodedLevel).
    void markUploaded() { m_uploaded = true; }
    UINT getWidth() const { return m_width; }
    UINT getHeight() const { return m_height; }
    GLenum getTarget() const { return m_target; }
    UINT getLevel() const { return m_level; }
    D3DFORMAT getFormat() const { return m_format; }
    GLuint getGLRenderbuffer() const { return m_glRenderbuffer; }
    bool hasStencil() const { return m_hasStencil; }
    const std::vector<uint8_t>& getCpuBuffer() const { return m_cpuBuffer; }
    GLTextureObject* getOwner() const { return m_owner; }

private:
    // A level of a GL texture (its owner's, created on first use when deferred), or a plain surface (0).
    bool isTextureLevel() const { return m_owner != nullptr || m_glTexture != 0; }
    GLuint glTexture() const { return m_owner ? m_owner->name() : m_glTexture; }

    UINT m_width;
    UINT m_height;
    D3DFORMAT m_format;
    GLuint m_glTexture;
    UINT m_level;
    GLenum m_target;
    GLTextureObject* m_owner = nullptr;
    IUnknown* m_container = nullptr;
    UINT m_bpp = 4;
    GLuint m_glRenderbuffer = 0;
    bool m_hasStencil = false;
    bool m_readOnlyLock = false;
    bool m_uploaded = false;    // GL holds this level's data
    bool m_keepShadow = false;  // locked more than once: keep the CPU copy
    std::vector<uint8_t> m_cpuBuffer;
    std::vector<uint8_t> m_upload;
};

// -----------------------------------------------------------------------------
// IDirect3DTexture9GLES
// -----------------------------------------------------------------------------
class IDirect3DTexture9GLES : public D3D9ComObject<IDirect3DTexture9> {
public:
    IDirect3DTexture9GLES(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format)
        : m_width(width), m_height(height), m_format(format) {
        const UINT full = fullMipCount(width, height);
        UINT numLevels = (levels == 0 || levels > full) ? full : levels;
        if (usage & D3DUSAGE_AUTOGENMIPMAP) numLevels = full;
        m_obj.target = GL_TEXTURE_2D;
        m_obj.d3dLevels = (usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : numLevels;  // autogen: D3D exposes 1 level
        m_obj.pot = ((width & (width - 1)) == 0) && ((height & (height - 1)) == 0);
        m_obj.renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
        // Allocate the complete GL chain whenever mipmaps are in use so the texture is always complete.
        const UINT glLevels = m_obj.mipmapped() ? full : 1;
        m_obj.glLevels = glLevels;
        m_obj.width = width;
        m_obj.height = height;
        m_obj.glBytes = &m_glBytes;
        if (g_glesDeferTextureGL && !m_obj.renderTarget) m_obj.storagePending = true;   // GL storage on first use
        else m_obj.createStorage();
        const UINT d3dVisible = (usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : numLevels;
        UINT w = width, h = height;
        for (UINT i = 0; i < d3dVisible; ++i) {
            m_surfaces.push_back(new IDirect3DSurface9GLES(std::max(w, 1u), std::max(h, 1u), format, m_obj.tex, i, GL_TEXTURE_2D, &m_obj));
            m_surfaces.back()->setContainer(this);
            w >>= 1; h >>= 1;
        }
    }

    ~IDirect3DTexture9GLES() override {
        g_texGLBytes -= m_glBytes;
        for (auto* s : m_surfaces) delete s;
        if (m_obj.tex) glDeleteTextures(1, &m_obj.tex);
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { return E_NOTIMPL; }
    STDMETHOD(FreePrivateData)(REFGUID refguid) override { return S_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override { return 0; }
    STDMETHOD_(DWORD, GetLOD)() override { return 0; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return static_cast<DWORD>(m_surfaces.size()); }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) override { return S_OK; }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return D3DTEXF_LINEAR; }
    STDMETHOD_(void, GenerateMipSubLevels)() override {}

    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override {
        if (Level >= m_surfaces.size() || !pDesc) return D3DERR_INVALIDCALL;
        return m_surfaces[Level]->GetDesc(pDesc);
    }
    STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override {
        if (Level >= m_surfaces.size() || !ppSurfaceLevel) return D3DERR_INVALIDCALL;
        *ppSurfaceLevel = m_surfaces[Level];
        m_surfaces[Level]->AddRef();
        return D3D_OK;
    }
    STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) override {
        if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
        return m_surfaces[Level]->LockRect(pLockedRect, pRect, Flags);
    }
    STDMETHOD(UnlockRect)(UINT Level) override {
        if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
        return m_surfaces[Level]->UnlockRect();
    }
    STDMETHOD(AddDirtyRect)(const RECT* pDirtyRect) override { return S_OK; }

    GLuint getGLTexture() const { return m_obj.name(); }
    GLTextureObject* glObject() { return &m_obj; }
    UINT getWidth() const { return m_width; }
    UINT getHeight() const { return m_height; }
    D3DFORMAT getFormat() const { return m_format; }

    // A whole level of a texture the bridge decodes from DXT (A8B8G8R8), already decoded off the render thread:
    // the same result as LockRect, copying the pixels in and UnlockRect (format decision, RGB565, upload), without
    // the two full copies and the conversions on this thread. rgb565: the same pixels as RGB565 when the level is
    // fully opaque (worker-made), else null.
    bool uploadDecodedLevel(UINT level, const uint8_t* rgba, const uint8_t* rgb565) {
        if (level >= m_surfaces.size() || m_format != D3DFMT_A8B8G8R8 || m_obj.renderTarget) return false;
        IDirect3DSurface9GLES* surface = m_surfaces[level];
        if (!surface->getCpuBuffer().empty()) return false;   // a level kept for re-locks: the regular path
        HitchTimer hitch(2);
        if (!m_obj.formatDecided) {
            m_obj.formatDecided = true;
            if (rgb565) m_obj.respecifyAs565();   // first upload fully opaque, as UnlockRect decides
        }
        const UINT w = surface->getWidth(), h = surface->getHeight();
        glBindTexture(GL_TEXTURE_2D, m_obj.name());
        if (m_obj.rgb565) {
            std::vector<uint16_t> converted;
            if (!rgb565) {   // a level with alpha in a texture already stored as RGB565: drop alpha as UnlockRect does
                converted.resize(static_cast<size_t>(w) * h);
                for (size_t i = 0; i < converted.size(); ++i) {
                    const uint8_t* c = rgba + i * 4;
                    converted[i] = static_cast<uint16_t>(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
                }
                rgb565 = reinterpret_cast<const uint8_t*>(converted.data());
            }
            glsc::DriverCpuTimer timer;
            glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
        } else {
            glsc::DriverCpuTimer timer;
            glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        m_obj.onUpload(level, GL_TEXTURE_2D);
        surface->markUploaded();
        return true;
    }
    IDirect3DSurface9GLES* getSurfaceLevel(UINT level) const { return (level < m_surfaces.size()) ? m_surfaces[level] : nullptr; }

private:
    GLTextureObject m_obj;
    UINT m_width;
    UINT m_height;
    D3DFORMAT m_format;
    int64_t m_glBytes = 0;
    std::vector<IDirect3DSurface9GLES*> m_surfaces;
};

// -----------------------------------------------------------------------------
// IDirect3DCubeTexture9GLES
// -----------------------------------------------------------------------------
class IDirect3DCubeTexture9GLES : public D3D9ComObject<IDirect3DCubeTexture9> {
public:
    IDirect3DCubeTexture9GLES(UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format)
        : m_edgeLength(edgeLength), m_format(format) {
        const UINT full = fullMipCount(edgeLength, edgeLength);
        UINT numLevels = (levels == 0 || levels > full) ? full : levels;
        if (usage & D3DUSAGE_AUTOGENMIPMAP) numLevels = 1;
        m_obj.target = GL_TEXTURE_CUBE_MAP;
        m_obj.d3dLevels = numLevels;
        m_obj.pot = (edgeLength & (edgeLength - 1)) == 0;
        m_obj.renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
        glGenTextures(1, &m_obj.tex);
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_obj.tex);
        const UINT glLevels = m_obj.mipmapped() ? full : 1;
        m_obj.glLevels = glLevels;
        m_obj.width = m_obj.height = edgeLength;
        m_obj.glBytes = &m_glBytes;
        for (int face = 0; face < 6; ++face) {
            const GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
            UINT sz = edgeLength;
            for (UINT lvl = 0; lvl < glLevels; ++lvl) {
                glTexImage2D(target, lvl, GL_RGBA, std::max(sz, 1u), std::max(sz, 1u), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                m_glBytes += static_cast<int64_t>(std::max(sz, 1u)) * std::max(sz, 1u) * 4;
                if (lvl < numLevels) {
                    m_surfaces[face].push_back(new IDirect3DSurface9GLES(std::max(sz, 1u), std::max(sz, 1u), format, m_obj.tex, lvl, target, &m_obj));
                    m_surfaces[face].back()->setContainer(this);
                }
                sz >>= 1;
            }
        }
        g_texGLBytes += m_glBytes;
    }

    ~IDirect3DCubeTexture9GLES() override {
        g_texGLBytes -= m_glBytes;
        for (int face = 0; face < 6; ++face) {
            for (auto* s : m_surfaces[face]) delete s;
        }
        if (m_obj.tex) glDeleteTextures(1, &m_obj.tex);
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { return E_NOTIMPL; }
    STDMETHOD(FreePrivateData)(REFGUID refguid) override { return S_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_CUBETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override { return 0; }
    STDMETHOD_(DWORD, GetLOD)() override { return 0; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return static_cast<DWORD>(m_surfaces[0].size()); }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE FilterType) override { return S_OK; }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return D3DTEXF_LINEAR; }
    STDMETHOD_(void, GenerateMipSubLevels)() override {}

    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override {
        if (Level >= m_surfaces[0].size() || !pDesc) return D3DERR_INVALIDCALL;
        return m_surfaces[0][Level]->GetDesc(pDesc);
    }
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9** ppCubeMapSurface) override {
        int faceIdx = static_cast<int>(FaceType);
        if (faceIdx < 0 || faceIdx >= 6 || Level >= m_surfaces[faceIdx].size() || !ppCubeMapSurface) return D3DERR_INVALIDCALL;
        *ppCubeMapSurface = m_surfaces[faceIdx][Level];
        m_surfaces[faceIdx][Level]->AddRef();
        return D3D_OK;
    }
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT* pLockedRect, const RECT* pRect, DWORD Flags) override {
        int faceIdx = static_cast<int>(FaceType);
        if (faceIdx < 0 || faceIdx >= 6 || Level >= m_surfaces[faceIdx].size()) return D3DERR_INVALIDCALL;
        return m_surfaces[faceIdx][Level]->LockRect(pLockedRect, pRect, Flags);
    }
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) override {
        int faceIdx = static_cast<int>(FaceType);
        if (faceIdx < 0 || faceIdx >= 6 || Level >= m_surfaces[faceIdx].size()) return D3DERR_INVALIDCALL;
        return m_surfaces[faceIdx][Level]->UnlockRect();
    }
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES FaceType, const RECT* pDirtyRect) override { return S_OK; }

    GLuint getGLTexture() const { return m_obj.tex; }
    GLTextureObject* glObject() { return &m_obj; }
    UINT getEdgeLength() const { return m_edgeLength; }

private:
    int64_t m_glBytes = 0;
    GLTextureObject m_obj;
    UINT m_edgeLength;
    D3DFORMAT m_format;
    std::vector<IDirect3DSurface9GLES*> m_surfaces[6];
};

// -----------------------------------------------------------------------------
// IDirect3DVertexBuffer9GLES
// -----------------------------------------------------------------------------
class IDirect3DVertexBuffer9GLES : public D3D9ComObject<IDirect3DVertexBuffer9> {
public:
    IDirect3DVertexBuffer9GLES(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
        : m_length(length), m_usage(usage), m_fvf(fvf), m_pool(pool) {
        m_data.resize(length, 0);
        if (!g_glesDeferBufferGL) ensureGL(false);
    }
    // The GL buffer: at creation, or (bridge-created on the game thread) at first use on the render thread,
    // filled from the CPU copy.
    void ensureGL(bool withData = true) const {
        if (m_vbo) return;
        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_length, withData ? m_data.data() : nullptr, (m_usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    ~IDirect3DVertexBuffer9GLES() override {
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        for (auto& variant : m_swizzledColors) {
            if (variant.buffer) glDeleteBuffers(1, &variant.buffer);
        }
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { return E_NOTIMPL; }
    STDMETHOD(FreePrivateData)(REFGUID refguid) override { return S_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VERTEXBUFFER; }

    STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override {
        if (!ppbData) return D3DERR_INVALIDCALL;
        UINT size = (SizeToLock == 0) ? (m_length - OffsetToLock) : SizeToLock;
        if (OffsetToLock + size > m_length) return D3DERR_INVALIDCALL;
        *ppbData = m_data.data() + OffsetToLock;
        m_lockedOffset = OffsetToLock;
        m_lockedSize = size;
        return D3D_OK;
    }

    STDMETHOD(Unlock)() override {
        ++g_bufUnlocks; g_bufUnlockBytes += m_lockedSize; g_bufUnlockWholeBytes += m_length;
        if (m_usage & D3DUSAGE_DYNAMIC) ++g_bufDynamicUnlocks;
        ensureGL();
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, m_lockedOffset, m_lockedSize, m_data.data() + m_lockedOffset);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ++m_dataRevision;
        return D3D_OK;
    }

    const uint8_t* cpuData() const { return m_data.data(); }
    UINT cpuLength() const { return m_length; }
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC* pDesc) override {
        if (!pDesc) return D3DERR_INVALIDCALL;
        pDesc->Format = D3DFMT_VERTEXDATA;
        pDesc->Type = D3DRTYPE_VERTEXBUFFER;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = m_length;
        pDesc->FVF = m_fvf;
        return D3D_OK;
    }

    GLuint getVBO() const { ensureGL(); return m_vbo; }
    GLuint getSwizzledColorVBO(UINT stride, UINT colorOffset) {
        ensureGL();
        if (!stride || colorOffset + sizeof(uint32_t) > stride) return m_vbo;

        auto found = std::find_if(m_swizzledColors.begin(), m_swizzledColors.end(),
            [stride, colorOffset](const SwizzledColorBuffer& entry) {
                return entry.stride == stride && entry.colorOffset == colorOffset;
            });
        if (found == m_swizzledColors.end()) {
            SwizzledColorBuffer entry{};
            entry.stride = stride;
            entry.colorOffset = colorOffset;
            glGenBuffers(1, &entry.buffer);
            m_swizzledColors.push_back(entry);
            found = std::prev(m_swizzledColors.end());
        }
        if (found->revision != m_dataRevision) {
            const size_t vertices = m_data.size() / stride;
            std::vector<uint8_t> rgba(vertices * sizeof(uint32_t));
            for (size_t vertex = 0; vertex < vertices; ++vertex) {
                const uint8_t* source = m_data.data() + vertex * stride + colorOffset;
                uint8_t* color = rgba.data() + vertex * sizeof(uint32_t);
                color[0] = source[2];
                color[1] = source[1];
                color[2] = source[0];
                color[3] = source[3];
            }
            GLint previousArrayBuffer = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, found->buffer);
            glBufferData(GL_ARRAY_BUFFER, rgba.size(), rgba.data(),
                         (m_usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
            found->revision = m_dataRevision;
        }
        return found->buffer;
    }
    const uint8_t* getData() const { return m_data.data(); }
    UINT getLength() const { return m_length; }
    UINT getSize() const { return m_length; }

private:
    struct SwizzledColorBuffer {
        UINT stride = 0;
        UINT colorOffset = 0;
        GLuint buffer = 0;
        uint64_t revision = 0;
    };

    UINT m_length;
    DWORD m_usage;
    DWORD m_fvf;
    D3DPOOL m_pool;
    mutable GLuint m_vbo = 0;
    std::vector<uint8_t> m_data;
    uint64_t m_dataRevision = 1;
    std::vector<SwizzledColorBuffer> m_swizzledColors;
    UINT m_lockedOffset = 0;
    UINT m_lockedSize = 0;
};

// -----------------------------------------------------------------------------
// IDirect3DIndexBuffer9GLES
// -----------------------------------------------------------------------------
class IDirect3DIndexBuffer9GLES : public D3D9ComObject<IDirect3DIndexBuffer9> {
public:
    IDirect3DIndexBuffer9GLES(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
        : m_length(length), m_usage(usage), m_format(format), m_pool(pool) {
        m_data.resize(length, 0);
        if (!g_glesDeferBufferGL) ensureGL(false);
    }
    void ensureGL(bool withData = true) const {
        if (m_ibo) return;
        glGenBuffers(1, &m_ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_length, withData ? m_data.data() : nullptr, (m_usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    ~IDirect3DIndexBuffer9GLES() override {
        if (m_ibo) glDeleteBuffers(1, &m_ibo);
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(SetPrivateData)(REFGUID refguid, const void* pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    STDMETHOD(GetPrivateData)(REFGUID refguid, void* pData, DWORD* pSizeOfData) override { return E_NOTIMPL; }
    STDMETHOD(FreePrivateData)(REFGUID refguid) override { return S_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_INDEXBUFFER; }

    STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override {
        if (!ppbData) return D3DERR_INVALIDCALL;
        UINT size = (SizeToLock == 0) ? (m_length - OffsetToLock) : SizeToLock;
        if (OffsetToLock + size > m_length) return D3DERR_INVALIDCALL;
        *ppbData = m_data.data() + OffsetToLock;
        m_lockedOffset = OffsetToLock;
        m_lockedSize = size;
        return D3D_OK;
    }

    STDMETHOD(Unlock)() override {
        ensureGL();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
        ++g_bufUnlocks; g_bufUnlockBytes += m_lockedSize; g_bufUnlockWholeBytes += m_length;
        if (m_usage & D3DUSAGE_DYNAMIC) ++g_bufDynamicUnlocks;
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, m_lockedOffset, m_lockedSize, m_data.data() + m_lockedOffset);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        return D3D_OK;
    }

    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC* pDesc) override {
        if (!pDesc) return D3DERR_INVALIDCALL;
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_INDEXBUFFER;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = m_length;
        return D3D_OK;
    }

    GLuint getIBO() const { ensureGL(); return m_ibo; }
    D3DFORMAT getFormat() const { return m_format; }
    const uint8_t* getData() const { return m_data.data(); }
    UINT getLength() const { return m_length; }
    UINT getSize() const { return m_length; }

private:
    UINT m_length;
    DWORD m_usage;
    D3DFORMAT m_format;
    D3DPOOL m_pool;
    mutable GLuint m_ibo = 0;
    std::vector<uint8_t> m_data;
    UINT m_lockedOffset = 0;
    UINT m_lockedSize = 0;
};

// -----------------------------------------------------------------------------
// IDirect3DVertexDeclaration9GLES
// -----------------------------------------------------------------------------
class IDirect3DVertexDeclaration9GLES : public D3D9ComObject<IDirect3DVertexDeclaration9> {
public:
    explicit IDirect3DVertexDeclaration9GLES(const D3DVERTEXELEMENT9* pElements) {
        if (pElements) {
            while (pElements->Stream != 0xFF) {
                m_elements.push_back(*pElements);
                ++pElements;
            }
            // End element
            m_elements.push_back(*pElements);
        }
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }

    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9* pElement, UINT* pNumElements) override {
        if (pNumElements) *pNumElements = static_cast<UINT>(m_elements.size());
        if (pElement) std::memcpy(pElement, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
        return D3D_OK;
    }

    const std::vector<D3DVERTEXELEMENT9>& getElements() const { return m_elements; }

private:
    std::vector<D3DVERTEXELEMENT9> m_elements;
};

// -----------------------------------------------------------------------------
// IDirect3DQuery9GLES
// -----------------------------------------------------------------------------
class IDirect3DQuery9GLES : public D3D9ComObject<IDirect3DQuery9> {
public:
    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD_(D3DQUERYTYPE, GetType)() override { return D3DQUERYTYPE_EVENT; }
    STDMETHOD_(DWORD, GetDataSize)() override { return sizeof(BOOL); }
    STDMETHOD(Issue)(DWORD dwIssueFlags) override { return S_OK; }
    STDMETHOD(GetData)(void* pData, DWORD dwSize, DWORD dwGetDataFlags) override {
        if (pData && dwSize >= sizeof(BOOL)) {
            *reinterpret_cast<BOOL*>(pData) = TRUE;
        }
        return S_OK;
    }
};

// -----------------------------------------------------------------------------
// IDirect3DSwapChain9GLES
// -----------------------------------------------------------------------------
class IDirect3DSwapChain9GLES : public D3D9ComObject<IDirect3DSwapChain9> {
public:
    IDirect3DSwapChain9GLES(UINT width, UINT height, D3DFORMAT format) {
        m_backBuffer = new IDirect3DSurface9GLES(width, height, format);
    }

    ~IDirect3DSwapChain9GLES() override {
        if (m_backBuffer) m_backBuffer->Release();
    }

    STDMETHOD(Present)(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags) override {
        return D3D_OK;
    }
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9* pDestSurface) override { return E_NOTIMPL; }
    STDMETHOD(GetBackBuffer)(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override {
        if (!ppBackBuffer) return D3DERR_INVALIDCALL;
        *ppBackBuffer = m_backBuffer;
        m_backBuffer->AddRef();
        return D3D_OK;
    }
    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS* pRasterStatus) override { return E_NOTIMPL; }
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE* pMode) override { return E_NOTIMPL; }
    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override { return glesGetDevice(ppDevice); }
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS* pPresentationParameters) override { return E_NOTIMPL; }

private:
    IDirect3DSurface9GLES* m_backBuffer = nullptr;
};

} // namespace d3d9_gles
