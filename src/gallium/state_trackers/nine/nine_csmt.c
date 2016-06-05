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
#include "nine_pipe.h"

#include "nine_queue.h"
#include "nine_csmt.h"
#include "nine_csmt_structs.h"
#include "nine_csmt_helper.h"

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

// Resource functions

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

// functions to serialize

/* HOWTO
 *
 */
CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, EvictManagedResources,,,)

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, Reset,,nine_csmt_reset(This);,
					 HRESULT,
					 ARG_REF(D3DPRESENT_PARAMETERS, pPresentationParameters) )

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, Present,,,
                     HRESULT,
					 ARG_REF(const RECT, pSourceRect),
					 ARG_REF(const RECT, pDestRect),
					 ARG_VAL(HWND, hDestWindowOverride),
					 ARG_REF(const RGNDATA, pDirtyRegion))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, UpdateSurface,,,
						 ARG_BIND_REF(IDirect3DSurface9, pSourceSurface),
						 ARG_COPY_REF(RECT, pSourceRect),
						 ARG_BIND_REF(IDirect3DSurface9, pDestinationSurface),
						 ARG_COPY_REF(POINT, pDestPoint))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, UpdateTexture,,,
						 ARG_BIND_REF(IDirect3DBaseTexture9, pSourceTexture),
						 ARG_BIND_REF(IDirect3DBaseTexture9, pDestinationTexture))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, GetRenderTargetData,,,
						 ARG_BIND_REF(IDirect3DSurface9, pRenderTarget),
						 ARG_BIND_REF(IDirect3DSurface9, pDestSurface))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetFrontBufferData,,,
					 HRESULT,
					 ARG_VAL(UINT, iSwapChain),
					 ARG_REF(IDirect3DSurface9, pDestSurface))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, StretchRect,,,
						 ARG_BIND_REF(IDirect3DSurface9, pSourceSurface),
						 ARG_COPY_REF(RECT, pSourceRect),
						 ARG_BIND_REF(IDirect3DSurface9, pDestSurface),
						 ARG_COPY_REF(RECT, pDestRect),
						 ARG_VAL(D3DTEXTUREFILTERTYPE, Filter))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, ColorFill,,,
						 ARG_BIND_REF(IDirect3DSurface9, pSurface),
						 ARG_COPY_REF(RECT, pRect),
						 ARG_VAL(D3DCOLOR, color))

#if 0
CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetRenderTarget,,,
						 ARG_VAL(DWORD, RenderTargetIndex),
						 ARG_BIND_REF(IDirect3DSurface9, pRenderTarget))
#endif
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
#if 0
/* Available on PURE devices */
CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetRenderTarget,,,
					 HRESULT,
					 ARG_VAL(DWORD, RenderTargetIndex),
					 ARG_REF(IDirect3DSurface9*, ppRenderTarget))

#endif
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

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, BeginScene,,,)

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, EndScene,,,)

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, Clear,,,
						 ARG_VAL(DWORD, Count),
						 ARG_COPY_REF(D3DRECT, pRects),
						 ARG_VAL(DWORD, Flags),
						 ARG_VAL(D3DCOLOR, Color),
						 ARG_VAL(float, Z),
						 ARG_VAL(DWORD, Stencil))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetTransform,,,
						 ARG_VAL(D3DTRANSFORMSTATETYPE, State),
						 ARG_COPY_REF(D3DMATRIX, pMatrix))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, MultiplyTransform,,,
						 ARG_VAL(D3DTRANSFORMSTATETYPE, State),
						 ARG_COPY_REF(D3DMATRIX, pMatrix))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetViewport,,,
						 ARG_COPY_REF(D3DVIEWPORT9, pViewport))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetMaterial,,,
						 ARG_COPY_REF(D3DMATERIAL9, pMaterial))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetLight,,,
						 ARG_VAL(DWORD, Index),
						 ARG_COPY_REF(D3DLIGHT9, pLight))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, LightEnable,,,
						ARG_VAL(DWORD, Index),
						ARG_VAL(BOOL, Enable))

struct s_Device9_SetClipPlane_private {
    DWORD Index;
    float plane[4];
};

