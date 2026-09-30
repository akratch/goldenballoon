#!/usr/bin/env python3
"""ROM-free unit cover for the cross-architecture determinism gate.

The gate itself needs two executables of different architectures and half an
hour of Rosetta time, so its judgement logic would otherwise only ever be
exercised by the expensive run that is supposed to trust it. These tests pin the
parts that decide PASS or FAIL: the stream parser, the divergence locator, the
vacuity guards (empty stream, same-architecture pair, mismatched configuration,
stale binary) and the report a red run prints.

The vacuity guards get the most cover on purpose. A cross-architecture gate that
compares an arm64 build against an arm64 build reports "identical" and is
believed, so each way that can happen is asserted to be a FAILURE here.
"""

import unittest

import check_crossarch_determinism as gate


def stream(count, digest="0000000000000001", objs=7, start=0):
    """A well-formed capture of ``count`` ticks, all carrying ``digest``."""

    return [gate.SimHashRow(
        tick, objs, digest,
        f"[SIMHASH] tick={tick} objs={objs} h={digest}")
        for tick in range(start, start + count)]


class LipoParsingTests(unittest.TestCase):
    def test_thin_and_universal_slices_are_both_read(self):
        self.assertEqual(gate.parse_lipo_archs("arm64\n"), ("arm64",))
        self.assertEqual(gate.parse_lipo_archs("x86_64 arm64\n"),
                         ("x86_64", "arm64"))

    def test_no_output_reads_as_no_architecture(self):
        self.assertEqual(gate.parse_lipo_archs("   \n"), ())


class ArchitectureGuardTests(unittest.TestCase):
    def test_a_genuine_pair_has_no_complaint(self):
        self.assertEqual(
            gate.architecture_problems(("arm64",), ("x86_64",)), [])

    def test_two_arm64_binaries_are_refused(self):
        problems = gate.architecture_problems(("arm64",), ("arm64",))
        self.assertTrue(problems)
        self.assertIn("expected x86_64", " ".join(problems))

    def test_identical_pair_of_the_same_arch_cannot_pass_silently(self):
        # The check that matters: nothing here may return an empty problem list
        # for a pair that would compare a build against itself.
        for archs in (("arm64",), ("x86_64",)):
            with self.subTest(archs=archs):
                self.assertTrue(gate.architecture_problems(archs, archs))

    def test_universal_binaries_are_refused_on_either_side(self):
        fat = ("x86_64", "arm64")
        self.assertIn("universal",
                      " ".join(gate.architecture_problems(fat, ("x86_64",))))
        self.assertIn("universal",
                      " ".join(gate.architecture_problems(("arm64",), fat)))

    def test_an_unreadable_binary_is_a_failure_not_a_pass(self):
        self.assertTrue(gate.architecture_problems((), ("x86_64",)))


class CMakeCacheTests(unittest.TestCase):
    CACHE = (
        "# comment\n"
        "//a doc line\n"
        "\n"
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "MDKR_APP:BOOL=ON\n"
        "MDKR_APP-ADVANCED:INTERNAL=1\n"
        "not a cache line\n"
    )

    def test_entries_are_read_and_bookkeeping_rows_dropped(self):
        values = gate.parse_cmake_cache(self.CACHE)
        self.assertEqual(values["CMAKE_BUILD_TYPE"], "Release")
        self.assertEqual(values["MDKR_APP"], "ON")
        self.assertNotIn("MDKR_APP-ADVANCED", values)

    def test_matching_configurations_pass(self):
        cache = gate.parse_cmake_cache(self.CACHE)
        self.assertEqual(gate.configuration_problems(cache, dict(cache)), [])

    def test_a_differing_build_type_is_reported(self):
        arm = gate.parse_cmake_cache(self.CACHE)
        x86 = dict(arm, CMAKE_BUILD_TYPE="Debug")
        problems = gate.configuration_problems(arm, x86)
        self.assertEqual(len(problems), 1)
        self.assertIn("CMAKE_BUILD_TYPE", problems[0])

    def test_project_options_are_compared_without_being_listed(self):
        # A new MDKR_* option must be covered the day it is added, so the
        # comparison is driven by what the caches contain, not by a literal
        # list this file would have to be reminded to update.
        arm = {"MDKR_BRAND_NEW_OPTION": "ON"}
        x86 = {"MDKR_BRAND_NEW_OPTION": "OFF"}
        self.assertTrue(gate.configuration_problems(arm, x86))

    def test_a_missing_option_on_one_side_is_a_difference(self):
        self.assertTrue(gate.configuration_problems({"MDKR_X": "ON"}, {}))

    def test_architecture_keys_are_not_compared(self):
        arm = {"CMAKE_OSX_ARCHITECTURES": "arm64",
               "CMAKE_C_COMPILER": "/usr/bin/cc"}
        x86 = {"CMAKE_OSX_ARCHITECTURES": "x86_64",
               "CMAKE_C_COMPILER": "/usr/bin/cc"}
        self.assertEqual(gate.configuration_problems(arm, x86), [])


