/*
 * Copyright (c) 2020 - 2023 the ThorVG project. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef _WIN32
    #include <malloc.h>
#elif defined(__linux__)
    #include <alloca.h>
#else
    #include <stdlib.h>
#endif

#include "tvgMath.h"
#include "tvgRender.h"
#include "tvgSwCommon.h"

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/
constexpr auto DOWN_SCALE_TOLERANCE = 0.5f;

static bool _rasterDirectRGBAImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint32_t opacity = 255);


static inline uint8_t ALPHA(uint8_t* a)
{
    return *a;
}


static inline uint8_t IALPHA(uint8_t* a)
{
    return ~(*a);
}


static inline uint8_t _abgrLuma(uint8_t* c)
{
    auto v = *(uint32_t*)c;
    return ((((v&0xff)*54) + (((v>>8)&0xff)*183) + (((v>>16)&0xff)*19))) >> 8; //0.2125*R + 0.7154*G + 0.0721*B
}


static inline uint8_t _argbLuma(uint8_t* c)
{
    auto v = *(uint32_t*)c;
    return ((((v&0xff)*19) + (((v>>8)&0xff)*183) + (((v>>16)&0xff)*54))) >> 8; //0.0721*B + 0.7154*G + 0.2125*R
}


static inline uint8_t _abgrInvLuma(uint8_t* c)
{
    return ~_abgrLuma(c);
}


static inline uint8_t _argbInvLuma(uint8_t* c)
{
    return ~_argbLuma(c);
}


static inline uint32_t _abgrJoin(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (a << 24 | b << 16 | g << 8 | r);
}


static inline uint32_t _argbJoin(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (a << 24 | r << 16 | g << 8 | b);
}


static inline bool _compositing(const SwSurface* surface)
{
    if (!surface->compositor || (int)surface->compositor->method <= (int)CompositeMethod::ClipPath) return false;
    return true;
}


static inline bool _matting(const SwSurface* surface)
{
    if ((int)surface->compositor->method < (int)CompositeMethod::AddMask) return true;
    else return false;
}


static inline bool _masking(const SwSurface* surface)
{
    if ((int)surface->compositor->method >= (int)CompositeMethod::AddMask) return true;
    else return false;
}


#include "tvgSwRasterTexmap.h"
#include "tvgSwRasterC.h"
#include "tvgSwRasterAvx.h"
#include "tvgSwRasterNeon.h"


static inline uint32_t _halfScale(float scale)
{
    auto halfScale = static_cast<uint32_t>(0.5f / scale);
    if (halfScale == 0) halfScale = 1;
    return halfScale;
}

//Bilinear Interpolation
static uint32_t _interpUpScaler(const uint32_t *img, uint32_t w, uint32_t h, float sx, float sy)
{
    auto rx = (uint32_t)(sx);
    auto ry = (uint32_t)(sy);
    auto rx2 = rx + 1;
    if (rx2 >= w) rx2 = w - 1;
    auto ry2 = ry + 1;
    if (ry2 >= h) ry2 = h - 1;

    auto dx = static_cast<uint32_t>((sx - rx) * 255.0f);
    auto dy = static_cast<uint32_t>((sy - ry) * 255.0f);

    auto c1 = img[rx + ry * w];
    auto c2 = img[rx2 + ry * w];
    auto c3 = img[rx2 + ry2 * w];
    auto c4 = img[rx + ry2 * w];

    return INTERPOLATE(INTERPOLATE(c3, c4, dx), INTERPOLATE(c2, c1, dx), dy);
}


//2n x 2n Mean Kernel
static uint32_t _interpDownScaler(const uint32_t *img, uint32_t stride, uint32_t w, uint32_t h, uint32_t rx, uint32_t ry, uint32_t n)
{
    uint32_t c[4] = {0, 0, 0, 0};
    auto n2 = n * n;
    auto src = img + rx - n + (ry - n) * stride;

    for (auto y = ry - n; y < ry + n; ++y) {
        if (y >= h) continue;
        auto p = src;
        for (auto x = rx - n; x < rx + n; ++x, ++p) {
            if (x >= w) continue;
            c[0] += *p >> 24;
            c[1] += (*p >> 16) & 0xff;
            c[2] += (*p >> 8) & 0xff;
            c[3] += *p & 0xff;
        }
        src += stride;
    }
    for (auto i = 0; i < 4; ++i) {
        c[i] = (c[i] >> 2) / n2;
    }
    return (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
}


void _rasterGrayscale8(uint8_t *dst, uint32_t val, uint32_t offset, int32_t len)
{
    cRasterPixels(dst, val, offset, len);
}

/************************************************************************/
/* Rect                                                                 */
/************************************************************************/

static bool _rasterMaskedRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    //32bit channels composition
    if (surface->channelSize != sizeof(uint32_t)) return false;

    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * surface->compositor->image.stride + region.min.x);   //compositor buffer
    auto cstride = surface->compositor->image.stride;
    auto color = surface->blender.join(r, g, b, a);
    auto ialpha = 255 - a;
    auto method = surface->compositor->method;

    TVGLOG("SW_ENGINE", "Masked(%d) Rect [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    if (method == CompositeMethod::AddMask) {
        for (uint32_t y = 0; y < h; ++y) {
            auto cmp = cbuffer;
            for (uint32_t x = 0; x < w; ++x, ++cmp) {
                *cmp = color + ALPHA_BLEND(*cmp, ialpha);
            }
            cbuffer += cstride;
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t y = 0; y < h; ++y) {
            auto cmp = cbuffer;
            for (uint32_t x = 0; x < w; ++x, ++cmp) {
                *cmp = ALPHA_BLEND(*cmp, ialpha);
            }
            cbuffer += cstride;
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = surface->compositor->image.buf32 + (y * cstride + surface->compositor->bbox.min.x);
            if (y == region.min.y) {
                for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                    auto tmp = cmp;
                    auto x = surface->compositor->bbox.min.x;
                    while (x < surface->compositor->bbox.max.x) {
                        if (x == region.min.x) {
                            for (uint32_t i = 0; i < w; ++i, ++tmp) {
                                *tmp = ALPHA_BLEND(*tmp, a);
                            }
                            x += w;
                        } else {
                            *tmp = 0;
                            ++tmp;
                            ++x;
                        }
                    }
                    cmp += cstride;
                }
                y += (h - 1);
            } else {
                rasterRGBA32(cmp, 0x00000000, 0, w);
                cmp += cstride;
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t y = 0; y < h; ++y) {
            auto cmp = cbuffer;
            for (uint32_t x = 0; x < w; ++x, ++cmp) {
                *cmp = ALPHA_BLEND(color, IALPHA(*cmp)) + ALPHA_BLEND(*cmp, ialpha);
            }
            cbuffer += cstride;
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterMattedRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + ((region.min.y * surface->compositor->image.stride + region.min.x) * csize);   //compositor buffer
    auto alpha = surface->blender.alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Matted(%d) Rect [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);
    
    //32bits channels
    if (surface->channelSize == sizeof(uint32_t)) {
        auto color = surface->blender.join(r, g, b, a);
        auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
        for (uint32_t y = 0; y < h; ++y) {
            auto dst = &buffer[y * surface->stride];
            auto cmp = &cbuffer[y * surface->compositor->image.stride * csize];
            for (uint32_t x = 0; x < w; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE(color, *dst, alpha(cmp));
            }
        }
    //8bits grayscale
    } else if (surface->channelSize == sizeof(uint8_t)) {
        auto buffer = surface->buf8 + (region.min.y * surface->stride) + region.min.x;
        for (uint32_t y = 0; y < h; ++y) {
            auto dst = &buffer[y * surface->stride];
            auto cmp = &cbuffer[y * surface->compositor->image.stride * csize];
            for (uint32_t x = 0; x < w; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE8(a, *dst, alpha(cmp));
            }
        }
    }
    return true;
}


static bool _rasterSolidRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b)
{
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);

    //32bits channels
    if (surface->channelSize == sizeof(uint32_t)) {
        auto color = surface->blender.join(r, g, b, 255);
        auto buffer = surface->buf32 + (region.min.y * surface->stride);
        for (uint32_t y = 0; y < h; ++y) {
            rasterRGBA32(buffer + y * surface->stride, color, region.min.x, w);
        }
        return true;
    //8bits grayscale
    }
    if (surface->channelSize == sizeof(uint8_t)) {
        auto buffer = surface->buf8 + (region.min.y * surface->stride);
        for (uint32_t y = 0; y < h; ++y) {
            _rasterGrayscale8(buffer + y * surface->stride, 255, region.min.x, w);
        }
        return true;
    }
    return false;
}


