#include "PluginEditor.h"
#include "FileNaming.h"
#include "HostProject.h"

namespace
{
    // Веб-камера не умеет менять оптику, поэтому "объективы" эмулируются программно:
    // зум = цифровой кроп центра кадра, Fisheye = бочкообразное искажение.
    struct Lens
    {
        loc::Text name;
        float zoom;
        bool fisheye;
    };

    const Lens kLenses[] =
    {
        { loc::Text::lens13,      1.0f, false },
        { loc::Text::lens24,      1.5f, false },
        { loc::Text::lens48,      2.5f, false },
        { loc::Text::lens77,      4.0f, false },
        { loc::Text::lensFisheye, 1.0f, true  },
    };

    constexpr int kNumLenses = (int) (sizeof (kLenses) / sizeof (kLenses[0]));

    // Размеры окна: 16:9 по умолчанию, соотношение потом подстраивается под камеру
    // Минимум 480x270: при такой ширине панель настроек переносит кнопки на несколько строк
    constexpr int kMinW = 480, kMinH = 270, kMaxW = 1920, kMaxH = 1080;
    constexpr int kStartW = 640, kStartH = 360;

    const juce::Colour kControlBg   (0x66000000);
    const juce::Colour kControlLine (0x40ffffff);
    const juce::Colour kRecordingBg (0xa0d02020);

    const Lens& getLens (int index)
    {
        return kLenses[juce::jlimit (0, kNumLenses - 1, index)];
    }

    juce::File getRecordingsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMoviesDirectory)
                   .getChildFile ("CAM");
    }

    // Программный fisheye с билинейной интерполяцией
    juce::Image makeFisheye (const juce::Image& source)
    {
        const int sw = source.getWidth();
        const int sh = source.getHeight();

        if (sw < 2 || sh < 2)
            return {};

        const float scale = juce::jmin (1.0f, 640.0f / (float) sw);
        const int ow = juce::jmax (2, (int) ((float) sw * scale));
        const int oh = juce::jmax (2, (int) ((float) sh * scale));

        juce::Image src = source.convertedToFormat (juce::Image::ARGB);
        juce::Image out (juce::Image::ARGB, ow, oh, false, juce::SoftwareImageType());

        juce::Image::BitmapData in (src, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst (out, juce::Image::BitmapData::writeOnly);

        constexpr float k = 0.9f; // сила искажения

        auto lerp = [] (float a, float b, float t) { return a + (b - a) * t; };

        for (int oy = 0; oy < oh; ++oy)
        {
            auto* row = reinterpret_cast<juce::PixelARGB*> (dst.getLinePointer (oy));
            const float v = (((float) oy + 0.5f) / (float) oh) * 2.0f - 1.0f;

            for (int ox = 0; ox < ow; ++ox)
            {
                const float u = (((float) ox + 0.5f) / (float) ow) * 2.0f - 1.0f;
                const float r2 = u * u + v * v;
                const float f = (1.0f + k * r2) / (1.0f + k);
                const float su = u * f;
                const float sv = v * f;

                if (su < -1.0f || su > 1.0f || sv < -1.0f || sv > 1.0f)
                {
                    row[ox].setARGB (255, 0, 0, 0);
                    continue;
                }

                const float fx = (su + 1.0f) * 0.5f * (float) (sw - 1);
                const float fy = (sv + 1.0f) * 0.5f * (float) (sh - 1);
                const int x0 = (int) fx;
                const int y0 = (int) fy;
                const int x1 = juce::jmin (x0 + 1, sw - 1);
                const int y1 = juce::jmin (y0 + 1, sh - 1);
                const float tx = fx - (float) x0;
                const float ty = fy - (float) y0;

                const auto* r0 = reinterpret_cast<const juce::PixelARGB*> (in.getLinePointer (y0));
                const auto* r1 = reinterpret_cast<const juce::PixelARGB*> (in.getLinePointer (y1));
                const auto& p00 = r0[x0];
                const auto& p10 = r0[x1];
                const auto& p01 = r1[x0];
                const auto& p11 = r1[x1];

                auto mix = [&] (juce::uint8 a, juce::uint8 b, juce::uint8 c, juce::uint8 d)
                {
                    return (juce::uint8) juce::jlimit (0, 255,
                        (int) lerp (lerp ((float) a, (float) b, tx),
                                    lerp ((float) c, (float) d, tx), ty));
                };

                row[ox].setARGB (255,
                                 mix (p00.getRed(),   p10.getRed(),   p01.getRed(),   p11.getRed()),
                                 mix (p00.getGreen(), p10.getGreen(), p01.getGreen(), p11.getGreen()),
                                 mix (p00.getBlue(),  p10.getBlue(),  p01.getBlue(),  p11.getBlue()));
            }
        }

        return out;
    }
}

