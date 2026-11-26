//
//  Theme.swift
//  RISE-GUI
//
//  Design tokens for the RISE UI redesign (docs/gui/DESIGN_BRIEF.md).
//  Single source of truth for color, type, spacing and radii on macOS.
//  The Windows client mirrors these values in build/VS2022/RISE-GUI/Theme.h;
//  the two files are kept in sync by convention — change both together,
//  INCLUDING the light palette added below once Windows gains one too.
//
//  Palette lifted from the approved "RISE Prototype" design-comp
//  (claude.ai/design project "RISE UI Full Screen Layout").
//
//  Dark is the primary/first-class theme; light is the secondary,
//  mirrored theme (added for runtime light/dark switching — see
//  ThemeMode.swift). Every public token below is a computed `static
//  var` that forwards to `DarkPalette` or `LightPalette` based on
//  `ThemeState.mode`, so every existing call site (`Theme.bgPanel`,
//  `Theme.textPrimary`, ...) keeps compiling completely unchanged
//  while gaining runtime theme switching. Radii, fonts, and the
//  spectral identity gradient are theme-invariant and stay plain
//  `static let`s.
//

import SwiftUI
import CoreText

enum Theme {

    // MARK: - Dark palette (primary theme)

    private enum DarkPalette {
        // Surfaces
        static let bgBase = Color(hex: 0x131416)
        static let bgTopBar = Color(hex: 0x18191c)
        static let bgPanel = Color(hex: 0x17181b)
        static let bgCenter = Color(hex: 0x0e0f11)
        static let bgWell = Color(hex: 0x101114)
        static let bgCard = Color(hex: 0x131417)
        static let bgCardDeep = Color(hex: 0x131518)
        static let bgPopup = Color(hex: 0x1c1e22)
        static let bgHeader = Color(hex: 0x141518)
        static let bgTimeline = Color(hex: 0x111214)
        static let bgBubbleUser = Color(hex: 0x24262b)

        // Text ramp
        static let textPrimary = Color(hex: 0xe6e7e9)
        static let textSecondary = Color(hex: 0xc9cbd1)
        static let textTertiary = Color(hex: 0xb8bac0)
        static let textMuted = Color(hex: 0x9a9da4)
        static let textFaint = Color(hex: 0x8b8e94)
        static let textDim = Color(hex: 0x6f7278)
        static let textDisabled = Color(hex: 0x5c5f66)
        static let textGhost = Color(hex: 0x494c52)

        // Accents
        static let accent = Color(hex: 0x6db8e8)
        static let accentLight = Color(hex: 0x9ecbe8)
        static let accentSoft = Color(hex: 0x8fb8e8)
        // Text drawn directly ON a solid accent fill (ProposalCard Apply,
        // PropertiesPanel Save). Near-black on the light-blue dark-mode
        // accent (~8.7:1).
        static let textOnAccent = Color(hex: 0x0d1116)
        static let success = Color(hex: 0x7fb98a)
        static let successLight = Color(hex: 0xa9d4b1)
        static let warn = Color(hex: 0xe0b25a)
        static let dirty = Color(hex: 0xe8a33d)
        static let error = Color(hex: 0xe09a9a)
        static let errorStrong = Color(hex: 0xe05a5a)
        static let purple = Color(hex: 0xc8a0e8)
        static let gold = Color(hex: 0xd4b98a)
        static let teal = Color(hex: 0x8fd4c4)

        // Category tag colors that don't forward to another token
        static let catMaterial = Color(hex: 0xc9a0d4)
        static let catFilm = Color(hex: 0x8fa8c9)
        static let catVariant = Color(hex: 0xcf9fd6)

        // Borders — white at fixed opacities
        static let borderHairline = Color.white.opacity(0.07)
        static let borderLight = Color.white.opacity(0.09)
        static let borderMedium = Color.white.opacity(0.12)
        static let borderStrong = Color.white.opacity(0.14)
        static let borderHover = Color.white.opacity(0.25)

