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

void WebcamAudioProcessor::prepareToPlay (double sampleRate, int)
{
    hostCapture.setSampleRate (sampleRate);
}
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

    // Транспорт DAW: идёт ли воспроизведение и какой темп
    if (auto* transport = getPlayHead())
    {
        if (auto position = transport->getPosition())
        {
            hostPlaying.store (position->getIsPlaying());

            if (auto bpm = position->getBpm())
                hostBpm.store (*bpm);
        }
    }

    // Уровень входного звука для индикаторов (всегда, не только при записи)
    {
        const int n = buffer.getNumSamples();
        const int chs = juce::jmin (2, buffer.getNumChannels());

        if (n > 0 && chs > 0)
        {
            const float pl = buffer.getMagnitude (0, 0, n);
            const float pr = chs > 1 ? buffer.getMagnitude (1, 0, n) : pl;

            if (pl > hostPeakL.load (std::memory_order_relaxed)) hostPeakL.store (pl, std::memory_order_relaxed);
            if (pr > hostPeakR.load (std::memory_order_relaxed)) hostPeakR.store (pr, std::memory_order_relaxed);

            hostBlocks.fetch_add (1, std::memory_order_relaxed);
        }
    }

    // Копия входного звука для записи (работает только во время записи)
    hostCapture.push (buffer);

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
    xml.setAttribute ("look", lookIndex);
    xml.setAttribute ("vignette", vignette);
    xml.setAttribute ("grain", grain);
    xml.setAttribute ("lang", language);
    xml.setAttribute ("audioSource", audioSource);
    xml.setAttribute ("audioMode", audioMode);
    xml.setAttribute ("mic", micName);
    xml.setAttribute ("offsetMs", audioOffsetMs);
    xml.setAttribute ("sync", syncTransport);
    xml.setAttribute ("project", projectName);
    xml.setAttribute ("indicators", indicatorFlags);
    copyXmlToBinary (xml, destData);
}

void WebcamAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName ("WebcamViewerState"))
        {
            cameraName  = xml->getStringAttribute ("camera");
            lensIndex   = juce::jlimit (0, 99, xml->getIntAttribute ("lens", 1));
            mirror      = xml->getBoolAttribute ("mirror", false);
            lookIndex   = juce::jlimit (0, 6, xml->getIntAttribute ("look", 0));
            vignette    = xml->getBoolAttribute ("vignette", false);
            grain       = xml->getBoolAttribute ("grain", false);

            // Старые проекты без этих полей оставляют значения по умолчанию
            language    = juce::jlimit (0, 1, xml->getIntAttribute ("lang", language));
            // Раньше был флажок recordAudio (только микрофон): старые проекты читаем как "микрофон"
            const int legacySource = xml->getBoolAttribute ("recordAudio", false) ? 1 : 0;
            audioSource = juce::jlimit (0, 2, xml->getIntAttribute ("audioSource", legacySource));
            audioMode   = juce::jlimit (0, 1, xml->getIntAttribute ("audioMode", 0));
            micName     = xml->getStringAttribute ("mic");
            audioOffsetMs = juce::jlimit (-2000, 2000, xml->getIntAttribute ("offsetMs", 0));
            syncTransport = xml->getBoolAttribute ("sync", false);
            projectName = xml->getStringAttribute ("project");
            indicatorFlags = juce::jlimit (0, 31, xml->getIntAttribute ("indicators", 1));
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new WebcamAudioProcessor();
}
