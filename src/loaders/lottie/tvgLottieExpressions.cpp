/*
 * Copyright (c) 2024 the ThorVG project. All rights reserved.

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

#include <alloca.h>
#include "tvgLottieModel.h"
#include "tvgLottieExpressions.h"

#ifdef THORVG_LOTTIE_EXPRESSIONS_SUPPORT

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/


template<typename T>
static bool _dispatch(LottieExpression* exp, jerry_value_t eval)
{
    auto prop = static_cast<T*>(jerry_object_get_native_ptr(eval, nullptr));
    if (!prop) return false;
    auto target = static_cast<T*>(exp->property);
    *target = *prop;
    target->proxy = true;
    target->exp = exp;
    return true;
}


//find a target object from the source
static LottieObject* _content(LottieObject* target, const char* id)
{
    if (target->name && !strcmp(target->name, id)) return target;

    if (target->type != LottieObject::Type::Group && target->type != LottieObject::Type::Layer) return nullptr;

    //source has children, find recursively.
    auto group = static_cast<LottieGroup*>(target);

    for (auto c = group->children.begin(); c < group->children.end(); ++c) {
        if (auto ret = _content(*c, id)) return ret;
    }

    return nullptr;
}


static jerry_value_t _buildPath(const jerry_call_info_t* info, const jerry_value_t args[], const jerry_length_t argsCnt)
{
    if (argsCnt == 0) return jerry_undefined();

    auto arg0 = jerry_value_to_string(args[0]);
    auto len = jerry_string_length(arg0);
    auto name = (jerry_char_t*)alloca(len * sizeof(jerry_char_t) + 1);
    jerry_string_to_buffer(arg0, JERRY_ENCODING_UTF8, name, len);
    name[len] = '\0';

    jerry_value_free(arg0);

    auto source = static_cast<LottieObject*>(jerry_object_get_native_ptr(info->function, nullptr));
    auto target = _content(source, (char*)name);
    if (!target) return jerry_undefined();

    jerry_value_t property = jerry_object();
    jerry_object_set_native_ptr(property, nullptr, &static_cast<LottiePath*>(target)->pathset);
    jerry_object_set_sz(property, "path", property);

    return property;
}


static jerry_value_t _buildShape(const jerry_call_info_t* info, const jerry_value_t args[], const jerry_length_t argsCnt)
{
    if (argsCnt == 0) return jerry_undefined();

    auto arg0 = jerry_value_to_string(args[0]);
    auto len = jerry_string_length(arg0);
    auto name = (jerry_char_t*)alloca(len * sizeof(jerry_char_t) + 1);
    jerry_string_to_buffer(arg0, JERRY_ENCODING_UTF8, name, len);
    name[len] = '\0';

    jerry_value_free(arg0);

    auto source = static_cast<LottieObject*>(jerry_object_get_native_ptr(info->function, nullptr));
    auto target = _content(source, (char*)name);
    if (!target) return jerry_undefined();

    //find the path in the shape object
    auto shape = jerry_function_external(_buildPath);
    jerry_object_set_native_ptr(shape, nullptr, target);
    jerry_object_set_sz(shape, LottieExpressions::CONTENT, shape);

    return shape;
}


static jerry_value_t _buildContext(jerry_value_t context, LottieTransform* value)
{
    if (!value) return jerry_undefined();

    auto transform = jerry_object();

    if (jerry_value_is_null(context)) jerry_object_set_sz(transform, LottieExpressions::TRANSFORM, transform);
    else jerry_object_set_sz(context, LottieExpressions::TRANSFORM, transform);

    auto position = jerry_object();
    jerry_object_set_native_ptr(position, nullptr, &value->position);
    jerry_object_set_sz(transform, LottieExpressions::POSITION, position);
    jerry_value_free(position);

    auto rotation = jerry_object();
    jerry_object_set_native_ptr(rotation, nullptr, &value->rotation);
    jerry_object_set_sz(transform, LottieExpressions::ROTATION, rotation);
    jerry_value_free(rotation);

    auto scale = jerry_object();
    jerry_object_set_native_ptr(scale, nullptr, &value->scale);
    jerry_object_set_sz(transform, LottieExpressions::SCALE, scale);
    jerry_value_free(scale);

    return transform;
}


static jerry_value_t _buildLayer(const jerry_call_info_t* info, const jerry_value_t args[], const jerry_length_t argsCnt)
{
    if (argsCnt == 0) return jerry_undefined();

    auto arg0 = jerry_value_to_string(args[0]);
    auto len = jerry_string_length(arg0);
    auto name = (jerry_char_t*)alloca(len * sizeof(jerry_char_t) + 1);
    jerry_string_to_buffer(arg0, JERRY_ENCODING_UTF8, name, len);
    name[len] = '\0';

    jerry_value_free(arg0);

    auto source = static_cast<LottieObject*>(jerry_object_get_native_ptr(info->function, nullptr));
    auto target = _content(source, (char*)name);
    if (!target) return jerry_undefined();

    return _buildContext(jerry_null(), static_cast<LottieLayer*>(target)->transform);
}


static jerry_value_t _buildMultiply(const jerry_call_info_t* info, const jerry_value_t args[], const jerry_length_t argsCnt)
{
    if (argsCnt != 2) return jerry_undefined();

    auto arg0 = jerry_value_to_number(args[0]);
    auto arg1 = jerry_value_to_number(args[1]);

    auto ret = jerry_value_as_number(arg0) * jerry_value_as_number(arg1);

    jerry_value_free(arg0);
    jerry_value_free(arg1);

    return jerry_number(ret);
}


jerry_value_t LottieExpressions::evaluate(float frameNo, LottieExpression* exp)
{
    jerry_object_set_native_ptr(thisLayer, nullptr, exp->layer);
    jerry_object_set_native_ptr(thisProperty, nullptr, exp->property);
    jerry_object_set_native_ptr(content, nullptr, exp->layer);

    //TODO: handle this nicely.
    if (exp->object->type == LottieObject::Transform) {
        jerry_value_free(_buildContext(global, static_cast<LottieTransform*>(exp->object)));
    }

    return jerry_eval((jerry_char_t *) exp->code, strlen(exp->code), JERRY_PARSE_NO_OPTS);
}


/************************************************************************/
/* External Class Implementation                                        */
/************************************************************************/

