//
//  Theme.swift
//  RISE-GUI
//
//  Design tokens for the RISE UI redesign (docs/gui/DESIGN_BRIEF.md).
//  Single source of truth for color, type, spacing and radii on macOS.
//  The Windows client mirrors these values in build/VS2022/RISE-GUI/Theme.h;
//  the two files are kept in sync by convention — change both together.
//
//  Palette lifted from the approved "RISE Prototype" design-comp
//  (claude.ai/design project "RISE UI Full Screen Layout").
//

import SwiftUI
import CoreText

enum Theme {

    // MARK: - Surfaces (dark-first, neutral grays — color-accurate surround)

    /// Window / deepest background.
    static let bgBase = Color(hex: 0x131416)
    /// Top bar background.
    static let bgTopBar = Color(hex: 0x18191c)
    /// Side panel background (left agent/scene panel, right outliner/inspector).
    static let bgPanel = Color(hex: 0x17181b)
    /// Center viewport-column background (darkest, image-forward surround).
    static let bgCenter = Color(hex: 0x0e0f11)
    /// Input wells, value fields, log body, segmented-control troughs.
    static let bgWell = Color(hex: 0x101114)
    /// Cards inside panels (hero widgets, diff card body).
    static let bgCard = Color(hex: 0x131417)
    /// Card footers / slightly deeper card region.
    static let bgCardDeep = Color(hex: 0x131518)
    /// Menus, popovers, toasts.
    static let bgPopup = Color(hex: 0x1c1e22)
    /// Section header strips (log header, sub-toolbars).
    static let bgHeader = Color(hex: 0x141518)
    /// Timeline strip.
    static let bgTimeline = Color(hex: 0x111214)
    /// User chat bubble.
    static let bgBubbleUser = Color(hex: 0x24262b)

    // MARK: - Text

    static let textPrimary = Color(hex: 0xe6e7e9)
    static let textSecondary = Color(hex: 0xc9cbd1)
    static let textTertiary = Color(hex: 0xb8bac0)
    static let textMuted = Color(hex: 0x9a9da4)
    static let textFaint = Color(hex: 0x8b8e94)
    static let textDim = Color(hex: 0x6f7278)
    static let textDisabled = Color(hex: 0x5c5f66)
    static let textGhost = Color(hex: 0x494c52)

    // MARK: - Accents

    /// Primary accent — selection, agent presence, links, active states.
    static let accent = Color(hex: 0x6db8e8)
    /// Lighter accent for text on dark chips.
    static let accentLight = Color(hex: 0x9ecbe8)
    /// Softer accent for diff block headers / gutters.
    static let accentSoft = Color(hex: 0x8fb8e8)
    /// Success / parse-OK / additions.
    static let success = Color(hex: 0x7fb98a)
    /// Lighter success for diff "+" text.
    static let successLight = Color(hex: 0xa9d4b1)
    /// Warnings, region badge, WARN counts, gold values.
    static let warn = Color(hex: 0xe0b25a)
    /// Dirty-dot amber.
    static let dirty = Color(hex: 0xe8a33d)
    /// Errors / deletions (soft red text).
    static let error = Color(hex: 0xe09a9a)
    /// Strong red for diff-deletion backgrounds (use with opacity ~0.1).
    static let errorStrong = Color(hex: 0xe05a5a)
    /// Agent / reference / material identity.
    static let purple = Color(hex: 0xc8a0e8)
    /// Values / units gold (scene-text values, lens readouts).
    static let gold = Color(hex: 0xd4b98a)
    /// Animation-category teal.
    static let teal = Color(hex: 0x8fd4c4)

    // MARK: - Borders (white at fixed opacities)

    static let borderHairline = Color.white.opacity(0.07)
    static let borderLight = Color.white.opacity(0.09)
    static let borderMedium = Color.white.opacity(0.12)
    static let borderStrong = Color.white.opacity(0.14)
    static let borderHover = Color.white.opacity(0.25)

    // MARK: - Fills

    static let fillHover = Color.white.opacity(0.08)
    static let fillActive = Color.white.opacity(0.12)
    static let fillTrough = Color.white.opacity(0.10)

    // MARK: - Spectral identity gradient (380–780 nm motif)

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

    static let catRender = warn                      // RND
    static let catCamera = accentSoft                // CAM
    static let catLight = gold                       // LGT
    static let catObject = accentLight               // OBJ
    static let catMaterial = Color(hex: 0xc9a0d4)    // MAT
    static let catAnimation = teal                   // ANM
    static let catMedia = purple                     // MED

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
