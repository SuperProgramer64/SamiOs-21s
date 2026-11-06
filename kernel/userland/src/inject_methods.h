/* MIT. Guarded method compatibility for the two userland builds used by this port.
 * 20H392 addresses and ObjC signatures checked against the user's own cache.
 * Never accept an arbitrary implementation just because a selector exists.
 */
#ifndef HAKIOS_INJECT_METHODS_H
#define HAKIOS_INJECT_METHODS_H
static int hakios_method_matches(char kind, const char *cls, const char *sel,
    uintptr_t expected, uintptr_t actual)
{
    static const struct { char kind; const char *cls, *sel; uintptr_t old, current; } pairs[]={
        {'+',"BWCoreImageFilterRenderer","prewarmPortraitFilterVersion:semanticStyleFilters:",0x1a2115c0cUL,0x1a2165b8cUL},
        {'+',"MCMUserIdentityCache","personasAreSupported",0x1c3ea6298UL,0x1c4c699f8UL},
        {'-',"SBLockdownManager","_setupActivationState",0x1c4bc8dfcUL,0x1c5991a78UL},
        {'-',"SBSetupManager","_setSetupRequiredReason:",0x1c4cdd3a4UL,0x1c5aa608cUL},
        {'+',"XBApplicationSnapshotManifestImpl","_outputFormatForSnapshot:",0x1f1c91480UL,0x1f287f320UL},
        {'-',"SBLockHardwareButton","longPress:",0x1c4f6ab44UL,0x1c5d339b0UL},
        {'-',"SBMainDisplayPolicyAggregator","_allowsCapabilityPosterSwitcherWithExplanation:",0x1c507e0fcUL,0x1c5e46f8cUL},
        {'-',"SBAppSwitcherPageView","_createOrDestroyWallpaperOverlayView",0x1c4af7860UL,0x1c58c0460UL},
        {'-',"UIDevice","batteryLevel",0x1897021b0UL,0x18970b8bcUL},
        {'-',"UIDevice","batteryState",0x189209794UL,0x189212b64UL},
        {'-',"_UIBatteryView","showsPercentage",0x189a9370cUL,0x189a9d1c8UL},
        {'-',"_UIBatteryView","setShowsPercentage:",0x18926e150UL,0x189277520UL},
        {'-',"_UIBatteryView","_shouldShowBolt",0x189203e28UL,0x18920d1f8UL},
        {'-',"_UIBatteryView","chargePercent",0x189a935d8UL,0x189a9d094UL},
        {'-',"_UIBatteryView","setChargePercent:",0x18928d310UL,0x1892966e0UL},
        {'-',"CSBatteryChargingRingView","batteryChargingLevel",0x1c48c2cf4UL,0x1c568bd24UL},
    };
    if(actual==expected) return 1;
    for(unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0]);i++)
        if(kind==pairs[i].kind && !strcmp(cls,pairs[i].cls) && !strcmp(sel,pairs[i].sel) &&
            expected==pairs[i].old && actual==pairs[i].current) return 1;
    return 0;
}
#endif
