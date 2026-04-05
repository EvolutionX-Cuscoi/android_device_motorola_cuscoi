/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <compositionengine/UdfpsExtension.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define LHBM_NODE "/sys/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/card0/card0-DSI-1/local_hbm"

static std::atomic<uint64_t> sLastTouchMs(0);
static std::atomic<bool> sLhbmEnabled(false);
static std::atomic<bool> sThreadRunning(false);

static void setLhbm(const char* value) {
    int fd = open(LHBM_NODE, O_WRONLY);
    if (fd >= 0) {
        write(fd, value, strlen(value));
        close(fd);
    }
}

uint32_t getUdfpsDimZOrder(uint32_t z) {
    return z;
}

uint32_t getUdfpsZOrder(uint32_t z, bool touched) {
    if (touched) {
        sLastTouchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (!sLhbmEnabled) {
            setLhbm("1");
            sLhbmEnabled = true;

            if (!sThreadRunning) {
                sThreadRunning = true;
                std::thread([]() {
                    while (sLhbmEnabled) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        
                        auto currentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        
                        // Ghost Layer Watchdog: Kills LHBM 200ms after UI is destroyed
                        if (currentMs - sLastTouchMs > 200) {
                            sLhbmEnabled = false; 
                        }
                    }
                    
                    // Clean kill
                    setLhbm("0");
                    sThreadRunning = false;
                    
                }).detach();
            }
        }
    } else {
        // Finger explicitly lifted
        sLhbmEnabled = false;
    }
    return z;
}

uint64_t getUdfpsUsageBits(uint64_t usageBits, bool /* touched */) {
    return usageBits;
}
