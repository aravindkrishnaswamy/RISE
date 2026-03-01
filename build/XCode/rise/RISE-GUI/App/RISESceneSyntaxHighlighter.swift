import AppKit

/// Color and font theme for RISE scene file syntax highlighting.
/// All colors use NSColor.system* variants that automatically adapt to light/dark mode.
struct RISESceneTheme {
    let defaultText: NSColor = .labelColor
    let comment: NSColor = .systemGreen
    let fileHeader: NSColor = .systemPurple
    let blockKeyword: NSColor = .systemBlue
    let propertyKey: NSColor = .systemIndigo
    let preprocessor: NSColor = .systemOrange
    let loopDirective: NSColor = .systemOrange
    let command: NSColor = .systemTeal
    let macroRef: NSColor = .systemRed
    let mathExpr: NSColor = .systemPink
    let number: NSColor = .systemCyan
    let braces: NSColor = .systemGray

    let font: NSFont = .monospacedSystemFont(ofSize: 12, weight: .regular)
    let boldFont: NSFont = .monospacedSystemFont(ofSize: 12, weight: .bold)
}

/// Applies syntax highlighting to RISE .RISEscene file content in an NSTextStorage.
///
/// Installed as the `NSTextStorageDelegate` on the editor's text storage. Whenever
/// characters change, the full document is re-highlighted using a single-pass
/// line-by-line classification. This is fast enough for typical scene files
/// (a few thousand lines).
final class RISESceneSyntaxHighlighter: NSObject, NSTextStorageDelegate {

    let theme = RISESceneTheme()

    // MARK: - Block Type Keywords (107 total, from AsciiSceneParser.cpp)

    static let blockKeywords: Set<String> = [
        // Painters
        "uniformcolor_painter", "spectral_painter", "png_painter", "hdr_painter",
        "exr_painter", "tiff_painter", "checker_painter", "lines_painter",
        "mandelbrot_painter", "perlin2d_painter", "perlin3d_painter",
        "voronoi2d_painter", "voronoi3d_painter", "iridescent_painter",
        "blackbody_painter", "blend_painter",
        // Functions
        "piecewise_linear_function", "piecewise_linear_function2d",
        // Materials
        "lambertian_material", "perfectreflector_material", "perfectrefractor_material",
        "polished_material", "dielectric_material", "lambertian_luminaire_material",
        "phong_luminaire_material", "ashikminshirley_anisotropicphong_material",
        "isotropic_phong_material", "translucent_material", "biospec_skin_material",
        "generic_human_tissue_material", "composite_material",
        "ward_isotropic_material", "ward_anisotropic_material",
        "cooktorrance_material", "orennayar_material", "schlick_material",
        "datadriven_material",
        // Cameras
        "pinhole_camera", "onb_pinhole_camera", "thinlens_camera",
        "realistic_camera", "fisheye_camera", "orthographic_camera",
        // Geometry
        "sphere_geometry", "ellipsoid_geometry", "cylinder_geometry",
        "torus_geometry", "infiniteplane_geometry", "box_geometry",
        "clippedplane_geometry", "3dsmesh_geometry", "rawmesh_geometry",
        "rawmesh2_geometry", "beziermesh_geometry", "risemesh_geometry",
        "circulardisk_geometry", "bezierpatch_geometry", "bilinearpatch_geometry",
        // Modifiers
        "bumpmap_modifier",
        // Objects
        "standard_object", "csg_object",
        // Shader operations
        "ambientocclusion_shaderop", "directlighting_shaderop",
        "pathtracing_shaderop", "distributiontracing_shaderop",
        "finalgather_shaderop", "simple_sss_shaderop",
        "diffusion_approximation_sss_shaderop", "arealight_shaderop",
        "transparency_shaderop",
        // Shaders
        "standard_shader", "advanced_shader",
        "directvolumerendering_shader", "spectraldirectvolumerendering_shader",
        // Rasterizers
        "pixelpel_rasterizer", "pixelintegratingspectral_rasterizer",
        "adaptivepixelpel_rasterizer", "contrastAApixelpel_rasterizer",
        // Output
        "file_rasterizeroutput",
        // Lights
        "ambient_light", "omni_light", "spot_light", "directional_light",
        // Photon maps & gather
        "caustic_pel_photonmap", "translucent_pel_photonmap",
        "caustic_spectral_photonmap", "global_pel_photonmap",
        "global_spectral_photonmap", "shadow_photonmap",
        "caustic_pel_gather", "translucent_pel_gather",
        "caustic_spectral_gather", "global_pel_gather",
        "global_spectral_gather", "shadow_gather",
        // Other
        "irradiance_cache", "keyframe", "timeline", "animation_options",
    ]

    // MARK: - Pre-compiled Regex Patterns

