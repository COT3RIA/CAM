#include "PluginProcessor.h"
#include "PluginEditor.h"

WebcamAudioProcessor::WebcamAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // Язык по умолчанию: русский, если система русская, иначе английский
    language = juce::SystemStats::getUserLanguage().startsWithIgnoreCase ("ru") ? 1 : 0;
}

void WebcamAudioProcessor::prepareToPlay (double, int) {}
void WebcamAudioProcessor::releaseResources() {}

bool WebcamAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void WebcamAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Звук проходит насквозь без изменений. Лишние выходные каналы чистим.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* WebcamAudioProcessor::createEditor()
{
    return new WebcamAudioProcessorEditor (*this);
}

void WebcamAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("WebcamViewerState");
    xml.setAttribute ("camera", cameraName);
    xml.setAttribute ("lens", lensIndex);
    xml.setAttribute ("mirror", mirror);
    xml.setAttribute ("lang", language);
    xml.setAttribute ("recordAudio", recordAudio);
    xml.setAttribute ("audioMode", audioMode);
    xml.setAttribute ("mic", micName);
    copyXmlToBinary (xml, destData);
}

void WebcamAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName ("WebcamViewerState"))
        {
            cameraName  = xml->getStringAttribute ("camera");
            lensIndex   = xml->getIntAttribute ("lens", 1);
            mirror      = xml->getBoolAttribute ("mirror", false);

            // Старые проекты без этих полей оставляют значения по умолчанию
            language    = juce::jlimit (0, 1, xml->getIntAttribute ("lang", language));
            recordAudio = xml->getBoolAttribute ("recordAudio", false);
            audioMode   = juce::jlimit (0, 1, xml->getIntAttribute ("audioMode", 0));
            micName     = xml->getStringAttribute ("mic");
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WebcamAudioProcessor();
}
