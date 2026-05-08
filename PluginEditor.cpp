#include "PluginEditor.h"
#include <JuceHeader.h>
#include <cmath>

// 主编辑器实现
PuponvstAudioProcessorEditor::PuponvstAudioProcessorEditor(PuponvstAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p)
{
    setSize(900, 600);

    // 设置导航栏标题
    titleLabel.setText("Pupon v0.1.0 iisaacbeats.cn", juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    titleLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    
    addAndMakeVisible(titleLabel);
    
    // 设置等比放大约束器
    resizeConstrainer.setFixedAspectRatio(1.5f); // 宽高比为 1.5:1 (900:600)
    resizeConstrainer.setMinimumSize(600, 400);
    resizeConstrainer.setMaximumSize(1920, 1280);
    setConstrainer(&resizeConstrainer);
    
    // 初始化对角线端点
    auto controlArea = getLocalBounds();
    controlArea.removeFromTop(60); // 移除导航栏
    bottomCenter = juce::Point<float>(controlArea.getCentreX(), controlArea.getBottom());
    redLineEnd = juce::Point<float>(controlArea.getX(), controlArea.getY());
    blueLineEnd = juce::Point<float>(controlArea.getRight(), controlArea.getY());
    
    // 保存初始状态用于等比缩放
    initialControlArea = controlArea;
    initialRedLineEnd = redLineEnd;
    initialBlueLineEnd = blueLineEnd;
    initialControlWidth = controlArea.getWidth();
    initialControlHeight = controlArea.getHeight();
}

PuponvstAudioProcessorEditor::~PuponvstAudioProcessorEditor() {}

void PuponvstAudioProcessorEditor::paint(juce::Graphics& g)
{
    // 暗色主题背景
    g.fillAll(juce::Colour(0xFF1E1E1E));
    
    // 绘制导航栏
    auto navBar = getLocalBounds().removeFromTop(60);
    g.setColour(juce::Colour(0xFF252526));
    g.fillRect(navBar);
    
    // 导航栏边框
    g.setColour(juce::Colour(0xFF3C3C3C));
    g.drawLine(0, 60, getWidth(), 60, 1.0f);
    
    // 获取控制区域（统一移除导航栏高度，保持与交互判定一致）
    auto controlArea = getLocalBounds();
    controlArea.removeFromTop(60);
    
    // 直接在控制区域绘制正态分布曲线 (x轴在最底部)
    const float mean = controlArea.getCentreX();
    const float scale = controlArea.getWidth() / 6.0f; // ±3σ范围
    const float maxHeight = controlArea.getHeight() * 0.8f;
    const float baseY = controlArea.getBottom(); // x轴位置在最底部
    
    juce::Path normalPath;
    bool firstPoint = true;
    
    for (float x = controlArea.getX(); x <= controlArea.getRight(); x += 1.0f)
    {
        float normalizedX = (x - mean) / scale;
        float y = maxHeight * std::exp(-0.5f * normalizedX * normalizedX / (sigma * sigma));
        float plotY = baseY - y; // 从底部向上绘制
        
        if (firstPoint)
        {
            normalPath.startNewSubPath(x, plotY);
            firstPoint = false;
        }
        else
        {
            normalPath.lineTo(x, plotY);
        }
    }
    
    // 绘制正态分布曲线
    g.setColour(juce::Colours::white);
    g.strokePath(normalPath, juce::PathStrokeType(2.0f));
    
    // 绘制5根竖直线和交点（相对位置，随窗口等比缩放）
    const std::array<float, 5> relativePositions = {0.167f, 0.333f, 0.500f, 0.667f, 0.833f}; // 相对位置 [1/6, 1/3, 1/2, 2/3, 5/6]
    
    for (auto relPos : relativePositions)
    {
        float xPos = controlArea.getX() + relPos * controlArea.getWidth();
        
        if (xPos >= controlArea.getX() && xPos <= controlArea.getRight())
        {
            // 计算该x位置对应的y值（正态分布曲线上的点）
            float normalizedX = (xPos - mean) / scale;
            float yValue = maxHeight * std::exp(-0.5f * normalizedX * normalizedX / (sigma * sigma));
            float intersectionY = baseY - yValue;
            
            // 绘制竖直线（从底部到交点，再向上延伸10像素）
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawLine(xPos, baseY, xPos, intersectionY - 10.0f, 1.0f);
            
            // 绘制交点（白色小圆点）
            g.setColour(juce::Colours::white);
            g.fillEllipse(xPos - 3.0f, intersectionY - 3.0f, 6.0f, 6.0f);
        }
    }
    
    // 绘制射线：从下边缘中心点出发，经过 redLineEnd / blueLineEnd 延伸到控制区域边界
    // 此时 controlArea 已经移除了导航栏，与上方曲线绘制共用同一区域
    juce::Point<float> bottomCenterLocal(controlArea.getCentreX(), controlArea.getBottom());
    
    // 统一使用 redLineEnd / blueLineEnd 作为射线的"经过点"，计算真实延伸终点
    juce::Point<float> redRayEnd = calculateRayEnd(redLineEnd, controlArea);
    juce::Point<float> blueRayEnd = calculateRayEnd(blueLineEnd, controlArea);
    
    // 绘制红色射线
    g.setColour(juce::Colours::red);
    g.drawLine(bottomCenterLocal.x, bottomCenterLocal.y, redRayEnd.x, redRayEnd.y, 2.0f);
    
    // 绘制蓝色射线
    g.setColour(juce::Colours::blue);
    g.drawLine(bottomCenterLocal.x, bottomCenterLocal.y, blueRayEnd.x, blueRayEnd.y, 2.0f);
}

void PuponvstAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    auto controlArea = getLocalBounds();
    controlArea.removeFromTop(60);
    
    juce::Point<float> mousePos = event.position;
    
    // 鼠标必须在控制区域内才处理
    if (!controlArea.contains(mousePos.toInt()))
        return;
    
    // 重置所有拖动状态
    isDraggingRedLine = false;
    isDraggingBlueLine = false;
    isDraggingNormalCurve = false;
    
    // 统一的容差设置
    const float normalCurveThreshold = 12.0f; // 正态曲线容差
    const float rayThreshold = 8.0f;          // 射线容差
    
    // ========== 优先级 1: 检测正态分布曲线 ==========
    // 使用点到曲线的真实最短距离（采样法），实现各方向均匀的容差
    float distToCurve = distanceToNormalCurve(mousePos, controlArea);
    
    if (distToCurve <= normalCurveThreshold)
    {
        isDraggingNormalCurve = true;
        return;
    }
    
    // ========== 优先级 2: 检测红色射线 ==========
    // 使用实际的射线几何：从 bottomCenter 出发，经过 redLineEnd，延伸到上边界
    juce::Point<float> rayStart(controlArea.getCentreX(), controlArea.getBottom());
    juce::Point<float> redActualEnd = calculateRayEnd(redLineEnd, controlArea);
    
    if (isPointNearLine(mousePos, rayStart, redActualEnd, rayThreshold))
    {
        isDraggingRedLine = true;
        return;
    }
    
    // ========== 优先级 3: 检测蓝色射线 ==========
    juce::Point<float> blueActualEnd = calculateRayEnd(blueLineEnd, controlArea);
    
    if (isPointNearLine(mousePos, rayStart, blueActualEnd, rayThreshold))
    {
        isDraggingBlueLine = true;
        return;
    }
    
    // 都没有命中，忽略此次按下
}

