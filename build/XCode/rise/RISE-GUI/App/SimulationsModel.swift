//////////////////////////////////////////////////////////////////////
//
//  SimulationsModel.swift - Simulation M5 (macOS shell): the state the
//    Simulations panel and the timeline's baked-range track present,
//    and the actions they forward.
//
//    Everything here is read from, or forwarded to, the library through
//    RISEViewportBridge (RISE_API Simulation M3c/M4/M5).  The shell
//    computes no keys, inspects no files and runs no simulation logic:
//    states, cache locations, consumer geometries, ranges and the
//    trusted-location decision all come back from the library.  See
//    docs/SIMULATION_GUI_STATUS.md.
//
//////////////////////////////////////////////////////////////////////

import Combine
import Foundation
import SwiftUI

// MARK: - Value types (plain copies of one bridge read)

/// RISE_API GetSimulation state (0 missing .. 5 failed).
enum SimulationState: Int {
    case missing = 0, baking = 1, partial = 2, complete = 3, stale = 4, failed = 5

    var label: String {
        switch self {
        case .missing:  return "missing"
        case .baking:   return "baking"
        case .partial:  return "partial"
        case .complete: return "complete"
        case .stale:    return "stale"
        case .failed:   return "failed"
        }
    }
}

/// The colour classes the timeline track and the panel share.
enum SimulationRangeStyle: Equatable {
    case complete, partial, baking, stale, live

    var color: Color {
        switch self {
        case .complete: return Theme.success
        case .partial:  return Theme.warn
        case .baking:   return Theme.accent
        case .stale:    return Theme.dirty
        case .live:     return Theme.purple
        }
    }

    var label: String {
        switch self {
        case .complete: return "complete"
        case .partial:  return "partial"
        case .baking:   return "baking"
        case .stale:    return "stale"
        case .live:     return "live"
        }
    }
}

struct SimulationRow: Identifiable, Equatable {
    var id: String { name }
    let name: String
    let keyword: String
    let solverId: String
    let backend: String
    let fidelity: String
    let consumerGeometry: String
    let state: SimulationState
    let upstreamOutOfDate: Bool
    let interactive: Bool
    let framesAvailable: UInt64
    let framesTotal: UInt64
    let startTime: Double
    let interval: Double
    let cacheDirectory: String
    let upstream: String
    let inputs: String
    let errorMessage: String
    /// M5c: the library's estimate before a bake ("≈ 5 h, 4.0 GB"; "" when
    /// unknown) and why a stale cache is stale ("" unless stale).
    let estimateSummary: String
    let memoryEstimate: UInt64
    let wallEstimate: Double
    let staleReason: String
    /// GUI status pass: what stands behind `fidelity` (see RISESimulationInfo).
    let evidenceScope: String
    let evidenceCases: Int
    let fidelityLimit: String
    /// A group's `connect` / `owned` lines and any `port_source` lines, one per entry.
    let connections: [String]

    init(_ info: RISESimulationInfo) {
        name = info.name
        keyword = info.keyword
        solverId = info.solverId
        backend = info.backend
        fidelity = info.fidelity
        consumerGeometry = info.consumerGeometry
        state = SimulationState(rawValue: info.state) ?? .failed
        upstreamOutOfDate = info.upstreamOutOfDate
        interactive = info.interactive
        framesAvailable = info.framesAvailable
        framesTotal = info.framesTotal
        startTime = info.startTime
        interval = info.interval
        cacheDirectory = info.cacheDirectory
        upstream = info.upstream
        inputs = info.inputs
        errorMessage = info.errorMessage
        estimateSummary = info.estimateSummary
        memoryEstimate = info.memoryEstimate
        wallEstimate = info.wallEstimate
        staleReason = info.staleReason
        evidenceScope = info.evidenceScope
        evidenceCases = Int(info.evidenceCases)
        fidelityLimit = info.fidelityLimit
        connections = info.connections.split(separator: "\n").map(String.init)
    }

    /// A `simulation_group` is baked as one job (the bake manager runs its members
    /// together), so it is a producer although it has no solver of its own.
    var isGroup: Bool { keyword == "simulation_group" }

