#pragma once

#include <juce_core/juce_core.h>

// Все надписи плагина на двух языках: 0 = English, 1 = Русский.
// Файл должен быть в кодировке UTF-8 (в CMake для MSVC включён флаг /utf-8).
namespace loc
{
    // X(идентификатор, "English", "Русский"); "%s" заменяется на аргумент
    #define LOC_TEXTS(X) \
        X (none,               "", "") \
        X (lens13,             "13mm (full frame)", "13 мм (весь кадр)") \
        X (lens24,             "24mm", "24 мм") \
        X (lens48,             "48mm", "48 мм") \
        X (lens77,             "77mm", "77 мм") \
        X (lensFisheye,        "Fisheye", "Рыбий глаз") \
        X (mirror,             "Mirror", "Зеркало") \
        X (record,             "Record", "Запись") \
        X (stop,               "Stop", "Стоп") \
        X (openFolder,         "Open folder", "Папка") \
        X (audioOff,           "Audio: off", "Звук: выкл") \
        X (audioMic,           "Audio: microphone", "Звук: микрофон") \
        X (audioHost,          "Audio: DAW master", "Звук: мастер DAW") \
        X (syncTransport,      "Sync with DAW", "С транспортом DAW") \
        X (projectHint,        "Project name (auto)", "Проект (авто)") \
        X (offsetLabel,        "Audio delay:", "Сдвиг звука:") \
        X (msUnit,             "ms", "мс") \
        X (syncWaiting,        "Waiting for Play in the DAW...", "Жду Play в DAW...") \
        X (hostNotReady,       "DAW audio is not available yet. Press Play once and try again.", \
                               "Звук DAW пока недоступен. Один раз нажмите Play и повторите.") \
        X (hostNoAudio,        "No audio from the DAW. Put CAM on the Master channel and start playback.", \
                               "Звук от DAW не поступает. Поставьте CAM на мастер-канал и запустите воспроизведение.") \
        X (defaultMic,         "Default microphone", "Микрофон по умолчанию") \
        X (modeTogether,       "With video (AVI)", "Вместе с видео (AVI)") \
        X (modeSeparate,       "Separate file (WAV)", "Отдельным файлом (WAV)") \
        X (languageButton,     "Language: EN", "Язык: RU") \
        X (noCameras,          "No cameras found", "Камеры не найдены") \
        X (opening,            "Opening camera...", "Открываю камеру...") \
        X (waiting,            "Camera opened, waiting for video...", "Камера открыта, жду видео...") \
        X (waitingPlaceholder, "Waiting for video...", "Ожидание видео...") \
        X (live,               "Live: %s", "Видео: %s") \
        X (cameraError,        "Camera error: %s", "Ошибка камеры: %s") \
        X (cameraErrorGeneric, "Camera error: could not open the camera", "Ошибка камеры: не удалось открыть камеру") \
        X (noVideo,            "No video received. Check camera permission (host / OS) and that no other app is using the camera.", \
                               "Видео не поступает. Проверьте разрешение на камеру (для DAW и системы) и что камеру не использует другое приложение.") \
        X (cantRecordNoCamera, "Can't record: camera is not open", "Запись невозможна: камера не открыта") \
        X (cantRecordNoVideo,  "Can't record yet: no video from the camera", "Запись пока невозможна: нет видео с камеры") \
        X (recording,          "Recording to: %s", "Идёт запись в: %s") \
        X (saved,              "Saved: %s", "Сохранено: %s") \
        X (micError,           "Microphone error: %s", "Ошибка микрофона: %s") \
        X (writeError,         "Recording stopped: could not write to the file (disk full?)", \
                               "Запись остановлена: не удалось записать в файл (нет места на диске?)")

    enum class Text
    {
       #define LOC_ENUM(id, en, ru) id,
        LOC_TEXTS (LOC_ENUM)
       #undef LOC_ENUM
        count
    };

    inline juce::String get (int language, Text text)
    {
        struct Entry { const char* en; const char* ru; };

        static const Entry table[] =
        {
           #define LOC_ENTRY(id, en, ru) { en, ru },
            LOC_TEXTS (LOC_ENTRY)
           #undef LOC_ENTRY
        };

        static_assert (sizeof (table) / sizeof (table[0]) == (size_t) Text::count, "table must match the enum");

        const auto& e = table[(size_t) text];
        return juce::String::fromUTF8 (language == 1 ? e.ru : e.en);
    }

    inline juce::String format (int language, Text text, const juce::String& arg = {})
    {
        return get (language, text).replace ("%s", arg);
    }
}