static void
PureDevice9_SetClipPlane_rx( struct NineDevice9 *This,
                            void *arg )
{
    HRESULT r;
    struct s_Device9_SetClipPlane_private *args =
            (struct s_Device9_SetClipPlane_private *)arg;

    r = NineDevice9_SetClipPlane(This, args->Index, args->plane);
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
    struct s_Device9_SetClipPlane_private *args;

    user_assert(pPlane, D3DERR_INVALIDCALL);

    slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetClipPlane_private), (void **)&args);
    slot->data = args;
    slot->func = PureDevice9_SetClipPlane_rx;
    slot->this = (void *)This;

    args->Index = Index;
    memcpy(args->plane, pPlane, sizeof(args->plane));

    queue_set_slot_ready(ctx->queue, slot);

    return D3D_OK;
}

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetRenderState,,,
						ARG_VAL(D3DRENDERSTATETYPE, State),
						ARG_VAL(DWORD, Value))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetTexture,,,
						ARG_VAL(DWORD, Stage),
						ARG_BIND_REF(IDirect3DBaseTexture9, pTexture))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetTextureStageState,,,
						ARG_VAL(DWORD, Stage),
						ARG_VAL(D3DTEXTURESTAGESTATETYPE, Type),
						ARG_VAL(DWORD, Value))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetSamplerState,,,
						ARG_VAL(DWORD, Sampler),
						ARG_VAL(D3DTEXTURESTAGESTATETYPE, Type),
						ARG_VAL(DWORD, Value))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetScissorRect,,,
						ARG_COPY_REF(RECT, pRect))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, DrawPrimitive,,,
						ARG_VAL(D3DPRIMITIVETYPE, PrimitiveType),
						ARG_VAL(UINT, StartVertex),
						ARG_VAL(UINT, PrimitiveCount))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, DrawIndexedPrimitive,,,
						ARG_VAL(D3DPRIMITIVETYPE, PrimitiveType),
						ARG_VAL(INT, BaseVertexIndex),
						ARG_VAL(UINT, MinVertexIndex),
						ARG_VAL(UINT, NumVertices),
						ARG_VAL(UINT, startIndex),
						ARG_VAL(UINT, primCount))

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

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetVertexDeclaration,,,
						ARG_BIND_REF(IDirect3DVertexDeclaration9, pDecl))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetFVF,,,
						ARG_VAL(DWORD, FVF))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetVertexShader,,,
						ARG_BIND_REF(IDirect3DVertexShader9, pDecl))

struct s_Device9_SetShaderConstantF {
	UINT _StartRegister;
	UINT _Vector4fCount;
	float _vec[4 * 8];
};

struct s_Device9_SetShaderConstantI {
	UINT _StartRegister;
	UINT _Vector4iCount;
	int _vec[4 * 256]; //XXX
};

struct s_Device9_SetShaderConstantB {
	UINT _StartRegister;
	UINT _BoolCount;
	BOOL _vec[256];
};

static void
PureDevice9_SetVertexShaderConstantF_rx( void *this, void *arg )
{
    HRESULT r;
    struct NineDevice9 *This = (struct NineDevice9 *)this;
    struct s_Device9_SetShaderConstantF *args =
            (struct s_Device9_SetShaderConstantF *)arg;

    r = NineDevice9_SetVertexShaderConstantF(This, args->_StartRegister, (const float *)&args->_vec, args->_Vector4fCount);
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
    struct s_Device9_SetShaderConstantF *args;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    if (Vector4fCount == 1) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantF), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetVertexShaderConstantF_rx;
        slot->this = (void *)This;

        args->_StartRegister = StartRegister;
        args->_Vector4fCount = 1;
        memcpy(&args->_vec, pConstantData, sizeof(float[4]));
        queue_set_slot_ready(ctx->queue, slot);
    } else {
        for (i = 0; i < Vector4fCount; i+=8) {
            slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantF), (void **)&args);
            slot->data = args;
            slot->func = PureDevice9_SetVertexShaderConstantF_rx;
            slot->this = (void *)This;

            args->_StartRegister = StartRegister + i;
            args->_Vector4fCount = MIN2(Vector4fCount - i, 8);
            memcpy(&args->_vec, &pConstantData[i * 4], sizeof(float[4]) * args->_Vector4fCount);

            queue_set_slot_ready(ctx->queue, slot);
        }
    }

    return D3D_OK;
}

static void
PureDevice9_SetVertexShaderConstantI_rx( void *this, void *arg )
{
    HRESULT r;
    struct NineDevice9 *This = (struct NineDevice9 *)this;
    struct s_Device9_SetShaderConstantI *args =
            (struct s_Device9_SetShaderConstantI *)arg;

    r = NineDevice9_SetPixelShaderConstantI(This, args->_StartRegister, (int *)&args->_vec, args->_Vector4iCount);
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
    struct s_Device9_SetShaderConstantI *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < Vector4iCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantI), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetVertexShaderConstantI_rx;
        slot->this = (void *)This;

        args->_StartRegister = StartRegister + i;
        args->_Vector4iCount = 1;
        memcpy(&args->_vec, &pConstantData[i * 4], sizeof(int[4]));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