        // Fills
        static let fillHover = Color.white.opacity(0.08)
        static let fillActive = Color.white.opacity(0.12)
        static let fillTrough = Color.white.opacity(0.10)
    }

    // MARK: - Light palette (secondary theme, mirror of dark)
    //
    // Neutral (non-tinted) grays for color-accurate render surround.
    // `bgCenter` is deliberately a mid neutral gray rather than
    // bright white: the center column is the image-forward viewport
    // surround, and a bright-white surround would bias perceived
    // image brightness/contrast during color-critical evaluation —
    // same reason the dark palette uses its darkest surface there.
    // Accent hues are darkened from their dark-mode counterparts to
    // clear ~4.5:1 contrast against light backgrounds while staying
    // recognizably the same color family. Border/fill tokens switch
    // from white-opacity to black-opacity (white-opacity overlays are
    // invisible on a light background).

    private enum LightPalette {
        // Surfaces
        static let bgBase = Color(hex: 0xececee)
        static let bgTopBar = Color(hex: 0xf4f4f6)
        static let bgPanel = Color(hex: 0xf0f0f2)
        static let bgCenter = Color(hex: 0xb8b8bc)
        static let bgWell = Color(hex: 0xffffff)
        static let bgCard = Color(hex: 0xf7f7f9)
        static let bgCardDeep = Color(hex: 0xf2f3f5)
        static let bgPopup = Color(hex: 0xffffff)
        static let bgHeader = Color(hex: 0xebebee)
        static let bgTimeline = Color(hex: 0xf0f0f3)
        static let bgBubbleUser = Color(hex: 0xdde4ee)

        // Text ramp (inverted: dark text on light surfaces)
        static let textPrimary = Color(hex: 0x1a1b1e)
        static let textSecondary = Color(hex: 0x33353a)
        static let textTertiary = Color(hex: 0x4a4d54)
        static let textMuted = Color(hex: 0x5f636b)
        static let textFaint = Color(hex: 0x767a83)
        static let textDim = Color(hex: 0x8f939c)
        static let textDisabled = Color(hex: 0xa6aab2)
        static let textGhost = Color(hex: 0xc2c5cb)

        // Accents (darkened for contrast, same hue family)
        static let accent = Color(hex: 0x1a6fa8)
        static let accentLight = Color(hex: 0x2580bd)
        static let accentSoft = Color(hex: 0x3a77ad)
        // White, not the dark palette's near-black: the light-mode accent
        // is darkened for contrast, and near-black text on it is only
        // ~3.5:1 (fails AA for 11-12pt text) while white is ~4.6:1.
        // The Windows client (Theme.h/Theme.cpp) mirrors this divergence.
        static let textOnAccent = Color(hex: 0xffffff)
        static let success = Color(hex: 0x2e7d43)
        static let successLight = Color(hex: 0x3d8f52)
        static let warn = Color(hex: 0x9a6b10)
        static let dirty = Color(hex: 0xa86e14)
        static let error = Color(hex: 0xb03a3a)
        static let errorStrong = Color(hex: 0xc23030)
        static let purple = Color(hex: 0x7b4fa6)
        static let gold = Color(hex: 0x8a6c2f)
        static let teal = Color(hex: 0x2b7d6e)

        // Category tag colors that don't forward to another token
        static let catMaterial = Color(hex: 0x8a5a9e)
        static let catFilm = Color(hex: 0x56708f)
        static let catVariant = Color(hex: 0x93589e)

        // Borders — black at fixed opacities
        static let borderHairline = Color.black.opacity(0.10)
        static let borderLight = Color.black.opacity(0.13)
        static let borderMedium = Color.black.opacity(0.18)
        static let borderStrong = Color.black.opacity(0.22)
        static let borderHover = Color.black.opacity(0.35)

        // Fills
        static let fillHover = Color.black.opacity(0.06)
        static let fillActive = Color.black.opacity(0.10)
        static let fillTrough = Color.black.opacity(0.08)
    }

    // MARK: - Surfaces (dark-first, neutral grays — color-accurate surround)

