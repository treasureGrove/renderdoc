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
#include "Code/Interface/QRDInterface.h"

class QNetworkAccessManager;
class QNetworkReply;

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

private slots:
  void copyPrompt();
  void copyContextOnly();
  void refreshSnapshot();
  void sendToLLM();
  void onLLMFinished();
  void onProviderChanged(int idx);

private:
  Ui::AgentAssistantPanel *ui;
  ICaptureContext &m_Ctx;
  QNetworkAccessManager *m_net = NULL;
  int m_activeLLMProvider = -1;

  void rebuildSnapshot();
  QString formatPipelineSnapshot();
  void loadSettingsFromConfig();
  void saveSettingsToConfig();
  void updateProviderUi();
  void setLLMUiBusy(bool busy);
  void repopulateModelCombo();
};
