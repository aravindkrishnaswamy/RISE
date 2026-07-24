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
//  Phase 2 (live switching + "Follow system"): the switch is now LIVE --
//  no restart needed -- for every widget that follows the LIVE THEME-
//  SWITCH CONTRACT documented further down this file (search for that
//  heading). ThemeMode gained a third value, System, that tracks the OS
//  light/dark setting and re-resolves live if the OS setting changes
//  while RISE is running.
//

#pragma once

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QLinearGradient>
#include <QPixmap>
#include <QString>

#include <functional>

class QApplication;
class QLabel;

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
// Text drawn directly on a SOLID accent fill (e.g. ProposalCard's "Apply"
// button, ViewportProperties' "Save" button -- background-color:
// Theme::accent).  Mirrors the macOS client's Theme.swift `textOnAccent`
// token (added 2026-07-24; both platforms previously hardcoded
// 0x0d1116): near-black in Dark, WHITE in Light.  The per-mode split
// (P2 fix, 2026-07-23 review) exists because near-black reads fine
// against Dark's brighter accent (0x6db8e8, ~8.7:1) but goes ~3.5:1
// (fails WCAG AA text) against Light's deliberately darkened accent
// (0x1a6fa8 -- see LightPalette's own doc in Theme.cpp), which made
// ViewportProperties' Save button functionally invisible in Light mode.
// White-on-0x1a6fa8 is ~4.6:1, clearing AA.  Per-mode values set
// explicitly in both DarkPalette() and LightPalette(), like every
// other token below.
inline QColor textOnAccent{ 0x0d, 0x11, 0x16 };

// Fixed glyph/label tint for content painted on a solid-ish accent fill
// (e.g. the viewport toolbar's checked tool button, background
// Theme::fillActive). Mirrors App/Theme.swift's ViewportToolbar
// ToolButton, which hardcodes `.foregroundColor(isSelected ? .white :
// Theme.textTertiary)` -- Mac never varies the selected-state tint even
// though its own fillActive switches from white-opacity (dark) to
// black-opacity (light). Declared as a real per-mode token (not a bare
// `#ffffff` literal at each call site) so a future divergence from Mac
// is a one-line change here rather than a grep-and-replace; both
// palettes currently agree with Mac's hardcode.
inline QColor iconOnAccent{ 0xff, 0xff, 0xff };

// ------------------------------------------------- Borders / fills (alpha)
// whiteAlpha/blackAlpha are pure functions of an int, not tokens -- they
// stay unaffected by the mode switch.  The few call sites that hardcode
// `whiteAlpha(...)`/`blackAlpha(...)` directly are effects DELIBERATELY
// theme-invariant (e.g. RenderWidget's image scrim); every border/fill
// that should flip with the mode goes through the tokens below (the
// 2026-07-23 live-theming pass audited and converted the rest).

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

// ------------------------------------------------------------------- Icons
//
// Lucide SVGs (build/VS2022/RISE-GUI/icons/, ISC licensed — see
// icons/LICENSE-lucide.txt) bundled into the ":/icons/<name>.svg" resource
// prefix (resources.qrc).  These mirror the macOS client's SF Symbols
// usage: a small set of line-art glyphs, tinted per-call-site to whatever
// token color the caller needs (accent, textMuted, error, ...) rather than
// baked at author time.  `name` is the bare Lucide file name without the
// ".svg" extension or the "/icons" prefix, e.g. "move-3d", "circle-x".

/// Renders ":/icons/<name>.svg" at `sizePx` (logical) * `dpr` (physical
/// pixels), tinted flat with `color` (QPainter::CompositionMode_SourceIn
/// over the rasterized glyph alpha), with `setDevicePixelRatio(dpr)`
/// already applied so it drops straight into a QLabel/QPushButton at
/// `sizePx` logical size on a HiDPI display.  Returns a null QPixmap if
/// the resource doesn't exist -- callers can fall back to their existing
/// text/glyph rendering in that case.  Results are cached for the
/// process lifetime, keyed on (name, sizePx, color, dpr); the color is
/// part of the key, so switching Theme::mode() (which reassigns the
/// Theme:: token globals) needs no cache invalidation -- old and new
/// tints simply coexist as distinct cache entries.
QPixmap iconPixmap(const QString& name, int sizePx, const QColor& color, qreal dpr = 1.0);

/// Convenience wrapper around iconPixmap() that builds a QIcon with
/// pixmaps at the DPRs Windows commonly reports for 100/125/150/200%
/// scale factors, so QIcon::pixmap() picks a crisp source at whichever
/// scale the requesting widget's screen is running.
QIcon icon(const QString& name, int sizePx, const QColor& color);