    /// Window / deepest background.
    static var bgBase: Color { ThemeState.mode == .dark ? DarkPalette.bgBase : LightPalette.bgBase }
    /// Top bar background.
    static var bgTopBar: Color { ThemeState.mode == .dark ? DarkPalette.bgTopBar : LightPalette.bgTopBar }
    /// Side panel background (left agent/scene panel, right outliner/inspector).
    static var bgPanel: Color { ThemeState.mode == .dark ? DarkPalette.bgPanel : LightPalette.bgPanel }
    /// Center viewport-column background (darkest in dark mode / mid-gray in light mode, image-forward surround).
    static var bgCenter: Color { ThemeState.mode == .dark ? DarkPalette.bgCenter : LightPalette.bgCenter }
    /// Input wells, value fields, log body, segmented-control troughs.
    static var bgWell: Color { ThemeState.mode == .dark ? DarkPalette.bgWell : LightPalette.bgWell }
    /// Cards inside panels (hero widgets, diff card body).
    static var bgCard: Color { ThemeState.mode == .dark ? DarkPalette.bgCard : LightPalette.bgCard }
    /// Card footers / slightly deeper card region.
    static var bgCardDeep: Color { ThemeState.mode == .dark ? DarkPalette.bgCardDeep : LightPalette.bgCardDeep }
    /// Menus, popovers, toasts.
    static var bgPopup: Color { ThemeState.mode == .dark ? DarkPalette.bgPopup : LightPalette.bgPopup }
    /// Section header strips (log header, sub-toolbars).
    static var bgHeader: Color { ThemeState.mode == .dark ? DarkPalette.bgHeader : LightPalette.bgHeader }
    /// Timeline strip.
    static var bgTimeline: Color { ThemeState.mode == .dark ? DarkPalette.bgTimeline : LightPalette.bgTimeline }
    /// User chat bubble.
    static var bgBubbleUser: Color { ThemeState.mode == .dark ? DarkPalette.bgBubbleUser : LightPalette.bgBubbleUser }

    // MARK: - Text

    static var textPrimary: Color { ThemeState.mode == .dark ? DarkPalette.textPrimary : LightPalette.textPrimary }
    static var textSecondary: Color { ThemeState.mode == .dark ? DarkPalette.textSecondary : LightPalette.textSecondary }
    static var textTertiary: Color { ThemeState.mode == .dark ? DarkPalette.textTertiary : LightPalette.textTertiary }
    static var textMuted: Color { ThemeState.mode == .dark ? DarkPalette.textMuted : LightPalette.textMuted }
    static var textFaint: Color { ThemeState.mode == .dark ? DarkPalette.textFaint : LightPalette.textFaint }
    static var textDim: Color { ThemeState.mode == .dark ? DarkPalette.textDim : LightPalette.textDim }
    static var textDisabled: Color { ThemeState.mode == .dark ? DarkPalette.textDisabled : LightPalette.textDisabled }
    static var textGhost: Color { ThemeState.mode == .dark ? DarkPalette.textGhost : LightPalette.textGhost }

    // MARK: - Accents