static void
PureDevice9_SetVertexShaderConstantB_rx( void *this, void *arg )
{
    HRESULT r;
    struct NineDevice9 *This = (struct NineDevice9 *)this;
    struct s_Device9_SetShaderConstantB *args =
            (struct s_Device9_SetShaderConstantB *)arg;

    r = NineDevice9_SetVertexShaderConstantB(This, args->_StartRegister, (BOOL *)&args->_vec, args->_BoolCount);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetVertexShaderConstantB( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const BOOL *pConstantData,
                                     UINT BoolCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct s_Device9_SetShaderConstantB *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < BoolCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantB), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetVertexShaderConstantB_rx;
        slot->this = (void *)This;

        args->_StartRegister = StartRegister + i;
        args->_BoolCount = 1;
        memcpy(&args->_vec, &pConstantData[i], sizeof(BOOL));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetStreamSource,,,
						ARG_VAL(UINT, StreamNumber),
						ARG_BIND_REF(IDirect3DVertexBuffer9, pStreamData),
						ARG_VAL(UINT, OffsetInBytes),
						ARG_VAL(UINT, Stride))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetStreamSourceFreq,,,
						ARG_VAL(UINT, StreamNumber),
						ARG_VAL(UINT, Setting))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetIndices,,,
						ARG_BIND_REF(IDirect3DIndexBuffer9, pIndexData))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, SetPixelShader,,,
						ARG_BIND_REF(IDirect3DPixelShader9, pShader))

static void
PureDevice9_SetPixelShaderConstantF_rx( void *this, void *arg )
{
    HRESULT r;
    struct NineDevice9 *This = (struct NineDevice9 *)this;

    struct s_Device9_SetShaderConstantF *args =
            (struct s_Device9_SetShaderConstantF *)arg;

    r = NineDevice9_SetPixelShaderConstantF(This, args->_StartRegister, (const float *)&args->_vec, args->_Vector4fCount);
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
    struct s_Device9_SetShaderConstantF *args;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    if (Vector4fCount == 1) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantF), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetPixelShaderConstantF_rx;
        slot->this = (void *)This;

        args->_StartRegister = StartRegister;
        args->_Vector4fCount = 1;
        memcpy(&args->_vec, pConstantData, sizeof(float[4]));
        queue_set_slot_ready(ctx->queue, slot);
    } else {

        for (i = 0; i < Vector4fCount; i+=8) {
            slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantF), (void **)&args);
            slot->data = args;
            slot->func = PureDevice9_SetPixelShaderConstantF_rx;
            slot->this = (void *)This;

            args->_StartRegister = StartRegister + i;
            args->_Vector4fCount = MIN2(Vector4fCount - i, 8);
            memcpy(&args->_vec, &pConstantData[i * 4], sizeof(float[4]) * args->_Vector4fCount);

            queue_set_slot_ready(ctx->queue, slot);
        }
    }

    return D3D_OK;
}

static void
PureDevice9_SetPixelShaderConstantI_rx( void *this, void *arg )
{
    HRESULT r;
    struct NineDevice9 *This = (struct NineDevice9 *)this;

    struct s_Device9_SetShaderConstantI *args =
            (struct s_Device9_SetShaderConstantI *)arg;

    r = NineDevice9_SetPixelShaderConstantI(This, args->_StartRegister, (int *)&args->_vec, args->_Vector4iCount);
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
    struct s_Device9_SetShaderConstantI *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < Vector4iCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantI), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetPixelShaderConstantI_rx;
        slot->this = (void *)This;

        args->_StartRegister = StartRegister + i;
        args->_Vector4iCount = 1;
        memcpy(&args->_vec, &pConstantData[i * 4], sizeof(int[4]));

        queue_set_slot_ready(ctx->queue, slot);
    }

    return D3D_OK;
}

static void
PureDevice9_SetPixelShaderConstantB_rx( void *this, void *arg )
{
    HRESULT r;
    struct NineDevice9 *This = (struct NineDevice9 *)this;
    struct s_Device9_SetShaderConstantB *args =
            (struct s_Device9_SetShaderConstantB *)arg;

    r = NineDevice9_SetPixelShaderConstantB(This, args->_StartRegister, (BOOL *)&args->_vec, args->_BoolCount);
    if (r != D3D_OK)
        ERR("Failed with error %x\n", r);
}

