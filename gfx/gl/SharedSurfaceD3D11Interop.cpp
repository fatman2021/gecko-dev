/* -*- Mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 40; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SharedSurfaceD3D11Interop.h"

#include <d3d11.h>
#include "GLContext.h"
#include "WGLLibrary.h"

using namespace mozilla::gfx;
using namespace mozilla::gl;

/*
Sample Code for WGL_NV_DX_interop2:
Example: Render to Direct3D 11 backbuffer with openGL:

// create D3D11 device, context and swap chain.
ID3D11Device *device;
ID3D11DeviceContext *devCtx;
IDXGISwapChain *swapChain;

DXGI_SWAP_CHAIN_DESC scd;

<set appropriate swap chain parameters in scd>

hr = D3D11CreateDeviceAndSwapChain(NULL,                        // pAdapter
                                   D3D_DRIVER_TYPE_HARDWARE,    // DriverType
                                   NULL,                        // Software
                                   0,                           // Flags (Do not set D3D11_CREATE_DEVICE_SINGLETHREADED)
                                   NULL,                        // pFeatureLevels
                                   0,                           // FeatureLevels
                                   D3D11_SDK_VERSION,           // SDKVersion
                                   &scd,                        // pSwapChainDesc
                                   &swapChain,                  // ppSwapChain
                                   &device,                     // ppDevice
                                   NULL,                        // pFeatureLevel
                                   &devCtx);                    // ppImmediateContext

// Fetch the swapchain backbuffer
ID3D11Texture2D *dxColorbuffer;
swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID *)&dxColorbuffer);

// Create depth stencil texture
ID3D11Texture2D *dxDepthBuffer;
D3D11_TEXTURE2D_DESC depthDesc;
depthDesc.Usage = D3D11_USAGE_DEFAULT;
<set other depthDesc parameters appropriately>

// Create Views
ID3D11RenderTargetView *colorBufferView;
D3D11_RENDER_TARGET_VIEW_DESC rtd;
<set rtd parameters appropriately>
device->CreateRenderTargetView(dxColorbuffer, &rtd, &colorBufferView);

ID3D11DepthStencilView *depthBufferView;
D3D11_DEPTH_STENCIL_VIEW_DESC dsd;
<set dsd parameters appropriately>
device->CreateDepthStencilView(dxDepthBuffer, &dsd, &depthBufferView);

// Attach back buffer and depth texture to redertarget for the device.
devCtx->OMSetRenderTargets(1, &colorBufferView, depthBufferView);

// Register D3D11 device with GL
HANDLE gl_handleD3D;
gl_handleD3D = wglDXOpenDeviceNV(device);

// register the Direct3D color and depth/stencil buffers as
// renderbuffers in opengl
GLuint gl_names[2];
HANDLE gl_handles[2];

glGenRenderbuffers(2, gl_names);

gl_handles[0] = wglDXRegisterObjectNV(gl_handleD3D, dxColorBuffer,
                                      gl_names[0],
                                      GL_RENDERBUFFER,
                                      WGL_ACCESS_READ_WRITE_NV);

gl_handles[1] = wglDXRegisterObjectNV(gl_handleD3D, dxDepthBuffer,
                                      gl_names[1],
                                      GL_RENDERBUFFER,
                                      WGL_ACCESS_READ_WRITE_NV);

// attach the Direct3D buffers to an FBO
glBindFramebuffer(GL_FRAMEBUFFER, fbo);
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_RENDERBUFFER, gl_names[0]);
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                          GL_RENDERBUFFER, gl_names[1]);
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                          GL_RENDERBUFFER, gl_names[1]);

while (!done) {
      <direct3d renders to the render targets>

      // lock the render targets for GL access
      wglDXLockObjectsNVX(gl_handleD3D, 2, gl_handles);

      <opengl renders to the render targets>

      // unlock the render targets
      wglDXUnlockObjectsNVX(gl_handleD3D, 2, gl_handles);

      <direct3d renders to the render targets and presents
       the results on the screen>
}
*/

////////////////////////////////////////////////////////////////////////////////
// Shared Surface

