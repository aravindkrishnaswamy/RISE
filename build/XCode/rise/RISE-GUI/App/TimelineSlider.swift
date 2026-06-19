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
    var isPlaying: Bool = false
    var onPlayToggle: () -> Void = {}
    var onUserScrubBegan: () -> Void = {}
    var onScrubBegin: () -> Void = {}
    var onScrubEnd: () -> Void = {}

    @State private var isScrubbing = false

    var body: some View {
        HStack(spacing: 8) {
            Button {
                onPlayToggle()
            } label: {
                Image(systemName: isPlaying ? "stop.fill" : "play.fill")
                    .frame(width: 16)
            }
            .buttonStyle(.borderless)
            .help(isPlaying ? "Stop preview playback"
                            : "Play the active animation in the preview (loops until stopped)")

            Text(String(format: "%.2fs", time))
                .font(.caption.monospacedDigit())
                .frame(width: 56, alignment: .trailing)
                .foregroundStyle(.secondary)

            Slider(value: $time,
                   in: range,
                   onEditingChanged: { editing in
                       if editing && !isScrubbing {
                           isScrubbing = true
                           onUserScrubBegan()   // a manual drag interrupts preview play
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
