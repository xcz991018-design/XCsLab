"""Numeric cleanup preserves recognized digits."""
import unittest
from paddle_scale_ocr import numeric_text, narrow_digit_regions

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

if __name__ == "__main__":
    unittest.main()