static HRESULT NINE_WINAPI
PureDevice9_SetPixelShaderConstantB( struct NineDevice9 *This,
                                     UINT StartRegister,
                                     const BOOL *pConstantData,
                                     UINT BoolCount )
{
    struct csmt_context *ctx = This->csmt_context;
    struct queue_element* slot;
    struct s_Device9_SetShaderConstantB *args;
    int i;

    user_assert(pConstantData, D3DERR_INVALIDCALL);

    for (i = 0; i < BoolCount; i++) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_Device9_SetShaderConstantB), (void **)&args);
        slot->data = args;
        slot->func = PureDevice9_SetPixelShaderConstantB_rx;
        slot->this = (void *)This;

        args->_StartRegister = StartRegister + i;
        args->_BoolCount = 1;
        memcpy(&args->_vec, &pConstantData[i], sizeof(BOOL));

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

/* available on PURE devices */
CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetDirect3D,,,
					 HRESULT,
					 ARG_REF(IDirect3D9*, ppD3D9))

/* available on PURE devices */
CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetDisplayMode,,,
					 HRESULT,
					 ARG_VAL(UINT, iSwapChain),
					 ARG_REF(D3DDISPLAYMODE, pMode))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, SetCursorProperties,,,
				 HRESULT,
				 ARG_VAL(UINT, XHotSpot),
				 ARG_VAL(UINT, YHotSpot),
				 ARG_REF(IDirect3DSurface9, pCursorBitmap))

CREATE_FUNC_NON_BLOCKING_NO_RESULT(Device9, SetCursorPosition,,,
									ARG_VAL(int, X),
									ARG_VAL(int, Y),
									ARG_VAL(DWORD, Flags))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, ShowCursor,,,
				 HRESULT,
				 ARG_VAL(BOOL, bShow))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateAdditionalSwapChain,,,
					 HRESULT,
					 ARG_REF(D3DPRESENT_PARAMETERS, pPresentationParameters),
					 ARG_REF(IDirect3DSwapChain9*, pSwapChain))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetSwapChain,,,
					 HRESULT,
					 ARG_VAL(UINT, iSwapChain),
					 ARG_REF(IDirect3DSwapChain9*, pSwapChain))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetNumberOfSwapChains,,,
					 UINT)

/* Available on PURE devices */
CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, GetBackBuffer,,,
					 HRESULT,
					 ARG_VAL(UINT, iSwapChain),
					 ARG_VAL(UINT, iBackBuffer),
					 ARG_VAL(D3DBACKBUFFER_TYPE, Type),
					 ARG_REF(IDirect3DSurface9*, ppBackBuffer))


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

CREATE_FUNC_NON_BLOCKING_NO_RESULT(Device9, SetGammaRamp,,,
					 ARG_VAL(UINT, iSwapChain),
					 ARG_VAL(DWORD, Flags),
					 ARG_COPY_REF(D3DGAMMARAMP, pRamp))

CREATE_FUNC_BLOCKING_NO_RESULT(Device9, GetGammaRamp,,,
					 ARG_VAL(UINT, iSwapChain),
					 ARG_REF(D3DGAMMARAMP, pRamp))

//     user_assert(ppTexture, D3DERR_INVALIDCALL);

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateTexture,,,
					 HRESULT,
					 ARG_VAL(UINT, Width),
					 ARG_VAL(UINT, Height),
					 ARG_VAL(UINT, Levels),
					 ARG_VAL(DWORD, Usage),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DPOOL, Pool),
					 ARG_REF(IDirect3DTexture9*, ppTexture),
					 ARG_REF(HANDLE, pSharedHandle))

//     user_assert(ppVolumeTexture, D3DERR_INVALIDCALL);

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateVolumeTexture,,,
					 HRESULT,
					 ARG_VAL(UINT, Width),
					 ARG_VAL(UINT, Height),
					 ARG_VAL(UINT, Depth),
					 ARG_VAL(UINT, Levels),
					 ARG_VAL(DWORD, Usage),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DPOOL, Pool),
					 ARG_REF(IDirect3DVolumeTexture9*, ppVolumeTexture),
					 ARG_REF(HANDLE, pSharedHandle))

