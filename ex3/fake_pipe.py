import os

fds = []
N = 20000

for i in range(N):
    try:
        r, w = os.pipe()
        fds.append((r, w))
    except OSError as e:
        print("stopped at", i, e)
        break

print("created pipes:", len(fds))
input("Press Enter to close pipes...")

for r, w in fds:
    os.close(r)
    os.close(w)