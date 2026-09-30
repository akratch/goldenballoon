import unittest
from check_camera_held_shot_faults import verify

BEFORE = "camera_held_chain_probe tick=2135 slot=0 continued=1 prior_length=1 valid=1 target=(0,0)"
PUBLISHED = "camera_held_shot_published tick=2135 slot=0 frame=2200 active=1"
SUMMARY = ("camera_obstruction_observe summary tick=2136 projection_mismatches=0 "
           "resolved={corrected=1 penetrated=0 invalid=0 degraded=0} "
           "probe_degraded=0 target_hidden=0 end")
DETAIL = ("camera_obstruction_observe detail tick=2136 "
          "resolve={status=4 valid=1 corrected=1 discontinuity=1 resolved=(0,0,0) clearance=0 end}")
AFTER = "camera_held_chain_probe tick=2137 slot=0 continued=0 prior_length=0 valid=1 target=(0,0)"


class FaultEvidence(unittest.TestCase):
    def test_positive(self):
        verify("\n".join((BEFORE, PUBLISHED, SUMMARY, DETAIL, AFTER)), "projection")
        verify("\n".join((BEFORE, PUBLISHED, SUMMARY.replace("probe_degraded=0", "probe_degraded=1"))), "query")

    def test_negative_controls(self):
        projection = "\n".join((BEFORE, PUBLISHED, SUMMARY, DETAIL, AFTER))
        query = "\n".join((BEFORE, PUBLISHED, SUMMARY.replace("probe_degraded=0", "probe_degraded=1")))
        bad = [(projection.replace(AFTER, ""), "projection"),
               (projection.replace("status=4", "status=0"), "projection"),
               (projection.replace("continued=0", "continued=1"), "projection"),
               (projection.replace(BEFORE, ""), "projection"),
               (projection.replace("penetrated=0", "penetrated=1"), "projection"),
               (query.replace("probe_degraded=1", "probe_degraded=0"), "query"),
               (query.replace("target_hidden=0", "target_hidden=1"), "query"),
               (query + "\n" + query, "query"), ("", "query")]
        for output, kind in ((projection, "projection"), (query, "query")):
            for replacement in ("", PUBLISHED + "\n" + PUBLISHED,
                                PUBLISHED.replace("slot=0", "slot=1"),
                                PUBLISHED.replace("active=1", "active=0"),
                                PUBLISHED.replace("frame=2200", "frame=-1")):
                bad.append((output.replace(PUBLISHED, replacement), kind))
        for output, kind in bad:
            with self.subTest(kind=kind, output=output), self.assertRaises(ValueError):
                verify(output, kind)


if __name__ == "__main__":
    unittest.main()
