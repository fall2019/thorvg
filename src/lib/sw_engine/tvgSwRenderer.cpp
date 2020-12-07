/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd. All rights reserved.

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
#include <math.h>
#include "tvgSwCommon.h"
#include "tvgTaskScheduler.h"
#include "tvgSwRenderer.h"

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/
static bool initEngine = false;
static uint32_t rendererCnt = 0;

struct CompositeCtx
{
    SwSurface surface;
    SwSurface* recover;
    SwImage image;
};


struct SwTask : Task
{
    Matrix* transform = nullptr;
    SwSurface* surface = nullptr;
    RenderUpdateFlag flags = RenderUpdateFlag::None;
    vector<Composite> compList;
    uint32_t opacity;

    virtual bool dispose() = 0;
};


struct SwShapeTask : SwTask
{
    SwShape shape;
    const Shape* sdata = nullptr;
    bool compStroking;

    void run(unsigned tid) override
    {
        if (opacity == 0) return;  //Invisible

        /* Valid filling & stroking each increases the value by 1.
           This value is referenced for compositing shape & stroking. */
        uint32_t addStroking = 0;

        //Valid Stroking?
        uint8_t strokeAlpha = 0;
        auto strokeWidth = sdata->strokeWidth();
        if (HALF_STROKE(strokeWidth) > 0) {
            sdata->strokeColor(nullptr, nullptr, nullptr, &strokeAlpha);
        }

        SwSize clip = {static_cast<SwCoord>(surface->w), static_cast<SwCoord>(surface->h)};

        //invisible shape turned to visible by alpha.
        auto prepareShape = false;
        if (!shapePrepared(&shape) && ((flags & RenderUpdateFlag::Color) || (opacity > 0))) prepareShape = true;

        //Shape
        if (flags & (RenderUpdateFlag::Path | RenderUpdateFlag::Transform) || prepareShape) {
            uint8_t alpha = 0;
            sdata->fillColor(nullptr, nullptr, nullptr, &alpha);
            alpha = static_cast<uint8_t>(static_cast<uint32_t>(alpha) * opacity / 255);
            bool renderShape = (alpha > 0 || sdata->fill());
            if (renderShape || strokeAlpha) {
                shapeReset(&shape);
                if (!shapePrepare(&shape, sdata, tid, clip, transform)) goto end;
                if (renderShape) {
                    /* We assume that if stroke width is bigger than 2,
                       shape outline below stroke could be full covered by stroke drawing.
                       Thus it turns off antialising in that condition. */
                    auto antiAlias = (strokeAlpha == 255 && strokeWidth > 2) ? false : true;
                    if (!shapeGenRle(&shape, sdata, clip, antiAlias, compList.size() > 0 ? true : false)) goto end;
                    ++addStroking;
                }
            }
        }

        //Fill
        if (flags & (RenderUpdateFlag::Gradient | RenderUpdateFlag::Transform)) {
            auto fill = sdata->fill();
            if (fill) {
                auto ctable = (flags & RenderUpdateFlag::Gradient) ? true : false;
                if (ctable) shapeResetFill(&shape);
                if (!shapeGenFillColors(&shape, fill, transform, surface, ctable)) goto end;
            } else {
                shapeDelFill(&shape);
            }
        }

        //Stroke
        if (flags & (RenderUpdateFlag::Stroke | RenderUpdateFlag::Transform)) {
            if (strokeAlpha > 0) {
                shapeResetStroke(&shape, sdata, transform);
                if (!shapeGenStrokeRle(&shape, sdata, tid, transform, clip)) goto end;
                ++addStroking;
            } else {
                shapeDelStroke(&shape);
            }
        }

        //Composition
        for (auto comp : compList) {
            if (comp.method == CompositeMethod::ClipPath) {
                auto compShape = &static_cast<SwShapeTask*>(comp.edata)->shape;
                //Clip shape rle
                if (shape.rle) {
                    if (compShape->rect) rleClipRect(shape.rle, &compShape->bbox);
                    else if (compShape->rle) rleClipPath(shape.rle, compShape->rle);
                }
                //Clip stroke rle
                if (shape.strokeRle) {
                    if (compShape->rect) rleClipRect(shape.strokeRle, &compShape->bbox);
                    else if (compShape->rle) rleClipPath(shape.strokeRle, compShape->rle);
                }
            }
        }
    end:
        shapeDelOutline(&shape, tid);
        if (addStroking == 2 && opacity < 255) compStroking = true;
        else compStroking = false;
    }

