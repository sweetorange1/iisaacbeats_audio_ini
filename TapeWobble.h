#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <random>
#include <vector>

/**
 * TapeWobble
 * ----------
 * 模拟老磁带机马达运动不均匀（wow & flutter）的跑调效果。
 *
 * 算法：变长延迟线 + 多 LFO 调制读指针 + 4 点 Hermite 插值。
 *
 * 思路：
 *   - 维护一个固定容量的圆形延迟缓冲（容量 ≥ 最大可能延迟 + 一个 block）。
 *   - 写指针每 sample 前进 1。
 *   - 读指针 = 写指针 - delaySamples(t)，其中 delaySamples(t) 在中心值附近被
 *     多个低频信号（wow + flutter + random）调制。
 *   - 由于读指针速率随 delay 的导数变化：
 *         instRatio(t) = 1 - d(delaySamples)/dt / sampleRate
 *     当 delay 增大（读指针"变慢"）→ ratio < 1 → 降调；反之升调。
 *     物理上等价于"磁带机马达转速忽快忽慢"。
 *   - 读取处用 4 点 Hermite 插值（C1 连续），确保延迟变长/变短过程中信号
 *     完全平滑、无任何相位跳变 / STFT 帧切换 → 没有任何源头会产生电流声。
 *
 * 调制源：
 *   - wow（主体，频率由外部设置 ↔ 宿主一小节一周期）—— 模拟马达大周期不平衡
 *   - flutter（~6 Hz 固定）—— 模拟卷盘机械抖动（与 BPM 无关）
 *   - random（~0.8 Hz 截止 1 阶低通白噪声）—— 模拟磁带受潮 / 老化（与 BPM 无关）
 *
 * 振幅控制策略：
 *   听感上的"瞬时跑调音程"取决于 d(delay)/dt，而不是延迟本身。
 *   对于一个 LFO   delay = A * sin(2π f t)   →   d(delay)/dt 峰值 = A * 2π * f
 *   所以"峰值频率比变化" = A * 2π * f / sampleRate
 *
 *   我们希望 depth=1 时的最大瞬时跑调约 ±2 半音 (≈ ±12.25%)，因此：
 *       A_LFO = (depth * peakRatio_total * weight_LFO) / (2π * f_LFO) * sampleRate
 *
 *   → 当 wow 频率随 BPM 变化时，振幅会自动按 1/f 反比缩放，
 *     听感上的跑调音程恒定，不会因为 BPM 变慢而过头。
 *
 * 用户控制：
 *   - depth ∈ [0, 1]：整体跑调强度，0 = 完全旁通；1 = 满档 ±2 半音瞬时跑调
 *   - wowFrequencyHz：wow 主 LFO 的目标频率（"一小节一周期" → 1 / barSeconds）
 *
 * 线程安全：
 *   prepare/process 由 audio 线程调用；setDepth / setWowFrequencyHz
 *   可由任意线程调用（atomic）。
 */
class TapeWobble
{
public:
    TapeWobble();
    ~TapeWobble() = default;

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void process(juce::AudioBuffer<float>& buffer);

    // depth ∈ [0, 1]
    void setDepth(float d) noexcept
    {
        depth.store(juce::jlimit(0.0f, 1.0f, d), std::memory_order_relaxed);
    }

    // 设置 wow 主 LFO 目标频率（Hz）。典型用法：传入 1 / barSeconds（一小节一周期）。
    // 内部会做一阶平滑追踪，BPM 切换时不会有频率跳变 → 不会引入 click。
    // 取值约束：[kMinWowFreqHz, kMaxWowFreqHz]，超出会被裁剪。
    void setWowFrequencyHz(double hz) noexcept
    {
        const double clamped = juce::jlimit((double) kMinWowFreqHz, (double) kMaxWowFreqHz, hz);
        wowFreqTarget.store(clamped, std::memory_order_relaxed);
    }

