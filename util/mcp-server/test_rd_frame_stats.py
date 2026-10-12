"""Unit tests for frame hotspot ranking. No RenderDoc runtime required."""

import unittest

import rd_frame_stats as stats


def _tree():
    return [
        {
            "eid": 1,
            "name": "Shadow",
            "kind": "marker",
            "children": [
                {
                    "eid": 2,
                    "name": "Cascade",
                    "kind": "draw",
                    "num_indices": 3000,
                    "num_instances": 4,
                    "children": [],
                },
                {
                    "eid": 3,
                    "name": "Tiny",
                    "kind": "draw",
                    "num_indices": 6,
                    "num_instances": 1,
                    "children": [],
                },
            ],
        },
        {
            "eid": 4,
            "name": "Lighting",
            "kind": "marker",
            "children": [
                {
                    "eid": 5,
                    "name": "DispatchLight",
                    "kind": "dispatch",
                    "dispatch": (8, 8, 1),
                    "children": [],
                }
            ],
        },
    ]


class FrameStatsTest(unittest.TestCase):
    def test_eid_comes_from_the_first_column_only(self):
        self.assertEqual(stats.eid_from_report_line("| 12 | Shadow | 2 | 0 | 4,002 | 0 |"), 12)
        self.assertIsNone(stats.eid_from_report_line("|  | (unmarked) | 3 | 0 | 90 | 0 |"))
        self.assertIsNone(stats.eid_from_report_line("| 29.49 MB | 2560x1440 | RGBA | 1 | GBuffer |"))

    def test_format_bytes(self):
        self.assertEqual(stats.format_bytes(0), "0 B")
        self.assertEqual(stats.format_bytes(1536), "1.50 KB")
        self.assertEqual(stats.format_bytes(3 * 1024 * 1024), "3.00 MB")

    def test_heaviest_draw_is_instanced_cascade(self):
        flat = stats.flatten(_tree())
        draws = stats.heaviest_draws(flat, limit=5)
        self.assertEqual(draws[0]["eid"], 2)
        self.assertEqual(draws[0]["cost"], 12000)
        self.assertEqual(draws[0]["triangles"], 4000)
        self.assertEqual(draws[0]["marker"], "Shadow")
        self.assertEqual(draws[1]["eid"], 3)

    def test_pass_costs_rank_shadow_above_lighting(self):
        flat = stats.flatten(_tree())
        rows = stats.pass_costs(flat)
        self.assertEqual(rows[0]["name"], "Shadow")
        self.assertEqual(rows[0]["eid"], 1)
        self.assertEqual(rows[0]["draws"], 2)
        self.assertEqual(rows[0]["triangles"], 4002)
        self.assertEqual(rows[1]["name"], "Lighting")
        self.assertEqual(rows[1]["dispatches"], 1)
        self.assertEqual(rows[1]["groups"], 64)

    def test_single_root_marker_drills_one_level(self):
        tree = [
            {
                "eid": 10,
                "name": "Frame",
                "kind": "marker",
                "children": [
                    {
                        "eid": 11,
                        "name": "Shadow",
                        "kind": "marker",
                        "children": [
                            {"eid": 12, "name": "Big", "kind": "draw", "num_indices": 900, "num_instances": 1}
                        ],
                    },
                    {
                        "eid": 13,
                        "name": "UI",
                        "kind": "marker",
                        "children": [
                            {"eid": 14, "name": "Quad", "kind": "draw", "num_indices": 6, "num_instances": 1}
                        ],
                    },
                ],
            }
        ]
        flat = stats.flatten(tree)
        self.assertEqual(stats.grouping_depth(flat), 2)
        rows = stats.pass_costs(flat)
        self.assertEqual(rows[0]["name"], "Frame / Shadow")
        self.assertEqual(rows[0]["eid"], 11)
        self.assertEqual(rows[1]["name"], "Frame / UI")

    def test_search_matches_name_and_pass(self):
        flat = stats.flatten(_tree())
        hits = stats.search_events(flat, "cascade", limit=10)
        self.assertEqual([h["eid"] for h in hits], [2])
        light = stats.search_events(flat, "light", limit=10)
        self.assertIn(4, [h["eid"] for h in light])
        self.assertIn(5, [h["eid"] for h in light])

    def test_zero_instance_count_counts_as_one(self):
        flat = stats.flatten(
            [{"eid": 1, "name": "Draw", "kind": "draw", "num_indices": 30, "num_instances": 0}]
        )
        self.assertEqual(stats.heaviest_draws(flat, 1)[0]["triangles"], 10)

    def test_report_contains_ranked_sections(self):
        flat = stats.flatten(_tree())
        textures = [
            {"name": "TinyTex", "width": 4, "height": 4, "depth": 1, "mips": 1, "format": "R8", "byte_size": 16},
            {
                "name": "GBuffer",
                "width": 2560,
                "height": 1440,
                "depth": 1,
                "mips": 1,
                "format": "R16G16B16A16_FLOAT",
                "byte_size": 2560 * 1440 * 8,
            },
        ]
        buffers = [
            {"name": "VB", "length": 4096},
            {"name": "BigVB", "length": 8 * 1024 * 1024},
        ]
        text = stats.format_report(flat, textures, buffers)
        self.assertIn("## Passes", text)
        self.assertIn("Shadow", text)
        self.assertLess(text.index("GBuffer"), text.index("TinyTex"))
        self.assertLess(text.index("BigVB"), text.index("| 4.00 KB | VB |"))
        self.assertIn("4,002", text)

    def test_shader_filter_and_truncation(self):
        stages = [
            {
                "stage": "Vertex",
                "entry": "VSMain",
                "resource_id": "ResourceId::1",
                "constant_blocks": ["cbPerFrame (64 bytes, 2 vars)"],
                "textures": [],
                "samplers": [],
                "disassembly": "vs",
            },
            {
                "stage": "Pixel",
                "entry": "PSMain",
                "resource_id": "ResourceId::2",
                "constant_blocks": [],
                "textures": ["g_Albedo @ 0:1"],
                "samplers": ["g_Sampler"],
                "disassembly": "X" * 250,
            },
        ]
        text = stats.format_shader_stages(stages, stage_filter="pixel", max_chars=200)
        self.assertIn("PSMain", text)
        self.assertIn("g_Albedo", text)
        self.assertNotIn("VSMain", text)
        self.assertIn("truncated", text)
        missing = stats.format_shader_stages(stages, stage_filter="raygen")
        self.assertIn("No shader stage matched", missing)
        self.assertIn("Vertex", missing)
        self.assertIn("(No shader bound", stats.format_shader_stages([]))