static bool _rasterRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterMattedRect(surface, region, r, g, b, a);
        else return _rasterMaskedRect(surface, region, r, g, b, a);
    } else {
        if (a == 255) {
            return _rasterSolidRect(surface, region, r, g, b);
        } else {
#if defined(THORVG_AVX_VECTOR_SUPPORT)
            return avxRasterTranslucentRect(surface, region, r, g, b, a);
#elif defined(THORVG_NEON_VECTOR_SUPPORT)
            return neonRasterTranslucentRect(surface, region, r, g, b, a);
#else
            return cRasterTranslucentRect(surface, region, r, g, b, a);
#endif
        }
    }
    return false;
}


/************************************************************************/
/* Rle                                                                  */
/************************************************************************/

static bool _rasterMaskedRle(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    TVGLOG("SW_ENGINE", "Masked(%d) Rle", (int)surface->compositor->method);

    //32bit channels composition
    if (surface->channelSize != sizeof(uint32_t)) return false;

    auto span = rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto cstride = surface->compositor->image.stride;
    auto color = surface->blender.join(r, g, b, a);
    auto method = surface->compositor->method;
    uint32_t src;

    if (method == CompositeMethod::AddMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            if (span->coverage == 255) src = color;
            else src = ALPHA_BLEND(color, span->coverage);
            auto ialpha = IALPHA(src);
            for (auto x = 0; x < span->len; ++x, ++cmp) {
                *cmp = src + ALPHA_BLEND(*cmp, ialpha);
            }
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            if (span->coverage == 255) src = color;
            else src = ALPHA_BLEND(color, span->coverage);
            auto ialpha = IALPHA(src);
            for (auto x = 0; x < span->len; ++x, ++cmp) {
                *cmp = ALPHA_BLEND(*cmp, ialpha);
            }
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = &cbuffer[y * cstride];
            uint32_t x = surface->compositor->bbox.min.x;
            while (x < surface->compositor->bbox.max.x) {
                if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                    if (span->coverage == 255) src = color;
                    else src = ALPHA_BLEND(color, span->coverage);
                    auto alpha = ALPHA(src);
                    for (uint32_t i = 0; i < span->len; ++i) {
                        cmp[x + i] = ALPHA_BLEND(cmp[x + i], alpha);
                    }
                    x += span->len;
                    ++span;
                } else {
                    cmp[x] = 0;
                    ++x;
                }
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            if (span->coverage == 255) src = color;
            else src = ALPHA_BLEND(color, span->coverage);
            auto ialpha = IALPHA(src);
            for (uint32_t x = 0; x < span->len; ++x, ++cmp) {
                *cmp = ALPHA_BLEND(src, IALPHA(*cmp)) + ALPHA_BLEND(*cmp, ialpha);
            }
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterMattedRle(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    TVGLOG("SW_ENGINE", "Matted(%d) Rle", (int)surface->compositor->method);

    auto span = rle->spans;
    auto cbuffer = surface->compositor->image.buf8;
    auto csize = surface->compositor->image.channelSize;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    //32bit channels
    if (surface->channelSize == sizeof(uint32_t)) {
        uint32_t src;
        auto color = surface->blender.join(r, g, b, a);
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto dst = &surface->buf32[span->y * surface->stride + span->x];
            auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
            if (span->coverage == 255) src = color;
            else src = ALPHA_BLEND(color, span->coverage);
            for (uint32_t x = 0; x < span->len; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE(src, *dst, alpha(cmp));
            }
        }
        return true;
    }
    //8bit grayscale
    if (surface->channelSize == sizeof(uint8_t)) {
        uint8_t src;
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto dst = &surface->buf8[span->y * surface->stride + span->x];
            auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
            if (span->coverage == 255) src = a;
            else src = MULTIPLY(a, span->coverage);
            for (uint32_t x = 0; x < span->len; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE8(src, *dst, alpha(cmp));
            }
        }
        return true;
    }
    return false;
}


static bool _rasterSolidRle(SwSurface* surface, const SwRleData* rle, uint8_t r, uint8_t g, uint8_t b)
{
    auto span = rle->spans;

    //32bit channels
    if (surface->channelSize == sizeof(uint32_t)) {
        auto color = surface->blender.join(r, g, b, 255);
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            if (span->coverage == 255) {
                rasterRGBA32(surface->buf32 + span->y * surface->stride, color, span->x, span->len);
            } else {
                auto dst = &surface->buf32[span->y * surface->stride + span->x];
                auto src = ALPHA_BLEND(color, span->coverage);
                auto ialpha = 255 - span->coverage;
                for (uint32_t x = 0; x < span->len; ++x, ++dst) {
                    *dst = src + ALPHA_BLEND(*dst, ialpha);
                }
            }
        }
    //8bit grayscale
    } else if (surface->channelSize == sizeof(uint8_t)) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            if (span->coverage == 255) {
                _rasterGrayscale8(surface->buf8 + span->y * surface->stride, 255, span->x, span->len);
            } else {
                auto dst = &surface->buf8[span->y * surface->stride + span->x];
                for (uint32_t x = 0; x < span->len; ++x, ++dst) {
                    *dst = span->coverage;
                }
            }
        }
    }
    return true;
}


