#!/usr/bin/env python3
"""pod - the agent's hands on Podslicer.

One command per question a session actually asks, so nothing below needs to be
re-derived from the shell by the next session:

    pod status                       what state is the build, staging, datadir, last crash in
    pod inspect  <3mf>               what does this project DECLARE (plates, contexts, key values)
    pod run      [flags]             drive the real app through the perf driver and report honestly
    pod slice    <3mf> --printer P   open, retarget, slice, keep the G-code (the proven write path)
    pod check    <gcode> [flags]     what did a slice ACTUALLY use (petkos-gcode-check)
    pod verify                       the in-app suite   (tools/petkos-verify.ps1)
    pod import-check                 the five-number cross-printer suite
    pod build    [--full]            dev build, or the detached full rebuild
    pod stage                        copy fresh binaries over the launcher's staging pair
    pod logs     [--crash|--errors|--grep X|--tail N]
    pod conf     [--repair]          validate the app config's JSON + MD5 trailer; repair with backup
    pod audit-vendors                self-incompatible default_print_profile declarations

House rules baked in rather than remembered: the staging copy goes stale silently
(pod status says so); PowerShell -Filter has no character classes; debug_network_*
logs are not app logs; a killed run writes no CSV; the conf's MD5 trailer tears on
a hard kill; PETKOS_ACCEPT / PETKOS_TEST_ASSIGN left set will hijack a run.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, time, zipfile
from pathlib import Path

REPO    = Path(__file__).resolve().parent.parent
DATADIR = REPO / "datadir"
LOGDIR  = DATADIR / "log"
EXE     = REPO / "build" / "src" / "Release" / "orca-slicer.exe"
DLL     = REPO / "build" / "src" / "Release" / "OrcaSlicer.dll"
STAGE   = REPO / "build" / "OrcaSlicer"
CONF    = DATADIR / "Podslicer.conf"
PERFDIR = REPO / "perf-runs"
TOOLS   = REPO / "tools"

def newest(pattern, exclude=None, logdir=None):
    files = [p for p in (logdir or LOGDIR).glob(pattern) if not (exclude and exclude in p.name)]
    return max(files, key=lambda p: p.stat().st_mtime) if files else None

def app_running():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return "orca-slicer" in out, "msbuild" in out

def running_slicer_datadirs():
    """(pid, datadir-from-command-line) for every running slicer. Empty datadir = unknown."""
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='orca-slicer.exe'\" | "
          "ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }")
    out = subprocess.run(["pwsh", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    hits = []
    for line in out.splitlines():
        pid, _, cmd = line.partition("\t")
        m = re.search(r'--datadir"?\s+"?([^"]+?)"?(?:\s+-|\s*$)', cmd)
        hits.append((pid.strip(), m.group(1).strip() if m else ""))
    return hits

def ensure_clone(fresh=False):
    """Create (or reuse) the datadir-verify clone and return its path.

    The ONE clone implementation - the ps1 harnesses call `pod clone` rather than keeping
    their own copies of these rules. Copies everything except plugins/ (143 MB) and log/,
    then makes the conf STOP CLAIMING networking is installed: a conf that says
    installed_networking=true beside an empty plugins dir walks on_init_network into
    m_networking_need_update, and post_init answers that with a MODAL download dialog -
    which is how an unattended suite run hangs for its whole timeout with a healthy event
    loop and no driver. Falsifying the claim honestly (this datadir really has no plugin)
    makes the app treat it as a clean no-networking install and boot straight through.
    The MD5 trailer is recomputed because the app treats a mismatch as a torn write.
    """
    clone = REPO / "datadir-verify"
    if fresh and clone.exists():
        shutil.rmtree(clone)
    if not clone.exists():
        clone.mkdir(parents=True)
        for entry in DATADIR.iterdir():
            if entry.name in ("plugins", "log"):
                continue
            if entry.is_dir():
                shutil.copytree(entry, clone / entry.name)
            else:
                shutil.copy2(entry, clone / entry.name)
        conf_path = clone / CONF.name
        if conf_path.exists():
            raw = conf_path.read_bytes()
            m = re.search(rb"# MD5 checksum \w+", raw)
            body = raw[: m.start()] if m else raw
            cfg = json.loads(body.decode("utf-8"))
            cfg.setdefault("app", {})["installed_networking"] = False
            body = json.dumps(cfg, indent=4, ensure_ascii=False).encode("utf-8") + b"\n"
            conf_path.write_bytes(body + b"# MD5 checksum " + hashlib.md5(body).hexdigest().upper().encode() + b"\n")
    (clone / "log").mkdir(exist_ok=True)
    return clone

def run_datadir():
    """The datadir a driven run should use: the live one, or the clone when a slicer is open.

    The live one is preferred because it is the config that actually resolves - real presets,
    a parsed conf. The clone is used only when a slicer IS running, because the app holds one
    datadir per instance and a check must never require closing a session that may hold an
    unsaved project.
    """
    running, _ = app_running()
    if not running:
        return DATADIR, LOGDIR, False
    clone = ensure_clone()
    return clone, clone / "log", True

#Modal dialogs from crash handlers, CRT leak reports and the hard-error host do not belong to
#the app's window tree, block everything, and wait for a human. A run that hangs on one looks
#exactly like a run still working. Petko: "you might not catch it until I click OK on it."
DANGER_TITLES = r"Application Error|Memory Leak|Visual Leak Detector|Microsoft Visual C\+\+|orca-slicer\.exe"
_ENUM_PS = r'''
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class W {
  [DllImport("user32.dll")] public static extern bool EnumWindows(Func<IntPtr,IntPtr,bool> cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$hits = @()
[W]::EnumWindows({ param($h, $p)
  if ([W]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 256; [void][W]::GetWindowText($h, $sb, 256)
    $t = $sb.ToString()
    if ($t -match 'PATTERN') { $script:hits += "$([int64]$h)|$t" }
  }; $true }, [IntPtr]::Zero) | Out-Null
$hits
CLOSE
'''

def sweep_dialogs(close=False):
    ps = _ENUM_PS.replace("PATTERN", DANGER_TITLES).replace(
        "CLOSE", "if ($env:POD_CLOSE -eq '1') { $hits | ForEach-Object { "
                 "[void][W]::PostMessage([IntPtr][int64]($_ -split '\\|')[0], 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } }")
    env = dict(os.environ, POD_CLOSE="1" if close else "0")
    out = subprocess.run(["pwsh", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True, env=env).stdout.strip()
    hits = [l for l in out.splitlines() if "|" in l]
    for h in hits:
        print(f"   MODAL DIALOG: {h.split('|', 1)[1]}" + ("  -> closed" if close else ""))
    return hits

# ---------------------------------------------------------------- status
def cmd_status(a):
    running, building = app_running()
    print(f"binary   : {EXE}" + ("  [MISSING]" if not EXE.exists() else ""))
    if DLL.exists():
        built = DLL.stat().st_mtime
        print(f"built    : {time.strftime('%d %b %H:%M:%S', time.localtime(built))}")
        sdll = STAGE / "OrcaSlicer.dll"
        if sdll.exists():
            drift = built - sdll.stat().st_mtime
            print(f"staging  : {'CURRENT' if abs(drift) < 2 else f'STALE by {int(drift)}s - run: pod stage'}")
    if running:
        for pid, dd in running_slicer_datadirs():
            whose = "holds the LIVE datadir" if dd and Path(dd) == DATADIR else \
                    (f"on {dd}" if dd else "datadir unknown")
            print(f"app      : RUNNING (pid {pid}, {whose}); driven runs will use a datadir clone")
    else:
        print("app      : not running")
    print(f"build    : {'MSBUILD ACTIVE - do not edit sources' if building else 'idle'}")
    ok, why = conf_valid()
    print(f"conf     : {'valid' if ok else 'BROKEN - ' + why + '  (pod conf --repair)'}")
    for env in ("PETKOS_PERF", "PETKOS_PERF_SCRIPT", "PETKOS_ACCEPT", "PETKOS_TEST_ASSIGN"):
        if os.environ.get(env):
            print(f"env      : {env} is SET and will affect the next launch")
    crash = newest("crash_*.log")
    if crash:
        age = (time.time() - crash.stat().st_mtime) / 60
        print(f"crash    : {crash.name}  ({age:.0f} min ago{'  <-- FRESH' if age < 30 else ''})")

# ---------------------------------------------------------------- inspect
KEYS = ["printer_settings_id", "print_settings_id", "filament_settings_id", "printer_model",
        "filament_type", "filament_max_volumetric_speed", "wall_loops",
        "sparse_infill_density", "sparse_infill_pattern", "layer_height"]

def cmd_inspect(a):
    z = zipfile.ZipFile(a.project)
    names = z.namelist()
    cfg = {}
    if "Metadata/project_settings.config" in names:
        cfg = json.loads(z.read("Metadata/project_settings.config"))
    print(f"== {Path(a.project).name}")
    for k in KEYS:
        if k in cfg:
            v = cfg[k]
            print(f"   {k:<34} {', '.join(v) if isinstance(v, list) else v}")
    ms = z.read("Metadata/model_settings.config").decode("utf-8", "replace") if \
         "Metadata/model_settings.config" in names else ""
    plates = re.findall(r'key="plater_printer_preset"\s+value="([^"]*)"', ms)
    print(f"   plates: {ms.count('<plate>')}, with per-plate printer: {len(plates)}")
    for i, p in enumerate(plates):
        print(f"     plate {i + 1}: {p}")
    painted = sum(1 for n in names if n.endswith(".model")
                  for chunk in [z.read(n)[:4_000_000]] if b"paint_color" in chunk)
    print(f"   paint_color present in {painted} model part(s)")
    if a.json:
        print(json.dumps({k: cfg.get(k) for k in KEYS}))

# ---------------------------------------------------------------- run / slice
def drive(spec, project=None, timeout=900, keep_env=False):
    """Launch the app under the perf driver, wait, and report from its own log."""
    datadir, logdir, cloned = run_datadir()
    if cloned:
        print("== a slicer is open, so this runs on its own datadir copy")
    #hygiene by default: these hijack a run when left set. keep_env is for the run that MEANS
    #them - the fixture builder sets PETKOS_TEST_ASSIGN deliberately.
    if not keep_env:
        for env in ("PETKOS_ACCEPT", "PETKOS_TEST_ASSIGN"):
            os.environ.pop(env, None)
    before = {p.name for p in logdir.glob("debug_*.log*") if "network" not in p.name}
    env = dict(os.environ, PETKOS_PERF="1", PETKOS_PERF_OUT=str(PERFDIR / "pod"),
               PETKOS_PERF_SCRIPT=spec)
    args = [str(EXE), "--datadir", str(datadir)] + ([str(project)] if project else [])
    print(f"== running: {spec}")
    t0 = time.time()
    proc = subprocess.Popen(args, env=env)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        #Before killing, look for the reason a run hangs invisibly: a modal dialog waiting
        #for a click that never comes. Found ones are closed so the run can finish dying.
        if sweep_dialogs(close=True):
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
        if proc.poll() is None:
            proc.kill()
            print(f"KILLED after {timeout}s - the driver never quit; that is a result, not a detail")
    print(f"   app exited after {int(time.time() - t0)}s (rc={proc.returncode})")
    new = [p for p in logdir.glob("debug_*.log*")
           if "network" not in p.name and p.name not in before]
    verdicts = []
    for p in new:
        text = p.read_text(encoding="utf-8", errors="replace")
        verdicts += re.findall(
            r"PETKOS_PERF_SCRIPT: [^\n]*(?:CHECK|ASSIGN|SLICE|passed|FAILED)[^\n]*", text)
    for v in verdicts:
        print("   " + v.strip())
    crash = newest("crash_*.log", logdir=logdir)
    if crash and crash.stat().st_mtime > t0:
        print(f"CRASHED - {crash.name}; top frames:")
        for line in crash.read_text(errors="replace").splitlines():
            if "Slic3r::" in line:
                print("   " + line.strip()[:170])
                break
    failed = any("FAILED" in v for v in verdicts) or (crash and crash.stat().st_mtime > t0)
    return 1 if failed else 0

def cmd_run(a):
    sys.exit(drive(a.spec, a.project, a.timeout, keep_env=a.keep_env))

def cmd_slice(a):
    gcode = Path(a.out or (PERFDIR / (Path(a.project).stem + ".gcode"))).resolve()
    spec = (f"plates=0,warmup=25,orbit=0,switch=0,assign=1,printer={a.printer},"
            f"board=0,drag=0,pick=0,scope=0,context=1,gcode={gcode},quit=1")
    rc = drive(spec, a.project, a.timeout)
    if gcode.exists():
        print(f"   kept {gcode}")
        subprocess.run([sys.executable, str(TOOLS / "petkos-gcode-check.py"), str(gcode),
                        "--expect-printer", a.printer])
    else:
        print("FAIL: no G-code emitted"); rc = 1
    sys.exit(rc)

# ---------------------------------------------------------------- wrappers
def pwsh(script, *args):
    return subprocess.run(["pwsh", "-Command",
                           f"& '{TOOLS / script}' " + " ".join(args)]).returncode

def cmd_verify(a):       sys.exit(pwsh("petkos-verify.ps1"))
def cmd_import_check(a): sys.exit(pwsh("petkos-import-check.ps1", "-TimeoutSec 1500"))
def cmd_check(a):
    extra = sum([["--expect-printer", a.expect_printer] if a.expect_printer else [],
                 ["--expect-filament", a.expect_filament] if a.expect_filament else [],
                 ["--min-flow", str(a.min_flow)] if a.min_flow else [],
                 ["--json"] if a.json else []], [])
    sys.exit(subprocess.run([sys.executable, str(TOOLS / "petkos-gcode-check.py"),
                             a.gcode] + extra).returncode)
def cmd_audit_vendors(a):
    sys.exit(subprocess.run([sys.executable, str(TOOLS / "petkos-vendor-audit.py")]).returncode)

def cmd_build(a):
    if a.full:
        sys.exit(pwsh("petkos-build-detached.ps1"))
    rc = pwsh("petkos-dev-build.ps1")
    if rc == 0 and not a.no_stage:
        cmd_stage(a)
    sys.exit(rc)

def cmd_stage(a):
    for name in ("OrcaSlicer.dll", "orca-slicer.exe"):
        shutil.copy2(REPO / "build" / "src" / "Release" / name, STAGE / name)
    print("staging refreshed")

# ---------------------------------------------------------------- logs / conf
def cmd_logs(a):
    if a.crash:
        src = newest("crash_*.log")
    else:
        src = newest("debug_*.log.0", exclude="network")
    if not src:
        sys.exit("no log found")
    print(f"== {src.name}")
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    if a.grep:
        lines = [l for l in lines if re.search(a.grep, l)]
    elif a.errors:
        lines = [l for l in lines if "[error]" in l or "[fatal]" in l]
    for l in lines[-a.tail:]:
        print(l[:220])

def conf_valid():
    if not CONF.exists():
        return False, "missing"
    raw = CONF.read_bytes()
    m = re.search(rb"# MD5 checksum (\w+)", raw)
    if not m:
        return False, "no MD5 trailer"
    body = raw[: m.start()]
    if hashlib.md5(body).hexdigest().upper() != m.group(1).decode():
        return False, "MD5 mismatch (torn write)"
    try:
        json.loads(body.decode("utf-8"))
    except Exception as e:
        return False, f"invalid JSON: {e}"
    return True, ""

def cmd_conf(a):
    ok, why = conf_valid()
    if ok:
        print("conf valid"); return
    print(f"conf BROKEN: {why}")
    if not a.repair:
        sys.exit(1)
    backup = Path("E:/Backups") / f"Podslicer.conf.pre-repair-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(CONF, backup)
    raw = CONF.read_bytes()
    m = re.search(rb"# MD5 checksum \w+", raw)
    body = raw[: m.start()] if m else raw
    json.loads(body.decode("utf-8"))  # refuse to bless invalid JSON with a fresh checksum
    fixed = body + b"# MD5 checksum " + hashlib.md5(body).hexdigest().upper().encode() + b"\n"
    CONF.write_bytes(fixed)
    (DATADIR / "Podslicer.conf.bak").write_bytes(fixed)
    print(f"repaired; original kept at {backup}")

# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(prog="pod", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(f=cmd_status)
    p = sub.add_parser("inspect"); p.add_argument("project"); p.add_argument("--json", action="store_true"); p.set_defaults(f=cmd_inspect)
    p = sub.add_parser("run"); p.add_argument("spec"); p.add_argument("--project"); p.add_argument("--timeout", type=int, default=900); p.add_argument("--keep-env", action="store_true", help="honour PETKOS_ACCEPT/PETKOS_TEST_ASSIGN already in the environment"); p.set_defaults(f=cmd_run)
    p = sub.add_parser("slice"); p.add_argument("project"); p.add_argument("--printer", required=True); p.add_argument("--out"); p.add_argument("--timeout", type=int, default=1500); p.set_defaults(f=cmd_slice)
    p = sub.add_parser("check"); p.add_argument("gcode"); p.add_argument("--expect-printer"); p.add_argument("--expect-filament"); p.add_argument("--min-flow", type=float); p.add_argument("--json", action="store_true"); p.set_defaults(f=cmd_check)
    sub.add_parser("verify").set_defaults(f=cmd_verify)
    sub.add_parser("import-check").set_defaults(f=cmd_import_check)
    p = sub.add_parser("build"); p.add_argument("--full", action="store_true"); p.add_argument("--no-stage", action="store_true"); p.set_defaults(f=cmd_build)
    sub.add_parser("stage").set_defaults(f=cmd_stage)
    p = sub.add_parser("logs"); p.add_argument("--crash", action="store_true"); p.add_argument("--errors", action="store_true"); p.add_argument("--grep"); p.add_argument("--tail", type=int, default=25); p.set_defaults(f=cmd_logs)
    p = sub.add_parser("conf"); p.add_argument("--repair", action="store_true"); p.set_defaults(f=cmd_conf)
    sub.add_parser("audit-vendors").set_defaults(f=cmd_audit_vendors)
    p = sub.add_parser("clone"); p.add_argument("--fresh", action="store_true"); p.set_defaults(f=lambda a: print(ensure_clone(a.fresh)))
    p = sub.add_parser("dialogs"); p.add_argument("--close", action="store_true"); p.set_defaults(f=lambda a: sweep_dialogs(a.close) or print("no modal dialogs found"))
    a = ap.parse_args()
    a.f(a)

if __name__ == "__main__":
    main()
