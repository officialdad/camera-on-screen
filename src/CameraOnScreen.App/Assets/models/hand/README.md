# MediaPipe hand models — provenance + I/O contract

This directory vendors two ONNX models used by the finger-control feature:

- `palm_detection.onnx` — palm/hand detector
- `hand_landmark.onnx` — 21-point hand landmark regressor

Later tasks (`Task 2` onward) copy the tensor names, shapes, layouts, and value
ranges below **verbatim**. Every value in this file was read from either the
actual ONNX Runtime `InferenceSession` metadata (probe output reproduced at
the bottom) or the source repo's own Python pre/post-processing code — no
value here is a guess.

## Source selection (license gate)

The plan's Global Constraints require Apache-2.0/MIT redistribution rights
and name a primary candidate (Qualcomm AI Hub) with a fallback (PINTO model
zoo) if the primary's license doesn't qualify.

### Primary candidate — REJECTED

**Qualcomm AI Hub, `qualcomm/MediaPipe-Hand-Detection` (Hugging Face)**

```
curl.exe -s https://huggingface.co/api/models/qualcomm/MediaPipe-Hand-Detection
```

- `cardData.license` = `"other"`, repo tag = `license:other` (not
  Apache-2.0/MIT).
- The repo's own `LICENSE` file (`https://huggingface.co/qualcomm/MediaPipe-Hand-Detection/resolve/main/LICENSE`)
  just points to the *original* trained model's license
  (`https://github.com/zmurez/MediaPipePyTorch/blob/master/LICENSE`, itself
  Apache-2.0 by inheritance from Google MediaPipe) — but the AI-Hub-optimized
  export assets that Qualcomm actually distributes (the ONNX/QNN/TFLITE zips
  linked from the model card) are classified `license: other`, i.e. the
  restrictive Qualcomm AI Hub Model License, not Apache-2.0/MIT.
- Per the plan's explicit gate: **REJECT** — fall back to a PINTO-lineage
  source.

Evidence URLs:
- https://huggingface.co/qualcomm/MediaPipe-Hand-Detection (model card, `license: other`)
- https://huggingface.co/api/models/qualcomm/MediaPipe-Hand-Detection (API: `"license": "other"`)

### Fallback — PINTO model zoo direct source: impractical

