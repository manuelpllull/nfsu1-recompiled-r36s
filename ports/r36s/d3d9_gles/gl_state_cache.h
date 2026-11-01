#pragma once
// Redundant-state filter for the GLES2 calls the D3D9 layer makes.
//
// The layer re-applies the whole D3D9 pipeline state on every draw (render states, viewport, vertex
// attributes, texture bindings): ~50-70 GL calls per draw, ~50k per frame in a race. Mobile drivers
// (Mali on the R36S) also treat a state "change" to the same value as dirty and re-validate it at the
// next draw, so on the device most of the D3D9 cost was driver work for state that never changed.
// Every state-setting GL function used by the layer is redirected (macros below) through a shadow of the
// GL state; a call only reaches the driver when the value differs. All layer code (and the virtual
// keyboard overlay) goes through these wrappers, so the shadow always matches the real state; code
// outside the layer that touches GL must call glsc::invalidate() afterwards.
//
// Only one GL context and one thread (the game thread) use these, so the cache is plain globals.
#ifdef __ANDROID__
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <SDL2/SDL_opengles2.h>
#endif
#include <cstdint>
#include <cstring>
#include <ctime>

namespace glsc {

struct AttribPtr { GLuint buffer; GLint size; GLenum type; GLboolean norm; GLsizei stride; const void* ptr; bool valid; };

struct State {
    // caps: index by capIndex(); -1 unknown, 0 off, 1 on
    int8_t caps[8];
    GLenum blendSrcRGB, blendDstRGB, blendSrcA, blendDstA; bool blendFuncValid;
    GLenum blendEqRGB, blendEqA; bool blendEqValid;
    GLfloat blendColor[4]; bool blendColorValid;
    GLenum depthFunc; bool depthFuncValid;
    GLboolean depthMask; bool depthMaskValid;
    GLfloat depthNear, depthFar; bool depthRangeValid;
    GLboolean colorMask[4]; bool colorMaskValid;
    GLenum cullFace; bool cullFaceValid;
    GLenum frontFace; bool frontFaceValid;
    GLfloat polyFactor, polyUnits; bool polyValid;
    GLint vp[4]; bool vpValid;
    GLint sc[4]; bool scValid;
    GLenum stFunc[2]; GLint stRef[2]; GLuint stMask[2]; bool stFuncValid[2];
    GLenum stFail[2], stZFail[2], stZPass[2]; bool stOpValid[2];
    GLuint stWriteMask; bool stWriteMaskValid;
    GLfloat clearColor[4]; bool clearColorValid;
    GLfloat clearDepth; bool clearDepthValid;
    GLint clearStencil; bool clearStencilValid;
    GLenum activeUnit; bool activeValid;
    GLuint tex2D[16]; GLuint texCube[16]; bool texValid2D[16]; bool texValidCube[16];
    GLuint arrayBuffer, elementBuffer; bool arrayValid, elementValid;
    GLuint framebuffer; bool fbValid;
    GLuint renderbuffer; bool rbValid;
    GLuint program; bool programValid;
    int8_t attribEnabled[16];
    AttribPtr attrib[16];
    GLfloat attribConst[16][4]; bool attribConstValid[16];
    GLint unpackAlign, packAlign;
};

inline State& st() { static State s; return s; }
// GL calls the layer issued (before filtering) and calls that reached the driver.
inline uint64_t& requested() { static uint64_t n = 0; return n; }
inline uint64_t& issued() { static uint64_t n = 0; return n; }

inline void invalidate() {
    State& s = st();
    std::memset(&s, 0, sizeof(s));
    for (auto& c : s.caps) c = -1;
    for (auto& e : s.attribEnabled) e = -1;
    s.unpackAlign = s.packAlign = -1;
}
inline bool& initialized() { static bool b = false; return b; }
inline State& S() { if (!initialized()) { initialized() = true; invalidate(); } return st(); }

inline int capIndex(GLenum cap) {
    switch (cap) {
        case GL_BLEND: return 0; case GL_CULL_FACE: return 1; case GL_DEPTH_TEST: return 2;
        case GL_POLYGON_OFFSET_FILL: return 3; case GL_SCISSOR_TEST: return 4; case GL_STENCIL_TEST: return 5;
        case GL_DITHER: return 6; case GL_SAMPLE_ALPHA_TO_COVERAGE: return 7; default: return -1;
    }
}
#define GLSC_REQ() (++::glsc::requested())
#define GLSC_ISSUE() (++::glsc::issued())

inline void Enable(GLenum cap) { GLSC_REQ(); int i = capIndex(cap); State& s = S(); if (i >= 0 && s.caps[i] == 1) return; if (i >= 0) s.caps[i] = 1; GLSC_ISSUE(); (glEnable)(cap); }
inline void Disable(GLenum cap) { GLSC_REQ(); int i = capIndex(cap); State& s = S(); if (i >= 0 && s.caps[i] == 0) return; if (i >= 0) s.caps[i] = 0; GLSC_ISSUE(); (glDisable)(cap); }

inline void BlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) {
    GLSC_REQ(); State& s = S();
    if (s.blendFuncValid && s.blendSrcRGB == a && s.blendDstRGB == b && s.blendSrcA == c && s.blendDstA == d) return;
    s.blendSrcRGB = a; s.blendDstRGB = b; s.blendSrcA = c; s.blendDstA = d; s.blendFuncValid = true;
    GLSC_ISSUE(); (glBlendFuncSeparate)(a, b, c, d);
}
inline void BlendFunc(GLenum a, GLenum b) { BlendFuncSeparate(a, b, a, b); }
inline void BlendEquationSeparate(GLenum a, GLenum b) {
    GLSC_REQ(); State& s = S();
    if (s.blendEqValid && s.blendEqRGB == a && s.blendEqA == b) return;
    s.blendEqRGB = a; s.blendEqA = b; s.blendEqValid = true; GLSC_ISSUE(); (glBlendEquationSeparate)(a, b);
}
inline void BlendEquation(GLenum a) { BlendEquationSeparate(a, a); }
inline void BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    GLSC_REQ(); State& s = S(); const GLfloat c[4] = {r, g, b, a};
    if (s.blendColorValid && std::memcmp(c, s.blendColor, sizeof(c)) == 0) return;
    std::memcpy(s.blendColor, c, sizeof(c)); s.blendColorValid = true; GLSC_ISSUE(); (glBlendColor)(r, g, b, a);
}
inline void DepthFunc(GLenum f) { GLSC_REQ(); State& s = S(); if (s.depthFuncValid && s.depthFunc == f) return; s.depthFunc = f; s.depthFuncValid = true; GLSC_ISSUE(); (glDepthFunc)(f); }
inline void DepthMask(GLboolean m) { GLSC_REQ(); State& s = S(); if (s.depthMaskValid && s.depthMask == m) return; s.depthMask = m; s.depthMaskValid = true; GLSC_ISSUE(); (glDepthMask)(m); }
inline void DepthRangef(GLfloat n, GLfloat f) { GLSC_REQ(); State& s = S(); if (s.depthRangeValid && s.depthNear == n && s.depthFar == f) return; s.depthNear = n; s.depthFar = f; s.depthRangeValid = true; GLSC_ISSUE(); (glDepthRangef)(n, f); }
inline void ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    GLSC_REQ(); State& s = S();
    if (s.colorMaskValid && s.colorMask[0] == r && s.colorMask[1] == g && s.colorMask[2] == b && s.colorMask[3] == a) return;
    s.colorMask[0] = r; s.colorMask[1] = g; s.colorMask[2] = b; s.colorMask[3] = a; s.colorMaskValid = true;
    GLSC_ISSUE(); (glColorMask)(r, g, b, a);
}
inline void CullFace(GLenum m) { GLSC_REQ(); State& s = S(); if (s.cullFaceValid && s.cullFace == m) return; s.cullFace = m; s.cullFaceValid = true; GLSC_ISSUE(); (glCullFace)(m); }
inline void FrontFace(GLenum m) { GLSC_REQ(); State& s = S(); if (s.frontFaceValid && s.frontFace == m) return; s.frontFace = m; s.frontFaceValid = true; GLSC_ISSUE(); (glFrontFace)(m); }
inline void PolygonOffset(GLfloat f, GLfloat u) { GLSC_REQ(); State& s = S(); if (s.polyValid && s.polyFactor == f && s.polyUnits == u) return; s.polyFactor = f; s.polyUnits = u; s.polyValid = true; GLSC_ISSUE(); (glPolygonOffset)(f, u); }
inline void Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    GLSC_REQ(); State& s = S();
    if (s.vpValid && s.vp[0] == x && s.vp[1] == y && s.vp[2] == w && s.vp[3] == h) return;
    s.vp[0] = x; s.vp[1] = y; s.vp[2] = w; s.vp[3] = h; s.vpValid = true; GLSC_ISSUE(); (glViewport)(x, y, w, h);
}
inline void Scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    GLSC_REQ(); State& s = S();
    if (s.scValid && s.sc[0] == x && s.sc[1] == y && s.sc[2] == w && s.sc[3] == h) return;
    s.sc[0] = x; s.sc[1] = y; s.sc[2] = w; s.sc[3] = h; s.scValid = true; GLSC_ISSUE(); (glScissor)(x, y, w, h);
}
inline void StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    GLSC_REQ(); State& s = S();
    const bool f = face == GL_FRONT || face == GL_FRONT_AND_BACK, b = face == GL_BACK || face == GL_FRONT_AND_BACK;
    const bool same = (!f || (s.stFuncValid[0] && s.stFunc[0] == func && s.stRef[0] == ref && s.stMask[0] == mask)) &&
                      (!b || (s.stFuncValid[1] && s.stFunc[1] == func && s.stRef[1] == ref && s.stMask[1] == mask));
    if (same) return;
    if (f) { s.stFunc[0] = func; s.stRef[0] = ref; s.stMask[0] = mask; s.stFuncValid[0] = true; }
    if (b) { s.stFunc[1] = func; s.stRef[1] = ref; s.stMask[1] = mask; s.stFuncValid[1] = true; }
    GLSC_ISSUE(); (glStencilFuncSeparate)(face, func, ref, mask);
}
inline void StencilOpSeparate(GLenum face, GLenum a, GLenum b2, GLenum c) {
    GLSC_REQ(); State& s = S();
    const bool f = face == GL_FRONT || face == GL_FRONT_AND_BACK, b = face == GL_BACK || face == GL_FRONT_AND_BACK;
    const bool same = (!f || (s.stOpValid[0] && s.stFail[0] == a && s.stZFail[0] == b2 && s.stZPass[0] == c)) &&
                      (!b || (s.stOpValid[1] && s.stFail[1] == a && s.stZFail[1] == b2 && s.stZPass[1] == c));
    if (same) return;
    if (f) { s.stFail[0] = a; s.stZFail[0] = b2; s.stZPass[0] = c; s.stOpValid[0] = true; }
    if (b) { s.stFail[1] = a; s.stZFail[1] = b2; s.stZPass[1] = c; s.stOpValid[1] = true; }
    GLSC_ISSUE(); (glStencilOpSeparate)(face, a, b2, c);
}
inline void StencilMask(GLuint m) { GLSC_REQ(); State& s = S(); if (s.stWriteMaskValid && s.stWriteMask == m) return; s.stWriteMask = m; s.stWriteMaskValid = true; GLSC_ISSUE(); (glStencilMask)(m); }
inline void ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    GLSC_REQ(); State& s = S(); const GLfloat c[4] = {r, g, b, a};
    if (s.clearColorValid && std::memcmp(c, s.clearColor, sizeof(c)) == 0) return;
    std::memcpy(s.clearColor, c, sizeof(c)); s.clearColorValid = true; GLSC_ISSUE(); (glClearColor)(r, g, b, a);
}
inline void ClearDepthf(GLfloat d) { GLSC_REQ(); State& s = S(); if (s.clearDepthValid && s.clearDepth == d) return; s.clearDepth = d; s.clearDepthValid = true; GLSC_ISSUE(); (glClearDepthf)(d); }
inline void ClearStencil(GLint v) { GLSC_REQ(); State& s = S(); if (s.clearStencilValid && s.clearStencil == v) return; s.clearStencil = v; s.clearStencilValid = true; GLSC_ISSUE(); (glClearStencil)(v); }

