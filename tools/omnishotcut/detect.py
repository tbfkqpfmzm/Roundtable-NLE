"""
OmniShotCut wrapper for Roundtable NLE.
Uses system ffmpeg binary + subprocess (no ffmpeg-python dependency).
Usage: python detect.py --input video.mp4 --checkpoint path.pth --output out.json
"""
import os, sys, json, argparse, subprocess, tempfile

_REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "repo")
sys.path.insert(0, _REPO)


def find_ffmpeg():
    """Find the ffmpeg binary — prefer Roundtable's bundled one."""
    candidates = [
        os.path.join(os.path.dirname(__file__), "..", "..", "third_party", "ffmpeg", "bin", "ffmpeg.exe"),
        "ffmpeg", "ffmpeg.exe",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    # Try PATH
    for c in ["ffmpeg", "ffmpeg.exe"]:
        if subprocess.call([c, "-version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
            return c
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--output", required=True)
    opt = parser.parse_args()

    for p in [opt.input, opt.checkpoint]:
        if not os.path.exists(p):
            with open(opt.output, "w") as f:
                json.dump({"error": f"File not found: {p}"}, f)
            sys.exit(1)

    ffmpeg_bin = find_ffmpeg()
    if not ffmpeg_bin:
        with open(opt.output, "w") as f:
            json.dump({"error": "ffmpeg binary not found"}, f)
        sys.exit(1)

    # ── Patch torch.load BEFORE any OmniShotCut imports ──
    import torch
    torch.load = lambda *a, **kw: torch.serialization.load(*a, **{**kw, "weights_only": False})

    import numpy as np

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Device: {device}", flush=True)

    from architecture.backbone import build_backbone
    from architecture.transformer import build_transformer
    from architecture.model import OmniShotCut
    from datasets.transforms import Video_Augmentation_Transform
    from test_code.inference import (
        get_video_fps_safe, split_videos,
        prune_non_context_ranges, merge_ranges,
    )

    # ── Load model ──
    ckpt = torch.load(opt.checkpoint, map_location="cpu")
    args = ckpt["args"]
    backbone = build_backbone(args)
    transformer = build_transformer(args)
    model = OmniShotCut(
        backbone, transformer,
        num_intra_relation_classes=args.num_intra_relation_classes,
        num_inter_relation_classes=args.num_inter_relation_classes,
        num_frames=args.max_process_window_length,
        num_queries=args.num_queries,
        aux_loss=args.aux_loss,
    )
    model.load_state_dict(ckpt["model"], strict=True)
    model.to(device)
    model.eval()
    print(f"Model loaded on {device}", flush=True)

    # ── Read video frames via ffmpeg CLI → raw rgb24 pipe ──
    fps = get_video_fps_safe(opt.input)
    pw, ph = args.process_width, args.process_height
    cmd = [
        ffmpeg_bin, "-hide_banner", "-loglevel", "error",
        "-i", opt.input,
        "-f", "rawvideo", "-pix_fmt", "rgb24",
        "-s", f"{pw}x{ph}",
        "-vsync", "passthrough",
        "pipe:1"
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    raw, stderr = proc.communicate()
    if proc.returncode != 0:
        with open(opt.output, "w") as f:
            json.dump({"error": f"ffmpeg failed: {stderr.decode()[:500]}"}, f)
        sys.exit(1)

    video = np.frombuffer(raw, np.uint8).reshape(-1, ph, pw, 3)

    # ── Run inference ──
    video_transform = Video_Augmentation_Transform(set_type="val")
    ctx = 0
    pred_ranges_full, intra_full, inter_full = [], [], []
    window = args.max_process_window_length

    total_clips = max(1, len(video) / (window - 2 * ctx))
    for clip_idx, (clip, pad) in enumerate(split_videos(video, window, ctx)):
        tensor = video_transform(clip).unsqueeze(0).to(device)
        with torch.inference_mode():
            out = model(tensor)

        pi = out["intra_clip_logits"].softmax(-1)[0, :, :-1]
        pj = out["inter_clip_logits"].softmax(-1)[0, :, :-1]
        pr = out["pred_shot_logits"].softmax(-1)[0, :, :-1]

        ranges, intras, inters = [], [], []
        start = 0
        for k in range(pi.shape[0]):
            end = int(pr[k].argmax())
            if start < end:
                ranges.append([start, end])
                intras.append(int(pi[k].argmax()))
                inters.append(int(pj[k].argmax()))
            start = end
            if end >= window - pad:
                break

        ranges, intras, inters = prune_non_context_ranges(ranges, intras, inters, window, ctx)
        merge_ranges(pred_ranges_full, intra_full, inter_full, ranges, intras, inters)

        pct = int(100 * (clip_idx + 1) / total_clips)
        print(f"Progress: {pct}%", flush=True)

    cuts = []
    for r in pred_ranges_full:
        f = int(r[0])
        cuts.append({"frame": f, "time": round(f / fps if fps else 0.0, 3), "score": 1.0})

    result = {"cuts": cuts, "total_frames": int(len(video)), "fps": round(fps, 2)}
    with open(opt.output, "w") as f:
        json.dump(result, f, indent=2)
    print(f"Done. {len(cuts)} cuts.", flush=True)


if __name__ == "__main__":
    main()
