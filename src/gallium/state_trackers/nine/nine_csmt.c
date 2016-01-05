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

#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "os/os_thread.h"
#include "util/u_transfer.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"

#include "nine_queue.h"
#include "nine_csmt.h"
#include "nine_csmt_structs.h"

#include "authenticatedchannel9.h"
#include "basetexture9.h"
#include "cryptosession9.h"
#include "cubetexture9.h"
#include "device9.h"
#include "device9ex.h"
#include "device9video.h"
#include "indexbuffer9.h"
#include "pixelshader9.h"
#include "query9.h"
#include "resource9.h"
#include "stateblock9.h"
#include "surface9.h"
#include "swapchain9.h"
#include "swapchain9ex.h"
#include "texture9.h"
#include "vertexbuffer9.h"
#include "vertexdeclaration9.h"
#include "vertexshader9.h"
#include "volume9.h"
#include "volumetexture9.h"

#define DBG_CHANNEL DBG_DEVICE

#define DEBUG_CREATE_ON_SEPERATE_CONTEXT 1

pipe_static_mutex(d3d_csmt_global);

struct csmt_context {
    HANDLE render_thread;
    struct concurrent_queue *queue;
    boolean terminate;
    IDirect3DSurface9 *rt[NINE_MAX_SIMULTANEOUS_RENDERTARGETS];
    IDirect3DSurface9 *ds;
    struct u_upload_mgr *vertex_uploader;
    struct u_upload_mgr *index_uploader;
};

