#pragma once

#include "d3d9_gles.h"

namespace d3d9_gles {

inline GLenum convertPrimitiveType(D3DPRIMITIVETYPE type) {
    switch (type) {
        case D3DPT_POINTLIST:     return GL_POINTS;
        case D3DPT_LINELIST:      return GL_LINES;
        case D3DPT_LINESTRIP:     return GL_LINE_STRIP;
        case D3DPT_TRIANGLELIST:  return GL_TRIANGLES;
        case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
        case D3DPT_TRIANGLEFAN:   return GL_TRIANGLE_FAN;
        default:                  return GL_TRIANGLES;
    }
}

inline GLsizei primitiveCountToVertexCount(D3DPRIMITIVETYPE type, UINT count) {
    switch (type) {
        case D3DPT_POINTLIST:     return count;
        case D3DPT_LINELIST:      return count * 2;
        case D3DPT_LINESTRIP:     return count + 1;
        case D3DPT_TRIANGLELIST:  return count * 3;
        case D3DPT_TRIANGLESTRIP: return count + 2;
        case D3DPT_TRIANGLEFAN:   return count + 2;
        default:                  return count * 3;
    }
}

inline GLenum convertBlendFactor(D3DBLEND blend) {
    switch (blend) {
        case D3DBLEND_ZERO:            return GL_ZERO;
        case D3DBLEND_ONE:             return GL_ONE;
        case D3DBLEND_SRCCOLOR:        return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR:     return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA:        return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA:     return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA:       return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA:    return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR:       return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR:    return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT:     return GL_SRC_ALPHA_SATURATE;
        default:                       return GL_ONE;
    }
}

inline GLenum convertCullMode(D3DCULL cull) {
    switch (cull) {
        case D3DCULL_CW:  return GL_FRONT; // D3D9 CW is GL front
        case D3DCULL_CCW: return GL_BACK;
        default:          return GL_BACK;
    }
}

inline GLenum convertCompareFunc(D3DCMPFUNC func) {
    switch (func) {
        case D3DCMP_NEVER:        return GL_NEVER;
        case D3DCMP_LESS:         return GL_LESS;
        case D3DCMP_EQUAL:        return GL_EQUAL;
        case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
        case D3DCMP_GREATER:      return GL_GREATER;
        case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
        case D3DCMP_ALWAYS:       return GL_ALWAYS;
        default:                  return GL_LEQUAL;
    }
}

inline GLenum convertTextureWrap(D3DTEXTUREADDRESS addr) {
    switch (addr) {
        case D3DTADDRESS_WRAP:   return GL_REPEAT;
        case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
        case D3DTADDRESS_CLAMP:  return GL_CLAMP_TO_EDGE;
        default:                 return GL_REPEAT;
    }
}

inline GLenum convertTextureFilter(D3DTEXTUREFILTERTYPE minFilter, D3DTEXTUREFILTERTYPE mipFilter) {
    if (mipFilter == D3DTEXF_NONE) {
        return (minFilter == D3DTEXF_POINT) ? GL_NEAREST : GL_LINEAR;
    }
    if (minFilter == D3DTEXF_POINT) {
        return (mipFilter == D3DTEXF_POINT) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
    } else {
        return (mipFilter == D3DTEXF_POINT) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
    }
}

} // namespace d3d9_gles