    /// A `simulation_cache` has no solver: it is never baked or run live.
    var isProducer: Bool { !solverId.isEmpty || isGroup }

    /// The label shown for the cache: `preview` / `predictive`, or "no cache".
    var labelText: String { fidelity.isEmpty ? "no cache" : fidelity }

    /// The evidence behind the label, one sentence: what the label covers (and how many
    /// validation cases back it), or why `predictive` is not earnable. "" when neither is known.
    var evidenceText: String {
        var parts: [String] = []
        if !evidenceScope.isEmpty {
            parts.append(evidenceScope + (evidenceCases > 0 ? " (\(evidenceCases) validation case\(evidenceCases == 1 ? "" : "s"))" : ""))
        }
        if !fidelityLimit.isEmpty && fidelity != "predictive" { parts.append("not predictive: " + fidelityLimit) }
        return parts.joined(separator: "; ")
    }

    /// "0.000–3.000 s @ 30 Hz" from the configuration's time support.
    var timeSupportLabel: String {
        guard framesTotal > 0 else { return "—" }
        let end = startTime + interval * Double(max(framesTotal, 1) - 1)
        let hz = interval > 0 ? String(format: " @ %.4g Hz", 1.0 / interval) : ""
        return String(format: "%.3f–%.3f s", startTime, end) + hz
    }
}

struct LiveRunRow: Equatable {
    let playing: Bool
    let finished: Bool
    let failed: Bool
    let latestFrame: Int64
    let latestSceneTime: Double
    let framesPublished: UInt64
    let framesTotal: UInt64

    init(_ st: RISELiveSimulationStatus) {
        playing = st.playing
        finished = st.finished
        failed = st.failed
        latestFrame = st.latestFrame
        latestSceneTime = st.latestSceneTime
        framesPublished = st.framesPublished
        framesTotal = st.framesTotal
    }
}

/// The latest bake job of one simulation (library bake manager).
struct BakeJobRow: Equatable {
    enum State: Int {
        case queued = 0, waitingForGPU = 1, running = 2, stopping = 3, stopped = 4,
             finished = 5, failed = 6, cancelled = 7
        var label: String {
            switch self {
            case .queued: return "queued"
            case .waitingForGPU: return "waiting for GPU"
            case .running: return "running"
            case .stopping: return "stopping"
            case .stopped: return "stopped"
            case .finished: return "finished"
            case .failed: return "failed"
            case .cancelled: return "cancelled"
            }
        }
    }
    let jobId: UInt64
    let state: State
    let resumable: Bool
    let adopted: Bool
    let simulatedTime: Double
    let endTime: Double
    let framesWritten: UInt64
    let framesTotal: UInt64
    let etaSeconds: Double
    let backend: String
    let waitingFor: String
    let detail: String
    let errorMessage: String

    init(_ job: RISESimulationBakeJob) {
        jobId = job.jobId
        state = State(rawValue: job.state) ?? .failed
        resumable = job.resumable
        adopted = job.adopted
        simulatedTime = job.simulatedTime
        endTime = job.endTime
        framesWritten = job.framesWritten
        framesTotal = job.framesTotal
        etaSeconds = job.etaSeconds
        backend = job.backend
        waitingFor = job.waitingFor
        detail = job.detail
        errorMessage = job.errorMessage
    }

    /// Queued, waiting, running or stopping: Stop applies, the range grows.
    var isActive: Bool { state.rawValue <= State.stopping.rawValue }

    var summary: String {
        var text = state.label
        if state == .running || state == .stopping {
            text += " \(framesWritten)/\(framesTotal) frames"
            if etaSeconds >= 0 { text += String(format: ", ~%.0f s left", etaSeconds) }
        }
        if state == .queued && !waitingFor.isEmpty { text += " (\(waitingFor))" }
        if adopted { text += ", adopted" }
        return text
    }
}

/// One time-varying asset's range, as the timeline draws it.
struct TimelineAssetRange: Identifiable, Equatable {
    var id: String { name }
    let name: String
    let kind: Int
    let style: SimulationRangeStyle
    let start: Double
    let end: Double
    let availableEnd: Double
    let framesAvailable: UInt64
    let framesTotal: UInt64

