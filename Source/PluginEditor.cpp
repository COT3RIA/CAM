#include "PluginEditor.h"
#include "FileNaming.h"
#include "HostProject.h"

namespace
{
    // Веб-камера не умеет менять оптику, поэтому "объективы" эмулируются программно:
    // зум = цифровой кроп центра кадра, остальное искажения, полосы или размытие.
    // id хранится в проекте DAW: у старых объективов он не менялся (0..4), новые добавлены с 5.
    enum class LensKind { plain, fisheye, pincushion, tiltShift };

    struct Lens
    {
        int id;
        loc::Text name;
        float zoom;
        LensKind kind;
        bool letterbox;   // чёрные полосы под кинокадр 2.39:1
    };

    // Порядок в списке: по возрастанию фокусного, затем эффекты
    const Lens kLenses[] =
    {
        { 0,  loc::Text::lens13,         1.0f, LensKind::plain,      false },
        { 1,  loc::Text::lens24,         1.5f, LensKind::plain,      false },
        { 5,  loc::Text::lens35,         2.0f, LensKind::plain,      false },
        { 2,  loc::Text::lens48,         2.5f, LensKind::plain,      false },
        { 6,  loc::Text::lens50,         2.6f, LensKind::plain,      false },
        { 3,  loc::Text::lens77,         4.0f, LensKind::plain,      false },
        { 7,  loc::Text::lens85,         4.4f, LensKind::plain,      false },
        { 8,  loc::Text::lens135,        6.5f, LensKind::plain,      false },
        { 4,  loc::Text::lensFisheye,    1.0f, LensKind::fisheye,    false },
        { 9,  loc::Text::lensPincushion, 1.0f, LensKind::pincushion, false },
        { 10, loc::Text::lensCinema,     1.0f, LensKind::plain,      true  },
        { 11, loc::Text::lensTiltShift,  1.0f, LensKind::tiltShift,  false },
    };

    constexpr int kNumLenses = (int) (sizeof (kLenses) / sizeof (kLenses[0]));

    // Размеры окна: 16:9 по умолчанию, соотношение потом подстраивается под камеру
    // Минимум 480x270: при такой ширине панель настроек переносит кнопки на несколько строк
    constexpr int kMinW = 480, kMinH = 270, kMaxW = 1920, kMaxH = 1080;
    constexpr int kStartW = 640, kStartH = 360;

    // Индикаторы звука (биты в proc.indicatorFlags)
    constexpr int kFxMeterPill = 1, kFxMiniMeter = 2, kFxStatusDot = 4, kFxWave = 8, kFxPulse = 16;
    constexpr int kFxAll = kFxMeterPill | kFxMiniMeter | kFxStatusDot | kFxWave | kFxPulse;
    constexpr int kMeterPillWidth = 136, kMeterPillHeight = 26;

