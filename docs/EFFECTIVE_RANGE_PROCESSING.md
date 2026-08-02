# Effective-range detection and tracking model

This document defines what “accurate within the effective detection range”
means for AesaRadarSim, describes the implemented algorithms, and records the
assumptions that limit the result. It covers the low-risk first phase of the
range-processing plan and the subsequent near-range extension.
The detector threshold and approximately 3 km transmit eclipse remain
unchanged; the default receiver now processes the observable tail of an echo
that overlaps transmission.

The implementation is internally consistent with the simulation's simplified
waveform and measurement model. It is not a validated performance model for
SPY-6 or any other operational radar.

## Scope

The implemented processing provides:

- pulse-eclipsed return processing below approximately 3 km by default;
- an option to restore the former hard minimum receive range;
- 100 km instrumented range;
- target-dependent sensitivity inside that envelope;
- symmetric range-cell reporting;
- S/N-derived measurement uncertainty and Cartesian track covariance;
- slant-range track association; and
- continuous track identity across elevation-bar transitions.

It does not add adaptive CFAR, Doppler filtering, fractional-delay matched
filtering, continuous angle estimation, clutter, or a probability-of-detection
model. The sub-3 km behavior is an explicit rectangular-pulse approximation,
not a detailed transmitter-leakage or receiver-recovery model.

## RF and timing basis

The representative, unclassified search waveform uses:

| Quantity | Value | Consequence |
|---|---:|---|
| Carrier frequency | 3 GHz | approximately 9.99 cm wavelength |
| Pulse repetition frequency | 1 kHz | approximately 149.90 km unambiguous range |
| Baseband bandwidth | 1 MHz | approximately 149.90 m range resolution |
| Pulse width | 20 microseconds | approximately 2.998 km receive blind range |
| Dwell time | 10 ms | 10 pulses integrated per dwell |
| Instrumented range | 100 km | 668 complex range cells |

A monostatic return travels to the target and back, so its delay is

```text
t_return = 2 * R / c
```

where `R` is slant range and `c` is the speed of light. The factor of two is
already present in every timing-derived range quantity. For example, a 3 km
target has an approximately 20 microsecond round-trip delay, while a 100 km
target has an approximately 667 microsecond delay. Both returns fit inside the
1 millisecond pulse-repetition interval, but the 3 km return overlaps the
20-microsecond transmit interval. The default receiver processes the portion
that remains after transmission ends. The legacy-disable option rejects the
overlap completely.

“Range” in detection and association means slant range:

```text
R = sqrt(east^2 + north^2 + up^2)
```

Horizontal or ground range is `sqrt(east^2 + north^2)`. They are equal only
for a target at zero elevation.

## Sub-3 km receiver and legacy disable option

Truncated-return processing is enabled without an additional argument:

```bash
./build/radar_app.app/Contents/MacOS/radar_app --domain 92
```

To reproduce the former hard 3 km receive gate:

```bash
./scripts/start-all.sh --domain 92 --disable-sub-3km
```

The disable option changes only receiver observations below the normal minimum
receive range.

For a rectangular transmitted pulse of duration `T`, a target at true slant
range `R < R_blind` has round-trip delay `tau < T`. Its echo extends from
`tau` through `tau + T`, but the receiver opens only at `T`. The observable
tail therefore lasts `tau`, giving the coherent voltage fraction

```text
f_capture = tau / T = R / R_blind
```

Correlating that tail with the full rectangular pulse produces an equal-match
delay plateau from the true delay through the receive-open boundary. The
simulation chooses the midpoint as a deterministic representative:

```text
R_apparent = (R_true + R_blind) / 2
```

This result is intentionally biased outward. A target at 1.5 km, for example,
contributes approximately half of its full coherent voltage and appears near
2.25 km before range-cell quantization. At the approximately 2.998 km boundary,
capture fraction and apparent range both converge continuously to their normal
values. A target approaching zero range contributes an ever-shorter tail, and
non-positive ranges are rejected.