//    user_assert(ppCubeTexture, D3DERR_INVALIDCALL);

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateCubeTexture,,,
					 HRESULT,
					 ARG_VAL(UINT, EdgeLength),
					 ARG_VAL(UINT, Levels),
					 ARG_VAL(DWORD, Usage),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DPOOL, Pool),
					 ARG_REF(IDirect3DCubeTexture9*, ppCubeTexture),
					 ARG_REF(HANDLE, pSharedHandle))

 // user_assert(ppVertexBuffer, D3DERR_INVALIDCALL);

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateVertexBuffer,,,
					 HRESULT,
					 ARG_VAL(UINT, Length),
					 ARG_VAL(DWORD, Usage),
					 ARG_VAL(DWORD, FVF),
					 ARG_VAL(D3DPOOL, Pool),
					 ARG_REF(IDirect3DVertexBuffer9*, ppIndexBuffer),
					 ARG_REF(HANDLE, pSharedHandle))


//    user_assert(ppIndexBuffer, D3DERR_INVALIDCALL);

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateIndexBuffer,,,
					 HRESULT,
					 ARG_VAL(UINT, Length),
					 ARG_VAL(DWORD, Usage),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DPOOL, Pool),
					 ARG_REF(IDirect3DIndexBuffer9*, ppIndexBuffer),
					 ARG_REF(HANDLE, pSharedHandle))


CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateRenderTarget,,,
					 HRESULT,
					 ARG_VAL(UINT, Width),
					 ARG_VAL(UINT, Height),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DMULTISAMPLE_TYPE, MultiSample),
					 ARG_VAL(DWORD, MultisampleQuality),
					 ARG_VAL(BOOL, Pureable),
					 ARG_REF(IDirect3DSurface9*, ppSurface),
					 ARG_REF(HANDLE, pSharedHandle))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateDepthStencilSurface,,,
					 HRESULT,
					 ARG_VAL(UINT, Width),
					 ARG_VAL(UINT, Height),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DMULTISAMPLE_TYPE, MultiSample),
					 ARG_VAL(DWORD, MultisampleQuality),
					 ARG_VAL(BOOL, Discard),
					 ARG_REF(IDirect3DSurface9*, ppSurface),
					 ARG_REF(HANDLE, pSharedHandle))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateOffscreenPlainSurface,,,
					 HRESULT,
					 ARG_VAL(UINT, Width),
					 ARG_VAL(UINT, Height),
					 ARG_VAL(D3DFORMAT, Format),
					 ARG_VAL(D3DPOOL, Pool),
					 ARG_REF(IDirect3DSurface9*, ppSurface),
					 ARG_REF(HANDLE, pSharedHandle))

/* allowed on PURE devices */
CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateStateBlock,,,
					 HRESULT,
					 ARG_VAL(D3DSTATEBLOCKTYPE, Type),
					 ARG_REF(IDirect3DStateBlock9*, ppSB))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateVertexDeclaration,,,
					 HRESULT,
					 ARG_REF(const D3DVERTEXELEMENT9, pVertexElements),
					 ARG_REF(IDirect3DVertexDeclaration9*, ppDecl))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateVertexShader,,,
					 HRESULT,
					 ARG_REF(const DWORD, pFunction),
					 ARG_REF(IDirect3DVertexShader9*, ppShader))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreatePixelShader,,,
					 HRESULT,
					 ARG_REF(const DWORD, pFunction),
					 ARG_REF(IDirect3DPixelShader9*, ppShader))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, CreateQuery,,,
					 HRESULT,
					 ARG_VAL(D3DQUERYTYPE, Type),
					 ARG_REF(IDirect3DQuery9*, ppQuery))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Device9, BeginStateBlock,,,)

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, EndStateBlock,,,
		HRESULT,
		ARG_REF(IDirect3DStateBlock9 *,ppSB))

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

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9, ValidateDevice,,,
					HRESULT,
					ARG_REF(DWORD, pNumPasses))

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

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, PresentEx,,,
					HRESULT,
					ARG_REF(RECT, pSourceRect),
					ARG_REF(RECT, pDestRect),
					ARG_VAL(HWND, hDestWindowOverride),
					ARG_REF(RGNDATA, pDirtyRegion),
					ARG_VAL(DWORD, dwFlags))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, CheckDeviceState,,,
					HRESULT,
					ARG_VAL(HWND, hDestinationWindow))

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

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, CreateRenderTargetEx,,,
					HRESULT,
					ARG_VAL(UINT, Width),
					ARG_VAL(UINT, Height),
					ARG_VAL(D3DFORMAT, Format),
					ARG_VAL(D3DMULTISAMPLE_TYPE, MultiSample),
					ARG_VAL(DWORD, MultisampleQuality),
					ARG_VAL(BOOL, Pureable),
					ARG_REF(IDirect3DSurface9*, ppSurface),
					ARG_REF(HANDLE, pSharedHandle),
					ARG_VAL(DWORD, Usage))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, CreateOffscreenPlainSurfaceEx,,,
					HRESULT,
					ARG_VAL(UINT, Width),
					ARG_VAL(UINT, Height),
					ARG_VAL(D3DFORMAT, Format),
					ARG_VAL(D3DPOOL, Pool),
					ARG_REF(IDirect3DSurface9*, ppSurface),
					ARG_REF(HANDLE, pSharedHandle),
					ARG_VAL(DWORD, Usage))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, CreateDepthStencilSurfaceEx,,,
					HRESULT,
					ARG_VAL(UINT, Width),
					ARG_VAL(UINT, Height),
					ARG_VAL(D3DFORMAT, Format),
					ARG_VAL(D3DMULTISAMPLE_TYPE, MultiSample),
					ARG_VAL(DWORD, MultisampleQuality),
					ARG_VAL(BOOL, Discard),
					ARG_REF(IDirect3DSurface9*, ppSurface),
					ARG_REF(HANDLE, pSharedHandle),
					ARG_VAL(DWORD, Usage))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, ResetEx,,,
					HRESULT,
					ARG_REF(D3DPRESENT_PARAMETERS, pPresentationParameters),
					ARG_REF(D3DDISPLAYMODEEX, pFullscreenDisplayMode))

