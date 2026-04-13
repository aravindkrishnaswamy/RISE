//////////////////////////////////////////////////////////////////////
//
//  MainWindow.h - Main application window with 3-panel layout.
//
//  Ported from the Mac app's ContentView.swift.
//  Layout: [optional editor | [render view / [controls | log]]]
//
//////////////////////////////////////////////////////////////////////

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>

class RenderEngine;
class RenderWidget;
class ControlsWidget;
class LogWidget;
class SceneEditor;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpenScene();
    void onEditToggle();
    void onClear();
    void onRender();
    void onRenderAnimation();
    void onCancel();

    void onStateChanged(int newState);
    void onSceneSizeDetected(int width, int height);
    void onSaveAndReload(const QString& filePath);

private:
    void createMenuBar();
    void createStatusBar();
    void updateStatusBar();

    RenderEngine* m_engine = nullptr;
    RenderWidget* m_renderWidget = nullptr;
    ControlsWidget* m_controlsWidget = nullptr;
    LogWidget* m_logWidget = nullptr;
    SceneEditor* m_sceneEditor = nullptr;

    QSplitter* m_mainSplitter = nullptr;     // horizontal: editor | right
    QSplitter* m_rightSplitter = nullptr;    // vertical: render | bottom
    QSplitter* m_bottomSplitter = nullptr;   // horizontal: controls | log

    bool m_editorVisible = false;
};

#endif // MAINWINDOW_H
