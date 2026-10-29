"""Numeric cleanup preserves recognized digits."""
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paddle_scale_ocr import numeric_text, narrow_digit_regions, numeric_component_regions, tighten_numeric_boxes

class NumericTextTests(unittest.TestCase):
    def test_tick_suffix(self):
        self.assertEqual(numeric_text("0.8-"), "0.8")
    def test_no_digit_substitution(self):
        self.assertEqual(numeric_text("80"), "80")
        self.assertEqual(numeric_text("90"), "90")
    def test_negative_sign_preserved_for_later_filter(self):
        self.assertEqual(numeric_text("-0.2"), "-0.2")
    def test_full_width_decimal(self):
        self.assertEqual(numeric_text(" 0．6 "), "0.6")
        self.assertEqual(numeric_text("MPa-"), "MPa-")

class NarrowDigitTests(unittest.TestCase):
    def test_short_serif_digit_survives_decimal_box_height(self):
        import numpy as np
        image = np.full((120, 120, 3), 180, dtype=np.uint8)
        # Decimal OCR box height is 50; the single glyph is only 30 high
        # and its serif makes it wider than the previous 0.3*h cutoff.
        image[40:70, 60:64] = 20
        image[40:44, 54:64] = 20
        self.assertIn((54, 40, 64, 70), narrow_digit_regions(image, [50, 50, 50]))

    def test_short_ticks_are_not_candidates(self):
        import numpy as np
        image = np.full((120, 120, 3), 180, dtype=np.uint8)
        image[40:55, 60:64] = 20
        self.assertEqual(narrow_digit_regions(image, [50]), [])

    def test_no_size_reference(self):
        self.assertEqual(narrow_digit_regions(None, []), [])

class TightNumericBoxTests(unittest.TestCase):
    def setUp(self):
        self.token = dict(text="0.8", raw_text="0.8-", score=.95,
                          polygon=[[100,100],[220,100],[220,180],[100,180]])
        self.group = dict(text="0.8", score=.99, method="component_crop",
                          polygon=[[110,110],[160,110],[160,170],[110,170]])

    def test_verified_glyph_box_excludes_trailing_tick(self):
        tighten_numeric_boxes([self.token], [self.group])
        self.assertEqual(self.token["polygon"], self.group["polygon"])
        self.assertEqual(self.token["raw_text"], "0.8-")
        self.assertEqual(self.token["text"], "0.8")

    def test_disagreement_does_not_change_box(self):
        before = [p[:] for p in self.token["polygon"]]
        self.group["text"] = "0.6"
        tighten_numeric_boxes([self.token], [self.group])
        self.assertEqual(self.token["polygon"], before)

    def test_low_confidence_does_not_change_box(self):
        before = [p[:] for p in self.token["polygon"]]
        self.group["score"] = .89
        tighten_numeric_boxes([self.token], [self.group])
        self.assertEqual(self.token["polygon"], before)

    def test_distant_label_does_not_change_box(self):
        before = [p[:] for p in self.token["polygon"]]
        self.group["polygon"] = [[300,110],[350,110],[350,170],[300,170]]
        tighten_numeric_boxes([self.token], [self.group])
        self.assertEqual(self.token["polygon"], before)


class ComponentLabelTests(unittest.TestCase):
    def test_label_touching_a_long_needle_is_proposed(self):
        import cv2
        import numpy as np
        image = np.full((180, 300, 3), 180, dtype=np.uint8)
        cv2.putText(image, "0.2", (30, 110), cv2.FONT_HERSHEY_SIMPLEX, 1.4, (20, 20, 20), 3)
        cv2.line(image, (10, 112), (250, 112), (20, 20, 20), 3)
        for variant in (image, cv2.GaussianBlur(image, (3, 3), .65)):
            regions = numeric_component_regions(variant, [40, 43])
            self.assertTrue(any(l < 45 < r and l < 90 < r and t < 90 < b and r < 140
                                for l, t, r, b in regions))

    def test_separate_baselines_do_not_form_a_label(self):
        import cv2
        import numpy as np
        image = np.full((180, 300, 3), 180, dtype=np.uint8)
        cv2.putText(image, "0", (30, 60), cv2.FONT_HERSHEY_SIMPLEX, 1.4, (20, 20, 20), 3)
        cv2.putText(image, "2", (70, 140), cv2.FONT_HERSHEY_SIMPLEX, 1.4, (20, 20, 20), 3)
        self.assertEqual(numeric_component_regions(image, [40]), [])

    def test_blank_region_has_no_proposals(self):
        import numpy as np
        self.assertEqual(numeric_component_regions(np.full((100, 100, 3), 180, dtype=np.uint8), [40]), [])

if __name__ == "__main__":
    unittest.main()
