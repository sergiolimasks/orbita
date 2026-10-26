#!/usr/bin/env python3
"""List recent XCloud360 telemetry sessions stored in R2.

Env vars required:
    R2_ACCOUNT_ID
    R2_ACCESS_KEY_ID
    R2_SECRET_ACCESS_KEY

Usage:
    ./list.py                       # last 24h
    ./list.py --days 7              # last 7 days
    ./list.py --console <id>        # filter by console_id prefix
    ./list.py --game ASSASSINS...   # filter by game (metadata)
"""
import argparse
import os
import sys
from datetime import datetime, timedelta, timezone

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
    ap.add_argument("--days", type=int, default=1)
    ap.add_argument("--console", help="filter by console_id prefix")
    ap.add_argument("--game", help="filter by game metadata")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    s3 = client()
    today = datetime.now(timezone.utc).date()
    found = 0

    for i in range(args.days):
        date = today - timedelta(days=i)
        prefix = f"logs/{date.isoformat()}/"
        if args.console:
            prefix += args.console + "/"

        paginator = s3.get_paginator("list_objects_v2")
        for page in paginator.paginate(Bucket=BUCKET, Prefix=prefix):
            for obj in page.get("Contents", []):
                key = obj["Key"]
                size = obj["Size"]
                mtime = obj["LastModified"]

                if args.game or args.verbose:
                    head = s3.head_object(Bucket=BUCKET, Key=key)
                    meta = head.get("Metadata", {})
                    if args.game and args.game not in meta.get("game", ""):
                        continue
                    extra = f"  game={meta.get('game','?')}  v={meta.get('app_version','?')}  reason={meta.get('flush_reason','?')}"
                else:
                    extra = ""

                print(f"{mtime.isoformat()}  {size:>7}B  {key}{extra}")
                found += 1

    print(f"\n{found} session(s) in last {args.days} day(s)", file=sys.stderr)


if __name__ == "__main__":
    main()
