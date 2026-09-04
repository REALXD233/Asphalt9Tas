package dev.a9tas.android;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Verified, data-driven execution backends. Game-build profiles are kept separate. */
final class ArtifactRegistry {
    static final class IdentityHelper {
        final String hostMachine;
        final String asset;
        final String deviceName;
        final String sha256;

        IdentityHelper(String hostMachine, String asset, String deviceName, String sha256) {
            this.hostMachine = hostMachine;
            this.asset = asset;
            this.deviceName = deviceName;
            this.sha256 = sha256;
        }
    }

    static final class Artifact {
        final String asset;
        final String deviceName;
        final String mode;
        final String sha256;

        Artifact(String asset, String deviceName, String mode, String sha256) {
            this.asset = asset;
            this.deviceName = deviceName;
            this.mode = mode;
            this.sha256 = sha256;
        }
    }

    static final class Backend {
        final String id;
        final String label;
        final boolean enabled;
        final boolean accepted;
        final String hostMachine;
        final String bridgeSet;
        final String libcBinding;
        final String libcSha256;
        final String carrierReceiptPrefix;
        final String payloadStaging;
        final String carrierDeviceName;
        final String controllerDeviceName;
        final String observerDeviceName;
        final String payloadDeviceName;
        final String bootstrapDeviceName;
        final String practiceProbeDeviceName;
        final String profileAutogenDeviceName;
        final List<Artifact> artifacts;

        Backend(String id, String label, boolean enabled, boolean accepted,
                String hostMachine, String bridgeSet,
                String libcBinding, String libcSha256,
                String carrierReceiptPrefix, String carrierDeviceName,
                String payloadStaging,
                String controllerDeviceName, String observerDeviceName,
                String payloadDeviceName, String bootstrapDeviceName,
                String practiceProbeDeviceName,
                String profileAutogenDeviceName,
                List<Artifact> artifacts) {
            this.id = id;
            this.label = label;
            this.enabled = enabled;
            this.accepted = accepted;
            this.hostMachine = hostMachine;
            this.bridgeSet = bridgeSet;
            this.libcBinding = libcBinding;
            this.libcSha256 = libcSha256;
            this.carrierReceiptPrefix = carrierReceiptPrefix;
            this.payloadStaging = payloadStaging;
            this.carrierDeviceName = carrierDeviceName;
            this.controllerDeviceName = controllerDeviceName;
            this.observerDeviceName = observerDeviceName;
            this.payloadDeviceName = payloadDeviceName;
            this.bootstrapDeviceName = bootstrapDeviceName;
            this.practiceProbeDeviceName = practiceProbeDeviceName;
            this.profileAutogenDeviceName = profileAutogenDeviceName;
            this.artifacts = Collections.unmodifiableList(artifacts);
        }

        boolean matches(String machine, String bridges, String libcSha) {
            return hostMachine.equals(machine) && bridgeSet.equals(bridges) &&
                    ("dynamic".equals(libcBinding) || libcSha256.equals(libcSha));
        }

        boolean requiresLibcIdentity() { return "exact".equals(libcBinding); }

        boolean selectable() { return enabled && accepted; }

        String runtimeLabel() { return label + " · " + id; }
    }

    final List<Backend> backends;
    final List<IdentityHelper> identityHelpers;

    private ArtifactRegistry(List<Backend> backends, List<IdentityHelper> identityHelpers) {
        this.backends = Collections.unmodifiableList(backends);
        this.identityHelpers = Collections.unmodifiableList(identityHelpers);
    }

    int artifactCount() {
        int count = 0;
        for (Backend backend : backends) count += backend.artifacts.size();
        return count;
    }

    Backend find(String machine, String bridge, String libcSha) {
        Backend match = null;
        for (Backend backend : backends) {
            if (!backend.selectable()) continue;
            if (!backend.matches(machine, bridge, libcSha)) continue;
            if (match != null) return null;
            match = backend;
        }
        return match;
    }

    Backend findById(String id) {
        Backend match = null;
        for (Backend backend : backends) {
            if (!backend.selectable()) continue;
            if (!backend.id.equals(id)) continue;
            if (match != null) return null;
            match = backend;
        }
        return match;
    }

