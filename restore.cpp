#ifdef _WIN32
#include <windows.h>
#endif
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

namespace fs = std::filesystem;

// 解码视频帧并保存为 JPEG 图像
bool decode_and_save_frame(AVFrame* frame, AVCodecContext* codec_ctx, 
                          const std::string& output_path, 
                          int quality = 100) {
    // 查找 JPEG 编码器
    const AVCodec* jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jpeg_codec) {
        std::cerr << "JPEG 编码器未找到" << std::endl;
        return false;
    }

    // 创建 JPEG 编码器上下文
    AVCodecContext* jpeg_ctx = avcodec_alloc_context3(jpeg_codec);
    if (!jpeg_ctx) {
        std::cerr << "无法分配 JPEG 编码器上下文" << std::endl;
        return false;
    }

    // 设置编码器参数
    jpeg_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;  // JPEG 兼容格式
    jpeg_ctx->time_base = {1, 30};            // 帧率
    jpeg_ctx->width = frame->width;
    jpeg_ctx->height = frame->height;
    
    // 设置 JPEG 质量 (1-100, 100 为最高质量)
    av_opt_set_int(jpeg_ctx, "qscale", quality, 0);

    // 打开编码器
    if (avcodec_open2(jpeg_ctx, jpeg_codec, nullptr) < 0) {
        std::cerr << "无法打开 JPEG 编码器" << std::endl;
        avcodec_free_context(&jpeg_ctx);
        return false;
    }

    // 创建数据包
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "无法分配数据包" << std::endl;
        avcodec_free_context(&jpeg_ctx);
        return false;
    }

    // 发送帧到编码器
    int ret = avcodec_send_frame(jpeg_ctx, frame);
    if (ret < 0) {
        std::cerr << "发送帧到编码器失败: " << av_err2str(ret) << std::endl;
        av_packet_free(&pkt);
        avcodec_free_context(&jpeg_ctx);
        return false;
    }

    // 接收编码后的数据包
    ret = avcodec_receive_packet(jpeg_ctx, pkt);
    if (ret < 0) {
        std::cerr << "接收数据包失败: " << av_err2str(ret) << std::endl;
        av_packet_free(&pkt);
        avcodec_free_context(&jpeg_ctx);
        return false;
    }

    // 将编码后的数据写入文件
    FILE* file = fopen(output_path.c_str(), "wb");
    if (!file) {
        std::cerr << "无法打开输出文件: " << output_path << std::endl;
        av_packet_free(&pkt);
        avcodec_free_context(&jpeg_ctx);
        return false;
    }

    size_t written = fwrite(pkt->data, 1, pkt->size, file);
    if (written != pkt->size) {
        std::cerr << "写入文件不完整: 预期 " << pkt->size << " 字节，实际写入 " << written << " 字节" << std::endl;
    }
    fclose(file);

    // 清理资源
    av_packet_free(&pkt);
    avcodec_free_context(&jpeg_ctx);

    return true;
}