**`PINTO0309/PINTO_model_zoo`, folder `033_Hand_Detection_and_Tracking`**
(https://github.com/PINTO0309/PINTO_model_zoo/tree/main/033_Hand_Detection_and_Tracking)

- License confirmed Apache-2.0 at the folder level
  (`033_Hand_Detection_and_Tracking/LICENSE`, `Apache License Version 2.0`)
  under an overall MIT-licensed repo (`PINTO_model_zoo/LICENSE`, GitHub API
  `license.spdx_id = MIT`).
- BUT the folder's `download.sh` pulls one monolithic archive:
  `https://s3.ap-northeast-2.wasabisys.com/pinto-model-zoo/033_Hand_Detection_and_Tracking/resources.tar.gz`
  — confirmed via `curl.exe -sI` at **804,684,102 bytes (~767 MiB)**, bundling
  every resolution/precision/framework variant in the folder. Downloading
  ~767 MiB to extract two ONNX files under ~15 MB total is impractical.
- Per the task brief's explicit allowance ("If PINTO zoo files are hosted via
  download scripts/Google Drive and direct download proves impractical,
  other acceptable Apache-2.0/MIT ONNX conversion sources are fine"), moved
  to the next source.

### Chosen source — accepted

**`PINTO0309/hand-gesture-recognition-using-onnx`** (GitHub), commit
`e2deb2e024c855b2d7d875975c4ff3f636b6e83b` (main @ 2026-07-08):
https://github.com/PINTO0309/hand-gesture-recognition-using-onnx

- **License: Apache-2.0.** Evidence:
  - Repo `LICENSE` file (`Apache License, Version 2.0`):
    https://raw.githubusercontent.com/PINTO0309/hand-gesture-recognition-using-onnx/main/LICENSE
    (copied into this directory as `LICENSE.txt`).
  - GitHub API: `curl.exe -s https://api.github.com/repos/PINTO0309/hand-gesture-recognition-using-onnx`
    → `license.spdx_id = "Apache-2.0"`.
- **Provenance lineage:** Google MediaPipe Hands (Apache-2.0) →
  `Kazuhito00/hand-gesture-recognition-using-mediapipe` (linked from this
  repo's `README.md`) → this repo's ONNX conversion. Same PINTO0309 author
  and ecosystem named as the plan's fallback; a well-known, actively
  maintained ONNX-conversion repo of the same MediaPipe Hands models.
- Files used directly from the repo (raw URLs pinned to the commit above):
  - Palm detector: `model/palm_detection/palm_detection_full_inf_post_192x192.onnx`
    (4,842,332 bytes / 4.6 MiB)
  - Hand landmark: `model/hand_landmark/hand_landmark_sparse_Nx3x224x224.onnx`
    (10,631,867 bytes / 10.1 MiB)
  - Both within the required 1–15 MB sanity range; both start with ONNX
    protobuf magic (`08 08 3A ...`, verified via `Format-Hex -Count 16`, not
    an HTML error page).

Note on naming: the landmark file is named `..._sparse_...` in this repo
rather than `..._full_...`. It is still the 224×224, 21-landmark + presence +
handedness MediaPipe hand landmark model (3 outputs match the plan's expected
shape family exactly — see probe output below); no `..._full_...`-named ONNX
export of the landmark model was found in this repo. Recorded here rather
than silently assumed equivalent to "full".

## Model 1: `palm_detection.onnx`

Source file: `model/palm_detection/palm_detection_full_inf_post_192x192.onnx`
(the `_inf_post_` suffix denotes inference **+ post-processing baked into the
graph**).

### Input

| Name    | Shape         | Dtype | Layout |
|---------|---------------|-------|--------|
| `input` | `[1,3,192,192]` | float32 | NCHW |

- **Value range: 0..1** (not −1..1). Evidence — `palm_detection.py`
  `__preprocess`:
  ```python
  padded_image = np.divide(padded_image, 255.0)   # 0..1
  padded_image = padded_image[..., ::-1]           # BGR -> RGB
  padded_image = padded_image.transpose((2, 0, 1))  # HWC -> CHW
  ```
- **Channel order: RGB** (source image is read/decoded as BGR by OpenCV
  convention, then explicitly flipped to RGB before feeding the model).

### Output

Single output tensor (probe-confirmed name is literally self-describing):

| Name | Shape | Dtype |
|------|-------|-------|
| `pdscore_boxx_boxy_boxsize_kp0x_kp0y_kp2x_kp2y` | `[-1,8]` (dynamic N candidate hands) | float32 |

Column order (evidence — `palm_detection.py` `__postprocess`, unpacking each
row as `pd_score, box_x, box_y, box_size, kp0_x, kp0_y, kp2_x, kp2_y = box`):

| Col | Field | Meaning |
|-----|-------|---------|
| 0 | `pd_score` | palm detection confidence score |
| 1 | `box_x` | box center X, **normalized 0..1** relative to the padded square input (`sqn_*` = "square-normalized" in the source's own variable naming) |
| 2 | `box_y` | box center Y, normalized 0..1, same convention |
| 3 | `box_size` | box side length, normalized 0..1 (fraction of the square input side) |
| 4 | `kp0_x`, 5 `kp0_y` | keypoint 0 (x,y), normalized 0..1 |
| 6 | `kp2_x`, 7 `kp2_y` | keypoint 2 (x,y), normalized 0..1 |

`kp0`/`kp2` are 2 of MediaPipe's 7 standard palm-detector keypoints (by the
well-known MediaPipe palm-detector keypoint schema: index 0 = wrist center,
index 2 = middle-finger MCP — external knowledge, not read off this repo's
code, flagged as such). The source's `__postprocess` uses only these two to
derive a rotation angle (`atan2` of `kp2 - kp0`) for aligning the downstream
landmark crop; the other 5 standard keypoints are not exposed by this export.

### Decode status: **post-processed (baked-in), NOT raw SSD anchors**

The probe shows a single dynamic-length `[-1,8]` output — not a fixed
2016-row (192 input) or 2944-row (256 input) raw-anchor tensor. The `_inf_post_`
filename and the postprocess code (which consumes already-normalized
`box_x/box_y/box_size` fields, not anchor-relative regressor deltas) confirm
decode + NMS are baked into the ONNX graph itself. **Task 2's palm decoder
does not need anchor-grid decode logic for this specific export** — the
score-threshold filter (source default `0.60`) is the only postprocessing
left to the caller.

Reference only (raw-anchor layer config, **not applicable to this export**,
recorded per the task brief in case a raw-anchor variant is substituted
later): 2016 raw anchors ↔ 192 input, SSD layers `[(8,2),(16,2),(16,2),(16,2)]`;
2944 raw anchors ↔ 256 input, SSD layers `[(8,2),(16,2),(32,2),(32,2),(32,2)]`.

## Model 2: `hand_landmark.onnx`

Source file: `model/hand_landmark/hand_landmark_sparse_Nx3x224x224.onnx`.

### Input

| Name    | Shape             | Dtype | Layout |
|---------|-------------------|-------|--------|
| `input` | `[-1,3,224,224]` (dynamic batch) | float32 | NCHW |

- **Value range: 0..1**, **channel order RGB** — identical preprocessing to
  the palm model (`hand_landmark.py` `__preprocess` uses the same
  `/255.0` → BGR→RGB flip → `transpose((2,0,1))` sequence).

### Outputs (3, in `get_outputs()`/probe order — matches the source's own
unpacking order `xyz_x21s, hand_scores, left_hand_0_or_right_hand_1s = onnx_session.run(...)`)

| Name | Shape | Dtype | Meaning |
|------|-------|-------|---------|
| `xyz_x21` | `[-1,63]` | float32 | 21 hand landmarks × (x,y,z), **interleaved** `x0,y0,z0, x1,y1,z1, …, x20,y20,z20` |
| `hand_score` | `[-1,1]` | float32 | hand-presence / confidence score for the crop |
| `lefthand_0_or_righthand_1` | `[-1,1]` | float32 | handedness: **0 = left hand, 1 = right hand** |

Landmark order evidence — `hand_landmark.py` `__postprocess`:
```python
rrn_lms = xyz_x21          # raw model output, per hand
rrn_lms = rrn_lms / input_h   # normalizes by 224 (input_h)
...
rescaled_xy = [[v[0], v[1]] for v in zip(rrn_lms[0::3], rrn_lms[1::3])]
# => index 0,3,6,... = x; index 1,4,7,... = y; (index 2,5,8,... = z, unused
#    by this 2D-only postprocess but present per the interleaved x,y,z triple)
```
**Important:** the raw `xyz_x21` output is in **pixel units of the 224×224
input crop**, not pre-normalized to 0..1 — the source code itself divides by
`input_h` (224) to normalize before further use. Downstream code (Task 3/4)
must apply that same `/224.0` (or `/input_h`, `/input_w`) normalization if it
wants 0..1-relative landmark coordinates.

`hand_score` and `lefthand_0_or_righthand_1` names/roles evidence — the same
file's docstring:
```
hand_scores: np.ndarray
    float32[N, 1]
    Hand score.
left_hand_0_or_right_hand_1s: np.ndarray
    float32[N, 1]
    0: Left hand
    1: Right hand
```

## Probe output (raw, reproduced verbatim)

Probe built per the task brief in a scratch dir outside the repo
(`Microsoft.ML.OnnxRuntime` 1.27.0, `dotnet run -- palm_detection.onnx
hand_landmark.onnx`):

```
== ...\palm_detection.onnx
  IN  input: Float [1,3,192,192]
  OUT pdscore_boxx_boxy_boxsize_kp0x_kp0y_kp2x_kp2y: Float [-1,8]
== ...\hand_landmark.onnx
  IN  input: Float [-1,3,224,224]
  OUT xyz_x21: Float [-1,63]
  OUT hand_score: Float [-1,1]
  OUT lefthand_0_or_righthand_1: Float [-1,1]
```

This matches the plan's expected shape family for a **post-processed** palm
detector (`192` input; the raw `[1,2016,18]+[1,2016,1]` family does not
apply here) and for the landmark model (`224`-ish input; `[*,63]` landmarks +
`[*,1]` presence + `[*,1]` handedness).
