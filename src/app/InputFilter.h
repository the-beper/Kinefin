// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>

namespace Kinefin {
// The application-wide input rules of the settings: keyboard autorepeat follows main.enableInputRepeat, except in a field that takes text (there the arrows and the key must repeat); main.disablemouse drops mouse input on the full player and in the picture-in-picture window
class InputFilter final : public QObject {
    Q_OBJECT
  public:
    explicit InputFilter(QObject *parent = nullptr);
    void setRepeatEnabled(bool enabled) { m_repeatEnabled = enabled; }
    void setMouseBlocked(bool blocked) { m_mouseBlocked = blocked; }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    bool blocksMouse(QObject *object) const;

    bool m_repeatEnabled = true;
    bool m_mouseBlocked = false;
};
} // namespace Kinefin
