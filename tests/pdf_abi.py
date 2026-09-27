"""旧 48 字节 DocOcrInput 与扩展 PDF 选项的公共二进制边界。"""
import ctypes as c
import json
from pathlib import Path
import sys
import tempfile

from PIL import Image


class View(c.Structure):
    _fields_ = [('data', c.c_char_p), ('size', c.c_size_t)]


class Bytes(c.Structure):
    _fields_ = [('data', c.POINTER(c.c_uint8)), ('size', c.c_size_t),
                ('allocation_id', c.c_uint64)]


class OldInput(c.Structure):
    _fields_ = [('struct_size', c.c_uint32), ('data', c.POINTER(c.c_uint8)),
                ('size', c.c_size_t), ('format', c.c_uint32), ('width', c.c_uint32),
                ('height', c.c_uint32), ('row_stride', c.c_size_t)]


class NewInput(c.Structure):
    _fields_ = OldInput._fields_ + [('first_page', c.c_uint32),
                                    ('last_page', c.c_uint32), ('dpi', c.c_uint32),
                                    ('max_page_pixels', c.c_uint64)]


class Result(c.Structure):
    _fields_ = [('struct_size', c.c_uint32), ('json', Bytes), ('markdown', Bytes)]


def main():
    library = c.CDLL(sys.argv[1])
    library.dococr_create.argtypes = [View, c.POINTER(c.c_uint64)]
    library.dococr_job_create.argtypes = [c.c_uint64, c.POINTER(c.c_uint64)]
    library.dococr_job_run.argtypes = [c.c_uint64, c.c_void_p]
    library.dococr_job_result.argtypes = [c.c_uint64, c.POINTER(Result)]
    library.dococr_job_manifest.argtypes = [c.c_uint64, c.POINTER(Bytes)]
    library.dococr_bytes_free.argtypes = [c.POINTER(Bytes)]
    library.dococr_job_destroy.argtypes = [c.c_uint64]
    library.dococr_destroy.argtypes = [c.c_uint64]
    with tempfile.TemporaryDirectory(prefix='dococr-pdf-abi-') as temporary:
        path = Path(temporary) / '两页.pdf'
        first = Image.new('RGB', (2, 2), 'white')
        first.save(path, save_all=True, append_images=[Image.new('RGB', (2, 2), 'red')])
        data = path.read_bytes()
        owned = (c.c_uint8 * len(data)).from_buffer_copy(data)
        engine = c.c_uint64()
        name = b'fixture:sample'
        assert library.dococr_create(View(name, len(name)), c.byref(engine)) == 0
        assert c.sizeof(OldInput) == 48
        for request, pages in [
            (OldInput(c.sizeof(OldInput), owned, len(data), 5, 0, 0, 0), [1, 2]),
            (NewInput(c.sizeof(NewInput), owned, len(data), 5, 0, 0, 0,
                      2, 2, 72, 1000), [2]),
        ]:
            job = c.c_uint64()
            assert library.dococr_job_create(engine, c.byref(job)) == 0
            assert library.dococr_job_run(job, c.byref(request)) == 0
            result = Result(c.sizeof(Result))
            assert library.dococr_job_result(job, c.byref(result)) == 0
            document = json.loads(c.string_at(result.json.data, result.json.size))
            assert [page['pdf_page_number'] for page in document['pages']] == pages
            assert all(page['page_id'] == f'p{number:04d}' for page, number in
                       zip(document['pages'], pages))
            assert library.dococr_bytes_free(c.byref(result.json)) == 0
            assert library.dococr_bytes_free(c.byref(result.markdown)) == 0
            manifest_bytes = Bytes()
            assert library.dococr_job_manifest(job, c.byref(manifest_bytes)) == 0
            manifest = json.loads(c.string_at(manifest_bytes.data, manifest_bytes.size))
            assert [item['pdf_page_number'] for item in manifest['pdf']['pages']] == pages
            assert library.dococr_bytes_free(c.byref(manifest_bytes)) == 0
            assert library.dococr_job_destroy(job) == 0
        assert library.dococr_destroy(engine) == 0


if __name__ == '__main__':
    main()
