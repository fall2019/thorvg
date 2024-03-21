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

#ifndef _TVG_LOTTIE_EXPRESSIONS_H_
#define _TVG_LOTTIE_EXPRESSIONS_H_

#include "tvgCommon.h"

struct LottieComposition;
struct LottieLayer;
struct LottieObject;
struct LottieExpression;


#ifdef THORVG_LOTTIE_EXPRESSIONS_SUPPORT

#include "jerryscript.h"

struct LottieExpressions
{
public:
    //reserved specifieres
    static constexpr char CONTENT[] = "content";
    static constexpr char POSITION[] = "position";
    static constexpr char ROTATION[] = "rotation";
    static constexpr char SCALE[] = "scale";
    static constexpr char TRANSFORM[] = "transform";

    LottieExpressions();
    ~LottieExpressions();

    //TODO: generalize
    bool dispatch(float frameNo, LottieExpression* exp);
    bool dispatchFloat(float frameNo, float& out, LottieExpression* exp);
    bool dispatchPathSet(float frameNo, Array<PathCommand>& cmds, Array<Point>& pts, LottieExpression* exp);

    void prepare(LottieComposition* comp);
    void update(float frameNo);

private:
    jerry_value_t evaluate(float frameNo, LottieExpression* exp);

    LottieComposition* comp;

    jerry_value_t global;
    jerry_value_t thisProperty;
    jerry_value_t thisLayer;
    jerry_value_t content;  //content("name")  #look for the named object
};

#else

struct LottieExpressions
{
    LottieExpressions() {}

    bool dispatch(TVG_UNUSED float frameNo, TVG_UNUSED LottieExpression* exp) { return true; }
    bool dispatchFloat(TVG_UNUSED float frameNo, TVG_UNUSED float& out, TVG_UNUSED LottieExpression* exp) { return false; }
    bool dispatchPathSet(TVG_UNUSED float frameNo, TVG_UNUSED Array<PathCommand>& cmds, TVG_UNUSED Array<Point>& pts, TVG_UNUSED LottieExpression* exp) { return true; }
    void prepare(TVG_UNUSED LottieComposition* comp) {}
    void update(TVG_UNUSED float frameNo) {}
};

#endif //THORVG_LOTTIE_EXPRESSIONS_SUPPORT

#endif //_TVG_LOTTIE_EXPRESSIONS_H_