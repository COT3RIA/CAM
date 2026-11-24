#include "Recorder.h"

namespace
{
    constexpr int kFps = 30;
    constexpr int64_t kMaxPartBytes = 1800000000; // ~1.8 ГБ на часть AVI

    // Запись через JUCE: корректно работает с путями в Unicode (кириллица в имени пользователя)
    class JuceFileSink : public media::Sink
    {
    public:
        explicit JuceFileSink (const juce::File& file) : stream (file) {}

        bool isOk() const noexcept { return stream.openedOk(); }

        bool write (const void* data, size_t size) override { return stream.write (data, size); }
        bool seek (int64_t position) override               { return stream.setPosition (position); }
        int64_t tell() override                             { return stream.getPosition(); }

    private:
        juce::FileOutputStream stream;
    };
}

//==============================================================================
MicCapture::MicCapture()
{
    buffer.resize ((size_t) fifo.getTotalSize(), 0.0f);
}

MicCapture::~MicCapture()
{
    stop();
}

juce::StringArray MicCapture::getInputDeviceNames()
{
    juce::AudioDeviceManager manager;
    auto& types = manager.getAvailableDeviceTypes();

    if (types.isEmpty())
        return {};

    auto* type = types.getFirst();
    type->scanForDevices();
    return type->getDeviceNames (true);
}

juce::String MicCapture::start (const juce::String& deviceName)
{
    stop();
    fifo.reset();

    juce::String error;

    if (deviceName.isNotEmpty())
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = deviceName;
        setup.outputDeviceName = {};
        setup.sampleRate = 0.0;
        setup.bufferSize = 0;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;

        // Выходное устройство не нужно (0 каналов), открывается только вход
        error = deviceManager.initialise (1, 0, nullptr, false, {}, &setup);
    }
    else
    {
        error = deviceManager.initialise (1, 0, nullptr, true);
    }

    if (error.isNotEmpty())
        return error;

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return "could not open the input device";

    if (device->getActiveInputChannels().countNumberOfSetBits() == 0)
        return "the input device has no input channels (check microphone permission)";

    sampleRate.store (device->getCurrentSampleRate());
    deviceManager.addAudioCallback (this);
    running = true;
    return {};
}

void MicCapture::stop()
{
    if (running)
    {
        deviceManager.removeAudioCallback (this);
        running = false;
    }

    deviceManager.closeAudioDevice();
}

int MicCapture::read (float* dest, int maxSamples)
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (maxSamples, start1, size1, start2, size2);

    if (size1 > 0)
        juce::FloatVectorOperations::copy (dest, buffer.data() + start1, size1);

    if (size2 > 0)
        juce::FloatVectorOperations::copy (dest + size1, buffer.data() + start2, size2);

    fifo.finishedRead (size1 + size2);
    return size1 + size2;
}

void MicCapture::discardAll()
{
    // Не fifo.reset(): аудиопоток в это время может писать в буфер. Читатель просто выбрасывает накопленное.
    const int available = fifo.getNumReady();

    if (available > 0)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (available, start1, size1, start2, size2);
        fifo.finishedRead (size1 + size2);
    }
}

void MicCapture::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
        sampleRate.store (device->getCurrentSampleRate());
}

void MicCapture::audioDeviceStopped() {}

void MicCapture::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                   float* const* outputChannelData, int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    // Если буфер переполнен (поток записи не успевает), лишнее отбрасывается
    auto mixToMono = [&] (int sampleIndex)
    {
        float sum = 0.0f;
        int count = 0;

        for (int ch = 0; ch < numInputChannels; ++ch)
        {
            if (inputChannelData[ch] != nullptr)
            {
                sum += inputChannelData[ch][sampleIndex];
                ++count;
            }
        }

        return count > 0 ? sum / (float) count : 0.0f;
    };

    for (int i = 0; i < size1; ++i)
        buffer[(size_t) (start1 + i)] = mixToMono (i);

    for (int i = 0; i < size2; ++i)
        buffer[(size_t) (start2 + i)] = mixToMono (size1 + i);

    fifo.finishedWrite (size1 + size2);
}

//==============================================================================
AvRecorder::AvRecorder() : juce::Thread ("Webcam AV recorder")
{
    jpeg.setQuality (0.8f);
    floatBuffer.resize (16384);
    pcmBuffer.resize (16384);
}

AvRecorder::~AvRecorder()
{
    stop();
}

bool AvRecorder::openPart (int partIndex)
{
    juce::File file = baseFile;

    if (partIndex > 1)
        file = baseFile.getSiblingFile (baseFile.getFileNameWithoutExtension()
                                        + "_part" + juce::String (partIndex)
                                        + baseFile.getFileExtension());

    auto sink = std::make_unique<JuceFileSink> (file);

    if (! sink->isOk())
        return false;

    if (mode == Mode::aviWithAudio)
    {
        avi = std::make_unique<media::AviWriter> (std::move (sink));
        return avi->begin (width, height, kFps, sampleRate, 1);
    }

    wav = std::make_unique<media::WavWriter> (std::move (sink));
    return wav->begin (sampleRate, 1);
}