    var helpText: String {
        "\(name): \(style.label), \(framesAvailable) of \(framesTotal) frames"
            + String(format: " (%.3f–%.3f s baked, to %.3f s)", start, availableEnd, end)
    }
}

/// A bake or record the library refused for its cache location.  The user
/// decides whether to trust `suggestedRoot`; nothing is trusted silently.
struct CacheTrustRequest: Identifiable, Equatable {
    enum Action: Equatable { case bake, resume, record }
    let id = UUID()
    let simulation: String
    let action: Action
    let directory: String
    let reason: String
    let suggestedRoot: String
}

// MARK: - Trusted cache roots (user preference, re-applied at launch)

enum TrustedCacheRootStore {
    static let defaultsKey = "trustedSimulationCacheRoots"
    /// The app's preferences; the headless probe swaps in a private suite.
    static var defaults: UserDefaults = .standard

    static var persisted: [String] {
        defaults.stringArray(forKey: defaultsKey) ?? []
    }

    /// Re-apply every root the user trusted in an earlier session.  A root
    /// that no longer exists is skipped (and kept, in case a drive is just
    /// not mounted yet); the library refuses it on its own terms.
    static func applyPersisted() {
        for root in persisted {
            var error: NSString?
            if !RISEViewportBridge.addTrustedCacheRoot(root, error: &error) {
                NSLog("RISE: trusted simulation cache root not applied (%@): %@", root, (error as String?) ?? "")
            }
        }
    }

    /// Trust `root` for this process and persist it.  Only called from an
    /// explicit user action ("Trust this location").
    @discardableResult
    static func trust(_ root: String, error: inout String?) -> Bool {
        var err: NSString?
        guard RISEViewportBridge.addTrustedCacheRoot(root, error: &err) else {
            error = (err as String?) ?? "the library refused the directory"
            return false
        }
        var roots = persisted
        if !roots.contains(root) { roots.append(root) }
        defaults.set(roots, forKey: defaultsKey)
        return true
    }
}

// MARK: - Bake service (bake manager C API; see docs/SIMULATION_SOLVER_CONTRACT.md)

/// Bake / stop / resume of a scene simulation.  The library's bake manager
/// (another M5 track) owns scheduling, workers and progress; this protocol
/// is the shell's whole view of it.
protocol SimulationBakeService: AnyObject {
    /// False while this build has no bake manager in the library.
    var isAvailable: Bool { get }
    func bake(_ simulation: String, force: Bool, error: inout String?) -> Bool
    func stop(_ simulation: String, error: inout String?) -> Bool
    func resume(_ simulation: String, error: inout String?) -> Bool
}

/// The library bake manager through the bridge (RISE_API_SceneEditController_
/// SimulationBake / BakeResume / BakeStop).  Queueing is quick: the manager
/// starts a rise-sim worker process and monitors it.
final class LibraryBakeService: SimulationBakeService {
    private weak var bridge: RISEViewportBridge?
    init(bridge: RISEViewportBridge?) { self.bridge = bridge }
    var isAvailable: Bool { bridge != nil }
    private func call(_ body: (RISEViewportBridge, AutoreleasingUnsafeMutablePointer<NSString?>) -> Bool,
                      _ error: inout String?) -> Bool {
        guard let bridge else { error = "the scene is closed"; return false }
        var err: NSString?
        let ok = body(bridge, &err)
        if !ok { error = (err as String?) ?? "refused" }
        return ok
    }
    func bake(_ simulation: String, force: Bool, error: inout String?) -> Bool {
        call({ $0.simulationBake(simulation, force: force, error: $1) }, &error)
    }
    func stop(_ simulation: String, error: inout String?) -> Bool {
        call({ $0.simulationBakeStop(simulation, kill: false, error: $1) }, &error)
    }
    func resume(_ simulation: String, error: inout String?) -> Bool {
        call({ $0.simulationBakeResume(simulation, error: $1) }, &error)
    }
}