    private static let bracesRegex = try! NSRegularExpression(pattern: #"[{}]"#)
    private static let macroRefRegex = try! NSRegularExpression(pattern: #"@[A-Za-z_]\w*"#)
    private static let mathExprRegex = try! NSRegularExpression(pattern: #"\$\([^)]*\)"#)
    private static let numberRegex = try! NSRegularExpression(
        pattern: #"(?<=\s)-?(?:\d+\.?\d*|\.\d+)(?=\s|$)"#
    )
    private static let propertyKeyRegex = try! NSRegularExpression(
        pattern: #"^(\t+)(\w+)"#, options: .anchorsMatchLines
    )

    // MARK: - NSTextStorageDelegate

    func textStorage(
        _ textStorage: NSTextStorage,
        didProcessEditing editedMask: NSTextStorageEditActions,
        range editedRange: NSRange,
        changeInLength delta: Int
    ) {
        // Only re-highlight when actual characters changed, not attribute-only edits.
        // This prevents an infinite loop: we change attributes → delegate fires again
        // → but the second call has only .editedAttributes → we skip it.
        guard editedMask.contains(.editedCharacters) else { return }
        highlightAll(in: textStorage)
    }

    // MARK: - Full-Document Highlighting

    /// Re-highlights the entire text storage using a single-pass line-by-line scan.
    func highlightAll(in textStorage: NSTextStorage) {
        let text = textStorage.string
        let fullRange = NSRange(location: 0, length: textStorage.length)
        guard fullRange.length > 0 else { return }

        // Reset everything to default font and color
        textStorage.addAttributes([
            .font: theme.font,
            .foregroundColor: theme.defaultText,
        ], range: fullRange)

        let nsString = text as NSString
        var lineStart = 0

        while lineStart < nsString.length {
            var lineEnd = 0
            var contentsEnd = 0
            nsString.getLineStart(
                nil, end: &lineEnd, contentsEnd: &contentsEnd,
                for: NSRange(location: lineStart, length: 0)
            )

            let lineRange = NSRange(location: lineStart, length: contentsEnd - lineStart)
            let lineStr = nsString.substring(with: lineRange)
            let trimmed = lineStr.trimmingCharacters(in: .whitespaces)

            if lineStart == 0 && trimmed.hasPrefix("RISE ASCII SCENE") {
                // File header line
                textStorage.addAttributes([
                    .foregroundColor: theme.fileHeader,
                    .font: theme.boldFont,
                ], range: lineRange)

            } else if trimmed.hasPrefix("#") {
                // Comment — color the entire line and skip sub-line patterns
                textStorage.addAttribute(
                    .foregroundColor, value: theme.comment, range: lineRange
                )

            } else if trimmed.hasPrefix(">") {
                // Command directive
                textStorage.addAttribute(
                    .foregroundColor, value: theme.command, range: lineRange
                )

            } else if trimmed.hasPrefix("DEFINE ") || trimmed.hasPrefix("define ")
                        || trimmed.hasPrefix("!") {
                // DEFINE / ! directive
                textStorage.addAttribute(
                    .foregroundColor, value: theme.preprocessor, range: lineRange
                )

            } else if trimmed.hasPrefix("UNDEF ") || trimmed.hasPrefix("undef ")
                        || trimmed.hasPrefix("~") {
                // UNDEF / ~ directive
                textStorage.addAttribute(
                    .foregroundColor, value: theme.preprocessor, range: lineRange
                )

            } else if trimmed.hasPrefix("FOR ") || trimmed == "ENDFOR"
                        || trimmed.hasPrefix("ENDFOR") {
                // Loop directive
                textStorage.addAttributes([
                    .foregroundColor: theme.loopDirective,
                    .font: theme.boldFont,
                ], range: lineRange)

            } else if Self.blockKeywords.contains(trimmed) {
                // Block type keyword (appears alone on a line)
                textStorage.addAttributes([
                    .foregroundColor: theme.blockKeyword,
                    .font: theme.boldFont,
                ], range: lineRange)

            } else {
                // Regular line — apply sub-line highlighting for property keys,
                // braces, macros, math expressions, and numbers.
                highlightLineContents(
                    nsString: nsString, lineRange: lineRange,
                    lineStr: lineStr, storage: textStorage
                )
            }

            lineStart = lineEnd
        }
    }

    // MARK: - Sub-line Highlighting

    /// Applies fine-grained highlighting to a non-classified line (typically
    /// a property line inside a block, a bare brace, or a blank line).
    private func highlightLineContents(
        nsString: NSString, lineRange: NSRange,
        lineStr: String, storage: NSTextStorage
    ) {
        let lineNSRange = NSRange(location: 0, length: lineStr.count)

        // Property key: first word on a tab-indented line
        let propertyMatches = Self.propertyKeyRegex.matches(
            in: lineStr, range: lineNSRange
        )
        if let match = propertyMatches.first, match.numberOfRanges >= 3 {
            let keyLocalRange = match.range(at: 2)
            let keyRange = NSRange(
                location: lineRange.location + keyLocalRange.location,
                length: keyLocalRange.length
            )
            storage.addAttribute(.foregroundColor, value: theme.propertyKey, range: keyRange)
        }

        // Braces
        applyRegex(
            Self.bracesRegex, color: theme.braces,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Macro references (@NAME) — applied before numbers so @I doesn't get
        // partially colored if adjacent to digits.
        applyRegex(
            Self.macroRefRegex, color: theme.macroRef,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Math expressions $(...)
        applyRegex(
            Self.mathExprRegex, color: theme.mathExpr,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Numeric literals
        applyRegex(
            Self.numberRegex, color: theme.number,
            in: nsString, searchRange: lineRange, storage: storage
        )
    }

    // MARK: - Helpers

    private func applyRegex(
        _ regex: NSRegularExpression, color: NSColor,
        in nsString: NSString, searchRange: NSRange, storage: NSTextStorage
    ) {
        let matches = regex.matches(
            in: nsString as String, range: searchRange
        )
        for match in matches {
            storage.addAttribute(.foregroundColor, value: color, range: match.range)
        }
    }
}
