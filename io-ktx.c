/* 
 * GdkPixbuf library - TGA image loader
 * Copyright (C) 2019 Julius Ikkala
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "ktx.h"
// We only need the enums from gl.h, since KTX uses them.
#include <GL/gl.h>

#define GDK_PIXBUF_ENABLE_BACKEND
#include "gdk-pixbuf/gdk-pixbuf-io.h"
#undef GDK_PIXBUF_ENABLE_BACKEND

#undef max
#define max(a, b) ((a) > (b) ? (a) : (b))

G_MODULE_EXPORT void fill_vtable(GdkPixbufModule * module);
G_MODULE_EXPORT void fill_info(GdkPixbufFormat * info);

static unsigned gl_format_channel_count(GLint format)
{
    switch(format)
    {
    case GL_RED:
    case GL_STENCIL_INDEX:
    case GL_DEPTH_COMPONENT:
        return 1;
    case GL_RG:
        return 2;
    case GL_RGB:
    case GL_BGR:
        return 3;
    case GL_RGBA:
    case GL_BGRA:
        return 4;
    default:
        // Unsupported format
        return 0;
    }
}

static unsigned gl_type_sizeof(GLenum type)
{
    switch(type)
    {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        return 2;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FIXED:
    case GL_FLOAT:
        return 4;
    case GL_DOUBLE:
        return 8;
    default:
        // Unsupported data type
        return 0;
    }
}

// https://gist.github.com/rygorous/2156668
static float half_to_float(unsigned short hf)
{
    typedef unsigned int uint;

    union FP32
    {
        uint u;
        float f;
        struct
        {
            uint Mantissa : 23;
            uint Exponent : 8;
            uint Sign : 1;
        };
    };

    union FP16
    {
        unsigned short u;
        struct
        {
            uint Mantissa : 10;
            uint Exponent : 5;
            uint Sign : 1;
        };
    } h = {hf};

    static const union FP32 magic = { 113 << 23 };
    static const uint shifted_exp = 0x7c00 << 13; // exponent mask after shift
    union FP32 o;

    o.u = (h.u & 0x7fff) << 13;     // exponent/mantissa bits
    uint exp = shifted_exp & o.u;   // just the exponent
    o.u += (127 - 15) << 23;        // exponent adjust

    // handle exponent special cases
    if (exp == shifted_exp) // Inf/NaN?
        o.u += (128 - 16) << 23;    // extra exp adjust
    else if (exp == 0) // Zero/Denormal?
    {
        o.u += 1 << 23;             // extra exp adjust
        o.f -= magic.f;             // renormalize
    }

    o.u |= (h.u & 0x8000) << 16;    // sign bit
    return o.f;
}

static guchar read_gl_type(const void* data, GLenum type)
{
    int itmp;
    unsigned utmp;
    double ftmp;
    switch(type)
    {
    case GL_BYTE:
        itmp = *(const char*)data;
        return max(itmp*2, 0);
    case GL_UNSIGNED_BYTE:
        return *(const guchar*)data;
    case GL_SHORT:
        itmp = *(const short*)data;
        return max(itmp/128, 0);
    case GL_UNSIGNED_SHORT:
        utmp = *(const unsigned short*)data;
        return utmp/257;
    case GL_HALF_FLOAT:
        // Parsing halfs is a bit more difficult than the others...
        ftmp = half_to_float(*(const unsigned short*)data);
        if(ftmp < 0) ftmp = 0;
        if(ftmp > 1) ftmp = 1;
        return ftmp*255;
    case GL_INT:
        itmp = *(const int*)data;
        return max(itmp/8421504, 0);
    case GL_UNSIGNED_INT:
        utmp = *(const unsigned*)data;
        return utmp/16843009;
    case GL_FLOAT:
        ftmp = *(const float*)data;
        if(ftmp < 0) ftmp = 0;
        if(ftmp > 1) ftmp = 1;
        return ftmp*255;
    case GL_DOUBLE:
        ftmp = *(const double*)data;
        if(ftmp < 0) ftmp = 0;
        if(ftmp > 1) ftmp = 1;
        return ftmp*255;
    default:
        // Unsupported data type
        return 0;
    }
}

static void destroy_data(guchar *pixels, gpointer data)
{
    g_free(pixels);
}

struct ktx_context
{
    GdkPixbuf *pixbuf;

    GdkPixbufModuleSizeFunc size_func;
    GdkPixbufModuleUpdatedFunc updated_func;
    GdkPixbufModulePreparedFunc prepared_func;
    gpointer user_data;

    guchar* buf;
    guint buf_size;
    gint width, height;
};

static void free_ktx_context(struct ktx_context *context)
{
    if(!context) return;

    if(context->buf)
    {
        g_free(context->buf);
        context->buf = NULL;
    }
    context->buf_size = 0;
    g_free(context);
}

static gpointer ktx_image_begin_load(
    GdkPixbufModuleSizeFunc size_func,
    GdkPixbufModulePreparedFunc prepared_func,
    GdkPixbufModuleUpdatedFunc updated_func,
    gpointer user_data,
    GError **error
){
    struct ktx_context* context;
    context = g_new0 (struct ktx_context, 1);
    if(!context)
        return NULL;

    context->size_func = size_func;
    context->updated_func = updated_func;
    context->prepared_func = prepared_func;
    context->user_data = user_data;

    context->buf = NULL;
    context->buf_size = 0;

    return context;
}

static gboolean ktx_image_load_increment(
    gpointer data,
    const guchar *buf,
    guint size,
    GError **error
){
    struct ktx_context *context = (struct ktx_context *)data;

    guint old_size = context->buf_size;
    context->buf_size += size;
    context->buf = g_realloc(context->buf, context->buf_size);
    memcpy(context->buf+old_size, buf, size);

	return TRUE;
}

static gboolean ktx_image_stop_load(gpointer data, GError **error)
{
    struct ktx_context *context = (struct ktx_context *)data;
    ktxTexture* texture = NULL;
    KTX_error_code result = KTX_SUCCESS;
    ktx_size_t offset = 0;
    ktx_uint8_t* src_data = NULL;
    guchar* dst_data = NULL;
    GdkPixbuf* pixbuf = NULL;
    unsigned src_channel_count = 0;
    unsigned src_type_size = 0;
    unsigned src_offset = 0;
    unsigned dst_channel_count = 0;
    unsigned dst_offset = 0;

    result = ktxTexture_CreateFromMemory(
        context->buf, context->buf_size, 
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture
    );

    if(result != KTX_SUCCESS)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Failed to read file");
        goto fail;
    }

    if(texture->numDimensions != 2)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Only 2-dimensional textures are supported");
        goto fail;
    }

    if(texture->isCompressed == KTX_TRUE)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Compressed textures are not supported");
        goto fail;
    }

    if(texture->isCubemap == KTX_TRUE)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Cubemaps are not supported");
        goto fail;
    }

    if(texture->isArray == KTX_TRUE)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Array textures are not supported");
        goto fail;
    }

    result = ktxTexture_GetImageOffset(texture, 0, 0, 0, &offset);
    if(result != KTX_SUCCESS)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Unable to find image");
        goto fail;
    }

    if(context->size_func)
    {
        context->width = texture->baseWidth;
        context->height = texture->baseHeight;
        context->size_func(
            &context->width,
            &context->height,
            context->user_data
        );
    }

    src_data = ktxTexture_GetData(texture) + offset;
    src_channel_count = gl_format_channel_count(texture->glFormat);
    src_type_size = gl_type_sizeof(texture->glType);

    if(src_type_size == 0 || src_channel_count == 0)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
            "Unsupported image format");
        goto fail;
    }

    dst_data = g_malloc(
        sizeof(guchar)*4*texture->baseWidth*texture->baseHeight
    );
    dst_channel_count = src_channel_count == 4 ? 4 : 3;

    for(unsigned y = 0; y < texture->baseHeight; ++y)
    {
        for(unsigned x = 0; x < texture->baseWidth; ++x)
        {
            guchar rgba[4] = {0,0,0,0};

            // Read pixel from src
            for(unsigned i = 0; i < src_channel_count; ++i)
            {
                rgba[i] = read_gl_type(
                    src_data + src_offset,
                    texture->glType
                );
                src_offset += src_type_size;
            }

            // Put BGR & BGRA into the Correct Order.
            if(texture->glFormat == GL_BGR || texture->glFormat == GL_BGRA)
            {
                guchar tmp = rgba[2];
                rgba[2] = rgba[0];
                rgba[0] = tmp;
            }

            // Write pixel to dst
            for(unsigned i = 0; i < dst_channel_count; ++i)
            {
                dst_data[dst_offset++] = rgba[i];
            }
        }
    }

    pixbuf = gdk_pixbuf_new_from_data(
        dst_data,
        GDK_COLORSPACE_RGB,
        dst_channel_count == 4,
        8,
        texture->baseWidth, texture->baseHeight,
        texture->baseWidth * dst_channel_count,
        destroy_data,
        NULL
    );

    if(!pixbuf)
    {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED,
             "Failed to decode image");
        goto fail;
    }

    if(context->prepared_func)
        context->prepared_func(pixbuf, NULL, context->user_data);

    ktxTexture_Destroy(texture);
    free_ktx_context(context);
    return TRUE;

fail:
    ktxTexture_Destroy(texture);
    free_ktx_context(context);
    return FALSE;
}

#ifndef INCLUDE_ktx
#define MODULE_ENTRY(function) G_MODULE_EXPORT void function
#else
#define MODULE_ENTRY(function) void _gdk_pixbuf__jasper_ ## function
#endif

MODULE_ENTRY (fill_vtable) (GdkPixbufModule * module)
{
    // EOG refuses to work with just module->load :(
    // So I had to waste a lot of time creating these fake incremental
    // loading things which I can't really implement properly using libktx.
	module->begin_load = ktx_image_begin_load;
	module->stop_load = ktx_image_stop_load;
	module->load_increment = ktx_image_load_increment;
}

MODULE_ENTRY (fill_info) (GdkPixbufFormat * info)
{
    static const GdkPixbufModulePattern signature[] = {
        {"\xABKTX 11\xBB\r\n\x1A\n", NULL, 100},
        { NULL, NULL, 0 }
    };
    static const gchar *mime_types[] = {
        "image/ktx",
        NULL
    };
    static const gchar *extensions[] = {
        "ktx",
        NULL
    };

    info->name = "ktx";
    info->signature = (GdkPixbufModulePattern *) signature;
    info->description = "The KTX image format";
    info->mime_types = (gchar **) mime_types;
    info->extensions = (gchar **) extensions;
    info->flags = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license = "LGPL";
    info->disabled = FALSE;
}
