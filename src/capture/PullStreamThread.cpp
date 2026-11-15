#include "capture/PullStreamThread.hpp"
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>

#include "im2d.h"
#include "rga.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
}

// Rockchip FFmpeg 的 AV_PIX_FMT_DRM_PRIME data[0] 以 AVDRMFrameDescriptor 开头，
// 后面紧跟 MPP buffer 引用。这里使用与 hwcontext_rkmpp.h 相同的 ABI 前缀，避免
// 交叉编译 sysroot 缺少 libdrm 的 drm_fourcc.h 时无法包含该私有扩展头。
struct RkMppDrmFrameDescriptor {
    AVDRMFrameDescriptor drm_desc;
    MppBuffer buffers[AV_DRM_MAX_PLANES];
};

// 只接受 DMA-BUF 描述符输出，图像缩放由 RGA 完成。
static AVPixelFormat choose_drm_prime(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_DRM_PRIME) return *format;
    }
    std::cerr << "[PullStream] decoder does not offer DRM_PRIME output\n";
    return AV_PIX_FMT_NONE;
}

static bool rga_resize_fd(int src_fd, int src_stride, int src_vstride, int dst_fd, int dst_stride,
                          int sw, int sh, int dw, int dh) {
    rga_buffer_t src_buf = wrapbuffer_fd(src_fd, sw, sh, RK_FORMAT_YCbCr_420_SP,//NV12半平面布局
                                         src_stride, src_vstride);
    rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, dw, dh, RK_FORMAT_YCbCr_420_SP,
                                         dst_stride, dh);
    rga_buffer_t pat_buf{};
    const IM_STATUS ret = improcess(src_buf, dst_buf, pat_buf,
                                    im_rect{0, 0, sw, sh},
                                    im_rect{0, 0, dw, dh},
                                    im_rect{0, 0, 0, 0}, 0);
    if (ret <= 0) {
        std::cerr << "[PullStream] RGA DMA-BUF resize failed: " << imStrError(ret)
                  << " (" << ret << ")\n" << std::flush;
        return false;
    }
    return true;
}

PullStreamThread::PullStreamThread(const PullStreamConfig& cfg,
                                 BlockingQueue<Frame>& enc_queue,
                                 BlockingQueue<Frame>& infer_queue,
                                 std::shared_ptr<FrameBufferPool> frame_pool)
    : cfg_(cfg), enc_queue_(enc_queue), infer_queue_(infer_queue),
      frame_pool_(std::move(frame_pool)) {}//这里的目的是将形参中shared_ptr的所有权给到frame_pool_，这样缓冲区的引用计数就不会在函数结束时减少，从而避免缓冲区被提前释放。

PullStreamThread::~PullStreamThread() {
    stop();
    if (output_buffer_group_) {
        mpp_buffer_group_put(output_buffer_group_);
        output_buffer_group_ = nullptr;
    }
}

std::string PullStreamThread::log_tag() const {
    return "[channel=" + cfg_.channel_id + "] ";
}

void PullStreamThread::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&PullStreamThread::run, this);
    std::cout << log_tag() << "[PullStream] Started pulling from: " << cfg_.url << "\n" << std::flush;
}

void PullStreamThread::stop() {
    // The FFmpeg interrupt callback observes this flag during blocked network I/O.
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    std::cout << log_tag() << "[PullStream] Stopped pull stream thread.\n" << std::flush;
}

