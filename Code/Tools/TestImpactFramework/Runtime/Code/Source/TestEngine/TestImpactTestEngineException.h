/*
 * Copyright (c) Contributors to the Open 3D Engine Project
 * 
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <TestImpactFramework/TestImpactException.h>

namespace TestImpact
{
    //! Exception for test engine runs and related operations.
    class TestEngineException : public Exception
    {
    public:
        using Exception::Exception;
    };
} // namespace TestImpact