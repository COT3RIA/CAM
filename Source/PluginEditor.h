#pragma once

#include "PluginProcessor.h"
#include "Recorder.h"
#include "Strings.h"
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

    // Как именно идёт текущая запись
    enum class RecordKind
    {
        none,
        nativeVideo,        // видео пишет сама камера (WMV/MOV), без звука
        nativeVideoAndWav,  // видео пишет камера, звук отдельным WAV-файлом
        avi                 // один AVI-файл: видео и звук вместе
    };

    // CameraDevice::Listener (вызывается из потока камеры)
    void imageReceived (const juce::Image& image) override;

    // Timer (сообщение-поток)
    void timerCallback() override;

    void openCamera (int deviceIndex);
    void cameraOpened (std::unique_ptr<juce::CameraDevice> device, const juce::String& error);
    void closeCamera();
    void toggleRecording();
    void startRecording (bool byTransport = false);
    void stopRecordingInternal();
    void refreshDisplay();
    void pollTransport();
    AudioSettings makeAudioSettings() const;
    juce::String resolveProjectName() const;
    juce::String makeUniqueBaseName (const juce::File& folder) const;

    void setStatus (loc::Text id, const juce::String& arg = {});
    juce::String getStatusText() const;
    juce::String tr (loc::Text text) const { return loc::get (proc.language, text); }

    void styleControls();
    void populateMicBox();
    void applyLanguage();
    void updateControlsEnabled();
    juce::String getEffectiveMicName() const;

    void updateOverlay();
    void adaptToVideoAspect (int videoWidth, int videoHeight);
    juce::Rectangle<int> getStatusBounds() const;
    juce::Rectangle<int> getRecBounds() const;

    WebcamAudioProcessor& proc;

    std::unique_ptr<juce::CameraDevice> camera;
    int openGeneration = 0;

    juce::StringArray cameraNames, micNames;

    OverlayPanel topBar;
    juce::ComboBox cameraBox, lensBox, audioBox, micBox, modeBox;
    juce::ToggleButton mirrorButton, syncButton;
    juce::TextButton recordButton, folderButton, langButton;
    juce::Label offsetLabel;
    juce::Slider offsetSlider;
    juce::TextEditor projectEdit;

    loc::Text statusId = loc::Text::none;
    juce::String statusArg;
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
    bool recordedByTransport = false;
    bool prevHostPlaying = false;
    bool hostWarned = false;
    RecordKind recordKind = RecordKind::none;
    juce::uint32 recordStartMs = 0;
    juce::String recordingInfo;
    std::unique_ptr<AvRecorder> avRecorder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebcamAudioProcessorEditor)
};