inline void ActiveTexture(GLenum unit) { GLSC_REQ(); State& s = S(); if (s.activeValid && s.activeUnit == unit) return; s.activeUnit = unit; s.activeValid = true; GLSC_ISSUE(); (glActiveTexture)(unit); }
inline int currentUnit() { State& s = S(); if (!s.activeValid) { s.activeValid = true; s.activeUnit = GL_TEXTURE0; GLSC_ISSUE(); (glActiveTexture)(GL_TEXTURE0); } return static_cast<int>(s.activeUnit - GL_TEXTURE0) & 15; }
inline void BindTexture(GLenum target, GLuint tex) {
    GLSC_REQ(); State& s = S(); const int u = currentUnit();
    if (target == GL_TEXTURE_2D) { if (s.texValid2D[u] && s.tex2D[u] == tex) return; s.tex2D[u] = tex; s.texValid2D[u] = true; }
    else if (target == GL_TEXTURE_CUBE_MAP) { if (s.texValidCube[u] && s.texCube[u] == tex) return; s.texCube[u] = tex; s.texValidCube[u] = true; }
    GLSC_ISSUE(); (glBindTexture)(target, tex);
}
inline void DeleteTextures(GLsizei n, const GLuint* t) {
    State& s = S();
    for (GLsizei i = 0; i < n; ++i) for (int u = 0; u < 16; ++u) {
        if (s.tex2D[u] == t[i]) s.texValid2D[u] = false;
        if (s.texCube[u] == t[i]) s.texValidCube[u] = false;
    }
    (glDeleteTextures)(n, t);
}
inline void BindBuffer(GLenum target, GLuint b) {
    GLSC_REQ(); State& s = S();
    if (target == GL_ARRAY_BUFFER) { if (s.arrayValid && s.arrayBuffer == b) return; s.arrayBuffer = b; s.arrayValid = true; }
    else if (target == GL_ELEMENT_ARRAY_BUFFER) { if (s.elementValid && s.elementBuffer == b) return; s.elementBuffer = b; s.elementValid = true; }
    GLSC_ISSUE(); (glBindBuffer)(target, b);
}
inline void DeleteBuffers(GLsizei n, const GLuint* b) {
    State& s = S();
    for (GLsizei i = 0; i < n; ++i) {
        if (s.arrayBuffer == b[i]) s.arrayValid = false;
        if (s.elementBuffer == b[i]) s.elementValid = false;
        for (auto& a : s.attrib) if (a.buffer == b[i]) a.valid = false;
    }
    (glDeleteBuffers)(n, b);
}
inline void BindFramebuffer(GLenum target, GLuint fb) { GLSC_REQ(); State& s = S(); if (s.fbValid && s.framebuffer == fb) return; s.framebuffer = fb; s.fbValid = true; GLSC_ISSUE(); (glBindFramebuffer)(target, fb); }
inline void DeleteFramebuffers(GLsizei n, const GLuint* f) { State& s = S(); for (GLsizei i = 0; i < n; ++i) if (s.framebuffer == f[i]) s.fbValid = false; (glDeleteFramebuffers)(n, f); }
inline void BindRenderbuffer(GLenum target, GLuint rb) { GLSC_REQ(); State& s = S(); if (s.rbValid && s.renderbuffer == rb) return; s.renderbuffer = rb; s.rbValid = true; GLSC_ISSUE(); (glBindRenderbuffer)(target, rb); }
inline void DeleteRenderbuffers(GLsizei n, const GLuint* r) { State& s = S(); for (GLsizei i = 0; i < n; ++i) if (s.renderbuffer == r[i]) s.rbValid = false; (glDeleteRenderbuffers)(n, r); }
inline void UseProgram(GLuint p) { GLSC_REQ(); State& s = S(); if (s.programValid && s.program == p) return; s.program = p; s.programValid = true; GLSC_ISSUE(); (glUseProgram)(p); }
inline void DeleteProgram(GLuint p) { State& s = S(); if (s.program == p) s.programValid = false; (glDeleteProgram)(p); }
inline void EnableVertexAttribArray(GLuint i) { GLSC_REQ(); State& s = S(); if (i < 16 && s.attribEnabled[i] == 1) return; if (i < 16) s.attribEnabled[i] = 1; GLSC_ISSUE(); (glEnableVertexAttribArray)(i); }
inline void DisableVertexAttribArray(GLuint i) { GLSC_REQ(); State& s = S(); if (i < 16 && s.attribEnabled[i] == 0) return; if (i < 16) s.attribEnabled[i] = 0; GLSC_ISSUE(); (glDisableVertexAttribArray)(i); }
inline void VertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr) {
    GLSC_REQ(); State& s = S();
    if (i < 16) {
        // The pointer binds to the ARRAY_BUFFER bound now.
        const GLuint buf = s.arrayValid ? s.arrayBuffer : 0xFFFFFFFFu;
        AttribPtr& a = s.attrib[i];
        if (a.valid && s.arrayValid && a.buffer == buf && a.size == size && a.type == type && a.norm == norm && a.stride == stride && a.ptr == ptr) return;
        a = AttribPtr{buf, size, type, norm, stride, ptr, s.arrayValid};
    }
    GLSC_ISSUE(); (glVertexAttribPointer)(i, size, type, norm, stride, ptr);
}
inline void VertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    GLSC_REQ(); State& s = S();
    if (i < 16) {
        const GLfloat v[4] = {x, y, z, w};
        if (s.attribConstValid[i] && std::memcmp(v, s.attribConst[i], sizeof(v)) == 0) return;
        std::memcpy(s.attribConst[i], v, sizeof(v)); s.attribConstValid[i] = true;
    }
    GLSC_ISSUE(); (glVertexAttrib4f)(i, x, y, z, w);
}
inline void PixelStorei(GLenum p, GLint v) {
    GLSC_REQ(); State& s = S();
    GLint* slot = p == GL_UNPACK_ALIGNMENT ? &s.unpackAlign : p == GL_PACK_ALIGNMENT ? &s.packAlign : nullptr;
    if (slot && *slot == v) return;
    if (slot) *slot = v;
    GLSC_ISSUE(); (glPixelStorei)(p, v);
}
// CPU time the game thread spends inside the GL driver (draws sampled 1 in 8, swap, clears, texture
// uploads, mipmap generation). It is the part of a frame whose cost is the driver's, not ours: in the
// VM it is llvmpipe/GLX work, on the R36S Mali driver work. Used by the [R36S model].
inline double& driverNs() { static double n = 0; return n; }
struct DriverCpuTimer {
    timespec t0{};
    double scale;
    explicit DriverCpuTimer(double s = 1.0) : scale(s) { if (scale > 0) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0); }
    ~DriverCpuTimer() {
        if (scale <= 0) return;
        timespec t1{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
        driverNs() += scale * ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec));
    }
};

}  // namespace glsc