/// Same as icon(), but also registers a second tint for QIcon::Disabled
/// (e.g. Theme::textDisabled) so disabled toolbar/menu actions dim
/// automatically instead of showing the enabled-state tint at reduced
/// opacity.
QIcon icon(const QString& name, int sizePx, const QColor& color, const QColor& disabledColor);

/// Same as icon(name, sizePx, color, disabledColor), but also registers a
/// third tint for QIcon::Active -- the mode QCommonStyle requests for an
/// auto-raise QToolButton under the mouse (QCommonStyle::CE_ToolButtonLabel
/// promotes State_MouseOver + State_AutoRaise to QIcon::Active). Restores
/// the hover-tint QSS rule (`:hover { color: ... }`) that stopped doing
/// anything once these buttons' text became an icon -- Qt's icon-mode
/// lookup, not the stylesheet, drives the hover tint for icon-only
/// QToolButtons now. NOTE: QPushButton does NOT map hover to QIcon::Active
/// (QCommonStyle only promotes State_HasFocus there) -- this overload only
/// takes effect on QToolButton (autoRaise) sites; callers with a
/// QPushButton must convert to QToolButton to pick it up (see
/// StartWidget.cpp's removeBtn/configBtn) or leave the icon static.
QIcon icon(const QString& name, int sizePx, const QColor& color, const QColor& disabledColor, const QColor& activeColor);

/// Sets `label`'s pixmap to iconPixmap(name, sizePx, colorProvider(),
/// label->devicePixelRatioF()) immediately, then installs a small
/// QObject-based event filter (parented to `label`, so its lifetime is
/// automatic -- no explicit cleanup needed at any call site) that
/// re-renders the pixmap whenever:
///   (a) the label crosses a DPR boundary (QEvent::DevicePixelRatioChange,
///       e.g. the window is dragged from a 100% monitor to a 200% one on
///       a mixed-DPI multi-monitor setup), or
///   (b) the live theme switches (QEvent::PaletteChange -- see the LIVE
///       THEME-SWITCH CONTRACT below for why that's the event every
///       widget, not just top-level ones, actually receives).
/// Case (b) is why this overload takes a COLOR PROVIDER rather than a
/// fixed QColor: re-rendering with the color captured at bind time would
/// just repaint the same stale tint after a Dark/Light switch. Pass a
/// closure that reads the CURRENT token, e.g.
/// `Theme::bindIconLabel(label, "info", 16, []{ return Theme::textDim; });`
/// -- it's re-invoked on every repaint, so it always reflects whichever
/// palette is active *at repaint time*. Safe to call again on the same
/// label to change name/size/provider (e.g. an expand/collapse chevron
/// flip, or a status icon whose tint depends on live state) -- re-binds
/// the existing filter's stored parameters and repaints immediately
/// rather than stacking a second filter.
void bindIconLabel(QLabel* label, const QString& name, int sizePx, std::function<QColor()> colorProvider);

/// Convenience overload for a genuinely fixed color that is NOT a
/// Theme:: token (e.g. a color baked from live render/scene state that
/// has nothing to do with light/dark mode). Internally just wraps
/// `color` in a provider closure and forwards to the overload above --
/// a label bound through THIS overload still repaints on DPR change and
/// on a theme switch, it just repaints with the SAME fixed color both
/// times. If the color you're passing is (or is derived from) a Theme::
/// token, prefer the provider overload above instead, or this label will
/// silently go stale on the next theme switch exactly like the baked-QSS
/// problem this whole contract exists to fix.
void bindIconLabel(QLabel* label, const QString& name, int sizePx, const QColor& color);

