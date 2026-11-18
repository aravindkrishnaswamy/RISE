#!/usr/bin/env python3
"""Seal external NON-PHYSICAL DIAGNOSTIC movies and representative still set."""
import json
import pathlib
import subprocess
import sys
from PIL import Image
from r243_structure_diagnostic import LABEL, ROOT, record, sha

out = pathlib.Path(sys.argv[1])
if 'nonphysical_diagnostic' not in out.name:
    raise ValueError('diagnostic directory required')
launch = json.loads((out/'nonphysical_diagnostic_launch.json').read_text())
if launch['label'] != LABEL or launch['physical_preview_eligible'] is not False:
    raise ValueError('diagnostic provenance required')
inventory = json.loads((out/'nonphysical_diagnostic_frames.sha256.json').read_text())
if len(inventory) != 235:
    raise ValueError('incomplete frames')
sidecars=[]
for index, entry in enumerate(inventory):
    if entry['file'] != f'nonphysical_diagnostic_temperature_{index:04d}.png':
        raise ValueError('wrong ordering')
    sidepath=out/(entry['file']+'.provenance.json')
    side=json.loads(sidepath.read_text())
    if sha(out/entry['file']) != entry['sha256'] or sha(sidepath) != entry['sidecar_sha256']:
        raise ValueError('frame changed')
    if side['label'] != LABEL or side['physical_preview_eligible'] is not False or side['source_frame']!=index:
        raise ValueError('wrong frame provenance')
    if sha(pathlib.Path(side['source_file']))!=side['source_vdb_sha256']:
        raise ValueError('source changed')
    sidecars.append(side)
for first, count, name in [(64,64,'nonphysical_diagnostic_structure_clip_16fps.mov'),
                           (0,235,'nonphysical_diagnostic_full_statistics_16fps.mov')]:
    movie=out/name
    scripts=[ROOT/'tools/fire/r243_encode_nonphysical_diagnostic.swift',ROOT/'tools/fire/r243_verify_nonphysical_diagnostic.swift']
    for script,args in [(scripts[0],[str(out),str(movie),str(first),str(count)]),
                        (scripts[1],[str(movie),str(count)])]:
        result=subprocess.run(['swift',str(script),*args],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (out/(name+'.'+script.stem+'.log')).write_text(result.stdout)
        print(result.stdout,flush=True)
    provenance=dict(launch, file=name, sha256=sha(movie), native_fps=16,
        first_source_frame=first, frame_count=count, playback_duration_s=count/16,
        source_first_time_s=sidecars[first]['simulation_time_s'],
        source_last_time_s=sidecars[first+count-1]['simulation_time_s'],
        final_frame_hold_s=1/16, codec='H264 SDR', physical_preview_eligible=False,
        encoder_sha256=sha(scripts[0]),verifier_sha256=sha(scripts[1]),
        finalizer_sha256=sha(pathlib.Path(__file__)),
        ordered_inputs=inventory[first:first+count])
    record(out/(name+'.provenance.json'),provenance)
    decoded=out/(name+'.nonphysical_diagnostic_decoded.png')
    record(out/(decoded.name+'.provenance.json'),dict(label=LABEL,physical_preview_eligible=False,
        parent_movie_sha256=sha(movie),sha256=sha(decoded),source_frame=first+count//2))
stills=[64,80,96,112]
canvas=Image.new('RGB',(1040,760))
for panel,index in enumerate(stills):
    with Image.open(out/inventory[index]['file']) as image:
        canvas.paste(image.resize((520,380)),((panel%2)*520,(panel//2)*380))
still=out/'nonphysical_diagnostic_representative_stills.png'
canvas.save(still)
record(out/(still.name+'.provenance.json'),dict(label=LABEL,physical_preview_eligible=False,
    file=still.name,sha256=sha(still),source_frames=stills,ordered_inputs=[inventory[i] for i in stills]))
record(out/'nonphysical_diagnostic_complete.json',dict(label=LABEL,physical_preview_eligible=False,
    artifacts=[dict(file=p.name,sha256=sha(p),bytes=p.stat().st_size) for p in sorted(out.iterdir()) if p.is_file()]))
