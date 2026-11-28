"""Offline CLI contract controls; no renderer or scientific-validation claim."""
import importlib.util,json,pathlib,signal,subprocess,tempfile,sys,os
repo=pathlib.Path(os.environ.get('RISE_REPO',pathlib.Path(__file__).resolve().parents[1]))
spec=importlib.util.spec_from_file_location('measure_test',repo/'tools/test_hylios_measure.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
binary=pathlib.Path(sys.argv[1]);count=0
with tempfile.TemporaryDirectory(prefix='hylios-bake-cli-') as temp:
 root=pathlib.Path(temp);source=root/'source.json';output=root/'response.asset'
 source.write_text(json.dumps(module.specimen(),sort_keys=True)+'\n')
 base=['bake-local','--specimen',str(source),'--output',str(output),'--wavelength-nm','550','--placement','null','--post-scatter','discard','--boundary','smooth','--histories','8','--max-transitions','1000','--seed','18446744073709551615','--fiber-volume-fraction','0']
 def call(extra=(),replace=None,ok=True):
  global count
  args=list(base)
  for key,value in (replace or {}).items():args[args.index(key)+1]=str(value)
  p=subprocess.run([str(binary)]+args+list(extra),capture_output=True,text=True,timeout=2 if str(source)!=args[args.index('--specimen')+1] else 30)
  assert (p.returncode==0)==ok,(args,p.returncode,p.stdout,p.stderr)
  count+=1;return p
 call(['--incident-t-cells','2','--incident-phi-cells','3','--outgoing-t-cells','3','--outgoing-phi-cells','4'])
 assert output.is_file() and len(list(root.glob('response.asset.statistics.*.cbor')))==1
 before=output.read_bytes()
 call(replace={'--max-transitions':1},ok=False);assert output.read_bytes()==before
 call(['--estimator','weighted'],ok=False);assert output.read_bytes()==before
 call(['--max-total-histories','1'],ok=False);assert output.read_bytes()==before
 call(['--max-source-bytes','1'],ok=False);assert output.read_bytes()==before
 call(['--max-report-bytes','1'],ok=False);assert output.read_bytes()==before
 call(replace={'--seed':'18446744073709551616'},ok=False);assert output.read_bytes()==before
 call(['--replicates','18446744073709551615'],ok=False);assert output.read_bytes()==before
 call(['--unknown-option','1'],ok=False);assert output.read_bytes()==before
 # Exact two-node coverage, independently sampled normal and angular grids.
 multi=list(base);multi[multi.index('--wavelength-nm')]='--wavelengths-nm';multi[multi.index('550')]='550,1400'
 p=subprocess.run([str(binary)]+multi,text=True,capture_output=True,timeout=30);assert p.returncode==0,p.stderr;count+=1
 output.unlink();[p.unlink() for p in root.glob('response.asset.statistics.*.cbor')];[p.unlink() for p in root.glob('response.asset.source.*.json')]
 # Interrupt only after observing installed handlers; there can be no completion
 # before interruption because this run asks for millions of separate histories.
 long=list(base);long[long.index('--histories')+1]='1000000'
 long+=['--max-total-histories','20000000','--max-total-work','200000000000']
 p=subprocess.Popen([str(binary)]+long,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 ready=p.stderr.readline();assert ready.strip()=='bake-local: signal handlers ready',ready
 p.send_signal(signal.SIGINT);out,err=p.communicate(timeout=30)
 assert p.returncode==130,(p.returncode,out,ready+err)
 assert not output.exists() and not list(root.glob('response.asset.statistics.*.cbor')) and not list(root.glob('response.asset.source.*.json'))
 count+=1
 if hasattr(os,'mkfifo'):
  fifo=root/'specimen.fifo';os.mkfifo(fifo)
  call(replace={'--specimen':str(fifo)},ok=False)
  call(replace={'--specimen':str(root)},ok=False)
  assert not output.exists() and not list(root.glob('response.asset.statistics.*.cbor')) and not list(root.glob('response.asset.source.*.json'))
print(count,'CLI success/refusal/exact-seed/cancellation controls passed')
