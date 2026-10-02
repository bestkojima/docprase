"""从公共作业证据生成显式复核记录；仅生成配置，不自动判定水印。"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def candidate_digest(job, diagnostics, candidate_id):
    """与公共张量工件一致的 裁图范围 + little-endian float32 行 + int32 mask 行。"""
    tensors = diagnostics['raw_tensor_assets']
    rows = (job/tensors['fetch_name_0']).read_bytes()
    if len(rows) != 300*7*4:
        raise ValueError('候选张量长度不符')
    with (job/tensors['fetch_name_2']).open('rb') as stream:
        def integer():
            return struct.unpack('<I', stream.read(4))[0]
        if stream.readline() != b'DOCOCR_MASK_RLE_V1\n' or [integer() for _ in range(3)] != [300, 200, 200]:
            raise ValueError('mask 格式不符')
        target = None
        for index in range(300):
            first, runs = integer(), integer()
            if first > 1 or not 0 < runs <= 40000:
                raise ValueError('mask runs 不符')
            row = bytearray()
            for run in range(runs):
                count = integer()
                if not count or len(row)//4 + count > 40000:
                    raise ValueError('mask run 长度不符')
                row.extend(struct.pack('<I', (first+run) % 2)*count)
            if len(row) != 160000:
                raise ValueError('mask 行长度不符')
            if index == candidate_id:
                target = row
        if stream.read(1) or target is None:
            raise ValueError('mask 尾部/候选不符')
    crop = diagnostics['candidates'][candidate_id]['crop_bbox']
    if crop is None:
        raise ValueError('候选已经过滤，不生成新的跳过记录')
    prefix = (json.dumps(crop, separators=(',', ':')) + ':').encode()
    return hashlib.sha256(prefix + rows[candidate_id*28:(candidate_id+1)*28] + target).hexdigest()


def make_review(job, document, candidate_id, decision, reason, page_index=0):
    diagnostics = document.get('layout_diagnostics') or document['pages'][page_index]['layout_diagnostics']
    if not 0 <= candidate_id < diagnostics['candidate_count'] or not reason.strip():
        raise ValueError('候选不存在或理由为空')
    return dict(page_rgb_sha256=diagnostics['candidate_reviews']['page_rgb_sha256'],
                candidate_id=candidate_id, candidate_sha256=candidate_digest(job, diagnostics, candidate_id),
                decision=decision, reason=reason)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--job', type=Path, required=True, help='已用 layout_candidate_reviews: [] 收集证据的作业')
    parser.add_argument('--candidate', type=int, required=True)
    parser.add_argument('--page-index', type=int, default=0, help='PDF 页面数组下标，从 0 起')
    parser.add_argument('--decision', choices=['confirmed_watermark', 'confirmed_decoration', 'suspected', 'keep'], required=True)
    parser.add_argument('--reason', required=True, help='原图内容及复核依据，不得仅以缺少 GT 为由')
    parser.add_argument('--out', type=Path, required=True, help='写出单条复核 JSON，加入 execution.layout_candidate_reviews 数组')
    args = parser.parse_args()
    document = json.loads((args.job/'document.json').read_text())
    review = make_review(args.job, document, args.candidate, args.decision, args.reason, args.page_index)
    with args.out.open('x') as stream:
        json.dump(review, stream, ensure_ascii=False, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