juce::String AvRecorder::start (Mode modeToUse, const juce::File& file, int videoWidth, int videoHeight,
                                const juce::String& micName)
{
    stop();

    mode = modeToUse;
    baseFile = file;
    width = videoWidth;
    height = videoHeight;
    part = 1;
    failed.store (false);
    startRequested.store (mode == Mode::aviWithAudio); // для WAV ждём beginAudio()
    lastJpeg.clear();

    if (mode == Mode::aviWithAudio && (width < 2 || height < 2))
        return "no video frames yet";

    const auto error = mic.start (micName);

    if (error.isNotEmpty())
        return error;

    sampleRate = juce::roundToInt (mic.getSampleRate());

    if (sampleRate <= 0)
    {
        mic.stop();
        return "the input device reported no sample rate";
    }

    if (! openPart (1))
    {
        mic.stop();
        avi.reset();
        wav.reset();
        return "could not create the file: " + file.getFullPathName();
    }

    if (! startThread())
    {
        mic.stop();
        finishFiles();
        return "could not start the recording thread";
    }

    return {};
}

void AvRecorder::pushFrame (const juce::Image& frame)
{
    const juce::ScopedLock sl (frameLock);
    latestFrame = frame;
    newFrame = true;
}

void AvRecorder::beginAudio()
{
    startRequested.store (true);
    notify();
}

void AvRecorder::stop()
{
    mic.stop();

    if (isThreadRunning())
    {
        signalThreadShouldExit();
        notify();
        waitForThreadToExit (5000);
    }

    finishFiles();
}

void AvRecorder::finishFiles()
{
    if (avi != nullptr)
    {
        avi->finish();
        avi.reset();
    }

    if (wav != nullptr)
    {
        wav->finish();
        wav.reset();
    }

    const juce::ScopedLock sl (frameLock);
    latestFrame = juce::Image();
    newFrame = false;
}

bool AvRecorder::encodeFrame (const juce::Image& frame)
{
    // Размер видео в AVI фиксирован; кадр другого размера пропускаем (останется предыдущий)
    if (frame.getWidth() != width || frame.getHeight() != height)
        return false;

    juce::MemoryOutputStream out;

    if (! jpeg.writeImageToStream (frame, out))
        return false;

    const auto* data = static_cast<const uint8_t*> (out.getData());
    lastJpeg.assign (data, data + out.getDataSize());
    return ! lastJpeg.empty();
}

bool AvRecorder::writeVideoFrame()
{
    if (avi->getBytesWritten() > kMaxPartBytes)
    {
        avi->finish();
        avi.reset();

        if (! openPart (part + 1))
        {
            failed.store (true);
            return false;
        }

        ++part;
    }

    if (! avi->writeVideo (lastJpeg.data(), lastJpeg.size()))
    {
        failed.store (true);
        return false;
    }

    return true;
}

void AvRecorder::pumpAudio (bool flushAll)
{
    const int minChunk = juce::jmax (1, sampleRate / 50); // не меньше 20 мс, чтобы не плодить мелкие куски

    for (;;)
    {
        const int available = mic.getNumAvailable();

        if (available <= 0 || (! flushAll && available < minChunk))
            break;

        const int n = mic.read (floatBuffer.data(), juce::jmin (available, (int) floatBuffer.size()));

        if (n <= 0)
            break;

        for (int i = 0; i < n; ++i)
            pcmBuffer[(size_t) i] = (int16_t) juce::jlimit (-32768, 32767,
                                                             juce::roundToInt (floatBuffer[(size_t) i] * 32767.0f));

        const bool ok = (mode == Mode::aviWithAudio)
                          ? (avi != nullptr && avi->writeAudio (pcmBuffer.data(), (size_t) n))
                          : (wav != nullptr && wav->write (pcmBuffer.data(), (size_t) n));

        if (! ok)
        {
            failed.store (true);
            break;
        }
    }
}

void AvRecorder::run()
{
    const bool withVideo = (mode == Mode::aviWithAudio);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    int64_t framesWritten = 0;

    // Начало звука выравниваем с началом видео
    if (withVideo)
    {
        mic.discardAll();
    }
    else
    {
        // WAV: микрофон уже работает, но в файл звук пойдёт только после beginAudio(),
        // а всё накопленное до этого момента выбрасывается
        while (! startRequested.load() && ! threadShouldExit())
        {
            mic.discardAll();
            wait (2);
        }
    }

    while (! threadShouldExit() && ! failed.load())
    {
        pumpAudio (false);

        if (withVideo)
        {
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            const int64_t shouldHave = (int64_t) (elapsed * (double) kFps) + 1;

            if (shouldHave > framesWritten)
            {
                juce::Image frame;

                {
                    const juce::ScopedLock sl (frameLock);

                    if (newFrame)
                    {
                        frame = latestFrame;
                        newFrame = false;
                    }
                }

                if (frame.isValid())
                    encodeFrame (frame);

                // Частота кадров в файле постоянная: если камера отдаёт меньше или кодирование
                // не успело, предыдущий кадр повторяется, и видео не расходится со звуком
                while (framesWritten < shouldHave && ! lastJpeg.empty() && ! failed.load())
                {
                    if (! writeVideoFrame())
                        break;

                    ++framesWritten;
                }
            }
        }

        wait (4);
    }

    pumpAudio (true);
}
