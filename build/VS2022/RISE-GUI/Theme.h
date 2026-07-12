//
//  Theme.h
//  RISE-GUI (Windows / Qt6)
//
//  Design tokens for the RISE UI redesign (docs/gui/DESIGN_BRIEF.md).
//  Single source of truth for color, type, spacing and radii on Windows.
//  The macOS client mirrors these values in
//  build/XCode/rise/RISE-GUI/App/Theme.swift; the two files are kept in
//  sync by convention — change both together.
//
//  Palette lifted from the approved "RISE Prototype" design comp
//  (claude.ai/design project "RISE UI Full Screen Layout").
//
//  UI refinement item 3 (light/dark theme switch): dark is the primary /
//  first-class theme; light is the secondary, mirrored theme, added for
//  runtime switching from File > Theme (see MainWindow::createMenuBar).
//  Every token below used to be `inline const QColor` (one static value,
//  fixed at process start).  To support a runtime switch WITHOUT breaking
//  every call site (`Theme::bgPanel` is used as a VALUE, with no parens,
//  all over this codebase — turning the tokens into functions would touch
//  every one of those sites), the tokens are now plain `inline QColor`
//  globals, reassigned in place by `applyThemeTokens()` whenever the mode
//  changes.  Reads are GUI-thread-only, same as every other Qt widget
//  call in this app — there is no cross-thread synchronization here and
//  none is needed.  C++17 inline variables give a single definition
//  across translation units with no ODR risk; the tokens most other
//  tokens are DERIVED from (catRender = warn, etc.) are re-assigned
//  explicitly by applyThemeTokens() every time rather than relying on
//  static-initialization order, so there is no initialization-order
//  fragility either.
//

#pragma once

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QString>

class QApplication;