The ordinary radar-equation voltage is multiplied by `f_capture`. A half-cell
minimum range in the far-field amplitude calculation and a normalized voltage
clip of `64` bound extremely close returns. The clip represents finite
receiver dynamic range; it prevents an extreme near-field truth point
from destabilizing I/Q or display scaling. It does not affect ordinary
returns at or beyond 3 km.

The default mode can therefore create a detection and track where the hard
gate produced no report, but the reported range is not the target's true
range. Strong ambiguous plots need not associate with the inbound track. On
the production offset flyby, the established track may coast out and a new
track may be initiated by the truncated or outbound returns. That behavior is
intentional: it exposes the inaccurate-detection/track ambiguity rather than
artificially widening gates to preserve identity.

## Return synthesis

For a target inside the receive and instrumented-range gates, the simulated
voltage amplitude is

```text
A = K * f_active * P(azimuth offset) * sqrt(RCS_linear) / R^2
```

where:

- `K` is the demo calibration scaled by wavelength;
- `f_active` is the active aperture fraction;
- `P` is the voltage response of the calculated azimuth array pattern;
- `RCS_linear = 10^(RCS_dBsm / 10)`; and
- `R` is slant range in metres.

### Calibration and simulator voltage units

`A` is an unbounded, voltage-like value in arbitrary simulator units. It is
not a physical receiver voltage, a value normalized to the interval `[0, 1]`,
or a fraction of ADC full scale. The aperture fraction and pattern response
are normalized factors, but their product with the range/RCS model can produce
an `A` greater than one.

At the current 3 GHz carrier, the wavelength-scaled calibration is

```text
K = 3.0e8 * (wavelength / 0.10 m) = 299,792,458
```

The most useful reference point for explaining that scale is a healthy array,
a target at beam center, and a `0 dBsm` (`1 m^2`) RCS:

```text
f_active = 1
P(0) = 1
sqrt(RCS_linear) = 1
A = 299,792,458 / R^2

A = 1 at R = sqrt(299,792,458) = 17,314.5 m
```

This means a `1 m^2` effective radar cross section at approximately 17.3 km
places a unit-magnitude target phasor in the center range cell before noise.
It does not imply that a physical one-square-metre object is a perfect
reflector; RCS depends on shape, material, aspect, and frequency. The adjacent
compressed-pulse cells receive `0.4 A` in the current three-cell response.

For carrier phase `phi`, independent noise samples are added to both complex
components:

```text
I = A * cos(phi) + N_I
Q = A * sin(phi) + N_Q

N_I, N_Q ~ Gaussian(mean = 0, sigma = 0.05)
```

The noise-free magnitude is `sqrt(I^2 + Q^2) = A`; `sqrt(2) * 0.05` is the
RMS magnitude of the two-dimensional noise vector, not a multiplier applied
to `A` or to one noise sample.

This follows the monostatic radar equation's `1/R^4` received-power law:
voltage is proportional to the square root of power and therefore follows
`1/R^2`, while the square root of RCS produces `sqrt(RCS_linear)`. `K` absorbs
the omitted transmitter, nominal-gain, propagation-constant, loss, and
receiver-scale terms. The current `f_active` and `P` terms are effective
amplitude factors applied once; the model does not independently apply a
one-way antenna pattern on transmit and receive. It is consequently a
demo-calibrated square-root radar-equation model, not a rigorous two-way RF
link budget.

### How RMA outages modify I/Q

The receiver does not apply an outage percentage to a completed RawReturn.
Instead, it reduces and reshapes each target phasor before adding that phasor
to the pulse's I/Q cells. With 16 RMAs of 64 elements each,

```text
f_active = active_elements / 1024
         = (16 - offline_RMAs) / 16

A_degraded = K * f_active * P_mask(offset)
             * sqrt(RCS_linear) / R^2

I_pulse = A_degraded * cos(phi) + N_I
Q_pulse = A_degraded * sin(phi) + N_Q
```

