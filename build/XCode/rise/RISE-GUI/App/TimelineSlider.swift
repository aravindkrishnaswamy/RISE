//////////////////////////////////////////////////////////////////////
//
//  TimelineSlider.swift - Bottom-of-window time scrubber for the
//    interactive viewport.  Visible only when the loaded scene
//    has any keyframed elements.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI

struct TimelineSlider: View {
    @Binding var time: Double
    let range: ClosedRange<Double>
    var onScrubBegin: () -> Void = {}
    var onScrubEnd: () -> Void = {}

    @State private var isScrubbing = false

    var body: some View {
        HStack(spacing: 8) {
            Text(String(format: "%.2fs", time))
                .font(.caption.monospacedDigit())
                .frame(width: 56, alignment: .trailing)
                .foregroundStyle(.secondary)

            Slider(value: $time,
                   in: range,
                   onEditingChanged: { editing in
                       if editing && !isScrubbing {
                           isScrubbing = true
                           onScrubBegin()
                       } else if !editing && isScrubbing {
                           isScrubbing = false
                           onScrubEnd()
                       }
                   })

            Text(String(format: "%.2fs", range.upperBound))
                .font(.caption.monospacedDigit())
                .frame(width: 56, alignment: .leading)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 6))
    }
}
