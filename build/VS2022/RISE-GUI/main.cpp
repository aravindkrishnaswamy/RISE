//////////////////////////////////////////////////////////////////////
//
//  main.cpp - Entry point for the RISE Qt GUI application
//
//////////////////////////////////////////////////////////////////////

#include <QApplication>
#include "MainWindow.h"
#include "Theme.h"

int main(int argc, char* argv[])
{
    // UI redesign: the workspace chrome (Theme.h) is dark-only.  The
    // "windows11" Fluent style ignores QPalette in several places
    // (menus, scrollbars), which fights that design — Fusion + an
    // explicit dark QPalette built from the Theme tokens is the
    // reliable cross-widget dark path.  Mirrors the macOS client's
    // blanket `NSApp.appearance = .darkAqua` in RISEApp.swift.
    QApplication app(argc, argv);
    app.setApplicationName("RISE");
    app.setOrganizationName("RISE");

    // Register the bundled IBM Plex faces before any widget is created —
    // Theme::sans/mono fall back to system fonts if this fails.
    Theme::registerFonts();

    // Must run before MainWindow (or any other widget) is constructed
    // so every widget picks up the dark palette + app font from the
    // start rather than needing a repaint after the fact.
    Theme::applyDarkPalette(app);

    // MainWindow's constructor sets its own default size (1440x900)
    // and minimum size (1320x760) — see MainWindow.cpp.
    MainWindow window;
    window.show();

    return app.exec();
}
