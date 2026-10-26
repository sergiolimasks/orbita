#!/usr/bin/env python3
"""Download and pretty-print one XCloud360 telemetry blob from R2.

Env vars required:
    R2_ACCOUNT_ID
    R2_ACCESS_KEY_ID
    R2_SECRET_ACCESS_KEY

Usage:
    ./pull.py <s3-key>
    ./pull.py logs/2026-10-25/<console>/<session>-<ms>.json.gz
    ./pull.py <s3-key> --raw             # raw JSON, no pretty-print
    ./pull.py <s3-key> --events          # just the events array, one per line
"""
import argparse
import gzip
import json
import os
import sys

try:
    import boto3
except ImportError:
    sys.exit("missing deps — run: pip install -r requirements.txt")

BUCKET = "xcloud360-logs"


def client():
    account_id = os.environ["R2_ACCOUNT_ID"]
    return boto3.client(
        "s3",
        endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
        aws_access_key_id=os.environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=os.environ["R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("key")
    ap.add_argument("--raw", action="store_true", help="raw JSON (no pretty-print)")
    ap.add_argument("--events", action="store_true", help="print events array one per line")
    args = ap.parse_args()

    s3 = client()
    obj = s3.get_object(Bucket=BUCKET, Key=args.key)
    body = obj["Body"].read()

    try:
        text = gzip.decompress(body).decode("utf-8")
    except (OSError, gzip.BadGzipFile):
        text = body.decode("utf-8", errors="replace")

    data = json.loads(text)

    if args.events:
        for ev in data.get("events", []):
            print(json.dumps(ev, separators=(",", ":"), ensure_ascii=False))
    elif args.raw:
        print(text)
    else:
        print(json.dumps(data, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
