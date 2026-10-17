#!/usr/bin/env python3
"""Compile the actual native scope predicates and clause-removal mutants.

Synthetic inputs only. This does not execute Metal or qualify an onset run.
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "tests/FireSequenceTest.cpp").read_text()
constant = re.search(r'constexpr const char\* R216CheckpointSHA=.*?;', source).group()
functions = []
for name in ("R216KernelAccepted", "R216RetainedScopeAccepted", "R216AttemptPublicationAccepted"):
    functions.append(re.search(r'bool '+name+r'\([^\n]*(?:\n[^{}]*)?\)\n\t\{.*?\n\t\}',
                               source, re.S).group())
harness = r'''
int main() {
 const std::string old="d580d078ea083ee18032abfb027271ada9d4d1dabab50b35cb6eb7a503258a0e";
 const bool ok=R216RetainedScopeAccepted(10,true,R216CheckpointSHA,true,false)
  &&R216RetainedScopeAccepted(10,true,R216CheckpointSHA,false,true)
  &&!R216RetainedScopeAccepted(8,true,R216CheckpointSHA,true,false)
  &&!R216RetainedScopeAccepted(10,false,R216CheckpointSHA,true,false)
  &&!R216RetainedScopeAccepted(10,true,"changed",true,false)
  &&!R216RetainedScopeAccepted(10,true,R216CheckpointSHA,false,false)
  &&R216AttemptPublicationAccepted(true,true,old)
  &&!R216AttemptPublicationAccepted(true,true,"changed")
  &&R216AttemptPublicationAccepted(false,true,"changed")
  &&R216AttemptPublicationAccepted(true,false,"");
 return ok?0:1;
}
'''
program = '#include <string>\n'+constant+'\n'+'\n'.join(functions)+harness
mutations = {
    "tier": [("tier==10.0", "true"), ("const double tier", "const double")],
    "resume": [("&&resume&&", "&&true&&"), ("const bool resume", "const bool")],
    "checkpoint": [("checkpointSHA==R216CheckpointSHA", "true"), ("const std::string& checkpointSHA", "const std::string&")],
    "mode": [("(continuation||probe)", "true"), ("const bool continuation,const bool probe", "const bool,const bool")],
    "kernel": [("||R216KernelAccepted(kernel)", "||(R216KernelAccepted(kernel)||!kernel.empty())")],
}
with tempfile.TemporaryDirectory(prefix="rise-r216-scope-mutants-") as temporary:
    root = Path(temporary)
    for name, replacement in [("green", None), *mutations.items()]:
        text = program
        for before, after in replacement or []:
            if before not in text:
                raise AssertionError("missing native clause: "+before)
            text = text.replace(before, after)
        if replacement is not None and text == program:
            raise AssertionError("mutant did not change predicate: "+name)
        cpp, executable = root / (name+".cpp"), root / name
        cpp.write_text(text)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)], check=True)
        status = subprocess.run([str(executable)]).returncode
        if status != (0 if name == "green" else 1):
            raise AssertionError("unexpected scope mutant result: "+name)
        print("R216_COMPILED_SCOPE "+name+" status="+str(status))