//==============================================================================
WebcamAudioProcessorEditor::WebcamAudioProcessorEditor (WebcamAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    addAndMakeVisible (topBar);
    topBar.addAndMakeVisible (cameraBox);
    topBar.addAndMakeVisible (lensBox);
    topBar.addAndMakeVisible (mirrorButton);
    topBar.addAndMakeVisible (recordButton);
    topBar.addAndMakeVisible (folderButton);
    topBar.addAndMakeVisible (audioBox);
    topBar.addAndMakeVisible (micBox);
    topBar.addAndMakeVisible (modeBox);
    topBar.addAndMakeVisible (offsetLabel);
    topBar.addAndMakeVisible (offsetSlider);
    topBar.addAndMakeVisible (syncButton);
    topBar.addAndMakeVisible (projectEdit);
    topBar.addAndMakeVisible (langButton);

    offsetSlider.setSliderStyle (juce::Slider::IncDecButtons);
    offsetSlider.setRange (-2000.0, 2000.0, 5.0);
    offsetSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Horizontal);
    offsetSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, 28);
    offsetSlider.setValue (proc.audioOffsetMs, juce::dontSendNotification);

    projectEdit.setMultiLine (false);
    projectEdit.setReturnKeyStartsNewLine (false);
    projectEdit.setText (proc.projectName, false);
    projectEdit.setInputRestrictions (60);

    styleControls();

    cameraNames = juce::CameraDevice::getAvailableDevices();

    if (cameraNames.isEmpty())
    {
        setStatus (loc::Text::noCameras);
    }
    else
    {
        cameraBox.addItemList (cameraNames, 1);
        int idx = cameraNames.indexOf (proc.cameraName);
        if (idx < 0) idx = 0;
        cameraBox.setSelectedItemIndex (idx, juce::dontSendNotification);
    }

    micNames = MicCapture::getInputDeviceNames();

    mirrorButton.setToggleState (proc.mirror, juce::dontSendNotification);
    syncButton.setToggleState (proc.syncTransport, juce::dontSendNotification);
    prevHostPlaying = proc.hostPlaying.load();

    populateMicBox();
    applyLanguage();       // заполняет объективы и режимы и расставляет надписи
    updateControlsEnabled();

    cameraBox.onChange = [this]
    {
        proc.cameraName = cameraBox.getText();
        openCamera (cameraBox.getSelectedItemIndex());
    };

    lensBox.onChange = [this]
    {
        proc.lensIndex = lensBox.getSelectedItemIndex();
        refreshDisplay();
    };

    mirrorButton.onClick = [this]
    {
        proc.mirror = mirrorButton.getToggleState();
        repaint();
    };

    audioBox.onChange = [this]
    {
        proc.audioSource = juce::jlimit (0, 2, audioBox.getSelectedItemIndex());
        updateControlsEnabled();
    };

    offsetSlider.onValueChange = [this]
    {
        proc.audioOffsetMs = juce::roundToInt (offsetSlider.getValue());
    };

    projectEdit.onTextChange = [this]
    {
        proc.projectName = projectEdit.getText();
    };

    syncButton.onClick = [this]
    {
        proc.syncTransport = syncButton.getToggleState();
        prevHostPlaying = proc.hostPlaying.load(); // уже идущее воспроизведение запись не запускает

        if (proc.syncTransport && ! recording)
            setStatus (loc::Text::syncWaiting);
    };

    micBox.onChange = [this]
    {
        const int id = micBox.getSelectedId();
        proc.micName = (id >= 2 && id - 2 < micNames.size()) ? micNames[id - 2] : juce::String();
    };

    modeBox.onChange = [this]
    {
        proc.audioMode = juce::jlimit (0, 1, modeBox.getSelectedItemIndex());
    };

    langButton.onClick = [this]
    {
        proc.language = 1 - juce::jlimit (0, 1, proc.language);
        applyLanguage();
    };

    recordButton.onClick = [this] { toggleRecording(); };

    folderButton.onClick = []
    {
        auto folder = getRecordingsFolder();
        folder.createDirectory();
        folder.startAsProcess();
    };

    setResizeLimits (kMinW, kMinH, kMaxW, kMaxH);

    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kStartW / (double) kStartH);

    setSize (kStartW, kStartH);

    // При открытии окна панель видна 3 секунды, потом прячется, пока курсор не вернётся
    showUntilMs = juce::Time::getMillisecondCounter() + 3000;

    startTimerHz (30);

    if (cameraNames.size() > 0)
        openCamera (cameraBox.getSelectedItemIndex());
}

