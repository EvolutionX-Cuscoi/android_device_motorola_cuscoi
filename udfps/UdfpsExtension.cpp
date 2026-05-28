/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <compositionengine/UdfpsExtension.h>
#include "include/UdfpsHandler.h"
#include <log/log.h>

static UdfpsHandler* gUdfpsHandler = nullptr;

// External declaration for the factory function provided by the linked library
extern "C" UdfpsHandlerFactory* getUdfpsHandlerFactory();

uint32_t getUdfpsDimZOrder(uint32_t z) {
    ALOGE("UdfpsExtension: getUdfpsDimZOrder called with z=%u", z);
    return z;
}

uint32_t getUdfpsZOrder(uint32_t z, bool touched) {
    ALOGE("UdfpsExtension: getUdfpsZOrder called with z=%u touched=%d", z, touched);
    if (!gUdfpsHandler) {
        ALOGE("UdfpsExtension: gUdfpsHandler is null, attempting to get factory");
        UdfpsHandlerFactory* factory = getUdfpsHandlerFactory();
        if (factory) {
            ALOGE("UdfpsExtension: Factory obtained, creating handler");
            gUdfpsHandler = factory->create();
            ALOGE("UdfpsExtension: Created handler instance: %p", gUdfpsHandler);
             // We can't pass a valid fingerprint_device_t here from SF, so pass nullptr.
             // The handler implementation should use a different mechanism or handle null.
            gUdfpsHandler->init(nullptr);
        } else {
            ALOGE("UdfpsExtension: Failed to get factory");
        }
    }

    if (gUdfpsHandler) {
        if (touched) {
            ALOGE("UdfpsExtension: calling onFingerDown");
            gUdfpsHandler->onFingerDown(0, 0, 0, 0);
        } else {
            ALOGE("UdfpsExtension: calling onFingerUp");
            gUdfpsHandler->onFingerUp();
        }
    }

    return z;
}

uint64_t getUdfpsUsageBits(uint64_t usageBits, bool /* touched */) {
    ALOGE("UdfpsExtension: getUdfpsUsageBits called");
    return usageBits;
}
