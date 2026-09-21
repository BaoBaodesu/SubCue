#include "workspace_router.h"

#include "app_controller.h"
#include "rough_cut_controller.h"

namespace subcue {

WorkspaceRouter::WorkspaceRouter(AppController *editor, RoughCutController *roughCut, QObject *parent)
    : QObject(parent), editor_(editor), roughCut_(roughCut)
{
    connect(editor_, &AppController::busyChanged, this, &WorkspaceRouter::canSwitchChanged);
    connect(roughCut_, &RoughCutController::busyChanged, this, &WorkspaceRouter::canSwitchChanged);
}

bool WorkspaceRouter::canSwitch() const
{
    return editor_ && roughCut_ && !editor_->busy() && !roughCut_->busy();
}

bool WorkspaceRouter::switchTo(const QString &workspace)
{
    if (!canSwitch() || (workspace != QLatin1String("subtitle")
        && workspace != QLatin1String("roughcut"))) return false;
    if (workspace_ == workspace) return true;
    if (workspace_ == QLatin1String("subtitle")) editor_->releasePlayback();
    else roughCut_->releasePlayback();
    workspace_ = workspace;
    if (workspace_ == QLatin1String("subtitle")) editor_->claimPlayback();
    else roughCut_->claimPlayback();
    emit workspaceChanged();
    return true;
}

} // namespace subcue