WebcamAudioProcessorEditor::~WebcamAudioProcessorEditor()
{
    stopTimer();
    closeCamera();
}

//==============================================================================
void WebcamAudioProcessorEditor::styleControls()
{
    for (auto* box : { &cameraBox, &lensBox, &audioBox, &micBox, &modeBox })
    {
        box->setColour (juce::ComboBox::backgroundColourId, kControlBg);
        box->setColour (juce::ComboBox::outlineColourId, kControlLine);
        box->setColour (juce::ComboBox::textColourId, juce::Colours::white);
        box->setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
    }

    for (auto* button : { &recordButton, &folderButton, &langButton })
    {
        button->setColour (juce::TextButton::buttonColourId, kControlBg);
        button->setColour (juce::TextButton::buttonOnColourId, kControlBg);
        button->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    for (auto* toggle : { &mirrorButton, &syncButton })
    {
        toggle->setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        toggle->setColour (juce::ToggleButton::tickColourId, juce::Colours::white);
        toggle->setColour (juce::ToggleButton::tickDisabledColourId, juce::Colours::grey);
    }

    offsetLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    offsetLabel.setJustificationType (juce::Justification::centredRight);
    offsetLabel.setFont (juce::FontOptions (13.0f));

    offsetSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    offsetSlider.setColour (juce::Slider::textBoxBackgroundColourId, kControlBg);
    offsetSlider.setColour (juce::Slider::textBoxOutlineColourId, kControlLine);
    offsetSlider.setColour (juce::Slider::textBoxHighlightColourId, juce::Colour (0xff3a6ea5));
    offsetSlider.setColour (juce::Slider::backgroundColourId, kControlBg);
    offsetSlider.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    offsetSlider.setColour (juce::Slider::trackColourId, juce::Colours::white);

    projectEdit.setColour (juce::TextEditor::backgroundColourId, kControlBg);
    projectEdit.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    projectEdit.setColour (juce::TextEditor::outlineColourId, kControlLine);
    projectEdit.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0x99ffffff));
    projectEdit.setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff3a6ea5));
    projectEdit.setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
}

void WebcamAudioProcessorEditor::populateMicBox()
{
    micBox.clear (juce::dontSendNotification);
    micBox.addItem (tr (loc::Text::defaultMic), 1);

    for (int i = 0; i < micNames.size(); ++i)
        micBox.addItem (micNames[i], i + 2);

    const int idx = micNames.indexOf (proc.micName);
    micBox.setSelectedId (proc.micName.isNotEmpty() && idx >= 0 ? idx + 2 : 1, juce::dontSendNotification);
}

// Расставляет все надписи на выбранном языке (вызывается при запуске и при переключении языка)
void WebcamAudioProcessorEditor::applyLanguage()
{
    mirrorButton.setButtonText (tr (loc::Text::mirror));
    syncButton.setButtonText (tr (loc::Text::syncTransport));
    offsetLabel.setText (tr (loc::Text::offsetLabel), juce::dontSendNotification);
    offsetSlider.setTextValueSuffix (" " + tr (loc::Text::msUnit));
    projectEdit.setTextToShowWhenEmpty (tr (loc::Text::projectHint), juce::Colour (0x99ffffff));

    audioBox.clear (juce::dontSendNotification);
    audioBox.addItem (tr (loc::Text::audioOff), 1);
    audioBox.addItem (tr (loc::Text::audioMic), 2);
    audioBox.addItem (tr (loc::Text::audioHost), 3);
    audioBox.setSelectedItemIndex (juce::jlimit (0, 2, proc.audioSource), juce::dontSendNotification);
    folderButton.setButtonText (tr (loc::Text::openFolder));
    langButton.setButtonText (tr (loc::Text::languageButton));
    recordButton.setButtonText (tr (recording ? loc::Text::stop : loc::Text::record));

    lensBox.clear (juce::dontSendNotification);

    for (int i = 0; i < kNumLenses; ++i)
        lensBox.addItem (tr (kLenses[i].name), i + 1);

    lensBox.setSelectedItemIndex (juce::jlimit (0, kNumLenses - 1, proc.lensIndex), juce::dontSendNotification);

    modeBox.clear (juce::dontSendNotification);
    modeBox.addItem (tr (loc::Text::modeTogether), 1);
    modeBox.addItem (tr (loc::Text::modeSeparate), 2);
    modeBox.setSelectedItemIndex (juce::jlimit (0, 1, proc.audioMode), juce::dontSendNotification);

    populateMicBox();

    cameraBox.setTextWhenNothingSelected (cameraNames.isEmpty() ? tr (loc::Text::noCameras) : juce::String());

    repaint();
}

