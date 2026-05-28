/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "include/UdfpsHandler.h"

#include <dlfcn.h>
#include <log/log.h>

#define UDFPS_HANDLER_LIB_NAME "/vendor/lib64/libudfpshandler.so"
#define UDFPS_HANDLER_FACTORY_SYMBOL "UDFPS_HANDLER_FACTORY"

extern "C" UdfpsHandlerFactory* getUdfpsHandlerFactory() {
    void* handle = dlopen(UDFPS_HANDLER_LIB_NAME, RTLD_NOW);
    if (!handle) {
        ALOGE("UDFPS: dlopen failed: %s", dlerror());
        return nullptr;
    }

    auto* factory = reinterpret_cast<UdfpsHandlerFactory*>(
        dlsym(handle, UDFPS_HANDLER_FACTORY_SYMBOL)
    );

    if (!factory) {
        ALOGE("UDFPS: dlsym failed: %s", dlerror());
        dlclose(handle);
        return nullptr;
    }

    return factory;
}
