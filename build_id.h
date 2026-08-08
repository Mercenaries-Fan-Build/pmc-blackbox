/* build_id.h — run identity: `BUILD` fingerprint records + the `LOADER` line.
 * See build_id.c for the record grammar and the qsha256 definition. */
#ifndef PMC_BUILD_ID_H
#define PMC_BUILD_ID_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* pmc_bb's own ASI-loader state, for EmitLoaderIdentity's `self=` token. */
#define PMC_SELF_ENABLED     0   /* we scanned every search path              */
#define PMC_SELF_STOOD_DOWN  1   /* dxwrapper owns the scan; we cover the rest */
#define PMC_SELF_DISABLED    2   /* compiled out (-DPMC_DISABLE_ASI_LOADER)    */

/**
 * Emit the single `LOADER active=… self=… external=…` line.
 *
 * `exe_dir` must end in a backslash. `self_state` is one of PMC_SELF_*.
 * Cheap: two GetFileAttributes sweeps, no hashing, no module enumeration.
 * Safe to call from DllMain.
 */
void EmitLoaderIdentity(const char *exe_dir, int self_state);

/**
 * Start the background fingerprint pass that writes the `BUILD` records.
 *
 * Returns immediately; all file I/O happens on the spawned thread AFTER the
 * loader lock is released (CreateThread in DllMain does not run the thread
 * until then). `self` is the DLL's own HINSTANCE, so the record for this
 * module names the bytes that are actually mapped rather than whatever sits
 * at some assumed path. That mapped path is also what identifies the build
 * variant, since each ships under its own filename.
 */
void StartBuildFingerprint(HINSTANCE self);

#endif /* PMC_BUILD_ID_H */
