import Foundation
import AVFoundation
import ImageIO
import CoreVideo

let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let output = URL(fileURLWithPath: CommandLine.arguments[2])
guard !FileManager.default.fileExists(atPath: output.path) else { fatalError("output exists") }
let frames = (0..<235).map { directory.appendingPathComponent(String(format: "fire_display_denoised%04d.png", $0)) }
let writer = try AVAssetWriter(outputURL: output, fileType: .mov)
writer.movieTimeScale = 16000
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264,
    AVVideoWidthKey: 512, AVVideoHeightKey: 768,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 10_000_000,
                                     AVVideoMaxKeyFrameIntervalKey: 16]
])
input.expectsMediaDataInRealTime = false
input.mediaTimeScale = 16000
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input,
    sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32ARGB,
                                 kCVPixelBufferWidthKey as String: 512,
                                 kCVPixelBufferHeightKey as String: 768,
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
              image.width == 512, image.height == 768 else { fatalError("bad frame \(index)") }
        var pixel: CVPixelBuffer?
        guard let pool = adaptor.pixelBufferPool,
              CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pixel) == kCVReturnSuccess,
              let buffer = pixel else { fatalError("pixel buffer") }
        CVPixelBufferLockBaseAddress(buffer, [])
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: 512, height: 768,
            bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue) else { fatalError("context") }
        context.draw(image, in: CGRect(x: 0, y: 0, width: 512, height: 768))
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
writer.endSession(atSourceTime: CMTime(value: 235, timescale: 16))
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
guard writer.status == .completed else { fatalError("finish: \(String(describing: writer.error))") }
print("encoded 235 frames at 16 fps; duration 14.6875 s including final-frame hold")
