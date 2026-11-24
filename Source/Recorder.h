#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_graphics/juce_graphics.h>

#include <atomic>
#include <memory>
#include <vector>

#include "MediaWriters.h"

//==============================================================================
// Захват звука с микрофона (системное устройство ввода) в кольцевой буфер, моно.
class MicCapture : private juce::AudioIODeviceCallback
{
public:
    MicCapture();
    ~MicCapture() override;

    // Имена устройств ввода (устройство при этом не открывается)
    static juce::StringArray getInputDeviceNames();

    // Открывает микрофон. Пустое имя = устройство по умолчанию.
    // Возвращает текст ошибки или пустую строку, если всё получилось.
    juce::String start (const juce::String& deviceName);
    void stop();

    double getSampleRate() const noexcept { return sampleRate.load(); }
    int getNumAvailable() const noexcept  { return fifo.getNumReady(); }

    // Забирает до maxSamples накопленных отсчётов, возвращает сколько забрано
    int read (float* dest, int maxSamples);
    void discardAll();

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    juce::AudioDeviceManager deviceManager;
    juce::AbstractFifo fifo { 48000 * 4 };
    std::vector<float> buffer;
    std::atomic<double> sampleRate { 0.0 };
    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicCapture)
};

//==============================================================================
// Запись в фоновом потоке:
//  - aviWithAudio: один AVI-файл (MJPEG-видео из кадров камеры + звук с микрофона);
//  - wavOnly: только звук в WAV (видео в это время пишет сама камера).
// Длинные AVI-записи автоматически делятся на части (_part2, _part3 ...).
class AvRecorder : private juce::Thread
{
public:
    enum class Mode { aviWithAudio, wavOnly };

    AvRecorder();
    ~AvRecorder() override;

    // Возвращает текст ошибки или пустую строку. Для aviWithAudio нужен размер кадров камеры.
    juce::String start (Mode modeToUse, const juce::File& file, int videoWidth, int videoHeight,
                        const juce::String& micName);

    // Новый кадр камеры (только для aviWithAudio). Кадр после передачи не менять.
    void pushFrame (const juce::Image& frame);

    // Только для wavOnly: микрофон уже открыт, но звук в файл не пишется, пока не вызван этот метод.
    // Вызывать сразу после запуска записи видео камерой, чтобы начало звука совпало с началом видео.
    void beginAudio();

    void stop();

    bool isRunning() const    { return isThreadRunning(); }
    bool hasFailed() const noexcept { return failed.load(); }

private:
    void run() override;
    bool openPart (int partIndex);
    bool encodeFrame (const juce::Image& frame);
    bool writeVideoFrame();
    void pumpAudio (bool flushAll);
    void finishFiles();

    Mode mode = Mode::aviWithAudio;
    juce::File baseFile;
    int width = 0, height = 0, sampleRate = 48000, part = 1;

    MicCapture mic;
    std::unique_ptr<media::AviWriter> avi;
    std::unique_ptr<media::WavWriter> wav;

    juce::CriticalSection frameLock;
    juce::Image latestFrame;
    bool newFrame = false;

    juce::JPEGImageFormat jpeg;
    std::vector<uint8_t> lastJpeg;
    std::vector<float> floatBuffer;
    std::vector<int16_t> pcmBuffer;

    std::atomic<bool> failed { false };
    std::atomic<bool> startRequested { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AvRecorder)
};
