//////////////////////////////////////////////////////////////////////
//
//  RefinementStatusFormatter.swift - Shared "refinement status" text
//    mapping: (phase, scaleDivisor, renderState) -> display strings +
//    a progress-bar fraction.
//
//  Both readouts consume this one implementation: TopBar.swift's
//  `renderStatusCluster` (via its `status` computed property) and
//  ViewportView's bottom-left refinement pill — so the two can never
//  disagree at a glance.  Add any new refinement-status surface here,
//  not as a hand-copy.
//
//////////////////////////////////////////////////////////////////////

import Foundation
import CoreGraphics

enum RefinementStatusFormatter {
    struct Status {
        /// Primary readout text — "Refining · rung 3/6", "Paused",
        /// "Polishing", "Settled", "Production 42%", "Cancelling…".
        let text: String
        /// Small tracked-letter-spacing label shown beside the text.
        let label: String
        /// 0...1 — how full the mini progress bar should draw.
        let fraction: CGFloat
    }

    /// Refinement ladder rung, 1...6, derived from the preview-scale
    /// divisor (a power of two, 1...32 — 1 = full res = rung 6).
    /// Mirrors TopBar's private `rung` computed property.
    static func rung(scaleDivisor: UInt32) -> Int {
        let d = max(1, scaleDivisor)
        let log2d = Int(log2(Double(d)).rounded())
        return max(1, min(6, 6 - log2d))
    }

    /// `phase`: -1 no controller, 0 Idle/Settled, 1 Rendering (ladder),
    /// 2 Refining, 3 Polishing, 4 Paused — mirrors
    /// `RISEViewportBridge.refinementPhase(scaleDivisor:)`.
    static func status(phase: Int,
                        scaleDivisor: UInt32,
                        isProduction: Bool,
                        isCancelling: Bool,
                        productionProgress: Double,
                        isProductionPaused: Bool = false) -> Status {
        if isCancelling {
            return Status(text: "Cancelling…", label: "CANCELLING",
                          fraction: CGFloat(productionProgress))
        }
        if isProduction {
            let pct = Int(productionProgress * 100)
            if isProductionPaused {
                // Item 4: workers parked at the bridge's pause gate —
                // progress honestly frozen at the pause point.
                return Status(text: "Paused \(pct)%", label: "PAUSED — PRODUCTION",
                              fraction: CGFloat(productionProgress))
            }
            return Status(text: "Production \(pct)%", label: "PRODUCTION \(pct)%",
                          fraction: CGFloat(productionProgress))
        }
        let r = rung(scaleDivisor: scaleDivisor)
        switch phase {
        case 4: return Status(text: "Paused", label: "PAUSED", fraction: CGFloat(r) / 6.0)
        case 2: return Status(text: "Refining · rung \(r)/6", label: "REFINING", fraction: CGFloat(r) / 6.0)
        case 1: return Status(text: "Rendering", label: "REFINING", fraction: CGFloat(r) / 6.0)
        case 3: return Status(text: "Polishing", label: "DENOISED — NOT FINAL", fraction: 1.0)
        case 0: return Status(text: "Settled", label: "SETTLED", fraction: 1.0)
        default: return Status(text: "—", label: "—", fraction: 0)
        }
    }
}