class StalenessTests(unittest.TestCase):
    def test_nothing_is_claimed_when_no_source_was_found(self):
        self.assertEqual(gate.staleness_problems({}, 0.0, None), [])

    def test_a_binary_older_than_the_newest_source_is_refused(self):
        class Fake:
            def __init__(self, mtime):
                self._mtime = mtime

            def stat(self):
                return type("St", (), {"st_mtime": self._mtime})()

            def __str__(self):
                return "fake-binary"

        problems = gate.staleness_problems(
            {"the x86_64 binary": Fake(10.0)}, 20.0, gate.Path("game/x.c"))
        self.assertEqual(len(problems), 1)
        self.assertIn("older than", problems[0])
        self.assertEqual(
            gate.staleness_problems({"b": Fake(30.0)}, 20.0,
                                    gate.Path("game/x.c")), [])


class RowParsingTests(unittest.TestCase):
    def test_rows_are_read_and_other_logging_ignored(self):
        text = ("[mdkr64] native port dev\n"
                "[SIMHASH] tick=0 objs=3 h=00000000deadbeef\n"
                "[HASHOBJ] tick=0 RNG=00051234\n"
                "[SIMHASH] tick=1 objs=4 h=00000000cafebabe\n")
        rows = gate.parse_simhash_rows(text)
        self.assertEqual([row.tick for row in rows], [0, 1])
        self.assertEqual(rows[1].objs, 4)
        self.assertEqual(rows[1].digest, "00000000cafebabe")

    def test_a_torn_final_line_is_dropped_rather_than_half_parsed(self):
        # A killed run leaves a partial line. Parsing it into a short digest
        # would manufacture a divergence with an I/O cause.
        text = ("[SIMHASH] tick=0 objs=3 h=00000000deadbeef\n"
                "[SIMHASH] tick=1 objs=4 h=0000000")
        self.assertEqual(len(gate.parse_simhash_rows(text)), 1)

    def test_a_prefixed_line_is_not_mistaken_for_a_row(self):
        self.assertEqual(
            gate.parse_simhash_rows(
                "prefix [SIMHASH] tick=0 objs=1 h=0000000000000001\n"), [])


class StreamContractTests(unittest.TestCase):
    def test_a_complete_stream_passes(self):
        self.assertEqual(gate.stream_problems(stream(10), 10, "arm64"), [])

    def test_an_empty_stream_is_a_failure(self):
        problems = gate.stream_problems([], 10, "arm64")
        self.assertTrue(problems)
        self.assertIn("vacuous", problems[0])

    def test_a_short_stream_is_a_failure(self):
        self.assertTrue(gate.stream_problems(stream(9), 10, "arm64"))

    def test_non_contiguous_ticks_are_a_failure(self):
        rows = stream(5)
        rows[3] = rows[3]._replace(tick=99)
        problems = gate.stream_problems(rows, 5, "arm64")
        self.assertTrue(any("contiguous" in p for p in problems))


class DivergenceTests(unittest.TestCase):
    def test_identical_streams_have_no_divergence(self):
        self.assertIsNone(gate.first_divergence(stream(20), stream(20)))

    def test_the_first_differing_tick_is_reported_not_the_last(self):
        arm = stream(20)
        x86 = stream(20)
        x86[5] = x86[5]._replace(digest="000000000000ffff")
        x86[9] = x86[9]._replace(digest="000000000000eeee")
        self.assertEqual(gate.first_divergence(arm, x86), 5)

    def test_a_population_difference_alone_diverges(self):
        arm = stream(5)
        x86 = stream(5)
        x86[2] = x86[2]._replace(objs=8)
        self.assertEqual(gate.first_divergence(arm, x86), 2)

    def test_a_truncated_stream_diverges_at_its_end(self):
        self.assertEqual(gate.first_divergence(stream(10), stream(4)), 4)

    def test_raw_text_alone_never_decides_the_verdict(self):
        # Comparing the printed line would make a future format change look
        # like an architecture divergence.
        arm = stream(3)
        x86 = [row._replace(raw=row.raw.replace("objs", "count"))
               for row in stream(3)]
        self.assertIsNone(gate.first_divergence(arm, x86))


