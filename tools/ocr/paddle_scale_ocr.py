"""PaddleOCR 3.x adapter. Result file separates JSON from library stdout logs."""
import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

NUMERIC = re.compile(r"^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)$")


def numeric_text(text):
    """Drop a trailing tick stroke, without changing any recognized digit."""
    text = text.strip().replace("．", ".").replace("−", "-")
    if text.endswith("-") and NUMERIC.fullmatch(text.rstrip("-")):
        return text.rstrip("-")
    return text


def refine_tall_digits(pipeline_options, view, tokens, rotation, restore):
    """Recognize narrow boxes upright. The normal OCR crop rotates h/w>=1.5,
    which turns a single vertical '1' sideways before recognizing it.
    """
    import cv2
    import numpy as np
    from paddleocr import TextRecognition

    height, width = view.shape[:2]
    regions = []
    numeric_heights = []
    for token in tokens:
        if token["rotation"] != rotation:
            continue
        polygon = np.asarray(token["view_polygon"], dtype=float)
        left, top = np.floor(polygon.min(axis=0)).astype(int)
        right, bottom = np.ceil(polygon.max(axis=0)).astype(int)
        w, h = right-left, bottom-top
        if NUMERIC.fullmatch(token["text"]) and token["score"] >= .7 and h > min(width,height)*.025:
            numeric_heights.append(h)
        tall = h > 1.3*w
        numeric_fragment = len(token["text"]) <= 6 and any(c.isdigit() for c in token["text"])
        if not ((tall or numeric_fragment) and h > min(width, height)*.025 and h < min(width, height)*.18 and w >= 3):
            continue
        if any(abs(left-r[0])+abs(top-r[1]) < 8 for r in regions):
            continue
        regions.append((max(0,left),max(0,top),min(width,right),min(height,bottom)))
    # A text detector can omit a lone '1' completely. Propose upright connected
    # glyphs of comparable height to detected numbers; recognition still decides.
    if numeric_heights:
        digit_height = float(np.median(numeric_heights))
        gray = cv2.cvtColor(view, cv2.COLOR_BGR2GRAY)
        _, mask = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
        count, _, stats, _ = cv2.connectedComponentsWithStats(mask)
        for left, top, w, h, area in stats[1:count]:
            if not (.75*digit_height <= h <= 1.5*digit_height and 3 <= w <= .3*h and area >= .12*w*h):
                continue
            if any(abs(left-r[0])+abs(top-r[1]) < 12 for r in regions):
                continue
            regions.append((int(left),int(top),int(left+w),int(top+h)))
    if not regions:
        return []
    model_name = pipeline_options.get("text_recognition_model_name", "PP-OCRv5_server_rec")
    recognizer = TextRecognition(model_name=model_name, model_dir=pipeline_options.get("text_recognition_model_dir"),
                                 device="cpu", enable_mkldnn=False)
    crops, owners = [], []
    for region in regions[:64]:
        left, top, right, bottom = region
        if right <= left or bottom <= top:
            continue
        crop = view[top:bottom, left:right]
        # Add a small background margin without rotating the tall glyph.
        pad = max(3, int((bottom-top)*.12))
        crop = cv2.copyMakeBorder(crop, pad, pad, pad, pad, cv2.BORDER_REPLICATE)
        crops.append(cv2.resize(crop, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC))
        owners.append(region)
    result = []
    for prediction, region in zip(recognizer.predict(crops), owners):
        text, confidence = numeric_text(prediction["rec_text"]), float(prediction["rec_score"])
        if not NUMERIC.fullmatch(text) or confidence < .65:
            continue
        left, top, right, bottom = region
        result.append(dict(text=text, score=confidence, rotation=rotation, orientation_preferred=True,
                           method="upright_crop", polygon=[list(restore(float(x),float(y))) for x,y in
                           ((left,top),(right,top),(right,bottom),(left,bottom))]))
    recognizer.close()
    return result