The independent `sigma = 0.05` I/Q noise is not scaled with the online-RMA
fraction. After the target contributions and noise are combined, the signal
processor performs the same dwell RMS calculation and compares it with the
same `0.26` threshold.

The exact 16-bit RMA mask matters, not only its population count. Beamformer
turns the physical mask into azimuth column weights and publishes the resulting
active fraction, boresight error, main-beam width, sidelobe locations and
levels, and 181-sample normalized azimuth cut in `Radar/BeamPatternStatus`.
DetectionProcessor consumes that same status and evaluates `P_mask(offset)`
when generating I/Q. Consequently, masks with the same percentage offline can
produce different voltage amplitudes at the same commanded offset when the
failed RMAs occupy different horizontal positions.

The **Beam Formation** panel renders this same `BeamPatternStatus`; its pattern
and outage behavior are therefore a visualization of the response used by the
receiver synthesizer, not a decorative or independent approximation. With all
RMAs offline, RawReturn continues to carry noise for observability, but no
target phasor is added and the aperture-online gate suppresses detections.

The present pattern model is a one-dimensional azimuth cut. It distinguishes
the horizontal/column geometry of offline RMAs, including asymmetric
boresight and sidelobe changes. Vertically separated RMAs in the same column
block produce the same azimuth response because a two-dimensional elevation
array-factor model is not yet implemented; elevation currently remains a
separate acceptance gate.

The model assumes a constant RCS for each target. It does not currently model
aspect-dependent RCS, Swerling fluctuations, propagation loss, atmospheric
attenuation, terrain, sea state, multipath, ducting, jamming, or receiver
saturation.

## Dwell integration and detection

Each 10 ms dwell contains ten 1 kHz pulses. The signal processor performs
noncoherent power integration:

```text
M_bin = sqrt((1 / N) * sum(I_pulse^2 + Q_pulse^2)),  N = 10
```

A range cell becomes a plot when all of the following are true:

1. its integrated magnitude is greater than the fixed `0.26` threshold;
2. it is a local maximum relative to its two adjacent cells; and
3. the receive aperture has at least one active element.

### Why the threshold is 0.26

`0.26` is an engineering calibration for this demonstration, not a threshold
derived from a current adaptive-CFAR noise estimate or an operational `Pfa`
requirement. It was originally selected for the single-pulse complex Gaussian
noise model, where each I and Q component has `sigma = 0.05`. A noise-only
single-pulse magnitude is Rayleigh distributed, giving

```text
P(sqrt(I^2 + Q^2) > T) = exp(-T^2 / (2 * sigma^2))

T = 0.26, sigma = 0.05  ->  P(exceedance) ~= 1.34e-6 per bin
```

That probability put the original high-rate detector in the useful demo
regime of a few raw noise excursions per second across all faces rather than
tens of thousands. The local-maximum test reduced the number that became
plots. At the same time, `0.26` left useful margin for the calibrated target
returns: relative to the complex-noise RMS,

```text
noise_magnitude_RMS = sqrt(2) * 0.05 = 0.07071
0.26 / 0.07071 = 3.677  ->  11.31 dB reported magnitude ratio
```

The production detector now averages power over the normal ten pulses in a
dwell before applying the same threshold. Under the ideal independent-noise
model,

```text
M = sqrt((1/N) * sum(I_p^2 + Q_p^2))
N * M^2 / sigma^2 ~ chi-square with 2N degrees of freedom

N = 10, M > 0.26  ->  chi-square(20) > 270.4
```

The corresponding ideal noise-only tail probability is approximately
`8.6e-46` per integrated bin before the local-maximum test. Thus `0.26` is no
longer justified as a calibrated false-alarm-rate setting for the current
integrator. It is intentionally retained as the established sensitivity
calibration: it preserves the demonstrated target ranges, deterministic replay
behavior, and tracker workload while strongly suppressing isolated noise.

Using the root-sum-square engineering approximation, the threshold corresponds
to a noise-free target voltage of

