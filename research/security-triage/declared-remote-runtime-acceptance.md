# Declared remote runtime acceptance criteria

- Local exact-artifact runtime discovery remains unchanged and independently profile-bound.
- Remote discovery connects only to exact Registry-declared HTTP(S) URLs and prevalidated global IPs.
- No redirects, authentication material, path guessing, port scanning or tool invocation.
- Complete tool inventories are persisted only after bounded pagination terminates.
- Remote scheduler failures cannot suppress publication of successful local observations.
- Interrupted remote runs recover from stale `running` state.
- Local + remote + publication share a bounded service deadline.
