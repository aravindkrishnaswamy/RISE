//
//  ThemeMode.swift
//  RISE-GUI
//
//  Runtime dark/light theme switching for the UI redesign (Theme.swift).
//  Dark is the primary/first-class theme; light is the mirror palette.
//

import AppKit

/// The two supported UI appearances.
enum ThemeMode: String {
    case dark
    case light
}

/// Global source of truth for the current theme mode.
///
/// Deliberately NOT an `ObservableObject` — `Theme.swift`'s token
/// accessors (and `RISESceneTheme` / `SceneEditorWindow`'s editor
/// palette) just read `ThemeState.mode` on every access, the same way
/// they'd read any other constant. SwiftUI re-render on a mode flip is
/// driven separately, by `ContentView` keying its root view off the
/// `"themeMode"` `@AppStorage`-backed `UserDefaults` key that
/// `setMode(_:)` below also writes — see ContentView.swift.
///
/// Mechanism summary for callers wiring the theme toggle (e.g. a
/// File-menu item):
///   - At app startup (once, before any themed view renders), call
///     `ThemeState.loadPersisted()`.
///   - On toggle, call `ThemeState.setMode(_:)`. It updates the
///     in-memory mode, persists it, updates `NSApplication`'s
///     appearance, AND writes the same `"themeMode"` UserDefaults key
///     that ContentView's `@AppStorage("themeMode")` watches — so the
///     root `.id()` rebuild fires automatically. No separate
///     notification is needed.
enum ThemeState {
    /// UserDefaults key. Shared with ContentView's
    /// `@AppStorage("themeMode")` — keep the string literal identical
    /// in both places (AppStorage doesn't let us reference this
    /// constant directly from a property-wrapper argument).
    static let userDefaultsKey = "themeMode"

    /// Current mode. Defaults to `.dark` (the primary theme) until
    /// `loadPersisted()` runs.
    private(set) static var mode: ThemeMode = .dark

    /// Load the persisted mode from UserDefaults. Call once at app
    /// startup (from RISEApp's init), before any themed view renders.
    static func loadPersisted() {
        if let raw = UserDefaults.standard.string(forKey: userDefaultsKey),
           let saved = ThemeMode(rawValue: raw) {
            mode = saved
        }
    }

    /// Switch the active theme mode: updates the in-memory mode,
    /// persists it to UserDefaults (the same key ContentView's
    /// `@AppStorage` observes, so the root `.id()` hook re-fires and
    /// every `Theme.*` token re-evaluates), and updates
    /// `NSApplication`'s appearance so system chrome (menus, window
    /// titlebar, NSAlert/NSSavePanel) follows.
    static func setMode(_ newMode: ThemeMode) {
        mode = newMode
        UserDefaults.standard.set(newMode.rawValue, forKey: userDefaultsKey)
        NSApplication.shared.appearance = NSAppearance(named: newMode == .dark ? .darkAqua : .aqua)
    }
}
