# Run under an injected tsmalloc: exercises a real interpreter's allocation
# patterns across threads and checks the library really is loaded.
import ctypes
import json
import threading

ts = ctypes.CDLL(None)
ts.ts_is_ours.argtypes = [ctypes.c_void_p]
ts.ts_is_ours.restype = ctypes.c_int
ts.malloc.argtypes = [ctypes.c_size_t]
ts.malloc.restype = ctypes.c_void_p
ts.free.argtypes = [ctypes.c_void_p]

probe = ts.malloc(128)
assert ts.ts_is_ours(probe), "malloc did not come from tsmalloc"
ts.free(probe)

results = []


def work(seed):
    data = [{"key": str(i * seed), "values": list(range(i % 40))} for i in range(8000)]
    text = json.dumps(data)
    results.append(json.loads(text) == data and len(bytearray(text, "utf8")) > 0)


threads = [threading.Thread(target=work, args=(n + 1,)) for n in range(6)]
for t in threads:
    t.start()
for t in threads:
    t.join()

assert len(results) == 6 and all(results)
print("override_python ok")