// -------------------------------------------------------------- Scrollbars
//
// Stock Fusion QScrollBar is heavy and visually mismatched against this
// app's slim panel chrome. Returns a QSS snippet -- ~8px vertical and
// horizontal, transparent track, rounded (radius 4) handle tinted from the
// CURRENT Theme:: token values, a slightly stronger hover state, and
// zero-size add/sub-line step buttons. Apply it directly to the QScrollBar
// widget itself, e.g. `scrollArea->verticalScrollBar()->setStyleSheet(
// Theme::scrollBarStyleSheet());` -- NOT to the QScrollArea/QAbstractScroll
// Area, which would risk the selector leaking into unrelated children.
// Bakes token colors into the returned string, so -- like every other
// stylesheet in this app -- callers must re-invoke this and re-apply it
// from restyleTheme() (see the LIVE THEME-SWITCH CONTRACT below) to track
// a live theme switch.
QString scrollBarStyleSheet();

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
// UI refinement item 3, Phase 2 (live switching + "Follow system").
// Superseded the old "restart recommended" mechanism: see the LIVE
// THEME-SWITCH CONTRACT below, which every panel/widget class with
// token-dependent styling must follow.
//
// =====================================================================
// LIVE THEME-SWITCH CONTRACT -- read this before styling ANY widget
// with a Theme:: token, and cite this block from new panel code.
// =====================================================================
//
// 1. Theme::setMode(newMode, app) reassigns every Theme:: token global
//    to the new mode's palette (applyThemeTokens, Theme.cpp), persists
//    the choice to QSettings ("themeMode"), then calls
//    Theme::applyPalette(app). QApplication::setPalette() inside
//    applyPalette() kicks off QT'S OWN palette-resolution cascade:
//    top-level windows (MainWindow) receive QEvent::ApplicationPalette-
//    Change; each top-level widget's default QWidget::event() responds
//    by calling resolvePalette() on ITSELF, which -- via
//    QWidgetPrivate::propagatePaletteChange() -- sends
//    QEvent::PaletteChange to itself and recurses into every descendant
//    widget, all the way down the tree.
//
//    IMPORTANT, verified against this tree's vendored Qt 6.9.3 source
//    (qtbase/src/widgets/kernel/qapplication.cpp:1213-1217 + :1746-1756,
//    qwidget.cpp:1860 propagatePaletteChange / :4633 resolvePalette /
//    :9234 the ApplicationPaletteChange case): QEvent::ApplicationPalette-
//    Change is delivered ONLY to top-level/window widgets -- Qt Widgets'
//    own dispatch loop for it explicitly skips `!widget->isWindow()`
//    widgets. Every nested (non-top-level) widget -- i.e. essentially
//    every panel in this app, since MainWindow is the only top-level
//    widget -- instead receives QEvent::PaletteChange, delivered by the
//    C++ propagation cascade above, not a second application-wide
//    event post. **QEvent::PaletteChange is therefore the one event
//    that reaches every widget in this app, top-level or nested -- use
//    THAT, not ApplicationPaletteChange, as your live-switch hook.**
//    (MainWindow gets both -- ApplicationPaletteChange first, then its
//    own PaletteChange from resolving itself -- so hooking
//    PaletteChange uniformly, including in MainWindow, is correct and
//    keeps the rule the same for every widget class.)
//
// 2. Every panel/widget class that bakes Theme:: tokens into a
//    setStyleSheet() string, a locally-set QPalette (setPalette() on a
//    widget makes an explicit override that stops tracking the
//    QApplication palette automatically), or a Theme::icon() /
//    Theme::bindIconLabel() tint gets a private `void restyleTheme();`
//    that contains ALL of that widget's token-dependent styling in one
//    place. Call it:
//      (a) once at the end of the widget's constructor, and
//      (b) from a `void changeEvent(QEvent* e) override` that calls the
//          base class implementation FIRST, then calls restyleTheme()
//          when `e->type() == QEvent::PaletteChange`.
//    restyleTheme() MUST be idempotent (safe to call any number of
//    times with no widgets created or destroyed in between) and MUST
//    NOT create widgets -- it only re-applies EXISTING widgets'
//    stylesheets / palettes / icon tints from the CURRENT Theme::
//    token values. See MainWindow::restyleTheme() for the reference
//    implementation (splitter handle color, the tab-strip border
//    stylesheet, the active/inactive tab-button stylesheets, the
//    left/right panel border stylesheets, and the locally-set
//    QPalette::Window fills).
//
// 3. Custom-painted widgets whose paintEvent() reads Theme:: tokens
//    directly already repaint with live values on every paint; Qt's own
//    setPalette_helper() already calls `update()` on every widget whose
//    resolved palette changed, so in practice these need NO extra code.
//    The one exception is a widget that CACHES a token value across
//    paints (e.g. into a member instead of re-reading Theme::x each
//    paintEvent) -- that one still needs a changeEvent(QEvent::Palette-
//    Change) hook, but only to refresh the cached value + call update(),
//    not a full restyleTheme().
//
// 4. TWO RE-ENTRANCY GUARDS, both required, uniformly, on every class
//    that implements point 2's changeEvent() hook (added after a
//    startup crash + latent infinite-recursion risk found 2026-07-23):
//
//    (a) QWidget::setStyleSheet() triggers an IMMEDIATE synchronous style
//        repolish; QStyleSheetStyle::polish() sets the widget's palette,
//        which synchronously delivers QEvent::PaletteChange to that same
//        widget, RIGHT THEN, before setStyleSheet() returns. If any
//        restyleTheme() call -- including the ctor-end one -- calls
//        setStyleSheet() on `this` (or a child) BEFORE every member
//        pointer restyleTheme() touches has been constructed, the
//        re-entrant changeEvent() -> restyleTheme() call dereferences a
//        still-null member and crashes (0xC0000005 in Qt6Widgets.dll).
//        The SAME mechanism risks unbounded recursion any time
//        restyleTheme() -> setStyleSheet() -> PaletteChange ->
//        restyleTheme() forms a cycle after startup, not just during
//        construction.
//    (b) Guard with two members, `bool m_themeReady = false;` and
//        `int m_themeEpochSeen = -1;`:
//          - Set `m_themeReady = true;` immediately BEFORE the ctor-end
//            restyleTheme() call (i.e. member construction is complete
//            enough that restyleTheme() is safe to run at all).
//          - restyleTheme() records `m_themeEpochSeen = Theme::
//            paletteEpoch();` as its FIRST line -- this fires both from
//            the ctor-end call and from every changeEvent()-triggered
//            call, so the epoch is always current the instant
//            restyleTheme() starts running (before any setStyleSheet()
//            call inside it can trigger a synchronous re-entrant
//            PaletteChange).
//          - changeEvent() becomes: call the base implementation FIRST,
//            then `if (e->type() == QEvent::PaletteChange && m_themeReady
//            && m_themeEpochSeen != Theme::paletteEpoch()) restyleTheme();`
//        Theme::paletteEpoch() increments exactly once per real token
//        application (Theme.cpp's applyThemeTokens() choke point) --
//        NOT once per setStyleSheet call -- so a re-entrant PaletteChange
//        synchronously nested inside the SAME restyleTheme() call always
//        sees an epoch it has already recorded and is skipped, while a
//        genuine LATER theme switch (new epoch) still restyles normally.
//        m_themeReady alone blocks the startup crash (no restyleTheme()
//        call is possible before the ctor reaches its own call); the
//        epoch check alone blocks the post-startup recursion risk. Keep
//        both, uniformly, on every changeEvent()-overriding class -- see
//        MainWindow::changeEvent / MainWindow::restyleTheme for the
//        reference implementation.
//
// 5. QUEUED (never synchronous) row rebuilds for panels whose dynamic
//    rows bake tokens (added 2026-07-23 review, P2 fix). Point 2 already
//    forbids restyleTheme() from creating widgets -- several panels
//    (OutlinerWidget, ViewportProperties, EnvironmentPanel) have rows
//    that are torn down and rebuilt wholesale on every data refresh
//    (rebuild() / rebuildPropertyRows()), baking Theme:: tokens into
//    each row at BUILD time rather than reading them live in a
//    paintEvent. Those panels correctly do NOT call their own
//    rebuild-style method synchronously from restyleTheme() -- but the
//    self-heal they lean on instead (the next render frame's
//    imageUpdated-driven refresh, or the next selection/edit) never
//    fires if the theme switches while the viewport is completely IDLE,
//    leaving stale-palette rows on screen until the next real
//    interaction. The fix is a QUEUED follow-up, `QTimer::singleShot(0,
//    this, [this]() { ... });`, appended at the end of restyleTheme():
//    it defers the widget-recreation work OUT of the synchronous
//    palette-change cascade restyleTheme() runs inside (satisfying
//    point 2) and onto the next event-loop turn, where creating/
//    destroying widgets is safe again. Two rules for what the queued
//    lambda may call:
//      (a) Prefer the CHEAPEST existing rebuild path that replays from
//          members ALREADY CACHED on the panel (e.g. OutlinerWidget's
//          m_entitiesByCategory, EnvironmentPanel's m_env/m_hasInfo) --
//          never a method that pulls a fresh snapshot from the engine/
//          controller (e.g. ViewportBridge::propertySnapshot(), which
//          calls RISE_API_SceneEditController_RefreshProperties and can
//          block on the controller's commit mutex during a render).  If
//          the panel's only rebuild path ALSO pulls, add a
//          cache-only variant (see ViewportProperties::
//          rebuildPropertyRows's `pullFreshSnapshot` parameter) rather
//          than either skipping the panel or accepting the round-trip
//          risk from inside a deferred callback.
//      (b) Respect any existing re-entrancy guard the panel already has
//          for the SAME rebuild path (e.g. ViewportProperties'
//          m_scrubbing, which guards against deleting a ScrubHandle
//          whose mouseMoveEvent might still be on the call stack) --
//          the queued lambda must check it too, not just the panel's
//          normal refresh() entry point.
//    A queued rebuild that loses a race with a real refresh()/rebuild()
//    that runs first is a harmless no-op, not a hazard -- both replay
//    idempotently from the same underlying state.
//
// System mode: ThemeMode::System resolves to Dark or Light at apply
// time via QGuiApplication::styleHints()->colorScheme() (added Qt 6.5;
// this tree ships Qt 6.9.3, confirmed via qtcoreversion.h). Theme::mode()
// returns whatever the user picked (Dark / Light / System, i.e. what's
// persisted); Theme::effectiveMode() returns what's actually APPLIED to
// the token globals right now -- always Dark or Light, NEVER System.
// Panels that need to know "am I dark or light" (as opposed to "did the
// user pick System") should read effectiveMode().
//
// Theme::installSystemThemeWatcher(app), called once from main.cpp
// immediately after Theme::applyPalette(app), connects
// QGuiApplication::styleHints()->colorSchemeChanged to a handler that --
// ONLY when the persisted mode is System -- re-resolves effectiveMode(),
// re-runs applyThemeTokens, and calls Theme::applyPalette(app) again.
// That means flipping the OS theme while RISE is running (no restart,
// no menu click) updates the whole app live, the same way a manual
// Theme::setMode() call does, and follows the exact same PaletteChange
// cascade described in point 1 above.
//
// If the platform reports Qt::ColorScheme::Unknown (no OS-level signal
// available, e.g. some Linux window managers, or a Windows build where
// the platform theme plugin hasn't wired it up), System resolves to
// Dark -- RISE's primary/first-class theme -- rather than leaving the
// token globals unresolved or flapping.

