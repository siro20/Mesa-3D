/*
 * Copyright 2016 Patrick Rudolph <siro@das-labor.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#ifndef _NINE_CSMT_H_
#define _NINE_CSMT_H_

#include "d3d9.h"
#include "d3dadapter/d3dadapter9.h"

struct NineDevice9;
struct csmt_context;

struct csmt_context *nine_csmt_create( struct NineDevice9 *This );
void nine_csmt_destroy( struct NineDevice9 *This, struct csmt_context *ctx );


extern IDirect3DDevice9Vtbl PureDevice9_vtable;
extern IDirect3DDevice9ExVtbl PureDevice9Ex_vtable;
extern IDirect3DAuthenticatedChannel9Vtbl PureAuthenticatedChannel9_vtable;
extern IDirect3DCryptoSession9Vtbl PureCryptoSession9_vtable;
extern IDirect3DCubeTexture9Vtbl PureCubeTexture9_vtable;
extern IDirect3DDevice9VideoVtbl PureDevice9Video_vtable;
extern IDirect3DIndexBuffer9Vtbl PureIndexBuffer9_vtable;
extern IDirect3DPixelShader9Vtbl PurePixelShader9_vtable;
extern IDirect3DQuery9Vtbl PureQuery9_vtable;
extern IDirect3DStateBlock9Vtbl PureStateBlock9_vtable;
extern IDirect3DSurface9Vtbl PureSurface9_vtable;
extern IDirect3DSwapChain9Vtbl PureSwapChain9_vtable;
extern IDirect3DSwapChain9ExVtbl PureSwapChain9Ex_vtable;
extern IDirect3DTexture9Vtbl PureTexture9_vtable;
extern IDirect3DVertexBuffer9Vtbl PureVertexBuffer9_vtable;
extern IDirect3DVertexDeclaration9Vtbl PureVertexDeclaration9_vtable;
extern IDirect3DVertexShader9Vtbl PureVertexShader9_vtable;
extern IDirect3DVolume9Vtbl PureVolume9_vtable;
extern IDirect3DVolumeTexture9Vtbl PureVolumeTexture9_vtable;
extern IDirect3DVolumeTexture9Vtbl PureVolumeTexture9_vtable;
extern ID3DAdapter9Vtbl PureAdapter9_vtable;

#endif
