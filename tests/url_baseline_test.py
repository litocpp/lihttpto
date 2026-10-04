import unittest

from url_baseline import compare, usv_string


class UrlBaselineTest(unittest.TestCase):
    def test_success_and_optional_fields(self):
        self.assertEqual(compare({"href": "http://a/"}, {"failure": False, "href": "http://a/"}), {})

    def test_expected_failure_does_not_require_error_text(self):
        self.assertEqual(compare({"failure": True}, {"failure": True, "error": "syntax"}), {})

    def test_acceptance_and_rejection_mismatches(self):
        for expected, actual in [(True, False), (False, True)]:
            self.assertEqual(compare({"failure": expected}, {"failure": actual}),
                             {"failure": {"expected": expected, "actual": actual}})

    def test_missing_origin_is_not_assumed_opaque(self):
        self.assertEqual(compare({"origin": "null"}, {"failure": False}),
                         {"origin": {"expected": "null", "actual": None}})

    def test_multiple_fields_are_reported(self):
        mismatches = compare({"href": "http://a/", "port": ""},
                             {"failure": False, "href": "http://a:80", "port": "80"})
        self.assertEqual(set(mismatches), {"href", "port"})

    def test_usv_conversion_preserves_pairs_and_replaces_lone_surrogates(self):
        self.assertEqual(usv_string("x\ud83d\udca9\ud800\udc00\ud800y\udc00"), "x💩𐀀�y�")
        self.assertEqual(usv_string("雪\x00"), "雪\x00")


if __name__ == "__main__":
    unittest.main()
