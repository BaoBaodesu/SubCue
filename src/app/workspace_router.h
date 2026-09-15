#pragma once

#include <QtCore/QObject>

namespace subcue {

class AppController;
class RoughCutController;

class WorkspaceRouter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString workspace READ workspace NOTIFY workspaceChanged)
    Q_PROPERTY(bool canSwitch READ canSwitch NOTIFY canSwitchChanged)

public:
    WorkspaceRouter(AppController *editor, RoughCutController *roughCut, QObject *parent = nullptr);

    [[nodiscard]] QString workspace() const { return workspace_; }
    [[nodiscard]] bool canSwitch() const;
    Q_INVOKABLE bool switchTo(const QString &workspace);

signals:
    void workspaceChanged();
    void canSwitchChanged();

private:
    AppController *editor_ = nullptr;
    RoughCutController *roughCut_ = nullptr;
    QString workspace_ = QStringLiteral("subtitle");
};

} // namespace subcue
