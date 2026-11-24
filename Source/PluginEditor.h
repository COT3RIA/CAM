#pragma once

#include "PluginProcessor.h"
#include <juce_video/juce_video.h>

class WebcamAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::CameraDevice::Listener,
                                   private juce::Timer
{
public:
    explicit WebcamAudioProcessorEditor (WebcamAudioProcessor&);
    ~WebcamAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Полупрозрачная панель, которая лежит поверх видео
    struct OverlayPanel : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0x73000000));
            g.fillRoundedRectangle (b, 10.0f);
            g.setColour (juce::Colour (0x1fffffff));
            g.drawRoundedRectangle (b.reduced (0.5f), 10.0f, 1.0f);
        }
    };

    // CameraDevice::Listener (вызывается из потока камеры)
    void imageReceived (const juce::Image& image) override;

    // Timer (сообщение-поток)
    void timerCallback() override;

    void openCamera (int deviceIndex);
    void cameraOpened (std::unique_ptr<juce::CameraDevice> device, const juce::String& error);
    void closeCamera();
    void toggleRecording();
    void stopRecordingInternal();
    void refreshDisplay();
    void setStatus (const juce::String& text);

    void styleControls();
    void updateOverlay();
    void adaptToVideoAspect (int videoWidth, int videoHeight);
    juce::Rectangle<int> getStatusBounds() const;
    juce::Rectangle<int> getRecBounds() const;

    WebcamAudioProcessor& proc;

    std::unique_ptr<juce::CameraDevice> camera;
    int openGeneration = 0;

    juce::StringArray cameraNames;

    OverlayPanel topBar;
    juce::ComboBox cameraBox, lensBox;
    juce::ToggleButton mirrorButton { "Mirror" };
    juce::TextButton recordButton { "Record" };
    juce::TextButton folderButton { "Open folder" };

    juce::String statusText;
    float overlayAlpha = 1.0f;
    juce::uint32 showUntilMs = 0;

    juce::CriticalSection frameLock;
    juce::Image latestFrame;
    bool newFrame = false;

    juce::Image currentFrame;
    juce::Image fisheyeImage;

    bool gotFrame = false;
    bool noVideoWarned = false;
    juce::uint32 openedAtMs = 0;

    bool recording = false;
    juce::uint32 recordStartMs = 0;
    juce::File currentFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebcamAudioProcessorEditor)
};
