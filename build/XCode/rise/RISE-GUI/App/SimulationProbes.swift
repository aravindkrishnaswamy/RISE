//////////////////////////////////////////////////////////////////////
//
//  SimulationProbes.swift - Simulation M5: process-start bootstrap
//    (built-in solvers, persisted trusted cache roots) and the
//    headless probes that verify the simulation UI's model layer
//    without a WindowServer session.
//
//    The probes drive the REAL RenderViewModel / SimulationsModel /
//    RISEViewportBridge stack the window uses (scene load, viewport
//    render thread, library playback clock, live runs), only without
//    SwiftUI views.  Same conventions as ViewportReattachProbe
//    (RISEApp.swift): opt-in through RISE_GUI_HEADLESS_PROBE, print
//    "<name>: PASS/FAIL -- label" lines, exit 0 (all pass) or 1.
//
//    RISE_GUI_HEADLESS_PROBE=simulation_m5
//    RISE_GUI_HEADLESS_PROBE_SCENE   scene (default
//                                    scenes/Tests/Simulation/cloth_flag_scene.RISEscene)
//    RISE_GUI_HEADLESS_PROBE_STEPS   comma list of: worker, list, trust, bake, live, scrub,
//                                    edit, props, playrule (run in that order; bake uses the
//                                    bundled Contents/MacOS/rise-sim, or RISE_SIM_WORKER;
//                                    CPU cloth only).  M5c: `worker` checks the bundled
//                                    worker is found with no override; `props` drives the
//                                    Inspector's Simulation category (fields, a numeric edit,
//                                    stale with its reason, undo); `playrule` checks the
//                                    Play button's rate (RISE_GUI_HEADLESS_PROBE_RATE:
//                                    realtime | stepping).
//    RISE_GUI_HEADLESS_PROBE_EXPECT  expected state of `flag` for `list`
//                                    (missing | complete | ...; default: any)
//    RISE_GUI_HEADLESS_PROBE_OUT     directory for PNG snapshots (optional)
//    RISE_MEDIA_PATH                 repo root, as for every probe
//
//    `trust` must run WITHOUT RISE_SIMULATION_TRUSTED_ROOTS covering the
//    scene's cache (it checks that the library refuses, offers a root,
//    and accepts after the user-equivalent "Trust" action); it persists
//    into a probe-private defaults suite, removed afterwards.  The other
//    steps need the cache location accepted (set the environment
//    variable).  docs/SIMULATION_M5B_RESULTS.md has the full recipe.
//
//////////////////////////////////////////////////////////////////////

import AppKit
import Combine
import Foundation
import SwiftUI

enum SimulationBootstrap {
    static func install() {
        var error: NSString?
        if !RISEViewportBridge.registerBuiltinSolvers(&error) {
            NSLog("RISE: built-in simulation solvers not registered: %@", (error as String?) ?? "")
        }
        TrustedCacheRootStore.applyPersisted()
    }
}

@MainActor
enum SimulationProbes {
    private static var allOK = true
    private static var probeViewModel: RenderViewModel? = nil
    private static var probeOutDir: String? = nil

    private static func step(_ label: String, _ ok: Bool) {
        print("SimulationProbe: \(ok ? "PASS" : "FAIL") -- \(label)")
        fflush(stdout)
        if !ok { allOK = false }
    }

    private static func note(_ text: String) {
        print("SimulationProbe: note -- \(text)")
        fflush(stdout)
    }