    bool dispose() override
    {
       shapeFree(&shape);
       return true;
    }
};


struct SwImageTask : SwTask
{
    SwImage image;
    const Picture* pdata = nullptr;
    uint32_t* pixels = nullptr;

    void run(unsigned tid) override
    {
        SwSize clip = {static_cast<SwCoord>(surface->w), static_cast<SwCoord>(surface->h)};

        //Invisible shape turned to visible by alpha.
        auto prepareImage = false;
        if (!imagePrepared(&image) && ((flags & RenderUpdateFlag::Image) || (opacity > 0))) prepareImage = true;

        if (prepareImage) {
            imageReset(&image);
            if (!imagePrepare(&image, pdata, tid, clip, transform)) goto end;

            //Composition?
            if (compList.size() > 0) {
                if (!imageGenRle(&image, pdata, clip, false, true)) goto end;
                if (image.rle) {
                    for (auto comp : compList) {
                        if (comp.method == CompositeMethod::ClipPath) {
                            auto compShape = &static_cast<SwShapeTask*>(comp.edata)->shape;
                            if (compShape->rect) rleClipRect(image.rle, &compShape->bbox);
                            else if (compShape->rle) rleClipPath(image.rle, compShape->rle);
                        }
                     }
                }
            }
        }
        if (pixels) image.data = pixels;
    end:
        imageDelOutline(&image, tid);
    }

    bool dispose() override
    {
       imageFree(&image);
       return true;
    }
};


static void _termEngine()
{
    if (rendererCnt > 0) return;

    mpoolTerm();
}


/************************************************************************/
/* External Class Implementation                                        */
/************************************************************************/

SwRenderer::~SwRenderer()
{
    clear();

    if (mainSurface) delete(mainSurface);

    --rendererCnt;
    if (!initEngine) _termEngine();
}


bool SwRenderer::clear()
{
    for (auto task : tasks) task->done();
    tasks.clear();

    return true;
}


bool SwRenderer::sync()
{
    return true;
}


bool SwRenderer::target(uint32_t* buffer, uint32_t stride, uint32_t w, uint32_t h, uint32_t cs)
{
    if (!buffer || stride == 0 || w == 0 || h == 0) return false;

    if (!mainSurface) {
        mainSurface = new SwSurface;
        if (!mainSurface) return false;
    }

    mainSurface->buffer = buffer;
    mainSurface->stride = stride;
    mainSurface->w = w;
    mainSurface->h = h;
    mainSurface->cs = cs;

    return rasterCompositor(mainSurface);
}


bool SwRenderer::preRender()
{
    return rasterClear(mainSurface);
}


bool SwRenderer::postRender()
{
    tasks.clear();

    //Clear Composite Surface
    if (compSurface) {
        if (compSurface->buffer) free(compSurface->buffer);
        delete(compSurface);
    }
    compSurface = nullptr;

    return true;
}


bool SwRenderer::render(TVG_UNUSED const Picture& picture, void *data)
{
    auto task = static_cast<SwImageTask*>(data);
    task->done();

    return rasterImage(mainSurface, &task->image, task->transform, task->opacity);
}


