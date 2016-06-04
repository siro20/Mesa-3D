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

struct csmt_dword3_args {
    DWORD arg1;
    DWORD arg2;
    DWORD arg3;
};

struct csmt_int1_uint5_args {
    INT arg1_i;
    UINT arg1_u;
    UINT arg2_u;
    UINT arg3_u;
    UINT arg4_u;
    UINT arg5_u;
};

struct csmt_dword_light_args {
    DWORD arg1;
    D3DLIGHT9 light1;
};

struct csmt_dword_float4_args {
    DWORD arg1_i;
    float arg1_f;
    float arg2_f;
    float arg3_f;
    float arg4_f;
};

struct csmt_dword_void_args {
    DWORD arg1;
    void *obj1;
};

struct csmt_dword_matrix_args {
    DWORD arg1;
    D3DMATRIX mat1;
};

struct csmt_dword4_float1_rect_args {
    DWORD arg1;
    DWORD arg2;
    DWORD arg3;
    DWORD arg4;
    float arg1_f;
    D3DRECT *rect1;
    D3DRECT _rect;
};

struct csmt_uint2_vec4_args {
    UINT arg1;
    UINT arg2;
    float vec1[4];
};

struct csmt_uint2_vec32_args {
    UINT arg1;
    UINT arg2;
    float vec1[4 * 8];
};

struct csmt_uint3_void_args {
    UINT arg1;
    UINT arg2;
    UINT arg3;
    void *obj1;
};

struct csmt_dword_void2_rect2_point_args {
    DWORD arg1;
    RECT *rect1;
    RECT *rect2;
    void *obj1;
    void *obj2;
    POINT *point1;

    RECT _rect1;
    RECT _rect2;
    POINT _point1;
};

struct csmt_dword_uint_void_box_args {
    DWORD arg1;
    uint arg1_u;
    void *obj1;
    D3DBOX *box1;
    D3DBOX _box1;
};

struct csmt_dword3_void3_uint4_result_args {
    DWORD arg1;
    DWORD arg2;
    DWORD arg3;
    void *obj1;
    void *obj2;
    void *obj3;
    UINT arg1_u;
    UINT arg2_u;
    UINT arg3_u;
    UINT arg4_u;
    HRESULT *result;
};

/* for drawPrimitveUP */
struct csmt_dword1_uint2_data_args {
    DWORD arg1;
    UINT arg1_u;
    UINT arg2_u;
    UINT length;
    UINT reserved;
    char data[0];
};

struct csmt_dword3_void4_result_args {
    DWORD arg1;
    DWORD arg2;
    DWORD arg3;
    void *obj1;
    void *obj2;
    void *obj3;
    void *obj4;
    HRESULT *result;
};

struct csmt_dword1_void1_result_args {
    DWORD arg1;
    void *obj1;
    HRESULT *result;
};
