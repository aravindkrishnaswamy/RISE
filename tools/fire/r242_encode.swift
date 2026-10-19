import Foundation
import AVFoundation
import ImageIO
import CoreVideo

let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let output = URL(fileURLWithPath: CommandLine.arguments[2])
guard !FileManager.default.fileExists(atPath: output.path) else { fatalError("output exists") }
let frames = (0..<235).map { directory.appendingPathComponent(String(format: "fire_display_denoised%04d.png", $0)) }
let width = CommandLine.arguments.count > 3 ? Int(CommandLine.arguments[3])! : 512
let height = CommandLine.arguments.count > 4 ? Int(CommandLine.arguments[4])! : 768
let writer = try AVAssetWriter(outputURL: output, fileType: .mov)
writer.movieTimeScale = 16000
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264,
    AVVideoWidthKey: width, AVVideoHeightKey: height,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 10_000_000,
                                     AVVideoMaxKeyFrameIntervalKey: 16]
])
input.mediaTimeScale = 16000
let receiver = writer.inputPixelBufferReceiver(for: input, pixelBufferAttributes: nil)
try writer.start()
writer.startSession(atSourceTime: .zero)
for (index, url) in frames.enumerated() {
    let prepared: CVReadOnlyPixelBuffer = try autoreleasepool {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
              image.width == width, image.height == height else { fatalError("bad frame \(index)") }
        var pixel: CVPixelBuffer?
        let attributes = [kCVPixelBufferCGImageCompatibilityKey as String: true,
                          kCVPixelBufferCGBitmapContextCompatibilityKey as String: true]
        guard CVPixelBufferCreate(kCFAllocatorDefault, width, height,
              kCVPixelFormatType_32ARGB, attributes as CFDictionary, &pixel) == kCVReturnSuccess,
              let buffer = pixel else { fatalError("pixel buffer") }
        CVPixelBufferLockBaseAddress(buffer, [])
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue) else { fatalError("context") }
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        CVPixelBufferUnlockBaseAddress(buffer, [])
        return CVReadOnlyPixelBuffer(unsafeBuffer: buffer)
    }
    try await receiver.append(prepared, with: CMTime(value: Int64(index), timescale: 16))
}
writer.endSession(atSourceTime: CMTime(value: 235, timescale: 16))
receiver.finish()
await writer.finishWriting()
guard writer.status == .completed else { fatalError("finish: \(String(describing: writer.error))") }
print("encoded 235 frames at 16 fps; duration 14.6875 s including final-frame hold")
