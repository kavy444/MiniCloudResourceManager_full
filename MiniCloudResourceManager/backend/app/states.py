"""Job life cycle stored in the database.

    NEW        saved, never scheduled (also the state after an edit)
    READY      selected for a scheduling run, waiting to be admitted / dispatched
    WAITING    valid, but the cluster has no free CPU/RAM/storage for it right now (retried on every release)
    RUNNING    resources reserved, executing
    COMPLETED  finished; resources released exactly once
    FAILED     rejected (invalid or larger than every server) or aborted - see status_reason
    CANCELLED  withdrawn by the user; never scheduled again

The C++ engine uses QUEUED/REJECTED internally; they map to READY and FAILED here. A run is a self-contained
simulation, so WAITING/RUNNING are *passed through* while a run is saved (each step is checked below) and a
finished run leaves the job COMPLETED or FAILED.
"""
from __future__ import annotations

NEW, READY, WAITING, RUNNING = "NEW", "READY", "WAITING", "RUNNING"
COMPLETED, FAILED, CANCELLED = "COMPLETED", "FAILED", "CANCELLED"
ALL_STATES = (NEW, READY, RUNNING, WAITING, COMPLETED, FAILED, CANCELLED)

ALLOWED: dict[str, frozenset] = {
    NEW: frozenset({READY, CANCELLED}),
    READY: frozenset({RUNNING, WAITING, FAILED, CANCELLED, NEW}),
    WAITING: frozenset({RUNNING, FAILED, CANCELLED, NEW}),   # NEW = edited while waiting
    RUNNING: frozenset({COMPLETED, FAILED}),
    COMPLETED: frozenset({READY, NEW}),   # re-run, or edited
    FAILED: frozenset({READY, NEW}),
    CANCELLED: frozenset(),               # final
}

CANCELLABLE = frozenset({NEW, READY, WAITING})
SCHEDULABLE = frozenset({NEW, READY, WAITING, COMPLETED, FAILED})  # everything except CANCELLED / RUNNING


def can_transition(current: str, target: str) -> bool:
    return target in ALLOWED.get(current, frozenset())


def assert_transition(current: str, target: str) -> None:
    if not can_transition(current, target):
        raise ValueError(f"invalid job state transition {current} -> {target}")


def run_path(current: str, outcome: str, had_waiting: bool) -> list[str]:
    """The sequence of states a job passes through while a run is saved.

    outcome is the engine's final status for the process: COMPLETED or REJECTED.
    Every step is validated, so a bug here fails loudly instead of corrupting state.
    """
    steps: list[str] = []
    state = current
    if state != READY:
        steps.append(READY)
        state = READY
    if had_waiting:
        steps.append(WAITING)
        state = WAITING
    if outcome == "COMPLETED":
        steps += [RUNNING, COMPLETED]
    else:
        steps.append(FAILED)
    cursor = current
    for step in steps:
        assert_transition(cursor, step)
        cursor = step
    return steps