static bool _rasterRle(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (!rle) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterMattedRle(surface, rle, r, g, b, a);
        else return _rasterMaskedRle(surface, rle, r, g, b, a);
    } else {
        if (a == 255) {
            return _rasterSolidRle(surface, rle, r, g, b);
        } else {
#if defined(THORVG_AVX_VECTOR_SUPPORT)
            return avxRasterTranslucentRle(surface, rle, r, g, b, a);
#elif defined(THORVG_NEON_VECTOR_SUPPORT)
            return neonRasterTranslucentRle(surface, rle, r, g, b, a);
#else
            return cRasterTranslucentRle(surface, rle, r, g, b, a);
#endif
        }
    }
    return false;
}


/************************************************************************/
/* RLE Transformed RGBA Image                                           */
/************************************************************************/

static bool _transformedRleRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* transform, uint32_t opacity)
{
    auto ret = _rasterTexmapPolygon(surface, image, transform, nullptr, opacity);

    //Masking Composition
    if (_compositing(surface) && _masking(surface)) {
        return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
    }

    return ret;

}


/************************************************************************/
/* RLE Scaled RGBA Image                                                */
/************************************************************************/

static bool _rasterScaledMaskedRleRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint32_t opacity, uint32_t halfScale)
{
    TVGLOG("SW_ENGINE", "Scaled Masked(%d) Rle Image", (int)surface->compositor->method);

    auto span = image->rle->spans;
    auto method = surface->compositor->method;

    if (method == CompositeMethod::AddMask) {
        #define SCALED_RLE_IMAGE_ADD_MASK
            #include "tvgSwRasterScaledMaskedRleImage.h"
        #undef SCALED_RLE_IMAGE_ADD_MASK
    } else if (method == CompositeMethod::SubtractMask) {
        #define SCALED_RLE_IMAGE_SUB_MASK
            #include "tvgSwRasterScaledMaskedRleImage.h"
        #undef SCALED_RLE_IMAGE_SUB_MASK
    } else if (method == CompositeMethod::IntersectMask) {
        #define SCALED_RLE_IMAGE_INT_MASK
            #include "tvgSwRasterScaledMaskedRleImage.h"
        #undef SCALED_RLE_IMAGE_INT_MASK
    } else if (method == CompositeMethod::DifferenceMask) {
        #define SCALED_RLE_IMAGE_DIF_MASK
            #include "tvgSwRasterScaledMaskedRleImage.h"
        #undef SCALED_RLE_IMAGE_DIF_MASK
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterScaledMattedRleRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint32_t opacity, uint32_t halfScale)
{
    TVGLOG("SW_ENGINE", "Scaled Matted(%d) Rle Image", (int)surface->compositor->method);

    auto span = image->rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    //Center (Down-Scaled)
    if (image->scale < DOWN_SCALE_TOLERANCE) {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto sy = (uint32_t)(span->y * itransform->e22 + itransform->e23);
            if (sy >= image->h) continue;
            auto dst = &surface->buf32[span->y * surface->stride + span->x];
            auto cmp = &surface->compositor->image.buf8[(span->y * surface->compositor->image.stride + span->x) * csize];
            auto a = MULTIPLY(span->coverage, opacity);
            if (a == 255) {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst, cmp += csize) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto tmp = ALPHA_BLEND(_interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale), alpha(cmp));
                    *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
                }
            } else {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst, cmp += csize) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = _interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale);
                    auto tmp = ALPHA_BLEND(src, MULTIPLY(alpha(cmp), a));
                    *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
                }
            }
        }
    //Center (Up-Scaled)
    } else {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto sy = span->y * itransform->e22 + itransform->e23;
            if ((uint32_t)sy >= image->h) continue;
            auto dst = &surface->buf32[span->y * surface->stride + span->x];
            auto cmp = &surface->compositor->image.buf8[(span->y * surface->compositor->image.stride + span->x) * csize];
            auto a = MULTIPLY(span->coverage, opacity);
            if (a == 255) {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst, cmp += csize) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto tmp = ALPHA_BLEND(_interpUpScaler(image->buf32, image->w, image->h, sx, sy), alpha(cmp));
                    *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
                }
            } else {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst, cmp += csize) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = _interpUpScaler(image->buf32, image->w, image->h, sx, sy);
                    auto tmp = ALPHA_BLEND(src, MULTIPLY(alpha(cmp), a));
                    *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
                }
            }
        }
    }
    return true;
}


static bool _rasterScaledRleRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint32_t opacity, uint32_t halfScale)
{
    auto span = image->rle->spans;

    //Center (Down-Scaled)
    if (image->scale < DOWN_SCALE_TOLERANCE) {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto sy = (uint32_t)(span->y * itransform->e22 + itransform->e23);
            if (sy >= image->h) continue;
            auto dst = &surface->buf32[span->y * surface->stride + span->x];
            auto alpha = MULTIPLY(span->coverage, opacity);
            if (alpha == 255) {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = _interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            } else {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = ALPHA_BLEND(_interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale), alpha);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            }
        }
    //Center (Up-Scaled)
    } else {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto sy = span->y * itransform->e22 + itransform->e23;
            if ((uint32_t)sy >= image->h) continue;
            auto dst = &surface->buf32[span->y * surface->stride + span->x];
            auto alpha = MULTIPLY(span->coverage, opacity);
            if (alpha == 255) {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = _interpUpScaler(image->buf32, image->w, image->h, sx, sy);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            } else {
                for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = ALPHA_BLEND(_interpUpScaler(image->buf32, image->w, image->h, sx, sy), alpha);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            }
        }
    }
    return true;
}


static bool _scaledRleRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* transform, const SwBBox& region, uint32_t opacity)
{
    Matrix itransform;

    if (transform) {
        if (!mathInverse(transform, &itransform)) return false;
    } else mathIdentity(&itransform);

    auto halfScale = _halfScale(image->scale);

    if (_compositing(surface)) {
        if (_matting(surface)) _rasterScaledMattedRleRGBAImage(surface, image, &itransform, region, opacity, halfScale);
        else _rasterScaledMaskedRleRGBAImage(surface, image, &itransform, region, opacity, halfScale);
    } else {
        return _rasterScaledRleRGBAImage(surface, image, &itransform, region, opacity, halfScale);
    }
    return false;
}


/************************************************************************/
/* RLE Direct RGBA Image                                                */
/************************************************************************/

