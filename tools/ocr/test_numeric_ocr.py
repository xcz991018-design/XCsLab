"""Numeric cleanup preserves recognized digits."""
import unittest
from paddle_scale_ocr import numeric_text

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

if __name__ == "__main__":
    unittest.main()
