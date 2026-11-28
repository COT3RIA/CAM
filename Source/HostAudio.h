#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <vector>

//==============================================================================
// Захват звука, который DAW подаёт на вход плагина (если плагин стоит на мастер-канале,
// это вся мастер-шина проекта). Всегда стерео. Без блокировок и выделения памяти в аудиопотоке:
// аудиопоток пишет (push), поток записи читает (read).
class HostAudioCapture
{
public:
    static constexpr int kCapacityFrames = 1 << 18; // около 5 секунд при 48 кГц

    HostAudioCapture() : fifo (kCapacityFrames)
    {
        left.assign ((size_t) kCapacityFrames, 0.0f);
        right.assign ((size_t) kCapacityFrames, 0.0f);
    }

    // Вызывается из prepareToPlay
    void setSampleRate (double rate) noexcept { sampleRate.store (rate); }
    double getSampleRate() const noexcept     { return sampleRate.load(); }

    // Включается только на время записи
    void setActive (bool shouldCapture) noexcept
    {
        if (shouldCapture)
            blocksPushed.store (0);

        active.store (shouldCapture);
    }

    // Сколько блоков звука пришло от DAW с начала записи (0 = DAW звук не присылает)
    int64_t getBlocksPushed() const noexcept { return blocksPushed.load(); }

    // Аудиопоток
    void push (const juce::AudioBuffer<float>& buffer) noexcept
    {
        if (! active.load (std::memory_order_relaxed))
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        if (numSamples <= 0 || numChannels <= 0)
            return;

        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (numChannels > 1 ? 1 : 0);

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (numSamples, start1, size1, start2, size2); // при переполнении лишнее отбрасывается

        if (size1 > 0)
        {
            juce::FloatVectorOperations::copy (left.data() + start1, l, size1);
            juce::FloatVectorOperations::copy (right.data() + start1, r, size1);
        }

        if (size2 > 0)
        {
            juce::FloatVectorOperations::copy (left.data() + start2, l + size1, size2);
            juce::FloatVectorOperations::copy (right.data() + start2, r + size1, size2);
        }

        fifo.finishedWrite (size1 + size2);
        blocksPushed.fetch_add (1);
    }

    // Поток записи
    int getNumAvailable() const noexcept { return fifo.getNumReady(); }

    // Забирает до maxFrames кадров в формате LRLR..., возвращает число кадров
    int read (float* interleaved, int maxFrames)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (maxFrames, start1, size1, start2, size2);

        auto copyRange = [&] (int start, int size, int outOffset)
        {
            for (int i = 0; i < size; ++i)
            {
                interleaved[2 * (outOffset + i)]     = left[(size_t) (start + i)];
                interleaved[2 * (outOffset + i) + 1] = right[(size_t) (start + i)];
            }
        };

        copyRange (start1, size1, 0);
        copyRange (start2, size2, size1);

        fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

    // Читатель просто выбрасывает накопленное (reset() из чужого потока небезопасен)
    void discardAll()
    {
        const int available = fifo.getNumReady();

        if (available > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead (available, start1, size1, start2, size2);
            fifo.finishedRead (size1 + size2);
        }
    }

private:
    juce::AbstractFifo fifo;
    std::vector<float> left, right;
    std::atomic<double> sampleRate { 0.0 };
    std::atomic<bool> active { false };
    std::atomic<int64_t> blocksPushed { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostAudioCapture)
};