static bool _rasterDirectMaskedRleRGBAImage(SwSurface* surface, const SwImage* image, uint32_t opacity)
{
    TVGLOG("SW_ENGINE", "Direct Masked(%d) Rle Image", (int)surface->compositor->method);

    auto span = image->rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto ctride = surface->compositor->image.stride;
    auto method = surface->compositor->method;

    if (method == CompositeMethod::AddMask) {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto src = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
            auto cmp = &cbuffer[span->y * ctride + span->x];
            auto alpha = MULTIPLY(span->coverage, opacity);
            if (alpha == 255) {
                for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                    *cmp = *src + ALPHA_BLEND(*cmp, IALPHA(*src));
                }
            } else {
                for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                    *cmp = INTERPOLATE(*src, *cmp, alpha);
                }
            }
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto src = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
            auto cmp = &cbuffer[span->y * ctride + span->x];
            auto alpha = MULTIPLY(span->coverage, opacity);
            if (alpha == 255) {
                for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                    *cmp = ALPHA_BLEND(*cmp, IALPHA(*src));
                }
            } else {
                for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                    auto t = ALPHA_BLEND(*src, alpha);
                    *cmp = ALPHA_BLEND(*cmp, IALPHA(t));
                }
            }
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = &cbuffer[y * ctride];
            auto x = surface->compositor->bbox.min.x;
            while (x < surface->compositor->bbox.max.x) {
                if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                    auto alpha = MULTIPLY(span->coverage, opacity);
                    auto src = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
                    if (alpha == 255) {
                        for (uint32_t i = 0; i < span->len; ++i, ++src) {
                            cmp[x + i] = ALPHA_BLEND(cmp[x + i], ALPHA(*src));
                        }
                    } else {
                        for (uint32_t i = 0; i < span->len; ++i, ++src) {
                            auto t = ALPHA_BLEND(*src, alpha);
                            cmp[x + i] = ALPHA_BLEND(cmp[x + i], ALPHA(t));
                        }
                    }
                    x += span->len;
                    ++span;
                } else {
                    cmp[x] = 0;
                    ++x;
                }
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
            auto src = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
            auto cmp = &cbuffer[span->y * ctride + span->x];
            auto alpha = MULTIPLY(span->coverage, opacity);
            if (alpha == 255) {
                for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                    *cmp = ALPHA_BLEND(*src, IALPHA(*cmp)) + ALPHA_BLEND(*cmp, IALPHA(*src));
                }
            } else {
                for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                    auto t = ALPHA_BLEND(*src, alpha);
                    *cmp = ALPHA_BLEND(t, IALPHA(*cmp)) + ALPHA_BLEND(*cmp, IALPHA(t));
                }
            }
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterDirectMattedRleRGBAImage(SwSurface* surface, const SwImage* image, uint32_t opacity)
{
    TVGLOG("SW_ENGINE", "Direct Matted(%d) Rle Image", (int)surface->compositor->method);

    auto span = image->rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
        auto img = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
        auto a = MULTIPLY(span->coverage, opacity);
        if (a == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img, cmp += csize) {
                auto tmp = ALPHA_BLEND(*img, alpha(cmp));
                *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
            }
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img, cmp += csize) {
                auto tmp = ALPHA_BLEND(*img, MULTIPLY(a, alpha(cmp)));
                *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
            }
        }
    }
    return true;
}


static bool _rasterDirectRleRGBAImage(SwSurface* surface, const SwImage* image, uint32_t opacity)
{
    auto span = image->rle->spans;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto img = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
        auto alpha = MULTIPLY(span->coverage, opacity);
        if (alpha == 255) {
            *dst = *img + ALPHA_BLEND(*dst, IALPHA(*img));
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img) {
                auto src = ALPHA_BLEND(*img, alpha);
                *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
            }
        }
    }
    return true;
}


static bool _directRleRGBAImage(SwSurface* surface, const SwImage* image, uint32_t opacity)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterDirectMattedRleRGBAImage(surface, image, opacity);
        else return _rasterDirectMaskedRleRGBAImage(surface, image, opacity);
    } else {
        return _rasterDirectRleRGBAImage(surface, image, opacity);
    }
    return false;
}


/************************************************************************/
/* Transformed RGBA Image                                               */
/************************************************************************/

static bool _transformedRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* transform, const SwBBox& region, uint32_t opacity)
{
    auto ret = _rasterTexmapPolygon(surface, image, transform, &region, opacity);

    //Masking Composition
    if (_compositing(surface) && _masking(surface)) {
        return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
    }

    return ret;
}


static bool _transformedRGBAImageMesh(SwSurface* surface, const SwImage* image, const RenderMesh* mesh, const Matrix* transform, const SwBBox* region, uint32_t opacity)
{
    //TODO: Not completed for all cases.
    return _rasterTexmapPolygonMesh(surface, image, mesh, transform, region, opacity);
}


/************************************************************************/
/*Scaled RGBA Image                                                     */
/************************************************************************/

static bool _rasterScaledMaskedRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint32_t opacity, uint32_t halfScale)
{
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto method = surface->compositor->method;

    TVGLOG("SW_ENGINE", "Scaled Masked(%d) Image [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    if (method == CompositeMethod::AddMask) {
        #define SCALED_IMAGE_ADD_MASK
            #include "tvgSwRasterScaledMaskedImage.h"
        #undef SCALED_IMAGE_ADD_MASK
    } else if (method == CompositeMethod::SubtractMask) {
        #define SCALED_IMAGE_SUB_MASK
            #include "tvgSwRasterScaledMaskedImage.h"
        #undef SCALED_IMAGE_SUB_MASK
    } else if (method == CompositeMethod::IntersectMask) {
        #define SCALED_IMAGE_INT_MASK
            #include "tvgSwRasterScaledMaskedImage.h"
        #undef SCALED_IMAGE_INT_MASK
    } else if (method == CompositeMethod::DifferenceMask) {
        #define SCALED_IMAGE_DIF_MASK
            #include "tvgSwRasterScaledMaskedImage.h"
        #undef SCALED_IMAGE_DIF_MASK
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterScaledMattedRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint32_t opacity, uint32_t halfScale)
{
    auto dbuffer = surface->buf32 + (region.min.y * surface->stride + region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Scaled Matted(%d) Image [Region: %lu %lu %lu %lu]", (int)surface->compositor->method, region.min.x, region.min.y, region.max.x - region.min.x, region.max.y - region.min.y);

    // Down-Scaled
    if (image->scale < DOWN_SCALE_TOLERANCE) {
        for (auto y = region.min.y; y < region.max.y; ++y) {
            auto sy = (uint32_t)(y * itransform->e22 + itransform->e23);
            if (sy >= image->h) continue;
            auto dst = dbuffer;
            auto cmp = cbuffer;
            if (opacity == 255) {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst, cmp += csize) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = _interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale);
                    auto temp = ALPHA_BLEND(src, alpha(cmp));
                    *dst = temp + ALPHA_BLEND(*dst, IALPHA(temp));
                }
            } else {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst, cmp += csize) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = _interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale);
                    auto temp = ALPHA_BLEND(src, MULTIPLY(opacity, alpha(cmp)));
                    *dst = temp + ALPHA_BLEND(*dst, IALPHA(temp));
                }
            }
            dbuffer += surface->stride;
            cbuffer += surface->compositor->image.stride * csize;
        }
    // Up-Scaled
    } else {
        for (auto y = region.min.y; y < region.max.y; ++y) {
            auto sy = y * itransform->e22 + itransform->e23;
            if ((uint32_t)sy >= image->h) continue;
            auto dst = dbuffer;
            auto cmp = cbuffer;
            if (opacity == 255) {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst, cmp += csize) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = _interpUpScaler(image->buf32, image->w, image->h, sx, sy);
                    auto temp = ALPHA_BLEND(src, alpha(cmp));
                    *dst = temp + ALPHA_BLEND(*dst, IALPHA(temp));
                }
            } else {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst, cmp += csize) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = _interpUpScaler(image->buf32, image->w, image->h, sx, sy);
                    auto temp = ALPHA_BLEND(src, MULTIPLY(opacity, alpha(cmp)));
                    *dst = temp + ALPHA_BLEND(*dst, IALPHA(temp));
                }
            }
            dbuffer += surface->stride;
            cbuffer += surface->compositor->image.stride * csize;
        }
    }
    return true;
}


