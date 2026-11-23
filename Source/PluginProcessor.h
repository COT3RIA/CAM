#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Аудио-часть плагина: просто пропускает звук без изменений.
// Вся "полезная" работа (камера, линзы, запись) живёт в редакторе (окне плагина).
class WebcamAudioProcessor : public juce::AudioProcessor
{
public:
    WebcamAudioProcessor();
    ~WebcamAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Webcam Viewer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Настройки, которые сохраняются вместе с проектом DAW
    juce::String cameraName;
    int lensIndex = 1;
    bool mirror = false;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebcamAudioProcessor)
};
