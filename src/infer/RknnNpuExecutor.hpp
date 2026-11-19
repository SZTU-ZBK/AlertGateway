#pragma once

#include <memory>
#include <string>

#include "common/Frame.hpp"
#include "infer/NpuInferenceScheduler.hpp"

struct ModelConfig;

// 每个执行器独占模型上下文、输入 DMA 内存和后处理器，由一个调度工作线程使用。
class RknnNpuExecutor {
public:
    explicit RknnNpuExecutor(const ModelConfig& config, int core_mask = 7);
    ~RknnNpuExecutor();

    RknnNpuExecutor(const RknnNpuExecutor&) = delete;
    RknnNpuExecutor& operator=(const RknnNpuExecutor&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool ready() const;
    NpuInferenceResult execute(const NpuChannelConfig& channel, const Frame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int core_mask_;
};