bool LottieExpressions::dispatch(float frameNo, LottieExpression* exp)
{
    auto eval = evaluate(frameNo, exp);

    auto bm_rt = jerry_object_get_sz(global, "$bm_rt");
    auto ret = true;

    //apply the result
    if (!jerry_value_is_undefined(bm_rt)) {
        switch (exp->type) {
            case LottieProperty::Type::Point: {
                ret = _dispatch<LottiePoint>(exp, eval);
                break;
            }
            case LottieProperty::Type::Opacity: {
                ret = _dispatch<LottieOpacity>(exp, eval);
                break;
            }
            case LottieProperty::Type::Color: {
                ret = _dispatch<LottieColor>(exp, eval);
                break;
            }
            case LottieProperty::Type::ColorStop: {
                ret = _dispatch<LottieColorStop>(exp, eval);
                break;
            }
            case LottieProperty::Type::Position: {
                ret = _dispatch<LottiePosition>(exp, eval);
                break;
            }
            case LottieProperty::Type::TextDoc: {
                ret = _dispatch<LottieTextDoc>(exp, eval);
                break;
            }
            default: ret = false;
        }
    } else {
        TVGERR("LOTTIE", "Failed Expressions!");
        ret = false;
    }

    jerry_value_free(bm_rt);
    jerry_value_free(eval);

    return ret;
}


bool LottieExpressions::dispatchFloat(float frameNo, float& out, LottieExpression* exp)
{
    auto eval = evaluate(frameNo, exp);

    if (jerry_value_is_number(eval)) {
        out = jerry_value_as_number(eval);
    } else {
        auto prop = static_cast<LottieFloat*>(jerry_object_get_native_ptr(eval, nullptr));
        if (prop) out = (*prop)(frameNo);
    }

    jerry_value_free(eval);

    return true;
}


bool LottieExpressions::dispatchPathSet(float frameNo, Array<PathCommand>& cmds, Array<Point>& pts, LottieExpression* exp)
{
    auto eval = evaluate(frameNo, exp);

    auto pathset = static_cast<LottiePathSet*>(jerry_object_get_native_ptr(eval, nullptr));
    if (!pathset) return false;

    (*pathset)(frameNo, cmds, pts, *this);

    jerry_value_free(eval);

    return true;
}


LottieExpressions::LottieExpressions()
{
    jerry_init(JERRY_INIT_EMPTY);
}


LottieExpressions::~LottieExpressions()
{
    jerry_value_free(content);
    jerry_value_free(thisProperty);
    jerry_value_free(thisLayer);
    jerry_value_free(global);

    jerry_cleanup();
}


void LottieExpressions::prepare(LottieComposition* comp)
{
    this->comp = comp;

    global = jerry_current_realm();

    //content("name")  #look for the named object
    content = jerry_function_external(_buildShape);
    jerry_object_set_sz(global, LottieExpressions::CONTENT, content);

    thisLayer = jerry_object();
    jerry_object_set_sz(global, "thisLayer", thisLayer);

    thisProperty = jerry_object();
    jerry_object_set_sz(global, "thisProperty", thisProperty);

    //current composition
    auto thisComp = jerry_object();
    jerry_object_set_native_ptr(thisComp, nullptr, comp);
    jerry_object_set_sz(global, "thisComp", thisComp);

    //TODO: confirm layer??
    auto layer = jerry_function_external(_buildLayer);
    jerry_object_set_sz(thisComp, "layer", layer);
    jerry_object_set_native_ptr(layer, nullptr, comp->root);

    //$bm_mul(a, b)  #multiply a, b 
    auto bm_mul = jerry_function_external(_buildMultiply);
    jerry_object_set_sz(global, "$bm_mul", bm_mul);

    jerry_value_free(bm_mul);
    jerry_value_free(layer);
    jerry_value_free(thisComp);
}


void LottieExpressions::update(float frameNo)
{
    //time  #current time in seconds
    auto time = jerry_number((frameNo - comp->startFrame) / comp->frameCnt() * comp->duration());
    jerry_object_set_sz(global, "time", time);

    jerry_value_free(time);
}

#endif //THORVG_LOTTIE_EXPRESSIONS_SUPPORT