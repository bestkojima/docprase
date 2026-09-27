"""Create the project-authored two-column printed acceptance page."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


HERE = Path(__file__).resolve().parent
REGULAR = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def main():
    image = Image.new("RGB", (1200, 1500), "white")
    draw = ImageDraw.Draw(image)
    title = ImageFont.truetype(BOLD, 36)
    body = ImageFont.truetype(REGULAR, 25)
    small = ImageFont.truetype(REGULAR, 22)
    draw.text((78, 60), "Printed Science Practice - Form A", font=title, fill="black")
    draw.line((78, 125, 1120, 125), fill="black", width=2)
    draw.text((78, 170), "Section 1: Measurement", font=title, fill="black")
    left = [
        (78, 270, "1. Measure the length of a pencil."),
        (78, 315, "Record the answer in centimeters."),
        (78, 430, "2. A meter has one hundred"),
        (78, 475, "centimeters. Convert 2 m to cm."),
        (78, 610, "3. Explain why repeated readings"),
        (78, 655, "can reduce random error."),
    ]
    right = [
        (640, 270, "4. The chart shows two readings."),
        (640, 315, "Which reading is larger?"),
        (640, 740, "5. Write the result as a sentence."),
        (640, 785, "Include the unit in your answer."),
    ]
    for x, y, line in left + right:
        draw.text((x, y), line, font=body, fill="black")
    draw.rectangle((720, 420, 1040, 630), outline="black", width=3)
    draw.line((770, 585, 990, 585), fill="black", width=3)
    draw.line((770, 585, 770, 460), fill="black", width=3)
    draw.rectangle((810, 520, 865, 585), fill="#4472c4")
    draw.rectangle((905, 475, 960, 585), fill="#4472c4")
    draw.text((805, 600), "A", font=small, fill="black")
    draw.text((905, 600), "B", font=small, fill="black")
    draw.text((720, 660), "Figure 1. Recorded readings.", font=small, fill="black")
    draw.line((78, 910, 1120, 910), fill="black", width=2)
    draw.text((78, 950), "Section 2: Short answer", font=title, fill="black")
    draw.text((78, 1045), "6. State one reason to check a measurement twice.", font=body,
              fill="black")
    draw.text((78, 1150), "End of practice page.", font=body, fill="black")
    image.save(HERE / "printed_science_2col.png", optimize=True)


if __name__ == "__main__":
    main()