static bool _rasterScaledRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint32_t opacity, uint32_t halfScale)
{
    auto dbuffer = surface->buf32 + (region.min.y * surface->stride + region.min.x);

    // Down-Scaled
    if (image->scale < DOWN_SCALE_TOLERANCE) {
        for (auto y = region.min.y; y < region.max.y; ++y, dbuffer += surface->stride) {
            auto sy = (uint32_t)(y * itransform->e22 + itransform->e23);
            if (sy >= image->h) continue;
            auto dst = dbuffer;
            if (opacity == 255) {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = _interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            } else {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                    auto sx = (uint32_t)(x * itransform->e11 + itransform->e13);
                    if (sx >= image->w) continue;
                    auto src = ALPHA_BLEND(_interpDownScaler(image->buf32, image->stride, image->w, image->h, sx, sy, halfScale), opacity);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            }
        }
    // Up-Scaled
    } else {
        for (auto y = region.min.y; y < region.max.y; ++y, dbuffer += surface->stride) {
            auto sy = fabsf(y * itransform->e22 + itransform->e23);
            if (sy >= image->h) continue;
            auto dst = dbuffer;
            if (opacity == 255) {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = _interpUpScaler(image->buf32, image->w, image->h, sx, sy);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            } else {
                for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                    auto sx = x * itransform->e11 + itransform->e13;
                    if ((uint32_t)sx >= image->w) continue;
                    auto src = ALPHA_BLEND(_interpUpScaler(image->buf32, image->w, image->h, sx, sy), opacity);
                    *dst = src + ALPHA_BLEND(*dst, IALPHA(src));
                }
            }
        }
    }
    return true;
}


static bool _scaledRGBAImage(SwSurface* surface, const SwImage* image, const Matrix* transform, const SwBBox& region, uint32_t opacity)
{
    Matrix itransform;

    if (transform) {
        if (!mathInverse(transform, &itransform)) return false;
    } else mathIdentity(&itransform);

    auto halfScale = _halfScale(image->scale);

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterScaledMattedRGBAImage(surface, image, &itransform, region, opacity, halfScale);
        else return _rasterScaledMaskedRGBAImage(surface, image, &itransform, region, opacity, halfScale);
    } else {
        return _rasterScaledRGBAImage(surface, image, &itransform, region, opacity, halfScale);
    }
    return false;
}


/************************************************************************/
/* Direct RGBA Image                                                    */
/************************************************************************/

static bool _rasterDirectMaskedRGBAImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint32_t opacity)
{
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto method = surface->compositor->method;

    TVGLOG("SW_ENGINE", "Direct Masked(%d) Image  [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    if (method == CompositeMethod::AddMask) {
        auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x); //compositor buffer
        auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);
        for (uint32_t y = 0; y < h; ++y) {
            auto cmp = cbuffer;
            auto src = sbuffer;
            if (opacity == 255) {
                for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                    *cmp = *src + ALPHA_BLEND(*cmp, IALPHA(*src));
                }
            } else {
                for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                    *cmp = INTERPOLATE(*src, *cmp, opacity);
                }
            }
            cbuffer += cstride;
            sbuffer += image->stride;
        }
    } else if (method == CompositeMethod::SubtractMask) {
        auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x); //compositor buffer
        auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);
        for (uint32_t y = 0; y < h; ++y) {
            auto cmp = cbuffer;
            auto src = sbuffer;
            if (opacity == 255) {
                for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                    *cmp = ALPHA_BLEND(*cmp, IALPHA(*src));
                }
            } else {
                for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                    auto t = ALPHA_BLEND(*src, opacity);
                    *cmp = ALPHA_BLEND(*cmp, IALPHA(t));
                }
            }
            cbuffer += cstride;
            sbuffer += image->stride;
        }
    } else if (method == CompositeMethod::IntersectMask) {
        auto cbuffer = surface->compositor->image.buf32 + (surface->compositor->bbox.min.y * cstride + surface->compositor->bbox.min.x);
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            if (y == region.min.y) {
                auto cbuffer2 = cbuffer;
                for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                    auto tmp = cbuffer2;
                    auto x = surface->compositor->bbox.min.x;
                    while (x < surface->compositor->bbox.max.x) {
                        if (x == region.min.x) {
                            auto src = &image->buf32[(y2 + image->oy) * image->stride + (x + image->ox)];
                            if (opacity == 255) {
                                for (uint32_t i = 0; i < w; ++i, ++tmp, ++src) {
                                    *tmp = ALPHA_BLEND(*tmp, ALPHA(*src));
                                }
                            } else {
                                for (uint32_t i = 0; i < w; ++i, ++tmp, ++src) {
                                    auto t = ALPHA_BLEND(*src, opacity);
                                    *tmp = ALPHA_BLEND(*tmp, ALPHA(t));
                                }
                            }
                            x += w;
                        } else {
                            *tmp = 0;
                            ++tmp;
                            ++x;
                        }
                    }
                    cbuffer2 += cstride;
                }
                y += (h - 1);
            } else {
                rasterRGBA32(cbuffer, 0x00000000, 0, surface->compositor->bbox.max.x - surface->compositor->bbox.min.x);
            }
            cbuffer += cstride;
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x); //compositor buffer
        auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);
        for (uint32_t y = 0; y < h; ++y) {
            auto cmp = cbuffer;
            auto src = sbuffer;
            if (opacity == 255) {
                for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                    *cmp = ALPHA_BLEND(*src, IALPHA(*cmp)) + ALPHA_BLEND(*cmp, IALPHA(*src));
                }
            } else {
                for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                    auto t = ALPHA_BLEND(*src, opacity);
                    *cmp = ALPHA_BLEND(t, IALPHA(*cmp)) + ALPHA_BLEND(*cmp, IALPHA(t));
                }
            }
            cbuffer += cstride;
            sbuffer += image->stride;
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterDirectMattedRGBAImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint32_t opacity)
{
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Direct Matted(%d) Image  [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize; //compositor buffer

    for (uint32_t y = 0; y < h; ++y) {
        auto dst = buffer;
        auto cmp = cbuffer;
        auto src = sbuffer;
        if (opacity == 255) {
            for (uint32_t x = 0; x < w; ++x, ++dst, ++src, cmp += csize) {
                auto tmp = ALPHA_BLEND(*src, alpha(cmp));
                *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
            }
        } else {
            for (uint32_t x = 0; x < w; ++x, ++dst, ++src, cmp += csize) {
                auto tmp = ALPHA_BLEND(*src, MULTIPLY(opacity, alpha(cmp)));
                *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
            }
        }
        buffer += surface->stride;
        cbuffer += surface->compositor->image.stride * csize;
        sbuffer += image->stride;
    }
    return true;
}


static bool _rasterDirectRGBAImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint32_t opacity)
{
    auto dbuffer = &surface->buf32[region.min.y * surface->stride + region.min.x];
    auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);

    for (auto y = region.min.y; y < region.max.y; ++y) {
        auto dst = dbuffer;
        auto src = sbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; x++, dst++, src++) {
                *dst = *src + ALPHA_BLEND(*dst, IALPHA(*src));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst, ++src) {
                auto tmp = ALPHA_BLEND(*src, opacity);
                *dst = tmp + ALPHA_BLEND(*dst, IALPHA(tmp));
            }
        }
        dbuffer += surface->stride;
        sbuffer += image->stride;
    }
    return true;
}