void WebcamAudioProcessorEditor::updateControlsEnabled()
{
    const bool idle = ! recording;

    const int source = proc.audioSource;

    cameraBox.setEnabled (idle);
    audioBox.setEnabled (idle);
    micBox.setEnabled (idle);
    modeBox.setEnabled (idle);
    offsetSlider.setEnabled (idle);
    projectEdit.setEnabled (idle);

    // Лишние настройки прячутся: микрофон нужен только для микрофона, сдвиг только для звука DAW
    micBox.setVisible (source == 1);
    modeBox.setVisible (source != 0);
    offsetLabel.setVisible (source == 2);
    offsetSlider.setVisible (source == 2);

    if (getWidth() > 0)
        resized();
}

// Если сохранённый микрофон отключили, пишем с устройства по умолчанию
juce::String WebcamAudioProcessorEditor::getEffectiveMicName() const
{
    return micNames.contains (proc.micName) ? proc.micName : juce::String();
}

void WebcamAudioProcessorEditor::setStatus (loc::Text id, const juce::String& arg)
{
    statusId = id;
    statusArg = arg;
    repaint();
}

juce::String WebcamAudioProcessorEditor::getStatusText() const
{
    return loc::format (proc.language, statusId, statusArg);
}

//==============================================================================
// Панель и строка статуса видны, пока курсор над плагином (или открыт выпадающий список),
// и ещё полсекунды после этого. Потом плавно гаснут.
void WebcamAudioProcessorEditor::updateOverlay()
{
    const auto now = juce::Time::getMillisecondCounter();

    const bool hovered = isMouseOver (true)
                          || cameraBox.isPopupActive() || lensBox.isPopupActive()
                          || micBox.isPopupActive() || modeBox.isPopupActive() || audioBox.isPopupActive()
                          || projectEdit.hasKeyboardFocus (true);

    if (hovered)
        showUntilMs = now + 500;

    const bool wantVisible = hovered || (juce::int32) (showUntilMs - now) > 0;

    const float target = wantVisible ? 1.0f : 0.0f;
    const float step   = wantVisible ? 0.20f : 0.07f;

    float next = overlayAlpha;

    if (next < target)      next = juce::jmin (target, next + step);
    else if (next > target) next = juce::jmax (target, next - step);

    if (next != overlayAlpha)
    {
        overlayAlpha = next;
        topBar.setAlpha (overlayAlpha);
        topBar.setVisible (overlayAlpha > 0.01f);
        repaint (getStatusBounds());
    }
}

// Подгоняет окно под пропорции кадра камеры, чтобы не было полос по краям
void WebcamAudioProcessorEditor::adaptToVideoAspect (int videoWidth, int videoHeight)
{
    if (videoWidth < 2 || videoHeight < 2)
        return;

    const double aspect = (double) videoWidth / (double) videoHeight;

    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (aspect);

    const int newHeight = juce::jlimit (kMinH, kMaxH, juce::roundToInt ((double) getWidth() / aspect));

    if (std::abs (newHeight - getHeight()) > 2)
        setSize (getWidth(), newHeight);
}

juce::Rectangle<int> WebcamAudioProcessorEditor::getStatusBounds() const
{
    const int reserved = recording ? 112 : 0;
    return { 8, getHeight() - 8 - 26, juce::jmax (0, getWidth() - 16 - reserved), 26 };
}

juce::Rectangle<int> WebcamAudioProcessorEditor::getRecBounds() const
{
    return { getWidth() - 8 - 100, getHeight() - 8 - 26, 100, 26 };
}