void PuponvstAudioProcessorEditor::mouseDrag(const juce::MouseEvent& event)
{
    auto controlArea = getLocalBounds();
    controlArea.removeFromTop(60);
    
    // 如果没有命中任何线，mouseDown中没有设置拖动状态，这里直接忽略
    if (!isDraggingNormalCurve && !isDraggingRedLine && !isDraggingBlueLine)
        return;
    
    juce::Point<float> mousePos = event.position;
    
    // 限制鼠标位置在控制区域内
    mousePos.x = juce::jlimit<float>((float)controlArea.getX(), (float)controlArea.getRight(), mousePos.x);
    mousePos.y = juce::jlimit<float>((float)controlArea.getY(), (float)controlArea.getBottom(), mousePos.y);
    
    // ========== 1. 处理正态曲线拖动 ==========
    if (isDraggingNormalCurve)
    {
        // 根据鼠标位置反推 sigma，使曲线经过鼠标点
        const float mean = controlArea.getCentreX();
        const float scale = controlArea.getWidth() / 6.0f;
        const float maxHeight = controlArea.getHeight() * 0.8f;
        const float baseY = controlArea.getBottom();
        
        float normalizedX = (mousePos.x - mean) / scale;
        // currentY 是曲线在该x位置的高度（从底部向上），要让曲线经过鼠标：
        // 鼠标y = baseY - currentY  =>  currentY = baseY - mousePos.y
        float currentY = baseY - mousePos.y;
        
        // 钳制 currentY 在 (0, maxHeight] 之间
        currentY = juce::jlimit(1.0f, maxHeight, currentY);
        
        float yRatio = currentY / maxHeight;
        
        // 正态曲线公式：yRatio = exp(-0.5 * normalizedX^2 / sigma^2)
        // => sigma = |normalizedX| / sqrt(-2 * ln(yRatio))
        // 特殊情况：当 normalizedX ≈ 0（鼠标在中心x附近），无法反推 sigma
        if (std::abs(normalizedX) > 0.01f && yRatio < 0.9999f)
        {
            float logValue = -2.0f * std::log(yRatio);
            if (logValue > 1e-6f)
            {
                float newSigma = std::abs(normalizedX) / std::sqrt(logValue);
                sigma = juce::jlimit(0.1f, 3.0f, newSigma);
            }
        }
        
        repaint();
    }
    // ========== 2. 处理红色射线拖动 ==========
    else if (isDraggingRedLine)
    {
        // 让红色射线经过鼠标点：直接将 redLineEnd 设为鼠标位置
        // （射线从 bottomCenter 出发，经过 redLineEnd 延伸，所以设为鼠标点即可让射线经过鼠标）
        redLineEnd = mousePos;
        
        // 蓝色射线关于垂直中线对称
        float centreX = controlArea.getCentreX();
        blueLineEnd = juce::Point<float>(2.0f * centreX - redLineEnd.x, redLineEnd.y);
        
        repaint();
    }
    // ========== 3. 处理蓝色射线拖动 ==========
    else if (isDraggingBlueLine)
    {
        blueLineEnd = mousePos;
        
        float centreX = controlArea.getCentreX();
        redLineEnd = juce::Point<float>(2.0f * centreX - blueLineEnd.x, blueLineEnd.y);
        
        repaint();
    }
}

void PuponvstAudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    isDraggingRedLine = false;
    isDraggingBlueLine = false;
    isDraggingNormalCurve = false;
}

// 辅助函数：检查点是否靠近线段（返回点到线段的距离是否小于阈值）
bool PuponvstAudioProcessorEditor::isPointNearLine(const juce::Point<float>& point, 
                    const juce::Point<float>& lineStart, 
                    const juce::Point<float>& lineEnd, 
                    float threshold)
{
    // 计算点到线段的最短距离
    float lineLength = lineStart.getDistanceFrom(lineEnd);
    if (lineLength < 1.0f) return point.getDistanceFrom(lineStart) <= threshold;
    
    // 使用参数化的投影 t，钳制在 [0, 1] 保证投影点落在线段上
    float t = juce::jlimit(0.0f, 1.0f, 
        ((point.x - lineStart.x) * (lineEnd.x - lineStart.x) + 
         (point.y - lineStart.y) * (lineEnd.y - lineStart.y)) / (lineLength * lineLength));
    
    juce::Point<float> closestPoint(
        lineStart.x + t * (lineEnd.x - lineStart.x),
        lineStart.y + t * (lineEnd.y - lineStart.y)
    );
    
    return point.getDistanceFrom(closestPoint) <= threshold;
}

// 计算正态曲线在指定x位置的Y坐标
float PuponvstAudioProcessorEditor::getNormalCurveY(float x, const juce::Rectangle<int>& controlArea) const
{
    const float mean = controlArea.getCentreX();
    const float scale = controlArea.getWidth() / 6.0f;
    const float maxHeight = controlArea.getHeight() * 0.8f;
    const float baseY = (float)controlArea.getBottom();
    
    float normalizedX = (x - mean) / scale;
    float y = maxHeight * std::exp(-0.5f * normalizedX * normalizedX / (sigma * sigma));
    return baseY - y;
}

// 计算点到正态曲线的最短距离（使用采样法，遍历曲线段计算最短距离）
float PuponvstAudioProcessorEditor::distanceToNormalCurve(const juce::Point<float>& point, 
                                                          const juce::Rectangle<int>& controlArea) const
{
    // 在鼠标x坐标附近采样曲线点，并计算到连续曲线段的距离
    // 采样范围：以鼠标x为中心，左右各扩展 searchRange 像素
    const float searchRange = 40.0f;
    const float sampleStep = 2.0f;
    
    float minX = juce::jmax((float)controlArea.getX(), point.x - searchRange);
    float maxX = juce::jmin((float)controlArea.getRight(), point.x + searchRange);
    
    float minDist = std::numeric_limits<float>::max();
    
    juce::Point<float> prev(minX, getNormalCurveY(minX, controlArea));
    for (float x = minX + sampleStep; x <= maxX; x += sampleStep)
    {
        juce::Point<float> curr(x, getNormalCurveY(x, controlArea));
        
        // 计算 point 到线段 [prev, curr] 的距离
        float segLen = prev.getDistanceFrom(curr);
        if (segLen >= 1e-4f)
        {
            float t = juce::jlimit(0.0f, 1.0f,
                ((point.x - prev.x) * (curr.x - prev.x) +
                 (point.y - prev.y) * (curr.y - prev.y)) / (segLen * segLen));
            juce::Point<float> proj(prev.x + t * (curr.x - prev.x),
                                    prev.y + t * (curr.y - prev.y));
            float d = point.getDistanceFrom(proj);
            if (d < minDist) minDist = d;
        }
        else
        {
            float d = point.getDistanceFrom(prev);
            if (d < minDist) minDist = d;
        }
        prev = curr;
    }
    
    return minDist;
}

