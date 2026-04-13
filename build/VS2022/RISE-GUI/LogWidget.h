//////////////////////////////////////////////////////////////////////
//
//  LogWidget.h - Color-coded scrolling log output panel.
//
//  Ported from the Mac app's LogOutputView.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef LOGWIDGET_H
#define LOGWIDGET_H

#include <QWidget>
#include <QPlainTextEdit>
#include <QPushButton>

class LogWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LogWidget(QWidget* parent = nullptr);

public slots:
    void appendLog(int level, const QString& message);
    void clearLog();

private:
    QPlainTextEdit* m_textEdit = nullptr;
    QPushButton* m_clearBtn = nullptr;
    int m_messageCount = 0;

    static constexpr int MAX_LOG_MESSAGES = 10000;
};

#endif // LOGWIDGET_H
