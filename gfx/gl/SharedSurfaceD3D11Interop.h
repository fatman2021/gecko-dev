/* -*- Mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 40; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SHARED_SURFACE_D3D11_INTEROP_H_
#define SHARED_SURFACE_D3D11_INTEROP_H_

#include <windows.h>
#include "SharedSurface.h"
#include "mozilla/Atomics.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace mozilla {
namespace gl {

class DXGLDevice;
class GLContext;
class WGLLibrary;

class SharedSurface_D3D11Interop
    : public SharedSurface
{
    const GLuint mProdRB;
    const RefPtr<DXGLDevice> mDXGL;
    const HANDLE mObjectWGL;
    const HANDLE mSharedHandle;
    const RefPtr<ID3D11Texture2D> mTextureD3D;
    RefPtr<IDXGIKeyedMutex> mKeyedMutex;
    RefPtr<IDXGIKeyedMutex> mConsumerKeyedMutex;
    RefPtr<ID3D11Texture2D> mConsumerTexture;

    Atomic<uint32_t> mAcquireKey;
    Atomic<uint32_t> mReleaseKey;

protected:
    bool mLockedForGL;

public:
    static UniquePtr<SharedSurface_D3D11Interop> Create(const RefPtr<DXGLDevice>& dxgl,
                                                        GLContext* gl,
                                                        const gfx::IntSize& size,
                                                        bool hasAlpha);

protected:
    SharedSurface_D3D11Interop(GLContext* gl,
                               const gfx::IntSize& size,
                               bool hasAlpha,
                               GLuint renderbufferGL,
                               const RefPtr<DXGLDevice>& dxgl,
                               HANDLE objectWGL,
                               const RefPtr<ID3D11Texture2D>& textureD3D,
                               HANDLE sharedHandle,
                               const RefPtr<IDXGIKeyedMutex>& keyedMutex);

    virtual void LockProdImpl() MOZ_OVERRIDE { }
    virtual void UnlockProdImpl() MOZ_OVERRIDE { }

public:
    virtual ~SharedSurface_D3D11Interop();

    static SharedSurface_D3D11Interop* Cast(SharedSurface* surf) {
        MOZ_ASSERT(surf->mType == SharedSurfaceType::DXGLInterop2);

        return (SharedSurface_D3D11Interop*)surf;
    }

    virtual void ConsumerAcquireImpl() MOZ_OVERRIDE;
    virtual void ConsumerReleaseImpl() MOZ_OVERRIDE;

    virtual void ProducerAcquireImpl() MOZ_OVERRIDE;
    virtual void ProducerReleaseImpl() MOZ_OVERRIDE;

    virtual void Fence() MOZ_OVERRIDE { }
    virtual bool WaitSync() MOZ_OVERRIDE { return true; }
    virtual bool PollSync() MOZ_OVERRIDE { return true; }

    virtual GLuint ProdRenderbuffer() MOZ_OVERRIDE {
        return mProdRB;
    }

    // Implementation-specific functions below:
    HANDLE GetSharedHandle() const {
        return mSharedHandle;
    }

    const RefPtr<ID3D11Texture2D>& GetConsumerTexture() const {
        return mConsumerTexture;
    }
};


class SurfaceFactory_D3D11Interop
    : public SurfaceFactory
{
public:
    const RefPtr<DXGLDevice> mDXGL;

    static UniquePtr<SurfaceFactory_D3D11Interop> Create(GLContext* gl,
                                                         const SurfaceCaps& caps);

protected:
    SurfaceFactory_D3D11Interop(GLContext* gl, const SurfaceCaps& caps,
                                const RefPtr<DXGLDevice>& dxgl);

public:
    virtual ~SurfaceFactory_D3D11Interop();

protected:
    virtual UniquePtr<SharedSurface> CreateShared(const gfx::IntSize& size) MOZ_OVERRIDE {
        bool hasAlpha = mReadCaps.alpha;
        return SharedSurface_D3D11Interop::Create(mDXGL, mGL, size, hasAlpha);
    }
};

} /* namespace gl */
} /* namespace mozilla */

#endif /* SHARED_SURFACE_D3D11_INTEROP_H_ */