enum class ThemeMode { Dark, Light, System };

/// The mode the user picked (Dark / Light / System) -- i.e. what's
/// persisted to QSettings. Defaults to Dark until loadPersistedMode()
/// runs. NOTE: this can be ThemeMode::System; if you need to know which
/// concrete palette is actually applied right now, call effectiveMode()
/// instead.
ThemeMode mode();

/// The concrete palette actually applied to the token globals right
/// now: always ThemeMode::Dark or ThemeMode::Light, resolved from
/// mode() (System is resolved via QStyleHints::colorScheme() -- see the
/// LIVE THEME-SWITCH CONTRACT's "System mode" section above). Panels
/// that branch on "am I dark or light" should read this, not mode().
ThemeMode effectiveMode();

/// Monotonically increasing counter, incremented exactly once per real
/// token application -- the single choke point inside applyThemeTokens()
/// (Theme.cpp), which loadPersistedMode(), setMode(), and the system
/// theme watcher's colorSchemeChanged handler all funnel through. NOT
/// incremented by applyPalette() itself (main.cpp's initial startup call
/// invokes applyPalette() once with no paired applyThemeTokens() call,
/// which would otherwise double-count). See point 4 of the LIVE THEME-
/// SWITCH CONTRACT above -- every changeEvent(QEvent::PaletteChange)
/// handler compares this against a per-widget `m_themeEpochSeen` and
/// restyles only on a NEW epoch, which is what makes a setStyleSheet-
/// triggered synchronous re-entrant PaletteChange (delivered mid-
/// restyleTheme(), before the epoch can have changed again) a no-op
/// instead of a crash or an infinite loop.
int paletteEpoch();