void PullStreamThread::run() {
    uint64_t next_frame_id = 0;
    AVCodecContext* codec_ctx = nullptr;
    int video_stream_idx = -1;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint64_t decoded_frames = 0;
    uint64_t enc_pushed = 0;
    uint64_t enc_dropped = 0;
    uint64_t infer_pushed = 0;
    uint64_t infer_dropped = 0;
    uint64_t paced_dropped = 0;
    uint64_t read_us = 0;
    uint64_t read_calls = 0;
    uint64_t decode_send_us = 0;
    uint64_t decode_send_packets = 0;
    uint64_t decode_receive_us = 0;
    uint64_t decode_frames = 0;
    uint64_t resize_us = 0;
    uint64_t resize_frames = 0;
    uint64_t enqueue_us = 0;
    uint64_t enqueue_frames = 0;
    int mismatch_count = 0;
    int warn_count = 0;
    auto stats_t = std::chrono::steady_clock::now();

    if (!output_buffer_group_) {
        const MPP_RET group_ret = mpp_buffer_group_get_internal(
            &output_buffer_group_, MPP_BUFFER_TYPE_DRM);
        if (group_ret != MPP_OK || !output_buffer_group_) {
            std::cerr << log_tag() << "[PullStream] failed to create DRM output buffer group: "
                      << group_ret << "\n" << std::flush;
            return;
        }
    }

    while (running_) {
        // 1. 尝试打开输入流
        AVFormatContext* format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            std::cerr << log_tag() << "[PullStream] avformat_alloc_context failed\n";
            break;
        }
        format_ctx->interrupt_callback.callback = interrupt_callback; //用于在阻塞的网络 I/O 操作中检查线程是否应该停止。FFmpeg 在执行网络操作时会调用这个回调函数，如果返回非零值，FFmpeg 会中断当前操作。
        format_ctx->interrupt_callback.opaque = this;
        AVDictionary* options = nullptr;
        // 强制 TCP 传输，防止 RTSP 丢包花屏
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        // 连接与读取超时（微秒，5秒）
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "rw_timeout", "5000000", 0);

        std::cout << log_tag() << "[PullStream] Connecting to " << cfg_.url << "...\n" << std::flush;
        int ret = avformat_open_input(&format_ctx, cfg_.url.c_str(), nullptr, &options);
        if (options) {
            av_dict_free(&options);
        }

        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << log_tag() << "[PullStream] avformat_open_input failed: " << err_buf
                      << ". Retrying in " << cfg_.reconnect_sec << "s...\n" << std::flush;

            // 重试延迟退避
            for (int i = 0; i < cfg_.reconnect_sec && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        // 2. 检索流信息
        ret = avformat_find_stream_info(format_ctx, nullptr);
        if (ret < 0) {
            std::cerr << "[PullStream] avformat_find_stream_info failed\n" << std::flush;
            avformat_close_input(&format_ctx);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // 3. 寻找最佳视频流
        video_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);//选择一条最佳的AVMEDIA_TYPE_VIDEO流
        if (video_stream_idx < 0) {
            std::cerr << "[PullStream] No suitable video stream found\n" << std::flush;
            avformat_close_input(&format_ctx);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        AVStream* video_stream = format_ctx->streams[video_stream_idx];

        // 4. 只使用 Rockchip 硬解码器，要求输出 DRM_PRIME DMA buffer。
        const char* decoder_name = nullptr;
        if (video_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
            decoder_name = "h264_rkmpp";
        } else if (video_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            decoder_name = "hevc_rkmpp";
        } else {
            std::cerr << log_tag() << "[PullStream] hardware-only decoding supports H.264/HEVC only\n";
            avformat_close_input(&format_ctx);
            break;
        }
        const AVCodec* selected_codec = avcodec_find_decoder_by_name(decoder_name);
        if (!selected_codec) {
            std::cerr << log_tag() << "[PullStream] missing hardware decoder "
                      << decoder_name << "\n" << std::flush;
            avformat_close_input(&format_ctx);
            break;
        }
        std::cout << log_tag() << "[PullStream] decoder=" << decoder_name << "\n";
        codec_ctx = avcodec_alloc_context3(selected_codec);//这里只是在初始化并创建特定的解码器上下文
        if (!codec_ctx) {
            std::cerr << "[PullStream] avcodec_alloc_context3 failed\n" << std::flush;
            avformat_close_input(&format_ctx);
            continue;
        }

        ret = avcodec_parameters_to_context(codec_ctx, format_ctx->streams[video_stream_idx]->codecpar);//这里是复制视频流汇总对应编码器上下文的具体参数
        if (ret < 0) {
            std::cerr << "[PullStream] avcodec_parameters_to_context failed\n" << std::flush;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            continue;
        }

        codec_ctx->thread_count = 1;
        codec_ctx->thread_type = 0;
        codec_ctx->get_format = choose_drm_prime;
        av_opt_set(codec_ctx->priv_data, "afbc", "off", 0);
        av_opt_set(codec_ctx->priv_data, "buf_mode", "half", 0);
        ret = avcodec_open2(codec_ctx, selected_codec, nullptr);  //ff_ret = avcodec_open2(pCodecCtx, pCodec, nullptr); 打开并初始化ffmpeg解码器
        if (ret < 0) {
            std::cerr << "[PullStream] avcodec_open2 failed\n" << std::flush;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            continue;
        }

        std::cout << "[PullStream] Successfully connected & opened decoder. Resolution: "
                  << codec_ctx->width << "x" << codec_ctx->height
                  << ", Format: " << av_get_pix_fmt_name(codec_ctx->pix_fmt) << "\n" << std::flush;

        double source_fps = 0.0;
        if (video_stream->avg_frame_rate.den > 0) {
            source_fps = av_q2d(video_stream->avg_frame_rate);
        } else if (video_stream->r_frame_rate.den > 0) {
            source_fps = av_q2d(video_stream->r_frame_rate);
        }
        int target_fps = cfg_.fps;
        int frame_step = 1;
        if (source_fps > 0.0 && target_fps > 0) {
            frame_step = std::max(1, (int)std::round(source_fps / target_fps));
        }
        std::cout << "[PullStream] channel=" << cfg_.channel_id
                  << " source_fps=" << (int)std::round(source_fps)
                  << " target_fps=" << target_fps
                  << " frame_step=" << frame_step << "\n" << std::flush;

        const int output_width = cfg_.output_width > 0 ? cfg_.output_width : codec_ctx->width;
        const int output_height = cfg_.output_height > 0 ? cfg_.output_height : codec_ctx->height;
        if (output_width <= 0 || output_height <= 0 || (output_width & 1) || (output_height & 1)) {
            std::cerr << log_tag() << "[PullStream] invalid decoded output size "
                      << output_width << "x" << output_height << "\n" << std::flush;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            break;
        }
        const bool resize_after_decode = output_width != codec_ctx->width ||
                                         output_height != codec_ctx->height;
        if (resize_after_decode) {
            std::cout << log_tag() << "[PullStream] post_decode_resize="
                      << codec_ctx->width << "x" << codec_ctx->height << " -> "
                      << output_width << "x" << output_height
                      << " (RGA DMA-BUF path)\n" << std::flush;
        }

        // 5. 解码主循环
        uint64_t frame_index = 0;
        const int64_t source_frame_duration_ms = source_fps > 0.0
            ? std::max<int64_t>(1, static_cast<int64_t>(std::llround(1000.0 / source_fps)))
            : 33;
        int64_t source_pts_origin_ms = AV_NOPTS_VALUE;
        int64_t last_media_pts_ms = AV_NOPTS_VALUE;
        while (running_) {
            const auto read_started = std::chrono::steady_clock::now();
            ret = av_read_frame(format_ctx, packet);  //从解开的封装里读取packet，解封装的过程是将封装格式（如MP4、MKV、RTSP等）中的数据解析出来，得到原始的音视频数据包。这个函数会阻塞直到读取到一个完整的packet或者发生错误。
            read_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - read_started).count());
            ++read_calls;
            if (ret < 0) {
                // 读取完毕或出错，触发重连
                char err_buf[256];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[PullStream] av_read_frame read EOF or error: " << err_buf << "\n" << std::flush;
                break;
            }

            if (packet->stream_index == video_stream_idx) {
                const auto decode_send_started = std::chrono::steady_clock::now();
                ret = avcodec_send_packet(codec_ctx, packet); //把packet送入解码器，解码器会将其放入内部队列中等待解码
                decode_send_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - decode_send_started).count());
                ++decode_send_packets;
                if (ret < 0) {
                    std::cerr << "[PullStream] avcodec_send_packet error: " << ret << "\n" << std::flush;
                    av_packet_unref(packet);
                    break;
                }

                for (;;) {
                    const auto decode_receive_started = std::chrono::steady_clock::now();
                    const int receive_ret = avcodec_receive_frame(codec_ctx, frame); //  取出解码图像
                    decode_receive_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - decode_receive_started).count());
                    if (receive_ret != 0) break; // AVERROR(EAGAIN) 或 AVERROR_EOF，表示没有更多帧可接收，打断循环
                    ++decode_frames;
                    int w = frame->width;
                    int h = frame->height;
                    if (w != cfg_.width || h != cfg_.height || (w & 1) || (h & 1)) {
                        if (mismatch_count++ % 30 == 0) {
                            std::cerr << log_tag() << "[PullStream] Decoded resolution " << w << "x" << h
                                      << " does not match configured source " << cfg_.width
                                      << "x" << cfg_.height << "; dropping frame\n" << std::flush;
                        }
                        av_frame_unref(frame);
                        continue;
                    }
                    ++decoded_frames;

                    frame_index++;
                    if (frame_step > 1 && (frame_index % frame_step != 0)) {
                        ++paced_dropped;
                        av_frame_unref(frame);
                        continue;
                    }

                    const size_t frame_size = static_cast<size_t>(output_width) * output_height * 3 / 2;

                    Frame frame_obj;     //自定义的frame结构体，后面我自己改一下，它需要包含一个对DMA缓冲池的引用计数。
                    frame_obj.width = output_width;
                    frame_obj.height = output_height;
                    frame_obj.frame_id = ++next_frame_id;
                    frame_obj.pixel_format = PixelFormat::NV12;
                    int64_t media_pts_ms = AV_NOPTS_VALUE;
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
                        video_stream->time_base.num > 0 && video_stream->time_base.den > 0) {
                        media_pts_ms = av_rescale_q(frame->best_effort_timestamp,
                                                    video_stream->time_base,
                                                    AVRational{1, 1000});
                        if (source_pts_origin_ms == AV_NOPTS_VALUE) {
                            source_pts_origin_ms = media_pts_ms;
                        }
                        media_pts_ms -= source_pts_origin_ms;
                    }
                    const int64_t fallback_step_ms = source_frame_duration_ms * frame_step;
                    if (media_pts_ms == AV_NOPTS_VALUE ||
                        (last_media_pts_ms != AV_NOPTS_VALUE && media_pts_ms <= last_media_pts_ms)) {
                        media_pts_ms = last_media_pts_ms == AV_NOPTS_VALUE
                            ? 0
                            : last_media_pts_ms + fallback_step_ms;
                    }
                    last_media_pts_ms = media_pts_ms;
                    frame_obj.pts_ms = media_pts_ms;
                    frame_obj.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();

                    const bool drm_frame = frame->format == AV_PIX_FMT_DRM_PRIME; // 仅处理硬解码的 DRM_PRIME 输出
                    if (drm_frame) {
                        const auto* descriptor = reinterpret_cast<const RkMppDrmFrameDescriptor*>(frame->data[0]);
                        if (!descriptor || descriptor->drm_desc.nb_objects <= 0 ||
                            descriptor->drm_desc.nb_objects > AV_DRM_MAX_PLANES ||
                            descriptor->drm_desc.nb_layers != 1) {
                            std::cerr << log_tag() << "[PullStream] invalid DRM_PRIME descriptor\n";
                            av_frame_unref(frame);
                            continue;
                        }
                        const auto& layer = descriptor->drm_desc.layers[0];
                        // DRM_FORMAT_NV12 FourCC，避免引入板端缺失的 drm_fourcc.h。
                        constexpr uint32_t nv12_fourcc = 0x3231564e;
                        if (layer.format != nv12_fourcc || layer.nb_planes != 2) {
                            std::cerr << log_tag() << "[PullStream] expected two-plane NV12 DRM layout\n";
                            av_frame_unref(frame);
                            continue;
                        }
                        const auto& plane = layer.planes[0];
                        const int object_index = plane.object_index;
                        if (object_index < 0 || object_index >= descriptor->drm_desc.nb_objects) {
                            av_frame_unref(frame);
                            continue;
                        }
                        const int src_fd = descriptor->drm_desc.objects[object_index].fd;
                        const int src_stride = static_cast<int>(plane.pitch);
                        MppBuffer src_buffer = descriptor->buffers[object_index];
                        // 从 UV 偏移推导垂直 stride，支持解码器的行高对齐填充。
                        if (src_fd < 0 || src_stride < w || !src_buffer || plane.offset != 0 ||
                            layer.planes[1].object_index != object_index ||
                            layer.planes[1].pitch != plane.pitch ||
                            layer.planes[1].offset < plane.pitch * h ||
                            layer.planes[1].offset % plane.pitch != 0 ||
                            descriptor->drm_desc.objects[object_index].format_modifier != 0) {
                            std::cerr << log_tag() << "[PullStream] unsupported DMA-BUF layout\n";
                            av_frame_unref(frame);
                            continue;
                        }

                        const int src_vstride = static_cast<int>(layer.planes[1].offset / plane.pitch);
                        // stride 不匹配时也通过 RGA 整理成下游要求的紧凑布局。
                        if (resize_after_decode || src_stride != output_width || src_vstride != output_height) {
                            MppBuffer output_buffer = nullptr; //输出的buffer句柄由MPP创建
                            if (mpp_buffer_get(output_buffer_group_, &output_buffer, frame_size) != MPP_OK ||
                                !output_buffer) {
                                std::cerr << log_tag() << "[PullStream] DRM output buffer allocation failed\n";
                                av_frame_unref(frame);
                                continue;
                            }
                            const int dst_fd = mpp_buffer_get_fd(output_buffer);
                            const int dst_stride = output_width;
                            const auto resize_started = std::chrono::steady_clock::now();
                            const bool rga_ok = rga_resize_fd(src_fd, src_stride, src_vstride, dst_fd, dst_stride,
                                                             w, h, output_width, output_height);
                            resize_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - resize_started).count());
                            ++resize_frames;
                            if (!rga_ok) {
                                mpp_buffer_put(output_buffer);
                                av_frame_unref(frame);
                                continue;
                            }
                            frame_obj.dma_buffer = std::make_shared<MppBufferHolder>(output_buffer);
                            frame_obj.dma_stride = dst_stride;
                        } else {
                            mpp_buffer_inc_ref(src_buffer);//要对原始的 DMA-BUF 句柄增加引用计数，避免在 frame_obj 使用完毕前被释放
                            frame_obj.dma_buffer = std::make_shared<MppBufferHolder>(src_buffer);//不需要resize的话，直接把原来的SRC_BUFFER的句柄传给frame_obj，后续使用完毕后会自动释放
                            frame_obj.dma_stride = src_stride;
                        }
                    } else {
                        // 不再接受 CPU 像素输出格式。
                        if (warn_count++ % 100 == 0) {
                            std::cerr << log_tag() << "[PullStream] Warning: Unsupported frame format "
                                      << frame->format << " (" << av_get_pix_fmt_name((AVPixelFormat)frame->format)
                                      << "). Skipping...\n" << std::flush;
                        }
                        av_frame_unref(frame);
                        continue;
                    }

                    // 与 V4L2 路径一致：编码队列满时最多等待 100ms。
                    // 拉流/解码偶发 burst 不应因容量为 1 的队列立即丢掉编码帧；
                    // 有界等待仍可在编码真正卡住时避免无限阻塞。
                    if (running_) {
                        const auto enqueue_started = std::chrono::steady_clock::now();
                        if (enc_queue_.push(frame_obj, 100)) ++enc_pushed; //队列内自带锁，满了最多等待100ms
                        else ++enc_dropped;
                        size_t replaced = 0;
                        if (infer_queue_.push_latest(std::move(frame_obj), &replaced)) {
                            ++infer_pushed;
                            infer_dropped += replaced;
                        } else {
                            ++infer_dropped;
                        }
                        enqueue_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - enqueue_started).count());
                        ++enqueue_frames;
                    }

                    auto now = std::chrono::steady_clock::now();
                    const double elapsed = std::chrono::duration<double>(now - stats_t).count();
                    if (elapsed >= 10.0) {
                        std::cout << "[PullStats] channel=" << cfg_.channel_id
                                  << " input_fps=" << (decoded_frames / elapsed)
                                  << " decoded=" << decoded_frames
                                  << " enc_push=" << enc_pushed
                                  << " enc_drop=" << enc_dropped
                                  << " infer_push=" << infer_pushed
                                  << " infer_drop=" << infer_dropped
                                  << " pace_drop=" << paced_dropped
                                  << " read_avg_ms=" << (read_calls == 0 ? 0.0 : read_us / 1000.0 / read_calls)
                                  << " decode_send_avg_ms=" << (decode_send_packets == 0 ? 0.0 : decode_send_us / 1000.0 / decode_send_packets)
                                  << " decode_receive_avg_ms=" << (decode_frames == 0 ? 0.0 : decode_receive_us / 1000.0 / decode_frames)
                                  << " resize_avg_ms=" << (resize_frames == 0 ? 0.0 : resize_us / 1000.0 / resize_frames)
                                  << " enqueue_avg_ms=" << (enqueue_frames == 0 ? 0.0 : enqueue_us / 1000.0 / enqueue_frames)
                                  << " queues=" << enc_queue_.size() << "/" << infer_queue_.size()
                                  << "\n" << std::flush;
                        decoded_frames = enc_pushed = enc_dropped = 0;
                        infer_pushed = infer_dropped = paced_dropped = 0;
                        read_us = read_calls = decode_send_us = decode_send_packets = 0;
                        decode_receive_us = decode_frames = 0;
                        resize_us = resize_frames = enqueue_us = enqueue_frames = 0;
                        stats_t = now;
                    }
                }
            }
            av_packet_unref(packet);//释放packet的内存，避免内存泄漏
        }

        // 6. 连接断开清理，准备下一轮重连
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        std::cout << log_tag() << "[PullStream] Connection closed. Reconnecting...\n" << std::flush;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
}

int PullStreamThread::interrupt_callback(void* opaque) {
    const auto* self = static_cast<const PullStreamThread*>(opaque);
    return self == nullptr || !self->running_.load();
}