    // Уровень по шкале дБ: -48 дБ и тише = 0, 0 дБ = 1
    float levelFromPeak (float peak)
    {
        if (peak <= 0.0f)
            return 0.0f;

        return juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (peak) + 48.0f) / 48.0f);
    }

    juce::Colour meterColour (float level, bool clipped)
    {
        if (clipped)       return juce::Colour (0xffe5484d);
        if (level > 0.85f) return juce::Colour (0xfff0b429);
        return juce::Colour (0xff2fbf71);
    }

    void drawLevelBar (juce::Graphics& g, juce::Rectangle<float> r, float level, bool clipped, float alpha)
    {
        g.setColour (juce::Colours::white.withAlpha (0.18f * alpha));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

        if (level > 0.003f)
        {
            g.setColour (meterColour (level, clipped).withAlpha (alpha));
            g.fillRoundedRectangle (r.withWidth (juce::jmax (r.getHeight(), r.getWidth() * level)), r.getHeight() * 0.5f);
        }
    }

    const juce::Colour kControlBg   (0x66000000);
    const juce::Colour kControlLine (0x40ffffff);
    const juce::Colour kRecordingBg (0xa0d02020);

    const Lens& getLens (int id)
    {
        for (const auto& lens : kLenses)
            if (lens.id == id)
                return lens;

        return kLenses[1]; // неизвестный id (например, из будущей версии): 24 мм
    }

    juce::File getRecordingsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMoviesDirectory)
                   .getChildFile ("CAM");
    }

    // Программное искажение с билинейной интерполяцией: "рыбий глаз" (бочка) или подушка
    juce::Image makeWarp (const juce::Image& source, bool pincushion)
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
                const float f = pincushion ? 1.0f - 0.16f * r2            // края растянуты: подушка
                                           : (1.0f + k * r2) / (1.0f + k); // края сжаты: бочка
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

    // Тилт-шифт: резкая полоса по центру, верх и низ размыты (эффект миниатюры)
    juce::Image makeTiltShift (const juce::Image& source)
    {
        const int sw = source.getWidth();
        const int sh = source.getHeight();

        if (sw < 2 || sh < 2)
            return {};

        const float scale = juce::jmin (1.0f, 640.0f / (float) sw);
        const int ow = juce::jmax (8, (int) ((float) sw * scale));
        const int oh = juce::jmax (8, (int) ((float) sh * scale));

        juce::Image sharp = source.convertedToFormat (juce::Image::ARGB)
                                  .rescaled (ow, oh, juce::Graphics::mediumResamplingQuality);

        // Размытие: сильное уменьшение и обратное увеличение с интерполяцией
        juce::Image blurred = sharp.rescaled (juce::jmax (2, ow / 8), juce::jmax (2, oh / 8), juce::Graphics::highResamplingQuality)
                                   .rescaled (ow, oh, juce::Graphics::highResamplingQuality);

        juce::Image out = sharp.createCopy();

        juce::Image::BitmapData dst (out, juce::Image::BitmapData::readWrite);
        juce::Image::BitmapData blr (blurred, juce::Image::BitmapData::readOnly);

        for (int y = 0; y < oh; ++y)
        {
            const float v = std::abs ((((float) y + 0.5f) / (float) oh) * 2.0f - 1.0f);
            const float t = juce::jlimit (0.0f, 1.0f, (v - 0.22f) / 0.5f);
            const float w = t * t * (3.0f - 2.0f * t);          // 0 в центре, 1 у краёв
            const int wi = (int) (w * 256.0f);

            if (wi == 0)
                continue;

            auto* d = reinterpret_cast<juce::PixelARGB*> (dst.getLinePointer (y));
            const auto* b = reinterpret_cast<const juce::PixelARGB*> (blr.getLinePointer (y));

            for (int x = 0; x < ow; ++x)
            {
                auto mix = [wi] (int a, int c) { return (juce::uint8) (a + (((c - a) * wi) >> 8)); };
                d[x].setARGB (255, mix (d[x].getRed(),   b[x].getRed()),
                                   mix (d[x].getGreen(), b[x].getGreen()),
                                   mix (d[x].getBlue(),  b[x].getBlue()));
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
    topBar.addAndMakeVisible (lookBox);
    topBar.addAndMakeVisible (vignetteButton);
    topBar.addAndMakeVisible (grainButton);
    topBar.addAndMakeVisible (recordButton);
    topBar.addAndMakeVisible (folderButton);
    topBar.addAndMakeVisible (audioBox);
    topBar.addAndMakeVisible (micBox);
    topBar.addAndMakeVisible (modeBox);
    topBar.addAndMakeVisible (offsetLabel);
    topBar.addAndMakeVisible (offsetSlider);
    topBar.addAndMakeVisible (syncButton);
    topBar.addAndMakeVisible (projectEdit);
    topBar.addAndMakeVisible (indicatorsButton);
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
    vignetteButton.setToggleState (proc.vignette, juce::dontSendNotification);
    grainButton.setToggleState (proc.grain, juce::dontSendNotification);
    syncButton.setToggleState (proc.syncTransport, juce::dontSendNotification);
    prevHostPlaying = proc.hostPlaying.load();
    lastHostBlocks = proc.hostBlocks.load();
    lastHostBlockMs = juce::Time::getMillisecondCounter();

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
        proc.lensIndex = lensBox.getSelectedId() - 1; // id объектива, не позиция в списке
        refreshDisplay();
    };

    lookBox.onChange = [this]
    {
        proc.lookIndex = juce::jlimit (0, 6, lookBox.getSelectedItemIndex());
        reapplyLook();
    };

    vignetteButton.onClick = [this]
    {
        proc.vignette = vignetteButton.getToggleState();
        reapplyLook();
    };

    grainButton.onClick = [this]
    {
        proc.grain = grainButton.getToggleState();
        reapplyLook();
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
    indicatorsButton.onClick = [this] { showIndicatorsMenu(); };

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
    for (auto* box : { &cameraBox, &lensBox, &lookBox, &audioBox, &micBox, &modeBox })
    {
        box->setColour (juce::ComboBox::backgroundColourId, kControlBg);
        box->setColour (juce::ComboBox::outlineColourId, kControlLine);
        box->setColour (juce::ComboBox::textColourId, juce::Colours::white);
        box->setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
    }

    for (auto* button : { &recordButton, &folderButton, &indicatorsButton, &langButton })
    {
        button->setColour (juce::TextButton::buttonColourId, kControlBg);
        button->setColour (juce::TextButton::buttonOnColourId, kControlBg);
        button->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    for (auto* toggle : { &mirrorButton, &vignetteButton, &grainButton, &syncButton })
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
    vignetteButton.setButtonText (tr (loc::Text::vignette));
    grainButton.setButtonText (tr (loc::Text::grain));

    lookBox.clear (juce::dontSendNotification);
    {
        const loc::Text lookNames[] = { loc::Text::lookNone, loc::Text::lookBw, loc::Text::lookSepia, loc::Text::lookWarm,
                                        loc::Text::lookCool, loc::Text::lookContrast, loc::Text::lookVhs };

        for (int i = 0; i < 7; ++i)
            lookBox.addItem (tr (lookNames[i]), i + 1);

        lookBox.setSelectedItemIndex (juce::jlimit (0, 6, proc.lookIndex), juce::dontSendNotification);
    }
    syncButton.setButtonText (tr (loc::Text::syncTransport));
    indicatorsButton.setButtonText (tr (loc::Text::indicators));
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
        lensBox.addItem (tr (kLenses[i].name), kLenses[i].id + 1);

    lensBox.setSelectedId (getLens (proc.lensIndex).id + 1, juce::dontSendNotification);

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
                          || micBox.isPopupActive() || modeBox.isPopupActive() || audioBox.isPopupActive() || lookBox.isPopupActive()
                          || projectEdit.hasKeyboardFocus (true) || indicatorsMenuOpen;

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
    // Строка статуса лежит между метром слева и плашкой REC справа
    const int left = isMeterPillShown() ? 8 + kMeterPillWidth + 6 : 8;
    const int right = recording ? getRecBounds().getWidth() + 8 + 6 : 8;
    return { left, getHeight() - 8 - 26, juce::jmax (0, getWidth() - left - right), 26 };
}

juce::Rectangle<int> WebcamAudioProcessorEditor::getRecBounds() const
{
    const int fx = proc.indicatorFlags;
    const int width = 100 + ((fx & kFxStatusDot) != 0 ? 16 : 0) + ((fx & kFxMiniMeter) != 0 ? 62 : 0);
    return { getWidth() - 8 - width, getHeight() - 8 - 26, width, 26 };
}

bool WebcamAudioProcessorEditor::isMeterPillShown() const
{
    return (proc.indicatorFlags & kFxMeterPill) != 0 && proc.audioSource != 0;
}

juce::Rectangle<int> WebcamAudioProcessorEditor::getMeterPillBounds() const
{
    return { 8, getHeight() - 8 - kMeterPillHeight, kMeterPillWidth, kMeterPillHeight };
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

    rawFrame = juce::Image();
    currentFrame = juce::Image();
    lensImage = juce::Image();
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
    updateLevels();

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

        rawFrame = frame;
        currentFrame = applyLook (frame);

        if (recordKind == RecordKind::avi && avRecorder != nullptr)
            avRecorder->pushFrame (currentFrame);

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

// Цветовой фильтр, виньетка, зерно. Результат виден в окне и попадает в AVI-запись
// (запись камеры в WMV/MOV идёт мимо плагина и фильтров не получает).
juce::Image WebcamAudioProcessorEditor::applyLook (const juce::Image& source)
{
    const auto look = (looks::Look) juce::jlimit (0, 6, proc.lookIndex);

    if (! source.isValid() || (look == looks::Look::none && ! proc.vignette && ! proc.grain))
        return source;

    juce::Image out = source.convertedToFormat (juce::Image::ARGB);

    if (out.getPixelData().get() == source.getPixelData().get())
        out = source.createCopy(); // формат уже ARGB: не портим исходный кадр

    {
        juce::Image::BitmapData data (out, juce::Image::BitmapData::readWrite);

       #if JUCE_LITTLE_ENDIAN
        const looks::ChannelOffsets offsets { 2, 1, 0 };   // в памяти B G R A
       #else
        const looks::ChannelOffsets offsets { 1, 2, 3 };   // в памяти A R G B
       #endif

        looks::process (data.data, data.width, data.height, data.lineStride, data.pixelStride,
                        offsets, look, proc.vignette, proc.grain, ++lookFrameCounter);
    }

    return out;
}

// Фильтр поменяли: пересчитываем последний кадр, чтобы эффект был виден сразу, а не со следующим кадром камеры
void WebcamAudioProcessorEditor::reapplyLook()
{
    if (rawFrame.isValid())
    {
        currentFrame = applyLook (rawFrame);
        refreshDisplay();
    }
}

void WebcamAudioProcessorEditor::refreshDisplay()
{
    if (currentFrame.isValid())
    {
        switch (getLens (proc.lensIndex).kind)
        {
            case LensKind::fisheye:    lensImage = makeWarp (currentFrame, false); break;
            case LensKind::pincushion: lensImage = makeWarp (currentFrame, true);  break;
            case LensKind::tiltShift:  lensImage = makeTiltShift (currentFrame);   break;
            case LensKind::plain:      break;
        }
    }

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
// Индикаторы звука
void WebcamAudioProcessorEditor::updateLevels()
{
    const auto now = juce::Time::getMillisecondCounter();
    const int source = proc.audioSource;

    // Звук DAW идёт, пока хост присылает блоки (в паузе DAW блоки тоже приходят, но с тишиной)
    const auto blocks = proc.hostBlocks.load();

    if (blocks != lastHostBlocks)
    {
        lastHostBlocks = blocks;
        lastHostBlockMs = now;
    }

    float peakL = 0.0f, peakR = 0.0f;

    if (source == 2)
    {
        peakL = proc.hostPeakL.exchange (0.0f);
        peakR = proc.hostPeakR.exchange (0.0f);
        audioState = (now - lastHostBlockMs < 1000) ? AudioState::ok : AudioState::noSignal;
    }
    else if (source == 1)
    {
        // Микрофон открывается только на время записи, вне записи уровня нет
        if (recording && avRecorder != nullptr)
        {
            peakL = peakR = avRecorder->takeMicPeak();
            audioState = AudioState::ok;
        }
        else
        {
            audioState = AudioState::idle;
        }
    }
    else
    {
        audioState = AudioState::off;
    }

    auto update = [] (float& level, int& hold, float peak)
    {
        const float target = levelFromPeak (peak);
        level = juce::jmax (target, level * 0.88f);

        if (peak >= 0.99f)
            hold = 20;
        else if (hold > 0)
            --hold;
    };

    update (levelL, clipHoldL, peakL);
    update (levelR, clipHoldR, peakR);

    waveHistory[(size_t) waveHead] = (levelL + levelR) * 0.5f;
    waveHead = (waveHead + 1) % (int) waveHistory.size();

    // Перерисовываем только нижнюю полосу окна, где живут индикаторы
    if (proc.indicatorFlags != 0 && source != 0 && gotFrame)
        repaint (0, getHeight() - 64, getWidth(), 64);
}

void WebcamAudioProcessorEditor::showIndicatorsMenu()
{
    struct Entry { int bit; loc::Text text; };
    const Entry entries[] =
    {
        { kFxMeterPill, loc::Text::fxMeterPill }, { kFxMiniMeter, loc::Text::fxMiniMeter },
        { kFxStatusDot, loc::Text::fxStatusDot }, { kFxWave,      loc::Text::fxWave },
        { kFxPulse,     loc::Text::fxPulse }
    };

    juce::PopupMenu menu;
    int id = 1;

    for (const auto& entry : entries)
        menu.addItem (id++, tr (entry.text), true, (proc.indicatorFlags & entry.bit) != 0);

    menu.addSeparator();
    menu.addItem (100, tr (loc::Text::fxAll), true, proc.indicatorFlags == kFxAll);
    menu.addItem (101, tr (loc::Text::fxNone), true, proc.indicatorFlags == 0);

    indicatorsMenuOpen = true;

    juce::Component::SafePointer<WebcamAudioProcessorEditor> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&indicatorsButton),
                        [safe] (int result)
    {
        if (safe == nullptr)
            return;

        safe->indicatorsMenuOpen = false;
        auto& fxFlags = safe->proc.indicatorFlags;

        if (result >= 1 && result <= 5)
            fxFlags ^= (1 << (result - 1));
        else if (result == 100)
            fxFlags = kFxAll;
        else if (result == 101)
            fxFlags = 0;

        safe->resized(); // ширина плашки REC и строки статуса зависит от набора индикаторов
        safe->repaint();
    });
}

// Подпись по источнику звука: треугольник Play и "DAW" либо микрофон и "MIC"
void WebcamAudioProcessorEditor::drawMeterPill (juce::Graphics& g, float alpha) const
{
    const auto b = getMeterPillBounds().toFloat();
    const bool warn = (audioState == AudioState::noSignal);
    const bool dim = (audioState == AudioState::idle);
    const float contentAlpha = alpha * (dim ? 0.6f : 1.0f);

    g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
    g.fillRoundedRectangle (b, 8.0f);

    const auto fg = (warn ? juce::Colour (0xffffb4b4) : juce::Colours::white).withAlpha (contentAlpha);
    g.setColour (fg);

    const float cx = b.getX() + 17.0f, cy = b.getCentreY();

    if (proc.audioSource == 2)
    {
        juce::Path play;
        play.addTriangle (cx - 4.0f, cy - 5.5f, cx - 4.0f, cy + 5.5f, cx + 5.0f, cy);
        g.fillPath (play);
    }
    else
    {
        g.fillRoundedRectangle (cx - 2.5f, cy - 6.0f, 5.0f, 8.5f, 2.5f);
        g.drawLine (cx - 4.5f, cy + 1.0f, cx - 4.5f, cy + 2.0f, 1.2f);
        g.drawLine (cx + 4.5f, cy + 1.0f, cx + 4.5f, cy + 2.0f, 1.2f);
        g.drawLine (cx - 4.5f, cy + 2.0f, cx + 4.5f, cy + 2.0f, 1.2f);
        g.drawLine (cx, cy + 2.5f, cx, cy + 6.0f, 1.2f);
        g.drawLine (cx - 3.0f, cy + 6.0f, cx + 3.0f, cy + 6.0f, 1.2f);
    }

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (proc.audioSource == 2 ? "DAW" : "MIC",
                juce::Rectangle<float> (b.getX() + 30.0f, b.getY(), 36.0f, b.getHeight()),
                juce::Justification::centredLeft, false);

    const float mx = b.getX() + 68.0f, mw = 58.0f, mh = 5.0f;
    drawLevelBar (g, { mx, cy - 7.0f, mw, mh }, levelL, clipHoldL > 0, contentAlpha);
    drawLevelBar (g, { mx, cy + 2.0f, mw, mh }, levelR, clipHoldR > 0, contentAlpha);
}

void WebcamAudioProcessorEditor::drawWave (juce::Graphics& g, float alpha) const
{
    const float w = (float) getWidth();
    const float centreY = (float) getHeight() - 32.0f;
    const int n = (int) waveHistory.size();
    const float barW = w / (float) n;

    g.setColour (juce::Colours::white.withAlpha (0.35f * alpha));

    for (int i = 0; i < n; ++i)
    {
        const float level = waveHistory[(size_t) ((waveHead + i) % n)];
        const float half = juce::jmax (0.5f, level * 22.0f);
        g.fillRect (i * barW, centreY - half, juce::jmax (1.0f, barW - 1.0f), half * 2.0f);
    }
}

// Плашка записи: [точка состояния] [мини-метр] [красная точка] REC мм:сс
void WebcamAudioProcessorEditor::drawRecPill (juce::Graphics& g) const
{
    const int fx = proc.indicatorFlags;
    const auto rec = getRecBounds();
    const auto secs = (int) ((juce::Time::getMillisecondCounter() - recordStartMs) / 1000);
    const float cy = (float) rec.getCentreY();

    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (rec.toFloat(), 8.0f);

    float x = (float) rec.getX() + 10.0f;

    if ((fx & kFxStatusDot) != 0)
    {
        juce::Colour c (0xff8b919b);

        if (audioState == AudioState::ok)
            c = juce::Colour (0xff2fbf71);
        else if (audioState == AudioState::noSignal)
            c = ((juce::Time::getMillisecondCounter() / 500) % 2 == 0) ? juce::Colour (0xffe5484d) : juce::Colour (0x44e5484d);

        g.setColour (c);
        g.fillEllipse (x, cy - 4.0f, 8.0f, 8.0f);
        x += 16.0f;
    }

    if ((fx & kFxMiniMeter) != 0)
    {
        drawLevelBar (g, { x, cy - 6.0f, 54.0f, 4.0f }, levelL, clipHoldL > 0, 1.0f);
        drawLevelBar (g, { x, cy + 2.0f, 54.0f, 4.0f }, levelR, clipHoldR > 0, 1.0f);
        x += 62.0f;
    }

    // Красная точка; при включённой пульсации размер и яркость следуют за громкостью
    float dotSize = 10.0f, dotAlpha = 1.0f;

    if ((fx & kFxPulse) != 0)
    {
        const float v = juce::jmin (1.0f, (levelL + levelR) * 0.5f);
        dotSize = 8.0f + v * 6.0f;
        dotAlpha = 0.45f + v * 0.55f;
    }

    g.setColour (juce::Colours::red.withAlpha (dotAlpha));
    g.fillEllipse (x + 5.0f - dotSize * 0.5f, cy - dotSize * 0.5f, dotSize, dotSize);

    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (juce::String::formatted ("REC %02d:%02d", secs / 60, secs % 60),
                rec.withTrimmedLeft ((int) (x - (float) rec.getX()) + 16), juce::Justification::centredLeft, false);
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
    const juce::Image& img = lens.kind == LensKind::plain ? currentFrame : lensImage;
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
        const float zoom = lens.zoom;
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

    // Кинокадр 2.39:1: чёрные полосы сверху и снизу (только в окне)
    if (haveVideo && lens.letterbox)
    {
        const int bar = (H - juce::roundToInt ((double) W / 2.39)) / 2;

        if (bar > 0)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (0, 0, W, bar);
            g.fillRect (0, H - bar, W, bar);
        }
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

    // Индикаторы звука гаснут вместе с панелью, когда курсор ушёл
    if (haveVideo && overlayAlpha > 0.01f && audioState != AudioState::off)
    {
        if ((proc.indicatorFlags & kFxWave) != 0)
            drawWave (g, overlayAlpha);

        if (isMeterPillShown())
            drawMeterPill (g, overlayAlpha);
    }

    // Индикатор записи виден всегда, чтобы не забыть про идущую запись
    if (recording)
        drawRecPill (g);
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
    items.push_back ({ &lookBox,      130,         false });
    items.push_back ({ &vignetteButton, 96,        false });
    items.push_back ({ &grainButton,  80,          false });
    items.push_back ({ &recordButton, 90,          false });
    items.push_back ({ &folderButton, 100,         false });
    items.push_back ({ &audioBox,     170,         false });

    if (source == 1) items.push_back ({ &micBox,       flexibleMin, true  });
    if (source != 0) items.push_back ({ &modeBox,      190,         false });
    if (source == 2) items.push_back ({ &offsetLabel,  92,          false });
    if (source == 2) items.push_back ({ &offsetSlider, 130,         false });

    items.push_back ({ &syncButton,   160,         false });
    items.push_back ({ &projectEdit,  flexibleMin, true  });
    items.push_back ({ &indicatorsButton, 110,     false });
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