    /// Primary accent — selection, agent presence, links, active states.
    static var accent: Color { ThemeState.mode == .dark ? DarkPalette.accent : LightPalette.accent }
    /// Lighter accent for text on dark chips.
    static var accentLight: Color { ThemeState.mode == .dark ? DarkPalette.accentLight : LightPalette.accentLight }
    /// Softer accent for diff block headers / gutters.
    static var accentSoft: Color { ThemeState.mode == .dark ? DarkPalette.accentSoft : LightPalette.accentSoft }
    /// Text drawn directly ON a solid accent fill (Apply / Save pills).
    /// Near-black in dark mode, white in light mode — see the palette
    /// comments for the AA-contrast rationale of the divergence.
    static var textOnAccent: Color { ThemeState.mode == .dark ? DarkPalette.textOnAccent : LightPalette.textOnAccent }
    /// Success / parse-OK / additions.
    static var success: Color { ThemeState.mode == .dark ? DarkPalette.success : LightPalette.success }
    /// Lighter success for diff "+" text.
    static var successLight: Color { ThemeState.mode == .dark ? DarkPalette.successLight : LightPalette.successLight }
    /// Warnings, region badge, WARN counts, gold values.
    static var warn: Color { ThemeState.mode == .dark ? DarkPalette.warn : LightPalette.warn }
    /// Dirty-dot amber.
    static var dirty: Color { ThemeState.mode == .dark ? DarkPalette.dirty : LightPalette.dirty }
    /// Errors / deletions (soft red text).
    static var error: Color { ThemeState.mode == .dark ? DarkPalette.error : LightPalette.error }
    /// Strong red for diff-deletion backgrounds (use with opacity ~0.1).
    static var errorStrong: Color { ThemeState.mode == .dark ? DarkPalette.errorStrong : LightPalette.errorStrong }
    /// Agent / reference / material identity.
    static var purple: Color { ThemeState.mode == .dark ? DarkPalette.purple : LightPalette.purple }
    /// Values / units gold (scene-text values, lens readouts).
    static var gold: Color { ThemeState.mode == .dark ? DarkPalette.gold : LightPalette.gold }
    /// Animation-category teal.
    static var teal: Color { ThemeState.mode == .dark ? DarkPalette.teal : LightPalette.teal }

    // MARK: - Borders (white-opacity in dark mode, black-opacity in light mode)

    static var borderHairline: Color { ThemeState.mode == .dark ? DarkPalette.borderHairline : LightPalette.borderHairline }
    static var borderLight: Color { ThemeState.mode == .dark ? DarkPalette.borderLight : LightPalette.borderLight }
    static var borderMedium: Color { ThemeState.mode == .dark ? DarkPalette.borderMedium : LightPalette.borderMedium }
    static var borderStrong: Color { ThemeState.mode == .dark ? DarkPalette.borderStrong : LightPalette.borderStrong }
    static var borderHover: Color { ThemeState.mode == .dark ? DarkPalette.borderHover : LightPalette.borderHover }

    // MARK: - Fills

    static var fillHover: Color { ThemeState.mode == .dark ? DarkPalette.fillHover : LightPalette.fillHover }
    static var fillActive: Color { ThemeState.mode == .dark ? DarkPalette.fillActive : LightPalette.fillActive }
    static var fillTrough: Color { ThemeState.mode == .dark ? DarkPalette.fillTrough : LightPalette.fillTrough }

    // MARK: - Spectral identity gradient (380–780 nm motif)
    //
    // Theme-invariant: the spectral gradient is a brand/identity motif,
    // not a surface or text color, so it does not switch with mode.

    static let spectralStops: [Gradient.Stop] = [
        .init(color: Color(hex: 0x5b21b6), location: 0.00),
        .init(color: Color(hex: 0x2563eb), location: 0.30),
        .init(color: Color(hex: 0x0d9488), location: 0.55),
        .init(color: Color(hex: 0xa3b515), location: 0.75),
        .init(color: Color(hex: 0xe05a00), location: 1.00),
    ]

    static var spectralGradient: LinearGradient {
        LinearGradient(gradient: Gradient(stops: spectralStops),
                       startPoint: .leading, endPoint: .trailing)
    }

    // MARK: - Category tag colors (outliner)

    static var catRender: Color { warn }                      // RND
    static var catCamera: Color { accentSoft }                // CAM
    static var catLight: Color { gold }                       // LGT
    static var catObject: Color { accentLight }               // OBJ
    static var catMaterial: Color { ThemeState.mode == .dark ? DarkPalette.catMaterial : LightPalette.catMaterial } // MAT
    static var catAnimation: Color { teal }                   // ANM
    static var catMedia: Color { purple }                     // MED
    /// Output Settings (Film) tag — muted slate blue, distinct from
    /// every other category accent.
    static var catFilm: Color { ThemeState.mode == .dark ? DarkPalette.catFilm : LightPalette.catFilm }        // FLM
    /// scene_variant overlay tag — soft violet, distinct from
    /// catMaterial's purple and catMedia's purple.
    static var catVariant: Color { ThemeState.mode == .dark ? DarkPalette.catVariant : LightPalette.catVariant } // VAR

