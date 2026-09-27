"""生成非零 CropBox 且旋转 90 度的 PDF；需 ReportLab 5.0.1、pypdf 6.19.0。"""
from io import BytesIO
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from reportlab.pdfgen import canvas

buffer = BytesIO()
doc = canvas.Canvas(buffer, pagesize=(100, 200), invariant=1)
doc.setFont('Helvetica', 12)
doc.drawString(25, 100, 'Crop and rotation')
doc.showPage()
doc.save()
page = PdfReader(BytesIO(buffer.getvalue())).pages[0]
page.cropbox.lower_left = (20, 30)
page.cropbox.upper_right = (80, 150)
page.rotate(90)
writer = PdfWriter()
writer.add_page(page)
with Path(__file__).with_name('rotated_crop.pdf').open('wb') as output:
    writer.write(output)

huge = PdfWriter()
huge.add_blank_page(width=10000, height=10000)
with Path(__file__).with_name('huge_page.pdf').open('wb') as output:
    huge.write(output)
