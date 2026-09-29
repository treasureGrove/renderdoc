/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "AgentAssistantPanel.h"
#include <QClipboard>
#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QImage>
#include <QLineEdit>
#include <QStandardPaths>
#include <QStringList>
#include <initializer_list>
#include <cstdint>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCheckBox>
#include <QFontDatabase>
#include <QLabel>
#include <QtGlobal>
#include <QPalette>
#include <QRegExp>
#include <QKeySequence>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QUrlQuery>
#include "Code/Interface/PersistentConfig.h"
#include "Code/QRDUtils.h"
#include "ui_AgentAssistantPanel.h"

enum class AgentLLMBackend : int
{
  OpenAI = 0,
  Anthropic = 1,
  GoogleGemini = 2,
  AzureOpenAI = 3,
  OpenAICompatible = 4,
  OpenRouter = 5,
  GLM_Zhipu = 6,
  GitHubModels = 7,
  DeepSeek = 8,
  DashScope = 9,    // Aliyun Bailian (Qwen)
  Moonshot = 10,    // Moonshot AI (Kimi)
  Qianfan = 11,     // Baidu Qianfan (ERNIE)
  Volcano = 12,     // Volcano Engine Ark (Doubao)
};

// Bitmask: which data "skills" to attach to the LLM context for the current question (Pipeline Agent).
enum AgentDataSkill : uint32_t
{
  AgentSkill_CaptureMeta = 1u << 0,
  AgentSkill_Action = 1u << 1,
  AgentSkill_PipelineSummary = 1u << 2,
  AgentSkill_ViewGeom = 1u << 3,
  AgentSkill_Shaders = 1u << 4,
  AgentSkill_OMTargets = 1u << 5,
  AgentSkill_BlendStencil = 1u << 6,
  AgentSkill_Descriptors = 1u << 7,
  AgentSkill_DescriptorAccess = 1u << 8,
  AgentSkill_ShaderMessages = 1u << 9,
  AgentSkill_ResourceBrief = 1u << 10,
  AgentSkill_AssetCatalog = 1u << 11,
  AgentSkill_All = 0xFFFFFFFFu
};

// Grove Glass: TAWEBTOOL-aligned emerald frosted-glass theme with background image.
// White semi-transparent glass over the backdrop; emerald #26a884 accents.
static QString agentPanelStylesheetAnime()
{
  return QStringLiteral(R"CSS(
#AgentAssistantPanel {
  background: transparent;
  border: none;
}
#AgentAssistantPanel QWidget {
  background: transparent;
}
#AgentAssistantPanel QLabel {
  font: 14px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  color: #17242B;
  background-color: transparent;
}
#AgentAssistantPanel #helpLabel {
  font: 12px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  color: #3F545C;
  background-color: rgba(255, 255, 255, 140);
  padding: 8px 10px;
  border-radius: 8px;
}
#AgentAssistantPanel QGroupBox {
  border: 1px solid rgba(255, 255, 255, 60);
  border-top: 1px solid rgba(255, 255, 255, 90);
  border-left: 1px solid rgba(255, 255, 255, 70);
  border-radius: 18px;
  background-color: rgba(255, 255, 255, 78);
  margin-top: 16px;
  padding: 16px 12px 12px 12px;
}
#AgentAssistantPanel QGroupBox::title {
  subcontrol-origin: margin;
  left: 14px;
  padding: 0 8px;
  font: bold 14px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  color: #0F2438;
}
#AgentAssistantPanel QLineEdit,
#AgentAssistantPanel QComboBox {
  background-color: rgba(255, 255, 255, 92);
  border: 1px solid rgba(255, 255, 255, 60);
  border-bottom: 1px solid rgba(117, 210, 184, 110);
  border-radius: 8px;
  font: 14px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  padding: 5px 10px;
  color: #17242B;
  min-height: 22px;
  selection-background-color: rgba(38, 168, 132, 160);
  selection-color: white;
}
#AgentAssistantPanel QComboBox::drop-down {
  border: none;
  width: 24px;
}
#AgentAssistantPanel QComboBox::down-arrow {
  image: none;
  border-left: 4px solid transparent;
  border-right: 4px solid transparent;
  border-top: 5px solid #147A63;
  margin-right: 8px;
}
#AgentAssistantPanel QComboBox QAbstractItemView {
  background-color: rgba(248, 250, 252, 245);
  border: 1px solid rgba(100, 140, 180, 90);
  border-radius: 6px;
  color: #17242B;
  selection-background-color: rgba(38, 168, 132, 140);
  selection-color: #0F1A28;
  padding: 4px;
  outline: none;
}
#AgentAssistantPanel QComboBox QAbstractItemView::item {
  padding: 4px 8px;
  min-height: 22px;
}
#AgentAssistantPanel QComboBox QAbstractItemView::item:hover {
  background-color: rgba(38, 168, 132, 60);
}
#AgentAssistantPanel QLineEdit:hover,
#AgentAssistantPanel QComboBox:hover {
  background-color: rgba(255, 255, 255, 70);
  border: 1px solid rgba(255, 255, 255, 90);
}
#AgentAssistantPanel QLineEdit:focus {
  border: 1px solid rgba(38, 168, 132, 160);
  border-bottom: 1px solid rgba(20, 122, 99, 200);
}
#AgentAssistantPanel QLineEdit:disabled,
#AgentAssistantPanel QComboBox:disabled {
  color: #5F7078;
  background-color: rgba(255, 255, 255, 45);
  border: 1px solid rgba(255, 255, 255, 25);
}
#AgentAssistantPanel QPlainTextEdit {
  background-color: rgba(255, 255, 255, 88);
  color: #17242B;
  border: 1px solid rgba(255, 255, 255, 50);
  border-top: 1px solid rgba(255, 255, 255, 70);
  border-radius: 12px;
  padding: 10px;
  font: 11pt Consolas, "Cascadia Mono", "Courier New", monospace;
  selection-background-color: rgba(38, 168, 132, 160);
  selection-color: white;
}
#AgentAssistantPanel #chatLog {
  background-color: rgba(255, 255, 255, 82);
  border: 1px solid rgba(255, 255, 255, 40);
  border-top: 1px solid rgba(255, 255, 255, 65);
  border-left: 1px solid rgba(255, 255, 255, 50);
  border-radius: 18px;
  padding: 8px;
  font: 13px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  color: #17242B;
}
#AgentAssistantPanel #snapshotToggleBtn {
  background-color: transparent;
  border: none;
  color: #147A63;
  font: bold 13px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  text-align: left;
  padding: 2px 4px;
}
#AgentAssistantPanel #snapshotToggleBtn:hover {
  color: #0E5C4B;
  text-decoration: underline;
}
#AgentAssistantPanel QPushButton {
  background-color: rgba(255, 255, 255, 72);
  border: 1px solid rgba(255, 255, 255, 50);
  border-top: 1px solid rgba(255, 255, 255, 75);
  border-radius: 8px;
  color: #17242B;
  font: 14px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  padding: 5px 12px 6px 12px;
  min-height: 24px;
}
#AgentAssistantPanel QPushButton:hover {
  background-color: rgba(255, 255, 255, 88);
  border: 1px solid rgba(255, 255, 255, 80);
  border-top: 1px solid rgba(255, 255, 255, 110);
  color: #11221E;
}
#AgentAssistantPanel QPushButton:pressed {
  color: #3F545C;
  background-color: rgba(255, 255, 255, 30);
}
#AgentAssistantPanel QPushButton:disabled {
  color: rgba(180, 200, 220, 80);
  background-color: rgba(255, 255, 255, 15);
  border: 1px solid rgba(255, 255, 255, 20);
}
#AgentAssistantPanel #sendLLMButton {
  color: white;
  background-color: rgba(38, 168, 132, 185);
  border: 1px solid rgba(38, 168, 132, 140);
  border-top: 1px solid rgba(55, 200, 160, 190);
  font-weight: bold;
}
#AgentAssistantPanel #sendLLMButton:hover {
  background-color: rgba(38, 168, 132, 220);
  border-top: 1px solid rgba(90, 215, 175, 210);
}
#AgentAssistantPanel #sendLLMButton:pressed {
  background-color: rgba(20, 122, 99, 210);
  border: 1px solid rgba(20, 122, 99, 190);
}
#AgentAssistantPanel #sendLLMButton:disabled {
  color: rgba(180, 200, 220, 100);
  background-color: rgba(38, 168, 132, 40);
  border: 1px solid rgba(38, 168, 132, 30);
}
#AgentAssistantPanel #continueToChatButton {
  color: white;
  background-color: rgba(38, 168, 132, 185);
  border: 1px solid rgba(38, 168, 132, 140);
  border-top: 1px solid rgba(55, 200, 160, 190);
  font-weight: bold;
  min-height: 30px;
}
#AgentAssistantPanel #continueToChatButton:hover {
  background-color: rgba(38, 168, 132, 220);
}
#AgentAssistantPanel QCheckBox {
  color: #17242B;
  font: 14px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  spacing: 8px;
  min-height: 22px;
}
#AgentAssistantPanel QCheckBox::indicator {
  width: 18px;
  height: 18px;
  border-radius: 5px;
  border: 1px solid rgba(255, 255, 255, 60);
  background-color: rgba(255, 255, 255, 35);
}
#AgentAssistantPanel QCheckBox::indicator:hover {
  background-color: rgba(255, 255, 255, 55);
  border: 1px solid rgba(255, 255, 255, 80);
}
#AgentAssistantPanel QCheckBox::indicator:checked {
  background-color: rgba(38, 168, 132, 185);
  border: 1px solid rgba(38, 168, 132, 150);
}
#AgentAssistantPanel QCheckBox::indicator:checked:hover {
  background-color: rgba(38, 168, 132, 220);
  border: 1px solid rgba(38, 168, 132, 190);
}
#AgentAssistantPanel QCheckBox:disabled {
  color: rgba(180, 200, 220, 80);
}
#AgentAssistantPanel QCheckBox::indicator:disabled {
  border: 1px solid rgba(255, 255, 255, 20);
  background-color: rgba(255, 255, 255, 12);
}
#AgentAssistantPanel #statusLabel {
  font: bold 12px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  color: #0D2B24;
  padding: 2px 4px;
  background-color: rgba(255, 255, 255, 100);
  border-radius: 6px;
}
#AgentAssistantPanel #tokenUsageLabel {
  font: 11px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  color: #5F7078;
  padding: 0px 4px;
  background-color: transparent;
}
#AgentAssistantPanel #stopButton {
  color: white;
  background-color: rgba(220, 70, 60, 175);
  border: 1px solid rgba(220, 70, 60, 135);
  border-top: 1px solid rgba(255, 130, 120, 150);
  font-weight: bold;
}
#AgentAssistantPanel #stopButton:hover {
  background-color: rgba(220, 70, 60, 215);
  border-top: 1px solid rgba(255, 140, 130, 180);
}
#AgentAssistantPanel #stopButton:pressed {
  background-color: rgba(180, 45, 40, 205);
}
#AgentAssistantPanel #stopButton:disabled {
  color: rgba(180, 200, 220, 80);
  background-color: rgba(180, 45, 40, 30);
  border: 1px solid rgba(180, 45, 40, 20);
}
#AgentAssistantPanel #searchEdit {
  background-color: rgba(255, 255, 255, 90);
  border: 1px solid rgba(255, 255, 255, 55);
  border-radius: 8px;
  font: 12px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  padding: 3px 8px;
  min-height: 18px;
  color: #17242B;
}
#AgentAssistantPanel #searchNextBtn,
#AgentAssistantPanel #searchCloseBtn {
  padding: 2px 8px;
  min-height: 18px;
  font: 12px "LXGW WenKai Screen", "LXGW WenKai", "KaiTi", "Microsoft YaHei";
  background-color: rgba(255, 255, 255, 75);
  border: 1px solid rgba(255, 255, 255, 45);
  border-radius: 6px;
  color: #17242B;
}
#AgentAssistantPanel #searchNextBtn:hover,
#AgentAssistantPanel #searchCloseBtn:hover {
  background-color: rgba(255, 255, 255, 60);
}
)CSS");
}

// Dark panel for RDDark / Native-dark.
static QString agentPanelStylesheetDark()
{
  return QStringLiteral(R"CSS(
#AgentAssistantPanel {
  background-color: #1A2235;
}
#AgentAssistantPanel QLabel {
  font: 14px "Segoe UI", "Microsoft YaHei";
  color: #D0DAE8;
  background-color: transparent;
}
#AgentAssistantPanel #helpLabel {
  font: 12px "Segoe UI", "Microsoft YaHei";
  color: #8898B0;
}
#AgentAssistantPanel QGroupBox {
  border: 1px solid rgba(140, 185, 230, 80);
  border-radius: 10px;
  background-color: rgba(180, 210, 240, 50);
  margin-top: 16px;
  padding: 16px 12px 12px 12px;
}
#AgentAssistantPanel QGroupBox::title {
  subcontrol-origin: margin;
  left: 14px;
  padding: 0 8px;
  font: bold 14px "Segoe UI", "Microsoft YaHei";
  color: #9CC0E0;
}
#AgentAssistantPanel QLineEdit,
#AgentAssistantPanel QComboBox {
  background-color: rgba(160, 195, 235, 40);
  border: 1px solid rgba(140, 185, 230, 70);
  border-bottom: 1px solid rgba(130, 180, 225, 110);
  border-radius: 6px;
  font: 14px "Segoe UI", "Microsoft YaHei";
  padding: 5px 10px;
  color: #D0DAE8;
  min-height: 22px;
  selection-background-color: rgba(140, 200, 234, 160);
  selection-color: #1A2235;
}
#AgentAssistantPanel QComboBox::drop-down {
  border: none;
  width: 24px;
}
#AgentAssistantPanel QComboBox::down-arrow {
  image: none;
  border-left: 4px solid transparent;
  border-right: 4px solid transparent;
  border-top: 5px solid #90C8E8;
  margin-right: 8px;
}
#AgentAssistantPanel QComboBox QAbstractItemView {
  background-color: #222D42;
  border: 1px solid rgba(140, 185, 230, 80);
  border-radius: 6px;
  color: #D0DAE8;
  selection-background-color: rgba(140, 200, 234, 120);
  selection-color: #1A2235;
  padding: 4px;
  outline: none;
}
#AgentAssistantPanel QComboBox QAbstractItemView::item {
  padding: 4px 8px;
  min-height: 22px;
}
#AgentAssistantPanel QComboBox QAbstractItemView::item:hover {
  background-color: rgba(140, 200, 234, 60);
}
#AgentAssistantPanel QLineEdit:hover,
#AgentAssistantPanel QComboBox:hover {
  background-color: rgba(160, 195, 235, 55);
  border: 1px solid rgba(140, 185, 230, 100);
}
#AgentAssistantPanel QLineEdit:focus {
  border: 1px solid rgba(140, 200, 234, 120);
  border-bottom: 1px solid rgba(140, 200, 234, 180);
}
#AgentAssistantPanel QLineEdit:disabled,
#AgentAssistantPanel QComboBox:disabled {
  color: #546478;
  background-color: rgba(100, 130, 165, 18);
  border: 1px solid rgba(120, 155, 190, 30);
}
#AgentAssistantPanel QPlainTextEdit {
  background-color: rgba(160, 195, 235, 28);
  color: #C8D4E6;
  border: 1px solid rgba(140, 185, 230, 60);
  border-radius: 10px;
  padding: 10px;
  font: 11pt Consolas, "Cascadia Mono", "Courier New", monospace;
  selection-background-color: rgba(140, 200, 234, 160);
  selection-color: #1A2235;
}
#AgentAssistantPanel #chatLog {
  background-color: rgba(180, 210, 240, 30);
  border: 1px solid rgba(140, 185, 230, 55);
  border-radius: 10px;
  padding: 8px;
  font: 13px "Segoe UI", "Microsoft YaHei";
  color: #D0DAE8;
}
#AgentAssistantPanel #snapshotToggleBtn {
  background-color: transparent;
  border: none;
  color: #90C8E8;
  font: bold 13px "Segoe UI", "Microsoft YaHei";
  text-align: left;
  padding: 2px 4px;
}
#AgentAssistantPanel #snapshotToggleBtn:hover {
  color: #B0D8F0;
}
#AgentAssistantPanel QPushButton {
  background-color: rgba(160, 195, 235, 45);
  border: 1px solid rgba(140, 185, 230, 75);
  border-radius: 6px;
  color: #B8C8E0;
  font: 14px "Segoe UI", "Microsoft YaHei";
  padding: 5px 12px 6px 12px;
  min-height: 24px;
}
#AgentAssistantPanel QPushButton:hover {
  background-color: rgba(160, 195, 235, 70);
  border: 1px solid rgba(140, 185, 230, 110);
  color: #E0E8F4;
}
#AgentAssistantPanel QPushButton:pressed {
  color: #8AA0C0;
  background-color: rgba(140, 175, 215, 35);
}
#AgentAssistantPanel QPushButton:disabled {
  color: #4A5A70;
  background-color: rgba(120, 150, 185, 14);
  border: 1px solid rgba(120, 155, 190, 25);
}
#AgentAssistantPanel #sendLLMButton {
  color: #1A2235;
  background-color: rgba(140, 200, 234, 210);
  border: 1px solid rgba(140, 200, 234, 180);
  font-weight: bold;
}
#AgentAssistantPanel #sendLLMButton:hover {
  background-color: rgba(160, 215, 242, 230);
}
#AgentAssistantPanel #sendLLMButton:pressed {
  color: rgba(26, 34, 53, 180);
  background-color: rgba(120, 185, 225, 190);
}
#AgentAssistantPanel #sendLLMButton:disabled {
  color: #546478;
  background-color: rgba(120, 155, 190, 35);
  border: 1px solid rgba(120, 155, 190, 28);
}
#AgentAssistantPanel #continueToChatButton {
  color: #1A2235;
  background-color: rgba(140, 200, 234, 210);
  border: 1px solid rgba(140, 200, 234, 180);
  font-weight: bold;
  min-height: 30px;
}
#AgentAssistantPanel #continueToChatButton:hover {
  background-color: rgba(160, 215, 242, 230);
}
#AgentAssistantPanel QCheckBox {
  color: #B8C8E0;
  font: 14px "Segoe UI", "Microsoft YaHei";
  spacing: 8px;
  min-height: 22px;
}
#AgentAssistantPanel QCheckBox::indicator {
  width: 18px;
  height: 18px;
  border-radius: 5px;
  border: 1px solid rgba(140, 185, 230, 80);
  background-color: rgba(160, 195, 235, 30);
}
#AgentAssistantPanel QCheckBox::indicator:hover {
  background-color: rgba(160, 195, 235, 55);
  border: 1px solid rgba(140, 185, 230, 110);
}
#AgentAssistantPanel QCheckBox::indicator:checked {
  background-color: rgba(140, 200, 234, 210);
  border: 1px solid rgba(140, 200, 234, 190);
}
#AgentAssistantPanel QCheckBox::indicator:checked:hover {
  background-color: rgba(160, 215, 242, 230);
  border: 1px solid rgba(160, 215, 242, 210);
}
#AgentAssistantPanel QCheckBox:disabled {
  color: #4A5A70;
}
#AgentAssistantPanel QCheckBox::indicator:disabled {
  border: 1px solid rgba(120, 155, 190, 28);
  background-color: rgba(120, 150, 185, 14);
}
#AgentAssistantPanel #statusLabel {
  font: bold 12px "Segoe UI", "Microsoft YaHei";
  color: #90C8E8;
  padding: 2px 4px;
  background-color: transparent;
}
#AgentAssistantPanel #tokenUsageLabel {
  font: 11px "Segoe UI", "Microsoft YaHei";
  color: #6888A8;
  padding: 0px 4px;
  background-color: transparent;
}
#AgentAssistantPanel #stopButton {
  color: white;
  background-color: rgba(224, 100, 80, 200);
  border: 1px solid rgba(200, 80, 60, 180);
  font-weight: bold;
}
#AgentAssistantPanel #stopButton:hover {
  background-color: rgba(210, 85, 65, 220);
}
#AgentAssistantPanel #stopButton:pressed {
  background-color: rgba(190, 70, 50, 200);
}
#AgentAssistantPanel #stopButton:disabled {
  color: #4A5A70;
  background-color: rgba(120, 80, 70, 30);
  border: 1px solid rgba(120, 80, 70, 20);
}
#AgentAssistantPanel #searchEdit {
  border-radius: 4px;
  font: 12px "Segoe UI", "Microsoft YaHei";
  padding: 3px 6px;
  min-height: 18px;
}
#AgentAssistantPanel #searchNextBtn,
#AgentAssistantPanel #searchCloseBtn {
  padding: 2px 8px;
  min-height: 18px;
  font: 12px "Segoe UI", "Microsoft YaHei";
}
)CSS");
}

