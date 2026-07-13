//
//  Theme.cpp
//  RISE-GUI (Windows / Qt6)
//
//  Font registration + lookup for the bundled IBM Plex faces, plus the
//  UI refinement item 3 light/dark mode mechanism.  See Theme.h for the
//  token declarations this pairs with.
//

#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>
#include <QSettings>

namespace Theme {

namespace {
bool s_fontsAvailable = false;
}

bool fontsAvailable()
{
	return s_fontsAvailable;
}

void registerFonts()
{
	static bool s_registered = false;
	if (s_registered) {
		return;
	}
	s_registered = true;

	const char* kFontResources[] = {
		":/fonts/IBMPlexSans-Regular.ttf",
		":/fonts/IBMPlexSans-Medium.ttf",
		":/fonts/IBMPlexSans-SemiBold.ttf",
		":/fonts/IBMPlexSans-Bold.ttf",
		":/fonts/IBMPlexMono-Regular.ttf",
		":/fonts/IBMPlexMono-Medium.ttf",
		":/fonts/IBMPlexMono-SemiBold.ttf",
	};

	bool anyLoaded = false;
	for (const char* res : kFontResources) {
		if (QFontDatabase::addApplicationFont(QString::fromLatin1(res)) >= 0) {
			anyLoaded = true;
		}
	}

	// Only claim availability if the base family actually resolved —
	// callers fall back to system faces otherwise.
	s_fontsAvailable = anyLoaded
		&& QFontDatabase::hasFamily(QStringLiteral("IBM Plex Sans"))
		&& QFontDatabase::hasFamily(QStringLiteral("IBM Plex Mono"));
}

namespace {

QFont makeFont(const QString& family, const QString& abbrevStyleFamily,
               int pixelSize, QFont::Weight weight)
{
	// DirectWrite usually unifies the Plex faces under the typographic
	// family with named styles; older enumerations expose the abbreviated
	// per-weight families ("IBM Plex Sans Medm" / "... SmBld") instead.
	// Prefer the unified family and let Qt match the weight; if the
	// abbreviated family exists as its own entry, use it directly so the
	// exact face is picked rather than a synthesized weight.
	if (!abbrevStyleFamily.isEmpty() && QFontDatabase::hasFamily(abbrevStyleFamily)) {
		QFont f(abbrevStyleFamily);
		f.setPixelSize(pixelSize);
		return f;
	}
	QFont f(family);
	f.setPixelSize(pixelSize);
	f.setWeight(weight);
	return f;
}

QString abbrevFor(const QString& base, QFont::Weight weight)
{
	switch (weight) {
	case QFont::Medium:   return base + QStringLiteral(" Medm");
	case QFont::DemiBold: return base + QStringLiteral(" SmBld");
	default:              return QString();
	}
}

} // namespace

QFont sans(int pixelSize, QFont::Weight weight)
{
	if (!s_fontsAvailable) {
		QFont f = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
		f.setPixelSize(pixelSize);
		f.setWeight(weight);
		return f;
	}
	return makeFont(QStringLiteral("IBM Plex Sans"),
	                abbrevFor(QStringLiteral("IBM Plex Sans"), weight),
	                pixelSize, weight);
}

QFont mono(int pixelSize, QFont::Weight weight)
{
	if (!s_fontsAvailable) {
		QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
		f.setPixelSize(pixelSize);
		f.setWeight(weight);
		return f;
	}
	return makeFont(QStringLiteral("IBM Plex Mono"),
	                abbrevFor(QStringLiteral("IBM Plex Mono"), weight),
	                pixelSize, weight);
}

void applyPalette(QApplication& app)
{
	if (QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
		QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	}

	QPalette pal;
	pal.setColor(QPalette::Window, bgBase);
	pal.setColor(QPalette::WindowText, textPrimary);
	pal.setColor(QPalette::Base, bgWell);
	pal.setColor(QPalette::AlternateBase, bgCard);
	pal.setColor(QPalette::ToolTipBase, bgPopup);
	pal.setColor(QPalette::ToolTipText, textPrimary);
	pal.setColor(QPalette::Text, textPrimary);
	pal.setColor(QPalette::Button, bgPanel);
	pal.setColor(QPalette::ButtonText, textPrimary);
	pal.setColor(QPalette::BrightText, textPrimary);
	pal.setColor(QPalette::Link, accent);
	pal.setColor(QPalette::LinkVisited, accentSoft);
	pal.setColor(QPalette::Highlight, accent);
	pal.setColor(QPalette::HighlightedText, QColor(0x0a, 0x0b, 0x0c));

	pal.setColor(QPalette::Disabled, QPalette::WindowText, textDisabled);
	pal.setColor(QPalette::Disabled, QPalette::Text, textDisabled);
	pal.setColor(QPalette::Disabled, QPalette::ButtonText, textDisabled);
	pal.setColor(QPalette::Disabled, QPalette::Highlight, borderMedium);
	pal.setColor(QPalette::Disabled, QPalette::HighlightedText, textDisabled);

	app.setPalette(pal);
	app.setFont(sans(13));
}

// =====================================================================
// UI refinement item 3 — light/dark mode
// =====================================================================
//
// KNOWN GAP (honest, not silently papered over): a large fraction of
// this app's widgets bake `Theme::hex(...)` / `Theme::rgba(...)` token
// values into a `setStyleSheet()` string AT CONSTRUCTION TIME, not in a
// repaint/paintEvent that re-reads the tokens live.  Reassigning the
// token globals below (and re-applying the QPalette) changes what
// *future* setStyleSheet() calls and QPalette-driven native chrome
// look like immediately, but every already-built widget whose
// stylesheet string was baked before the switch keeps its stale colors
// until that widget is reconstructed -- in practice, until the app is
// restarted.  Inventory of files with baked-in `setStyleSheet` colors
// (grep `setStyleSheet` across build/VS2022/RISE-GUI/*.cpp), roughly by
// call-site count, as of this writing:
//   ViewportProperties.cpp (26), ChatPanel.cpp (17), ProposalCard.cpp (17),
//   TopBar.cpp (16), LogWidget.cpp (12), ViewportToolbar.cpp (11),
//   OutlinerWidget.cpp (11), ViewportTimeline.cpp (8), SceneEditor.cpp (8),
//   MainWindow.cpp (5), RenderWidget.cpp (3).
// A few paint via QPainter reading tokens live each frame (e.g.
// TopBar's TopBarLogoSwatch/TopBarProgressStrip, ViewportWidget) and
// DO pick up a mode switch immediately with no extra work. A future
// repolish pass could thread a "theme changed" signal through every
// stylesheet-baking widget's own re-apply method; that is explicitly
// OUT OF SCOPE for this slice -- see MainWindow::createMenuBar's
// File > Theme handler for the one-time notice this honesty implies.

namespace {

struct Palette {
	QColor bgBase, bgTopBar, bgPanel, bgCenter, bgWell, bgCard, bgCardDeep,
	       bgPopup, bgHeader, bgTimeline, bgBubbleUser;
	QColor textPrimary, textSecondary, textTertiary, textMuted, textFaint,
	       textDim, textDisabled, textGhost;
	QColor accent, accentLight, accentSoft, success, successLight, warn,
	       dirty, error, errorStrong, purple, gold, teal;
	QColor catMaterial, catFilm, catVariant;
	QColor borderHairline, borderLight, borderMedium, borderStrong, borderHover;
	QColor fillHover, fillActive, fillTrough;
};

const Palette& DarkPalette()
{
	static const Palette p = [] {
		Palette q;
		q.bgBase       = QColor(0x13, 0x14, 0x16);
		q.bgTopBar     = QColor(0x18, 0x19, 0x1c);
		q.bgPanel      = QColor(0x17, 0x18, 0x1b);
		q.bgCenter     = QColor(0x0e, 0x0f, 0x11);
		q.bgWell       = QColor(0x10, 0x11, 0x14);
		q.bgCard       = QColor(0x13, 0x14, 0x17);
		q.bgCardDeep   = QColor(0x13, 0x15, 0x18);
		q.bgPopup      = QColor(0x1c, 0x1e, 0x22);
		q.bgHeader     = QColor(0x14, 0x15, 0x18);
		q.bgTimeline   = QColor(0x11, 0x12, 0x14);
		q.bgBubbleUser = QColor(0x24, 0x26, 0x2b);

		q.textPrimary   = QColor(0xe6, 0xe7, 0xe9);
		q.textSecondary = QColor(0xc9, 0xcb, 0xd1);
		q.textTertiary  = QColor(0xb8, 0xba, 0xc0);
		q.textMuted     = QColor(0x9a, 0x9d, 0xa4);
		q.textFaint     = QColor(0x8b, 0x8e, 0x94);
		q.textDim       = QColor(0x6f, 0x72, 0x78);
		q.textDisabled  = QColor(0x5c, 0x5f, 0x66);
		q.textGhost     = QColor(0x49, 0x4c, 0x52);

		q.accent       = QColor(0x6d, 0xb8, 0xe8);
		q.accentLight  = QColor(0x9e, 0xcb, 0xe8);
		q.accentSoft   = QColor(0x8f, 0xb8, 0xe8);
		q.success      = QColor(0x7f, 0xb9, 0x8a);
		q.successLight = QColor(0xa9, 0xd4, 0xb1);
		q.warn         = QColor(0xe0, 0xb2, 0x5a);
		q.dirty        = QColor(0xe8, 0xa3, 0x3d);
		q.error        = QColor(0xe0, 0x9a, 0x9a);
		q.errorStrong  = QColor(0xe0, 0x5a, 0x5a);
		q.purple       = QColor(0xc8, 0xa0, 0xe8);
		q.gold         = QColor(0xd4, 0xb9, 0x8a);
		q.teal         = QColor(0x8f, 0xd4, 0xc4);

		q.catMaterial = QColor(0xc9, 0xa0, 0xd4);
		q.catFilm     = QColor(0x8f, 0xa8, 0xc9);
		q.catVariant  = QColor(0xcf, 0x9f, 0xd6);

		// White-opacity borders/fills, matching Theme.swift's DarkPalette
		// `Color.white.opacity(...)`.
		q.borderHairline = whiteAlpha(18);   // ~0.07
		q.borderLight    = whiteAlpha(23);   // ~0.09
		q.borderMedium   = whiteAlpha(31);   // ~0.12
		q.borderStrong   = whiteAlpha(36);   // ~0.14
		q.borderHover    = whiteAlpha(64);   // ~0.25
		q.fillHover      = whiteAlpha(20);   // ~0.08
		q.fillActive     = whiteAlpha(31);   // ~0.12
		q.fillTrough     = whiteAlpha(26);   // ~0.10
		return q;
	}();
	return p;
}

// Light palette — mirrors App/Theme.swift's `LightPalette` exactly,
// including the deliberate `bgCenter` mid-gray: the center column is
// the image-forward viewport surround, and a bright-white surround
// would bias perceived image brightness/contrast during color-critical
// evaluation — same reason the dark palette uses its darkest surface
// there.  Accent hues are darkened from their dark-mode counterparts to
// clear ~4.5:1 contrast against light backgrounds while staying
// recognizably the same color family.  Border/fill tokens switch from
// white-opacity to black-opacity (white-opacity overlays are invisible
// on a light background).
const Palette& LightPalette()
{
	static const Palette p = [] {
		Palette q;
		q.bgBase       = QColor(0xec, 0xec, 0xee);
		q.bgTopBar     = QColor(0xf4, 0xf4, 0xf6);
		q.bgPanel      = QColor(0xf0, 0xf0, 0xf2);
		q.bgCenter     = QColor(0xb8, 0xb8, 0xbc);
		q.bgWell       = QColor(0xff, 0xff, 0xff);
		q.bgCard       = QColor(0xf7, 0xf7, 0xf9);
		q.bgCardDeep   = QColor(0xf2, 0xf3, 0xf5);
		q.bgPopup      = QColor(0xff, 0xff, 0xff);
		q.bgHeader     = QColor(0xeb, 0xeb, 0xee);
		q.bgTimeline   = QColor(0xf0, 0xf0, 0xf3);
		q.bgBubbleUser = QColor(0xdd, 0xe4, 0xee);

		q.textPrimary   = QColor(0x1a, 0x1b, 0x1e);
		q.textSecondary = QColor(0x33, 0x35, 0x3a);
		q.textTertiary  = QColor(0x4a, 0x4d, 0x54);
		q.textMuted     = QColor(0x5f, 0x63, 0x6b);
		q.textFaint     = QColor(0x76, 0x7a, 0x83);
		q.textDim       = QColor(0x8f, 0x93, 0x9c);
		q.textDisabled  = QColor(0xa6, 0xaa, 0xb2);
		q.textGhost     = QColor(0xc2, 0xc5, 0xcb);

		q.accent       = QColor(0x1a, 0x6f, 0xa8);
		q.accentLight  = QColor(0x25, 0x80, 0xbd);
		q.accentSoft   = QColor(0x3a, 0x77, 0xad);
		q.success      = QColor(0x2e, 0x7d, 0x43);
		q.successLight = QColor(0x3d, 0x8f, 0x52);
		q.warn         = QColor(0x9a, 0x6b, 0x10);
		q.dirty        = QColor(0xa8, 0x6e, 0x14);
		q.error        = QColor(0xb0, 0x3a, 0x3a);
		q.errorStrong  = QColor(0xc2, 0x30, 0x30);
		q.purple       = QColor(0x7b, 0x4f, 0xa6);
		q.gold         = QColor(0x8a, 0x6c, 0x2f);
		q.teal         = QColor(0x2b, 0x7d, 0x6e);

		q.catMaterial = QColor(0x8a, 0x5a, 0x9e);
		q.catFilm     = QColor(0x56, 0x70, 0x8f);
		q.catVariant  = QColor(0x93, 0x58, 0x9e);

		q.borderHairline = blackAlpha(26);   // ~0.10
		q.borderLight    = blackAlpha(33);   // ~0.13
		q.borderMedium   = blackAlpha(46);   // ~0.18
		q.borderStrong   = blackAlpha(56);   // ~0.22
		q.borderHover    = blackAlpha(89);   // ~0.35
		q.fillHover      = blackAlpha(15);   // ~0.06
		q.fillActive     = blackAlpha(26);   // ~0.10
		q.fillTrough     = blackAlpha(20);   // ~0.08
		return q;
	}();
	return p;
}

/// Reassigns every mode-dependent token global from `p`, then
/// recomputes the tokens that forward to another token on macOS
/// (catRender = warn, etc.) so they can never go stale relative to
/// whichever base token they mirror.
void applyThemeTokens(ThemeMode m)
{
	const Palette& p = (m == ThemeMode::Dark) ? DarkPalette() : LightPalette();

	bgBase = p.bgBase; bgTopBar = p.bgTopBar; bgPanel = p.bgPanel;
	bgCenter = p.bgCenter; bgWell = p.bgWell; bgCard = p.bgCard;
	bgCardDeep = p.bgCardDeep; bgPopup = p.bgPopup; bgHeader = p.bgHeader;
	bgTimeline = p.bgTimeline; bgBubbleUser = p.bgBubbleUser;

	textPrimary = p.textPrimary; textSecondary = p.textSecondary;
	textTertiary = p.textTertiary; textMuted = p.textMuted;
	textFaint = p.textFaint; textDim = p.textDim;
	textDisabled = p.textDisabled; textGhost = p.textGhost;

	accent = p.accent; accentLight = p.accentLight; accentSoft = p.accentSoft;
	success = p.success; successLight = p.successLight; warn = p.warn;
	dirty = p.dirty; error = p.error; errorStrong = p.errorStrong;
	purple = p.purple; gold = p.gold; teal = p.teal;

	catMaterial = p.catMaterial; catFilm = p.catFilm; catVariant = p.catVariant;

	borderHairline = p.borderHairline; borderLight = p.borderLight;
	borderMedium = p.borderMedium; borderStrong = p.borderStrong;
	borderHover = p.borderHover;
	fillHover = p.fillHover; fillActive = p.fillActive; fillTrough = p.fillTrough;

	// Derived category-tag tokens (computed properties on macOS) -- see
	// Theme.h's doc for why these are re-derived here instead of relying
	// on a one-time `= warn`-style initializer.
	catRender = warn;
	catCamera = accentSoft;
	catLight = gold;
	catObject = accentLight;
	catAnimation = teal;
	catMedia = purple;
}

ThemeMode s_mode = ThemeMode::Dark;

// Shared with MainWindow's File > Theme checkmarks: keep this string
// literal identical everywhere it appears.
const char* kSettingsKey = "themeMode";

} // namespace

ThemeMode mode()
{
	return s_mode;
}

void loadPersistedMode()
{
	QSettings settings;
	const QString raw = settings.value(QString::fromLatin1(kSettingsKey)).toString();
	s_mode = (raw == QStringLiteral("light")) ? ThemeMode::Light : ThemeMode::Dark;
	applyThemeTokens(s_mode);
}

void setMode(ThemeMode newMode, QApplication& app)
{
	if (newMode == s_mode) return;
	s_mode = newMode;
	QSettings settings;
	settings.setValue(QString::fromLatin1(kSettingsKey),
	                  newMode == ThemeMode::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
	applyThemeTokens(s_mode);
	applyPalette(app);
}

} // namespace Theme
