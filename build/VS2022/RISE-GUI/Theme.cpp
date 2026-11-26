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
#include <QEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSettings>
#include <QSvgRenderer>
#include <QVariant>

#include <QHash>

#include <utility>

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
	// Hinting: Qt's own HintingPreference doc table (qfont.cpp) states that
	// on "Windows and DirectWrite enabled in Qt", PreferNoHinting and
	// PreferVerticalHinting both resolve to the SAME thing -- "Vertical
	// hinting" -- while PreferDefaultHinting/PreferFullHinting resolve to
	// "Full hinting" (the current spindly-at-9-13px look). Cocoa on macOS
	// always renders unhinted regardless of preference. So there is no
	// DirectWrite behavioral difference between PreferNoHinting and
	// PreferVerticalHinting -- both give the closest attainable match to
	// the Mac client's unhinted look while keeping glyphs snapped to the
	// pixel grid vertically (no baseline swim). Use PreferNoHinting so the
	// *intent* (Mac-parity, least hinting) matches across platforms even
	// though DirectWrite quietly downgrades it to vertical-only.
	if (!abbrevStyleFamily.isEmpty() && QFontDatabase::hasFamily(abbrevStyleFamily)) {
		QFont f(abbrevStyleFamily);
		f.setPixelSize(pixelSize);
		f.setHintingPreference(QFont::PreferNoHinting);
		return f;
	}
	QFont f(family);
	f.setPixelSize(pixelSize);
	f.setWeight(weight);
	f.setHintingPreference(QFont::PreferNoHinting);
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
		f.setHintingPreference(QFont::PreferNoHinting);
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
		f.setHintingPreference(QFont::PreferNoHinting);
		return f;
	}
	return makeFont(QStringLiteral("IBM Plex Mono"),
	                abbrevFor(QStringLiteral("IBM Plex Mono"), weight),
	                pixelSize, weight);
}

// =====================================================================
// Icons -- Lucide SVGs tinted to a Theme:: token color
// =====================================================================
//
// Mirrors the macOS client's SF Symbols usage (Image(systemName:) +
// .foregroundColor(...)): a single vector source per glyph, tinted at
// draw time rather than shipping pre-colored raster art per theme.

QPixmap iconPixmap(const QString& name, int sizePx, const QColor& color, qreal dpr)
{
	// Process-lifetime cache. Keyed on every input that changes the
	// rendered result, INCLUDING color -- so a light/dark mode switch
	// (which reassigns the Theme:: token globals and is passed straight
	// through by call sites) never needs explicit invalidation here: the
	// old tint and the new tint are simply two different cache entries,
	// and the stale one is never looked up again once callers start
	// passing the new token value.
	static QHash<QString, QPixmap> cache;

	const QString key = QStringLiteral("%1@%2#%3@%4")
		.arg(name)
		.arg(sizePx)
		.arg(color.rgba(), 0, 16)
		.arg(qRound(dpr * 100));

	auto found = cache.constFind(key);
	if (found != cache.constEnd()) {
		return found.value();
	}

	const QString resPath = QStringLiteral(":/icons/%1.svg").arg(name);
	QSvgRenderer renderer(resPath);
	if (!renderer.isValid()) {
		// Resource missing -- return a null QPixmap so callers can keep
		// their existing text/glyph fallback instead of drawing nothing.
		return QPixmap();
	}

	const int physSize = qMax(1, qRound(sizePx * dpr));
	QPixmap pm(physSize, physSize);
	pm.fill(Qt::transparent);

	QPainter painter(&pm);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	renderer.render(&painter, QRectF(0, 0, physSize, physSize));

	// Flat-tint the rasterized glyph: SourceIn keeps the destination's
	// (glyph's) alpha channel but replaces its color with `color`,
	// exactly like SF Symbols' template-image tinting.
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(pm.rect(), color);
	painter.end();

	pm.setDevicePixelRatio(dpr);
	cache.insert(key, pm);
	return pm;
}

