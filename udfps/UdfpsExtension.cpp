/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <compositionengine/UdfpsExtension.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define LHBM_NODE "/sys/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/card0/card0-DSI-1/local_hbm"

// ─── tunables ────────────────────────────────────────────────────────────────
// How long to keep LHBM alive after the last touched=true, before killing it.
// Covers enrolment frame gaps and brief UI reconstructions.
static constexpr uint64_t LHBM_LINGER_MS = 300;

// Minimum time LHBM must stay ON once enabled (avoids rapid on/off flicker).
static constexpr uint64_t LHBM_MIN_ON_MS  = 80;
// ─────────────────────────────────────────────────────────────────────────────

enum class LhbmRequest { IDLE, ENABLE, DISABLE };

static std::mutex              sLock;
static std::condition_variable sCv;
static LhbmRequest             sRequest      = LhbmRequest::IDLE;
static bool                    sWorkerAlive  = false;

// Timestamps (milliseconds, steady_clock)
static std::atomic<uint64_t>   sLastTouchMs{0};
static std::atomic<uint64_t>   sEnabledAtMs{0};
static std::atomic<bool>       sPhysicallyOn{false};

// ─── helpers ─────────────────────────────────────────────────────────────────

static uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void writeLhbm(bool on) {
    const char* val = on ? "1" : "0";
    int fd = open(LHBM_NODE, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write(fd, val, 1); // always 1 byte — no strlen needed
        close(fd);
        sPhysicallyOn = on;
    }
}

// ─── worker thread ───────────────────────────────────────────────────────────
// Single persistent thread; woken by condition variable.
// Owns all sysfs writes — nothing else calls writeLhbm().

static void lhbmWorker() {
    bool lhbmOn = false;

    while (true) {
        std::unique_lock<std::mutex> lk(sLock);

        if (!lhbmOn) {
            // Sleeping: wait indefinitely for an ENABLE request
            sCv.wait(lk, [] {
                return sRequest == LhbmRequest::ENABLE ||
                       sRequest == LhbmRequest::DISABLE; // DISABLE while off = exit
            });
        } else {
            // Active: wake up to check linger expiry
            uint64_t elapsed  = nowMs() - sLastTouchMs.load();
            uint64_t lingerLeft = (elapsed < LHBM_LINGER_MS)
                                  ? (LHBM_LINGER_MS - elapsed) : 0;
            if (lingerLeft == 0) {
                // Linger expired while waiting — fall through to disable
            } else {
                sCv.wait_for(lk, std::chrono::milliseconds(lingerLeft));
            }
        }

        LhbmRequest req = sRequest;
        sRequest = LhbmRequest::IDLE;
        lk.unlock();

        if (!lhbmOn && req == LhbmRequest::ENABLE) {
            writeLhbm(true);
            sEnabledAtMs = nowMs();
            lhbmOn = true;
            continue; // loop back to wait with linger logic
        }

        if (lhbmOn) {
            // Check: has a new touch arrived inside the linger window?
            uint64_t sinceTouch = nowMs() - sLastTouchMs.load();
            if (sinceTouch < LHBM_LINGER_MS && req != LhbmRequest::DISABLE) {
                // Still within linger — keep going
                continue;
            }

            // Enforce minimum ON time to avoid flicker
            uint64_t onDuration = nowMs() - sEnabledAtMs.load();
            if (onDuration < LHBM_MIN_ON_MS) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(LHBM_MIN_ON_MS - onDuration));
            }

            writeLhbm(false);
            lhbmOn = false;

            // After turning off, check if a new ENABLE snuck in
            {
                std::lock_guard<std::mutex> lk2(sLock);
                if (sRequest == LhbmRequest::ENABLE) {
                    // Re-enable immediately on next iteration
                    continue;
                }
            }
            // No new touch — truly idle, exit thread
            break;
        }

        if (req == LhbmRequest::DISABLE && !lhbmOn) {
            break; // nothing to do, exit cleanly
        }
    }

    std::lock_guard<std::mutex> lk(sLock);
    sWorkerAlive = false;
    // sPhysicallyOn is already false here
}

static void requestLhbm(bool enable) {
    std::lock_guard<std::mutex> lk(sLock);

    sRequest = enable ? LhbmRequest::ENABLE : LhbmRequest::DISABLE;

    if (!sWorkerAlive) {
        if (!enable) return; // nothing running, nothing to kill
        sWorkerAlive = true;
        std::thread(lhbmWorker).detach();
    }

    sCv.notify_one();
}

// ─── UDFPS extension API ─────────────────────────────────────────────────────

uint32_t getUdfpsDimZOrder(uint32_t z) {
    return z;
}

uint32_t getUdfpsZOrder(uint32_t z, bool touched) {
    if (touched) {
        sLastTouchMs = nowMs(); // refresh linger deadline on every touched=true
        if (!sPhysicallyOn) {
            requestLhbm(true);
        }
    } else {
        // Don't hard-kill; let the linger window decide.
        // sLastTouchMs is simply not refreshed, so the worker
        // will naturally time out and disable LHBM.
        //
        // Only send an explicit DISABLE if we need to abort early
        // (e.g. overlay fully torn down). For now, linger handles it.
    }
    return z;
}

uint64_t getUdfpsUsageBits(uint64_t usageBits, bool /* touched */) {
    return usageBits;
}
