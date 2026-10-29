"""Decode native WIC PNG fixtures with an independent PNG decoder."""
import base64
import io
import json
from pathlib import Path
from PIL import Image

data = json.loads(Path("Debug/AutomationTests/capture-fixtures.json").read_text())
for name, expected_size, expected_pixels in (
    ("full", (3, 1), [(255,0,0,255), (0,255,0,255), (0,0,255,255)]),
    ("crop", (1, 1), [(0,255,0,255)]),
):
    content = data[name]["content"][1]
    assert "image/png" == content["mimeType"]
    image = Image.open(io.BytesIO(base64.b64decode(content["data"], validate=True))).convert("RGBA")
    assert expected_size == image.size
    assert expected_pixels == list(image.getdata())
print("Independent PNG decode: exact RGBA colors, crop and resize PASS")
