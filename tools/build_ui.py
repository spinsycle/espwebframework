# PlatformIO pre-script: gzip ui/* into <build>/fsdata/www/*.gz and point the
# filesystem image at it, so `pio run -t uploadfs` ships a compressed UI.
# Works in the framework repo (ui/ next to platformio.ini) and in an app repo: with no ui/ of its own it
# uses the one inside the installed framework library, .pio/libdeps/<env>/espwebframework/ui.
Import("env")
import gzip, os, shutil

project = env.subst("$PROJECT_DIR")
src = os.path.join(project, "ui")
if not os.path.isdir(src):
    libs = os.path.join(project, ".pio", "libdeps", env.subst("$PIOENV"))
    link = os.path.join(libs, "espwebframework.pio-link")  # a symlink:// install leaves this note instead of a folder
    src = os.path.join(libs, "espwebframework", "ui")
    if os.path.isfile(link):
        import json
        spec = json.load(open(link))
        src = os.path.join(os.path.normpath(os.path.join(spec["cwd"], spec["spec"]["uri"][len("symlink://"):])), "ui")
    if not os.path.isdir(src):
        print("build_ui: no ui/ here and the framework library is not installed yet; run `pio pkg install` and build again")
out = os.path.join(project, ".pio", "fsdata", env.subst("$PIOENV"))
www = os.path.join(out, "www")

if os.path.isdir(src):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(www)
    # The ESP8266 has only a handful of TCP connections: a browser asking for html, css and five scripts at once
    # leaves some unanswered. So the scripts named in index.html ship as ONE bundle.js and the html points at it.
    import re
    with open(os.path.join(src, "index.html"), encoding="utf-8") as f:
        html = f.read()
    scripts = re.findall(r'<script src="([^"]+\.js)"></script>', html)
    bundle = "\n;\n".join(open(os.path.join(src, n), encoding="utf-8").read() for n in scripts).encode("utf-8")
    page = re.sub(r'(<script src="[^"]+\.js"></script>\n?)+', '<script src="bundle.js"></script>\n', html, count=1).encode("utf-8")
    files = {n: open(os.path.join(src, n), "rb").read() for n in sorted(os.listdir(src)) if n not in scripts and n != "index.html"}
    files["index.html"] = page
    files["bundle.js"] = bundle
    for name, raw in sorted(files.items()):
        # mtime=0 keeps the image reproducible
        with open(os.path.join(www, name + ".gz"), "wb") as f:
            f.write(gzip.compress(raw, 9, mtime=0))
    # Optional app seed: <app>/seed/{devices,automation,dashboard}.json ship as /config/*.json so a fresh `uploadfs`
    # boots with the app's devices, rules and dashboard already in place. The firmware still validates them at boot
    # (bad device entries are skipped); here we only refuse unparseable JSON or a wrong schema_version.
    seed = os.path.join(project, "seed")
    if os.path.isdir(seed):
        import json
        cfg = os.path.join(out, "config")
        os.makedirs(cfg)
        for name in sorted(os.listdir(seed)):
            if name not in ("devices.json", "automation.json", "dashboard.json"):
                raise SystemExit("build_ui: seed/%s is not one of devices.json, automation.json, dashboard.json" % name)
            with open(os.path.join(seed, name), encoding="utf-8") as f:
                try:
                    doc = json.load(f)
                except ValueError as e:
                    raise SystemExit("build_ui: seed/%s is not valid JSON: %s" % (name, e))
            if doc.get("schema_version") != 1:
                raise SystemExit("build_ui: seed/%s needs \"schema_version\": 1" % name)
            shutil.copyfile(os.path.join(seed, name), os.path.join(cfg, name))
    env.Replace(PROJECT_DATA_DIR=out)
