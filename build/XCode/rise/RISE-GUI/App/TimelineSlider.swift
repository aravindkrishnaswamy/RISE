//////////////////////////////////////////////////////////////////////
//
//  TimelineSlider.swift - Bottom-of-window time scrubber for the
//    interactive viewport.  Visible whenever the live scene has
//    keyframed elements, including timelines added after load.
//
//  UI redesign center-column slice: restyled to the design comp's
//  transport row (⏮ / play-pause / ⏭ + "MM:SS / MM:SS" readout + a
//  custom trough/fill/playhead scrub track) with a "Render movie…"
//  chip on the trailing edge.  The scrub-begin/move/end CALLBACK
//  CONTRACT is unchanged from the pre-redesign `Slider`-based
//  implementation — `onUserScrubBegan` + `onScrubBegin` still fire
//  once at drag start, `time` still updates continuously during the
//  drag for UI display while `onScrubMove` forwards the native move
//  synchronously between those callbacks, and `onScrubEnd` still fires
//  once at drag end — only the widget
//  producing those calls changed (a `DragGesture` over a custom
//  track instead of `Slider`'s built-in gesture), because `Slider`'s
//  native AppKit thumb can't be reskinned to the comp's thin
//  trough + 2×14 playhead look.
//
//////////////////////////////////////////////////////////////////////

import SwiftUI

struct TimelineSlider: View {
    @EnvironmentObject var viewModel: RenderViewModel

    @Binding var time: Double
    let range: ClosedRange<Double>
    var isPlaying: Bool = false
    var onPlayToggle: () -> Void = {}
    var onUserScrubBegan: () -> Void = {}
    var onJump: (Double) -> Void = { _ in }
    var onScrubBegin: () -> Void = {}
    var onScrubMove: (Double) -> Void = { _ in }
    var onScrubEnd: () -> Void = {}

    @State private var isScrubbing = false

    var body: some View {
        HStack(spacing: 12) {
            transport

            Text("\(timeString(time)) / \(timeString(range.upperBound))")
                .font(Theme.mono(11))
                .foregroundColor(Theme.textSecondary)

            scrubTrack

            if viewModel.hasAnimation {
                renderMovieChip
            }
        }
        .padding(.horizontal, 12)
        .frame(height: 58)
        .background(Theme.bgTimeline)
        .overlay(alignment: .top) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
        }
    }

    // MARK: - Transport

    private var transport: some View {
        HStack(spacing: 7) {
            Button {
                jumpTo(range.lowerBound)
            } label: {
                Text("⏮")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textFaint)
            }
            .buttonStyle(.plain)
            .help("Rewind to start")

            Button(action: onPlayToggle) {
                Image(systemName: isPlaying ? "stop.fill" : "play.fill")
                    .font(.system(size: 10))
                    .foregroundColor(Theme.textPrimary)
                    .frame(width: 24, height: 24)
                    .background(Theme.fillActive, in: RoundedRectangle(cornerRadius: 6))
            }
            .buttonStyle(.plain)
            .help(isPlaying ? "Stop preview playback"
                            : "Play the active animation in the preview (loops until stopped)")

            Button {
                jumpTo(range.upperBound)
            } label: {
                Text("⏭")
                    .font(.system(size: 11))
                    .foregroundColor(Theme.textFaint)
            }
            .buttonStyle(.plain)
            .help("Jump to end")
        }
    }

    // MARK: - Scrub track

    private var scrubTrack: some View {
        GeometryReader { geom in
            let w = max(1, geom.size.width)
            let span = max(range.upperBound - range.lowerBound, 0.0001)
            let frac = CGFloat((time - range.lowerBound) / span)
            let fillWidth = max(0, min(w, w * frac))

            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 1)
                    .fill(Theme.fillTrough)
                    .frame(height: 2)
                RoundedRectangle(cornerRadius: 1)
                    .fill(Theme.accent)
                    .frame(width: fillWidth, height: 2)
                RoundedRectangle(cornerRadius: 1)
                    .fill(Color.white)
                    .frame(width: 2, height: 14)
                    .offset(x: max(0, min(w - 2, fillWidth - 1)))
            }
            .frame(maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        if !isScrubbing {
                            isScrubbing = true
                            onUserScrubBegan()
                            onScrubBegin()
                        }
                        let x = max(0, min(w, value.location.x))
                        let newTime = range.lowerBound + Double(x / w) * span
                        time = newTime
                        onScrubMove(newTime)
                    }
                    .onEnded { _ in
                        isScrubbing = false
                        onScrubEnd()
                    }
            )
        }
        .frame(height: 14)
    }

    // MARK: - Render movie chip

    private var renderMovieChip: some View {
        Button {
            viewModel.startAnimationRender()
        } label: {
            Text("Render movie…")
                .font(Theme.sans(11))
                .foregroundColor(Theme.textTertiary)
                .padding(.horizontal, 11)
                .padding(.vertical, 5)
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.borderLight, lineWidth: 1))
        }
        .buttonStyle(.plain)
        // Same gate as the Render menu's "Render Animation…" item
        // (RISEApp.swift) — `viewModel.canStartProductionRender`.
        .disabled(!viewModel.canStartProductionRender)
        .opacity(viewModel.canStartProductionRender ? 1.0 : 0.4)
        .help(viewModel.canStartProductionRender
              ? "Render the active animation to an HDR movie"
              : "Unavailable while a render is in flight")
    }

    // MARK: - Helpers

    /// Jumps to an instant time (rewind / to-end) through a direct callback
    /// whose owner performs the native begin/move/end synchronously.  A
    /// binding mutation here would be observed only after this function
    /// returned, too late to sit between Begin and End.
    private func jumpTo(_ t: Double) {
        onUserScrubBegan()
        onJump(t)
    }

    private func timeString(_ t: Double) -> String {
        let clamped = max(0, t)
        let m = Int(clamped) / 60
        let s = Int(clamped) % 60
        return String(format: "%02d:%02d", m, s)
    }
}