namespace Theme {

// ---------------------------------------------------------------- Surfaces
// Dark-first, neutral grays — a color-accurate surround for the render.
// Light-mode values mirror App/Theme.swift's `LightPalette` exactly
// (including the deliberate `bgCenter` mid-gray — see applyThemeTokens's
// doc for the rationale, ported verbatim from the Swift comment).

inline QColor bgBase{ 0x13, 0x14, 0x16 };        // window / deepest background
inline QColor bgTopBar{ 0x18, 0x19, 0x1c };      // top bar
inline QColor bgPanel{ 0x17, 0x18, 0x1b };       // side panels
inline QColor bgCenter{ 0x0e, 0x0f, 0x11 };      // viewport column (darkest)
inline QColor bgWell{ 0x10, 0x11, 0x14 };        // input wells / value fields / log body
inline QColor bgCard{ 0x13, 0x14, 0x17 };        // cards inside panels
inline QColor bgCardDeep{ 0x13, 0x15, 0x18 };    // card footers
inline QColor bgPopup{ 0x1c, 0x1e, 0x22 };       // menus, popovers, toasts
inline QColor bgHeader{ 0x14, 0x15, 0x18 };      // section header strips
inline QColor bgTimeline{ 0x11, 0x12, 0x14 };    // timeline strip
inline QColor bgBubbleUser{ 0x24, 0x26, 0x2b };  // user chat bubble

// -------------------------------------------------------------------- Text

inline QColor textPrimary{ 0xe6, 0xe7, 0xe9 };
inline QColor textSecondary{ 0xc9, 0xcb, 0xd1 };
inline QColor textTertiary{ 0xb8, 0xba, 0xc0 };
inline QColor textMuted{ 0x9a, 0x9d, 0xa4 };
inline QColor textFaint{ 0x8b, 0x8e, 0x94 };
inline QColor textDim{ 0x6f, 0x72, 0x78 };
inline QColor textDisabled{ 0x5c, 0x5f, 0x66 };
inline QColor textGhost{ 0x49, 0x4c, 0x52 };

// ----------------------------------------------------------------- Accents

inline QColor accent{ 0x6d, 0xb8, 0xe8 };        // selection / agent / links
inline QColor accentLight{ 0x9e, 0xcb, 0xe8 };   // text on dark accent chips
inline QColor accentSoft{ 0x8f, 0xb8, 0xe8 };    // diff headers / gutters
inline QColor success{ 0x7f, 0xb9, 0x8a };       // parse OK / additions
inline QColor successLight{ 0xa9, 0xd4, 0xb1 };  // diff "+" text
inline QColor warn{ 0xe0, 0xb2, 0x5a };          // warnings / region badge
inline QColor dirty{ 0xe8, 0xa3, 0x3d };         // dirty-dot amber
inline QColor error{ 0xe0, 0x9a, 0x9a };         // errors / deletions (soft red)
inline QColor errorStrong{ 0xe0, 0x5a, 0x5a };   // diff-deletion bg (use ~10% alpha)
inline QColor purple{ 0xc8, 0xa0, 0xe8 };        // agent / reference / material
inline QColor gold{ 0xd4, 0xb9, 0x8a };          // values / units
inline QColor teal{ 0x8f, 0xd4, 0xc4 };          // animation category

// ------------------------------------------------- Borders / fills (alpha)
// whiteAlpha/blackAlpha are pure functions of an int, not tokens -- they
// stay unaffected by the mode switch (some scattered setStyleSheet call
// sites hardcode `whiteAlpha(...)` directly for effects that are meant to
// stay a white overlay regardless of theme; see the mode-switch honest-gap
// note in Theme.cpp for the inventory of call sites that do NOT yet
// respect light mode).

inline QColor whiteAlpha(int alphaPercent255)
{
	return QColor(0xff, 0xff, 0xff, alphaPercent255);
}

inline QColor blackAlpha(int alphaPercent255)
{
	return QColor(0x00, 0x00, 0x00, alphaPercent255);
}

inline QColor borderHairline = whiteAlpha(18);   // ~0.07
inline QColor borderLight = whiteAlpha(23);      // ~0.09
inline QColor borderMedium = whiteAlpha(31);     // ~0.12
inline QColor borderStrong = whiteAlpha(36);     // ~0.14
inline QColor borderHover = whiteAlpha(64);      // ~0.25
inline QColor fillHover = whiteAlpha(20);        // ~0.08
inline QColor fillActive = whiteAlpha(31);       // ~0.12
inline QColor fillTrough = whiteAlpha(26);       // ~0.10

// --------------------------------------- Spectral identity gradient stops
// The 380–780 nm motif: violet → blue → teal → chartreuse → orange.
// Theme-invariant (brand/identity motif, not a surface or text color) --
// does NOT switch with mode.  Mirrors Theme.swift's `spectralStops`.

struct SpectralStop { qreal pos; QColor color; };

inline const SpectralStop spectralStops[] = {
	{ 0.00, QColor(0x5b, 0x21, 0xb6) },
	{ 0.30, QColor(0x25, 0x63, 0xeb) },
	{ 0.55, QColor(0x0d, 0x94, 0x88) },
	{ 0.75, QColor(0xa3, 0xb5, 0x15) },
	{ 1.00, QColor(0xe0, 0x5a, 0x00) },
};

inline void applySpectralStops(QLinearGradient& g)
{
	for (const SpectralStop& s : spectralStops) {
		g.setColorAt(s.pos, s.color);
	}
}

// --------------------------------------------- Category tag colors (outliner)
// catRender/catCamera/catLight/catObject/catAnimation/catMedia forward to
// another token on macOS (computed properties in Theme.swift); here they
// are plain globals kept in sync by applyThemeTokens() -- see that
// function's doc for why a one-time `= warn`-style initializer isn't
// enough once the tokens are mode-switchable.

inline QColor catRender{ 0xe0, 0xb2, 0x5a };             // RND (= warn)
inline QColor catCamera{ 0x8f, 0xb8, 0xe8 };             // CAM (= accentSoft)
inline QColor catLight{ 0xd4, 0xb9, 0x8a };              // LGT (= gold)
inline QColor catObject{ 0x9e, 0xcb, 0xe8 };             // OBJ (= accentLight)
inline QColor catMaterial{ 0xc9, 0xa0, 0xd4 };           // MAT
inline QColor catAnimation{ 0x8f, 0xd4, 0xc4 };          // ANM (= teal)
inline QColor catMedia{ 0xc8, 0xa0, 0xe8 };              // MED (= purple)
// Output Settings (Film) tag — muted slate blue, distinct from every
// other category accent.  Mirrors Theme.swift's catFilm.
inline QColor catFilm{ 0x8f, 0xa8, 0xc9 };               // FLM
// scene_variant overlay tag — soft violet, distinct from catMaterial's
// and catMedia's purples.  Mirrors Theme.swift's catVariant.
inline QColor catVariant{ 0xcf, 0x9f, 0xd6 };            // VAR

// ------------------------------------------------------------------- Radii

inline constexpr int radiusSmall = 5;
inline constexpr int radiusMedium = 7;
inline constexpr int radiusLarge = 9;
inline constexpr int radiusCard = 10;

// ------------------------------------------------------------------- Fonts
//
// The bundled IBM Plex release TTFs abbreviate face family names
// ("IBM Plex Sans Medm", "IBM Plex Sans SmBld"); DirectWrite groups them
// under the typographic family "IBM Plex Sans", which Qt6 exposes as one
// family with Medium/SemiBold styles.  registerFonts() must run once at
// startup (before any widget is created); sans()/mono() fall back to
// system faces if registration failed.

bool fontsAvailable();
void registerFonts();
QFont sans(int pixelSize, QFont::Weight weight = QFont::Normal);
QFont mono(int pixelSize, QFont::Weight weight = QFont::Normal);

// ------------------------------------------------------------ App style
//
// Qt's "windows11" style ignores QPalette in several places (menus,
// scrollbars), which fights a dark-only design.  Fusion + an explicit
// QPalette built from the CURRENT token values is the reliable
// cross-widget dark path — apply BEFORE constructing any widget
// (main.cpp calls this before `MainWindow window;`).  Mirrors the macOS
// client's blanket `NSApp.appearance = NSAppearance(named: .darkAqua)`
// in RISEApp.swift.
void applyPalette(QApplication& app);

// ------------------------------------------------------- Light/dark mode
//
// UI refinement item 3.  See MainWindow::createMenuBar's File > Theme
// submenu for the user-facing toggle and the "some panels update after
// restarting" caveat this mechanism is honest about (Theme.cpp's doc
// has the full inventory of setStyleSheet call sites that bake colors
// in at widget-construction time and therefore do NOT repaint live).

enum class ThemeMode { Dark, Light };

/// Current mode.  Defaults to Dark (the primary theme) until
/// loadPersistedMode() runs.
ThemeMode mode();

/// Load the persisted mode from QSettings ("themeMode") and apply its
/// token values (see applyThemeTokens in Theme.cpp).  Call once at
/// startup, before any widget is constructed and before applyPalette().
void loadPersistedMode();

/// Switch the active mode: reassigns every token global to the new
/// palette's values, persists the choice to QSettings ("themeMode"),
/// and re-applies the QPalette + app font to `app` so native chrome
/// (menus, dialogs, scrollbars) follows live.  Custom-styled widgets
/// that baked colors into a setStyleSheet() string at construction time
/// do NOT repaint automatically -- see MainWindow's File > Theme handler
/// for the one-time "restart recommended" notice.
void setMode(ThemeMode newMode, QApplication& app);

// css-style helpers for the scattered setStyleSheet call sites
inline QString rgba(const QColor& c)
{
	return QStringLiteral("rgba(%1,%2,%3,%4)")
		.arg(c.red()).arg(c.green()).arg(c.blue())
		.arg(QString::number(c.alphaF(), 'f', 2));
}

inline QString hex(const QColor& c)
{
	return c.name(QColor::HexRgb);
}

} // namespace Theme
