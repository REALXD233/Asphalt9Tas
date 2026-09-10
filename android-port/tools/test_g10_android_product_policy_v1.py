#!/usr/bin/env python3
"""Offline identity and scope gate for the G10 Android product shell."""

from __future__ import annotations

import hashlib
import json
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "A9TasAndroid"
MAIN = PROJECT / "app" / "src" / "main"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()[:24]
    require(data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR",
            f"invalid PNG launcher asset: {path}")
    return struct.unpack(">II", data[16:24])


def java_block(source: str, signature: str) -> str:
    """Return one Java method body, including its braces."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated Java block: {signature}")


def main() -> int:
    product_build = (ROOT / "build-g10-android-product-v1.ps1").read_text("utf-8")
    require("sync-native-arm64-runtime-assets-v1.ps1" in product_build and
            "Native ARM64 runtime asset synchronization failed" in product_build,
            "Android product build can package stale ARM64 runtime artifacts")
    require("build-android-identity-probe-v1.ps1" in product_build and
            "build-practice-mode-readonly-probe-v1.ps1" in product_build,
            "Android product build can package stale architecture probes")
    arm64_build_scripts = (
        "build-native-arm64-early-loader-v1.ps1",
        "build-native-arm64-g4-controller-v1.ps1",
        "build-native-arm64-g8-readonly-observer-v1.ps1",
        "build-g4-multi-hook-runtime-v1.ps1",
        "build-android-identity-probe-v1.ps1",
        "build-practice-mode-readonly-probe-v1.ps1",
    )
    for script_name in arm64_build_scripts:
        script = (ROOT / script_name).read_text("utf-8")
        require("max-page-size=16384" in script and
                "common-page-size=16384" in script and
                "Assert-A9TasElfLoadAlignment" in script,
                f"ARM64 16 KB ELF alignment gate missing: {script_name}")
    require("[switch]$DeveloperTest" in product_build and
            "[switch]$NoBuiltInProfiles" in product_build and
            "$productVersionCode = if ($NoBuiltInProfiles) { 34 } elseif ($DeveloperTest) { 33 } else { 32 }" in product_build and
            "'0.8.0-profile-autogen-empty-devtest'" in product_build and
            "'0.8.0-profile-autogen-devtest'" in product_build and
            "'0.8.0-profile-autogen'" in product_build and
            "--version-code $productVersionCode" in product_build and
            "--version-name $productVersionName" in product_build,
            "manual APK packager version drifted from the product identity")
    for token in ("DeveloperTest must remain debug-signed",
                  "a9tas-0.8.0-profile-autogen-devtest.apk", "developer_test=",
                  "dev.a9tas.android.devtest", "A9 TAS DEV",
                  "--custom-package dev.a9tas.android"):
        require(token in product_build, f"developer test packaging boundary missing: {token}")
    for token in ("NoBuiltInProfiles is a developer-test-only build mode",
                  "a9tas-0.8.0-profile-autogen-empty-devtest.apk",
                  "dev.a9tas.android.autogentest", "A9 TAS AUTO",
                  "assets/runtime/a9tas_arm64_profile_autogen_v1",
                  "NoBuiltInProfiles APK unexpectedly contains a game BuildProfile",
                  "NoBuiltInProfiles APK registry is not empty", "built_in_profiles="):
        require(token in product_build,
                f"empty-Profile developer packaging boundary missing: {token}")
    for token in ("[ValidateSet('Debug','Release')]", "ReleaseKeystorePath",
                  "ReleaseKeyAlias", "A9TAS_RELEASE_STORE_PASSWORD",
                  "A9TAS_RELEASE_KEY_PASSWORD", "--ks-pass env:",
                  "--key-pass env:", "Release keystore must live outside the source workspace",
                  "Release artifact is signed by a debug certificate",
                  "signer_cert_sha256"):
        require(token in product_build, f"release signing invariant missing: {token}")
    signing_runbook = (ROOT / "RELEASE_SIGNING_RUNBOOK.md").read_text("utf-8")
    for token in ("-AsSecureString", "ZeroFreeBSTR", "two encrypted offline backups",
                  "A9TAS_RELEASE_STORE_PASSWORD", "signer_cert_sha256"):
        require(token in signing_runbook, f"release signing runbook invariant missing: {token}")
    require("build_android_icon_v2.ps1" in product_build and
            "Android launcher icon generation failed" in product_build,
            "product build does not regenerate launcher icons from its master")
    icon_builder = (ROOT / "tools/build_android_icon_v2.ps1").read_text("utf-8")
    require("a9-tas-icon-black.png" in icon_builder and "mark=A9" in icon_builder and
            "[switch]$UseExistingMaster = $true" in icon_builder and
            "$size * 0.15" in icon_builder,
            "provided A9 artwork or adaptive safe inset missing")
    manifest = ET.parse(MAIN / "AndroidManifest.xml").getroot()
    android = "{http://schemas.android.com/apk/res/android}"
    application = manifest.find("application")
    require(application is not None, "missing application")
    require(application.get(android + "icon") == "@mipmap/ic_launcher" and
            application.get(android + "roundIcon") == "@mipmap/ic_launcher_round",
            "adaptive and round product icons are not both configured")
    icon_master = PROJECT / "design" / "a9-tas-icon-black.png"
    require(icon_master.is_file() and png_size(icon_master)[0] >= 1024 and
            png_size(icon_master)[0] == png_size(icon_master)[1],
            "high-resolution square icon master is missing")
    for density, legacy_size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                                 ("xxhdpi", 144), ("xxxhdpi", 192)):
        directory = MAIN / "res" / f"mipmap-{density}"
        require(png_size(directory / "ic_launcher.png") ==
                (legacy_size, legacy_size),
                f"legacy launcher size drifted for {density}")
        require(png_size(directory / "ic_launcher_round.png") ==
                (legacy_size, legacy_size),
                f"round launcher size drifted for {density}")
        adaptive_size = round(legacy_size * 2.25)
        require(png_size(directory / "ic_launcher_foreground.png") ==
                (adaptive_size, adaptive_size),
                f"adaptive foreground size drifted for {density}")
    services = application.findall("service")
    require(len(services) == 1, "exactly one private foreground service required")
    require(services[0].get(android + "exported") == "false", "service must be private")

    artifact_manifest = json.loads((MAIN / "assets/runtime/manifest.json").read_text("utf-8"))
    require(artifact_manifest["schema"] == 2, "artifact schema")
    artifact_sets = artifact_manifest["artifact_sets"]
    require(artifact_sets, "empty runtime backend registry")
    artifacts = []
    environment_keys: set[tuple[str, str, str, str]] = set()
    backend_ids: set[str] = set()
    for backend in artifact_sets:
        require(isinstance(backend.get("enabled", True), bool),
                "invalid runtime enabled flag")
        require(isinstance(backend.get("accepted", True), bool),
                "invalid runtime accepted flag")
        require(not backend.get("accepted", True) or backend.get("enabled", True),
                "accepted runtime backend must be enabled")
        binding = backend.get("libc_binding", "exact")
        require(binding in {"exact", "dynamic"}, "invalid runtime libc binding")
        require(binding != "dynamic" or
                (backend["host_machine"] == "arm64" and
                 backend["bridge_set"] == "none" and
                 backend.get("payload_staging") == "target_app_cache"),
                "dynamic libc binding escaped native ARM64 private staging")
        require(backend["id"] not in backend_ids, "duplicate runtime backend id")
        backend_ids.add(backend["id"])
        key = (backend["host_machine"], backend["bridge_set"],
               binding, backend["libc_sha256"] if binding == "exact" else "dynamic")
        require(key not in environment_keys, "ambiguous runtime backend identity")
        environment_keys.add(key)
        require(re.fullmatch(r"[0-9a-f]{64}", backend["libc_sha256"]) is not None,
                "invalid runtime libc identity")
        names = {item["device_name"] for item in backend["artifacts"]}
        require({"carrier", "controller", "observer", "payload", "practice_probe"} <=
                backend["roles"].keys(), "missing mandatory runtime role")
        require(set(backend["roles"].values()) <= names,
                "runtime role is not backed by an artifact")
        practice_probe = backend["roles"]["practice_probe"]
        expected_arch = "arm64" if backend["host_machine"] == "arm64" else "x86_64"
        require(expected_arch in practice_probe,
                "practice probe architecture does not match its runtime backend")
        for item in backend["artifacts"]:
            artifacts.append(item)
            path = MAIN / "assets" / item["asset"]
            require(path.is_file(), f"missing asset {path}")
            require(sha256(path) == item["sha256"], f"asset drift {path.name}")
            require(item["mode"] in {"0644", "0700"}, "invalid fixed mode")

    profiles = json.loads((MAIN / "assets/profiles/registry.json").read_text("utf-8"))
    require(profiles["schema"] == 1 and profiles["profiles"], "empty profile registry")
    native_hashes: set[str] = set()
    for profile in profiles["profiles"]:
        require(re.fullmatch(r"[0-9a-f]{64}", profile["native_sha256"]) is not None,
                "invalid native hash")
        require(profile["native_sha256"] not in native_hashes, "duplicate native hash")
        native_hashes.add(profile["native_sha256"])
        path = MAIN / "assets" / profile["profile_asset"]
        require(path.is_file() and sha256(path) == profile["profile_sha256"],
                "profile asset mismatch")
        practice = profile.get("practice_proof", {})
        proof_keys = {"vptr0_rva", "vptr588_rva", "vptr6c0_rva",
                      "vptr718_rva", "vptr748_rva"}
        require(set(practice) == proof_keys,
                "profile must carry the exact five-vptr PracticeRace proof")
        proof_values = []
        for key in proof_keys:
            value = int(practice[key], 0)
            require(value > 0 and value % 8 == 0,
                    f"invalid aligned PracticeRace vptr RVA: {key}")
            proof_values.append(value)
        require(len(set(proof_values)) == len(proof_values),
                "PracticeRace vptr RVAs must be distinct")

    java_root = MAIN / "java/dev/a9tas/android"
    sources = {path.name: path.read_text("utf-8") for path in java_root.glob("*.java")}
    joined = "\n".join(sources.values())
    require("/proc/[0-9]*" in sources["GameProcessScanner.java"], "missing process enumeration")
    require("libAsphalt9\\\\.so" in sources["GameProcessScanner.java"], "missing exact library scan")
    for token in ("A9TAS_SCAN_V2", "app_count", "-ge 10000",
                  "scanProgress(result.output)", "scan_one", "fallback",
                  "RootShell.runFixedScript(FIXED_SCAN_SCRIPT, 45L)"):
        require(token in sources["GameProcessScanner.java"],
                f"missing bounded Android 16 process-scan behavior: {token}")
    require("recoverHashes" in sources["GameProcessScanner.java"] and
            "A9TAS_HASH_V1" in sources["GameProcessScanner.java"],
            "missing bundled live library hash")
    require("startTicks" in sources["GameProcessScanner.java"], "missing PID reuse identity")
    require("registry.find(sha)" in sources["GameProcessScanner.java"], "missing profile selection")
    for token in ("runtimeSupported()", "runtimeRegistry.find(", "UNKNOWN_SHA",
                  "nativeIdentityAvailable()", "libcIdentityAvailable()",
                  "identityHelperFor", "--expect", "0300) a=x86",
                  "2800) a=arm", "s=${20}", "lc=${lm#* /}",
                  'line.split("\\\\t", -1)',
                  "native ARM64 · adapter pending", "libhoudini",
                  "libndk_translation", "bridgeSet",
                  "A9TAS_RUNTIME_V1"):
        require(token in sources["GameProcessScanner.java"],
                f"missing runtime-environment compatibility gate: {token}")
    runtime_registry = sources["ArtifactRegistry.java"]
    for token in ("artifact_sets", "class Backend", "environmentKeys",
                  "if (match != null) return null",
                  "boolean selectable()", "if (!backend.selectable()) continue",
                  "requiresLibcIdentity()", '"dynamic".equals(libcBinding)',
                  "carrierDeviceName", "controllerDeviceName",
                  "observerDeviceName", "payloadDeviceName",
                  "bootstrapDeviceName", "payloadStaging", "optionalRole"):
        require(token in runtime_registry,
                f"missing data-driven runtime backend invariant: {token}")
    native_candidates = [item for item in artifact_sets
                         if item["host_machine"] == "arm64"]
    require(len(native_candidates) == 1 and
            native_candidates[0].get("enabled") is True and
            native_candidates[0].get("accepted") is True and
            native_candidates[0].get("libc_binding") == "dynamic" and
            native_candidates[0]["libc_sha256"] ==
            "3e2b2d8e1fca7d528cd4af6d38bfea49743aeab6788cdbe4ba944786de178263",
            "verified Android 7 native ARM64 backend is not dynamically accepted")
    require("(!backend.requiresLibcIdentity() || libcIdentityAvailable())" in
            sources["GameProcessScanner.java"],
            "native ARM64 selection still requires an exact libc file identity")
    require("prepared_runtime_backend" in joined and
            "EXTRA_RUNTIME_BACKEND, backend.id" in sources["MainActivity.java"],
            "runtime backend identity is not persisted end-to-end")
    require("supported()" in sources["MainActivity.java"], "missing incompatible-build refusal")
    for token in ("experimental_identity_bypass", "selectedExperimentalProfile",
                  "findExperimental", "EXTRA_EXPERIMENTAL_BYPASS"):
        require(token in joined, f"missing explicit experimental bypass invariant: {token}")
    require("EXTRA_NATIVE_SHA, candidate.nativeSha256" in sources["MainActivity.java"] and
            "experimental ? profile.nativeSha256" not in sources["MainActivity.java"],
            "experimental mode must not forge an observed game-library hash")
    require("libAsphalt9" in sources["GameProcessScanner.java"],
            "experimental selection must remain scoped to mapped A9 processes")
    require("loadAndVerify" in sources["MainActivity.java"], "missing asset verification")
    deployer = sources["ArtifactDeployer.java"]
    for token in ("am force-stop", "carrierReceiptPrefix", "TracerPid:",
                  "libAsphalt9[.]so", "--expect", "identityHelperFor",
                   "target_app_cache", "command -v restorecon >/dev/null 2>&1",
                   "restorecon -RFD", "$payload_target", "payload_pending",
                   "mv -f",
                    "G10_PREPARE passed=1",
                   "G10_STAGE artifacts_verified", "G10_STAGE force_stop",
                   "G10_STAGE old_task_settled", "G10_STAGE arm_carrier",
                   "G10_STAGE identity_receipt", "G10_STAGE identity_bound",
                   "receipt_pid", "receipt_start", "a9tas_identity_ready"):
        require(token in deployer, f"missing proven PrepareProcess invariant: {token}")
    require('append(quote(localPayload)).append(" \\"$payload_target\\";")' not in deployer,
            "PrepareProcess must not truncate an executable-mapped payload")
    require("p=$(a9tas_find_pids); set -- $p" not in deployer,
            "post-carrier identity must bind the receipt, not rescan /proc")
    require("sha256sum" not in deployer and
            "sha256sum" not in sources["SessionOrchestrator.java"] and
            "sha256sum" not in sources["GameProcessScanner.java"],
            "Android runtime must not depend on a system sha256sum applet")
    require("awk" not in deployer and
            "awk" not in sources["SessionOrchestrator.java"] and
            "awk" not in sources["GameProcessScanner.java"],
            "Android 7 runtime must not depend on an optional awk applet")
    for token in ("profile.open(context)", "profile.profileSha256",
                  'a9tas_g8_runtime_build_profile_v1.bin'):
        require(token in deployer, f"missing per-build profile deployment: {token}")
    profile_registry = sources["BuildProfileRegistry.java"]
    for token in ("A9_PROFILE_BUNDLE_V1", "A9_IMPORTED_PROFILE_V1",
                  "importBundle", "exportBundle", "profile_base64", "parseProfile",
                  "imported-profiles"):
        require(token in profile_registry,
                f"missing runtime-imported BuildProfile support: {token}")
    for token in ("REQUEST_IMPORT_PROFILE", "REQUEST_EXPORT_PROFILE",
                  "chooseProfileExportDestination", "exportSelectedProfile"):
        require(token in sources["MainActivity.java"],
                f"missing BuildProfile document workflow: {token}")
    for token in ("A9_PROFILE_AUTOGEN_V1", "trap cleanup EXIT HUP INT TERM",
                  "candidate.startTicks", "candidate.libraryPath"):
        require(token in sources["Arm64ProfileAutoGenerator.java"],
                f"missing bounded ARM64 Profile autogeneration invariant: {token}")
    for token in ("profile_autogen_binding", "generatedIdentityBinding",
                  "start_ticks", "library_path", "autogen_session"):
        require(token in sources["GameProcessScanner.java"] or
                token in sources["MainActivity.java"],
                f"missing same-process generated Profile binding: {token}")
    developer_build = sources["DeveloperBuild.java"]
    for token in ('VERSION_SUFFIX = "-devtest"', "EXPIRES_UTC_SECONDS = 1793491200L",
                  "version.endsWith(VERSION_SUFFIX)", "now <= EXPIRES_UTC_SECONDS"):
        require(token in developer_build,
                f"developer test identity/expiry boundary missing: {token}")
    require("if (DeveloperBuild.enabled(context))" in sources["LicenseManager.java"] and
            "if (DeveloperBuild.enabled(context))" in sources["SessionOrchestrator.java"] and
            "practice_proof_bypassed_devtest" in sources["SessionOrchestrator.java"],
            "developer test bypass is not isolated behind product identity")
    require("RootShell.runFixedScript" in deployer, "prepare must use one root transaction")
    root_shell = sources["RootShell.java"]
    for token in ("persistentProcess", "ensurePersistentSession", "LinkedBlockingQueue",
                  "A9TAS_ROOT_BEGIN_", "A9TAS_ROOT_END_", "closePersistent",
                  "abortPersistent()", "process.destroy()", "startRootProcess",
                  '"/system/bin/su"'):
        require(token in root_shell, f"missing single-grant root session invariant: {token}")
    require("ArtifactDeployer.prepare" in sources["TasForegroundService.java"],
            "foreground service does not own preparation")
    service_source = sources["TasForegroundService.java"]
    on_start = java_block(service_source,
                          "@Override public int onStartCommand(Intent intent")
    require("!operationActive.get()" in on_start and
            "DiagnosticBundle.beginOperation(this, action)" in on_start,
            "control signals or duplicate taps must not replace the active diagnostic identity")
    prepare_dispatch_start = service_source.index(
        "} else if (ACTION_START.equals(action)) {")
    prepare_dispatch_end = service_source.index(
        "} else if (ACTION_INSTALL_SESSION.equals(action)) {",
        prepare_dispatch_start)
    prepare_dispatch = service_source[prepare_dispatch_start:prepare_dispatch_end]
    require('.putBoolean("session_hooks_installed", false)' not in prepare_dispatch and
            '.putBoolean("resident_pending_queued", false)' not in prepare_dispatch and
            'runExclusive("prepare"' in prepare_dispatch,
            "Prepare dispatch must acquire exclusive ownership before replacing live session state")
    # Button dispatch is intentionally side-effect free until runExclusive wins
    # its compare-and-set.  Otherwise a duplicate/mistaken tap can overwrite
    # the visible state of the real operation even though no second worker is
    # allowed to run.
    for action_marker, next_marker, exclusive_marker in (
            ("} else if (ACTION_INSTALL_SESSION.equals(action)) {",
             "} else if (ACTION_QUICK_RECORD.equals(action)) {",
             'runExclusive("install"'),
            ("} else if (ACTION_RECORD_LIFECYCLE.equals(action)) {",
             "} else if (ACTION_REPLAY_SELECTED.equals(action)) {",
             'runExclusive("record"'),
            ("} else if (ACTION_REPLAY_SELECTED.equals(action)) {",
             "return START_NOT_STICKY;", 'runExclusive("replay"')):
        dispatch_start = service_source.index(action_marker)
        dispatch_end = service_source.index(next_marker, dispatch_start)
        dispatch = service_source[dispatch_start:dispatch_end]
        require(exclusive_marker in dispatch and "setState(" not in dispatch and
                "DiagnosticBundle.recordFailure" not in dispatch,
                f"{action_marker} must acquire exclusive ownership before preflight/state publication")
    prepare_method = java_block(service_source,
                                "private void prepare(int pid, long startTicks")
    require("ArtifactDeployer.prepare" in prepare_method and
            "SessionOrchestrator.reconcileTerminatedRuntimeState(this)" in
                    prepare_method,
            "failed Prepare must preserve a live predecessor or clear an exactly terminated one")
    orchestrator = sources["SessionOrchestrator.java"]
    # All resident queue markers are process-bound.  A restored, terminated or
    # newly prepared PID must never inherit a host-side pending marker for a
    # payload that no longer exists.  This mirrors upstream ClearQueuedReplay:
    # clear both the queued mode and its input/runtime ownership together.
    for owner_name, owner_source in (
            ("SessionOrchestrator.java", orchestrator),
            ("TasForegroundService.java", sources["TasForegroundService.java"]),
            ("MainActivity.java", sources["MainActivity.java"])):
        require('.putBoolean("resident_pending_queued", false)' in owner_source and
                '.remove("resident_pending_frame_limit")' in owner_source and
                '.putBoolean("record_waiting_retry", false)' in owner_source,
                f"process-bound resident queue cleanup missing: {owner_name}")
    for token in ("runControllerFileReceipt", "runControllerStdoutReceipt",
                  "G10_CONTROLLER_FILE_RECEIPT", "G10_CONTROLLER_STDOUT_RECEIPT",
                  "G4_ACTION action=6 ", "G4_PASSIVE installed=1 ", ".a9pio2",
                  "PhysicsIntervalReceipt.parse", "restoreIfNeeded",
                  "G4_ACTION action=3 ", 'hasToken(receipt, "status=3")',
                  "am force-stop", "session_hooks_installed",
                  "verifyFixedArtifacts", "prepared_ready", "record-life",
                  'identity, "status", owner', "waitForCompletion",
                  "G4_STATUS\\\\s+complete=", "OperationCancelledException",
                   'identity, "dump", owner',
                   "G4_RECORD_DUMP passed=1", "completion=1,2",
                   "dumpCompletedRecordingAfterHelpersSettle",
                   "isCompletedHelperSettleRace",
                   "copyRecordingIntoApp", "A9TasArchive.inspectSource",
                  "G10_RECORD_GAME_FOREGROUND esc=1", "resolveLauncher",
                  "reconcileTerminatedRuntimeState", "G10_RUNTIME_RECONCILE",
                   "clearTerminatedRuntimeState"):
        require(token in orchestrator, f"missing proven session invariant: {token}")
    require('for (int attempt = 0; attempt < 4; attempt++)' in orchestrator and
            'G4_COMPLETE_REJECT code=1 ' in orchestrator and
            'active=([1-9][0-9]*)' in orchestrator and
            'hasToken(receipt, "status=2")' in orchestrator and
            'hasToken(receipt, "error=0")' in orchestrator and
            'hasToken(receipt, "missing=0x0")' in orchestrator and
            'hasToken(receipt, "completion=2/2")' in orchestrator,
            "record dump must retry only the bounded terminal-helper settling race")
    profile_registry = sources["BuildProfileRegistry.java"]
    for token in ("practiceVptr0Rva", "practiceVptr588Rva",
                  "practiceVptr6c0Rva", "practiceVptr718Rva",
                  "practiceVptr748Rva"):
        require(token in profile_registry,
                f"PracticeRace proof is not represented by BuildProfileRegistry: {token}")
    require(orchestrator.count("requirePracticeMode(context, identity);") == 4,
            "every mutating TAS entry point must have exactly one practice-mode gate")
    for signature in (
            "static InstallReceipt installPassive(Context context)",
            "private static RecordReceipt recordLifecycle(Context context,",
            "static ReplayReceipt queueNextRacePrefix(Context context,",
            "static ReplayReceipt replaySelected(Context context, OperationObserver observer,\n"
            "                                        boolean retainAtTarget,\n"
            "                                        boolean gameAlreadyForeground,\n"
            "                                        boolean prearmContinuation)"):
        body = java_block(orchestrator, signature)
        require(body.count("requirePracticeMode(context, identity);") == 1,
                f"missing action-boundary practice-mode gate: {signature}")
    restore_body = java_block(
            orchestrator, "static String restoreIfNeeded(Context context)")
    require("requirePracticeMode" not in restore_body,
            "cleanup/restore must remain available after leaving practice mode")
    practice_body = java_block(
            orchestrator,
            "private static long requirePracticeMode(Context context, Identity identity)")
    for token in ("practiceProbeDeviceName", "practiceVptr0Rva",
                  "practiceVptr588Rva", "practiceVptr6c0Rva",
                  "practiceVptr718Rva", "practiceVptr748Rva",
                  'hasToken(output, "candidates=1")',
                  'hasToken(output, "capped=0")',
                  "仅允许在国服独立“练习”模式的比赛中启用 TAS"):
        require(token in practice_body,
                f"practice-mode gate lost a fail-closed invariant: {token}")
    require("prepared_experimental_bypass" not in practice_body and
            "experimental" not in practice_body,
            "experimental BuildProfile selection must not bypass practice-mode authorization")
    require('hasToken(output, "unreadable_bytes=0")' not in practice_body,
            "unrelated transient heap read failures must not reject an exact unique practice object")
    require('output + ".transport.txt"' in orchestrator and
            'cat " + transport' in orchestrator,
            "controller failure diagnostics must survive a missing file receipt")
    for token in ("provenNoMutationInstallFailure", "artifact_runtime|open_mem|freeze|",
                  "clean_original_precondition",
                  "build_profile_publish", "uncertain=0 detached=1",
                  "if (installAttempted) rollbackOrTerminate"):
        require(token in orchestrator,
                f"missing bounded no-mutation install failure handling: {token}")
    require("preloaded_precondition" not in orchestrator[
                orchestrator.index("private static boolean provenNoMutationInstallFailure"):],
            "a failed preloaded precondition may indicate an existing hook and must restore")
    require('new String[]{"passed=1", "complete=1"' not in orchestrator,
            "G4_STATUS does not carry passed=1; do not reject valid record/replay receipts")
    require(orchestrator.count('new String[]{"complete=1", "ticks=" + ticks') == 2,
            "record and replay must validate the native G4_STATUS completion contract")
    require('identity.packageName.equals(game.getString("package"))' not in orchestrator,
            "replay must not reject a channel package when the exact native build matches")
    require('boolean unknownBuildFallback = experimental &&' in orchestrator and
            'require((unknownBuildFallback ||' in orchestrator and
            'identity.nativeSha.equals(game.getString("native_sha256")))' in orchestrator,
            "runtime fallback must not weaken native-hash replay binding for a known build")
    registry_source = sources["BuildProfileRegistry.java"]
    require("Profile resolve(String nativeSha256, String requestedId, boolean experimental)" in
            registry_source and "Profile exact = find(nativeSha256);" in registry_source and
            "if (exact != null) return exact;" in registry_source,
            "an exact native build must override stale/manual Profile selection")
    require(joined.count("profiles.resolve(nativeSha, profileId, experimental)") >= 3,
            "prepare, runtime identity and archive paths must share exact-first Profile resolution")
    require("A known game build and an unclassified host runtime are independent" in
            sources["TasForegroundService.java"] and
            '.putBoolean("prepared_experimental_bypass", experimental)' in
            sources["TasForegroundService.java"],
            "prepare must keep Profile identity separate from runtime compatibility fallback")
    for token in ("(!experimental &&",
                  "!selected.nativeSha256.equals(profile.nativeSha256)"):
        require(token in joined,
                f"experimental profile override is not carried through the full product path: {token}")
    require("SessionOrchestrator.reconcileTerminatedRuntimeState(this)" in
            sources["MainActivity.java"],
            "scan must reconcile process-bound state left by a terminated game")
    receipt = sources["PhysicsIntervalReceipt.java"]
    for token in ("A9PIO2\\0\\0", "REFERENCE_HOOK0", "REFERENCE_GETTER",
                  "IDENTITY_STABLE", "INTERVAL_STABLE", "SAMPLE_OPTIONS_HEAD_OK",
                  "owner identity was not stable", "selfTest"):
        require(token in receipt, f"missing A9PIO2 decoder invariant: {token}")
    require("ACTION_INSTALL_SESSION" in sources["TasForegroundService.java"],
            "foreground service does not own passive install")
    require("installSessionButton" in sources["MainActivity.java"],
            "session installation is not exposed in product UI")
    require("recordButton" in sources["MainActivity.java"] and
            "ACTION_RECORD_LIFECYCLE" in sources["TasForegroundService.java"],
            "lifecycle recording is not exposed in product UI")
    for token in ("ACTION_QUICK_RECORD", "ACTION_QUICK_REPLAY", "ACTION_CANCEL",
                  "ACTION_RESTORE", "operation_active", "cancel_pending"):
        require(token in joined, f"missing same-process workflow invariant: {token}")
    library = sources["A9TasLibrary.java"]
    for token in ("A9TasArchive.HEADER_SIZE", "A9TasArchive.canonical",
                  "A9TasArchive.inspectSource", "A9TasArchive.inspect(pending)",
                  "raw recording identity changed before packaging",
                  "recording_id", "build_profile_sha256", "target_tick",
                  "destination.write(buffer, 0, count)", "getFD().sync()",
                  "library.duplicate_recording_id", "metadataForLatestRaw",
                  "persistRawMetadataSnapshot", "materializeReplaySource",
                  "materializeTargetPrefix", "target replay prefix identity mismatch",
                  "materialized replay source hash mismatch", "static Entry rename",
                  "static void delete", "recording changed before rename",
                  "recording changed before delete", "Os.rename(",
                  "atomic recording publish failed"):
        require(token in library, f"missing canonical Android A9TAS1 writer invariant: {token}")
    require("java.nio.file" not in joined and "java.time" not in joined,
            "Android 7 build contains an API-26-only Java dependency")
    timestamp = sources["UtcTimestamp.java"]
    for token in ("SimpleDateFormat", "GregorianCalendar", "setLenient(false)",
                  "getTimeInMillis()", "nowSeconds()", "valid(String value)",
                  "selfTest()"):
        require(token in timestamp, f"missing API-24 UTC timestamp invariant: {token}")
    require("UtcTimestamp.selfTest()" in sources["MainActivity.java"],
            "API-24 timestamp self-test is not executed at startup")
    gradle = (PROJECT / "app/build.gradle.kts").read_text("utf-8")
    require("minSdk = 24" in gradle and "versionCode = 32" in gradle and
            'versionName = "0.8.0-profile-autogen"' in gradle,
            "Android 7 product identity is not configured")
    require("CLOCK_PERSIST_INTERVAL_SECONDS = 60L" in
            sources["LicenseManager.java"],
            "license watermark persistence is not rate-limited")
    for token in ("cachedUiLicenseStatus", "cachedUiLicenseUntilElapsed",
                  "uiLicenseStatus()", "+ 15_000L"):
        require(token in sources["MainActivity.java"],
                f"missing bounded UI license verification cache: {token}")
    deployer = sources["ArtifactDeployer.java"]
    for token in ("output.isFile() && expectedSha.equals(sha256(output))",
                  "G10_STAGE stale_session_files_cleaned",
                  "/data/local/tmp/a9tas-g10-replay-session-*"):
        require(token in deployer, f"missing preparation optimization: {token}")
    for token in ("reconcileInstalledVersion(session)",
                  "apk_version_code_seen", "RECOVERY_REQUIRED"):
        require(token in sources["MainActivity.java"],
                f"missing APK-update session reconciliation: {token}")
    orchestrator = sources["SessionOrchestrator.java"]
    for token in ("installed session ownership could not be persisted",
                  "recording/session ownership state could not be persisted",
                  "replay/session ownership state could not be persisted"):
        require(token in orchestrator,
                f"missing durable hook-ownership publication: {token}")
    require('.putBoolean("session_hooks_installed", true).apply()' not in
            orchestrator,
            "installed-hook ownership must not be published asynchronously")
    require("replaySpeedSpinner" in sources["MainActivity.java"] and
            "replay_speed_factor" in sources["MainActivity.java"] and
            "replay_speed_factor" in sources["SessionOrchestrator.java"],
            "fast replay selection is not exposed and persisted")
    for token in ("setupToggleButton", "updateSetupVisibility",
                  "更换游戏或管理许可", "收起游戏与许可设置"):
        require(token in joined,
                f"missing ready-state setup disclosure invariant: {token}")
    root_shell = sources["RootShell.java"]
    require(".isAlive()" not in root_shell and "processAlive(Process process)" in root_shell and
            "process.exitValue()" in root_shell and "IllegalThreadStateException" in root_shell,
            "RootShell process liveness must remain compatible with API 24")
    require("RootShell" not in library and "su " not in library,
            "app-private recording library must not require root")
    record_flow = orchestrator[orchestrator.index("static RecordReceipt recordLifecycle"):]
    require("A9TasLibrary.pack" in record_flow and
            "final boolean retainRuntime = true" in record_flow and
            "restored = true;" in record_flow,
            "successful recording must retain its resident session before packaging")
    for token in ("final boolean retainRuntime = true",
                  '.putString("attempt_draft_path", local.getAbsolutePath())',
                  '.putBoolean("brush_archived_record_ready", true)'):
        require(token in record_flow,
                f"paused checkpoint must publish its full draft without immediate restore: {token}")
    main_activity = sources["MainActivity.java"]
    for token in ("targetTickFromLength", 'targetTickFromLength("500", 800) != 499',
                  'targetTickFromLength("600", 800) != 599',
                  "recoverLatestOrphanAttemptDraft",
                  "promoteRecoveredAttemptDraftAsync",
                  'boolean recoveryRequired = "RECOVERY_REQUIRED".equals(state)',
                  'recordButton.setText(recoveryRequired ? "请先恢复旧会话"'):
        require(token in joined,
                f"missing reversible Tick-length checkpoint workflow: {token}")
    layout_text = (MAIN / "res/layout/activity_main.xml").read_text("utf-8")
    require("按所选长度裁剪副本" in layout_text and
            "回放长度（Tick 数）" in layout_text and
            "重开后自动准备下一局" in layout_text and
            all(token not in layout_text for token in (
                "attemptDraftText", "draftTickCountInput",
                "saveAttemptButton", "discardAttemptButton")),
            "Tick-length actions are not explicit in the product UI")
    overlay = sources["TasOverlayController.java"]
    manifest = (PROJECT / "app/src/main/AndroidManifest.xml").read_text("utf-8")
    for token in ("android.permission.SYSTEM_ALERT_WINDOW", "overlayButton",
                  "ACTION_SHOW_OVERLAY", "ACTION_HIDE_OVERLAY",
                  "ACTION_OVERLAY_CHECKPOINT", "TYPE_APPLICATION_OVERLAY",
                  "TYPE_PHONE", "FLAG_NOT_FOCUSABLE", "snapToEdge"):
        require(token in joined + manifest,
                f"missing game overlay control invariant: {token}")
    require("RootShell" not in overlay and "runController" not in overlay and
            "SMOOTH_REFRESH_MILLIS = 1000L" in overlay and
            "overlayRefreshMillis()" in overlay and
            'getBoolean("control_low_latency", true)' in overlay,
            "overlay must remain a cached-state UI with a bounded selectable refresh rate")
    for token in ("ScrollView", "连续刷圈 · 高频操作", "回放与分段续录",
                  "运行选项", "环境、恢复与高级操作",
                  '"RECORDING".equals(state)', 'preparing ? "准备中"',
                  'failure ? "ERR"', 'failure ? "操作失败"',
                  "registerOnSharedPreferenceChangeListener", "isFailureState",
                  "ACTION_QUICK_REPLAY", "ACTION_BRANCH_RECORD",
                  "ACTION_CHECKPOINT_BRANCH", "回放已保存片段并续录",
                  "cycleReplaySpeed", "replay_pause_at_target",
                  "cycleControlResponseMode", "极速响应", "游戏流畅",
                  'checkpointButton.setText("已接收 · 正在保存…")',
                  'checkpointUiPending ? "保存请求已接收"',
                  "overlayRecordingSpinner", "saveOverlayMetadata",
                  "保存名称与详细信息", "编辑当前录像信息",
                  "metadataContainer", "disableOverlayTextInput",
                  "overlayTargetDraft", "darkSpinnerAdapter",
                  "加载到此 Tick 并暂停检查"):
        require(token in overlay,
                f"missing truthful or complete scrollable overlay surface: {token}")
    require('"RECORD_ARMING".equals(session.getString("state", ""))' in
            sources["TasForegroundService.java"],
            "recording status is published before the first runtime progress receipt")
    require('armText.contains("G4_OBJECT_DIAG main_candidates=0")' in orchestrator and
            "未检测到当前比赛对象" in orchestrator,
            "missing actionable no-race-object failure translation")
    require("observationPollMillis()" in orchestrator and
            "observationPoll * 4L" in orchestrator and
            "now - lastNotification >= 3000L" in sources["TasForegroundService.java"] and
            'getBoolean("control_low_latency", true) ? 250L : 750L' in
                    sources["TasForegroundService.java"] and
            'getBoolean("control_low_latency", true) ? 900L : 2500L' in
                    sources["TasForegroundService.java"] and
            '"control_low_latency", true) ? 250L : 1000L' in
                    sources["TasForegroundService.java"],
            "selectable low-latency versus low-overhead cadence is not enforced")
    require("TextWatcher liveMetadataWriter" in sources["MainActivity.java"] and
            "titleInput.addTextChangedListener(liveMetadataWriter)" in
                    sources["MainActivity.java"] and
            "static Entry editMetadata" in sources["A9TasLibrary.java"],
            "recording names or library metadata are not persisted at their truthful boundary")
    service_source = sources["TasForegroundService.java"]
    overlay_checkpoint = java_block(
        service_source, "private void requestOverlayCheckpoint()")
    require("checkpointAtNextClosedTick" not in overlay_checkpoint and
            "checkpointRequested.compareAndSet(false, true)" in overlay_checkpoint and
            "不会取消暂停" in overlay_checkpoint,
            "save must seal existing ticks without requesting another Tick or ESC")
    observer_source = java_block(
        service_source, "private SessionOrchestrator.OperationObserver observer(String kind)")
    require("checkpointRequested.get()" in observer_source and
            "boolean checkpointRequested()" in observer_source and
            'getBoolean("record_pause_interrupt", true)' in observer_source,
            "explicit saves and automatic pause saves must remain independent")
    exact_checkpoint = java_block(orchestrator,
                                  "static boolean checkpointAtNextClosedTick(Context context)")
    for token in ('"checkpoint-pause"', '"G4_ACTION action=29 "',
                  '"stage=checkpoint_boundary_timeout"',
                  'hasToken(receipt, "detached=1")',
                  'hasToken(receipt, "checkpoint=1")',
                  'hasToken(receipt, "pause=1")',
                  'hasToken(receipt, "release=1")'):
        require(token in exact_checkpoint,
                f"exact closed-Tick checkpoint transport missing: {token}")
    require('checkpoint = hasToken(status, "completion=1,3")' in orchestrator,
            "asynchronous exact checkpoint completion is not classified as a draft")
    for token in ("requestCheckpointBranch", "branchAfterWaitingCancellation",
                  "branchAfterCleanWaitingCancellation", "BRANCH_SWITCHING"):
        require(token in service_source,
                f"missing checkpoint-to-branch handoff invariant: {token}")
    for token in ("auto_retry_record", "waitForRetryCountdown",
                  "record_waiting_retry", "archiveCheckpoint(receipt)",
                  "A9TasLibrary.promoteDraft"):
        require(token in joined,
                 f"missing continuous Retry recording workflow: {token}")
    branch_editor = sources["A9TasBranchEditor.java"]
    continuous_adopt = java_block(branch_editor,
                                  "static A9TasLibrary.Entry adoptContinuous")
    require("A9TasLibrary.promoteDraft" in continuous_adopt and
            "A9TasLibrary.pack" not in continuous_adopt,
            "continuous branch must atomically promote its private draft before packaging")
    for token in ("rejectExactSkippedSourceFrame",
                  "replay-to-record boundary skipped one authoritative physics tick",
                  "MessageDigest.isEqual(actual, skippedNext)"):
        require(token in branch_editor,
                f"continuous branch skipped-frame regression guard missing: {token}")
    for token in ("prearmContinuation ? 2",
                  'hasToken(status, "prefix_ticks=" + replayTicks)',
                  'hasToken(status, "handoff=1")'):
        require(token in orchestrator,
                f"Android atomic replay/record handoff contract missing: {token}")
    require('"branch".equals(kind)' in overlay_checkpoint and
            '"record".equals(kind)' in overlay_checkpoint and
            "checkpointRequested.compareAndSet(false, true)" in overlay_checkpoint,
            "ordinary and continuous recordings must save without a second root waiter")
    payload_source = (ROOT / "src" / "payload_g4_multi_hook_runtime_v1.cpp").read_text(
        "utf-8")
    active_retry_queue = payload_source[
        payload_source.index("bool QueueActiveRecordRetry()"):
        payload_source.index("bool ArmSession()")]
    require("ArmControlValid(false, true)" in active_retry_queue,
            "fresh active recording queue is validated as an inactive control")
    continuous_flow = service_source[
        service_source.index("private void recordLifecycle(boolean autoInstall)"):
        service_source.index("private A9TasLibrary.Entry archiveCheckpoint")]
    require('if (receipt.retryDiscarded)' in continuous_flow and
            'resumePausedRace = false;' in continuous_flow and
            continuous_flow.index('resumePausedRace = false;') <
            continuous_flow.index('continue;'),
            "direct Retry must discard and immediately rearm without resuming by ESC")
    require('if (resumePausedRace)' in record_flow and
            'archivedRearm ? "rearm-record"' in record_flow,
            "rearm-record must bypass the initial paused-race resume path")
    retry_wait = orchestrator[
        orchestrator.index("static void waitForRetryCountdown"):
        orchestrator.index("static void pauseRetryCountdown")]
    for token in ('identity, "wait-retry-pause", owner',
                  "G8_RETRY_WAIT passed=1", 'hasToken(status, "lifecycle=2")',
                  'hasToken(status, "valid=1")', 'hasToken(status, "pause=1")'):
        require(token in retry_wait,
                f"missing resident Retry detect-and-pause invariant: {token}")
    require("Thread.sleep(750L)" not in retry_wait,
            "Retry detection must not return to periodic Java/root polling")
    for token in ('identity, "queue-record", owner',
                  'identity, "wait-activation", owner',
                  'identity, "queue-active-record", owner',
                  'resident_retry_record_armed',
                  'resident_retry_auto_loop'):
        require(token in orchestrator,
                f"missing scan-free resident Retry flow: {token}")
    for token in ("SessionOrchestrator.queueNextRaceRecord",
                  "SessionOrchestrator.waitForDirectRetryRecord",
                  "SessionOrchestrator.queueNextRacePrefix",
                  "publishQueuedBranchAndContinue",
                  'setState("RETRY_QUEUED"'):
        require(token in continuous_flow,
                f"continuous record flow does not use resident Retry activation: {token}")
    resident_prefix = java_block(
        orchestrator, "static ReplayReceipt queueNextRacePrefix(Context context,")
    for token in ('identity, "queue-replay", owner',
                  'identity, "wait-activation", owner',
                  "waitForCompletionPauseAndRearm",
                  '.putBoolean("branch_runtime_prearmed", true)',
                  '.putString("last_replay_archive_sha", selected.archiveSha256)',
                  "requirePracticeMode(context, identity);"):
        require(token in resident_prefix,
                f"missing resident Retry prefix handoff invariant: {token}")
    require("waitForRetryCountdown" not in resident_prefix and
            "input keyevent 111" not in resident_prefix,
            "resident prefix queue must not reintroduce countdown scanning or a pre-replay ESC")
    for token in ("terminal_lifecycle=([0-9]+)",
                  "completionLifecycle == 2 || completionLifecycle == 22",
                  "lifecycle captured at completion"):
        require(token in orchestrator,
                f"missing immutable Retry-versus-finish classification: {token}")
    require('if (preferences.getBoolean("resident_retry_auto_loop", false))' in
            continuous_flow and
            continuous_flow.index(
                'if (preferences.getBoolean("resident_retry_auto_loop", false))') <
            continuous_flow.index("SessionOrchestrator.queueNextRaceRecord"),
            "resident direct-Retry loop must be consumed before one-shot requeue")
    immutable_status_source = (ROOT / "src" /
                               "g4_input_action_controller_v1.cpp").read_text("utf-8")
    require('" completion=%u,%u lifecycle=%u terminal_lifecycle=%u"' in
            immutable_status_source and
            "evidence.reserved[0] >> 56u" in immutable_status_source,
            "native status-file receipt does not publish immutable completion lifecycle")
    record_lifecycle = java_block(
        orchestrator,
        "private static RecordReceipt recordLifecycle(Context context,")
    for token in ('stage=seal_precondition', 'hasToken(sealedText, "uncertain=0")',
                  'hasToken(sealedText, "detached=1")',
                  'hasToken(sealedText, "process_killed=0")',
                  'checkpoint = checkpointSealed && sealedCheckpoint'):
        require(token in record_lifecycle,
                f"missing clean pause-versus-Retry resolution invariant: {token}")
    require(record_lifecycle.index("terminal_lifecycle=([0-9]+)") <
            record_lifecycle.index('String remoteRecording = prefix + ".a9g4r2"'),
            "Retry must be classified before dump/copy/archive publication")
    interrupted_branch = service_source[
        service_source.index("catch (SessionOrchestrator.ReplayInterruptedException interrupted)"):
        service_source.index("catch (SessionOrchestrator.OperationCancelledException cancelled)",
                             service_source.index("catch (SessionOrchestrator.ReplayInterruptedException interrupted)"))]
    require('.putBoolean("branch_pending_continuous", continuous)' in interrupted_branch,
            "interrupted prefix handoff loses continuous-session ownership")
    retry_pause = orchestrator[
        orchestrator.index("static void pauseRetryCountdown"):
        orchestrator.index("private static void waitAfterHandoffArm")]
    require("input keyevent 111" not in retry_pause and "sleep 1" not in retry_pause,
            "compatibility Retry pause boundary must not inject a second ESC")
    for token in ("continuous_load_selected", "scheduleConfiguredNextAttempt",
                  "CONTINUOUS_PREFIX_LOADING", "branchRecord(true)",
                  "freshRecordAfterCleanOperation"):
        require(token in service_source,
                f"missing configurable next-attempt state-machine invariant: {token}")
    branch_continuation = java_block(
        service_source,
        "private void continuePendingBranch(android.content.SharedPreferences preferences,")
    for token in ("SessionOrchestrator.queueNextRacePrefix",
                  "SessionOrchestrator.queueNextRaceRecord",
                  "freshRecordAfterCleanOperation = true"):
        require(token in branch_continuation,
                f"accepted branch does not retain the resident Retry loop: {token}")
    replay_install = java_block(
            service_source, "private void ensureReplaySessionInstalled()")
    require('getBoolean("brush_archived_record_ready", false)' in replay_install and
            "SessionOrchestrator.restoreIfNeeded" not in replay_install and
            "return;" in replay_install,
            "archived brush runtime must be reused instead of restored and relocated")
    session_install = java_block(
            service_source, "private void ensureSessionInstalled()")
    require('getBoolean("prepared_ready", false)' in session_install and
            "请先扫描并准备所选游戏" in session_install and
            session_install.index('getBoolean("prepared_ready", false)') <
            session_install.index("SessionOrchestrator.installPassive"),
            "quick actions must reject an invalidated prepared process before native install")
    replay_flow = java_block(
        orchestrator,
        "static ReplayReceipt replaySelected(Context context, OperationObserver observer,\n"
        "                                        boolean retainAtTarget,\n"
        "                                        boolean gameAlreadyForeground,\n"
        "                                        boolean prearmContinuation)")
    for token in ('archivedRearm ? "rearm-replay" : "replay"',
                  "expectedAction = archivedRearm ? 13 : 9",
                  "armRejectedCleanly", 'hasToken(armText, "uncertain=0")'):
        require(token in replay_flow,
                f"missing archived replay handoff invariant: {token}")
    cancel_before_arm = java_block(
        service_source, "private void cancelBeforeArmIfRequested()")
    require("restoreIfNeeded" not in cancel_before_arm and
            "OperationCancelledException" in cancel_before_arm,
            "cancelling before arm must retain the resident session")
    require("SESSION_REPAIRING" not in service_source and
            "cleanArchivedRearmFailure" not in service_source and
            "restoreCompletedContinuousSessionIfNeeded" not in service_source,
            "clean operation failures must not trigger automatic uninstall/reinstall")
    for token in ("boolean armRejectedCleanly = false",
                  "armAttempted && !armConfirmed && armRejectedCleanly"):
        require(token in record_flow,
                f"clean record-arm rejection must preserve resident hooks: {token}")
    controller_source = (ROOT / "src" / "g4_input_action_controller_v1.cpp").read_text("utf-8")
    payload_source = (ROOT / "src" / "payload_g4_multi_hook_runtime_v1.cpp").read_text("utf-8")
    brake_setter = java_block(payload_source, "G4BrakeSetterEntryV1(void* owner, float* value)")
    steering_setter = java_block(payload_source, "G4SteeringSetterEntryV1(void* owner, float* value)")
    accelerator_setter = java_block(payload_source, "G4AcceleratorSetterEntryV1(void* owner, float* value)")
    for name, setter in (("brake", brake_setter),
                         ("steering", steering_setter),
                         ("accelerator", accelerator_setter)):
        disabled = setter.index(
            "if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0)")
        entry = setter.index("__atomic_fetch_add(&g_evidence.setter_entries")
        require(disabled < entry and "return;" in setter[disabled:entry],
                f"disabled {name} setter must be a pure pass-through before evidence mutation")
    require("setter_last_object" not in brake_setter[:brake_setter.index(
                "if (G4SetterOwnerQualifiedV1(owner))")] and
            "setter_last_object" not in steering_setter[:steering_setter.index(
                "if (G4SetterOwnerQualifiedV1(owner))")],
            "post-completion Retry calls can still corrupt the sealed setter identity")
    dispatcher_setter = java_block(
        payload_source, "G4DispatcherEntryV1(void* owner, std::int64_t* elapsed)")
    resident_rearm_payload = java_block(
        payload_source, "bool RearmArchivedSession(bool replay, bool lifecycle_callback)")
    arm_control = java_block(
        payload_source, "bool ArmControlValid(bool archived_rearm = false,")
    require("std::atomic<std::uint32_t> g_dispatcher_tid" in payload_source and
            "std::atomic<std::uint32_t> g_dispatcher_depth" in payload_source and
            "EnterDispatcher(tid)" in dispatcher_setter and
            "LeaveDispatcher(tid, dispatcher_tracked)" in dispatcher_setter and
            dispatcher_setter.index("EnterDispatcher(tid)") <
            dispatcher_setter.index("G4DispatcherOriginalV1(owner, elapsed)") <
            dispatcher_setter.index("LeaveDispatcher(tid, dispatcher_tracked)"),
            "resident Retry lacks same-thread dispatcher nesting identity")
    require("1u + CurrentDispatcherDepth(Tid())" in resident_rearm_payload and
            "ArmControlValid(true, false, expected_active_helpers)" in
                    resident_rearm_payload and
            "observed_active_helpers != expected_active_helpers" in arm_control and
            "0x100000000ULL" in arm_control,
            "resident Retry helper accounting is not exact or diagnosable")
    require("thread_local" not in payload_source and
            "__emutls_get_address" not in payload_source,
            "NativeBridge payload must not acquire an unproven TLS runtime dependency")
    restore_flow = java_block(payload_source, "bool Restore()")
    require("__atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE)" in restore_flow and
            restore_flow.index("kAllRestored | kAllSetterSlotsRestored") <
            restore_flow.index("__atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE)") <
            restore_flow.index("g_install_state.store(2, std::memory_order_release)") <
            restore_flow.index("__atomic_store_n(&g_evidence.status, kRestored, __ATOMIC_RELEASE)"),
            "a cancelled active run must become reinstallable only after full hook restoration")
    resident_rearm = java_block(
        controller_source, "bool CompletedResidentSessionForRearmValid(")
    require("SealedLifecycleRecordForRearmValid" in resident_rearm and
            "PausedReplayForBranchValid" in resident_rearm and
            "ArchivedBundleMatchesSealedRace" not in controller_source,
            "resident rearm must accept completed record or replay without binding the next replay bytes")
    for token in ("CancelledQueuedPredecessorControl(",
                  "CompleteReceiptValid(cancelled_queue_predecessor,",
                  "RecordedBuffersValid(mem, runtime, cancelled_queue_predecessor,",
                  "cancelled_queue_receipt || current_receipt",
                  "? existing.archived_frame_count",
                  "? existing.archived_interval_count"):
        require(token in controller_source,
                f"cancelled resident queue cannot safely rearm a merged prefix: {token}")
    controller_cancel = java_block(
        controller_source, "} else if (action == Action::kCancelPending) {")
    payload_cancel = java_block(payload_source, "bool CancelPending()")
    for name, cancel in (("controller", controller_cancel),
                         ("payload", payload_cancel)):
        require("const bool completed_record_queue =" in cancel and
                "!completed_record_queue" in cancel,
                f"sealed resident record queue cannot be cancelled by {name}")
    cancelled_rearm = controller_source[
        controller_source.index("bool CancelledQueuedPredecessorControl("):
        controller_source.index("bool RecordedBuffersValid(")]
    require(cancelled_rearm.index("CompletionReason::kManualCheckpoint") <
            cancelled_rearm.index("CompletionPolicy::kRaceLifecycle") and
            "CompletionReason::kFixedFrameLimit" in cancelled_rearm and
            "CompletionPolicy::kFixedFrameLimit" in cancelled_rearm,
            "cancelled queue predecessor semantics are not completion-reason driven")
    target_pause_flow = controller_source[
        controller_source.index("bool target_pause_injected = false"):
        controller_source.index("else if (terminal && action == Action::kWaitForCompletionAndStop)",
                                controller_source.index("bool target_pause_injected = false"))]
    for token in ("target_completion_barrier",
                  "kReplayCompletionBarrierEnabled",
                  "payload holds the exact terminal replay Tick"):
        require(token in target_pause_flow,
                f"target-tick synchronous barrier flow is missing: {token}")
    for forbidden in ("early_pause_requested", "8ull * speed",
                      "stable_early_polls"):
        require(forbidden not in target_pause_flow,
                f"obsolete guessed early-pause flow remains: {forbidden}")
    require("kill(pid, SIGSTOP)" not in target_pause_flow and
            "kill(pid, SIGCONT)" not in target_pause_flow,
            "ordinary target pause must not freeze or bounce the game process")
    require("waiting_control.active_helpers == 0" in controller_source,
            "target-tick pause must wait for the terminal hook to retire")
    for token in ("continuousLoadSelectedCheck", "nextAttemptConfigurable",
                  "targetTickInput.addTextChangedListener"):
        require(token in sources["MainActivity.java"],
                f"missing live next-attempt configuration UI invariant: {token}")
    require("continuousLoadSelectedCheck" in layout_text and
            "continuous_load_selected" in sources["TasOverlayController.java"],
            "next-attempt strategy is not exposed in both product UIs")
    require("recordingSpinner" in sources["MainActivity.java"] and
            "packageLatestRecording" in sources["MainActivity.java"] and
            "selected_archive" in sources["MainActivity.java"],
            "canonical recording library is not exposed in product UI")
    replay_flow = orchestrator[orchestrator.index("static ReplayReceipt replaySelected"):]
    require('identity.profile.buildId.equals(game.getString("build_id"))' in replay_flow and
            'identity.profile.profileSha256.equals' not in replay_flow,
            "replay compatibility must bind the game build, not a mutable tool profile SHA")
    for token in ('identity, armAction, owner',
                  'identity, "status", owner', "waitForCompletion",
                  'identity, "diff", owner',
                  '"G4_ACTION action=" + expectedAction + " "',
                  "G5_REPLAY_DIAGNOSTIC_DUMP passed=1",
                  "selected recording belongs to a different game build",
                  '.putBoolean("session_hooks_installed", true)',
                  '.putBoolean("brush_archived_record_ready", true)'):
        require(token in replay_flow, f"missing frozen Android replay invariant: {token}")
    require("post-replay restore uncertain" not in replay_flow,
            "successful replay must not unload the resident session")
    require("ACTION_REPLAY_SELECTED" in sources["TasForegroundService.java"] and
            "replayButton" in sources["MainActivity.java"],
            "selected canonical replay is not exposed through the private service UI")
    for token in ("renameButton", "deleteButton", "cancelButton", "restoreButton",
                  "operationProgress", "showRenameDialog", "showDeleteDialog"):
        require(token in sources["MainActivity.java"],
                f"missing requested Android experience surface: {token}")
    for token in ("recordingFilterInput", "recordingSortSpinner", "libraryCountText",
                  "bulkDeleteButton", "undoDeleteButton", "showBulkDeleteDialog",
                  "undoLastBulkDelete"):
        require(token in sources["MainActivity.java"],
                f"missing priority recording-library surface: {token}")
    for token in ("static List<Entry> view", "lineageLabel", "static int trashBatch",
                  "static int restoreLastTrashBatch", "lastTrashCount",
                  'new File(context.getFilesDir(), "library-trash")', "Os.rename("):
        require(token in library,
                f"missing priority recording-library invariant: {token}")
    branch_editor = sources["A9TasBranchEditor.java"]
    for token in ('metadata(base, " · 分支 T"', '"branch", baseTargetTick',
                  'metadata(source, " · 截取 T"', '"trim", targetTick',
                  'new JSONObject().put("kind", kind)',
                  '.put("parent_recording_id"', '.put("source_tick"'):
        require(token in branch_editor,
                f"missing immutable branch-lineage invariant: {token}")
    for token in ("initializeColdProcessUi", 'putString("state", "READY")',
                  '"brush_archived_record_ready"', '"开始连续刷圈"'):
        require(token in sources["MainActivity.java"],
                f"missing cold-start or retained brush-session feedback: {token}")
    for token in ("targetTickInput", "replay_target_tick",
                  "replay_target_archive_sha", "last_replay_target_tick",
                  "pauseAtTargetCheck", "replay_pause_at_target",
                  'identity, "wait-pause", owner', "target_pause=1",
                  "REPLAY_PAUSED_AT_TARGET", "openCancellationSignal",
                  "closeCancellationSignal", "activeCancellationSignal",
                  "publishCancellationSignal", "cancelled=1"):
        require(token in joined, f"missing inclusive target-tick replay surface: {token}")
    for token in ("target_barrier=1", "automaticPause",
                  "boolean hardStopAtTarget = false"):
        require(token in joined, f"missing non-freezing branch handoff: {token}")
    dispatcher_flow = java_block(payload_source, "G4DispatcherEntryV1(")
    for token in ("hold_at_replay_completion",
                  "__atomic_fetch_sub(&g_control.active_helpers",
                  "nanosleep(&one_millisecond",
                  "__atomic_load_n(&g_control.completed"):
        require(token in dispatcher_flow,
                f"missing in-process replay completion barrier: {token}")
    for token in ("accelerated_generation",
                  "__atomic_load_n(&g_control.mode, __ATOMIC_ACQUIRE)",
                  "__atomic_load_n(&g_control.generation, __ATOMIC_ACQUIRE)",
                  "__atomic_load_n(&g_control.replay_speed_factor, __ATOMIC_ACQUIRE)"):
        require(token in dispatcher_flow,
                f"accelerated replay can leak into the next record generation: {token}")
    require(dispatcher_flow.index("__atomic_fetch_sub(&g_control.active_helpers") <
            dispatcher_flow.index("while (g_install_state.load"),
            "completion barrier must release its helper receipt before waiting")
    wait_pause = orchestrator[orchestrator.index(
        "private static String waitForCompletionAndPause"):orchestrator.index(
        "private static void throwIfCancelled")]
    require(wait_pause.index("openCancellationSignal") <
            wait_pause.index("runControllerFileReceipt") <
            wait_pause.index("closeCancellationSignal"),
            "target-tick cancellation signal lifetime is not scoped around the blocking wait")
    service = sources["TasForegroundService.java"]
    request_cancel = service[service.index("private void requestCancellation"):service.index(
        "private void startRestore")]
    require(request_cancel.index("cancelRequested.set(true)") <
            request_cancel.index("publishCancellationSignal"),
            "target-tick cancellation must publish only after the Java flag")
    require("partial target replay is not yet supported" not in joined,
            "product still rejects AluTasV2 target-tick replay")
    for token in ("materialized replay fixed-delta changed",
                  "last_replay_fixed_delta_us", "fixedDeltaUs"):
        require(token in replay_flow,
                f"missing recording-bound fixed-delta replay receipt: {token}")
    for token in ("branchAutoHandoffCheck", "branchDelayInput",
                   "branch_auto_handoff", "branch_resume_delay_seconds",
                   "branch_handoff_mode_v2",
                   "branch_pending", "BRANCH_PAUSED", "BRANCH_ARMING",
                   "BRANCH_ARMED_PAUSED", "waitAfterHandoffArm",
                   "onHandoffArmed", "onHandoffResumed",
                   "waitIndefinitelyAtTickZero", "onHandoffArmed(-1)",
                   "verifyContinuousBoundary", "pause interval omitted"):
        require(token in joined,
                f"missing two-stage pause-free branch invariant: {token}")
    record_handoff = orchestrator[orchestrator.index(
        "String armAction = pausedReplayHandoff"):orchestrator.index(
        "String status;")]
    require(record_handoff.index("runControllerFileReceipt") <
            record_handoff.index("waitAfterHandoffArm") <
            record_handoff.index("input keyevent 111"),
            "automatic continuation must arm before its configurable paused buffer")
    require("continuePendingBranch(preferences, automatic ? delaySeconds : 0" in
            sources["TasForegroundService.java"] and
            "true, automatic" in sources["TasForegroundService.java"] and
            "waitAtBranchPoint" not in sources["TasForegroundService.java"],
            "branch continuation must always arm before either manual or timed resume")
    for token in ("ReplayInterruptedException", "replayInterruptionRequested",
                  "ACTION_INTERRUPT_REPLAY", "停止加载并从当前点续录",
                  'putLong("branch_pending_target_tick", target)',
                  'cancelRequested.set(false)'):
        require(token in joined,
                f"missing active-prefix interruption handoff invariant: {token}")
    require("ActiveReplayForBranchValid" in controller_source and
            "RearmFromActiveReplayAtClosedBoundary" in
            (ROOT / "src" / "g3_boundary_adapter_v1.h").read_text("utf-8") and
            "RearmFromActiveReplayAtClosedBoundary" in
            (ROOT / "src" / "g4_g3_adapter_v1.h").read_text("utf-8"),
            "native active-prefix handoff seam is incomplete")
    for token in ('"wait-pause-rearm"', "G4_ACTION action=23 ",
                  "branch_runtime_prearmed", "continuationPrearmed"):
        require(token in joined,
                f"missing one-transaction replay-to-record handoff: {token}")
    require('replaySelected(this, observer("branch"), true,' in
            sources["TasForegroundService.java"] and
            "gameAlreadyForeground, true" in
            sources["TasForegroundService.java"],
            "normal branch path must request native continuation prearm")
    branch_flow = java_block(sources["TasForegroundService.java"],
                             "private void branchRecord(boolean continuous, boolean allowCleanRepair,\n"
                             "                              boolean gameAlreadyForeground)")
    require(branch_flow.index(
                    "catch (SessionOrchestrator.ReplayInterruptedException") <
            branch_flow.index(
                    "catch (SessionOrchestrator.OperationCancelledException"),
            "active replay interruption must be handled before ordinary cancellation")
    require("status.contains(\" evidence=2,\")" in orchestrator and
            "Thread.sleep(10L);" in orchestrator,
            "race-end completion must tolerate the final helper retirement window")
    importer = sources["RecordingImporter.java"]
    for token in ("ACTION_OPEN_DOCUMENT", "REQUEST_IMPORT_RECORDING",
                  "RecordingImporter.importArchive"):
        require(token in sources["MainActivity.java"],
                f"missing user-selected import flow: {token}")
    for token in ("openInputStream", "A9TasArchive.inspect(pending)",
                  "A9TAS1 import exceeds size limit", "recording ID conflicts",
                  "atomic import publish failed", "published import identity mismatch"):
        require(token in importer, f"missing strict SAF import invariant: {token}")
    exporter = sources["RecordingExporter.java"]
    for token in ("ACTION_CREATE_DOCUMENT", "REQUEST_EXPORT_RECORDING",
                  "RecordingExporter.export"):
        require(token in sources["MainActivity.java"],
                f"missing user-selected export flow: {token}")
    for token in ("openFileDescriptor", '"rwt"', "A9TasArchive.inspectSource",
                  "A9TasArchive.inspect(source)",
                  "recording changed before export", "exported document hash mismatch"):
        require(token in exporter, f"missing exact export invariant: {token}")
    require("MANAGE_EXTERNAL_STORAGE" not in joined and
            "WRITE_EXTERNAL_STORAGE" not in joined,
            "broad storage permission is forbidden")
    archive = sources["A9TasArchive.java"]
    for token in ("archive.manifest_hash", "archive.recording_hash",
                  "a9g4r2.interval_ordinal", "a9g4r2.missing_tick_interval",
                  "manifest.recording_identity", "RandomAccessFile"):
        require(token in archive, f"missing Android recording validation: {token}")
    sample = MAIN / "assets/selftest/a9tas1-synthetic.a9tas"
    require(sample.is_file() and sample.stat().st_size > 160, "missing decoder selftest archive")
    require("A9TasArchive.inspect" in sources["MainActivity.java"],
            "Android decoder selftest is not executed")
    startup_source = sources["MainActivity.java"]
    require(all(token in startup_source for token in
                ("A9TAS1_TARGET", "A9PIO2", "TICK_LENGTH", "COLD_RECOVERY_PASS")),
            "combined Android decoder and Tick-length receipt is not persisted")
    for forbidden in ("arbitraryAddress", "writeAddress", "manualPid", "customLibraryPath"):
        require(forbidden not in joined, f"forbidden production surface: {forbidden}")

    diagnostics = sources["DiagnosticBundle.java"]
    for token in ("SCHEMA = 2", "SESSION_KEYS", "installCrashHandler", "beginOperation",
                  "recordState", "recordFailure", "error_id", "root_receipt",
                  "ZipOutputStream", "summary.json", "state-history.jsonl",
                  "state-history.previous.jsonl", "last-crash.txt", "MAX_JOURNAL_BYTES",
                  "compatibility-report.json", "diagnostic_operation_id",
                  "diagnostic bundle changed before export", "exported diagnostic hash mismatch"):
        require(token in diagnostics, f"missing privacy-bounded diagnostics invariant: {token}")
    for forbidden in ("LicenseManager", "license_blob", "licenseInput", "/proc/",
                      "/data/local/tmp", "logcat", "latest_recording\"", "selected_archive\""):
        require(forbidden not in diagnostics,
                f"diagnostic bundle crosses its privacy boundary: {forbidden}")
    main_activity = sources["MainActivity.java"]
    service = sources["TasForegroundService.java"]
    layout_text = (MAIN / "res/layout/activity_main.xml").read_text("utf-8")
    require(all(token in main_activity for token in
                ("REQUEST_EXPORT_DIAGNOSTICS", "diagnosticButton",
                 "chooseDiagnosticDestination", "exportDiagnostics",
                 "Intent.ACTION_CREATE_DOCUMENT", "application/zip",
                 "Intent.ACTION_SEND", "Intent.EXTRA_STREAM", "ClipData.newRawUri",
                 "DiagnosticShareProvider.uriFor", "runCompatibilityPreflight",
                 "DiagnosticBundle.installCrashHandler(this)",
                 'DiagnosticBundle.recordState(this, "UI_OPEN"',
                 'DiagnosticBundle.recordFailure(this, "STARTUP_SELFTEST"')),
            "diagnostic export is not fully exposed through the system document picker")
    require("DiagnosticBundle.installCrashHandler(this)" in service and
            "DiagnosticBundle.beginOperation(this, action)" in service and
            "DiagnosticBundle.recordState(this, state, detail)" in service and
            "DiagnosticBundle.recordFailure(this, operation, error)" in service,
            "foreground operation diagnostics are not fully journaled")
    service_start = java_block(
        service, "@Override public int onStartCommand(Intent intent, int flags, int startId)")
    foreground_index = service_start.index(
        "startForeground(NOTIFICATION_ID, notification);")
    require(foreground_index < service_start.index(
                "DiagnosticBundle.beginOperation(this, action)") and
            all(foreground_index < service_start.index(marker) for marker in
                ('if (ACTION_HIDE_OVERLAY.equals(action))',
                 'if (ACTION_CANCEL.equals(action))',
                 'if (ACTION_RESTORE.equals(action))',
                 'if (ACTION_STOP.equals(action))')),
            "foreground service promotion must precede diagnostics and every early return")
    require(all(token in layout_text for token in
                ("@+id/diagnosticButton", "@+id/diagnosticSaveButton",
                 "@+id/compatibilityButton", "无需 ADB", "只读兼容性预检")),
            "diagnostic export affordance is missing")
    provider_source = sources["DiagnosticShareProvider.java"]
    for token in ("MODE_READ_ONLY", 'if (!\"r\".equals(mode))',
                  "getCanonicalFile", "getPathSegments().size() != 1",
                  "grantUriPermissions"):
        haystack = provider_source if token != "grantUriPermissions" else \
                (MAIN / "AndroidManifest.xml").read_text("utf-8")
        require(token in haystack, f"diagnostic share provider invariant missing: {token}")
    providers = application.findall("provider")
    require(len(providers) == 1 and
            providers[0].get(android + "name") == ".DiagnosticShareProvider" and
            providers[0].get(android + "exported") == "false" and
            providers[0].get(android + "grantUriPermissions") == "true",
            "diagnostic provider must be private and grant-scoped")
    compatibility = sources["CompatibilityReport.java"]
    for token in ("read_only", "preflight_installs_hooks", "preflight_game_state_writes",
                  "GameProcessScanner.scan", "A9COMPAT_V2", "root_uid",
                  "compatibility_supported_count", "experimental_runtime_candidate_count",
                  "findExperimental"):
        require(token in compatibility, f"compatibility preflight invariant missing: {token}")
    for forbidden in ("ptrace", "pwrite", "process_vm_writev", "am force-stop", "kill -"):
        require(forbidden not in compatibility,
                f"compatibility preflight is not read-only: {forbidden}")
    require("DiagnosticBundle.recordRootReceipt" in sources["RootShell.java"],
            "root command receipts are not attached to diagnostic operations")
    recovery = sources["RecoveryPolicy.java"]
    for token in ("requiresRecovery", "!serviceLive && hooksInstalled",
                  "A stale Java operation flag alone cannot have modified the game",
                  "selfTest"):
        require(token in recovery,
                f"cold-process recovery policy invariant missing: {token}")
    for token in ("reconcileColdRuntime(session)", "RecoveryPolicy.selfTest()",
                  "TasForegroundService.isLive()", 'putString("state", "RECOVERY_REQUIRED")',
                  '"RECOVERY_REQUIRED".equals(preferences.getString("state", ""))',
                  'preferences.getBoolean("session_hooks_installed", false)'):
        require(token in main_activity,
                f"cold-process recovery wiring invariant missing: {token}")
    require(main_activity.index("reconcileColdRuntime(session)") <
            main_activity.index("recoverLatestOrphanAttemptDraft(session)") <
            main_activity.index("promoteRecoveredAttemptDraftAsync(session)"),
            "cold-process recovery must precede orphan draft discovery and promotion")
    require("static boolean isLive()" in service,
            "foreground service liveness is not exposed to cold recovery")

    print("G10_ANDROID_PRODUCT_POLICY passed=1 foreground_service=1 "
          f"profiles={len(native_hashes)} runtime_backends={len(artifact_sets)} "
          f"artifacts={len(artifacts)} "
          "profile_by_native_sha=1 dynamic_profile_deploy=1 profile_import_export=1 "
          "cross_channel_replay=1 "
          "a9tas1_file_decoder=1 prepare_process_port=1 "
          "arbitrary_write_api=0 passive_session_port=1 lifecycle_record_port=1 "
          "a9tas1_writer=1 recording_library=1 saf_export=1 "
          "saf_import=1 replay_port=1 replay_live=1 same_process_shortcuts=1 "
          "progress_cancel_resident=1 rename_delete=1 target_tick_pause=1 "
          "library_filter_sort=1 trash_undo=1 branch_lineage=1 "
          "fixed_delta_binding=1 "
          "android7_api24=1 api24_root_process=1 status_receipt_contract=1 "
          "practice_mode_gate=1 privacy_bounded_diagnostics_v2=1 "
          "grant_scoped_share=1 readonly_compatibility_preflight=1 "
          "cold_process_recovery=1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, OSError, ValueError, ET.ParseError) as error:
        print(f"G10_ANDROID_PRODUCT_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