//==============================================================================
void WebcamAudioProcessorEditor::openCamera (int deviceIndex)
{
    closeCamera();

    if (deviceIndex < 0 || deviceIndex >= cameraNames.size())
        return;

    setStatus (loc::Text::opening);
    openedAtMs = juce::Time::getMillisecondCounter();

    const int generation = ++openGeneration;
    juce::Component::SafePointer<WebcamAudioProcessorEditor> safe (this);

    juce::CameraDevice::openDeviceAsync (
        deviceIndex,
        [safe, generation] (juce::CameraDevice* device, const juce::String& error)
        {
            std::unique_ptr<juce::CameraDevice> owned (device);

            if (safe == nullptr || generation != safe->openGeneration)
                return; // окно закрыто или выбрана другая камера, owned удалится сам

            safe->cameraOpened (std::move (owned), error);
        },
        320, 240, 1280, 720, true);
}

void WebcamAudioProcessorEditor::cameraOpened (std::unique_ptr<juce::CameraDevice> device,
                                               const juce::String& error)
{
    if (device == nullptr)
    {
        if (error.isNotEmpty())
            setStatus (loc::Text::cameraError, error);
        else
            setStatus (loc::Text::cameraErrorGeneric);

        return;
    }

    camera = std::move (device);
    camera->addListener (this);
    openedAtMs = juce::Time::getMillisecondCounter();
    setStatus (loc::Text::waiting);
}

void WebcamAudioProcessorEditor::closeCamera()
{
    ++openGeneration;

    if (recording)
        stopRecordingInternal();

    if (camera != nullptr)
    {
        camera->removeListener (this);
        camera.reset();
    }

    gotFrame = false;
    noVideoWarned = false;

    {
        const juce::ScopedLock sl (frameLock);
        latestFrame = juce::Image();
        newFrame = false;
    }

    currentFrame = juce::Image();
    fisheyeImage = juce::Image();
    repaint();
}

void WebcamAudioProcessorEditor::imageReceived (const juce::Image& image)
{
    // Поток камеры: делаем копию (буфер камеры может переиспользоваться)
    juce::Image copy = image.createCopy();

    const juce::ScopedLock sl (frameLock);
    latestFrame = copy;
    newFrame = true;
}

void WebcamAudioProcessorEditor::timerCallback()
{
    updateOverlay();
    pollTransport();

    // Звук DAW не приходит: плагин не на мастер-канале или хост не вызывает обработку
    if (recording && ! hostWarned && recordKind != RecordKind::nativeVideo
        && proc.audioSource == 2 && juce::Time::getMillisecondCounter() - recordStartMs > 2000
        && proc.hostCapture.getBlocksPushed() == 0)
    {
        hostWarned = true;
        showUntilMs = juce::Time::getMillisecondCounter() + 6000;
        setStatus (loc::Text::hostNoAudio);
    }

    // Запись остановилась из-за ошибки (например, закончилось место на диске)
    if (recording && avRecorder != nullptr && avRecorder->hasFailed())
    {
        stopRecordingInternal();
        setStatus (loc::Text::writeError);
    }

    juce::Image frame;
    bool have = false;

    {
        const juce::ScopedLock sl (frameLock);
        if (newFrame)
        {
            frame = latestFrame;
            newFrame = false;
            have = true;
        }
    }

    if (have && frame.isValid())
    {
        if (! gotFrame)
        {
            gotFrame = true;
            setStatus (loc::Text::live, juce::String (frame.getWidth()) + "x" + juce::String (frame.getHeight()));
            adaptToVideoAspect (frame.getWidth(), frame.getHeight());
        }

        currentFrame = frame;

        if (recordKind == RecordKind::avi && avRecorder != nullptr)
            avRecorder->pushFrame (frame);

        refreshDisplay();
    }
    else
    {
        if (camera != nullptr && ! gotFrame && ! noVideoWarned
            && juce::Time::getMillisecondCounter() - openedAtMs > 4000)
        {
            noVideoWarned = true;
            setStatus (loc::Text::noVideo);
        }

        if (recording)
            repaint (getRecBounds()); // обновить таймер записи
    }
}

void WebcamAudioProcessorEditor::refreshDisplay()
{
    if (getLens (proc.lensIndex).fisheye && currentFrame.isValid())
        fisheyeImage = makeFisheye (currentFrame);

    repaint();
}

//==============================================================================
void WebcamAudioProcessorEditor::toggleRecording()
{
    if (! recording)
    {
        startRecording (false);
    }
    else
    {
        stopRecordingInternal();
        setStatus (loc::Text::saved, recordingInfo);
    }
}

