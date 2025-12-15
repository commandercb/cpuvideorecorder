// beta125tryforfinal11.cpp
// DXGI capture -> libswscale -> libx264 (H.264) -> AVI/MP4
// Improved error checking and correct FFmpeg packet/flush handling.
//    $ g++ -std=gnu++17 -O2 -o beta125tryforfinal10.exe beta125tryforfinal10.cpp   -lgdi32 -luser32 -ld3d11 -ldxgi -ld3dcompiler   -lavformat -lavcodec -lavutil -lswscale -lswresample   -lz -lbz2 -lwinmm -lavfilter -lavdevice   -lws2_32 -lstdc++fs

// dxgi_capture_split.cpp
// DXGI capture -> libswscale -> libx264 (H.264) -> AVI/MP4
// Supports dynamic frame pool and splitting files at ~1.9 GB

// DXGI Recorder with Safe Real-Time Timing Fix
// Supports dynamic frame pool and splitting files at ~1.9 GB

// DXGI Recorder with Safe Real-Time Timing Fix
// Supports dynamic frame pool and splitting files at ~1.9 GB
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <thread>
#include <csignal>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

std::atomic<bool> stopRecording(false);
void signalHandler(int) { stopRecording = true; }

std::string getTimestampedFilename(int index = 0) {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &timeT);

    std::ostringstream oss;
    oss << "dxgi_output_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
    if (index > 0) oss << "_" << index;
    oss << ".avi";
    return oss.str();
}

static void ff_err(int ret) {
    if (ret >= 0) return;
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    std::cerr << "FFmpeg error: " << buf << " (" << ret << ")\n";
}

// ---------------- Frame Queue ----------------
struct FrameItem {
    AVFrame* frame;
    int64_t pts;
};

class FrameQueue {
public:
    FrameQueue(size_t maxSize = 60) : maxSize(maxSize) {}

    void push(FrameItem item) {
        std::unique_lock<std::mutex> lock(mtx);
        if (q.size() >= maxSize) {
            FrameItem old = q.front();
            q.pop();
            if (old.frame) av_frame_free(&old.frame);
        }
        q.push(item);
        cv.notify_one();
    }

    bool pop(FrameItem& item) {
        std::unique_lock<std::mutex> lock(mtx);
        while (q.empty() && !stopRecording.load()) cv.wait(lock);
        if (q.empty()) return false;
        item = q.front();
        q.pop();
        return true;
    }

    bool empty() {
        std::unique_lock<std::mutex> lock(mtx);
        return q.empty();
    }

private:
    std::queue<FrameItem> q;
    std::mutex mtx;
    std::condition_variable cv;
    size_t maxSize;
};

// ---------------- Frame Pool ----------------
class FramePool {
public:
    FramePool(int size, int width, int height, AVPixelFormat pix_fmt)
        : width(width), height(height), pix_fmt(pix_fmt) {
        for (int i = 0; i < size; ++i) {
            AVFrame* f = createFrame();
            if (f) freeFrames.push(f);
        }
    }

    ~FramePool() {
        while (!freeFrames.empty()) {
            AVFrame* f = freeFrames.front();
            freeFrames.pop();
            av_frame_free(&f);
        }
    }

    AVFrame* acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        if (freeFrames.empty()) return createFrame();
        AVFrame* f = freeFrames.front();
        freeFrames.pop();
        return f;
    }

    void release(AVFrame* f) {
        if (!f) return;
        f->pts = 0;
        std::unique_lock<std::mutex> lock(mtx);
        freeFrames.push(f);
    }

private:
    AVFrame* createFrame() {
        AVFrame* f = av_frame_alloc();
        if (!f) return nullptr;
        f->format = pix_fmt;
        f->width = width;
        f->height = height;
        if (av_frame_get_buffer(f, 32) < 0) {
            av_frame_free(&f);
            return nullptr;
        }
        return f;
    }

    int width, height;
    AVPixelFormat pix_fmt;
    std::queue<AVFrame*> freeFrames;
    std::mutex mtx;
};