    // MARK: - Radii

    static let radiusSmall: CGFloat = 5
    static let radiusMedium: CGFloat = 7
    static let radiusLarge: CGFloat = 9
    static let radiusCard: CGFloat = 10

    // MARK: - Type scale

    // PostScript names as shipped in the IBM Plex 1.1.0 release TTFs
    // (the release fonts abbreviate face names: "Medm", "SmBld").

    /// UI sans — IBM Plex Sans with system fallback.
    static func sans(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        if FontBootstrap.plexAvailable {
            switch weight {
            case .bold: return .custom("IBMPlexSans-Bold", fixedSize: size)
            case .semibold: return .custom("IBMPlexSans-SmBld", fixedSize: size)
            case .medium: return .custom("IBMPlexSans-Medm", fixedSize: size)
            default: return .custom("IBMPlexSans", fixedSize: size)
            }
        }
        return .system(size: size, weight: weight)
    }

    /// Monospace — IBM Plex Mono with system fallback (scene text, readouts, log).
    static func mono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        if FontBootstrap.plexAvailable {
            switch weight {
            case .semibold, .bold: return .custom("IBMPlexMono-SmBld", fixedSize: size)
            case .medium: return .custom("IBMPlexMono-Medm", fixedSize: size)
            default: return .custom("IBMPlexMono", fixedSize: size)
            }
        }
        return .system(size: size, weight: weight, design: .monospaced)
    }

    /// NSFont variants for AppKit-hosted views (text editor, suggestions).
    static func monoNSFont(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        if FontBootstrap.plexAvailable,
           let f = NSFont(name: weight == .regular ? "IBMPlexMono" : "IBMPlexMono-Medm", size: size) {
            return f
        }
        return NSFont.monospacedSystemFont(ofSize: size, weight: weight)
    }
}

// MARK: - Font registration

enum FontBootstrap {
    /// True once the bundled IBM Plex faces registered successfully.
    private(set) static var plexAvailable = false

    /// Register the bundled IBM Plex TTFs for this process. Call once at app startup,
    /// before any view renders. Safe to call repeatedly.
    static func registerBundledFonts() {
        guard !plexAvailable else { return }
        let names = [
            "IBMPlexSans-Regular", "IBMPlexSans-Medium", "IBMPlexSans-SemiBold", "IBMPlexSans-Bold",
            "IBMPlexMono-Regular", "IBMPlexMono-Medium", "IBMPlexMono-SemiBold",
        ]
        var registeredAny = false
        for name in names {
            guard let url = Bundle.main.url(forResource: name, withExtension: "ttf") else { continue }
            var cfError: Unmanaged<CFError>?
            if CTFontManagerRegisterFontsForURL(url as CFURL, .process, &cfError) {
                registeredAny = true
            } else if let err = cfError?.takeRetainedValue() {
                // kCTFontManagerErrorAlreadyRegistered (105) is fine — count it as present.
                if CFErrorGetCode(err) == 105 { registeredAny = true }
            }
        }
        plexAvailable = registeredAny && NSFont(name: "IBMPlexSans", size: 13) != nil
    }
}

// MARK: - Hex color convenience

extension Color {
    /// sRGB color from a 0xRRGGBB literal.
    init(hex: UInt32, opacity: Double = 1.0) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xff) / 255.0,
                  green: Double((hex >> 8) & 0xff) / 255.0,
                  blue: Double(hex & 0xff) / 255.0,
                  opacity: opacity)
    }
}

extension NSColor {
    /// sRGB color from a 0xRRGGBB literal (AppKit-side twin of Color(hex:)).
    convenience init(hex: UInt32, alpha: CGFloat = 1.0) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xff) / 255.0,
                  green: CGFloat((hex >> 8) & 0xff) / 255.0,
                  blue: CGFloat(hex & 0xff) / 255.0,
                  alpha: alpha)
    }
}
