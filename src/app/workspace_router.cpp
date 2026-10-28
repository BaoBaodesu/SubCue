#include "workspace_router.h"

#include "app_controller.h"
#include "rough_cut_controller.h"
#include "audio_preview_controller.h"
#include "project_session.h"

namespace subcue {

WorkspaceRouter::WorkspaceRouter(AppController *editor, RoughCutController *roughCut, QObject *parent)
    : QObject(parent), editor_(editor), roughCut_(roughCut)
{
    connect(editor_, &AppController::busyChanged, this, &WorkspaceRouter::canSwitchChanged);
    connect(roughCut_, &RoughCutController::busyChanged, this, &WorkspaceRouter::canSwitchChanged);
}

bool WorkspaceRouter::canSwitch() const
{
    return editor_ && roughCut_ && (!session_ || !session_->busy());
}
void WorkspaceRouter::setProjectSession(ProjectSession *session)
{
    if (session_) disconnect(session_, nullptr, this, nullptr);
    session_ = session;
    if (session_) connect(session_, &ProjectSession::busyChanged, this, &WorkspaceRouter::canSwitchChanged);
    emit canSwitchChanged();
}

bool WorkspaceRouter::switchTo(const QString &workspace)
{
    if (!canSwitch() || (workspace != QLatin1String("subtitle")
        && workspace != QLatin1String("roughcut") && workspace != QLatin1String("voice"))) return false;
    if (workspace_ == workspace) return true;
    if (preview_) preview_->stop();
    if (workspace_ == QLatin1String("subtitle")) editor_->releasePlayback();
    else if (workspace_ == QLatin1String("roughcut")) roughCut_->releasePlayback();
    workspace_ = workspace;
    if (workspace_ == QLatin1String("subtitle")) editor_->claimPlayback();
    else if (workspace_ == QLatin1String("roughcut")) roughCut_->claimPlayback();
    emit workspaceChanged();
    return true;
}
void WorkspaceRouter::setAudioPreview(AudioPreviewController *preview)
{
    if (preview_) { preview_->stop(); disconnect(preview_, nullptr, this, nullptr); }
    preview_ = preview;
    if (!preview) return;
    connect(preview, &AudioPreviewController::requested, this, &WorkspaceRouter::suspendPlayback);
    connect(preview, &AudioPreviewController::released, this, &WorkspaceRouter::restorePlayback);
}
void WorkspaceRouter::suspendPlayback() { editor_->releasePlayback(); roughCut_->releasePlayback(); }
void WorkspaceRouter::restorePlayback()
{
    if (workspace_ == QLatin1String("subtitle")) editor_->claimPlayback();
    else if (workspace_ == QLatin1String("roughcut")) roughCut_->claimPlayback();
}

} // namespace subcue