// 主解码函数
bool decode_video_to_images(const std::string& video_path,
                            const std::string& txt_path,
                            const std::string& output_dir) {
    // 确保输出目录存在
    if (!fs::exists(output_dir) && !fs::create_directories(output_dir)) {
        std::cerr << "无法创建输出目录: " << output_dir << std::endl;
        return false;
    }

    // 检查索引文件是否存在
    if (!fs::exists(txt_path)) {
        std::cerr << "索引文件不存在: " << txt_path << std::endl;
        return false;
    }

    // 读取帧索引映射
    std::ifstream index_file(txt_path);
    if (!index_file.is_open()) {
        std::cerr << "无法打开索引文件: " << txt_path << std::endl;
        return false;
    }

    std::vector<std::string> frame_indices;
    std::string line;
    while (std::getline(index_file, line)) {
        if (!line.empty()) {
            frame_indices.push_back(line);
        }
    }
    index_file.close();

    // 注册所有编解码器
    avformat_network_init();
    
    // 检查视频文件是否存在
    if (!fs::exists(video_path)) {
        std::cerr << "视频文件不存在: " << video_path << std::endl;
        return false;
    }

    // 打开视频文件
    AVFormatContext* format_ctx = nullptr;
    int ret = avformat_open_input(&format_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret != 0) {
        std::cerr << "无法打开视频文件: " << video_path << std::endl;
        std::cerr << "错误代码: " << av_err2str(ret) << std::endl;
        return false;
    }

    // 获取流信息
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "无法获取流信息" << std::endl;
        avformat_close_input(&format_ctx);
        return false;
    }

    // 查找视频流
    int video_stream_index = -1;
    AVCodecParameters* codec_params = nullptr;
    const AVCodec* codec = nullptr;
    
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            codec_params = format_ctx->streams[i]->codecpar;
            codec = avcodec_find_decoder(codec_params->codec_id);
            break;
        }
    }
    
    if (video_stream_index == -1 || !codec) {
        std::cerr << "未找到视频流或解码器" << std::endl;
        avformat_close_input(&format_ctx);
        return false;
    }

    // 创建解码器上下文
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "无法分配解码器上下文" << std::endl;
        avformat_close_input(&format_ctx);
        return false;
    }

    // 复制编解码器参数到上下文
    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        std::cerr << "无法复制编解码器参数" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return false;
    }

    // 强制使用软件解码
    codec_ctx->hw_device_ctx = nullptr;
    codec_ctx->get_format = nullptr; // 禁用硬件加速回调

    // 打开解码器
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "无法打开解码器" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return false;
    }

    // 创建帧和包
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (!frame || !packet) {
        std::cerr << "无法分配帧或包" << std::endl;
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return false;
    }

    // 创建图像转换上下文 (如果需要)
    SwsContext* sws_ctx = nullptr;
    if (codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P && 
        codec_ctx->pix_fmt != AV_PIX_FMT_YUVJ420P) {
        sws_ctx = sws_getContext(
            codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
            codec_ctx->width, codec_ctx->height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!sws_ctx) {
            std::cerr << "无法创建图像转换上下文" << std::endl;
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            return false;
        }
    }

    // 解码循环
    int frame_count = 0;
    int decoded_frames = 0;
    bool success = true;

    while (true) {
        ret = av_read_frame(format_ctx, packet);
        if (ret < 0) {
            break;
        }

        if (packet->stream_index == video_stream_index) {
            // 检查解码器上下文和数据包是否有效
            if (!codec_ctx || !packet) {
                std::cerr << "无效的解码器上下文或数据包" << std::endl;
                av_packet_unref(packet);
                continue;
            }
            
            // 发送数据包到解码器
            ret = avcodec_send_packet(codec_ctx, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            // 接收解码后的帧
            while (true) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "接收帧失败: " << av_err2str(ret) << std::endl;
                    success = false;
                    break;
                }

                // 检查是否超出索引范围
                if (frame_count >= static_cast<int>(frame_indices.size())) {
                    av_frame_unref(frame);
                    break;
                }

                // 准备输出文件路径
                std::string output_path = output_dir + "/" + frame_indices[frame_count] + ".jpg";

                AVFrame* output_frame = frame;
                AVFrame* converted_frame = nullptr;

                // 如果像素格式不兼容，进行转换
                if (sws_ctx) {
                    converted_frame = av_frame_alloc();
                    if (!converted_frame) {
                        std::cerr << "无法分配转换帧" << std::endl;
                        success = false;
                        break;
                    }

                    converted_frame->format = AV_PIX_FMT_YUV420P;
                    converted_frame->width = frame->width;
                    converted_frame->height = frame->height;

                    if (av_frame_get_buffer(converted_frame, 0) < 0) {
                        std::cerr << "无法分配转换帧缓冲区" << std::endl;
                        av_frame_free(&converted_frame);
                        success = false;
                        break;
                    }

                    // 执行转换
                    int convert_ret = sws_scale(sws_ctx, 
                              frame->data, frame->linesize, 0, frame->height,
                              converted_frame->data, converted_frame->linesize);
                    if (convert_ret <= 0) {
                        std::cerr << "图像转换失败" << std::endl;
                        av_frame_free(&converted_frame);
                        success = false;
                        break;
                    }

                    output_frame = converted_frame;
                }

                // 保存帧为JPEG
                if (!decode_and_save_frame(output_frame, codec_ctx, output_path)) {
                    std::cerr << "保存帧失败: " << output_path << std::endl;
                    success = false;
                } else {
                    decoded_frames++;
                }

                // 清理转换帧
                if (converted_frame) {
                    av_frame_free(&converted_frame);
                }
                
                frame_count++;
            }
        }
        
        av_packet_unref(packet);
    }

    // 刷新解码器缓冲区
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
        // 处理剩余的帧（如果有）
        if (frame_count < static_cast<int>(frame_indices.size())) {
            std::string output_path = output_dir + "/" + frame_indices[frame_count] + ".jpg";
            if (!decode_and_save_frame(frame, codec_ctx, output_path)) {
                std::cerr << "保存帧失败: " << output_path << std::endl;
                success = false;
            } else {
                decoded_frames++;
            }
            frame_count++;
        }
    }

    // 清理资源
    if (sws_ctx) sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    
    return success;
}

int main(int argc, char* argv[]) {
    #ifdef _WIN32
    // 设置 DLL 搜索路径 - 指向本地 FFmpeg 安装目录
    const char* dllPath = "D:\\ffmpeg-n7.1.1-56-gc2184b65d2-win64-gpl-shared-7.1\\bin";
    if (!SetDllDirectoryA(dllPath)) {
        std::cerr << "Failed to set DLL directory: " << GetLastError() << std::endl;
        return 1;
    }
    #endif

    // 定义摄像头配置
    const std::vector<std::string> cameras = {
        "ofilm_around_front_190_3M",
        "ofilm_around_rear_190_3M",
        "ofilm_around_left_190_3M",
        "ofilm_around_right_190_3M"
    };
    
    // 默认路径
    std::string video_dir = "c:\\Users\\bykong4\\Desktop\\image\\video";
    std::string output_base = "c:\\Users\\bykong4\\Desktop\\image";
    
    // 处理每个摄像头
    bool all_success = true;
    for (const auto& prefix : cameras) {
        std::string video_path = video_dir + "\\" + prefix + ".mp4";
        std::string txt_path = video_dir + "\\" + prefix + ".txt";
        std::string output_dir = output_base + "\\" + prefix;
        
        // 检查文件是否存在
        if (!fs::exists(video_path)) {
            std::cerr << "错误: 视频文件不存在: " << video_path << std::endl;
            all_success = false;
            continue;
        }
        
        if (!fs::exists(txt_path)) {
            std::cerr << "错误: 索引文件不存在: " << txt_path << std::endl;
            all_success = false;
            continue;
        }
        
        // 确保输出目录存在
        if (!fs::exists(output_dir) && !fs::create_directories(output_dir)) {
            std::cerr << "错误: 无法创建输出目录: " << output_dir << std::endl;
            all_success = false;
            continue;
        }
        
        if (!decode_video_to_images(video_path, txt_path, output_dir)) {
            std::cerr << "处理失败: " << prefix << std::endl;
            all_success = false;
        }
    }
    
    if (all_success) {
        std::cout << "所有摄像头视频处理成功!" << std::endl;
    } else {
        std::cerr << "部分摄像头视频处理失败，请查看错误信息了解详情。" << std::endl;
    }

    return 0;
}
