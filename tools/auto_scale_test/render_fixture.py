"""Generate an upright dial for an optional real PaddleOCR smoke test."""
import math
import sys
import cv2
import numpy as np

image = np.full((800, 800, 3), 250, dtype=np.uint8)


def point(angle, radius):
    angle = math.radians(angle)
    return round(400 + radius * math.cos(angle)), round(400 + radius * math.sin(angle))


cv2.circle(image, (400, 400), 350, (20, 20, 20), 2)
for index in range(51):
    major = index % 10 == 0
    angle = 135 + 270 * index / 50
    cv2.line(image, point(angle, 290 if major else 317), point(angle, 335), (15, 15, 15), 4 if major else 2)
    if major:
        text = f"{index / 50:g}"
        (width, height), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 1.1, 2)
        x, y = point(angle, 245)
        cv2.putText(image, text, (x - width // 2, y + height // 2), cv2.FONT_HERSHEY_SIMPLEX, 1.1, (15, 15, 15), 2, cv2.LINE_AA)
cv2.putText(image, "MPa", (365, 465), cv2.FONT_HERSHEY_SIMPLEX, .9, (15, 15, 15), 2)
cv2.line(image, (400, 400), point(210, 230), (15, 15, 15), 5)
cv2.circle(image, (400, 400), 18, (15, 15, 15), -1)
if not cv2.imwrite(sys.argv[1], image):
    raise RuntimeError("Cannot write fixture image")