```text
A_threshold ~= sqrt(0.26^2 - 2 * 0.05^2) = 0.2502
```

`Pfa` is the probability of false alarm: the probability that the detector
reports a threshold crossing when no target signal is present. It is normally
stated per resolution cell or per independent detector test. That per-test
probability is not the complete system false-alarm rate. For a small `Pfa`, a
first-order opportunity count is

```text
expected false alarms ~= Pfa * tested bins * dwells * faces
```

The local-maximum requirement, correlations between bins or pulses, and other
gates change the exact system result. A true CFAR detector estimates the local
noise or clutter level and adjusts its threshold to maintain a selected
`Pfa`. This simulator's fixed `0.26` threshold has an analytically meaningful
`Pfa` only while the stated independent Gaussian-noise model is valid; it does
not estimate a local reference window or maintain a calibrated false-alarm
probability when the background changes. The term “CFAR-like” in the code
refers only to the fixed-threshold local-maximum peak picker.

Changing the noise model, adding clutter, or requiring an explicit `Pd`/`Pfa`
would therefore require recalculating or replacing this constant rather than
treating `0.26` as universally meaningful.

For the simulation's current controlled background, this simple algorithm
performs well. The effective-range changes leave the detector's 294 plots in
the deterministic 60-second replay unchanged; the former extra track was a
downstream slant-range/elevation-bar association problem, not a failure of
threshold detection. A more elaborate adaptive CFAR implementation is
therefore not required to correct the behavior observed in this phase.

That conclusion applies specifically to the present independent Gaussian-noise
model. Realistic sea clutter, weather returns, interference, and fluctuating
target RCS may be introduced in a later phase. Those effects create a
range- and time-varying background for which adaptive thresholding and
explicit probability-of-detection/false-alarm validation would become more
important.

The reported S/N is

```text
S/N_reported_dB = 20 * log10(M_bin / noise_magnitude_RMS)
noise_magnitude_RMS = sqrt(2) * 0.05
```

Consequently, the fixed threshold creates an approximately 11.31 dB minimum
reported value. This field is a magnitude-to-noise-RMS ratio. It should not be
interpreted as a laboratory-calibrated post-detection power SNR.

## Range-cell estimate

The return synthesizer assigns a target in
`[bin * resolution, (bin + 1) * resolution)` to `bin`. The detection now
reports the cell center:

```text
R_reported = (bin + 0.5) * range_resolution
```

This replaces the former lower-edge report. In the idealized single-target
case it removes the negative range bias and bounds quantization error to
approximately ±74.95 m. No sub-cell interpolation is claimed.

## Effective sensitivity

For documentation and tests, expected integrated magnitude is approximated by
root-sum-square combination of coherent target voltage and complex-noise RMS:

```text
M_expected ~= sqrt(A^2 + noise_magnitude_RMS^2)
```

Solving `M_expected = 0.26` gives a required target voltage of approximately
`0.2502`. The nominal beam-center threshold-crossing range is then

```text
R_threshold =
  sqrt(K * f_active * P * sqrt(RCS_linear) / required_target_voltage)
```

This is a mean-magnitude engineering boundary, not a hard detection boundary.
Noise makes detection probabilistic near the threshold; the current simulator
does not calculate `Pd` or `Pfa`. The 100 km instrumented range caps the
effective result.

> **Model limitation:** The sensitivity values below come from the simulator's
> simplified, demo-calibrated return model. They are not validated SPY-6 or
> operational-radar performance figures and must not be used to infer real-world
> detection ranges. The model omits aspect-dependent and fluctuating RCS,
> clutter, propagation and atmospheric losses, multipath, interference,
> jamming, and calibrated probability of detection or false alarm.

| Target type | Target RCS | Nominal beam-center crossing | At the -3 dB beam edge | Effective cap |
|---|---:|---:|---:|---:|
| Drone | -15 dBsm | 14.6 km | 12.3 km | unchanged |
| Missile | -10 dBsm | 19.5 km | 16.4 km | unchanged |
| Fighter | 0 dBsm | 34.6 km | 29.1 km | unchanged |
| Decoy | +5 dBsm | 46.2 km | 38.9 km | unchanged |
| Bomber | +20 dBsm | 109.5 km | 92.1 km | 100 km center |
| Ship | +35 dBsm | 259.6 km | 218.3 km | 100 km throughout the main beam |

