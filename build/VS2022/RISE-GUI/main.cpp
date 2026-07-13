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
    // UI redesign: the workspace chrome (Theme.h) defaults to dark, with
    // a runtime light/dark switch (UI refinement item 3 — File > Theme).
    // The "windows11" Fluent style ignores QPalette in several places
    // (menus, scrollbars), which fights that design — Fusion + an
    // explicit QPalette built from the Theme tokens is the reliable
    // cross-widget path.  Mirrors the macOS client's blanket
    // `NSApp.appearance = ...` in RISEApp.swift.
    QApplication app(argc, argv);
    app.setApplicationName("RISE");
    app.setOrganizationName("RISE");

    // Register the bundled IBM Plex faces before any widget is created —
    // Theme::sans/mono fall back to system fonts if this fails.
    Theme::registerFonts();

    // UI refinement item 3: read the persisted light/dark choice (needs
    // QSettings, hence must run AFTER setOrganizationName/setApplicationName
    // above) and reassign every Theme:: token global to that palette's
    // values, BEFORE applyPalette() and BEFORE MainWindow (or any other
    // widget) is constructed — so every widget picks up the right palette
    // + app font from the start rather than needing a repaint after the
    // fact.  Mirrors the macOS client's `ThemeState.loadPersisted()` call
    // in RISEApp.init.
    Theme::loadPersistedMode();
    Theme::applyPalette(app);

    // MainWindow's constructor sets its own minimum size (1320x760) —
    // see MainWindow.cpp.  UI refinement item 2: launch maximized
    // (mirrors the macOS client's `window.setFrame(screen.visibleFrame)`
    // in RISEApp.swift) instead of the previous fixed 1440x900 default.
    MainWindow window;
    window.showMaximized();

    return app.exec();
}
