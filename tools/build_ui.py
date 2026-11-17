# PlatformIO pre-script: gzip ui/* into <build>/fsdata/www/*.gz and point the
# filesystem image at it, so `pio run -t uploadfs` ships a compressed UI.
# ponytail: assumes the framework repo layout; app projects get a wrapper in P9.
Import("env")
import gzip, os, shutil

project = env.subst("$PROJECT_DIR")
src = os.path.join(project, "ui")
out = os.path.join(project, ".pio", "fsdata", env.subst("$PIOENV"))
www = os.path.join(out, "www")

if os.path.isdir(src):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(www)
    for name in sorted(os.listdir(src)):
        with open(os.path.join(src, name), "rb") as f:
            raw = f.read()
        # mtime=0 keeps the image reproducible
        with open(os.path.join(www, name + ".gz"), "wb") as f:
            f.write(gzip.compress(raw, 9, mtime=0))
    env.Replace(PROJECT_DATA_DIR=out)
