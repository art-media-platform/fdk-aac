#!/usr/bin/env python3
"""Static file server that follows the AMP portal's serving rules FMOD depends on: first body bytes at once,
Range (206) with Content-Length, never a short body, never a redirect. Logs every request as one JSON line
(Range requests per open and seek can be counted from it).

    python3 serve_range.py <dir> <port> <log.jsonl>
"""
import http.server
import json
import os
import re
import sys
import threading
import time

MIME = {
    ".aac": "audio/aac", ".m4a": "audio/mp4", ".mp4": "audio/mp4", ".mp3": "audio/mpeg",
    ".wav": "audio/wav", ".flac": "audio/flac", ".ogg": "audio/ogg",
}
LOCK = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def record(self, **fields):
        fields["t"] = round(time.time(), 4)
        with LOCK, open(self.server.log_path, "a") as log:
            log.write(json.dumps(fields) + "\n")

    def do_GET(self):
        path = os.path.join(self.server.root, os.path.basename(self.path.split("?")[0]))
        if not os.path.isfile(path):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self.record(path=self.path, status=404)
            return
        size = os.path.getsize(path)
        start, end, status = 0, size - 1, 200
        header = self.headers.get("Range")
        if header:
            match = re.match(r"bytes=(\d*)-(\d*)", header)
            if match:
                first, last = match.group(1), match.group(2)
                if first == "":
                    start, end = max(0, size - int(last)), size - 1
                else:
                    start = int(first)
                    end = int(last) if last else size - 1
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    self.record(path=self.path, range=header, status=416)
                    return
                end = min(end, size - 1)
                status = 206
        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", MIME.get(os.path.splitext(path)[1].lower(), "application/octet-stream"))
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        self.record(path=self.path, range=header, status=status, start=start, length=length)
        sent = 0
        try:
            with open(path, "rb") as source:
                source.seek(start)
                while sent < length:
                    chunk = source.read(min(65536, length - sent))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.record(path=self.path, done=sent, of=length)


def main():
    root, port, log_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.root = os.path.abspath(root)
    server.log_path = log_path
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