The -3 dB column uses a voltage-pattern response of `sqrt(0.5)`. Since range
is proportional to the square root of voltage response, the edge range is
approximately `0.8409` of beam-center range. Array outages reduce range
through both active aperture fraction and the changed pattern response.

The often-used “approximately 17 km for a -15 dBsm drone” remains a useful
optimistic demo rule of thumb when noise magnitude is linearly credited
toward the threshold. The internally consistent root-sum-square crossing used
for regression and uncertainty work is 14.6 km. Neither number is a
probability-of-detection guarantee.

## Measurement uncertainty

Each accepted plot receives uncertainty derived from its reported S/N. The
model first converts the reported dB amplitude ratio to a power ratio:

```text
rho = 10^(S/N_reported_dB / 10)
```

Values are bounded below by the detector threshold and above by 80 dB.
Non-finite input uses a 12 dB default. This prevents malformed external
samples from creating unbounded association gates or covariance.

The one-standard-deviation estimates are:

```text
sigma_range_quantization = range_resolution / sqrt(12)
sigma_range_noise        = range_resolution / (2 * sqrt(rho))
sigma_range_ambiguity    = ambiguity_width / sqrt(12)
sigma_range              = RSS(sigma_range_quantization,
                               sigma_range_noise,
                               sigma_range_ambiguity)

sigma_az_quantization = azimuth_step / sqrt(12)
sigma_az_noise        = nominal_beamwidth / (2 * sqrt(2 * rho))
sigma_az              = RSS(sigma_az_quantization, sigma_az_noise)

sigma_elevation = elevation_bar_width / sqrt(12)
```

`RSS(...)` means root-sum-square of the independent terms. Uniform
quantization gives the `/sqrt(12)` terms. The inverse-SNR terms are
conservative centroiding approximations, not a Cramér-Rao bound. High-S/N
uncertainty approaches the approximately 43.27 m range floor and 0.65°
raster-bearing floor.

For ordinary reports, `sigma_range_ambiguity` is zero. For a truncated report
below 3 km, the selected midpoint implies
`R_true = 2 * R_apparent - R_blind`; the remaining correlation plateau spans
`R_blind - R_true`. Treating that unresolved plateau as uniform carries its
large uncertainty into association gates and published covariance. Strong S/N
therefore cannot incorrectly erase pulse-eclipse ambiguity.

Elevation uncertainty does not shrink with S/N because the processor reports
only one of the 3°, 14°, or 25° dwell-bar centers. Without a continuous
elevation estimator, stronger energy cannot reveal a sub-bar angle. The
11-degree bar width therefore gives an approximately 3.18° standard
deviation.

## Association and elevation-bar continuity

The tracker predicts each Cartesian state with its alpha-beta velocity. It
then associates a plot using:

- slant-range error, not horizontal-range error;
- azimuth cross-range error; and
- motion uncertainty during initiation.

The range gate is the greater of the existing 375 m gate and three reported
range standard deviations, plus initiation motion uncertainty. The azimuth
gate is similarly the greater of the existing 2.6° gate and three reported
azimuth standard deviations. At normal accepted S/N values the established
gates remain dominant, so this phase does not broadly increase association
or alter detector sensitivity.

Elevation is deliberately excluded from nearest-neighbor scoring. A bar
center is an illumination label, not an angle measurement. Each bar is
treated as the interval `center ± 5.5°`:

- if predicted track elevation is inside the illuminated interval, it is
  retained;
- if it is outside, it is projected to the nearest interval boundary; and
- when the bar label changes, the position is moved to that boundary and the
  beta velocity correction is suppressed for that update.

