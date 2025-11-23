#!/bin/bash

export RISE_MEDIA_PATH=$(pwd)/

# Create rendered directory if it doesn't exist
mkdir -p rendered

# Find all .RISEscene files in scenes/FeatureBased
find scenes/FeatureBased -name "*.RISEscene" | while read scene; do
    echo "Rendering $scene..."
    
    # Extract filename for log or check
    filename=$(basename "$scene")
    
    # Run rise with piped input to render and quit
    # We ignore the exit code because rise seems to exit with 1 on 'quit'
    printf "render\nquit\n" | ./bin/rise "$scene" > /dev/null 2>&1
    
    # Check if output file exists (assuming default output naming convention)
    # Most scenes output to rendered/filename.tga (or .png but we know it's tga)
    # We can't easily know the exact output path without parsing the scene file,
    # but we can check if *something* was created or just rely on the run.
    # For now, we just run them.
done

echo "Batch rendering complete."
