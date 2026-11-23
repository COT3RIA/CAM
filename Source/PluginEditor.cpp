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
    addAndMakeVisible (cameraBox);
    addAndMakeVisible (lensBox);
    addAndMakeVisible (mirrorButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (folderButton);
    addAndMakeVisible (statusLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

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
        repaint (videoArea);
    };

    recordButton.onClick = [this] { toggleRecording(); };

    folderButton.onClick = []
    {
        auto folder = getRecordingsFolder();
        folder.createDirectory();
        folder.startAsProcess();
    };

    setResizeLimits (560, 340, 1920, 1200);
    setSize (760, 520);

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
void WebcamAudioProcessorEditor::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

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
            repaint (videoArea); // обновить таймер записи
    }
}

void WebcamAudioProcessorEditor::refreshDisplay()
{
    if (getLens (proc.lensIndex).fisheye && currentFrame.isValid())
        fisheyeImage = makeFisheye (currentFrame);

    repaint (videoArea);
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
    cameraBox.setEnabled (true);
}

//==============================================================================
void WebcamAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1b1b));
    g.setColour (juce::Colours::black);
    g.fillRect (videoArea);

    const auto& lens = getLens (proc.lensIndex);
    const juce::Image& img = lens.fisheye ? fisheyeImage : currentFrame;

    if (! img.isValid())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (16.0f);
        g.drawText (cameraNames.isEmpty() ? "No cameras found" : "Waiting for video...",
                    videoArea, juce::Justification::centred);
        return;
    }

    {
        juce::Graphics::ScopedSaveState state (g);

        const float zoom = lens.fisheye ? 1.0f : lens.zoom;
        const int imgW = img.getWidth();
        const int imgH = img.getHeight();
        const int cropW = juce::jmax (2, (int) ((float) imgW / zoom));
        const int cropH = juce::jmax (2, (int) ((float) imgH / zoom));
        const int cropX = (imgW - cropW) / 2;
        const int cropY = (imgH - cropH) / 2;

        const auto dest = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                              .appliedTo (juce::Rectangle<float> ((float) cropW, (float) cropH),
                                          videoArea.toFloat());

        g.reduceClipRegion (videoArea);

        if (proc.mirror)
            g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f, dest.getCentreX(), dest.getCentreY()));

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const auto d = dest.toNearestInt();
        g.drawImage (img, d.getX(), d.getY(), d.getWidth(), d.getHeight(),
                     cropX, cropY, cropW, cropH);
    }

    // Оверлей (не зеркалится)
    g.setFont (14.0f);
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.drawText (lens.name, videoArea.reduced (8), juce::Justification::topRight);

    if (recording)
    {
        const auto secs = (int) ((juce::Time::getMillisecondCounter() - recordStartMs) / 1000);
        g.setColour (juce::Colours::red);
        g.fillEllipse ((float) videoArea.getX() + 10.0f, (float) videoArea.getY() + 10.0f, 12.0f, 12.0f);
        g.setColour (juce::Colours::white);
        g.drawText (juce::String::formatted ("REC %02d:%02d", secs / 60, secs % 60),
                    videoArea.getX() + 28, videoArea.getY() + 6, 120, 20,
                    juce::Justification::centredLeft);
    }
}

void WebcamAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (8);

    auto top = r.removeFromTop (28);
    folderButton.setBounds (top.removeFromRight (110));
    top.removeFromRight (6);
    recordButton.setBounds (top.removeFromRight (90));
    top.removeFromRight (10);
    mirrorButton.setBounds (top.removeFromRight (80));
    top.removeFromRight (6);
    lensBox.setBounds (top.removeFromRight (150));
    top.removeFromRight (6);
    cameraBox.setBounds (top);

    r.removeFromTop (6);
    statusLabel.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (4);

    videoArea = r;
}
