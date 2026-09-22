/*
 * fpbootd_spike_test_harness.c
 *
 * Standalone test harness for FpbootdSpike.bundle -- loads the built
 * bundle with dlopen(), manually calls AuthorizationPluginCreate(),
 * MechanismCreate(), and MechanismInvoke() with fake-but-functional
 * callback stubs, exactly mimicking what a real pluginhost process
 * would do. This is a genuinely ordinary command-line program you run
 * from Terminal -- it NEVER touches system.login.console, NEVER
 * registers anything, and has nothing to do with a real login attempt.
 * Its only purpose is proving the plugin loads, runs, connects to
 * fpbootd, and calls SetResult(Allow) correctly, before that code is
 * ever trusted anywhere near a real login flow.
 *
 * Usage:
 *   ./fpbootd_spike_test_harness [path-to-bundle-executable]
 * Defaults to FpbootdSpike.bundle/Contents/MacOS/fpbootd_spike if no
 * argument given.
 *
 * While this runs, fpbootd's own log is worth watching in a second
 * terminal to confirm the daemon side saw the connection too:
 *   tail -f /Library/Logs/fpbootd.log
 * And the plugin's own syslog output:
 *   log stream --predicate 'process == "fpbootd_spike_test_harness"'
 */

#include <dlfcn.h>
#include <Security/Authorization.h>
#include <Security/AuthorizationPlugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef OSStatus (*AuthorizationPluginCreateFn)(const AuthorizationCallbacks *callbacks,
                                                 AuthorizationPluginRef *outPlugin,
                                                 const AuthorizationPluginInterface **outPluginInterface);

static OSStatus harness_SetResult(AuthorizationEngineRef inEngine, AuthorizationResult inResult) {
    (void)inEngine;
    const char *name = "UNKNOWN";
    switch (inResult) {
        case kAuthorizationResultAllow: name = "kAuthorizationResultAllow"; break;
        case kAuthorizationResultDeny: name = "kAuthorizationResultDeny"; break;
        case kAuthorizationResultUndefined: name = "kAuthorizationResultUndefined"; break;
        case kAuthorizationResultUserCanceled: name = "kAuthorizationResultUserCanceled"; break;
    }
    printf("[harness] SetResult called with: %s (%d)\n", name, (int)inResult);
    if (inResult != kAuthorizationResultAllow) {
        printf("[harness] *** WARNING: plugin returned something other than Allow. ***\n");
        printf("[harness] *** The safety guarantee this whole design depends on failed. ***\n");
    }
    return noErr;
}

static OSStatus harness_RequestInterrupt(AuthorizationEngineRef inEngine) {
    (void)inEngine;
    printf("[harness] RequestInterrupt called (unexpected for this plugin)\n");
    return noErr;
}

static OSStatus harness_DidDeactivate(AuthorizationEngineRef inEngine) {
    (void)inEngine;
    printf("[harness] DidDeactivate called\n");
    return noErr;
}

static OSStatus harness_GetContextValue(AuthorizationEngineRef inEngine, AuthorizationString inKey,
                                         AuthorizationContextFlags *outContextFlags,
                                         const AuthorizationValue **outValue) {
    (void)inEngine; (void)outContextFlags;
    printf("[harness] GetContextValue called for key: %s (returning none)\n", inKey);
    *outValue = NULL;
    return errAuthorizationInternal;
}

static OSStatus harness_SetContextValue(AuthorizationEngineRef inEngine, AuthorizationString inKey,
                                         AuthorizationContextFlags inContextFlags,
                                         const AuthorizationValue *inValue) {
    (void)inEngine; (void)inContextFlags; (void)inValue;
    printf("[harness] SetContextValue called for key: %s\n", inKey);
    return noErr;
}

static OSStatus harness_GetHintValue(AuthorizationEngineRef inEngine, AuthorizationString inKey,
                                      const AuthorizationValue **outValue) {
    (void)inEngine;
    printf("[harness] GetHintValue called for key: %s (returning none)\n", inKey);
    *outValue = NULL;
    return errAuthorizationInternal;
}

