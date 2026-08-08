/* build_id.h — run identity: `BUILD` fingerprint records + the `LOADER` line.
 * See build_id.c for the record grammar and the qsha256 definition. */
#ifndef PMC_BUILD_ID_H
#define PMC_BUILD_ID_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* pmc_bb's own ASI-loader state, for EmitLoaderIdentity's `self=` token.
 *
 * There is no longer a "stood down" state. It meant "another loader owns the
 * scan, so we covered only what it missed", which required detecting that
 * loader — the thing dropped in v0.6.0 (see "Coexisting with other ASI
 * loaders" in pmc_blackbox.c). Built in means we scan; compiled out means we
 * do not; nothing in between.
 *
 * Values are internal to this binary — the wire format is the token string. */
#define PMC_SELF_ENABLED     0   /* we scanned every search path           */
#define PMC_SELF_DISABLED    1   /* compiled out (-DPMC_DISABLE_ASI_LOADER) */

/**
 * Emit the single `LOADER self=…` line. `self_state` is one of PMC_SELF_*.
 *
 * Reports on pmc_bb's own loader only. Detecting third-party mod loaders is
 * modkit's job: that is a question about the install directory, which modkit
 * owns and can inspect properly, rather than something to guess at from inside
 * the process. The `active=` and `external=` tokens were dropped in v0.6.0 for
 * that reason — see EmitLoaderIdentity in build_id.c.
 *
 * Touches no files at all now, so it is trivially safe to call from DllMain.
 */
void EmitLoaderIdentity(int self_state);

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