/// Stand-in used when no bake manager is available (kept for builds or
/// shells without one).
final class UnavailableBakeService: SimulationBakeService {
    var isAvailable: Bool { false }
    private func refuse(_ simulation: String, _ error: inout String?) -> Bool {
        error = "Baking from the app needs the library bake manager, which is not in this build. "
            + "Bake from Terminal: rise_sim bake <scene> \(simulation)"
        return false
    }
    func bake(_ simulation: String, force: Bool, error: inout String?) -> Bool { refuse(simulation, &error) }
    func stop(_ simulation: String, error: inout String?) -> Bool { refuse(simulation, &error) }
    func resume(_ simulation: String, error: inout String?) -> Bool { refuse(simulation, &error) }
}

// MARK: - Off-main live calls

/// The narrowest Sendable wrapper for the blocking live-run calls (same
/// pattern as PainterPreviewFetcher): a weak bridge and only these calls.
/// Each one is serialized against -shutdown inside the bridge.
nonisolated final class SimulationLiveRunner: @unchecked Sendable {
    private weak var bridge: RISEViewportBridge?
    init(bridge: RISEViewportBridge) { self.bridge = bridge }

    private func run(_ body: (RISEViewportBridge, AutoreleasingUnsafeMutablePointer<NSString?>) -> Bool) -> String? {
        guard let bridge else { return "the scene is closed" }
        var err: NSString?
        return body(bridge, &err) ? nil : ((err as String?) ?? "refused")
    }
    func attach(_ simulation: String) -> String? { run { $0.liveAttachSimulation(simulation, error: $1) } }
    func detach(_ geometry: String) -> String? { run { $0.liveDetachGeometry(geometry, error: $1) } }
    func reset(_ geometry: String) -> String? { run { $0.liveResetGeometry(geometry, error: $1) } }
    func record(_ geometry: String, _ dir: String) -> String? {
        run { $0.liveRecordGeometry(geometry, directory: dir, error: $1) }
    }
    func resync() -> String? { run { $0.liveResync($1) } }
}

// MARK: - Model

@MainActor
final class SimulationsModel: ObservableObject {
    @Published private(set) var rows: [SimulationRow] = []
    @Published private(set) var liveRuns: [String: LiveRunRow] = [:]      // by simulation name
    @Published private(set) var ranges: [TimelineAssetRange] = []
    @Published private(set) var busy: Set<String> = []                    // simulations with a call in flight
    @Published private(set) var bakeJobs: [String: BakeJobRow] = [:]      // latest job per simulation
    @Published var selected: String? = nil
    @Published var lastMessage: String = ""
    @Published var trustRequest: CacheTrustRequest? = nil
    /// True once a read succeeded for the current bridge (false: never read
    /// or the scene was busy every time so far).
    @Published private(set) var loaded: Bool = false

    var bakeService: SimulationBakeService = UnavailableBakeService()

    /// Called after a live attach succeeds so playback runs through the
    /// library clock (RenderViewModel.startPreviewPlay).
    var onLiveAttached: (() -> Void)? = nil
    /// Called before a detach so the clock does not drive a vanishing run.
    var onWillChangeLiveRuns: (() -> Void)? = nil

    private weak var bridge: RISEViewportBridge?
    private var runner: SimulationLiveRunner?
    private var pollTimer: Timer?
    private var lastSceneRevision: String? = nil
    private var lastBakeCounter: UInt64? = nil

    // MARK: Lifecycle

    func attach(bridge: RISEViewportBridge?) {
        pollTimer?.invalidate()
        pollTimer = nil
        self.bridge = bridge
        runner = bridge.map { SimulationLiveRunner(bridge: $0) }
        bakeService = LibraryBakeService(bridge: bridge)
        rows = []
        liveRuns = [:]
        bakeJobs = [:]
        lastBakeCounter = nil
        ranges = []
        busy = []
        selected = nil
        lastMessage = ""
        trustRequest = nil
        loaded = false
        lastSceneRevision = nil
        refresh()
    }

    var hasSimulations: Bool { !rows.isEmpty }

