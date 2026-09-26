"""公共 C ABI 验证运行中配置快照和后续作业预算。"""
import copy
import ctypes as c
import hashlib
import json
import pathlib
import sys
import tempfile
import threading
import time

from cli_integration import png_2x2
from config_integration import config


class View(c.Structure):
    _fields_ = [("data", c.c_char_p), ("size", c.c_size_t)]


class Bytes(c.Structure):
    _fields_ = [("data", c.POINTER(c.c_uint8)), ("size", c.c_size_t), ("allocation_id", c.c_uint64)]


class Input(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("data", c.POINTER(c.c_uint8)),
                ("size", c.c_size_t), ("format", c.c_uint32), ("width", c.c_uint32),
                ("height", c.c_uint32), ("row_stride", c.c_size_t)]


def main():
    lib = c.CDLL(sys.argv[1])
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_reconfigure.argtypes = [c.c_uint64, View]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(Input)]
    lib.dococr_job_manifest.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_poll_events.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_execution_plan.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        old = config(root)
        old["backend"] = "fixture:slow"

        def view(value):
            encoded = json.dumps(value, ensure_ascii=True).encode()
            return View(encoded, len(encoded))

        engine = c.c_uint64()
        assert lib.dococr_create(view(old), c.byref(engine)) == 0
        old_job = c.c_uint64()
        assert lib.dococr_job_create(engine, c.byref(old_job)) == 0
        image = png_2x2()
        data = (c.c_uint8 * len(image)).from_buffer_copy(image)
        request = Input(c.sizeof(Input), data, len(image), 1, 0, 0, 0)
        statuses = []
        runner = threading.Thread(target=lambda: statuses.append(lib.dococr_job_run(old_job, c.byref(request))))
        runner.start()
        observed_running = False
        for _ in range(100):
            event_bytes = Bytes()
            assert lib.dococr_job_poll_events(old_job, c.byref(event_bytes)) == 0
            event = json.loads(c.string_at(event_bytes.data, event_bytes.size))
            assert lib.dococr_bytes_free(c.byref(event_bytes)) == 0
            if event["state"] == "running":
                observed_running = True
                break
            time.sleep(0.01)
        assert observed_running
        new = copy.deepcopy(old)
        new["execution"]["max_page_pixels"] = 1
        assert lib.dococr_reconfigure(engine, view(new)) == 0
        plan_bytes = Bytes()
        assert lib.dococr_execution_plan(engine, c.byref(plan_bytes)) == 0
        new_plan = json.loads(c.string_at(plan_bytes.data, plan_bytes.size))
        assert lib.dococr_bytes_free(c.byref(plan_bytes)) == 0
        runner.join()
        assert statuses == [0], statuses
        manifest_bytes = Bytes()
        assert lib.dococr_job_manifest(old_job, c.byref(manifest_bytes)) == 0
        old_manifest = json.loads(c.string_at(manifest_bytes.data, manifest_bytes.size))
        assert old_manifest["effective_parameters"]["max_page_pixels"] == 16000000
        assert old_manifest["config_hash"] != new_plan["config_hash"]
        assert lib.dococr_bytes_free(c.byref(manifest_bytes)) == 0
        assert lib.dococr_job_destroy(old_job) == 0

        changed_file = root / "replacement.bin"
        changed_file.write_bytes(b"replacement contract\n")
        changed = copy.deepcopy(new)
        for model in changed["models"].values():
            model["artifacts"][0] = {"path": changed_file.name,
                                       "sha256": hashlib.sha256(changed_file.read_bytes()).hexdigest()}
        assert lib.dococr_reconfigure(engine, view(changed)) == 9

        new_job = c.c_uint64()
        assert lib.dococr_job_create(engine, c.byref(new_job)) == 0
        assert lib.dococr_job_run(new_job, c.byref(request)) == 10
        assert lib.dococr_job_destroy(new_job) == 0
        assert lib.dococr_destroy(engine) == 0


if __name__ == "__main__":
    main()
