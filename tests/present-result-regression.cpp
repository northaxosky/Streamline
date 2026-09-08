/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
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

#include <cstdio>
#include <cstring>

#include <dxgi1_2.h>

#include "source/core/sl.interposer/dxgi/dxgiPresent.h"

namespace
{

struct Invocation
{
    HRESULT beforeResult = S_OK;
    HRESULT nativeResult = S_OK;
    HRESULT afterResult = S_OK;
    bool skip = false;
    bool notifyResult = true;
    unsigned beforeCalls = 0;
    unsigned presentCalls = 0;
    unsigned present1Calls = 0;
    unsigned notifyCalls = 0;
    unsigned afterCalls = 0;
    UINT afterFlags = UINT_MAX;
    const DXGI_PRESENT_PARAMETERS* present1Parameters = nullptr;
};

HRESULT runPresent(Invocation& invocation, UINT flags)
{
    return sl::interposer::invokePresent(
        [&](bool& skip)
        {
            ++invocation.beforeCalls;
            skip = invocation.skip;
            return invocation.beforeResult;
        },
        [&]()
        {
            ++invocation.presentCalls;
            return invocation.nativeResult;
        },
        [&](bool)
        {
            ++invocation.notifyCalls;
            return invocation.notifyResult;
        },
        [&]()
        {
            ++invocation.afterCalls;
            invocation.afterFlags = flags;
            return invocation.afterResult;
        });
}

HRESULT runPresent1(Invocation& invocation, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    return sl::interposer::invokePresent(
        [&](bool& skip)
        {
            ++invocation.beforeCalls;
            skip = invocation.skip;
            return invocation.beforeResult;
        },
        [&]()
        {
            ++invocation.present1Calls;
            invocation.present1Parameters = parameters;
            return invocation.nativeResult;
        },
        [&](bool)
        {
            ++invocation.notifyCalls;
            return invocation.notifyResult;
        },
        [&]()
        {
            ++invocation.afterCalls;
            invocation.afterFlags = flags;
            return invocation.afterResult;
        });
}

bool check(const char* method, const char* name, HRESULT actual, HRESULT expected,
    const Invocation& invocation, UINT flags, unsigned nativeCalls, unsigned notifyCalls, unsigned afterCalls)
{
    const bool isPresent1 = std::strcmp(method, "Present1") == 0;
    const unsigned actualNativeCalls = isPresent1 ? invocation.present1Calls : invocation.presentCalls;
    const bool correctNativeMethod = isPresent1 ? invocation.presentCalls == 0 : invocation.present1Calls == 0;
    const bool correctParameters = !isPresent1 || nativeCalls == 0 || invocation.present1Parameters != nullptr;
    const bool correctFlags = afterCalls == 0 ? invocation.afterFlags == UINT_MAX : invocation.afterFlags == flags;
    const bool passed = actual == expected &&
        invocation.beforeCalls == 1 &&
        actualNativeCalls == nativeCalls &&
        correctNativeMethod &&
        invocation.notifyCalls == notifyCalls &&
        invocation.afterCalls == afterCalls &&
        correctParameters &&
        correctFlags;
    if (!passed)
    {
        std::printf(
            "%s %s failed: result=0x%08lX expected=0x%08lX before=%u native=%u/%u notify=%u/%u after=%u/%u\n",
            method, name, static_cast<unsigned long>(actual), static_cast<unsigned long>(expected),
            invocation.beforeCalls, actualNativeCalls, nativeCalls, invocation.notifyCalls, notifyCalls,
            invocation.afterCalls, afterCalls);
    }
    return passed;
}

template<typename Invoke>
bool runCases(const char* method, Invoke&& invoke)
{
    struct Case
    {
        const char* name;
        HRESULT nativeResult;
        UINT flags;
        unsigned afterCalls;
    };
    const Case cases[] = {
        { "success", S_OK, 0, 1 },
        { "retry", DXGI_ERROR_WAS_STILL_DRAWING, DXGI_PRESENT_DO_NOT_WAIT, 0 },
        { "occluded", DXGI_STATUS_OCCLUDED, 0, 1 },
        { "device-removed", DXGI_ERROR_DEVICE_REMOVED, 0, 0 },
        { "generic-failure", E_FAIL, 0, 0 },
        { "test-occluded", DXGI_STATUS_OCCLUDED, DXGI_PRESENT_TEST, 1 },
    };

    bool passed = true;
    for (const auto& test : cases)
    {
        Invocation invocation{};
        invocation.nativeResult = test.nativeResult;
        const HRESULT actual = invoke(invocation, test.flags);
        passed &= check(method, test.name, actual, test.nativeResult, invocation,
            test.flags, 1, 1, test.afterCalls);
    }

    {
        Invocation invocation{};
        invocation.afterResult = E_ABORT;
        const HRESULT actual = invoke(invocation, 0);
        passed &= check(method, "after-failure", actual, E_ABORT, invocation, 0, 1, 1, 1);
    }
    {
        Invocation invocation{};
        invocation.nativeResult = DXGI_ERROR_WAS_STILL_DRAWING;
        invocation.afterResult = E_ABORT;
        const HRESULT actual = invoke(invocation, DXGI_PRESENT_DO_NOT_WAIT);
        passed &= check(method, "native-failure-precedes-after", actual,
            DXGI_ERROR_WAS_STILL_DRAWING, invocation, DXGI_PRESENT_DO_NOT_WAIT, 1, 1, 0);
    }
    {
        Invocation invocation{};
        invocation.skip = true;
        invocation.beforeResult = S_FALSE;
        const HRESULT actual = invoke(invocation, 0);
        passed &= check(method, "before-skip", actual, S_FALSE, invocation, 0, 0, 1, 1);
    }
    {
        Invocation invocation{};
        invocation.beforeResult = E_INVALIDARG;
        const HRESULT actual = invoke(invocation, 0);
        passed &= check(method, "before-failure", actual, E_INVALIDARG, invocation, 0, 0, 0, 0);
    }
    {
        Invocation invocation{};
        invocation.notifyResult = false;
        const HRESULT actual = invoke(invocation, 0);
        passed &= check(method, "secondary-swapchain", actual, S_OK, invocation, 0, 1, 1, 0);
    }
    return passed;
}

}

int main()
{
    const bool presentPassed = runCases("Present", [](Invocation& invocation, UINT flags)
    {
        return runPresent(invocation, flags);
    });
    const bool present1Passed = runCases("Present1", [](Invocation& invocation, UINT flags)
    {
        DXGI_PRESENT_PARAMETERS parameters{};
        return runPresent1(invocation, flags, &parameters);
    });
    return presentPassed && present1Passed ? 0 : 1;
}
