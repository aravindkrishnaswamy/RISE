//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.swift - Tool-mode picker for the interactive
//    3D viewport, organised Photoshop-style: each toolbar slot is
//    a CATEGORY (Select / Camera / ObjectTransform), and a slot
//    button on regular click activates the category's last-used
//    sub-tool.  Long-press / right-click opens a flyout with all
//    sub-tools so the user can change the last-used pick.
//
//  Three slots:
//    1. Select          — single tool (no flyout)
//    2. Camera          — Orbit, Pan, Zoom, Roll
//    3. ObjectTransform — Translate, Rotate, Scale  (gizmo overlay
//                          renders in ViewportView when active)
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

    /// Category the tool belongs to in the Photoshop-style slot
    /// model.  Mirrors RISE::SceneEditController::CategoryForTool.
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

/// Photoshop-style toolbar slot.  Mirrors
/// `RISE::SceneEditController::ToolCategory` / `RISEViewportToolCategory`.
enum ViewportToolCategory: Int, CaseIterable, Identifiable {
    case select          = 0
    case camera          = 1
    case objectTransform = 2

    var id: Int { rawValue }

    /// Sub-tools surfaced in the category's flyout.  Order matters —
    /// shown top-to-bottom in the menu.
    var subTools: [ViewportTool] {
        switch self {
        case .select:          return [.select]
        case .camera:          return [.orbitCamera, .panCamera, .zoomCamera, .rollCamera]
        case .objectTransform: return [.translateObject, .rotateObject, .scaleObject]
        }
    }

    /// Slot tooltip describing what the category covers.
    var tooltip: String {
        switch self {
        case .select:
            return "Select — click an object in the viewport to make it the next edit's target"
        case .camera:
            return "Camera — orbit, pan, zoom, or roll the camera (right-click to switch sub-tool)"
        case .objectTransform:
            return "Transform — translate, rotate, or scale the selected object via the gizmo (right-click to switch sub-tool)"
        }
    }

    var bridgeValue: RISEViewportToolCategory {
        return RISEViewportToolCategory(rawValue: rawValue) ?? .select
    }
}

struct ViewportToolbar: View {
    @Binding var selectedTool: ViewportTool
    /// Per-category last-used sub-tool memory.  Driven by the C++
    /// controller (so it stays consistent across cmd-Z and re-renders);
    /// the SwiftUI parent passes a closure that reads from the bridge.
    /// Defaults to category-defaults if no bridge is attached yet.
    var lastSubToolForCategory: (ViewportToolCategory) -> ViewportTool = { cat in
        cat.subTools.first ?? .select
    }
    var onUndo: () -> Void = {}
    var onRedo: () -> Void = {}

    var body: some View {
        HStack(spacing: 4) {
            ForEach(ViewportToolCategory.allCases) { category in
                CategorySlot(
                    category: category,
                    selectedTool: $selectedTool,
                    lastSubToolForCategory: lastSubToolForCategory
                )
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

/// One Photoshop-style toolbar slot: a button showing the category's
/// last-used sub-tool icon, with a context-menu / long-press flyout
/// to switch the active sub-tool.  Clicking the button activates the
/// shown sub-tool; right-clicking (or long-press on a trackpad)
/// opens the flyout so the user can pick a different one.
private struct CategorySlot: View {
    let category: ViewportToolCategory
    @Binding var selectedTool: ViewportTool
    let lastSubToolForCategory: (ViewportToolCategory) -> ViewportTool

    var body: some View {
        let tools = category.subTools
        let showFlyout = tools.count > 1
        // The tool icon to show on the slot button: if the current
        // selectedTool belongs to this category, use it (so the slot
        // always reflects what's active).  Otherwise the category's
        // last-used (via the bridge) — or its first sub-tool as a
        // fallback when the bridge has no memory yet.
        let shownTool: ViewportTool = {
            if selectedTool.category == category { return selectedTool }
            return lastSubToolForCategory(category)
        }()
        let isSelected = (selectedTool.category == category)

        if showFlyout {
            // Menu with a primary tap action: clicking the body picks
            // `shownTool`, while the trailing-chevron / right-click
            // / long-press opens the sub-tool list.
            Menu {
                ForEach(tools) { sub in
                    Button {
                        selectedTool = sub
                    } label: {
                        Label(sub.label, systemImage: sub.iconName)
                    }
                }
            } label: {
                SlotIcon(tool: shownTool, isSelected: isSelected, hasFlyout: true)
            } primaryAction: {
                selectedTool = shownTool
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .help(category.tooltip)
        } else {
            // Single-tool slot (Select).  No flyout.
            Button {
                selectedTool = shownTool
            } label: {
                SlotIcon(tool: shownTool, isSelected: isSelected, hasFlyout: false)
            }
            .buttonStyle(.borderless)
            .help(category.tooltip)
        }
    }
}

private struct SlotIcon: View {
    let tool: ViewportTool
    let isSelected: Bool
    let hasFlyout: Bool

    var body: some View {
        ZStack(alignment: .bottomTrailing) {
            Image(systemName: tool.iconName)
                .font(.system(size: 14, weight: isSelected ? .semibold : .regular))
                .foregroundColor(isSelected ? .white : .primary)
                .frame(width: 28, height: 28)
                .background(
                    isSelected ? Color.accentColor : Color.clear,
                    in: RoundedRectangle(cornerRadius: 5)
                )

            if hasFlyout {
                // Tiny chevron in the bottom-right corner so the
                // affordance for "more sub-tools available" is
                // visible (matches Adobe Photoshop's small triangle
                // on multi-tool slots).
                Image(systemName: "triangle.fill")
                    .font(.system(size: 5))
                    .rotationEffect(.degrees(180))
                    .foregroundColor(isSelected ? .white : .secondary)
                    .padding(2)
            }
        }
    }
}
