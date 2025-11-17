//
// recorder.cpp
// Compile with:
// audio09e.cpp
// Single-file screen + audio recorder using WASAPI and FFmpeg (FFmpeg 6.x+ friendly)
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <comdef.h>
#include <cstdint>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <iomanip>


#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

// WAV header struct (unchanged)
struct WAVHeader {
    char riff[4] = {'R','I','F','F'};
    uint32_t chunkSize;
    char wave[4] = {'W','A','V','E'};
    char fmt[4] = {'f','m','t',' '};
    uint32_t subChunk1Size = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4] = {'d','a','t','a'};
    uint32_t dataSize;
};

// Function to write the WAV header (unchanged)
void WriteWAVHeader(std::ofstream &outFile, uint16_t channels, uint32_t sampleRate, uint16_t bitsPerSample, uint32_t dataSize) {
    WAVHeader header;
    header.numChannels = channels;
    header.sampleRate = sampleRate;
    header.bitsPerSample = bitsPerSample;
    header.byteRate = sampleRate * channels * bitsPerSample / 8;
    header.blockAlign = channels * bitsPerSample / 8;
    header.dataSize = dataSize;
    header.chunkSize = 36 + dataSize;

    outFile.write(reinterpret_cast<char*>(&header), sizeof(header));
}

// Function to get the current time as a formatted string (YYYYMMDD_HHMMSS)
std::string GetTimestamp() {
    // Get current time
    std::time_t now = std::time(nullptr);
    std::tm* tmStruct = std::localtime(&now);

    // Format time into YYYYMMDD_HHMMSS
    std::stringstream ss;
    ss << (tmStruct->tm_year + 1900)  // year
       << std::setw(2) << std::setfill('0') << (tmStruct->tm_mon + 1) // month
       << std::setw(2) << std::setfill('0') << tmStruct->tm_mday      // day
       << "_"
       << std::setw(2) << std::setfill('0') << tmStruct->tm_hour      // hour
       << std::setw(2) << std::setfill('0') << tmStruct->tm_min       // minute
       << std::setw(2) << std::setfill('0') << tmStruct->tm_sec;      // second
    return ss.str();
}

int main() {
    CoInitialize(nullptr);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
        std::cerr << "Failed to create enumerator\n"; return -1;
    }

    if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) {
        std::cerr << "Failed to get default device\n"; return -1;
    }

    if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient))) {
        std::cerr << "Failed to activate audio client\n"; return -1;
    }

    WAVEFORMATEX* pwfx = nullptr;
    if (FAILED(pAudioClient->GetMixFormat(&pwfx))) {
        std::cerr << "Failed to get mix format\n"; return -1;
    }

    if (FAILED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                        10000000, 0, pwfx, nullptr))) {
        std::cerr << "Failed to initialize audio client\n"; return -1;
    }

    if (FAILED(pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient))) {
        std::cerr << "Failed to get capture client\n"; return -1;
    }

    if (FAILED(pAudioClient->Start())) {
        std::cerr << "Failed to start capture\n"; return -1;
    }

    std::cout << "Recording system audio. Press Enter to stop...\n";

    // Generate a dynamic file name with timestamp
    std::string fileName = "recording_" + GetTimestamp() + ".wav";

    std::ofstream outFile(fileName, std::ios::binary);
    WriteWAVHeader(outFile, pwfx->nChannels, pwfx->nSamplesPerSec, 16, 0); // 16-bit PCM output

    std::vector<BYTE> audioData;
    bool recording = true;
    float maxSample = 0.0f;

    while (recording) {
        UINT32 packetLength = 0;
        if (FAILED(pCaptureClient->GetNextPacketSize(&packetLength))) break;

        while (packetLength != 0) {
            BYTE* pData;
            UINT32 numFrames;
            DWORD flags;

            if (FAILED(pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr))) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                float* fData = reinterpret_cast<float*>(pData);
                for (UINT32 i = 0; i < numFrames * pwfx->nChannels; i++) {
                    maxSample = std::max(maxSample, std::abs(fData[i]));
                }

                for (UINT32 i = 0; i < numFrames * pwfx->nChannels; i++) {
                    float sample = fData[i];
                    if (maxSample > 1.0f) sample /= maxSample; // dynamic scaling
                    int16_t s16 = static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
                    audioData.push_back(s16 & 0xFF);
                    audioData.push_back((s16 >> 8) & 0xFF);
                }
            } else {
                audioData.insert(audioData.end(), numFrames * pwfx->nBlockAlign, 0);
            }

            if (FAILED(pCaptureClient->ReleaseBuffer(numFrames))) break;
            if (FAILED(pCaptureClient->GetNextPacketSize(&packetLength))) break;
        }

        if (GetAsyncKeyState(VK_RETURN) & 0x8000) recording = false;
        Sleep(10);
    }

    pAudioClient->Stop();

    outFile.seekp(0, std::ios::beg);
    WriteWAVHeader(outFile, pwfx->nChannels, pwfx->nSamplesPerSec, 16, static_cast<uint32_t>(audioData.size()));
    outFile.write(reinterpret_cast<char*>(audioData.data()), audioData.size());
    outFile.close();

    CoTaskMemFree(pwfx);
    if (pCaptureClient) pCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();
    CoUninitialize();

    std::cout << "Recording finished: " << fileName << "\n";
    return 0;
}
