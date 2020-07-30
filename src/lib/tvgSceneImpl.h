/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd All Rights Reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *               http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
#ifndef _TVG_SCENE_IMPL_H_
#define _TVG_SCENE_IMPL_H_

#include "tvgCommon.h"

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/

struct Scene::Impl
{
    vector<Paint*> paints;

    bool dispose(RenderMethod& renderer)
    {
        for (auto paint : paints) {
            paint->IMPL->dispose(renderer);
            delete(paint);
        }
        paints.clear();

        return true;
    }

    bool update(RenderMethod &renderer, const RenderTransform* transform, RenderUpdateFlag flag)
    {
        for(auto paint: paints) {
            if (!paint->IMPL->update(renderer, transform, static_cast<uint32_t>(flag))) return false;
        }
        return true;
    }

    bool render(RenderMethod &renderer)
    {
        for(auto paint: paints) {
            if(!paint->IMPL->render(renderer)) return false;
        }
        return true;
    }

    bool bounds(float* px, float* py, float* pw, float* ph)
    {
        auto x = FLT_MAX;
        auto y = FLT_MAX;
        auto w = 0.0f;
        auto h = 0.0f;

        for(auto paint: paints) {
            auto x2 = FLT_MAX;
            auto y2 = FLT_MAX;
            auto w2 = 0.0f;
            auto h2 = 0.0f;

            if (paint->IMPL->bounds(&x2, &y2, &w2, &h2)) return false;

            //Merge regions
            if (x2 < x) x = x2;
            if (x + w < x2 + w2) w = (x2 + w2) - x;
            if (y2 < y) y = x2;
            if (y + h < y2 + h2) h = (y2 + h2) - y;
        }

        if (px) *px = x;
        if (py) *py = y;
        if (pw) *pw = w;
        if (ph) *ph = h;

        return true;
    }
};

#endif //_TVG_SCENE_IMPL_H_