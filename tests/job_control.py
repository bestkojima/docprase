"""通过公共 C ABI 验证作业控制、终态和同一引擎恢复。"""
import ctypes as c
import io
import json
import os
import pathlib
import shlex
import shutil
import sys
import tempfile
import threading
import time

from cli_integration import png_2x2
from config_abi import View, Bytes
from printed_page_integration import ROOT, config
from PIL import Image


class Input(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("data", c.POINTER(c.c_uint8)),
                ("size", c.c_size_t), ("format", c.c_uint32), ("width", c.c_uint32),
                ("height", c.c_uint32), ("row_stride", c.c_size_t),
                ("first_page", c.c_uint32), ("last_page", c.c_uint32),
                ("dpi", c.c_uint32), ("max_page_pixels", c.c_uint64),
                ("timeout_ms", c.c_uint32)]


class Result(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("json", Bytes), ("markdown", Bytes)]


def main():
    os.chdir(ROOT)
    lib = c.CDLL(sys.argv[1])
    lib.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    lib.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    lib.dococr_job_run.argtypes = [c.c_uint64, c.POINTER(Input)]
    lib.dococr_job_cancel.argtypes = [c.c_uint64]
    lib.dococr_job_wait.argtypes = [c.c_uint64, c.c_uint32]
    lib.dococr_job_status.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_next_event.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_manifest.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    lib.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    lib.dococr_job_destroy.argtypes = [c.c_uint64]
    lib.dococr_destroy.argtypes = [c.c_uint64]
    lib.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]

    def read_bytes(buffer):
        value = c.string_at(buffer.data, buffer.size)
        assert lib.dococr_bytes_free(c.byref(buffer)) == 0
        return value

    def read_json(function, handle):
        buffer = Bytes()
        status = function(handle, c.byref(buffer))
        return status, json.loads(read_bytes(buffer)) if status == 0 else None

    def make_engine(scenario):
        payload = (json.dumps(config(scenario)).encode() if scenario.startswith('printed_page')
                   else ('fixture:' + scenario).encode())
        engine = c.c_uint64()
        assert lib.dococr_create(View(payload, len(payload)), c.byref(engine)) == 0
        return engine.value

    def new_job(engine):
        job = c.c_uint64()
        assert lib.dococr_job_create(engine, c.byref(job)) == 0
        return job.value

    image = png_2x2()
    owned = (c.c_uint8 * len(image)).from_buffer_copy(image)

    def request(timeout=0):
        return Input(c.sizeof(Input), owned, len(image), 1, 0, 0, 0, 0, 0, 0, 0, timeout)

    gate_dir = tempfile.TemporaryDirectory()
    gate = pathlib.Path(gate_dir.name) / 'gate'
    entered = pathlib.Path(gate_dir.name) / 'entered'
    os.environ['DOCOCR_TEST_GATE_PATH'] = str(gate)
    os.environ['DOCOCR_TEST_ENTERED_PATH'] = str(entered)
    gate.touch()
    # 固定推理闸门确认当前区域确实占有后端，再从另一线程请求取消。
    engine = make_engine('printed_page_gate')
    job = new_job(engine)
    result = []
    req = request()
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(req))))
    worker.start()
    seen_region = False
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not seen_region:
        code, event = read_json(lib.dococr_job_next_event, job)
        if code == 0 and event['kind'] == 'region_started':
            seen_region = True
            break
        if code == 8:
            time.sleep(0.005)
    assert seen_region, '未观察到公共区域阶段事件'
    while time.monotonic() < deadline and not entered.exists():
        time.sleep(0.005)
    assert entered.exists(), '后端尚未进入区域推理'
    assert lib.dococr_job_cancel(job) == 0
    status, snapshot = read_json(lib.dococr_job_status, job)
    assert status == 0 and snapshot['state'] == 'cancelling', snapshot
    assert snapshot['cancellation_granularity'] == 'after_backend_call'
    assert lib.dococr_job_destroy(job) == 3
    assert lib.dococr_destroy(engine) == 3
    assert lib.dococr_job_wait(job, 1) == 3
    gate.unlink()
    worker.join(timeout=5)
    assert not worker.is_alive() and result == [7], result
    assert lib.dococr_job_wait(job, 0) == 0
    status, snapshot = read_json(lib.dococr_job_status, job)
    assert status == 0 and snapshot['state'] == 'cancelled' and snapshot['terminal']
    assert snapshot['page_completed'] == 0
    remaining = []
    while True:
        code, event = read_json(lib.dococr_job_next_event, job)
        if code == 8:
            break
        assert code == 0
        remaining.append(event)
    assert not any(event['kind'] == 'region_started' for event in remaining)
    assert not any(event['kind'] == 'page_completed' for event in remaining)
    assert remaining[-1]['kind'] == 'terminal'
    assert lib.dococr_job_cancel(job) == 1
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_job_cancel(job) == 2
    # 重建后同一引擎的新作业仍给出干净内容。
    job = new_job(engine)
    retry_request = request()
    assert lib.dococr_job_run(job, c.byref(retry_request)) == 0
    output = Result(c.sizeof(Result))
    assert lib.dococr_job_result(job, c.byref(output)) == 0
    document = json.loads(read_bytes(output.json))
    assert '中文，English!' in json.dumps(document, ensure_ascii=False), document
    read_bytes(output.markdown)
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0

    # 取消与区域后端异常同时发生时，公开终态须保留已知后端错误。
    gate.touch()
    entered.unlink()
    engine = make_engine('generation_gate_error')
    job = new_job(engine)
    req = request()
    result = []
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(req))))
    worker.start()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not entered.exists():
        time.sleep(0.005)
    assert entered.exists()
    assert lib.dococr_job_cancel(job) == 0
    gate.unlink()
    worker.join(timeout=5)
    assert result == [6], result
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['state'] == 'failed' and snapshot['cancel_requested'], snapshot
    assert snapshot['error']['code'] == 'region_inference_exception', snapshot
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0

    # 后端在版面调用内收到取消后以异常返回，仍不可把页面计为完成。
    gate.touch()
    entered.unlink()
    engine = make_engine('printed_page_layout_gate_error')
    job = new_job(engine)
    req = request()
    result = []
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(req))))
    worker.start()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not entered.exists():
        time.sleep(0.005)
    assert entered.exists()
    assert lib.dococr_job_cancel(job) == 0
    gate.unlink()
    worker.join(timeout=5)
    assert result == [7], result
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['state'] == 'cancelled' and snapshot['page_completed'] == 0
    _, manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['failure_code'] == 'layout_interrupted_error'
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0

    # 协作超时：等待超时不是作业结束，直到后端调用返回仍为 Busy。
    gate.touch()
    entered.unlink()
    engine = make_engine('printed_page_gate')
    job = new_job(engine)
    req = request(500)
    result = []
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(req))))
    worker.start()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not entered.exists():
        time.sleep(0.005)
    assert entered.exists()
    while time.monotonic() < deadline:
        status, snapshot = read_json(lib.dococr_job_status, job)
        if status == 0 and snapshot['timeout_requested']:
            break
        time.sleep(0.005)
    assert snapshot['timeout_requested'], snapshot
    assert lib.dococr_job_wait(job, 0) == 3
    assert lib.dococr_job_destroy(job) == 3
    gate.unlink()
    worker.join(timeout=5)
    assert result == [11], result
    status, snapshot = read_json(lib.dococr_job_status, job)
    assert status == 0 and snapshot['state'] == 'timed_out' and snapshot['terminal']
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0

    # 受控 OOM 后只可在重建成功后接收新作业；调用方最多一次重试。
    engine = make_engine('printed_page_oom_once')
    job = new_job(engine)
    req = request()
    assert lib.dococr_job_run(job, c.byref(req)) == 6
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['state'] == 'failed' and snapshot['error']['code'] == 'out_of_memory'
    assert snapshot['engine_ready']
    _, manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['failure_code'] == 'out_of_memory'
    assert lib.dococr_job_destroy(job) == 0
    retry_count = 0
    while retry_count < 1:
        retry_count += 1
        job = new_job(engine)
        req = request()
        assert lib.dococr_job_run(job, c.byref(req)) == 0
        result = Result(c.sizeof(Result))
        assert lib.dococr_job_result(job, c.byref(result)) == 0
        assert '中文，English!' in read_bytes(result.json).decode()
        read_bytes(result.markdown)
        assert lib.dococr_job_destroy(job) == 0
    assert retry_count == 1
    assert lib.dococr_destroy(engine) == 0

    # 受控后端失败保留 raw evidence，重建后的新作业无前一次错误。
    engine = make_engine('printed_page_fail_once')
    job = new_job(engine)
    req = request()
    assert lib.dococr_job_run(job, c.byref(req)) == 0
    result = Result(c.sizeof(Result))
    assert lib.dococr_job_result(job, c.byref(result)) == 0
    failed_document = read_bytes(result.json).decode()
    read_bytes(result.markdown)
    assert 'controlled raw' in failed_document and 'partial' in failed_document
    assert lib.dococr_job_destroy(job) == 0
    job = new_job(engine)
    req = request()
    assert lib.dococr_job_run(job, c.byref(req)) == 0
    result = Result(c.sizeof(Result))
    assert lib.dococr_job_result(job, c.byref(result)) == 0
    clean_document = read_bytes(result.json).decode()
    read_bytes(result.markdown)
    assert 'controlled raw' not in clean_document and '中文，English!' in clean_document
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0

    # 导出清单时的分配失败仍释放运行占用，不会误报成功或悬挂 Busy。
    engine = make_engine('printed_page_finalization_oom')
    job = new_job(engine)
    req = request()
    assert lib.dococr_job_run(job, c.byref(req)) == 6
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['state'] == 'failed' and snapshot['terminal'] and not snapshot['running']
    assert snapshot['error']['code'] == 'out_of_memory'
    assert snapshot['error']['stage'] == 'finalization'
    assert not snapshot['engine_ready']
    assert lib.dococr_job_wait(job, 0) == 0
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0

    # 持续失败只允许显式重试一次；第二次仍失败后停止。
    engine = make_engine('printed_page_failure')
    statuses = []
    for attempt in range(2):
        job = new_job(engine)
        req = request()
        assert lib.dococr_job_run(job, c.byref(req)) == 0
        result = Result(c.sizeof(Result))
        assert lib.dococr_job_result(job, c.byref(result)) == 0
        document = json.loads(read_bytes(result.json))
        read_bytes(result.markdown)
        statuses.append(document['status'])
        assert lib.dococr_job_destroy(job) == 0
    assert statuses == ['partial', 'partial']
    assert lib.dococr_destroy(engine) == 0

    # 重建失败时旧运行清单仍可取，新作业被拒绝以隔离不确定后端。
    engine = make_engine('printed_page_rebuild_failure')
    job = new_job(engine)
    req = request()
    assert lib.dococr_job_run(job, c.byref(req)) == 0
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['terminal'] and not snapshot['engine_ready']
    assert snapshot['recovery_error'] == 'backend_rebuild_failed'
    _, manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['recovery']['status'] == 'failed'
    assert any(region['status'] == 'failed' for region in manifest['regions'])
    assert lib.dococr_job_destroy(job) == 0
    blocked = new_job(engine)
    req = request()
    assert lib.dococr_job_run(blocked, c.byref(req)) == 6
    assert lib.dococr_job_destroy(blocked) == 0
    assert lib.dococr_destroy(engine) == 0

    # PDF 第二页区域被闸门阻塞时取消，第一页与当前页阶段证据仍在运行清单。
    two_pages = [Image.new('RGB', (2, 2), 'white') for _ in range(2)]
    pdf_stream = io.BytesIO()
    two_pages[0].save(pdf_stream, format='PDF', save_all=True,
                      append_images=two_pages[1:], resolution=72)
    pdf = pdf_stream.getvalue()
    pdf_owned = (c.c_uint8 * len(pdf)).from_buffer_copy(pdf)
    pdf_request = Input(c.sizeof(Input), pdf_owned, len(pdf), 5, 0, 0, 0,
                        1, 2, 72, 0, 0)
    os.environ['DOCOCR_TEST_GATE_PAGE'] = '2'
    gate.touch()
    entered.unlink()
    engine = make_engine('printed_page_gate')
    job = new_job(engine)
    result = []
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(pdf_request))))
    worker.start()
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not entered.exists():
        time.sleep(0.01)
    assert entered.exists(), result
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['page_completed'] == 1 and snapshot['page_current'] == 2, snapshot
    assert lib.dococr_job_cancel(job) == 0
    assert lib.dococr_job_destroy(job) == 3
    gate.unlink()
    worker.join(timeout=8)
    assert result == [7], result
    _, manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['pdf']['completed_pages'] == 1, manifest['pdf']
    assert [page['status'] for page in manifest['pdf']['pages']] == ['partial', 'cancelled']
    assert manifest['pdf']['pages'][1]['regions'][-1]['status'] == 'cancelled'
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0
    os.environ.pop('DOCOCR_TEST_GATE_PAGE')

    # Poppler 子进程在受控闸门内运行时，取消不提前释放作业或 PDF 临时目录。
    poppler_dir = pathlib.Path(gate_dir.name) / 'poppler'
    poppler_dir.mkdir()
    (poppler_dir / 'pdfinfo').symlink_to(shutil.which('pdfinfo'))
    renderer = poppler_dir / 'pdftoppm'
    renderer.write_text('#!/bin/sh\n'
                        'case " $* " in *" -png "*)\n'
                        '  touch "$DOCOCR_TEST_RENDER_ENTERED"\n'
                        '  while [ -e "$DOCOCR_TEST_RENDER_GATE" ]; do sleep 0.01; done\n'
                        '  if [ "$DOCOCR_TEST_RENDER_FAIL" = "1" ]; then\n'
                        '    echo controlled_render_failure >&2\n'
                        '    exit 23\n'
                        '  fi\n'
                        '  ;;\n'
                        'esac\n'
                        'exec ' + shlex.quote(shutil.which('pdftoppm')) + ' "$@"\n')
    renderer.chmod(0o755)
    render_gate = pathlib.Path(gate_dir.name) / 'render.gate'
    render_entered = pathlib.Path(gate_dir.name) / 'render.entered'
    render_gate.touch()
    os.environ['DOCOCR_POPPLER_BIN'] = str(poppler_dir)
    os.environ['DOCOCR_TEST_RENDER_GATE'] = str(render_gate)
    os.environ['DOCOCR_TEST_RENDER_ENTERED'] = str(render_entered)
    engine = make_engine('sample')
    job = new_job(engine)
    result = []
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(pdf_request))))
    worker.start()
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not render_entered.exists():
        time.sleep(0.01)
    assert render_entered.exists(), result
    assert lib.dococr_job_cancel(job) == 0
    assert lib.dococr_job_wait(job, 0) == 3
    assert lib.dococr_job_destroy(job) == 3
    render_gate.unlink()
    worker.join(timeout=8)
    assert result == [7], result
    _, manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['pdf']['pages'][0]['status'] == 'cancelled'
    assert manifest['pdf']['completed_pages'] == 0
    assert manifest['pdf']['pages'][0]['render_ms'] > 0
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0
    # 子进程在收到取消后实际失败时，失败代码、页面错误和取消请求均可追溯。
    render_gate.touch()
    render_entered.unlink()
    os.environ['DOCOCR_TEST_RENDER_FAIL'] = '1'
    engine = make_engine('printed_page')
    job = new_job(engine)
    result = []
    worker = threading.Thread(target=lambda: result.append(lib.dococr_job_run(job, c.byref(pdf_request))))
    worker.start()
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not render_entered.exists():
        time.sleep(0.01)
    assert render_entered.exists(), result
    assert lib.dococr_job_cancel(job) == 0
    render_gate.unlink()
    worker.join(timeout=8)
    assert result == [6], result
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['state'] == 'failed' and snapshot['cancel_requested'], snapshot
    assert snapshot['error']['code'] == 'pdf_tool_failed', snapshot
    _, manifest = read_json(lib.dococr_job_manifest, job)
    assert manifest['failure_code'] == 'pdf_tool_failed', manifest
    assert manifest['pdf']['pages'][0]['status'] == 'failed', manifest
    assert manifest['pdf']['pages'][0]['error']['code'] == 'pdf_tool_failed', manifest
    assert manifest['pdf']['completed_pages'] == 0, manifest
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0
    os.environ.pop('DOCOCR_TEST_RENDER_FAIL')
    os.environ.pop('DOCOCR_POPPLER_BIN')
    os.environ.pop('DOCOCR_TEST_RENDER_GATE')
    os.environ.pop('DOCOCR_TEST_RENDER_ENTERED')

    # 20 页产生的事件超过固定 64 条，溢出计数可查且终态事件保留。
    pages = [Image.new('RGB', (4, 4), 'white') for _ in range(20)]
    pdf_stream = io.BytesIO()
    pages[0].save(pdf_stream, format='PDF', save_all=True, append_images=pages[1:], resolution=72)
    pdf = pdf_stream.getvalue()
    pdf_owned = (c.c_uint8 * len(pdf)).from_buffer_copy(pdf)
    pdf_request = Input(c.sizeof(Input), pdf_owned, len(pdf), 5, 0, 0, 0,
                        0, 0, 72, 0, 0)
    engine = make_engine('sample')
    job = new_job(engine)
    assert lib.dococr_job_run(job, c.byref(pdf_request)) == 0
    _, snapshot = read_json(lib.dococr_job_status, job)
    assert snapshot['events_pending'] == 64 and snapshot['events_dropped'] > 0
    events = []
    while True:
        code, event = read_json(lib.dococr_job_next_event, job)
        if code == 8:
            break
        assert code == 0
        events.append(event)
    assert len(events) == 64 and events[-1]['kind'] == 'terminal'
    assert lib.dococr_job_destroy(job) == 0
    assert lib.dococr_destroy(engine) == 0
    gate_dir.cleanup()


if __name__ == '__main__':
    main()