    /// Read the simulations, their live runs and the timeline ranges.  A busy
    /// scene keeps the previous values (the reads are non-blocking).
    func refresh() {
        guard let bridge else { return }
        if let infos = bridge.simulations() {
            let newRows = infos.map(SimulationRow.init)
            if rows != newRows { rows = newRows }
            if !loaded { loaded = true }
            if let sel = selected, !newRows.contains(where: { $0.name == sel }) { selected = nil }
        }
        var newLive: [String: LiveRunRow] = [:]
        for row in rows where !row.consumerGeometry.isEmpty {
            if let st = bridge.liveStatus(forGeometry: row.consumerGeometry) {
                newLive[row.name] = LiveRunRow(st)
            } else if let previous = liveRuns[row.name], bridge.liveSimulationCount > 0 {
                newLive[row.name] = previous   // contended read: keep the last value
            }
        }
        if bridge.liveSimulationCount == 0 { newLive = [:] }
        if liveRuns != newLive { liveRuns = newLive }
        if let jobs = bridge.simulationBakeJobs() {
            var latest: [String: BakeJobRow] = [:]
            for job in jobs { latest[job.simulation] = BakeJobRow(job) }   // oldest first: the last wins
            if bakeJobs != latest { bakeJobs = latest }
        }
        if let assets = bridge.timeVaryingAssets() {
            let newRanges = assets.map { asset -> TimelineAssetRange in
                // Stale is a property of the producing simulation, not of the
                // cache's own range: join on the consumer geometry name.
                let producer = rows.first { $0.consumerGeometry == asset.name }
                let style: SimulationRangeStyle
                switch asset.state {
                case 3: style = .live
                case 2: style = .baking
                default:
                    if producer?.state == .stale { style = .stale }
                    else { style = asset.state == 0 ? .complete : .partial }
                }
                return TimelineAssetRange(name: asset.name, kind: asset.kind, style: style,
                                          start: asset.start, end: asset.end,
                                          availableEnd: asset.availableEnd,
                                          framesAvailable: asset.framesAvailable,
                                          framesTotal: asset.framesTotal)
            }
            if ranges != newRanges { ranges = newRanges }
        }
        updatePolling()
    }

    /// Poll at 1 Hz only while something grows: a bake, a live run, or a call
    /// in flight.  Otherwise the panel refreshes on events.
    private var needsPolling: Bool {
        !busy.isEmpty || !liveRuns.isEmpty
            || bakeJobs.values.contains { $0.isActive }
            || rows.contains { $0.state == .baking }
            || ranges.contains { $0.style == .baking || $0.style == .live }
    }

