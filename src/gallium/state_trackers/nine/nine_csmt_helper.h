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

// get number of arguments with __NARG__
#define __NARG__(...)  __NARG_I_(__VA_ARGS__,__RSEQ_N())
#define __NARG_I_(...) __ARG_N(__VA_ARGS__)
#define __ARG_N( \
      _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
     _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
     _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
     _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
     _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
     _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
     _61,_62,_63,N,...) N
#define __RSEQ_N() \
     63,62,61,60,                   \
     59,58,57,56,55,54,53,52,51,50, \
     49,48,47,46,45,44,43,42,41,40, \
     39,38,37,36,35,34,33,32,31,30, \
     29,28,27,26,25,24,23,22,21,20, \
     19,18,17,16,15,14,13,12,11,10, \
     9,8,7,6,5,4,3,2,1,0

#define _args_for_unbind_1(a) a;
#define _args_for_unbind_5(a, b, c, d, e) e;
#define _args_for_unbind_10(a, b, c, d, e, ...) e; _args_for_unbind_5(__VA_ARGS__)
#define _args_for_unbind_15(a, b, c, d, e, ...) e; _args_for_unbind_10(__VA_ARGS__)
#define _args_for_unbind_20(a, b, c, d, e, ...) e; _args_for_unbind_15(__VA_ARGS__)
#define _args_for_unbind_25(a, b, c, d, e, ...) e; _args_for_unbind_20(__VA_ARGS__)
#define _args_for_unbind_30(a, b, c, d, e, ...) e; _args_for_unbind_25(__VA_ARGS__)
#define _args_for_unbind_35(a, b, c, d, e, ...) e; _args_for_unbind_30(__VA_ARGS__)
#define _args_for_unbind_40(a, b, c, d, e, ...) e; _args_for_unbind_35(__VA_ARGS__)
#define _args_for_unbind_45(a, b, c, d, e, ...) e; _args_for_unbind_40(__VA_ARGS__)
#define _args_for_unbind_50(a, b, c, d, e, ...) e; _args_for_unbind_45(__VA_ARGS__)

#define _EFUNC_(n) _args_for_unbind_##n
#define _EFUNC(n) _EFUNC_(n)

#define ARGS_FOR_UNBIND(...) _EFUNC(__NARG__(__VA_ARGS__)) (__VA_ARGS__)

#define _args_for_call_1(a) a
#define _args_for_call_5(a, b, c, d, e) ,d
#define _args_for_call_10(a, b, c, d, e, ...) ,d _args_for_call_5(__VA_ARGS__)
#define _args_for_call_15(a, b, c, d, e, ...) ,d _args_for_call_10(__VA_ARGS__)
#define _args_for_call_20(a, b, c, d, e, ...) ,d _args_for_call_15(__VA_ARGS__)
#define _args_for_call_25(a, b, c, d, e, ...) ,d _args_for_call_20(__VA_ARGS__)
#define _args_for_call_30(a, b, c, d, e, ...) ,d _args_for_call_25(__VA_ARGS__)
#define _args_for_call_35(a, b, c, d, e, ...) ,d _args_for_call_30(__VA_ARGS__)
#define _args_for_call_40(a, b, c, d, e, ...) ,d _args_for_call_35(__VA_ARGS__)
#define _args_for_call_45(a, b, c, d, e, ...) ,d _args_for_call_40(__VA_ARGS__)
#define _args_for_call_50(a, b, c, d, e, ...) ,d _args_for_call_45(__VA_ARGS__)

#define _DFUNC_(n) _args_for_call_##n
#define _DFUNC(n) _DFUNC_(n)

#define ARGS_FOR_CALL(...) _DFUNC(__NARG__(__VA_ARGS__)) (__VA_ARGS__)

