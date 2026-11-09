#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "capture/IVideoSource.hpp"
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/FrameBufferPool.hpp"
#include "common/SharedDetections.hpp"
#include "encode/EncodeThread.hpp"
#include "infer/InferThread.hpp"
#include "mqtt/MqttThread.hpp"
#include "stream/StreamThread.hpp"

// 单个输入通道的完整运行时对象。各通道的队列、跟踪状态和硬件上下文互不共享；
// main 只负责创建、启动和停止多个 ChannelPipeline。
class ChannelPipeline {
public:
    ChannelPipeline(std::string channel_id, nlohmann::json config,
                    NpuInferenceSchedulerPtr npu_scheduler = nullptr);
    ~ChannelPipeline();

    ChannelPipeline(const ChannelPipeline&) = delete;
    ChannelPipeline& operator=(const ChannelPipeline&) = delete;

    const std::string& id() const { return channel_id_; }
    void start();
    void stop_source();
    void stop();

private:
    void build();

    std::string channel_id_;
    nlohmann::json config_;
    // 仅由 main 创建一次并注入各 pipeline；P2 起 InferThread 使用它执行 NPU job。
    NpuInferenceSchedulerPtr npu_scheduler_;

    // 声明在队列之前，保证队列残留 Frame 释放时池仍然存活。
    std::shared_ptr<FrameBufferPool> frame_pool_; //复用图像内存。 保存已经用完的像素缓冲区，后续帧可以重复使用，减少频繁申请和释放内存。
    std::unique_ptr<BlockingQueue<Frame>> enc_queue_;   // 存放等待编码的图像帧。 拉流线程放入 Frame，编码线程取出。
    std::unique_ptr<BlockingQueue<Frame>> infer_queue_;  //存放等待推理的图像帧。 拉流线程放入 Frame，推理线程取出；当前拉流路径会用新帧替换待处理的旧帧。
    std::unique_ptr<BlockingQueue<EncodedPacket>> stream_queue_; //存放已经编码好的视频包。 编码线程放入 H.264 EncodedPacket，推流线程取出并发送。
    std::unique_ptr<BlockingQueue<std::string>> mqtt_queue_;   //存放等待上报的检测摘要。 推理线程放入 JSON 字符串，MQTT 线程取出并发布。
    std::unique_ptr<SharedDetections> shared_dets_;     //保存供画框使用的检测结果。 推理线程更新检测框，编码线程读取；还负责框的关联、平滑和时间对齐。
    std::unique_ptr<IVideoSource> video_source_;        //获取视频图像。 它指向视频源接口；你当前关注的拉流路径中，实际对象是 PullStreamThread，负责连接视频源、解码并投递图像。
    std::unique_ptr<InferThread> infer_;               //分析图像中的目标。 负责推理流程，得到目标的位置、类别和置信度，更新 shared_dets_，并生成上报摘要。
    std::unique_ptr<EncodeThread> encoder_;             //画框并重新编码。 取出图像，读取 shared_dets_，画上检测框和标签，再用 MPP 编码为 H.264。
    std::unique_ptr<StreamThread> streamer_;        //上报检测信息。 连接 MQTT Broker，将 mqtt_queue_ 中的消息发布到指定 topic。
    std::unique_ptr<MqttThread> mqtter_;

    bool source_started_ = false;
    bool stopped_ = false;
};
