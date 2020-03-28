#!/usr/bin/env python3

import re
import sys

def main():
    file = open(sys.argv[1], "r")
    lines = file.read().split('\n')

    gmem_match = re.compile(r": rendering (\S+)x(\S+) tiles")
    sysmem_match = re.compile(r": rendering sysmem (\S+)x(\S+)")
    blit_match = re.compile(r": END BLIT")
    elapsed_match = re.compile(r"ELAPSED: (\S+) ns")
    eof_match = re.compile(r"END OF FRAME (\S+)")

    # Times in ns:
    times_blit = []
    times_sysmem = []
    times_gmem = []
    times = None

    for line in lines:
        match = re.search(gmem_match, line)
        if match is not None:
            #print("GMEM")
            if times is not None:
                print("expected times to not be set yet")
            times = times_gmem
            continue

        match = re.search(sysmem_match, line)
        if match is not None:
            #print("SYSMEM")
            if times is not None:
                print("expected times to not be set yet")
            times = times_sysmem
            continue

        match = re.search(blit_match, line)
        if match is not None:
            #print("BLIT")
            if times is not None:
                print("expected times to not be set yet")
            times = times_blit
            continue

        match = re.search(eof_match, line)
        if match is not None:
            frame_nr = int(match.group(1))
            print("FRAME[{}]: {} blits ({:,} ns), {} SYSMEM ({:,} ns), {} GMEM ({:,} ns)".format(
                    frame_nr,
                    len(times_blit), sum(times_blit),
                    len(times_sysmem), sum(times_sysmem),
                    len(times_gmem), sum(times_gmem)
                ))
            times_blit = []
            times_sysmem = []
            times_gmem = []
            times = None
            continue

        match = re.search(elapsed_match, line)
        if match is not None:
            time = int(match.group(1))
            #print("ELAPSED: " + str(time) + " ns")
            times.append(time)
            times = None
            continue


if __name__ == "__main__":
    main()