// Based on QFluentWidgets light QSS values.
// Based on ElaWidgetTools light theme (Liniyous/ElaWidgetTools, MIT).
// PrimaryNormal #0067C0, WindowBase #F3F3F3, BasicBase #FDFDFD, BasicBorder #E5E5E5,
// BasicText black, BasicDetailsText #878787.
static QString agentPanelStylesheetLight()
{
  return QStringLiteral(R"CSS(
#AgentAssistantPanel {
  background-color: #F3F3F3;
}
#AgentAssistantPanel QLabel {
  font: 14px "Segoe UI", "Microsoft YaHei";
  color: black;
  background-color: transparent;
}
#AgentAssistantPanel #helpLabel {
  font: 12px "Segoe UI", "Microsoft YaHei";
  color: #878787;
}
#AgentAssistantPanel #chatLog {
  background-color: rgba(255, 255, 255, 180);
  border: 1px solid #E5E5E5;
  border-radius: 8px;
  padding: 8px;
  font: 13px "Segoe UI", "Microsoft YaHei";
  color: black;
}
#AgentAssistantPanel #snapshotToggleBtn {
  background-color: transparent;
  border: none;
  color: #0067C0;
  font: bold 13px "Segoe UI", "Microsoft YaHei";
  text-align: left;
  padding: 2px 4px;
}
#AgentAssistantPanel QGroupBox {
  border: 1px solid #E5E5E5;
  border-radius: 8px;
  background-color: rgba(255, 255, 255, 160);
  margin-top: 16px;
  padding: 16px 12px 12px 12px;
}
#AgentAssistantPanel QGroupBox::title {
  subcontrol-origin: margin;
  left: 14px;
  padding: 0 8px;
  font: bold 14px "Segoe UI", "Microsoft YaHei";
  color: #5C5C5F;
}
#AgentAssistantPanel QLineEdit,
#AgentAssistantPanel QComboBox {
  background-color: #FDFDFD;
  border: 1px solid #E5E5E5;
  border-bottom: 1px solid #868686;
  border-radius: 5px;
  font: 14px "Segoe UI", "Microsoft YaHei";
  padding: 5px 10px;
  color: black;
  min-height: 22px;
  selection-background-color: #0067C0;
  selection-color: white;
}
#AgentAssistantPanel QLineEdit:hover,
#AgentAssistantPanel QComboBox:hover {
  background-color: #F3F3F3;
}
#AgentAssistantPanel QLineEdit:focus {
  border-bottom: 1px solid #0067C0;
}
#AgentAssistantPanel QLineEdit:disabled,
#AgentAssistantPanel QComboBox:disabled {
  color: #B6B6B6;
  background-color: #F5F5F5;
  border: 1px solid #E5E5E5;
}
#AgentAssistantPanel QPlainTextEdit {
  background-color: rgba(255, 255, 255, 160);
  color: black;
  border: 1px solid #E5E5E5;
  border-radius: 8px;
  padding: 10px;
  font: 11pt Consolas, "Cascadia Mono", "Courier New", monospace;
  selection-background-color: #0067C0;
  selection-color: white;
}
#AgentAssistantPanel QPushButton {
  background-color: rgba(204, 204, 204, 70);
  border: 1px solid #E5E5E5;
  border-radius: 5px;
  color: black;
  font: 14px "Segoe UI", "Microsoft YaHei";
  padding: 5px 12px 6px 12px;
  min-height: 24px;
}
#AgentAssistantPanel QPushButton:hover {
  background-color: #F3F3F3;
  border: 1px solid #DADADA;
}
#AgentAssistantPanel QPushButton:pressed {
  color: #5A5A5D;
  background-color: #F7F7F7;
}
#AgentAssistantPanel QPushButton:disabled {
  color: #B6B6B6;
  background-color: #F5F5F5;
  border: 1px solid #E5E5E5;
}
#AgentAssistantPanel #sendLLMButton {
  color: white;
  background-color: #0067C0;
  border: 1px solid #1975C5;
  font-weight: bold;
}
#AgentAssistantPanel #sendLLMButton:hover {
  background-color: #1975C5;
}
#AgentAssistantPanel #sendLLMButton:pressed {
  color: rgba(255, 255, 255, 200);
  background-color: #3183CA;
  border: 1px solid #3183CA;
}
#AgentAssistantPanel #sendLLMButton:disabled {
  color: #B6B6B6;
  background-color: #F5F5F5;
  border: 1px solid #F5F5F5;
}
#AgentAssistantPanel #continueToChatButton {
  color: white;
  background-color: #0067C0;
  border: 1px solid #1975C5;
  font-weight: bold;
  min-height: 30px;
}
#AgentAssistantPanel #continueToChatButton:hover {
  background-color: #1975C5;
}
#AgentAssistantPanel QCheckBox {
  color: black;
  font: 14px "Segoe UI", "Microsoft YaHei";
  spacing: 8px;
  min-height: 22px;
}
#AgentAssistantPanel QCheckBox::indicator {
  width: 18px;
  height: 18px;
  border-radius: 5px;
  border: 1px solid #A0A0A0;
  background-color: #FDFDFD;
}
#AgentAssistantPanel QCheckBox::indicator:hover {
  background-color: #F3F3F3;
}
#AgentAssistantPanel QCheckBox::indicator:checked {
  background-color: #0067C0;
  border: 1px solid #0067C0;
}
#AgentAssistantPanel QCheckBox::indicator:checked:hover {
  background-color: #1975C5;
  border: 1px solid #1975C5;
}
#AgentAssistantPanel QCheckBox:disabled {
  color: #B6B6B6;
}
#AgentAssistantPanel QCheckBox::indicator:disabled {
  border: 1px solid #A8A8A8;
  background-color: #F5F5F5;
}
#AgentAssistantPanel #statusLabel {
  font: bold 12px "Segoe UI", "Microsoft YaHei";
  color: #0067C0;
  padding: 2px 4px;
  background-color: transparent;
}
#AgentAssistantPanel #tokenUsageLabel {
  font: 11px "Segoe UI", "Microsoft YaHei";
  color: #878787;
  padding: 0px 4px;
  background-color: transparent;
}
#AgentAssistantPanel #stopButton {
  color: white;
  background-color: #D04030;
  border: 1px solid #C03828;
  font-weight: bold;
}
#AgentAssistantPanel #stopButton:hover {
  background-color: #C03828;
}
#AgentAssistantPanel #stopButton:pressed {
  background-color: #B03020;
}
#AgentAssistantPanel #stopButton:disabled {
  color: #B6B6B6;
  background-color: #F0E0E0;
  border: 1px solid #E5D5D5;
}
#AgentAssistantPanel #searchEdit {
  border-radius: 4px;
  font: 12px "Segoe UI", "Microsoft YaHei";
  padding: 3px 6px;
  min-height: 18px;
}
#AgentAssistantPanel #searchNextBtn,
#AgentAssistantPanel #searchCloseBtn {
  padding: 2px 8px;
  min-height: 18px;
  font: 12px "Segoe UI", "Microsoft YaHei";
}
)CSS");
}

static bool questionContainsAny(const QString &haystack, const std::initializer_list<QString> &subs)
{
  const QString h = haystack.toLower();
  for(const QString &s : subs)
  {
    if(s.isEmpty())
      continue;
    if(h.contains(s, Qt::CaseInsensitive))
      return true;
  }
  return false;
}

// MSVC: keep Chinese out of raw string literals (encoding); UTF-8 bytes are ASCII-safe here.
static QString utf8Zh(const char *utf8Bytes)
{
  return QString::fromUtf8(utf8Bytes);
}

static uint32_t selectAgentDataSkills(const QString &question)
{
  const QString q = question.trimmed();
  if(q.size() < 2)
    return AgentSkill_All;

  uint32_t m = 0;

  if(questionContainsAny(q, {lit("blend"), lit("mrt"), lit("alpha"), lit("premulti"),
                             utf8Zh("\xe6\xb7\xb7\xe5\x90\x88"), utf8Zh("\xe6\xb7\xb7\xe8\x89\xb2")}))
    m |= AgentSkill_BlendStencil | AgentSkill_OMTargets;

  if(questionContainsAny(q, {lit("depth"), lit("stencil"), lit("z-test"), lit("ztest"),
                             utf8Zh("\xe6\xb7\xb1\xe5\xba\xa6"), utf8Zh("\xe6\xa8\xa1\xe6\x9d\xbf")}))
    m |= AgentSkill_BlendStencil | AgentSkill_OMTargets;

  if(questionContainsAny(
         q, {lit("texture"), lit("srv"), lit("uav"), lit("sample"), lit("sampler"), lit("bind"),
             lit("descriptor"), lit("binding"), lit("resource"), utf8Zh("\xe8\xb4\xbf\xe5\x9b\xbe"),
             utf8Zh("\xe7\xba\xb9\xe7\x90\x86"), utf8Zh("\xe9\x87\x87\xe6\xa0\xb7"),
             utf8Zh("\xe6\x8f\x8f\xe8\xbf\xb0\xe7\xac\xa6"), utf8Zh("\xe7\xbb\x91\xe5\xae\x9a"),
             utf8Zh("\xe5\x9b\xbe\xe5\x83\x8f")}))
    m |= AgentSkill_Descriptors | AgentSkill_ResourceBrief | AgentSkill_OMTargets | AgentSkill_Shaders |
         AgentSkill_AssetCatalog;

  if(questionContainsAny(q, {lit("shader"), lit("disasm"), lit("disassemble"), lit("spirv"), lit("hlsl"),
                             lit("dxil"), lit("assembly"), utf8Zh("\xe7\x9d\x80\xe8\x89\xb2\xe5\x99\xa8"),
                             utf8Zh("\xe5\x8f\x8d\xe7\xbc\x96\xe8\xaf\x91"), utf8Zh("\xe6\xb1\x87\xe7\xbc\x96")}))
    m |= AgentSkill_Shaders | AgentSkill_Descriptors;

  if(questionContainsAny(q, {lit("model"), lit("mesh"), utf8Zh("\xe7\xbd\x91\xe6\xa0\xbc"),
                             utf8Zh("\xe6\xa8\xa1\xe5\x9e\x8b")}))
    m |= AgentSkill_AssetCatalog | AgentSkill_ViewGeom | AgentSkill_PipelineSummary;

  if(questionContainsAny(
         q,
         {lit("catalog"), utf8Zh("\xe6\xb8\x85\xe5\x8d\x95"), utf8Zh("\xe5\x88\x97\xe8\xa1\xa8"),
          utf8Zh("\xe6\x89\x80\xe6\x9c\x89\xe8\xb4\xbf\xe5\x9b\xbe"),
          utf8Zh("\xe6\x89\x80\xe6\x9c\x89\xe7\xba\xb9\xe7\x90\x86"),
          utf8Zh("\xe6\x89\x80\xe6\x9c\x89\xe8\xb5\x84\xe6\xba\x90"),
          utf8Zh("\xe8\xb5\x84\xe6\xba\x90\xe5\x88\x97\xe8\xa1\xa8"), lit("capture-wide"),
          utf8Zh("\xe5\x85\xa8\xe5\xb1\x80\xe8\xb5\x84\xe6\xba\x90")}))
    m |= AgentSkill_AssetCatalog;

  if(questionContainsAny(
         q, {lit("vertex"), lit("index"), lit("indices"), lit("vb"), lit("ib"), lit("layout"), lit("input"),
             utf8Zh("\xe9\xa1\xb6\xe7\x82\xb9"), utf8Zh("\xe7\xb4\xa2\xe5\xbc\x95"),
             utf8Zh("\xe5\x87\xa0\xe4\xbd\x95"), utf8Zh("\xe5\x9b\xbe\xe5\x85\x83"), lit("topology")}))
    m |= AgentSkill_ViewGeom | AgentSkill_PipelineSummary;

  if(questionContainsAny(
         q, {lit("marker"), lit("region"), lit("pass"), lit("eid"), lit("event"), lit("action"),
             utf8Zh("\xe6\xa0\x87\xe8\xae\xb0"), utf8Zh("\xe5\x8c\xba\xe5\x9f\x9f"),
             utf8Zh("\xe4\xba\x8b\xe4\xbb\xb6"), utf8Zh("\xe8\x8c\x83\xe5\x9b\xb4")}))
    m |= AgentSkill_Action | AgentSkill_DescriptorAccess;

  if(questionContainsAny(q, {lit("dispatch"), lit("compute"), lit("threadgroup"), lit("workgroup"),
                             utf8Zh("\xe8\xae\xa1\xe7\xae\x97"), utf8Zh("\xe8\xb0\x83\xe5\xba\xa6")}))
    m |= AgentSkill_Action | AgentSkill_PipelineSummary | AgentSkill_Descriptors | AgentSkill_Shaders;

  if(questionContainsAny(q, {lit("printf"), lit("print"), lit("message"),
                             utf8Zh("\xe8\xb0\x83\xe8\xaf\x95\xe8\xbe\x93\xe5\x87\xba")}))
    m |= AgentSkill_ShaderMessages;

  if(questionContainsAny(q, {lit("viewport"), lit("scissor"), lit("raster"),
                             utf8Zh("\xe8\xa7\x86\xe5\x8f\xa3"), utf8Zh("\xe8\xa3\x81\xe5\x89\xaa"),
                             utf8Zh("\xe5\x85\x89\xe6\xa0\x85")}))
    m |= AgentSkill_ViewGeom | AgentSkill_PipelineSummary;

  if(questionContainsAny(
         q, {lit("output"), lit("rtv"), lit("render target"), lit("fbo"), lit("framebuffer"),
             utf8Zh("\xe9\xa2\x9c\xe8\x89\xb2\xe8\xbe\x93\xe5\x87\xba"),
             utf8Zh("\xe6\xb8\xb2\xe6\x9f\x93\xe7\x9b\xae\xe6\xa0\x87"), lit("attachment")}))
    m |= AgentSkill_OMTargets | AgentSkill_BlendStencil;

  if(m == 0)
    return AgentSkill_All;

  m |= AgentSkill_CaptureMeta | AgentSkill_Action;
  return m;
}

static QString agentDataSkillMaskSummary(uint32_t mask)
{
  if(mask == AgentSkill_All)
    return lit("(all sections)");

  QStringList parts;
  if(mask & AgentSkill_CaptureMeta)
    parts << lit("capture");
  if(mask & AgentSkill_Action)
    parts << lit("action");
  if(mask & AgentSkill_PipelineSummary)
    parts << lit("pipeline");
  if(mask & AgentSkill_ViewGeom)
    parts << lit("view-geom");
  if(mask & AgentSkill_Shaders)
    parts << lit("shaders");
  if(mask & AgentSkill_OMTargets)
    parts << lit("om-targets");
  if(mask & AgentSkill_BlendStencil)
    parts << lit("blend-stencil");
  if(mask & AgentSkill_Descriptors)
    parts << lit("descriptors");
  if(mask & AgentSkill_DescriptorAccess)
    parts << lit("descriptor-access");
  if(mask & AgentSkill_ShaderMessages)
    parts << lit("shader-msgs");
  if(mask & AgentSkill_ResourceBrief)
    parts << lit("resource-brief");
  if(mask & AgentSkill_AssetCatalog)
    parts << lit("asset-catalog");
  return parts.join(lit(", "));
}

static QString rdcToQString(const rdcstr &s);
static QString resourceFormatToQString(const ResourceFormat &f);
static QString textureSwizzle4ToQString(const TextureSwizzle4 &sw);

static uint32_t agentMinU32(uint32_t a, uint32_t b)
{
  return a < b ? a : b;
}

static void appendCaptureAssetCatalog(QTextStream &ts, ICaptureContext &ctx)
{
  const rdcarray<TextureDescription> &texs = ctx.GetTextures();
  const rdcarray<BufferDescription> &bufs = ctx.GetBuffers();
  const rdcarray<ResourceDescription> &regs = ctx.GetResources();

  const uint32_t kMaxTex = 128;
  const uint32_t kMaxBuf = 128;
  const uint32_t kMaxReg = 256;

  ts << lit(
      "Capture-wide asset catalog (metadata from the opened .rdc; not texel/pixel or buffer bytes).\n");
  ts << lit(
      "For mesh/vertex data of the current draw, use the view-geom section (IB/VB, vertex input layout, "
      "topology).\n\n");

  ts << lit("Textures in capture: total=") << (uint32_t)texs.size() << lit(" (showing first ")
     << agentMinU32(kMaxTex, (uint32_t)texs.size()) << lit(")\n");
  for(uint32_t i = 0; i < kMaxTex && i < (uint32_t)texs.size(); i++)
  {
    const TextureDescription &t = texs[i];
    ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(t.resourceId)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(t.resourceId)) << lit("\" ") << t.width << lit("x") << t.height
       << lit("x") << t.depth << lit(" dim=") << t.dimension << lit(" type=")
       << rdcToQString(ToStr(t.type)) << lit(" fmt=") << resourceFormatToQString(t.format) << lit(" mips=")
       << t.mips << lit(" arraysize=") << t.arraysize << lit(" cubemap=")
       << (t.cubemap ? lit("yes") : lit("no")) << lit(" msSamp=") << t.msSamp << lit(" approxBytes=")
       << (qulonglong)t.byteSize << lit(" category=") << rdcToQString(ToStr(t.creationFlags)) << lit("\n");
  }
  if((uint32_t)texs.size() > kMaxTex)
    ts << lit("  ... ") << ((uint32_t)texs.size() - kMaxTex) << lit(" more textures omitted\n");
  ts << lit("\n");

  ts << lit("Buffers in capture: total=") << (uint32_t)bufs.size() << lit(" (showing first ")
     << agentMinU32(kMaxBuf, (uint32_t)bufs.size()) << lit(")\n");
  for(uint32_t i = 0; i < kMaxBuf && i < (uint32_t)bufs.size(); i++)
  {
    const BufferDescription &b = bufs[i];
    ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(b.resourceId)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(b.resourceId)) << lit("\" bytes=") << (qulonglong)b.length
       << lit(" category=") << rdcToQString(ToStr(b.creationFlags)) << lit(" gpuAddr=0x")
       << QString::number((qulonglong)b.gpuAddress, 16) << lit("\n");
  }
  if((uint32_t)bufs.size() > kMaxBuf)
    ts << lit("  ... ") << ((uint32_t)bufs.size() - kMaxBuf) << lit(" more buffers omitted\n");
  ts << lit("\n");

  ts << lit("Resource registry (all ResourceId entries): total=") << (uint32_t)regs.size()
     << lit(" (showing first ") << agentMinU32(kMaxReg, (uint32_t)regs.size()) << lit(")\n");
  for(uint32_t i = 0; i < kMaxReg && i < (uint32_t)regs.size(); i++)
  {
    const ResourceDescription &r = regs[i];
    ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(r.resourceId)) << lit(" type=")
       << rdcToQString(ToStr(r.type)) << lit(" name=\"") << rdcToQString(ctx.GetResourceName(r.resourceId))
       << lit("\" autoGen=") << (ctx.IsAutogeneratedName(r.resourceId) ? lit("yes") : lit("no"))
       << lit("\n");
  }
  if((uint32_t)regs.size() > kMaxReg)
    ts << lit("  ... ") << ((uint32_t)regs.size() - kMaxReg) << lit(" more registry entries omitted\n");
  ts << lit("\n");
}

static QString rdcToQString(const rdcstr &s)
{
  if(s.empty())
    return QString();
  return QString::fromUtf8(s.data(), (int)s.size());
}

// ResourceFormat / TextureSwizzle4 have no DoStringise in the UI link set; use API-friendly forms.
static QString resourceFormatToQString(const ResourceFormat &f)
{
  return rdcToQString(f.Name());
}

static QString textureSwizzle4ToQString(const TextureSwizzle4 &sw)
{
  return rdcToQString(ToStr(sw.red)) + rdcToQString(ToStr(sw.green)) + rdcToQString(ToStr(sw.blue)) +
         rdcToQString(ToStr(sw.alpha));
}

static rdcstr qstrToRdc(const QString &s)
{
  QByteArray b = s.toUtf8();
  return rdcstr(b.data(), b.size());
}

static void appendShaderReflectionBindingSummary(rdcstr &out, const ShaderReflection *refl)
{
  if(!refl)
    return;

  out += "Shader reflection layout: ";
  out += ToStr(refl->encoding);
  out += " entry=";
  out += refl->entryPoint;
  out += " dispatchThreads ";
  out += ToStr(refl->dispatchThreadsDimension[0]);
  out += " ";
  out += ToStr(refl->dispatchThreadsDimension[1]);
  out += " ";
  out += ToStr(refl->dispatchThreadsDimension[2]);
  out += "\n";

  const size_t kMaxList = 32;
  out += "  Constant blocks (";
  out += ToStr((uint32_t)refl->constantBlocks.size());
  out += "):\n";
  for(size_t i = 0; i < refl->constantBlocks.size() && i < kMaxList; i++)
  {
    const ConstantBlock &cb = refl->constantBlocks[i];
    out += "    [";
    out += ToStr((uint32_t)i);
    out += "] ";
    out += cb.name;
    out += " set/space=";
    out += ToStr(cb.fixedBindSetOrSpace);
    out += " bind=";
    out += ToStr(cb.fixedBindNumber);
    out += " bytes=";
    out += ToStr(cb.byteSize);
    out += "\n";
  }
  if(refl->constantBlocks.size() > kMaxList)
    out += "    ... more constant blocks omitted\n";

  out += "  Read-only resources (";
  out += ToStr((uint32_t)refl->readOnlyResources.size());
  out += "):\n";
  for(size_t i = 0; i < refl->readOnlyResources.size() && i < kMaxList; i++)
  {
    const ShaderResource &sr = refl->readOnlyResources[i];
    out += "    [";
    out += ToStr((uint32_t)i);
    out += "] ";
    out += sr.name;
    out += " ";
    out += ToStr(sr.descriptorType);
    out += " ";
    out += ToStr(sr.textureType);
    out += " bind=";
    out += ToStr(sr.fixedBindNumber);
    out += "\n";
  }
  if(refl->readOnlyResources.size() > kMaxList)
    out += "    ... more read-only resources omitted\n";

  out += "  Read/write resources (";
  out += ToStr((uint32_t)refl->readWriteResources.size());
  out += "):\n";
  for(size_t i = 0; i < refl->readWriteResources.size() && i < kMaxList; i++)
  {
    const ShaderResource &sr = refl->readWriteResources[i];
    out += "    [";
    out += ToStr((uint32_t)i);
    out += "] ";
    out += sr.name;
    out += " ";
    out += ToStr(sr.descriptorType);
    out += " ";
    out += ToStr(sr.textureType);
    out += " bind=";
    out += ToStr(sr.fixedBindNumber);
    out += "\n";
  }
  if(refl->readWriteResources.size() > kMaxList)
    out += "    ... more read/write resources omitted\n";

  out += "  Samplers (";
  out += ToStr((uint32_t)refl->samplers.size());
  out += ")\n";
}

