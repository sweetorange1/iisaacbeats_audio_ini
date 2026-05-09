#include "TapeWobble.h"
#include <cmath>

TapeWobble::TapeWobble()
    : rng(0xC0FFEE)
{
}

void TapeWobble::prepare(double newSampleRate, int /*maxBlockSize*/, int numChannels)
{
    sampleRate          = juce::jmax(8000.0, newSampleRate);
    numChannelsPrepared = juce::jlimit(1, 2, numChannels);

    // ---- 延迟缓冲容量：按"最慢 wow + 满档深度"的最坏情况预留 ----
    //
    // 振幅 A_wow = (peakRatio * weight_wow) / (2π * f_wow) * sampleRate
    // 在 f_wow = kMinWowFreqHz 时取最大值。
    //
    // 同时 random 等效频率近似 = kRandomCutoffHz（其调制速率上限），
    // flutter 在 kFlutterFreqHz —— 它们各自的 A 也按相同公式给出。
    // 三者振幅相加给出 maxModSamples（用最坏情况叠加，作保守上限）。
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;

    auto ampForLFO = [&](double weight, double freqHz)
    {
        // 振幅（samples）= (peakRatio * weight) / (2π * f) * sampleRate
        return ((double) kPeakRatioAtFullDepth * weight)
             / (twoPi * juce::jmax(1.0e-3, freqHz))
             * sampleRate;
    };

    const double aWowMax  = ampForLFO((double) kWowWeight,     (double) kMinWowFreqHz);
    const double aRandMax = ampForLFO((double) kRandomWeight,  (double) kRandomCutoffHz);
    const double aFlutMax = ampForLFO((double) kFlutterWeight, (double) kFlutterFreqHz);

    // 最坏情况：三者相位重合、同时拉到极值
    maxModSamples    = aWowMax + aRandMax + aFlutMax;
    // 中心延迟略大于最大调制幅度，给 hermite 4 点边界留 4 sample 头室
    baseDelaySamples = maxModSamples + 4.0;

    // 缓冲容量：base + max + hermite 边界 padding；取 2 的幂便于 mask 取模
    int needed = (int) std::ceil(baseDelaySamples + maxModSamples) + 32;
    int p2 = 1;
    while (p2 < needed) p2 <<= 1;
    bufferSize = p2;
    bufferMask = p2 - 1;

    for (int ch = 0; ch < 2; ++ch)
        delayBuf[(size_t) ch].assign((size_t) bufferSize, 0.0f);

    writePos = 0;

    // LFO 复位
    wowPhase     = 0.0;
    flutterPhase = 0.0;
    randomState  = 0.0;

    // 频率平滑：一阶低通追踪 target，时间常数 τ ≈ 50ms
    // α = 1 - exp(-1 / (τ * fs))；这里换算成"每 sample"
    const double tauSec = 0.050;
    freqSmoothCoeff = 1.0 - std::exp(-1.0 / (tauSec * sampleRate));

    // depth 平滑：时间常数 τ ≈ 30ms。拖动黄线该时间内走完从 0→1，
    // 在人耳听起来是平滑渐变而不是"哔"一下；同时足够快不影响手感。
    const double tauDepthSec = 0.030;
    depthSmoothCoeff = 1.0 - std::exp(-1.0 / (tauDepthSec * sampleRate));
    depthSmoothed = (double) depth.load(std::memory_order_relaxed);

    // 当前 wowFreq 直接取 target，避免启动时从 0 慢慢拉起来
    wowFreq = wowFreqTarget.load(std::memory_order_relaxed);
}

void TapeWobble::reset()
{
    for (int ch = 0; ch < 2; ++ch)
        std::fill(delayBuf[(size_t) ch].begin(), delayBuf[(size_t) ch].end(), 0.0f);
    writePos     = 0;
    wowPhase     = 0.0;
    flutterPhase = 0.0;
    randomState  = 0.0;
    wowFreq      = wowFreqTarget.load(std::memory_order_relaxed);
    depthSmoothed = (double) depth.load(std::memory_order_relaxed);
}

