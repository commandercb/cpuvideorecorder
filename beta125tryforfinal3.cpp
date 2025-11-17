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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

volatile bool stopRecording = false;
void signalHandler(int) { stopRecording = true; }

std::string getTimestampedFilename() {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &timeT);

    std::ostringstream oss;
    oss << "dxgi_output_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".avi";
    return oss.str();
}

// --- Frame queue ---
struct FrameItem {
    AVFrame* frame;
    int64_t pts;
};

class FrameQueue {
public:
    void push(FrameItem item) {
        std::unique_lock<std::mutex> lock(mtx);
        q.push(item);
        cv.notify_one();
    }

    bool pop(FrameItem& item) {
        std::unique_lock<std::mutex> lock(mtx);
        while (q.empty() && !stopRecording) cv.wait(lock);
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
};

// --- Frame pool ---
class FramePool {
public:
    FramePool(int size, int width, int height, AVPixelFormat pix_fmt) {
        for (int i = 0; i < size; ++i) {
            AVFrame* f = av_frame_alloc();
            f->format = pix_fmt;
            f->width = width;
            f->height = height;
            av_frame_get_buffer(f, 32); // allocate data buffers
            freeFrames.push(f);
        }
    }

    AVFrame* acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        if (freeFrames.empty()) return nullptr;
        AVFrame* f = freeFrames.front();
        freeFrames.pop();
        return f;
    }

    void release(AVFrame* f) {
        // Do NOT unref buffer, just reset PTS
        f->pts = 0;
        std::unique_lock<std::mutex> lock(mtx);
        freeFrames.push(f);
    }

private:
    std::queue<AVFrame*> freeFrames;
    std::mutex mtx;
};

int main() {
    signal(SIGINT, signalHandler);

    const int width = 1280;
    const int height = 720;
    const int targetFPS = 30;
    std::string filename = getTimestampedFilename();

    // --- 1. D3D11 device ---
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        std::cerr << "Failed to create D3D11 device\n";
        return -1;
    }

    // --- 2. DXGI duplication ---
    ComPtr<IDXGIDevice> dxgiDevice;
    device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);
    ComPtr<IDXGIOutputDuplication> duplication;
    if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
        std::cerr << "Failed to duplicate output\n";
        return -1;
    }

    // --- 3. FFmpeg init ---
    avformat_network_init();
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
    codecCtx->bit_rate = 12 * 1000 * 1000; // 12 Mbps
    codecCtx->rc_buffer_size = codecCtx->bit_rate;
    codecCtx->rc_max_rate = codecCtx->bit_rate;
    codecCtx->gop_size = 120; 
    codecCtx->max_b_frames = 0;
    codecCtx->time_base = {1, targetFPS};
    codecCtx->thread_count = 1;
    av_opt_set(codecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx->priv_data, "tune", "fastdecode", 0);
    av_opt_set(codecCtx->priv_data, "profile", "main", 0);

    AVStream* videoStream = avformat_new_stream(outCtx, codec);
    if (!videoStream) { std::cerr << "Failed to create stream\n"; return -1; }
    videoStream->time_base = codecCtx->time_base;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        std::cerr << "Failed to open codec\n"; return -1;
    }
    avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Failed to open output file\n"; return -1;
        }
    }

    if (avformat_write_header(outCtx, nullptr) < 0) {
        std::cerr << "Error writing header\n"; return -1;
    }

    // --- 4. SwsContext ---
    SwsContext* swsCtx = sws_getContext(
        width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    FrameQueue frameQueue;

    // --- CPU staging texture ---
    D3D11_TEXTURE2D_DESC cpuDesc = {};
    cpuDesc.Width = width;
    cpuDesc.Height = height;
    cpuDesc.MipLevels = 1;
    cpuDesc.ArraySize = 1;
    cpuDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    cpuDesc.SampleDesc.Count = 1;
    cpuDesc.Usage = D3D11_USAGE_STAGING;
    cpuDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    cpuDesc.BindFlags = 0;
    cpuDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> cpuTexture;
    device->CreateTexture2D(&cpuDesc, nullptr, &cpuTexture);

    // --- Frame pool ---
    FramePool framePool(50, width, height, codecCtx->pix_fmt);

    // --- 5. Encoder thread ---
    std::thread encoderThread([&]() {
        AVPacket pkt = {};
        FrameItem item;
        while (!stopRecording || !frameQueue.empty()) {
            if (!frameQueue.pop(item)) continue;

            if (avcodec_send_frame(codecCtx, item.frame) < 0) {
                std::cerr << "Error sending frame to encoder\n";
                break;
            }

            while (avcodec_receive_packet(codecCtx, &pkt) == 0) {
                av_interleaved_write_frame(outCtx, &pkt);
                av_packet_unref(&pkt);
            }

            framePool.release(item.frame);
        }

        // Flush encoder
        while (avcodec_receive_packet(codecCtx, &pkt) == 0) {
            av_interleaved_write_frame(outCtx, &pkt);
            av_packet_unref(&pkt);
        }
    });

    std::cout << "Recording... Ctrl+C to stop\n";

    // --- 6. Capture loop ---
    int64_t frameCounter = 0;
    auto nextFrameTime = std::chrono::high_resolution_clock::now();
    const auto frameInterval = std::chrono::milliseconds(1000 / targetFPS);

    while (!stopRecording) {
        auto now = std::chrono::high_resolution_clock::now();

        if (now > nextFrameTime + frameInterval) {
            int skipCount = std::chrono::duration_cast<std::chrono::milliseconds>(now - nextFrameTime).count() / frameInterval.count();
            frameCounter += skipCount;
            nextFrameTime += frameInterval * (skipCount + 1);
        } else {
            std::this_thread::sleep_until(nextFrameTime);
            nextFrameTime += frameInterval;
        }

        ComPtr<IDXGIResource> desktopResource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        if (duplication->AcquireNextFrame(250, &frameInfo, &desktopResource) != S_OK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        ComPtr<ID3D11Texture2D> frameTexture;
        desktopResource.As(&frameTexture);

        context->CopyResource(cpuTexture.Get(), frameTexture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        context->Map(cpuTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);

        AVFrame* frameYUV = framePool.acquire();
        if (!frameYUV) {
            std::cerr << "Pool empty, skipping frame\n";
            context->Unmap(cpuTexture.Get(), 0);
            duplication->ReleaseFrame();
            continue;
        }

        av_frame_make_writable(frameYUV);

        uint8_t* srcData[1] = { (uint8_t*)mapped.pData };
        int srcLinesize[1] = { (int)mapped.RowPitch };
        sws_scale(swsCtx, srcData, srcLinesize, 0, height, frameYUV->data, frameYUV->linesize);

int scaled = sws_scale(swsCtx, srcData, srcLinesize, 0, height, frameYUV->data, frameYUV->linesize);
if (scaled <= 0) {
    std::cerr << "sws_scale failed\n";
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

    std::cout << "Recording finished: " << filename << "\n";
    return 0;
}
