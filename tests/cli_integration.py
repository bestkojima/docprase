"""通过真实 CLI 和公共作业 ABI 检查文件导出；fixture 不参与生产目标。"""
import json
import base64
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def png_2x2() -> bytes:
    # 上行红、绿；下行蓝、白。期望插图裁剪为蓝、白两像素。
    rows = b"\0\xff\0\0\0\xff\0" + b"\0\0\0\xff\xff\xff\xff"
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def png_variant(depth: int, color: int, row: bytes) -> bytes:
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, depth, color, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"\0" + row)) + chunk(b"IEND", b""))


def decode_one_row_rgb(png: bytes) -> bytes:
    assert png.startswith(b"\x89PNG\r\n\x1a\n")
    cursor = 8
    compressed = b""
    width = height = 0
    while cursor < len(png):
        size = struct.unpack_from(">I", png, cursor)[0]
        kind = png[cursor + 4:cursor + 8]
        payload = png[cursor + 8:cursor + 8 + size]
        if kind == b"IHDR":
            width, height, depth, color, *_ = struct.unpack(">IIBBBBB", payload)
            assert depth == 8 and color == 2
        if kind == b"IDAT":
            compressed += payload
        cursor += 12 + size
    assert width == 2 and height == 1
    row = zlib.decompress(compressed)
    filtered = row[0]
    pixels = bytearray(row[1:])
    for i in range(len(pixels)):
        left = pixels[i - 3] if i >= 3 else 0
        if filtered == 1 or filtered == 4:
            pixels[i] = (pixels[i] + left) & 255
        elif filtered == 3:
            pixels[i] = (pixels[i] + left // 2) & 255
        else:
            assert filtered in (0, 2)
    return bytes(pixels)


def run(binary: str, backend: str, source: pathlib.Path, out: pathlib.Path, expected: int = 0):
    result = subprocess.run([binary, "--backend", backend, "--input", str(source),
                             "--out", str(out)], capture_output=True, text=True)
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    return result


def inspect(out: pathlib.Path):
    document = json.loads((out / "document.json").read_text(encoding="utf-8"))
    markdown = (out / "document.md").read_text(encoding="utf-8")
    page = document["pages"][0]
    block_ids = {block["id"] for block in page["blocks"]}
    layout_ids = {block["id"] for block in page["layout_blocks"]}
    region_ids = {region["id"] for region in page["regions"]}
    assert len(block_ids) == len(page["blocks"])
    assert len(layout_ids) == len(page["layout_blocks"])
    assert len(region_ids) == len(page["regions"])
    assert set(page["reading_order"]) == block_ids
    assert all(set(region["source_layout_block_ids"]) <= layout_ids for region in page["regions"])
    assert all(set(block["source_region_ids"]) <= region_ids for block in page["blocks"])
    resources = {asset["path"]: asset for asset in document["resources"]}
    assert len(resources) == len(document["resources"])
    assert all(asset["source_block_id"] in block_ids for asset in document["resources"])
    assert all(block["content"]["resource"] is None or
               block["content"]["resource"] in resources for block in page["blocks"])
    for asset in document["resources"]:
        path = out / asset["path"]
        assert path.is_file(), asset
        assert asset["path"] in markdown
        assert path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    return document, markdown


def main():
    fixture, production = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="dococr-cli-") as work:
        root = pathlib.Path(work)
        source = root / "试卷样例.png"
        source.write_bytes(png_2x2())
        original = source.read_bytes()
        first, second = root / "第一次", root / "第二次"
        run(fixture, "fixture:sample", source, first)
        run(fixture, "fixture:sample", source, second)
        document, markdown = inspect(first)
        assert document["schema_version"] == "1.0"
        assert document["status"] == "ok"
        assert document["pages"][0]["page_id"] == "p0001"
        assert document["pages"][0]["reading_order"] == ["b0001", "b0002"]
        assert [b["type"] for b in document["pages"][0]["blocks"]] == ["text", "image"]
        assert all(b["confidence"] is None for b in document["pages"][0]["blocks"])
        assert all(b["geometry_granularity"] == "region" for b in document["pages"][0]["blocks"])
        assert all(b["candidate_rank"] == 7 for b in document["pages"][0]["layout_blocks"])
        assert len({b["id"] for b in document["pages"][0]["blocks"]}) == 2
        assert "测试文字" in markdown and "![插图](assets/p0001-b0002.png)" in markdown
        assert len(document["resources"]) == 1
        asset = (first / document["resources"][0]["path"]).read_bytes()
        assert decode_one_row_rgb(asset) == bytes([0, 0, 255, 255, 255, 255])
        assert source.read_bytes() == original
        for relative in ["document.json", "document.md", "assets/p0001-b0002.png"]:
            assert (first / relative).read_bytes() == (second / relative).read_bytes()

        blank = root / "blank"
        run(fixture, "fixture:blank", source, blank)
        blank_json, blank_md = inspect(blank)
        assert blank_json["status"] == "blank" and blank_md == ""
        assert blank_json["pages"][0]["blocks"] == [] and blank_json["resources"] == []

        partial = root / "partial"
        run(fixture, "fixture:failure", source, partial)
        partial_json, partial_md = inspect(partial)
        assert partial_json["status"] == "partial"
        assert "[识别失败：b0001]" in partial_md
        assert partial_json["pages"][0]["blocks"][0]["provenance"]["raw_output"] == "partial raw output"
        assert len(partial_json["resources"]) == 1

        bad_encoding = root / "bad-utf8"
        run(fixture, "fixture:invalid_utf8", source, bad_encoding)
        bad_json, _ = inspect(bad_encoding)
        bad_block = bad_json["pages"][0]["blocks"][0]
        assert bad_json["status"] == "partial" and bad_block["status"] == "failed"
        assert bad_block["provenance"]["raw_output"] is None
        assert base64.b64decode(bad_block["provenance"]["raw_output_base64"]) == b"\xfe"
        assert base64.b64decode(bad_block["provenance"]["text_base64"]) == b"\xff"
        assert base64.b64decode(bad_block["error_base64"]) == b"\xfd"

        run(production, "none", source, root / "production", expected=4)
        assert not (root / "production" / "document.json").exists()
        broken = root / "broken.png"
        broken.write_bytes(b"\x89PNG\r\n\x1a\ninvalid")
        run(fixture, "fixture:sample", broken, root / "broken-out", expected=3)
        for suffix, encoded in {
            "alpha": png_variant(8, 6, bytes([255, 0, 0, 128])),
            "rgb16": png_variant(16, 2, bytes([0, 255, 0, 0, 0, 0])),
        }.items():
            path = root / f"{suffix}.png"
            path.write_bytes(encoded)
            run(fixture, "fixture:sample", path, root / suffix, expected=3)


if __name__ == "__main__":
    main()
