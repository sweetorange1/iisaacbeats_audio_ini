#pragma once

#include <JuceHeader.h>
#include <array>
#include "PluginProcessor.h"

class PuponvstAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    PuponvstAudioProcessorEditor(PuponvstAudioProcessor&);
    ~PuponvstAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    // ===== 动效驱动 =====
    // Timer 回调：每帧从 processor 拉取一小段样本，更新音频电平；推进时间相位；然后 repaint()
    void timerCallback() override;
    
    // 平滑后的音频电平 (RMS 近似) ∈ [0, 1]，用于驱动呼吸式动效
    float audioLevel = 0.0f;
    // 短时 peak，用于驱动更瞬时的冲击（例如激光能量光斑扩张）
    float audioPeak = 0.0f;
    // 时间相位（秒），单调累加，用于激光游走光斑、光球扫光等与时间相关的动画
    float animPhase = 0.0f;
    PuponvstAudioProcessor& processor;
    // GUI组件
    juce::Label titleLabel;       // 大标题 "Pupon"，字号 32
    juce::Label versionLabel;     // 副标题 "v0.1.0 iisaacbeats.cn"，字号 20

    // 自定义字体（AtomicMarker.otf，二进制资源加载）
    juce::Typeface::Ptr atomicMarkerTypeface;
    
    // 窗口等比缩放约束器
    juce::ComponentBoundsConstrainer resizeConstrainer;
    
    // 正态分布参数
    float sigma = 1.0f;
    
    // ===== 射线斜率模型 =====
    // 坐标系定义：以控制区域下边缘中点(bottomCenter)为原点，X轴水平向右，Y轴向上（数学坐标系）
    // 红线斜率记为 rayslopeK，蓝线斜率为 -rayslopeK（左右对称）
    // 射线总是向上发射（Y分量 > 0）
    // 默认：红线指向左上角 (k<0)，蓝线指向右上角 (k>0)
    float rayslopeK = 0.0f;       // 红线斜率（数学坐标系下）
    bool  isVerticalRay = false;  // 是否为垂直射线（k为无穷大时两线重合向正上方）
    
    // 射线交互状态
    bool isDraggingRedLine = false;
    bool isDraggingBlueLine = false;
    bool isDraggingNormalCurve = false;
    
    // ===== 5个圆点的拖动状态 =====
    // 圆点按从左到右索引 0..4；分组：组L = {0,1}，组C = {2}，组R = {3,4}
    // offsetT[i] 表示圆点在"轨道"上的位置：1.0 = 默认位置（竖直线与正态曲线的交点，最高），
    //                                     0.0 = 控制区域底部（最低）
    std::array<float, 5> dotOffsetT { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    int draggingDotIndex = -1; // -1 表示没有拖动圆点；否则为 0..4

    // ===== 黄色颤音控制器（横线） =====
    // vibratoT ∈ [0, 1]：0 = 线位于控制区最底部（无颤音），1 = 线位于控制区最顶部（最大颤音）
    // 由用户上下拖动这条横线改变。
    //   t ∈ [0, 0.15] 为死区（黄线仍是直线，无任何颤音）
    //   t ∈ [0.15, 1.0] 时颤音生效，振幅按 (t - 0.15)/0.85 线性映射
    // 初始值给一点（0.05），刚好处于死区边界 → 默认无颤音
    float vibratoT = 0.05f;
    bool  isDraggingReverbLine = false;
    
    // 控制区域下边缘中点（射线发射原点，屏幕坐标）
    juce::Point<float> bottomCenter;
    
    // 辅助函数
    bool isPointNearLine(const juce::Point<float>& point, 
                       const juce::Point<float>& lineStart, 
                       const juce::Point<float>& lineEnd, 
                       float threshold);

    // 计算正态曲线在指定x位置的Y坐标
    float getNormalCurveY(float x, const juce::Rectangle<int>& controlArea) const;
    
    // 计算点到正态曲线的最短距离
    float distanceToNormalCurve(const juce::Point<float>& point, 
                                 const juce::Rectangle<int>& controlArea) const;
    
    // 根据斜率 k (数学坐标系, Y轴向上)，从原点(controlArea下边缘中点)出发计算射线与控制区域边界的交点(屏幕坐标)
    // isVertical = true 时表示垂直向上射线
    juce::Point<float> calculateRayEndBySlope(float k, bool isVertical,
                                               const juce::Rectangle<int>& controlArea) const;
    
    // 根据鼠标屏幕坐标计算相对于原点的斜率（数学坐标系，Y轴向上）
    // 保护策略：dyUp 过小（鼠标贴下边）→ k=0（水平）；|dx| 过小（鼠标几乎正上方）→ 斜率饱和到一个大的有限值
    // 始终返回 true；不会再产生垂直射线状态
    bool computeSlopeFromMouse(const juce::Point<float>& mousePos,
                                const juce::Rectangle<int>& controlArea,
                                float& outK) const;

    // ===== 圆点几何辅助 =====
    // 返回第 i (0..4) 根竖直线的屏幕 X
    float getDotColumnX(int i, const juce::Rectangle<int>& controlArea) const;
    // 返回第 i 根竖直线与正态曲线的交点 Y（即圆点轨道的顶端，t=1 时的位置）
    float getDotTrackTopY(int i, const juce::Rectangle<int>& controlArea) const;
    // 返回第 i 个圆点在当前 offsetT 下的屏幕中心坐标
    juce::Point<float> getDotCenter(int i, const juce::Rectangle<int>& controlArea) const;

    // ===== 参数同步：把 5 个圆点的 gain / pan 推送给 Processor（音频线程会读取）=====
    // 调用时机：初始化、拖动圆点 / 射线 / 正态曲线之后、窗口 resize 之后
    void pushDotParamsToProcessor();

    // 构造函数初始化标志：在构造完成前为 false，期间 setSize() 触发的早期 resized()
    // 不应该把"还未恢复完毕"的成员值（默认 0/1 等）镜像到 Processor 的 editorState，
    // 否则会覆盖宿主 setStateInformation 恢复出的真存档，导致参数无法保存。
    bool initialised = false;

    // 黄色颤音横线的屏幕 Y 坐标
    //   t = 0 → 控制区底部   t = 1 → 控制区顶部
    float getReverbLineY(const juce::Rectangle<int>& controlArea) const;

    // 黄色颤音横线当前的"正弦振幅"（像素），与 paint 中的绘制保持一致。
    // 死区时返回 0；进入颤音区后随归一化深度从 0 → maxAmp。
    // 用途：mouseDown 判定时把命中区域随波形振幅一起放大，避免波峰/波谷处点击穿过线。
    float getReverbWaveAmplitudePx(const juce::Rectangle<int>& controlArea) const;

    // 视觉位置 t_visual → 推送给 processor 的 vibratoT_audio 的非线性映射（前缓后陡）
    // 设计：在死区(t<0.15)内保持线性（直接透传），死区外用 gamma=2.5 的幂曲线
    //       让前 70% 的拖动行程内仅产生 ~30% 的颤音强度，最后 30% 行程产生剩余 70%。
    // 输入/输出都是 [0, 1]；视觉位置不变（黄线 Y 仍按 t_visual 显示），仅改变音频端深度感知。
    static float mapVibratoVisualToAudio(float tVisual) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PuponvstAudioProcessorEditor)
};