//Blenders for the following scenarios: [Composition / Non-Composition] * [Opaque / Translucent]
static bool _directRGBAImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint32_t opacity)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterDirectMattedRGBAImage(surface, image, region, opacity);
        else return _rasterDirectMaskedRGBAImage(surface, image, region, opacity);
    } else {
        return _rasterDirectRGBAImage(surface, image, region, opacity);
    }
    return false;
}


//Blenders for the following scenarios: [RLE / Whole] * [Direct / Scaled / Transformed]
static bool _rasterRGBAImage(SwSurface* surface, SwImage* image, const Matrix* transform, const SwBBox& region, uint32_t opacity)
{
    //RLE Image
    if (image->rle) {
        if (image->direct) return _directRleRGBAImage(surface, image, opacity);
        else if (image->scaled) return _scaledRleRGBAImage(surface, image, transform, region, opacity);
        else return _transformedRleRGBAImage(surface, image, transform, opacity);
    //Whole Image
    } else {
        if (image->direct) return _directRGBAImage(surface, image, region, opacity);
        else if (image->scaled) return _scaledRGBAImage(surface, image, transform, region, opacity);
        else return _transformedRGBAImage(surface, image, transform, region, opacity);
    }
}


/************************************************************************/
/* Rect Linear Gradient                                                 */
/************************************************************************/

static bool _rasterLinearGradientMaskedRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x);
    auto method = surface->compositor->method;

    TVGLOG("SW_ENGINE", "Masked(%d) Linear Gradient [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    if (method == CompositeMethod::AddMask) {
        for (uint32_t y = 0; y < h; ++y) {
            fillLinear(fill, cbuffer, region.min.y + y, region.min.x, w, opAddMask, 255);
            cbuffer += surface->stride;
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t y = 0; y < h; ++y) {
            fillLinear(fill, cbuffer, region.min.y + y, region.min.x, w, opSubMask, 255);
            cbuffer += surface->stride;
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = surface->compositor->image.buf32 + (y * cstride + surface->compositor->bbox.min.x);
            if (y == region.min.y) {
                for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                    auto tmp = cmp;
                    auto x = surface->compositor->bbox.min.x;
                    while (x < surface->compositor->bbox.max.x) {
                        if (x == region.min.x) {
                            fillLinear(fill, tmp, y2, x, w, opIntMask, 255);
                            x += w;
                            tmp += w;
                        } else {
                            *tmp = 0;
                            ++tmp;
                            ++x;
                        }
                    }
                    cmp += cstride;
                }
                y += (h - 1);
            } else {
                rasterRGBA32(cmp, 0x00000000, 0, surface->compositor->bbox.max.x -surface->compositor->bbox.min.x);
                cmp += cstride;
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t y = 0; y < h; ++y) {
            fillLinear(fill, cbuffer, region.min.y + y, region.min.x, w, opDifMask, 255);
            cbuffer += surface->stride;
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox, 255);
}


static bool _rasterLinearGradientMattedRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Matted(%d) Linear Gradient [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    for (uint32_t y = 0; y < h; ++y) {
        fillLinear(fill, buffer, region.min.y + y, region.min.x, w, cbuffer, alpha, csize, 255);
        buffer += surface->stride;
        cbuffer += surface->stride * csize;
    }
    return true;
}


static bool _rasterTranslucentLinearGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);

    for (uint32_t y = 0; y < h; ++y) {
        fillLinear(fill, buffer, region.min.y + y, region.min.x, w, opBlend);
        buffer += surface->stride;
    }
    return true;
}


static bool _rasterSolidLinearGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);

    for (uint32_t y = 0; y < h; ++y) {
        fillLinear(fill, buffer + y * surface->stride, region.min.y + y, region.min.x, w);
    }
    return true;
}


static bool _rasterLinearGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterLinearGradientMattedRect(surface, region, fill);
        else return _rasterLinearGradientMaskedRect(surface, region, fill);
    } else {
        if (fill->translucent) return _rasterTranslucentLinearGradientRect(surface, region, fill);
        else _rasterSolidLinearGradientRect(surface, region, fill);
    }
    return false;
}


/************************************************************************/
/* Rle Linear Gradient                                                  */
/************************************************************************/

static bool _rasterLinearGradientMaskedRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    TVGLOG("SW_ENGINE", "Masked(%d) Rle Linear Gradient", (int)surface->compositor->method);

    auto span = rle->spans;
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32;
    auto method = surface->compositor->method;

    if (method == CompositeMethod::AddMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            fillLinear(fill, cmp, span->y, span->x, span->len, opAddMask, span->coverage);
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            fillLinear(fill, cmp, span->y, span->x, span->len, opSubMask, span->coverage);
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = &cbuffer[y * cstride];
            uint32_t x = surface->compositor->bbox.min.x;
            while (x < surface->compositor->bbox.max.x) {
                if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                    fillLinear(fill, cmp, span->y, span->x, span->len, opIntMask, span->coverage);
                    x += span->len;
                    ++span;
                } else {
                    cmp[x] = 0;
                    ++x;
                }
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            fillLinear(fill, cmp, span->y, span->x, span->len, opDifMask, span->coverage);
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox, 255);
}


static bool _rasterLinearGradientMattedRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    TVGLOG("SW_ENGINE", "Matted(%d) Rle Linear Gradient", (int)surface->compositor->method);

    auto span = rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
        fillLinear(fill, dst, span->y, span->x, span->len, cmp, alpha, csize, span->coverage);
    }
    return true;
}


static bool _rasterTranslucentLinearGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) fillLinear(fill, dst, span->y, span->x, span->len, opBlend);
        else fillLinear(fill, dst, span->y, span->x, span->len, opAlphaBlend, span->coverage);
    }
    return true;
}


static bool _rasterSolidLinearGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) fillLinear(fill, dst, span->y, span->x, span->len);
        else fillLinear(fill, dst, span->y, span->x, span->len, opInterpolate, span->coverage);
    }
    return true;
}


static bool _rasterLinearGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (!rle) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterLinearGradientMattedRle(surface, rle, fill);
        else return _rasterLinearGradientMaskedRle(surface, rle, fill);
    } else {
        if (fill->translucent) return _rasterTranslucentLinearGradientRle(surface, rle, fill);
        else return _rasterSolidLinearGradientRle(surface, rle, fill);
    }
    return false;
}