    // 视觉同步：UI 用此值绘制黄线"波浪"形态（[0,1) 主 wow 相位）
    float getWowPhase01() const noexcept
    {
        return wowPhase01Display.load(std::memory_order_relaxed);
    }

    // === wow 频率合理范围 ===
    // 慢的下界：30 BPM 4/4 一小节 = 8s/cycle → 0.125 Hz
    // 快的上界：300 BPM 4/4 一小节 ≈ 0.8s/cycle → 1.25 Hz
    // 给一个稍宽松的范围保护
    static constexpr double kMinWowFreqHz = 0.10;   // 最慢 wow（约 40 BPM 4/4 时 1 小节）
    static constexpr double kMaxWowFreqHz = 2.00;   // 最快 wow（防止用户/host 给出过快值）

private:
    double sampleRate = 44100.0;
    int    numChannelsPrepared = 2;

    // 延迟缓冲：容量 = 最大可能延迟 + 安全 padding
    int                                   bufferSize   = 0;
    int                                   bufferMask   = 0;     // bufferSize 为 2 的幂时使用
    std::array<std::vector<float>, 2>     delayBuf;             // L/R
    int                                   writePos     = 0;

    // 中心延迟（samples）—— 让瞬时延迟可以在 ±maxModSamples 内自由摆动而不溢出
    // 按"最慢 wow（kMinWowFreqHz）+ 满档深度"的最坏情况预留
    double baseDelaySamples = 0.0;
    double maxModSamples    = 0.0;

    // === LFO 状态 ===
    // 1) wow（主体）—— 频率可被外部设置，内部平滑追踪
    double wowPhase = 0.0;       // 周期数，浮点累加
    double wowFreq  = 0.5;       // 当前实际使用的 Hz（每 sample 朝 wowFreqTarget 平滑追踪）
    std::atomic<double> wowFreqTarget { 0.5 };  // 外部目标 Hz

    // 2) random：1 阶低通白噪声做"慢随机走步"
    double randomState  = 0.0;
    static constexpr double kRandomCutoffHz = 0.8;

    // 3) flutter：固定频率，模拟卷盘机械抖动
    double flutterPhase = 0.0;
    static constexpr double kFlutterFreqHz = 6.2;

    // === 振幅权重（depth=1 时三者瞬时频率比加和的最大值 = kPeakRatioAtFullDepth）===
    // 主导是 wow，random 制造老化感，flutter 给"机械味"
    static constexpr float  kPeakRatioAtFullDepth = 0.122f;  // ≈ 2^(2/12)-1，≈ ±2 半音
    static constexpr float  kWowWeight     = 0.65f;
    static constexpr float  kRandomWeight  = 0.25f;
    static constexpr float  kFlutterWeight = 0.10f;

    // 用户深度（来自 UI 的目标值，可能在 block 间阶跃变化）
    std::atomic<float> depth { 0.0f };

    // depth 平滑值（音频线程内独占，sample 级一阶低通追踪 depth）
    // 拖动黄线时即使 UI 端 depth 跳变，这里也会以毫秒级时间常数平滑过渡，
    // 从根源消除"参数阶跃 → 振幅阶跃 → 延迟读位置阶跃 → 咔哒声"的传导链。
    double depthSmoothed = 0.0;
    // 平滑系数（在 prepare 中根据 sampleRate 计算，对应 ~30ms 时间常数）
    double depthSmoothCoeff = 0.0;

    // 视觉同步：每 block 起点的 wow 相位 [0,1)
    std::atomic<float> wowPhase01Display { 0.0f };

    // 噪声源（音频线程独占；构造时种好种子）
    std::mt19937 rng;
    std::uniform_real_distribution<double> uni { -1.0, 1.0 };

    // 频率平滑系数（一阶低通）：对应 ~50ms 时间常数
    // 在 prepare 中根据 sampleRate 计算
    double freqSmoothCoeff = 0.0;

    // 内部：4 点 Hermite 插值取 buf 在"读位置 = writePos - delay"处的样本
    static inline float hermite4(float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c0 = x0;
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }
};
