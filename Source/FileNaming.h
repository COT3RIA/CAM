#pragma once

// Имена файлов записи: проект, BPM, дата (без зависимостей от JUCE, чтобы можно было тестировать отдельно).
// Все строки в UTF-8.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace naming
{

// Безопасное имя для файла: убирает запрещённые символы, пробелы превращает в "_",
// обрезает до maxBytes, не разрезая многобайтовые символы UTF-8 (кириллица)
inline std::string sanitizeFileName (const std::string& input, size_t maxBytes = 48)
{
    std::string out;
    bool lastWasSpace = false;

    for (unsigned char c : input)
    {
        if (c < 0x20 || c == 0x7f || c == '\\' || c == '/' || c == ':' || c == '*'
            || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            continue;

        if (c == ' ')
        {
            if (! lastWasSpace)
                out.push_back ('_');

            lastWasSpace = true;
            continue;
        }

        lastWasSpace = false;
        out.push_back ((char) c);
    }

    if (out.size() > maxBytes)
    {
        size_t cut = maxBytes;

        // 10xxxxxx = продолжение многобайтового символа: отступаем к его началу
        while (cut > 0 && (((unsigned char) out[cut]) & 0xC0) == 0x80)
            --cut;

        out.resize (cut);
    }

    // Точки, пробелы и подчёркивания по краям Windows не любит или они бесполезны
    while (! out.empty() && (out.back() == '.' || out.back() == '_'))
        out.pop_back();

    size_t start = 0;

    while (start < out.size() && (out[start] == '.' || out[start] == '_'))
        ++start;

    return out.substr (start);
}

// "140" или "128.5"; пустая строка, если BPM неизвестен
inline std::string formatBpm (double bpm)
{
    if (! std::isfinite (bpm) || bpm < 20.0 || bpm > 999.0)
        return {};

    char buffer[32];

    if (std::fabs (bpm - std::round (bpm)) < 0.05)
        std::snprintf (buffer, sizeof buffer, "%d", (int) std::lround (bpm));
    else
        std::snprintf (buffer, sizeof buffer, "%.1f", bpm);

    return buffer;
}

// <проект>_<BPM>bpm_<дата>, например "My_Beat_140bpm_2026-11-27_09-15-30"
inline std::string buildBaseName (const std::string& project, double bpm, const std::string& dateTime)
{
    std::string name = sanitizeFileName (project);

    if (name.empty())
        name = "CAM";

    const auto bpmText = formatBpm (bpm);

    if (! bpmText.empty())
        name += "_" + bpmText + "bpm";

    if (! dateTime.empty())
        name += "_" + dateTime;

    return name;
}

//==============================================================================
// Имя проекта из заголовка главного окна DAW. Берётся только заголовок, в котором есть название
// известной DAW (иначе это не главное окно), а из него первая часть, не являющаяся названием DAW.
// Если DAW не пишет проект в заголовок, возвращается пустая строка (тогда используется поле в плагине).
namespace detail
{
    inline std::string lowerAscii (std::string s)
    {
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z')
                c = (char) (c - 'A' + 'a');

        return s;
    }

    inline bool containsDawName (const std::string& text)
    {
        static const char* const names[] =
        {
            "fl studio", "ableton", "reaper", "cubase", "nuendo", "studio one", "bitwig", "logic pro",
            "pro tools", "reason", "cakewalk", "ardour", "waveform", "samplitude", "digital performer",
            "tracktion", "mixcraft", "acid pro", "sonar", "garageband", "maschine"
        };

        const auto lower = lowerAscii (text);

        for (const char* n : names)
            if (lower.find (n) != std::string::npos)
                return true;

        return false;
    }

    inline std::string trim (const std::string& s)
    {
        size_t a = 0, b = s.size();

        while (a < b && (s[a] == ' ' || s[a] == '\t'))
            ++a;

        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
            --b;

        return s.substr (a, b - a);
    }

    inline bool endsWith (const std::string& s, const std::string& suffix)
    {
        return s.size() >= suffix.size() && s.compare (s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    inline std::vector<std::string> splitTitle (const std::string& title)
    {
        static const char* const separators[] = { " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", " | " };

        std::vector<std::string> parts;
        size_t pos = 0;

        while (pos <= title.size())
        {
            size_t best = std::string::npos;
            size_t bestLen = 0;

            for (const char* sep : separators)
            {
                const size_t f = title.find (sep, pos);

                if (f != std::string::npos && (best == std::string::npos || f < best))
                {
                    best = f;
                    bestLen = std::char_traits<char>::length (sep);
                }
            }

            if (best == std::string::npos)
            {
                parts.push_back (title.substr (pos));
                break;
            }

            parts.push_back (title.substr (pos, best - pos));
            pos = best + bestLen;
        }

        return parts;
    }

    // Убирает [скобки], маркер несохранённых изменений и расширение файла проекта
    inline std::string cleanSegment (std::string s)
    {
        s = trim (s);

        if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
            s = trim (s.substr (1, s.size() - 2));

        while (! s.empty() && (s.back() == '*' || s.back() == ' '))
            s.pop_back();

        static const char* const extensions[] =
        {
            ".flp", ".als", ".rpp", ".rpp-bak", ".cpr", ".song", ".bwproject", ".logicx", ".ptx",
            ".npr", ".rns", ".cwp", ".mmp", ".reason"
        };

        const auto lower = lowerAscii (s);

        for (const char* ext : extensions)
        {
            if (endsWith (lower, ext))
            {
                s.resize (s.size() - std::char_traits<char>::length (ext));
                break;
            }
        }

        return trim (s);
    }
}

inline std::string extractProjectName (const std::string& windowTitle)
{
    if (! detail::containsDawName (windowTitle))
        return {};

    for (const auto& segment : detail::splitTitle (windowTitle))
    {
        if (detail::containsDawName (segment))
            continue;

        const auto cleaned = detail::cleanSegment (segment);

        if (! cleaned.empty())
            return cleaned;
    }

    return {};
}

} // namespace naming