// 计算从 bottomCenter 出发经过 passPoint 延伸到控制区域矩形边界的终点
// 支持任意方向（包括锐角、直角、钝角），射线会与矩形的上/左/右/下边界相交
juce::Point<float> PuponvstAudioProcessorEditor::calculateRayEnd(const juce::Point<float>& passPoint,
                                                                  const juce::Rectangle<int>& controlArea) const
{
    juce::Point<float> start((float)controlArea.getCentreX(), (float)controlArea.getBottom());
    juce::Point<float> direction = passPoint - start;
    
    // 如果 passPoint 与 start 几乎重合，返回一个安全默认值
    if (std::abs(direction.x) < 1e-4f && std::abs(direction.y) < 1e-4f)
    {
        return juce::Point<float>(start.x, (float)controlArea.getY());
    }
    
    const float left   = (float)controlArea.getX();
    const float right  = (float)controlArea.getRight();
    const float top    = (float)controlArea.getY();
    const float bottom = (float)controlArea.getBottom();
    
    // 参数化射线：P(t) = start + t * direction，t >= 0 沿射线方向
    // 求射线与四条边界（left / right / top / bottom）的交点中 t 最小的那个
    float minT = std::numeric_limits<float>::max();
    
    auto tryEdge = [&](float t) {
        if (t > 1e-6f && t < minT)
        {
            float px = start.x + direction.x * t;
            float py = start.y + direction.y * t;
            // 交点必须落在矩形边界上（含很小的容差）
            const float eps = 0.5f;
            if (px >= left - eps && px <= right + eps &&
                py >= top - eps && py <= bottom + eps)
            {
                minT = t;
            }
        }
    };
    
    // 与上边界 y = top 相交
    if (std::abs(direction.y) > 1e-6f)
    {
        tryEdge((top - start.y) / direction.y);
        // 与下边界 y = bottom 相交
        tryEdge((bottom - start.y) / direction.y);
    }
    // 与左边界 x = left 相交
    if (std::abs(direction.x) > 1e-6f)
    {
        tryEdge((left - start.x) / direction.x);
        // 与右边界 x = right 相交
        tryEdge((right - start.x) / direction.x);
    }
    
    if (minT == std::numeric_limits<float>::max())
    {
        // 理论上不会发生（start 在矩形边界上，射线方向非零总能找到出射点）
        return passPoint;
    }
    
    float endX = start.x + direction.x * minT;
    float endY = start.y + direction.y * minT;
    
    // 钳制以消除浮点误差
    endX = juce::jlimit(left, right, endX);
    endY = juce::jlimit(top, bottom, endY);
    
    return juce::Point<float>(endX, endY);
}

void PuponvstAudioProcessorEditor::resized()
{
    // 导航栏布局
    auto navBar = getLocalBounds().removeFromTop(60);
    titleLabel.setBounds(navBar.reduced(20, 0));
    
    // 更新控制区域和下边界中心点
    auto controlArea = getLocalBounds();
    controlArea.removeFromTop(60);
    bottomCenter = juce::Point<float>(controlArea.getCentreX(), controlArea.getBottom());
    
    // 窗口缩放时，射线端点总是进行等比缩放，保持斜率不变
    // 只有在用户主动拖动射线时，才使用鼠标位置来更新射线端点
    if (!isDraggingRedLine && !isDraggingBlueLine)
    {
        // 保持射线斜率不变，只根据窗口大小等比缩放端点位置
        float widthRatio = (float)controlArea.getWidth() / initialControlWidth;
        float heightRatio = (float)controlArea.getHeight() / initialControlHeight;
        
        redLineEnd = juce::Point<float>(
            controlArea.getX() + (initialRedLineEnd.x - initialControlArea.getX()) * widthRatio,
            controlArea.getY() + (initialRedLineEnd.y - initialControlArea.getY()) * heightRatio
        );
        
        blueLineEnd = juce::Point<float>(
            controlArea.getX() + (initialBlueLineEnd.x - initialControlArea.getX()) * widthRatio,
            controlArea.getY() + (initialBlueLineEnd.y - initialControlArea.getY()) * heightRatio
        );
    }
}