    private func updatePolling() {
        if needsPolling {
            guard pollTimer == nil else { return }
            let timer = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
                Task { @MainActor [weak self] in self?.pollTick() }
            }
            RunLoop.main.add(timer, forMode: .common)
            pollTimer = timer
        } else {
            pollTimer?.invalidate()
            pollTimer = nil
        }
    }

    /// One poll: pick up newly baked frames (the library rebinds consumers
    /// and re-renders) and re-read the states.
    private func pollTick() {
        if let bridge, bakeJobs.values.contains(where: { $0.isActive }) || rows.contains(where: { $0.state == .baking }) {
            _ = bridge.refreshSimulationFrames()
        }
        refresh()
    }

    /// The bake manager's process-wide change counter (RenderViewModel polls
    /// it at 2 Hz): a bake started elsewhere (the agent, another document)
    /// or one that changed state refreshes the panel and installs new frames.
    func bakeCounterObserved(_ counter: UInt64) {
        guard lastBakeCounter != counter else { return }
        lastBakeCounter = counter
        if let bridge { _ = bridge.refreshSimulationFrames() }
        refresh()
    }

    /// Scene edits (property panel, scene text, agent) change keys and
    /// states.  RenderViewModel calls this from its poll with the CST head
    /// revision; on a change the states are re-read and live runs re-synced
    /// against the edited scene (the library restarts only runs whose
    /// configuration changed).  An edit never starts a bake.
    func sceneRevisionObserved(_ revision: String) {
        guard lastSceneRevision != revision else { return }
        let first = lastSceneRevision == nil
        lastSceneRevision = revision
        guard !first else { return }
        if !liveRuns.isEmpty, let runner {
            Task.detached { [weak self] in
                let error = runner.resync()
                await MainActor.run { [weak self] in
                    if let error { self?.lastMessage = error }
                    self?.refresh()
                }
            }
        } else {
            refresh()
        }
    }

    // MARK: Actions (forwarded; the library decides)

    func row(_ name: String) -> SimulationRow? { rows.first { $0.name == name } }

    func isLive(_ name: String) -> Bool { liveRuns[name] != nil }

    func bake(_ name: String) { startCacheAction(name, .bake) }
    func resume(_ name: String) { startCacheAction(name, .resume) }
    func record(_ name: String) { startCacheAction(name, .record) }

    func stopBake(_ name: String) {
        var error: String?
        if !bakeService.stop(name, error: &error) { lastMessage = error ?? "stop refused" }
        refresh()
    }

    /// Bake, resume and record write the simulation's cache: ask the library
    /// first whether its location policy accepts the directory.
    private func startCacheAction(_ name: String, _ action: CacheTrustRequest.Action) {
        guard let row = row(name) else { return }
        var suggested: NSString?
        var reason: NSString?
        if !row.cacheDirectory.isEmpty,
           !RISEViewportBridge.checkCacheLocation(row.cacheDirectory, suggestedRoot: &suggested, error: &reason) {
            trustRequest = CacheTrustRequest(simulation: name, action: action,
                                             directory: row.cacheDirectory,
                                             reason: (reason as String?) ?? "the cache location was refused",
                                             suggestedRoot: (suggested as String?) ?? "")
            return
        }
        perform(action, row)
    }

    /// The user chose "Trust this location" for `request`.
    func trustAndContinue(_ request: CacheTrustRequest) {
        trustRequest = nil
        guard !request.suggestedRoot.isEmpty else { return }
        var error: String?
        guard TrustedCacheRootStore.trust(request.suggestedRoot, error: &error) else {
            lastMessage = error ?? "not trusted"
            return
        }
        var suggested: NSString?
        var reason: NSString?
        guard RISEViewportBridge.checkCacheLocation(request.directory, suggestedRoot: &suggested, error: &reason) else {
            lastMessage = "Still refused after trusting \(request.suggestedRoot): \((reason as String?) ?? "")"
            return
        }
        if let row = row(request.simulation) { perform(request.action, row) }
    }

    func cancelTrust() { trustRequest = nil }

    private func perform(_ action: CacheTrustRequest.Action, _ row: SimulationRow) {
        switch action {
        case .bake, .resume:
            var error: String?
            let ok = action == .bake ? bakeService.bake(row.name, force: row.state == .complete, error: &error)
                                     : bakeService.resume(row.name, error: &error)
            lastMessage = ok ? "" : (error ?? "refused")
            refresh()
        case .record:
            guard !row.consumerGeometry.isEmpty, let runner else { return }
            let geometry = row.consumerGeometry, dir = row.cacheDirectory
            runOffMain(row.name) { runner.record(geometry, dir) }
        }
    }

    /// Live: attach the scene simulation to its consumer and play it through
    /// the library clock.  Attach configures the solver (about 0.5 s on first
    /// use), so it runs off the main thread.
    func goLive(_ name: String) {
        guard let row = row(name), row.interactive, let runner else { return }
        runOffMain(name, after: { [weak self] ok in if ok { self?.onLiveAttached?() } }) {
            runner.attach(name)
        }
    }

    func stopLive(_ name: String) {
        guard let row = row(name), !row.consumerGeometry.isEmpty, let runner else { return }
        onWillChangeLiveRuns?()
        let geometry = row.consumerGeometry
        runOffMain(name) { runner.detach(geometry) }
    }

    func resetLive(_ name: String) {
        guard let row = row(name), !row.consumerGeometry.isEmpty, let runner else { return }
        let geometry = row.consumerGeometry
        runOffMain(name) { runner.reset(geometry) }
    }

    private func runOffMain(_ name: String, after: ((Bool) -> Void)? = nil,
                            _ call: @escaping @Sendable () -> String?) {
        guard !busy.contains(name) else { return }
        busy.insert(name)
        updatePolling()
        Task.detached { [weak self] in
            let error = call()
            await MainActor.run { [weak self] in
                guard let self else { return }
                self.busy.remove(name)
                self.lastMessage = error ?? ""
                self.refresh()
                after?(error == nil)
            }
        }
    }
}
