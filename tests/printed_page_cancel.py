"""从公共 C ABI 验证最后一个区域推理期间取消后不会返回成功。"""
import ctypes as c
import json
import os
import sys
import threading
import time

from cli_integration import png_2x2
from config_abi import View, Bytes, Input
from printed_page_integration import ROOT, config


def main():
    os.chdir(ROOT)
    lib = c.CDLL(sys.argv[1])
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(Input)]
    lib.dococr_job_cancel.argtypes = [c.c_uint64]
    lib.dococr_job_poll_events.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    encoded = json.dumps(config('printed_page_slow')).encode()
    engine = c.c_uint64()
    assert lib.dococr_create(View(encoded, len(encoded)), c.byref(engine)) == 0
    job = c.c_uint64()
    assert lib.dococr_job_create(engine, c.byref(job)) == 0
    image = png_2x2()
    owned = (c.c_uint8 * len(image)).from_buffer_copy(image)
    request = Input(c.sizeof(Input), owned, len(image), 1, 0, 0, 0)
    results = []
    thread = threading.Thread(target=lambda: results.append(lib.dococr_job_run(job, c.byref(request))))
    thread.start()
    started = False
    for _ in range(100):
        item = Bytes()
        assert lib.dococr_job_poll_events(job, c.byref(item)) == 0
        event = json.loads(c.string_at(item.data, item.size))
        assert lib.dococr_bytes_free(c.byref(item)) == 0
        if event['state'] == 'running':
            started = True
            break
        time.sleep(0.01)
    assert started
    time.sleep(0.3)
    assert thread.is_alive(), '受控最后区域推理应仍在运行'
    assert lib.dococr_job_cancel(job) == 0
    thread.join(timeout=5)
    assert results == [7], results
    item = Bytes()
    assert lib.dococr_job_poll_events(job, c.byref(item)) == 0
    event = json.loads(c.string_at(item.data, item.size))
    assert lib.dococr_bytes_free(c.byref(item)) == 0
    assert event['state'] == 'cancelled', event
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0


if __name__ == '__main__':
    main()