    /// Run the main run loop (timers, main-queue Tasks, image delivery)
    /// until `condition` holds or `timeout` seconds pass.
    @discardableResult
    private static func spin(_ timeout: Double, until condition: () -> Bool = { false }) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return true }
            RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.01))
        }
        return condition()
    }

    static func runIfRequested() {
        let env = ProcessInfo.processInfo.environment
        guard env["RISE_GUI_HEADLESS_PROBE"] == "simulation_m5" else { return }
        let scene = env["RISE_GUI_HEADLESS_PROBE_SCENE"]
            ?? "scenes/Tests/Simulation/cloth_flag_scene.RISEscene"
        let steps = Set((env["RISE_GUI_HEADLESS_PROBE_STEPS"] ?? "list,live")
            .split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) })
        let expect = env["RISE_GUI_HEADLESS_PROBE_EXPECT"]
        let outDir = env["RISE_GUI_HEADLESS_PROBE_OUT"]
        if let outDir { try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true) }
        let scenePath = scene.hasPrefix("/") ? scene
            : FileManager.default.currentDirectoryPath + "/" + scene

        FontBootstrap.registerBundledFonts()
        let vm = RenderViewModel()
        probeViewModel = vm
        probeOutDir = outDir
        var frames = 0
        var lastImage: NSImage? = nil
        let sink = vm.$renderedImage.sink { image in
            if let image { frames += 1; lastImage = image }
        }

        // Load through the same path File > Open uses (untitled: the probe
        // must not touch the user's recent-files list).
        vm.loadScene(at: scenePath, untitled: true)
        step("scene loads (\(scene))", spin(60) { vm.viewportBridge != nil || vm.renderState != .loading })
        guard let vb = vm.viewportBridge else {
            step("viewport bridge attached", false)
            finish(vm)
        }
        step("first viewport frame", spin(30) { frames > 0 })
        step("timeline visible (hasAnimation)", spin(3) { vm.hasAnimation })
        let model = vm.simulations
        step("simulations read", spin(5) { model.refresh(); return model.loaded })

        if steps.contains("worker") { workerStep() }
        if steps.contains("list") { listStep(model, expect: expect) }
        if steps.contains("trust") { trustStep(model) }
        if steps.contains("bake") { bakeStep(model) }
        if steps.contains("live") {
            liveStep(vm, model, vb, frames: { frames }, image: { lastImage }, outDir: outDir)
        }
        if steps.contains("scrub") {
            scrubStep(vm, model, vb, frames: { frames }, image: { lastImage }, outDir: outDir)
        }
        if steps.contains("edit") { editStep(vm, model) }
        if steps.contains("props") { propsStep(vm, model, vb) }
        if steps.contains("playrule") { playRuleStep(vm, vb, expect: env["RISE_GUI_HEADLESS_PROBE_RATE"]) }
        _ = sink
        finish(vm)
    }

    private static func finish(_ vm: RenderViewModel) -> Never {
        vm.stopPreviewPlay()
        vm.viewportBridge?.shutdown()
        print(allOK ? "SimulationProbe: ALL PASS" : "SimulationProbe: FAILURE (see above)")
        fflush(stdout)
        exit(allOK ? 0 : 1)
    }

    // MARK: (c) the simulations list model

    private static func listStep(_ model: SimulationsModel, expect: String?) {
        let rows = model.rows
        note("rows: " + rows.map { "\($0.name)[\($0.keyword) \($0.solverId) \($0.state.label) "
            + "\($0.framesAvailable)/\($0.framesTotal) fidelity=\($0.fidelity.isEmpty ? "-" : $0.fidelity) "
            + "consumer=\($0.consumerGeometry) cache=\($0.cacheDirectory)]" }.joined(separator: "; "))
        step("one simulation row", rows.count == 1)
        guard let flag = model.row("flag") else { step("row `flag` present", false); return }
        step("flag: keyword cloth_simulation", flag.keyword == "cloth_simulation")
        step("flag: solver cloth.shell", flag.solverId == "cloth.shell")
        step("flag: interactive", flag.interactive)
        step("flag: backend cpu", flag.backend == "cpu")
        step("flag: consumer geo_flag", flag.consumerGeometry == "geo_flag")
        step("flag: 91 frames in the configuration", flag.framesTotal == 91)
        step("flag: 30 Hz time support from 0", abs(flag.interval - 1.0 / 30.0) < 1e-9 && flag.startTime == 0)
        step("flag: cache beside the scene (caches/flag)", flag.cacheDirectory.hasSuffix("/caches/flag"))
        step("flag: scene input pole", flag.inputs.contains("pole"))
        if let expect {
            step("flag: state \(expect) (got \(flag.state.label))", flag.state.label == expect)
            if expect == "complete" {
                step("flag: 91 frames available", flag.framesAvailable == 91)
                step("flag: fidelity label reported", !flag.fidelity.isEmpty)
                step("timeline range for geo_flag is complete",
                     model.ranges.contains { $0.name == "geo_flag" && $0.style == .complete
                                              && $0.framesAvailable == 91 })
            } else if expect == "missing" {
                step("flag: no frames available", flag.framesAvailable == 0)
                step("no timeline range while unbaked", !model.ranges.contains { $0.name == "geo_flag" })
            }
        }
        step("no live run before Live", !model.isLive("flag"))
    }

    // MARK: trusted cache location flow

    private static func trustStep(_ model: SimulationsModel) {
        // A probe-private preference domain: the user's real one is untouched.
        let suite = "rise.simulation-m5.probe"
        UserDefaults.standard.removePersistentDomain(forName: suite)
        guard let flag = model.row("flag") else { step("trust: row `flag` present", false); return }
        var suggested: NSString?
        var reason: NSString?
        let accepted = RISEViewportBridge.checkCacheLocation(flag.cacheDirectory, suggestedRoot: &suggested, error: &reason)
        note("location check: accepted=\(accepted) reason=\((reason as String?) ?? "") suggested=\((suggested as String?) ?? "")")
        step("trust: the library refuses the scene-relative cache on this mount", !accepted)
        // Record writes the cache too, and (with no live run) cannot start
        // anything after the prompt -- the probe never bakes from here.
        model.record("flag")
        guard let request = model.trustRequest else {
            step("trust: Record raises the trust prompt instead of writing", false)
            return
        }
        step("trust: Record raises the trust prompt instead of writing", true)
        step("trust: the prompt carries the library's reason", request.reason.contains("ancestor"))
        step("trust: a root is offered (\(request.suggestedRoot))", !request.suggestedRoot.isEmpty
             && flag.cacheDirectory.hasPrefix(request.suggestedRoot))
        step("trust: nothing trusted before the user acts",
             !RISEViewportBridge.trustedCacheRoots().contains(request.suggestedRoot))
        TrustedCacheRootStore.defaults = UserDefaults(suiteName: suite) ?? .standard
        model.trustAndContinue(request)
        step("trust: root in force after Trust", RISEViewportBridge.trustedCacheRoots().contains(request.suggestedRoot))
        step("trust: root persisted in preferences", TrustedCacheRootStore.persisted.contains(request.suggestedRoot))
        step("trust: location now accepted",
             RISEViewportBridge.checkCacheLocation(flag.cacheDirectory, suggestedRoot: nil, error: nil))
        _ = spin(10) { !model.busy.contains("flag") }
        note("after trust, record reported: \(model.lastMessage)")
        step("trust: the action was forwarded after Trust (refused: no live run)",
             model.trustRequest == nil && model.lastMessage.contains("live"))
        TrustedCacheRootStore.defaults = .standard
        UserDefaults.standard.removePersistentDomain(forName: suite)
    }

    // MARK: bake from the app (library bake manager, rise-sim worker)

    private static func bakeStep(_ model: SimulationsModel) {
        guard let before = model.row("flag") else { step("bake: row `flag` present", false); return }
        note("bake: state before \(before.state.label)")
        let started = Date()
        model.bake("flag")
        note("bake: queue reported '\(model.lastMessage)'")
        step("bake: queued without a trust prompt", model.trustRequest == nil && model.lastMessage.isEmpty)
        var sawJob = false, sawPartialRange = false
        var lastWritten: UInt64 = 0, monotonic = true
        let done = spin(600) {
            if let job = model.bakeJobs["flag"] {
                sawJob = true
                if job.framesWritten < lastWritten { monotonic = false }
                lastWritten = max(lastWritten, job.framesWritten)
            }
            if model.ranges.contains(where: { $0.name == "geo_flag" && $0.framesAvailable < $0.framesTotal
                                              && $0.framesAvailable > 0 }) { sawPartialRange = true }
            if let job = model.bakeJobs["flag"], !job.isActive { return true }
            return false
        }
        let job = model.bakeJobs["flag"]
        note(String(format: "bake: %@ after %.1f s; job %@", done ? "ended" : "timed out",
                    Date().timeIntervalSince(started), job?.summary ?? "none") + " " + (job?.errorMessage ?? ""))
        step("bake: a bake job appeared", sawJob)
        step("bake: the job finished", job?.state == .finished)
        step("bake: frames written never decreased", monotonic)
        model.refresh()
        _ = spin(5) { model.refresh(); return model.row("flag")?.state == .complete }
        step("bake: flag is complete (got \(model.row("flag")?.state.label ?? "-"))", model.row("flag")?.state == .complete)
        step("bake: the timeline range is complete, 91 frames",
             model.ranges.contains { $0.name == "geo_flag" && $0.style == .complete && $0.framesAvailable == 91 })
        note("bake: a partial range was seen while baking: \(sawPartialRange)")
        if let vm = probeViewModel { renderUI(vm, probeOutDir, "after_bake") }
    }

    // MARK: (a) live session through the library clock

    private static func liveStep(_ vm: RenderViewModel, _ model: SimulationsModel, _ vb: RISEViewportBridge,
                                 frames: () -> Int, image: () -> NSImage?, outDir: String?) {
        let attachStart = Date()
        model.goLive("flag")
        step("live: attach completes", spin(30) { model.isLive("flag") && !model.busy.contains("flag") })
        note(String(format: "live: attach took %.2f s; message: %@", Date().timeIntervalSince(attachStart),
                    model.lastMessage))
        step("live: playback started on the library clock", spin(5) { vm.isPreviewPlaying })
        let t0 = vm.sceneTime
        let f0 = frames()
        spin(2.0)
        let t1 = vm.sceneTime
        let f1 = frames()
        let status = vm.lastPlaybackStatus
        note(String(format: "live: scene time %.3f -> %.3f s, viewport frames %d -> %d, clock shown %llu dropped %llu solver waits %llu",
                    t0, t1, f0, f1, status?.framesShown ?? 0, status?.framesDropped ?? 0, status?.solverWaits ?? 0))
        step("live: displayed time advanced in 2 s", t1 > t0 + 0.5)
        step("live: clock reports live", status?.live == true)
        step("live: clock showed frames", (status?.framesShown ?? 0) > 10)
        step("live: viewport delivered frames", f1 > f0 + 5)
        model.refresh()
        step("live: timeline range state live", model.ranges.contains { $0.name == "geo_flag" && $0.style == .live })
        renderUI(vm, outDir, "live")
        vm.stopPreviewPlay()
        spin(1.0)
        model.refresh()
        if let run = model.liveRuns["flag"] {
            note(String(format: "live: run published %llu/%llu frames, latest at %.3f s; playhead %.3f s",
                        run.framesPublished, run.framesTotal, run.latestSceneTime, vm.sceneTime))
            step("live: the playhead is not past the latest published frame",
                 vm.sceneTime <= run.latestSceneTime + 1e-9)
        } else {
            step("live: run status readable", false)
        }
        spin(0.5)
        if let outDir, let img = image() { save(img, "\(outDir)/live_after_2s.png") }
        // Scrubbing a live run reads its published frames (backwards too).
        for t in [0.5, 2.5] {
            vm.jumpTimelineSceneTime(to: t, using: vb)
            let start = frames()
            _ = spin(20) { frames() > start }
            spin(2.5)
            let shown = vb.lastSceneTime()
            note(String(format: "live: scrub to %.2f shows %.3f", t, shown))
            if let outDir, let img = image() {
                let name = String(format: "live_t%.1f", t)
                save(img, "\(outDir)/\(name).png")
                let bakedPath = String(format: "%@/baked_t%.1f.png", outDir, t)
                if let baked = NSImage(contentsOfFile: bakedPath) {
                    note(String(format: "live: |live - baked| at t=%.1f (96x54 luminance): %.4f", t,
                                meanAbsDifference(img, baked)))
                }
            }
        }
        model.stopLive("flag")
        step("live: detach completes", spin(15) { !model.isLive("flag") && !model.busy.contains("flag") })
    }

    // MARK: (b) scrubbing a baked cache

    private static func scrubStep(_ vm: RenderViewModel, _ model: SimulationsModel, _ vb: RISEViewportBridge,
                                  frames: () -> Int, image: () -> NSImage?, outDir: String?) {
        guard let flag = model.row("flag"), flag.state == .complete else {
            step("scrub: the cache is baked (state complete)", false)
            return
        }
        func showAt(_ t: Double) -> NSImage? {
            vm.jumpTimelineSceneTime(to: t, using: vb)
            let start = frames()
            _ = spin(20) { frames() > start }
            spin(2.5)   // let the viewport refine past the scrub preview scale
            return image()
        }
        let a = showAt(0.5)
        step("scrub: scene time 0.5 shown", abs(vb.lastSceneTime() - 0.5) < 1e-9)
        let b = showAt(2.5)
        step("scrub: scene time 2.5 shown", abs(vb.lastSceneTime() - 2.5) < 1e-9)
        if let outDir {
            if let a { save(a, "\(outDir)/baked_t0.5.png") }
            if let b { save(b, "\(outDir)/baked_t2.5.png") }
        }
        let diff = (a != nil && b != nil) ? meanAbsDifference(a!, b!) : 0
        note(String(format: "scrub: mean |difference| of 96x54 luminance between t=0.5 and t=2.5: %.4f", diff))
        step("scrub: the viewport image changes between the two times", diff > 0.002)
        let back = showAt(0.5)
        let same = (a != nil && back != nil) ? meanAbsDifference(a!, back!) : 1
        note(String(format: "scrub: difference t=0.5 vs t=0.5 again: %.4f", same))
        step("scrub: returning to t=0.5 shows the same pose", same < diff * 0.5)

        // Ordinary playback through the library clock over the baked range.
        let t0 = vm.sceneTime
        vm.startPreviewPlay()
        step("playback: started", vm.isPreviewPlaying)
        spin(1.5)
        let t1 = vm.sceneTime
        note(String(format: "playback: %.3f -> %.3f s; shown %llu dropped %llu", t0, t1,
                    vm.lastPlaybackStatus?.framesShown ?? 0, vm.lastPlaybackStatus?.framesDropped ?? 0))
        step("playback: the playhead moves", t1 != t0)
        step("playback: not live", vm.lastPlaybackStatus?.live == false)
        vm.stopPreviewPlay()
        step("playback: stopped", !vm.isPreviewPlaying)
    }

    // MARK: an edit marks the simulation stale and starts nothing

    private static func editStep(_ vm: RenderViewModel, _ model: SimulationsModel) {
        guard model.row("flag")?.state == .complete else {
            step("edit: starts from a complete cache", false)
            return
        }
        let jobsBefore = model.bakeJobs["flag"]?.jobId
        // The in-app agent surface (the same commit path the chat and the
        // agent panel use): one parameter of the producer chunk.
        let response = vm.sendAgentRequest(
            #"{"jsonrpc":"2.0","id":1,"method":"propose_patch","params":{"target":"flag","param":"substeps","value":"12"}}"#)
        note("edit: propose_patch -> \(response.prefix(200))")
        step("edit: applied", response.contains("\"applied\":true"))
        // RenderViewModel's 2 Hz poll sees the new CST revision and re-reads.
        let stale = spin(10) { model.row("flag")?.state == .stale }
        step("edit: flag becomes stale (got \(model.row("flag")?.state.label ?? "-"))", stale)
        step("edit: the stale cache still plays (91 frames available)", model.row("flag")?.framesAvailable == 91)
        step("edit: the timeline marks the range stale",
             model.ranges.contains { $0.name == "geo_flag" && $0.style == .stale })
        spin(2)
        step("edit: no bake was started", model.bakeJobs["flag"]?.jobId == jobsBefore
             && model.bakeJobs["flag"].map { !$0.isActive } ?? true)
        model.selected = "flag"
        renderUI(vm, probeOutDir, "stale_selected")
    }

    // MARK: M5c: the worker ships in the bundle

    private static func workerStep() {
        let env = ProcessInfo.processInfo.environment
        step("worker: no RISE_SIM_WORKER override in this run", env["RISE_SIM_WORKER"] == nil)
        var path: NSString?
        var error: NSString?
        let found = RISEViewportBridge.simulationWorkerPath(&path, error: &error)
        let bundled = (Bundle.main.bundlePath as NSString).appendingPathComponent("Contents/MacOS/rise-sim")
        note("worker: \(found ? (path as String? ?? "") : "not found: " + ((error as String?) ?? ""))")
        step("worker: the bake manager finds Contents/MacOS/rise-sim",
             found && (path as String?).map { URL(fileURLWithPath: $0).resolvingSymlinksInPath().path
                 == URL(fileURLWithPath: bundled).resolvingSymlinksInPath().path } == true)
        step("worker: it is executable", FileManager.default.isExecutableFile(atPath: bundled))
    }

    // MARK: M5c: the Inspector's Simulation category

    private static func propsStep(_ vm: RenderViewModel, _ model: SimulationsModel, _ vb: RISEViewportBridge) {
        guard model.row("flag")?.state == .complete else {
            step("props: starts from a complete cache", false)
            return
        }
        let jobBefore = model.bakeJobs["flag"]?.jobId
        step("props: the outliner lists the simulation", vb.categoryEntities(.simulation) == ["flag"])
        vm.jumpToEntity(category: .simulation, name: "flag")
        step("props: selecting it makes it the Inspector's selection",
             vb.selectionCategory == .simulation && vb.selectionName == "flag")
        vb.refreshProperties()
        let rows = vb.propertySnapshot(for: .simulation)
        func row(_ name: String) -> RISEViewportProperty? { rows.first { $0.name == name } }
        note("props: \(rows.count) rows: " + rows.prefix(12).map { "\($0.name)=\($0.value)" }.joined(separator: " "))
        step("props: descriptor-driven fields (cloth_simulation)", rows.count > 20 && row("chunk_type")?.value == "cloth_simulation")
        step("props: vertices_u shows 31, editable", row("vertices_u")?.value == "31" && row("vertices_u")?.editable == true)
        step("props: substeps is an editable numeric field", row("substeps")?.editable == true)
        let follow = row("follow_object[0]")
        note("props: follow_object[0] kind \(follow?.kind ?? -1) presets \(follow?.presets.map { $0.value } ?? [])")
        step("props: follow_object[0] is a reference row (\(follow?.value ?? "-")) offering scene objects",
             follow.flatMap { PropertyKind(rawValue: $0.kind) } == .reference && follow?.value == "pole" && follow?.presets.contains { $0.value == "pole" } == true)
        let key0 = model.row("flag")?.state
        step("props: edit substeps through the panel path", vb.setProperty(for: .simulation, name: "substeps", value: "12"))
        let stale = spin(10) { model.row("flag")?.state == .stale }
        let reason = model.row("flag")?.staleReason ?? ""
        step("props: the simulation becomes stale (\(model.row("flag")?.state.label ?? "-"))", stale && key0 == .complete)
        step("props: the stale reason is reported (\(reason))", reason == "parameters changed (substeps)")
        vb.refreshProperties()
        step("props: the row shows the new value",
             vb.propertySnapshot(for: .simulation).first { $0.name == "substeps" }?.value == "12")
        renderUI(vm, probeOutDir, "props_stale")
        spin(2)
        step("props: no bake was started", model.bakeJobs["flag"]?.jobId == jobBefore
             && model.bakeJobs["flag"].map { !$0.isActive } ?? true)
        vm.undo()
        let current = spin(10) { model.row("flag")?.state == .complete }
        step("props: undo restores the current cache (\(model.row("flag")?.state.label ?? "-"))", current)
        step("props: no stale reason after undo", model.row("flag")?.staleReason.isEmpty == true)
        if let est = model.row("flag")?.estimateSummary { note("props: estimate \(est)") }
        step("props: the row carries the library's estimate", !(model.row("flag")?.estimateSummary.isEmpty ?? true))
    }

    // MARK: M5c: the Play button's rate

    private static func playRuleStep(_ vm: RenderViewModel, _ vb: RISEViewportBridge, expect: String?) {
        vm.jumpTimelineSceneTime(to: vm.animationTimeStart, using: vb)
        var t0: Double = 0, t1: Double = 0
        var n: UInt = 0
        _ = vb.getAnimationOptions(timeStart: &t0, timeEnd: &t1, numFrames: &n)
        let grid = n > 1 && t1 > t0 ? Double(n - 1) / (t1 - t0) : 30
        vm.startPreviewPlay()
        step("playrule: started", vm.isPreviewPlaying)
        spin(1.0)
        let st = vm.lastPlaybackStatus
        let scale = st?.timeScale ?? 0
        let stepping = abs(scale * grid - 30) < 1e-6
        note(String(format: "playrule: %lu frames over %.3f-%.3f s (grid %.3f fps); clock fps %.3f, time scale %.4f, "
                    + "shown %llu dropped %llu in %.2f s", n, t0, t1, grid, st?.fps ?? 0, scale,
                    st?.framesShown ?? 0, st?.framesDropped ?? 0, st?.wallSeconds ?? 0))
        if expect == "stepping" {
            step("playrule: an ordinary animation steps 30 authored frames per second", stepping && abs((st?.fps ?? 0) - grid) < 1e-6)
        } else {
            step("playrule: simulation assets play in real time", scale == 1)
        }
        vm.stopPreviewPlay()
        step("playrule: stopped", !vm.isPreviewPlaying)
    }

    // MARK: offscreen renders of the panel and the timeline (for a human)

    /// Renders the Simulations panel and the timeline bar (with its baked-
    /// range track) through SwiftUI's ImageRenderer -- no window needed.
    /// M5c: both appearances (`_dark` / `_light`), through the in-memory
    /// theme override (nothing persisted) plus the matching colorScheme.
    private static func renderUI(_ vm: RenderViewModel, _ outDir: String?, _ name: String) {
        guard let outDir else { return }
        let original = ThemeState.mode
        for mode in [ThemeMode.dark, ThemeMode.light] {
            ThemeState.overrideForOffscreenRender(mode)
            let scheme: ColorScheme = mode == .dark ? .dark : .light
            let panel = SimulationsPanel(model: vm.simulations)
                .environmentObject(vm)
                .frame(width: 344)
                .background(Theme.bgPanel)
                .environment(\.colorScheme, scheme)
            let timeline = TimelineSlider(time: .constant(vm.sceneTime),
                                          range: vm.animationTimeStart...vm.animationTimeEnd,
                                          isPlaying: vm.isPreviewPlaying)
                .environmentObject(vm)
                .frame(width: 900)
                .background(Theme.bgTimeline)
                .environment(\.colorScheme, scheme)
            let tag = mode == .dark ? "dark" : "light"
            for (view, suffix) in [(AnyView(panel), "panel"), (AnyView(timeline), "timeline")] {
                let renderer = ImageRenderer(content: view)
                renderer.scale = 2
                if let image = renderer.nsImage { save(image, "\(outDir)/ui_\(name)_\(suffix)_\(tag).png") }
                else { note("ui: \(suffix) did not render") }
            }
        }
        ThemeState.overrideForOffscreenRender(original)
    }

    // MARK: Image helpers (probe-only)

    private static func save(_ image: NSImage, _ path: String) {
        guard let tiff = image.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff),
              let png = rep.representation(using: .png, properties: [:]) else { return }
        try? png.write(to: URL(fileURLWithPath: path))
        note("wrote \(path)")
    }

    /// Mean absolute luminance difference (0..1) of the two images, both
    /// resampled to 96x54 so a preview-scale frame compares with a full one.
    private static func meanAbsDifference(_ a: NSImage, _ b: NSImage) -> Double {
        func gray(_ image: NSImage) -> [UInt8]? {
            let w = 96, h = 54
            var pixels = [UInt8](repeating: 0, count: w * h)
            guard let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil),
                  let ctx = CGContext(data: &pixels, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)
            else { return nil }
            ctx.interpolationQuality = .medium
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
            return pixels
        }
        guard let ga = gray(a), let gb = gray(b) else { return 0 }
        var sum = 0
        for i in 0..<ga.count { sum += abs(Int(ga[i]) - Int(gb[i])) }
        return Double(sum) / Double(ga.count) / 255.0
    }
}