#define _args_for_decl_1(a) a
#define _args_for_decl_5(a, b, c, d, e) ,c
#define _args_for_decl_10(a, b, c, d, e, ...) ,c _args_for_decl_5(__VA_ARGS__)
#define _args_for_decl_15(a, b, c, d, e, ...) ,c _args_for_decl_10(__VA_ARGS__)
#define _args_for_decl_20(a, b, c, d, e, ...) ,c _args_for_decl_15(__VA_ARGS__)
#define _args_for_decl_25(a, b, c, d, e, ...) ,c _args_for_decl_20(__VA_ARGS__)
#define _args_for_decl_30(a, b, c, d, e, ...) ,c _args_for_decl_25(__VA_ARGS__)
#define _args_for_decl_35(a, b, c, d, e, ...) ,c _args_for_decl_30(__VA_ARGS__)
#define _args_for_decl_40(a, b, c, d, e, ...) ,c _args_for_decl_35(__VA_ARGS__)
#define _args_for_decl_45(a, b, c, d, e, ...) ,c _args_for_decl_40(__VA_ARGS__)
#define _args_for_decl_50(a, b, c, d, e, ...) ,c _args_for_decl_45(__VA_ARGS__)

#define _CFUNC_(n) _args_for_decl_##n
#define _CFUNC(n) _CFUNC_(n)

#define ARGS_FOR_DECLARATION(...) _CFUNC(__NARG__(__VA_ARGS__)) (__VA_ARGS__)

#define _args_for_assign_1(a) a
#define _args_for_assign_5(a, b, c, d, e) b;
#define _args_for_assign_10(a, b, c, d, e, ...) b; _args_for_assign_5(__VA_ARGS__)
#define _args_for_assign_15(a, b, c, d, e, ...) b; _args_for_assign_10(__VA_ARGS__)
#define _args_for_assign_20(a, b, c, d, e, ...) b; _args_for_assign_15(__VA_ARGS__)
#define _args_for_assign_25(a, b, c, d, e, ...) b; _args_for_assign_20(__VA_ARGS__)
#define _args_for_assign_30(a, b, c, d, e, ...) b; _args_for_assign_25(__VA_ARGS__)
#define _args_for_assign_35(a, b, c, d, e, ...) b; _args_for_assign_30(__VA_ARGS__)
#define _args_for_assign_40(a, b, c, d, e, ...) b; _args_for_assign_35(__VA_ARGS__)
#define _args_for_assign_45(a, b, c, d, e, ...) b; _args_for_assign_40(__VA_ARGS__)
#define _args_for_assign_50(a, b, c, d, e, ...) b; _args_for_assign_45(__VA_ARGS__)

#define _BFUNC_(n) _args_for_assign_##n
#define _BFUNC(n) _BFUNC_(n)

#define ARGS_FOR_ASSIGN(...) _BFUNC(__NARG__(__VA_ARGS__)) (__VA_ARGS__)

#define _args_for_struct_1(a) a;
#define _args_for_struct_5(a, b, c, d, e) a;
#define _args_for_struct_10(a, b, c, d, e, ...) a; _args_for_struct_5(__VA_ARGS__)
#define _args_for_struct_15(a, b, c, d, e, ...) a; _args_for_struct_10(__VA_ARGS__)
#define _args_for_struct_20(a, b, c, d, e, ...) a; _args_for_struct_15(__VA_ARGS__)
#define _args_for_struct_25(a, b, c, d, e, ...) a; _args_for_struct_20(__VA_ARGS__)
#define _args_for_struct_30(a, b, c, d, e, ...) a; _args_for_struct_25(__VA_ARGS__)
#define _args_for_struct_35(a, b, c, d, e, ...) a; _args_for_struct_30(__VA_ARGS__)
#define _args_for_struct_40(a, b, c, d, e, ...) a; _args_for_struct_35(__VA_ARGS__)
#define _args_for_struct_45(a, b, c, d, e, ...) a; _args_for_struct_40(__VA_ARGS__)
#define _args_for_struct_50(a, b, c, d, e, ...) a; _args_for_struct_45(__VA_ARGS__)

#define _AFUNC_(n) _args_for_struct_##n
#define _AFUNC(n) _AFUNC_(n)