void TapeWobble::process(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numCh      = juce::jmin(numChannelsPrepared, buffer.getNumChannels());
    if (numSamples <= 0 || numCh <= 0) return;

    // 目标深度（UI 拖动 → atomic），内部会逐 sample 向该目标一阶追踪，
    // 避免 UI 阶跃直接作用于振幅/延迟造成读位置跳变 → 咔哒/电流声。
    const double dTarget = (double) depth.load(std::memory_order_relaxed);

    // ---- 写入端 ----
    // 不论是否有调制，都需要把当前 block 写入延迟线（保持时间轴连续）
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* in = buffer.getReadPointer(ch);
        float* buf = delayBuf[(size_t) ch].data();
        for (int i = 0; i < numSamples; ++i)
        {
            buf[(writePos + i) & bufferMask] = in[i];
        }
    }

    // ---- 调制系数（lambda使用平滑后的 dCur，每次调用传入）----
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;

    auto ampSamplesFor = [&](double dCur, double weight, double freqHz) -> double
    {
        return (dCur * (double) kPeakRatioAtFullDepth * weight)
             / (twoPi * juce::jmax(1.0e-3, freqHz))
             * sampleRate;
    };

    // 1 阶低通系数（"慢随机走步"）：截止 fc → α = 1 - exp(-2π fc / fs)
    const double alphaRand = 1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi
                                            * (double) kRandomCutoffHz / sampleRate);

    // flutter 相位增量
    const double dPhaseFlutter = (double) kFlutterFreqHz / sampleRate;

    // 视觉相位（block 起点 wow 相位的小数部分）
    {
        double p = wowPhase - std::floor(wowPhase);
        if (p < 0.0) p += 1.0;
        wowPhase01Display.store((float) p, std::memory_order_relaxed);
    }

    const double targetFreq = wowFreqTarget.load(std::memory_order_relaxed);

    // ---- 读取端：sample 级演算瞬时 delay 并 4 点 Hermite 插值 ----
    for (int i = 0; i < numSamples; ++i)
    {
        // 频率平滑追踪：wowFreq += α * (target - wowFreq)
        wowFreq += freqSmoothCoeff * (targetFreq - wowFreq);
        // depth 平滑追踪（从根源消除"UI 拖动阶跃 → 振幅阶跃 → 读位置跳变 → 咔哒声"）
        depthSmoothed += depthSmoothCoeff * (dTarget - depthSmoothed);

        // 推进各 LFO
        const double dPhaseWow = wowFreq / sampleRate;
        wowPhase     += dPhaseWow;
        flutterPhase += dPhaseFlutter;

        // 随机走步（左右共享同一源，更"机械"，更像磁带）
        const double white = uni(rng);
        randomState += alphaRand * (white - randomState);

        const double sWow  = std::sin(twoPi * wowPhase);
        const double sFlut = std::sin(twoPi * flutterPhase);

        // sample 级重算振幅（深度每 sample 都在变化）
        const double aWow  = ampSamplesFor(depthSmoothed, (double) kWowWeight,     wowFreq);
        const double aFlut = ampSamplesFor(depthSmoothed, (double) kFlutterWeight, (double) kFlutterFreqHz);
        const double aRand = ampSamplesFor(depthSmoothed, (double) kRandomWeight,  (double) kRandomCutoffHz);

        // 瞬时调制偏移（samples，可正可负）
        const double mod = aWow * sWow + aFlut * sFlut + aRand * randomState;

        // 瞬时延迟（samples）。深度=0 时 mod=0 → 延迟恒等于 baseDelaySamples
        // （base 是固定值，不会引入额外可听延迟变化；总延迟很小且和 RubberBand
        //  主延迟相比可忽略，因此不再单独做 latency 上报）
        const double delay = baseDelaySamples + mod;

        // 读位置：写指针 + 当前 sample 偏移 - delay
        const double readPos   = (double) writePos + (double) i - delay;
        const double readFloor = std::floor(readPos);
        const float  frac      = (float) (readPos - readFloor);

        const int idx0 = (int) readFloor;
        const int im1  = (idx0 - 1) & bufferMask;
        const int i0   = (idx0    ) & bufferMask;
        const int i1   = (idx0 + 1) & bufferMask;
        const int i2   = (idx0 + 2) & bufferMask;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* buf = delayBuf[(size_t) ch].data();
            const float xm1 = buf[im1];
            const float x0  = buf[i0];
            const float x1  = buf[i1];
            const float x2  = buf[i2];
            const float y   = hermite4(xm1, x0, x1, x2, frac);
            buffer.getWritePointer(ch)[i] = y;
        }
    }

    // 推进写指针
    writePos = (writePos + numSamples) & bufferMask;

    // 防止相位累积过大造成精度下降
    if (wowPhase     > 1.0e6) wowPhase     -= std::floor(wowPhase);
    if (flutterPhase > 1.0e6) flutterPhase -= std::floor(flutterPhase);
}
