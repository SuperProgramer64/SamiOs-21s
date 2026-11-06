#!/usr/bin/env python3
"""Regression: current UI hooks must pass the exact-address compatibility guard."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "inject_methods.h"
int main(void){
 const struct {const char *cls,*sel;uintptr_t old,now;} rows[]={
 {"SBLockHardwareButton","longPress:",0x1c4f6ab44UL,0x1c5d339b0UL},
 {"SBMainDisplayPolicyAggregator","_allowsCapabilityPosterSwitcherWithExplanation:",0x1c507e0fcUL,0x1c5e46f8cUL},
 {"SBAppSwitcherPageView","_createOrDestroyWallpaperOverlayView",0x1c4af7860UL,0x1c58c0460UL},
 {"UIDevice","batteryLevel",0x1897021b0UL,0x18970b8bcUL},
 {"UIDevice","batteryState",0x189209794UL,0x189212b64UL},
 {"_UIBatteryView","showsPercentage",0x189a9370cUL,0x189a9d1c8UL},
 {"_UIBatteryView","setShowsPercentage:",0x18926e150UL,0x189277520UL},
 {"_UIBatteryView","_shouldShowBolt",0x189203e28UL,0x18920d1f8UL},
 {"_UIBatteryView","chargePercent",0x189a935d8UL,0x189a9d094UL},
 {"_UIBatteryView","setChargePercent:",0x18928d310UL,0x1892966e0UL},
 {"CSBatteryChargingRingView","batteryChargingLevel",0x1c48c2cf4UL,0x1c568bd24UL},
 };
 for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);i++){
  assert(hakios_method_matches('-',rows[i].cls,rows[i].sel,rows[i].old,rows[i].now));
  assert(!hakios_method_matches('+',rows[i].cls,rows[i].sel,rows[i].old,rows[i].now));
  assert(!hakios_method_matches('-',rows[i].cls,rows[i].sel,rows[i].old,rows[i].now+4));
  assert(!hakios_method_matches('-',"Other",rows[i].sel,rows[i].old,rows[i].now));
 }
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'check.c').write_text(src)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root/'userland/src'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
print('Eleven current UI methods: expected class, instance-kind and address accepted; mismatches rejected: PASS')