// Запись стартует и останавливается вместе с Play/Stop в DAW (если включена синхронизация)
void WebcamAudioProcessorEditor::pollTransport()
{
    const bool playing = proc.hostPlaying.load();
    const bool started = playing && ! prevHostPlaying;
    const bool stopped = ! playing && prevHostPlaying;
    prevHostPlaying = playing;

    if (! proc.syncTransport)
        return;

    if (started && ! recording)
    {
        startRecording (true);
    }
    else if (stopped && recording && recordedByTransport)
    {
        stopRecordingInternal();
        setStatus (loc::Text::saved, recordingInfo);
    }
}

AudioSettings WebcamAudioProcessorEditor::makeAudioSettings() const
{
    AudioSettings settings;

    if (proc.audioSource == 2)
    {
        settings.source = AudioSettings::Source::host;
        settings.host = &proc.hostCapture;
        settings.offsetMs = proc.audioOffsetMs;
    }
    else
    {
        settings.source = AudioSettings::Source::microphone;
        settings.micName = getEffectiveMicName();
    }

    return settings;
}

// Поле в плагине важнее автоопределения: пользователь сам знает, как назвать запись
juce::String WebcamAudioProcessorEditor::resolveProjectName() const
{
    const auto manual = proc.projectName.trim();

    if (manual.isNotEmpty())
        return manual;

    return detectHostProjectName();
}

// <проект>_<BPM>bpm_<дата>; если такой файл уже есть (две записи за секунду), добавляется номер
juce::String WebcamAudioProcessorEditor::makeUniqueBaseName (const juce::File& folder) const
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    const auto base = juce::String::fromUTF8 (
        naming::buildBaseName (resolveProjectName().toStdString(), proc.hostBpm.load(), stamp.toStdString()).c_str());

    for (int n = 1; n < 1000; ++n)
    {
        const auto candidate = n == 1 ? base : base + "_" + juce::String (n);
        bool taken = false;

        for (auto ext : { ".wmv", ".mov", ".avi", ".wav" })
            taken = taken || folder.getChildFile (candidate + ext).exists();

        if (! taken)
            return candidate;
    }

    return base;
}

void WebcamAudioProcessorEditor::startRecording (bool byTransport)
{
    if (camera == nullptr)
    {
        setStatus (loc::Text::cantRecordNoCamera);
        return;
    }

    auto folder = getRecordingsFolder();
    folder.createDirectory();

    const juce::String baseName = makeUniqueBaseName (folder);

   #if JUCE_WINDOWS
    const juce::String videoExt = ".wmv";
   #else
    const juce::String videoExt = ".mov";
   #endif

    const auto videoFile = folder.getChildFile (baseName + videoExt);

    // Звук DAW: нужно, чтобы хост уже сообщил частоту дискретизации (prepareToPlay)
    if (proc.audioSource == 2 && proc.hostCapture.getSampleRate() <= 0.0)
    {
        setStatus (loc::Text::hostNotReady);
        return;
    }

    const auto audio = makeAudioSettings();

    if (proc.audioSource == 0)
    {
        // Только видео: пишет сама камера
        camera->startRecordingToFile (videoFile, 2);
        recordKind = RecordKind::nativeVideo;
        recordingInfo = videoFile.getFullPathName();
    }
    else if (proc.audioMode == 0)
    {
        // Видео и звук вместе: один AVI-файл (MJPEG + PCM), кадры берём из превью
        if (! gotFrame || ! currentFrame.isValid())
        {
            setStatus (loc::Text::cantRecordNoVideo);
            return;
        }

        const auto aviFile = folder.getChildFile (baseName + ".avi");

        avRecorder = std::make_unique<AvRecorder>();
        avRecorder->pushFrame (currentFrame);

        const auto error = avRecorder->start (AvRecorder::Mode::aviWithAudio, aviFile,
                                              currentFrame.getWidth(), currentFrame.getHeight(), audio);

        if (error.isNotEmpty())
        {
            avRecorder.reset();
            setStatus (loc::Text::micError, error);
            return;
        }

        recordKind = RecordKind::avi;
        recordingInfo = aviFile.getFullPathName();
    }
    else
    {
        // Звук отдельным WAV-файлом, видео пишет камера. Источник звука открывается первым (микрофон
        // открывается долго), но в файл звук идёт только с момента запуска записи видео.
        const auto wavFile = folder.getChildFile (baseName + ".wav");

        avRecorder = std::make_unique<AvRecorder>();

        const auto error = avRecorder->start (AvRecorder::Mode::wavOnly, wavFile, 0, 0, audio);

        if (error.isNotEmpty())
        {
            avRecorder.reset();
            setStatus (loc::Text::micError, error);
            return;
        }

        camera->startRecordingToFile (videoFile, 2);
        avRecorder->beginAudio(); // звук в WAV начинается только теперь, вместе с видео
        recordKind = RecordKind::nativeVideoAndWav;
        recordingInfo = videoFile.getFullPathName() + "  +  " + wavFile.getFileName();
    }

    recording = true;
    recordedByTransport = byTransport;
    hostWarned = false;
    recordStartMs = juce::Time::getMillisecondCounter();
    recordButton.setButtonText (tr (loc::Text::stop));
    recordButton.setColour (juce::TextButton::buttonColourId, kRecordingBg);
    recordButton.setColour (juce::TextButton::buttonOnColourId, kRecordingBg);
    updateControlsEnabled();
    setStatus (loc::Text::recording, recordingInfo);
}

