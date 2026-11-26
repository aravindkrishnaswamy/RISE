//////////////////////////////////////////////////////////////////////
//
//  LogWidget.h - Color-coded scrolling log output panel, restyled
//    (RISE UI redesign) with a severity-filter header, live WARN/ERR
//    counters, a text filter, a follow (auto-scroll) toggle, and a
//    collapse-to-status-strip affordance.
//
//  Ported from the Mac app's LogOutputView.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef LOGWIDGET_H
#define LOGWIDGET_H

#include <QWidget>
#include <QColor>
#include <QVector>
#include <QString>

class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QLabel;
class QLineEdit;
class QStackedWidget;

class LogWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LogWidget(QWidget* parent = nullptr);

public slots:
    void appendLog(int level, const QString& message);
    void clearLog();

    /// Collapse to the 28px status strip / expand back to the full
    /// view.  Distinct from the View > Log (Alt+L) menu action, which
    /// hides/shows the WHOLE drawer (this widget included) inside
    /// MainWindow's center column -- this is an in-place collapse
    /// mirroring the design comp's chevron affordance.
    void setCollapsed(bool collapsed);

private slots:
    void onSeverityFilterClicked();
    void onTextFilterChanged(const QString& text);
    void onFollowToggled();
    void onCollapseToggled();

protected:
    /// LIVE THEME-SWITCH CONTRACT (Theme.h): calls the base
    /// implementation first, then restyleTheme() on QEvent::PaletteChange.
    void changeEvent(QEvent* e) override;

private:
    /// LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies every one of
    /// LogWidget's token-dependent styling sites (header chrome, filter
    /// pills, count chips, the text-edit palette, the collapsed strip)
    /// from the CURRENT Theme:: token values, and re-invokes
    /// rebuildVisibleText() so already-appended log lines' per-line
    /// QTextCharFormat colors (baked in at append time -- see
    /// colorForLevel/appendEntryIfVisible) pick up the new palette too,
    /// not just future lines. Called once at the end of the constructor
    /// and again from changeEvent() on every QEvent::PaletteChange.
    /// Idempotent, creates no widgets.
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // See MainWindow.h for the full rationale; uniform across every
    // changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    struct LogEntry {
        int     level = 0;
        QString message;   // already carries an "HH:mm:ss " prefix
    };

    // Log level values from ILog.h (bitmask).
    enum : int {
        kLevelEvent   = 1,
        kLevelInfo    = 2,
        kLevelWarning = 4,
        kLevelError   = 8,
        kLevelFatal   = 16,
    };

    bool passesFilter(const LogEntry& e) const;
    void rebuildVisibleText();
    void appendEntryIfVisible(const LogEntry& e);
    void updateCounters();
    void updateCollapsedStrip();
    QColor colorForLevel(int level) const;

    QVector<LogEntry> m_entries;
    int m_warnCount = 0;
    int m_errCount  = 0;

    // Current severity filter: 0 = All, else one of the kLevel* masks.
    int m_severityFilter = 0;
    QString m_textFilter;
    bool m_follow = true;
    bool m_collapsed = false;

    QStackedWidget* m_stack = nullptr;   // index 0 = full view, index 1 = collapsed strip

    // ---- Full view --------------------------------------------------------
    // Promoted from ctor-locals to members so restyleTheme() can
    // re-apply their baked Theme:: styling on a live theme switch --
    // neither is otherwise touched by any update*() method.
    QWidget*        m_header        = nullptr;
    QLabel*         m_headerTitle   = nullptr;
    QVector<QToolButton*> m_severityButtons;   // All / Info / Warn / Error
    QLabel*         m_warnCountChip = nullptr;
    QLabel*         m_errCountChip  = nullptr;
    QLineEdit*      m_filterEdit    = nullptr;
    QToolButton*    m_followButton  = nullptr;
    QPushButton*    m_clearBtn      = nullptr;
    QToolButton*    m_collapseBtn   = nullptr;
    QPlainTextEdit* m_textEdit      = nullptr;

    // ---- Collapsed strip ----------------------------------------------------
    // m_collapsedStrip promoted the same way (ClickableStrip is a
    // QWidget subclass defined in the .cpp's anonymous namespace, so
    // it's stored here by its QWidget base).
    QWidget* m_collapsedStrip      = nullptr;
    QLabel*  m_collapsedStripTitle = nullptr;
    QLabel*  m_collapsedText       = nullptr;
    QLabel*  m_collapsedWarnChip   = nullptr;

    static constexpr int MAX_LOG_MESSAGES = 10000;
};

#endif // LOGWIDGET_H