static void appendSamplerDetailLine(QTextStream &ts, ICaptureContext &ctx, const SamplerDescriptor &s)
{
  if(s.type != DescriptorType::Sampler && s.type != DescriptorType::ImageSampler)
    return;
  ts << lit("           Sampler: obj=") << rdcToQString(ToStr(s.object)) << lit(" \"")
     << rdcToQString(ctx.GetResourceName(s.object)) << lit("\" filter=")
     << rdcToQString(ToStr(s.filter)) << lit(" addrUVW=") << rdcToQString(ToStr(s.addressU))
     << lit("/") << rdcToQString(ToStr(s.addressV)) << lit("/") << rdcToQString(ToStr(s.addressW))
     << lit(" compare=") << rdcToQString(ToStr(s.compareFunction)) << lit(" lod=[")
     << s.minLOD << lit(",") << s.maxLOD << lit("] bias=") << s.mipBias << lit(" maxAniso=")
     << s.maxAnisotropy << lit(" unormCoords=") << (s.unnormalized ? lit("yes") : lit("no"))
     << lit(" seamlessCube=") << (s.seamlessCubemaps ? lit("yes") : lit("no")) << lit("\n");
}

static void appendDescriptorDetailLine(QTextStream &ts, ICaptureContext &ctx, const Descriptor &d)
{
  if(d.resource == ResourceId() && d.type == DescriptorType::Unknown)
  {
    ts << lit("           (empty descriptor)\n");
    return;
  }

  ts << lit("           Descriptor: type=") << rdcToQString(ToStr(d.type)) << lit(" viewFmt=")
     << resourceFormatToQString(d.format) << lit(" texType=") << rdcToQString(ToStr(d.textureType))
     << lit(" view=") << rdcToQString(ToStr(d.view)) << lit(" mips[") << (uint32_t)d.firstMip
     << lit("+") << (uint32_t)d.numMips << lit("] slices[") << d.firstSlice << lit("+")
     << d.numSlices << lit("] swizzle=") << textureSwizzle4ToQString(d.swizzle) << lit(" minLodClamp=")
     << d.minLODClamp << lit(" bufOff=") << d.byteOffset << lit(" bufSize=") << d.byteSize
     << lit(" structCount=") << d.bufferStructCount << lit(" elemBytes=") << d.elementByteSize
     << lit("\n");
  if(d.resource != ResourceId())
  {
    ts << lit("           Resource: id=") << rdcToQString(ToStr(d.resource)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(d.resource)) << lit("\"");
    const TextureDescription *tex = ctx.GetTexture(d.resource);
    if(tex)
    {
      ts << lit("  TEXTURE storageFmt=") << resourceFormatToQString(tex->format) << lit(" size=")
         << tex->width << lit("x") << tex->height << lit("x") << tex->depth << lit(" mips=")
         << tex->mips << lit(" arraysize=") << tex->arraysize << lit(" dim=") << tex->dimension
         << lit(" type=") << rdcToQString(ToStr(tex->type));
    }
    else if(const BufferDescription *buf = ctx.GetBuffer(d.resource))
    {
      ts << lit("  BUFFER length=") << buf->length;
    }
    ts << lit("\n");
  }
  if(d.secondary != ResourceId())
  {
    ts << lit("           Secondary: id=") << rdcToQString(ToStr(d.secondary)) << lit(" \"")
       << rdcToQString(ctx.GetResourceName(d.secondary)) << lit("\"\n");
  }
}

static rdcstr collectShaderDisassembly(ICaptureContext &ctx, IReplayController *r)
{
  rdcstr ret;
  if(!r)
    return ret;

  const PipeState &pipe = ctx.CurPipelineState();
  if(!pipe.IsCaptureLoaded())
    return ret;

  ResourceId pipeObj = pipe.GetGraphicsPipelineObject();
  if(pipeObj == ResourceId())
    pipeObj = pipe.GetComputePipelineObject();

  const size_t kMaxDefaultDisasm = 100000;
  const size_t kMaxAltDisasm = 48000;
  const size_t kMaxExtraTargets = 2;

  rdcarray<rdcstr> targets = r->GetDisassemblyTargets(pipeObj != ResourceId());

  for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
  {
    ShaderStage st = (ShaderStage)s;
    const ShaderReflection *refl = pipe.GetShaderReflection(st);
    if(!refl || refl->resourceId == ResourceId())
      continue;

    ret += "=== Shader stage ";
    ret += ToStr(st);
    ret += " id=";
    ret += ToStr(refl->resourceId);
    ret += " ===\n";
    appendShaderReflectionBindingSummary(ret, refl);
    ret += "\n";

    {
      rdcstr disasm = r->DisassembleShader(pipeObj, refl, rdcstr());
      if(disasm.size() > kMaxDefaultDisasm)
      {
        disasm.resize(kMaxDefaultDisasm);
        disasm += "\n... (default disassembly truncated)\n";
      }
      ret += "--- Default disassembly ---\n";
      ret += disasm;
      ret += "\n";
    }

    size_t extras = 0;
    // Index 0 is usually the same as DisassembleShader(..., "") (e.g. D3D12 "DXBC/DXIL"); start at 1
    for(size_t ti = 1; ti < targets.size() && extras < kMaxExtraTargets; ti++)
    {
      const rdcstr &target = targets[ti];

      rdcstr disasm = r->DisassembleShader(pipeObj, refl, target);
      if(disasm.empty() || disasm.size() < 32)
        continue;
      if(disasm.contains("Invalid Shader"))
        continue;

      if(disasm.size() > kMaxAltDisasm)
      {
        disasm.resize(kMaxAltDisasm);
        disasm += "\n... (alternate disassembly truncated)\n";
      }
      ret += "--- Disassembly target: ";
      ret += target;
      ret += " ---\n";
      ret += disasm;
      ret += "\n";
      extras++;
    }

    ret += "\n";
  }

  if(ret.empty())
    ret = "(No shader reflection / disassembly available at this event.)\n";

  return ret;
}

static QString systemPrompt()
{
  return lit(
      "You are an expert GPU graphics debugger helping a user inside RenderDoc.\n"
      "You receive a structured text dump of the capture and current event. The dump begins with "
      "\"Context data skills\" listing which sections were included based on the user's question "
      "(keywords route to blend, OM targets, descriptors, shaders, geometry, etc.; an empty question "
      "requests all sections).\n"
      "When present, sections may include: API metadata, action parameters, marker/event hierarchy, "
      "viewports/scissors, index/vertex buffers, vertex layout, render targets and depth, full per-RT "
      "color/alpha blend equations, stencil summary, per-stage descriptors with texture/buffer details "
      "(view format, mips, slices, swizzle, backing sizes) and sampler state (filter, address, LOD, "
      "compare), descriptor access, shader messages, a brief bound-resource list, a capture-wide "
      "texture/buffer/resource registry (dimensions, formats, categories; not raw texels or buffer "
      "bytes), plus optional multi-target shader disassembly appended separately when enabled.\n"
      "Explain clearly, cite which draw/dispatch or shader stage you refer to, and avoid guessing "
      "when information is missing.\n"
      "When referencing specific events, always write them as 'EID 1234' (the word EID followed by "
      "the numeric event ID). These become clickable links so the user can navigate directly "
      "to that event in the Event Browser.\n\n"
      "=== AVAILABLE TOOLS ===\n"
      "You have three tools to pull more capture data on demand: list_events, scan_pass and "
      "fetch_eid. This session exposes them through the API's native function-calling interface; "
      "invoke them as real tool calls whenever possible. If native tool calling is unavailable "
      "for this provider, write the equivalent bracket command on its own line instead -- "
      "[LIST_EVENTS], [SCAN_PASS nnnn] or [FETCH_EID nnnn] -- and the system executes it and "
      "returns the result automatically.\n\n"
      "1) list_events  -- Returns the full frame event tree (all markers, draw calls, dispatches, "
      "clears, copies, presents) in a hierarchical text format. Use this FIRST when the user asks "
      "to reverse-engineer or summarize the entire rendering pipeline. The tree shows marker names, "
      "EIDs, action types, draw parameters, and render target IDs.\n\n"
      "2) scan_pass(marker_eid)  -- Scans a marker/pass at the given EID. Collects all leaf draw/"
      "dispatch/clear/copy events under that marker, samples up to 8 of them, and returns a "
      "condensed pipeline snapshot for each sampled event. Use this to understand what a render "
      "pass does (its shaders, render targets, blend state, etc.) without fetching every single "
      "draw.\n\n"
      "3) fetch_eid(eid)  -- Fetches the full pipeline snapshot for a single event. Use this when "
      "you need detailed state for one specific draw call (vertex/index buffers, full descriptor "
      "tables, shader disassembly if enabled, etc.).\n\n"
      "=== REVERSE-ENGINEERING THE RENDERING PIPELINE ===\n"
      "When the user asks you to reverse-engineer or analyze the full rendering pipeline, follow "
      "this workflow:\n"
      "  Step 1: Call list_events to get the full frame event tree.\n"
      "  Step 2: Identify the top-level render passes from the marker hierarchy.\n"
      "  Step 3: Call scan_pass on each major pass to sample its draws and understand "
      "what each pass renders (GBuffer, shadows, lighting, post-processing, UI, etc.).\n"
      "  Step 4: If needed, call fetch_eid on specific draws for deeper detail.\n"
      "  Step 5: Synthesize everything into a clear, ordered pipeline summary:\n"
      "    - What each pass does (purpose, render targets, shaders)\n"
      "    - The rendering order (what happens first, second, etc.)\n"
      "    - How passes connect (which pass's output becomes another's input)\n"
      "    - Final composition and present\n"
      "Do NOT ask the user to navigate manually. Do NOT stop after listing events -- always analyze "
      "the passes automatically and provide a complete pipeline breakdown.\n");
}

// ===== native function-calling (tools) support ===============================
//
// m_apiMessages holds the conversation in OpenAI message shape (system/user/assistant
// messages, assistant "tool_calls" arrays, "tool" result messages). Each provider
// payload builder serialises that canonical form into its own protocol.

struct AgentToolSpec
{
  const char *name;
  const char *desc;
};

static const AgentToolSpec kAgentTools[] = {
    {"list_events",
     "Return the full frame event tree: all markers, draw calls, dispatches, clears, copies and "
     "presents with their EIDs, in hierarchical text form. Use FIRST when summarizing or "
     "reverse-engineering the whole rendering pipeline."},
    {"scan_pass",
     "Scan a marker/pass at the given EID: collects all leaf draw/dispatch/clear/copy events under "
     "that marker, samples up to 8 of them and returns a condensed pipeline snapshot for each. Use "
     "to understand what a render pass does without fetching every draw."},
    {"fetch_eid",
     "Fetch the full pipeline snapshot for a single event EID: bound buffers, descriptors, shader "
     "disassembly (if enabled), render targets and state. Use for deep detail on one specific "
     "draw."},
};

static QJsonObject toolParameterSchema(const QString &name)
{
  QJsonObject schema;
  schema[lit("type")] = lit("object");
  QJsonObject props;
  QJsonArray required;
  if(name == lit("scan_pass"))
  {
    QJsonObject p;
    p[lit("type")] = lit("integer");
    p[lit("description")] = lit("EID of the marker/pass to scan");
    props[lit("marker_eid")] = p;
    required.append(lit("marker_eid"));
  }
  else if(name == lit("fetch_eid"))
  {
    QJsonObject p;
    p[lit("type")] = lit("integer");
    p[lit("description")] = lit("EID of the draw/dispatch to fetch");
    props[lit("eid")] = p;
    required.append(lit("eid"));
  }
  schema[lit("properties")] = props;
  schema[lit("required")] = required;
  return schema;
}

static QJsonArray openAIToolsJson()
{
  QJsonArray tools;
  for(const AgentToolSpec &t : kAgentTools)
  {
    QJsonObject fn;
    fn[lit("name")] = QString::fromUtf8(t.name);
    fn[lit("description")] = QString::fromUtf8(t.desc);
    fn[lit("parameters")] = toolParameterSchema(QString::fromUtf8(t.name));
    QJsonObject o;
    o[lit("type")] = lit("function");
    o[lit("function")] = fn;
    tools.append(o);
  }
  return tools;
}

static QJsonArray anthropicToolsJson()
{
  QJsonArray tools;
  for(const AgentToolSpec &t : kAgentTools)
  {
    QJsonObject o;
    o[lit("name")] = QString::fromUtf8(t.name);
    o[lit("description")] = QString::fromUtf8(t.desc);
    o[lit("input_schema")] = toolParameterSchema(QString::fromUtf8(t.name));
    tools.append(o);
  }
  return tools;
}

static QJsonArray geminiFunctionDeclarations()
{
  QJsonArray decls;
  for(const AgentToolSpec &t : kAgentTools)
  {
    QJsonObject o;
    o[lit("name")] = QString::fromUtf8(t.name);
    o[lit("description")] = QString::fromUtf8(t.desc);
    o[lit("parameters")] = toolParameterSchema(QString::fromUtf8(t.name));
    decls.append(o);
  }
  return decls;
}

static QJsonObject toolArgsFromString(const QString &json)
{
  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  return doc.isObject() ? doc.object() : QJsonObject();
}

static QJsonArray openAIMessagesJson(const QJsonArray &msgs)
{
  QJsonArray out;
  for(const QJsonValue &v : msgs)
  {
    QJsonObject m = v.toObject();
    QJsonObject c;
    c[lit("role")] = m[lit("role")];
    if(m.contains(lit("content")))
      c[lit("content")] = m[lit("content")];
    if(m.contains(lit("tool_call_id")))
      c[lit("tool_call_id")] = m[lit("tool_call_id")];
    if(m.contains(lit("tool_calls")))
    {
      QJsonArray calls;
      for(const QJsonValue &tv : m[lit("tool_calls")].toArray())
      {
        QJsonObject tc = tv.toObject();
        QJsonObject fn;
        fn[lit("name")] = tc[lit("function")].toObject()[lit("name")];
        fn[lit("arguments")] = tc[lit("function")].toObject()[lit("arguments")];
        QJsonObject o;
        o[lit("id")] = tc[lit("id")];
        o[lit("type")] = lit("function");
        o[lit("function")] = fn;
        calls.append(o);
      }
      c[lit("tool_calls")] = calls;
    }
    out.append(c);
  }
  return out;
}

static QJsonArray anthropicMessagesFromCanonical(const QJsonArray &msgs)
{
  QJsonArray out;
  QJsonArray pendingToolResults;
  auto flushToolResults = [&out, &pendingToolResults]() {
    if(pendingToolResults.isEmpty())
      return;
    QJsonArray blocks;
    for(const QJsonValue &v : pendingToolResults)
    {
      QJsonObject t = v.toObject();
      QJsonObject b;
      b[lit("type")] = lit("tool_result");
      b[lit("tool_use_id")] = t[lit("tool_call_id")];
      b[lit("content")] = t[lit("content")];
      blocks.append(b);
    }
    QJsonObject u;
    u[lit("role")] = lit("user");
    u[lit("content")] = blocks;
    out.append(u);
    pendingToolResults = QJsonArray();
  };

  for(const QJsonValue &v : msgs)
  {
    QJsonObject m = v.toObject();
    QString role = m[lit("role")].toString();
    if(role == lit("system"))
      continue;
    if(role == lit("tool"))
    {
      pendingToolResults.append(m);
      continue;
    }
    flushToolResults();
    if(role == lit("assistant"))
    {
      QJsonArray blocks;
      QString txt = m[lit("content")].toString();
      if(!txt.isEmpty())
      {
        QJsonObject b;
        b[lit("type")] = lit("text");
        b[lit("text")] = txt;
        blocks.append(b);
      }
      for(const QJsonValue &tv : m[lit("tool_calls")].toArray())
      {
        QJsonObject tc = tv.toObject();
        QJsonObject fn = tc[lit("function")].toObject();
        QJsonObject b;
        b[lit("type")] = lit("tool_use");
        b[lit("id")] = tc[lit("id")];
        b[lit("name")] = fn[lit("name")];
        b[lit("input")] = toolArgsFromString(fn[lit("arguments")].toString());
        blocks.append(b);
      }
      if(blocks.isEmpty())
        continue;
      QJsonObject am;
      am[lit("role")] = lit("assistant");
      am[lit("content")] = blocks;
      out.append(am);
      continue;
    }
    if(role == lit("user"))
    {
      QJsonObject um;
      um[lit("role")] = lit("user");
      QJsonValue cval = m[lit("content")];
      if(cval.isString())
      {
        um[lit("content")] = cval;
      }
      else if(cval.isArray())
      {
        QJsonArray blocks;
        for(const QJsonValue &pv : cval.toArray())
        {
          QJsonObject part = pv.toObject();
          if(part[lit("type")].toString() == lit("text"))
          {
            QJsonObject b;
            b[lit("type")] = lit("text");
            b[lit("text")] = part[lit("text")];
            blocks.append(b);
          }
          else if(part[lit("type")].toString() == lit("image_url"))
          {
            QString url = part[lit("image_url")].toObject()[lit("url")].toString();
            const QString prefix = lit("data:image/png;base64,");
            if(url.startsWith(prefix))
            {
              QJsonObject src;
              src[lit("type")] = lit("base64");
              src[lit("media_type")] = lit("image/png");
              src[lit("data")] = url.mid(prefix.size());
              QJsonObject b;
              b[lit("type")] = lit("image");
              b[lit("source")] = src;
              blocks.append(b);
            }
          }
        }
        um[lit("content")] = blocks;
      }
      out.append(um);
      continue;
    }
    out.append(m);
  }
  flushToolResults();
  return out;
}

static QJsonArray geminiContentsFromCanonical(const QJsonArray &msgs)
{
  QJsonArray out;
  for(const QJsonValue &v : msgs)
  {
    QJsonObject m = v.toObject();
    QString role = m[lit("role")].toString();
    if(role == lit("system"))
      continue;
    QJsonObject turn;
    turn[lit("role")] =
        (role == lit("assistant")) ? QJsonValue(lit("model")) : QJsonValue(lit("user"));
    QJsonArray parts;
    QJsonValue cval = m[lit("content")];
    if(cval.isString() && !cval.toString().isEmpty())
    {
      QJsonObject tp;
      tp[lit("text")] = cval;
      parts.append(tp);
    }
    else if(cval.isArray())
    {
      for(const QJsonValue &pv : cval.toArray())
      {
        QJsonObject part = pv.toObject();
        if(part[lit("type")].toString() == lit("text"))
        {
          QJsonObject tp;
          tp[lit("text")] = part[lit("text")];
          parts.append(tp);
        }
        else if(part[lit("type")].toString() == lit("image_url"))
        {
          QString url = part[lit("image_url")].toObject()[lit("url")].toString();
          const QString prefix = lit("data:image/png;base64,");
          if(url.startsWith(prefix))
          {
            QJsonObject idata;
            idata[lit("mime_type")] = lit("image/png");
            idata[lit("data")] = url.mid(prefix.size());
            QJsonObject ip;
            ip[lit("inline_data")] = idata;
            parts.append(ip);
          }
        }
      }
    }
    if(role == lit("assistant"))
    {
      for(const QJsonValue &tv : m[lit("tool_calls")].toArray())
      {
        QJsonObject tc = tv.toObject();
        QJsonObject fn = tc[lit("function")].toObject();
        QJsonObject fc;
        fc[lit("name")] = fn[lit("name")];
        fc[lit("args")] = toolArgsFromString(fn[lit("arguments")].toString());
        QJsonObject fp;
        fp[lit("functionCall")] = fc;
        parts.append(fp);
      }
    }
    if(role == lit("tool"))
    {
      QJsonObject fr;
      fr[lit("name")] = m[lit("rd_name")].toString();
      QJsonObject resp;
      resp[lit("result")] = m[lit("content")].toString();
      fr[lit("response")] = resp;
      QJsonObject fp;
      fp[lit("functionResponse")] = fr;
      parts.append(fp);
    }
    if(parts.isEmpty())
      continue;
    turn[lit("parts")] = parts;
    out.append(turn);
  }
  return out;
}

