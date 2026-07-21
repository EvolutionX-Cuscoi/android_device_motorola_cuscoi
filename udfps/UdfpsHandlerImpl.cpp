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
#include <dlfcn.h>

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

        std::thread([this]() {
            int fd = -1;
            const char* paths[] = {
                "/sys/class/touchscreen/primary/udfps_pressed",
                "/sys/class/touchscreen/touchscreen_dev/udfps_pressed",
                "/sys/devices/virtual/touchscreen/primary/udfps_pressed"
            };

            for (auto& path : paths) {
                fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    LOG(INFO) << "Opened udfps_pressed node at " << path;
                    break;
                }
            }

            if (fd < 0) {
                LOG(ERROR) << "Failed to open udfps_pressed node";
                return;
            }

            struct pollfd fodPoll = {
                .fd = fd,
                .events = POLLERR | POLLPRI,
                .revents = 0,
            };

            while (true) {
                int rc = poll(&fodPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll fd, err: " << rc;
                    continue;
                }

                char c;
                lseek(fd, 0, SEEK_SET);
                if (read(fd, &c, 1) == 1) {
                    if (c == '1') {
                        this->onFingerDown(0, 0, 0, 0);
                    } else if (c == '0') {
                        this->onFingerUp();
                    }
                }
            }
        }).detach();
    }

    void onFingerDown(uint32_t x, uint32_t y, float /* minor */, float /* major */) override {
        if (mFingerDown.exchange(true)) return; // Already down

        LOG(INFO) << "Finger down: x=" << x << " y=" << y;
        
        setHbm(true);
        usleep(30000); 
        
        // Only Goodix and JIIOV need the proprietary TrustZone poke
        if (mIsGoodix) {
            sendGoodixExtCmd(241);
        } else if (mIsJiiov) {
            sendJiiovPointerEvent(true);
        }
    }

    void onFingerUp() override {
        if (!mFingerDown.exchange(false)) return; // Already up

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
    std::atomic<bool> mFingerDown{false};

    // Helper to call Goodix's proprietary extCmd
    int sendGoodixExtCmd(int32_t cmd) {
        if (!mDevice) {
            LOG(ERROR) << "mDevice is null, cannot send extCmd";
            return -1;
        }
        
        void** dev_vtable = reinterpret_cast<void**>(mDevice);
        auto extCmd = reinterpret_cast<int (*)(fingerprint_device_t*, int32_t)>(dev_vtable[26]);
        if (extCmd) {
            LOG(DEBUG) << "Sending Goodix extCmd (vtable 26): " << cmd;
            return extCmd(mDevice, cmd);
        }
        
        LOG(ERROR) << "Failed to resolve Goodix extCmd pointer";
        return -1;
    }

    int sendJiiovPointerEvent(bool down) {
        if (!mDevice) return -1;
        
        // Due to AOSP vs Motorola header mismatches, the fingerprint_device_t struct
        // is 1 pointer larger on Moto (extCmd at index 16). 
        // In Moto's fingerprint_device_t, hw_device_t is 15 pointers (120 bytes),
        // followed by 11 standard HAL pointers, then extCmd (index 26), 
        // and finally reserved[0] at index 27.
        void** dev_vtable = reinterpret_cast<void**>(mDevice);
        void* jiiov_dev = dev_vtable[27];
        
        if (!jiiov_dev) {
            LOG(ERROR) << "jiiov_dev is null at index 27";
            return -1;
        }

        void* anc_handle = dlopen("anc.hal.so", RTLD_NOW);
        if (!anc_handle) {
            LOG(ERROR) << "Failed to dlopen anc.hal.so";
            return -1;
        }
        
        int (*ancExec)(void*, int32_t, const uint8_t*, uint32_t, uint8_t**, uint32_t*) = 
            reinterpret_cast<int (*)(void*, int32_t, const uint8_t*, uint32_t, uint8_t**, uint32_t*)>(dlsym(anc_handle, "AncExcuteCommand"));
        
        int ret = -1;
        if (ancExec) {
            LOG(DEBUG) << "Sending JIIOV AncExcuteCommand: " << (down ? "PointerDown (13)" : "PointerUp (14)");
            uint8_t in_buf[3];
            if (down) {
                // For PointerDown (13), the service passes {type, auth, 99}.
                // Decompiled code defaults 'type' to 3 and 'auth' to 0. 
                // Passing 97 here causes out-of-bounds array access in FpmExcuteCommand.
                in_buf[0] = 3;
                in_buf[1] = 0;
                in_buf[2] = 99;
            } else {
                // For PointerUp (14), the service hardcodes 0x6261 (97, 98) and 99.
                in_buf[0] = 97;
                in_buf[1] = 98;
                in_buf[2] = 99;
            }
            uint8_t* out_buf = nullptr;
            uint32_t out_len = 0;
            ret = ancExec(jiiov_dev, down ? 13 : 14, in_buf, 3, &out_buf, &out_len);
        } else {
            LOG(ERROR) << "Failed to resolve AncExcuteCommand";
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