int main() {
    signal(SIGINT, signalHandler);
    av_log_set_level(AV_LOG_ERROR);

    const int width = 1600;
    const int height = 900;
    const int targetFPS = 60; // CFR for file, DXGI-paced capture xxx was 30

    // ---------------- D3D11 / DXGI ----------------
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) { std::cerr << "Failed to create D3D11 device\n"; return -1; }

    ComPtr<IDXGIDevice> dxgiDevice;
    device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) { std::cerr << "GetAdapter failed\n"; return -1; }
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) { std::cerr << "EnumOutputs failed\n"; return -1; }
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);
    ComPtr<IDXGIOutputDuplication> duplication;
    if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) { std::cerr << "DuplicateOutput failed\n"; return -1; }

    // ---------------- FFmpeg Output ----------------
    int fileIndex = 0;
    std::string filename = getTimestampedFilename(fileIndex);
    AVFormatContext* outCtx = nullptr;
    if (avformat_alloc_output_context2(&outCtx, nullptr, "avi", filename.c_str()) < 0) {
        std::cerr << "Failed to allocate output context\n"; return -1;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) { std::cerr << "H.264 codec not found\n"; return -1; }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = 12 * 1000 * 1000;
    codecCtx->gop_size = 120;
    codecCtx->max_b_frames = 0;
    codecCtx->time_base = AVRational{ 1, targetFPS };
    codecCtx->framerate = AVRational{ targetFPS, 1 };
    codecCtx->thread_count = 4;

    av_opt_set(codecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx->priv_data, "tune", "fastdecode", 0);
    av_opt_set(codecCtx->priv_data, "profile", "main", 0);

    if (outCtx->oformat->flags & AVFMT_GLOBALHEADER)
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVStream* videoStream = avformat_new_stream(outCtx, codec);
    videoStream->time_base = codecCtx->time_base;
    videoStream->avg_frame_rate = codecCtx->framerate;
    videoStream->r_frame_rate = codecCtx->framerate;

    int ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0) { ff_err(ret); return -1; }

    ret = avcodec_parameters_from_context(videoStream->codecpar, codecCtx);
    if (ret < 0) { ff_err(ret); return -1; }

    if (!(outCtx->oformat->flags & AVFMT_NOFILE))
        avio_open(&outCtx->pb, filename.c_str(), AVIO_FLAG_WRITE);

    ret = avformat_write_header(outCtx, nullptr);
    if (ret < 0) { ff_err(ret); return -1; }

    // ---------------- Scaling ----------------
    SwsContext* swsCtx = sws_getContext(
        width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    FrameQueue frameQueue(60);
    FramePool framePool(200, width, height, codecCtx->pix_fmt);

    // ---------------- CPU Readback Texture ----------------
    D3D11_TEXTURE2D_DESC cpuDesc = {};
    cpuDesc.Width = width;
    cpuDesc.Height = height;
    cpuDesc.MipLevels = 1;
    cpuDesc.ArraySize = 1;
    cpuDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    cpuDesc.SampleDesc.Count = 1;
    cpuDesc.Usage = D3D11_USAGE_STAGING;
    cpuDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> cpuTexture;
    hr = device->CreateTexture2D(&cpuDesc, nullptr, &cpuTexture);
    if (FAILED(hr) || !cpuTexture) { std::cerr << "CreateTexture2D failed\n"; return -1; }

    // ---------------- Encoder Thread ----------------
    std::thread encoderThread([&]() {
        AVPacket* pkt = av_packet_alloc();
        FrameItem item;
        while (!stopRecording.load() || !frameQueue.empty()) {
            if (!frameQueue.pop(item)) continue;

            int sret = avcodec_send_frame(codecCtx, item.frame);
            framePool.release(item.frame);
            if (sret < 0) { ff_err(sret); break; }

            while (true) {
                int r = avcodec_receive_packet(codecCtx, pkt);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
                if (r < 0) { ff_err(r); break; }

                pkt->stream_index = videoStream->index;
                av_interleaved_write_frame(outCtx, pkt);
                av_packet_unref(pkt);

                if (avio_tell(outCtx->pb) >= 3900000000) {
                    av_write_trailer(outCtx);
                    avio_close(outCtx->pb);
                    avformat_free_context(outCtx);

                    fileIndex++;
                    filename = getTimestampedFilename(fileIndex);
                    avformat_alloc_output_context2(&outCtx, nullptr, "avi", filename.c_str());
                    avio_open(&outCtx->pb, filename.c_str(), AVIO_FLAG_WRITE);

                    videoStream = avformat_new_stream(outCtx, codec);
                    videoStream->time_base = codecCtx->time_base;
                    videoStream->avg_frame_rate = codecCtx->framerate;
                    videoStream->r_frame_rate = codecCtx->framerate;
                    avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

                    int wr = avformat_write_header(outCtx, nullptr);
                    if (wr < 0) { ff_err(wr); return; }
                }
            }
        }

        avcodec_send_frame(codecCtx, nullptr);
        while (true) {
            int r = avcodec_receive_packet(codecCtx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) break;
            pkt->stream_index = videoStream->index;
            av_interleaved_write_frame(outCtx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    });

    std::cout << "Recording (low-impact DXGI)... Ctrl+C to stop\n";

    // ---------------- Capture Loop (DXGI-paced, NON-BLOCKING) ----------------
    int64_t frameCounter = 0;

    while (!stopRecording.load()) {
        ComPtr<IDXGIResource> desktopResource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

        HRESULT a = duplication->AcquireNextFrame(
            0, // NON-BLOCKING for lowest gameplay impact
            &frameInfo,
            &desktopResource);

        if (a == DXGI_ERROR_WAIT_TIMEOUT)
            continue;
        if (a == DXGI_ERROR_ACCESS_LOST || a == DXGI_ERROR_INVALID_CALL)
            continue;
        if (FAILED(a))
            continue;

        ComPtr<ID3D11Texture2D> frameTexture;
        desktopResource.As(&frameTexture);
        context->CopyResource(cpuTexture.Get(), frameTexture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(cpuTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            duplication->ReleaseFrame();
            continue;
        }

        AVFrame* frameYUV = framePool.acquire();
        if (!frameYUV) {
            context->Unmap(cpuTexture.Get(), 0);
            duplication->ReleaseFrame();
            continue;
        }

        av_frame_make_writable(frameYUV);

        uint8_t* srcData[1] = { (uint8_t*)mapped.pData };
        int srcLinesize[1] = { (int)mapped.RowPitch };
        int scaled = sws_scale(swsCtx, srcData, srcLinesize, 0, height, frameYUV->data, frameYUV->linesize);
        if (scaled <= 0) {
            framePool.release(frameYUV);
            context->Unmap(cpuTexture.Get(), 0);
            duplication->ReleaseFrame();
            continue;
        }

        frameYUV->pts = frameCounter++;
        frameQueue.push({ frameYUV, frameYUV->pts });

        context->Unmap(cpuTexture.Get(), 0);
        duplication->ReleaseFrame();
    }

    encoderThread.join();

    av_write_trailer(outCtx);
    sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);
    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) avio_close(outCtx->pb);
    avformat_free_context(outCtx);

    std::cout << "Recording finished\n";
    return 0;
}