/************************************************************************/
/* Rect Radial Gradient                                                 */
/************************************************************************/

static bool _rasterRadialGradientMaskedRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x);
    auto method = surface->compositor->method;

    TVGLOG("SW_ENGINE", "Masked(%d) Radial Gradient [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    if (method == CompositeMethod::AddMask) {
        for (uint32_t y = 0; y < h; ++y) {
            fillRadial(fill, cbuffer, region.min.y + y, region.min.x, w, opAddMask, 255);
            cbuffer += surface->stride;
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t y = 0; y < h; ++y) {
            fillRadial(fill, cbuffer, region.min.y + y, region.min.x, w, opSubMask, 255);
            cbuffer += surface->stride;
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = surface->compositor->image.buf32 + (y * cstride + surface->compositor->bbox.min.x);
            if (y == region.min.y) {
                for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                    auto tmp = cmp;
                    auto x = surface->compositor->bbox.min.x;
                    while (x < surface->compositor->bbox.max.x) {
                        if (x == region.min.x) {
                            fillRadial(fill, tmp, y2, x, w, opIntMask, 255);
                            x += w;
                            tmp += w;
                        } else {
                            *tmp = 0;
                            ++tmp;
                            ++x;
                        }
                    }
                    cmp += cstride;
                }
                y += (h - 1);
            } else {
                rasterRGBA32(cmp, 0x00000000, 0, w);
                cmp += cstride;
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t y = 0; y < h; ++y) {
            fillRadial(fill, cbuffer, region.min.y + y, region.min.x, w, opDifMask, 255);
            cbuffer += surface->stride;
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox, 255);
}


static bool _rasterRadialGradientMattedRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Matted(%d) Radial Gradient [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    for (uint32_t y = 0; y < h; ++y) {
        fillRadial(fill, buffer, region.min.y + y, region.min.x, w, cbuffer, alpha, csize, 255);
        buffer += surface->stride;
        cbuffer += surface->stride * csize;
    }
    return true;
}


static bool _rasterTranslucentRadialGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);

    for (uint32_t y = 0; y < h; ++y) {
        auto dst = buffer;
        fillRadial(fill, dst, region.min.y + y, region.min.x, w, opBlend);
        buffer += surface->stride;
    }
    return true;
}


static bool _rasterSolidRadialGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);

    for (uint32_t y = 0; y < h; ++y) {
        fillRadial(fill, &buffer[y * surface->stride], region.min.y + y, region.min.x, w);
    }
    return true;
}


static bool _rasterRadialGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterRadialGradientMattedRect(surface, region, fill);
        else return _rasterRadialGradientMaskedRect(surface, region, fill);
    } else {
        if (fill->translucent) return _rasterTranslucentRadialGradientRect(surface, region, fill);
        else return _rasterSolidRadialGradientRect(surface, region, fill);
    }
    return false;
}


/************************************************************************/
/* RLE Radial Gradient                                                  */
/************************************************************************/

static bool _rasterRadialGradientMaskedRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    TVGLOG("SW_ENGINE", "Masked(%d) Rle Radial Gradient", (int)surface->compositor->method);

    auto span = rle->spans;
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32;
    auto method = surface->compositor->method;

    if (method == CompositeMethod::AddMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            fillLinear(fill, cmp, span->y, span->x, span->len, opAddMask, span->coverage);
        }
    } else if (method == CompositeMethod::SubtractMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            fillLinear(fill, cmp, span->y, span->x, span->len, opSubMask, span->coverage);
        }
    } else if (method == CompositeMethod::IntersectMask) {
        for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
            auto cmp = &cbuffer[y * cstride];
            uint32_t x = surface->compositor->bbox.min.x;
            while (x < surface->compositor->bbox.max.x) {
                if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                    fillLinear(fill, cmp, span->y, span->x, span->len, opIntMask, span->coverage);
                    x += span->len;
                    ++span;
                } else {
                    cmp[x] = 0;
                    ++x;
                }
            }
        }
    } else if (method == CompositeMethod::DifferenceMask) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto cmp = &cbuffer[span->y * cstride + span->x];
            fillLinear(fill, cmp, span->y, span->x, span->len, opDifMask, span->coverage);
        }
    } else return false;

    //Masking Composition
    return _rasterDirectRGBAImage(surface, &surface->compositor->image, surface->compositor->bbox, 255);
}


static bool _rasterRadialGradientMattedRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    TVGLOG("SW_ENGINE", "Matted(%d) Rle Radial Gradient", (int)surface->compositor->method);

    auto span = rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8;
    auto alpha = surface->blender.alpha(surface->compositor->method);

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
        fillRadial(fill, dst, span->y, span->x, span->len, cmp, alpha, csize, span->coverage);
    }
    return true;
}


static bool _rasterTranslucentRadialGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) fillRadial(fill, dst, span->y, span->x, span->len, opBlend);
        else fillRadial(fill, dst, span->y, span->x, span->len, opAlphaBlend, span->coverage);
    }
    return true;
}


static bool _rasterSolidRadialGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) fillRadial(fill, dst, span->y, span->x, span->len);
        else fillRadial(fill, dst, span->y, span->x, span->len, opInterpolate, span->coverage);
    }
    return true;
}


static bool _rasterRadialGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (!rle) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterRadialGradientMattedRle(surface, rle, fill);
        else return _rasterRadialGradientMaskedRle(surface, rle, fill);
    } else {
        if (fill->translucent) _rasterTranslucentRadialGradientRle(surface, rle, fill);
        else return _rasterSolidRadialGradientRle(surface, rle, fill);
    }
    return false;
}

/************************************************************************/
/* External Class Implementation                                        */
/************************************************************************/

void rasterRGBA32(uint32_t *dst, uint32_t val, uint32_t offset, int32_t len)
{
#if defined(THORVG_AVX_VECTOR_SUPPORT)
    avxRasterRGBA32(dst, val, offset, len);
#elif defined(THORVG_NEON_VECTOR_SUPPORT)
    neonRasterRGBA32(dst, val, offset, len);
#else
    cRasterPixels<uint32_t>(dst, val, offset, len);
#endif
}


bool rasterCompositor(SwSurface* surface)
{
    //See CompositeMethod, Alpha:3, InvAlpha:4, Luma:5, InvLuma:6
    surface->blender.alphas[0] = ALPHA;
    surface->blender.alphas[1] = IALPHA;

    if (surface->cs == ColorSpace::ABGR8888 || surface->cs == ColorSpace::ABGR8888S) {
        surface->blender.join = _abgrJoin;
        surface->blender.alphas[2] = _abgrLuma;
        surface->blender.alphas[3] = _abgrInvLuma;
    } else if (surface->cs == ColorSpace::ARGB8888 || surface->cs == ColorSpace::ARGB8888S) {
        surface->blender.join = _argbJoin;
        surface->blender.alphas[2] = _argbLuma;
        surface->blender.alphas[3] = _argbInvLuma;
    } else {
        TVGERR("SW_ENGINE", "Unsupported Colorspace(%d) is expected!", surface->cs);
        return false;
    }
    return true;
}


