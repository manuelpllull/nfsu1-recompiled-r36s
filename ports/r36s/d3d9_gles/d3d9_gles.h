#pragma once

#include <d3d9.h>
#ifdef __ANDROID__
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/log.h>
struct SDL_Window;
typedef void* SDL_GLContext;
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>
#endif
#include "gl_state_cache.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <chrono>

// Per-frame hitch accounting on the render thread (NFSU2 HITCH lines, d3d9_gles.cpp Present):
// 0 shader compiles, 1 program links, 2 texture level uploads (convert + glTexSubImage2D), 3 texture creation.
inline std::atomic<uint64_t> g_hitchNs[4];
inline std::atomic<uint32_t> g_hitchN[4];
struct HitchTimer {
    int kind; std::chrono::steady_clock::time_point t0;
    explicit HitchTimer(int k) : kind(k), t0(std::chrono::steady_clock::now()) {}
    ~HitchTimer() {
        g_hitchNs[kind].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
        g_hitchN[kind].fetch_add(1, std::memory_order_relaxed);
    }
};

#ifndef STDMETHODIMP
#define STDMETHODIMP HRESULT
#endif

// The single device of this backend. Every resource's GetDevice returns it (D3DX relies on this:
// with GetDevice failing on shaders it silently never uploads any shader constants).
inline IDirect3DDevice9*& glesCurrentDevice() { static IDirect3DDevice9* d = nullptr; return d; }
inline HRESULT glesGetDevice(IDirect3DDevice9** ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = glesCurrentDevice();
    if (!*ppDevice) return D3DERR_INVALIDCALL;
    (*ppDevice)->AddRef();
    return D3D_OK;
}

// Base template for lightweight COM object implementation
template <typename BaseInterface>
class D3D9ComObject : public BaseInterface {
public:
    D3D9ComObject() : m_refCount(1) {}
    virtual ~D3D9ComObject() = default;

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        *ppvObj = this;
        AddRef();
        return S_OK;
    }

    STDMETHOD_(ULONG, AddRef)() override {
        return ++m_refCount;
    }

    STDMETHOD_(ULONG, Release)() override {
        ULONG count = --m_refCount;
        if (count == 0) {
            delete this;
        }
        return count;
    }

protected:
    std::atomic<ULONG> m_refCount;
};

// Public WSI & bridge functions (matching DXVK bridge interface)
extern "C" {
    IDirect3D9* Direct3DCreate9(UINT sdk);
    void dxvkAndroidSetWindow(void* window);
    HWND dxvkAndroidGetWindowHandle();
    void dxvkAndroidSetRenderSize(uint32_t width, uint32_t height);
    uint32_t getD3D9PresentCount();
}
