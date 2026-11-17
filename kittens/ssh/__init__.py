import math


def seconds_or_auto(x: str) -> float:
    ''' Parse a number of seconds or auto (returned as -1), keep in sync with seconds_or_auto() in liveness.go '''
    x = x.strip()
    if x.lower() == 'auto':
        return -1.
    ans = float(x)
    if math.isnan(ans) or ans < 0:
        raise ValueError(f'{x!r} is not auto or a non-negative number of seconds')
    if math.isinf(ans):
        ans = 0.  # never time out
    return ans
