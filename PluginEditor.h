#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class PuponvstAudioProcessorEditor : public juce::AudioProcessorEditor
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
    PuponvstAudioProcessor& processor;
    // GUI组件
    juce::Label titleLabel;
    
    // 等比缩放相关变量
    juce::ComponentBoundsConstrainer resizeConstrainer; // 窗口约束器，用于实现等比放大
    juce::Rectangle<int> initialControlArea;     // 初始控制区域
    juce::Point<float> initialRedLineEnd;       // 初始红色射线端点
    juce::Point<float> initialBlueLineEnd;      // 初始蓝色射线端点
    int initialControlWidth = 0;                // 初始控制区域宽度
    int initialControlHeight = 0;              // 初始控制区域高度
    
    // 正态分布参数
    float sigma = 1.0f;
    
    // 射线交互状态
    bool isDraggingRedLine = false;
    bool isDraggingBlueLine = false;
    bool isDraggingNormalCurve = false; // 新增：正态曲线拖动状态
    juce::Point<float> redLineEnd;
    juce::Point<float> blueLineEnd;
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
    
    // 计算射线从bottomCenter出发经过passPoint延伸到controlArea上边界的终点
    juce::Point<float> calculateRayEnd(const juce::Point<float>& passPoint,
                                        const juce::Rectangle<int>& controlArea) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PuponvstAudioProcessorEditor)
};