#pragma once

#include "d3d9_gles.h"
#include "d3d9_gles_state.h"
#include "d3d9_gles_resources.h"
#include "d3d9_gles_shaders.h"

#include <array>

namespace d3d9_gles {

class IDirect3D9GLES;

// -----------------------------------------------------------------------------
// IDirect3DDevice9GLES
// -----------------------------------------------------------------------------
class IDirect3DDevice9GLES : public D3D9ComObject<IDirect3DDevice9> {
public:
    IDirect3DDevice9GLES(IDirect3D9GLES* parentD3D, SDL_Window* window, const D3DPRESENT_PARAMETERS& params);
    ~IDirect3DDevice9GLES() override;

    // --- IUnknown & Device Info ---
    STDMETHOD(TestCooperativeLevel)() override { return D3D_OK; }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override { return 256 * 1024 * 1024; }
    STDMETHOD(EvictManagedResources)() override { return D3D_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D9** ppD3D9) override;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9* pCaps) override;
    STDMETHOD(GetDisplayMode)(UINT iSwapChain, D3DDISPLAYMODE* pMode) override;
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS* pParameters) override;
    STDMETHOD(SetCursorProperties)(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override { return D3D_OK; }
    STDMETHOD_(void, SetCursorPosition)(int X, int Y, DWORD Flags) override {}
    STDMETHOD_(BOOL, ShowCursor)(BOOL bShow) override { return TRUE; }
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override;
    STDMETHOD(GetSwapChain)(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override;
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override { return 1; }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) override;
    STDMETHOD(Present)(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override;
    STDMETHOD(GetBackBuffer)(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override;
    STDMETHOD(GetRasterStatus)(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { return E_NOTIMPL; }
    STDMETHOD(SetDialogBoxMode)(BOOL bEnableDialogs) override { return D3D_OK; }
    STDMETHOD_(void, SetGammaRamp)(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) override {}
    STDMETHOD_(void, GetGammaRamp)(UINT iSwapChain, D3DGAMMARAMP* pRamp) override {}

    // --- Resource Creation ---
    STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override { return E_NOTIMPL; }
    STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, const POINT* pDestPoint) override { noteUnimplemented("UpdateSurface"); return D3D_OK; }
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override { noteUnimplemented("UpdateTexture"); return D3D_OK; }
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override { noteUnimplemented("GetRenderTargetData"); return E_NOTIMPL; }
    STDMETHOD(GetFrontBufferData)(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override { return E_NOTIMPL; }
    STDMETHOD(StretchRect)(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) override { noteUnimplemented("ColorFill"); return D3D_OK; }
    STDMETHOD(CreateOffscreenPlainSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return E_NOTIMPL; }

    // --- Render Targets & Depth ---
    STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override;
    STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil) override;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** ppZStencilSurface) override;

    // --- Scene & Clear ---
    STDMETHOD(BeginScene)() override { return D3D_OK; }
    STDMETHOD(EndScene)() override { return D3D_OK; }
    STDMETHOD(Clear)(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;

    // --- Transforms & Viewport ---
    struct Matrix4 {
        float m[4][4];
        static Matrix4 identity() {
            Matrix4 r{};
            for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
            return r;
        }
        static Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
            Matrix4 r{};
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    r.m[i][j] = a.m[i][0] * b.m[0][j] +
                                a.m[i][1] * b.m[1][j] +
                                a.m[i][2] * b.m[2][j] +
                                a.m[i][3] * b.m[3][j];
                }
            }
            return r;
        }
    };
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override;
    STDMETHOD(SetViewport)(const D3DVIEWPORT9* pViewport) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT9* pViewport) override;

    // --- Material & Lighting ---
    STDMETHOD(SetMaterial)(const D3DMATERIAL9* pMaterial) override { return D3D_OK; }
    STDMETHOD(GetMaterial)(D3DMATERIAL9* pMaterial) override { return E_NOTIMPL; }
    STDMETHOD(SetLight)(DWORD Index, const D3DLIGHT9* pLight) override { return D3D_OK; }
    STDMETHOD(GetLight)(DWORD Index, D3DLIGHT9* pLight) override { return E_NOTIMPL; }
    STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) override { return D3D_OK; }
    STDMETHOD(GetLightEnable)(DWORD Index, BOOL* pEnable) override { return E_NOTIMPL; }
    STDMETHOD(SetClipPlane)(DWORD Index, const float* pPlane) override { noteUnimplemented("SetClipPlane"); return D3D_OK; }
    STDMETHOD(GetClipPlane)(DWORD Index, float* pPlane) override { return E_NOTIMPL; }

    // --- Render States ---
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD* pValue) override;

    // --- State Blocks ---
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override;
    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9** ppSB) override;
    friend class IDirect3DStateBlock9GLES;

    STDMETHOD(SetClipStatus)(const D3DCLIPSTATUS9* pClipStatus) override { return D3D_OK; }
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9* pClipStatus) override { return E_NOTIMPL; }

    // --- Textures & Samplers ---
    STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override;
    STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture9* pTexture) override;
    STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override;
    STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    STDMETHOD(GetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override;
    STDMETHOD(SetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    STDMETHOD(ValidateDevice)(DWORD* pNumPasses) override { if (pNumPasses) *pNumPasses = 1; return D3D_OK; }

    STDMETHOD(SetPaletteEntries)(UINT PaletteNumber, const PALETTEENTRY* pEntries) override { return D3D_OK; }
    STDMETHOD(GetPaletteEntries)(UINT PaletteNumber, PALETTEENTRY* pEntries) override { return E_NOTIMPL; }
    STDMETHOD(SetCurrentTexturePalette)(UINT PaletteNumber) override { return D3D_OK; }
    STDMETHOD(GetCurrentTexturePalette)(UINT* PaletteNumber) override { return E_NOTIMPL; }
    STDMETHOD(SetScissorRect)(const RECT* pRect) override;
    STDMETHOD(GetScissorRect)(RECT* pRect) override;
    STDMETHOD(SetSoftwareVertexProcessing)(BOOL bSoftware) override { return D3D_OK; }
    STDMETHOD_(BOOL, GetSoftwareVertexProcessing)() override { return FALSE; }
    STDMETHOD(SetNPatchMode)(float nSegments) override { return D3D_OK; }
    STDMETHOD_(float, GetNPatchMode)() override { return 0.0f; }

    // --- Draw Primitives ---
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    STDMETHOD(ProcessVertices)(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { return E_NOTIMPL; }

    // --- Vertex Declarations & FVF ---
    STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* pDecl) override;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** ppDecl) override;
    STDMETHOD(SetFVF)(DWORD FVF) override;
    STDMETHOD(GetFVF)(DWORD* pFVF) override;

    // --- Shaders ---
    STDMETHOD(CreateVertexShader)(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) override;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* pShader) override;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** ppShader) override;
    STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override;
    STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override;
    STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override;
    STDMETHOD(GetVertexShaderConstantI)(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { m_pipelineManager.getVSConstantI(StartRegister, pConstantData, Vector4iCount); return D3D_OK; }
    STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override;
    STDMETHOD(GetVertexShaderConstantB)(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { m_pipelineManager.getVSConstantB(StartRegister, pConstantData, BoolCount); return D3D_OK; }

    // --- Streams & Indices ---
    STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override;
    STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) override;
    STDMETHOD(SetStreamSourceFreq)(UINT StreamNumber, UINT Setting) override { if (Setting != 1) noteUnimplemented("SetStreamSourceFreq(instancing)"); return D3D_OK; }
    STDMETHOD(GetStreamSourceFreq)(UINT StreamNumber, UINT* pSetting) override { return E_NOTIMPL; }
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* pIndexData) override;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** ppIndexData) override;

    // --- Pixel Shaders ---
    STDMETHOD(CreatePixelShader)(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) override;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* pShader) override;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ppShader) override;
    STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override;
    STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override;
    STDMETHOD(SetPixelShaderConstantI)(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override;
    STDMETHOD(GetPixelShaderConstantI)(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { m_pipelineManager.getPSConstantI(StartRegister, pConstantData, Vector4iCount); return D3D_OK; }
    STDMETHOD(SetPixelShaderConstantB)(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override;
    STDMETHOD(GetPixelShaderConstantB)(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { m_pipelineManager.getPSConstantB(StartRegister, pConstantData, BoolCount); return D3D_OK; }

    STDMETHOD(DrawRectPatch)(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) override { return E_NOTIMPL; }
    STDMETHOD(DrawTriPatch)(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) override { return E_NOTIMPL; }
    STDMETHOD(DeletePatch)(UINT Handle) override { return D3D_OK; }
    STDMETHOD(CreateQuery)(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override;

public:
    // ---- device state (D3D9 semantics) ----
    struct StreamBinding {
        IDirect3DVertexBuffer9GLES* buffer = nullptr;
        UINT offset = 0;
        UINT stride = 0;
    };
    static constexpr int kMaxStages = 16;   // sampler stages
    static constexpr int kMaxTSS = 8;       // texture-stage (fixed-function) stages
    static constexpr int kMaxRS = 256;
    static constexpr int kMaxSS = 16;
    static constexpr int kMaxTSSType = 33;

    DWORD rs(D3DRENDERSTATETYPE s) const { return (s < kMaxRS) ? m_rs[s] : 0; }
    DWORD tss(int stage, D3DTEXTURESTAGESTATETYPE t) const { return m_tss[stage][t < kMaxTSSType ? t : 0]; }
    DWORD ss(int stage, D3DSAMPLERSTATETYPE t) const { return m_ss[stage][t < kMaxSS ? t : 0]; }

    // Recorded / captured state block contents (only the 'has' parts are applied).
    struct StateBlockData;

private:
    friend class IDirect3DStateBlock9GLES;

    // instanceLayout: id of the instanced vertex-shader variant to use (0 = the normal one); *instancingUnavailable
    // is set when that variant cannot be made (nothing is drawn then: the caller draws the instances one by one).
    bool prepareDraw(const void* upData, UINT upStride, INT baseVertex, uint32_t instanceLayout = 0, bool* instancingUnavailable = nullptr);
public:
    // One DrawIndexedPrimitive repeated for `instances` objects that differ only in the vertex-shader registers
    // `registers` (count each): data holds instances x count vec4. Leaves the register file as the last instance
    // set it. false = not drawn (no GLES 3, or no instanced variant): draw them one by one.
    bool drawIndexedInstanced(D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT primitives,
                              const uint16_t* registers, uint32_t count, uint32_t instances, const float* data);
    // Several DrawPrimitiveUP calls merged by the render thread (no state change between them): one indexed
    // triangle-list draw of user vertices. Same D3D9 side effects as DrawPrimitiveUP: stream 0 is unset, the
    // index buffer is kept.
    void drawUserTriangles(const void* vertices, UINT vertexCount, const uint16_t* indices, UINT indexCount, UINT stride);
private:
    struct Instancing {
        bool initialized = false, available = false;
        GLuint instanceNumbers = 0;   // vertex buffer: 0, 1, 2, ... kMaxInstancesPerDraw-1 (float)
        void (*drawElementsInstanced)(GLenum, GLsizei, GLenum, const void*, GLsizei) = nullptr;
        void (*vertexAttribDivisor)(GLuint, GLuint) = nullptr;
        uint64_t draws = 0, instances = 0;
    } m_instancing;
    void initInstancing();
    void bindRenderTarget();
    void createBackBufferTarget(UINT width, UINT height);
    void destroyBackBufferTarget();
    void presentBackBuffer();
    void applyViewportAndScissor();
    void applyRenderStates();
    void applyTextures(const struct ShaderVariant* psVariant, bool ffpPS, int ffpStages);
    void applySamplerParams(int stage, IDirect3DBaseTexture9* tex);
    const std::vector<D3DVERTEXELEMENT9>& currentElements();
    void traceDraw(const char* kind, UINT prims);
    void noteUnimplemented(const char* what);
    void resetStateDefaults();
    void captureAll(StateBlockData& d, D3DSTATEBLOCKTYPE type);
    void applyBlock(const StateBlockData& d);
    StateBlockData* recording() { return m_recording; }

    IDirect3D9GLES* m_parentD3D = nullptr;
    SDL_Window* m_sdlWindow = nullptr;
    SDL_GLContext m_glContext = nullptr;
#ifdef __ANDROID__
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLConfig  m_eglConfig = nullptr;
#endif
    D3DPRESENT_PARAMETERS m_presentParams{};

    IDirect3DSurface9GLES* m_defaultBackBuffer = nullptr;
    IDirect3DSurface9GLES* m_defaultDepthBuffer = nullptr;
    IDirect3DSurface9GLES* m_currentRenderTarget = nullptr;
    IDirect3DSurface9GLES* m_currentDepthBuffer = nullptr;
    GLuint m_fbo = 0;
    // Back buffer when the game's render size differs from the screen (another resolution, aspect or a
    // rotated panel): the game draws into this texture and Present scales it onto the window (letterboxed).
    // 0 when the sizes match: the game then draws straight into the window, as before.
    GLuint m_bbFbo = 0, m_bbTex = 0, m_bbDepth = 0;
    int m_screenW = 0, m_screenH = 0;
    bool m_rtFlip = false;      // rendering into a texture: flip Y so rows keep D3D orientation
    UINT m_rtWidth = 640, m_rtHeight = 480;
    bool m_fboDirty = true;

    ProgramPipelineManager m_pipelineManager;
    LinkedProgram* m_currentProgram = nullptr;

    IDirect3DVertexShader9GLES* m_boundVS = nullptr;
    IDirect3DPixelShader9GLES* m_boundPS = nullptr;

    StreamBinding m_streams[16]{};
    IDirect3DIndexBuffer9GLES* m_boundIB = nullptr;
    IDirect3DVertexDeclaration9GLES* m_boundDecl = nullptr;
    DWORD m_boundFVF = 0;
    std::unordered_map<DWORD, std::vector<D3DVERTEXELEMENT9>> m_fvfElements;

    IDirect3DBaseTexture9* m_boundTextures[kMaxStages]{};
    DWORD m_rs[kMaxRS]{};
    DWORD m_ss[kMaxStages][kMaxSS]{};
    DWORD m_tss[kMaxTSS][kMaxTSSType]{};

    D3DVIEWPORT9 m_viewport{};
    RECT m_scissorRect{};

    Matrix4 m_worldMatrix = Matrix4::identity();
    Matrix4 m_viewMatrix = Matrix4::identity();
    Matrix4 m_projMatrix = Matrix4::identity();
    std::unordered_map<DWORD, Matrix4> m_otherTransforms;

    StateBlockData* m_recording = nullptr;

    GLuint m_dummy2D = 0;
    GLuint m_dummyCube = 0;
    bool m_attribEnabled[16]{};
    int m_maxAttribs = 8;
};

// -----------------------------------------------------------------------------
// State blocks
// -----------------------------------------------------------------------------
struct IDirect3DDevice9GLES::StateBlockData {
    std::unordered_map<DWORD, DWORD> rs;
    std::unordered_map<DWORD, DWORD> ss[kMaxStages];
    std::unordered_map<DWORD, DWORD> tss[kMaxTSS];
    std::unordered_map<DWORD, IDirect3DBaseTexture9*> textures;
    bool hasVS = false, hasPS = false;
    IDirect3DVertexShader9GLES* vs = nullptr;
    IDirect3DPixelShader9GLES* ps = nullptr;
    std::unordered_map<UINT, std::array<float, 4>> vsF, psF;
    std::unordered_map<UINT, std::array<int, 4>> vsI, psI;
    std::unordered_map<UINT, BOOL> vsB, psB;
    bool hasDecl = false, hasFVF = false;
    IDirect3DVertexDeclaration9GLES* decl = nullptr;
    DWORD fvf = 0;
    std::unordered_map<UINT, StreamBinding> streams;
    bool hasIB = false;
    IDirect3DIndexBuffer9GLES* ib = nullptr;
    bool hasViewport = false, hasScissor = false;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    std::unordered_map<DWORD, Matrix4> transforms;
};

class IDirect3DStateBlock9GLES : public D3D9ComObject<IDirect3DStateBlock9> {
public:
    IDirect3DStateBlock9GLES(IDirect3DDevice9GLES* device, D3DSTATEBLOCKTYPE type, bool recorded);
    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) override;
    STDMETHOD(Capture)() override;
    STDMETHOD(Apply)() override;
    IDirect3DDevice9GLES::StateBlockData& data() { return m_data; }

private:
    IDirect3DDevice9GLES* m_device = nullptr;
    D3DSTATEBLOCKTYPE m_type = D3DSBT_ALL;
    IDirect3DDevice9GLES::StateBlockData m_data;
};

// -----------------------------------------------------------------------------
// IDirect3D9GLES
// -----------------------------------------------------------------------------
class IDirect3D9GLES : public D3D9ComObject<IDirect3D9> {
public:
    STDMETHOD(RegisterSoftwareDevice)(void* pInitializeFunction) override { return D3D_OK; }
    STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override;
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) override { return 1; }
    STDMETHOD(EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override;
    STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) override;
    STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return D3D_OK; }
    STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        if (CheckFormat >= D3DFMT_DXT1 && CheckFormat <= D3DFMT_DXT5) {
            return D3DERR_NOTAVAILABLE;
        }
        return D3D_OK;
    }
    STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { if (pQualityLevels) *pQualityLevels = 1; return D3D_OK; }
    STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return D3D_OK; }
    STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return D3D_OK; }
    STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override;
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) override { return reinterpret_cast<HMONITOR>(uintptr_t{1}); }
    STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) override;
};

} // namespace d3d9_gles
