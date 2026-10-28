// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <mutex>

#include "common/polyfill_thread.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class TurboMode {
public:
    explicit TurboMode(const vk::Instance& instance, const vk::InstanceDispatch& dld);
    ~TurboMode();

    void QueueSubmitted();

private:
    void Run(std::stop_token stop_token);

    // The spinner needs its own device on every platform now (Android included) so it can keep
    // non-Adreno GPUs (e.g. PowerVR) clocked up via a background compute workload.
    Device m_device;
    MemoryAllocator m_allocator;
#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Adreno/Qualcomm has a direct kernel turbo hook; everything else uses the compute spinner.
    bool m_use_adrenotools{};
#endif
    std::mutex m_submission_lock;
    std::condition_variable_any m_submission_cv;
    std::chrono::time_point<std::chrono::steady_clock> m_submission_time{};

    std::jthread m_thread;
};

} // namespace Vulkan