/*static*/ UniquePtr<SharedSurface_D3D11Interop>
SharedSurface_D3D11Interop::Create(GLContext* gl,
                                   WGLLibrary* wgl,
                                   HANDLE mWGLD3DDevice,
                                   ID3D11Device* d3d,
                                   const gfx::IntSize& size,
                                   bool hasAlpha)
{
    // Create a texture in case we need to readback.
    DXGI_FORMAT format = hasAlpha ? DXGI_FORMAT_B8G8R8A8_UNORM
                                  : DXGI_FORMAT_B8G8R8X8_UNORM;
    CD3D11_TEXTURE2D_DESC desc(format, size.width, size.height, 1, 1);
    nsRefPtr<ID3D11Texture2D> textureD3D;
    HRESULT hr = d3d->CreateTexture2D(&desc, nullptr, getter_AddRefs(textureD3D));
    if (FAILED(hr)) {
        NS_WARNING("Failed to create texture for CanvasLayer!");
        return nullptr;
    }

    GLuint textureGL = 0;
    gl->MakeCurrent();
    gl->fGenTextures(1, &textureGL);
    HANDLE textureWGL = wgl->fDXRegisterObject(mWGLD3DDevice,
                                               textureD3D,
                                               textureGL,
                                               LOCAL_GL_TEXTURE_2D,
                                               // LOCAL_WGL_ACCESS_READ_WRITE);
                                               LOCAL_WGL_ACCESS_WRITE_DISCARD_NV);
    //gl->fBindTexture(LOCAL_GL_TEXTURE_2D, textureGL);
    //gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_BASE_LEVEL, 0);
    //gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAX_LEVEL, 0);
    //gl->fBindTexture(LOCAL_GL_TEXTURE_2D, 0);

    if (!textureWGL) {
        NS_WARNING("Failed to register D3D object with WGL.");
        return nullptr;
    }

    typedef SharedSurface_D3D11Interop ptrT;
    UniquePtr<ptrT> ret ( new ptrT(gl, textureGL, wgl, mWGLD3DDevice, textureWGL,
                                   textureD3D, size, hasAlpha) );

    return Move(ret);
}

SharedSurface_D3D11Interop::SharedSurface_D3D11Interop(GLContext* gl,
                                                       GLuint textureGL,
                                                       WGLLibrary* wgl,
                                                       HANDLE wglD3DDevice,
                                                       HANDLE textureWGL,
                                                       ID3D11Texture2D* textureD3D,
                                                       const gfx::IntSize& size,
                                                       bool hasAlpha)
    : SharedSurface(SharedSurfaceType::DXGLInterop2,
                    AttachmentType::GLTexture,
                    gl,
                    size,
                    hasAlpha)
    , mProdTex(textureGL)
    , mWGL(wgl)
    , mWGLD3DDevice(wglD3DDevice)
    , mTextureWGL(textureWGL)
    , mConsumerTexture(textureD3D)
{}

SharedSurface_D3D11Interop::~SharedSurface_D3D11Interop()
{
    mWGL->fDXUnlockObjects(mWGLD3DDevice, 1, &mTextureWGL);
    mWGL->fDXUnregisterObject(mWGLD3DDevice, mTextureWGL);
}

void
SharedSurface_D3D11Interop::LockProdImpl()
{
    // printf_stderr("0x%08x<D3D11Interop>::LockProd\n", this);
    MOZ_ALWAYS_TRUE( mWGL->fDXLockObjects(mWGLD3DDevice, 1, &mTextureWGL) );
}

void
SharedSurface_D3D11Interop::UnlockProdImpl()
{
    // printf_stderr("0x%08x<D3D11Interop>::UnlockProd\n", this);
    MOZ_ALWAYS_TRUE( mWGL->fDXUnlockObjects(mWGLD3DDevice, 1, &mTextureWGL) );
}

void
SharedSurface_D3D11Interop::Fence()
{
    // mGL->fFinish();
}

bool
SharedSurface_D3D11Interop::WaitSync()
{
    // Since we glFinish in Fence(), we're always going to be resolved here.
    return true;
}

bool
SharedSurface_D3D11Interop::PollSync()
{
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Factory

/*static*/ UniquePtr<SurfaceFactory_D3D11Interop>
SurfaceFactory_D3D11Interop::Create(GLContext* gl,
                                    ID3D11Device* d3d,
                                    const SurfaceCaps& caps)
{
    WGLLibrary* wgl = &sWGLLib;
    if (!wgl || !wgl->HasDXInterop2())
        return nullptr;

    HANDLE wglD3DDevice = wgl->fDXOpenDevice(d3d);
    if (!wglD3DDevice) {
        NS_WARNING("Failed to open D3D device for use by WGL.");
        return nullptr;
    }

    typedef SurfaceFactory_D3D11Interop ptrT;
    UniquePtr<ptrT> ret( new ptrT(gl, wgl, wglD3DDevice, d3d, caps) );

    return Move(ret);
}

SurfaceFactory_D3D11Interop::SurfaceFactory_D3D11Interop(GLContext* gl,
                                                         WGLLibrary* wgl,
                                                         HANDLE wglD3DDevice,
                                                         ID3D11Device* d3d,
                                                         const SurfaceCaps& caps)
    : SurfaceFactory(gl, SharedSurfaceType::DXGLInterop2, caps)
    , mWGL(wgl)
    , mWGLD3DDevice(wglD3DDevice)
    , mD3D(d3d)
{}

SurfaceFactory_D3D11Interop::~SurfaceFactory_D3D11Interop()
{
    mWGL->fDXCloseDevice(mWGLD3DDevice);
}
