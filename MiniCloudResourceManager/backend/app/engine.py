"""Bridge to the C++ engine.

The engine is a separate program (mcrm_engine) that reads ONE JSON request on stdin and writes ONE JSON
response on stdout. Python starts a fresh process per request, so there is no shared state between requests
and nothing in the engine needs locking. The scheduling logic itself stays in C++.
"""
from __future__ import annotations

import json
import os
import subprocess
import threading
from typing import Any

from .errors import ApiError, unavailable

MAX_RESPONSE_BYTES = 64 * 1024 * 1024


class EngineClient:
    def __init__(self, path: str, timeout_s: int = 20, max_parallel: int = 4):
        self.path = path
        self.timeout_s = timeout_s
        # At most `max_parallel` engine processes at once (real concurrency control for request threads).
        self._slots = threading.BoundedSemaphore(max_parallel)

    def available(self) -> bool:
        return os.path.isfile(self.path) and os.access(self.path, os.X_OK)

    def call(self, request: dict[str, Any]) -> dict[str, Any]:
        """Send one request. Returns the engine's JSON object (it may say {"ok": false, "errors": [...]})."""
        if not self.available():
            raise unavailable(
                f"The C++ engine was not found at '{self.path}'. Build it first (see README, 'Build the C++ engine')."
            )
        payload = json.dumps(request).encode("utf-8")
        with self._slots:
            try:
                proc = subprocess.run(
                    [self.path], input=payload, capture_output=True, timeout=self.timeout_s, check=False
                )
            except subprocess.TimeoutExpired as exc:
                raise ApiError(504, "engine_timeout", f"The scheduling engine took longer than {self.timeout_s}s") from exc
            except OSError as exc:
                raise unavailable(f"The C++ engine could not be started: {exc}") from exc
        if proc.returncode != 0:
            raise ApiError(502, "engine_failed", f"The scheduling engine exited with code {proc.returncode}")
        if len(proc.stdout) > MAX_RESPONSE_BYTES:
            raise ApiError(502, "engine_failed", "The scheduling engine returned an unexpectedly large response")
        try:
            data = json.loads(proc.stdout.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            raise ApiError(502, "engine_failed", "The scheduling engine returned invalid JSON") from exc
        if not isinstance(data, dict):
            raise ApiError(502, "engine_failed", "The scheduling engine returned an unexpected response")
        return data