static QByteArray makeOpenAIChatPayload(const QString &model, const QJsonArray &msgs,
                                        bool withTools)
{
  QJsonObject root;
  root[lit("model")] = model;
  root[lit("messages")] = openAIMessagesJson(msgs);
  if(withTools)
    root[lit("tools")] = openAIToolsJson();
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QByteArray makeAnthropicPayload(const QString &model, const QJsonArray &msgs, bool withTools)
{
  QJsonObject root;
  root[lit("model")] = model;
  root[lit("max_tokens")] = 8192;
  root[lit("system")] = systemPrompt();
  root[lit("messages")] = anthropicMessagesFromCanonical(msgs);
  if(withTools)
    root[lit("tools")] = anthropicToolsJson();
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QByteArray makeGeminiPayload(const QJsonArray &msgs, bool withTools)
{
  QJsonObject root;
  QJsonObject sit;
  sit[lit("text")] = systemPrompt();
  QJsonArray siParts;
  siParts.append(sit);
  QJsonObject si;
  si[lit("parts")] = siParts;
  root[lit("systemInstruction")] = si;
  root[lit("contents")] = geminiContentsFromCanonical(msgs);
  if(withTools)
  {
    QJsonObject td;
    td[lit("functionDeclarations")] = geminiFunctionDeclarations();
    QJsonArray tools;
    tools.append(td);
    root[lit("tools")] = tools;
  }
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QString truncateToolBody(QString body)
{
  const int kMaxToolBody = 450000;
  if(body.size() > kMaxToolBody)
  {
    body.truncate(kMaxToolBody);
    body += lit("\n\n[Truncated for API limits.]\n");
  }
  return body;
}

static QString openAIMessageContentToString(const QJsonValue &contentVal, QString &err)
{
  if(contentVal.isString())
  {
    QString s = contentVal.toString();
    if(!s.isEmpty())
      return s;
  }
  else if(contentVal.isArray())
  {
    QString pieces;
    for(const QJsonValue &part : contentVal.toArray())
    {
      QJsonObject o = part.toObject();
      if(o[lit("type")].toString() == lit("text"))
        pieces += o[lit("text")].toString();
    }
    if(!pieces.isEmpty())
      return pieces;
  }
  err = lit("No message content in response");
  return QString();
}

// Normalised provider response: visible text plus any native tool calls.
struct AgentLLMResult
{
  QString text;
  QString err;
  QJsonArray toolCalls;    // entries: {"id":string, "name":string, "args":object}
  bool hasToolCalls() const { return !toolCalls.isEmpty(); }
};

static AgentLLMResult parseOpenAIResponse(const QJsonObject &root)
{
  AgentLLMResult r;
  if(root.contains(lit("error")))
  {
    QJsonObject e = root[lit("error")].toObject();
    r.err = e[lit("message")].toString();
    if(r.err.isEmpty())
      r.err = lit("Unknown API error");
    return r;
  }
  QJsonArray choices = root[lit("choices")].toArray();
  if(choices.isEmpty())
  {
    r.err = lit("Empty choices in response");
    return r;
  }
  QJsonObject msg = choices[0].toObject()[lit("message")].toObject();
  QString contentErr;
  r.text = openAIMessageContentToString(msg[lit("content")], contentErr);
  for(const QJsonValue &v : msg[lit("tool_calls")].toArray())
  {
    QJsonObject tc = v.toObject();
    QJsonObject fn = tc[lit("function")].toObject();
    QJsonObject norm;
    norm[lit("id")] = tc[lit("id")].toString();
    norm[lit("name")] = fn[lit("name")].toString();
    norm[lit("args")] = toolArgsFromString(fn[lit("arguments")].toString());
    r.toolCalls.append(norm);
  }
  return r;
}

static AgentLLMResult parseAnthropicResponse(const QJsonObject &root)
{
  AgentLLMResult r;
  if(root.contains(lit("error")))
  {
    QJsonObject e = root[lit("error")].toObject();
    r.err = e[lit("message")].toString();
    if(r.err.isEmpty())
      r.err = lit("Anthropic API error");
    return r;
  }
  QStringList texts;
  for(const QJsonValue &v : root[lit("content")].toArray())
  {
    QJsonObject blk = v.toObject();
    QString type = blk[lit("type")].toString();
    if(type == lit("text"))
    {
      texts << blk[lit("text")].toString();
    }
    else if(type == lit("tool_use"))
    {
      QJsonObject norm;
      norm[lit("id")] = blk[lit("id")].toString();
      norm[lit("name")] = blk[lit("name")].toString();
      norm[lit("args")] = blk[lit("input")].toObject();
      r.toolCalls.append(norm);
    }
  }
  r.text = texts.join(lit("\n"));
  if(r.text.isEmpty() && !r.hasToolCalls())
    r.err = lit("No text block in Anthropic response");
  return r;
}

static AgentLLMResult parseGeminiResponse(const QJsonObject &root)
{
  AgentLLMResult r;
  if(root.contains(lit("error")))
  {
    QJsonObject e = root[lit("error")].toObject();
    r.err = e[lit("message")].toString();
    return r;
  }
  QJsonArray cands = root[lit("candidates")].toArray();
  if(cands.isEmpty())
  {
    r.err = lit("No candidates in Gemini response");
    return r;
  }
  QJsonArray parts = cands[0].toObject()[lit("content")].toObject()[lit("parts")].toArray();
  if(parts.isEmpty())
  {
    r.err = lit("No parts in Gemini response");
    return r;
  }
  QStringList texts;
  int callIdx = 0;
  for(const QJsonValue &v : parts)
  {
    QJsonObject part = v.toObject();
    if(part.contains(lit("text")))
    {
      texts << part[lit("text")].toString();
    }
    else if(part.contains(lit("functionCall")))
    {
      QJsonObject fc = part[lit("functionCall")].toObject();
      QJsonObject norm;
      norm[lit("id")] = QStringLiteral("gcall_%1").arg(callIdx++);
      norm[lit("name")] = fc[lit("name")].toString();
      norm[lit("args")] = fc[lit("args")].toObject();
      r.toolCalls.append(norm);
    }
  }
  r.text = texts.join(lit("\n"));
  return r;
}

AgentAssistantPanel::AgentAssistantPanel(ICaptureContext &ctx, QWidget *parent)
    : QFrame(parent), ui(new Ui::AgentAssistantPanel), m_Ctx(ctx)
{
  ui->setupUi(this);
  m_helpTextFull = ui->helpLabel->text();
  setFrameShape(QFrame::NoFrame);
  setAutoFillBackground(false);

  {
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList tryPaths;
    tryPaths << appDir + lit("/agent_bg.jpg");
    tryPaths << lit("E:/ProjectGithub/renderdoc/qrenderdoc/Resources/agent_bg.jpg");
    tryPaths << appDir + lit("/../qrenderdoc/Resources/agent_bg.jpg");
    tryPaths << lit(":/agent_bg.jpg");
    for(const QString &path : tryPaths)
    {
      if(QFileInfo(path).exists() || path.startsWith(lit(":/")))
      {
        m_bgPixmap = QPixmap(path);
        if(!m_bgPixmap.isNull())
        {
          qDebug("AgentAssistant: loaded background from: %s (%dx%d)",
                 qPrintable(path), m_bgPixmap.width(), m_bgPixmap.height());
          break;
        }
      }
    }
    if(m_bgPixmap.isNull())
      qWarning("AgentAssistant: background image not found in any search path");
  }

  applyReadableFonts();

  m_net = new QNetworkAccessManager(this);

  ui->providerCombo->addItem(lit("OpenAI"), (int)AgentLLMBackend::OpenAI);
  ui->providerCombo->addItem(lit("Anthropic"), (int)AgentLLMBackend::Anthropic);
  ui->providerCombo->addItem(lit("Google Gemini"), (int)AgentLLMBackend::GoogleGemini);
  ui->providerCombo->addItem(lit("Azure OpenAI"), (int)AgentLLMBackend::AzureOpenAI);
  ui->providerCombo->addItem(lit("OpenAI-compatible URL"), (int)AgentLLMBackend::OpenAICompatible);
  ui->providerCombo->addItem(lit("OpenRouter"), (int)AgentLLMBackend::OpenRouter);
  ui->providerCombo->addItem(lit("Zhipu GLM"), (int)AgentLLMBackend::GLM_Zhipu);
  ui->providerCombo->addItem(lit("GitHub Models"), (int)AgentLLMBackend::GitHubModels);
  ui->providerCombo->addItem(lit("DeepSeek"), (int)AgentLLMBackend::DeepSeek);
  ui->providerCombo->addItem(lit("Aliyun DashScope (Qwen)"), (int)AgentLLMBackend::DashScope);
  ui->providerCombo->addItem(lit("Moonshot Kimi"), (int)AgentLLMBackend::Moonshot);
  ui->providerCombo->addItem(lit("Baidu Qianfan (ERNIE)"), (int)AgentLLMBackend::Qianfan);
  ui->providerCombo->addItem(lit("Volcano Ark (Doubao)"), (int)AgentLLMBackend::Volcano);

  m_activeLLMProvider = -1;
  loadSettingsFromConfig();
  m_activeLLMProvider = ui->providerCombo->currentIndex();
  updateProviderUi();

  QObject::connect(ui->providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                   &AgentAssistantPanel::onProviderChanged);
  QObject::connect(ui->copyPromptButton, &QPushButton::clicked, this, &AgentAssistantPanel::copyPrompt);
  QObject::connect(ui->copyContextButton, &QPushButton::clicked, this,
                   &AgentAssistantPanel::copyContextOnly);
  QObject::connect(ui->refreshButton, &QPushButton::clicked, this, &AgentAssistantPanel::refreshSnapshot);
  QObject::connect(ui->sendLLMButton, &QPushButton::clicked, this, &AgentAssistantPanel::sendToLLM);
  QObject::connect(ui->analyzeFrameButton, &QPushButton::clicked, this,
                   &AgentAssistantPanel::analyzeFullFrame);
  QObject::connect(ui->continueToChatButton, &QPushButton::clicked, this,
                   &AgentAssistantPanel::onContinueToChat);

  QObject::connect(ui->apiTokenEdit, &QLineEdit::editingFinished, this,
                   &AgentAssistantPanel::onPersistConnectionFields);
  QObject::connect(ui->apiTokenEdit, &QLineEdit::returnPressed, this,
                   &AgentAssistantPanel::onPersistConnectionFields);
  QObject::connect(ui->azureEndpointEdit, &QLineEdit::editingFinished, this,
                   &AgentAssistantPanel::onPersistConnectionFields);
  QObject::connect(ui->azureDeploymentEdit, &QLineEdit::editingFinished, this,
                   &AgentAssistantPanel::onPersistConnectionFields);
  QObject::connect(ui->compatibleBaseUrlEdit, &QLineEdit::editingFinished, this,
                   &AgentAssistantPanel::onPersistConnectionFields);

  QObject::connect(ui->includeDisasmCheck, &QCheckBox::stateChanged, this,
                   &AgentAssistantPanel::onPersistConnectionFields);
  QObject::connect(ui->includeThumbsCheck, &QCheckBox::stateChanged, this,
                   &AgentAssistantPanel::onPersistConnectionFields);
  QObject::connect(ui->exportReportButton, &QPushButton::clicked, this,
                   &AgentAssistantPanel::exportReport);

  if(ui->questionEdit)
  {
    QObject::connect(ui->questionEdit, &QLineEdit::textChanged, this,
                     &AgentAssistantPanel::rebuildSnapshot);
    QObject::connect(ui->questionEdit, &QLineEdit::returnPressed, this,
                     &AgentAssistantPanel::sendToLLM);
  }

  QObject::connect(ui->chatLog, &QTextBrowser::anchorClicked, this,
                   &AgentAssistantPanel::onChatLinkClicked);
  QObject::connect(ui->snapshotToggleBtn, &QPushButton::toggled, this,
                   &AgentAssistantPanel::onSnapshotToggled);
  QObject::connect(ui->stopButton, &QPushButton::clicked, this, &AgentAssistantPanel::stopLLM);

  QObject::connect(ui->searchNextBtn, &QPushButton::clicked, this, &AgentAssistantPanel::onSearchNext);
  QObject::connect(ui->searchCloseBtn, &QPushButton::clicked, this,
                   &AgentAssistantPanel::onSearchClose);
  QObject::connect(ui->searchEdit, &QLineEdit::returnPressed, this,
                   &AgentAssistantPanel::onSearchNext);

  m_searchShortcut = new QShortcut(QKeySequence(lit("Ctrl+F")), this);
  QObject::connect(m_searchShortcut, &QShortcut::activated, this,
                   &AgentAssistantPanel::onSearchToggle);

  ui->snapshotEdit->setMaximumHeight(0);
  ui->snapshotEdit->setVisible(false);

  m_Ctx.AddCaptureViewer(this);

  rebuildSnapshot();
  updateSetupVsChatLayout();
  applyPanelChrome();
}

void AgentAssistantPanel::showEvent(QShowEvent *e)
{
  applyPanelChrome();
  QFrame::showEvent(e);
}

void AgentAssistantPanel::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  p.setRenderHint(QPainter::SmoothPixmapTransform);

  const QString styleId = rdcToQString(m_Ctx.Config().UIStyle).trimmed();
  bool isAnime = (styleId.compare(lit("RDAnimeGlass"), Qt::CaseInsensitive) == 0);

  if(!isAnime)
  {
    p.fillRect(rect(), palette().window());
    return;
  }

  if(!m_bgPixmap.isNull())
  {
    if(m_bgScaledSize != size())
    {
      m_bgScaled =
          m_bgPixmap.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      m_bgScaledSize = size();
    }
    int x = (width() - m_bgScaled.width()) / 2;
    int y = (height() - m_bgScaled.height()) / 2;
    p.drawPixmap(x, y, m_bgScaled);
    p.fillRect(rect(), QColor(7, 20, 17, 45));
  }
  else
  {
    QLinearGradient grad(0, 0, width(), height());
    grad.setColorAt(0.0, QColor(0xEE, 0xF7, 0xF3));
    grad.setColorAt(0.5, QColor(0xDF, 0xEF, 0xE8));
    grad.setColorAt(1.0, QColor(0xD0, 0xE8, 0xDD));
    p.fillRect(rect(), grad);
  }
}



AgentAssistantPanel::~AgentAssistantPanel()
{
  saveSettingsToConfig();
  m_Ctx.RemoveCaptureViewer(this);
  delete ui;
}

void AgentAssistantPanel::OnCaptureLoaded()
{
  m_snapshotValid = false;
  rebuildSnapshot();
}

void AgentAssistantPanel::OnCaptureClosed()
{
  m_snapshotValid = false;
  rebuildSnapshot();
}

void AgentAssistantPanel::OnSelectedEventChanged(uint32_t eventId)
{
  (void)eventId;
  rebuildSnapshot();
}

void AgentAssistantPanel::OnEventChanged(uint32_t eventId)
{
  (void)eventId;
  rebuildSnapshot();
}

static void countLeafActions(const rdcarray<ActionDescription> &actions, int &draws,
                             int &dispatches, int &clears, int &copies)
{
  for(const ActionDescription &a : actions)
  {
    uint32_t f = (uint32_t)a.flags;
    if(f & (uint32_t)ActionFlags::Drawcall)
      draws++;
    if(f & ((uint32_t)ActionFlags::Dispatch | (uint32_t)ActionFlags::MeshDispatch |
            (uint32_t)ActionFlags::DispatchRay))
      dispatches++;
    if(f & (uint32_t)ActionFlags::Clear)
      clears++;
    if(f & ((uint32_t)ActionFlags::Copy | (uint32_t)ActionFlags::Resolve))
      copies++;
    if(!a.children.empty())
      countLeafActions(a.children, draws, dispatches, clears, copies);
  }
}

static void walkEventTree(const rdcarray<ActionDescription> &actions, int depth, QString &out,
                          int maxDepth)
{
  if(depth > maxDepth)
    return;
  QString indent(depth * 2, QLatin1Char(' '));
  for(const ActionDescription &a : actions)
  {
    uint32_t flags = (uint32_t)a.flags;
    bool isMarker = (flags & (uint32_t)ActionFlags::PushMarker) != 0;
    bool isDraw = (flags & (uint32_t)ActionFlags::Drawcall) != 0;
    bool isDispatch = (flags & ((uint32_t)ActionFlags::Dispatch | (uint32_t)ActionFlags::MeshDispatch |
                                (uint32_t)ActionFlags::DispatchRay)) != 0;
    bool isClear = (flags & (uint32_t)ActionFlags::Clear) != 0;
    bool isCopy = (flags & ((uint32_t)ActionFlags::Copy | (uint32_t)ActionFlags::Resolve)) != 0;
    bool isPresent = (flags & (uint32_t)ActionFlags::Present) != 0;

    QString name = QString::fromUtf8(a.customName.c_str());
    if(name.isEmpty())
      name = QStringLiteral("event");

    QString tag;
    if(isDraw)
    {
      tag = QStringLiteral(" [Draw %1 idx, %2 inst]").arg(a.numIndices).arg(a.numInstances);
      QString outs;
      for(int i = 0; i < 8; i++)
      {
        if(a.outputs[i] != ResourceId())
        {
          if(!outs.isEmpty())
            outs += lit(",");
          outs += rdcToQString(ToStr(a.outputs[i]));
        }
      }
      if(!outs.isEmpty())
        tag += QStringLiteral(" RT:{%1}").arg(outs);
      if(a.depthOut != ResourceId())
        tag += QStringLiteral(" DS:%1").arg(rdcToQString(ToStr(a.depthOut)));
    }
    else if(isDispatch)
      tag = QStringLiteral(" [Dispatch %1x%2x%3]")
                .arg(a.dispatchDimension[0])
                .arg(a.dispatchDimension[1])
                .arg(a.dispatchDimension[2]);
    else if(isClear)
      tag = lit(" [Clear]");
    else if(isCopy)
      tag = lit(" [Copy/Resolve]");
    else if(isPresent)
      tag = lit(" [Present]");
    else if(isMarker && !a.children.empty())
    {
      int dc = 0, dp = 0, cl = 0, cp = 0;
      countLeafActions(a.children, dc, dp, cl, cp);
      QStringList parts;
      if(dc > 0) parts << QStringLiteral("%1 draws").arg(dc);
      if(dp > 0) parts << QStringLiteral("%1 dispatches").arg(dp);
      if(cl > 0) parts << QStringLiteral("%1 clears").arg(cl);
      if(cp > 0) parts << QStringLiteral("%1 copies").arg(cp);
      tag = QStringLiteral(" {%1}").arg(parts.join(lit(", ")));
    }

    out += QStringLiteral("%1EID %2: %3%4\n").arg(indent).arg(a.eventId).arg(name).arg(tag);

    if(!a.children.empty())
      walkEventTree(a.children, depth + 1, out, maxDepth);
  }
}

QString AgentAssistantPanel::formatEventTree()
{
  if(!m_Ctx.IsCaptureLoaded())
    return QString();

  const rdcarray<ActionDescription> &roots = m_Ctx.CurRootActions();
  if(roots.empty())
    return lit("(No actions in capture)");

  int totalDraws = 0, totalDisp = 0, totalClear = 0, totalCopy = 0;
  countLeafActions(roots, totalDraws, totalDisp, totalClear, totalCopy);
  int total = totalDraws + totalDisp + totalClear + totalCopy;

  int maxDepth = 4;
  if(total < 200)
    maxDepth = 6;
  else if(total > 2000)
    maxDepth = 3;

  QString out = QStringLiteral("=== Frame event tree (%1 draws, %2 dispatches, %3 clears, "
                               "%4 copies) ===\n")
                    .arg(totalDraws)
                    .arg(totalDisp)
                    .arg(totalClear)
                    .arg(totalCopy);
  walkEventTree(roots, 0, out, maxDepth);

  const int kTreeLimit = 80000;
  if(out.size() > kTreeLimit)
  {
    out.truncate(kTreeLimit);
    out += lit("\n... [event tree truncated, use [SCAN_PASS nnnn] on specific markers]\n");
  }

  return out;
}

void AgentAssistantPanel::rebuildSnapshot()
{
  // The heavy part is formatPipelineSnapshot(); what it renders depends only on the
  // question's skill routing and the current event, so skip when neither changed
  // (fires on every keystroke and on both event-change callbacks otherwise).
  if(m_buildingToolSnapshot)
    return;
  const uint32_t skills =
      selectAgentDataSkills(ui->questionEdit ? ui->questionEdit->text() : QString());
  const uint32_t eid = m_Ctx.IsCaptureLoaded() ? m_Ctx.CurEvent() : 0;
  if(m_snapshotValid && skills == m_snapshotSkills && eid == m_snapshotEID)
    return;
  m_snapshotValid = true;
  m_snapshotSkills = skills;
  m_snapshotEID = eid;
  ui->snapshotEdit->setPlainText(formatPipelineSnapshot());
}

QString AgentAssistantPanel::snapshotForEID(uint32_t eid)
{
  if(!m_Ctx.IsCaptureLoaded())
    return QString();

  // temporarily switching events would otherwise trigger two full panel rebuilds
  m_buildingToolSnapshot = true;

  uint32_t savedSel = m_Ctx.CurSelectedEvent();
  uint32_t savedCur = m_Ctx.CurEvent();

  m_Ctx.SetEventID({this}, eid, eid, true);

  m_Ctx.Replay().BlockInvoke([](IReplayController *) {});

  QString snap = formatPipelineSnapshot();

  QString shaderBlock;
  if(ui->includeDisasmCheck->isChecked())
  {
    rdcstr disasm;
    m_Ctx.Replay().BlockInvoke(
        [&](IReplayController *r) { disasm = collectShaderDisassembly(m_Ctx, r); });
    shaderBlock = rdcToQString(disasm);
  }

  m_Ctx.SetEventID({this}, savedSel, savedCur, true);
  m_Ctx.Replay().BlockInvoke([](IReplayController *) {});

  m_buildingToolSnapshot = false;

  if(!shaderBlock.isEmpty())
    snap += lit("\n") + shaderBlock;

  return snap;
}

static const ActionDescription *findActionByEID(const rdcarray<ActionDescription> &actions,
                                                uint32_t eid)
{
  for(const ActionDescription &a : actions)
  {
    if(a.eventId == eid)
      return &a;
    if(!a.children.empty())
    {
      const ActionDescription *found = findActionByEID(a.children, eid);
      if(found)
        return found;
    }
  }
  return NULL;
}

static void collectLeafDraws(const ActionDescription &marker, QVector<uint32_t> &draws)
{
  for(const ActionDescription &c : marker.children)
  {
    uint32_t f = (uint32_t)c.flags;
    bool isDraw = (f & (uint32_t)ActionFlags::Drawcall) != 0;
    bool isDispatch = (f & ((uint32_t)ActionFlags::Dispatch | (uint32_t)ActionFlags::MeshDispatch |
                            (uint32_t)ActionFlags::DispatchRay)) != 0;
    bool isClear = (f & (uint32_t)ActionFlags::Clear) != 0;
    bool isCopy = (f & ((uint32_t)ActionFlags::Copy | (uint32_t)ActionFlags::Resolve)) != 0;

    if(isDraw || isDispatch || isClear || isCopy)
      draws.push_back(c.eventId);
    if(!c.children.empty())
      collectLeafDraws(c, draws);
  }
}

QString AgentAssistantPanel::scanPassSummary(uint32_t markerEID)
{
  if(!m_Ctx.IsCaptureLoaded())
    return QString();

  const rdcarray<ActionDescription> &roots = m_Ctx.CurRootActions();
  const ActionDescription *marker = findActionByEID(roots, markerEID);
  if(!marker)
    return QStringLiteral("EID %1 not found in event tree.").arg(markerEID);

  QVector<uint32_t> draws;
  collectLeafDraws(*marker, draws);
  if(draws.isEmpty())
    return QStringLiteral("No draw/dispatch/clear/copy actions found under EID %1 (%2).")
        .arg(markerEID)
        .arg(QString::fromUtf8(marker->customName.c_str()));

  const int kMaxSamples = 8;
  QVector<uint32_t> sampled;
  if(draws.size() <= kMaxSamples)
  {
    sampled = draws;
  }
  else
  {
    sampled.push_back(draws.first());
    for(int i = 1; i < kMaxSamples - 1; i++)
    {
      int idx = (int)((double)i / (kMaxSamples - 1) * (draws.size() - 1));
      sampled.push_back(draws[idx]);
    }
    sampled.push_back(draws.last());
  }

  QString out = QStringLiteral("=== Pass scan: EID %1 (%2) === %3 total actions, sampling %4\n\n")
                    .arg(markerEID)
                    .arg(QString::fromUtf8(marker->customName.c_str()))
                    .arg(draws.size())
                    .arg(sampled.size());

  uint32_t savedSel = m_Ctx.CurSelectedEvent();
  uint32_t savedCur = m_Ctx.CurEvent();

  for(uint32_t eid : sampled)
  {
    m_Ctx.SetEventID({this}, eid, eid, true);
    m_Ctx.Replay().BlockInvoke([](IReplayController *) {});

    QString snap = formatPipelineSnapshot();

    const int kSnapLimit = 4000;
    if(snap.size() > kSnapLimit)
    {
      snap.truncate(kSnapLimit);
      snap += lit("\n... [truncated]\n");
    }

    out += QStringLiteral("--- EID %1 ---\n%2\n\n").arg(eid).arg(snap);
  }

  m_Ctx.SetEventID({this}, savedSel, savedCur, true);
  m_Ctx.Replay().BlockInvoke([](IReplayController *) {});

  return out;
}

QString AgentAssistantPanel::formatPipelineSnapshot()
{
  ICaptureContext &ctx = m_Ctx;

  if(!ctx.IsCaptureLoaded())
  {
    return lit("No capture loaded.\n\nOpen an .rdc file to populate this summary.");
  }

  QString ret;
  QTextStream ts(&ret);

  const QString questionForSkills = ui->questionEdit ? ui->questionEdit->text() : QString();
  const uint32_t skills = selectAgentDataSkills(questionForSkills);

  ts << lit("=== RenderDoc capture + event snapshot (for LLM) ===\n");
  ts << lit("Context data skills (from your question): ") << agentDataSkillMaskSummary(skills)
     << lit("\n");
  if(skills != AgentSkill_All)
  {
    ts << lit("Tip: Mention blend, texture/binding/model/mesh, shader/disasm, vertex/index, marker/eid, "
              "depth/stencil, dispatch, printf, attachment, or catalog/list keywords for capture-wide "
              "textures/buffers; clear the question for a full snapshot.\n");
  }
  if((skills & AgentSkill_Shaders) &&
     questionContainsAny(questionForSkills,
                         {lit("disasm"), lit("disassemble"), lit("assembly"), utf8Zh("\xe5\x8f\x8d\xe7\xbc\x96\xe8\xaf\x91"),
                          utf8Zh("\xe6\xb1\x87\xe7\xbc\x96"), lit("dxil"), lit("spirv")}) &&
     ui->includeDisasmCheck && !ui->includeDisasmCheck->isChecked())
  {
    ts << lit("[Tip: Enable \"Include shader disassembly\" to attach DisassembleShader output when sending "
              "to the LLM.]\n");
  }
  ts << lit("\n");

  const uint32_t kMaxVBuf = 32;
  const uint32_t kMaxVtxAttr = 48;
  const uint32_t kMaxViewport = 16;
  const uint32_t kMaxDescPerStage = 64;
  const uint32_t kMaxColorBlendRTs = 16;
  const uint32_t kMaxDescAccess = 96;
  const uint32_t kMaxShaderMsg = 24;
  const uint32_t kMaxResBrief = 64;

  if(skills & AgentSkill_CaptureMeta)
  {
    ts << lit("Capture file: ") << rdcToQString(ctx.GetCaptureFilename()) << lit("\n");

    const APIProperties &api = ctx.APIProps();
    ts << lit("Graphics API (captured): ") << ToQStr(api.pipelineType) << lit("\n");
    ts << lit("Local replay API: ") << ToQStr(api.localRenderer) << lit("\n");
    ts << lit("GPU vendor: ") << rdcToQString(ToStr(api.vendor)) << lit("  remoteReplay=")
       << (api.remoteReplay ? lit("true") : lit("false")) << lit("  degraded=")
       << (api.degraded ? lit("true") : lit("false")) << lit("  shaderDebugging=")
       << (api.shaderDebugging ? lit("true") : lit("false")) << lit("  pixelHistory=")
       << (api.pixelHistory ? lit("true") : lit("false")) << lit("\n");

    const FrameDescription &fi = ctx.FrameInfo();
    ts << lit("Frame number: ") << fi.frameNumber << lit("  captureUnixTimeUTC: ") << fi.captureTime
       << lit("  uncompressedCaptureBytes: ") << fi.uncompressedFileSize << lit("\n");
    if(fi.stats.recorded)
    {
      ts << lit("Frame statistics (API-specific; may be sparse): draws.calls=")
         << fi.stats.draws.calls << lit(" instanced=") << fi.stats.draws.instanced << lit(" indirect=")
         << fi.stats.draws.indirect << lit("  dispatches.calls=") << fi.stats.dispatches.calls
         << lit("\n");
    }
    ts << lit("Annotations in capture: ") << (fi.containsAnnotations ? lit("yes") : lit("no"))
       << lit("\n");
    ts << lit("Current event ID: ") << ctx.CurEvent() << lit("  Selected event ID: ")
       << ctx.CurSelectedEvent() << lit("\n");
    ts << lit("Textures in capture: ") << (uint32_t)ctx.GetTextures().size() << lit("  Buffers: ")
       << (uint32_t)ctx.GetBuffers().size() << lit("\n\n");
  }

  const ActionDescription *action = ctx.GetAction(ctx.CurEvent());
  if((skills & AgentSkill_Action) && action)
  {
    ts << lit("Current action (event ") << action->eventId << lit(" actionId ") << action->actionId
       << lit(")\n");
    ts << lit("  Name: ") << rdcToQString(action->GetName(ctx.GetStructuredFile())) << lit("\n");
    ts << lit("  Flags: ") << rdcToQString(ToStr(action->flags)) << lit("\n");
    ts << lit("  Draw: numIndices=") << action->numIndices << lit(" numInstances=")
       << action->numInstances << lit(" baseVertex=") << action->baseVertex << lit(" indexOffset=")
       << action->indexOffset << lit(" vertexOffset=") << action->vertexOffset
       << lit(" instanceOffset=") << action->instanceOffset << lit(" drawIndex=") << action->drawIndex
       << lit("\n");
    ts << lit("  Dispatch: groups xyz=") << action->dispatchDimension[0] << lit(" ")
       << action->dispatchDimension[1] << lit(" ") << action->dispatchDimension[2]
       << lit(" threadsPerGroup xyz=") << action->dispatchThreadsDimension[0] << lit(" ")
       << action->dispatchThreadsDimension[1] << lit(" ") << action->dispatchThreadsDimension[2]
       << lit(" baseGroup xyz=") << action->dispatchBase[0] << lit(" ") << action->dispatchBase[1]
       << lit(" ") << action->dispatchBase[2] << lit("\n");
    ts << lit("  Coarse outputs[0..7]: ");
    for(int i = 0; i < 8; i++)
      ts << rdcToQString(ToStr(action->outputs[i])) << lit(" ");
    ts << lit("\n  Coarse depthOut: ") << rdcToQString(ToStr(action->depthOut)) << lit("\n");

    ts << lit("  Marker / action hierarchy (root to current):\n");
    rdcarray<const ActionDescription *> chain;
    for(const ActionDescription *walk = action; walk; walk = walk->parent)
      chain.push_back(walk);
    for(size_t ci = chain.size(); ci > 0; ci--)
    {
      const ActionDescription *a = chain[ci - 1];
      ts << lit("    - EID ") << a->eventId << lit(": ")
         << rdcToQString(a->GetName(ctx.GetStructuredFile())) << lit("\n");
    }
    ts << lit("\n");
  }
  else if(skills & AgentSkill_Action)
  {
    ts << lit("No action matches the current event (still showing global/capture state).\n\n");
  }

  if(skills & AgentSkill_AssetCatalog)
    appendCaptureAssetCatalog(ts, ctx);

  const PipeState &pipe = ctx.CurPipelineState();
  if(pipe.IsCaptureLoaded())
  {
    const rdcarray<Descriptor> outs = pipe.GetOutputTargets();

    if(skills & AgentSkill_PipelineSummary)
    {
      ts << lit("Pipeline state (API-agnostic summary)\n");
      ts << lit("  Topology: ") << rdcToQString(ToStr(pipe.GetPrimitiveTopology())) << lit("\n");
      ts << lit("  Graphics PSO: ") << rdcToQString(ToStr(pipe.GetGraphicsPipelineObject())) << lit("\n");
      ts << lit("  Compute PSO: ") << rdcToQString(ToStr(pipe.GetComputePipelineObject())) << lit("\n");
      ts << lit("  Tessellation enabled: ")
         << (pipe.IsTessellationEnabled() ? lit("yes") : lit("no"))
         << lit("  MultiviewBroadcastCount: ") << pipe.MultiviewBroadcastCount()
         << lit("  RasterizedStream: ") << pipe.GetRasterizedStream() << lit("\n");
      ts << lit("  SupportsResourceArrays: ") << (pipe.SupportsResourceArrays() ? lit("yes") : lit("no"))
         << lit("  SupportsBarriers: ") << (pipe.SupportsBarriers() ? lit("yes") : lit("no"))
         << lit("\n\n");
    }

    if(skills & AgentSkill_ViewGeom)
    {
      ts << lit("Viewports (first ") << kMaxViewport << lit(")\n");
      for(uint32_t vi = 0; vi < kMaxViewport; vi++)
      {
        Viewport vp = pipe.GetViewport(vi);
        if(vp.width == 0 && vp.height == 0)
          continue;
        ts << lit("  [") << vi << lit("] x=") << vp.x << lit(" y=") << vp.y << lit(" w=") << vp.width
           << lit(" h=") << vp.height << lit(" minDepth=") << vp.minDepth << lit(" maxDepth=")
           << vp.maxDepth << lit("\n");
      }
      ts << lit("Scissors (first ") << kMaxViewport << lit(")\n");
      for(uint32_t si = 0; si < kMaxViewport; si++)
      {
        Scissor sc = pipe.GetScissor(si);
        if(sc.width == 0 && sc.height == 0)
          continue;
        ts << lit("  [") << si << lit("] x=") << sc.x << lit(" y=") << sc.y << lit(" w=") << sc.width
           << lit(" h=") << sc.height << lit("\n");
      }
      ts << lit("\n");

      ts << lit("Index buffer\n");
      {
        BoundVBuffer ib = pipe.GetIBuffer();
        ts << lit("  resource=") << rdcToQString(ToStr(ib.resourceId)) << lit(" \"")
           << rdcToQString(ctx.GetResourceName(ib.resourceId)) << lit("\" offset=") << ib.byteOffset
           << lit(" stride=") << ib.byteStride << lit(" size=") << ib.byteSize
           << lit(" primitiveRestart=") << (pipe.IsRestartEnabled() ? lit("on") : lit("off"))
           << lit(" restartIndex=") << pipe.GetRestartIndex() << lit("\n\n");
      }

      ts << lit("Vertex buffers (first ") << kMaxVBuf << lit(" slots)\n");
      {
        rdcarray<BoundVBuffer> vbs = pipe.GetVBuffers();
        for(uint32_t i = 0; i < kMaxVBuf && i < (uint32_t)vbs.size(); i++)
        {
          const BoundVBuffer &vb = vbs[i];
          if(vb.resourceId == ResourceId())
            continue;
          ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(vb.resourceId)) << lit(" \"")
             << rdcToQString(ctx.GetResourceName(vb.resourceId)) << lit("\" offset=") << vb.byteOffset
             << lit(" stride=") << vb.byteStride << lit(" size=") << vb.byteSize << lit("\n");
        }
      }
      ts << lit("\nVertex input attributes (first ") << kMaxVtxAttr << lit(")\n");
      {
        rdcarray<VertexInputAttribute> attrs = pipe.GetVertexInputs();
        for(uint32_t i = 0; i < kMaxVtxAttr && i < (uint32_t)attrs.size(); i++)
        {
          const VertexInputAttribute &va = attrs[i];
          ts << lit("  [") << i << lit("] vbIndex=") << va.vertexBuffer << lit(" offset=") << va.byteOffset
             << lit(" perInstance=") << (va.perInstance ? lit("yes") : lit("no"))
             << lit(" instanceRate=") << va.instanceRate << lit(" fmt=")
             << resourceFormatToQString(va.format) << lit(" name=\"") << rdcToQString(va.name) << lit("\"\n");
        }
      }
      ts << lit("\n");
    }

    if(skills & AgentSkill_Shaders)
    {
      ts << lit("Bound shaders\n");
      for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
      {
        ShaderStage st = (ShaderStage)s;
        ResourceId sh = pipe.GetShader(st);
        if(sh != ResourceId())
        {
          ts << lit("  ") << rdcToQString(ToStr(st)) << lit(": id=") << rdcToQString(ToStr(sh))
             << lit(" name=\"") << rdcToQString(ctx.GetResourceName(sh)) << lit("\" entry=")
             << rdcToQString(pipe.GetShaderEntryPoint(st)) << lit("\n");
        }
      }
      ts << lit("\n");
    }

    if(skills & AgentSkill_OMTargets)
    {
      ts << lit("Output merger: color RTVs (render targets)\n");
      for(size_t i = 0; i < outs.size(); i++)
      {
        if(outs[i].resource == ResourceId())
          continue;
        ts << lit("  [") << (uint32_t)i << lit("] ") << rdcToQString(ToStr(outs[i].resource)) << lit(" \"")
           << rdcToQString(ctx.GetResourceName(outs[i].resource)) << lit("\" descType=")
           << rdcToQString(ToStr(outs[i].type)) << lit(" viewFmt=")
           << resourceFormatToQString(outs[i].format) << lit(" texType=")
           << rdcToQString(ToStr(outs[i].textureType)) << lit(" view=")
           << rdcToQString(ToStr(outs[i].view)) << lit(" mips[") << (uint32_t)outs[i].firstMip
           << lit("+") << (uint32_t)outs[i].numMips << lit("] slices[") << outs[i].firstSlice
           << lit("+") << outs[i].numSlices << lit("] swizzle=")
           << textureSwizzle4ToQString(outs[i].swizzle) << lit("\n");
      }
      ts << lit("Depth-stencil target\n");
      {
        Descriptor ds = pipe.GetDepthTarget();
        if(ds.resource != ResourceId())
        {
          ts << lit("  ") << rdcToQString(ToStr(ds.resource)) << lit(" \"")
             << rdcToQString(ctx.GetResourceName(ds.resource)) << lit("\" descType=")
             << rdcToQString(ToStr(ds.type)) << lit(" viewFmt=") << resourceFormatToQString(ds.format)
             << lit(" mips[") << (uint32_t)ds.firstMip << lit("+") << (uint32_t)ds.numMips
             << lit("] slices[") << ds.firstSlice << lit("+") << ds.numSlices << lit("]\n");
        }
        else
        {
          ts << lit("  (none)\n");
        }
        Descriptor dsr = pipe.GetDepthResolveTarget();
        if(dsr.resource != ResourceId())
        {
          ts << lit("Depth resolve: ") << rdcToQString(ToStr(dsr.resource)) << lit(" \"")
             << rdcToQString(ctx.GetResourceName(dsr.resource)) << lit("\"\n");
        }
      }
    }

    if(skills & AgentSkill_BlendStencil)
    {
      ts << lit("\nColor blend state (per render target, including RGB/A factors and ops)\n");
      ts << lit("  independentBlending=")
         << (pipe.IsIndependentBlendingEnabled() ? lit("yes") : lit("no")) << lit("\n");
      {
        rdcarray<ColorBlend> blends = pipe.GetColorBlends();
        for(size_t i = 0; i < blends.size() && i < kMaxColorBlendRTs; i++)
        {
          const ColorBlend &b = blends[i];
          ts << lit("  RTV[") << (uint32_t)i << lit("] blendEnabled=")
             << (b.enabled ? lit("yes") : lit("no")) << lit(" writeMask=0x")
             << QString::number(b.writeMask, 16) << lit(" logicOpEnabled=")
             << (b.logicOperationEnabled ? lit("yes") : lit("no")) << lit(" logicOp=")
             << rdcToQString(ToStr(b.logicOperation)) << lit("\n");
          ts << lit("           color: op=") << rdcToQString(ToStr(b.colorBlend.operation))
             << lit(" src=") << rdcToQString(ToStr(b.colorBlend.source)) << lit(" dst=")
             << rdcToQString(ToStr(b.colorBlend.destination)) << lit("\n");
          ts << lit("           alpha: op=") << rdcToQString(ToStr(b.alphaBlend.operation))
             << lit(" src=") << rdcToQString(ToStr(b.alphaBlend.source)) << lit(" dst=")
             << rdcToQString(ToStr(b.alphaBlend.destination)) << lit("\n");
        }
        if(blends.size() > kMaxColorBlendRTs)
          ts << lit("  ... ") << ((uint32_t)blends.size() - kMaxColorBlendRTs)
             << lit(" more RT blend slots omitted\n");
      }

      ts << lit("\nStencil faces (front then back): ref compareMask writeMask function\n");
      {
        rdcpair<StencilFace, StencilFace> sten = pipe.GetStencilFaces();
        ts << lit("  front: ref=") << sten.first.reference << lit(" cmpMask=0x")
           << QString::number(sten.first.compareMask, 16) << lit(" wrMask=0x")
           << QString::number(sten.first.writeMask, 16) << lit(" ")
           << rdcToQString(ToStr(sten.first.function)) << lit("\n");
        ts << lit("  back:  ref=") << sten.second.reference << lit(" cmpMask=0x")
           << QString::number(sten.second.compareMask, 16) << lit(" wrMask=0x")
           << QString::number(sten.second.writeMask, 16) << lit(" ")
           << rdcToQString(ToStr(sten.second.function)) << lit("\n");
      }
    }

    if(skills & AgentSkill_Descriptors)
    {
      ts << lit("\nPer-stage descriptors (onlyUsed=true; CB / read-only inputs / UAVs / samplers).\n");
      ts << lit(
          "Read-only entries include input textures and buffers (SRV, sampled image, etc.): view "
          "format, mip/slice range, swizzle, and backing texture dimensions/storage format.\n");
      for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
      {
        ShaderStage st = (ShaderStage)s;
        rdcarray<UsedDescriptor> cbs = pipe.GetConstantBlocks(st, true);
        rdcarray<UsedDescriptor> ros = pipe.GetReadOnlyResources(st, true);
        rdcarray<UsedDescriptor> rws = pipe.GetReadWriteResources(st, true);
        rdcarray<UsedDescriptor> samps = pipe.GetSamplers(st, true);
        if(cbs.empty() && ros.empty() && rws.empty() && samps.empty())
          continue;
        ts << lit("  ") << rdcToQString(ToStr(st)) << lit(": CBs=") << (uint32_t)cbs.size() << lit(" RO=")
           << (uint32_t)ros.size() << lit(" RW=") << (uint32_t)rws.size() << lit(" Samplers=")
           << (uint32_t)samps.size() << lit("\n");
        auto dumpList = [&](const QString &label, const rdcarray<UsedDescriptor> &list,
                            bool isSamplerList) {
          if(list.empty())
            return;
          ts << lit("    ") << label << lit(":\n");
          for(uint32_t i = 0; i < kMaxDescPerStage && i < (uint32_t)list.size(); i++)
          {
            const UsedDescriptor &ud = list[i];
            ts << lit("      [") << i << lit("] accessType=") << rdcToQString(ToStr(ud.access.type))
               << lit(" stage=") << rdcToQString(ToStr(ud.access.stage)) << lit(" bindIdx=")
               << ud.access.index << lit(" arrayEl=") << ud.access.arrayElement << lit(" unused=")
               << (ud.access.staticallyUnused ? lit("yes") : lit("no")) << lit("\n");
            if(isSamplerList)
            {
              appendSamplerDetailLine(ts, ctx, ud.sampler);
            }
            else
            {
              appendDescriptorDetailLine(ts, ctx, ud.descriptor);
              appendSamplerDetailLine(ts, ctx, ud.sampler);
            }
          }
          if(list.size() > kMaxDescPerStage)
            ts << lit("      ... ") << ((uint32_t)list.size() - kMaxDescPerStage) << lit(" more\n");
        };
        dumpList(lit("CB (constant buffers)"), cbs, false);
        dumpList(lit("RO (textures, buffers, input attachments, etc.)"), ros, false);
        dumpList(lit("RW (UAVs / image stores)"), rws, false);
        dumpList(lit("SMP (sampler objects only)"), samps, true);
      }
    }

    if(skills & AgentSkill_DescriptorAccess)
    {
      ts << lit("\nDescriptor access records (first ") << kMaxDescAccess << lit(")\n");
      {
        const rdcarray<DescriptorAccess> &acc = pipe.GetDescriptorAccess();
        ts << lit("  total=") << (uint32_t)acc.size() << lit("\n");
        for(uint32_t i = 0; i < kMaxDescAccess && i < (uint32_t)acc.size(); i++)
        {
          const DescriptorAccess &a = acc[i];
          ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(a.stage)) << lit(" ")
             << rdcToQString(ToStr(a.type)) << lit(" idx=") << a.index << lit(" arrayEl=")
             << a.arrayElement << lit(" store=") << rdcToQString(ToStr(a.descriptorStore))
             << lit(" unused=") << (a.staticallyUnused ? lit("yes") : lit("no")) << lit("\n");
        }
        if(acc.size() > kMaxDescAccess)
          ts << lit("  ... ") << ((uint32_t)acc.size() - kMaxDescAccess) << lit(" more\n");
      }
    }

    if(skills & AgentSkill_ShaderMessages)
    {
      ts << lit("\nShader printf / messages (first ") << kMaxShaderMsg << lit(")\n");
      {
        const rdcarray<ShaderMessage> &msgs = pipe.GetShaderMessages();
        if(msgs.empty())
          ts << lit("  (none)\n");
        for(uint32_t i = 0; i < kMaxShaderMsg && i < (uint32_t)msgs.size(); i++)
        {
          const ShaderMessage &m = msgs[i];
          ts << lit("  [") << i << lit("] ") << rdcToQString(ToStr(m.stage)) << lit(" line=")
             << m.disassemblyLine << lit(" ") << rdcToQString(m.message) << lit("\n");
        }
      }
    }

    if(skills & AgentSkill_ResourceBrief)
    {
      ts << lit("\nRender targets + bound input/output textures/buffers (brief, first ") << kMaxResBrief
         << lit(" unique ids)\n");
      {
        rdcarray<ResourceId> seen;
        auto noteRes = [&](ResourceId id) {
          if(id == ResourceId())
            return;
          for(ResourceId s : seen)
            if(s == id)
              return;
          if((uint32_t)seen.size() >= kMaxResBrief)
            return;
          seen.push_back(id);
        };
        for(const Descriptor &d : outs)
          noteRes(d.resource);
        noteRes(pipe.GetDepthTarget().resource);
        for(uint8_t s = 0; s < (uint8_t)ShaderStage::Count; s++)
        {
          ShaderStage st = (ShaderStage)s;
          for(const UsedDescriptor &ud : pipe.GetReadOnlyResources(st, true))
            noteRes(ud.descriptor.resource);
          for(const UsedDescriptor &ud : pipe.GetReadWriteResources(st, true))
            noteRes(ud.descriptor.resource);
        }
        for(ResourceId id : seen)
        {
          const TextureDescription *tex = ctx.GetTexture(id);
          if(tex)
          {
            ts << lit("  T ") << rdcToQString(ToStr(id)) << lit(" \"")
               << rdcToQString(ctx.GetResourceName(tex->resourceId)) << lit("\" ") << tex->width
               << lit("x") << tex->height << lit("x") << tex->depth << lit(" mips=") << tex->mips
               << lit(" arraysize=") << tex->arraysize << lit(" fmt=")
               << resourceFormatToQString(tex->format) << lit("\n");
          }
          else
          {
            const BufferDescription *buf = ctx.GetBuffer(id);
            if(buf)
            {
              ts << lit("  B ") << rdcToQString(ToStr(id)) << lit(" \"")
                 << rdcToQString(ctx.GetResourceName(buf->resourceId)) << lit("\" length=") << buf->length
                 << lit("\n");
            }
            else
            {
              ts << lit("  ? ") << rdcToQString(ToStr(id)) << lit(" \"")
                 << rdcToQString(ctx.GetResourceName(id)) << lit("\"\n");
            }
          }
        }
      }
    }
  }

  ts << lit(
      "\nNote: long lists and shader dumps are truncated to keep requests within model context "
      "windows; use Refresh, narrower events, or a more specific question (see Context data skills) for "
      "more detail.\n");
  ts << lit("=== End snapshot ===\n");

  const int kMaxSnapshotChars = 280000;
  if(ret.size() > kMaxSnapshotChars)
  {
    ret.truncate(kMaxSnapshotChars);
    ret += lit(
        "\n\n[Snapshot truncated at character limit; move to a specific draw/dispatch or ask a "
        "narrower question.]\n");
  }

  return ret;
}