static HRESULT NINE_WINAPI
PureResource9_SetPrivateData( struct NineResource9 *This,
                              REFGUID refguid,
                              const void *pData,
                              DWORD SizeOfData,
                              DWORD Flags )
{
    HRESULT r;
    pipe_mutex_lock(d3d_csmt_global);
    r = NineResource9_SetPrivateData(This, refguid, pData, SizeOfData, Flags);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureResource9_GetPrivateData( struct NineResource9 *This,
                              REFGUID refguid,
                              void *pData,
                              DWORD *pSizeOfData )
{
    HRESULT r;
    pipe_mutex_lock(d3d_csmt_global);
    r = NineResource9_GetPrivateData(This, refguid, pData, pSizeOfData);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureResource9_FreePrivateData( struct NineResource9 *This,
                               REFGUID refguid )
{
    HRESULT r;
    pipe_mutex_lock(d3d_csmt_global);
    r = NineResource9_FreePrivateData(This, refguid);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static DWORD NINE_WINAPI
PureResource9_SetPriority( struct NineResource9 *This,
                           DWORD PriorityNew )
{
    DWORD r;
    pipe_mutex_lock(d3d_csmt_global);
    r = NineResource9_SetPriority(This, PriorityNew);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static DWORD NINE_WINAPI
PureResource9_GetPriority( struct NineResource9 *This )
{
    DWORD r;
    pipe_mutex_lock(d3d_csmt_global);
    r = NineResource9_GetPriority(This);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}


static void
PureDevice9_EvictManagedResources_rx( struct NineDevice9 *This,
                                      void *arg)
{
    HRESULT r;
    (void) arg;

    r = NineDevice9_EvictManagedResources(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_EvictManagedResources( struct NineDevice9 *This )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureDevice9_EvictManagedResources_rx;
    slot->this = (void *)This;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_Reset_rx( struct NineDevice9 *This,
                            void *arg )
{
    struct csmt_dword1_void1_result_args *args =
            (struct csmt_dword1_void1_result_args *)arg;

    *args->result = NineDevice9_Reset(This,
            (D3DPRESENT_PARAMETERS *)args->obj1);

    nine_csmt_reset(This);
}

static HRESULT NINE_WINAPI
PureDevice9_Reset( struct NineDevice9 *This,
                   D3DPRESENT_PARAMETERS *pPresentationParameters )
{
    struct csmt_context *ctx = This->csmt_context;
    struct csmt_dword1_void1_result_args *args;
    struct queue_element* slot;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword1_void1_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_Reset_rx;
    slot->this = (void *)This;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_Present_rx( struct NineDevice9 *This,
                            void *arg )
{
    struct csmt_dword3_void4_result_args *args =
            (struct csmt_dword3_void4_result_args *)arg;

    *args->result = NineDevice9_Present(This,
            args->obj1,
            args->obj2,
            args->obj3,
            args->obj4);
}

static HRESULT NINE_WINAPI
PureDevice9_Present( struct NineDevice9 *This,
                     const RECT *pSourceRect,
                     const RECT *pDestRect,
                     HWND hDestWindowOverride,
                     const RGNDATA *pDirtyRegion )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_Present_rx;
    slot->this = (void *)This;

    /* pass pointers as we are waiting for result */
    args->obj1 = pSourceRect;
    args->obj2 = pDestRect;
    args->obj3 = hDestWindowOverride;
    args->obj4 = pDirtyRegion;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    nine_bind(&ctx->rt[0], (IDirect3DSurface9 *)This->swapchains[0]->buffers[0]);

    return r;
}

static void
PureDevice9_UpdateSurface_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineDevice9_UpdateSurface(This,
            (IDirect3DSurface9 *)args->obj1,
            args->rect1,
            (IDirect3DSurface9 *)args->obj2,
            args->point1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
    nine_bind(&args->obj2, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_UpdateSurface( struct NineDevice9 *This,
                           IDirect3DSurface9 *pSourceSurface,
                           const RECT *pSourceRect,
                           IDirect3DSurface9 *pDestinationSurface,
                           const POINT *pDestPoint )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_UpdateSurface_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pSourceSurface);

    args->obj2 = NULL;
    nine_bind(&args->obj2, pDestinationSurface);

    if (pDestPoint) {
        args->point1 = &args->_point1;
        args->_point1 = *pDestPoint;
    } else {
        args->point1 = NULL;
    }

    if (pSourceRect) {
        args->rect1 = &args->_rect1;
        args->_rect1 = *pSourceRect;
    } else {
        args->rect1 = NULL;
    }
    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_UpdateTexture_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineDevice9_UpdateTexture(This, args->obj1, args->obj2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
    nine_bind(&args->obj2, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_UpdateTexture( struct NineDevice9 *This,
                           IDirect3DBaseTexture9 *pSourceTexture,
                           IDirect3DBaseTexture9 *pDestinationTexture )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_UpdateTexture_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pSourceTexture);

    args->obj2 = NULL;
    nine_bind(&args->obj2, pDestinationTexture);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_GetRenderTargetData_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineDevice9_GetRenderTargetData(This, args->obj1, args->obj2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
    nine_bind(&args->obj2, NULL);
}

/* Available on PURE devices */
static HRESULT NINE_WINAPI
PureDevice9_GetRenderTargetData( struct NineDevice9 *This,
                                 IDirect3DSurface9 *pRenderTarget,
                                 IDirect3DSurface9 *pDestSurface )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_GetRenderTargetData_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pRenderTarget);

    args->obj2 = NULL;
    nine_bind(&args->obj2, pDestSurface);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_GetFrontBufferData_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_GetFrontBufferData(This, args->arg1, (IDirect3DSurface9 *)args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_GetFrontBufferData( struct NineDevice9 *This,
                                UINT iSwapChain,
                                IDirect3DSurface9 *pDestSurface )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_GetFrontBufferData_rx;
    slot->this = (void *)This;

    args->arg1 = iSwapChain;
    args->obj1 = (void *)pDestSurface;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_StretchRect_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineDevice9_StretchRect(This, args->obj1,
            args->rect1,
            args->obj2,
            args->rect2,
            args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);

    nine_bind(&args->obj1, NULL);
    nine_bind(&args->obj2, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_StretchRect( struct NineDevice9 *This,
                         IDirect3DSurface9 *pSourceSurface,
                         const RECT *pSourceRect,
                         IDirect3DSurface9 *pDestSurface,
                         const RECT *pDestRect,
                         D3DTEXTUREFILTERTYPE Filter )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_StretchRect_rx;
    slot->this = (void *)This;

    args->arg1 = Filter;
    if (pSourceRect) {
        args->rect1 = &args->_rect1;
        args->_rect1 = *pSourceRect;
    } else {
        args->rect1 = NULL;
    }
    if (pDestRect) {
        args->rect2 = &args->_rect2;
        args->_rect2 = *pDestRect;
    } else {
        args->rect2 = NULL;
    }
    args->obj1 = NULL;
    nine_bind(&args->obj1, pSourceSurface);

    args->obj2 = NULL;
    nine_bind(&args->obj2, pDestSurface);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_ColorFill_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineDevice9_ColorFill(This, args->obj1,
            args->rect1,
            args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);

    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_ColorFill( struct NineDevice9 *This,
                       IDirect3DSurface9 *pSurface,
                       const RECT *pRect,
                       D3DCOLOR color )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_ColorFill_rx;
    slot->this = (void *)This;

    args->arg1 = color;
    if (pRect) {
        args->rect1 = &args->_rect1;
        args->_rect1 = *pRect;
    } else {
        args->rect1 = NULL;
    }

    args->obj1 = NULL;
    nine_bind(&args->obj1, pSurface);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetRenderTarget_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetRenderTarget(This, args->arg1, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetRenderTarget( struct NineDevice9 *This,
                             DWORD RenderTargetIndex,
                             IDirect3DSurface9 *pRenderTarget )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    user_assert(RenderTargetIndex < This->caps.NumSimultaneousRTs, D3DERR_INVALIDCALL);

    if (pRenderTarget == ctx->rt[RenderTargetIndex])
        return D3D_OK;

    /* Track rt outside of device context.
     * There's no need to wait for CSMT and we don't touch device state */
    nine_bind(&ctx->rt[RenderTargetIndex], pRenderTarget);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetRenderTarget_rx;
    slot->this = (void *)This;

    args->arg1 = RenderTargetIndex;
    args->obj1 = NULL;
    nine_bind(&args->obj1, pRenderTarget);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

/* Available on PURE devices */
static HRESULT NINE_WINAPI
PureDevice9_GetRenderTarget( struct NineDevice9 *This,
                             DWORD RenderTargetIndex,
                             IDirect3DSurface9 **ppRenderTarget )
{
    struct csmt_context *ctx = This->csmt_context;

    user_assert(RenderTargetIndex < This->caps.NumSimultaneousRTs, D3DERR_INVALIDCALL);
    user_assert(ppRenderTarget, D3DERR_INVALIDCALL);

    /* Track rt outside of device context.
     * There's no need to wait for CSMT and we don't touch device state */
    *ppRenderTarget = (IDirect3DSurface9 *)ctx->rt[RenderTargetIndex];
    if (!ctx->rt[RenderTargetIndex])
        return D3DERR_NOTFOUND;

    NineUnknown_AddRef(NineUnknown(ctx->rt[RenderTargetIndex]));
    return D3D_OK;
}

static void
PureDevice9_SetDepthStencilSurface_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetDepthStencilSurface(This, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetDepthStencilSurface( struct NineDevice9 *This,
                                    IDirect3DSurface9 *pNewZStencil )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    if (pNewZStencil == ctx->ds)
        return D3D_OK;

    /* Track ds outside of device context.
     * There's no need to wait for CSMT and we don't touch device state. */
    nine_bind(&ctx->ds, pNewZStencil);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetDepthStencilSurface_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pNewZStencil);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

/* Available on PURE devices */
static HRESULT NINE_WINAPI
PureDevice9_GetDepthStencilSurface( struct NineDevice9 *This,
                                    IDirect3DSurface9 **ppZStencilSurface )
{
    struct csmt_context *ctx = This->csmt_context;
    user_assert(ppZStencilSurface, D3DERR_INVALIDCALL);

    /* Track ds outside of device context.
     * There's no need to wait for CSMT and we don't touch device state. */
    *ppZStencilSurface = (IDirect3DSurface9 *)ctx->ds;
    if (!ctx->ds)
        return D3DERR_NOTFOUND;

    NineUnknown_AddRef(NineUnknown(ctx->ds));
    return D3D_OK;
}

static void
PureDevice9_BeginScene_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    (void) arg;

    r = NineDevice9_BeginScene(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_BeginScene( struct NineDevice9 *This )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureDevice9_BeginScene_rx;
    slot->this = (void *)This;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_EndScene_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    (void) arg;

    r = NineDevice9_EndScene(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_EndScene( struct NineDevice9 *This )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureDevice9_EndScene_rx;
    slot->this = (void *)This;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_Clear_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    struct csmt_dword4_float1_rect_args *args =
            (struct csmt_dword4_float1_rect_args *)arg;

    r = NineDevice9_Clear(This,
            args->arg1,
            args->rect1,
            args->arg2,
            args->arg3,
            args->arg1_f,
            args->arg4);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_Clear( struct NineDevice9 *This,
                   DWORD Count,
                   const D3DRECT *pRects,
                   DWORD Flags,
                   D3DCOLOR Color,
                   float Z,
                   DWORD Stencil )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword4_float1_rect_args *args;

    user_assert(Count < 2 ,D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword4_float1_rect_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_Clear_rx;
    slot->this = (void *)This;

    args->arg1 = Count;
    args->arg2 = Flags;
    args->arg3 = Color;
    args->arg4 = Stencil;
    args->arg1_f = Z;
    if (pRects) {
        args->rect1 = &args->_rect;
        args->_rect = *pRects;
    } else {
        args->rect1 = NULL;
    }

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetTransform_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_matrix_args *args =
            (struct csmt_dword_matrix_args *)arg;

    r = NineDevice9_SetTransform(This, args->arg1, &args->mat1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetTransform( struct NineDevice9 *This,
                          D3DTRANSFORMSTATETYPE State,
                          const D3DMATRIX *pMatrix )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_matrix_args *args;

    user_assert(pMatrix, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_matrix_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetTransform_rx;
    slot->this = (void *)This;

    args->arg1 = State;
    args->mat1 = *pMatrix;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_MultiplyTransform_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_matrix_args *args =
            (struct csmt_dword_matrix_args *)arg;

    r = NineDevice9_MultiplyTransform(This, args->arg1, &args->mat1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_MultiplyTransform( struct NineDevice9 *This,
                               D3DTRANSFORMSTATETYPE State,
                               const D3DMATRIX *pMatrix )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_matrix_args *args;

    user_assert(pMatrix, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_matrix_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_MultiplyTransform_rx;
    slot->this = (void *)This;

    args->arg1 = State;
    args->mat1 = *pMatrix;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetViewport_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    D3DVIEWPORT9 *args =
            (D3DVIEWPORT9 *)arg;

    r = NineDevice9_SetViewport(This, args);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetViewport( struct NineDevice9 *This,
                         const D3DVIEWPORT9 *pViewport )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    D3DVIEWPORT9 *args;

    user_assert(pViewport, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(D3DVIEWPORT9), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetViewport_rx;
    slot->this = (void *)This;

    *args = *(D3DVIEWPORT9 *)pViewport;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetMaterial_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    D3DMATERIAL9 *args =
            (D3DMATERIAL9 *)arg;

    r = NineDevice9_SetMaterial(This, args);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetMaterial( struct NineDevice9 *This,
                         const D3DMATERIAL9 *pMaterial )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    D3DMATERIAL9 *args;

    user_assert(pMaterial, E_POINTER);

    slot = queue_get_free_slot(ctx->queue, sizeof(D3DMATERIAL9), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetMaterial_rx;
    slot->this = (void *)This;

    *args = *(D3DMATERIAL9 *)pMaterial;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetLight_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    struct csmt_dword_light_args *args =
            (struct csmt_dword_light_args *)arg;

    r = NineDevice9_SetLight(This, args->arg1, &args->light1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetLight( struct NineDevice9 *This,
                      DWORD Index,
                      const D3DLIGHT9 *pLight )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_light_args *args;

    user_assert(pLight, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_light_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetLight_rx;
    slot->this = (void *)This;

    args->arg1 = Index;
    args->light1 = *(D3DLIGHT9 *)pLight;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_LightEnable_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    struct csmt_dword3_args *args =
            (struct csmt_dword3_args *)arg;

    r = NineDevice9_LightEnable(This, args->arg1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_LightEnable( struct NineDevice9 *This,
                         DWORD Index,
                         BOOL Enable )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_LightEnable_rx;
    slot->this = (void *)This;

    args->arg1 = Index;
    args->arg2 = Enable;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetClipPlane_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    struct csmt_dword_float4_args *args =
            (struct csmt_dword_float4_args *)arg;

    r = NineDevice9_SetClipPlane(This, args->arg1_i, &args->arg1_f);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetClipPlane( struct NineDevice9 *This,
                          DWORD Index,
                          const float *pPlane )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_float4_args *args;

    user_assert(pPlane, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_float4_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetClipPlane_rx;
    slot->this = (void *)This;

    args->arg1_i = Index;
    args->arg1_f = pPlane[0];
    args->arg2_f = pPlane[1];
    args->arg3_f = pPlane[2];
    args->arg4_f = pPlane[3];

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetRenderState_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword3_args *args =
            (struct csmt_dword3_args *)arg;

    r = NineDevice9_SetRenderState(This, args->arg1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetRenderState( struct NineDevice9 *This,
                            D3DRENDERSTATETYPE State,
                            DWORD Value )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetRenderState_rx;
    slot->this = (void *)This;

    args->arg1 = State;
    args->arg2 = Value;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetTexture_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetTexture(This, args->arg1, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetTexture( struct NineDevice9 *This,
                        DWORD Stage,
                        IDirect3DBaseTexture9 *pTexture )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetTexture_rx;
    slot->this = (void *)This;

    args->arg1 = Stage;
    args->obj1 = NULL;
    nine_bind(&args->obj1, pTexture);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetTextureStageState_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_dword3_args *args =
            (struct csmt_dword3_args *)arg;

    r = NineDevice9_SetTextureStageState(This, args->arg1, args->arg2, args->arg3);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetTextureStageState( struct NineDevice9 *This,
                                  DWORD Stage,
                                  D3DTEXTURESTAGESTATETYPE Type,
                                  DWORD Value )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetTextureStageState_rx;
    slot->this = (void *)This;

    args->arg1 = Stage;
    args->arg2 = Type;
    args->arg3 = Value;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetSamplerState_rx( struct NineDevice9 *This,
                             void *arg )
{
    HRESULT r;
    struct csmt_dword3_args *args =
            (struct csmt_dword3_args *)arg;

    r = NineDevice9_SetSamplerState(This, args->arg1, args->arg2, args->arg3);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetSamplerState( struct NineDevice9 *This,
                             DWORD Sampler,
                             D3DSAMPLERSTATETYPE Type,
                             DWORD Value )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetSamplerState_rx;
    slot->this = (void *)This;

    args->arg1 = Sampler;
    args->arg2 = Type;
    args->arg3 = Value;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetScissorRect_rx( struct NineDevice9 *This,
                              void *arg )
{
    HRESULT r;
    struct csmt_dword4_float1_rect_args *args =
            (struct csmt_dword4_float1_rect_args *)arg;

    r = NineDevice9_SetScissorRect(This, (RECT *)args->rect1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetScissorRect( struct NineDevice9 *This,
                            const RECT *pRect )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword4_float1_rect_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword4_float1_rect_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetScissorRect_rx;
    slot->this = (void *)This;

    if (pRect) {
        args->rect1 = &args->_rect;
        args->_rect = *(D3DRECT *)pRect;
    } else {
        args->rect1 = NULL;
    }

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_DrawPrimitive_rx( struct NineDevice9 *This,
                              void *arg )
{
    HRESULT r;
    struct csmt_int1_uint5_args *args =
            (struct csmt_int1_uint5_args *)arg;

    r = NineDevice9_DrawPrimitive(This,
                                args->arg1_u,
                                args->arg2_u,
                                args->arg3_u);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_DrawPrimitive( struct NineDevice9 *This,
                           D3DPRIMITIVETYPE PrimitiveType,
                           UINT StartVertex,
                           UINT PrimitiveCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_int1_uint5_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_int1_uint5_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_DrawPrimitive_rx;
    slot->this = (void *)This;

    args->arg1_u = PrimitiveType;
    args->arg2_u = StartVertex;
    args->arg3_u = PrimitiveCount;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_DrawIndexedPrimitive_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_int1_uint5_args *args =
            (struct csmt_int1_uint5_args *)arg;

    r = NineDevice9_DrawIndexedPrimitive(This,
                                            args->arg1_u,
                                            args->arg1_i,
                                            args->arg2_u,
                                            args->arg3_u,
                                            args->arg4_u,
                                            args->arg5_u);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_DrawIndexedPrimitive( struct NineDevice9 *This,
                                  D3DPRIMITIVETYPE PrimitiveType,
                                  INT BaseVertexIndex,
                                  UINT MinVertexIndex,
                                  UINT NumVertices,
                                  UINT startIndex,
                                  UINT primCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_int1_uint5_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_int1_uint5_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_DrawIndexedPrimitive_rx;
    slot->this = (void *)This;

    args->arg1_i = BaseVertexIndex;
    args->arg1_u = PrimitiveType;
    args->arg2_u = MinVertexIndex;
    args->arg3_u = NumVertices;
    args->arg4_u = startIndex;
    args->arg5_u = primCount;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_DrawPrimitiveUP_rx( struct NineDevice9 *This,
                                void *arg )
{
    HRESULT r;
    struct csmt_dword1_uint2_data_args *args =
            (struct csmt_dword1_uint2_data_args *)arg;

    r = NineDevice9_DrawPrimitiveUP(This,
                                    args->arg1,
                                    args->arg1_u,
                                    args->data,
                                    args->arg2_u);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_DrawPrimitiveUP( struct NineDevice9 *This,
                             D3DPRIMITIVETYPE PrimitiveType,
                             UINT PrimitiveCount,
                             const void *pVertexStreamZeroData,
                             UINT VertexStreamZeroStride )
{
    unsigned count, length;
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword1_uint2_data_args *args;

    user_assert(pVertexStreamZeroData && VertexStreamZeroStride,
                   D3DERR_INVALIDCALL);

    count = prim_count_to_vertex_count(PrimitiveType, PrimitiveCount);
    length = count * VertexStreamZeroStride;
    DBG("Got a chunk of %d bytes vertex data\n", length);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword1_uint2_data_args) + length, (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_DrawPrimitiveUP_rx;
    slot->this = (void *)This;

    args->arg1 = PrimitiveType;
    args->arg1_u = PrimitiveCount;
    args->arg2_u = VertexStreamZeroStride;
    args->length = length;
    memcpy(args->data, pVertexStreamZeroData, length);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_DrawIndexedPrimitiveUP_rx( struct NineDevice9 *This,
                                       void *arg )
{
    HRESULT r;
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    r = NineDevice9_DrawIndexedPrimitiveUP(This,
                                    args->arg1,
                                    args->arg1_u,
                                    args->arg2_u,
                                    args->arg3_u,
                                    args->obj1,
                                    args->arg2,
                                    args->obj2,
                                    args->arg4_u);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    FREE(args->obj1);
    FREE(args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_DrawIndexedPrimitiveUP( struct NineDevice9 *This,
                                    D3DPRIMITIVETYPE PrimitiveType,
                                    UINT MinVertexIndex,
                                    UINT NumVertices,
                                    UINT PrimitiveCount,
                                    const void *pIndexData,
                                    D3DFORMAT IndexDataFormat,
                                    const void *pVertexStreamZeroData,
                                    UINT VertexStreamZeroStride )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    int count, index_size;

    user_assert(pVertexStreamZeroData && VertexStreamZeroStride,
                   D3DERR_INVALIDCALL);
    count = prim_count_to_vertex_count(PrimitiveType, PrimitiveCount);
    index_size = (IndexDataFormat == D3DFMT_INDEX16) ? 2 : 4;

    void *data1 = malloc((NumVertices + MinVertexIndex) * VertexStreamZeroStride);
    ERR("copying %d bytes\n", (NumVertices + MinVertexIndex) * VertexStreamZeroStride);
    memcpy((uint8_t*)data1 + MinVertexIndex * VertexStreamZeroStride,
            (uint8_t*)pVertexStreamZeroData + MinVertexIndex * VertexStreamZeroStride,
            NumVertices * VertexStreamZeroStride);

    void *data2 = malloc(count * index_size);
    ERR("copying %d bytes\n", count * index_size);
    memcpy(data2, pIndexData, count * index_size);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_DrawIndexedPrimitiveUP_rx;
    slot->this = (void *)This;

    args->arg1 = PrimitiveType;
    args->arg2 = IndexDataFormat;

    args->arg1_u = MinVertexIndex;
    args->arg2_u = NumVertices;
    args->arg3_u = PrimitiveCount;
    args->arg4_u = VertexStreamZeroStride;

    args->obj1 = data1;
    args->obj2 = data2;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetVertexDeclaration_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetVertexDeclaration(This, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetVertexDeclaration( struct NineDevice9 *This,
                                  IDirect3DVertexDeclaration9 *pDecl )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetVertexDeclaration_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pDecl);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetFVF_rx( struct NineDevice9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword3_args *args =
            (struct csmt_dword3_args *)arg;

    r = NineDevice9_SetFVF(This, args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetFVF( struct NineDevice9 *This,
                    DWORD FVF )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetFVF_rx;
    slot->this = (void *)This;

    args->arg1 = FVF;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetVertexShader_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetVertexShader(This, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetVertexShader( struct NineDevice9 *This,
                             IDirect3DVertexShader9 *pShader )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetVertexShader_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pShader);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetVertexShaderConstantF_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_uint2_vec4_args *args =
            (struct csmt_uint2_vec4_args *)arg;

    r = NineDevice9_SetVertexShaderConstantF(This, args->arg1, (const float *)&args->vec1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetVertexShaderConstantF( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const float *pConstantData,
                                     UINT Vector4fCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    if (Vector4fCount == 1) {
        struct csmt_uint2_vec4_args *args;
        slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec4_args), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetVertexShaderConstantF_rx;
        slot->this = (void *)This;

        args->arg1 = StartRegister;
        args->arg2 = 1;
        memcpy(&args->vec1, pConstantData, sizeof(float[4]));
        queue_set_slot_ready(ctx->queue, slot);
    } else {
        struct csmt_uint2_vec32_args *args;

        for (i = 0; i < Vector4fCount; i+=8) {
            slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec32_args), (void **)&args);
            slot->data = args;
            slot->func = PureDevice9_SetVertexShaderConstantF_rx;
            slot->this = (void *)This;

            args->arg1 = StartRegister + i;
            args->arg2 = MIN2(Vector4fCount - i, 8);
            memcpy(&args->vec1, &pConstantData[i * 4], sizeof(float[4]) * args->arg2);

            queue_set_slot_ready(ctx->queue, slot);
        }
    }

    return D3D_OK;
}

static void
PureDevice9_SetVertexShaderConstantI_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_uint2_vec4_args *args =
            (struct csmt_uint2_vec4_args *)arg;

    /* assume sizeof(int) == sizeof(float) */
    r = NineDevice9_SetPixelShaderConstantB(This, args->arg1, (int *)&args->vec1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetVertexShaderConstantI( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const int *pConstantData,
                                     UINT Vector4iCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_uint2_vec4_args *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < Vector4iCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec4_args), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetVertexShaderConstantI_rx;
        slot->this = (void *)This;

        args->arg1 = StartRegister + i;
        args->arg2 = 1;
        /* assume sizeof(int) == sizeof(float) */
        memcpy(&args->vec1, &pConstantData[i * 4], sizeof(int[4]));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

static void
PureDevice9_SetVertexShaderConstantB_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_uint2_vec4_args *args =
            (struct csmt_uint2_vec4_args *)arg;

    /* assume sizeof(BOOL) == sizeof(float) */
    r = NineDevice9_SetVertexShaderConstantB(This, args->arg1, (BOOL *)&args->vec1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetVertexShaderConstantB( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const int *pConstantData,
                                     UINT BoolCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_uint2_vec4_args *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < BoolCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec4_args), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetVertexShaderConstantB_rx;
        slot->this = (void *)This;

        args->arg1 = StartRegister + i;
        args->arg2 = 1;
        /* assume sizeof(BOOL) == sizeof(float) */
        memcpy(&args->vec1, &pConstantData[i * 4], sizeof(BOOL[4]));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

static void
PureDevice9_SetStreamSource_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_uint3_void_args *args =
            (struct csmt_uint3_void_args *)arg;

    r = NineDevice9_SetStreamSource(This, args->arg1, args->obj1, args->arg2, args->arg3);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetStreamSource( struct NineDevice9 *This,
                             UINT StreamNumber,
                             IDirect3DVertexBuffer9 *pStreamData,
                             UINT OffsetInBytes,
                             UINT Stride )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_uint3_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint3_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetStreamSource_rx;
    slot->this = (void *)This;

    args->arg1 = StreamNumber;
    args->arg2 = OffsetInBytes;
    args->arg3 = Stride;
    args->obj1 = NULL;
    nine_bind(&args->obj1, pStreamData);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetStreamSourceFreq_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_int1_uint5_args *args =
            (struct csmt_int1_uint5_args *)arg;

    r = NineDevice9_SetStreamSourceFreq(This, args->arg1_u, args->arg2_u);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetStreamSourceFreq( struct NineDevice9 *This,
                                 UINT StreamNumber,
                                 UINT Setting )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_int1_uint5_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_int1_uint5_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetStreamSourceFreq_rx;
    slot->this = (void *)This;

    args->arg1_u = StreamNumber;
    args->arg2_u = Setting;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetIndices_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetIndices(This, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetIndices( struct NineDevice9 *This,
        IDirect3DIndexBuffer9 *pIndexData )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetIndices_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pIndexData);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetPixelShader_rx( struct NineDevice9 *This,
                               void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_SetPixelShader(This, args->obj1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&args->obj1, NULL);
}

static HRESULT NINE_WINAPI
PureDevice9_SetPixelShader( struct NineDevice9 *This,
        IDirect3DPixelShader9 *pShader )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetPixelShader_rx;
    slot->this = (void *)This;

    args->obj1 = NULL;
    nine_bind(&args->obj1, pShader);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_SetPixelShaderConstantF_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_uint2_vec4_args *args =
            (struct csmt_uint2_vec4_args *)arg;

    r = NineDevice9_SetPixelShaderConstantF(This, args->arg1, (const float *)&args->vec1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetPixelShaderConstantF( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const float *pConstantData,
                                     UINT Vector4fCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    if (Vector4fCount == 1) {
        struct csmt_uint2_vec4_args *args;
        slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec4_args), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetPixelShaderConstantF_rx;
        slot->this = (void *)This;

        args->arg1 = StartRegister;
        args->arg2 = 1;
        memcpy(&args->vec1, pConstantData, sizeof(float[4]));
        queue_set_slot_ready(ctx->queue, slot);
    } else {
        struct csmt_uint2_vec32_args *args;

        for (i = 0; i < Vector4fCount; i+=8) {
            slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec32_args), (void **)&args);
            slot->data = args;
            slot->func = PureDevice9_SetPixelShaderConstantF_rx;
            slot->this = (void *)This;

            args->arg1 = StartRegister + i;
            args->arg2 = MIN2(Vector4fCount - i, 8);
            memcpy(&args->vec1, &pConstantData[i * 4], sizeof(float[4]) * args->arg2);

            queue_set_slot_ready(ctx->queue, slot);
        }
    }

    return D3D_OK;
}

static void
PureDevice9_SetPixelShaderConstantI_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_uint2_vec4_args *args =
            (struct csmt_uint2_vec4_args *)arg;

    /* assume sizeof(int) == sizeof(float) */
    r = NineDevice9_SetPixelShaderConstantI(This, args->arg1, (int *)&args->vec1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetPixelShaderConstantI( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const int *pConstantData,
                                     UINT Vector4iCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_uint2_vec4_args *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < Vector4iCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec4_args), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetPixelShaderConstantI_rx;
        slot->this = (void *)This;

        args->arg1 = StartRegister + i;
        args->arg2 = 1;
        /* assume sizeof(int) == sizeof(float) */
        memcpy(&args->vec1, &pConstantData[i * 4], sizeof(int[4]));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

static void
PureDevice9_SetPixelShaderConstantB_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_uint2_vec4_args *args =
            (struct csmt_uint2_vec4_args *)arg;

    /* assume sizeof(BOOL) == sizeof(float) */
    r = NineDevice9_SetPixelShaderConstantB(This, args->arg1, (BOOL *)&args->vec1, args->arg2);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetPixelShaderConstantB( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const int *pConstantData,
                                     UINT BoolCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_uint2_vec4_args *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < BoolCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint2_vec4_args), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetPixelShaderConstantB_rx;
        slot->this = (void *)This;

        args->arg1 = StartRegister + i;
        args->arg2 = 1;
        /* assume sizeof(BOOL) == sizeof(float) */
        memcpy(&args->vec1, &pConstantData[i * 4], sizeof(BOOL[4]));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

/* Get-functions, unimplemented functions, likely unused functions,
 * Create-functions, ...
 */
static HRESULT NINE_WINAPI
PureDevice9_GetIndices( struct NineDevice9 *This,
                        IDirect3DIndexBuffer9 **ppIndexData )
{
    ERR("called, but PURE device requested.\n");

    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_TestCooperativeLevel( struct NineDevice9 *This )
{
    return NineDevice9_TestCooperativeLevel(This);
}

static UINT NINE_WINAPI
PureDevice9_GetAvailableTextureMem( struct NineDevice9 *This )
{
    return NineDevice9_GetAvailableTextureMem(This);
}

static HRESULT NINE_WINAPI
PureDevice9_GetDirect3D( struct NineDevice9 *This,
                         IDirect3D9 **ppD3D9 )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9_GetDirect3D(This, ppD3D9);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureDevice9_GetDisplayMode( struct NineDevice9 *This,
                            UINT iSwapChain,
                            D3DDISPLAYMODE *pMode )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9_GetDisplayMode(This, iSwapChain, pMode);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureDevice9_SetCursorProperties( struct NineDevice9 *This,
                                 UINT XHotSpot,
                                 UINT YHotSpot,
                                 IDirect3DSurface9 *pCursorBitmap )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9_SetCursorProperties(This, XHotSpot, YHotSpot, pCursorBitmap);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static void
PureDevice9_SetCursorPosition_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_args *args =
            (struct csmt_dword3_args *)arg;

    NineDevice9_SetCursorPosition(This,
            args->arg1,
            args->arg2,
            args->arg3);
}

static void NINE_WINAPI
PureDevice9_SetCursorPosition( struct NineDevice9 *This,
                               int X,
                               int Y,
                               DWORD Flags )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetCursorPosition_rx;
    slot->this = (void *)This;

    args->arg1 = X;
    args->arg2 = Y;
    args->arg3 = Flags;

    queue_set_slot_ready(ctx->queue, slot);
}

static void
PureDevice9_ShowCursor_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_ShowCursor(This, args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static BOOL NINE_WINAPI
PureDevice9_ShowCursor( struct NineDevice9 *This,
                        BOOL bShow )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_ShowCursor_rx;
    slot->this = (void *)This;

    args->arg1 = bShow;
    //XXX return value
    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static HRESULT NINE_WINAPI
PureDevice9_CreateAdditionalSwapChain( struct NineDevice9 *This,
                                       D3DPRESENT_PARAMETERS *pPresentationParameters,
                                       IDirect3DSwapChain9 **pSwapChain )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9_CreateAdditionalSwapChain(This, pPresentationParameters, pSwapChain);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static void
PureDevice9_GetSwapChain_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_GetSwapChain(This,
            args->arg1_u,
            (IDirect3DSwapChain9 **)args->obj1);
}

static HRESULT NINE_WINAPI
PureDevice9_GetSwapChain( struct NineDevice9 *This,
                          UINT iSwapChain,
                          IDirect3DSwapChain9 **pSwapChain )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_GetSwapChain_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pSwapChain;
    args->arg1_u = iSwapChain;

    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static UINT NINE_WINAPI
PureDevice9_GetNumberOfSwapChains( struct NineDevice9 *This )
{
    UINT r;
    ERR("called\n");

    r = NineDevice9_GetNumberOfSwapChains(This);
    return r;
}

static void
PureDevice9_GetBackBuffer_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_GetBackBuffer(This,
            args->arg1_u,
            args->arg2_u,
            args->arg1,
            (IDirect3DSurface9 **)args->obj1);
}

/* Available on PURE devices */
static HRESULT NINE_WINAPI
PureDevice9_GetBackBuffer( struct NineDevice9 *This,
                           UINT iSwapChain,
                           UINT iBackBuffer,
                           D3DBACKBUFFER_TYPE Type,
                           IDirect3DSurface9 **ppBackBuffer )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_GetBackBuffer_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)ppBackBuffer;
    args->arg1 = Type;
    args->arg1_u = iSwapChain;
    args->arg2_u = iBackBuffer;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static HRESULT NINE_WINAPI
PureDevice9_GetRasterStatus( struct NineDevice9 *This,
                             UINT iSwapChain,
                             D3DRASTER_STATUS *pRasterStatus )
{
    HRESULT r;
    ERR("called, but PURE device requested.\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9_GetRasterStatus(This, iSwapChain, pRasterStatus);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureDevice9_SetDialogBoxMode( struct NineDevice9 *This,
                              BOOL bEnableDialogs )
{
    STUB(D3DERR_INVALIDCALL);
}

static void NINE_WINAPI
PureDevice9_SetGammaRamp( struct NineDevice9 *This,
                          UINT iSwapChain,
                          DWORD Flags,
                          const D3DGAMMARAMP *pRamp )
{
    pipe_mutex_lock(d3d_csmt_global);
    NineDevice9_SetGammaRamp(This, iSwapChain, Flags, pRamp);
    pipe_mutex_unlock(d3d_csmt_global);
}

static void NINE_WINAPI
PureDevice9_GetGammaRamp( struct NineDevice9 *This,
                          UINT iSwapChain,
                          D3DGAMMARAMP *pRamp )
{
    ERR("called, but PURE device requested.\n");
}

static void
PureDevice9_CreateTexture_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateTexture(This,
            args->arg1_u,
            args->arg2_u,
            args->arg3_u,
            args->arg1,
            args->arg2,
            args->arg4_u,
            (IDirect3DTexture9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateTexture( struct NineDevice9 *This,
                           UINT Width,
                           UINT Height,
                           UINT Levels,
                           DWORD Usage,
                           D3DFORMAT Format,
                           D3DPOOL Pool,
                           IDirect3DTexture9 **ppTexture,
                           HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppTexture, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateTexture_rx;
    slot->this = (void *)This;

    args->arg1_u = Width;
    args->arg2_u = Height;
    args->arg3_u = Levels;
    args->arg4_u = Pool;

    args->arg1 = Usage;
    args->arg2 = Format;
    args->obj1 = (void *)ppTexture;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateVolumeTexture_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateVolumeTexture(This,
            args->arg1_u,
            args->arg2_u,
            args->arg3_u,
            args->arg4_u,
            args->arg1,
            args->arg2,
            args->arg3,
            (IDirect3DVolumeTexture9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateVolumeTexture( struct NineDevice9 *This,
                                 UINT Width,
                                 UINT Height,
                                 UINT Depth,
                                 UINT Levels,
                                 DWORD Usage,
                                 D3DFORMAT Format,
                                 D3DPOOL Pool,
                                 IDirect3DVolumeTexture9 **ppVolumeTexture,
                                 HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppVolumeTexture, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateVolumeTexture_rx;
    slot->this = (void *)This;

    args->arg1_u = Width;
    args->arg2_u = Height;
    args->arg3_u = Depth;
    args->arg4_u = Levels;

    args->arg1 = Usage;
    args->arg2 = Format;
    args->arg3 = Pool;

    args->obj1 = (void *)ppVolumeTexture;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateCubeTexture_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateCubeTexture(This,
            args->arg1_u,
            args->arg2_u,
            args->arg1,
            args->arg2,
            args->arg3,
            (IDirect3DCubeTexture9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateCubeTexture( struct NineDevice9 *This,
                               UINT EdgeLength,
                               UINT Levels,
                               DWORD Usage,
                               D3DFORMAT Format,
                               D3DPOOL Pool,
                               IDirect3DCubeTexture9 **ppCubeTexture,
                               HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppCubeTexture, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateCubeTexture_rx;
    slot->this = (void *)This;

    args->arg1_u = EdgeLength;
    args->arg2_u = Levels;

    args->arg1 = Usage;
    args->arg2 = Format;
    args->arg3 = Pool;

    args->obj1 = (void *)ppCubeTexture;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateVertexBuffer_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateVertexBuffer(This,
            args->arg1_u,
            args->arg1,
            args->arg2,
            args->arg3,
            (IDirect3DVertexBuffer9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateVertexBuffer( struct NineDevice9 *This,
                                UINT Length,
                                DWORD Usage,
                                DWORD FVF,
                                D3DPOOL Pool,
                                IDirect3DVertexBuffer9 **ppVertexBuffer,
                                HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppVertexBuffer, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateVertexBuffer_rx;
    slot->this = (void *)This;

    args->arg1_u = Length;

    args->arg1 = Usage;
    args->arg2 = FVF;
    args->arg3 = Pool;

    args->obj1 = (void *)ppVertexBuffer;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateIndexBuffer_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateIndexBuffer(This,
            args->arg1_u,
            args->arg1,
            args->arg2,
            args->arg3,
            (IDirect3DIndexBuffer9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateIndexBuffer( struct NineDevice9 *This,
                               UINT Length,
                               DWORD Usage,
                               D3DFORMAT Format,
                               D3DPOOL Pool,
                               IDirect3DIndexBuffer9 **ppIndexBuffer,
                               HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppIndexBuffer, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateIndexBuffer_rx;
    slot->this = (void *)This;

    args->arg1_u = Length;

    args->arg1 = Usage;
    args->arg2 = Format;
    args->arg3 = Pool;

    args->obj1 = (void *)ppIndexBuffer;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateRenderTarget_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateRenderTarget(This,
            args->arg1_u,
            args->arg2_u,
            args->arg1,
            args->arg2,
            args->arg3,
            args->arg3_u,
            (IDirect3DSurface9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateRenderTarget( struct NineDevice9 *This,
                                UINT Width,
                                UINT Height,
                                D3DFORMAT Format,
                                D3DMULTISAMPLE_TYPE MultiSample,
                                DWORD MultisampleQuality,
                                BOOL Pureable,
                                IDirect3DSurface9 **ppSurface,
                                HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppSurface, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateRenderTarget_rx;
    slot->this = (void *)This;

    args->arg1_u = Width;
    args->arg2_u = Height;
    args->arg3_u = Pureable;

    args->arg1 = Format;
    args->arg2 = MultiSample;
    args->arg3 = MultisampleQuality;

    args->obj1 = (void *)ppSurface;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateDepthStencilSurface_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateDepthStencilSurface(This,
            args->arg1_u,
            args->arg2_u,
            args->arg1,
            args->arg2,
            args->arg3,
            args->arg3_u,
            (IDirect3DSurface9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateDepthStencilSurface( struct NineDevice9 *This,
                                       UINT Width,
                                       UINT Height,
                                       D3DFORMAT Format,
                                       D3DMULTISAMPLE_TYPE MultiSample,
                                       DWORD MultisampleQuality,
                                       BOOL Discard,
                                       IDirect3DSurface9 **ppSurface,
                                       HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppSurface, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateDepthStencilSurface_rx;
    slot->this = (void *)This;

    args->arg1_u = Width;
    args->arg2_u = Height;
    args->arg3_u = Discard;

    args->arg1 = Format;
    args->arg2 = MultiSample;
    args->arg3 = MultisampleQuality;

    args->obj1 = (void *)ppSurface;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateOffscreenPlainSurface_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateOffscreenPlainSurface(This,
            args->arg1_u,
            args->arg2_u,
            args->arg1,
            args->arg2,
            (IDirect3DSurface9 **)args->obj1,
            args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateOffscreenPlainSurface( struct NineDevice9 *This,
                                         UINT Width,
                                         UINT Height,
                                         D3DFORMAT Format,
                                         D3DPOOL Pool,
                                         IDirect3DSurface9 **ppSurface,
                                         HANDLE *pSharedHandle )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppSurface, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateOffscreenPlainSurface_rx;
    slot->this = (void *)This;

    args->arg1_u = Width;
    args->arg2_u = Height;

    args->arg1 = Format;
    args->arg2 = Pool;

    args->obj1 = (void *)ppSurface;
    args->obj2 = pSharedHandle;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateStateBlock_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateStateBlock(This,
            args->arg1,
            (IDirect3DStateBlock9 **)args->obj1);
}

/* allowed on PURE devices */
static HRESULT NINE_WINAPI
PureDevice9_CreateStateBlock( struct NineDevice9 *This,
                              D3DSTATEBLOCKTYPE Type,
                              IDirect3DStateBlock9 **ppSB )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppSB, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateStateBlock_rx;
    slot->this = (void *)This;

    args->arg1 = Type;

    args->obj1 = (void *)ppSB;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateVertexDeclaration_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateVertexDeclaration(This,
            (const D3DVERTEXELEMENT9 *)args->obj1,
            (IDirect3DVertexDeclaration9 **)args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateVertexDeclaration( struct NineDevice9 *This,
                                     const D3DVERTEXELEMENT9 *pVertexElements,
                                     IDirect3DVertexDeclaration9 **ppDecl )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(pVertexElements, D3DERR_INVALIDCALL);
    user_assert(ppDecl, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateVertexDeclaration_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pVertexElements;
    args->obj2 = (void *)ppDecl;

    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateVertexShader_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateVertexShader(This,
            (const DWORD *)args->obj1,
            (IDirect3DVertexShader9 **)args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateVertexShader( struct NineDevice9 *This,
                                const DWORD *pFunction,
                                IDirect3DVertexShader9 **ppShader )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(pFunction, D3DERR_INVALIDCALL);
    user_assert(ppShader, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateVertexShader_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pFunction;
    args->obj2 = (void *)ppShader;

    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreatePixelShader_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreatePixelShader(This,
            (const DWORD *)args->obj1,
            (IDirect3DPixelShader9 **)args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9_CreatePixelShader( struct NineDevice9 *This,
                               const DWORD *pFunction,
                               IDirect3DPixelShader9 **ppShader )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(pFunction, D3DERR_INVALIDCALL);
    user_assert(ppShader, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreatePixelShader_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pFunction;
    args->obj2 = (void *)ppShader;

    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_CreateQuery_rx( struct NineDevice9 *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9_CreateQuery(This,
            args->arg1,
            (IDirect3DQuery9 **)args->obj1);
}

static HRESULT NINE_WINAPI
PureDevice9_CreateQuery( struct NineDevice9 *This,
                         D3DQUERYTYPE Type,
                         IDirect3DQuery9 **ppQuery )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    user_assert(ppQuery, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_CreateQuery_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)ppQuery;
    args->arg1 = Type;

    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureDevice9_BeginStateBlock_rx( struct NineDevice9 *This,
                                     void *arg )
{
    (void) arg;

    NineDevice9_BeginStateBlock(This);
}

static HRESULT NINE_WINAPI
PureDevice9_BeginStateBlock( struct NineDevice9 *This )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureDevice9_BeginStateBlock_rx;
    slot->this = (void *)This;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureDevice9_EndStateBlock_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_EndStateBlock(This,
            (IDirect3DStateBlock9 **)args->obj1);
    if (r != D3D_OK)
        ERR("failed with %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_EndStateBlock( struct NineDevice9 *This,
                           IDirect3DStateBlock9 **ppSB )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    user_assert(ppSB, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_EndStateBlock_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)ppSB;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return *ppSB ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetTransform( struct NineDevice9 *This,
                          D3DTRANSFORMSTATETYPE State,
                          D3DMATRIX *pMatrix )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetViewport( struct NineDevice9 *This,
                         D3DVIEWPORT9 *pViewport )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetMaterial( struct NineDevice9 *This,
                         D3DMATERIAL9 *pMaterial )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetLight( struct NineDevice9 *This,
                      DWORD Index,
                      D3DLIGHT9 *pLight )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetLightEnable( struct NineDevice9 *This,
                            DWORD Index,
                            BOOL *pEnable )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetClipPlane( struct NineDevice9 *This,
                          DWORD Index,
                          float *pPlane )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetRenderState( struct NineDevice9 *This,
                            D3DRENDERSTATETYPE State,
                            DWORD *pValue )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_SetClipStatus( struct NineDevice9 *This,
                           const D3DCLIPSTATUS9 *pClipStatus )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetClipStatus( struct NineDevice9 *This,
                           D3DCLIPSTATUS9 *pClipStatus )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetTexture( struct NineDevice9 *This,
                        DWORD Stage,
                        IDirect3DBaseTexture9 **ppTexture )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetTextureStageState( struct NineDevice9 *This,
                                  DWORD Stage,
                                  D3DTEXTURESTAGESTATETYPE Type,
                                  DWORD *pValue )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetSamplerState( struct NineDevice9 *This,
                             DWORD Sampler,
                             D3DSAMPLERSTATETYPE Type,
                             DWORD *pValue )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static void
PureDevice9_ValidateDevice_rx( struct NineDevice9 *This,
                                     void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineDevice9_ValidateDevice(This,
            (DWORD *)args->obj1);
    if (r != D3D_OK)
        ERR("failed with %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_ValidateDevice( struct NineDevice9 *This,
                            DWORD *pNumPasses )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    user_assert(pNumPasses, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_ValidateDevice_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pNumPasses;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return D3D_OK;
}

static HRESULT NINE_WINAPI
PureDevice9_SetPaletteEntries( struct NineDevice9 *This,
                               UINT PaletteNumber,
                               const PALETTEENTRY *pEntries )
{
    STUB(D3D_OK); /* like wine */
}

static HRESULT NINE_WINAPI
PureDevice9_GetPaletteEntries( struct NineDevice9 *This,
                               UINT PaletteNumber,
                               PALETTEENTRY *pEntries )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_SetCurrentTexturePalette( struct NineDevice9 *This,
                                      UINT PaletteNumber )
{
    STUB(D3D_OK); /* like wine */
}

static HRESULT NINE_WINAPI
PureDevice9_GetCurrentTexturePalette( struct NineDevice9 *This,
                                      UINT *PaletteNumber )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetScissorRect( struct NineDevice9 *This,
                            RECT *pRect )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_SetSoftwareVertexProcessing( struct NineDevice9 *This,
                                         BOOL bSoftware )
{
    STUB(D3DERR_INVALIDCALL);
}

static BOOL NINE_WINAPI
PureDevice9_GetSoftwareVertexProcessing( struct NineDevice9 *This )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_SetNPatchMode( struct NineDevice9 *This,
                           float nSegments )
{
    STUB(D3DERR_INVALIDCALL);
}

static float NINE_WINAPI
PureDevice9_GetNPatchMode( struct NineDevice9 *This )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_ProcessVertices( struct NineDevice9 *This,
                             UINT SrcStartIndex,
                             UINT DestIndex,
                             UINT VertexCount,
                             IDirect3DVertexBuffer9 *pDestBuffer,
                             IDirect3DVertexDeclaration9 *pVertexDecl,
                             DWORD Flags )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetVertexDeclaration( struct NineDevice9 *This,
                                  IDirect3DVertexDeclaration9 **ppDecl )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetFVF( struct NineDevice9 *This,
                    DWORD *pFVF )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetVertexShader( struct NineDevice9 *This,
                             IDirect3DVertexShader9 **ppShader )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetVertexShaderConstantF( struct NineDevice9 *This,
                                      UINT StartRegister,
                                      float *pConstantData,
                                      UINT Vector4fCount )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetVertexShaderConstantI( struct NineDevice9 *This,
                                      UINT StartRegister,
                                      int *pConstantData,
                                      UINT Vector4iCount )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetVertexShaderConstantB( struct NineDevice9 *This,
                                      UINT StartRegister,
                                      BOOL *pConstantData,
                                      UINT BoolCount )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetStreamSource( struct NineDevice9 *This,
                             UINT StreamNumber,
                             IDirect3DVertexBuffer9 **ppStreamData,
                             UINT *pOffsetInBytes,
                             UINT *pStride )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetStreamSourceFreq( struct NineDevice9 *This,
                                 UINT StreamNumber,
                                 UINT *pSetting )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetPixelShader( struct NineDevice9 *This,
                            IDirect3DPixelShader9 **ppShader )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetPixelShaderConstantF( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     float *pConstantData,
                                     UINT Vector4fCount )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetPixelShaderConstantI( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     int *pConstantData,
                                     UINT Vector4iCount )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_GetPixelShaderConstantB( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     BOOL *pConstantData,
                                     UINT BoolCount )
{
    ERR("called, but PURE device requested.\n");
    return D3DERR_INVALIDCALL;
}

static HRESULT NINE_WINAPI
PureDevice9_DrawRectPatch( struct NineDevice9 *This,
                           UINT Handle,
                           const float *pNumSegs,
                           const D3DRECTPATCH_INFO *pRectPatchInfo )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9_DrawTriPatch( struct NineDevice9 *This,
                          UINT Handle,
                          const float *pNumSegs,
                          const D3DTRIPATCH_INFO *pTriPatchInfo )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9_DeletePatch( struct NineDevice9 *This,
                         UINT Handle )
{
    STUB(D3DERR_INVALIDCALL);
}

IDirect3DDevice9Vtbl PureDevice9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureDevice9_TestCooperativeLevel,
    (void *)PureDevice9_GetAvailableTextureMem,
    (void *)PureDevice9_EvictManagedResources,
    (void *)PureDevice9_GetDirect3D,
    (void *)NineDevice9_GetDeviceCaps, /* immutable */
    (void *)PureDevice9_GetDisplayMode,
    (void *)NineDevice9_GetCreationParameters, /* immutable */
    (void *)PureDevice9_SetCursorProperties,
    (void *)PureDevice9_SetCursorPosition,
    (void *)PureDevice9_ShowCursor,
    (void *)PureDevice9_CreateAdditionalSwapChain,
    (void *)PureDevice9_GetSwapChain,
    (void *)PureDevice9_GetNumberOfSwapChains,
    (void *)PureDevice9_Reset,
    (void *)PureDevice9_Present,
    (void *)PureDevice9_GetBackBuffer,
    (void *)PureDevice9_GetRasterStatus,
    (void *)PureDevice9_SetDialogBoxMode,
    (void *)PureDevice9_SetGammaRamp,
    (void *)PureDevice9_GetGammaRamp,
    (void *)PureDevice9_CreateTexture,
    (void *)PureDevice9_CreateVolumeTexture,
    (void *)PureDevice9_CreateCubeTexture,
    (void *)PureDevice9_CreateVertexBuffer,
    (void *)PureDevice9_CreateIndexBuffer,
    (void *)PureDevice9_CreateRenderTarget,
    (void *)PureDevice9_CreateDepthStencilSurface,
    (void *)PureDevice9_UpdateSurface,
    (void *)PureDevice9_UpdateTexture,
    (void *)PureDevice9_GetRenderTargetData,
    (void *)PureDevice9_GetFrontBufferData,
    (void *)PureDevice9_StretchRect,
    (void *)PureDevice9_ColorFill,
    (void *)PureDevice9_CreateOffscreenPlainSurface,
    (void *)PureDevice9_SetRenderTarget,
    (void *)PureDevice9_GetRenderTarget,
    (void *)PureDevice9_SetDepthStencilSurface,
    (void *)PureDevice9_GetDepthStencilSurface,
    (void *)PureDevice9_BeginScene,
    (void *)PureDevice9_EndScene,
    (void *)PureDevice9_Clear,
    (void *)PureDevice9_SetTransform,
    (void *)PureDevice9_GetTransform,
    (void *)PureDevice9_MultiplyTransform,
    (void *)PureDevice9_SetViewport,
    (void *)PureDevice9_GetViewport,
    (void *)PureDevice9_SetMaterial,
    (void *)PureDevice9_GetMaterial,
    (void *)PureDevice9_SetLight,
    (void *)PureDevice9_GetLight,
    (void *)PureDevice9_LightEnable,
    (void *)PureDevice9_GetLightEnable,
    (void *)PureDevice9_SetClipPlane,
    (void *)PureDevice9_GetClipPlane,
    (void *)PureDevice9_SetRenderState,
    (void *)PureDevice9_GetRenderState,
    (void *)PureDevice9_CreateStateBlock,
    (void *)PureDevice9_BeginStateBlock,
    (void *)PureDevice9_EndStateBlock,
    (void *)PureDevice9_SetClipStatus,
    (void *)PureDevice9_GetClipStatus,
    (void *)PureDevice9_GetTexture,
    (void *)PureDevice9_SetTexture,
    (void *)PureDevice9_GetTextureStageState,
    (void *)PureDevice9_SetTextureStageState,
    (void *)PureDevice9_GetSamplerState,
    (void *)PureDevice9_SetSamplerState,
    (void *)PureDevice9_ValidateDevice,
    (void *)PureDevice9_SetPaletteEntries,
    (void *)PureDevice9_GetPaletteEntries,
    (void *)PureDevice9_SetCurrentTexturePalette,
    (void *)PureDevice9_GetCurrentTexturePalette,
    (void *)PureDevice9_SetScissorRect,
    (void *)PureDevice9_GetScissorRect,
    (void *)PureDevice9_SetSoftwareVertexProcessing,
    (void *)PureDevice9_GetSoftwareVertexProcessing,
    (void *)PureDevice9_SetNPatchMode,
    (void *)PureDevice9_GetNPatchMode,
    (void *)PureDevice9_DrawPrimitive,
    (void *)PureDevice9_DrawIndexedPrimitive,
    (void *)PureDevice9_DrawPrimitiveUP,
    (void *)PureDevice9_DrawIndexedPrimitiveUP,
    (void *)PureDevice9_ProcessVertices,
    (void *)PureDevice9_CreateVertexDeclaration,
    (void *)PureDevice9_SetVertexDeclaration,
    (void *)PureDevice9_GetVertexDeclaration,
    (void *)PureDevice9_SetFVF,
    (void *)PureDevice9_GetFVF,
    (void *)PureDevice9_CreateVertexShader,
    (void *)PureDevice9_SetVertexShader,
    (void *)PureDevice9_GetVertexShader,
    (void *)PureDevice9_SetVertexShaderConstantF,
    (void *)PureDevice9_GetVertexShaderConstantF,
    (void *)PureDevice9_SetVertexShaderConstantI,
    (void *)PureDevice9_GetVertexShaderConstantI,
    (void *)PureDevice9_SetVertexShaderConstantB,
    (void *)PureDevice9_GetVertexShaderConstantB,
    (void *)PureDevice9_SetStreamSource,
    (void *)PureDevice9_GetStreamSource,
    (void *)PureDevice9_SetStreamSourceFreq,
    (void *)PureDevice9_GetStreamSourceFreq,
    (void *)PureDevice9_SetIndices,
    (void *)PureDevice9_GetIndices,
    (void *)PureDevice9_CreatePixelShader,
    (void *)PureDevice9_SetPixelShader,
    (void *)PureDevice9_GetPixelShader,
    (void *)PureDevice9_SetPixelShaderConstantF,
    (void *)PureDevice9_GetPixelShaderConstantF,
    (void *)PureDevice9_SetPixelShaderConstantI,
    (void *)PureDevice9_GetPixelShaderConstantI,
    (void *)PureDevice9_SetPixelShaderConstantB,
    (void *)PureDevice9_GetPixelShaderConstantB,
    (void *)PureDevice9_DrawRectPatch,
    (void *)PureDevice9_DrawTriPatch,
    (void *)PureDevice9_DeletePatch,
    (void *)PureDevice9_CreateQuery
};

static HRESULT NINE_WINAPI
PureDevice9Ex_SetConvolutionMonoKernel( struct NineDevice9Ex *This,
                                        UINT width,
                                        UINT height,
                                        float *rows,
                                        float *columns )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_ComposeRects( struct NineDevice9Ex *This,
                            IDirect3DSurface9 *pSrc,
                            IDirect3DSurface9 *pDst,
                            IDirect3DVertexBuffer9 *pSrcRectDescs,
                            UINT NumRects,
                            IDirect3DVertexBuffer9 *pDstRectDescs,
                            D3DCOMPOSERECTSOP Operation,
                            int Xoffset,
                            int Yoffset )
{
    STUB(D3DERR_INVALIDCALL);
}

static void
PureDevice9Ex_PresentEx_rx( struct NineDevice9Ex *This,
                                     void *arg )
{
    struct csmt_dword3_void4_result_args *args =
            (struct csmt_dword3_void4_result_args *)arg;

    *args->result = NineDevice9Ex_PresentEx(This,
            (const RECT *)args->obj1,
            (const RECT *)args->obj2,
            (HWND)args->obj3,
            (const RGNDATA *)args->obj4,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_PresentEx( struct NineDevice9Ex *This,
                         const RECT *pSourceRect,
                         const RECT *pDestRect,
                         HWND hDestWindowOverride,
                         const RGNDATA *pDirtyRegion,
                         DWORD dwFlags )
{
    struct csmt_context *ctx = This->base.csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9Ex_PresentEx_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pSourceRect;
    args->obj2 = (void *)pDestRect;
    args->obj3 = (void *)hDestWindowOverride;
    args->obj4 = (void *)pDirtyRegion;
    args->arg1 = dwFlags;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static HRESULT NINE_WINAPI
PureDevice9Ex_GetGPUThreadPriority( struct NineDevice9Ex *This,
                                    INT *pPriority )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_SetGPUThreadPriority( struct NineDevice9Ex *This,
                                    INT Priority )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_WaitForVBlank( struct NineDevice9Ex *This,
                             UINT iSwapChain )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_CheckResourceResidency( struct NineDevice9Ex *This,
                                      IDirect3DResource9 **pResourceArray,
                                      UINT32 NumResources )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_SetMaximumFrameLatency( struct NineDevice9Ex *This,
                                      UINT MaxLatency )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_GetMaximumFrameLatency( struct NineDevice9Ex *This,
                                      UINT *pMaxLatency )
{
    STUB(D3DERR_INVALIDCALL);
}

static void
PureDevice9Ex_CheckDeviceState_rx( struct NineDevice9Ex *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9Ex_CheckDeviceState(This,
                                                   args->obj1);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_CheckDeviceState( struct NineDevice9Ex *This,
                                HWND hDestinationWindow )
{
    struct csmt_context *ctx = This->base.csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9Ex_CheckDeviceState_rx;
    slot->this = (void *)This;

    args->obj1 = hDestinationWindow;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static HRESULT NINE_WINAPI
PureDevice9Ex_CreateRenderTargetEx( struct NineDevice9Ex *This,
                                    UINT Width,
                                    UINT Height,
                                    D3DFORMAT Format,
                                    D3DMULTISAMPLE_TYPE MultiSample,
                                    DWORD MultisampleQuality,
                                    BOOL Pureable,
                                    IDirect3DSurface9 **ppSurface,
                                    HANDLE *pSharedHandle,
                                    DWORD Usage )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9Ex_CreateRenderTargetEx(This, Width, Height, Format, MultiSample, MultisampleQuality, Pureable, ppSurface, pSharedHandle, Usage);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureDevice9Ex_CreateOffscreenPlainSurfaceEx( struct NineDevice9Ex *This,
                                             UINT Width,
                                             UINT Height,
                                             D3DFORMAT Format,
                                             D3DPOOL Pool,
                                             IDirect3DSurface9 **ppSurface,
                                             HANDLE *pSharedHandle,
                                             DWORD Usage )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9Ex_CreateOffscreenPlainSurfaceEx(This, Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static HRESULT NINE_WINAPI
PureDevice9Ex_CreateDepthStencilSurfaceEx( struct NineDevice9Ex *This,
                                           UINT Width,
                                           UINT Height,
                                           D3DFORMAT Format,
                                           D3DMULTISAMPLE_TYPE MultiSample,
                                           DWORD MultisampleQuality,
                                           BOOL Discard,
                                           IDirect3DSurface9 **ppSurface,
                                           HANDLE *pSharedHandle,
                                           DWORD Usage )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9Ex_CreateDepthStencilSurfaceEx(This, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

static void
PureDevice9Ex_ResetEx_rx( struct NineDevice9Ex *This,
                                     void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineDevice9Ex_ResetEx(This,
            (D3DPRESENT_PARAMETERS *)args->obj1,
            (D3DDISPLAYMODEEX *)args->obj2);
}

static HRESULT NINE_WINAPI
PureDevice9Ex_ResetEx( struct NineDevice9Ex *This,
                       D3DPRESENT_PARAMETERS *pPresentationParameters,
                       D3DDISPLAYMODEEX *pFullscreenDisplayMode )
{
    struct csmt_context *ctx = This->base.csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9Ex_ResetEx_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pPresentationParameters;
    args->obj2 = (void *)pFullscreenDisplayMode;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static HRESULT NINE_WINAPI
PureDevice9Ex_GetDisplayModeEx( struct NineDevice9Ex *This,
                                UINT iSwapChain,
                                D3DDISPLAYMODEEX *pMode,
                                D3DDISPLAYROTATION *pRotation )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineDevice9Ex_GetDisplayModeEx(This, iSwapChain, pMode, pRotation);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

IDirect3DDevice9ExVtbl PureDevice9Ex_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureDevice9_TestCooperativeLevel,
    (void *)PureDevice9_GetAvailableTextureMem,
    (void *)PureDevice9_EvictManagedResources,
    (void *)PureDevice9_GetDirect3D,
    (void *)NineDevice9_GetDeviceCaps,
    (void *)PureDevice9_GetDisplayMode,
    (void *)NineDevice9_GetCreationParameters,
    (void *)PureDevice9_SetCursorProperties,
    (void *)PureDevice9_SetCursorPosition,
    (void *)PureDevice9_ShowCursor,
    (void *)PureDevice9_CreateAdditionalSwapChain,
    (void *)PureDevice9_GetSwapChain,
    (void *)PureDevice9_GetNumberOfSwapChains,
    (void *)PureDevice9_Reset,
    (void *)PureDevice9_Present,
    (void *)PureDevice9_GetBackBuffer,
    (void *)PureDevice9_GetRasterStatus,
    (void *)PureDevice9_SetDialogBoxMode,
    (void *)PureDevice9_SetGammaRamp,
    (void *)PureDevice9_GetGammaRamp,
    (void *)PureDevice9_CreateTexture,
    (void *)PureDevice9_CreateVolumeTexture,
    (void *)PureDevice9_CreateCubeTexture,
    (void *)PureDevice9_CreateVertexBuffer,
    (void *)PureDevice9_CreateIndexBuffer,
    (void *)PureDevice9_CreateRenderTarget,
    (void *)PureDevice9_CreateDepthStencilSurface,
    (void *)PureDevice9_UpdateSurface,
    (void *)PureDevice9_UpdateTexture,
    (void *)PureDevice9_GetRenderTargetData,
    (void *)PureDevice9_GetFrontBufferData,
    (void *)PureDevice9_StretchRect,
    (void *)PureDevice9_ColorFill,
    (void *)PureDevice9_CreateOffscreenPlainSurface,
    (void *)PureDevice9_SetRenderTarget,
    (void *)PureDevice9_GetRenderTarget,
    (void *)PureDevice9_SetDepthStencilSurface,
    (void *)PureDevice9_GetDepthStencilSurface,
    (void *)PureDevice9_BeginScene,
    (void *)PureDevice9_EndScene,
    (void *)PureDevice9_Clear,
    (void *)PureDevice9_SetTransform,
    (void *)PureDevice9_GetTransform,
    (void *)PureDevice9_MultiplyTransform,
    (void *)PureDevice9_SetViewport,
    (void *)PureDevice9_GetViewport,
    (void *)PureDevice9_SetMaterial,
    (void *)PureDevice9_GetMaterial,
    (void *)PureDevice9_SetLight,
    (void *)PureDevice9_GetLight,
    (void *)PureDevice9_LightEnable,
    (void *)PureDevice9_GetLightEnable,
    (void *)PureDevice9_SetClipPlane,
    (void *)PureDevice9_GetClipPlane,
    (void *)PureDevice9_SetRenderState,
    (void *)PureDevice9_GetRenderState,
    (void *)PureDevice9_CreateStateBlock,
    (void *)PureDevice9_BeginStateBlock,
    (void *)PureDevice9_EndStateBlock,
    (void *)PureDevice9_SetClipStatus,
    (void *)PureDevice9_GetClipStatus,
    (void *)PureDevice9_GetTexture,
    (void *)PureDevice9_SetTexture,
    (void *)PureDevice9_GetTextureStageState,
    (void *)PureDevice9_SetTextureStageState,
    (void *)PureDevice9_GetSamplerState,
    (void *)PureDevice9_SetSamplerState,
    (void *)PureDevice9_ValidateDevice,
    (void *)PureDevice9_SetPaletteEntries,
    (void *)PureDevice9_GetPaletteEntries,
    (void *)PureDevice9_SetCurrentTexturePalette,
    (void *)PureDevice9_GetCurrentTexturePalette,
    (void *)PureDevice9_SetScissorRect,
    (void *)PureDevice9_GetScissorRect,
    (void *)PureDevice9_SetSoftwareVertexProcessing,
    (void *)PureDevice9_GetSoftwareVertexProcessing,
    (void *)PureDevice9_SetNPatchMode,
    (void *)PureDevice9_GetNPatchMode,
    (void *)PureDevice9_DrawPrimitive,
    (void *)PureDevice9_DrawIndexedPrimitive,
    (void *)PureDevice9_DrawPrimitiveUP,
    (void *)PureDevice9_DrawIndexedPrimitiveUP,
    (void *)PureDevice9_ProcessVertices,
    (void *)PureDevice9_CreateVertexDeclaration,
    (void *)PureDevice9_SetVertexDeclaration,
    (void *)PureDevice9_GetVertexDeclaration,
    (void *)PureDevice9_SetFVF,
    (void *)PureDevice9_GetFVF,
    (void *)PureDevice9_CreateVertexShader,
    (void *)PureDevice9_SetVertexShader,
    (void *)PureDevice9_GetVertexShader,
    (void *)PureDevice9_SetVertexShaderConstantF,
    (void *)PureDevice9_GetVertexShaderConstantF,
    (void *)PureDevice9_SetVertexShaderConstantI,
    (void *)PureDevice9_GetVertexShaderConstantI,
    (void *)PureDevice9_SetVertexShaderConstantB,
    (void *)PureDevice9_GetVertexShaderConstantB,
    (void *)PureDevice9_SetStreamSource,
    (void *)PureDevice9_GetStreamSource,
    (void *)PureDevice9_SetStreamSourceFreq,
    (void *)PureDevice9_GetStreamSourceFreq,
    (void *)PureDevice9_SetIndices,
    (void *)PureDevice9_GetIndices,
    (void *)PureDevice9_CreatePixelShader,
    (void *)PureDevice9_SetPixelShader,
    (void *)PureDevice9_GetPixelShader,
    (void *)PureDevice9_SetPixelShaderConstantF,
    (void *)PureDevice9_GetPixelShaderConstantF,
    (void *)PureDevice9_SetPixelShaderConstantI,
    (void *)PureDevice9_GetPixelShaderConstantI,
    (void *)PureDevice9_SetPixelShaderConstantB,
    (void *)PureDevice9_GetPixelShaderConstantB,
    (void *)PureDevice9_DrawRectPatch,
    (void *)PureDevice9_DrawTriPatch,
    (void *)PureDevice9_DeletePatch,
    (void *)PureDevice9_CreateQuery,
    (void *)PureDevice9Ex_SetConvolutionMonoKernel,
    (void *)PureDevice9Ex_ComposeRects,
    (void *)PureDevice9Ex_PresentEx,
    (void *)PureDevice9Ex_GetGPUThreadPriority,
    (void *)PureDevice9Ex_SetGPUThreadPriority,
    (void *)PureDevice9Ex_WaitForVBlank,
    (void *)PureDevice9Ex_CheckResourceResidency,
    (void *)PureDevice9Ex_SetMaximumFrameLatency,
    (void *)PureDevice9Ex_GetMaximumFrameLatency,
    (void *)PureDevice9Ex_CheckDeviceState,
    (void *)PureDevice9Ex_CreateRenderTargetEx,
    (void *)PureDevice9Ex_CreateOffscreenPlainSurfaceEx,
    (void *)PureDevice9Ex_CreateDepthStencilSurfaceEx,
    (void *)PureDevice9Ex_ResetEx,
    (void *)PureDevice9Ex_GetDisplayModeEx
};

static void
PureSurface9_GetContainer_rx( struct NineSurface9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineSurface9_GetContainer(This,
            (REFIID)args->obj2,
            (void **)args->obj3);
}

static HRESULT NINE_WINAPI
PureSurface9_GetContainer( struct NineSurface9 *This,
                           REFIID riid,
                           void **ppContainer )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSurface9_GetContainer_rx;
    slot->this = (void *)This;

    args->obj2 = (void *)riid;
    args->obj3 = (void *)ppContainer;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSurface9_LockRect_rx( struct NineSurface9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineSurface9_LockRect(This,
            (D3DLOCKED_RECT *)args->obj2,
            (const RECT *)args->obj3,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureSurface9_LockRect( struct NineSurface9 *This,
                       D3DLOCKED_RECT *pPureedRect,
                       const RECT *pRect,
                       DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSurface9_LockRect_rx;
    slot->this = (void *)This;

    args->obj2 = (void *)pPureedRect;
    args->obj3 = (void *)pRect;
    args->arg1 = Flags;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSurface9_UnlockRect_rx( struct NineSurface9 *This,
                       void *arg )
{
    HRESULT r;

    r = NineSurface9_UnlockRect(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureSurface9_UnlockRect( struct NineSurface9 *This )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureSurface9_UnlockRect_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static HRESULT NINE_WINAPI
PureSurface9_GetDC( struct NineSurface9 *This,
                    HDC *phdc )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureSurface9_ReleaseDC( struct NineSurface9 *This,
                        HDC hdc )
{
    STUB(D3DERR_INVALIDCALL);
}

IDirect3DSurface9Vtbl PureSurface9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureResource9_SetPriority,
    (void *)PureResource9_GetPriority,
    (void *)NineResource9_PreLoad, /* nop */
    (void *)NineResource9_GetType, /* immutable */
    (void *)PureSurface9_GetContainer,
    (void *)NineSurface9_GetDesc, /* immutable */
    (void *)PureSurface9_LockRect,
    (void *)PureSurface9_UnlockRect,
    (void *)PureSurface9_GetDC,
    (void *)PureSurface9_ReleaseDC
};

static HRESULT NINE_WINAPI
PureAuthenticatedChannel9_GetCertificateSize( struct NineAuthenticatedChannel9 *This,
                                              UINT *pCertificateSize )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureAuthenticatedChannel9_GetCertificate( struct NineAuthenticatedChannel9 *This,
                                          UINT CertifacteSize,
                                          BYTE *ppCertificate )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureAuthenticatedChannel9_NegotiateKeyExchange( struct NineAuthenticatedChannel9 *This,
                                                UINT DataSize,
                                                void *pData )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureAuthenticatedChannel9_Query( struct NineAuthenticatedChannel9 *This,
                                 UINT InputSize,
                                 const void *pInput,
                                 UINT OutputSize,
                                 void *pOutput )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureAuthenticatedChannel9_Configure( struct NineAuthenticatedChannel9 *This,
                                     UINT InputSize,
                                     const void *pInput,
                                     D3DAUTHENTICATEDCHANNEL_CONFIGURE_OUTPUT *pOutput )
{
    STUB(D3DERR_INVALIDCALL);
}

IDirect3DAuthenticatedChannel9Vtbl PureAuthenticatedChannel9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureAuthenticatedChannel9_GetCertificateSize,
    (void *)PureAuthenticatedChannel9_GetCertificate,
    (void *)PureAuthenticatedChannel9_NegotiateKeyExchange,
    (void *)PureAuthenticatedChannel9_Query,
    (void *)PureAuthenticatedChannel9_Configure
};

static void
PureBaseTexture9_SetLOD_rx( struct NineBaseTexture9 *This,
                       void *arg )
{
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;
    // XXX
    *(DWORD *)args->obj1 = NineBaseTexture9_SetLOD(This, args->arg1);
}

static DWORD NINE_WINAPI
PureBaseTexture9_SetLOD( struct NineBaseTexture9 *This,
                         DWORD LODNew )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;
    DWORD r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureBaseTexture9_SetLOD_rx;
    slot->this = (void *)This;

    args->arg1 = LODNew;
    args->obj1 = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureBaseTexture9_GetLOD_rx( struct NineBaseTexture9 *This,
                       void *arg )
{
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;
    // XXX
    *(DWORD *)args->obj1 = NineBaseTexture9_GetLOD(This);
}

static DWORD NINE_WINAPI
PureBaseTexture9_GetLOD( struct NineBaseTexture9 *This )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;
    DWORD r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureBaseTexture9_GetLOD_rx;
    slot->this = (void *)This;

    args->obj1 = &r;
    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureBaseTexture9_SetAutoGenFilterType_rx( struct NineBaseTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineBaseTexture9_SetAutoGenFilterType(This, args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureBaseTexture9_SetAutoGenFilterType( struct NineBaseTexture9 *This,
                                       D3DTEXTUREFILTERTYPE FilterType )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureBaseTexture9_SetAutoGenFilterType_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    args->arg1 = FilterType;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureBaseTexture9_GetAutoGenFilterType_rx( struct NineBaseTexture9 *This,
                       void *arg )
{
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    *(D3DTEXTUREFILTERTYPE *)args->obj1 = NineBaseTexture9_GetAutoGenFilterType(This);
}

static D3DTEXTUREFILTERTYPE NINE_WINAPI
PureBaseTexture9_GetAutoGenFilterType( struct NineBaseTexture9 *This )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;
    D3DTEXTUREFILTERTYPE r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureBaseTexture9_GetAutoGenFilterType_rx;
    slot->this = (void *)This;

    args->obj1 = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureBaseTexture9_PreLoad_rx( struct NineBaseTexture9 *This,
                       void *arg )
{
    (void) arg;
    NineBaseTexture9_PreLoad(This);
}

static void NINE_WINAPI
PureBaseTexture9_PreLoad( struct NineBaseTexture9 *This )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureBaseTexture9_PreLoad_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);
}

static void
PureBaseTexture9_GenerateMipSubLevels_rx( struct NineBaseTexture9 *This,
                       void *arg )
{
    (void) arg;
    NineBaseTexture9_GenerateMipSubLevels(This);
}

static void NINE_WINAPI
PureBaseTexture9_GenerateMipSubLevels( struct NineBaseTexture9 *This )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureBaseTexture9_GenerateMipSubLevels_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_GetCertificateSize( struct NineCryptoSession9 *This,
                                       UINT *pCertificateSize )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_GetCertificate( struct NineCryptoSession9 *This,
                                   UINT CertifacteSize,
                                   BYTE *ppCertificate )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_NegotiateKeyExchange( struct NineCryptoSession9 *This,
                                         UINT DataSize,
                                         void *pData )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_EncryptionBlt( struct NineCryptoSession9 *This,
                                  IDirect3DSurface9 *pSrcSurface,
                                  IDirect3DSurface9 *pDstSurface,
                                  UINT DstSurfaceSize,
                                  void *pIV )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_DecryptionBlt( struct NineCryptoSession9 *This,
                                  IDirect3DSurface9 *pSrcSurface,
                                  IDirect3DSurface9 *pDstSurface,
                                  UINT SrcSurfaceSize,
                                  D3DENCRYPTED_BLOCK_INFO *pEncryptedBPureInfo,
                                  void *pContentKey,
                                  void *pIV )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_GetSurfacePitch( struct NineCryptoSession9 *This,
                                    IDirect3DSurface9 *pSrcSurface,
                                    UINT *pSurfacePitch )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_StartSessionKeyRefresh( struct NineCryptoSession9 *This,
                                           void *pRandomNumber,
                                           UINT RandomNumberSize )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_FinishSessionKeyRefresh( struct NineCryptoSession9 *This )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureCryptoSession9_GetEncryptionBltKey( struct NineCryptoSession9 *This,
                                        void *pReadbackKey,
                                        UINT KeySize )
{
    STUB(D3DERR_INVALIDCALL);
}

IDirect3DCryptoSession9Vtbl PureCryptoSession9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureCryptoSession9_GetCertificateSize,
    (void *)PureCryptoSession9_GetCertificate,
    (void *)PureCryptoSession9_NegotiateKeyExchange,
    (void *)PureCryptoSession9_EncryptionBlt,
    (void *)PureCryptoSession9_DecryptionBlt,
    (void *)PureCryptoSession9_GetSurfacePitch,
    (void *)PureCryptoSession9_StartSessionKeyRefresh,
    (void *)PureCryptoSession9_FinishSessionKeyRefresh,
    (void *)PureCryptoSession9_GetEncryptionBltKey
};

static void
PureCubeTexture9_LockRect_rx( struct NineCubeTexture9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineCubeTexture9_LockRect(This,
            args->arg1,
            args->arg1_u,
            (D3DLOCKED_RECT *)args->obj2,
            (const RECT *)args->obj3,
            args->arg2);
}

static HRESULT NINE_WINAPI
PureCubeTexture9_LockRect( struct NineCubeTexture9 *This,
                           D3DCUBEMAP_FACES FaceType,
                           UINT Level,
                           D3DLOCKED_RECT *pPureedRect,
                           const RECT *pRect,
                           DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureCubeTexture9_LockRect_rx;
    slot->this = (void *)This;

    args->arg1_u = Level;
    args->arg1 = FaceType;
    args->arg2 = Flags;
    args->obj2 = (void *)pPureedRect;
    args->obj3 = (void *)pRect;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureCubeTexture9_UnlockRect_rx( struct NineCubeTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword_uint_void_box_args *args =
            (struct csmt_dword_uint_void_box_args *)arg;

    r = NineCubeTexture9_UnlockRect(This,
            args->arg1,
            args->arg1_u);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureCubeTexture9_UnlockRect( struct NineCubeTexture9 *This,
                             D3DCUBEMAP_FACES FaceType,
                             UINT Level )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_uint_void_box_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_uint_void_box_args), (void **)&args);
    slot->data = args;
    slot->func = PureCubeTexture9_UnlockRect_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    args->arg1_u = Level;
    args->arg1 = FaceType;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureCubeTexture9_AddDirtyRect_rx( struct NineCubeTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineCubeTexture9_AddDirtyRect(This,
            args->arg1,
            args->rect1);

    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureCubeTexture9_AddDirtyRect( struct NineCubeTexture9 *This,
                               D3DCUBEMAP_FACES FaceType,
                               const RECT *pDirtyRect )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureCubeTexture9_AddDirtyRect_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    if (pDirtyRect) {
        args->rect1 = &args->_rect1;
        args->_rect1 = *pDirtyRect;
    } else {
        args->rect1 = NULL;
    }
    args->arg1 = FaceType;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DCubeTexture9Vtbl PureCubeTexture9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureResource9_SetPriority,
    (void *)PureResource9_GetPriority,
    (void *)PureBaseTexture9_PreLoad,
    (void *)NineResource9_GetType, /* immutable */
    (void *)PureBaseTexture9_SetLOD,
    (void *)PureBaseTexture9_GetLOD,
    (void *)NineBaseTexture9_GetLevelCount, /* immutable */
    (void *)PureBaseTexture9_SetAutoGenFilterType,
    (void *)PureBaseTexture9_GetAutoGenFilterType,
    (void *)PureBaseTexture9_GenerateMipSubLevels,
    (void *)NineCubeTexture9_GetLevelDesc, /* immutable */
    (void *)NineCubeTexture9_GetCubeMapSurface, /* AddRef */
    (void *)PureCubeTexture9_LockRect,
    (void *)PureCubeTexture9_UnlockRect,
    (void *)PureCubeTexture9_AddDirtyRect
};

static HRESULT NINE_WINAPI
PureDevice9Video_GetContentProtectionCaps( struct NineDevice9Video *This,
                                           const GUID *pCryptoType,
                                           const GUID *pDecodeProfile,
                                           D3DCONTENTPROTECTIONCAPS *pCaps )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Video_CreateAuthenticatedChannel( struct NineDevice9Video *This,
                                             D3DAUTHENTICATEDCHANNELTYPE ChannelType,
                                             IDirect3DAuthenticatedChannel9 **ppAuthenticatedChannel,
                                             HANDLE *pChannelHandle )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureDevice9Video_CreateCryptoSession( struct NineDevice9Video *This,
                                      const GUID *pCryptoType,
                                      const GUID *pDecodeProfile,
                                      IDirect3DCryptoSession9 **ppCryptoSession,
                                      HANDLE *pCryptoHandle )
{
    STUB(D3DERR_INVALIDCALL);
}

IDirect3DDevice9VideoVtbl PureDevice9Video_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureDevice9Video_GetContentProtectionCaps,
    (void *)PureDevice9Video_CreateAuthenticatedChannel,
    (void *)PureDevice9Video_CreateCryptoSession
};

static void
PureIndexBuffer9_Lock_rx( struct NineIndexBuffer9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineIndexBuffer9_Lock(This,
            args->arg1_u,
            args->arg2_u,
            (void **)args->obj2,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureIndexBuffer9_Lock( struct NineIndexBuffer9 *This,
                       UINT OffsetToPure,
                       UINT SizeToPure,
                       void **ppbData,
                       DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureIndexBuffer9_Lock_rx;
    slot->this = This;
    nine_bind(&slot->this, This);

    args->arg1_u = OffsetToPure;
    args->arg2_u = SizeToPure;
    args->obj2 = (void *)ppbData;
    args->arg1 = Flags;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureIndexBuffer9_Unlock_rx( struct NineIndexBuffer9 *This,
                       void *arg )
{
    HRESULT r;
    (void) arg;

    r = NineIndexBuffer9_Unlock(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureIndexBuffer9_Unlock( struct NineIndexBuffer9 *This )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureIndexBuffer9_Unlock_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DIndexBuffer9Vtbl PureIndexBuffer9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureResource9_SetPriority,
    (void *)PureResource9_GetPriority,
    (void *)NineResource9_PreLoad, /* nop */
    (void *)NineResource9_GetType, /* immutable */
    (void *)PureIndexBuffer9_Lock,
    (void *)PureIndexBuffer9_Unlock,
    (void *)NineIndexBuffer9_GetDesc /* immutable */
};

IDirect3DPixelShader9Vtbl PurePixelShader9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice,
    (void *)NinePixelShader9_GetFunction
};

static void
PureQuery9_Issue_rx( struct NineQuery9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword_void_args *args =
            (struct csmt_dword_void_args *)arg;

    r = NineQuery9_Issue(This, args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureQuery9_Issue( struct NineQuery9 *This,
                  DWORD dwIssueFlags )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureQuery9_Issue_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    args->arg1 = dwIssueFlags;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureQuery9_GetData_rx( struct NineDevice9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineQuery9_GetData((struct NineQuery9 *)args->obj1,
            args->obj2,
            args->arg1,
            args->arg2);
}

static HRESULT NINE_WINAPI
PureQuery9_GetData( struct NineQuery9 *This,
                    void *pData,
                    DWORD dwSize,
                    DWORD dwGetDataFlags )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureQuery9_GetData_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)This;
    args->obj2 = pData;
    args->arg1 = dwSize;
    args->arg2 = dwGetDataFlags;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

IDirect3DQuery9Vtbl PureQuery9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Query9 iface */
    (void *)NineQuery9_GetType, /* immutable */
    (void *)NineQuery9_GetDataSize, /* immutable */
    (void *)PureQuery9_Issue,
    (void *)PureQuery9_GetData
};

static void
PureStateBlock9_Capture_rx( struct NineStateBlock9 *This,
                       void *arg )
{
    HRESULT r;

    r = NineStateBlock9_Capture(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureStateBlock9_Capture( struct NineStateBlock9 *This )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureStateBlock9_Capture_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureStateBlock9_Apply_rx( struct NineStateBlock9 *This,
                       void *arg )
{
    HRESULT r;

    r = NineStateBlock9_Apply(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureStateBlock9_Apply( struct NineStateBlock9 *This )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureStateBlock9_Apply_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DStateBlock9Vtbl PureStateBlock9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of StateBlock9 iface */
    (void *)PureStateBlock9_Capture,
    (void *)PureStateBlock9_Apply
};

static void
PureSwapChain9_Present_rx( struct NineSwapChain9 *This,
                       void *arg )
{
    struct csmt_dword3_void4_result_args *args =
            (struct csmt_dword3_void4_result_args *)arg;

    *args->result = NineSwapChain9_Present(This,
            (const RECT *)args->obj1,
            (const RECT *)args->obj2,
            (HWND)args->obj3,
            (const RGNDATA *)args->obj4,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureSwapChain9_Present( struct NineSwapChain9 *This,
                        const RECT *pSourceRect,
                        const RECT *pDestRect,
                        HWND hDestWindowOverride,
                        const RGNDATA *pDirtyRegion,
                        DWORD dwFlags )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSwapChain9_Present_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pSourceRect;
    args->obj2 = (void *)pDestRect;
    args->obj3 = (void *)hDestWindowOverride;
    args->obj4 = (void *)pDirtyRegion;
    args->arg1 = dwFlags;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSwapChain9_GetFrontBufferData_rx( struct NineSwapChain9 *This,
                       void *arg )
{
    struct csmt_dword1_void1_result_args *args =
            (struct csmt_dword1_void1_result_args *)arg;

    *args->result = NineSwapChain9_GetFrontBufferData(This,
            (IDirect3DSurface9 *)args->obj1);
}

static HRESULT NINE_WINAPI
PureSwapChain9_GetFrontBufferData( struct NineSwapChain9 *This,
                                   IDirect3DSurface9 *pDestSurface )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword1_void1_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword1_void1_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSwapChain9_GetFrontBufferData_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pDestSurface;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSwapChain9_GetBackBuffer_rx( struct NineSwapChain9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineSwapChain9_GetBackBuffer(This,
            args->arg1_u,
            args->arg1,
            (IDirect3DSurface9 **)args->obj1);
}

static HRESULT NINE_WINAPI
PureSwapChain9_GetBackBuffer( struct NineSwapChain9 *This,
                              UINT iBackBuffer,
                              D3DBACKBUFFER_TYPE Type,
                              IDirect3DSurface9 **ppBackBuffer )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSwapChain9_GetBackBuffer_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)ppBackBuffer;
    args->arg1 = Type;
    args->arg1_u = iBackBuffer;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSwapChain9_GetRasterStatus_rx( struct NineSwapChain9 *This,
                       void *arg )
{
    struct csmt_dword1_void1_result_args *args =
            (struct csmt_dword1_void1_result_args *)arg;

    *args->result = NineSwapChain9_GetRasterStatus(This,
            (D3DRASTER_STATUS *)args->obj1);
}

static HRESULT NINE_WINAPI
PureSwapChain9_GetRasterStatus( struct NineSwapChain9 *This,
                                D3DRASTER_STATUS *pRasterStatus )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword1_void1_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword1_void1_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSwapChain9_GetRasterStatus_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pRasterStatus;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSwapChain9_GetDisplayMode_rx( struct NineSwapChain9 *This,
                       void *arg )
{
    struct csmt_dword1_void1_result_args *args =
            (struct csmt_dword1_void1_result_args *)arg;

    *args->result = NineSwapChain9_GetDisplayMode(This,
            (D3DDISPLAYMODE *)args->obj1);
}

static HRESULT NINE_WINAPI
PureSwapChain9_GetDisplayMode( struct NineSwapChain9 *This,
                               D3DDISPLAYMODE *pMode )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword1_void1_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword1_void1_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSwapChain9_GetDisplayMode_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pMode;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureSwapChain9_GetPresentParameters_rx( struct NineSwapChain9 *This,
                       void *arg )
{
    struct csmt_dword1_void1_result_args *args =
            (struct csmt_dword1_void1_result_args *)arg;

    *args->result = NineSwapChain9_GetPresentParameters(This,
            (D3DPRESENT_PARAMETERS *)args->obj1);
}

static HRESULT NINE_WINAPI
PureSwapChain9_GetPresentParameters( struct NineSwapChain9 *This,
                                     D3DPRESENT_PARAMETERS *pPresentationParameters )
{
    struct csmt_context *ctx = This->base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword1_void1_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword1_void1_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureSwapChain9_GetPresentParameters_rx;
    slot->this = (void *)This;

    args->obj1 = (void *)pPresentationParameters;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

IDirect3DSwapChain9Vtbl PureSwapChain9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureSwapChain9_Present,
    (void *)PureSwapChain9_GetFrontBufferData,
    (void *)PureSwapChain9_GetBackBuffer,
    (void *)PureSwapChain9_GetRasterStatus,
    (void *)PureSwapChain9_GetDisplayMode,
    (void *)NineUnknown_GetDevice, /* actually part of SwapChain9 iface */
    (void *)PureSwapChain9_GetPresentParameters
};

static HRESULT NINE_WINAPI
PureSwapChain9Ex_GetLastPresentCount( struct NineSwapChain9Ex *This,
                                      UINT *pLastPresentCount )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureSwapChain9Ex_GetPresentStats( struct NineSwapChain9Ex *This,
                                  D3DPRESENTSTATS *pPresentationStatistics )
{
    STUB(D3DERR_INVALIDCALL);
}

static HRESULT NINE_WINAPI
PureSwapChain9Ex_GetDisplayModeEx( struct NineSwapChain9Ex *This,
                                   D3DDISPLAYMODEEX *pMode,
                                   D3DDISPLAYROTATION *pRotation )
{
    HRESULT r;
    ERR("called\n");

    pipe_mutex_lock(d3d_csmt_global);
    r = NineSwapChain9Ex_GetDisplayModeEx(This, pMode, pRotation);
    pipe_mutex_unlock(d3d_csmt_global);
    return r;
}

IDirect3DSwapChain9ExVtbl PureSwapChain9Ex_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)PureSwapChain9_Present,
    (void *)PureSwapChain9_GetFrontBufferData,
    (void *)PureSwapChain9_GetBackBuffer,
    (void *)PureSwapChain9_GetRasterStatus,
    (void *)PureSwapChain9_GetDisplayMode,
    (void *)NineUnknown_GetDevice, /* actually part of NineSwapChain9 iface */
    (void *)PureSwapChain9_GetPresentParameters,
    (void *)PureSwapChain9Ex_GetLastPresentCount,
    (void *)PureSwapChain9Ex_GetPresentStats,
    (void *)PureSwapChain9Ex_GetDisplayModeEx
};

static void
PureTexture9_LockRect_rx( struct NineTexture9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineTexture9_LockRect(This,
            args->arg1_u,
            (D3DLOCKED_RECT *)args->obj2,
            (const RECT *)args->obj3,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureTexture9_LockRect( struct NineTexture9 *This,
                       UINT Level,
                       D3DLOCKED_RECT *pPureedRect,
                       const RECT *pRect,
                       DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureTexture9_LockRect_rx;
    slot->this = (void *)This;

    args->obj2 = (void *)pPureedRect;
    args->obj3 = (void *)pRect;
    args->arg1 = Flags;
    args->arg1_u = Level;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureTexture9_UnlockRect_rx( struct NineTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_uint3_void_args *args =
            (struct csmt_uint3_void_args *)arg;

    r = NineTexture9_UnlockRect(This, args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureTexture9_UnlockRect( struct NineTexture9 *This,
                         UINT Level )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_uint3_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint3_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureTexture9_UnlockRect_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    args->arg1 = Level;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureTexture9_AddDirtyRect_rx( struct NineTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword_void2_rect2_point_args *args =
            (struct csmt_dword_void2_rect2_point_args *)arg;

    r = NineTexture9_AddDirtyRect(This, args->rect1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureTexture9_AddDirtyRect( struct NineTexture9 *This,
                           const RECT *pDirtyRect )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_void2_rect2_point_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_void2_rect2_point_args), (void **)&args);
    slot->data = args;
    slot->func = PureTexture9_AddDirtyRect_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    if (pDirtyRect) {
        args->rect1 = &args->_rect1;
        args->_rect1 = *pDirtyRect;
    } else {
        args->rect1 = NULL;
    }

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DTexture9Vtbl PureTexture9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureResource9_SetPriority,
    (void *)PureResource9_GetPriority,
    (void *)PureBaseTexture9_PreLoad,
    (void *)NineResource9_GetType, /* immutable */
    (void *)PureBaseTexture9_SetLOD,
    (void *)PureBaseTexture9_GetLOD,
    (void *)NineBaseTexture9_GetLevelCount, /* immutable */
    (void *)PureBaseTexture9_SetAutoGenFilterType,
    (void *)PureBaseTexture9_GetAutoGenFilterType,
    (void *)PureBaseTexture9_GenerateMipSubLevels,
    (void *)NineTexture9_GetLevelDesc, /* immutable */
    (void *)NineTexture9_GetSurfaceLevel, /* AddRef */
    (void *)PureTexture9_LockRect,
    (void *)PureTexture9_UnlockRect,
    (void *)PureTexture9_AddDirtyRect
};

static void
PureVertexBuffer9_Lock_rx( struct NineVertexBuffer9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineVertexBuffer9_Lock(This,
            args->arg1_u,
            args->arg2_u,
            (void **)args->obj2,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureVertexBuffer9_Lock( struct NineVertexBuffer9 *This,
                        UINT OffsetToPure,
                        UINT SizeToPure,
                        void **ppbData,
                        DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureVertexBuffer9_Lock_rx;
    slot->this = (void *)This;

    args->arg1_u = OffsetToPure;
    args->arg2_u = SizeToPure;
    args->obj2 = (void *)ppbData;
    args->arg1 = Flags;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureVertexBuffer9_Unlock_rx( struct NineVertexBuffer9 *This,
                       void *arg )
{
    HRESULT r;

    r = NineVertexBuffer9_Unlock(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureVertexBuffer9_Unlock( struct NineVertexBuffer9 *This )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureVertexBuffer9_Unlock_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DVertexBuffer9Vtbl PureVertexBuffer9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureResource9_SetPriority,
    (void *)PureResource9_GetPriority,
    (void *)NineResource9_PreLoad, /* nop */
    (void *)NineResource9_GetType, /* immutable */
    (void *)PureVertexBuffer9_Lock,
    (void *)PureVertexBuffer9_Unlock,
    (void *)NineVertexBuffer9_GetDesc /* immutable */
};

IDirect3DVertexDeclaration9Vtbl PureVertexDeclaration9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of VertexDecl9 iface */
    (void *)NineVertexDeclaration9_GetDeclaration
};

IDirect3DVertexShader9Vtbl PureVertexShader9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice,
    (void *)NineVertexShader9_GetFunction
};

static void
PureVolume9_GetContainer_rx( struct NineVolume9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineVolume9_GetContainer(This,
            (REFIID)args->obj2,
            (void **)args->obj3);
}

static HRESULT NINE_WINAPI
PureVolume9_GetContainer( struct NineVolume9 *This,
                          REFIID riid,
                          void **ppContainer )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureVolume9_GetContainer_rx;
    slot->this = (void *)This;

    args->obj2 = (void *)riid;
    args->obj3 = (void *)ppContainer;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureVolume9_LockBox_rx( struct NineVolume9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineVolume9_LockBox(This,
            (D3DLOCKED_BOX *)args->obj2,
            (const D3DBOX *)args->obj3,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureVolume9_LockBox( struct NineVolume9 *This,
                     D3DLOCKED_BOX *pPureedVolume,
                     const D3DBOX *pBox,
                     DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureVolume9_LockBox_rx;
    slot->this = (void *)This;

    args->arg1 = Flags;
    args->obj2 = (void *)pPureedVolume;
    args->obj3 = (void *)pBox;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureVolume9_UnlockBox_rx( struct NineVolume9 *This,
                       void *arg )
{
    HRESULT r;

    r = NineVolume9_UnlockBox(This);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureVolume9_UnlockBox( struct NineVolume9 *This )
{
    struct csmt_context *ctx = This->base.base.device->csmt_context;
    struct queue_element* slot;

    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = PureVolume9_UnlockBox_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DVolume9Vtbl PureVolume9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Volume9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureVolume9_GetContainer,
    (void *)NineVolume9_GetDesc, /* immutable */
    (void *)PureVolume9_LockBox,
    (void *)PureVolume9_UnlockBox
};

static void
PureVolumeTexture9_LockBox_rx( struct NineVolumeTexture9 *This,
                       void *arg )
{
    struct csmt_dword3_void3_uint4_result_args *args =
            (struct csmt_dword3_void3_uint4_result_args *)arg;

    *args->result = NineVolumeTexture9_LockBox(This,
            args->arg1_u,
            (D3DLOCKED_BOX *)args->obj2,
            (const D3DBOX *)args->obj3,
            args->arg1);
}

static HRESULT NINE_WINAPI
PureVolumeTexture9_LockBox( struct NineVolumeTexture9 *This,
                            UINT Level,
                            D3DLOCKED_BOX *pPureedVolume,
                            const D3DBOX *pBox,
                            DWORD Flags )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword3_void3_uint4_result_args *args;
    HRESULT r;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword3_void3_uint4_result_args), (void **)&args);
    slot->data = args;
    slot->func = PureVolumeTexture9_LockBox_rx;
    slot->this = (void *)This;

    args->arg1_u = Level;
    args->arg1 = Flags;
    args->obj2 = (void *)pPureedVolume;
    args->obj3 = (void *)pBox;
    args->result = &r;

    queue_set_slot_ready_and_wait(ctx->queue, slot);

    return r;
}

static void
PureVolumeTexture9_UnlockBox_rx( struct NineVolumeTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_uint3_void_args *args =
            (struct csmt_uint3_void_args *)arg;

    r = NineVolumeTexture9_UnlockBox(This, args->arg1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureVolumeTexture9_UnlockBox( struct NineVolumeTexture9 *This,
                              UINT Level )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_uint3_void_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_uint3_void_args), (void **)&args);
    slot->data = args;
    slot->func = PureVolumeTexture9_UnlockBox_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    args->arg1 = Level;

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

static void
PureVolumeTexture9_AddDirtyBox_rx( struct NineVolumeTexture9 *This,
                       void *arg )
{
    HRESULT r;
    struct csmt_dword_uint_void_box_args *args =
            (struct csmt_dword_uint_void_box_args *)arg;

    r = NineVolumeTexture9_AddDirtyBox(This, args->box1);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
    nine_bind(&This, NULL);
}

static HRESULT NINE_WINAPI
PureVolumeTexture9_AddDirtyBox( struct NineVolumeTexture9 *This,
                                const D3DBOX *pDirtyBox )
{
    struct csmt_context *ctx = This->base.base.base.device->csmt_context;
    struct queue_element* slot;
    struct csmt_dword_uint_void_box_args *args;

    slot = queue_get_free_slot(ctx->queue, sizeof(struct csmt_dword_uint_void_box_args), (void **)&args);
    slot->data = args;
    slot->func = PureVolumeTexture9_AddDirtyBox_rx;
    slot->this = NULL;
    nine_bind(&slot->this, This);

    if (pDirtyBox) {
        args->box1 = &args->_box1;
        args->_box1 = *pDirtyBox;
    } else {
        args->box1 = NULL;
    }

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

IDirect3DVolumeTexture9Vtbl PureVolumeTexture9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of Resource9 iface */
    (void *)PureResource9_SetPrivateData,
    (void *)PureResource9_GetPrivateData,
    (void *)PureResource9_FreePrivateData,
    (void *)PureResource9_SetPriority,
    (void *)PureResource9_GetPriority,
    (void *)PureBaseTexture9_PreLoad,
    (void *)NineResource9_GetType, /* immutable */
    (void *)PureBaseTexture9_SetLOD,
    (void *)PureBaseTexture9_GetLOD,
    (void *)NineBaseTexture9_GetLevelCount, /* immutable */
    (void *)PureBaseTexture9_SetAutoGenFilterType,
    (void *)PureBaseTexture9_GetAutoGenFilterType,
    (void *)PureBaseTexture9_GenerateMipSubLevels,
    (void *)NineVolumeTexture9_GetLevelDesc, /* immutable */
    (void *)NineVolumeTexture9_GetVolumeLevel, /* AddRef */
    (void *)PureVolumeTexture9_LockBox,
    (void *)PureVolumeTexture9_UnlockBox,
    (void *)PureVolumeTexture9_AddDirtyBox
};

ID3DAdapter9Vtbl PureAdapter9_vtable = { /* not used */
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL,
    (void *)NULL
};

/* CSMT functions */
static int nine_csmt_worker( void *arg ) {
    struct csmt_context *ctx = arg;
    struct queue_element *slot;
    int i;
    DBG("csmt worker spawned\n");

    while (!ctx->terminate) {
        slot = queue_wait_slot_ready(ctx->queue);

        /* decode */
        slot->func(slot->this, slot->data);

        queue_set_slot_processed(ctx->queue, slot);
    }
    nine_concurrent_queue_delete(ctx->queue);

    for (i = 0; i < NINE_MAX_SIMULTANEOUS_RENDERTARGETS; i++) {
        nine_bind(&ctx->rt[i], NULL);
    }
    nine_bind(&ctx->ds, NULL);
    u_upload_destroy(ctx->vertex_uploader);
    u_upload_destroy(ctx->index_uploader);

    FREE(ctx);
    DBG("csmt worker destroyed\n");
    return 0;
}

struct csmt_context *nine_csmt_create( struct NineDevice9 *This ) {
    struct csmt_context *ctx;

    ctx = CALLOC_STRUCT(csmt_context);
    if (!ctx)
        return NULL;

    ctx->vertex_uploader = u_upload_create(This->pipe, 1024 * 128,
                                            PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_STREAM);
    if (!ctx->vertex_uploader) {
        FREE(ctx);
        return NULL;
    }

    ctx->index_uploader = u_upload_create(This->pipe, 128 * 1024,
                                           PIPE_BIND_INDEX_BUFFER, PIPE_USAGE_STREAM);
    if (!ctx->index_uploader) {
        u_upload_destroy(This->vertex_uploader);
        FREE(ctx);
        return NULL;
    }

    ctx->queue = nine_concurrent_queue_create();
    if (!ctx->queue) {
        u_upload_destroy(This->index_uploader);
        u_upload_destroy(This->vertex_uploader);
        FREE(ctx);
        return NULL;
    }

#ifndef DEBUG_SINGLETHREAD
    if (This->minor_version_num <= 1) {
        ERR("Presentation Interface 1.2 required\n");
    }
    ctx->render_thread = NineSwapChain9_CreateThread(This->swapchains[0], nine_csmt_worker, ctx);
    if (!ctx->render_thread) {
        u_upload_destroy(This->index_uploader);
        u_upload_destroy(This->vertex_uploader);
        nine_concurrent_queue_delete(ctx->queue);
        FREE(ctx);
        return NULL;
    }
#endif

    DBG("Returning context %p\n", ctx);

    return ctx;
}

void nine_csmt_reset( struct NineDevice9 *This ) {
    int i;
    struct csmt_context *ctx = This->csmt_context;

    /* Reset internal state */
    nine_bind(&ctx->rt[0], (IDirect3DSurface9 *)This->swapchains[0]->buffers[0]);
    if (This->state.rs[D3DRS_ZENABLE])
        nine_bind(&ctx->ds, (IDirect3DSurface9 *)This->swapchains[0]->zsbuf);

    for (i = 1; i < NINE_MAX_SIMULTANEOUS_RENDERTARGETS; i++) {
        nine_bind(&ctx->rt[i], NULL);
    }
}

void nine_csmt_destroy( struct NineDevice9 *This, struct csmt_context *ctx ) {
    struct queue_element* slot;
    HANDLE render_thread = ctx->render_thread;

    /* NOP */
    slot = queue_get_free_slot(ctx->queue, 0, NULL);
    slot->data = NULL;
    slot->func = queue_get_nop();

    ctx->terminate = TRUE;

    queue_set_slot_ready(ctx->queue, slot);
    /* wake worker thread */
    queue_wake(ctx->queue);

    NineSwapChain9_WaitForThread(This->swapchains[0], render_thread);
}

