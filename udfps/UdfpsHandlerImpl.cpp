/*
 * Copyright (C) 2024-2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HBM transport adapted from LineageOS change 474261, patch set 10:
 * https://review.lineageos.org/c/LineageOS/android_hardware_motorola/+/474261/10
 *
 * Build: retain libbase and libdl; add generated_kernel_headers to header_libs.
 * The selected kernel headers must expose Motorola's panel_param_info,
 * PARAM_HBM and DRM_IOCTL_SET_PANEL_FEATURE in display/drm/sde_drm.h.
 *
 * The proprietary HAL offsets and AncExcuteCommand ABI below are inherited
 * from the supplied cuscoi handler, NOT established by the LineageOS change.
 * They require matching 64-bit Motorola blobs. This is fingerprint_device_t
 * storage, not a C++ virtual-method table.
 */

#define LOG_TAG "UdfpsHandler.cuscoi"

#include "include/UdfpsHandler.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <display/drm/sde_drm.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr char kDrmDevice[] = "/dev/dri/card0";
constexpr int kHbmOff = 0;
constexpr int kHbmFodOn = 2;
constexpr int32_t kGoodixFingerDown = 241;
constexpr int32_t kGoodixFingerUp = 242;
constexpr size_t kExtCmdSlot = 26;
constexpr size_t kJiiovDeviceSlot = 27;

// Preserve the supplied handler's 400 ms fallback. Unlike the original,
// expiry releases the vendor finger state as well as HBM. Increase this if
// enrollment requires longer captures. The referenced handler uses 200 ms.
constexpr auto kCaptureTimeout = std::chrono::milliseconds(400);
constexpr auto kHbmSettleTime = std::chrono::milliseconds(30);
using Clock = std::chrono::steady_clock;
}  // namespace

class CuscoiUdfpsHandler : public UdfpsHandler {
  public:
    ~CuscoiUdfpsHandler() {
        mStopping.store(true);
        wakeWorker();
        if (mWorker.joinable()) mWorker.join();

        std::lock_guard<std::mutex> lock(mMutex);
        releaseFingerLocked();
        if (mAncHandle != nullptr) dlclose(mAncHandle);
    }

    void init(fingerprint_device_t* device) override {
        // init is a one-time lifecycle operation, before any callbacks.
        mDevice = device;
        const auto vendor = android::base::GetProperty("persist.vendor.hardware.fingerprint", "");
        mIsGoodix = vendor == "goodix";
        mIsJiiov = vendor == "jiiov" || vendor == "anc";
        LOG(INFO) << "Initializing cuscoi UDFPS, vendor=" << vendor;

        if (mIsJiiov) {
            mAncHandle = dlopen("anc.hal.so", RTLD_NOW | RTLD_LOCAL);
            if (mAncHandle == nullptr) {
                LOG(ERROR) << "dlopen(anc.hal.so): " << dlerror();
            } else {
                mAncExecute = reinterpret_cast<AncExecute>(dlsym(mAncHandle, "AncExcuteCommand"));
                if (mAncExecute == nullptr) {
                    LOG(ERROR) << "dlsym(AncExcuteCommand): " << dlerror();
                }
            }
        }

        mWakeFd.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        if (mWakeFd.get() < 0) {
            PLOG(ERROR) << "eventfd failed; refusing to enable HBM without watchdog";
            return;
        }
        mWorker = std::thread(&CuscoiUdfpsHandler::workerLoop, this);
    }

