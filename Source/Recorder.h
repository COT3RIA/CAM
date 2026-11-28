#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_graphics/juce_graphics.h>

#include <atomic>
#include <memory>
#include <vector>

#include "AudioAligner.h"
#include "HostAudio.h"
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

    // Пик (0..1) с момента прошлого вызова; для индикатора уровня
    float takePeak() noexcept { return peak.exchange (0.0f); }

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
    std::atomic<float> peak { 0.0f };
    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicCapture)
};

//==============================================================================
// Откуда брать звук для записи
struct AudioSettings
{
    enum class Source { microphone, host };

    Source source = Source::microphone;
    juce::String micName;               // микрофон: пусто = по умолчанию
    HostAudioCapture* host = nullptr;   // звук DAW (стерео)
    int offsetMs = 0;                   // только для звука DAW: + звук позже видео, - раньше
};

//==============================================================================
// Запись в фоновом потоке:
//  - aviWithAudio: один AVI-файл (MJPEG-видео из кадров камеры + звук);
//  - wavOnly: только звук в WAV (видео в это время пишет сама камера).
// Звук: микрофон (моно) или мастер-шина DAW (стерео).
// Длинные AVI-записи автоматически делятся на части (_part2, _part3 ...).
class AvRecorder : private juce::Thread
{
public:
    enum class Mode { aviWithAudio, wavOnly };

    AvRecorder();
    ~AvRecorder() override;

    // Возвращает текст ошибки или пустую строку. Для aviWithAudio нужен размер кадров камеры.
    juce::String start (Mode modeToUse, const juce::File& file, int videoWidth, int videoHeight,
                        const AudioSettings& audio);

    // Новый кадр камеры (только для aviWithAudio). Кадр после передачи не менять.
    void pushFrame (const juce::Image& frame);

    // Только для wavOnly: источник звука уже открыт, но звук в файл не пишется, пока не вызван этот метод.
    // Вызывать сразу после запуска записи видео камерой, чтобы начало звука совпало с началом видео.
    void beginAudio();

    void stop();

    bool isRunning() const    { return isThreadRunning(); }
    bool hasFailed() const noexcept { return failed.load(); }

    // Уровень микрофона для индикатора (0, если источник не микрофон)
    float takeMicPeak() { return mic != nullptr ? mic->takePeak() : 0.0f; }

private:
    void run() override;
    bool openPart (int partIndex);
    bool encodeFrame (const juce::Image& frame);
    bool writeVideoFrame();
    bool writePcm (const int16_t* data, int numFrames);
    bool writeSilence (int64_t numFrames);
    void pumpAudio (bool flushAll);
    void finishFiles();

    int audioAvailable() const;
    int audioRead (float* dest, int maxFrames);
    void audioDiscard();

    Mode mode = Mode::aviWithAudio;
    juce::File baseFile;
    int width = 0, height = 0, sampleRate = 48000, channels = 1, part = 1;

    AudioSettings settings;
    std::unique_ptr<MicCapture> mic;
    HostAudioCapture* activeHost = nullptr;
    media::AudioAligner aligner { 48000, 0, false };
    int64_t audioFramesWritten = 0;

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