Thus a 14°→25° handoff projects to their shared 19.5° boundary. It does not
convert one continuous target into two Cartesian points at 14° and 25°, and
it cannot inject an artificial vertical-velocity impulse. If the handoff
occurs while velocity is still being initialized, the endpoint baseline is
restarted at the interval boundary so the bar change cannot masquerade as
target translation. The regression reproduces the observed approximately
22.84 km, 326° transition and requires one persistent track ID with no
fragment.

Track confirmation is otherwise unchanged: three independent scan visits,
separated by at least 600 ms and occurring inside a 6-second window, confirm
a track. Confirmed tracks coast for 12 seconds. The alpha-beta gains remain
`alpha = 0.55` and `beta = 0.20`.

## Published Cartesian covariance

`TargetTrack.covariance` is now populated from the latest plot uncertainty
instead of a fixed 50 m standard deviation on every axis. Assuming independent
range, azimuth, and elevation errors, the implementation transforms polar
measurement variance into ENU position covariance:

```text
C_ENU = J * diag(sigma_range^2,
               sigma_azimuth_rad^2,
               sigma_elevation_rad^2) * J^T
```

`J` is the exact spherical-to-Cartesian Jacobian evaluated at the filtered
track position. All nine row-major covariance elements are published,
including cross-axis terms.

This covariance characterizes the latest measurement geometry. The tracker is
an alpha-beta filter, not a Kalman filter, so the covariance is not a
propagated posterior state covariance and does not grow during coast. The
coarse bar-only elevation estimate appropriately produces much larger
vertical uncertainty than the former fixed value.

## Verification

The portable regressions verify:

- timing-derived range resolution, blind range, and unambiguous range;
- default observation and explicitly disabled rejection of sub-3 km returns;
- coherent capture fraction, outward apparent-range bias, boundary
  continuity, and bounded near-return voltage;
- range-cell center reporting;
- fixed-threshold S/N floor;
- sensitivity crossings for -15 through +35 dBsm;
- the 100 km instrumented cap;
- monotonic S/N-dependent uncertainty and quantization floors;
- symmetric, bearing-rotating Cartesian covariance;
- deterministic four-target replay with one birth per truth target;
- hard-gate coast/drop/reacquisition versus continuous default plots in an
  idealized fixed-bearing 4 km–700 m–4 km transit;
- three-scan confirmation and 12-second coast behavior; and
- persistent identity and no velocity impulse at the 14°→25° elevation
  transition.

## Deferred work and risk

The following remain separate phases because they change detection counts,
false alarms, computational load, or the receiver's validity envelope:

1. **Adaptive CFAR:** use training and guard cells with an explicit `Pfa`.
   This changes sensitivity in noise and clutter and therefore needs new
   golden scenarios.
2. **Matched-filter fractional range:** distribute a delayed compressed pulse
   according to fractional sample position and interpolate its peak. This can
   provide sub-cell range estimates but changes adjacent-cell amplitudes.
3. **Doppler processing:** coherently retain pulse phase, form range-Doppler
   cells, and estimate radial velocity. Raw I/Q contains phase evolution, but
   the current noncoherent integrator intentionally discards it.
4. **Continuous angle estimation:** model simultaneous channels or monopulse
   ratios. S/N could then reduce azimuth and elevation uncertainty below the
   raster/bar floors.
5. **Clutter and fluctuating targets:** add land/sea/weather backgrounds,
   target fluctuation models, and explicit `Pd/Pfa` validation.
6. **Higher-fidelity sub-3 km processing:** replace the default rectangular
   midpoint approximation with an explicitly sampled waveform, fractional
   delay, transmitter leakage, duplexer recovery, saturation/recovery, and
   matched-filter false-peak behavior. The current option exposes the primary
   pulse-eclipse energy loss and range ambiguity without claiming those
   hardware details.

Adaptive CFAR and Doppler are intentionally not mixed into this phase. They
are valuable, but they alter the event stream and carry more regression risk
than correcting the interpretation and tracking of measurements the
simulation already produces.