    Backend findExperimental(String machine, String bridges) {
        Backend match = null;
        for (Backend backend : backends) {
            if (!backend.hostMachine.equals(machine) || !backend.bridgeSet.equals(bridges)) continue;
            if (match != null) return null;
            match = backend;
        }
        return match;
    }

    Backend findAnyById(String id) {
        Backend match = null;
        for (Backend backend : backends) {
            if (!backend.id.equals(id)) continue;
            if (match != null) return null;
            match = backend;
        }
        return match;
    }

    IdentityHelper identityHelperFor(String machine) {
        IdentityHelper match = null;
        for (IdentityHelper helper : identityHelpers) {
            if (!helper.hostMachine.equals(machine)) continue;
            if (match != null) return null;
            match = helper;
        }
        return match;
    }

    static ArtifactRegistry loadAndVerify(Context context) throws Exception {
        JSONObject root = new JSONObject(new String(BuildProfileRegistry.readAll(
                context.getAssets().open("runtime/manifest.json")),
                java.nio.charset.StandardCharsets.UTF_8));
        if (root.getInt("schema") != 2)
            throw new IOException("unsupported runtime backend manifest");
        JSONArray helperArray = root.getJSONArray("identity_helpers");
        List<IdentityHelper> helpers = new ArrayList<>();
        Set<String> helperMachines = new HashSet<>();
        for (int index = 0; index < helperArray.length(); ++index) {
            JSONObject item = helperArray.getJSONObject(index);
            String machine = item.getString("host_machine");
            String asset = item.getString("asset");
            String name = item.getString("device_name");
            String sha = item.getString("sha256").toLowerCase(Locale.ROOT);
            if (!machine.matches("x86_64|arm64") || !helperMachines.add(machine) ||
                    !asset.matches("runtime/[A-Za-z0-9_.-]+") ||
                    !name.matches("[A-Za-z0-9_.-]+") || !sha.matches("[0-9a-f]{64}"))
                throw new IOException("invalid identity helper entry");
            byte[] bytes = BuildProfileRegistry.readAll(context.getAssets().open(asset));
            if (!sha.equals(hex(MessageDigest.getInstance("SHA-256").digest(bytes))))
                throw new IOException("identity helper hash mismatch: " + name);
            helpers.add(new IdentityHelper(machine, asset, name, sha));
        }
        if (helpers.size() != 2 || !helperMachines.contains("arm64") ||
                !helperMachines.contains("x86_64"))
            throw new IOException("incomplete identity helper registry");
        JSONArray sets = root.getJSONArray("artifact_sets");
        if (sets.length() == 0) throw new IOException("empty runtime backend registry");
        List<Backend> backends = new ArrayList<>();
        Set<String> backendIds = new HashSet<>();
        Set<String> environmentKeys = new HashSet<>();
        for (int backendIndex = 0; backendIndex < sets.length(); ++backendIndex) {
            JSONObject item = sets.getJSONObject(backendIndex);
            String id = item.getString("id");
            String label = item.getString("label");
            boolean enabled = !item.has("enabled") || item.getBoolean("enabled");
            boolean accepted = !item.has("accepted") || item.getBoolean("accepted");
            String machine = item.getString("host_machine");
            String bridges = item.getString("bridge_set");
            String libcBinding = item.has("libc_binding") ?
                    item.getString("libc_binding") : "exact";
            String libcSha = item.getString("libc_sha256").toLowerCase(Locale.ROOT);
            String carrierReceiptPrefix = item.getString("carrier_receipt_prefix");
            String payloadStaging = item.getString("payload_staging");
            JSONObject roles = item.getJSONObject("roles");
            if (!id.matches("[a-z0-9][a-z0-9_.-]{2,95}") || label.isEmpty() ||
                    !machine.matches("x86_64|arm64") ||
                    !bridges.matches("none|libhoudini[.]so|libnb[.]so|libndk_translation[.]so|" +
                            "libhoudini[.]so[+]libnb[.]so|" +
                            "libhoudini[.]so[+]libndk_translation[.]so|" +
                            "libnb[.]so[+]libndk_translation[.]so|" +
                            "libhoudini[.]so[+]libnb[.]so[+]libndk_translation[.]so") ||
                    !libcBinding.matches("exact|dynamic") ||
                    !libcSha.matches("[0-9a-f]{64}") ||
                    !carrierReceiptPrefix.matches("[A-Z0-9_]{4,64}") ||
                    !payloadStaging.matches("local_tmp|target_app_cache") ||
                    (payloadStaging.equals("target_app_cache") && !machine.equals("arm64")) ||
                    (libcBinding.equals("dynamic") &&
                            (!machine.equals("arm64") || !bridges.equals("none") ||
                                    !payloadStaging.equals("target_app_cache"))) ||
                    (accepted && !enabled) ||
                    !backendIds.add(id) ||
                    !environmentKeys.add(machine + "\n" + bridges + "\n" + libcBinding +
                            "\n" + (libcBinding.equals("exact") ? libcSha : "dynamic")))
                throw new IOException("invalid or duplicate runtime backend identity");

            String carrier = role(roles, "carrier");
            String controller = role(roles, "controller");
            String observer = role(roles, "observer");
            String payload = role(roles, "payload");
            // Native ARM64 loaders do not need the x86 NativeBridge bootstrap.
            // Keep the role mandatory for the existing backend, but allow it to
            // be absent from a future, separately accepted native backend.
            String bootstrap = optionalRole(roles, "bootstrap");
            String practiceProbe = role(roles, "practice_probe");
            String profileAutogen = optionalRole(roles, "profile_autogen");
            if (profileAutogen != null && !machine.equals("arm64"))
                throw new IOException("Profile autogen is only valid for native ARM64");
            JSONArray array = item.getJSONArray("artifacts");
            if (array.length() == 0) throw new IOException("empty backend artifact set: " + id);
            List<Artifact> artifacts = new ArrayList<>();
            Set<String> deviceNames = new HashSet<>();
            for (int index = 0; index < array.length(); ++index) {
                JSONObject artifact = array.getJSONObject(index);
                String asset = artifact.getString("asset");
                String name = artifact.getString("device_name");
                String mode = artifact.getString("mode");
                String sha = artifact.getString("sha256").toLowerCase(Locale.ROOT);
                if (!asset.matches("(?:runtime|profiles)/[A-Za-z0-9_.-]+") ||
                        !name.matches("[A-Za-z0-9_.-]+") || !deviceNames.add(name) ||
                        !(mode.equals("0644") || mode.equals("0700")) ||
                        !sha.matches("[0-9a-f]{64}"))
                    throw new IOException("invalid backend artifact entry: " + id);
                byte[] bytes = BuildProfileRegistry.readAll(context.getAssets().open(asset));
                if (!sha.equals(hex(MessageDigest.getInstance("SHA-256").digest(bytes))))
                    throw new IOException("artifact hash mismatch: " + name);
                artifacts.add(new Artifact(asset, name, mode, sha));
            }
            for (String required : new String[]{carrier, controller, observer, payload,
                    bootstrap, practiceProbe, profileAutogen})
                if (required != null && !deviceNames.contains(required))
                    throw new IOException("backend role is not an artifact: " + required);
            backends.add(new Backend(id, label, enabled, accepted,
                    machine, bridges, libcBinding, libcSha, carrierReceiptPrefix, carrier,
                    payloadStaging, controller, observer, payload, bootstrap,
                    practiceProbe, profileAutogen, artifacts));
        }
        return new ArtifactRegistry(backends, helpers);
    }

    private static String role(JSONObject roles, String name) throws Exception {
        String value = roles.getString(name);
        if (!value.matches("[A-Za-z0-9_.-]+"))
            throw new IOException("invalid backend role: " + name);
        return value;
    }

    private static String optionalRole(JSONObject roles, String name) throws Exception {
        if (!roles.has(name)) return null;
        return role(roles, name);
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes)
            builder.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return builder.toString();
    }
}
