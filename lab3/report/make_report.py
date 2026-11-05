"""Create the short lab report after real screenshots and check results are supplied.

Usage: python make_report.py --evidence evidence.json
Paths in evidence.json are relative to that file. Expected keys:
overview, emissive (screenshot paths), checks (list of verified statements),
limitations (optional plain-language statement of remaining limitations).
The report uses the previous lab's cover as the layout reference.
"""

from argparse import ArgumentParser
from io import BytesIO
import json
from pathlib import Path
from xml.sax.saxutils import escape

import pdfplumber
from pypdf import PdfReader, PdfWriter
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.platypus import Frame, Paragraph


ROOT = Path(__file__).resolve().parents[2]


def register_fonts():
    fonts = Path("C:/Windows/Fonts")
    for name, filename in (("Times", "times.ttf"), ("TimesBold", "timesbd.ttf"),
                           ("TimesBoldItalic", "timesbi.ttf")):
        pdfmetrics.registerFont(TTFont(name, str(fonts / filename)))


def cover_page(reference):
    """Keep reference coordinates, Times faces and sizes; change two lines only."""
    stream = BytesIO()
    with pdfplumber.open(reference) as document:
        page = document.pages[0]
        width, height = page.width, page.height
        result = canvas.Canvas(stream, pagesize=(width, height))
        replacements = {"Лабораторная работа №2": "Лабораторная работа №3",
                        "Камера и освещение": "Текстурирование"}
        replaced = set()
        for line in page.extract_text_lines():
            first = line["chars"][0]
            face = first["fontname"]
            font = ("TimesBoldItalic" if "BoldItalic" in face else
                    "TimesBold" if "Bold" in face else "Times")
            size = first["size"]
            original = line["text"]
            text = replacements.get(original, original)
            # The PDF text matrix carries the actual baseline. Reconstructing it
            # from the font ascent would shift the cover on every generation.
            baseline = first["matrix"][5]
            result.setFont(font, size)
            if original in replacements:
                replaced.add(original)
                result.drawCentredString((line["x0"] + line["x1"]) / 2, baseline, text)
            else:
                result.drawString(line["x0"], baseline, text)
        if replaced != set(replacements):
            raise ValueError("The reference cover does not contain the expected lab title")
        result.save()
    return PdfReader(stream).pages[0]