void AgentAssistantPanel::loadSettingsFromConfig()
{
  PersistentConfig &cfg = m_Ctx.Config();
  int p = cfg.AgentAssistant_LLMProvider;
  if(p < 0 || p > (int)AgentLLMBackend::Volcano)
    p = 0;
  ui->providerCombo->blockSignals(true);
  ui->providerCombo->setCurrentIndex(p);
  ui->providerCombo->blockSignals(false);

  ui->azureEndpointEdit->setText(rdcToQString(cfg.AgentAssistant_AzureEndpoint));
  ui->azureDeploymentEdit->setText(rdcToQString(cfg.AgentAssistant_AzureDeployment));
  ui->compatibleBaseUrlEdit->setText(rdcToQString(cfg.AgentAssistant_CompatibleBaseUrl));
  ui->includeDisasmCheck->setChecked(cfg.AgentAssistant_IncludeShaderDisassembly);
  ui->includeThumbsCheck->setChecked(cfg.AgentAssistant_IncludeTextureThumbs);

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_OpenAIApiKey));
      break;
    case AgentLLMBackend::Anthropic:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_AnthropicApiKey));
      break;
    case AgentLLMBackend::GoogleGemini:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_GoogleApiKey));
      break;
    case AgentLLMBackend::AzureOpenAI:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_AzureApiKey));
      break;
    case AgentLLMBackend::OpenAICompatible:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_CompatibleApiKey));
      break;
    case AgentLLMBackend::OpenRouter:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_OpenRouterApiKey));
      break;
    case AgentLLMBackend::GLM_Zhipu:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_GLMApiKey));
      break;
    case AgentLLMBackend::GitHubModels:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_GitHubModelsApiKey));
      break;
    case AgentLLMBackend::DeepSeek:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_DeepSeekApiKey));
      break;
    case AgentLLMBackend::DashScope:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_DashScopeApiKey));
      break;
    case AgentLLMBackend::Moonshot:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_MoonshotApiKey));
      break;
    case AgentLLMBackend::Qianfan:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_QianfanApiKey));
      break;
    case AgentLLMBackend::Volcano:
      ui->apiTokenEdit->setText(rdcToQString(cfg.AgentAssistant_VolcanoApiKey));
      break;
  }

  repopulateModelCombo();
}

