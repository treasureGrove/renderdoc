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

#pragma once

#include <QFrame>
#include <QList>
#include <QPair>
#include <QPixmap>
#include "Code/Interface/QRDInterface.h"

class QNetworkAccessManager;
class QNetworkReply;
class QShortcut;
class QShowEvent;
class QUrl;

namespace Ui
{
class AgentAssistantPanel;
}

class AgentAssistantPanel : public QFrame, public IAgentAssistant, public ICaptureViewer
{
  Q_OBJECT

public:
  explicit AgentAssistantPanel(ICaptureContext &ctx, QWidget *parent = 0);
  ~AgentAssistantPanel();

  QWidget *Widget() override { return this; }

  void OnCaptureLoaded() override;
  void OnCaptureClosed() override;
  void OnSelectedEventChanged(uint32_t eventId) override;
  void OnEventChanged(uint32_t eventId) override;

protected:
  void showEvent(QShowEvent *e) override;
  void paintEvent(QPaintEvent *e) override;

private slots:
  void copyPrompt();
  void copyContextOnly();
  void refreshSnapshot();
  void sendToLLM();
  void stopLLM();
  void onLLMFinished();
  void onProviderChanged(int idx);
  void onContinueToChat();
  void onPersistConnectionFields();
  void onChatLinkClicked(const QUrl &url);
  void onSnapshotToggled(bool checked);
  void onSearchNext();
  void onSearchClose();
  void onSearchToggle();

private:
  Ui::AgentAssistantPanel *ui;
  ICaptureContext &m_Ctx;
  QNetworkAccessManager *m_net = NULL;
  QNetworkReply *m_activeReply = NULL;
  QShortcut *m_searchShortcut = NULL;
  QPixmap m_bgPixmap;
  QPixmap m_bgScaled;
  QSize m_bgScaledSize;
  int m_activeLLMProvider = -1;
  QString m_helpTextFull;
  int m_searchMatchIndex = -1;

  int m_totalPromptTokens = 0;
  int m_totalCompletionTokens = 0;

  struct ChatMessage
  {
    bool isUser;
    QString text;
  };
  QList<ChatMessage> m_chatHistory;

  int m_toolUseRound = 0;
  static const int kMaxToolUseRounds = 15;
  QString m_lastUserQuestion;
  uint32_t m_preToolEID = 0;

  void rebuildSnapshot();
  QString formatPipelineSnapshot();
  QString formatEventTree();
  QString snapshotForEID(uint32_t eid);
  QString scanPassSummary(uint32_t markerEID);
  void loadSettingsFromConfig();
  void saveSettingsToConfig();
  void updateProviderUi();
  void setLLMUiBusy(bool busy);
  void repopulateModelCombo();
  bool hasMinimumLLMConnection() const;
  void updateSetupVsChatLayout();
  void applyPanelChrome();
  void applyReadableFonts();
  void appendChatMessage(bool isUser, const QString &text);
  void renderChatLog();
  QString linkifyEIDs(const QString &text);
  uint32_t parseFetchEID(const QString &text);
  uint32_t parseScanPass(const QString &text);
  void sendFollowUpWithData(uint32_t eid);
  void sendFollowUpWithScan(uint32_t markerEID);
  void sendLLMRequestRaw(const QString &userBody);
  void setStatusText(const QString &text);
  void parseTokenUsage(const QJsonObject &root);
  void updateTokenLabel();
};
