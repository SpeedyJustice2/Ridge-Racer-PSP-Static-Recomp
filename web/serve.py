#!/usr/bin/env python3
"""Local web server for the browser build (serves the folder it is started in, or the folder given as the first argument).
The game needs "cross-origin isolation" (two headers) which a plain file:// open or `python -m http.server` does not send."""
import http.server, os, sys

class H(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.wasm': 'application/wasm', '.js': 'text/javascript'}
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()
    def log_message(self, *a): pass

if len(sys.argv) > 1: os.chdir(sys.argv[1])
port = int(os.environ.get('PORT', '8080'))
print(f'Ridge Racer (browser build): open http://localhost:{port}/   (Ctrl+C to stop)')
http.server.ThreadingHTTPServer(('127.0.0.1', port), H).serve_forever()
