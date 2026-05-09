#include "PitchShiftEngine.h"
#include <rubberband/RubberBandLiveShifter.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using RubberBand::RubberBandLiveShifter;

PitchShiftEngine::PitchShiftEngine()
{
    for (int i = 0; i < kNumBands; ++i)
    {
        dotGains[(size_t)i].store(0.0f, std::memory_order_relaxed);
        dotPans [(size_t)i].store(0.0f, std::memory_order_relaxed);
    }
}

PitchShiftEngine::~PitchShiftEngine() = default;

void PitchShiftEngine::prepare(double newSampleRate, int maxBlockSize, int numChannels)
{
    sampleRate           = newSampleRate;
    maxBlockSizePrepared = juce::jmax(1, maxBlockSize);
    numChannelsPrepared  = juce::jlimit(1, 2, numChannels);

    // 为每个 band/ch 创建一个"单声道 LiveShifter"
    // 选项说明：
    //   OptionWindowShort         —— 低延迟窗口，音质/延迟的合理折中（~55ms@44.1k）
    //   OptionFormantShifted      —— 不做共振峰保留（对非人声一般更自然；需要保留人声
    //                                口型可以改为 OptionFormantPreserved）
    //   OptionChannelsApart       —— 多声道独立处理（我们这里每个 shifter 只有 1 ch，
    //                                此选项对单声道无实际影响）
    const int rbOptions =
          RubberBandLiveShifter::OptionWindowShort
        | RubberBandLiveShifter::OptionFormantShifted
        | RubberBandLiveShifter::OptionChannelsApart;

    reportedLatency = 0;

    for (int b = 0; b < kNumBands; ++b)
    {
        const double ratio = std::pow(2.0, kSemitones[(size_t)b] / 12.0);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& s = state[(size_t)b][(size_t)ch];

            s.shifter = std::make_unique<RubberBandLiveShifter>(
                (size_t)std::llround(sampleRate), /* channels */ 1, rbOptions);
            s.shifter->setPitchScale(ratio);

            s.rbBlockSize = (int)s.shifter->getBlockSize();
            jassert(s.rbBlockSize > 0);

            s.inAccum.assign((size_t)s.rbBlockSize, 0.0f);
            s.inAccumSize = 0;

            s.shiftIn .assign((size_t)s.rbBlockSize, 0.0f);
            s.shiftOut.assign((size_t)s.rbBlockSize, 0.0f);

            // outRing 容量：为最坏情况预留几倍 blockSize + 一次 DAW block 的余量
            const int cap = s.rbBlockSize * 4 + maxBlockSizePrepared * 2 + 32;
            s.outRing.assign((size_t)cap, 0.0f);
            s.outHead = 0;
            s.outTail = 0;

            // 记录延迟（五路相同，取任一即可；保守起见取最大）
            const int lat = (int)s.shifter->getStartDelay();
            reportedLatency = juce::jmax(reportedLatency, lat);
        }
    }
}

void PitchShiftEngine::reset()
{
    for (int b = 0; b < kNumBands; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& s = state[(size_t)b][(size_t)ch];
            if (s.shifter) s.shifter->reset();

            std::fill(s.inAccum.begin(), s.inAccum.end(), 0.0f);
            s.inAccumSize = 0;

            std::fill(s.outRing.begin(), s.outRing.end(), 0.0f);
            s.outHead = 0;
            s.outTail = 0;
        }
    }
}

void PitchShiftEngine::setDotGain(int index, float gain)
{
    if (index < 0 || index >= kNumBands) return;
    dotGains[(size_t)index].store(juce::jlimit(0.0f, 1.0f, gain), std::memory_order_relaxed);
}

void PitchShiftEngine::setDotPan(int index, float pan)
{
    if (index < 0 || index >= kNumBands) return;
    dotPans[(size_t)index].store(juce::jlimit(-1.0f, 1.0f, pan), std::memory_order_relaxed);
}

