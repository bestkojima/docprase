"""由已存核验记录重建对照图；不生成或修改内容裁决。"""
import argparse
import math
from pathlib import Path
import json

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.output or root / 'output/issue31-visual-review-20261003'
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 20)
    for file in sorted((root / 'docs/issue-31/content-review').glob('*.json')):
        if not file.stem.startswith(('development-', 'evaluation-')):
            continue
        record = json.loads(file.read_text())
        source = Path(record['source']['path'])
        if not source.is_file():
            source = root / source.relative_to('/home/dr/project/docprase')
        with Image.open(source) as original:
            image = original.convert('RGB')
        cards = []
        for item in record['items']:
            if item['kind'] == 'inline':
                continue  # 行内式以完整父段落核验，避免失去上下文。
            left, top, right, bottom = item['bbox']
            crop = image.crop((math.floor(left), math.floor(top), math.ceil(right), math.ceil(bottom)))
            factor = min(2.0, 1250 / crop.width)
            crop = crop.resize((round(crop.width * factor), round(crop.height * factor)))
            label = f"{item['id']} / {item.get('block_id')} / {'EXACT' if item['strict_exact'] else 'DIFF'}"
            cards.append((label, crop))
        sheets, batch, height = [], [], 0
        for label, crop in cards:
            if batch and height + crop.height + 38 > 1850:
                sheets.append(batch)
                batch, height = [], 0
            batch.append((label, crop))
            height += crop.height + 38
        if batch:
            sheets.append(batch)
        folder = output / record['page']
        folder.mkdir(parents=True, exist_ok=True)
        for number, batch in enumerate(sheets, 1):
            sheet = Image.new('RGB', (1300, sum(c.height + 38 for _, c in batch) + 10), '#eee')
            draw = ImageDraw.Draw(sheet)
            y = 0
            for label, crop in batch:
                draw.text((8, y + 4), label, font=font, fill='black')
                sheet.paste(crop, (8, y + 32))
                y += crop.height + 38
            sheet.save(folder / f'sheet-{number:02}.jpg', quality=94)
    print(f'对照图：{output}')


if __name__ == '__main__':
    main()