QIcon icon(const QString& name, int sizePx, const QColor& color)
{
	QIcon result;
	static const qreal kDprs[] = { 1.0, 1.25, 1.5, 2.0 };
	for (qreal dpr : kDprs) {
		QPixmap pm = iconPixmap(name, sizePx, color, dpr);
		if (!pm.isNull()) {
			result.addPixmap(pm);
		}
	}
	return result;
}

QIcon icon(const QString& name, int sizePx, const QColor& color, const QColor& disabledColor)
{
	QIcon result = icon(name, sizePx, color);
	static const qreal kDprs[] = { 1.0, 1.25, 1.5, 2.0 };
	for (qreal dpr : kDprs) {
		QPixmap pm = iconPixmap(name, sizePx, disabledColor, dpr);
		if (!pm.isNull()) {
			result.addPixmap(pm, QIcon::Disabled);
		}
	}
	return result;
}

QIcon icon(const QString& name, int sizePx, const QColor& color, const QColor& disabledColor, const QColor& activeColor)
{
	QIcon result = icon(name, sizePx, color, disabledColor);
	static const qreal kDprs[] = { 1.0, 1.25, 1.5, 2.0 };
	for (qreal dpr : kDprs) {
		QPixmap pm = iconPixmap(name, sizePx, activeColor, dpr);
		if (!pm.isNull()) {
			result.addPixmap(pm, QIcon::Active);
		}
	}
	return result;
}

// ---------------------------------------------------------------------
// bindIconLabel -- DPR-safe QLabel icon binding
// ---------------------------------------------------------------------
//
// See Theme.h's doc. Deliberately NOT a Q_OBJECT class (no moc step for
// this .cpp) -- eventFilter() is already virtual on plain QObject, and
// re-bind lookup goes through a QVariant<void*> dynamic property on the
// label instead of qobject_cast/findChild, both of which need Q_OBJECT
// to identify the derived type correctly.
namespace {

class IconLabelBinding : public QObject
{
public:
	explicit IconLabelBinding(QLabel* label)
		: QObject(label), m_label(label)
	{
		label->installEventFilter(this);
	}

	void rebind(QString name, int sizePx, std::function<QColor()> colorProvider)
	{
		m_name = std::move(name);
		m_sizePx = sizePx;
		m_colorProvider = std::move(colorProvider);
		repaint();
	}

protected:
	bool eventFilter(QObject* watched, QEvent* event) override
	{
		// DevicePixelRatioChange: the label crossed a monitor DPR
		// boundary, re-render at the new physical size.
		// PaletteChange: the live theme switched -- see Theme.h's LIVE
		// THEME-SWITCH CONTRACT for why this (not ApplicationPalette-
		// Change) is the event that actually reaches a nested QLabel.
		// Re-invoking m_colorProvider() here (rather than reusing a
		// value captured at bind time) is what makes the re-render
		// pick up the NEW token value instead of repainting the old
		// tint at the same size.
		if (watched == m_label &&
		    (event->type() == QEvent::DevicePixelRatioChange ||
		     event->type() == QEvent::PaletteChange)) {
			repaint();
		}
		return QObject::eventFilter(watched, event);
	}

private:
	void repaint()
	{
		if (!m_colorProvider) return;
		m_label->setPixmap(iconPixmap(m_name, m_sizePx, m_colorProvider(), m_label->devicePixelRatioF()));
	}

	QLabel* m_label;
	QString m_name;
	int m_sizePx = 0;
	std::function<QColor()> m_colorProvider;
};

// Shared property key: also doubles as the "does this label already have
// a binding" check, so a second bindIconLabel() call re-binds the
// existing filter instead of installing a duplicate one.
const char* kBindingPropertyKey = "themeIconLabelBinding";

} // namespace

