//////////////////////////////////////////////////////////////////////
//
//  RISESyntaxHighlighter.cpp - Syntax highlighting implementation.
//
//  Ported from the Mac app's RISESceneSyntaxHighlighter.swift.
//  Single-pass line-by-line classification with sub-line regex.
//
//////////////////////////////////////////////////////////////////////

#include "RISESyntaxHighlighter.h"

#include <QFont>
#include <QFontDatabase>

// Static regex patterns (matching Mac app)
const QRegularExpression RISESyntaxHighlighter::s_bracesRegex(QStringLiteral("[{}]"));
const QRegularExpression RISESyntaxHighlighter::s_macroRefRegex(QStringLiteral("@[A-Za-z_]\\w*"));
const QRegularExpression RISESyntaxHighlighter::s_mathExprRegex(QStringLiteral("\\$\\([^)]*\\)"));
const QRegularExpression RISESyntaxHighlighter::s_numberRegex(QStringLiteral("(?<=\\s)-?(?:\\d+\\.?\\d*|\\.\\d+)(?=\\s|$)"));
const QRegularExpression RISESyntaxHighlighter::s_propertyKeyRegex(QStringLiteral("^(\\t+)(\\w+)"));

// 126 block keywords from AsciiSceneParser.cpp::ParseAndLoadScene (matching Mac app).
// Keep synchronized with RISESceneSyntaxHighlighter.swift.
const QSet<QString> RISESyntaxHighlighter::s_blockKeywords = {
    // Painters (27)
    "uniformcolor_painter", "spectral_painter", "png_painter", "hdr_painter",
    "exr_painter", "tiff_painter", "checker_painter", "lines_painter",
    "mandelbrot_painter", "perlin2d_painter", "gerstnerwave_painter",
    "perlin3d_painter", "turbulence3d_painter", "wavelet3d_painter",
    "reactiondiffusion3d_painter", "gabor3d_painter", "simplex3d_painter",
    "sdf3d_painter", "curlnoise3d_painter", "domainwarp3d_painter",
    "perlinworley3d_painter", "worley3d_painter",
    "voronoi2d_painter", "voronoi3d_painter", "iridescent_painter",
    "blackbody_painter", "blend_painter",
    // Functions (2)
    "piecewise_linear_function", "piecewise_linear_function2d",
    // Materials (23)
    "lambertian_material", "perfectreflector_material", "perfectrefractor_material",
    "polished_material", "dielectric_material",
    "subsurfacescattering_material", "randomwalk_sss_material",
    "lambertian_luminaire_material", "phong_luminaire_material",
    "ashikminshirley_anisotropicphong_material", "isotropic_phong_material",
    "translucent_material", "biospec_skin_material",
    "donner_jensen_skin_bssrdf_material", "generic_human_tissue_material",
    "composite_material", "ward_isotropic_material", "ward_anisotropic_material",
    "ggx_material", "cooktorrance_material", "orennayar_material",
    "schlick_material", "datadriven_material",
    // Cameras (6)
    "pinhole_camera", "onb_pinhole_camera", "thinlens_camera",
    "realistic_camera", "fisheye_camera", "orthographic_camera",
    // Geometry (15)
    "sphere_geometry", "ellipsoid_geometry", "cylinder_geometry",
    "torus_geometry", "infiniteplane_geometry", "box_geometry",
    "clippedplane_geometry", "3dsmesh_geometry", "rawmesh_geometry",
    "rawmesh2_geometry", "risemesh_geometry", "circulardisk_geometry",
    "bezierpatch_geometry", "bilinearpatch_geometry", "displaced_geometry",
    // Modifiers (1)
    "bumpmap_modifier",
    // Media (3)
    "homogeneous_medium", "heterogeneous_medium", "painter_heterogeneous_medium",
    // Objects (2)
    "standard_object", "csg_object",
    // Shader operations (12; mis_pathtracing_shaderop is a legacy alias)
    "ambientocclusion_shaderop", "directlighting_shaderop",
    "pathtracing_shaderop", "mis_pathtracing_shaderop",
    "sms_shaderop", "distributiontracing_shaderop",
    "finalgather_shaderop", "simple_sss_shaderop",
    "diffusion_approximation_sss_shaderop", "donner_jensen_skin_sss_shaderop",
    "arealight_shaderop", "transparency_shaderop",
    // Shaders (4)
    "standard_shader", "advanced_shader",
    "directvolumerendering_shader", "spectraldirectvolumerendering_shader",
    // Rasterizers (10)
    "pixelpel_rasterizer", "pixelintegratingspectral_rasterizer",
    "bdpt_pel_rasterizer", "bdpt_spectral_rasterizer",
    "vcm_pel_rasterizer", "vcm_spectral_rasterizer",
    "pathtracing_pel_rasterizer", "pathtracing_spectral_rasterizer",
    "mlt_rasterizer", "mlt_spectral_rasterizer",
    // Output (1)
    "file_rasterizeroutput",
    // Lights (4)
    "ambient_light", "omni_light", "spot_light", "directional_light",
    // Photon maps & gather (12)
    "caustic_pel_photonmap", "translucent_pel_photonmap",
    "caustic_spectral_photonmap", "global_pel_photonmap",
    "global_spectral_photonmap", "shadow_photonmap",
    "caustic_pel_gather", "translucent_pel_gather",
    "caustic_spectral_gather", "global_pel_gather",
    "global_spectral_gather", "shadow_gather",
    // Other (4)
    "irradiance_cache", "keyframe", "timeline", "animation_options",
};

