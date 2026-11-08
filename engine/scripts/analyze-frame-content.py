#!/usr/bin/env python3
"""Inspect opt-in framegen-vulkan real/generated image sequences; requires NumPy."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct

HEADER = struct.Struct("<17Q")
FIELDS = ("magic", "version", "context", "source", "backend_frame", "generated", "image_index",
          "image", "recorded_ns", "completed_ns", "copy_queue", "caller_queue", "private_queue",
          "width", "height", "bytes", "batch")


def read_capture(path):
    records = []
    partial = False
    with Path(path).open("rb") as stream:
        while header := stream.read(HEADER.size):
            if len(header) != HEADER.size:
                partial = True
                break
            record = dict(zip(FIELDS, HEADER.unpack(header)))
            if (record["magic"] != 0x31544e4f434b414d or record["version"] not in (1, 2) or
                    (record["width"], record["height"], record["bytes"]) != (192, 108, 192*108*4) or
                    record["generated"] not in (0, 1) or not 1 <= record["batch"] <= (128 if record["version"] == 1 else 4096) or
                    record["completed_ns"] < record["recorded_ns"]):
                raise ValueError("invalid/unsupported content record")
            pixels = stream.read(record["bytes"])
            if len(pixels) != record["bytes"]:
                partial = True
                break
            record["pixels"] = pixels
            record["sha256"] = hashlib.sha256(pixels).hexdigest()
            records.append(record)
    return records, partial


def displacement(before, after):
    import numpy as np
    height, width = before.shape
    window = np.outer(np.hanning(height), np.hanning(width))
    a = np.fft.rfft2((before-before.mean())*window)
    b = np.fft.rfft2((after-after.mean())*window)
    cross = b * a.conjugate()
    correlation = np.fft.irfft2(cross / np.maximum(np.abs(cross), 1e-12), s=before.shape)
    y, x = np.unravel_index(np.argmax(correlation), correlation.shape)
    def refine(index, axis, length):
        samples = [correlation[(y+d)%height,x] if axis==0 else correlation[y,(x+d)%width] for d in (-1,0,1)]
        denominator = samples[0] - 2*samples[1] + samples[2]
        fraction = 0.5*(samples[0]-samples[2])/denominator if abs(denominator)>1e-12 else 0
        return float(index if index<=length//2 else index-length)+float(np.clip(fraction,-.5,.5))
    return [refine(x,1,width),refine(y,0,height)], float(correlation[y,x])


def analyze(records):
    import numpy as np
    groups = defaultdict(list)
    for r in records:
        groups[r["context"], r["source"], r["generated"]].append(r)
    pairs = []
    missing = 0
    for (context, source, role), current in sorted(groups.items()):
        if role:
            continue
        previous = groups.get((context,source-1,0),[])
        generated = groups.get((context,source,1),[])
        if len(previous)!=1 or len(current)!=1 or len(generated)!=1:
            missing += 1
            continue
        a,g,b = previous[0],generated[0],current[0]
        if b["backend_frame"]!=a["backend_frame"]+1 or g["backend_frame"]!=b["backend_frame"]:
            missing += 1
            continue
        def gray(r):
            image = np.frombuffer(r["pixels"],dtype=np.uint8).reshape(108,192,4).astype(float)
            return (image[:,:,0]*.299+image[:,:,1]*.587+image[:,:,2]*.114)[12:96,16:176]
        aa,gg,bb = map(gray,(a,g,b))
        source_motion,source_peak = displacement(aa,bb)
        generated_motion,generated_peak = displacement(aa,gg)
        length = float(np.dot(source_motion,source_motion))
        progress = float(np.dot(source_motion,generated_motion)/length) if length>=.25 else None
        pairs.append(dict(context=context,source=source,backend_frame=b["backend_frame"],
            generated_equals_previous=g["sha256"]==a["sha256"],generated_equals_current=g["sha256"]==b["sha256"],
            source_equals_previous=b["sha256"]==a["sha256"],source_motion=source_motion,
            generated_motion=generated_motion,estimated_progress=progress,
            source_peak=source_peak,generated_peak=generated_peak,
            previous_to_generated_mad=float(np.mean(np.abs(gg-aa))),
            generated_to_current_mad=float(np.mean(np.abs(bb-gg))),
            previous_to_current_mad=float(np.mean(np.abs(bb-aa)))))
    moving = [p for p in pairs if not p["source_equals_previous"]]
    return dict(records=len(records),adjacent_source_pairs=len(pairs),unavailable_pairs=missing,
        queue_identities=sorted({(r["copy_queue"],r["caller_queue"],r["private_queue"]) for r in records}),
        moving_pairs=len(moving),generated_exact_previous=sum(p["generated_equals_previous"] for p in moving),
        generated_exact_current=sum(p["generated_equals_current"] for p in moving),pairs=pairs,
        interpretation="Exact equality compares captured downsampled RGBA pixels. Motion/progress are image-registration estimates, not proof of temporal correctness or scanout. Ideal midpoint progress is about 0.5 during coherent camera translation. Completion time is CPU observation at the existing fence, not GPU execution time.")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("--json",type=Path)
    parser.add_argument("--preview",type=Path,help="optional generated/real GIF; additionally requires Pillow")
    args=parser.parse_args()
    try:
        records,partial=read_capture(args.capture)
        result=analyze(records)
        result["partial_last_record"]=partial
        if args.json:
            with args.json.open("x") as stream:json.dump(result,stream,indent=2)
        print(json.dumps({k:v for k,v in result.items() if k!="pairs"},indent=2))
        if args.preview:
            from PIL import Image
            ordered=sorted(records,key=lambda r:(r["context"],r["source"],not r["generated"]))
            images=[Image.frombytes("RGBA",(r["width"],r["height"]),r["pixels"]).convert("RGB") for r in ordered]
            if images:
                with args.preview.open("xb") as stream:
                    images[0].save(stream,format="GIF",save_all=True,append_images=images[1:],duration=80,loop=0)
    except (OSError,ValueError,ImportError) as error:
        parser.exit(1,f"Cannot analyze content capture: {error}\n")
    return 0


if __name__=="__main__":
    raise SystemExit(main())
