//////////////////////////////////////////////////////////////////////
//
//  main.cpp - Entry point for the RISE Qt GUI application
//
//////////////////////////////////////////////////////////////////////

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include "MainWindow.h"
#include "Theme.h"

int main(int argc, char* argv[])
{
    // Phase 3 crispness: Qt6's default HighDpiScaleFactorRoundingPolicy is
    // Round, which snaps fractional OS scale factors (125%, 150%, ...) to
    // the nearest whole factor. On the fractional scales Windows commonly
    // ships (125%/150% are the Settings > Display defaults on many laptop
    // panels), that rounding causes off-by-one hairline mismatches and
    // blurry upscaled pixmaps. PassThrough honors the OS scale exactly
    // instead. This must be set before the QApplication is constructed.
    // The icon pipeline (Theme::iconPixmap's DPR-keyed cache + the
    // DevicePixelRatioChange rebind in bindIconLabel) and the viewport's
    // devicePixelRatioF() handling cope with fractional DPR; the one
    // downstream fix this change required was HDRRenderWidget's swapchain
    // creation, whose int-cast DPR truncated 1.25/1.5 to 1 (fixed
    // alongside this, see HDRRenderWidget.cpp's swapchain-desc comment).
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

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

    // App icon for window decorations / Alt-Tab (the .exe/taskbar icon
    // is set separately via RISE-GUI.rc's ICON resource, wired into the
    // build via the <ResourceCompile Include="RISE-GUI.rc" /> entry in
    // RISE-GUI.vcxproj -- see VCXPROJ_NOTES.md; stale-comment fix,
    // 2026-07-23 review -- this used to describe the .rc as not yet
    // wired in). Mirrors the macOS client's bundled AppIcon.appiconset
    // artwork.
    app.setWindowIcon(QIcon(":/icons/app_logo_256.png"));

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

    // Phase 2 (live theme switching + "Follow system"): watch the OS
    // color-scheme signal so ThemeMode::System stays live across an OS
    // theme flip while RISE is running, not just at next launch. Must
    // run after loadPersistedMode()/applyPalette() (needs the QSettings
    // org/app name set above, and there's no reason to watch before the
    // initial palette is even applied) — see Theme.h's LIVE THEME-SWITCH
    // CONTRACT, "System mode" section, for the full mechanics.
    Theme::installSystemThemeWatcher(app);

    // MainWindow's constructor sets its own minimum size (1320x760) —
    // see MainWindow.cpp.  UI refinement item 2: launch maximized
    // (mirrors the macOS client's `window.setFrame(screen.visibleFrame)`
    // in RISEApp.swift) instead of the previous fixed 1440x900 default.
    MainWindow window;
    window.showMaximized();

    return app.exec();
}
