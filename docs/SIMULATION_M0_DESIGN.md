# M0 reference capture

Pin the source revision and binary digests before measuring. Use the current FireSequenceTest production driver and CLI playback interfaces. No solver, renderer, parser or public API changes belong in this milestone.

Keep immutable inputs, raw output frames and checkpoints, launch records, paired renders and build/test logs on the external SSD. For this checkout use the ignored rendered/simulation-m0 directory. Keep only the recipe, digests, measured results and M2 extraction inventory in docs. Use distinct run directories so a retry cannot overwrite a reference.

Run the make build and fast tier. Run the certified driver alone for a short window at each tier, recording the starting state, options, cadence, accepted steps and publication status. Retain raw SHA-256s. Verify checksums before comparing checkpoints, and account only for the producer build identity and its covering checksums. Compare all other bytes. Capture two independent renders with identical scene, time, options and output encoding. Record their pixel RMS and mean difference. Never use r275 fast output as a certified reference.