def configure_runtime():
    root = Path(__file__).resolve().parent
    settings = root / "offline.json"
    if not settings.is_file():
        return {}, root / "cache"
    if os.name == "nt":
        import ctypes
        if ctypes.windll.kernel32.GetACP() != 65001:
            raise RuntimeError("Offline OCR needs the packaged UTF-8 Python runtime and Windows 10 1903+ / Windows 11")
    config = json.loads(settings.read_text(encoding="utf-8"))
    options = dict(ocr_version=config["ocr_version"])
    for kind in ("detection", "recognition"):
        directory = (root / config[f"{kind}_model_dir"]).resolve()
        if not directory.is_relative_to(root) or not all((directory / name).is_file() for name in ("inference.json", "inference.pdiparams", "inference.yml")):
            raise FileNotFoundError(f"Offline {kind} model missing or invalid: {directory}")
        options[f"text_{kind}_model_name"] = config[f"{kind}_model_name"]
        options[f"text_{kind}_model_dir"] = str(directory)
    # A read-only installation needs no model downloads or writes beside the EXE.
    cache = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "GaugeReader" / "ocr" / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    os.environ["PADDLE_PDX_CACHE_HOME"] = str(cache / "paddlex")
    os.environ["PADDLE_HOME"] = str(cache / "paddle")
    os.environ["HF_HOME"] = str(cache / "huggingface")
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK"] = "True"

    def deny_network(event, args):
        if event in {"socket.connect", "socket.getaddrinfo", "socket.gethostbyname", "socket.gethostbyaddr", "socket.sendto", "socket.sendmsg"}:
            raise RuntimeError(f"Offline OCR blocked network operation: {event}")

    sys.addaudithook(deny_network)
    return options, cache


def recognize(image):
    bundled_options, cache = configure_runtime()
    os.environ.setdefault("PADDLE_PDX_CACHE_HOME", str(cache))
    os.environ.setdefault("PADDLE_HOME", str(cache / "paddle"))
    os.environ.setdefault("HF_HOME", str(cache / "huggingface"))
    os.environ.setdefault("PADDLE_PDX_MODEL_SOURCE", "bos")
    os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
    import cv2
    import numpy as np
    from paddleocr import PaddleOCR

    options = dict(
        ocr_version=os.environ.get("GAUGE_OCR_VERSION", "PP-OCRv5"),
        device="cpu",
        # Paddle 3.3 Windows oneDNN can reject PIR attributes in PP-OCRv5.
        enable_mkldnn=False,
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=False,
        # Single digits and decimal points occupy much smaller regions than prose.
        text_det_thresh=.2,
        text_det_box_thresh=.3,
    )
    if bundled_options:
        options.update(bundled_options)
        options.pop("ocr_version", None)  # Explicit bundled model names select the version.
    else:
        for kind in ("detection", "recognition"):
            directory = os.environ.get(f"GAUGE_OCR_{kind.upper()}_MODEL_DIR")
            if directory:
                options[f"text_{kind}_model_dir"] = directory
    pipeline = PaddleOCR(**options)
    pixels = cv2.imdecode(np.fromfile(image, dtype=np.uint8), cv2.IMREAD_COLOR)
    if pixels is None:
        raise ValueError("Cannot decode dial image")
    height, width = pixels.shape[:2]
    tokens = []
    # Dial numbers are upright. Do not rotate the image or search other text orientations.
    restore = lambda x, y: (x, y)
    for result in pipeline.predict(pixels):
        data = result.json
        if isinstance(data, str):
            data = json.loads(data)
        data = data.get("res", data)
        texts, scores, polygons = (data[key] for key in ("rec_texts", "rec_scores", "rec_polys"))
        if not (len(texts) == len(scores) == len(polygons)):
            raise ValueError("PaddleOCR returned inconsistent result lengths")
        for text, score, polygon in zip(texts, scores, polygons):
            points = [[float(p[0]), float(p[1])] for p in polygon]
            tokens.append(dict(text=numeric_text(text), raw_text=text, score=float(score), rotation=0,
                               orientation_preferred=True, view_polygon=points, polygon=points))
    tokens.extend(refine_tall_digits(options, pixels, tokens, 0, restore))
    for token in tokens:
        token.pop("view_polygon", None)
    return tokens


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        payload = dict(tokens=recognize(args.image))
    except Exception as error:
        payload = dict(error=f"{type(error).__name__}: {error}")
    Path(args.output).write_text(json.dumps(payload, ensure_ascii=False, allow_nan=False), encoding="utf-8")
    return 1 if "error" in payload else 0


if __name__ == "__main__":
    raise SystemExit(main())
