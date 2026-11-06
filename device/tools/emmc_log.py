#!/usr/bin/env python3
"""Read-only recovery of the bounded HakiOS eMMC console log.

Decode: emmc_log.py --input AREA.bin --output log.txt
Android: emmc_log.py --serial SERIAL --output log.txt
Recovery root shell: add --recovery --lba LBA_FROM_PREVIOUS_PLAN
Only dd if= is used. Never mounts, formats or writes the phone.
"""
import argparse,json,pathlib,struct,subprocess,zlib
BLOCK=4096
SECTORS=8192

def log_layout(region):
 end=region['super_start']+region['first']+region['sectors']
 if end<131072 or end>2**32 or end%8:raise ValueError('invalid aligned xnusys window')
 return dict(lba=end-SECTORS,sectors=SECTORS)

def decode(data):
 if len(data)%BLOCK:raise ValueError('log size must be a multiple of 4096')
 good=[];invalid=0
 for i in range(0,len(data),BLOCK):
  b=bytearray(data[i:i+BLOCK])
  if b[:8]!=b'HAKLOG01':invalid+=1;continue
  seq,run,pos,n,crc=struct.unpack_from('<QQQII',b,8);b[36:40]=bytes(4)
  if not seq or not run or not 0<n<=4032 or zlib.crc32(b)!=crc:invalid+=1;continue
  good.append((seq,run,pos,bytes(b[64:64+n])))
 if not good:raise ValueError('no valid HAKLOG01 records; logger absent, never flushed, or wrong area')
 run=max(good)[1];rows=sorted(r for r in good if r[1]==run)
 chunks=[];end=0
 for seq,boot,pos,b in rows:
  if pos>end:chunks.append(f'\n[HAKIOS: {pos-end} bytes unavailable before stream position {pos}]\n'.encode())
  elif pos<end:chunks.append(f'\n[HAKIOS: non-contiguous record {seq} at {pos}]\n'.encode())
  chunks.append(b);end=pos+len(b)
 return b''.join(chunks).decode('utf-8','replace').replace('\r',''),dict(run=run,records=len(rows),invalid=invalid,last_sequence=rows[-1][0],stream_end=end)

def main():
 ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
 ap.add_argument('--serial');ap.add_argument('--input',type=pathlib.Path);ap.add_argument('--output',required=True,type=pathlib.Path)
 ap.add_argument('--lba',type=int);ap.add_argument('--recovery',action='store_true');a=ap.parse_args()
 if a.input:data=a.input.read_bytes()
 else:
  if not a.serial:ap.error('--serial is required for phone reads')
  if a.recovery and a.lba is None:ap.error('recovery requires the recorded --lba from the Android plan')
  lba=a.lba
  if lba is None:
   import xnusys
   layout=log_layout(xnusys.region(a.serial));lba=layout['lba']
  if lba<0 or lba%8 or lba+SECTORS>2**32:ap.error('invalid aligned log LBA')
  cmd=f'dd if=/dev/block/mmcblk0 bs=4096 skip={lba//8} count=1024 2>/dev/null'
  shell=cmd if a.recovery else "su -c '"+cmd+"'"
  data=subprocess.check_output(['adb','-s',a.serial,'exec-out',shell])
  if len(data)!=4*1024*1024:raise SystemExit(f'short device read: {len(data)} bytes')
  a.output.with_suffix(a.output.suffix+'.bin').write_bytes(data)
 text,meta=decode(data);a.output.write_text(text);a.output.with_suffix(a.output.suffix+'.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta))
if __name__=='__main__':main()