void AgentAssistantPanel::saveSettingsToConfig()
{
  PersistentConfig &cfg = m_Ctx.Config();
  const int p = ui->providerCombo->currentIndex();
  const QString modelLine = ui->modelCombo->currentText().trimmed();

  cfg.AgentAssistant_LLMProvider = p;
  cfg.AgentAssistant_AzureEndpoint = qstrToRdc(ui->azureEndpointEdit->text().trimmed());
  cfg.AgentAssistant_AzureDeployment = qstrToRdc(ui->azureDeploymentEdit->text().trimmed());
  cfg.AgentAssistant_CompatibleBaseUrl = qstrToRdc(ui->compatibleBaseUrlEdit->text().trimmed());
  cfg.AgentAssistant_IncludeShaderDisassembly = ui->includeDisasmCheck->isChecked();
  cfg.AgentAssistant_IncludeTextureThumbs = ui->includeThumbsCheck->isChecked();

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
      cfg.AgentAssistant_OpenAIApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_OpenAIModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::Anthropic:
      cfg.AgentAssistant_AnthropicApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_AnthropicModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::GoogleGemini:
      cfg.AgentAssistant_GoogleApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_GoogleModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::AzureOpenAI:
      cfg.AgentAssistant_AzureApiKey = qstrToRdc(ui->apiTokenEdit->text());
      break;
    case AgentLLMBackend::OpenAICompatible:
      cfg.AgentAssistant_CompatibleApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_OpenAIModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::OpenRouter:
      cfg.AgentAssistant_OpenRouterApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_OpenRouterModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::GLM_Zhipu:
      cfg.AgentAssistant_GLMApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_GLMModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::GitHubModels:
      cfg.AgentAssistant_GitHubModelsApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_GitHubModelsModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::DeepSeek:
      cfg.AgentAssistant_DeepSeekApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_DeepSeekModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::DashScope:
      cfg.AgentAssistant_DashScopeApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_DashScopeModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::Moonshot:
      cfg.AgentAssistant_MoonshotApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_MoonshotModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::Qianfan:
      cfg.AgentAssistant_QianfanApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_QianfanModel = qstrToRdc(modelLine);
      break;
    case AgentLLMBackend::Volcano:
      cfg.AgentAssistant_VolcanoApiKey = qstrToRdc(ui->apiTokenEdit->text());
      cfg.AgentAssistant_VolcanoModel = qstrToRdc(modelLine);
      break;
  }

  cfg.Save();
}