void* SwRenderer::beginComposite(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    auto ctx = new CompositeCtx;
    if (!ctx) return nullptr;

    //SwImage, Optimize Me: Surface size from MainSurface(WxH) to Parameter W x H
    ctx->image.data = (uint32_t*) malloc(sizeof(uint32_t) * mainSurface->w * mainSurface->h);
    if (!ctx->image.data) {
        delete(ctx);
        return nullptr;
    }

    //Boundary Check
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > mainSurface->w) w = (mainSurface->w - x);
    if (y + h > mainSurface->h) h = (mainSurface->h - y);

    //FIXME: Should be removed if xywh is proper.
    x = 0;
    y = 0;
    w = mainSurface->w;
    h = mainSurface->h;

    ctx->image.bbox.min.x = x;
    ctx->image.bbox.min.y = y;
    ctx->image.bbox.max.x = x + w;
    ctx->image.bbox.max.y = y + h;
    ctx->image.w = mainSurface->w;
    ctx->image.h = mainSurface->h;

    //Inherits attributes from main surface
    ctx->surface.comp = mainSurface->comp;
    ctx->surface.stride = mainSurface->w;
    ctx->surface.cs = mainSurface->cs;

    //We know partial clear region
    ctx->surface.buffer = ctx->image.data + (ctx->surface.stride * y + x);
    ctx->surface.w = w;
    ctx->surface.h = h;

    rasterClear(&ctx->surface);

    //Recover context
    ctx->surface.buffer = ctx->image.data;
    ctx->surface.w = ctx->image.w;
    ctx->surface.h = ctx->image.h;

    //Switch render target
    ctx->recover = mainSurface;
    mainSurface = &ctx->surface;

    return ctx;
}


bool SwRenderer::endComposite(void* p, uint32_t opacity)
{
    if (!p) return false;
    auto ctx = static_cast<CompositeCtx*>(p);

    //Recover render target
    mainSurface = ctx->recover;

    auto ret = rasterImage(mainSurface, &ctx->image, nullptr, opacity);

    //Free resources
    free(ctx->image.data);
    delete(ctx);

    return ret;
}


bool SwRenderer::prepareComposite(const SwShapeTask* task, SwImage* image)
{
    if (!compSurface) {
        compSurface = new SwSurface;
        if (!compSurface) return false;
        *compSurface = *mainSurface;
        compSurface->buffer = (uint32_t*) malloc(sizeof(uint32_t) * mainSurface->w * mainSurface->h);
        if (!compSurface->buffer) {
            delete(compSurface);
            compSurface = nullptr;
            return false;
        }
    }

    //Setup SwImage to return
    image->data = compSurface->buffer;
    image->w = compSurface->w;
    image->h = compSurface->h;
    image->rle = nullptr;

    //Add stroke size to bounding box.
    auto strokeWidth = static_cast<SwCoord>(ceilf(task->sdata->strokeWidth() * 0.5f));
    image->bbox.min.x = task->shape.bbox.min.x - strokeWidth;
    image->bbox.min.y = task->shape.bbox.min.y - strokeWidth;
    image->bbox.max.x = task->shape.bbox.max.x + strokeWidth;
    image->bbox.max.y = task->shape.bbox.max.y + strokeWidth;

    if (image->bbox.min.x < 0) image->bbox.min.x = 0;
    if (image->bbox.min.y < 0) image->bbox.min.y = 0;
    if (image->bbox.max.x > image->w) image->bbox.max.x = image->w;
    if (image->bbox.max.y > image->h) image->bbox.max.y = image->h;

    //We know partial clear region
    compSurface->buffer = compSurface->buffer + (compSurface->stride * image->bbox.min.y) + image->bbox.min.x;
    compSurface->w = image->bbox.max.x - image->bbox.min.x;
    compSurface->h = image->bbox.max.y - image->bbox.min.y;

    rasterClear(compSurface);

    //Recover context
    compSurface->buffer = image->data;
    compSurface->w = image->w;
    compSurface->h = image->h;

    return true;
}


