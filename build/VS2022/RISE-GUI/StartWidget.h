//////////////////////////////////////////////////////////////////////
//
//  StartWidget.h - The no-scene launch screen (docs/gui/START_SCREEN.md).
//
//    Replaces the old "Open a .RISEscene file to begin" placeholder with
//    the three real entry intents, on screen simultaneously:
//      1. Resume  — a one-click list of the last 10 scenes (with missing-
//                   file states and per-row remove).
//      2. Locate  — the native file picker (also Ctrl+O), plus drag-and-
//                   drop of a .RISEscene anywhere on this widget.
//      3. Create  — a prompt the agent builds a scene from: loads the
//                   bundled EMPTY starter template as an untitled scene,
//                   switches to the Agent tab, and auto-sends the prompt
//                   (ratified decisions #2/#3: truly-empty starter,
//                   auto-send).
//
//    Grouped 2-column layout (decision #1): the two "open existing"
//    paths share the left column; the agent path owns the right column.
//
//    Mirrors the macOS StartView.swift.  This widget owns no scene state
//    itself -- every action is a signal MainWindow connects to the exact
//    same code paths Open Scene / Close Scene / the chat send already use.
//
//////////////////////////////////////////////////////////////////////

#ifndef STARTWIDGET_H
#define STARTWIDGET_H

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QMap>

class QLabel;
class QVBoxLayout;
class QPushButton;
class QPlainTextEdit;
class QFrame;
class QDragEnterEvent;
class QDropEvent;

class StartWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StartWidget(QWidget* parent = nullptr);

public slots:
    /// Recent-scenes data (spec §5.4): `paths` is the ordered, capped-at-10
    /// path list (most-recent-first, same list File > Open Recent uses);
    /// `meta` maps path -> last-opened epoch seconds (absent entries render
    /// without a time label, matching old-install / never-recorded rows).
    /// Existence of each path is re-checked HERE, at rebuild time ("verified
    /// per render of the screen" -- spec §4.1), not cached from a stale
    /// snapshot -- cheap at <= 10 rows.  Call whenever the underlying data
    /// changes AND right before this widget becomes visible again.
    void setRecents(const QStringList& paths, const QMap<QString, qint64>& meta);

    /// Start screen readiness (spec §6): mirrors the chat panel's real
    /// provider/key state so the Create column never dead-ends.
    /// `providerDisplayName` is shown as the chip when `configured` is
    /// true; ignored otherwise.
    void setAgentReadiness(bool configured, const QString& providerDisplayName);

    /// A load error banner across the top of the screen (spec §8, "a
    /// failed load lands back here").  Empty `message` hides the banner.
    void setErrorMessage(const QString& message);

    /// Double-click / re-entrancy guard for Create (spec §8): the button
    /// disables and reads "Creating..." while true.  MainWindow resets
    /// this to false on a load failure; a successful load simply hides
    /// this widget, so no explicit reset is needed on success (but the
    /// caller resets it anyway before the widget is shown again, e.g.
    /// after Close Scene, as a defensive refresh).
    void setCreateInFlight(bool inFlight);

signals:
    /// A recent row (or a drag-and-dropped file) was activated -- open it
    /// through the SAME path Open Recent / Open Scene use.  Only ever
    /// emitted for a path that existed at the moment of the gesture (a
    /// missing recent row has no click handler at all -- spec §4.1).
    void openPathRequested(const QString& path);

    /// The ✕ on a missing recent row.
    void removeRecentRequested(const QString& path);

    /// "Browse files..." -- routes to the existing native picker.
    void browseRequested();

    /// Create clicked with a non-empty, agent-configured-ready prompt.
    /// MainWindow loads the starter scene as untitled, switches to the
    /// Agent tab, and auto-sends `prompt`.  Never emitted for an empty
    /// prompt (that focuses the box instead, spec §4.3) or while
    /// unconfigured (the button is hidden then).
    void createRequested(const QString& prompt);

    /// "Set up..." in the unconfigured state -- opens the chat panel's
    /// own provider/key settings (this platform has no separate settings
    /// popover; the panel's provider row IS the settings surface).
    void setupAgentRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QWidget* buildOpenColumn();
    QWidget* buildCreateColumn();
    QWidget* buildRecentRow(const QString& path, qint64 epochSeconds);
    void rebuildRecentsList();
    void clearRecentsLayout();
    void updateReadinessUI();
    void onCreateClicked();
    static QString relativeTimeString(qint64 epochSeconds);

    // Recent-scenes data, pushed by MainWindow via setRecents().
    QStringList m_recentPaths;
    QMap<QString, qint64> m_recentMeta;

    // Left column -- recents list.
    QFrame* m_recentsContainer = nullptr;
    QVBoxLayout* m_recentsLayout = nullptr;

    // Right column -- create-with-agent.
    QLabel* m_providerChip = nullptr;
    QPlainTextEdit* m_promptEdit = nullptr;
    QPushButton* m_createBtn = nullptr;
    QLabel* m_createFootnote = nullptr;
    QWidget* m_unconfiguredContainer = nullptr;
    bool m_agentConfigured = false;
    QString m_providerName;
    bool m_createInFlight = false;

    // Error banner (top of screen, spec §8).
    QWidget* m_errorBanner = nullptr;
    QLabel* m_errorLabel = nullptr;
};

#endif // STARTWIDGET_H
