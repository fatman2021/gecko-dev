/* -*- Mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 40; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SHARED_SURFACE_D3D11_INTEROP_H_
#define SHARED_SURFACE_D3D11_INTEROP_H_

#include <windows.h>
#include "SharedSurface.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace mozilla {
namespace gl {

class GLContext;
class WGLLibrary;

class SharedSurface_D3D11Interop
    : public SharedSurface
{
public:
    static UniquePtr<SharedSurface_D3D11Interop> Create(GLContext* gl,
                                                        WGLLibrary* wgl,
                                                        HANDLE mWGLD3DDevice,
                                                        ID3D11Device* d3d,
                                                        const gfx::IntSize& size,
                                                        bool hasAlpha);

    static SharedSurface_D3D11Interop* Cast(SharedSurface* surf) {
        MOZ_ASSERT(surf->mType == SharedSurfaceType::DXGLInterop2);

        return (SharedSurface_D3D11Interop*)surf;
    }

protected:
    GLuint mProdTex;
    WGLLibrary* const mWGL;
    HANDLE mWGLD3DDevice;
    HANDLE mTextureWGL;
    RefPtr<ID3D11Texture2D> mConsumerTexture;

    SharedSurface_D3D11Interop(GLContext* gl,
                               GLuint textureGL,
                               WGLLibrary* wgl,
                               HANDLE wglD3DDevice,
                               HANDLE textureWGL,
                               ID3D11Texture2D* textureD3D,
                               const gfx::IntSize& size,
                               bool hasAlpha);

public:
    virtual ~SharedSurface_D3D11Interop();

    virtual void LockProdImpl() MOZ_OVERRIDE;
    virtual void UnlockProdImpl() MOZ_OVERRIDE;

    virtual void Fence() MOZ_OVERRIDE;
    virtual bool WaitSync() MOZ_OVERRIDE;
    virtual bool PollSync() MOZ_OVERRIDE;

    virtual GLuint ProdTexture() MOZ_OVERRIDE {
        return mProdTex;
    }

    virtual GLuint ProdRenderbuffer() MOZ_OVERRIDE {
        return mProdTex;
    }

    // Implementation-specific functions below:
    const RefPtr<ID3D11Texture2D>& GetConsumerTexture() const {
        return mConsumerTexture;
    }
};


class SurfaceFactory_D3D11Interop
    : public SurfaceFactory
{
protected:
    WGLLibrary* const mWGL;
    const HANDLE mWGLD3DDevice;
    nsRefPtr<ID3D11Device> mD3D;

public:
    static UniquePtr<SurfaceFactory_D3D11Interop> Create(GLContext* gl,
                                                         ID3D11Device* d3d,
                                                         const SurfaceCaps& caps);

protected:
    SurfaceFactory_D3D11Interop(GLContext* gl,
                                WGLLibrary* wgl,
                                HANDLE wglD3DDevice,
                                ID3D11Device* d3d,
                                const SurfaceCaps& caps);

    virtual UniquePtr<SharedSurface> CreateShared(const gfx::IntSize& size) MOZ_OVERRIDE {
        bool hasAlpha = mReadCaps.alpha;
        return SharedSurface_D3D11Interop::Create(mGL, mWGL, mWGLD3DDevice, mD3D, size,
                                                  hasAlpha);
    }

public:
    virtual ~SurfaceFactory_D3D11Interop();
};

} /* namespace gl */
} /* namespace mozilla */

#endif /* SHARED_SURFACE_D3D11_INTEROP_H_ */