#define ARGS_FOR_STRUCT(...) _AFUNC(__NARG__(__VA_ARGS__)) (__VA_ARGS__)

// bind funcs

#define THIS_BIND_FUNC_Device9 slot->this = (void *)This;
#define THIS_UNBIND_FUNC_Device9

#define THIS_BIND_FUNC_SwapChain9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_SwapChain9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_Device9Ex slot->this = (void *)This;
#define THIS_UNBIND_FUNC_Device9Ex

#define THIS_BIND_FUNC_SwapChain9Ex slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_SwapChain9Ex nine_bind(&This, NULL);

#define THIS_BIND_FUNC_Volume9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_Volume9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_VolumeTexture9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_VolumeTexture9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_Texture9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_Texture9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_CubeTexture9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_CubeTexture9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_Surface9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_Surface9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_BaseTexture9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_BaseTexture9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_Query9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_Query9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_StateBlock9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_StateBlock9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_VertexBuffer9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_VertexBuffer9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_IndexBuffer9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_IndexBuffer9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_AuthenticatedChannel9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_AuthenticatedChannel9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_CryptoSession9 slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_CryptoSession9 nine_bind(&This, NULL);

#define THIS_BIND_FUNC_Device9Video slot->this = NULL; nine_bind(&slot->this, This);
#define THIS_UNBIND_FUNC_Device9Video nine_bind(&This, NULL);

#define _THIS_BIND_FUNC(name) THIS_BIND_FUNC_##name
#define THIS_BIND_FUNC(name) _THIS_BIND_FUNC(name)

#define _THIS_UNBIND_FUNC(name) THIS_UNBIND_FUNC_##name
#define THIS_UNBIND_FUNC(name) _THIS_UNBIND_FUNC(name)

// get device funcs

#define GET_DEVICE_Volume9 This->base.device
#define GET_DEVICE_VolumeTexture9 This->base.base.base.device
#define GET_DEVICE_Device9 This
#define GET_DEVICE_Device9Ex &This->base
#define GET_DEVICE_SwapChain9 This->base.device
#define GET_DEVICE_SwapChain9Ex This->base.base.device
#define GET_DEVICE_Texture9 This->base.base.base.device
#define GET_DEVICE_VertexBuffer9 This->base.base.base.device
#define GET_DEVICE_IndexBuffer9 This->base.base.base.device
#define GET_DEVICE_Surface9 This->base.base.device
#define GET_DEVICE_BaseTexture9 This->base.base.device
#define GET_DEVICE_CubeTexture9 This->base.base.base.device
#define GET_DEVICE_Query9 This->base.device
#define GET_DEVICE_StateBlock9 This->base.device
#define GET_DEVICE_AuthenticatedChannel9 This->base.device
#define GET_DEVICE_CryptoSession9 This->base.device
#define GET_DEVICE_Device9Video This->base.device
#define GET_DEVICE_Resource9 This->base.device
#define GET_DEVICE_Unknown This->device

#define _GET_DEVICE_FUNC(name) GET_DEVICE_##name
#define GET_DEVICE_FUNC(name) struct NineDevice9 *device = _GET_DEVICE_FUNC(name);

#define GET_CONTEXT(name) GET_DEVICE_FUNC(name) struct csmt_context *ctx = device->csmt_context;

// serialization and deserialization

#define CREATE_SINK_NO_RESULT_UNBIND(name, func, ...) \
static void \
Pure##name##_##func##_rx( void *this, void *arg ) { \
    struct Nine ## name *This = (struct Nine ## name *) this; \
    struct s_##name##_##func##_private *args = (struct s_##name##_##func##_private *) arg; \
    (void) args; \
    Nine##name##_##func( \
        This ARGS_FOR_CALL( __VA_ARGS__ ) \
    ); \
    THIS_UNBIND_FUNC(name) ;\
    ARGS_FOR_UNBIND( __VA_ARGS__ ) ;\
}

