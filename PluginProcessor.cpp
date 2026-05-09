#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

PuponvstAudioProcessor::PuponvstAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
{
}

PuponvstAudioProcessor::~PuponvstAudioProcessor() 
{
    // 调试信息：析构函数开始执行
    DBG("PuponvstAudioProcessor destructor called");
    
    // 清理资源 - 不需要获取锁，因为对象即将被销毁
    oscilloscopeWritePos = 0;
    std::fill(oscilloscopeBuffer.begin(), oscilloscopeBuffer.end(), 0.0f);
    
    DBG("PuponvstAudioProcessor destructor completed successfully");
}

bool PuponvstAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
#else
    // 必须有输出
    if (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::disabled())
        return false;

#if ! JucePlugin_IsSynth
    // 作为效果器：输入/输出声道数必须一致（支持 mono/stereo）
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
#endif
}

void PuponvstAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);
    std::fill(oscilloscopeBuffer.begin(), oscilloscopeBuffer.end(), 0.0f);
    oscilloscopeWritePos = 0;

    // 初始化音频处理引擎
    const int numCh = juce::jmax(getTotalNumInputChannels(), getTotalNumOutputChannels());
    pitchEngine.prepare(sampleRate, samplesPerBlock, numCh);

    // 初始化老磁带跳调模块（先喂一个默认 wow 频率：120 BPM 4/4 → 一小节 = 2s → 0.5 Hz）
    tapeWobble.setWowFrequencyHz(1.0 / 2.0);
    tapeWobble.prepare(sampleRate, samplesPerBlock, numCh);

    // 颗音视觉状态复位
    vibDepthDisplay.store(0.0f, std::memory_order_relaxed);
    lastBarSeconds = 2.0;

    // 把 Rubber Band LiveShifter 的启动延迟上报给宿主 DAW，便于延迟补偿
    setLatencySamples(pitchEngine.getLatencySamples());
}

void PuponvstAudioProcessor::releaseResources()
{
    pitchEngine.reset();
    tapeWobble.reset();
}

void PuponvstAudioProcessor::pushSamplesToOscilloscope(const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);

    for (int i = 0; i < numSamples; ++i)
    {
        oscilloscopeBuffer[(size_t) oscilloscopeWritePos] = samples[i];
        oscilloscopeWritePos = (oscilloscopeWritePos + 1) % oscilloscopeBufferSize;
    }
}

void PuponvstAudioProcessor::getOscilloscopeSnapshot(juce::Array<float>& dest)
{
    dest.resize(oscilloscopeBufferSize);

    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);

    // 以 writePos 作为“最新数据之后的位置”，从旧到新拷贝
    for (int i = 0; i < oscilloscopeBufferSize; ++i)
    {
        const int idx = (oscilloscopeWritePos + i) % oscilloscopeBufferSize;
        dest.set(i, oscilloscopeBuffer[(size_t) idx]);
    }
}

void PuponvstAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels  = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // 清理多余输出通道（例如输入是mono而输出是stereo等情况）
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();
    juce::ignoreUnused(numSamples);

    // ===== 1. 计算本 block 的颗音深度 =====
    // 死区已取消：t 直接作为归一化深度。
    // 编辑器端已对视觉 t 做 gamma=2.5 非线性映射（前缓后陡），所以
    // 0~0.15 视觉位置对应到这里只剩极小的 audio t（≤ ~1%），听感上是"非常微弱的活气"。
    const float t = vibratoT.load(std::memory_order_relaxed);
    const float depthNorm = juce::jlimit(0.0f, 1.0f, t);

    // 推送深度给 TapeWobble（独立于 RubberBand 的老磁带跳调模块）
    tapeWobble.setDepth(depthNorm);

    // ===== 1.5 从宿主 PlayHead 拿一小节秒数 → 推送 wow 频率 =====
    // wow 主 LFO 周期 = 一小节（一小节完整波动一次回到起点），
    // 振幅会在 TapeWobble 内部按 1/f 反推，因此无论 BPM 多少，听感上
    // 最大跑调音程恒定在 ±2 半音 × depth。
    {
        double barSeconds = lastBarSeconds;
        if (auto* ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
            {
                const double bpm = pos->getBpm().orFallback(120.0);
                const auto   ts  = pos->getTimeSignature().orFallback(
                                       juce::AudioPlayHead::TimeSignature { 4, 4 });
                const int    num = juce::jmax(1, ts.numerator);
                const int    den = juce::jmax(1, ts.denominator);
                // 一拍秒数 = 60/bpm（按 4 分音符为一拍）；一小节 = num * (60/bpm) * (4/den)
                // 这样 6/8、3/4 等拍号也能正确处理
                barSeconds = (60.0 / juce::jmax(1.0, bpm))
                           * (double) num
                           * (4.0 / (double) den);
                lastBarSeconds = barSeconds;
            }
        }
        const double wowHz = 1.0 / juce::jmax(1.0e-3, barSeconds);
        tapeWobble.setWowFrequencyHz(wowHz);
    }

    // 同步视觉深度给 UI（相位由 TapeWobble 内部提供从而与听感同步）
    vibDepthDisplay.store(depthNorm, std::memory_order_relaxed);

    // ===== 2. 核心音频处理：5 路重采样 pitch shift + 混音 + 声相 + 硬削波 =====
    if (! bypassed)
    {
        pitchEngine.process(buffer);
        // ===== 3. 老磁带跳调（处理链末端，变长延迟线 + Hermite4 插值）=====
        // 即使深度 = 0 也会调用，以保证延迟线连续写入（避免从静默重启时出现口响）
        tapeWobble.process(buffer);
    }

    // 示例波形：抓取主输出的第0通道（已是处理后的信号）
    if (totalNumOutputChannels > 0)
        pushSamplesToOscilloscope(buffer.getReadPointer(0), buffer.getNumSamples());
}