CREATE_FUNC_BLOCKING_WITH_RESULT(Device9Ex, GetDisplayModeEx,,,
					HRESULT,
					ARG_VAL(UINT, iSwapChain),
					ARG_REF(D3DDISPLAYMODEEX, pMode),
					ARG_REF(D3DDISPLAYROTATION, pRotation))

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

CREATE_FUNC_BLOCKING_WITH_RESULT(Surface9, GetContainer,,,
					 HRESULT,
					 ARG_VAL(REFIID, riid),
					 ARG_REF(void *, ppContainer))

CREATE_FUNC_BLOCKING_WITH_RESULT(Surface9, LockRect,,,
					 HRESULT,
					 ARG_REF(D3DLOCKED_RECT, pPureedRect),
					 ARG_REF(RECT, pRect),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Surface9, UnlockRect,,,)

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

CREATE_FUNC_BLOCKING_WITH_RESULT(BaseTexture9, SetLOD,,,
					 DWORD,
					 ARG_VAL(DWORD, LODNew))

CREATE_FUNC_BLOCKING_WITH_RESULT(BaseTexture9, GetLOD,,,
					 DWORD)

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(BaseTexture9, SetAutoGenFilterType,,,
						 ARG_VAL(D3DTEXTUREFILTERTYPE, FilterType))


CREATE_FUNC_BLOCKING_WITH_RESULT(BaseTexture9, GetAutoGenFilterType,,,
					 D3DTEXTUREFILTERTYPE)

CREATE_FUNC_NON_BLOCKING_NO_RESULT(BaseTexture9, PreLoad,,,)
CREATE_FUNC_NON_BLOCKING_NO_RESULT(BaseTexture9, GenerateMipSubLevels,,,)

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