class PassTimingsTest(unittest.TestCase):
    def _flat(self):
        return stats.flatten(_tree())

    def test_children_sum_to_parent_and_parent_timestamp_is_not_added(self):
        flat = self._flat()
        # Marker samples (1 and 4) are large on purpose. They must not be added
        # on top of the child samples.
        durations = {1: 10.0, 2: 0.25, 3: 0.5, 4: 10.0, 5: 0.25, "2": 0.25}
        rolled, frame = stats.rollup_durations(flat, durations)
        self.assertAlmostEqual(rolled[1], rolled[2] + rolled[3])
        self.assertAlmostEqual(rolled[1], 0.75)
        self.assertAlmostEqual(rolled[4], rolled[5])
        self.assertAlmostEqual(rolled[4], 0.25)
        self.assertAlmostEqual(frame, rolled[1] + rolled[4])
        self.assertAlmostEqual(frame, 1.0)

        report = stats.pass_timings(flat, durations=durations)
        by_name = {row["name"]: row for row in report["passes"]}
        self.assertAlmostEqual(by_name["Shadow"]["gpu_seconds"], 0.75)
        self.assertAlmostEqual(by_name["Lighting"]["gpu_seconds"], 0.25)
        self.assertAlmostEqual(by_name["Shadow"]["gpu_ms"], 750.0)
        self.assertAlmostEqual(by_name["Shadow"]["pct"], 75.0)
        self.assertAlmostEqual(by_name["Lighting"]["pct"], 25.0)
        self.assertAlmostEqual(sum(row["pct"] for row in report["passes"]), 100.0)
        self.assertAlmostEqual(report["totals"]["gpu_seconds"], 1.0)
        self.assertEqual(report["passes"][0]["name"], "Shadow")

    def test_nested_parent_timestamp_is_not_added(self):
        tree = [
            {
                "eid": 10,
                "name": "Frame",
                "kind": "marker",
                "children": [
                    {
                        "eid": 11,
                        "name": "Shadow",
                        "kind": "marker",
                        "children": [
                            {"eid": 12, "name": "Big", "kind": "draw", "num_indices": 900, "num_instances": 1},
                            {"eid": 15, "name": "Small", "kind": "draw", "num_indices": 30, "num_instances": 1},
                        ],
                    },
                    {
                        "eid": 13,
                        "name": "UI",
                        "kind": "marker",
                        "children": [
                            {"eid": 14, "name": "Quad", "kind": "draw", "num_indices": 6, "num_instances": 1}
                        ],
                    },
                ],
            }
        ]
        flat = stats.flatten(tree)
        durations = {10: 99.0, 11: 50.0, 12: 0.5, 15: 0.25, 13: 20.0, 14: 0.25}
        rolled, frame = stats.rollup_durations(flat, durations)
        self.assertAlmostEqual(rolled[11], rolled[12] + rolled[15])
        self.assertAlmostEqual(rolled[11], 0.75)
        self.assertAlmostEqual(rolled[13], rolled[14])
        self.assertAlmostEqual(rolled[10], rolled[11] + rolled[13])
        self.assertAlmostEqual(frame, 1.0)
        self.assertGreater(abs(rolled[10] - 99.0), 1.0)

        report = stats.pass_timings(flat, durations=durations)
        by_name = {row["name"]: row for row in report["passes"]}
        self.assertAlmostEqual(by_name["Frame / Shadow"]["gpu_seconds"], 0.75)
        self.assertAlmostEqual(by_name["Frame / UI"]["gpu_seconds"], 0.25)
        self.assertEqual(by_name["Frame / Shadow"]["eid"], 11)
        self.assertAlmostEqual(by_name["Frame / Shadow"]["pct"] + by_name["Frame / UI"]["pct"], 100.0)
        self.assertEqual(by_name["Frame / Shadow"]["triangles"], 310)

    def test_draw_parent_timestamp_is_not_added(self):
        flat = stats.flatten(
            [
                {
                    "eid": 1,
                    "name": "Multi",
                    "kind": "draw",
                    "num_indices": 30,
                    "num_instances": 1,
                    "children": [
                        {"eid": 2, "name": "A", "kind": "draw", "num_indices": 12, "num_instances": 1},
                        {"eid": 3, "name": "B", "kind": "draw", "num_indices": 18, "num_instances": 1},
                    ],
                }
            ]
        )
        rolled, frame = stats.rollup_durations(flat, {1: 10.0, 2: 0.25, 3: 0.5})
        self.assertAlmostEqual(rolled[1], rolled[2] + rolled[3])
        self.assertAlmostEqual(frame, 0.75)
        report = stats.pass_timings(flat, durations={1: 10.0, 2: 0.25, 3: 0.5})
        self.assertAlmostEqual(report["passes"][0]["gpu_seconds"], 0.75)
        self.assertAlmostEqual(report["totals"]["gpu_seconds"], 0.75)

    def test_zero_and_missing_times(self):
        flat = self._flat()
        durations = {1: 8.0, 2: 0.0, 3: float("nan"), 4: float("inf")}
        rolled, frame = stats.rollup_durations(flat, durations)
        self.assertAlmostEqual(rolled[1], 0.0)
        self.assertAlmostEqual(rolled[2], 0.0)
        self.assertNotIn(3, rolled)
        self.assertNotIn(5, rolled)
        self.assertAlmostEqual(rolled[4], 0.0)
        self.assertAlmostEqual(frame, 0.0)
        self.assertAlmostEqual(rolled[1], rolled[2])

        report = stats.pass_timings(flat, durations=durations)
        by_name = {row["name"]: row for row in report["passes"]}
        self.assertAlmostEqual(by_name["Shadow"]["gpu_seconds"], 0.0)
        self.assertAlmostEqual(by_name["Shadow"]["gpu_ms"], 0.0)
        self.assertAlmostEqual(by_name["Shadow"]["pct"], 0.0)
        self.assertAlmostEqual(by_name["Lighting"]["gpu_seconds"], 0.0)
        self.assertTrue(report["sampled"])
        self.assertTrue(report["duration_supported"])

    def test_counter_sums_ignore_parent_samples(self):
        flat = self._flat()
        durations = {2: 0.25, 3: 0.5, 5: 0.25}
        counters = {
            1: {"ps": 1000, "vs": 80},
            2: {"ps": 10, "cs": 1},
            3: {"ps": 5},
            4: {"cs": 999},
            5: {"cs": 64},
        }
        report = stats.pass_timings(
            flat,
            durations=durations,
            counters=counters,
            counter_columns=["ps", "cs", "vs", "samples"],
        )
        by_name = {row["name"]: row for row in report["passes"]}
        self.assertEqual(by_name["Shadow"]["counters"]["ps"], 15)
        self.assertEqual(by_name["Shadow"]["counters"]["cs"], 1)
        self.assertEqual(by_name["Shadow"]["counters"]["vs"], 0)
        self.assertEqual(by_name["Lighting"]["counters"]["cs"], 64)
        self.assertEqual(by_name["Lighting"]["counters"]["ps"], 0)
        self.assertEqual(report["totals"]["counters"]["ps"], 15)
        self.assertEqual(report["totals"]["counters"]["cs"], 65)
        self.assertEqual(report["totals"]["counters"]["vs"], 0)
        self.assertEqual(report["totals"]["counters"]["samples"], 0)
        self.assertIn("PS invocations", report["text"])
        self.assertIn("| 1 | Shadow |", report["text"])
        shadow_line = next(line for line in report["text"].splitlines() if line.startswith("| 1 |"))
        self.assertEqual(stats.eid_from_report_line(shadow_line), 1)

    def test_unsampled_report_keeps_triangles_until_a_sample(self):
        flat = self._flat()
        report = stats.pass_timings(flat)
        self.assertFalse(report["sampled"])
        self.assertFalse(report["duration_supported"])
        self.assertIsNone(report["totals"]["gpu_ms"])
        self.assertEqual(report["passes"][0]["name"], "Shadow")
        self.assertEqual(report["passes"][0]["triangles"], 4002)
        self.assertIsNone(report["passes"][0]["gpu_ms"])
        self.assertIsNone(report["passes"][0]["pct"])
        self.assertIn("after sampling", report["note"])
        self.assertIn("not stored in the capture", report["note"])
        self.assertIn("---", report["text"])
        self.assertIn("4,002", report["text"])

    def test_unsupported_duration_still_sums_counters(self):
        flat = self._flat()
        report = stats.pass_timings(
            flat,
            counters={2: {"cs": 3}, 5: {"cs": 4}},
            duration_supported=False,
            counter_columns=["cs"],
        )
        self.assertTrue(report["sampled"])
        self.assertFalse(report["duration_supported"])
        self.assertIsNone(report["totals"]["gpu_ms"])
        self.assertEqual(report["totals"]["counters"]["cs"], 7)
        self.assertIn("does not support EventGPUDuration", report["note"])

    def test_limit_does_not_shrink_frame_totals(self):
        flat = self._flat()
        durations = {2: 0.25, 3: 0.5, 5: 0.25}
        full = stats.pass_timings(flat, durations=durations)
        limited = stats.pass_timings(flat, durations=durations, limit=1)
        self.assertEqual(len(limited["passes"]), 1)
        self.assertAlmostEqual(limited["totals"]["gpu_seconds"], full["totals"]["gpu_seconds"])
        self.assertEqual(limited["totals"]["triangles"], full["totals"]["triangles"])
        self.assertEqual(limited["totals"]["dispatches"], 1)


if __name__ == "__main__":
    unittest.main()
