import unittest
from check_camera_chain_evidence import ROW, audit

# Rows exactly as the runtime prints them (camera_obstruction_runtime.c), cut
# from a build-rel run of check_camera_motion_quality's first route at
# MDKR_CAMERA_TRACE=2 (3629d458). The rows above are written by this file and
# can only ever agree with the parser; these are what it is for. The held-shot
# fault gate audits every row of its own live runs as well.
REAL_SEED = ("camera_held_chain_probe tick=669 slot=0 continued=0 prior_length=0 valid=1 "
             "target=(0.02327,-0.23060) recovering=0 remaining=0.65183 returned=0 "
             "rejection=none contact=1 near=0 dwell=0.0167 divergence=0.306 leash=0.000 "
             "leash_from=nan")
REAL_CONTINUED = ("camera_held_chain_probe tick=670 slot=0 continued=1 prior_length=1 valid=1 "
                  "target=(0.02327,-0.23060) recovering=0 remaining=1.23307 returned=0 "
                  "rejection=none contact=1 near=0 dwell=0.0333 divergence=0.575 leash=0.000 "
                  "leash_from=nan")
REAL_BOOM = ("camera_held_chain_probe tick=688 slot=0 continued=1 prior_length=19 valid=0 "
             "target=(0.02327,-0.23060) recovering=0 remaining=nan returned=0 rejection=boom "
             "contact=1 near=0 dwell=0.3333 divergence=1.520 leash=0.000 leash_from=nan")
REAL_ENDPOINT = ("camera_held_chain_probe tick=717 slot=0 continued=1 prior_length=22 valid=0 "
                 "target=(nan,nan) recovering=0 remaining=nan returned=0 rejection=endpoint "
                 "contact=1 near=0 dwell=0.3833 divergence=0.411 leash=0.000 leash_from=nan")
REAL_FRAMING = ("camera_held_chain_probe tick=1356 slot=0 continued=0 prior_length=0 valid=0 "
                "target=(0.92818,1.59895) recovering=0 remaining=nan returned=0 "
                "rejection=framing contact=1 near=0 dwell=0.0167 divergence=0.321 leash=0.000 "
                "leash_from=nan")
REAL_RETURN = ("camera_held_chain_probe tick=1015 slot=0 continued=1 prior_length=92 valid=1 "
               "target=(0.00446,-0.13072) recovering=1 remaining=0.00000 returned=1 "
               "rejection=none contact=0 near=0 dwell=0.0000 divergence=0.000 leash=0.000 "
               "leash_from=nan")


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

    def test_leash_field(self):
        held = " rejection=none contact=1 near=0 dwell=0.0167 divergence=46.000"
        self.assertEqual(audit([row() + held + " leash=1.500 leash_from=47.500"])["accepted"], 1)
        self.assertEqual(audit([row() + held + " leash=0.000 leash_from=nan"])["accepted"], 1)
        self.assertEqual(audit([row() + held])["accepted"], 1)
        # A refused leashed view is not judged on its turn.
        self.assertEqual(audit([row(valid=0) + held.replace("=none", "=endpoint") +
                                " leash=1.500 leash_from=46.000"])["rejected"], 1)
        with self.assertRaisesRegex(ValueError, "invalid leash turn"):
            audit([row() + held + " leash=30.000 leash_from=76.000"])
        # Planned but not applied, or with no measurement of the view before it.
        for after in (" leash=1.500 leash_from=46.000", " leash=1.500 leash_from=45.000",
                      " leash=1.500 leash_from=nan", " leash=1.500"):
            with self.subTest(after=after), \
                    self.assertRaisesRegex(ValueError, "leash did not reduce the divergence"):
                audit([row() + held + after])
        with self.assertRaisesRegex(ValueError, "leash turned a recovering view"):
            audit([row(), row(2, 1, 1, recovering=1) +
                   " rejection=none contact=0 near=0 dwell=0.0000 divergence=46.000 leash=1.500"])

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

    def test_real_production_rows(self):
        # Every field the runtime prints is one the parser reads: the tail
        # groups are optional for older logs, so a match alone would accept a
        # row whose contact/leash fields no longer parse.
        for line in (REAL_SEED, REAL_CONTINUED, REAL_BOOM, REAL_ENDPOINT,
                     REAL_FRAMING, REAL_RETURN):
            with self.subTest(line=line):
                match = ROW.search(line)
                self.assertIsNotNone(match, "the production row no longer parses")
                self.assertEqual(match.end(), len(line))
                self.assertTrue(all(group is not None for group in match.groups()))
        self.assertEqual(audit([REAL_SEED, REAL_CONTINUED])["accepted"], 2)
        self.assertEqual(audit([REAL_FRAMING])["rejections"], {"framing": 1})
        # Out of its chain, a real continuation is refused for its history.
        with self.assertRaisesRegex(ValueError, "continuation lacks matching accepted history"):
            audit([REAL_BOOM])

    def test_rejection_classification(self):
        self.assertEqual(audit([row(valid=0) + " rejection=boom"])["rejections"], {"boom": 1})
        for line in (row() + " rejection=boom", row(valid=0) + " rejection=none",
                     row(valid=0) + " rejection=unknown"):
            with self.subTest(line=line), self.assertRaises(ValueError):
                audit([line])


if __name__ == "__main__":
    unittest.main()