bool SwRenderer::render(TVG_UNUSED const Shape& shape, void *data)
{
    auto task = static_cast<SwShapeTask*>(data);
    task->done();
    
    if (task->opacity == 0) return true;

    SwSurface* renderTarget;
    SwImage image;
    uint32_t opacity;

    //Do Composition
    if (task->compStroking) {
        if (!prepareComposite(task, &image)) return false;
        renderTarget = compSurface;
        opacity = 255;
    //No Composition
    } else {
        renderTarget = mainSurface;
        opacity = task->opacity;
    }

    //Main raster stage
    uint8_t r, g, b, a;

    if (auto fill = task->sdata->fill()) {
        //FIXME: pass opacity to apply gradient fill?
        rasterGradientShape(renderTarget, &task->shape, fill->id());
    } else{
        task->sdata->fillColor(&r, &g, &b, &a);
        a = static_cast<uint8_t>((opacity * (uint32_t) a) / 255);
        if (a > 0) rasterSolidShape(renderTarget, &task->shape, r, g, b, a);
    }

    task->sdata->strokeColor(&r, &g, &b, &a);
    a = static_cast<uint8_t>((opacity * (uint32_t) a) / 255);
    if (a > 0) rasterStroke(renderTarget, &task->shape, r, g, b, a);

    //Composition (Shape + Stroke) stage
    if (task->compStroking) rasterImage(mainSurface, &image, nullptr, task->opacity);

    return true;
}


bool SwRenderer::dispose(void *data)
{
    auto task = static_cast<SwTask*>(data);
    if (!task) return true;

    task->done();
    task->dispose();
    if (task->transform) free(task->transform);
    delete(task);

    return true;
}


void SwRenderer::prepareCommon(SwTask* task, const RenderTransform* transform, uint32_t opacity, vector<Composite>& compList, RenderUpdateFlag flags)
{
    if (compList.size() > 0) {
        //Guarantee composition targets get ready.
        for (auto comp : compList)  static_cast<SwShapeTask*>(comp.edata)->done();
        task->compList.assign(compList.begin(), compList.end());
    }

    if (transform) {
        if (!task->transform) task->transform = static_cast<Matrix*>(malloc(sizeof(Matrix)));
        *task->transform = transform->m;
    } else {
        if (task->transform) free(task->transform);
        task->transform = nullptr;
    }

    task->opacity = opacity;
    task->surface = mainSurface;
    task->flags = flags;

    tasks.push_back(task);
    TaskScheduler::request(task);
}


void* SwRenderer::prepare(const Picture& pdata, void* data, uint32_t *pixels, const RenderTransform* transform, uint32_t opacity, vector<Composite>& compList, RenderUpdateFlag flags)
{
    //prepare task
    auto task = static_cast<SwImageTask*>(data);
    if (!task) {
        task = new SwImageTask;
        if (!task) return nullptr;
    }

    if (flags == RenderUpdateFlag::None) return task;

    //Finish previous task if it has duplicated request.
    task->done();

    task->pdata = &pdata;
    task->pixels = pixels;

    prepareCommon(task, transform, opacity, compList, flags);

    return task;
}


void* SwRenderer::prepare(const Shape& sdata, void* data, const RenderTransform* transform, uint32_t opacity, vector<Composite>& compList, RenderUpdateFlag flags)
{
    //prepare task
    auto task = static_cast<SwShapeTask*>(data);
    if (!task) {
        task = new SwShapeTask;
        if (!task) return nullptr;
    }

    if (flags == RenderUpdateFlag::None) return task;

    //Finish previous task if it has duplicated request.
    task->done();
    task->sdata = &sdata;

    prepareCommon(task, transform, opacity, compList, flags);

    return task;
}


bool SwRenderer::init(uint32_t threads)
{
    if (rendererCnt > 0) return false;
    if (initEngine) return true;

    if (!mpoolInit(threads)) return false;

    initEngine = true;

    return true;
}


bool SwRenderer::term()
{
    if (!initEngine) return true;

    initEngine = false;

   _termEngine();

    return true;
}

SwRenderer* SwRenderer::gen()
{
    ++rendererCnt;
    return new SwRenderer();
}
