"""One error type for the whole API, so every failure has the same JSON shape:
{"error": {"code": "...", "message": "...", "details": [...]}}"""
from __future__ import annotations

from typing import Any


class ApiError(Exception):
    def __init__(self, status_code: int, code: str, message: str, details: Any = None):
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message
        self.details = details

    def body(self) -> dict:
        err: dict[str, Any] = {"code": self.code, "message": self.message}
        if self.details is not None:
            err["details"] = self.details
        return {"error": err}


def bad_request(message: str, details: Any = None) -> ApiError:
    return ApiError(400, "bad_request", message, details)


def unauthorized(message: str = "Authentication required") -> ApiError:
    return ApiError(401, "unauthorized", message)


def forbidden(message: str = "You are not allowed to do that") -> ApiError:
    return ApiError(403, "forbidden", message)


def not_found(what: str = "Resource") -> ApiError:
    # The same answer is used for "does not exist" and "belongs to someone else" so ids cannot be probed.
    return ApiError(404, "not_found", f"{what} not found")


def conflict(message: str, details: Any = None) -> ApiError:
    return ApiError(409, "conflict", message, details)


def unprocessable(message: str, details: Any = None) -> ApiError:
    return ApiError(422, "unprocessable", message, details)


def too_many(message: str) -> ApiError:
    return ApiError(429, "too_many_requests", message)


def unavailable(message: str) -> ApiError:
    return ApiError(503, "service_unavailable", message)