class ReportTests(unittest.TestCase):
    def test_a_value_divergence_names_the_tick_and_both_rows(self):
        arm = stream(10)
        x86 = stream(10)
        x86[4] = x86[4]._replace(digest="00000000000000ff",
                                 raw="[SIMHASH] tick=4 objs=7 "
                                     "h=00000000000000ff")
        report = "\n".join(gate.divergence_report("level 5", arm, x86, 4))
        self.assertIn("tick 4", report)
        self.assertIn("0000000000000001", report)
        self.assertIn("00000000000000ff", report)
        self.assertIn("population agrees", report)
        self.assertIn("MDKR_HASH_DUMP_TICK=3", report)

    def test_a_population_divergence_is_called_out_separately(self):
        arm = stream(10)
        x86 = stream(10)
        x86[4] = x86[4]._replace(objs=9)
        report = "\n".join(gate.divergence_report("level 5", arm, x86, 4))
        self.assertIn("POPULATION differs", report)

    def test_the_report_counts_how_much_of_the_tail_differs(self):
        arm = stream(10)
        x86 = stream(10)
        for index in range(4, 10):
            x86[index] = x86[index]._replace(digest="00000000000000ff")
        report = "\n".join(gate.divergence_report("level 5", arm, x86, 4))
        self.assertIn("4 ticks were identical", report)
        self.assertIn("6 of the 6 remaining ticks differ", report)

    def test_a_truncated_stream_is_reported_as_a_short_run(self):
        report = "\n".join(
            gate.divergence_report("level 5", stream(10), stream(4), 4))
        self.assertIn("different lengths", report)


class PositiveControlTests(unittest.TestCase):
    def test_a_diverging_control_is_accepted(self):
        base = stream(10)
        legacy = stream(10, digest="00000000000000ff")
        self.assertIsNone(gate.control_problem(base, legacy))

    def test_an_identical_control_invalidates_the_whole_run(self):
        problem = gate.control_problem(stream(10), stream(10))
        self.assertIsNotNone(problem)
        self.assertIn("unsupported", problem)


class ContractTests(unittest.TestCase):
    def test_the_gate_runs_the_v3_field_set(self):
        # Every other authoritative-state gate in the tree is v3. A silent
        # downgrade here would compare a narrower field set and call it
        # cross-architecture identity.
        self.assertEqual(gate.HASH_VERSION, "3")

    def test_the_default_routes_include_the_host_dependence_hotspots(self):
        # Levels 41, 37 and 11 are where check_state_hash.py measured a binary
        # disagreeing with itself because authoritative fields were seeded from
        # host memory. An architecture change is exactly the sort of thing that
        # moves that memory, so the default route set must reach them.
        self.assertIn(37, gate.DEFAULT_LEVELS)
        self.assertIn(11, gate.DEFAULT_LEVELS)


class ArchitectureFamilyTests(unittest.TestCase):
    """Apple's slice sub-variants must not read as a different architecture."""

    def test_variants_map_onto_their_family(self):
        self.assertEqual(gate.architecture_family("arm64e"), "arm64")
        self.assertEqual(gate.architecture_family("x86_64h"), "x86_64")

    def test_an_unknown_slice_has_no_family(self):
        self.assertIsNone(gate.architecture_family("i386"))
        self.assertIsNone(gate.architecture_family("ppc"))

    def test_a_variant_pair_still_counts_as_two_architectures(self):
        self.assertEqual(
            gate.architecture_problems(("arm64e",), ("x86_64h",)), [])

    def test_arm64e_against_arm64_is_still_a_self_comparison(self):
        problems = gate.architecture_problems(("arm64e",), ("arm64",))
        self.assertTrue(problems)
        self.assertIn("expected x86_64", " ".join(problems))

    def test_an_unrecognised_slice_fails_closed(self):
        problems = gate.architecture_problems(("i386",), ("x86_64",))
        self.assertTrue(problems)
        self.assertIn("unrecognised", problems[0])


if __name__ == "__main__":
    unittest.main()
