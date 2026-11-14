-- Seed data, migration 002: the three simulated servers (same defaults as the C++ engine's sample cluster).
-- Change the numbers later from the Resources page (administrators) or with an UPDATE; nothing else depends on them.
-- Servers are numbered 1, 2, 3; the id is also the server id used in Gantt charts and allocation history.

INSERT INTO resources (id, name, total_cpu, total_ram_gb, total_storage_gb) VALUES (1, 'Server-1', 8, 32, 500);
INSERT INTO resources (id, name, total_cpu, total_ram_gb, total_storage_gb) VALUES (2, 'Server-2', 16, 64, 1000);
INSERT INTO resources (id, name, total_cpu, total_ram_gb, total_storage_gb) VALUES (3, 'Server-3', 4, 16, 250);
