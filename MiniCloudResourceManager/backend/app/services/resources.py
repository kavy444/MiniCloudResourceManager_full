from __future__ import annotations

from ..errors import forbidden, not_found, unprocessable
from ..repositories import events as events_repo
from ..repositories import resources as resources_repo


def list_servers(ctx) -> dict:
    """The three simulated servers are shared cluster configuration (not per user)."""
    with ctx.db.transaction() as tx:
        rows = resources_repo.list_all(tx)
    servers = [resources_repo.present(r) for r in rows]
    tot = {k: sum(s["total"][k] for s in servers) for k in ("cpu", "ram_gb", "storage_gb")}
    used = {k: sum(s["allocated"][k] for s in servers) for k in ("cpu", "ram_gb", "storage_gb")}
    pct = lambda a, b: round(100.0 * a / b, 2) if b else 0.0
    return {"servers": servers, "totals": tot, "allocated": used,
            "utilization_percent": {"cpu": pct(used["cpu"], tot["cpu"]), "ram": pct(used["ram_gb"], tot["ram_gb"]),
                                    "storage": pct(used["storage_gb"], tot["storage_gb"])}}


def update_server(ctx, user: dict, resource_id: int, data: dict) -> dict:
    if user["role"] != "admin":
        raise forbidden("Only an administrator can change server capacities")
    try:
        with ctx.db.transaction() as tx:
            row = resources_repo.get(tx, resource_id, lock=True)
            if row is None:
                raise not_found("Server")
            if (data["total_cpu"] < row["allocated_cpu"] or data["total_ram_gb"] < row["allocated_ram_gb"]
                    or data["total_storage_gb"] < row["allocated_storage_gb"]):
                raise unprocessable("New capacity is below what is currently allocated on this server")
            resources_repo.update_config(tx, resource_id, data["name"], data["total_cpu"],
                                         data["total_ram_gb"], data["total_storage_gb"])
            events_repo.add(tx, user["id"], "SERVER_UPDATED",
                            f"Server {resource_id} set to '{data['name']}': CPU {data['total_cpu']}, "
                            f"RAM {data['total_ram_gb']} GB, storage {data['total_storage_gb']} GB")
            return resources_repo.present(resources_repo.get(tx, resource_id))
    except ctx.db.integrity_errors as exc:
        raise unprocessable("Invalid capacity or duplicate server name") from exc


def allocation_history(ctx, user_id: int, **filters) -> dict:
    with ctx.db.transaction() as tx:
        rows, total = resources_repo.search_allocations(tx, user_id, **filters)
    from .runs import _present_alloc
    return {"items": [_present_alloc(r) for r in rows], "total": total, "page": max(1, filters["page"]),
            "page_size": min(max(1, filters["page_size"]), 200)}
