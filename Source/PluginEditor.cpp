#include "PluginEditor.h"

namespace
{
    // Веб-камера не умеет менять оптику, поэтому "объективы" эмулируются программно:
    // зум = цифровой кроп центра кадра, Fisheye = бочкообразное искажение.
    struct Lens
    {
        const char* name;
        float zoom;
        bool fisheye;
    };

    const Lens kLenses[] =
    {
        { "13mm (full frame)", 1.0f, false },
        { "24mm",              1.5f, false },
        { "48mm",              2.5f, false },
        { "77mm",              4.0f, false },
        { "Fisheye",           1.0f, true  },
    };

    constexpr int kNumLenses = (int) (sizeof (kLenses) / sizeof (kLenses[0]));

    // Размеры окна: 16:9 по умолчанию, соотношение потом подстраивается под камеру
    constexpr int kMinW = 560, kMinH = 315, kMaxW = 1920, kMaxH = 1080;
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

    styleControls();

    cameraNames = juce::CameraDevice::getAvailableDevices();

    if (cameraNames.isEmpty())
    {
        cameraBox.setTextWhenNothingSelected ("No cameras found");
        setStatus ("No cameras found");
    }
    else
    {
        cameraBox.addItemList (cameraNames, 1);
        int idx = cameraNames.indexOf (proc.cameraName);
        if (idx < 0) idx = 0;
        cameraBox.setSelectedItemIndex (idx, juce::dontSendNotification);
    }

    for (int i = 0; i < kNumLenses; ++i)
        lensBox.addItem (kLenses[i].name, i + 1);

    lensBox.setSelectedItemIndex (juce::jlimit (0, kNumLenses - 1, proc.lensIndex),
                                  juce::dontSendNotification);

    mirrorButton.setToggleState (proc.mirror, juce::dontSendNotification);

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
    for (auto* box : { &cameraBox, &lensBox })
    {
        box->setColour (juce::ComboBox::backgroundColourId, kControlBg);
        box->setColour (juce::ComboBox::outlineColourId, kControlLine);
        box->setColour (juce::ComboBox::textColourId, juce::Colours::white);
        box->setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
    }

    for (auto* button : { &recordButton, &folderButton })
    {
        button->setColour (juce::TextButton::buttonColourId, kControlBg);
        button->setColour (juce::TextButton::buttonOnColourId, kControlBg);
        button->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    mirrorButton.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    mirrorButton.setColour (juce::ToggleButton::tickColourId, juce::Colours::white);
    mirrorButton.setColour (juce::ToggleButton::tickDisabledColourId, juce::Colours::grey);
}

void WebcamAudioProcessorEditor::setStatus (const juce::String& text)
{
    statusText = text;
    repaint();
}

//==============================================================================
// Панель и строка статуса видны, пока курсор над плагином (или открыт выпадающий список),
// и ещё полсекунды после этого. Потом плавно гаснут.
void WebcamAudioProcessorEditor::updateOverlay()
{
    const auto now = juce::Time::getMillisecondCounter();

    const bool hovered = isMouseOver (true) || cameraBox.isPopupActive() || lensBox.isPopupActive();

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

    setStatus ("Opening camera...");
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
        setStatus ("Camera error: " + (error.isNotEmpty() ? error : juce::String ("could not open the camera")));
        return;
    }

    camera = std::move (device);
    camera->addListener (this);
    openedAtMs = juce::Time::getMillisecondCounter();
    setStatus ("Camera opened, waiting for video...");
}

void WebcamAudioProcessorEditor::closeCamera()
{
    ++openGeneration;

    if (camera != nullptr)
    {
        if (recording)
            stopRecordingInternal();

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
            setStatus (juce::String ("Live: ") + juce::String (frame.getWidth()) + "x" + juce::String (frame.getHeight()));
            adaptToVideoAspect (frame.getWidth(), frame.getHeight());
        }

        currentFrame = frame;
        refreshDisplay();
    }
    else
    {
        if (camera != nullptr && ! gotFrame && ! noVideoWarned
            && juce::Time::getMillisecondCounter() - openedAtMs > 4000)
        {
            noVideoWarned = true;
            setStatus ("No video received. Check camera permission (host / OS) and that no other app is using the camera.");
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
    if (camera == nullptr)
    {
        setStatus ("Can't record: camera is not open");
        return;
    }

    if (! recording)
    {
        auto folder = getRecordingsFolder();
        folder.createDirectory();

       #if JUCE_WINDOWS
        const juce::String ext = ".wmv";
       #else
        const juce::String ext = ".mov";
       #endif

        currentFile = folder.getChildFile ("webcam_"
                                           + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S")
                                           + ext);

        camera->startRecordingToFile (currentFile, 2);
        recording = true;
        recordStartMs = juce::Time::getMillisecondCounter();
        recordButton.setButtonText ("Stop");
        recordButton.setColour (juce::TextButton::buttonColourId, kRecordingBg);
        recordButton.setColour (juce::TextButton::buttonOnColourId, kRecordingBg);
        cameraBox.setEnabled (false);
        setStatus ("Recording to: " + currentFile.getFullPathName());
    }
    else
    {
        stopRecordingInternal();
        setStatus ("Saved: " + currentFile.getFullPathName());
    }
}

void WebcamAudioProcessorEditor::stopRecordingInternal()
{
    if (camera != nullptr)
        camera->stopRecording();

    recording = false;
    recordButton.setButtonText ("Record");
    recordButton.setColour (juce::TextButton::buttonColourId, kControlBg);
    recordButton.setColour (juce::TextButton::buttonOnColourId, kControlBg);
    cameraBox.setEnabled (true);
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

    if (! haveVideo)
    {
        // Пока видео нет, сообщение (в том числе об ошибке) видно без наведения курсора
        g.setColour (juce::Colours::grey);
        g.setFont (16.0f);
        g.drawFittedText (statusText.isNotEmpty() ? statusText : juce::String ("Waiting for video..."),
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
    topBar.setBounds (8, 8, juce::jmax (0, getWidth() - 16), 44);

    auto r = topBar.getLocalBounds().reduced (8, 8);

    folderButton.setBounds (r.removeFromRight (100));
    r.removeFromRight (6);
    recordButton.setBounds (r.removeFromRight (80));
    r.removeFromRight (8);
    mirrorButton.setBounds (r.removeFromRight (74));
    r.removeFromRight (6);
    lensBox.setBounds (r.removeFromRight (140));
    r.removeFromRight (6);
    cameraBox.setBounds (r);

    repaint();
}
