#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run mkjobs on a valid job whose numeric throttle arrives as a plist string."""
import os,pathlib,plistlib,subprocess,tempfile

script=pathlib.Path(__file__).resolve().parents[1]/'userland/mkjobs.py'
with tempfile.TemporaryDirectory() as tmp:
    d=pathlib.Path(tmp);source=d/'system';jobs=source/'System/Library/LaunchDaemons'
    jobs.mkdir(parents=True);(source/'usr/bin').mkdir(parents=True)
    (source/'usr/bin/probe').write_text('')
    for label,value in (('org.hakios.probe.string','3'),('org.hakios.probe.integer',5)):
        job={'Label':label,'ProgramArguments':['/usr/bin/probe'],
             'ThrottleInterval':value,'MachServices':{label:True}}
        (jobs/(label+'.plist')).write_bytes(plistlib.dumps(job))
    for rest in (False,True):
        out=d/('rest' if rest else 'named')
        command=['python3',str(script),str(source),str(out)]
        if rest:command.insert(2,'--rest')
        else:command+=['org.hakios.probe.string','org.hakios.probe.integer']
        subprocess.run(command,check=True,capture_output=True)
        for label,wanted in (('org.hakios.probe.string',3),('org.hakios.probe.integer',5)):
            result=plistlib.loads((out/(label+'.plist')).read_bytes())
            assert type(result['ThrottleInterval']) is int and result['ThrottleInterval']==wanted,result
            assert result['ProgramArguments']==['/usr/bin/probe']
    print('mkjobs: explicit and on-demand jobs normalize numeric throttle strings; integer values retained PASS')
