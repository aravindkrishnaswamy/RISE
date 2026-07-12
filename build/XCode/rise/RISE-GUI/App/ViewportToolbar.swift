//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.swift - Tool-mode picker for the interactive
//    3D viewport.
//
//  UI-redesign discoverability pass: the previous Photoshop-style
//  design put a single button per CATEGORY (Select / Camera /
//  ObjectTransform), with the camera/transform sub-tools (orbit, pan,
//  zoom, roll; translate, rotate, scale) hidden behind a right-click
//  flyout.  User testing / bug report: nobody discovered the flyout,
//  so heavily-used sub-tools were effectively invisible.  All tools
//  are now individual, always-visible, labeled buttons grouped into
//  three visually separated clusters:
//
//    Select  — Select
//    Camera  — Orbit, Pan, Zoom, Roll
//    Object  — Move, Rotate, Scale   (gizmo overlay renders in
//               ViewportView when an Object tool is active)
//
//  Numeric values mirror SceneEditController::Tool /
//  SceneEditController::ToolCategory so the controller still
//  understands the full enum.  ScrubTimeline lives in the bottom
//  timeline bar, not the main toolbar.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI
import AppKit

/// Swift mirror of RISEViewportTool.  Identifiable for ForEach.
enum ViewportTool: Int, CaseIterable, Identifiable {
    case select          = 0
    case translateObject = 1
    case rotateObject    = 2
    case scaleObject     = 3
    case orbitCamera     = 4
    case panCamera       = 5
    case zoomCamera      = 6
    case scrubTimeline   = 7
    case rollCamera      = 8

    var id: Int { rawValue }

    /// Category the tool belongs to in the toolbar's grouping model.
    /// Mirrors RISE::SceneEditController::CategoryForTool.
    var category: ViewportToolCategory {
        switch self {
        case .select:           return .select
        case .translateObject:  return .objectTransform
        case .rotateObject:     return .objectTransform
        case .scaleObject:      return .objectTransform
        case .orbitCamera:      return .camera
        case .panCamera:        return .camera
        case .zoomCamera:       return .camera
        case .rollCamera:       return .camera
        case .scrubTimeline:    return .select  // timeline lives below the toolbar
        }
    }

    /// SF Symbol name for the toolbar button icon.  Force monochrome
    /// rendering at the call site (see `ToolButton` below) — macOS
    /// 26's SF Symbols catalog renders several of these in multicolor
    /// mode by default, which on the toolbar reads as ambiguous discs
    /// instead of recognizable arrow glyphs.  Every symbol below is
    /// distinct (no icon is reused across two different tools) so the
    /// glyph alone disambiguates even before the label is read; all
    /// names verified present in the local SF Symbols catalog.
    var iconName: String {
        switch self {
        case .select:          return "cursorarrow"
        case .translateObject: return "move.3d"
        case .rotateObject:    return "rotate.3d"
        case .scaleObject:     return "scale.3d"
        case .orbitCamera:     return "arrow.trianglehead.2.counterclockwise.rotate.90"
        case .panCamera:       return "hand.draw"
        case .zoomCamera:      return "plus.magnifyingglass"
        case .scrubTimeline:   return "timeline.selection"
        case .rollCamera:      return "rotate.right"
        }
    }

    /// Short label drawn under the icon on the toolbar button.
    var label: String {
        switch self {
        case .select:          return "Select"
        case .translateObject: return "Move"
        case .rotateObject:    return "Rotate"
        case .scaleObject:     return "Scale"
        case .orbitCamera:     return "Orbit"
        case .panCamera:       return "Pan"
        case .zoomCamera:      return "Zoom"
        case .scrubTimeline:   return "Scrub"
        case .rollCamera:      return "Roll"
        }
    }

    /// NSCursor that should appear when this tool is active and the
    /// pointer is over the viewport.  Plain arrow when no special
    /// affordance maps cleanly.
    var nsCursor: NSCursor {
        switch self {
        case .select:          return .arrow
        case .translateObject: return .openHand
        case .rotateObject:    return .openHand
        case .scaleObject:     return .resizeUpDown
        case .orbitCamera:     return .openHand
        case .panCamera:       return .openHand
        case .zoomCamera:      return .resizeUpDown
        case .scrubTimeline:   return .resizeLeftRight
        case .rollCamera:      return .resizeLeftRight
        }
    }

    /// Descriptive tooltip — what the tool does when active.  Shown
    /// verbatim as each button's `.help` text; no keyboard-shortcut
    /// hint is appended because none of these tools are bound to a
    /// keyboard shortcut anywhere in the app (checked RISEApp.swift's
    /// `.keyboardShortcut` menu-command list) — a fabricated hint
    /// would mislead rather than help.
    var tooltip: String {
        switch self {
        case .select:
            return "Select — click an object in the viewport to make it the target of the next edit"
        case .translateObject:
            return "Move — drag the selected object to move it through the scene"
        case .rotateObject:
            return "Rotate — drag to rotate the selected object around its origin"
        case .scaleObject:
            return "Scale — drag up/down to scale the selected object"
        case .orbitCamera:
            return "Orbit Camera — drag to rotate the camera around the scene"
        case .panCamera:
            return "Pan Camera — drag to translate the camera in screen plane"
        case .zoomCamera:
            return "Zoom Camera — drag to dolly the camera closer or farther"
        case .scrubTimeline:
            return "Scrub Timeline — drag the timeline slider at the bottom to scrub through animation"
        case .rollCamera:
            return "Roll Camera — drag horizontally to roll the camera around the (camera→look-at) axis"
        }
    }