def make_report(evidence_file, output):
    evidence_file = evidence_file.resolve()
    evidence = json.loads(evidence_file.read_text(encoding="utf-8"))
    images = {}
    for key in ("overview", "emissive"):
        path = Path(evidence[key])
        if not path.is_absolute():
            path = evidence_file.parent / path
        if not path.is_file():
            raise FileNotFoundError(path)
        images[key] = path
    checks = evidence.get("checks", [])
    if not checks or any(not isinstance(item, str) or not item.strip() for item in checks):
        raise ValueError("Supply the actual completed checks before generating the report")

    register_fonts()
    content = BytesIO()
    document = canvas.Canvas(content, pagesize=A4)
    width, height = A4
    body = ParagraphStyle("body", fontName="Times", fontSize=14, leading=19,
                          alignment=TA_JUSTIFY, firstLineIndent=30, spaceAfter=9)
    heading = ParagraphStyle("heading", fontName="TimesBold", fontSize=15, leading=21,
                             spaceBefore=7, spaceAfter=9)
    caption = ParagraphStyle("caption", fontName="Times", fontSize=12, leading=16,
                             alignment=TA_CENTER)

    def paragraph(text):
        return Paragraph(escape(text), body)

    def title(text):
        return Paragraph(escape(text), heading)

    def text_page(items, top=None):
        top = top if top is not None else height - 60
        frame = Frame(65, 65, width - 120, top - 65, leftPadding=0, rightPadding=0,
                      topPadding=0, bottomPadding=0)
        frame.addFromList(items, document)
        if items:
            raise RuntimeError("Report content does not fit on the intended page")

    def screenshot_page(title_text, image_path, caption_text, items):
        document.setFont("TimesBold", 16)
        document.drawCentredString(width / 2, height - 65, title_text)
        image = ImageReader(str(image_path))
        image_width, image_height = image.getSize()
        scale = min((width - 80) / image_width, 360 / image_height)
        draw_width, draw_height = image_width * scale, image_height * scale
        bottom = height - 95 - draw_height
        document.drawImage(image, (width - draw_width) / 2, bottom,
                           draw_width, draw_height, mask="auto")
        description = Paragraph(escape(caption_text), caption)
        _, caption_height = description.wrap(width - 100, 100)
        description.drawOn(document, 50, bottom - 12 - caption_height)
        text_page(items, top=bottom - caption_height - 29)
        document.showPage()

    text_page([
        title("Условие"),
        paragraph("Нужно добавить текстуры к объектам второй лабораторной. Вариант 10 - правильный додекаэдр. "
                  "Для оценки 5 нужны выбор материалов, анимированное семплирование, карты блика и свечения, "
                  "динамический UBO и загрузка геометрии в память GPU."),
        title("Метод решения"),
        paragraph("Оставил два додекаэдра, преобразования объектов, камеру, обе проекции и освещение "
                  "из второй работы. Вместо сплошного цвета использовал альбедо. Подготовил три материала "
                  "и добавил их независимый выбор для каждой фигуры через ImGui. Изображения загружаются "
                  "из файлов; для их декодирования используется сторонняя библиотека."),
        paragraph("К позиции и нормали вершины добавил UV. Для каждой пятиугольной грани вычисляю "
                  "плоскую проекцию в её локальном базисе. Все пять вершин используют одно преобразование, "
                  "поэтому внутренние диагонали треугольников не создают разрывов текстуры."),
        paragraph("Свойства моделей записываю в один динамический uniform-буфер. Шаг между структурами "
                  "учитывает minUniformBufferOffsetAlignment устройства. При рисовании передаю смещение "
                  "конкретной модели в vkCmdBindDescriptorSets. Для каждого материала выделяется свой "
                  "набор дескрипторов с текстурами."),
        paragraph("Текстуры, вершины и индексы находятся в памяти GPU. Сначала копирую данные в staging-буфер, "
                  "затем отправляю команды копирования. Для изображений задаю переходы состояний, "
                  "а staging освобождаю после завершения загрузки."),
        paragraph("Для эффекта несколько раз читаю одну альбедо по разным UV, которые меняются со временем, "
                  "и модулирую полученный цвет. Флаг эффекта и его параметры передаю через push constants. "
                  "При отключённом эффекте остаётся обычная выборка текстуры."),
        paragraph("Карта specular задаёт, где появляется блик Блинна-Фонга. Карту emissive прибавляю "
                  "к уже рассчитанному цвету без участия освещения. Если карта блика или свечения отсутствует, "
                  "в её дескриптор подставляю общую чёрную текстуру 1×1."),
    ])
    document.showPage()

    screenshot_page("Результаты", images["overview"],
                    "Рисунок 1. Два додекаэдра с разными материалами. Сохранены камера и источники света.",
                    [paragraph("На сцене можно независимо менять материал каждой фигуры. "
                               "Освещение учитывает текстуру альбедо и маску блика. "
                               "Нетривиальное семплирование включается отдельно для объекта."),
                     title("Проверка"), paragraph(" ".join(checks))])

    final_items = [paragraph("Свечение видно при выключенном освещении: оно прибавляется после "
                             "расчёта Блинна-Фонга. Чёрная служебная текстура позволяет использовать "
                             "материалы без дополнительных карт с тем же шейдером."),
                   title("Вывод"),
                   paragraph("В результате добавлены файловые текстуры, выбор материалов и анимированное "
                             "семплирование. Реализованы карты блика и свечения, динамический UBO и загрузка "
                             "геометрии в память GPU. Возможности второй лабораторной сохранены.")]
    limitation = evidence.get("limitations", "").strip()
    if limitation:
        final_items.append(paragraph(limitation))
    screenshot_page("Свечение без освещения", images["emissive"],
                    "Рисунок 2. Emissive-области сохраняют цвет при отключённом освещении.", final_items)
    document.save()

    result = PdfWriter()
    result.add_page(cover_page(ROOT / "312_Колосов_Лаб2.pdf"))
    for page in PdfReader(content).pages:
        result.add_page(page)
    result.add_metadata({"/Title": "Лабораторная работа №3. Текстурирование", "/Author": "Е. Е. Колосов"})
    if len(result.pages) != 4:
        raise RuntimeError("The report must contain exactly four pages")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as stream:
        result.write(stream)
    if len(PdfReader(output).pages) != 4:
        raise RuntimeError("Saved report has an unexpected page count")
    print(output)


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "312_Колосов_Лаб3.pdf")
    args = parser.parse_args()
    make_report(args.evidence, args.output)
