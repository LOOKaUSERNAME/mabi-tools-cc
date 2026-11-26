# Measurements and recommendations

**Connection time** measures how long it takes to establish TCP.
**Reply wait** measures the additional time to read the channel's
four-byte opening message. **Total time** is their sum. These timings
describe the connection doing the scan, not in-game latency, packet
loss or combat performance.

One warm-up check per channel is excluded. A check uses a single
monotonic deadline for connecting and reading the reply. Failed
checks are counted, not averaged as latencies. Jitter is the mean
absolute change in total time between consecutive successful checks.
A failed round breaks the pair rather than joining the points on
either side.

Recommendations require at least two channels with matching rounds
that qualify overall and in both scan halves. Fastest connection
minimises connection time. Most consistent minimises jitter. Suggested
channel favours successful checks, then the lowest mean total time
plus twice the jitter. Rounded display values do not establish an
exact tie. The report keeps the legacy single-choice field readable,
and records the displayed suggestion separately when exact leaders
tie. Near-ties use the separate similarity rule.

Completed history remains local. The recommendations' recent-history
window and the limits that delete saved files are separate rules.