bool rasterClear(SwSurface* surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!surface || !surface->buf32 || surface->stride == 0 || surface->w == 0 || surface->h == 0) return false;

    //32 bits
    if (surface->channelSize == sizeof(uint32_t)) {
        //full clear
        if (w == surface->stride) {
            rasterRGBA32(surface->buf32 + (surface->stride * y), 0x00000000, 0, w * h);
        //partial clear
        } else {
            auto buffer = surface->buf32 + (surface->stride * y + x);
            for (uint32_t i = 0; i < h; i++) {
                rasterRGBA32(buffer + (surface->stride * i), 0x00000000, 0, w);
            }
        }
    //8 bits
    } else if (surface->channelSize == sizeof(uint8_t)) {
        //full clear
        if (w == surface->stride) {
            _rasterGrayscale8(surface->buf8 + (surface->stride * y), 0x00, 0, w * h);
        //partial clear
        } else {
            auto buffer = surface->buf8 + (surface->stride * y + x);
            for (uint32_t i = 0; i < h; i++) {
                _rasterGrayscale8(buffer + (surface->stride * i), 0x00, 0, w);
            }
        }
    }
    return true;
}


void rasterUnpremultiply(Surface* surface)
{
    if (surface->channelSize != sizeof(uint32_t)) return;

    TVGLOG("SW_ENGINE", "Unpremultiply [Size: %d x %d]", surface->w, surface->h);

    //OPTIMIZE_ME: +SIMD
    for (uint32_t y = 0; y < surface->h; y++) {
        auto buffer = surface->buf32 + surface->stride * y;
        for (uint32_t x = 0; x < surface->w; ++x) {
            uint8_t a = buffer[x] >> 24;
            if (a == 255) {
                continue;
            } else if (a == 0) {
                buffer[x] = 0x00ffffff;
            } else {
                uint16_t r = ((buffer[x] >> 8) & 0xff00) / a;
                uint16_t g = ((buffer[x]) & 0xff00) / a;
                uint16_t b = ((buffer[x] << 8) & 0xff00) / a;
                if (r > 0xff) r = 0xff;
                if (g > 0xff) g = 0xff;
                if (b > 0xff) b = 0xff;
                buffer[x] = (a << 24) | (r << 16) | (g << 8) | (b);
            }
        }
    }
    surface->premultiplied = false;
}


void rasterPremultiply(Surface* surface)
{
    if (surface->channelSize != sizeof(uint32_t)) return;

    TVGLOG("SW_ENGINE", "Premultiply [Size: %d x %d]", surface->w, surface->h);

    //OPTIMIZE_ME: +SIMD
    auto buffer = surface->buf32;
    for (uint32_t y = 0; y < surface->h; ++y, buffer += surface->stride) {
        auto dst = buffer;
        for (uint32_t x = 0; x < surface->w; ++x, ++dst) {
            auto c = *dst;
            auto a = (c >> 24);
            *dst = (c & 0xff000000) + ((((c >> 8) & 0xff) * a) & 0xff00) + ((((c & 0x00ff00ff) * a) >> 8) & 0x00ff00ff);
        }
    }
    surface->premultiplied = true;
}


bool rasterGradientShape(SwSurface* surface, SwShape* shape, unsigned id)
{
    if (surface->channelSize == sizeof(uint8_t)) {
        TVGERR("SW_ENGINE", "Not supported grayscale gradient!");
        return false;
    }

    if (!shape->fill) return false;

    if (shape->fastTrack) {
        if (id == TVG_CLASS_ID_LINEAR) return _rasterLinearGradientRect(surface, shape->bbox, shape->fill);
        else if (id == TVG_CLASS_ID_RADIAL)return _rasterRadialGradientRect(surface, shape->bbox, shape->fill);
    } else {
        if (id == TVG_CLASS_ID_LINEAR) return _rasterLinearGradientRle(surface, shape->rle, shape->fill);
        else if (id == TVG_CLASS_ID_RADIAL) return _rasterRadialGradientRle(surface, shape->rle, shape->fill);
    }
    return false;
}


bool rasterGradientStroke(SwSurface* surface, SwShape* shape, unsigned id)
{
    if (surface->channelSize == sizeof(uint8_t)) {
        TVGERR("SW_ENGINE", "Not supported grayscale gradient!");
        return false;
    }

    if (!shape->stroke || !shape->stroke->fill || !shape->strokeRle) return false;

    if (id == TVG_CLASS_ID_LINEAR) return _rasterLinearGradientRle(surface, shape->strokeRle, shape->stroke->fill);
    else if (id == TVG_CLASS_ID_RADIAL) return _rasterRadialGradientRle(surface, shape->strokeRle, shape->stroke->fill);

    return false;
}


bool rasterShape(SwSurface* surface, SwShape* shape, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 255) {
        r = MULTIPLY(r, a);
        g = MULTIPLY(g, a);
        b = MULTIPLY(b, a);
    }

    if (shape->fastTrack) return _rasterRect(surface, shape->bbox, r, g, b, a);
    else return _rasterRle(surface, shape->rle, r, g, b, a);
}


bool rasterStroke(SwSurface* surface, SwShape* shape, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 255) {
        r = MULTIPLY(r, a);
        g = MULTIPLY(g, a);
        b = MULTIPLY(b, a);
    }

    return _rasterRle(surface, shape->strokeRle, r, g, b, a);
}


bool rasterImage(SwSurface* surface, SwImage* image, const RenderMesh* mesh, const Matrix* transform, const SwBBox& bbox, uint32_t opacity)
{
    if (surface->channelSize == sizeof(uint8_t)) {
        TVGERR("SW_ENGINE", "Not supported grayscale image!");
        return false;
    }

    //Verify Boundary
    if (bbox.max.x < 0 || bbox.max.y < 0 || bbox.min.x >= static_cast<SwCoord>(surface->w) || bbox.min.y >= static_cast<SwCoord>(surface->h)) return false;

    //TOOD: switch (image->format)
    //TODO: case: _rasterRGBImageMesh()
    //TODO: case: _rasterGrayscaleImageMesh()
    //TODO: case: _rasterAlphaImageMesh()
    if (mesh && mesh->triangleCnt > 0) return _transformedRGBAImageMesh(surface, image, mesh, transform, &bbox, opacity);
    else return _rasterRGBAImage(surface, image, transform, bbox, opacity);
}


bool rasterConvertCS(Surface* surface, ColorSpace to)
{
    //TOOD: Support SIMD accelerations
    auto from = surface->cs;

    if ((from == ColorSpace::ABGR8888 && to == ColorSpace::ARGB8888) || (from == ColorSpace::ABGR8888S && to == ColorSpace::ARGB8888S)) {
        surface->cs = to;
        return cRasterABGRtoARGB(surface);
    }
    if ((from == ColorSpace::ARGB8888 && to == ColorSpace::ABGR8888) || (from == ColorSpace::ARGB8888S && to == ColorSpace::ABGR8888S)) {
        surface->cs = to;
        return cRasterARGBtoABGR(surface);
    }

    return false;
}