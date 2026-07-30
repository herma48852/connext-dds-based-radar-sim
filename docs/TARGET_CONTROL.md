# Target Generator Control

`target_control` is a separate operator UI for changing target scenarios while
`radar_app` and `target_gen` continue running. It does not join the radar
simulation domain.

## Domain separation

For the standard domain-92 demo:

```text
Control domain 93                 Simulation domain 92

target_control  <---------->  target_gen  ---------->  TargetGen/TargetTruth
               management DDS             radar DDS
```

There is no DDS router between the domains. `target_gen` owns one participant
on each domain and transfers accepted control operations to its in-process
50 Hz scenario core.

Start the applications with:

```bash
radar_app --domain 92
target_gen --domain 92 --control-domain 93 --targets 32
target_control --domain 93
```

When omitted, `target_gen --control-domain` defaults to its simulation domain
plus one, wrapping from 232 to 0. The two domains may not be equal.
`target_control` defaults to domain 93.

The principal radar DDS architecture diagram intentionally remains scoped to
the simulation data plane and does not include this optional management plane.

## Scenario lifecycle

Choosing **ADD** is always additive. It creates a new scenario instance with a
unique instance ID; it never replaces existing targets. Repeated selections
create independent instances:

- Repeated 12 km orbits use deterministic golden-angle phase offsets.
- Repeated random fleets continue the deterministic random stream.
- Repeated presentation fleets continue the golden-angle formation so
  contacts from separate instances do not overlap.
- Repeated minimum-range transits rotate to different approach bearings.
- Repeated face-seam handoffs advance to another physical face boundary.
- Repeated crossing pairs rotate their complete geometry by the golden angle.
- Repeated boundary crossing pairs advance to the next physical face seam.

The active-scenario table can remove a complete scenario instance. The target
inventory can remove one target from a group. **CLEAR ALL** confirms before
disposing every current `TargetTruth` instance and leaves `target_gen` running
with an empty fleet. Target IDs remain monotonic and are not reused.

The `--targets N` startup behavior remains compatible with earlier releases:
it creates one `12 km Orbit` instance and, for `N > 1`, one `Random Fleet`
instance containing `N-1` targets. Those startup groups appear in the UI and
can be removed like groups added later.

## Scenario catalog

### 12 km Orbit

Adds one persistent fighter on the existing 12 km slant-range, 14-degree
elevation orbit. A second instance uses a different starting phase rather than
overlapping the first.

### Random Fleet

Adds the selected number of randomized inbound targets using the existing
fighter, bomber, missile, ship, drone, and decoy profiles. The UI default is
31 targets, matching the random portion of `--targets 32`. Random targets
continue to honor `target_gen --respawn-range`.

### Presentation Fleet

Adds a configurable persistent formation intended for screen-shared
demonstrations. The default six-target fleet contains one fighter, bomber,
missile, ship, drone, and decoy. It does not alter receiver sensitivity,
CFAR, or the tracker's three-scan confirmation rule. Instead, each target
orbits at a range comfortably inside its modeled RCS sensitivity:

| Type | Slant range | Elevation | RCS |
|---|---:|---:|---:|
| Fighter | 18 km | 14 deg | 0 dBsm |
| Bomber | 50 km | 14 deg | 20 dBsm |
| Missile | 12 km | 25 deg | -10 dBsm |
| Ship | 45 km | Surface | 35 dBsm |
| Drone | 9 km | 14 deg | -15 dBsm |
| Decoy | 30 km | 14 deg | 5 dBsm |

The air contacts are centered on modeled search elevation bars. The surface
ship remains at zero altitude inside the lowest bar's acceptance gate.
Golden-angle starting phases keep contacts separated, and repeated fleets
continue that phase sequence. Counts greater than six repeat the same mixed
profile at additional separated phases.

### Minimum-Range Transit

Adds one deterministic low-altitude flyby:

1. It appears inbound at 8 km slant range.
2. At 250 m/s it follows an offset straight-line chord.
3. It spends 15 seconds continuously inside 2 km slant range.
4. With the default receiver, it remains inside the radar's approximately
   3 km blind range for about 23.3 seconds, exceeding the tracker's 12-second
   coast.
5. With `radar_app --sub-3km`, its truncated pulse tails remain detectable but
   are reported at an outward-biased ambiguous range; without that option, it
   re-emerges outbound and can be acquired as a new track.
6. At 20 km outbound, `target_gen` disposes its keyed truth instance and marks
   the one-shot scenario complete.

### Face-Seam Handoff

Adds one fighter at 18 km slant range and 14 degrees elevation. It follows a
24-degree constant-range arc centered on a physical face boundary, crossing
from the Forward Starboard face into the Aft Starboard face on the first
launch. The transit lasts about 29 seconds before the truth instance is
disposed. Repeated launches cycle through the other face boundaries; after
all four boundaries have active instances, additional launches use 250 m
range offsets so targets do not overlap.

### Crossing Pair

Adds two fighters on symmetric converging paths centered near 18 km. Their
opposing tangential motion creates an obvious PPI crossing while their shared
inward component carries them toward the ship. At closest approach they retain
a 500 m horizontal miss and 300 m altitude separation. Both continue on
diverging paths and are disposed together after an 80-second transit.

### Boundary Crossing Pair

Adds the same two-fighter crossing geometry, but constrains the closest
approach to a physical face boundary. The first instance is centered on the
Forward Port/Forward Starboard seam at zero degrees ship-relative: the
fighters begin on opposite faces, pass with the same 500 m horizontal and
300 m vertical separation directly on the seam, and leave on exchanged faces.
Repeated launches cycle through the 90-, 180-, and 270-degree seams; later
instances use 250 m range offsets to prevent overlap.

## Control-domain DDS topics

| Topic | Direction | QoS | Purpose |
|---|---|---|---|
| `TargetControl/Request` | UI to generator | Reliable, volatile, depth 32 | Add/remove/clear commands |
| `TargetControl/Reply` | Generator to UI | Reliable, volatile, depth 32 | Correlated accepted/rejected result |
| `TargetControl/Snapshot` | Generator to UI | Reliable, transient-local, depth 1 | Catalog and authoritative live inventory |

The snapshot refreshes at 5 Hz and immediately after mutations. Its catalog
contains each scenario's name, label, description, and supported target-count
range, allowing future templates to appear without redesigning the UI.

## Automated control-plane check

With a dedicated test instance of `target_gen` running, the UI executable can
exercise the protocol without opening a window:

```bash
target_control --domain 93 --smoke-test
```

The smoke mode clears the test fleet, exercises all six catalog templates,
checks additive inventory and repeated-instance separation, then clears the
fleet again.
