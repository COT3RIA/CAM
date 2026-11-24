#include "PluginEditor.h"

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
    constexpr int kMinW = 640, kMinH = 360, kMaxW = 1920, kMaxH = 1080;
    constexpr int kStartW = 800, kStartH = 450;

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
                   .getChildFile ("WebcamViewer");
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
    topBar.addAndMakeVisible (audioButton);
    topBar.addAndMakeVisible (micBox);
    topBar.addAndMakeVisible (modeBox);
    topBar.addAndMakeVisible (langButton);

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
    audioButton.setToggleState (proc.recordAudio, juce::dontSendNotification);

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

    audioButton.onClick = [this]
    {
        proc.recordAudio = audioButton.getToggleState();
        updateControlsEnabled();
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
    for (auto* box : { &cameraBox, &lensBox, &micBox, &modeBox })
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

    for (auto* toggle : { &mirrorButton, &audioButton })
    {
        toggle->setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        toggle->setColour (juce::ToggleButton::tickColourId, juce::Colours::white);
        toggle->setColour (juce::ToggleButton::tickDisabledColourId, juce::Colours::grey);
    }
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
    audioButton.setButtonText (tr (loc::Text::recordAudio));
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

    cameraBox.setEnabled (idle);
    audioButton.setEnabled (idle);
    micBox.setEnabled (idle && proc.recordAudio);
    modeBox.setEnabled (idle && proc.recordAudio);
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
                          || micBox.isPopupActive() || modeBox.isPopupActive();

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
        startRecording();
    }
    else
    {
        stopRecordingInternal();
        setStatus (loc::Text::saved, recordingInfo);
    }
}

void WebcamAudioProcessorEditor::startRecording()
{
    if (camera == nullptr)
    {
        setStatus (loc::Text::cantRecordNoCamera);
        return;
    }

    auto folder = getRecordingsFolder();
    folder.createDirectory();

    const juce::String baseName = "webcam_" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");

   #if JUCE_WINDOWS
    const juce::String videoExt = ".wmv";
   #else
    const juce::String videoExt = ".mov";
   #endif

    const auto videoFile = folder.getChildFile (baseName + videoExt);

    if (! proc.recordAudio)
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
                                              currentFrame.getWidth(), currentFrame.getHeight(),
                                              getEffectiveMicName());

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
        // Звук отдельным WAV-файлом, видео пишет камера. Микрофон открывается первым (это долго),
        // но в файл звук идёт только с момента запуска записи видео.
        const auto wavFile = folder.getChildFile (baseName + ".wav");

        avRecorder = std::make_unique<AvRecorder>();

        const auto error = avRecorder->start (AvRecorder::Mode::wavOnly, wavFile, 0, 0, getEffectiveMicName());

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
    // Панель с настройками лежит поверх видео, видео занимает всё окно
    topBar.setBounds (8, 8, juce::jmax (0, getWidth() - 16), 78);

    auto r = topBar.getLocalBounds().reduced (8, 8);
    auto row1 = r.removeFromTop (28);
    r.removeFromTop (6);
    auto row2 = r.removeFromTop (28);

    // Первая строка: камера, объектив, зеркало, запись, папка
    folderButton.setBounds (row1.removeFromRight (100));
    row1.removeFromRight (6);
    recordButton.setBounds (row1.removeFromRight (90));
    row1.removeFromRight (8);
    mirrorButton.setBounds (row1.removeFromRight (96));
    row1.removeFromRight (6);
    lensBox.setBounds (row1.removeFromRight (150));
    row1.removeFromRight (6);
    cameraBox.setBounds (row1);

    // Вторая строка: звук, микрофон, режим записи звука, язык
    langButton.setBounds (row2.removeFromRight (130));
    row2.removeFromRight (6);
    modeBox.setBounds (row2.removeFromRight (190));
    row2.removeFromRight (6);
    audioButton.setBounds (row2.removeFromLeft (120));
    row2.removeFromLeft (6);
    micBox.setBounds (row2);

    repaint();
}