#define CREATE_SINK_NO_RESULT(name, func, ...) \
static void \
Pure##name##_##func##_rx( void *this, void *arg ) { \
    struct Nine ## name *This = (struct Nine ## name *) this; \
    struct s_##name##_##func##_private *args = (struct s_##name##_##func##_private *) arg; \
    (void) args; \
    Nine##name##_##func( \
        This ARGS_FOR_CALL( __VA_ARGS__ ) \
    ); \
}

#define CREATE_SINK_PRINT_RESULT(name, func, ...) \
static void \
Pure##name##_##func##_rx( void *this, void *arg ) { \
    HRESULT r; \
    struct Nine ## name *This = (struct Nine ## name *) this; \
    struct s_##name##_##func##_private *args = (struct s_##name##_##func##_private *) arg; \
    (void) args; \
    r = Nine##name##_##func( \
        This ARGS_FOR_CALL( __VA_ARGS__ ) \
    ); \
    if (r != D3D_OK) \
        ERR("Failed with error %x\n", r); \
    THIS_UNBIND_FUNC(name) ;\
    ARGS_FOR_UNBIND( __VA_ARGS__ ) ;\
}

#define CREATE_SINK_WITH_RESULT(name, func, ...) \
static void \
Pure##name##_##func##_rx( void *this, void *arg ) { \
    struct Nine ## name *This = (struct Nine ## name *) this; \
    struct s_##name##_##func##_private *args = (struct s_##name##_##func##_private *) arg; \
    (void) args; \
    *args->_result = Nine##name##_##func( \
        This ARGS_FOR_CALL( __VA_ARGS__ ) \
    ); \
}

