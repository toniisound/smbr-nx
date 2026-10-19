"""Extract selected members from a remote zip using HTTP Range requests."""
import io, sys, zipfile, urllib.request

class HttpFile(io.RawIOBase):
    def __init__(self, url):
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req) as r:
            self.url = r.geturl()
            self.size = int(r.headers["Content-Length"])
        self.pos = 0
    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos
    def seek(self, off, whence=0):
        self.pos = [off, self.pos + off, self.size + off][whence]
        return self.pos
    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0: return 0
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{self.pos+n-1}"})
        with urllib.request.urlopen(req) as r:
            data = r.read()
        b[:len(data)] = data
        self.pos += len(data)
        return len(data)

url, outdir, *patterns = sys.argv[1:]
f = io.BufferedReader(HttpFile(url), buffer_size=1 << 20)
with zipfile.ZipFile(f) as z:
    for i in z.infolist():
        if any(p in i.filename for p in patterns):
            print("extract", i.filename, i.file_size, flush=True)
            z.extract(i, outdir)
