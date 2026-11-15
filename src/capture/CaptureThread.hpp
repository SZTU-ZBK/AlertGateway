#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/FrameBufferPool.hpp"
#include "capture/IVideoSource.hpp"


// 摄像头配置，从 config.json 读取后传入 CaptureThread
struct CameraConfig {
    std::string channel_id = "single";
    std::string device;  // V4L2 设备节点，如 /dev/video20
    int width;           // 采集分辨率宽（像素）
    int height;          // 采集分辨率高（像素）
    int fps;             // 目标帧率，通过 VIDIOC_S_PARM 下发给驱动（驱动可能调整到最近支持值）
};

// V4L2 采集线程，Pipeline 的数据源头。实现 IVideoSource 接口。
//
// MMAP buffer 通过 EXPBUF 导出 fd，RGA 同步转换 YUYV → NV12 MPP buffer。
// 转换后立即 QBUF；编码与推理队列共享独立的 NV12 输出引用。
class CaptureThread : public IVideoSource {
public:
    CaptureThread(const CameraConfig& cfg,
                  BlockingQueue<Frame>& enc_queue,
                  BlockingQueue<Frame>& infer_queue,
                  std::shared_ptr<FrameBufferPool> frame_pool = nullptr);
    ~CaptureThread();

    // 依次执行：打开设备 → 申请并导出 DMA-BUF → 启动采集流 → 起线程
    void start();
    // 置停止标志 → join 线程 → 停止采集流 → 关闭 DMA-BUF fd → 关闭 fd
    void stop();

private:
    void run();          // 主循环：select → DQBUF → RGA → QBUF → 投队列

    // 同步转换，不归还源采集槽位；成功返回独立 NV12 buffer，失败返回空指针。
    std::shared_ptr<MppBufferHolder> convert_to_nv12(
        uint32_t buffer_index, size_t bytes_used, uint32_t flags);

    // V4L2 初始化三步，start() 中顺序调用
    bool open_device();  // open fd，VIDIOC_S_FMT 设置分辨率/YUYV格式，VIDIOC_S_PARM 设置帧率
    bool init_buffers(); // VIDIOC_REQBUFS 申请4块内核缓冲区，EXPBUF 导出 DMA-BUF fd
    bool start_stream(); // 将所有缓冲区 VIDIOC_QBUF 入队，VIDIOC_STREAMON 启动采集

    void stop_stream();  // VIDIOC_STREAMOFF 停止采集
    void close_device(); // 关闭所有导出 fd，close fd

    // V4L2 mmap 缓冲区描述符，内核分配、用户空间只读/写
    struct MmapBuffer {
        int dma_fd = -1; // 由 EXPBUF 导出，CaptureThread 负责 close
        size_t length = 0;
    };

    MppBufferGroup output_buffer_group_ = nullptr;
    CameraConfig            cfg_;
    BlockingQueue<Frame>&   enc_queue_;    // 每帧必达 → EncodeThread
    BlockingQueue<Frame>&   infer_queue_;  // 尽力投递 → InferThread（可丢帧）
    std::shared_ptr<FrameBufferPool> frame_pool_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};
    int                     fd_ = -1;          // V4L2 设备文件描述符
    std::vector<MmapBuffer> buffers_;           // mmap 缓冲区列表（通常4块）
};
