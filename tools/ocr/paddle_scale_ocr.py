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


def narrow_digit_regions(view, numeric_heights):
    """Find single upright glyphs even when the text detector omitted them."""
    import cv2
    import numpy as np

    if not numeric_heights:
        return []
    digit_height = float(np.median(numeric_heights))
    gray = cv2.cvtColor(view, cv2.COLOR_BGR2GRAY)
    _, global_mask = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY_INV | cv2.THRESH_OTSU)
    # Local contrast survives the uneven lighting and liquid on a dial. Keep
    # both segmentations: a stroke can merge with a tick in just one of them.
    block = max(3, int(digit_height * 1.5) | 1)
    local_mask = cv2.adaptiveThreshold(gray, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                                     cv2.THRESH_BINARY_INV, block, 9)
    regions = []
    for mask in (global_mask, local_mask):
        count, _, stats, _ = cv2.connectedComponentsWithStats(mask)
        for left, top, w, h, area in stats[1:count]:
            # A '1' is shorter than a decimal label's detector box (which
            # includes padding). Antialiasing also widens its serif.
            if not (.55*digit_height <= h <= 1.5*digit_height and
                    max(3, .1*h) <= w <= .5*h and area >= .12*w*h):
                continue
            region = (int(left), int(top), int(left+w), int(top+h))
            if region not in regions:
                regions.append(region)
    return regions


def numeric_component_regions(view, numeric_heights):
    """Propose missed labels from neighboring glyphs, independent of OCR text."""
    import cv2
    import numpy as np

    if not numeric_heights:
        return []
    digit_height = float(np.median(numeric_heights))
    gray = cv2.cvtColor(view, cv2.COLOR_BGR2GRAY)
    block = max(3, int(digit_height*1.5) | 1)
    mask = cv2.adaptiveThreshold(gray, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                                 cv2.THRESH_BINARY_INV, block, 9)
    # A needle touching a numeral joins it to a very wide component. Remove
    # long horizontal strokes only for proposing regions; OCR still decides.
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (max(31, int(digit_height*2)), 1))
    lines = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
    mask[cv2.dilate(lines, np.ones((3, 3), dtype=np.uint8)) > 0] = 0
    _, _, stats, _ = cv2.connectedComponentsWithStats(mask)
    glyphs = sorted((tuple(int(v) for v in s[:4]) for s in stats[1:]
                     if .55*digit_height <= s[3] <= 1.5*digit_height and
                     .1*s[3] <= s[2] <= 1.5*s[3] and s[4] >= .12*s[2]*s[3]), key=lambda s: s[0])
    regions = []
    for left, top, width, height in glyphs:
        right, bottom, count = left+width, top+height, 1
        for x, y, w, h in glyphs:
            if x < right or abs(y-top) > .25*height or not .65*height <= h <= 1.5*height:
                continue
            if x-right > .7*height:
                break
            right, bottom, count = x+w, max(bottom, y+h), count+1
            top = min(top, y)
            if count >= 2:
                regions.append((left, top, right, bottom))
            if count == 4:
                break
    # A blurred numeral can remain joined to the needle after line removal.
    # Use a neighboring intact glyph to bound its upper strokes and baseline;
    # do not take the needle's entire component as the text box.
    for left, top, width, height in glyphs:
        for x, y, w, h, _ in stats[1:]:
            if not (left+width <= x <= left+width+.7*height and
                    abs(y-top) <= .25*height and (w > 1.5*height or h > 1.5*height)):
                continue
            upper = mask[y:y+max(1, int(height*.6)), x:x+max(1, int(height*1.5))]
            columns = np.flatnonzero(np.any(upper, axis=0))
            if columns.size and .1*height <= columns[-1]-columns[0]+1 <= .9*height:
                regions.append((left, min(top, int(y)), int(x+columns[-1]+1), top+height))
    return regions


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
        if (NUMERIC.fullmatch(token["text"]) and len(token["text"]) <= 6 and
                token["score"] >= .7 and min(width,height)*.025 < h < min(width,height)*.18):
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
    detected_regions = set(regions)
    for region in narrow_digit_regions(view, numeric_heights):
        if region not in regions:
            regions.append(region)
    grouped_regions = set(numeric_component_regions(view, numeric_heights))
    for region in sorted(grouped_regions):
        left, top, right, bottom = region
        if any(abs((left+right)-(r[0]+r[2]))+abs((top+bottom)-(r[1]+r[3])) < min(bottom-top, r[3]-r[1])*.7
               and (right-left)*(bottom-top) >= .6*(r[2]-r[0])*(r[3]-r[1]) for r in detected_regions):
            continue
        if region not in regions:
            regions.append(region)
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
        # Replicating a tight component's border extends its black stroke into
        # the margin. Use the surrounding background instead.
        context = view[max(0,top-pad):min(height,bottom+pad), max(0,left-pad):min(width,right+pad)]
        background = tuple(float(c) for c in np.percentile(context, 75, axis=(0, 1)))
        crop = cv2.copyMakeBorder(crop, pad, pad, pad, pad, cv2.BORDER_CONSTANT, value=background)
        crops.append(cv2.resize(crop, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC))
        owners.append(region)
    result = []
    for prediction, region in zip(recognizer.predict(crops), owners):
        text, confidence = numeric_text(prediction["rec_text"]), float(prediction["rec_score"])
        if not NUMERIC.fullmatch(text) or confidence < .65:
            continue
        # Component proposals can include tick strokes: require strong numeric
        # group recognition, and only accept '1' from lone-stroke proposals.
        if region in grouped_regions and confidence < .9:
            continue
        if region not in detected_regions and region not in grouped_regions and (text != "1" or confidence < .9):
            continue
        left, top, right, bottom = region
        result.append(dict(text=text, score=confidence, rotation=rotation, orientation_preferred=True,
                           method="component_crop" if region in grouped_regions else "upright_crop", polygon=[list(restore(float(x),float(y))) for x,y in
                           ((left,top),(right,top),(right,bottom),(left,bottom))]))
    recognizer.close()
    return result


def tighten_numeric_boxes(tokens, refinements):
    """A separately recognized glyph group may remove a tick from a text box."""
    def bounds(polygon):
        return (min(p[0] for p in polygon), min(p[1] for p in polygon),
                max(p[0] for p in polygon), max(p[1] for p in polygon))

    groups = [r for r in refinements if r.get("method") == "component_crop" and
              r["score"] >= .9 and NUMERIC.fullmatch(r["text"])]
    for group in groups:
        a, b, c, d = bounds(group["polygon"])
        for token in tokens+refinements:
            if token is group or not NUMERIC.fullmatch(token["text"]):
                continue
            if abs(float(token["text"])-float(group["text"])) > 1e-9:
                continue
            if len(token.get("polygon", [])) != 4:
                continue
            left, top, right, bottom = bounds(token["polygon"])
            if (a >= left-3 and b >= top-3 and c <= right+3 and d <= bottom+3 and
                    (c-a)*(d-b) < .6*(right-left)*(bottom-top)):
                token["polygon"] = [p[:] for p in group["polygon"]]


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
    refinements = refine_tall_digits(options, pixels, tokens, 0, restore)
    tighten_numeric_boxes(tokens, refinements)
    tokens.extend(refinements)
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
