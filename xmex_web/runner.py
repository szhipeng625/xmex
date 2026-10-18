import http.server, json, os, subprocess, tempfile, time

BUNDLE = "/root/xmex_bundle"
XMEXC = os.path.join(BUNDLE, "xmexc")
LIBDIR = os.path.join(BUNDLE, "lib")
INCDIR = os.path.join(BUNDLE, "include")
ENV = dict(os.environ, LD_LIBRARY_PATH=LIBDIR)
MAX_SRC = 20000
COMPILE_TIMEOUT = 30
RUN_TIMEOUT = 10

def handle_run(code):
    if len(code) > MAX_SRC:
        return {"ok": False, "stage": "input", "output": "代码超过长度限制"}
    work = tempfile.mkdtemp(prefix="xmex_web_")
    src = os.path.join(work, "webrun.pys")
    obj = os.path.join(work, "webrun.o")
    drv = os.path.join(work, "drv.cpp")
    exe = os.path.join(work, "webrun")
    open(src, "w").write(code)
    open(drv, "w").write("#include <cstdio>\nextern \"C\" void xmex_webrun_main();\nint main(){ xmex_webrun_main(); return 0; }\n")
    t0 = time.time()
    try:
        p = subprocess.run([XMEXC, src, "-o", obj, "-I", "webrun"], capture_output=True, text=True, timeout=COMPILE_TIMEOUT, env=ENV)
    except subprocess.TimeoutExpired:
        return {"ok": False, "stage": "compile", "output": "编译超时"}
    compile_out = (p.stdout + p.stderr).strip()
    if p.returncode != 0 or not os.path.exists(obj):
        return {"ok": False, "stage": "compile", "output": compile_out or "编译失败"}
    try:
        p2 = subprocess.run(["g++", "-std=c++20", "-I" + INCDIR, drv, obj, "-o", exe, "-L" + LIBDIR, "-lxmex_runtime", LIBDIR + "/libstdc++.so.6", "-Wl,-rpath," + LIBDIR], capture_output=True, text=True, timeout=COMPILE_TIMEOUT, env=ENV)
    except subprocess.TimeoutExpired:
        return {"ok": False, "stage": "link", "output": "链接超时"}
    if p2.returncode != 0:
        return {"ok": False, "stage": "link", "output": (p2.stderr or "链接失败").strip()}
    try:
        p3 = subprocess.run([exe], capture_output=True, text=True, timeout=RUN_TIMEOUT, env=ENV)
        run_out = p3.stdout + p3.stderr
        rc = p3.returncode
    except subprocess.TimeoutExpired:
        return {"ok": False, "stage": "run", "output": "运行超时（超过 10 秒）"}
    elapsed = round((time.time() - t0) * 1000)
    return {"ok": rc == 0, "stage": "run", "output": run_out, "elapsed_ms": elapsed, "exit_code": rc}

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def _send(self, code, body):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(data)
    def do_OPTIONS(self):
        self._send(200, "{}")
    def do_GET(self):
        if self.path == "/health":
            self._send(200, json.dumps({"status": "ok"}))
        else:
            self._send(404, json.dumps({"error": "not found"}))
    def do_POST(self):
        if self.path != "/run":
            self._send(404, json.dumps({"error": "not found"}))
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"{}")
            result = handle_run(body.get("code", ""))
            self._send(200, json.dumps(result, ensure_ascii=False))
        except Exception as e:
            self._send(200, json.dumps({"ok": False, "stage": "server", "output": "服务端错误: " + str(e)}, ensure_ascii=False))

if __name__ == "__main__":
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 18090), H)
    print("xmex web runner listening on 127.0.0.1:18090", flush=True)
    srv.serve_forever()
