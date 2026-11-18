import Foundation
import AVFoundation
import CoreImage
import ImageIO
import UniformTypeIdentifiers

let width = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2])! : 512
let height = CommandLine.arguments.count > 3 ? Int(CommandLine.arguments[3])! : 768
let asset=AVURLAsset(url: URL(fileURLWithPath: CommandLine.arguments[1]))
let tracks=try await asset.loadTracks(withMediaType: .video)
guard tracks.count==1 else { fatalError("track count") }
let rate=try await tracks[0].load(.nominalFrameRate)
let duration=try await asset.load(.duration)
let reader=try AVAssetReader(asset: asset)
let output=AVAssetReaderTrackOutput(track: tracks[0], outputSettings: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
let provider = reader.outputProvider(for: output)
try reader.start()
var count=0
while let ready = try await provider.next() {
    ready.withUnsafeSampleBuffer { sample in
    guard let dimensions = CMSampleBufferGetImageBuffer(sample),
          CVPixelBufferGetWidth(dimensions)==width,
          CVPixelBufferGetHeight(dimensions)==height else { fatalError("decoded dimensions") }
    let time=CMSampleBufferGetPresentationTimeStamp(sample)
    guard CMTimeCompare(time,CMTime(value: Int64(count),timescale: 16))==0 else {
        print("wrong time frame=\(count) value=\(time.value) timescale=\(time.timescale)")
        exit(2)
    }
    if count==117 {
        guard let pixel=CMSampleBufferGetImageBuffer(sample) else { fatalError("no image") }
        let image=CIImage(cvPixelBuffer: pixel)
        guard let cg=CIContext().createCGImage(image, from: image.extent),
              let dest=CGImageDestinationCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[1]).deletingLastPathComponent().appendingPathComponent("decoded-frame117.png") as CFURL,
                  UTType.png.identifier as CFString,1,nil) else { fatalError("snapshot") }
        CGImageDestinationAddImage(dest,cg,nil)
        guard CGImageDestinationFinalize(dest) else { fatalError("write snapshot") }
    }
    count+=1
    }
}
print("measured count=\(count) status=\(reader.status.rawValue) rate=\(rate) duration=\(duration.value)/\(duration.timescale)")
guard reader.status == .completed, count==235, rate==16,
      CMTimeCompare(duration,CMTime(value:235,timescale:16))==0 else { exit(3) }
print("PASS decoded_frames=235 exact_PTS_n_over_16=1 nominal_fps=16 duration_s=14.6875")
