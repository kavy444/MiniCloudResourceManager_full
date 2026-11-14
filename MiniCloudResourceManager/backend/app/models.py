"""Request validation (Pydantic). The frontend validates too, but the backend never trusts it.
Limits mirror the C++ engine's limits so a request that passes here is not rejected by the engine for size."""
from __future__ import annotations

import re
from typing import Literal, Optional

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

ALGORITHMS = ("FCFS", "SJF", "PRIORITY_NP", "HRRN", "SRTF", "RR", "PRIORITY_P", "MLQ", "MLFQ")
AlgorithmName = Literal["FCFS", "SJF", "PRIORITY_NP", "HRRN", "SRTF", "RR", "PRIORITY_P", "MLQ", "MLFQ"]

MAX_JOBS_PER_RUN = 200
MAX_BURST = 10_000          # the engine allows 100000; the API is stricter to keep Gantt charts readable
MAX_TIME = 1_000_000

_USERNAME = re.compile(r"^[a-z0-9_.-]{3,50}$")
_CONTROL = re.compile(r"[\x00-\x1f\x7f]")


class Strict(BaseModel):
    # strict: JSON "5" is not accepted where a number is expected; unknown fields are rejected, not ignored
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True, strict=True)


# ---------------------------------------------------------------- auth
class Credentials(Strict):
    username: str = Field(min_length=3, max_length=50)
    password: str = Field(min_length=8, max_length=128)

    @field_validator("username")
    @classmethod
    def _username(cls, v: str) -> str:
        v = v.lower()
        if not _USERNAME.match(v):
            raise ValueError("username may only contain letters, digits, '.', '_' and '-' (3-50 characters)")
        return v


class LoginRequest(Strict):
    username: str = Field(min_length=1, max_length=50)
    password: str = Field(min_length=1, max_length=128)


# ---------------------------------------------------------------- jobs
class JobIn(Strict):
    name: str = Field(min_length=1, max_length=100)
    arrival_time: int = Field(default=0, ge=0, le=MAX_TIME)
    burst_time: int = Field(ge=1, le=MAX_BURST)
    priority: int = Field(default=2, ge=1, le=1000, description="smaller number = higher priority")
    queue_level: Optional[int] = Field(default=None, ge=0, le=7, description="MLQ queue index, empty = derive from priority")
    cpu_cores: int = Field(default=1, ge=1, le=1_000_000)
    ram_gb: int = Field(default=1, ge=1, le=1_000_000)
    storage_gb: int = Field(default=1, ge=1, le=1_000_000)

    @field_validator("name")
    @classmethod
    def _name(cls, v: str) -> str:
        if _CONTROL.search(v):
            raise ValueError("name must not contain control characters")
        return v


# ---------------------------------------------------------------- scheduling
class QueueSpecIn(Strict):
    policy: Literal["FCFS", "RR", "SJF", "PRIORITY"] = "FCFS"
    quantum: int = Field(default=0, ge=0, le=100_000)

    @model_validator(mode="after")
    def _rr_needs_quantum(self):
        if self.policy == "RR" and self.quantum < 1:
            raise ValueError("a Round Robin queue needs a quantum of at least 1")
        return self


class SchedParams(Strict):
    """Algorithm configuration shared by single runs and comparisons."""

    time_quantum: int = Field(default=2, ge=1, le=100_000)
    mlq_queues: Optional[list[QueueSpecIn]] = Field(default=None, min_length=1, max_length=8)
    mlfq_quanta: Optional[list[int]] = Field(default=None, min_length=1, max_length=8)
    mlfq_aging: int = Field(default=0, ge=0, le=MAX_TIME)
    use_cluster: bool = True
    job_ids: Optional[list[int]] = Field(default=None, min_length=1, max_length=MAX_JOBS_PER_RUN)

    @field_validator("mlfq_quanta")
    @classmethod
    def _quanta(cls, v):
        if v is not None and any(q < 0 or q > 100_000 for q in v):
            raise ValueError("MLFQ quanta must be between 0 (run to completion) and 100000")
        return v

    @field_validator("job_ids")
    @classmethod
    def _unique_ids(cls, v):
        if v is not None:
            if len(set(v)) != len(v):
                raise ValueError("job_ids must not contain duplicates")
            if any(i < 1 for i in v):
                raise ValueError("job_ids must be positive")
        return v


class RunRequest(SchedParams):
    algorithm: AlgorithmName


class ComparisonRequest(SchedParams):
    name: str = Field(default="Algorithm comparison", min_length=1, max_length=100)
    algorithms: list[AlgorithmName] = Field(min_length=2, max_length=9)

    @field_validator("algorithms")
    @classmethod
    def _unique(cls, v):
        if len(set(v)) != len(v):
            raise ValueError("each algorithm may be listed only once")
        return v


class CompareSavedRequest(Strict):
    run_ids: list[int] = Field(min_length=2, max_length=20)

    @field_validator("run_ids")
    @classmethod
    def _positive(cls, v):
        if any(i < 1 for i in v):
            raise ValueError("run ids must be positive")
        return v


# ---------------------------------------------------------------- resources
class ResourceUpdate(Strict):
    name: str = Field(min_length=1, max_length=50)
    total_cpu: int = Field(ge=1, le=1_000_000)
    total_ram_gb: int = Field(ge=1, le=1_000_000)
    total_storage_gb: int = Field(ge=1, le=1_000_000)


# ---------------------------------------------------------------- OS modules
class MemoryOpIn(Strict):
    type: Literal["ALLOC", "FREE"]
    process: str = Field(min_length=1, max_length=50)
    size: int = Field(default=0, ge=0, le=1_000_000_000)


class MemoryRequest(Strict):
    strategy: Literal["FIRST_FIT", "BEST_FIT", "WORST_FIT"]
    total_size: Optional[int] = Field(default=None, ge=1, le=1_000_000_000)
    partitions: Optional[list[int]] = Field(default=None, min_length=1, max_length=64)
    operations: list[MemoryOpIn] = Field(min_length=1, max_length=200)

    @model_validator(mode="after")
    def _one_model(self):
        if (self.total_size is None) == (self.partitions is None):
            raise ValueError("give either total_size (dynamic partitions) or partitions (fixed partitions), not both")
        if self.partitions is not None and any(p < 1 for p in self.partitions):
            raise ValueError("every partition size must be at least 1")
        return self


IntRow = list[int]


class BankerRequest(Strict):
    action: Literal["safety", "request", "detect"]
    available: list[int] = Field(min_length=1, max_length=16)
    max: Optional[list[IntRow]] = Field(default=None, max_length=32)
    allocation: list[IntRow] = Field(max_length=32)
    request: Optional[list] = None          # a vector for action=request, a matrix for action=detect
    process: Optional[int] = Field(default=None, ge=0, le=31)

    @model_validator(mode="after")
    def _shape(self):
        if self.action in ("safety", "request") and self.max is None:
            raise ValueError("'max' is required for safety and request checks")
        if self.action == "request" and (self.request is None or self.process is None):
            raise ValueError("'process' and 'request' are required for a request check")
        if self.action == "detect" and self.request is None:
            raise ValueError("'request' (matrix of outstanding requests) is required for detection")
        return self


class SyncRequest(Strict):
    scenario: Literal["race", "producer_consumer"]
    increments: int = Field(default=5, ge=1, le=50)
    use_mutex: bool = False
    items: int = Field(default=6, ge=1, le=30)
    buffer_size: int = Field(default=2, ge=1, le=10)
    wrong_order: bool = False
    consumer_delay: int = Field(default=0, ge=0, le=200)
