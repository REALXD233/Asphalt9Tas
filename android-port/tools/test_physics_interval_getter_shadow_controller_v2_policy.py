from pathlib import Path
import sys


def require(text: str, token: str) -> None:
    if token not in text:
        raise SystemExit(f"missing required controller token: {token}")


def reject(text: str, token: str) -> None:
    if token in text:
        raise SystemExit(f"forbidden controller token: {token}")


source = Path(sys.argv[1]).read_text(encoding="utf-8")

for required in (
    "I_ACCEPT_PHYSICS_INTERVAL_SHADOW_V2",
    'std::strcmp(text, "preflight")',
    "action == Action::kPreflight",
    "PHYSICS_INTERVAL_SHADOW_PREFLIGHT",
    "kNumericArgumentIndices[5] = {2, 3, 4, 5, 7}",
    "argv[kNumericArgumentIndices[i]]",
    "resolver::Resolve",
    "shadow::BuildPlan",
    "shadow::VerifyArmed",
    "shadow::VerifyRestored",
    "ReadStartTicks",
    "GameIdentity",
    "StopProcess",
    "ResumeProcess",
    "DisableAndDrain",
    "WriteExactVerified(mem, layout.shadow",
    "WriteExactVerified(mem, layout.continuation",
    "WriteExactVerified(mem, object, &plan.shadow_vptr",
    "WriteExactVerified(mem, object, &plan.original_vptr",
    "offsetof(payload::Control, enabled)",
    "stopped_control.active_calls == 0",
    "current_vptr != plan.shadow_vptr",
    "current_vptr != plan.original_vptr",
    "action == Action::kFinalize && !complete",
):
    require(source, required)

for forbidden in (
    "argv[i + 2]",
    "ptrace(",
    "PTRACE_",
    "mprotect(",
    "process_vm_writev",
    "dlopen(",
    "dlsym(",
    "__NR_ptrace",
):
    reject(source, forbidden)

# The source order is itself a safety invariant: shadow storage and the
# original continuation are published before the object's vptr is replaced;
# restoration text must also exist after the install site.
copy_pos = source.index("WriteExactVerified(mem, layout.shadow")
continue_pos = source.index("WriteExactVerified(mem, layout.continuation")
install_pos = source.index("WriteExactVerified(mem, object, &plan.shadow_vptr")
restore_pos = source.index("WriteExactVerified(mem, object, &plan.original_vptr")
if not (copy_pos < continue_pos < install_pos < restore_pos):
    raise SystemExit("shadow transaction source order is not fail-closed")

print("PHYSICS_INTERVAL_SHADOW_CONTROLLER_V2_POLICY passed=1")
