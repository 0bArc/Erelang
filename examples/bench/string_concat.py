import time

N = 20_000
t0 = time.perf_counter()
s = ""
for i in range(N):
    s = s + "x"
t1 = time.perf_counter()
print("INNER_MS=" + str(int(round((t1 - t0) * 1000))))
print("LEN=" + str(len(s)))