#define CREATE_SOURCE_D3D_OK_RESULT(name, function, pre, post, ...) \
static HRESULT NINE_WINAPI \
Pure##name##_##function( struct Nine##name *This ARGS_FOR_DECLARATION( __VA_ARGS__ ) ) \
{ \
    GET_CONTEXT(name) \
    struct queue_element* slot; \
    struct s_##name##_##function##_private *args; \
    \
    pipe_mutex_lock(d3d_csmt_global); \
    if (sizeof(struct s_##name##_##function##_private)) { \
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_##name##_##function##_private), (void **)&args); \
        slot->data = args; \
    } else { \
        slot = queue_get_free_slot(ctx->queue, 0, NULL); \
        slot->data = NULL; \
    } \
    slot->func = Pure##name##_##function##_rx; \
    THIS_BIND_FUNC(name) ;\
    ARGS_FOR_ASSIGN( __VA_ARGS__ ) ;\
    pre ;\
    queue_set_slot_ready(ctx->queue, slot); \
    post ;\
    pipe_mutex_unlock(d3d_csmt_global); \
    return D3D_OK; \
}

#define CREATE_SOURCE_NO_RESULT(name, function, pre, post, ...) \
static void NINE_WINAPI \
Pure##name##_##function( struct Nine##name *This ARGS_FOR_DECLARATION( __VA_ARGS__ ) ) \
{ \
    GET_CONTEXT(name) \
    struct queue_element* slot; \
    struct s_##name##_##function##_private *args; \
    \
    pipe_mutex_lock(d3d_csmt_global); \
    if (sizeof(struct s_##name##_##function##_private)) { \
        slot = queue_get_free_slot(ctx->queue, sizeof(struct s_##name##_##function##_private), (void **)&args); \
        slot->data = args; \
    } else { \
        slot = queue_get_free_slot(ctx->queue, 0, NULL); \
        slot->data = NULL; \
    } \
    slot->func = Pure##name##_##function##_rx; \
    THIS_BIND_FUNC(name) ;\
    ARGS_FOR_ASSIGN( __VA_ARGS__ ) ;\
    pre ;\
    queue_set_slot_ready(ctx->queue, slot); \
    post ;\
    pipe_mutex_unlock(d3d_csmt_global); \
}


#define CREATE_SOURCE_WITH_RESULT(name, function, pre, post, ret, ...) \
static ret NINE_WINAPI \
Pure##name##_##function( struct Nine##name *This ARGS_FOR_DECLARATION( __VA_ARGS__ ) ) \
{ \
    GET_CONTEXT(name) \
    struct queue_element* slot; \
    struct s_##name##_##function##_private *args; \
    ret r; \
    \
    pipe_mutex_lock(d3d_csmt_global); \
    assert(sizeof(struct s_##name##_##function##_private)); \
    slot = queue_get_free_slot(ctx->queue, sizeof(struct s_##name##_##function##_private), (void **)&args); \
    slot->data = args; \
    slot->func = Pure##name##_##function##_rx; \
    args->_result = &r; \
    slot->this = (void *)This; \
    ARGS_FOR_ASSIGN( __VA_ARGS__ ) \
    pre ;\
    queue_set_slot_ready_and_wait(ctx->queue, slot); \
    post ;\
    pipe_mutex_unlock(d3d_csmt_global); \
    return r; \
}

#define CREATE_STUB_INVALID_CALL(name, func, ...) \
static HRESULT NINE_WINAPI \
Pure##name##_##func( struct Nine##name This DEFINE_ARGS(__VA_ARGS__) ) \
{ \
    STUB(D3DERR_INVALIDCALL); \
}

#define CREATE_FUNC_NON_BLOCKING_PRINT_RESULT(name, func, pre, post, ...) \
struct s_##name##_##func##_private { ARGS_FOR_STRUCT( __VA_ARGS__ ) }; \
CREATE_SINK_PRINT_RESULT(name, func, __VA_ARGS__) \
CREATE_SOURCE_D3D_OK_RESULT(name, func, pre, post, __VA_ARGS__)

#define CREATE_FUNC_NON_BLOCKING_NO_RESULT(name, func, pre, post, ...) \
struct s_##name##_##func##_private { ARGS_FOR_STRUCT( __VA_ARGS__ ) }; \
CREATE_SINK_NO_RESULT_UNBIND(name, func, __VA_ARGS__) \
CREATE_SOURCE_NO_RESULT(name, func, pre, post, __VA_ARGS__)

#define CREATE_FUNC_BLOCKING_WITH_RESULT(name, func, pre, post, ret, ...) \
struct s_##name##_##func##_private { ret *_result; ARGS_FOR_STRUCT( __VA_ARGS__ ) }; \
CREATE_SINK_WITH_RESULT(name, func, __VA_ARGS__) \
CREATE_SOURCE_WITH_RESULT(name, func, pre, post, ret, __VA_ARGS__)

#define CREATE_FUNC_BLOCKING_NO_RESULT(name, func, pre, post, ...) \
struct s_##name##_##func##_private { ARGS_FOR_STRUCT( __VA_ARGS__ ) }; \
CREATE_SINK_NO_RESULT(name, func, __VA_ARGS__) \
CREATE_SOURCE_NO_RESULT(name, func, pre, post, __VA_ARGS__)

/* struct, assign func, tx arg list, rx arg list */
#define ARG_VAL(x, y) x _##y ; , args->_##y = y ; , x y , args->_##y ,
#define ARG_REF(x, y) x* _##y ; , args->_##y = y; , x *y , args->_##y ,
#define ARG_COPY_REF(x, y) x * _##y ; x __##y ; , if ( y ) { args->_##y = &args->__##y ; args->__##y = *y ; } else { args->_##y = NULL; } , x *y , args->_##y ,
#define ARG_BIND_REF(x, y) x * _##y , args->_##y = NULL ; if (args->_##y != y && args->_##y) NineUnknown_Unbind((void *)(args->_##y)); if ( args->_##y != y && y ) NineUnknown_Bind( (void *)y ); if ( args->_##y != y ) args->_##y = y ; , x *y , args->_##y, if (args->_##y != NULL && args->_##y) NineUnknown_Unbind((void *)(args->_##y)); args->_##y = NULL;

