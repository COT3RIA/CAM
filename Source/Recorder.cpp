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

    float blockPeak = 0.0f;

    for (int i = 0; i < size1; ++i)
    {
        const float v = mixToMono (i);
        buffer[(size_t) (start1 + i)] = v;
        blockPeak = juce::jmax (blockPeak, std::abs (v));
    }

    for (int i = 0; i < size2; ++i)
    {
        const float v = mixToMono (size1 + i);
        buffer[(size_t) (start2 + i)] = v;
        blockPeak = juce::jmax (blockPeak, std::abs (v));
    }

    if (blockPeak > peak.load (std::memory_order_relaxed))
        peak.store (blockPeak, std::memory_order_relaxed);

    fifo.finishedWrite (size1 + size2);
}

//==============================================================================
AvRecorder::AvRecorder() : juce::Thread ("CAM AV recorder")
{
    jpeg.setQuality (0.8f);
    floatBuffer.resize (16384 * 2);
    pcmBuffer.resize (16384 * 2);
}

AvRecorder::~AvRecorder()
{
    stop();
}

int AvRecorder::audioAvailable() const
{
    if (activeHost != nullptr)
        return activeHost->getNumAvailable();

    return mic != nullptr ? mic->getNumAvailable() : 0;
}

int AvRecorder::audioRead (float* dest, int maxFrames)
{
    if (activeHost != nullptr)
        return activeHost->read (dest, maxFrames);

    return mic != nullptr ? mic->read (dest, maxFrames) : 0;
}

void AvRecorder::audioDiscard()
{
    if (activeHost != nullptr)
        activeHost->discardAll();
    else if (mic != nullptr)
        mic->discardAll();
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
        return avi->begin (width, height, kFps, sampleRate, channels);
    }

    wav = std::make_unique<media::WavWriter> (std::move (sink));
    return wav->begin (sampleRate, channels);
}

juce::String AvRecorder::start (Mode modeToUse, const juce::File& file, int videoWidth, int videoHeight,
                                const AudioSettings& audio)
{
    stop();

    mode = modeToUse;
    baseFile = file;
    width = videoWidth;
    height = videoHeight;
    part = 1;
    settings = audio;
    audioFramesWritten = 0;
    failed.store (false);
    startRequested.store (mode == Mode::aviWithAudio); // для WAV ждём beginAudio()
    lastJpeg.clear();

    if (mode == Mode::aviWithAudio && (width < 2 || height < 2))
        return "no video frames yet";

    const bool fromHost = (settings.source == AudioSettings::Source::host);

    if (fromHost)
    {
        if (settings.host == nullptr)
            return "no DAW audio source";

        sampleRate = juce::roundToInt (settings.host->getSampleRate());

        if (sampleRate <= 0)
            return "DAW audio is not ready yet";

        channels = 2;
        activeHost = settings.host;
        activeHost->discardAll();
        activeHost->setActive (true);
    }
    else
    {
        mic = std::make_unique<MicCapture>();
        const auto error = mic->start (settings.micName);

        if (error.isNotEmpty())
        {
            mic.reset();
            return error;
        }

        sampleRate = juce::roundToInt (mic->getSampleRate());

        if (sampleRate <= 0)
        {
            mic.reset();
            return "the input device reported no sample rate";
        }

        channels = 1;
    }

    // Оффсет и подстройка под часы относятся только к звуку DAW: микрофон идёт непрерывно
    aligner = media::AudioAligner (sampleRate, fromHost ? settings.offsetMs : 0, fromHost);

    auto abortStart = [this]
    {
        if (activeHost != nullptr)
        {
            activeHost->setActive (false);
            activeHost = nullptr;
        }

        mic.reset();
        avi.reset();
        wav.reset();
    };

    if (! openPart (1))
    {
        abortStart();
        return "could not create the file: " + file.getFullPathName();
    }

    if (! startThread())
    {
        abortStart();
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
    // Источник звука останавливаем первым: поток записи ещё успеет дочитать накопленное
    if (mic != nullptr)
        mic->stop();

    if (activeHost != nullptr)
        activeHost->setActive (false);

    if (isThreadRunning())
    {
        signalThreadShouldExit();
        notify();
        waitForThreadToExit (5000);
    }

    finishFiles();
    mic.reset();
    activeHost = nullptr;
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

bool AvRecorder::writePcm (const int16_t* data, int numFrames)
{
    const bool ok = (mode == Mode::aviWithAudio)
                      ? (avi != nullptr && avi->writeAudio (data, (size_t) numFrames))
                      : (wav != nullptr && wav->write (data, (size_t) numFrames));

    if (! ok)
        failed.store (true);
    else
        audioFramesWritten += numFrames;

    return ok;
}

bool AvRecorder::writeSilence (int64_t numFrames)
{
    const int chunk = (int) (pcmBuffer.size() / (size_t) channels);
    std::fill (pcmBuffer.begin(), pcmBuffer.end(), (int16_t) 0);

    while (numFrames > 0)
    {
        const int n = (int) juce::jmin<int64_t> (numFrames, chunk);

        if (! writePcm (pcmBuffer.data(), n))
            return false;

        numFrames -= n;
    }

    return true;
}

void AvRecorder::pumpAudio (bool flushAll)
{
    const int minChunk = juce::jmax (1, sampleRate / 50); // не меньше 20 мс, чтобы не плодить мелкие куски
    const int maxFrames = (int) (floatBuffer.size() / (size_t) channels);

    for (;;)
    {
        const int available = audioAvailable();

        if (available <= 0 || (! flushAll && available < minChunk))
            break;

        const int n = audioRead (floatBuffer.data(), juce::jmin (available, maxFrames));

        if (n <= 0)
            break;

        // Отрицательный оффсет: начало звука выбрасывается
        const int drop = (int) aligner.takeFramesToDrop (n);
        const int keep = n - drop;

        if (keep <= 0)
            continue;

        const float* src = floatBuffer.data() + (size_t) drop * (size_t) channels;
        const int numSamples = keep * channels;

        for (int i = 0; i < numSamples; ++i)
            pcmBuffer[(size_t) i] = (int16_t) juce::jlimit (-32768, 32767, juce::roundToInt (src[i] * 32767.0f));

        if (! writePcm (pcmBuffer.data(), keep))
            break;
    }
}

void AvRecorder::run()
{
    const bool withVideo = (mode == Mode::aviWithAudio);

    // Начало звука выравниваем с началом видео
    if (withVideo)
    {
        audioDiscard();
    }
    else
    {
        // WAV: источник звука уже работает, но в файл звук пойдёт только после beginAudio(),
        // а всё накопленное до этого момента выбрасывается
        while (! startRequested.load() && ! threadShouldExit())
        {
            audioDiscard();
            wait (2);
        }
    }

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    int64_t framesWritten = 0;

    // Положительный оффсет: звук начинается позже видео, в начало добавляется тишина
    writeSilence (aligner.getInitialSilenceFrames());

    while (! threadShouldExit() && ! failed.load())
    {
        pumpAudio (false);

        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;

        // DAW остановлена и звук не присылает: добавляем тишину, чтобы звук не разошёлся с видео
        const auto silence = aligner.getSilenceToInsert (elapsed, audioFramesWritten);

        if (silence > 0)
            writeSilence (silence);

        if (withVideo)
        {
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
