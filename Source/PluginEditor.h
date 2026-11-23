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

    WebcamAudioProcessor& proc;

    std::unique_ptr<juce::CameraDevice> camera;
    int openGeneration = 0;

    juce::StringArray cameraNames;
    juce::ComboBox cameraBox, lensBox;
    juce::ToggleButton mirrorButton { "Mirror" };
    juce::TextButton recordButton { "Record" };
    juce::TextButton folderButton { "Open folder" };
    juce::Label statusLabel;

    juce::CriticalSection frameLock;
    juce::Image latestFrame;
    bool newFrame = false;

    juce::Image currentFrame;
    juce::Image fisheyeImage;
    juce::Rectangle<int> videoArea;

    bool gotFrame = false;
    bool noVideoWarned = false;
    juce::uint32 openedAtMs = 0;

    bool recording = false;
    juce::uint32 recordStartMs = 0;
    juce::File currentFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebcamAudioProcessorEditor)
};
