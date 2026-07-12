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

inline const QColor bgBase{ 0x13, 0x14, 0x16 };        // window / deepest background
inline const QColor bgTopBar{ 0x18, 0x19, 0x1c };      // top bar
inline const QColor bgPanel{ 0x17, 0x18, 0x1b };       // side panels
inline const QColor bgCenter{ 0x0e, 0x0f, 0x11 };      // viewport column (darkest)
inline const QColor bgWell{ 0x10, 0x11, 0x14 };        // input wells / value fields / log body
inline const QColor bgCard{ 0x13, 0x14, 0x17 };        // cards inside panels
inline const QColor bgCardDeep{ 0x13, 0x15, 0x18 };    // card footers
inline const QColor bgPopup{ 0x1c, 0x1e, 0x22 };       // menus, popovers, toasts
inline const QColor bgHeader{ 0x14, 0x15, 0x18 };      // section header strips
inline const QColor bgTimeline{ 0x11, 0x12, 0x14 };    // timeline strip
inline const QColor bgBubbleUser{ 0x24, 0x26, 0x2b };  // user chat bubble

// -------------------------------------------------------------------- Text

inline const QColor textPrimary{ 0xe6, 0xe7, 0xe9 };
inline const QColor textSecondary{ 0xc9, 0xcb, 0xd1 };
inline const QColor textTertiary{ 0xb8, 0xba, 0xc0 };
inline const QColor textMuted{ 0x9a, 0x9d, 0xa4 };
inline const QColor textFaint{ 0x8b, 0x8e, 0x94 };
inline const QColor textDim{ 0x6f, 0x72, 0x78 };
inline const QColor textDisabled{ 0x5c, 0x5f, 0x66 };
inline const QColor textGhost{ 0x49, 0x4c, 0x52 };

// ----------------------------------------------------------------- Accents

inline const QColor accent{ 0x6d, 0xb8, 0xe8 };        // selection / agent / links
inline const QColor accentLight{ 0x9e, 0xcb, 0xe8 };   // text on dark accent chips
inline const QColor accentSoft{ 0x8f, 0xb8, 0xe8 };    // diff headers / gutters
inline const QColor success{ 0x7f, 0xb9, 0x8a };       // parse OK / additions
inline const QColor successLight{ 0xa9, 0xd4, 0xb1 };  // diff "+" text
inline const QColor warn{ 0xe0, 0xb2, 0x5a };          // warnings / region badge
inline const QColor dirty{ 0xe8, 0xa3, 0x3d };         // dirty-dot amber
inline const QColor error{ 0xe0, 0x9a, 0x9a };         // errors / deletions (soft red)
inline const QColor errorStrong{ 0xe0, 0x5a, 0x5a };   // diff-deletion bg (use ~10% alpha)
inline const QColor purple{ 0xc8, 0xa0, 0xe8 };        // agent / reference / material
inline const QColor gold{ 0xd4, 0xb9, 0x8a };          // values / units
inline const QColor teal{ 0x8f, 0xd4, 0xc4 };          // animation category

// ------------------------------------------------- Borders / fills (alpha)

inline QColor whiteAlpha(int alphaPercent255)
{
	return QColor(0xff, 0xff, 0xff, alphaPercent255);
}

inline const QColor borderHairline = whiteAlpha(18);   // ~0.07
inline const QColor borderLight = whiteAlpha(23);      // ~0.09
inline const QColor borderMedium = whiteAlpha(31);     // ~0.12
inline const QColor borderStrong = whiteAlpha(36);     // ~0.14
inline const QColor borderHover = whiteAlpha(64);      // ~0.25
inline const QColor fillHover = whiteAlpha(20);        // ~0.08
inline const QColor fillActive = whiteAlpha(31);       // ~0.12
inline const QColor fillTrough = whiteAlpha(26);       // ~0.10

// --------------------------------------- Spectral identity gradient stops
// The 380–780 nm motif: violet → blue → teal → chartreuse → orange.

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

inline const QColor catRender = warn;                    // RND
inline const QColor catCamera = accentSoft;              // CAM
inline const QColor catLight = gold;                     // LGT
inline const QColor catObject = accentLight;             // OBJ
inline const QColor catMaterial{ 0xc9, 0xa0, 0xd4 };     // MAT
inline const QColor catAnimation = teal;                 // ANM
inline const QColor catMedia = purple;                   // MED
// Output Settings (Film) tag — muted slate blue, distinct from every
// other category accent.  Mirrors Theme.swift's catFilm.
inline const QColor catFilm{ 0x8f, 0xa8, 0xc9 };         // FLM
// scene_variant overlay tag — soft violet, distinct from catMaterial's
// and catMedia's purples.  Mirrors Theme.swift's catVariant.
inline const QColor catVariant{ 0xcf, 0x9f, 0xd6 };      // VAR

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
// QPalette built from the tokens above is the reliable cross-widget
// dark path — apply BEFORE constructing any widget (main.cpp calls this
// before `MainWindow window;`).  Mirrors the macOS client's blanket
// `NSApp.appearance = NSAppearance(named: .darkAqua)` in RISEApp.swift.
void applyDarkPalette(QApplication& app);

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
