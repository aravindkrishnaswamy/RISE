//
//  Theme.cpp
//  RISE-GUI (Windows / Qt6)
//
//  Font registration + lookup for the bundled IBM Plex faces.
//  See Theme.h for the token definitions this pairs with.
//

#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

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

void applyDarkPalette(QApplication& app)
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

} // namespace Theme