    void onFingerDown(uint32_t x, uint32_t y, float /*minor*/, float /*major*/) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStopping.load() || mWakeFd.get() < 0 || mFingerDown) return;

        LOG(INFO) << "Finger down: x=" << x << " y=" << y;
        if (!setHbmLocked(true)) return;

        // Preserve the supplied panel-settling delay. It is not a display
        // readiness acknowledgement; tune against the actual panel driver.
        std::this_thread::sleep_for(kHbmSettleTime);
        if (sendPointerEventLocked(true) != 0) {
            LOG(ERROR) << "Vendor finger-down command failed";
            // A failing command may still have partially changed vendor state.
            sendPointerEventLocked(false);
            setHbmLocked(false);
            mDeadline = Clock::now() + kCaptureTimeout;
            wakeWorker();
            return;
        }
        mFingerDown = true;
        mDeadline = Clock::now() + kCaptureTimeout;
        wakeWorker();
    }

    void onFingerUp() override {
        std::lock_guard<std::mutex> lock(mMutex);
        releaseFingerLocked();
        wakeWorker();
    }

    void onAcquired(int32_t result, int32_t vendorCode) override {
        LOG(DEBUG) << "onAcquired: result=" << result << " vendorCode=" << vendorCode;
    }

    void cancel() override {
        std::lock_guard<std::mutex> lock(mMutex);
        LOG(INFO) << "UDFPS cancel";
        releaseFingerLocked();
        wakeWorker();
    }

  private:
    using AncExecute = int (*)(void*, int32_t, const uint8_t*, uint32_t, uint8_t**, uint32_t*);

    bool setHbmLocked(bool enable) {
        if (mHbmActive == enable) return true;

        android::base::unique_fd fd(open(kDrmDevice, O_RDWR | O_CLOEXEC));
        if (fd.get() < 0) {
            PLOG(ERROR) << "Failed to open " << kDrmDevice;
            return false;
        }

        // Use the actual generated kernel UAPI; do not guess ioctl numbers
        // or substitute a hand-written struct with a potentially different ABI.
        struct panel_param_info param = {};
        param.param_idx = PARAM_HBM;
        param.value = enable ? kHbmFodOn : kHbmOff;
        int ret;
        do {
            ret = ioctl(fd.get(), DRM_IOCTL_SET_PANEL_FEATURE, &param);
        } while (ret < 0 && errno == EINTR);
        if (ret < 0) {
            PLOG(ERROR) << "DRM_IOCTL_SET_PANEL_FEATURE(PARAM_HBM, " << param.value << ") failed";
            return false;
        }
        mHbmActive = enable;
        LOG(INFO) << "FOD HBM " << (enable ? "enabled" : "disabled");
        return true;
    }

    int sendPointerEventLocked(bool down) {
        if (!mIsGoodix && !mIsJiiov) return 0;
        if (mDevice == nullptr) {
            LOG(ERROR) << "Cannot send vendor command: fingerprint device is null";
            return -1;
        }

        static_assert(sizeof(void*) == 8, "Motorola HAL offsets below require the 64-bit blob ABI");
        if (mIsGoodix) {
            using ExtCmd = int (*)(fingerprint_device_t*, int32_t);
            ExtCmd extCmd = nullptr;
            std::memcpy(&extCmd, reinterpret_cast<const uint8_t*>(mDevice) +
                                    kExtCmdSlot * sizeof(void*), sizeof(extCmd));
            if (extCmd == nullptr) {
                LOG(ERROR) << "Goodix extCmd pointer is null";
                return -1;
            }
            return extCmd(mDevice, down ? kGoodixFingerDown : kGoodixFingerUp);
        }

        if (mAncExecute == nullptr) return -1;
        void* jiiovDevice = nullptr;
        std::memcpy(&jiiovDevice, reinterpret_cast<const uint8_t*>(mDevice) +
                                    kJiiovDeviceSlot * sizeof(void*), sizeof(jiiovDevice));
        if (jiiovDevice == nullptr) {
            LOG(ERROR) << "JIIOV context pointer is null";
            return -1;
        }
        const uint8_t downData[] = {3, 0, 99};
        const uint8_t upData[] = {97, 98, 99};
        uint8_t* output = nullptr;
        uint32_t outputLength = 0;
        // Output ownership is blob-specific; do not guess an allocator/free API.
        return mAncExecute(jiiovDevice, down ? 13 : 14, down ? downData : upData,
                           sizeof(downData), &output, &outputLength);
    }

    void releaseFingerLocked() {
        if (mFingerDown) {
            const int ret = sendPointerEventLocked(false);
            if (ret != 0) LOG(ERROR) << "Vendor finger-up command failed: " << ret;
            mFingerDown = false;
        }
        setHbmLocked(false);
        // If disabling HBM failed, the watchdog retries rather than falsely
        // recording it as off. No detached callbacks can outlive this object.
        mDeadline = Clock::now() + kCaptureTimeout;
    }

    void wakeWorker() {
        if (mWakeFd.get() < 0) return;
        const uint64_t wake = 1;
        ssize_t ret;
        do {
            ret = write(mWakeFd.get(), &wake, sizeof(wake));
        } while (ret < 0 && errno == EINTR);
        // EAGAIN means a wake is already pending.
        if (ret < 0 && errno != EAGAIN) PLOG(ERROR) << "Worker wake failed";
    }

    static char readTouchState(int fd) {
        char state = '\0';
        if (lseek(fd, 0, SEEK_SET) < 0) return '\0';
        ssize_t ret;
        do {
            ret = read(fd, &state, 1);
        } while (ret < 0 && errno == EINTR);
        return ret == 1 ? state : '\0';
    }

    void workerLoop() {
        android::base::unique_fd touchFd;
        constexpr const char* paths[] = {
            "/sys/class/touchscreen/primary/udfps_pressed",
            "/sys/class/touchscreen/touchscreen_dev/udfps_pressed",
            "/sys/devices/virtual/touchscreen/primary/udfps_pressed",
        };
        for (const auto* path : paths) {
            touchFd.reset(open(path, O_RDONLY | O_CLOEXEC));
            if (touchFd.get() >= 0) {
                LOG(INFO) << "Opened " << path;
                // Clear any initial sysfs notification before polling.
                readTouchState(touchFd.get());
                break;
            }
        }
        if (touchFd.get() < 0) {
            LOG(WARNING) << "No udfps_pressed node; using framework pointer callbacks";
        }

        while (!mStopping.load()) {
            int timeout = -1;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mFingerDown || mHbmActive) {
                    const auto remaining = mDeadline - Clock::now();
                    if (remaining <= Clock::duration::zero()) {
                        LOG(INFO) << "Capture timeout: releasing finger and FOD HBM";
                        releaseFingerLocked();
                    }
                    if (mFingerDown || mHbmActive) {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                mDeadline - Clock::now()).count();
                        timeout = ms > 0 ? static_cast<int>(ms + 1) : 0;
                    }
                }
            }

            struct pollfd fds[] = {
                {mWakeFd.get(), POLLIN, 0},
                {touchFd.get(), POLLERR | POLLPRI, 0},
            };
            const int ret = poll(fds, 2, timeout);
            if (ret < 0) {
                if (errno == EINTR) continue;
                PLOG(ERROR) << "UDFPS poll failed";
                mStopping.store(true);
                std::lock_guard<std::mutex> lock(mMutex);
                releaseFingerLocked();
                return;
            }
            if (fds[0].revents & POLLIN) {
                uint64_t wake;
                ssize_t count;
                do {
                    count = read(mWakeFd.get(), &wake, sizeof(wake));
                } while (count < 0 && errno == EINTR);
            }
            if (mStopping.load()) break;
            if (fds[1].revents & (POLLHUP | POLLNVAL)) {
                LOG(ERROR) << "Touch node disconnected; using framework pointer callbacks";
                touchFd.reset();
                onFingerUp();
            } else if (fds[1].revents & (POLLERR | POLLPRI)) {
                const char state = readTouchState(touchFd.get());
                if (state == '1') onFingerDown(0, 0, 0, 0);
                else if (state == '0') onFingerUp();
                else {
                    LOG(ERROR) << "Invalid touch node state; disabling touch polling";
                    touchFd.reset();
                    onFingerUp();
                }
            }
        }
    }

    fingerprint_device_t* mDevice = nullptr;
    bool mIsGoodix = false;
    bool mIsJiiov = false;
    bool mFingerDown = false;
    bool mHbmActive = false;
    void* mAncHandle = nullptr;
    AncExecute mAncExecute = nullptr;
    Clock::time_point mDeadline{};
    std::mutex mMutex;
    std::atomic<bool> mStopping{false};
    android::base::unique_fd mWakeFd;
    std::thread mWorker;
};

static UdfpsHandler* create() {
    return new CuscoiUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" {
UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
}