CREATE_FUNC_BLOCKING_WITH_RESULT(CubeTexture9, LockRect,,,
					 HRESULT,
					 ARG_VAL(D3DCUBEMAP_FACES, FaceType),
					 ARG_VAL(UINT, Level),
					 ARG_REF(D3DLOCKED_RECT, pPureedRect),
					 ARG_REF(RECT, pRect),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(CubeTexture9, UnlockRect,,,
					 ARG_VAL(D3DCUBEMAP_FACES, FaceType),
					 ARG_VAL(UINT, Level))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(CubeTexture9, AddDirtyRect,,,
					 ARG_VAL(D3DCUBEMAP_FACES, FaceType),
					 ARG_COPY_REF(RECT, pDirtyRect))

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

#if 0
struct s_IndexBuffer9_Lock_private {
	HRESULT *_result;
	HRESULT __result;
	UINT _OffsetToPure;
	UINT _SizeToPure;
	void **_ppbData;
	void *__ppbData;
	DWORD _Flags;
};

CREATE_SINK_WITH_RESULT(IndexBuffer9, Lock,
						ARG_VAL(UINT, OffsetToPure),
						ARG_VAL(UINT, SizeToPure),
						ARG_REF(void *, ppbData),
						ARG_VAL(DWORD, Flags))

static HRESULT NINE_WINAPI
PureIndexBuffer9_Lock( struct NineIndexBuffer9 *This,
					UINT OffsetToPure,
					UINT SizeToPure,
					void **ppbData,
					DWORD Flags )
{
    GET_CONTEXT(IndexBuffer9)
    struct queue_element* slot;
    struct s_IndexBuffer9_Lock_private *args;
    HRESULT r;

    user_assert(ppbData, D3DERR_INVALIDCALL);

    if (This->base.base.pool == D3DPOOL_MANAGED) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_IndexBuffer9_Lock_private), (void **)&args);
        slot->data = args;
        slot->func = PureIndexBuffer9_Lock_rx;

        THIS_BIND_FUNC(IndexBuffer9)

        args->_result = &args->__result;
        args->_OffsetToPure = OffsetToPure;
    	args->_SizeToPure = SizeToPure;
    	args->_ppbData = &args->__ppbData;
    	args->_Flags = Flags;
        queue_set_slot_ready(ctx->queue, slot);

        *ppbData = (char *)This->base.managed.data + OffsetToPure;

        r = D3D_OK;
    } else {
		slot = queue_get_free_slot(ctx->queue, sizeof(struct s_IndexBuffer9_Lock_private), (void **)&args);
		slot->data = args;
		slot->func = PureIndexBuffer9_Lock_rx;
		slot->this = (void *)This;

		args->_result = &r;
		args->_OffsetToPure = OffsetToPure;
		args->_SizeToPure = SizeToPure;
		args->_ppbData = ppbData;
		args->_Flags = Flags;

		queue_set_slot_ready_and_wait(ctx->queue, slot);
    }

    return r;
}
#endif

CREATE_FUNC_BLOCKING_WITH_RESULT(IndexBuffer9, Lock,,,
					HRESULT,
					 ARG_VAL(UINT, OffsetToPure),
					 ARG_VAL(UINT, SizeToPure),
					 ARG_REF(void *, ppbData),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(IndexBuffer9, Unlock,,,)

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

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Query9, Issue,,,
		ARG_VAL(DWORD, dwIssueFlags))

CREATE_FUNC_BLOCKING_WITH_RESULT(Query9, GetData,,,
					HRESULT,
					 ARG_REF(void, pData),
					 ARG_VAL(DWORD, dwSize),
					 ARG_VAL(DWORD, dwGetDataFlags))

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

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(StateBlock9, Capture,,,)
CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(StateBlock9, Apply,,,)

IDirect3DStateBlock9Vtbl PureStateBlock9_vtable = {
    (void *)NineUnknown_QueryInterface,
    (void *)NineUnknown_AddRef,
    (void *)NineUnknown_Release,
    (void *)NineUnknown_GetDevice, /* actually part of StateBlock9 iface */
    (void *)PureStateBlock9_Capture,
    (void *)PureStateBlock9_Apply
};

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9, Present,,,
					 HRESULT,
					 ARG_REF(RECT, pSourceRect),
					 ARG_REF(RECT, pDestRect),
					 ARG_VAL(HWND, hDestWindowOverride),
					 ARG_REF(RGNDATA, pDirtyRegion),
					 ARG_VAL(DWORD, dwFlags))

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9, GetFrontBufferData,,,
					 HRESULT,
					 ARG_REF(IDirect3DSurface9, pDestSurface))

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9, GetBackBuffer,,,
					 HRESULT,
					 ARG_VAL(UINT, iBackBuffer),
					 ARG_VAL(D3DBACKBUFFER_TYPE, Type),
					 ARG_REF(IDirect3DSurface9*, ppBackBuffer))

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9, GetRasterStatus,,,
					 HRESULT,
					 ARG_REF(D3DRASTER_STATUS, pRasterStatus))

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9, GetDisplayMode,,,
					 HRESULT,
					 ARG_REF(D3DDISPLAYMODE, pMode))

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9, GetPresentParameters,,,
					 HRESULT,
					 ARG_REF(D3DPRESENT_PARAMETERS, pPresentationParameters))

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

CREATE_FUNC_BLOCKING_WITH_RESULT(SwapChain9Ex, GetDisplayModeEx,,,
					 HRESULT,
					 ARG_REF(D3DDISPLAYMODEEX, pMode),
					 ARG_REF(D3DDISPLAYROTATION, pRotation))

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