juce::AudioProcessorEditor* PuponvstAudioProcessor::createEditor() { return new PuponvstAudioProcessorEditor(*this); }
bool PuponvstAudioProcessor::hasEditor() const { return true; }

const juce::String PuponvstAudioProcessor::getName() const { return "Puponvst"; }
bool PuponvstAudioProcessor::acceptsMidi() const { return false; }
bool PuponvstAudioProcessor::producesMidi() const { return false; }
bool PuponvstAudioProcessor::isMidiEffect() const { return false; }
double PuponvstAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int PuponvstAudioProcessor::getNumPrograms() { return 1; }
int PuponvstAudioProcessor::getCurrentProgram() { return 0; }
void PuponvstAudioProcessor::setCurrentProgram(int) {}
const juce::String PuponvstAudioProcessor::getProgramName(int) { return {}; }
void PuponvstAudioProcessor::changeProgramName(int, const juce::String&) {}

void PuponvstAudioProcessor::setEditorState(const EditorState& s)
{
    const juce::SpinLock::ScopedLockType sl(editorStateLock);
    editorState = s;
    editorState.hasValidValues = true;
}

PuponvstAudioProcessor::EditorState PuponvstAudioProcessor::getEditorState() const
{
    const juce::SpinLock::ScopedLockType sl(editorStateLock);
    return editorState;
}

// ===== 状态序列化（保存到宿主工程）=====
// 用 ValueTree + XML 文本序列化，向后兼容（未来加字段时旧版工程可读）。
// 内容包含：5 圆点高度、红蓝射线斜率、正态曲线方差、黄色激光归一化值。
void PuponvstAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    EditorState s = getEditorState();
    // 即使 Editor 从未打开过，我们也写入当前的默认值（hasValidValues = false 时使用结构体默认值）
    juce::ValueTree tree("PuponState");
    tree.setProperty("version",       1,                    nullptr);
    tree.setProperty("rayslopeK",     s.rayslopeK,          nullptr);
    tree.setProperty("isVerticalRay", s.isVerticalRay,      nullptr);
    tree.setProperty("sigma",         s.sigma,              nullptr);
    tree.setProperty("vibratoT",      s.vibratoTVisual,     nullptr);
    for (int i = 0; i < 5; ++i)
        tree.setProperty("dot" + juce::String(i), s.dotOffsetT[(size_t) i], nullptr);

    if (auto xml = tree.createXml())
        copyXmlToBinary(*xml, destData);
}

void PuponvstAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (! xml->hasTagName("PuponState"))
            return;

        auto tree = juce::ValueTree::fromXml(*xml);
        if (! tree.isValid())
            return;

        EditorState s;
        s.rayslopeK     = (float) tree.getProperty("rayslopeK",     s.rayslopeK);
        s.isVerticalRay = (bool)  tree.getProperty("isVerticalRay", s.isVerticalRay);
        s.sigma         = (float) tree.getProperty("sigma",         s.sigma);
        s.vibratoTVisual= (float) tree.getProperty("vibratoT",      s.vibratoTVisual);
        for (int i = 0; i < 5; ++i)
            s.dotOffsetT[(size_t) i] = (float) tree.getProperty("dot" + juce::String(i),
                                                                 s.dotOffsetT[(size_t) i]);
        s.hasValidValues = true;

        {
            const juce::SpinLock::ScopedLockType sl(editorStateLock);
            editorState = s;
        }

        // vibratoT 也直接同步给音频线程（视觉值 → gamma 映射后的 audio 值）
        // 与 Editor 的映射保持一致：pow(t, 2.5)
        const float tAudio = std::pow(juce::jlimit(0.0f, 1.0f, s.vibratoTVisual), 2.5f);
        setVibratoT(tAudio);
    }
}

// 插件入口实现
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PuponvstAudioProcessor();
}