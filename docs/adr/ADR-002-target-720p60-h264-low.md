# ADR-002 — Target: 720p60 H.264 low complexity

**Status:** accepted (spec §3)

## Decision

- Resolution: 1280x720 (locked).
- Frame rate goal: 60 FPS. Acceptable initial: 55–60 FPS sustained. Fallback for diagnosis only: 30 FPS.
- Codec: H.264. SDP preference order:
  1. profile-level-id `420...` (investigated as Low/Baseline)
  2. `42e...`
  3. `4d...`

## Notes

- Actual negotiated profile MUST be logged and verified — do not assume Baseline just because requested.
- 30 FPS is not the final goal without proof that 60 FPS is technically impossible per §42 exhaustion checklist.
- Slice count per frame at 720p Low is UNKNOWN — must be measured by `tools/xcloud-capture` (P5) before deciding slice threading.

## Related

- Spec §3, §16, §17, §42
