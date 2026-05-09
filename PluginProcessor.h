#pragma once

#include <JuceHeader.h>
#include <array>
#include "PitchShiftEngine.h"
#include "TapeWobble.h"

class PuponvstAudioProcessor : public juce::AudioProcessor
{
public:
    PuponvstAudioProcessor();
    ~PuponvstAudioProcessor() override;

    // AudioProcessor overrides
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ===== 持久化的 UI 控制状态（由 Editor 拥有的"模型层"数值在此存档） =====
    // 设计：Editor 是临时组件（用户随时关闭/重开），状态必须存放在 Processor。
    //   - 每次用户交互后 Editor 主动调用 setEditorState(...) 推送一份镜像
    //   - setStateInformation 反序列化时填充这些字段
    //   - 新 Editor 构造时调用 getEditorState(...) 回填到自己的成员
    struct EditorState
    {
        // 红线斜率（数学坐标系，Y 向上）；蓝线斜率为其相反数
        float rayslopeK    = 0.0f;
        bool  isVerticalRay = false;
        // 正态曲线方差（标准差）
        float sigma        = 1.0f;
        // 5 个圆点在轨道上的归一化高度，1.0 = 顶端（默认最大），0.0 = 底部
        std::array<float, 5> dotOffsetT { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        // 黄色激光的归一化高度 [0,1]
        float vibratoTVisual = 0.05f;
        // 是否包含已保存的有效内容（Processor 刚被构造、尚未恢复时为 false）
        bool  hasValidValues = false;
    };

    void setEditorState(const EditorState& s);
    EditorState getEditorState() const;

    // 提供给界面线程使用：获取示波器数据快照（按时间从旧到新排列）
    void getOscilloscopeSnapshot(juce::Array<float>& dest);

    // 提供给界面线程：把 5 个圆点的当前 gain / pan 同步给音频线程（lock-free）
    // index ∈ [0, 4]，gain ∈ [0, 1]，pan ∈ [-1, +1]
    void setDotGain(int index, float gain) { pitchEngine.setDotGain(index, gain); }
    void setDotPan (int index, float pan ) { pitchEngine.setDotPan (index, pan ); }

    // ===== 颗音控制（黄色横线控制器） =====
    // vibratoT ∈ [0, 1]：UI 黄色横线的归一化高度
    //   t ∈ [0,    0.15] → 死区（深度 = 0，不跳调）
    //   t ∈ [0.15, 1.00] → 线性映射到深度 d ∈ (0, 1]
    // 深度 d 传递给独立的 TapeWobble 模块（老磁带 wow & flutter）。
    void  setVibratoT(float t) noexcept { vibratoT.store(juce::jlimit(0.0f, 1.0f, t), std::memory_order_relaxed); }
    float getVibratoT() const noexcept  { return vibratoT.load(std::memory_order_relaxed); }

    // ===== 视觉同步：UI 通过这两个 getter 拿到当前颗音相位 / 实际深度（半音单位） =====
    // 返回 [0, 1) 范围的相位（0 = 起点，0.5 = 半周期）；用于 UI 的正弦波绘制与"弱光爬动"
    // 相位源 = TapeWobble 中主 wow LFO 的相位（与听感上的跳调周期完全同步）
    float getVibPhase01() const noexcept    { return tapeWobble.getWowPhase01(); }
    // 返回当前归一化深度 [0, 1]（已含死区映射），仅用于视觉
    float getVibDepthNorm() const noexcept  { return vibDepthDisplay.load(std::memory_order_relaxed); }

    bool bypassed = false;

private:
    static constexpr int oscilloscopeBufferSize = 2048;

    void pushSamplesToOscilloscope(const float* samples, int numSamples);

    juce::SpinLock oscilloscopeLock;
    std::array<float, oscilloscopeBufferSize> oscilloscopeBuffer {};
    int oscilloscopeWritePos = 0;

    // 音频处理引擎（重采样 pitch shift + 5 路混音 + 声相 + 硬削波）
    PitchShiftEngine pitchEngine;

    // 老磁带跳调处理（独立于 RubberBand，变长延迟线 + Hermite4 插值实现）
    // 主要处于 pitchEngine 之后作为最后一级，完全避开 STFT/帧切换，从根本上消除毛刺电流声
    TapeWobble tapeWobble;

    // === 颗音参数 / 状态 ===
    // 死区阈值：黄线 t < 此值时不波动
    static constexpr float kVibDeadZone = 0.15f;

    std::atomic<float> vibratoT { 0.15f };          // 来自 UI（死区刚好处于边界）
    std::atomic<float> vibDepthDisplay   { 0.0f };  // [0,1] 归一化深度，仅 UI 视觉用

    // 缓存上次读到的"一小节秒数"，用于在没有 PlayHead 时回退（默认 120 BPM 4/4 = 2s）
    double lastBarSeconds = 2.0;

    // ===== 持久化的 UI 状态镜像（GUI 关闭时此处保留最新值，宿主存档读取于此）=====
    // 由 Editor 在每次交互后通过 setEditorState() 推送；Editor 重新打开时通过 getEditorState() 拉取。
    // 用 SpinLock 保护：写入只发生在消息线程（Editor 交互、setStateInformation），读取也只发生在消息线程
    // （新 Editor 构造、getStateInformation 由 host 在主线程调用），SpinLock 足够。
    mutable juce::SpinLock editorStateLock;
    EditorState            editorState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PuponvstAudioProcessor)
};