static OSStatus harness_SetHintValue(AuthorizationEngineRef inEngine, AuthorizationString inKey,
                                      const AuthorizationValue *inValue) {
    (void)inEngine; (void)inValue;
    printf("[harness] SetHintValue called for key: %s\n", inKey);
    return noErr;
}

static OSStatus harness_GetArguments(AuthorizationEngineRef inEngine,
                                      const AuthorizationValueVector **outArguments) {
    (void)inEngine;
    printf("[harness] GetArguments called (returning none)\n");
    *outArguments = NULL;
    return noErr;
}

static OSStatus harness_GetSessionId(AuthorizationEngineRef inEngine,
                                      AuthorizationSessionId *outSessionId) {
    (void)inEngine;
    printf("[harness] GetSessionId called\n");
    *outSessionId = 0;
    return noErr;
}

int main(int argc, char **argv) {
    const char *bundle_exec_path = (argc > 1) ? argv[1]
        : "FpbootdSpike.bundle/Contents/MacOS/fpbootd_spike";

    printf("=== Fpbootd Spike Standalone Test Harness ===\n");
    printf("Loading: %s\n\n", bundle_exec_path);

    void *handle = dlopen(bundle_exec_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen() failed: %s\n", dlerror());
        return 1;
    }

    AuthorizationPluginCreateFn createFn =
        (AuthorizationPluginCreateFn)dlsym(handle, "AuthorizationPluginCreate");
    if (!createFn) {
        fprintf(stderr, "dlsym(AuthorizationPluginCreate) failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }
    printf("[harness] Found AuthorizationPluginCreate, calling it...\n");

    AuthorizationCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.version = kAuthorizationCallbacksVersion;
    callbacks.SetResult = harness_SetResult;
    callbacks.RequestInterrupt = harness_RequestInterrupt;
    callbacks.DidDeactivate = harness_DidDeactivate;
    callbacks.GetContextValue = harness_GetContextValue;
    callbacks.SetContextValue = harness_SetContextValue;
    callbacks.GetHintValue = harness_GetHintValue;
    callbacks.SetHintValue = harness_SetHintValue;
    callbacks.GetArguments = harness_GetArguments;
    callbacks.GetSessionId = harness_GetSessionId;

    AuthorizationPluginRef plugin = NULL;
    const AuthorizationPluginInterface *pluginInterface = NULL;

    OSStatus status = createFn(&callbacks, &plugin, &pluginInterface);
    if (status != noErr || !plugin || !pluginInterface) {
        fprintf(stderr, "[harness] AuthorizationPluginCreate FAILED: status=%d\n", (int)status);
        dlclose(handle);
        return 1;
    }
    printf("[harness] AuthorizationPluginCreate succeeded.\n\n");

    printf("[harness] Calling MechanismCreate...\n");
    AuthorizationMechanismRef mechanism = NULL;
    /* Fake, non-NULL "engine" ref -- safe here specifically because
     * neither our plugin code nor this harness's own callback stubs
     * ever dereference it; it's only ever passed through opaquely. */
    status = pluginInterface->MechanismCreate(plugin, (AuthorizationEngineRef)0x1,
                                               "fpbootd-spike-test", &mechanism);
    if (status != noErr || !mechanism) {
        fprintf(stderr, "[harness] MechanismCreate FAILED: status=%d\n", (int)status);
        pluginInterface->PluginDestroy(plugin);
        dlclose(handle);
        return 1;
    }
    printf("[harness] MechanismCreate succeeded.\n\n");

    printf("[harness] Calling MechanismInvoke -- this is the real test.\n");
    printf("[harness] Watch a second terminal running:\n");
    printf("[harness]   tail -f /Library/Logs/fpbootd.log\n");
    printf("[harness] to confirm fpbootd itself saw the connection.\n\n");

    time_t start = time(NULL);
    status = pluginInterface->MechanismInvoke(mechanism);
    time_t elapsed = time(NULL) - start;

    printf("\n[harness] MechanismInvoke returned: status=%d, took ~%lds\n", (int)status, (long)elapsed);

    printf("\n[harness] Cleaning up...\n");
    pluginInterface->MechanismDestroy(mechanism);
    pluginInterface->PluginDestroy(plugin);
    dlclose(handle);

    printf("\n=== Done ===\n");
    return 0;
}