void AgentAssistantPanel::updateProviderUi()
{
  int idx = ui->providerCombo->currentIndex();
  bool azure = (idx == (int)AgentLLMBackend::AzureOpenAI);
  bool compat = (idx == (int)AgentLLMBackend::OpenAICompatible);

  ui->azureEndpointLabel->setVisible(azure);
  ui->azureEndpointEdit->setVisible(azure);
  ui->azureDeployLabel->setVisible(azure);
  ui->azureDeploymentEdit->setVisible(azure);

  ui->compatibleUrlLabel->setVisible(compat);
  ui->compatibleBaseUrlEdit->setVisible(compat);

  ui->modelLabel->setVisible(!azure);
  ui->modelCombo->setVisible(!azure);

  if(azure)
    ui->tokenLabel->setText(tr("Azure API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::GoogleGemini)
    ui->tokenLabel->setText(tr("Gemini API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::GitHubModels)
    ui->tokenLabel->setText(tr("GitHub PAT (models scope)"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::GLM_Zhipu)
    ui->tokenLabel->setText(tr("GLM API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::DeepSeek)
    ui->tokenLabel->setText(tr("DeepSeek API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::DashScope)
    ui->tokenLabel->setText(tr("DashScope API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::Moonshot)
    ui->tokenLabel->setText(tr("Moonshot API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::Qianfan)
    ui->tokenLabel->setText(tr("Qianfan API key"));
  else if((AgentLLMBackend)idx == AgentLLMBackend::Volcano)
    ui->tokenLabel->setText(tr("Volcano Ark API key"));
  else
    ui->tokenLabel->setText(tr("API token"));
}

void AgentAssistantPanel::repopulateModelCombo()
{
  PersistentConfig &cfg = m_Ctx.Config();
  const int idx = ui->providerCombo->currentIndex();

  ui->modelCombo->blockSignals(true);
  ui->modelCombo->clear();

  QString saved;
  QStringList presets;

  switch((AgentLLMBackend)idx)
  {
    case AgentLLMBackend::OpenAI:
      presets << lit("gpt-5.6-sol") << lit("gpt-5.6-terra") << lit("gpt-5.6-luna") << lit("gpt-5")
              << lit("gpt-5-mini") << lit("gpt-5-nano") << lit("gpt-4.1") << lit("gpt-4.1-mini")
              << lit("gpt-4.1-nano");
      saved = rdcToQString(cfg.AgentAssistant_OpenAIModel);
      break;
    case AgentLLMBackend::Anthropic:
      presets << lit("claude-opus-5") << lit("claude-opus-4-8") << lit("claude-opus-4-7")
              << lit("claude-opus-4-6") << lit("claude-sonnet-4-6") << lit("claude-haiku-4-5")
              << lit("claude-haiku-4-5-20251001");
      saved = rdcToQString(cfg.AgentAssistant_AnthropicModel);
      break;
    case AgentLLMBackend::GoogleGemini:
      presets << lit("gemini-3.6-flash") << lit("gemini-3.5-flash") << lit("gemini-3.5-flash-lite")
              << lit("gemini-3.1-flash-lite") << lit("gemini-3.1-pro-preview")
              << lit("gemini-3-flash-preview") << lit("gemini-2.5-pro") << lit("gemini-2.5-flash");
      saved = rdcToQString(cfg.AgentAssistant_GoogleModel);
      break;
    case AgentLLMBackend::AzureOpenAI: break;
    case AgentLLMBackend::OpenAICompatible:
      presets << lit("gpt-5.6-sol") << lit("gpt-5.6-terra") << lit("gpt-5.6-luna") << lit("gpt-5")
              << lit("gpt-5-mini") << lit("deepseek-v4-flash") << lit("deepseek-v4-pro")
              << lit("qwen3.7-plus") << lit("qwen-max") << lit("glm-5.2") << lit("kimi-k3");
      saved = rdcToQString(cfg.AgentAssistant_OpenAIModel);
      break;
    case AgentLLMBackend::OpenRouter:
      presets << lit("openai/gpt-5.6-sol") << lit("openai/gpt-5.6-terra")
              << lit("openai/gpt-5.6-luna") << lit("openai/gpt-5-mini") << lit("openai/gpt-4.1")
              << lit("anthropic/claude-opus-4.8") << lit("anthropic/claude-sonnet-4.6")
              << lit("anthropic/claude-haiku-4.5") << lit("google/gemini-3.6-flash")
              << lit("google/gemini-3.5-flash") << lit("google/gemini-3.5-flash-lite")
              << lit("deepseek/deepseek-v4-flash") << lit("deepseek/deepseek-v4-pro")
              << lit("qwen/qwen3.7-plus") << lit("moonshotai/kimi-k3")
              << lit("meta-llama/llama-3.3-70b-instruct") << lit("mistralai/mistral-large-2411");
      saved = rdcToQString(cfg.AgentAssistant_OpenRouterModel);
      break;
    case AgentLLMBackend::GLM_Zhipu:
      presets << lit("glm-5.2") << lit("glm-5.1") << lit("glm-4.7") << lit("glm-4.6")
              << lit("glm-4.5") << lit("glm-4-plus") << lit("glm-4-flash");
      saved = rdcToQString(cfg.AgentAssistant_GLMModel);
      break;
    case AgentLLMBackend::GitHubModels:
      presets << lit("openai/gpt-5.6-luna") << lit("openai/gpt-5.6-terra")
              << lit("openai/gpt-5.6-sol") << lit("openai/gpt-5-mini") << lit("openai/gpt-4.1")
              << lit("mistralai/Mistral-Large-2411") << lit("meta-llama/Llama-3.3-70B-Instruct")
              << lit("deepseek/DeepSeek-V3-0324") << lit("google/gemini-3.6-flash")
              << lit("google/gemini-3.5-flash");
      saved = rdcToQString(cfg.AgentAssistant_GitHubModelsModel);
      break;
    case AgentLLMBackend::DeepSeek:
      presets << lit("deepseek-v4-flash") << lit("deepseek-v4-pro");
      saved = rdcToQString(cfg.AgentAssistant_DeepSeekModel);
      break;
    case AgentLLMBackend::DashScope:
      presets << lit("qwen3.8-max") << lit("qwen3.7-plus") << lit("qwen3.7-flash")
              << lit("qwen-max") << lit("qwen-plus") << lit("qwen-turbo") << lit("qwen-long");
      saved = rdcToQString(cfg.AgentAssistant_DashScopeModel);
      break;
    case AgentLLMBackend::Moonshot:
      presets << lit("kimi-k3") << lit("kimi-k2.7-code") << lit("kimi-k2.6")
              << lit("moonshot-v1-128k");
      saved = rdcToQString(cfg.AgentAssistant_MoonshotModel);
      break;
    case AgentLLMBackend::Qianfan:
      presets << lit("ernie-4.0-turbo-8k") << lit("ernie-4.0-8k") << lit("ernie-3.5-8k")
              << lit("ernie-3.5-turbo-8k");
      saved = rdcToQString(cfg.AgentAssistant_QianfanModel);
      break;
    case AgentLLMBackend::Volcano:
      presets << lit("doubao-seed-2.1-pro") << lit("doubao-seed-2.1-turbo")
              << lit("doubao-seed-2.0-pro") << lit("doubao-seed-2.0-lite")
              << lit("doubao-seed-2.0-mini") << lit("doubao-seed-2.0-code")
              << lit("doubao-seed-1.8") << lit("doubao-seed-1.6") << lit("doubao-seed-1.6-flash");
      saved = rdcToQString(cfg.AgentAssistant_VolcanoModel);
      break;
  }

  if(!saved.isEmpty() && !presets.contains(saved))
    presets.prepend(saved);

  ui->modelCombo->addItems(presets);

  if(!saved.isEmpty())
  {
    int si = ui->modelCombo->findText(saved);
    if(si >= 0)
      ui->modelCombo->setCurrentIndex(si);
  }
  else if(ui->modelCombo->count() > 0)
  {
    ui->modelCombo->setCurrentIndex(0);
  }

  ui->modelCombo->setEditable(false);
  ui->modelCombo->blockSignals(false);
}

void AgentAssistantPanel::onProviderChanged(int idx)
{
  m_nativeToolsDisabled = false;
  PersistentConfig &cfg = m_Ctx.Config();
  if(m_activeLLMProvider >= 0 && m_activeLLMProvider != idx)
  {
    QString tok = ui->apiTokenEdit->text();
    QString mdl = ui->modelCombo->currentText().trimmed();
    switch((AgentLLMBackend)m_activeLLMProvider)
    {
      case AgentLLMBackend::OpenAI:
        cfg.AgentAssistant_OpenAIApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_OpenAIModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::Anthropic:
        cfg.AgentAssistant_AnthropicApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_AnthropicModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::GoogleGemini:
        cfg.AgentAssistant_GoogleApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_GoogleModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::AzureOpenAI: cfg.AgentAssistant_AzureApiKey = qstrToRdc(tok); break;
      case AgentLLMBackend::OpenAICompatible:
        cfg.AgentAssistant_CompatibleApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_OpenAIModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::OpenRouter:
        cfg.AgentAssistant_OpenRouterApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_OpenRouterModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::GLM_Zhipu:
        cfg.AgentAssistant_GLMApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_GLMModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::GitHubModels:
        cfg.AgentAssistant_GitHubModelsApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_GitHubModelsModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::DeepSeek:
        cfg.AgentAssistant_DeepSeekApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_DeepSeekModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::DashScope:
        cfg.AgentAssistant_DashScopeApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_DashScopeModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::Moonshot:
        cfg.AgentAssistant_MoonshotApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_MoonshotModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::Qianfan:
        cfg.AgentAssistant_QianfanApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_QianfanModel = qstrToRdc(mdl);
        break;
      case AgentLLMBackend::Volcano:
        cfg.AgentAssistant_VolcanoApiKey = qstrToRdc(tok);
        cfg.AgentAssistant_VolcanoModel = qstrToRdc(mdl);
        break;
    }
    cfg.AgentAssistant_AzureEndpoint = qstrToRdc(ui->azureEndpointEdit->text().trimmed());
    cfg.AgentAssistant_AzureDeployment = qstrToRdc(ui->azureDeploymentEdit->text().trimmed());
    cfg.AgentAssistant_CompatibleBaseUrl = qstrToRdc(ui->compatibleBaseUrlEdit->text().trimmed());
  }

  cfg.AgentAssistant_LLMProvider = idx;
  cfg.AgentAssistant_AzureEndpoint = qstrToRdc(ui->azureEndpointEdit->text().trimmed());
  cfg.AgentAssistant_AzureDeployment = qstrToRdc(ui->azureDeploymentEdit->text().trimmed());
  cfg.AgentAssistant_CompatibleBaseUrl = qstrToRdc(ui->compatibleBaseUrlEdit->text().trimmed());
  cfg.Save();

  m_activeLLMProvider = idx;
  loadSettingsFromConfig();
  m_activeLLMProvider = idx;
  updateProviderUi();
  updateSetupVsChatLayout();
}

bool AgentAssistantPanel::hasMinimumLLMConnection() const
{
  const int p = ui->providerCombo->currentIndex();
  const QString apiKey = ui->apiTokenEdit->text().trimmed();
  if(p == (int)AgentLLMBackend::OpenAICompatible)
    return true;
  if(p == (int)AgentLLMBackend::AzureOpenAI)
  {
    return !apiKey.isEmpty() && !ui->azureEndpointEdit->text().trimmed().isEmpty() &&
           !ui->azureDeploymentEdit->text().trimmed().isEmpty();
  }
  return !apiKey.isEmpty();
}

void AgentAssistantPanel::updateSetupVsChatLayout()
{
  const bool ready = hasMinimumLLMConnection();
  ui->chatSection->setVisible(ready);
  ui->continueToChatButton->setVisible(!ready);
  if(ready)
    ui->helpLabel->setText(m_helpTextFull);
  else
    ui->helpLabel->setText(
        tr("Choose the LLM provider and enter your API token below. It is saved to RenderDoc's "
           "config on this machine when you leave the field or press Save and continue. Then you "
           "can ask about the capture."));
}

void AgentAssistantPanel::applyPanelChrome()
{
  const QString styleId = rdcToQString(m_Ctx.Config().UIStyle).trimmed();
  const bool isAnime =
      (styleId.compare(lit("RDAnimeGlass"), Qt::CaseInsensitive) == 0);
  const bool explicitLight = (styleId.compare(lit("RDLight"), Qt::CaseInsensitive) == 0);
  bool dark = false;

  if(isAnime)
  {
    setStyleSheet(agentPanelStylesheetAnime());
    return;
  }

  if(!explicitLight)
  {
    if(styleId.compare(lit("RDDark"), Qt::CaseInsensitive) == 0)
      dark = true;
    else if(styleId.isEmpty() || styleId.compare(lit("Native"), Qt::CaseInsensitive) == 0)
      dark = palette().color(QPalette::Window).lightness() < 130;
  }

  setStyleSheet(dark ? agentPanelStylesheetDark() : agentPanelStylesheetLight());
}

void AgentAssistantPanel::applyReadableFonts()
{
  QFont help = ui->helpLabel->font();
  const qreal hps = help.pointSizeF();
  if(hps > 0.0)
    help.setPointSizeF(qBound(10.0, hps + 1.0, 14.0));
  else
    help.setPointSize(10);
  ui->helpLabel->setFont(help);

  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  const qreal mps = mono.pointSizeF();
  if(mps > 0.0)
    mono.setPointSizeF(qMax(mps, 10.0));
  else
    mono.setPointSize(10);
  ui->snapshotEdit->setFont(mono);
}

void AgentAssistantPanel::onContinueToChat()
{
  saveSettingsToConfig();
  if(!hasMinimumLLMConnection())
  {
    QMessageBox::warning(this, tr("Pipeline Agent"),
                         tr("Please enter the required API token (and for Azure, endpoint and "
                            "deployment) before continuing."));
    return;
  }
  updateSetupVsChatLayout();
}

void AgentAssistantPanel::onPersistConnectionFields()
{
  saveSettingsToConfig();
  updateSetupVsChatLayout();
}

void AgentAssistantPanel::stopLLM()
{
  if(m_activeReply)
  {
    m_activeReply->abort();
    m_activeReply = NULL;
  }
  m_toolUseRound = kMaxToolUseRounds;
  setLLMUiBusy(false);
  setStatusText(tr("Stopped"));
  appendChatMessage(false, tr("[Stopped by user]"));
}

void AgentAssistantPanel::setLLMUiBusy(bool busy)
{
  ui->sendLLMButton->setEnabled(!busy);
  ui->analyzeFrameButton->setEnabled(!busy && m_Ctx.IsCaptureLoaded());
  ui->exportReportButton->setEnabled(!busy && m_Ctx.IsCaptureLoaded());
  ui->stopButton->setEnabled(busy);
  ui->providerCombo->setEnabled(!busy);
  ui->apiTokenEdit->setEnabled(!busy);
  ui->modelCombo->setEnabled(!busy);
  ui->azureEndpointEdit->setEnabled(!busy);
  ui->azureDeploymentEdit->setEnabled(!busy);
  ui->compatibleBaseUrlEdit->setEnabled(!busy);
  ui->includeDisasmCheck->setEnabled(!busy);
  ui->continueToChatButton->setEnabled(!busy);
}

void AgentAssistantPanel::copyPrompt()
{
  QString q = ui->questionEdit->text().trimmed();
  if(q.isEmpty())
  {
    QMessageBox::information(this, tr("Pipeline Agent"),
                             tr("Please enter a question before copying."));
    return;
  }

  QString clip = lit("You are helping analyse a GPU frame in RenderDoc.\n\n");
  clip += lit("Question:\n") + q + lit("\n\n");
  clip += lit("Context:\n") + ui->snapshotEdit->toPlainText();

  QGuiApplication::clipboard()->setText(clip);
  QMessageBox::information(
      this, tr("Pipeline Agent"),
      tr("Copied the question and pipeline context to the clipboard.\n"
         "Paste it into your AI assistant (for example Cursor chat)."));
}

void AgentAssistantPanel::copyContextOnly()
{
  QGuiApplication::clipboard()->setText(ui->snapshotEdit->toPlainText());
  QMessageBox::information(this, tr("Pipeline Agent"),
                           tr("Copied the pipeline context to the clipboard."));
}

void AgentAssistantPanel::refreshSnapshot()
{
  rebuildSnapshot();
}

void AgentAssistantPanel::sendToLLM()
{
  saveSettingsToConfig();

  QString question = ui->questionEdit->text().trimmed();
  if(question.isEmpty())
  {
    QMessageBox::information(this, tr("Pipeline Agent"), tr("Please enter a question."));
    return;
  }

  const int p = ui->providerCombo->currentIndex();
  QString apiKey = ui->apiTokenEdit->text().trimmed();
  if(apiKey.isEmpty() && p != (int)AgentLLMBackend::OpenAICompatible)
  {
    QMessageBox::warning(this, tr("Pipeline Agent"), tr("Please enter an API token."));
    return;
  }
  if(p == (int)AgentLLMBackend::OpenAICompatible && apiKey.isEmpty())
  {
    QMessageBox::warning(this, tr("Pipeline Agent"),
                         tr("Many OpenAI-compatible endpoints still require a Bearer token."));
  }

  QString snapshot = ui->snapshotEdit->toPlainText();
  QString shaderBlock;
  if(ui->includeDisasmCheck->isChecked() && m_Ctx.IsCaptureLoaded())
  {
    rdcstr disasm;
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) { disasm = collectShaderDisassembly(m_Ctx, r); });
    shaderBlock = rdcToQString(disasm);
  }

  QString eventTree;
  if(m_Ctx.IsCaptureLoaded())
    eventTree = formatEventTree();

  QString userBody = snapshot;
  if(!eventTree.isEmpty())
    userBody += lit("\n\n") + eventTree;
  if(!shaderBlock.isEmpty())
    userBody += lit("\n\n") + shaderBlock;
  if(!m_pendingAnalyzeText.isEmpty())
  {
    userBody += lit("\n\n=== Shader to analyse: ") + m_pendingAnalyzeTitle + lit(" ===\n") +
                m_pendingAnalyzeText;
    m_pendingAnalyzeTitle.clear();
    m_pendingAnalyzeText.clear();
  }
  userBody += lit("\nUser question:\n") + question;

  startLLMRequest(question, userBody, true);
}

void AgentAssistantPanel::analyzeFullFrame()
{
  if(!m_Ctx.IsCaptureLoaded())
  {
    QMessageBox::information(this, tr("Pipeline Agent"), tr("Load a capture first."));
    return;
  }

  const int p = ui->providerCombo->currentIndex();
  QString apiKey = ui->apiTokenEdit->text().trimmed();
  if(apiKey.isEmpty() && p != (int)AgentLLMBackend::OpenAICompatible)
  {
    QMessageBox::warning(this, tr("Pipeline Agent"), tr("Please enter an API token."));
    return;
  }

  saveSettingsToConfig();

  // empty question routes ALL data skills into the snapshot
  const QString savedQuestion = ui->questionEdit->text();
  ui->questionEdit->clear();
  QString body = formatPipelineSnapshot();
  ui->questionEdit->setText(savedQuestion);

  setCursor(Qt::WaitCursor);
  setStatusText(tr("Collecting full frame data (event tree + per-pass scans + shaders)..."));

  body += lit("\n\n") + formatEventTree();

  if(ui->analyzeDeepCheck->isChecked())
  {
    body += lit("\n\n=== Per-pass summaries (sampled draws for each top-level pass) ===");
    int passIdx = 0;
    for(const ActionDescription &a : m_Ctx.CurRootActions())
    {
      if(a.children.empty())
        continue;
      if(++passIdx > 8)
      {
        body += lit("\n(further top-level passes omitted; use the scan_pass tool for them)");
        break;
      }
      QString name = QString::fromUtf8(a.customName.c_str());
      if(name.isEmpty())
        name = QStringLiteral("pass");
      body += QStringLiteral("\n\n--- Pass: %1 (EID %2) ---\n").arg(name).arg(a.eventId);
      body += scanPassSummary(a.eventId);
    }
  }
  else
  {
    body += lit("\n\nPer-pass sampling was skipped for speed. Use the scan_pass tool on the "
                "marker EIDs above to sample each pass you find relevant.");
  }

  if(ui->includeDisasmCheck->isChecked())
  {
    rdcstr disasm;
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) { disasm = collectShaderDisassembly(m_Ctx, r); });
    QString shaderBlock = rdcToQString(disasm);
    if(!shaderBlock.isEmpty())
      body += lit("\n\n") + shaderBlock;
  }
  unsetCursor();

  const QString question =
      tr("Analyse this entire frame end-to-end: identify every render pass, what each pass "
         "renders, which shaders and resources are involved, how the passes connect (which "
         "pass's output feeds which), and produce a complete ordered pipeline breakdown with "
         "EID references. Use the tools to drill into anything the summaries leave unclear.");

  startLLMRequest(question, body, true);
}

void AgentAssistantPanel::startLLMRequest(const QString &question, QString userBody, bool withThumbs)
{
  const int kMaxUserBodyChars = 450000;
  if(userBody.size() > kMaxUserBodyChars)
  {
    userBody.truncate(kMaxUserBodyChars);
    userBody += lit("\n\n[Total message truncated for API/request size limits.]\n");
  }

  QVector<ThumbData> thumbs;
  if(withThumbs && ui->includeThumbsCheck->isChecked() && m_Ctx.IsCaptureLoaded())
  {
    setStatusText(tr("Grabbing texture thumbnails..."));
    thumbs = grabThumbImages();
  }

  m_apiMessages = QJsonArray();
  QJsonObject sys;
  sys[lit("role")] = lit("system");
  sys[lit("content")] = systemPrompt();
  m_apiMessages.append(sys);
  for(const QJsonValue &v : m_apiHistory)
    m_apiMessages.append(v);
  QJsonObject u;
  u[lit("role")] = lit("user");
  if(thumbs.isEmpty())
  {
    u[lit("content")] = userBody;
  }
  else
  {
    QJsonArray content;
    QJsonObject tp;
    tp[lit("type")] = lit("text");
    tp[lit("text")] = userBody;
    content.append(tp);
    for(const QJsonValue &v : thumbnailsToApiParts(thumbs))
      content.append(v);
    u[lit("content")] = content;
    appendChatMessage(false, tr("Attached %1 texture thumbnail(s).").arg(thumbs.count()));
  }
  m_apiMessages.append(u);

  m_toolUseRound = 0;
  m_lastUserQuestion = question;
  m_preToolEID = m_Ctx.IsCaptureLoaded() ? m_Ctx.CurEvent() : 0;

  appendChatMessage(true, question);
  ui->questionEdit->clear();
  appendChatMessage(false, tr("Waiting for response..."));
  setLLMUiBusy(true);
  setStatusText(tr("Sending request to LLM..."));

  dispatchLLMRequest();
}

void AgentAssistantPanel::onLLMFinished()
{
  QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
  if(reply == m_activeReply)
    m_activeReply = NULL;
  setLLMUiBusy(false);
  if(!reply)
    return;

  reply->deleteLater();

  int p = ui->providerCombo->currentIndex();
  QString err;

  if(!m_chatHistory.isEmpty() && !m_chatHistory.last().isUser &&
     m_chatHistory.last().text == tr("Waiting for response..."))
    m_chatHistory.removeLast();

  if(reply->error() != QNetworkReply::NoError)
  {
    err = reply->errorString() + lit("\n") + QString::fromUtf8(reply->readAll());
    if(reply->error() == QNetworkReply::OperationCanceledError)
    {
      setStatusText(tr("Stopped"));
      return;
    }
    QString sslHint;
    if(err.contains(lit("SSL"), Qt::CaseInsensitive) || err.contains(lit("TLS"), Qt::CaseInsensitive))
    {
      sslHint = lit(
          "\n\n[SSL/TLS on Windows] Qt needs matching OpenSSL DLLs (same major version as your Qt "
          "build). Copy ssleay32.dll + libeay32.dll (for Qt 5.9) or libssl/libcrypto (for Qt 5.12+) "
          "from your Qt bin next to qrenderdoc.exe.");
    }
    appendChatMessage(false, tr("HTTP error:\n") + err + sslHint);
    setStatusText(tr("Error"));
    return;
  }

  QByteArray body = reply->readAll();
  QJsonParseError jerr;
  QJsonDocument doc = QJsonDocument::fromJson(body, &jerr);
  if(!doc.isObject())
  {
    appendChatMessage(false, tr("Invalid JSON response:\n") + QString::fromUtf8(body));
    setStatusText(tr("Error"));
    return;
  }

  QJsonObject root = doc.object();
  parseTokenUsage(root);

  AgentLLMResult resp;
  bool openaiFamily = true;
  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
    case AgentLLMBackend::AzureOpenAI:
    case AgentLLMBackend::OpenAICompatible:
    case AgentLLMBackend::OpenRouter:
    case AgentLLMBackend::GLM_Zhipu:
    case AgentLLMBackend::GitHubModels:
    case AgentLLMBackend::DeepSeek:
    case AgentLLMBackend::DashScope:
    case AgentLLMBackend::Moonshot:
    case AgentLLMBackend::Qianfan:
    case AgentLLMBackend::Volcano:
      resp = parseOpenAIResponse(root);
      break;
    case AgentLLMBackend::Anthropic:
      openaiFamily = false;
      resp = parseAnthropicResponse(root);
      break;
    case AgentLLMBackend::GoogleGemini:
      openaiFamily = false;
      resp = parseGeminiResponse(root);
      break;
  }

  if(!resp.err.isEmpty() && resp.text.isEmpty() && !resp.hasToolCalls())
  {
    // graceful degradation: some endpoints/models reject the `tools` parameter outright
    if(openaiFamily && !m_nativeToolsDisabled &&
       (resp.err.contains(lit("tool"), Qt::CaseInsensitive) ||
        resp.err.contains(lit("function"), Qt::CaseInsensitive)))
    {
      m_nativeToolsDisabled = true;
      setStatusText(tr("Provider rejected native tools; retrying with bracket protocol..."));
      dispatchLLMRequest();
      return;
    }
    appendChatMessage(false, tr("API error:\\n") + resp.err + lit("\\n\\nRaw:\\n") +
                                 QString::fromUtf8(body));
    setStatusText(tr("Error"));
    return;
  }

  // native function-calling round
  if(resp.hasToolCalls() && m_toolUseRound < kMaxToolUseRounds && m_Ctx.IsCaptureLoaded())
  {
    if(!resp.text.trimmed().isEmpty())
      appendChatMessage(false, resp.text.trimmed());

    QJsonArray callsCanonical;
    for(const QJsonValue &v : resp.toolCalls)
    {
      QJsonObject tc = v.toObject();
      QJsonObject fn;
      fn[lit("name")] = tc[lit("name")];
      fn[lit("arguments")] =
          QString::fromUtf8(QJsonDocument(tc[lit("args")].toObject()).toJson(QJsonDocument::Compact));
      QJsonObject c;
      c[lit("id")] = tc[lit("id")];
      c[lit("type")] = lit("function");
      c[lit("function")] = fn;
      callsCanonical.append(c);
    }

    QJsonObject am;
    am[lit("role")] = lit("assistant");
    am[lit("content")] = resp.text;
    am[lit("tool_calls")] = callsCanonical;
    m_apiMessages.append(am);

    for(const QJsonValue &v : callsCanonical)
    {
      QJsonObject c = v.toObject();
      QString callId = c[lit("id")].toString();
      QString name = c[lit("function")].toObject()[lit("name")].toString();
      QJsonObject args =
          toolArgsFromString(c[lit("function")].toObject()[lit("arguments")].toString());

      m_toolUseRound++;
      setStatusText(QStringLiteral("Tool: %1 (round %2/%3)")
                        .arg(name)
                        .arg(m_toolUseRound)
                        .arg(kMaxToolUseRounds));
      appendChatMessage(
          false,
          QStringLiteral("Tool call: %1(%2)")
              .arg(name)
              .arg(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))));

      QJsonObject tr;
      tr[lit("role")] = lit("tool");
      tr[lit("tool_call_id")] = callId;
      tr[lit("content")] = executeAgentTool(name, args);
      tr[lit("rd_name")] = name;
      m_apiMessages.append(tr);
    }

    setLLMUiBusy(true);
    dispatchLLMRequest();
    return;
  }

  // legacy bracket-protocol fallback (models/providers without native tool support)
  if(m_toolUseRound < kMaxToolUseRounds && m_Ctx.IsCaptureLoaded())
  {
    QString text = resp.text;
    bool listEvents = text.contains(lit("[LIST_EVENTS]"));
    uint32_t scanEID = parseScanPass(text);
    uint32_t fetchEID = parseFetchEID(text);

    if(listEvents || scanEID > 0 || fetchEID > 0)
    {
      QString cleanText = text;
      cleanText.replace(QRegExp(lit("\\\\[LIST_EVENTS\\\\]")), QString());
      cleanText.replace(QRegExp(lit("\\\\[SCAN_PASS\\\\s+\\\\d+\\\\]")), QString());
      cleanText.replace(QRegExp(lit("\\\\[FETCH_EID\\\\s+\\\\d+\\\\]")), QString());
      cleanText = cleanText.trimmed();
      if(!cleanText.isEmpty())
      {
        appendChatMessage(false, cleanText);
        QJsonObject am;
        am[lit("role")] = lit("assistant");
        am[lit("content")] = cleanText;
        m_apiMessages.append(am);
      }

      m_toolUseRound++;
      setLLMUiBusy(true);

      if(listEvents)
      {
        setStatusText(QStringLiteral("Tool: LIST_EVENTS (round %1/%2)")
                          .arg(m_toolUseRound)
                          .arg(kMaxToolUseRounds));
        appendChatMessage(false, tr("Building event tree..."));
        QString tree = formatEventTree();
        QString followUp = QStringLiteral(
            "[Tool result: event tree]\\n%1\\n"
            "[End tool result]\\n\\n"
            "Use this event tree to understand the full rendering pipeline. "
            "Use scan_pass on marker EIDs to sample draws in a pass, "
            "or fetch_eid for a single event's full pipeline state. "
            "Analyze the rendering order and provide a complete pipeline breakdown.")
                             .arg(tree);
        QJsonObject um;
        um[lit("role")] = lit("user");
        um[lit("content")] = truncateToolBody(followUp);
        m_apiMessages.append(um);
        dispatchLLMRequest();
      }
      else if(scanEID > 0)
      {
        setStatusText(QStringLiteral("Tool: SCAN_PASS EID %1 (round %2/%3)")
                          .arg(scanEID)
                          .arg(m_toolUseRound)
                          .arg(kMaxToolUseRounds));
        sendFollowUpWithScan(scanEID);
      }
      else
      {
        setStatusText(QStringLiteral("Tool: FETCH_EID %1 (round %2/%3)")
                          .arg(fetchEID)
                          .arg(m_toolUseRound)
                          .arg(kMaxToolUseRounds));
        sendFollowUpWithData(fetchEID);
      }
      return;
    }
  }

  QJsonObject finalMsg;
  finalMsg[lit("role")] = lit("assistant");
  finalMsg[lit("content")] = resp.text;
  m_apiMessages.append(finalMsg);
  appendTurnToHistory(m_lastUserQuestion, resp.text);
  appendChatMessage(false, resp.text);
  setStatusText(tr("Ready"));
}

void AgentAssistantPanel::appendChatMessage(bool isUser, const QString &text)
{
  m_chatHistory.append({isUser, text});
  renderChatLog();
}

static QString escapeHtml(const QString &s)
{
  QString out = s;
  out.replace(QLatin1Char('&'), lit("&amp;"));
  out.replace(QLatin1Char('<'), lit("&lt;"));
  out.replace(QLatin1Char('>'), lit("&gt;"));
  out.replace(QLatin1Char('\n'), lit("<br>"));
  return out;
}

QString AgentAssistantPanel::linkifyEIDs(const QString &html)
{
  QRegExp rx(lit("\\bEID\\s*(\\d+)\\b"));
  QString out = html;
  int offset = 0;
  while(rx.indexIn(out, offset) >= 0)
  {
    int pos = rx.pos();
    QString eid = rx.cap(1);
    QString link = QStringLiteral("<a href=\"eid://%1\" style=\"color:#0868C8; font-weight:600;\">EID %1</a>").arg(eid);
    out.replace(pos, rx.matchedLength(), link);
    offset = pos + link.size();
  }
  return out;
}

void AgentAssistantPanel::renderChatLog()
{
  QString html;
  html += lit("<html><body style=\"margin:4px;\">");

  for(const ChatMessage &msg : m_chatHistory)
  {
    QString escaped = escapeHtml(msg.text);
    escaped = linkifyEIDs(escaped);

    if(msg.isUser)
    {
      html += lit(
          "<div style=\"margin:6px 0; padding:10px 14px; "
          "background-color:rgba(200,230,250,95); "
          "border:1px solid rgba(80,140,190,55); "
          "border-top:1px solid rgba(120,180,220,70); "
          "border-radius:12px; text-align:right; color:#142838;\">"
          "<b style=\"color:#0B5A9E;\">You:</b><br>%1</div>")
                  .arg(escaped);
    }
    else
    {
      html += lit(
          "<div style=\"margin:6px 0; padding:10px 14px; "
          "background-color:rgba(255,255,255,200); "
          "border:1px solid rgba(90,130,170,45); "
          "border-top:1px solid rgba(200,215,230,90); "
          "border-left:1px solid rgba(180,200,220,70); "
          "border-radius:12px; color:#1A2D42;\">"
          "<b style=\"color:#0F2840;\">Agent:</b><br>%1</div>")
                  .arg(escaped);
    }
  }

  html += lit("</body></html>");

  ui->chatLog->setHtml(html);
  QScrollBar *sb = ui->chatLog->verticalScrollBar();
  if(sb)
    sb->setValue(sb->maximum());
}

void AgentAssistantPanel::onChatLinkClicked(const QUrl &url)
{
  if(url.scheme() == lit("eid"))
  {
    bool ok = false;
    uint32_t eid = url.host().toUInt(&ok);
    if(ok && m_Ctx.IsCaptureLoaded())
    {
      m_Ctx.SetEventID({}, eid, eid);
    }
  }
}

void AgentAssistantPanel::onSnapshotToggled(bool checked)
{
  if(checked)
  {
    ui->snapshotEdit->setMaximumHeight(16777215);
    ui->snapshotEdit->setVisible(true);
    ui->snapshotToggleBtn->setText(tr("Pipeline snapshot (click to collapse)"));
  }
  else
  {
    ui->snapshotEdit->setMaximumHeight(0);
    ui->snapshotEdit->setVisible(false);
    ui->snapshotToggleBtn->setText(tr("Pipeline snapshot (click to expand)"));
  }
}

void AgentAssistantPanel::setStatusText(const QString &text)
{
  ui->statusLabel->setText(text);
}

void AgentAssistantPanel::parseTokenUsage(const QJsonObject &root)
{
  QJsonObject usage = root[lit("usage")].toObject();
  if(usage.isEmpty())
    return;

  int prompt = usage[lit("prompt_tokens")].toInt(0);
  int completion = usage[lit("completion_tokens")].toInt(0);

  if(prompt <= 0 && completion <= 0)
  {
    prompt = usage[lit("input_tokens")].toInt(0);
    completion = usage[lit("output_tokens")].toInt(0);
  }

  if(prompt > 0)
    m_totalPromptTokens += prompt;
  if(completion > 0)
    m_totalCompletionTokens += completion;

  updateTokenLabel();
}

void AgentAssistantPanel::updateTokenLabel()
{
  int total = m_totalPromptTokens + m_totalCompletionTokens;
  if(total <= 0)
  {
    ui->tokenUsageLabel->setText(QString());
    return;
  }
  ui->tokenUsageLabel->setText(
      QStringLiteral("Tokens: %1 prompt + %2 completion = %3 total")
          .arg(m_totalPromptTokens)
          .arg(m_totalCompletionTokens)
          .arg(total));
}

void AgentAssistantPanel::onSearchToggle()
{
  bool visible = !ui->searchEdit->isVisible();
  ui->searchEdit->setVisible(visible);
  ui->searchNextBtn->setVisible(visible);
  ui->searchCloseBtn->setVisible(visible);
  if(visible)
  {
    ui->searchEdit->setFocus();
    ui->searchEdit->selectAll();
  }
  else
  {
    ui->chatLog->setExtraSelections(QList<QTextEdit::ExtraSelection>());
  }
}

void AgentAssistantPanel::onSearchClose()
{
  ui->searchEdit->setVisible(false);
  ui->searchNextBtn->setVisible(false);
  ui->searchCloseBtn->setVisible(false);
  ui->chatLog->setExtraSelections(QList<QTextEdit::ExtraSelection>());
  m_searchMatchIndex = -1;
}

void AgentAssistantPanel::onSearchNext()
{
  QString term = ui->searchEdit->text();
  if(term.isEmpty())
    return;

  QTextDocument *doc = ui->chatLog->document();
  QTextCursor cursor = ui->chatLog->textCursor();
  QTextCursor found = doc->find(term, cursor);

  if(found.isNull())
    found = doc->find(term, 0);

  if(!found.isNull())
  {
    ui->chatLog->setTextCursor(found);
    ui->chatLog->ensureCursorVisible();

    QTextEdit::ExtraSelection sel;
    sel.cursor = found;
    QTextCharFormat fmt;
    fmt.setBackground(QColor(255, 255, 120));
    fmt.setForeground(QColor(0, 0, 0));
    sel.format = fmt;
    ui->chatLog->setExtraSelections(QList<QTextEdit::ExtraSelection>() << sel);
  }
}

uint32_t AgentAssistantPanel::parseFetchEID(const QString &text)
{
  QRegExp rx(lit("\\[FETCH_EID\\s+(\\d+)\\]"));
  if(rx.indexIn(text) >= 0)
  {
    bool ok = false;
    uint32_t eid = rx.cap(1).toUInt(&ok);
    if(ok)
      return eid;
  }
  return 0;
}

uint32_t AgentAssistantPanel::parseScanPass(const QString &text)
{
  QRegExp rx(lit("\\[SCAN_PASS\\s+(\\d+)\\]"));
  if(rx.indexIn(text) >= 0)
  {
    bool ok = false;
    uint32_t eid = rx.cap(1).toUInt(&ok);
    if(ok)
      return eid;
  }
  return 0;
}

void AgentAssistantPanel::sendFollowUpWithScan(uint32_t markerEID)
{
  appendChatMessage(false,
                    QStringLiteral("Scanning pass EID %1 ...").arg(markerEID));

  QString scan = scanPassSummary(markerEID);
  if(scan.isEmpty())
  {
    appendChatMessage(false,
                      QStringLiteral("Could not scan pass for EID %1.").arg(markerEID));
    setLLMUiBusy(false);
    return;
  }

  QString followUp = QStringLiteral(
      "[Tool result: pass scan for EID %1]\\n%2\\n"
      "[End tool result]\\n\\n"
      "Continue your analysis using this pass scan data. "
      "You may call fetch_eid for more detail on specific draws, scan_pass on sub-passes, or "
      "list_events for the full tree. When ready, provide your complete pipeline analysis to the "
      "user.")
                       .arg(markerEID)
                       .arg(scan);

  QJsonObject um;
  um[lit("role")] = lit("user");
  um[lit("content")] = truncateToolBody(followUp);
  m_apiMessages.append(um);
  dispatchLLMRequest();
}

void AgentAssistantPanel::sendFollowUpWithData(uint32_t eid)
{
  appendChatMessage(false,
                    QStringLiteral("Fetching pipeline data for EID %1 ...").arg(eid));

  QString snap = snapshotForEID(eid);
  if(snap.isEmpty())
  {
    appendChatMessage(false,
                      QStringLiteral("Could not fetch data for EID %1.").arg(eid));
    setLLMUiBusy(false);
    return;
  }

  QString followUp = QStringLiteral(
      "[Tool result: pipeline snapshot for EID %1]\\n%2\\n"
      "[End tool result]\\n\\n"
      "Continue your analysis. Available tools: list_events, scan_pass, fetch_eid. "
      "When ready, provide your complete answer to the user.")
                         .arg(eid)
                         .arg(snap);

  QJsonObject um;
  um[lit("role")] = lit("user");
  um[lit("content")] = truncateToolBody(followUp);
  m_apiMessages.append(um);
  dispatchLLMRequest();
}

void AgentAssistantPanel::dispatchLLMRequest()
{
  PersistentConfig &cfg = m_Ctx.Config();
  const int p = ui->providerCombo->currentIndex();
  QString apiKey = ui->apiTokenEdit->text().trimmed();

  QUrl url;
  QByteArray payload;
  QNetworkRequest req;
  req.setHeader(QNetworkRequest::ContentTypeHeader, lit("application/json"));
  const bool withTools = !m_nativeToolsDisabled;

  switch((AgentLLMBackend)p)
  {
    case AgentLLMBackend::OpenAI:
      url = QUrl(lit("https://api.openai.com/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_OpenAIModel), m_apiMessages,
                                      withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    case AgentLLMBackend::Anthropic:
      url = QUrl(lit("https://api.anthropic.com/v1/messages"));
      payload = makeAnthropicPayload(rdcToQString(cfg.AgentAssistant_AnthropicModel), m_apiMessages,
                                     withTools);
      req.setRawHeader("x-api-key", apiKey.toUtf8());
      req.setRawHeader("anthropic-version", "2023-06-01");
      break;
    case AgentLLMBackend::GoogleGemini:
    {
      QString mid = rdcToQString(cfg.AgentAssistant_GoogleModel).trimmed();
      if(mid.isEmpty())
        mid = lit("gemini-1.5-flash");
      url.setUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1"
                                ":generateContent")
                     .arg(mid));
      QUrlQuery q;
      q.addQueryItem(lit("key"), apiKey);
      url.setQuery(q);
      payload = makeGeminiPayload(m_apiMessages, withTools);
      break;
    }
    case AgentLLMBackend::AzureOpenAI:
    {
      QString ep = rdcToQString(cfg.AgentAssistant_AzureEndpoint).trimmed();
      QString dep = rdcToQString(cfg.AgentAssistant_AzureDeployment).trimmed();
      while(ep.endsWith(QLatin1Char('/')))
        ep.chop(1);
      url = QUrl(ep + lit("/openai/deployments/") + dep +
                 lit("/chat/completions?api-version=2024-02-15-preview"));
      payload = makeOpenAIChatPayload(dep, m_apiMessages, withTools);
      req.setRawHeader("api-key", apiKey.toUtf8());
      break;
    }
    case AgentLLMBackend::OpenAICompatible:
    {
      QString base = rdcToQString(cfg.AgentAssistant_CompatibleBaseUrl).trimmed();
      while(base.endsWith(QLatin1Char('/')))
        base.chop(1);
      if(base.isEmpty())
        base = lit("https://api.openai.com");
      url = QUrl(base + lit("/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_OpenAIModel).trimmed(),
                                      m_apiMessages, withTools);
      if(!apiKey.isEmpty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    }
    case AgentLLMBackend::OpenRouter:
      url = QUrl(lit("https://openrouter.ai/api/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_OpenRouterModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      req.setRawHeader("HTTP-Referer", "https://renderdoc.org");
      req.setRawHeader("X-Title", "RenderDoc Pipeline Agent");
      break;
    case AgentLLMBackend::GLM_Zhipu:
      url = QUrl(lit("https://open.bigmodel.cn/api/paas/v4/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_GLMModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    case AgentLLMBackend::GitHubModels:
      url = QUrl(lit("https://models.github.ai/inference/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_GitHubModelsModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      req.setRawHeader("Accept", "application/vnd.github+json");
      req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
      break;
    case AgentLLMBackend::DeepSeek:
      url = QUrl(lit("https://api.deepseek.com/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_DeepSeekModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    case AgentLLMBackend::DashScope:
      url = QUrl(lit("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_DashScopeModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    case AgentLLMBackend::Moonshot:
      url = QUrl(lit("https://api.moonshot.cn/v1/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_MoonshotModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    case AgentLLMBackend::Qianfan:
      url = QUrl(lit("https://qianfan.baidubce.com/v2/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_QianfanModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
    case AgentLLMBackend::Volcano:
      url = QUrl(lit("https://ark.cn-beijing.volces.com/api/v3/chat/completions"));
      payload = makeOpenAIChatPayload(rdcToQString(cfg.AgentAssistant_VolcanoModel).trimmed(),
                                      m_apiMessages, withTools);
      req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
      break;
  }

  req.setUrl(url);
  req.setRawHeader("User-Agent", "RenderDoc-PipelineAgent/1.1");

  setStatusText(QStringLiteral("Waiting for LLM response (round %1/%2)...")
                    .arg(m_toolUseRound + 1)
                    .arg(kMaxToolUseRounds));

  m_activeReply = m_net->post(req, payload);
  QObject::connect(m_activeReply, &QNetworkReply::finished, this,
                   &AgentAssistantPanel::onLLMFinished);
}

QString AgentAssistantPanel::executeAgentTool(const QString &name, const QJsonObject &args)
{
  if(!m_Ctx.IsCaptureLoaded())
    return lit("Error: no capture is loaded.");

  if(name == lit("list_events") || name == lit("LIST_EVENTS"))
    return formatEventTree();

  static const char *eidKeys[] = {"marker_eid", "eid", "markerEID", "event_id", "eventid", "EID"};
  uint32_t eid = 0;
  for(const char *k : eidKeys)
  {
    if(args.contains(QString::fromUtf8(k)))
    {
      bool ok = false;
      eid = args.value(QString::fromUtf8(k)).toVariant().toUInt(&ok);
      if(ok && eid > 0)
        break;
      eid = 0;
    }
  }
  if(eid == 0)
    return lit("Error: missing or invalid integer argument (marker_eid for scan_pass, eid for "
               "fetch_eid).");

  if(name == lit("scan_pass") || name == lit("SCAN_PASS"))
  {
    QString scan = scanPassSummary(eid);
    return scan.isEmpty() ? QStringLiteral("No data found under marker EID %1.").arg(eid) : scan;
  }
  if(name == lit("fetch_eid") || name == lit("FETCH_EID"))
  {
    QString snap = snapshotForEID(eid);
    return snap.isEmpty() ? QStringLiteral("No pipeline snapshot available for EID %1.").arg(eid)
                          : snap;
  }
  return lit("Error: unknown tool '") + name +
         lit("'. Available tools: list_events, scan_pass, fetch_eid.");
}

void AgentAssistantPanel::appendTurnToHistory(const QString &question, const QString &answer)
{
  if(question.isEmpty() || answer.isEmpty())
    return;
  QJsonObject u;
  u[lit("role")] = lit("user");
  u[lit("content")] = question;
  QJsonObject a;
  a[lit("role")] = lit("assistant");
  a[lit("content")] = answer;
  m_apiHistory.append(u);
  m_apiHistory.append(a);
  while(m_apiHistory.size() > 8)
    m_apiHistory.removeAt(0);
}

void AgentAssistantPanel::AnalyzeShaderText(const rdcstr &shaderTitle, const rdcstr &shaderText)
{
  m_pendingAnalyzeTitle = rdcToQString(shaderTitle);
  m_pendingAnalyzeText = rdcToQString(shaderText);
  m_Ctx.ShowAgentAssistant();
  ui->questionEdit->setText(tr("What is this shader doing? What visual effect does it achieve?"));
}

QVector<AgentAssistantPanel::ThumbData> AgentAssistantPanel::grabThumbImages()
{
  QVector<ThumbData> ret;
  if(!m_Ctx.IsCaptureLoaded())
    return ret;

  struct ThumbJob
  {
    ResourceId id;
    QString label;
  };
  QVector<ThumbJob> jobs;
  rdcarray<Descriptor> outputs = m_Ctx.CurPipelineState().GetOutputTargets();
  for(int32_t i = 0; i < outputs.count() && jobs.count() < 4; i++)
    jobs.push_back({outputs[i].resource, QStringLiteral("output_%1").arg(jobs.count() + 1)});
  Descriptor depth = m_Ctx.CurPipelineState().GetDepthTarget();
  if(depth.resource != ResourceId())
    jobs.push_back({depth.resource, lit("depth")});

  if(jobs.isEmpty())
    return ret;

  const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  for(const ThumbJob &job : jobs)
  {
    const QString path = tmpDir + QStringLiteral("/renderdoc_agent_%1.png").arg(job.label);
    m_Ctx.Replay().BlockInvoke(
        [&job, &path](IReplayController *r) {
          TextureSave save;
          save.resourceId = job.id;
          save.destType = FileType::PNG;
          save.mip = 0;
          r->SaveTexture(save, path.toUtf8().constData());
        });

    QImage img(path);
    QFile::remove(path);
    if(img.isNull())
      continue;
    if(img.width() > 512 || img.height() > 512)
      img = img.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    if(png.isEmpty() || png.size() > 2 * 1024 * 1024)
      continue;

    ret.push_back({job.label, png});
  }
  return ret;
}

QJsonArray AgentAssistantPanel::thumbnailsToApiParts(const QVector<ThumbData> &thumbs)
{
  QJsonArray parts;
  for(const ThumbData &t : thumbs)
  {
    QJsonObject urlObj;
    urlObj[lit("url")] = lit("data:image/png;base64,") + QString::fromUtf8(t.png.toBase64());
    QJsonObject imgObj;
    imgObj[lit("type")] = lit("image_url");
    imgObj[lit("image_url")] = urlObj;
    parts.append(imgObj);
  }
  return parts;
}

QString AgentAssistantPanel::lastAssistantMessage() const
{
  for(int i = m_chatHistory.count() - 1; i >= 0; i--)
  {
    if(!m_chatHistory[i].isUser)
      return m_chatHistory[i].text;
  }
  return QString();
}

void AgentAssistantPanel::exportReport()
{
  if(!m_Ctx.IsCaptureLoaded())
  {
    QMessageBox::information(this, tr("Pipeline Agent"), tr("Load a capture first."));
    return;
  }

  QString path = RDDialog::getSaveFileName(this, tr("Export analysis report"),
                                           lit("renderdoc_report.md"), tr("Markdown (*.md)"));
  if(path.isEmpty())
    return;

  setCursor(Qt::WaitCursor);

  QString md;
  md += lit("# RenderDoc Capture Analysis\n\n");
  md += lit("- Capture: `%1`\n").arg(rdcToQString(m_Ctx.GetCaptureFilename()));
  md += lit("- Current EID: %1\n").arg(m_Ctx.CurEvent());
  md += lit("- Exported: %1\n\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate));

  md += lit("## Event tree\n\n```text\n%1\n```\n\n").arg(formatEventTree());

  md += lit("## Top-level passes\n\n");
  const rdcarray<ActionDescription> &roots = m_Ctx.CurRootActions();
  int passCount = 0;
  for(const ActionDescription &a : roots)
  {
    if(a.children.empty())
      continue;
    if(++passCount > 12)
    {
      md += lit("\n_(further passes omitted)_\n");
      break;
    }
    QString name = QString::fromUtf8(a.customName.c_str());
    if(name.isEmpty())
      name = QStringLiteral("pass");
    md += lit("### %1 (EID %2)\n\n```text\n%3\n```\n\n")
              .arg(name)
              .arg(a.eventId)
              .arg(scanPassSummary(a.eventId));
  }
  if(passCount == 0)
    md += lit("_No marker passes at the top level._\n\n");

  QString snap = ui->snapshotEdit->toPlainText();
  if(snap.size() > 200000)
  {
    snap.truncate(200000);
    snap += lit("\n... (truncated)\n");
  }
  md += lit("## Current event snapshot (EID %1)\n\n```text\n%2\n```\n\n")
            .arg(m_Ctx.CurEvent())
            .arg(snap);

  QVector<ThumbData> thumbs = grabThumbImages();
  if(!thumbs.isEmpty())
  {
    QFileInfo fi(path);
    const QString imgDirName = fi.completeBaseName() + lit("_files");
    QDir(fi.dir()).mkpath(imgDirName);
    md += lit("## Texture thumbnails\n\n");
    for(const ThumbData &t : thumbs)
    {
      const QString fname = QStringLiteral("%1/%2.png").arg(imgDirName).arg(t.label);
      QFile f(fi.dir().filePath(fname));
      if(f.open(QIODevice::WriteOnly))
      {
        f.write(t.png);
        f.close();
        md += lit("![%1](%2)\n").arg(t.label).arg(fname);
      }
    }
    md += lit("\n");
  }

  QString conclusion = lastAssistantMessage();
  if(!conclusion.isEmpty())
    md += lit("## Latest AI analysis conclusion\n\n%1\n").arg(conclusion);

  QFile out(path);
  if(out.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    out.write(md.toUtf8());
    out.close();
    unsetCursor();
    QMessageBox::information(this, tr("Pipeline Agent"),
                             tr("Report exported to:\n%1").arg(path));
  }
  else
  {
    unsetCursor();
    QMessageBox::warning(this, tr("Pipeline Agent"), tr("Could not write %1").arg(path));
  }
}
