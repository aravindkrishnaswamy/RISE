import Foundation
import AVFoundation
import ImageIO
import CoreVideo

let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let output = URL(fileURLWithPath: CommandLine.arguments[2])
guard !FileManager.default.fileExists(atPath: output.path) else { fatalError("output exists") }
let first = Int(CommandLine.arguments[3])!
let count = Int(CommandLine.arguments[4])!
guard output.lastPathComponent.contains("nonphysical_diagnostic"), first >= 0, count > 0, first+count <= 235 else { fatalError("diagnostic scope") }
let frames = (first..<first+count).map { directory.appendingPathComponent(String(format: "nonphysical_diagnostic_temperature_%04d.png", $0)) }
let writer = try AVAssetWriter(outputURL: output, fileType: .mov)
writer.movieTimeScale = 16000
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264,
    AVVideoWidthKey: 1040, AVVideoHeightKey: 760,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 10_000_000,
                                     AVVideoMaxKeyFrameIntervalKey: 16]
])
input.expectsMediaDataInRealTime = false
input.mediaTimeScale = 16000
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input,
    sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32ARGB,
                                 kCVPixelBufferWidthKey as String: 1040,
                                 kCVPixelBufferHeightKey as String: 760,
                                 kCVPixelBufferCGImageCompatibilityKey as String: true,
                                 kCVPixelBufferCGBitmapContextCompatibilityKey as String: true])
guard writer.canAdd(input) else { fatalError("cannot add video input") }
writer.add(input)
guard writer.startWriting() else { fatalError("start writing: \(String(describing: writer.error))") }
writer.startSession(atSourceTime: .zero)
for (index, url) in frames.enumerated() {
    try autoreleasepool {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
              image.width == 1040, image.height == 760 else { fatalError("bad frame \(index)") }
        var pixel: CVPixelBuffer?
        guard let pool = adaptor.pixelBufferPool,
              CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pixel) == kCVReturnSuccess,
              let buffer = pixel else { fatalError("pixel buffer") }
        CVPixelBufferLockBaseAddress(buffer, [])
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: 1040, height: 760,
            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue) else { fatalError("context") }
        context.draw(image, in: CGRect(x: 0, y: 0, width: 1040, height: 760))
        CVPixelBufferUnlockBaseAddress(buffer, [])
        while !input.isReadyForMoreMediaData {
            if writer.status == .failed { throw writer.error! }
            Thread.sleep(forTimeInterval: 0.001)
        }
        guard adaptor.append(buffer, withPresentationTime: CMTime(value: Int64(index), timescale: 16)) else {
            fatalError("append: \(String(describing: writer.error))")
        }
    }
}
writer.endSession(atSourceTime: CMTime(value: Int64(count), timescale: 16))
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
guard writer.status == .completed else { fatalError("finish: \(String(describing: writer.error))") }
print("encoded \(count) NON-PHYSICAL DIAGNOSTIC frames at 16 fps")
