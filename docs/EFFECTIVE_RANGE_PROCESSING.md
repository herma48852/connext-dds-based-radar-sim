# Effective-range detection and tracking model

This document defines what “accurate within the effective detection range”
means for AesaRadarSim, describes the implemented algorithms, and records the
assumptions that limit the result. It covers the low-risk first phase of the
range-processing plan. The detector threshold and the approximately 3 km
transmit/receive blind range are intentionally unchanged.

The implementation is internally consistent with the simulation's simplified
waveform and measurement model. It is not a validated performance model for
SPY-6 or any other operational radar.

## Scope

The implemented phase improves processing in the existing valid envelope:

- approximately 3 km minimum receive range;
- 100 km instrumented range;
- target-dependent sensitivity inside that envelope;
- symmetric range-cell reporting;
- S/N-derived measurement uncertainty and Cartesian track covariance;
- slant-range track association; and
- continuous track identity across elevation-bar transitions.

It does not add sub-3 km processing, adaptive CFAR, Doppler filtering,
fractional-delay matched filtering, continuous angle estimation, clutter, or a
probability-of-detection model. Those are listed under deferred work.

## RF and timing basis

The representative, unclassified search waveform uses:

| Quantity | Value | Consequence |
|---|---:|---|
| Carrier frequency | 3 GHz | approximately 9.99 cm wavelength |
| Bandwidth | 1 MHz | approximately 149.90 m range resolution |
| Pulse repetition frequency | 1 kHz | approximately 149.90 km unambiguous range |
| Pulse width | 20 microseconds | approximately 2.998 km receive blind range |
| Dwell time | 10 ms | 10 pulses integrated per dwell |
| Instrumented range | 100 km | 668 complex range cells |

A monostatic return travels to the target and back, so its delay is

```text
t_return = 2 R / c
```

where `R` is slant range and `c` is the speed of light. The factor of two is
already present in every timing-derived range quantity. For example, a 3 km
target has an approximately 20 microsecond round-trip delay, while a 100 km
target has an approximately 667 microsecond delay. Both returns fit inside the
1 millisecond pulse-repetition interval, but the 3 km return overlaps the
20-microsecond transmit interval and is therefore rejected by the current
receiver model.

“Range” in detection and association means slant range:

```text
R = sqrt(east^2 + north^2 + up^2)
```

Horizontal or ground range is `sqrt(east^2 + north^2)`. They are equal only
for a target at zero elevation.

## Return synthesis

For a target inside the receive and instrumented-range gates, the simulated
voltage amplitude is

```text
A = K f_active P(azimuth offset) sqrt(RCS_linear) / R^2
```

where:

- `K` is the demo calibration scaled by wavelength;
- `f_active` is the active aperture fraction;
- `P` is the voltage response of the calculated azimuth array pattern;
- `RCS_linear = 10^(RCS_dBsm / 10)`; and
- `R` is slant range in metres.

This follows the monostatic radar equation's `1/R^4` received-power law:
voltage is proportional to the square root of power and therefore follows
`1/R^2`. The target voltage is placed coherently into the selected range cell
and its two adjacent compressed-pulse sidelobe cells. Independent zero-mean
Gaussian noise with standard deviation `0.05` is added to each I and Q
component.

The model assumes a constant RCS for each target. It does not currently model
aspect-dependent RCS, Swerling fluctuations, propagation loss, atmospheric
attenuation, terrain, sea state, multipath, ducting, jamming, or receiver
saturation.

## Dwell integration and detection

Each 10 ms dwell contains ten 1 kHz pulses. The signal processor performs
noncoherent power integration:

```text
M_bin = sqrt((1/N) sum(I_pulse^2 + Q_pulse^2)),  N = 10
```

A range cell becomes a plot when all of the following are true:

1. its integrated magnitude is greater than the fixed `0.26` threshold;
2. it is a local maximum relative to its two adjacent cells; and
3. the receive aperture has at least one active element.

The term “CFAR-like” in the code refers to this peak picker. The threshold is
fixed; it does not estimate a local clutter or noise reference window and
does not provide a calibrated false-alarm probability.

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
S/N_reported_dB = 20 log10(M_bin / noise_magnitude_RMS)
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
  sqrt(K f_active P sqrt(RCS_linear) / required_target_voltage)
```

This is a mean-magnitude engineering boundary, not a hard detection boundary.
Noise makes detection probabilistic near the threshold; the current simulator
does not calculate `Pd` or `Pfa`. The 100 km instrumented range caps the
effective result.

| Target RCS | Nominal beam-center crossing | At the -3 dB beam edge | Effective cap |
|---:|---:|---:|---:|
| -15 dBsm | 14.6 km | 12.3 km | unchanged |
| -10 dBsm | 19.5 km | 16.4 km | unchanged |
| 0 dBsm | 34.6 km | 29.1 km | unchanged |
| +5 dBsm | 46.2 km | 38.9 km | unchanged |
| +20 dBsm | 109.5 km | 92.1 km | 100 km center |
| +35 dBsm | 259.6 km | 218.3 km | 100 km throughout the main beam |

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
sigma_range_noise        = range_resolution / (2 sqrt(rho))
sigma_range              = RSS(sigma_range_quantization,
                               sigma_range_noise)

sigma_az_quantization = azimuth_step / sqrt(12)
sigma_az_noise        = nominal_beamwidth / (2 sqrt(2 rho))
sigma_az              = RSS(sigma_az_quantization, sigma_az_noise)

sigma_elevation = elevation_bar_width / sqrt(12)
```

`RSS(a,b)` means `sqrt(a^2+b^2)`. Uniform quantization gives the `/sqrt(12)`
terms. The inverse-SNR terms are conservative centroiding approximations, not
a Cramér-Rao bound. High-S/N uncertainty approaches the approximately
43.27 m range floor and 0.65° raster-bearing floor.

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
C_ENU = J diag(sigma_range^2,
               sigma_azimuth_rad^2,
               sigma_elevation_rad^2) J^T
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
- range-cell center reporting;
- fixed-threshold S/N floor;
- sensitivity crossings for -15 through +35 dBsm;
- the 100 km instrumented cap;
- monotonic S/N-dependent uncertainty and quantization floors;
- symmetric, bearing-rotating Cartesian covariance;
- deterministic four-target replay with one birth per truth target;
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
6. **Sub-3 km processing:** model transmit/receive switching, pulse eclipse,
   partial pulse compression, and truncated-return false detections. Until
   that receiver behavior exists, the approximately 3 km boundary remains a
   hard validity limit.

Adaptive CFAR and Doppler are intentionally not mixed into this phase. They
are valuable, but they alter the event stream and carry more regression risk
than correcting the interpretation and tracking of measurements the
simulation already produces.