void WebcamAudioProcessorEditor::stopRecordingInternal()
{
    if ((recordKind == RecordKind::nativeVideo || recordKind == RecordKind::nativeVideoAndWav)
        && camera != nullptr)
        camera->stopRecording();

    if (avRecorder != nullptr)
    {
        avRecorder->stop();
        avRecorder.reset();
    }

    recordKind = RecordKind::none;
    recording = false;
    recordedByTransport = false;

    recordButton.setButtonText (tr (loc::Text::record));
    recordButton.setColour (juce::TextButton::buttonColourId, kControlBg);
    recordButton.setColour (juce::TextButton::buttonOnColourId, kControlBg);
    updateControlsEnabled();
    repaint();
}

//==============================================================================
void WebcamAudioProcessorEditor::paint (juce::Graphics& g)
{
    const int W = getWidth();
    const int H = getHeight();

    g.fillAll (juce::Colours::black);

    if (W <= 0 || H <= 0)
        return;

    const auto& lens = getLens (proc.lensIndex);
    const juce::Image& img = lens.fisheye ? fisheyeImage : currentFrame;
    const bool haveVideo = img.isValid();
    const auto statusText = getStatusText();

    if (! haveVideo)
    {
        // Пока видео нет, сообщение (в том числе об ошибке) видно без наведения курсора
        g.setColour (juce::Colours::grey);
        g.setFont (16.0f);
        g.drawFittedText (statusId != loc::Text::none ? statusText : tr (loc::Text::waitingPlaceholder),
                          getLocalBounds().reduced (40), juce::Justification::centred, 4);
    }
    else
    {
        juce::Graphics::ScopedSaveState state (g);

        // Кадр заполняет окно целиком: лишнее по краям обрезается (режим "cover")
        const float zoom = lens.fisheye ? 1.0f : lens.zoom;
        const int imgW = img.getWidth();
        const int imgH = img.getHeight();

        double cropW = (double) imgW / (double) zoom;
        double cropH = (double) imgH / (double) zoom;
        const double winAspect = (double) W / (double) H;

        if (cropW / cropH > winAspect)
            cropW = cropH * winAspect;
        else
            cropH = cropW / winAspect;

        const int cw = juce::jlimit (2, imgW, juce::roundToInt (cropW));
        const int ch = juce::jlimit (2, imgH, juce::roundToInt (cropH));
        const int cx = (imgW - cw) / 2;
        const int cy = (imgH - ch) / 2;

        if (proc.mirror)
            g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f, (float) W * 0.5f, (float) H * 0.5f));

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (img, 0, 0, W, H, cx, cy, cw, ch);
    }

    // Строка статуса: только при наведении курсора, плавно появляется поверх видео
    if (haveVideo && overlayAlpha > 0.01f && statusText.isNotEmpty())
    {
        const auto bar = getStatusBounds();

        if (bar.getWidth() > 40)
        {
            g.setColour (juce::Colours::black.withAlpha (0.45f * overlayAlpha));
            g.fillRoundedRectangle (bar.toFloat(), 8.0f);

            g.setColour (juce::Colours::white.withAlpha (0.9f * overlayAlpha));
            g.setFont (13.0f);
            g.drawText (statusText, bar.reduced (10, 0), juce::Justification::centredLeft, true);
        }
    }

    // Индикатор записи виден всегда, чтобы не забыть про идущую запись
    if (recording)
    {
        const auto rec = getRecBounds();
        const auto secs = (int) ((juce::Time::getMillisecondCounter() - recordStartMs) / 1000);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (rec.toFloat(), 8.0f);

        g.setColour (juce::Colours::red);
        g.fillEllipse ((float) rec.getX() + 10.0f, (float) rec.getCentreY() - 5.0f, 10.0f, 10.0f);

        g.setColour (juce::Colours::white);
        g.setFont (13.0f);
        g.drawText (juce::String::formatted ("REC %02d:%02d", secs / 60, secs % 60),
                    rec.withTrimmedLeft (26), juce::Justification::centredLeft, false);
    }
}