#define glEnable(c) ::glsc::Enable(c)
#define glDisable(c) ::glsc::Disable(c)
#define glBlendFunc(a, b) ::glsc::BlendFunc(a, b)
#define glBlendFuncSeparate(a, b, c, d) ::glsc::BlendFuncSeparate(a, b, c, d)
#define glBlendEquation(a) ::glsc::BlendEquation(a)
#define glBlendEquationSeparate(a, b) ::glsc::BlendEquationSeparate(a, b)
#define glBlendColor(r, g, b, a) ::glsc::BlendColor(r, g, b, a)
#define glDepthFunc(f) ::glsc::DepthFunc(f)
#define glDepthMask(m) ::glsc::DepthMask(m)
#define glDepthRangef(n, f) ::glsc::DepthRangef(n, f)
#define glColorMask(r, g, b, a) ::glsc::ColorMask(r, g, b, a)
#define glCullFace(m) ::glsc::CullFace(m)
#define glFrontFace(m) ::glsc::FrontFace(m)
#define glPolygonOffset(f, u) ::glsc::PolygonOffset(f, u)
#define glViewport(x, y, w, h) ::glsc::Viewport(x, y, w, h)
#define glScissor(x, y, w, h) ::glsc::Scissor(x, y, w, h)
#define glStencilFuncSeparate(f, a, b, c) ::glsc::StencilFuncSeparate(f, a, b, c)
#define glStencilOpSeparate(f, a, b, c) ::glsc::StencilOpSeparate(f, a, b, c)
#define glStencilMask(m) ::glsc::StencilMask(m)
#define glClearColor(r, g, b, a) ::glsc::ClearColor(r, g, b, a)
#define glClearDepthf(d) ::glsc::ClearDepthf(d)
#define glClearStencil(v) ::glsc::ClearStencil(v)
#define glActiveTexture(u) ::glsc::ActiveTexture(u)
#define glBindTexture(t, x) ::glsc::BindTexture(t, x)
#define glDeleteTextures(n, t) ::glsc::DeleteTextures(n, t)
#define glBindBuffer(t, b) ::glsc::BindBuffer(t, b)
#define glDeleteBuffers(n, b) ::glsc::DeleteBuffers(n, b)
#define glBindFramebuffer(t, f) ::glsc::BindFramebuffer(t, f)
#define glDeleteFramebuffers(n, f) ::glsc::DeleteFramebuffers(n, f)
#define glBindRenderbuffer(t, r) ::glsc::BindRenderbuffer(t, r)
#define glDeleteRenderbuffers(n, r) ::glsc::DeleteRenderbuffers(n, r)
#define glUseProgram(p) ::glsc::UseProgram(p)
#define glDeleteProgram(p) ::glsc::DeleteProgram(p)
#define glEnableVertexAttribArray(i) ::glsc::EnableVertexAttribArray(i)
#define glDisableVertexAttribArray(i) ::glsc::DisableVertexAttribArray(i)
#define glVertexAttribPointer(i, s, t, n, st, p) ::glsc::VertexAttribPointer(i, s, t, n, st, p)
#define glVertexAttrib4f(i, x, y, z, w) ::glsc::VertexAttrib4f(i, x, y, z, w)
#define glPixelStorei(p, v) ::glsc::PixelStorei(p, v)
