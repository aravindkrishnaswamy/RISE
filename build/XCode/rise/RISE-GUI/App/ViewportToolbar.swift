//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.swift - Tool-mode picker for the interactive
//    3D viewport.  Numeric values mirror SceneEditController::Tool
//    so the controller still understands the full enum, but the
//    toolbar only surfaces the tools that are wired up end-to-end:
//    Select + the three camera manipulators.  Object Translate /
//    Rotate / Scale and the standalone Scrub tool are intentionally
//    omitted — object editing is too much complexity for the current
//    state of the app, and timeline scrubbing is driven directly by
//    the bottom timeline bar.
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

    /// Tools surfaced in the toolbar.  Anything not in this list is
    /// available in the C++ enum but not exposed through the UI.
    static let visibleInToolbar: [ViewportTool] = [
        .select, .orbitCamera, .panCamera, .zoomCamera, .rollCamera
    ]

    /// SF Symbol name for the toolbar button icon.
    var iconName: String {
        switch self {
        case .select:          return "cursorarrow"
        case .translateObject: return "arrow.up.and.down.and.arrow.left.and.right"
        case .rotateObject:    return "arrow.triangle.2.circlepath"
        case .scaleObject:     return "arrow.up.left.and.arrow.down.right"
        case .orbitCamera:     return "rotate.3d"
        case .panCamera:       return "hand.draw"
        case .zoomCamera:      return "plus.magnifyingglass"
        case .scrubTimeline:   return "timeline.selection"
        case .rollCamera:      return "arrow.clockwise.circle"
        }
    }

    var label: String {
        switch self {
        case .select:          return "Select"
        case .translateObject: return "Translate"
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

    /// Descriptive tooltip — what the tool does when active.
    var tooltip: String {
        switch self {
        case .select:
            return "Select — click an object in the viewport to make it the target of the next edit"
        case .translateObject:
            return "Translate — drag the selected object to move it through the scene"
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

struct ViewportToolbar: View {
    @Binding var selectedTool: ViewportTool
    var onUndo: () -> Void = {}
    var onRedo: () -> Void = {}

    var body: some View {
        HStack(spacing: 4) {
            ForEach(ViewportTool.visibleInToolbar) { tool in
                let isSelected = (selectedTool == tool)
                Button {
                    selectedTool = tool
                } label: {
                    Image(systemName: tool.iconName)
                        .font(.system(size: 14, weight: isSelected ? .semibold : .regular))
                        .foregroundColor(isSelected ? .white : .primary)
                        .frame(width: 28, height: 28)
                }
                .buttonStyle(.borderless)
                .background(
                    isSelected ? Color.accentColor : Color.clear,
                    in: RoundedRectangle(cornerRadius: 5)
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 5)
                        .stroke(isSelected ? Color.accentColor : Color.clear, lineWidth: 1)
                )
                .help(tool.tooltip)
            }

            Divider()
                .frame(height: 20)
                .padding(.horizontal, 4)

            Button(action: onUndo) {
                Image(systemName: "arrow.uturn.backward").frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
            .help("Undo — revert the last edit (per-drag composites are one entry)")

            Button(action: onRedo) {
                Image(systemName: "arrow.uturn.forward").frame(width: 28, height: 28)
            }
            .buttonStyle(.borderless)
            .help("Redo — re-apply the most recently undone edit")
        }
        .padding(6)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
}
