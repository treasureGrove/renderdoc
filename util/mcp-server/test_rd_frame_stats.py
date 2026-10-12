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


if __name__ == "__main__":
    unittest.main()