// 确保 outRing 至少能再写入 extra 个样本（靠前面 shift 腾出空间）
void PitchShiftEngine::ensureOutRingSpace(BandChannel& b, int extra)
{
    const int cap = (int)b.outRing.size();
    if (b.outTail + extra <= cap)
        return;

    // 向前 shift 已消耗部分
    const int valid = b.outTail - b.outHead;
    if (valid > 0 && b.outHead > 0)
        std::memmove(b.outRing.data(),
                     b.outRing.data() + b.outHead,
                     (size_t)valid * sizeof(float));
    b.outTail -= b.outHead;
    b.outHead = 0;

    // 若还是不够（极端情况），扩容（在 audio 线程应极少发生，但为稳健起见处理）
    if (b.outTail + extra > cap)
    {
        const int newCap = juce::jmax(cap * 2, b.outTail + extra + 32);
        b.outRing.resize((size_t)newCap, 0.0f);
    }
}

void PitchShiftEngine::pumpInputToBand(int band, int ch, const float* src, int n)
{
    auto& b = state[(size_t)band][(size_t)ch];
    if (!b.shifter || b.rbBlockSize <= 0 || n <= 0) return;

    int consumed = 0;
    while (consumed < n)
    {
        const int toCopy = juce::jmin(n - consumed, b.rbBlockSize - b.inAccumSize);
        std::memcpy(b.inAccum.data() + b.inAccumSize,
                    src + consumed,
                    (size_t)toCopy * sizeof(float));
        b.inAccumSize += toCopy;
        consumed      += toCopy;

        if (b.inAccumSize == b.rbBlockSize)
        {
            // 调用 LiveShifter
            // shift() 的参数是 const float* const*（"每个声道一个数组"）
            std::memcpy(b.shiftIn.data(), b.inAccum.data(),
                        (size_t)b.rbBlockSize * sizeof(float));
            const float* inPtrs [1] = { b.shiftIn.data()  };
            float*       outPtrs[1] = { b.shiftOut.data() };
            b.shifter->shift(inPtrs, outPtrs);

            // 把 shiftOut 追加到 outRing
            ensureOutRingSpace(b, b.rbBlockSize);
            std::memcpy(b.outRing.data() + b.outTail,
                        b.shiftOut.data(),
                        (size_t)b.rbBlockSize * sizeof(float));
            b.outTail += b.rbBlockSize;

            b.inAccumSize = 0;
        }
    }
}

int PitchShiftEngine::drainFromBand(int band, int ch, float* dst, int n)
{
    auto& b = state[(size_t)band][(size_t)ch];
    const int avail = b.outTail - b.outHead;
    const int take  = juce::jmin(avail, n);
    if (take > 0)
    {
        std::memcpy(dst, b.outRing.data() + b.outHead, (size_t)take * sizeof(float));
        b.outHead += take;
    }
    if (take < n)
        std::memset(dst + take, 0, (size_t)(n - take) * sizeof(float));
    return take;
}

