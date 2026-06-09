/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.cuscoi"

#include "include/UdfpsHandler.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#define BACKLIGHT_PATH "/sys/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/card0/card0-DSI-1/local_hbm"

#define HBM_ON_MAGIC_VALUE  "1"
#define HBM_OFF_MAGIC_VALUE "0"

// Goodix specific extension commands
#define GOODIX_EXT_CMD_FINGER_DOWN 241
#define GOODIX_EXT_CMD_FINGER_UP   242

class CuscoiUdfpsHandler : public UdfpsHandler {
private:
    bool mIsGoodix = false;
    bool mIsJiiov = false;

public:
    void init(fingerprint_device_t* device) override {
        mDevice = device;
        
        // Check which sensor the init script found
        std::string fpVendor = android::base::GetProperty("persist.vendor.hardware.fingerprint", "");
        mIsGoodix = (fpVendor == "goodix");
        mIsJiiov = (fpVendor == "jiiov" || fpVendor == "anc");
        
        LOG(INFO) << "Cuscoi UDFPS handler initialized. Vendor: " << fpVendor;
    }

    void onFingerDown(uint32_t x, uint32_t y, float /* minor */, float /* major */) override {
        LOG(INFO) << "Finger down: x=" << x << " y=" << y;
        
        setHbm(true);
        usleep(30000); 
        
        // Only Goodix needs the proprietary TrustZone poke
        if (mIsGoodix) {
            sendGoodixExtCmd(241);
        } else if (mIsJiiov) {
            sendJiiovPointerEvent(true);
        }
    }

    void onFingerUp() override {
        LOG(INFO) << "Finger up";
        
        if (mIsGoodix) {
            sendGoodixExtCmd(242);
        } else if (mIsJiiov) {
            sendJiiovPointerEvent(false);
        }
        
        setHbm(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) override {
        // Do NOT turn off HBM here - this fires DURING capture, not after!
        // Turning off HBM here interrupts the optical sensor mid-capture.
        // The framework will call onFingerUp when the capture session ends.
        LOG(DEBUG) << "onAcquired: result=" << result << " vendorCode=" << vendorCode;
    }

    void cancel() override {
        LOG(INFO) << "UDFPS cancel";
        setHbm(false);
    }

private:
    fingerprint_device_t* mDevice = nullptr;
    std::atomic<int> mTimeoutToken{0};
    std::atomic<bool> mHbmActive{false};

    // Helper to bypass the standard struct and call Goodix's proprietary extCmd
    int sendGoodixExtCmd(int32_t cmd) {
        if (!mDevice) {
            LOG(ERROR) << "mDevice is null, cannot send extCmd";
            return -1;
        }
        
        // Cast the device pointer to an array of void pointers (the vtable)
        void** dev_vtable = reinterpret_cast<void**>(mDevice);
        
        // Grab the function pointer at index 26 (goodix_extCmd)
        auto extCmd = reinterpret_cast<int (*)(fingerprint_device_t*, int32_t)>(dev_vtable[26]);
        
        if (extCmd) {
            LOG(DEBUG) << "Sending Goodix extCmd: " << cmd;
            return extCmd(mDevice, cmd);
        }
        
        LOG(ERROR) << "Failed to resolve goodix_extCmd pointer";
        return -1;
    }

    int sendJiiovPointerEvent(bool down) {
        void* anc_handle = dlopen("anc.hal.so", RTLD_NOW);
        if (!anc_handle) {
            LOG(ERROR) << "Failed to dlopen anc.hal.so";
            return -1;
        }
        
        int (*eventFunc)() = reinterpret_cast<int (*)()>(dlsym(anc_handle, down ? "AncOnPointerDown" : "AncOnPointerUp"));
        if (!eventFunc) {
            eventFunc = reinterpret_cast<int (*)()>(dlsym(anc_handle, down ? "FpmOnPointerDown" : "FpmOnPointerUp"));
        }
        
        int ret = -1;
        if (eventFunc) {
            LOG(DEBUG) << "Sending JIIOV event: " << (down ? "PointerDown" : "PointerUp");
            ret = eventFunc();
        } else {
            LOG(ERROR) << "Failed to resolve JIIOV pointer event functions";
        }
        
        dlclose(anc_handle);
        return ret;
    }

    void setHbm(bool enable) {
        // Skip redundant state changes
        if (mHbmActive.load() == enable) {
            LOG(DEBUG) << "setHbm: Already " << (enable ? "ON" : "OFF") << ", skipping";
            return;
        }

        const char* value = enable ? HBM_ON_MAGIC_VALUE : HBM_OFF_MAGIC_VALUE;
        LOG(INFO) << "setHbm: " << (enable ? "ON" : "OFF");

        int fd = open(BACKLIGHT_PATH, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            LOG(ERROR) << "Failed to open backlight path: " << strerror(errno);
            return;
        }

        ssize_t written = write(fd, value, strlen(value));
        if (written < 0) {
            LOG(ERROR) << "Failed to write HBM value: " << strerror(errno);
        } else {
            mHbmActive.store(enable);
            LOG(INFO) << "HBM " << (enable ? "enabled" : "disabled") << " successfully";            
        }

        close(fd);

        /* * Safety Timeout with Cancellation Token:
         * Prevents stuck HBM if onFingerUp is never called.
         * Token ensures old timeouts don't fire after new finger down events.
         * 800ms allows sufficient time for enrollment captures.
         */
        if (enable) {
            int currentToken = ++mTimeoutToken;
            std::thread([this, currentToken]() {
                usleep(400000); // 400ms - Reduced to minimize white patch persistence
                if (mTimeoutToken.load() == currentToken && mHbmActive.load()) {
                    LOG(INFO) << "Safety Timeout: Forcing HBM OFF";
                    this->setHbm(false);
                }
            }).detach();
        } else {
            mTimeoutToken++; // Invalidate any pending timeout
        }
    }
};

static UdfpsHandler* create() {
    return new CuscoiUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
