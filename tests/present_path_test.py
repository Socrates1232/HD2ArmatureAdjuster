#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    source = (ROOT / "src" / "addon.cpp").read_text(encoding="utf-8")
    present = function_body(source, "void on_present(")
    assert "g_custom_intent.toggle()" in present
    assert "wake_custom_worker()" in present
    for forbidden in (
        "scan_custom_pose_input(",
        "service_custom_runtime_state(",
        "g_custom_runtime.toggle(",
        "WaitForSingleObject(",
    ):
        assert forbidden not in present, f"blocking custom work returned to on_present: {forbidden}"

    hunt_start = function_body(source, "bool ensure_hunt_thread()")
    assert "WaitForSingleObject(g_hunt_thread, 0)" in hunt_start

    queue = function_body(source, "void queue_custom_pose_input(")
    assert "k_custom_capture_bytes + k_custom_scan_overlap_bytes" in queue
    assert "map_size < k_custom_scan_chunk_bytes + k_custom_scan_overlap_bytes" in queue
    assert "g_custom_callback_scan_last_tick.compare_exchange_strong" in queue
    assert "observe_window(" not in queue

    scan = function_body(source, "void scan_custom_pose_input(")
    assert "g_custom_active_scan_buffer" in scan
    assert "k_custom_scan_chunk_bytes + k_custom_scan_overlap_bytes" in scan
    assert "g_custom_active_cursor" in scan

    worker = function_body(source, "DWORD WINAPI custom_worker_proc(")
    assert "scan_custom_pose_input(frame);" in worker
    assert "service_custom_runtime_state(frame);" in worker
    assert "service_custom_runtime_state(g_frame.load" not in worker
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