void PitchShiftEngine::process(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numCh      = juce::jlimit(1, 2, buffer.getNumChannels());
    if (numSamples <= 0) return;

    // ===== 1. 喂入所有 band 所有 ch，并按 rbBlockSize 驱动 LiveShifter =====
    //
    // 每路 band 的 pitch ratio 是固定的（在 prepare() 中一次性 setPitchScale），
    // 这里只负责把整 block 输入推进到对应 shifter 的输出环。
    //
    // 颤音"老磁带跑调"效果由独立的 TapeWobble 模块在本引擎之后串联完成，
    // 不再在 5 路 LiveShifter 内做实时 ratio 调制 —— 这是消除毛刺电流声
    // 的关键架构改造。
    //
    // 注意：先把输入拷一份再写回 buffer，避免读写冲突。
    juce::AudioBuffer<float> inputCopy;
    inputCopy.makeCopyOf(buffer, true);

    for (int b = 0; b < kNumBands; ++b)
        for (int ch = 0; ch < numCh; ++ch)
            pumpInputToBand(b, ch, inputCopy.getReadPointer(ch), numSamples);

    // ===== 2. 按 band 取出 numSamples 个输出，做 gain/pan/混音 =====
    // 这里的 pan 采用"声道间能量转移"模型（保留原立体声信息）：
    //   pan = 0（白）：  outL = L, outR = R           —— 完全不动
    //   pan > 0（蓝/向右）：设 p = pan ∈ (0,1]
    //                    outL = L * (1 - p)
    //                    outR = R + L * p            —— 左声道的 p 比例搬到右
    //   pan < 0（红/向左）：设 p = -pan ∈ (0,1]
    //                    outR = R * (1 - p)
    //                    outL = L + R * p            —— 右声道的 p 比例搬到左
    //   pan = ±1：  对侧完全清空，全部并入单边（真正的"纯红/纯蓝"）
    //
    // 注意：若输入本身只有单声道（numCh==1），则 R 不存在，我们用 L 代 R，
    //       此时本算法退化为"原值衰减 + 对侧拷贝"，效果与经典 pan 一致。
    static thread_local std::vector<float> accL, accR, tmpL, tmpR;
    if ((int)accL.size() < numSamples) accL.assign((size_t)numSamples, 0.0f);
    else std::fill(accL.begin(), accL.begin() + numSamples, 0.0f);
    if ((int)accR.size() < numSamples) accR.assign((size_t)numSamples, 0.0f);
    else std::fill(accR.begin(), accR.begin() + numSamples, 0.0f);
    if ((int)tmpL.size() < numSamples) tmpL.resize((size_t)numSamples);
    if ((int)tmpR.size() < numSamples) tmpR.resize((size_t)numSamples);

    for (int b = 0; b < kNumBands; ++b)
    {
        const float gain = dotGains[(size_t)b].load(std::memory_order_relaxed);
        const float pan  = juce::jlimit(-1.0f, 1.0f,
                                        dotPans[(size_t)b].load(std::memory_order_relaxed));

        // 先把该 band 两声道的 shifted 样本都取出来
        drainFromBand(b, 0, tmpL.data(), numSamples);
        if (numCh >= 2)
            drainFromBand(b, 1, tmpR.data(), numSamples);
        else
            std::memcpy(tmpR.data(), tmpL.data(), (size_t)numSamples * sizeof(float));

        if (gain <= 1.0e-6f) continue;

        // 声道间能量转移系数
        //   outL = aLL * L + aRL * R
        //   outR = aLR * L + aRR * R
        float aLL, aRL, aLR, aRR;
        if (pan >= 0.0f)
        {
            const float p = pan;             // 向右搬
            aLL = 1.0f - p;  aRL = 0.0f;
            aLR = p;         aRR = 1.0f;
        }
        else
        {
            const float p = -pan;            // 向左搬
            aLL = 1.0f;      aRL = p;
            aLR = 0.0f;      aRR = 1.0f - p;
        }

        const float kLL = gain * aLL;
        const float kRL = gain * aRL;
        const float kLR = gain * aLR;
        const float kRR = gain * aRR;

        const float* pL = tmpL.data();
        const float* pR = tmpR.data();
        for (int i = 0; i < numSamples; ++i)
        {
            const float L = pL[i];
            const float R = pR[i];
            accL[(size_t)i] += L * kLL + R * kRL;
            accR[(size_t)i] += L * kLR + R * kRR;
        }
    }

    // ===== 3. 硬削波并写回 buffer =====
    auto hardClip = [](float x) -> float
    {
        return juce::jlimit(-1.0f, 1.0f, x);
    };

    const int outChannels = buffer.getNumChannels();
    if (outChannels >= 2)
    {
        float* outL = buffer.getWritePointer(0);
        float* outR = buffer.getWritePointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] = hardClip(accL[(size_t)i]);
            outR[i] = hardClip(accR[(size_t)i]);
        }
        for (int ch = 2; ch < outChannels; ++ch)
            buffer.clear(ch, 0, numSamples);
    }
    else if (outChannels == 1)
    {
        float* out = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
            out[i] = hardClip(0.5f * (accL[(size_t)i] + accR[(size_t)i]));
    }
}