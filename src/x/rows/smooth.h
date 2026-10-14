// SPDX-License-Identifier: Apache-2.0
// Shared helpers for the SMOOTH rows (60 Hz promotion, re/ledger/SMOOTH.md). Not a mitigation itself.
//
// Master switch: [rows] SMOOTH=0 (default). While it is 0, every SMOOTH-* row refuses to install
// (no signature scan, no patch, no hook), so the build behaves exactly as without the SMOOTH rows.
//
// Scheme (interpolation, legacy M1/M2 timebase): logic invocations draw lerp(tick k-1, tick k, 0.5) =
// k-0.5, render-only invocations draw tick k untouched. Every displayed frame lies between two real
// game states (half a tick of latency, never overshoots); cuts/warps snap. The canonical (game)
// value is written back after the draw, so logic, scripts and collision only see game state.
// Camera (SMOOTH-001) and actor roots (SMOOTH-002) share this timebase; SMOOTH-003 (limbs) adds the
// half-step motion walk on render-only frames.
#pragma once
#include "../fps60.h"

struct SmoothStats {
    volatile LONG interp, interp_cut;      // lerped logic frames, cut snaps
};

// Interpolation cut distance (world units per tick), as the legacy M1/M2 snap (500 units/tick).
static const float SMOOTH_CUT_DIST = 500.0f;

bool smooth_master();                    // [rows] SMOOTH (default 0)
bool smooth_live(int row = 0);           // runtime toggle (F7; Ctrl+F7 turns SMOOTH-00<row> alone off)
void smooth_poll_hotkey();               // F7 edge; called from each SMOOTH row's pre()

// Interpolation: out = a..b at t for a world transform (row-vector, translation in
// row 3, rows = scale x rotation): translation lerp, rotation slerp (quaternion of the row-normalised
// 3x3; a mirrored basis keeps its sign), row lengths lerp, w column from b. Returns false (cut, out
// untouched) when the translation moved more than cut_dist, the rotation turned more than 60 degrees,
// the handedness differs, or the matrix is not affine.
bool smooth_interp(const float* a, const float* b, float t, float cut_dist, float* out);
// Same for a view matrix (world -> view): both are inverted, interpolated as camera transforms (eye
// position + orientation), and the result is inverted back. cut_dist applies to the eye.
bool smooth_interp_view(const float* a, const float* b, float t, float cut_dist, float* out);
// 4x4 helpers, row-vector convention (p' = p . M, translation in row 3).
bool smooth_affine_inverse(const float* m, float* out);   // false if not affine or singular
void smooth_mul(const float* a, const float* b, float* out); // out = a . b (out may not alias)