/// Load the persisted mode from QSettings ("themeMode"; a legacy
/// "dark"/"light" value or the new "system" value) and apply its
/// resolved token values (see applyThemeTokens in Theme.cpp).  Call
/// once at startup, before any widget is constructed and before
/// applyPalette().
void loadPersistedMode();

/// Switch the active mode: persists the choice to QSettings
/// ("themeMode"), resolves it to Dark/Light (effectiveMode()),
/// reassigns every token global to that palette's values, and
/// re-applies the QPalette + app font to `app` -- see the LIVE
/// THEME-SWITCH CONTRACT above for exactly how that reaches every
/// widget in the app, live, no restart required, PROVIDED every panel
/// follows the contract's restyleTheme() pattern. A no-op if `newMode`
/// is already the persisted mode.
void setMode(ThemeMode newMode, QApplication& app);

/// Call ONCE from main.cpp, immediately after Theme::applyPalette(app)
/// and after loadPersistedMode(). Connects
/// QGuiApplication::styleHints()->colorSchemeChanged to a handler that
/// re-resolves and re-applies the theme live whenever the OS scheme
/// changes AND the persisted mode is ThemeMode::System (a no-op
/// connection that just never fires its body otherwise). See the LIVE
/// THEME-SWITCH CONTRACT's "System mode" section above.
void installSystemThemeWatcher(QApplication& app);

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
