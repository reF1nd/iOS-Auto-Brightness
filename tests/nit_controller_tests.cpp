// SPDX-License-Identifier: GPL-3.0-only
// RefactorNitController port: expected anchors follow the framework's
// addDragOnArrays / enforceMonotonicityAndRestore / blendedNitAtLux by hand.
#include <cstdint>
#include <string>
#include <vector>

#include "check.hpp"
#include "nit_controller.hpp"

using namespace iab;

namespace {

void expect(const NitController& controller, const std::vector<Anchor>& expected) {
  const std::vector<Anchor>& anchors = controller.anchors();
  CHECK_EQ(anchors.size(), expected.size());
  for (size_t i = 0; i < anchors.size() && i < expected.size(); ++i) {
    CHECK_NEAR(anchors[i].lux, expected[i].lux, 1e-3);
    CHECK_NEAR(anchors[i].nit, expected[i].nit, 2e-3);
    CHECK_EQ(anchors[i].user, expected[i].user);
  }
}

void monotonic(const NitController& controller) {
  const std::vector<Anchor>& anchors = controller.anchors();
  CHECK(anchors.size() >= 4 && anchors.size() <= NitController::kMaxAnchors);
  // At most one dragged anchor inside each segment (drags right on a segment
  // end take over that end).
  int inner = 0;
  for (const Anchor& anchor : anchors) {
    if (anchor.user && anchor.lux != 0.0f && anchor.lux != 30.0f && anchor.lux != 600.0f && anchor.lux != 5000.0f) ++inner;
  }
  CHECK(inner <= 3);
  for (size_t i = 1; i < anchors.size(); ++i) {
    CHECK(anchors[i].lux > anchors[i - 1].lux);
    CHECK(anchors[i].nit >= anchors[i - 1].nit);
  }
}

void defaults() {
  const NitController controller;
  expect(controller, {{0, 2, 0, false}, {30, 135, 0, false}, {600, 220, 0, false}, {5000, 1060, 0, false}});
  CHECK_NEAR(controller.logical(15.0f), 68.5, 1e-6);
  CHECK_NEAR(controller.logical(300.0f), 175.263, 1e-6);
  CHECK_NEAR(controller.logical(20000.0f), 1060.0, 1e-6);
  CHECK(!controller.has_good_curve());
  CHECK_NEAR(controller.user_logical(), -1.0, 1e-9);
}

void dark_drag_matches_device() {
  // dumpsys after a drag in the dark: anchors=[(0.0,7.9595423,user),
  // (30.0,135.0,def), (600.0,220.0,def), (5000.0,1060.0,def)].
  NitController controller;
  controller.drag(0.0f, 7.9595423f);
  expect(controller, {{0, 7.9595423f, 1, true}, {30, 135, 0, false}, {600, 220, 0, false}, {5000, 1060, 0, false}});
  CHECK(!controller.has_good_curve());
  CHECK_NEAR(controller.user_lux(), 0.0, 1e-9);
  CHECK_NEAR(controller.logical(0.0f), 7.96, 1e-6);
}

void drag_blends_its_segment() {
  NitController controller;
  controller.drag(10.0f, 100.0f);
  expect(controller,
         {{0, 37.7778f, 0, false}, {10, 100, 1, true}, {30, 135, 0, false}, {600, 220, 0, false}, {5000, 1060, 0, false}});
  CHECK_NEAR(controller.logical(5.0f), 68.889, 1e-3);
  // A second drag in the same segment replaces the first, and the segment
  // ends are blended from the default curve again.
  controller.drag(20.0f, 60.0f);
  expect(controller,
         {{0, 2, 0, false}, {20, 60, 2, true}, {30, 114.5556f, 0, false}, {600, 220, 0, false}, {5000, 1060, 0, false}});
}

void three_segments_three_points() {
  NitController controller;
  controller.drag(10.0f, 100.0f);
  controller.drag(100.0f, 170.0f);
  expect(controller, {{0, 2, 0, false},
                      {10, 100, 0, true},
                      {30, 156.5452f, 0, false},
                      {100, 170, 0, true},
                      {600, 220, 0, false},
                      {5000, 1060, 0, false}});
  controller.drag(2000.0f, 600.0f);
  expect(controller, {{0, 2, 0, false},
                      {10, 100, 0, true},
                      {30, 135, 0, false},
                      {100, 170, 0, true},
                      {600, 296.8595f, 0, false},
                      {2000, 600, 0, true},
                      {5000, 1060, 0, false}});
  CHECK_EQ(controller.user_anchors(), 3);
  controller.drag(300.0f, 200.0f);
  CHECK_EQ(controller.user_anchors(), 3);
  CHECK_EQ(controller.anchors().size(), 7u);
  for (const Anchor& anchor : controller.anchors()) CHECK(anchor.lux != 100.0f);
  monotonic(controller);
}

void contradicting_drag_wins() {
  NitController controller;
  controller.drag(10.0f, 300.0f);
  // Everything brighter than the drag is floored by it.
  expect(controller,
         {{0, 171.1111f, 0, false}, {10, 300, 1, true}, {30, 300, 0, false}, {600, 300, 0, false}, {5000, 1060, 0, false}});
  CHECK(controller.has_good_curve());
  // Brighter light, dimmer slider: the 10 lux drag goes.
  controller.drag(100.0f, 120.0f);
  expect(controller,
         {{0, 2, 0, false}, {30, 120, 0, false}, {100, 120, 2, true}, {600, 216.8763f, 0, false}, {5000, 1060, 0, false}});
  CHECK(!controller.has_good_curve());
  monotonic(controller);
}

void good_curve_pulls_back_bright_drags() {
  NitController controller;
  controller.drag(500.0f, 900.0f);
  CHECK(controller.has_good_curve());
  CHECK_NEAR(controller.logical(500.0f), 900.0, 1e-3);
  bool found = false;
  for (const Anchor& anchor : controller.good_anchors()) {
    if (!anchor.user) continue;
    found = true;
    CHECK_NEAR(anchor.nit, 421.134, 1e-3);
  }
  CHECK(found);
  CHECK(controller.use_good_curve());
  CHECK_NEAR(controller.logical(500.0f), 421.134, 1e-3);
  CHECK(!controller.use_good_curve());
  monotonic(controller);
}

void high_lux_drags_land_on_the_last_anchor() {
  NitController controller;
  controller.drag(20000.0f, 1000.0f);
  CHECK_NEAR(controller.user_lux(), 8000.0, 1e-6);
  CHECK(controller.anchors().back().user);
  CHECK_NEAR(controller.anchors().back().nit, 1000.0, 1e-6);
  CHECK_NEAR(controller.logical(30000.0f), 1000.0, 1e-6);
  // A dragged segment end is never removed by later drags.
  controller.drag(2000.0f, 700.0f);
  CHECK_EQ(controller.user_anchors(), 2);
  monotonic(controller);
}

void random_drags_stay_monotonic() {
  NitController controller;
  uint32_t seed = 12345;
  const auto next = [&]() { return (seed = seed * 1103515245u + 12345u) >> 8; };
  for (int i = 0; i < 2000; ++i) {
    const float lux = static_cast<float>(next() % 9000) * (next() % 2 ? 1.0f : 0.01f);
    const float logical = 1.0f + static_cast<float>(next() % 1059);
    controller.drag(lux, logical);
    monotonic(controller);
    if (next() % 7 == 0) controller.use_good_curve();
  }
}

void learn_file_round_trip() {
  NitController controller;
  controller.drag(10.0f, 100.0f);
  controller.drag(500.0f, 900.0f);
  const std::string text = controller.serialize();
  CHECK(text.find("version=2\n") == 0);
  CHECK(text.find("good=") != std::string::npos);
  NitController restored;
  CHECK(restored.parse(text));
  CHECK_EQ(restored.anchors().size(), controller.anchors().size());
  for (size_t i = 0; i < restored.anchors().size(); ++i) {
    CHECK(restored.anchors()[i].lux == controller.anchors()[i].lux);
    CHECK(restored.anchors()[i].nit == controller.anchors()[i].nit);
    CHECK_EQ(restored.anchors()[i].order, controller.anchors()[i].order);
    CHECK_EQ(restored.anchors()[i].user, controller.anchors()[i].user);
  }
  CHECK(restored.has_good_curve());
  CHECK(restored.user_lux() == controller.user_lux());
  // The order counter continues: the next drag in a full array evicts as before.
  restored.drag(2000.0f, 950.0f);
  controller.drag(2000.0f, 950.0f);
  CHECK(restored.serialize() == controller.serialize());

  // Another ROM's default anchors, earlier formats and broken files are not used.
  RefactorConfig other;
  other.default_curve[3] = 140.0f;
  NitController foreign(other);
  CHECK(!foreign.parse(text));
  CHECK_EQ(foreign.user_anchors(), 0);
  NitController old;
  CHECK(!old.parse("user_offset=0.050000\nanchor=10.000 50.0000 1793000000\n"));
  CHECK(!old.parse(""));
  std::string broken = text;
  broken.replace(broken.find("anchor=0 "), 9, "anchor=99 ");
  CHECK(!old.parse(broken));
  CHECK_EQ(old.user_anchors(), 0);

  // The Zygisk reporter writes Java's Float.toString.
  NitController reported;
  CHECK(reported.parse("version=2\ndefaults=0.0 2.0 30.0 135.0 0.0 600.0 220.0 5000.0 1060.0\norder=2\n"
                       "user=0.0 8.783608\ngood_curve=0\nanchor=0.0 8.783608 2 1\nanchor=30.0 135.0 0 0\n"
                       "anchor=600.0 220.0 0 0\nanchor=5000.0 1060.0 0 0\n"));
  NitController dragged;
  dragged.drag(0.0f, 8.78360844f);
  CHECK(reported.same(dragged));
  dragged.drag(100.0f, 170.0f);
  CHECK(!reported.same(dragged));
}

void adopts_the_framework_state() {
  // The device's dump after the dark drag, and a made-up one with a good curve.
  NitController controller;
  CHECK(controller.adopt("Refactor Nit Controller Configuration:\n  mDisplayId=0\n  mIsHaveGoodCurve=false\n"
                         "  mUserLux=0.0\n  mUserLogicalNit=7.9595423\n"
                         "  anchors=[(0.0,7.9595423,user), (30.0,135.0,def), (600.0,220.0,def), (5000.0,1060.0,def)]\n"
                         "  goodAnchors=[(0.0,2.0,def), (30.0,135.0,def), (600.0,220.0,def), (5000.0,1060.0,def)]\n"));
  expect(controller, {{0, 7.9595423f, 1, true}, {30, 135, 0, false}, {600, 220, 0, false}, {5000, 1060, 0, false}});
  CHECK(!controller.has_good_curve());
  CHECK_NEAR(controller.user_logical(), 7.9595423, 1e-6);
  NitController same;
  same.drag(0.0f, 7.9595423f);
  CHECK(same.serialize() == controller.serialize());

  NitController good;
  CHECK(good.adopt("  mIsHaveGoodCurve=true\n  mUserLux=500.0\n  mUserLogicalNit=900.0\n"
                   "  anchors=[(0.0,2.0,def), (30.0,135.0,def), (500.0,900.0,user), (600.0,900.0,def), (5000.0,1060.0,def)]\n"
                   "  goodAnchors=[(0.0,2.0,def), (30.0,135.0,def), (500.0,421.13,user), (600.0,421.13,def), (5000.0,1060.0,def)]\n"));
  CHECK(good.has_good_curve());
  CHECK(good.use_good_curve());
  CHECK_NEAR(good.logical(500.0f), 421.13, 1e-3);

  // Nothing dragged, or nothing readable: keep our own state.
  NitController plain;
  CHECK(!plain.adopt("  anchors=[(0.0,2.0,def), (30.0,135.0,def), (600.0,220.0,def), (5000.0,1060.0,def)]\n"));
  CHECK(!plain.adopt("  anchors=[(0.0,2.0,user), (30.0,135.0,def)]\n"));
  CHECK(!plain.adopt(""));
  CHECK_EQ(plain.user_anchors(), 0);
}

void reset_restores_defaults() {
  NitController controller;
  controller.drag(10.0f, 100.0f);
  controller.reset();
  expect(controller, {{0, 2, 0, false}, {30, 135, 0, false}, {600, 220, 0, false}, {5000, 1060, 0, false}});
  CHECK(controller.serialize().find("order=0\n") != std::string::npos);
}

}  // namespace

int main() {
  defaults();
  dark_drag_matches_device();
  drag_blends_its_segment();
  three_segments_three_points();
  contradicting_drag_wins();
  good_curve_pulls_back_bright_drags();
  high_lux_drags_land_on_the_last_anchor();
  random_drags_stay_monotonic();
  learn_file_round_trip();
  adopts_the_framework_state();
  reset_restores_defaults();
  return finish("nit_controller_tests");
}
