#pragma once

// Выравнивание звука относительно видео (без зависимостей от JUCE, чтобы можно было тестировать отдельно).
//
// Оффсет (мс): положительный = звук ПОЗЖЕ видео (в начало добавляется тишина),
//              отрицательный = звук РАНЬШЕ видео (начало звука отбрасывается).
// padToClock: если хост (DAW) перестал присылать звук (остановлен), в файл добавляется тишина,
//             чтобы звук не разошёлся с видео, которое идёт по часам.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace media
{

class AudioAligner
{
public:
    // Допуск: пока звук отстаёт от часов меньше чем на это время, тишина не добавляется
    // (хосты присылают звук блоками, иногда довольно большими)
    static constexpr double kPadThresholdSeconds = 0.25;

    AudioAligner (int sampleRateHz, int offsetMilliseconds, bool padToClock)
        : rate (sampleRateHz), offsetMs (offsetMilliseconds), pad (padToClock)
    {
        if (offsetMs < 0)
            remainingToDrop = (int64_t) std::llround ((double) rate * (double) -offsetMs / 1000.0);
    }

    // Тишина, которую нужно записать в самом начале (оффсет больше нуля)
    int64_t getInitialSilenceFrames() const
    {
        return offsetMs > 0 ? (int64_t) std::llround ((double) rate * (double) offsetMs / 1000.0) : 0;
    }

    // Для каждого прочитанного куска: сколько кадров с его начала нужно выбросить (оффсет меньше нуля)
    int64_t takeFramesToDrop (int64_t framesJustRead)
    {
        const int64_t n = std::min (remainingToDrop, framesJustRead);
        remainingToDrop -= n;
        return n;
    }

    // Сколько кадров тишины добавить прямо сейчас, чтобы догнать часы (0, если не нужно)
    int64_t getSilenceToInsert (double elapsedSeconds, int64_t framesWrittenSoFar) const
    {
        if (! pad)
            return 0;

        const double expected = (elapsedSeconds + (double) offsetMs / 1000.0) * (double) rate;
        const double deficit = expected - (double) framesWrittenSoFar;

        return deficit > kPadThresholdSeconds * (double) rate ? (int64_t) deficit : 0;
    }

private:
    int rate;
    int offsetMs;
    bool pad;
    int64_t remainingToDrop = 0;
};

} // namespace media
