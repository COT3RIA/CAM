#pragma once

// Запись медиафайлов без зависимостей от JUCE (только стандартная библиотека):
//  - AviWriter: AVI (MJPEG-видео + PCM 16 бит), заголовки и индекс пишутся по классической схеме AVI 1.0
//  - WavWriter: WAV (PCM 16 бит)
// Работа с файлом идёт через интерфейс Sink, чтобы в плагине можно было использовать juce::FileOutputStream
// (это важно для путей с кириллицей на Windows).

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace media
{

class Sink
{
public:
    virtual ~Sink() = default;
    virtual bool write (const void* data, size_t size) = 0;
    virtual bool seek (int64_t position) = 0;
    virtual int64_t tell() = 0;
};

namespace detail
{
    inline void put32 (std::vector<uint8_t>& v, uint32_t x)
    {
        v.push_back ((uint8_t) (x & 0xff));
        v.push_back ((uint8_t) ((x >> 8) & 0xff));
        v.push_back ((uint8_t) ((x >> 16) & 0xff));
        v.push_back ((uint8_t) ((x >> 24) & 0xff));
    }

    inline void put16 (std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back ((uint8_t) (x & 0xff));
        v.push_back ((uint8_t) ((x >> 8) & 0xff));
    }

    inline void putTag (std::vector<uint8_t>& v, const char* tag)
    {
        for (int i = 0; i < 4; ++i)
            v.push_back ((uint8_t) tag[i]);
    }

    inline void patch32 (std::vector<uint8_t>& v, size_t pos, uint32_t x)
    {
        v[pos]     = (uint8_t) (x & 0xff);
        v[pos + 1] = (uint8_t) ((x >> 8) & 0xff);
        v[pos + 2] = (uint8_t) ((x >> 16) & 0xff);
        v[pos + 3] = (uint8_t) ((x >> 24) & 0xff);
    }
}

//==============================================================================
class AviWriter
{
public:
    explicit AviWriter (std::unique_ptr<Sink> sinkToUse) : sink (std::move (sinkToUse)) {}

    // Один видеопоток (MJPEG, постоянная частота кадров) и один аудиопоток (PCM 16 бит).
    bool begin (int widthPx, int heightPx, int framesPerSecond, int audioSampleRate, int audioChannels)
    {
        if (sink == nullptr || widthPx <= 0 || heightPx <= 0 || framesPerSecond <= 0
            || audioSampleRate <= 0 || audioChannels <= 0)
            return false;

        channels = audioChannels;
        blockAlign = (uint32_t) (audioChannels * 2);

        using namespace detail;
        std::vector<uint8_t> h;
        h.reserve (1024);

        // RIFF 'AVI '
        putTag (h, "RIFF");
        put32 (h, 0); // размер, допишем в finish()
        putTag (h, "AVI ");

        // LIST 'hdrl'
        putTag (h, "LIST");
        const size_t hdrlSizePos = h.size();
        put32 (h, 0);
        putTag (h, "hdrl");

        // avih (56 байт)
        putTag (h, "avih");
        put32 (h, 56);
        put32 (h, (uint32_t) (1000000 / framesPerSecond)); // dwMicroSecPerFrame
        put32 (h, 0);                                      // dwMaxBytesPerSec
        put32 (h, 0);                                      // dwPaddingGranularity
        put32 (h, 0x10 | 0x100);                           // AVIF_HASINDEX | AVIF_ISINTERLEAVED
        avihTotalFramesPos = h.size();
        put32 (h, 0);                                      // dwTotalFrames
        put32 (h, 0);                                      // dwInitialFrames
        put32 (h, 2);                                      // dwStreams
        put32 (h, 1 << 20);                                // dwSuggestedBufferSize
        put32 (h, (uint32_t) widthPx);
        put32 (h, (uint32_t) heightPx);
        put32 (h, 0); put32 (h, 0); put32 (h, 0); put32 (h, 0); // reserved

        // --- LIST 'strl' (видео)
        putTag (h, "LIST");
        const size_t strlVSizePos = h.size();
        put32 (h, 0);
        putTag (h, "strl");

        putTag (h, "strh");
        put32 (h, 56);
        putTag (h, "vids");
        putTag (h, "MJPG");
        put32 (h, 0);                          // flags
        put16 (h, 0); put16 (h, 0);            // priority, language
        put32 (h, 0);                          // initial frames
        put32 (h, 1);                          // scale
        put32 (h, (uint32_t) framesPerSecond); // rate
        put32 (h, 0);                          // start
        videoLengthPos = h.size();
        put32 (h, 0);                          // length (число кадров)
        put32 (h, 1 << 20);                    // suggested buffer
        put32 (h, 0xFFFFFFFFu);                // quality
        put32 (h, 0);                          // sample size
        put16 (h, 0); put16 (h, 0); put16 (h, (uint16_t) widthPx); put16 (h, (uint16_t) heightPx); // rcFrame

        putTag (h, "strf");
        put32 (h, 40);
        put32 (h, 40);                          // biSize
        put32 (h, (uint32_t) widthPx);
        put32 (h, (uint32_t) heightPx);
        put16 (h, 1);                           // planes
        put16 (h, 24);                          // bit count
        putTag (h, "MJPG");                     // compression
        put32 (h, (uint32_t) (widthPx * heightPx * 3));
        put32 (h, 0); put32 (h, 0); put32 (h, 0); put32 (h, 0);
        patch32 (h, strlVSizePos, (uint32_t) (h.size() - strlVSizePos - 4));

        // --- LIST 'strl' (аудио)
        putTag (h, "LIST");
        const size_t strlASizePos = h.size();
        put32 (h, 0);
        putTag (h, "strl");

        putTag (h, "strh");
        put32 (h, 56);
        putTag (h, "auds");
        put32 (h, 0);                          // handler
        put32 (h, 0);                          // flags
        put16 (h, 0); put16 (h, 0);
        put32 (h, 0);                          // initial frames
        put32 (h, blockAlign);                 // scale
        put32 (h, blockAlign * (uint32_t) audioSampleRate); // rate = байт в секунду
        put32 (h, 0);                          // start
        audioLengthPos = h.size();
        put32 (h, 0);                          // length (число отсчётов)
        put32 (h, 1 << 16);
        put32 (h, 0xFFFFFFFFu);
        put32 (h, blockAlign);                 // sample size
        put16 (h, 0); put16 (h, 0); put16 (h, 0); put16 (h, 0);

        putTag (h, "strf");
        put32 (h, 16);
        put16 (h, 1);                          // WAVE_FORMAT_PCM
        put16 (h, (uint16_t) audioChannels);
        put32 (h, (uint32_t) audioSampleRate);
        put32 (h, blockAlign * (uint32_t) audioSampleRate); // avg bytes per sec
        put16 (h, (uint16_t) blockAlign);
        put16 (h, 16);                         // bits per sample
        patch32 (h, strlASizePos, (uint32_t) (h.size() - strlASizePos - 4));

        patch32 (h, hdrlSizePos, (uint32_t) (h.size() - hdrlSizePos - 4));

        // LIST 'movi'
        putTag (h, "LIST");
        moviSizePos = h.size();
        put32 (h, 0);
        moviTagPos = h.size();
        putTag (h, "movi");

        if (! sink->write (h.data(), h.size()))
            return false;

        began = true;
        return true;
    }

    bool writeVideo (const uint8_t* jpeg, size_t size)
    {
        if (! writeChunk ("00dc", jpeg, size))
            return false;

        ++videoFrames;
        return true;
    }

    // samples: чередующиеся отсчёты int16, numFrames = число отсчётов на канал
    bool writeAudio (const int16_t* samples, size_t numFrames)
    {
        if (numFrames == 0)
            return true;

        std::vector<uint8_t> bytes;
        bytes.reserve (numFrames * blockAlign);

        for (size_t i = 0; i < numFrames * (size_t) channels; ++i)
            detail::put16 (bytes, (uint16_t) samples[i]);

        if (! writeChunk ("01wb", bytes.data(), bytes.size()))
            return false;

        audioSamples += numFrames;
        return true;
    }

    // Дописывает индекс и исправляет размеры в заголовках
    bool finish()
    {
        if (! began)
            return false;

        began = false;
        using namespace detail;

        const int64_t idxPos = sink->tell();

        std::vector<uint8_t> idx;
        idx.reserve (index.size() * 16 + 8);
        putTag (idx, "idx1");
        put32 (idx, (uint32_t) (index.size() * 16));

        for (const auto& e : index)
        {
            idx.push_back ((uint8_t) e.id[0]); idx.push_back ((uint8_t) e.id[1]);
            idx.push_back ((uint8_t) e.id[2]); idx.push_back ((uint8_t) e.id[3]);
            put32 (idx, 0x10); // AVIIF_KEYFRAME (и для звука)
            put32 (idx, e.offset);
            put32 (idx, e.size);
        }

        bool ok = sink->write (idx.data(), idx.size());
        const int64_t endPos = sink->tell();

        auto patch = [&] (size_t pos, uint32_t value)
        {
            std::vector<uint8_t> b;
            put32 (b, value);
            return sink->seek ((int64_t) pos) && sink->write (b.data(), 4);
        };

        ok = ok && patch (4, (uint32_t) (endPos - 8));
        ok = ok && patch (moviSizePos, (uint32_t) (idxPos - (int64_t) (moviSizePos + 4)));
        ok = ok && patch (avihTotalFramesPos, (uint32_t) videoFrames);
        ok = ok && patch (videoLengthPos, (uint32_t) videoFrames);
        ok = ok && patch (audioLengthPos, (uint32_t) audioSamples);
        ok = ok && sink->seek (endPos);

        return ok;
    }

    int64_t getBytesWritten() const { return bytesWritten; }
    int64_t getVideoFrames() const  { return videoFrames; }
    int64_t getAudioSamples() const { return audioSamples; }

private:
    struct IndexEntry
    {
        char id[4];
        uint32_t offset;
        uint32_t size;
    };

    bool writeChunk (const char* id, const uint8_t* data, size_t size)
    {
        if (! began)
            return false;

        const int64_t pos = sink->tell();

        // Классический AVI ограничен 4 ГБ (плеер не любит и больше 2 ГБ), поэтому длинные записи делятся на части снаружи
        if (pos + (int64_t) size + 64 > 0xFFFFFFF0LL)
            return false;

        std::vector<uint8_t> head;
        detail::putTag (head, id);
        detail::put32 (head, (uint32_t) size);

        if (! sink->write (head.data(), 8))
            return false;

        if (size > 0 && ! sink->write (data, size))
            return false;

        if (size & 1)
        {
            const uint8_t pad = 0;

            if (! sink->write (&pad, 1))
                return false;
        }

        IndexEntry e;
        std::memcpy (e.id, id, 4);
        e.offset = (uint32_t) (pos - (int64_t) moviTagPos); // относительно метки 'movi'
        e.size = (uint32_t) size;
        index.push_back (e);

        bytesWritten = sink->tell();
        return true;
    }

    std::unique_ptr<Sink> sink;
    bool began = false;
    int channels = 1;
    uint32_t blockAlign = 2;

    size_t avihTotalFramesPos = 0, videoLengthPos = 0, audioLengthPos = 0, moviSizePos = 0, moviTagPos = 0;

    std::vector<IndexEntry> index;
    int64_t videoFrames = 0, audioSamples = 0, bytesWritten = 0;
};

//==============================================================================
class WavWriter
{
public:
    explicit WavWriter (std::unique_ptr<Sink> sinkToUse) : sink (std::move (sinkToUse)) {}

    bool begin (int sampleRate, int numChannels)
    {
        if (sink == nullptr || sampleRate <= 0 || numChannels <= 0)
            return false;

        channels = numChannels;
        rate = sampleRate;

        using namespace detail;
        std::vector<uint8_t> h;
        putTag (h, "RIFF");
        put32 (h, 0);
        putTag (h, "WAVE");
        putTag (h, "fmt ");
        put32 (h, 16);
        put16 (h, 1); // PCM
        put16 (h, (uint16_t) numChannels);
        put32 (h, (uint32_t) sampleRate);
        put32 (h, (uint32_t) (sampleRate * numChannels * 2));
        put16 (h, (uint16_t) (numChannels * 2));
        put16 (h, 16);
        putTag (h, "data");
        put32 (h, 0);

        if (! sink->write (h.data(), h.size()))
            return false;

        began = true;
        return true;
    }

    bool write (const int16_t* samples, size_t numFrames)
    {
        if (! began)
            return false;

        std::vector<uint8_t> bytes;
        bytes.reserve (numFrames * (size_t) channels * 2);

        for (size_t i = 0; i < numFrames * (size_t) channels; ++i)
            detail::put16 (bytes, (uint16_t) samples[i]);

        if (dataBytes + bytes.size() > 0xFFFFFF00ull)
            return false;

        if (! bytes.empty() && ! sink->write (bytes.data(), bytes.size()))
            return false;

        dataBytes += bytes.size();
        return true;
    }

    bool finish()
    {
        if (! began)
            return false;

        began = false;

        auto patch = [&] (int64_t pos, uint32_t value)
        {
            std::vector<uint8_t> b;
            detail::put32 (b, value);
            return sink->seek (pos) && sink->write (b.data(), 4);
        };

        bool ok = patch (4, (uint32_t) (36 + dataBytes));
        ok = ok && patch (40, (uint32_t) dataBytes);
        ok = ok && sink->seek (44 + (int64_t) dataBytes);
        return ok;
    }

    uint64_t getDataBytes() const { return dataBytes; }

private:
    std::unique_ptr<Sink> sink;
    bool began = false;
    int channels = 1, rate = 48000;
    uint64_t dataBytes = 0;
};

} // namespace media
