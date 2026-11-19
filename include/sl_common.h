/*
* Copyright (c) 2026
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#pragma once

#include <cstdint>

#include "sl.h"
#include "sl_result.h"

//! Host-called entry points of the common plugin for hosts that present outside Streamline.
//!
//! Resolve with slGetFeatureFunction(sl::kFeatureCommon, "slCommonFrameEnd", fn) after the
//! device is set (see SL_FEATURE_FUN_IMPORT in sl.h).
//!
//! Use these only when no Streamline swap chain presents. Streamline then never sees a Present,
//! so frame counting, deferred destroys, pool GC and the VRAM budget would otherwise never run.
//! Never call slCommonFrameEnd while a Streamline swap chain also presents: the frame would
//! advance twice. The plugin logs an error if it observes both.

namespace sl
{

//! Call once per real host Present. DXGI_PRESENT_TEST presents are ignored.
using PFun_slCommonFrameEnd = sl::Result(uint32_t presentFlags);

//! Call before the host reallocates resources shared with Streamline (swap chain resize).
using PFun_slCommonResize = sl::Result();

} // namespace sl