void WebcamAudioProcessorEditor::resized()
{
    // Панель с настройками лежит поверх видео, видео занимает всё окно.
    // Кнопки раскладываются по строкам автоматически: чем уже окно, тем больше строк.
    // Скрытые элементы (например, микрофон при записи звука DAW) места не занимают.
    constexpr int pad = 8, gap = 6, rowHeight = 28, flexibleMin = 110;

    const int panelWidth = juce::jmax (0, getWidth() - 16);
    const int availableWidth = juce::jmax (0, panelWidth - 2 * pad);

    struct Item
    {
        juce::Component* component;
        int width;       // ширина; для растягиваемых это минимальная ширина
        bool flexible;   // забирает свободное место в своей строке
    };

    const int source = proc.audioSource;

    std::vector<Item> items;
    items.push_back ({ &cameraBox,    flexibleMin, true  });
    items.push_back ({ &lensBox,      150,         false });
    items.push_back ({ &mirrorButton, 96,          false });
    items.push_back ({ &recordButton, 90,          false });
    items.push_back ({ &folderButton, 100,         false });
    items.push_back ({ &audioBox,     170,         false });

    if (source == 1) items.push_back ({ &micBox,       flexibleMin, true  });
    if (source != 0) items.push_back ({ &modeBox,      190,         false });
    if (source == 2) items.push_back ({ &offsetLabel,  92,          false });
    if (source == 2) items.push_back ({ &offsetSlider, 130,         false });

    items.push_back ({ &syncButton,   160,         false });
    items.push_back ({ &projectEdit,  flexibleMin, true  });
    items.push_back ({ &langButton,   130,         false });

    const int numItems = (int) items.size();

    // Скрытые компоненты не должны оставаться видимыми
    for (auto* c : std::initializer_list<juce::Component*> { &micBox, &modeBox, &offsetLabel, &offsetSlider })
    {
        bool used = false;

        for (const auto& item : items)
            used = used || item.component == c;

        c->setVisible (used);
    }

    // Разбиваем на строки: новый элемент идёт в текущую строку, пока помещается
    std::vector<int> rowOf ((size_t) numItems, 0);
    int numRows = 1, used = 0;

    for (int i = 0; i < numItems; ++i)
    {
        const int w = juce::jmin (items[(size_t) i].width, availableWidth);
        const int needed = (used == 0 ? 0 : gap) + w;

        if (used > 0 && used + needed > availableWidth)
        {
            ++numRows;
            used = 0;
        }

        used += (used == 0 ? 0 : gap) + w;
        rowOf[(size_t) i] = numRows - 1;
    }

    const int panelHeight = 2 * pad + numRows * rowHeight + (numRows - 1) * gap;
    topBar.setBounds (8, 8, panelWidth, panelHeight);

    // Раскладываем каждую строку; свободное место делят между растягиваемыми элементами строки
    for (int row = 0; row < numRows; ++row)
    {
        int firstItem = -1, count = 0, fixedTotal = 0, flexCount = 0;

        for (int i = 0; i < numItems; ++i)
        {
            if (rowOf[(size_t) i] != row)
                continue;

            if (firstItem < 0)
                firstItem = i;

            ++count;
            fixedTotal += juce::jmin (items[(size_t) i].width, availableWidth);

            if (items[(size_t) i].flexible)
                ++flexCount;
        }

        const int freeSpace = juce::jmax (0, availableWidth - fixedTotal - (count - 1) * gap);
        const int extraEach = flexCount > 0 ? freeSpace / flexCount : 0;

        int x = pad;
        const int y = pad + row * (rowHeight + gap);

        for (int i = firstItem; i < numItems && rowOf[(size_t) i] == row; ++i)
        {
            int w = juce::jmin (items[(size_t) i].width, availableWidth);

            if (items[(size_t) i].flexible)
                w += extraEach;

            items[(size_t) i].component->setBounds (x, y, w, rowHeight);
            x += w + gap;
        }
    }

    repaint();
}
