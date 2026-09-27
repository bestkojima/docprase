"""生成项目自写、带可选文本层的两页中文教材 PDF。需 ReportLab 5.0.1。"""
from pathlib import Path

from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas


OUTPUT = Path(__file__).with_name('printed_textbook_2p.pdf')
pdfmetrics.registerFont(TTFont('DocChinese', '/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf'))
doc = canvas.Canvas(str(OUTPUT), pagesize=(595, 842), invariant=1)
doc.setTitle('端侧文档理解：自制两页印刷教材样例')
doc.setAuthor('docprase test fixture')

pages = [
    ('第一章 分数的意义', [
        '分数表示把一个整体平均分成若干份后取其中的一份或几份。',
        '例如，把一张纸平均分成四份，取其中的一份就是四分之一。',
        '分母说明平均分成的份数，分子说明所取的份数。',
        '观察一张图，先找出整体，再数一数阴影部分占几份。',
    ]),
    ('第二章 小数与长度', [
        '测量一支铅笔的长度时，可以先读出整厘米，再读出不足一厘米的部分。',
        '十个一毫米合成一厘米，因此一点五厘米也可以写成十五毫米。',
        '比较两个长度时，应先统一单位，再比较数值大小。',
        '请写出三件教室物品的长度，并说明选择了什么单位。',
    ]),
]
for title, lines in pages:
    doc.setFont('DocChinese', 20)
    doc.drawString(64, 750, title)
    doc.setFont('DocChinese', 14)
    for i, line in enumerate(lines):
        doc.drawString(64, 690 - i * 42, line)
    doc.showPage()
doc.save()
print(OUTPUT)
