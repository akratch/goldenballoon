import unittest
from check_camera_chain_evidence import audit


def row(tick=1, continued=0, length=0, valid=1, recovering=0, remaining=2, returned=0):
    return (f"camera_held_chain_probe tick={tick} slot=0 continued={continued} "
            f"prior_length={length} valid={valid} target=(0.1,-0.2) "
            f"recovering={recovering} remaining={remaining} returned={returned}")


class ChainEvidence(unittest.TestCase):
    def test_return(self):
        self.assertEqual(audit([row(), row(2, 1, 1, recovering=1, remaining=0, returned=1)], [2])["returned"], 1)

    def test_rejected_then_new_seed(self):
        self.assertEqual(audit([row(), row(2, 1, 1, valid=0), row(3)])["accepted"], 2)

    def test_negative_controls(self):
        bad = [[], ["camera_held_chain_probe bad"], [row(continued=1)],
               [row(), row(3, 1, 1)], [row(), row(2, 1, 2)],
               [row(length=1)], [row(recovering=1)],
               [row(remaining="nan")], [row(remaining=-1)],
               [row(returned=1)], [row().replace("0.1,-0.2", "1.1,-0.2")],
               [row(), row(2, 1, 1, valid=0), row(3, 1, 2)],
               [row(), row(2, 1, 1, recovering=1, remaining=0, returned=1), row(3, 1, 2)]]
        for lines in bad:
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                audit(lines)
        with self.assertRaises(ValueError):
            audit([row()], [99])

    def test_contact_fields(self):
        held = " dwell=0.0167 divergence=3.000"
        seed = row() + " rejection=none contact=1 near=0" + held
        self.assertEqual(audit([seed, row(2, 1, 1) + " rejection=none contact=0 near=1" + held])["accepted"], 2)
        with self.assertRaisesRegex(ValueError, "recovery claimed on a contact tick"):
            audit([seed, row(2, 1, 1, recovering=1) + " rejection=none contact=1 near=0" + held])
        with self.assertRaisesRegex(ValueError, "grazing read on a contact tick"):
            audit([row() + " rejection=none contact=1 near=1" + held])
        for bad in (" rejection=none contact=2 near=0" + held, " rejection=none contact=1" + held,
                    " rejection=none contact=0 near=2" + held, " rejection=none contact=1 near=0"):
            with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "malformed"):
                audit([row() + bad])

    def test_bound_fields(self):
        refused = row(valid=0) + " rejection=dwell contact=1 near=0 dwell=2.0167 divergence=nan"
        self.assertEqual(audit([refused])["rejections"], {"dwell": 1})
        self.assertEqual(audit([row(valid=0) + " rejection=divergence contact=1 near=0 "
                                "dwell=0.0167 divergence=50.000"])["rejections"], {"divergence": 1})
        for bad in (" rejection=none contact=1 near=0 dwell=-1 divergence=3.0",
                    " rejection=none contact=1 near=0 dwell=0.1 divergence=nan",
                    " rejection=none contact=1 near=0 dwell=0.1 divergence=181"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                audit([row() + bad])

    def test_rejection_classification(self):
        self.assertEqual(audit([row(valid=0) + " rejection=boom"])["rejections"], {"boom": 1})
        for line in (row() + " rejection=boom", row(valid=0) + " rejection=none",
                     row(valid=0) + " rejection=unknown"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                audit([line])


if __name__ == "__main__":
    unittest.main()