    var bridgeValue: RISEViewportTool {
        return RISEViewportTool(rawValue: rawValue) ?? .select
    }
}

/// Toolbar grouping.  Mirrors `RISE::SceneEditController::ToolCategory`
/// / `RISEViewportToolCategory`.
enum ViewportToolCategory: Int, CaseIterable, Identifiable {
    case select          = 0
    case camera          = 1
    case objectTransform = 2

    var id: Int { rawValue }

    /// Tools shown in this group, left to right.
    var subTools: [ViewportTool] {
        switch self {
        case .select:          return [.select]
        case .camera:          return [.orbitCamera, .panCamera, .zoomCamera, .rollCamera]
        case .objectTransform: return [.translateObject, .rotateObject, .scaleObject]
        }
    }

    var bridgeValue: RISEViewportToolCategory {
        return RISEViewportToolCategory(rawValue: rawValue) ?? .select
    }
}

/// UI redesign center-column slice: this toolbar lives INLINE in
/// ContentView's viewport toolbar row (the design comp's tool group),
/// not floating over the rendered image.  Flat `Theme.bgPanel`
/// segmented-group look that matches the row's other chips.  Undo /
/// Redo buttons were dropped from this row (the design comp's toolbar
/// has no undo/redo affordance here) — both remain reachable via the
/// Edit menu and its ⌘Z / ⇧⌘Z shortcuts, so no functionality is lost.
///
/// Discoverability redesign (see file header): every tool is now its
/// own always-visible, labeled button — no more category slot +
/// long-press/right-click flyout.  `RISEViewportBridge`'s
/// `lastSubToolForCategory:` (the "remember the last-used sub-tool
/// per category" Objective-C++ method the old flyout UI queried on
/// every render to decide which icon to show on a collapsed slot) is
/// no longer called from Swift — there is no collapsed slot left to
/// populate.  The bridge method itself is untouched and may still be
/// meaningful controller-side state; it is simply dead from the
/// Swift UI's point of view now.
struct ViewportToolbar: View {
    @Binding var selectedTool: ViewportTool

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(ViewportToolCategory.allCases.enumerated()), id: \.offset) { index, category in
                if index > 0 {
                    groupDivider
                }
                toolGroup(category.subTools)
            }
        }
        .padding(2)
        .background(Theme.bgPanel, in: RoundedRectangle(cornerRadius: Theme.radiusMedium))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                .stroke(Theme.borderHairline, lineWidth: 1)
        )
    }

    private func toolGroup(_ tools: [ViewportTool]) -> some View {
        HStack(spacing: 2) {
            ForEach(tools) { tool in
                ToolButton(
                    tool: tool,
                    isSelected: selectedTool == tool,
                    action: { selectedTool = tool }
                )
            }
        }
        .padding(.horizontal, 2)
    }

    private var groupDivider: some View {
        Rectangle()
            .fill(Theme.borderLight)
            .frame(width: 1, height: 30)
            .padding(.horizontal, 3)
    }
}

/// One always-visible tool button: icon + short label, sized well
/// past the 34×30 minimum hit area (grows to ~44×44 with the label)
/// so it reads clearly at a glance rather than requiring the user to
/// already know the glyph.  Active tool gets a filled background, a
/// white glyph/label, and a thin accent underline strip along the
/// bottom edge so "which tool is on" is unmistakable even at a
/// glance.
private struct ToolButton: View {
    let tool: ViewportTool
    let isSelected: Bool
    let action: () -> Void

    @State private var isHovering = false

    var body: some View {
        Button(action: action) {
            VStack(spacing: 3) {
                Image(systemName: tool.iconName)
                    .symbolRenderingMode(.monochrome)
                    .font(.system(size: 14, weight: isSelected ? .semibold : .regular))
                Text(tool.label)
                    .font(Theme.sans(9, isSelected ? .medium : .regular))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .foregroundColor(isSelected ? .white : Theme.textTertiary)
            .frame(minWidth: 44, minHeight: 44)
            .padding(.horizontal, 4)
            .background(
                backgroundColor,
                in: RoundedRectangle(cornerRadius: Theme.radiusSmall)
            )
            .overlay(alignment: .bottom) {
                if isSelected {
                    RoundedRectangle(cornerRadius: 1)
                        .fill(Theme.accent)
                        .frame(height: 2)
                        .padding(.horizontal, 7)
                        .padding(.bottom, 2)
                }
            }
        }
        .buttonStyle(.plain)
        .help(tool.tooltip)
        .onHover { hovering in isHovering = hovering }
    }

    private var backgroundColor: Color {
        if isSelected { return Theme.fillActive }
        if isHovering { return Theme.fillHover }
        return Color.clear
    }
}