void bindIconLabel(QLabel* label, const QString& name, int sizePx, std::function<QColor()> colorProvider)
{
	if (!label) return;

	const QVariant existing = label->property(kBindingPropertyKey);
	auto* binding = existing.isValid()
		? static_cast<IconLabelBinding*>(existing.value<void*>())
		: nullptr;
	if (!binding) {
		binding = new IconLabelBinding(label);
		label->setProperty(kBindingPropertyKey, QVariant::fromValue<void*>(binding));
	}
	binding->rebind(name, sizePx, std::move(colorProvider));
}

void bindIconLabel(QLabel* label, const QString& name, int sizePx, const QColor& color)
{
	// Fixed-color convenience wrapper -- see Theme.h's doc for when this
	// is (and isn't) the right overload to reach for.
	bindIconLabel(label, name, sizePx, [color]() { return color; });
}

QString scrollBarStyleSheet()
{
	// Slim, panel-matched scrollbars: transparent track, rounded handle
	// tinted from the CURRENT token values. Handle uses borderStrong (not
	// fillActive) -- it's a hairline stronger in both palettes (dark
	// whiteAlpha 36 vs 31; light blackAlpha 56 vs 26) and light mode's
	// bgWell is pure white, where fillActive's blackAlpha(26) reads as
	// nearly invisible for a control users need to grab. Hover promotes
	// to borderHover (dark whiteAlpha 64; light blackAlpha 89), matching
	// the hover step every other bordered control in this app already
	// uses. See Theme.h's doc for the intended application site
	// (`scrollBar->setStyleSheet(...)` on the QScrollBar itself, not the
	// scroll area) and the re-apply-from-restyleTheme() requirement.
	// Sibling helper: ChatPanel.cpp's transcriptScrollBarStyleSheet()
	// serves the chat transcript, which sits on bgPanel where the softer
	// fillActive resting tint reads fine.  Same geometry contract -- if
	// you change widths/radii/zero-size buttons here, change it there too.
	return QStringLiteral(
		"QScrollBar:vertical {"
		"  background: transparent;"
		"  width: 8px;"
		"  margin: 0px;"
		"}"
		"QScrollBar::handle:vertical {"
		"  background: %1;"
		"  border-radius: 4px;"
		"  min-height: 24px;"
		"}"
		"QScrollBar::handle:vertical:hover {"
		"  background: %2;"
		"}"
		"QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
		"  height: 0px;"
		"  width: 0px;"
		"  background: transparent;"
		"  border: none;"
		"}"
		"QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
		"  background: transparent;"
		"}"
		"QScrollBar:horizontal {"
		"  background: transparent;"
		"  height: 8px;"
		"  margin: 0px;"
		"}"
		"QScrollBar::handle:horizontal {"
		"  background: %1;"
		"  border-radius: 4px;"
		"  min-width: 24px;"
		"}"
		"QScrollBar::handle:horizontal:hover {"
		"  background: %2;"
		"}"
		"QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
		"  height: 0px;"
		"  width: 0px;"
		"  background: transparent;"
		"  border: none;"
		"}"
		"QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
		"  background: transparent;"
		"}"
	).arg(rgba(borderStrong), rgba(borderHover));
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
// UI refinement item 3 — light/dark mode (Phase 2: live switching)
// =====================================================================
//
// FORMER KNOWN GAP, NOW FIXED BY CONTRACT (see Theme.h's LIVE THEME-
// SWITCH CONTRACT block, which this paragraph used to describe as an
// open problem): a large fraction of this app's widgets bake
// `Theme::hex(...)` / `Theme::rgba(...)` token values into a
// `setStyleSheet()` string AT CONSTRUCTION TIME, not in a repaint that
// re-reads the tokens live. Reassigning the token globals below (and
// re-applying the QPalette) changes what *future* setStyleSheet() calls
// and QPalette-driven native chrome look like immediately, but an
// already-built widget's baked stylesheet string only refreshes if that
// widget re-runs its OWN styling code -- which is exactly what the
// restyleTheme() contract in Theme.h requires every such widget to do,
// hooked off QEvent::PaletteChange. MainWindow.cpp's restyleTheme() is
// the reference implementation; every panel with token-dependent styling
// (ViewportProperties, ChatPanel, ProposalCard, TopBar, LogWidget,
// ViewportToolbar, OutlinerWidget, ViewportTimeline, SceneEditor,
// StartWidget, EnvironmentPanel, ...) now has its own
// restyleTheme() citing the same contract.  RenderWidget is deliberately
// exempt -- its only styled colors are the mode-invariant
// blackAlpha/whiteAlpha scrim primitives, see RenderWidget.cpp's header
// comment.  (Stale-comment fix, 2026-07-23 review: this block used to
// describe the per-panel conversions as pending follow-up work; they
// have since landed.)  Widgets that paint via
// QPainter reading tokens live each frame (e.g. TopBar's
// TopBarLogoSwatch/TopBarProgressStrip, ViewportWidget) already picked
// up a mode switch immediately with no extra work, before and after
// this change -- see the contract's point 3.

namespace {

struct Palette {
	QColor bgBase, bgTopBar, bgPanel, bgCenter, bgWell, bgCard, bgCardDeep,
	       bgPopup, bgHeader, bgTimeline, bgBubbleUser;
	QColor textPrimary, textSecondary, textTertiary, textMuted, textFaint,
	       textDim, textDisabled, textGhost;
	QColor accent, accentLight, accentSoft, success, successLight, warn,
	       dirty, error, errorStrong, purple, gold, teal, iconOnAccent,
	       textOnAccent;
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
		// Same value in both palettes -- matches Mac's hardcoded `.white`
		// (see Theme.h's doc on this token).
		q.iconOnAccent = QColor(0xff, 0xff, 0xff);
		// Near-black on Dark's bright accent; the LIGHT palette flips
		// this to white (see LightPalette below and Theme.h's doc).
		// Mirrors macOS Theme.swift's DarkPalette.textOnAccent.
		q.textOnAccent = QColor(0x0d, 0x11, 0x16);

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
		// Same value in both palettes -- matches Mac's hardcoded `.white`
		// (see Theme.h's doc on this token).
		q.iconOnAccent = QColor(0xff, 0xff, 0xff);
		// P2 fix (2026-07-23 review): white, NOT a straight mirror of
		// Dark's near-black 0x0d1116 -- the light accent (0x1a6fa8 above)
		// is a darkened, saturated blue chosen for ~4.5:1 contrast against
		// LIGHT backgrounds, but that leaves it too DARK itself to host
		// near-black text on top: white-on-0x1a6fa8 is ~4.6:1 (passes
		// WCAG AA for normal text), while 0x0d1116-on-0x1a6fa8 was only
		// ~3.5:1 (fails AA text, borderline even for AA-large).  The Mac
		// client adopted the same per-mode split on 2026-07-24
		// (App/Theme.swift's LightPalette.textOnAccent = white) -- see
		// Theme.h's doc on this token for the callers (ViewportProperties'
		// Save button, ProposalCard's Apply button) this keeps legible.
		q.textOnAccent = QColor(0xff, 0xff, 0xff);

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

// LIVE THEME-SWITCH CONTRACT point 4 (Theme.h): incremented exactly once
// per applyThemeTokens() call below -- the single choke point every real
// token-reassignment path (loadPersistedMode, setMode, the system-theme
// watcher) funnels through. Deliberately NOT touched by applyPalette()
// itself, which is called both paired with applyThemeTokens() (setMode,
// the watcher) and standalone (main.cpp's initial post-loadPersistedMode
// call) -- incrementing there too would double-count the standalone case
// relative to nothing having actually changed.
int s_paletteEpoch = 0;

/// Reassigns every mode-dependent token global from `p`, then
/// recomputes the tokens that forward to another token on macOS
/// (catRender = warn, etc.) so they can never go stale relative to
/// whichever base token they mirror.
void applyThemeTokens(ThemeMode m)
{
	++s_paletteEpoch;

	// Defensive: callers are expected to pass an already-resolved mode
	// (Theme::effectiveMode() is always Dark or Light, never System --
	// see Theme.h) -- but if System ever reaches here directly, fall
	// back to Dark, RISE's primary/first-class theme, rather than
	// silently picking Light.
	const Palette& p = (m == ThemeMode::Light) ? LightPalette() : DarkPalette();

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
	iconOnAccent = p.iconOnAccent;
	textOnAccent = p.textOnAccent;

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

// s_mode: what the user picked (Dark / Light / System) -- persisted.
// s_effectiveMode: what's actually applied to the token globals right
// now -- always Dark or Light, never System.  See Theme::mode() /
// Theme::effectiveMode()'s docs in Theme.h.
ThemeMode s_mode = ThemeMode::Dark;
ThemeMode s_effectiveMode = ThemeMode::Dark;

// Shared with MainWindow's File > Theme checkmarks: keep this string
// literal identical everywhere it appears.
const char* kSettingsKey = "themeMode";

/// Qt::ColorScheme -> ThemeMode, for ThemeMode::System resolution.
/// Dark AND Unknown (no OS-level signal available on this platform)
/// both resolve to Dark -- RISE's primary/first-class theme -- per the
/// "System mode" section of Theme.h's LIVE THEME-SWITCH CONTRACT.
ThemeMode resolveSystemColorScheme()
{
	const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
	return (scheme == Qt::ColorScheme::Light) ? ThemeMode::Light : ThemeMode::Dark;
}

/// Persisted mode -> concrete Dark/Light mode actually applied.
ThemeMode resolveEffectiveMode(ThemeMode persisted)
{
	return (persisted == ThemeMode::System) ? resolveSystemColorScheme() : persisted;
}

QString modeToSettingsValue(ThemeMode m)
{
	switch (m) {
	case ThemeMode::Light:  return QStringLiteral("light");
	case ThemeMode::System: return QStringLiteral("system");
	case ThemeMode::Dark:
	default:                return QStringLiteral("dark");
	}
}

ThemeMode modeFromSettingsValue(const QString& raw)
{
	if (raw == QStringLiteral("light"))  return ThemeMode::Light;
	if (raw == QStringLiteral("system")) return ThemeMode::System;
	return ThemeMode::Dark;
}

} // namespace

ThemeMode mode()
{
	return s_mode;
}

ThemeMode effectiveMode()
{
	return s_effectiveMode;
}

int paletteEpoch()
{
	return s_paletteEpoch;
}

void loadPersistedMode()
{
	QSettings settings;
	const QString raw = settings.value(QString::fromLatin1(kSettingsKey)).toString();
	s_mode = modeFromSettingsValue(raw);
	s_effectiveMode = resolveEffectiveMode(s_mode);
	applyThemeTokens(s_effectiveMode);
}

void setMode(ThemeMode newMode, QApplication& app)
{
	if (newMode == s_mode) return;
	s_mode = newMode;
	QSettings settings;
	settings.setValue(QString::fromLatin1(kSettingsKey), modeToSettingsValue(newMode));
	s_effectiveMode = resolveEffectiveMode(s_mode);
	applyThemeTokens(s_effectiveMode);
	applyPalette(app);
}

void installSystemThemeWatcher(QApplication& app)
{
	// One-time connection for the process lifetime; main.cpp calls this
	// exactly once. `&app` as the context object ties the connection's
	// lifetime to the QApplication itself, so it's never left dangling.
	QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
	                  &app, [&app](Qt::ColorScheme) {
		if (s_mode != ThemeMode::System) return;
		const ThemeMode resolved = resolveSystemColorScheme();
		if (resolved == s_effectiveMode) return;  // no real change, skip the repaint churn
		s_effectiveMode = resolved;
		applyThemeTokens(s_effectiveMode);
		applyPalette(app);
	});
}

} // namespace Theme