RISESyntaxHighlighter::RISESyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(12);
    QFont boldFont = monoFont;
    boldFont.setBold(true);

    // Comment: green
    m_commentFmt.setForeground(QColor(0, 160, 0));
    m_commentFmt.setFont(monoFont);

    // File header: purple bold
    m_fileHeaderFmt.setForeground(QColor(128, 0, 128));
    m_fileHeaderFmt.setFont(boldFont);

    // Block keyword: blue bold
    m_blockKeywordFmt.setForeground(QColor(0, 80, 255));
    m_blockKeywordFmt.setFont(boldFont);

    // Property key: indigo
    m_propertyKeyFmt.setForeground(QColor(75, 0, 130));
    m_propertyKeyFmt.setFont(monoFont);

    // Command: teal
    m_commandFmt.setForeground(QColor(0, 128, 128));
    m_commandFmt.setFont(monoFont);

    // Preprocessor: orange
    m_preprocessorFmt.setForeground(QColor(230, 140, 0));
    m_preprocessorFmt.setFont(monoFont);

    // Loop directive: orange bold
    m_loopDirectiveFmt.setForeground(QColor(230, 140, 0));
    m_loopDirectiveFmt.setFont(boldFont);

    // Macro reference: red
    m_macroRefFmt.setForeground(QColor(220, 30, 30));
    m_macroRefFmt.setFont(monoFont);

    // Math expression: pink
    m_mathExprFmt.setForeground(QColor(255, 105, 180));
    m_mathExprFmt.setFont(monoFont);

    // Number: cyan
    m_numberFmt.setForeground(QColor(0, 170, 190));
    m_numberFmt.setFont(monoFont);

    // Braces: gray
    m_bracesFmt.setForeground(QColor(128, 128, 128));
    m_bracesFmt.setFont(monoFont);
}

void RISESyntaxHighlighter::highlightBlock(const QString& text)
{
    if (text.isEmpty()) return;

    QString trimmed = text.trimmed();

    // 1. File header (first line containing "RISE ASCII SCENE")
    if (currentBlock().blockNumber() == 0 && trimmed.startsWith("RISE ASCII SCENE")) {
        setFormat(0, text.length(), m_fileHeaderFmt);
        return;
    }

    // 2. Comment
    if (trimmed.startsWith('#')) {
        setFormat(0, text.length(), m_commentFmt);
        return;
    }

    // 3. Command directive
    if (trimmed.startsWith('>')) {
        setFormat(0, text.length(), m_commandFmt);
        return;
    }

    // 4. DEFINE / ! preprocessor
    if (trimmed.startsWith("DEFINE ") || trimmed.startsWith("define ") || trimmed.startsWith('!')) {
        setFormat(0, text.length(), m_preprocessorFmt);
        return;
    }

    // 5. UNDEF / ~ preprocessor
    if (trimmed.startsWith("UNDEF ") || trimmed.startsWith("undef ") || trimmed.startsWith('~')) {
        setFormat(0, text.length(), m_preprocessorFmt);
        return;
    }

    // 6. Loop directive (FOR / ENDFOR)
    if (trimmed.startsWith("FOR ") || trimmed == "ENDFOR" || trimmed.startsWith("ENDFOR")) {
        setFormat(0, text.length(), m_loopDirectiveFmt);
        return;
    }

    // 7. Block keyword (exact match on trimmed line)
    if (s_blockKeywords.contains(trimmed)) {
        setFormat(0, text.length(), m_blockKeywordFmt);
        return;
    }

    // 8. Regular line — apply sub-line patterns
    highlightLineContents(text);
}

void RISESyntaxHighlighter::highlightLineContents(const QString& text)
{
    // Property key: first word on tab-indented line
    QRegularExpressionMatch propMatch = s_propertyKeyRegex.match(text);
    if (propMatch.hasMatch() && propMatch.lastCapturedIndex() >= 2) {
        int start = propMatch.capturedStart(2);
        int length = propMatch.capturedLength(2);
        setFormat(start, length, m_propertyKeyFmt);
    }

    // Braces
    QRegularExpressionMatchIterator it = s_bracesRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_bracesFmt);
    }

    // Macro references (@NAME)
    it = s_macroRefRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_macroRefFmt);
    }

    // Math expressions $(...)
    it = s_mathExprRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_mathExprFmt);
    }

    // Numbers
    it = s_numberRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_numberFmt);
    }
}