CREATE_FUNC_BLOCKING_WITH_RESULT(Texture9, LockRect,,,
					 HRESULT,
					 ARG_VAL(UINT, Level),
					 ARG_REF(D3DLOCKED_RECT, pPureedRect),
					 ARG_REF(RECT, pRect),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Texture9, UnlockRect,,,
						 ARG_VAL(UINT, Level))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Texture9, AddDirtyRect,,,
						 ARG_COPY_REF(RECT, pDirtyRect))

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

#if 0
struct s_VertexBuffer9_Lock_private {
	HRESULT *_result;
	HRESULT __result;
	UINT _OffsetToPure;
	UINT _SizeToPure;
	void **_ppbData;
	void *__ppbData;
	DWORD _Flags;
};

CREATE_SINK_WITH_RESULT(VertexBuffer9, Lock,
						ARG_VAL(UINT, OffsetToPure),
						ARG_VAL(UINT, SizeToPure),
						ARG_REF(void *, ppbData),
						ARG_VAL(DWORD, Flags))

static HRESULT NINE_WINAPI
PureVertexBuffer9_Lock( struct NineVertexBuffer9 *This,
					UINT OffsetToPure,
					UINT SizeToPure,
					void **ppbData,
					DWORD Flags )
{
    GET_CONTEXT(VertexBuffer9)
    struct queue_element* slot;
    struct s_VertexBuffer9_Lock_private *args;
    HRESULT r;

    user_assert(ppbData, D3DERR_INVALIDCALL);

    if (This->base.base.pool == D3DPOOL_MANAGED) {
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_VertexBuffer9_Lock_private), (void **)&args);
        slot->data = args;
        slot->func = PureVertexBuffer9_Lock_rx;

        THIS_BIND_FUNC(VertexBuffer9)

        args->_result = &args->__result;
        args->_OffsetToPure = OffsetToPure;
    	args->_SizeToPure = SizeToPure;
    	args->_ppbData = &args->__ppbData;
    	args->_Flags = Flags;
        queue_set_slot_ready(ctx->queue, slot);

        *ppbData = (char *)This->base.managed.data + OffsetToPure;

        r = D3D_OK;
    } else {
		slot = queue_get_free_slot(ctx->queue, sizeof(struct s_VertexBuffer9_Lock_private), (void **)&args);
		slot->data = args;
		slot->func = PureVertexBuffer9_Lock_rx;
		slot->this = (void *)This;

		args->_result = &r;
		args->_OffsetToPure = OffsetToPure;
		args->_SizeToPure = SizeToPure;
		args->_ppbData = ppbData;
		args->_Flags = Flags;

		queue_set_slot_ready_and_wait(ctx->queue, slot);
    }

    return r;
}
#endif
CREATE_FUNC_BLOCKING_WITH_RESULT(VertexBuffer9, Lock,,,
					HRESULT,
					 ARG_VAL(UINT, OffsetToPure),
					 ARG_VAL(UINT, SizeToPure),
					 ARG_REF(void *, ppbData),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(VertexBuffer9, Unlock,,,)

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

CREATE_FUNC_BLOCKING_WITH_RESULT(Volume9, GetContainer,,,
					HRESULT,
					 ARG_VAL(REFIID, riid),
					 ARG_REF(void *, ppContainer))

CREATE_FUNC_BLOCKING_WITH_RESULT(Volume9, LockBox,,,
					HRESULT,
					 ARG_REF(D3DLOCKED_BOX, pPureedVolume),
					 ARG_REF(D3DBOX, pBox),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(Volume9, UnlockBox,,,)

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

CREATE_FUNC_BLOCKING_WITH_RESULT(VolumeTexture9, LockBox,,,
					HRESULT,
					 ARG_VAL(UINT, Level),
					 ARG_REF(D3DLOCKED_BOX, pPureedVolume),
					 ARG_REF(D3DBOX, pBox),
					 ARG_VAL(DWORD, Flags))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(VolumeTexture9, UnlockBox,,,
						 ARG_VAL(UINT, Level))

CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(VolumeTexture9, AddDirtyBox,,,
						 ARG_COPY_REF(D3DBOX, pDirtyBox))

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

    //TODO
    ctx->vertex_uploader = u_upload_create(This->pipe, 1024 * 128,
                                            PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_STREAM);
    if (!ctx->vertex_uploader) {
        FREE(ctx);
        return NULL;
    }

    //TODO
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

